#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scl_gen_list.py —— 扫描命令源码里的声明宏，生成注册表 scl_cmd_list.c

设计：
  - 只认两种宏（来自 scl/Inc/scl_reg.h）：
        SCL_CMD_DEFINE(name, fn, sync, help, args)
        SCL_CMD_DEFINE_NA(name, fn, sync, help)
        SCL_VAR_DEFINE(name, type, get, set)
    取**第一个参数**作为符号名；其它参数不解析（保持扫描规则稳定）。
  - 按**文件名排序**扫描，保证生成表顺序确定（影响 help 顺序与 opc 编号）。
  - 目录/文件可传多个；生成物含两个数组 + SCL_RegList_Init()（被 SCL_Init 弱符号调用）。

用法：
  python scl/tool/scl_gen_list.py scl/cmd -o scl/cmd/scl_cmd_list.c
  python scl/tool/scl_gen_list.py scl/cmd app/cmd -o build/scl_cmd_list.c [--quiet]
"""
import argparse
import os
import re
import sys

CMD_MACROS = ("SCL_CMD_DEFINE_NA", "SCL_CMD_DEFINE")
VAR_MACROS = ("SCL_VAR_DEFINE",)
ALL_MACROS = CMD_MACROS + VAR_MACROS

ID_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class GenError(Exception):
    pass


def _strip_comments(text):
    """去掉注释，避免注释里的宏示例被误扫。保留字符串字面量（宏参数里允许字符串）。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")          # 防止把两段标识符粘在一起
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _first_arg(text, pos):
    """从宏名后的 '(' 开始，返回第一层括号内的第一个参数（去空白），并返回结束位置。"""
    n = len(text)
    i = text.find("(", pos)
    if i < 0:
        raise GenError("宏缺少 '('")
    i += 1
    depth = 0
    start = i
    while i < n:
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if c == ")" and depth == 0:
                chunk = text[start:i]
                return chunk.split(",")[0].strip(), i + 1
            depth -= 1
        elif c == "," and depth == 0:
            return text[start:i].strip(), i + 1
        i += 1
    raise GenError("宏括号未闭合")


def scan_text(text, fname):
    """返回 (cmds, vars)：元素为 (name, line)。"""
    clean = _strip_comments(text)
    cmds = []
    vars_ = []
    for macro in ALL_MACROS:
        pos = 0
        while True:
            k = clean.find(macro, pos)
            if k < 0:
                break
            pos = k + len(macro)
            # 前一个字符必须是边界（避免匹配到 SCL_CMD_DEFINE_NA 里的前缀等）
            if k > 0 and (clean[k - 1].isalnum() or clean[k - 1] == "_"):
                continue
            # 长名优先：SCL_CMD_DEFINE 不能吃掉 SCL_CMD_DEFINE_NA
            if macro == "SCL_CMD_DEFINE" and clean.startswith("SCL_CMD_DEFINE_NA", k):
                continue
            try:
                name, _end = _first_arg(clean, pos)
            except GenError as e:
                raise GenError("%s: %s: %s" % (fname, macro, e))
            if not ID_RE.match(name):
                raise GenError("%s: %s 的第一个参数不是合法标识符: %r"
                               % (fname, macro, name))
            line = clean.count("\n", 0, k) + 1
            (cmds if macro in CMD_MACROS else vars_).append((name, line))
    return cmds, vars_


def collect(paths):
    """扫描目录/文件列表，返回 (cmds, vars, files)：均按文件名排序去重。"""
    files = []
    for p in paths:
        if os.path.isdir(p):
            for nm in sorted(os.listdir(p)):
                if nm.endswith(".c"):
                    files.append(os.path.join(p, nm))
        elif os.path.isfile(p):
            files.append(p)
        else:
            raise GenError("路径不存在: %s" % p)
    files = sorted(set(files))

    cmds = []
    vars_ = []
    seen_c = {}
    seen_v = {}
    for f in files:
        with open(f, encoding="utf-8", errors="replace") as fp:
            text = fp.read()
        c, v = scan_text(text, f)
        for nm, ln in c:
            if nm in seen_c:
                raise GenError("命令重名: %s（%s:%d 与 %s:%d）"
                               % (nm, f, ln, seen_c[nm][0], seen_c[nm][1]))
            seen_c[nm] = (f, ln)
            cmds.append((nm, f, ln))
        for nm, ln in v:
            if nm in seen_v:
                raise GenError("变量重名: %s（%s:%d 与 %s:%d）"
                               % (nm, f, ln, seen_v[nm][0], seen_v[nm][1]))
            seen_v[nm] = (f, ln)
            vars_.append((nm, f, ln))
    return cmds, vars_, files


