#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
s2c_test.py — Script2Chain 转译器测试（v0.2：类型化 var + 原子条件/赋值表达式降级）

  1) 单元：现代脚本 -> 期望汇编链（精确比对，label/jump + 类型 var + 运算指令）
  2) 错误：保留字/超限/递归/语法/&& 未支持等应报 S2CError
  3) 回喂：example/s2c/*.s2c 与单元链喂给真实 SCL（chain_runner）执行
"""

import os
import subprocess
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from scl_script2chain import translate, S2CError  # noqa: E402

PASS = 0
FAIL = 0


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  [PASS] " + msg)
    else:
        FAIL += 1
        print("  [FAIL] " + msg)


def section(t):
    print("\n===== " + t + " =====")


def unit_exact(src, expect, msg):
    try:
        chain, warns = translate(src)
        check(chain == expect, "%s\n       got: %s\n       exp: %s" %
              (msg, chain, expect))
        if warns:
            print("       (warns: %s)" % ", ".join(warns))
    except S2CError as e:
        check(False, "%s -> 意外报错: %s" % (msg, e.msg))


def test_unit():
    section("1. 单元（精确汇编链比对）")

    # var 显式类型 / 推断
    unit_exact("var bool b = true", "var bool b=true", "var bool 显式")
    unit_exact("var int n = 0", "var int n=0", "var int 显式")
    unit_exact("var flag f = -x", "var flag f=-x", "var flag 显式")
    unit_exact('var s = "hello world"', 'var string s="hello world"',
               "var 无类型推断 string")
    unit_exact("var n = 3", "var int n=3", "var 无类型推断 int")
    unit_exact("var b = true", "var bool b=true", "var 无类型推断 bool")
    unit_exact("var f = -x", "var flag f=-x", "var 无类型推断 flag")
    unit_exact("var a = 0x1F", "var int a=0x1F", "var 无类型推断 int(hex)")
    unit_exact("var b = 0b101", "var int b=0b101", "var 无类型推断 int(0b)")

    # const 顶层只读常量（v0.3：→ 链 var const ...）
    unit_exact("const int LIMIT=10", "var const int LIMIT=10", "const 显式类型顶层")
    unit_exact("const LIMIT=10", "var const int LIMIT=10", "const 推断类型 int")
    unit_exact("const DBG=true", "var const bool DBG=true", "const 推断类型 bool")
    # 顶层 'var const' 别名（与 const 等价，块内禁）
    unit_exact("var const int LIMIT=10", "var const int LIMIT=10", "var const 显式类型顶层")
    unit_exact("var const LIMIT=10", "var const int LIMIT=10", "var const 推断类型 int")

    # 原子条件：比较 → 运算指令；bool/变量 → btest
    unit_exact("if (mode == 1) { echo(a) } else { echo(b) }",
               "ieq mode 1;jump -a L1;echo b;jump L2;label L1;echo a;label L2",
               "if(比较表达式) else → ieq + label/jump")
    unit_exact("if (1 < 2) { echo(y) }",
               "ilt 1 2;jump -a L1;jump L2;label L1;echo y;label L2",
               "if(字面量比较) 真分支")
    unit_exact("var bool b=false\nif (!b) { echo(yes) }",
               "var bool b=false;bnot b;jump -a L1;jump L2;label L1;echo yes;label L2",
               "if(!bool变量) → bnot")
    # v0.3：&& || 短路（结果仍回 G_RETURN，供外层 if 的 jump -a）
    unit_exact("var bool p=true\nvar bool q=false\nif (p && q) { echo(A) } else { echo(B) }",
               "var bool p=true;var bool q=false;btest p;jump -a L1;setret 0;jump L2;"
               "label L1;btest q;label L2;jump -a L3;echo B;jump L4;label L3;echo A;label L4",
               "if(&&) else → 短路 and")
    unit_exact("var bool p=true\nvar bool q=false\nif (p || q) { echo(A) } else { echo(B) }",
               "var bool p=true;var bool q=false;btest p;jump -a L1;btest q;jump L2;"
               "label L1;setret 1;label L2;jump -a L3;echo B;jump L4;label L3;echo A;label L4",
               "if(||) else → 短路 or")

    # do-while + 算术赋值
    unit_exact("var int n=0\nwhile (n < 3) { n = n + 1 }",
               "var int n=0;label L1;iadd n 1 n;ilt n 3;jump -a L1",
               "while(比较) + n=n+1 → iadd/ilt/label/jump")

    # 无条件 if{} else（沿用 G_RETURN）
    unit_exact("if { } else { echo(n) }",
               "jump -a L1;echo n;label L1",
               "if{} 仅 else：真跳跳过")

    # ret 糖衣 + 引号参数
    unit_exact('ret(1); echo("a b", z)',
               'setret 1;echo "a b" z',
               "ret 糖衣 + 引号多参命令")

    # fn 参数内联
    unit_exact("fn note(x,y){ echo(x,y) }\nnote(\"p q\",2)",
               'echo "p q" 2',
               "fn 参数内联")

    # 赋值 / 取负 / 拷贝
    unit_exact("var int x = 10\nx = x / 2\necho(x)",
               "var int x=10;idiv x 2 x;echo x",
               "赋值除法写回")
    unit_exact("var int a = 5\na = -a",
               "var int a=5;ineg a a",
               "赋值取负 → ineg")
    # v0.3：多运算符算术/位运算（隐藏临时变量 __t0，用后 free）
    unit_exact("var int x=10\nx = x + 1 + 2",
               "var int x=10;iadd x 1 __t0;iadd __t0 2 x;free __t0",
               "多运算符算术 a+b+c → 临时变量")
    unit_exact("var int a=0x0f\nvar int x=0\nx = (a & 0x0f) | 0x10",
               "var int a=0x0f;var int x=0;iand a 0x0f __t0;ior __t0 0x10 x;free __t0",
               "位运算+括号优先 → iand/ior")
    unit_exact("var int n=5\nn = -n + 1",
               "var int n=5;ineg n __t0;iadd __t0 1 n;free __t0",
               "单目负 + 加法 → ineg 临时")
    # 条件比较两侧的算术/位括号（如寄存器位判断）
    unit_exact("var int i=3\nif ((i & 1) == 1) { echo(odd) } else { echo(even) }",
               "var int i=3;iand i 1 __t0;ieq __t0 1;free __t0;"
               "jump -a L1;echo even;jump L2;label L1;echo odd;label L2",
               "if((i&1)==1) 位判断 → iand+ieq 临时")
    # v0.3：字符串/flag 状态比较 → seq（运行时按 ${} 展开比较）
    unit_exact('var string st="idle"\nif (st == "ok") { echo(Y) } else { echo(N) }',
               'var string st=idle;seq ${st} ok;jump -a L1;echo N;jump L2;label L1;echo Y;label L2',
               "string 状态 == → seq")

    # else if 链（v0.3：可 else if）→ 嵌套 if 线性转译
    unit_exact("var int x=1\nif (x==1) { echo(a) } else if (x==2) { echo(b) } else { echo(c) }",
               "var int x=1;ieq x 1;jump -a L1;ieq x 2;jump -a L3;echo c;jump L4;"
               "label L3;echo b;label L4;jump L2;label L1;echo a;label L2",
               "if/else if/else → 嵌套 if 转译")


def unit_err(src, keyword, msg):
    try:
        translate(src)
        check(False, "%s -> 未报错（期望含 %s）" % (msg, keyword))
    except S2CError as e:
        check(keyword in e.msg, "%s -> %s" % (msg, e.msg))


def test_error():
    section("2. 错误用例（应报 S2CError）")
    unit_err("var int a=1\nvar int b=2\nvar int c=3\nvar int d=4\nvar int e=5",
             "超过", "变量存活 >4 报错")
    unit_err("var int toolongname=1", "过长", "变量名 >8 报错")
    unit_err("var int x=1234567890123456", "过长", "变量字面值 >15 报错")
    unit_err("fn a(){ a() }\na()", "递归", "fn 递归报错")
    unit_err("if(true){ const int a=1 }", "仅允许顶层", "const 不允许在块内")
    unit_err("if(true){ var const int a=1 }", "仅允许顶层", "var const 不允许在块内")
    unit_err('echo("unclosed', "未闭合", "字符串未闭合报错")
    unit_err("add(1,2", "')'", "缺右括号报错")
    unit_err("x 3", "需要 '('", "裸标识符语句报错")
    unit_err("var if = 1", "保留", "变量名用保留字报错")
    unit_err("y = 3", "需先用 var", "赋值未声明目标报错")
    unit_err("echo(\"a\" + \"b\")", "不允许", "实参中算术/比较未支持")


# ============================ 3. 回喂（真实 SCL 执行 chain_runner） ============================

RUNNER = ROOT / "build" / "chain_runner"
RUNNER_EXE = RUNNER
if os.name == "nt":
    RUNNER_EXE = RUNNER.with_suffix(".exe")

RUNNER_CFG = ["-DSCL_CFG_SCRIPT_MAX=2048", "-DSCL_CFG_BC_MAX=2048",
              "-DSCL_CFG_ARG_CACHE_MAX=1024", "-DSCL_CFG_LABEL_MAX=64",
              "-DSCL_CFG_VAR_MAX=8"]


def build_runner():
    (ROOT / "build").mkdir(exist_ok=True)
    # -pipe：gcc 直通汇编器，避免临时 .s 落盘（透明加密环境更稳）
    cmd = ["gcc", "-O2", "-pipe"] + RUNNER_CFG + [
        "-I", str(ROOT / "scl" / "Inc"),
        "-I", str(ROOT / "scl" / "Src"),
        "-I", str(ROOT / "example"),
        str(ROOT / "scl" / "Src" / "scl.c"),
        str(ROOT / "scl" / "Src" / "scl_var.c"),
        str(ROOT / "scl" / "Src" / "scl_env.c"),
        str(ROOT / "example" / "scl_port.c"),
        str(ROOT / "example" / "demo_cmds.c"),
        str(ROOT / "example" / "chain_runner.c"),
        "-o", str(RUNNER_EXE),
    ]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        print("  (gcc 构建 chain_runner 失败，回喂测试跳过)")
        err = (r.stderr.decode("utf-8", "replace").splitlines() or [""])[0]
        print("  " + err)
        return False
    return True


def run_chain(chain, tag):
    tmp = ROOT / "build" / ("_feed_%s.chain" % tag)
    tmp.write_text(chain, encoding="utf-8")
    try:
        r = subprocess.run([str(RUNNER_EXE), str(tmp)], capture_output=True,
                           timeout=60)
    except OSError:
        return ""
    return (r.stdout + r.stderr).decode("utf-8", "replace")


# ---------- 4. 预编译 const C 程序（scl_emit_c + SCL_RunProg，省 RAM） ----------

EMITC_MAIN = r'''
#include <stdio.h>
#include <string.h>
#include "scl.h"
#include "scl_port.h"
#include "demo_cmds.h"
typedef struct { const char *name; const scl_prog_t *prog; } P;
'''
EMITC_MAIN_TAIL = r'''
static const P s_progs[] = {
'''
# 尾部由代码追加 + 收尾


def _emitc_main_c(progs):
    """生成逐 tag 运行 runner 的 main 源。progs=[(tag, c_text, subs)]"""
    lines = [EMITC_MAIN]
    for tag, _c, _s in progs:
        lines.append('extern const scl_prog_t scl_%s_prog;' % tag)
    lines.append(EMITC_MAIN_TAIL)
    for tag, _c, _s in progs:
        lines.append('    { "%s", &scl_%s_prog },' % (tag, tag))
    lines.append('};')
    lines.append(r'''
int main(int argc, char *argv[])
{
    unsigned i;
    const P *p = NULL;
    long guard = 0;
    if (argc < 2) { printf("[NO-TAG]\n"); return 2; }
    for (i = 0; i < (unsigned)(sizeof(s_progs)/sizeof(s_progs[0])); i++)
    {
        if (strcmp(s_progs[i].name, argv[1]) == 0) { p = &s_progs[i]; break; }
    }
    if (p == NULL) { printf("[NO-PROG]\n"); return 2; }
    SCL_Init();
    Scl_Demo_Register();
    if (SCL_RunProg(p->prog) == 0) { printf("[RUN-REJECT]\n"); return 2; }
    while (!SCL_Idle())
    {
        SCL_Loop();
        guard++;
        if (guard > 5000000L)
        {
            SCL_Abort();
            while (!SCL_Idle()) { SCL_Loop(); }
            printf("[RUN-TIMEOUT]\n");
            return 3;
        }
    }
    printf("[RUN-OK]\n");
    return 0;
}
''')
    return "\n".join(lines)


def test_emitc():
    section("4. 预编译 const C 程序（scl_emit_c + SCL_RunProg，省 RAM）")
    try:
        import scl_emit_c as ec
    except Exception as e:  # noqa: BLE001
        check(False, "import scl_emit_c 失败: %s" % e)
        return

    # (tag, 源文件路径 or None, 现代源码 or None, 期望子串)
    cases = [
        ("demo1_if", str(ROOT / "example" / "s2c" / "demo1_if.s2c"), None,
         ["echo mode ok", "echo done", "RUN-OK"]),
        ("demo4_control", str(ROOT / "example" / "s2c" / "demo4_control.s2c"), None,
         ["echo even-sum=20", "echo idle-ok", "echo debug-on", "RUN-OK"]),
        ("arith", None,
         "var int a=7\nvar int b=2\na = a + b\nb = a * 2\necho(\"a=${a} b=${b}\")",
         ["echo a=9 b=18", "RUN-OK"]),
        ("logic", None,
         "var bool p=true\nvar bool q=false\n"
         "if ((p && q) || !q) { echo(Y) } else { echo(N) }",
         ["echo Y", "RUN-OK"]),
        ("forb", None,
         "for (var int i=0; i < 10; i = i + 1) {\n"
         "  if (i == 2) { break }\n  echo(\"b${i}\")\n}\necho(over)",
         ["echo b0", "echo b1", "echo over", "RUN-OK"]),
        ("strst", None,
         'var string st="idle"\nif (st == "ok") { echo(N) } else { echo(Y) }\n'
         'st = "ok"\nif (st == "ok") { echo(now) }',
         ["echo Y", "echo now", "RUN-OK"]),
    ]

    (ROOT / "build").mkdir(exist_ok=True)
    progs = []
    ok = True
    for tag, path, src, subs in cases:
        try:
            if path is not None:
                src_txt = pathlib.Path(path).read_text(encoding="utf-8")
            else:
                src_txt = src
            chain, _ = ec.translate(src_txt)
            bc, argc = ec.encode_chain(chain)
            progs.append((tag, ec.emit_c(tag, bc, argc, tag), subs))
        except Exception as e:  # noqa: BLE001
            check(False, "emit-c %s 生成失败: %s" % (tag, e))
            ok = False
    if not ok:
        return

    # v0.3：const 顶层只读常量回喂
    cases = [
        ("constst", None,
         "const int LIM=5\nvar int r=0\nr = LIM + 1\necho(\"r=${r} L=${LIM}\")",
         ["echo r=6 L=5", "RUN-OK"]),
    ]
    for tag, _p, src, subs in cases:
        try:
            chain, _ = ec.translate(src)
            bc, argc = ec.encode_chain(chain)
            progs.append((tag, ec.emit_c(tag, bc, argc, tag), subs))
        except Exception as e:  # noqa: BLE001
            check(False, "emit-c %s 生成失败: %s" % (tag, e))
            ok = False

    (ROOT / "build" / "_emitc_progs.c").write_text(
        "\n".join(p[1] for p in progs), encoding="utf-8")
    (ROOT / "build" / "_emitc_main.c").write_text(
        _emitc_main_c(progs), encoding="utf-8")

    # 两种变体都验证：默认（动态+预编译都开）与 RUN_TEXT_EN=0（纯预编译，RAM 最小）
    for exe_name, extra_cfg in (("prog_runner.exe", []),
                                ("prog_min.exe", ["-DSCL_CFG_RUN_TEXT_EN=0"])):
        exe = ROOT / "build" / exe_name
        cmd = ["gcc", "-O2", "-pipe"] + RUNNER_CFG + extra_cfg + [
            "-I", str(ROOT / "scl" / "Inc"), "-I", str(ROOT / "scl" / "Src"),
            "-I", str(ROOT / "example"),
            str(ROOT / "scl" / "Src" / "scl.c"),
            str(ROOT / "scl" / "Src" / "scl_var.c"),
            str(ROOT / "scl" / "Src" / "scl_env.c"),
            str(ROOT / "example" / "scl_port.c"),
            str(ROOT / "example" / "demo_cmds.c"),
            str(ROOT / "build" / "_emitc_progs.c"),
            str(ROOT / "build" / "_emitc_main.c"),
            "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0:
            check(False, "emit-c runner(%s) 编译失败" % exe_name)
            err = (r.stderr.decode("utf-8", "replace").splitlines() or [""])[0]
            print("  " + err)
            continue
        for tag, _c, subs in progs:
            try:
                rr = subprocess.run([str(exe), tag], capture_output=True, timeout=60)
                out = (rr.stdout + rr.stderr).decode("utf-8", "replace")
            except OSError:
                out = ""
            miss = [s for s in subs if s not in out]
            check(not miss and "RUN-OK" in out,
                  "emit-c %s [%s] %s%s" %
                  (tag, exe_name, "OK" if not miss else "FAIL",
                   (" missing=%s" % miss) if miss else ""))


def test_feed():
    section("3. 回喂（真实 SCL 执行）")
    if not build_runner():
        return

    # 4 个示例 demo
    demos = [
        ("demo1_if", str(ROOT / "example" / "s2c" / "demo1_if.s2c"),
         ["echo mode ok", "echo done", "RUN-OK"]),
        ("demo2_while", str(ROOT / "example" / "s2c" / "demo2_while.s2c"),
         ["step 100", "echo loop-end", "RUN-OK"]),
        ("demo3_fn", str(ROOT / "example" / "s2c" / "demo3_fn.s2c"),
         ["echo note alpha beta", "echo hit", "echo note line1 line2", "RUN-OK"]),
        ("demo4_control", str(ROOT / "example" / "s2c" / "demo4_control.s2c"),
         ["echo even-sum=20", "echo idle-ok", "echo debug-on", "RUN-OK"]),
    ]
    for tag, path, subs in demos:
        try:
            chain, _ = translate(pathlib.Path(path).read_text(encoding="utf-8"))
        except S2CError as e:
            check(False, "demo %s 转译失败: %s" % (tag, e.msg))
            continue
        out = run_chain(chain, tag)
        miss = [s for s in subs if s not in out]
        check(not miss and "RUN-OK" in out,
              "demo %s 回喂 %s%s" % (tag, "OK" if not miss else "FAIL",
                                     (" missing=%s" % miss) if miss else ""))

    # 现代源 → 链 → 真实执行（do-while 计数 + 算术 + bool）
    feeds = [
        ("dowhile", "var int n=0\nwhile (n < 3) { n = n + 1 }\necho(\"n=${n}\")",
         ["echo n=3", "RUN-OK"]),
        ("arith", "var int a=7\nvar int b=2\na = a + b\nb = a * 2\necho(\"a=${a} b=${b}\")",
         ["echo a=9 b=18", "RUN-OK"]),
        ("typed", "var bool b=true\nvar int i=7\nvar flag f=-x\necho(\"${b}/${i}/${f}\")",
         ["echo true/7/-x", "RUN-OK"]),
        ("cmpif", "var int x=5\nif (x < 10) { echo(small) } else { echo(big) }",
         ["echo small", "RUN-OK"]),
        ("boolif", "var bool ok=true\nif (!ok) { echo(bad) }\nif (ok) { echo(good) }",
         ["echo good", "RUN-OK"]),
        ("logic", "var bool p=true\nvar bool q=false\n"
                   "if ((p && q) || !q) { echo(Y) } else { echo(N) }",
         ["echo Y", "RUN-OK"]),
        ("wlogic", "var int n=0\nvar bool run=true\n"
                    "while (run && n < 2) { n = n + 1; echo(\"n=${n}\") }\n"
                    "echo(done)",
         ["echo n=1", "echo n=2", "echo done", "RUN-OK"]),
        ("forb", "for (var int i=0; i < 10; i = i + 1) {\n"
                 "  if (i == 2) { break }\n  echo(\"b${i}\")\n}\necho(over)",
         ["echo b0", "echo b1", "echo over", "RUN-OK"]),
        ("forc", "for (var int i=0; i < 5; i = i + 1) {\n"
                 "  if (i == 1) { continue }\n  echo(\"c${i}\")\n}",
         ["echo c0", "echo c2", "echo c3", "echo c4", "RUN-OK"]),
        ("wbrk", "var int n=0\nwhile (true) {\n  n = n + 1\n"
                 "  if (n >= 3) { break }\n  echo(\"w${n}\")\n}",
         ["echo w1", "echo w2", "RUN-OK"]),
        ("condbit", "var int r=0xF0\nif ((r & 0x0F) == 0x00) { echo(lo) } else { echo(hi) }\n"
                     "var int a=5\nvar int b=2\nif ((a + b) > 6) { echo(big) }",
         ["echo lo", "echo big", "RUN-OK"]),
        ("strst", "var string st=\"idle\"\nif (st == \"ok\") { echo(Y) } else { echo(N) }\n"
                  "st = \"ok\"\nif (st == \"ok\") { echo(now) }",
         ["echo N", "echo now", "RUN-OK"]),
        ("flagst", "var flag f=-x\nif (f == \"-x\") { echo(has) } else { echo(no) }",
         ["echo has", "RUN-OK"]),
        ("constst", "const int LIM=5\nvar int r=0\nr = LIM + 1\necho(\"r=${r} L=${LIM}\")",
         ["echo r=6 L=5", "RUN-OK"]),
    ]
    for tag, src, subs in feeds:
        try:
            chain, _ = translate(src)
        except S2CError as e:
            check(False, "feed %s 转译失败: %s" % (tag, e.msg))
            continue
        out = run_chain(chain, tag)
        miss = [s for s in subs if s not in out]
        check(not miss and "RUN-OK" in out,
              "feed %s 回喂 %s%s" % (tag, "OK" if not miss else "FAIL",
                                     (" missing=%s" % miss) if miss else ""))


def main():
    print("=== S2C v0.2 转译器测试（类型化 var + 表达式降级） ===")
    test_unit()
    test_error()
    test_feed()
    test_emitc()
    print("\n===== 汇总 =====")
    print("PASS=%d  FAIL=%d" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
