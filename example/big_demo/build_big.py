# -*- coding: utf-8 -*-
"""big_demo 一键构建：scripts/*.s2c -> gen/*.c（scl_emit_c --cmd）→ 编译 -> 运行自测。

用法：
  python example/big_demo/build_big.py          # 生成 + 编译 + 运行（自测断言全绿退出 0）
  python example/big_demo/build_big.py --no-run # 只生成 + 编译

依赖：仓库 tools/（scl_script2chain / scl_emit_c）与本机 gcc。
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent   # 仓库根
BD   = ROOT / "example" / "big_demo"
GEN  = BD / "gen"
EXE  = ROOT / "build" / "big_demo.exe" if os.name == "nt" else ROOT / "build" / "big_demo"

sys.path.insert(0, str(ROOT / "tools"))


def gen_scripts():
    import scl_emit_c
    GEN.mkdir(parents=True, exist_ok=True)
    made = []
    for src in sorted((BD / "scripts").glob("*.s2c")):
        name = src.stem
        out = GEN / ("sc_%s.c" % name)
        with open(str(src), "r", encoding="utf-8") as f:
            chain, _w = scl_emit_c.translate(f.read(), const_fold=True,
                                             var_max=12, name_max=8, value_max=15)
        bc, argc = scl_emit_c.encode_chain(chain)
        # 脚本命令：字节码末尾追加无参 free（--cmd 等价）
        bc = bc + [0x00, scl_emit_c.META_OPC["free"] & 0xFF, 0x00, 0x00]
        text = scl_emit_c.emit_c(name, bc, argc, str(src), cmd=name)
        out.write_text(text, encoding="utf-8")
        made.append(out.name)
    print("生成脚本命令 C:", ", ".join(made))


def build(debug=False):
    srcs = [
        # 库源：Src/ 下全部 .c（模块拆分后自动纳入）
        *[str(p) for p in sorted((ROOT / "scl" / "Src").glob("*.c"))],
        str(ROOT / "example" / "scl_port.c"),
        str(BD / "b_sim.c"),
        str(BD / "b_cmds1.c"),
        str(BD / "b_cmds2.c"),
        str(BD / "b_env.c"),
        str(BD / "b_scripts.c"),
        str(BD / "main.c"),
    ] + [str(p) for p in sorted(GEN.glob("sc_*.c"))]
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-pipe",
           "-DSCL_CFG_DYNAMIC_MEM_EN=1",
           "-DSCL_CFG_VAR_MAX=12",
           "-I", str(ROOT / "scl" / "Inc"), "-I", str(ROOT / "scl" / "Src"),
           "-I", str(ROOT / "example"), "-I", str(BD)] + srcs
    if debug:
        cmd += ["-g", "-O0"]
    cmd += ["-o", str(EXE)]
    (ROOT / "build").mkdir(exist_ok=True)
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write((r.stderr or b"").decode("utf-8", "replace")[-4000:])
        sys.exit(2)
    print("编译:", EXE)


def main():
    ap = argparse.ArgumentParser(description="big_demo 一键构建/运行")
    ap.add_argument("--no-run", action="store_true", help="只生成 + 编译，不运行")
    ap.add_argument("--debug", action="store_true", help="生成带调试符号的动态测试程序")
    args = ap.parse_args()
    gen_scripts()
    build(args.debug)
    if args.no_run:
        return 0
    r = subprocess.run([str(EXE)], timeout=180)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