def emit(cmds, vars_, files, out_name):
    L = []
    L.append("/* ===============================================================")
    L.append(" * SCL 注册表：%s（由 scl/tool/scl_gen_list.py 生成，勿手改）" % out_name)
    L.append(" * 命令 %d 条、静态变量 %d 个" % (len(cmds), len(vars_)))
    for nm, f, ln in cmds:
        L.append(" *   cmd  %-16s <- %s:%d" % (nm, f.replace("\\", "/"), ln))
    for nm, f, ln in vars_:
        L.append(" *   var  %-16s <- %s:%d" % (nm, f.replace("\\", "/"), ln))
    L.append(" * =============================================================== */")
    L.append('#include "scl.h"')
    L.append("#if (SCL_CFG_CMDDESC_EN != 0u)")
    for nm, _f, _l in cmds:
        L.append("extern const scl_cmd_desc_t s_desc_%s;" % nm)
    L.append("#endif")
    for nm, _f, _l in cmds:
        L.append("extern scl_cmd_t s_cmd_%s;" % nm)
    for nm, _f, _l in vars_:
        L.append("extern const scl_var_bind_t s_bind_%s;" % nm)
    L.append("")

    # ---- 命令数组（大小由生成器按实际条数决定；空表用 NULL 指针，不占空间） ----
    if cmds:
        L.append("scl_cmd_t * const scl_cmd_list[] = {")
        for nm, _f, _l in cmds:
            L.append("    &s_cmd_%s," % nm)
        L.append("};")
    else:
        L.append("scl_cmd_t * const * const scl_cmd_list = NULL;   /* 空表：NULL 指针 */")
    L.append("const int scl_cmd_list_n = %d;" % len(cmds))
    L.append("")

    # ---- 变量数组（同上：条数决定大小，空表 NULL） ----
    if vars_:
        L.append("const scl_var_bind_t * const scl_var_list[] = {")
        for nm, _f, _l in vars_:
            L.append("    &s_bind_%s," % nm)
        L.append("};")
    else:
        L.append("const scl_var_bind_t * const * const scl_var_list = NULL;   /* 空表：NULL 指针 */")
    L.append("const int scl_var_list_n = %d;" % len(vars_))
    L.append("")

    # ---- 命令描述表（先于注册入口定义，避免先用后定义） ----
    L.append("#if (SCL_CFG_CMDDESC_EN != 0u)")
    L.append("static const scl_cmd_desc_t * const scl_desc_list[] = {")
    if cmds:
        for nm, _f, _l in cmds:
            L.append("    &s_desc_%s," % nm)
    else:
        L.append("    NULL,")
    L.append("};")
    L.append("#endif")
    L.append("")

    # ---- 注册入口（SCL_Init 以弱符号调用；无表时该函数不存在也不报错） ----
    # opcode 由"表内下标"决定：nd->opc = SCL_OP_CMD_BASE + i（不再依赖注册顺序自增）
    L.append("/* 注册表命令数：供库侧校验 opc 预留区间 */")
    L.append("#define SCL_REG_CMD_COUNT %d" % len(cmds))
    L.append("#if (SCL_REG_CMD_COUNT > SCL_CFG_CMD_RESERVE)")
    L.append('#error "注册表命令数超过 SCL_CFG_CMD_RESERVE，请调大该宏（scl_cfg.h）"')
    L.append("#endif")
    L.append("")
    L.append("void SCL_RegList_Init(void)")
    L.append("{")
    L.append("    int i;")
    L.append("    for (i = 0; i < scl_cmd_list_n; i++)")
    L.append("    {")
    L.append("        scl_cmd_t *nd = scl_cmd_list[i];")
    L.append("        if (nd == NULL) { continue; }")
    L.append("        nd->opc = (uint16_t)(SCL_CFG_OP_CMD_BASE + i);   /* opcode = 表内下标 */")
    L.append("#if (SCL_CFG_CMDDESC_EN != 0u)")
    L.append("        SCL_CmdRegisterDesc(nd, scl_desc_list[i]);")
    L.append("#else")
    L.append("        SCL_RegisterCmd(nd);")
    L.append("#endif")
    L.append("    }")
    L.append("    for (i = 0; i < scl_var_list_n; i++)")
    L.append("    {")
    L.append("        SCL_VarBindOne(scl_var_list[i]);")
    L.append("    }")
    L.append("}")
    L.append("")
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description="SCL 注册表生成器（命令/静态变量）")
    ap.add_argument("paths", nargs="+", help="要扫描的目录或 .c 文件（可多个）")
    ap.add_argument("-o", "--output", required=True, help="输出 scl_cmd_list.c")
    ap.add_argument("--quiet", action="store_true", help="不打印扫描清单")
    a = ap.parse_args(argv)

    try:
        cmds, vars_, files = collect(a.paths)
    except GenError as e:
        print("scl_gen_list: error: %s" % e, file=sys.stderr)
        return 1

    text = emit(cmds, vars_, files, os.path.basename(a.output))
    os.makedirs(os.path.dirname(os.path.abspath(a.output)), exist_ok=True)
    with open(a.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    if not a.quiet:
        print("scl_gen_list: 扫描 %d 个文件 -> %s" % (len(files), a.output))
        for nm, f, ln in cmds:
            print("  cmd  %-16s %s:%d" % (nm, f, ln))
        for nm, f, ln in vars_:
            print("  var  %-16s %s:%d" % (nm, f, ln))
        print("scl_gen_list: 命令 %d 条、静态变量 %d 个" % (len(cmds), len(vars_)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
