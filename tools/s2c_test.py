#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
s2c_test.py — Script2Chain 转译器测试（v4：输出 label/jump 汇编）

  1) 单元：现代脚本 -> 期望 label/jump 汇编链（精确比对）
  2) 错误：保留字/变量超限/值过长/fn 递归/语法错等应报 S2CError
  3) 回喂：把 example/s2c/*.s2c 与单元链喂给真实 SCL（chain_runner）执行
     （首次运行会尝试用 gcc 编译 example/chain_runner.c）
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

    # if(cond) ... else ...
    unit_exact('if (cmp(1,1)) { echo("a") } else { echo("b") }',
               'cmp 1 1;jump -a L1;echo b;jump L2;label L1;echo a;label L2',
               "if(命令条件)/else → label/jump")

    # while(cond) do-while
    unit_exact("demo_reset(3)\nwhile (demo_inc()) { echo(\"x\") }",
               "demo_reset 3;label L1;echo x;demo_inc;jump -a L1",
               "while(do-while) → label+条件 jump")

    # ret 糖衣 + if(命令) + 引号参数
    unit_exact('ret(1); if (cmp(2,2)) { echo("a b",z) } else { echo("c") }',
               'setret 1;cmp 2 2;jump -a L1;echo c;jump L2;'
               'label L1;echo "a b" z;label L2',
               "ret 糖衣 + if 真走引号多参")

    # 无条件 if{} 只用 else（真则跳过）
    unit_exact('if { } else { echo("n") }',
               'jump -a L1;echo n;label L1',
               "if{} 仅 else：真跳跳过")

    # while(false)：do-while 下 body 执行一次
    unit_exact("while (false) { echo(x) }\necho(ok)",
               "label L1;echo x;setret 0;jump -a L1;echo ok",
               "while(false) do-while：body 一次")

    # fn 参数内联 + 多行实参 + 变量
    unit_exact('var t = "hello world"\n'
               'fn note(x,y){ echo(x,y) }\n'
               'note("p q",2)\necho(${t})',
               'var t="hello world";echo "p q" 2;echo ${t}',
               "fn 参数内联 + 引号多词参数 + 变量")


def unit_err(src, keyword, msg):
    try:
        translate(src)
        check(False, "%s -> 未报错（期望含 %s）" % (msg, keyword))
    except S2CError as e:
        check(keyword in e.msg, "%s -> %s" % (msg, e.msg))


def test_error():
    section("2. 错误用例（应报 S2CError）")
    unit_err("var a=1\nvar b=2\nvar c=3", "超过", "变量存活 >2 报错")
    unit_err("var toolongname=1", "过长", "变量名 >8 报错")
    unit_err("var x=1234567890123456", "过长", "变量字面值 >15 报错")
    unit_err("fn a(){ a() }\na()", "递归", "fn 递归报错")
    unit_err('echo("unclosed', "未闭合", "字符串未闭合报错")
    unit_err("add(1,2", "')'", "缺右括号报错")
    unit_err("x 3", "需要 '('", "裸标识符语句报错")
    unit_err("var if = 1", "保留", "变量名用保留字报错")
    unit_err("while(1){ echo(x) }", "条件", "while 条件非调用报错")
    unit_err("free nobody", "未定义", "free 未定义变量报错")
    unit_err("jump(L1)", "关键字", "运行时关键字 label/jump 不可调用")


# ============================ 3. 回喂（真实 SCL 执行 chain_runner） ============================

RUNNER = ROOT / "build" / "chain_runner"
RUNNER_EXE = RUNNER
if os.name == "nt":
    RUNNER_EXE = RUNNER.with_suffix(".exe")

RUNNER_CFG = ["-DSCL_CFG_SCRIPT_MAX=2048", "-DSCL_CFG_BC_MAX=2048",
              "-DSCL_CFG_ARG_CACHE_MAX=1024", "-DSCL_CFG_LABEL_MAX=64"]


def build_runner():
    (ROOT / "build").mkdir(exist_ok=True)
    cmd = ["gcc", "-O2"] + RUNNER_CFG + [
        "-I", str(ROOT / "scl" / "Inc"),
        "-I", str(ROOT / "example"),
        str(ROOT / "scl" / "Src" / "scl.c"),
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
        r = subprocess.run([str(RUNNER_EXE), str(tmp)],
                           capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return False, "[runner timeout]"
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    return (r.returncode == 0 and "RUN-OK" in out and "RUN-TIMEOUT" not in out), out


def test_backfeed():
    section("3. 回喂（真实 SCL 执行 chain_runner）")
    if not (RUNNER_EXE.exists() and RUNNER_EXE.stat().st_size > 0):
        if not build_runner():
            return

    # 3.1 示例 .s2c 全转全跑
    for f in sorted((ROOT / "example" / "s2c").glob("*.s2c")):
        try:
            chain, _ = translate(f.read_text(encoding="utf-8"))
        except S2CError as e:
            check(False, "%s 转译报错: %s" % (f.name, e.msg))
            continue
        ok, out = run_chain(chain, f.stem)
        check(ok, "回喂 %s (链长 %d)" % (f.name, len(chain)))
        if not ok:
            print("       " + out.replace("\n", " / ")[:400])

    # 3.2 do-while 语义行为：demo2 循环 body 次数
    chain, _ = translate(
        "demo_reset(3)\nfn nd(){ demo_inc() }\nwhile (nd()) { echo(\"step\") }")
    ok, out = run_chain(chain, "dowhile")
    check(ok and out.count("echo step") == 3,
          "回喂 do-while：body 3 次 + RUN-OK")
    if not ok:
        print("       " + out.replace("\n", " / ")[:400])

    # 3.3 if/else 行为：真走 then
    chain, _ = translate(
        'if (cmp(1,1)) { echo("hit") } else { echo("miss") }')
    ok, out = run_chain(chain, "ifelse")
    check(ok and "echo hit" in out and "echo miss" not in out,
          "回喂 if(cond)/else：真走 then")
    if not ok:
        print("       " + out.replace("\n", " / ")[:400])

    # 3.4 while 内嵌 if（引号交替 + 终止性）
    chain, _ = translate(
        "demo_reset(2)\nwhile (demo_inc()) {\n"
        "    if (cmp(1,1)) { echo(\"x\") }\n}\necho(\"end\")")
    ok, out = run_chain(chain, "nest")
    check(ok and out.count("echo x") == 2, "回喂 while 内嵌 if（body×2）")
    if not ok:
        print("       " + out.replace("\n", " / ")[:400])


# ============================ main ============================

def main():
    print("=== Script2Chain 转译器测试（v4 label/jump） ===")
    test_unit()
    test_error()
    test_backfeed()
    print("\n===== 汇总 =====\nPASS=%d  FAIL=%d" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
