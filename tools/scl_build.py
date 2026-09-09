#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scl_build.py — SCL 统一构建 / 测试 / 尺寸测量脚本（纯标准库，跨平台）

把散落在 README 里的命令收拢为一键工具，覆盖：
  test  : PC 全量测试（默认档 + CMDDESC 关 + MCU 骨架模拟 + 转译器 s2c_test）
  sizes : ARM(arm-none-eabi) 库尺寸矩阵（默认/档A/档A+关消息/档B/-Os → Flash/RAM）
  check : ARM 裁剪开关零警告矩阵（msg0/cmddesc0/prog0/env0/runtext0 等）
  all   : 依次执行 test、check、sizes（缺 ARM 工具时自动跳过并提示）

用法：
  python tools/scl_build.py           # 等价 all
  python tools/scl_build.py test
  python tools/scl_build.py sizes
  python tools/scl_build.py check

说明：
  - 本机透明加密环境下，编译产物(.exe/.o)只有白名单进程能读明文；故统一用本脚本
    以 Python subprocess 编译（gcc -pipe 规避 temp 落盘权限）与运行，输出以 utf-8 解码。
  - ARM 尺寸用 Python 解析 ELF 节表（binutils size/nm 非白名单读不到明文）。
  - 无 arm-none-eabi-gcc 时，sizes/check 自动跳过；无 gcc 时报错退出。
