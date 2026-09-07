#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
s2c_test.py — Script2Chain 转译器测试

内容：
  1) 单元：现代脚本 -> 期望指令链（精确比对）
  2) 错误：保留字/变量超限/值过长/fn 递归/语法错等应报 S2CError
  3) 回喂：把 example/s2c/*.s2c 转出的链喂给真实 SCL（chain_runner）执行
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


# ============================ 1. 单元：精确链比对 ============================

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
    section("1. 单元（精确链比对）")

    # if/else 无条件（沿用 G_RETURN）
    unit_exact(
        "var sp=100\ncmp(1,${sp})\n"
        "if {\n echo(${sp},\"ok\")\n} else {\n echo(${sp},\"ng\")\n}",
        'var sp=100;cmp(1,${sp});if -t "echo(${sp},ok)" -f "echo(${sp},ng)"',
        "if/else 无条件 + 变量/字符串参数")

    # while(cond)（C 语义）+ fn 作条件
    unit_exact(
        "var sp=100\nfn not_done() {\n demo_inc()\n}\n"
        "demo_reset(3)\nwhile (not_done()) {\n echo(${sp})\n}",
        'var sp=100;demo_reset(3);demo_inc();'
        'if -t "while -b;echo(${sp});demo_inc();while -e"',
        "while(用户fn) 门控 do-while")

    # ret 糖衣 + if(命令) + 分支内引号嵌套 + 多语句 ';'
    unit_exact(
        'ret(1); if (cmp(2,2)) { echo("a b",x) } else { echo("c") }; false',
        'setret(1);cmp(2,2);if -t "echo(\'a b\',x)" -f "echo(c)";setret(0)',
        "ret/false 糖衣 + if(命令) + 嵌套引号自动交替")

    # while(false) 编译为空（不执行）
    unit_exact("while (false) { echo(x) }\necho(ok)",
               "echo(ok)",
               "while(false) 编译为空")

    # 多行实参 + 引号字符串
    unit_exact('note("line1",\n     "line2")',
               'note(line1,line2)',
               "多行实参（无空格字符串转裸词）")

    # 值带空格（引号包裹，SCL var 可用）
    unit_exact('var t = "hello world"\necho(${t})',
               'var t="hello world";echo(${t})',
               "变量值含空格")


# ============================ 2. 错误用例 ============================

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

    # 正常用例：fn 体内 var 也能编译（仅一次调用，槽数不超）
    try:
        chain, _ = translate("var a=1\nfn f(){ var b=2 }\nf()")
        check(chain == "var a=1;var b=2", "fn 内 var 正常编译(单次调用)")
    except S2CError as e:
        check(False, "fn 内 var 编译失败: %s" % e.msg)


# ============================ 3. 回喂（真实 SCL 执行） ============================

RUNNER = ROOT / "build" / "chain_runner"
RUNNER_EXE = RUNNER
if os.name == "nt":
    RUNNER_EXE = RUNNER.with_suffix(".exe")


def build_runner():
    (ROOT / "build").mkdir(exist_ok=True)
    cmd = [
        "gcc", "-O2", "-DSCL_CFG_SCRIPT_MAX=2048",
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
        print("  " + (r.stderr.decode("utf-8", "replace").splitlines() or [""])[0])
        return False
    return True


def run_chain(chain, tag):
    """把链写入临时文件并交给 chain_runner 执行；返回 (ok, output)"""
    tmp = ROOT / "build" / ("_feed_%s.chain" % tag)
    tmp.write_text(chain, encoding="utf-8")
    try:
        r = subprocess.run([str(RUNNER_EXE), str(tmp)],
                           capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return False, "[runner timeout]"
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    return (r.returncode == 0 and "RUN-OK" in out), out


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
        if not ok and FAIL > 0:
            print("       " + out.replace("\n", " / ")[:400])

    # 3.2 单元里的两条代表性链也真实跑
    chain2 = ('var sp=100;demo_reset(3);demo_inc();'
              'if -t "while -b;echo(${sp});demo_inc();while -e"')
    ok, out = run_chain(chain2, "unit2")
    check(ok, "回喂 while(fn) 单元链")
    if not ok:
        print("       " + out.replace("\n", " / ")[:400])

    chain3 = 'setret(1);cmp(2,2);if -t "echo(\'a b\',x)" -f "echo(c)";setret(0)'
    ok, out = run_chain(chain3, "unit3")
    check(ok, "回喂 ret/if(嵌套引号) 单元链")
    if not ok:
        print("       " + out.replace("\n", " / ")[:400])


# ============================ main ============================

def main():
    print("=== Script2Chain 转译器测试 ===")
    test_unit()
    test_error()
    test_backfeed()
    print("\n===== 汇总 =====\nPASS=%d  FAIL=%d" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