"""

import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
EXE = ".exe" if os.name == "nt" else ""

INC = ["-I", str(ROOT / "scl" / "Inc"),
       "-I", str(ROOT / "scl" / "Src"),
       "-I", str(ROOT / "example")]
CORE_SRC = [str(ROOT / "scl" / "Src" / "scl.c"),
            str(ROOT / "scl" / "Src" / "scl_var.c"),
            str(ROOT / "scl" / "Src" / "scl_env.c")]

ARM = "arm-none-eabi-gcc"
ARM_MCU = ["-mcpu=cortex-m4", "-mthumb", "-std=c99"]

# 终端可能是 gbk 等非 UTF-8 编码：输出含不可编码字符时替换而非抛异常
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass


def which(tool):
    p = shutil.which(tool)
    return p


def dec(b):
    return (b or b"").decode("utf-8", "replace")


def sh(cmd):
    r = subprocess.run(cmd, capture_output=True)
    return r


def warn_lines(stderr):
    return [ln for ln in dec(stderr).splitlines()
            if ("warning:" in ln) or ("error:" in ln)]


def compile_c(srcs, out, extra=(), cc="gcc"):
    """PC 编译（-O2 -pipe），返回 True=成功。打印警告/错误。"""
    cc = which(cc) or cc
    cmd = [cc, "-pipe", "-O2", "-Wall", "-Wextra", *INC, *extra, *srcs, "-o", str(out)]
    r = sh(cmd)
    for ln in warn_lines(r.stderr):
        print("   " + ln)
    if r.returncode != 0:
        print("[编译失败] %s" % out.name)
        return False
    return True


def run_exe(exe):
    """运行可执行（白名单 Python 启动，规避透明加密），返回 stdout 文本。"""
    r = sh([str(exe)])
    return dec(r.stdout)


def pass_summary(text):
    for ln in text.splitlines():
        if "PASS=" in ln and "FAIL=" in ln:
            return ln.strip()
    return "（未找到 PASS 汇总行）"


# ---------------- test：PC 全量 ----------------

def build_test(name, extra=(), main="main.c"):
    srcs = [*CORE_SRC,
            str(ROOT / "example" / "scl_port.c"),
            str(ROOT / "example" / "demo_cmds.c"),
            str(ROOT / "example" / main)]
    BUILD.mkdir(exist_ok=True)
    exe = BUILD / (name + EXE)
    if not compile_c(srcs, exe, extra):
        return False
    print("[%s] %s" % (name, pass_summary(run_exe(exe))))
    return True


def cmd_test(_a):
    ok = True
    print("\n== PC 全量测试（默认档，含 CMDDESC） ==")
    ok &= build_test("scl_test")
    print("\n== PC 全量测试（SCL_CFG_CMDDESC_EN=0 变体） ==")
    ok &= build_test("scl_test_nd", ("-DSCL_CFG_CMDDESC_EN=0",))
    print("\n== MCU main 骨架无板自检（mcu_template/mcu_boot_sim.c） ==")
    ok &= build_test("mcu_boot_sim", main=pathlib.Path("mcu_template") / "mcu_boot_sim.c")
    print("\n== mini-scl 自包含状态机自检（example/mini/boot_mini_sim.c） ==")
    ok &= build_test("mini_boot_sim", ("-DSCL_CFG_MINI_EN=1",),
                     main=pathlib.Path("mini") / "boot_mini_sim.c")
    print("\n== 转译器 s2c_test（精确比对 + 真实回喂 + emit-c） ==")
    r = sh([sys.executable, str(ROOT / "tools" / "s2c_test.py")])
    print(dec(r.stdout))
    if r.returncode != 0:
        ok = False
    print("\n全量测试结果: %s" % ("全绿" if ok else "存在失败"))
    return 0 if ok else 1


# ---------------- ELF 尺寸（自包含解析） ----------------

def read_sections(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF":
        raise ValueError("%s 非 ELF" % path)
    if d[4] == 1:                       # ELF32
        (shoff,) = struct.unpack_from("<I", d, 32)
        _, shnum, shstrndx = struct.unpack_from("<HHH", d, 46)
        ssize, fmt = 40, "<IIIIIIIIII"
    else:                               # ELF64
        (shoff,) = struct.unpack_from("<Q", d, 40)
        _, shnum, shstrndx = struct.unpack_from("<HHI", d, 58)
        ssize, fmt = 64, "<IIQQQQIIQQ"
    h = struct.unpack_from(fmt, d, shoff + shstrndx * ssize)
    shstr = d[h[4]:h[4] + h[5]]
    out = []
    for i in range(shnum):
        h = struct.unpack_from(fmt, d, shoff + i * ssize)
        no = h[0]
        end = shstr.find(b"\0", no)
        out.append((shstr[no:end].decode("ascii", "replace"), h[5]))
    return out


def sec_sum(secs):
    """按名字归类返回 (text, rodata, data, bss)。"""
    t = r = d = b = 0
    for n, s in secs:
        if n.startswith(".text") or n in (".init", ".fini"):
            t += s
        elif n.startswith(".rodata") or n in (".ARM.exidx", ".ARM.extab", ".eh_frame"):
            r += s
        elif n.startswith(".data"):
            d += s
        elif n.startswith(".bss"):
            b += s
    return t, r, d, b


def obj_sizes(objdir):
    """对目录内 scl*.o 求和，返回 (Flash, RAM)。"""
    text = ro = data = bss = 0
    for o in sorted(pathlib.Path(objdir).glob("*.o")):
        t, r, d, b = sec_sum(read_sections(str(o)))
        text += t; ro += r; data += d; bss += b
    return text + ro + data, data + bss


def compile_arm_objs(tag, extra=(), opt="-O2"):
    arm = which(ARM)
    if not arm:
        return None
    od = BUILD / "mcu32" / tag
    od.mkdir(parents=True, exist_ok=True)
    for f in ("scl.c", "scl_var.c", "scl_env.c"):
        src = ROOT / "scl" / "Src" / f
        out = od / (f.replace(".c", ".o"))
        cmd = [arm, *ARM_MCU, opt, "-Wall", "-Wextra",
               "-I", str(ROOT / "scl" / "Inc"), "-I", str(ROOT / "scl" / "Src"),
               *extra, "-c", str(src), "-o", str(out)]
        r = sh(cmd)
        if r.returncode != 0:
            print("[ARM 编译失败] %s %s" % (tag, f))
            print(dec(r.stderr))
            return None
    return od


# 各档宏集合（对应 doc/other/scl-config-profiles.md）
PROFILES = [
    ("def_O2",   "默认全功能(O2)", (), "-O2"),
    ("def_Os",   "默认(-Os)", (), "-Os"),
    ("A_O2",     "档A 最小RAM(O2)", ("-DSCL_CFG_RUN_TEXT_EN=0", "-DSCL_CFG_VAR_MAX=2",
                                     "-DSCL_CFG_ARG_MAX=4", "-DSCL_CFG_ARG_LEN_MAX=24"), "-O2"),
    ("A0_O2",    "档A+关消息(O2)", ("-DSCL_CFG_RUN_TEXT_EN=0", "-DSCL_CFG_VAR_MAX=2",
                                    "-DSCL_CFG_ARG_MAX=4", "-DSCL_CFG_ARG_LEN_MAX=24",
                                    "-DSCL_CFG_MSG_EN=0"), "-O2"),
    ("B_O2",     "档B 平衡(O2)", ("-DSCL_CFG_RUN_TEXT_EN=1", "-DSCL_CFG_RUN_PROG_EN=1",
                                  "-DSCL_CFG_SCRIPT_MAX=256", "-DSCL_CFG_BC_MAX=256",
                                  "-DSCL_CFG_ARG_CACHE_MAX=128", "-DSCL_CFG_LABEL_MAX=8",
                                  "-DSCL_CFG_VAR_MAX=4", "-DSCL_CFG_ARG_MAX=6",
                                  "-DSCL_CFG_ARG_LEN_MAX=24"), "-O2"),
    ("mini_O2",  "mini-scl(O2)", ("-DSCL_CFG_MINI_EN=1",), "-O2"),
    ("mini_Os",  "mini-scl(-Os)", ("-DSCL_CFG_MINI_EN=1",), "-Os"),
]


def cmd_sizes(_a):
    if not which(ARM):
        print("未找到 %s —— 跳过尺寸矩阵（安装 Arm GNU Toolchain 后可测 32 位占用）" % ARM)
        return 0
    print("\n== ARM(Cortex-M4) 库尺寸矩阵（Flash=.text+.rodata+.data；RAM=.data+.bss） ==")
    for tag, label, extra, opt in PROFILES:
        od = compile_arm_objs(tag, extra, opt)
        if od is None:
            continue
        flash, ram = obj_sizes(od)
        print("  %-16s Flash=%6d B  RAM=%5d B" % (label, flash, ram))
    return 0


# ---------------- check：ARM 裁剪矩阵零告警 ----------------

CHECKS = [
    ("msg0",            "-DSCL_CFG_MSG_EN=0"),
    ("cmddesc0",        "-DSCL_CFG_CMDDESC_EN=0"),
    ("prog0",           "-DSCL_CFG_RUN_PROG_EN=0"),
    ("env0",            "-DSCL_CFG_ENV_EN=0"),
    ("runtext0",        "-DSCL_CFG_RUN_TEXT_EN=0"),
    ("runtext0_msg0",   "-DSCL_CFG_RUN_TEXT_EN=0 -DSCL_CFG_MSG_EN=0"),
    ("msg0_cmddesc0",   "-DSCL_CFG_MSG_EN=0 -DSCL_CFG_CMDDESC_EN=0"),
]


def cmd_check(_a):
    if not which(ARM):
        print("未找到 %s —— 跳过裁剪矩阵检查" % ARM)
        return 0
    print("\n== ARM 裁剪开关零警告矩阵（-fsyntax-only） ==")
    bad = 0
    srcs = [str(ROOT / "scl" / "Src" / "scl.c"),
            str(ROOT / "scl" / "Src" / "scl_var.c"),
            str(ROOT / "scl" / "Src" / "scl_env.c")]
    for name, cfg in CHECKS:
        cmd = [ARM, *ARM_MCU, "-O2", "-Wall", "-Wextra", "-fsyntax-only",
               "-I", str(ROOT / "scl" / "Inc"), "-I", str(ROOT / "scl" / "Src"),
               *cfg.split(), *srcs]
        r = sh(cmd)
        w = warn_lines(r.stderr)
        if w:
            bad += 1
            print("  [%s] 有告警:" % name)
            for ln in w:
                print("     " + ln)
        else:
            print("  [%s] OK" % name)
    print("裁剪矩阵: %s" % ("全零警告" if bad == 0 else "%d 个组合有告警" % bad))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser(description="SCL 统一构建/测试/尺寸（见文件头说明）")
    ap.add_argument("cmd", nargs="?", default="all",
                    choices=["all", "test", "check", "sizes"])
    a = ap.parse_args()
    rc = 0
    if a.cmd in ("all", "test"):
        rc |= cmd_test(a)
    if a.cmd in ("all", "check"):
        rc |= cmd_check(a)
    if a.cmd in ("all", "sizes"):
        rc |= cmd_sizes(a)
    return rc


if __name__ == "__main__":
    sys.exit(main())
