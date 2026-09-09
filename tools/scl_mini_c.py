#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scl_mini_c.py —— mini-scl：把指令链/现代语法脚本编译成自包含 switch 状态机 C

与 scl_emit_c.py（const 字节码 + 解释器 SCL_RunProg）不同，mini 直接产出
"switch 状态机 C"：
  - 脚本 var → 生成的 .c 内 static 文本缓冲（+def 标志）；用户可见变量每次写入
    同时 SCL_VarSetT 镜像，外部仍可用 SCL_VarGet 读取（观感与解释器一致）。
  - 每条链指令 → step() 的 switch 一个 case；每次调用只执行一个动作（非阻塞）。
  - label/jump/jump -a → 状态跳转；jump -a 用库 SCL_Ret_Get（命令副作用置位）。
  - 业务命令调用 → SCL_CmdInvoke()（按名调用，desc 校验同解释器）；异步命令返回 2
    → step 自持等待态，用 SCL_AsyncPoll 轮询到完成再前进。
  - 运行时不再需要：文本编译器 / 字节码解释器 / label 表 / ${} 展开表
    （mini 自带 Mini_Exp 处理 ${}：脚本 var 优先，否则 SCL_VarGet 兜底含 env）。

用法：
  python tools/scl_mini_c.py boot.s2c -o boot_mini.c [--name boot]
  python tools/scl_mini_c.py --chain boot.chain -o boot_mini.c
  # 也可由 scl_emit_c.py --mini 调用（同一 emit_mini_c）
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scl_script2chain import translate, S2CError  # noqa: E402

# ---------- 与 scl_emit_c.py 一致的字面量/链原语 ----------
T_BOOL = 0x01
T_INT = 0x02
T_FLAG = 0x03
T_STR = 0x04

OPWORDS = [
    ("iadd", 0x10), ("isub", 0x11), ("imul", 0x12), ("idiv", 0x13), ("imod", 0x14), ("ineg", 0x15),
    ("ieq", 0x16), ("ine", 0x17), ("igt", 0x18), ("ige", 0x19), ("ilt", 0x1A), ("ile", 0x1B),
    ("band", 0x1C), ("bor", 0x1D), ("bnot", 0x1E), ("btest", 0x1F),
    ("iand", 0x20), ("ior", 0x21), ("ixor", 0x22), ("inot", 0x23), ("shl", 0x24), ("shr", 0x25),
    ("seq", 0x26), ("sneq", 0x27),
]


class MiniError(Exception):
    """mini 生成错误。"""


def _is_sp(c):
    return c in " \t\r\n"


def _is_ascii_digit(c):
    return "0" <= c <= "9"


def _parse_i32(s):
    """十进制/0x/0b → int32 或 None（与解释器一致）。"""
    if not s:
        return None
    if len(s) > 2 and s[0] == "0" and s[1] in "xXbB":
        base = 16 if s[1] in "xX" else 2
        v = 0
        for ch in s[2:]:
            if _is_ascii_digit(ch):
                d = ord(ch) - 48
            elif "a" <= ch <= "f":
                d = ord(ch) - 87
            elif "A" <= ch <= "F":
                d = ord(ch) - 55
            else:
                return None
            if d >= base:
                return None
            if v > (0xFFFFFFFF - d) // base:
                return None
            v = v * base + d
        if v > 0x7FFFFFFF:
            v -= 0x100000000
        return v
    i = 0
    neg = False
    if s[0] == "-":
        neg = True
        i = 1
        if i >= len(s):
            return None
    v = 0
    lim = 2147483648 if neg else 2147483647
    for ch in s[i:]:
        if not _is_ascii_digit(ch):
            return None
        d = ord(ch) - 48
        if v > (lim - d) // 10:
            return None
        v = v * 10 + d
    return (-v) if neg else v


def _lit_payload(tok):
    """token → (type, payload)；裸 token 类型化同解释器。"""
    if len(tok) == 2 and tok[0] == "-" and ("a" <= tok[1] <= "z" or "A" <= tok[1] <= "Z"):
        return (T_FLAG, bytes([ord(tok[1])]))
    low = tok.lower()
    if low == "true":
        return (T_BOOL, bytes([1]))
    if low == "false":
        return (T_BOOL, bytes([0]))
    iv = _parse_i32(tok)
    if iv is not None:
        u = iv & 0xFFFFFFFF
        return (T_INT, bytes([(u >> 24) & 0xFF, (u >> 16) & 0xFF, (u >> 8) & 0xFF, u & 0xFF]))
    return (T_STR, tok.encode("utf-8"))


def _clauses(chain):
    """链文本 → 子句原文（引号内 ';' 不分割）。"""
    out = []
    n = len(chain)
    p = 0
    while True:
        while p < n and (chain[p] == ";" or _is_sp(chain[p])):
            p += 1
        if p >= n:
            break
        cs = p
        q = None
        while p < n:
            ch = chain[p]
            if ch == ";" and q is None:
                break
            if ch in ('"', "'"):
                if q is None:
                    q = ch
                elif q == ch:
                    q = None
            p += 1
        ce = p
        if p < n and chain[p] == ";":
            p += 1
        out.append(chain[cs:ce])
    return out


def _split_head(clause):
    """→ (head, head_end_index, 参数原文)；'#' 注释返回 (None,None,None)。"""
    n = len(clause)
    he = 0
    while he < n and not _is_sp(clause[he]):
        he += 1
    if he > 0 and clause[0] == "#":
        return (None, None, None)
    rs = he
    while rs < n and _is_sp(clause[rs]):
        rs += 1
    re = n
    while re > rs and _is_sp(clause[re - 1]):
        re -= 1
    return (clause[0:he], rs, clause[rs:re])


# ---------- mini 解析 / 生成 ----------
_INT_BIN = {"iadd", "isub", "imul", "idiv", "imod", "iand", "ior", "ixor", "shl", "shr"}
_INT_UN = {"ineg", "inot"}
_CMP = {"ieq", "ine", "igt", "ige", "ilt", "ile"}
_BOOL2 = {"band", "bor"}
_BOOL1 = {"bnot", "btest"}
_STRCMP = {"seq", "sneq"}


def _cstr(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return '"%s"' % "".join(out)


def _toks(raw):
    out = []
    n = len(raw)
    i = 0
    while i < n:
        while i < n and _is_sp(raw[i]):
            i += 1
        if i >= n:
            break
        c = raw[i]
        if c in ('"', "'"):
            q = c
            i += 1
            j = raw.find(q, i)
            if j < 0:
                raise MiniError("引号未闭合")
            out.append(raw[i:j])
            i = j + 1
        else:
            j = i
            while j < n and not _is_sp(raw[j]) and raw[j] not in ('"', "'"):
                j += 1
            out.append(raw[i:j])
            i = j
    return out


def _canon(tok):
    ty, payload = _lit_payload(tok)
    if ty == T_BOOL:
        return (T_BOOL, "true" if payload[0] else "false")
    if ty == T_INT:
        v = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3]
        if v > 0x7FFFFFFF:
            v -= 0x100000000
        return (T_INT, str(v))
    if ty == T_FLAG:
        return (T_FLAG, "-" + chr(payload[0]))
    return (T_STR, tok)


def _parse_var_decl(raw):
    raw = raw.strip()
    if raw == "" or raw == "free":
        return None
    eq = raw.find("=")
    if eq < 0:
        return None
    head = raw[:eq].strip()
    val = raw[eq + 1:].strip()
    if len(val) >= 2 and val[0] in ('"', "'") and val[-1] == val[0]:
        val = val[1:-1]
    hd = head.split()
    isc = False
    if hd and hd[0] == "const":
        isc = True
        hd = hd[1:]
    if len(hd) != 2:
        raise MiniError("var 声明形态不合法: %r" % raw)
    typ, nm = hd[0], hd[1]
    if typ not in ("bool", "int", "flag", "string"):
        raise MiniError("var 类型不合法: %r" % typ)
    return (isc, nm, typ, val)


def _mini_insts(chain):
    insts = []
    labels = {}
    for cl in _clauses(chain):
        head, _rs, raw = _split_head(cl)
        if head is None:
            continue
        if head == "label":
            nm = raw.strip()
            if not nm:
                raise MiniError("label: 名不合法")
            if nm in labels:
                raise MiniError("label 重名: %s" % nm)
            labels[nm] = len(insts)
            continue
        if head == "jump":
            parts = raw.split(None, 1)
            cond = False
            if parts and parts[0].startswith("-") and len(parts[0]) == 2 and parts[0][1] in "ab":
                cond = (parts[0][1] == "a")
                tgt = parts[1].strip() if len(parts) > 1 else ""
            else:
                tgt = raw.strip()
            if not tgt:
                raise MiniError("jump: 缺少目标 label")
            insts.append({"kind": "jumpa" if cond else "jump", "tgt": tgt})
            continue
        if head == "callf":
            tgt = raw.strip()
            if not tgt:
                raise MiniError("callf: 缺少目标 label")
            insts.append({"kind": "callf", "tgt": tgt})
            continue
        if head == "retf":
            insts.append({"kind": "retf"})
            continue
        opw = None
        for (w, _o) in OPWORDS:
            if head == w:
                opw = w
                break
        if opw is not None:
            insts.append({"kind": "op", "op": opw, "toks": _toks(raw)})
            continue
        if head == "var":
            pv = _parse_var_decl(raw)
            if pv is None:
                raise MiniError("mini: var 列表/查询形态不支持（raw=%r）" % raw)
            isc, nm, typ, val = pv
            if typ == "int":
                iv = _parse_i32(val)
                if iv is None:
                    raise MiniError("var int %s 初值非数值: %r" % (nm, val))
                val = str(iv)
            elif typ == "bool":
                val = "true" if val.lower() in ("true", "1") else "false"
            insts.append({"kind": "vardecl", "name": nm, "type": typ,
                          "const": isc, "val": val})
            continue
        if head == "free":
            nm = raw.strip()
            insts.append({"kind": "free1", "name": nm} if nm else {"kind": "freeall"})
            continue
        if head in ("help", "cache"):
            raise MiniError("mini: meta '%s' 在自包含模式不支持" % head)
        if not (0 < len(head) < 256):
            raise MiniError("命令名不合法: %r" % head)
        insts.append({"kind": "call", "name": head,
                      "args": [_canon(t) for t in _toks(raw)]})
    return insts, labels


def emit_mini_c(name, chain, source_note):
    insts, labels = _mini_insts(chain)
    if not insts:
        raise MiniError("无指令")
    n = len(insts)

    for it in insts:
        if it["kind"] in ("jump", "jumpa", "callf"):
            if it["tgt"] not in labels:
                raise MiniError("%s: label '%s' 未定义" % (it["kind"], it["tgt"]))
            it["st"] = labels[it["tgt"]]

    sym = {}
    for it in insts:
        if it["kind"] == "vardecl":
            nm = it["name"]
            internal = nm.startswith("__")
            old = sym.get(nm)
            if old is None or (it["const"] and not old["const"]):
                sym[nm] = {"type": it["type"], "const": it["const"], "internal": internal}

    L = []
    L.append("/* ===============================================================")
    L.append(" * SCL mini 自包含状态机程序：%s（由 tools/scl_mini_c.py / scl_emit_c.py --mini 生成，勿手改）" % name)
    L.append(" * 来源：%s" % source_note)
    L.append(" * 用法：SCL_Init(); <注册所需命令>; %s_mini_start();" % name)
    L.append(" *       然后周期调用 %s_mini_step()（每次一个动作，非阻塞）；返回 0 表示完成。" % name)
    L.append(" * 特性：脚本变量为 static 文本缓冲；用户可见变量写入时 SCL_VarSetT 镜像，")
    L.append(" *       外部可用 SCL_VarGet 读取（观感与解释器一致）。")
    L.append(" * 裁剪：本文件自包含步进，不再需要文本编译器/字节码解释器/label 表。")
    L.append(" * ===============================================================")
    L.append(" */")
    L.append('#include "scl.h"')
    L.append("")
    L.append("#define MINI_VLEN  16u          /* 变量文本缓冲（含 '\\0'） */")
    L.append("#define MINI_VN    %du          /* 脚本变量个数 */" % len(sym))
    L.append("#define MINI_NST   %du          /* 状态数（超界=完成） */" % n)
    L.append("#define MINI_STEP_LIMIT 1000000u")
    L.append("/* 静态助手可能在某个程序里用不到：抑制未用告警（GCC/Clang；其它编译器为空） */")
    L.append("#if defined(__GNUC__) || defined(__clang__)")
    L.append("#define MINI_UNUSED __attribute__((unused))")
    L.append("#else")
    L.append("#define MINI_UNUSED")
    L.append("#endif")
    L.append("")

    L.append("/* ---- 脚本变量（static；用户可见者写入即镜像到 SCL） ---- */")
    L.append("typedef struct { const char *name; char *txt; uint8_t *def; uint8_t tag; } mini_vref_t;")
    L.append("/* tag: bit0=const 只读；bit1=内部临时变量（不镜像） */")
    if sym:
        for nm in sym:
            L.append("static char m_%s[MINI_VLEN];" % nm)
            L.append("static uint8_t m_%s_def;" % nm)
        L.append("static mini_vref_t s_vs[MINI_VN] = {")
        for nm in sym:
            s = sym[nm]
            tag = (1 if s["const"] else 0) | (2 if s["internal"] else 0)
            L.append('    { %s, m_%s, &m_%s_def, 0x%02X },'
                     % (_cstr(nm), nm, nm, tag))
        L.append("};")
    else:
        L.append("static mini_vref_t s_vs[MINI_VN];")
    L.append("")

    L.append("/* ---- 运行状态 ---- */")
    L.append("static uint16_t s_st;      /* 当前状态 */")
    L.append("static uint32_t s_steps;   /* 已推进动作数（步限保护） */")
    L.append("static uint8_t  s_wait;    /* 异步命令等待中 */")
    L.append("static uint16_t s_pend;    /* 异步完成后进入的状态 */")
    L.append("static uint8_t  s_fault;   /* 运行错误（如除零/写 const） */")
    L.append("static uint16_t s_retst;   /* callf 返回点（单层子程序） */")
    L.append("")

    # ---------- 无 libc 运行时助手 ----------
    L.append("/* ---- 无 libc 小助手 ---- */")
    L.append("static MINI_UNUSED unsigned Mini_Len(const char *s)")
    L.append("{")
    L.append("    unsigned n = 0u;")
    L.append("    while (s[n] != '\\0') { n++; }")
    L.append("    return n;")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED uint8_t Mini_Eq(const char *a, const char *b)")
    L.append("{")
    L.append("    unsigned i;")
    L.append("    for (i = 0u; a[i] != '\\0' && b[i] != '\\0'; i++)")
    L.append("    {")
    L.append("        if (a[i] != b[i]) { return 0u; }")
    L.append("    }")
    L.append("    return (a[i] == b[i]) ? 1u : 0u;")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED void Mini_Copy(char *dst, const char *src, unsigned cap)")
    L.append("{")
    L.append("    unsigned i = 0u;")
    L.append("    while ((i + 1u < cap) && (src[i] != '\\0')) { dst[i] = src[i]; i++; }")
    L.append("    dst[i] = '\\0';")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED int32_t Mini_Num(const char *s, int *ok)")
    L.append("{")
    L.append("    int32_t v = 0;")
    L.append("    int neg = 0;")
    L.append("    int base = 10;")
    L.append("    int i = 0;")
    L.append("    *ok = 1;")
    L.append("    if (s == NULL) { *ok = 0; return 0; }")
    L.append("    if (s[i] == '-') { neg = 1; i++; }")
    L.append("    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }")
    L.append("    else if (s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) { base = 2; i += 2; }")
    L.append("    if (s[i] == '\\0') { *ok = 0; return 0; }")
    L.append("    while (s[i] != '\\0')")
    L.append("    {")
    L.append("        int d;")
    L.append("        char c = s[i];")
    L.append("        if (c >= '0' && c <= '9') { d = c - '0'; }")
    L.append("        else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }")
    L.append("        else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }")
    L.append("        else { *ok = 0; return 0; }")
    L.append("        if (d >= base) { *ok = 0; return 0; }")
    L.append("        if (v > (0x7FFFFFFF - d) / base) { *ok = 0; return 0; }")
    L.append("        v = v * base + d;")
    L.append("        i++;")
    L.append("    }")
    L.append("    return neg ? -v : v;")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED void Mini_Fmt(int32_t val, char *dst, unsigned cap)")
    L.append("{")
    L.append("    char t[12];")
    L.append("    unsigned k = 0u;")
    L.append("    uint32_t u;")
    L.append("    unsigned i;")
    L.append("    unsigned off;")
    L.append("    if (val < 0) { off = 1u; u = (uint32_t)(-(val + 1)) + 1u; }")
    L.append("    else { off = 0u; u = (uint32_t)val; }")
    L.append("    if (u == 0u) { t[k++] = '0'; }")
    L.append("    while (u != 0u) { t[k++] = (char)('0' + (u % 10u)); u /= 10u; }")
    L.append("    if (off != 0u && off < cap) { dst[0] = '-'; }")
    L.append("    for (i = 0u; i < k && off + i + 1u < cap; i++)")
    L.append("    {")
    L.append("        dst[off + i] = t[k - 1u - i];")
    L.append("    }")
    L.append("    dst[(off + (k < cap - off ? k : cap - 1u - off))] = '\\0';")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED int Mini_VFind(const char *name)")
    L.append("{")
    L.append("    unsigned i;")
    L.append("    for (i = 0u; i < MINI_VN; i++)")
    L.append("    {")
    L.append("        if (s_vs[i].name != NULL && Mini_Eq(s_vs[i].name, name)) { return (int)i; }")
    L.append("    }")
    L.append("    return -1;")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED int Mini_Set(const char *name, uint8_t type, const char *txt, uint8_t isconst)")
    L.append("{")
    L.append("    int i = Mini_VFind(name);")
    L.append("    if (i < 0) { return -1; }")
    L.append("    if ((s_vs[i].tag & 0x01u) != 0u && isconst == 0u) { return -1; }")
    L.append("    Mini_Copy(s_vs[i].txt, txt, MINI_VLEN);")
    L.append("    *s_vs[i].def = 1u;")
    L.append("    if ((s_vs[i].tag & 0x02u) == 0u)")
    L.append("    {")
    L.append("        if (isconst != 0u) { (void)SCL_VarSetConst(name, type, txt); }")
    L.append("        else { (void)SCL_VarSetT(name, type, txt); }")
    L.append("    }")
    L.append("    return 0;")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED void Mini_Free(const char *name)")
    L.append("{")
    L.append("    int i = Mini_VFind(name);")
    L.append("    if (i < 0) { return; }")
    L.append("    if ((s_vs[i].tag & 0x01u) != 0u) { return; }")
    L.append("    s_vs[i].txt[0] = '\\0';")
    L.append("    *s_vs[i].def = 0u;")
    L.append("    if ((s_vs[i].tag & 0x02u) == 0u) { (void)SCL_VarFree(name); }")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED int32_t Mini_NumTok(const char *tok, int *ok)")
    L.append("{")
    L.append("    int i;")
    L.append("    const char *v;")
    L.append("    *ok = 1;")
    L.append("    i = Mini_VFind(tok);")
    L.append("    if (i >= 0)")
    L.append("    {")
    L.append("        if (*s_vs[i].def == 0u) { *ok = 0; return 0; }")
    L.append("        return Mini_Num(s_vs[i].txt, ok);")
    L.append("    }")
    L.append("    if (Mini_Eq(tok, \"true\")) { return 1; }")
    L.append("    if (Mini_Eq(tok, \"false\")) { return 0; }")
    L.append("    if (tok[0] == '$' && tok[1] == '{')")
    L.append("    {")
    L.append("        const char *nm = tok + 2;")
    L.append("        unsigned len = Mini_Len(nm);")
    L.append("        char nb[24]; unsigned k;")
    L.append("        if (len == 0u || nm[len - 1u] != '}') { *ok = 0; return 0; }")
    L.append("        for (k = 0u; k + 1u < len && k + 1u < sizeof(nb); k++) { nb[k] = nm[k]; }")
    L.append("        nb[k] = '\\0';")
    L.append("        v = SCL_VarGet(nb);")
    L.append("        if (v == NULL) { *ok = 0; return 0; }")
    L.append("        if (Mini_Eq(v, \"true\")) { return 1; }")
    L.append("        if (Mini_Eq(v, \"false\")) { return 0; }")
    L.append("        return Mini_Num(v, ok);")
    L.append("    }")
    L.append("    v = SCL_VarGet(tok);")
    L.append("    if (v != NULL)")
    L.append("    {")
    L.append("        if (Mini_Eq(v, \"true\")) { return 1; }")
    L.append("        if (Mini_Eq(v, \"false\")) { return 0; }")
    L.append("        return Mini_Num(v, ok);")
    L.append("    }")
    L.append("    return Mini_Num(tok, ok);")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED uint8_t Mini_TruthTok(const char *tok)")
    L.append("{")
    L.append("    int i = Mini_VFind(tok);")
    L.append("    const char *v;")
    L.append("    int ok;")
    L.append("    if (i >= 0)")
    L.append("    {")
    L.append("        if (*s_vs[i].def == 0u) { return 0u; }")
    L.append("        v = s_vs[i].txt;")
    L.append("    }")
    L.append("    else if (tok[0] == '$' && tok[1] == '{')")
    L.append("    {")
    L.append("        const char *nm = tok + 2;")
    L.append("        unsigned len = Mini_Len(nm);")
    L.append("        char nb[24]; unsigned k;")
    L.append("        if (len == 0u || nm[len - 1u] != '}') { return 0u; }")
    L.append("        for (k = 0u; k + 1u < len && k + 1u < sizeof(nb); k++) { nb[k] = nm[k]; }")
    L.append("        nb[k] = '\\0';")
    L.append("        v = SCL_VarGet(nb);")
    L.append("        if (v == NULL) { return 0u; }")
    L.append("    }")
    L.append("    else")
    L.append("    {")
    L.append("        v = SCL_VarGet(tok);")
    L.append("        if (v == NULL) { v = tok; }")
    L.append("    }")
    L.append("    if (Mini_Eq(v, \"true\")) { return 1u; }")
    L.append("    if (Mini_Eq(v, \"false\")) { return 0u; }")
    L.append("    ok = 0;")
    L.append("    if (v[0] != '\\0' && Mini_Num(v, &ok) != 0 && ok) { return 1u; }")
    L.append("    if (ok) { return 0u; }")
    L.append("    if (v[0] == '\\0') { return 0u; }")
    L.append("    if (Mini_Eq(v, \"0\")) { return 0u; }")
    L.append("    return 1u;")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED const char *Mini_TokText(const char *tok)")
    L.append("{")
    L.append("    int i = Mini_VFind(tok);")
    L.append("    const char *v;")
    L.append("    if (i >= 0) { return (*s_vs[i].def != 0u) ? s_vs[i].txt : \"\"; }")
    L.append("    if (tok[0] == '$' && tok[1] == '{')")
    L.append("    {")
    L.append("        const char *nm = tok + 2;")
    L.append("        unsigned len = Mini_Len(nm);")
    L.append("        static char nb[24]; unsigned k;")
    L.append("        if (len == 0u || nm[len - 1u] != '}') { return \"\"; }")
    L.append("        for (k = 0u; k + 1u < len && k + 1u < sizeof(nb); k++) { nb[k] = nm[k]; }")
    L.append("        nb[k] = '\\0';")
    L.append("        v = SCL_VarGet(nb);")
    L.append("        return (v != NULL) ? v : \"\";")
    L.append("    }")
    L.append("    v = SCL_VarGet(tok);")
    L.append("    return (v != NULL) ? v : tok;")
    L.append("}")
    L.append("")
    L.append("static MINI_UNUSED void Mini_Exp(const char *tpl, char *dst, unsigned cap)")
    L.append("{")
    L.append("    unsigned di = 0u;")
    L.append("    unsigned i = 0u;")
    L.append("    while (tpl[i] != '\\0')")
    L.append("    {")
    L.append("        if (tpl[i] == '$' && tpl[i + 1] == '{')")
    L.append("        {")
    L.append("            unsigned s2 = i + 2u;")
    L.append("            unsigned e2 = s2;")
    L.append("            const char *val;")
    L.append("            while (tpl[e2] != '\\0' && tpl[e2] != '}') { e2++; }")
    L.append("            if (tpl[e2] == '}')")
    L.append("            {")
    L.append("                char nb[24];")
    L.append("                unsigned k = 0u;")
    L.append("                unsigned j;")
    L.append("                while (s2 + k < e2 && k + 1u < sizeof(nb)) { nb[k] = tpl[s2 + k]; k++; }")
    L.append("                nb[k] = '\\0';")
    L.append("                j = (unsigned)Mini_VFind(nb);")
    L.append("                val = (j < MINI_VN && *s_vs[j].def != 0u) ? s_vs[j].txt : SCL_VarGet(nb);")
    L.append("                if (val != NULL)")
    L.append("                {")
    L.append("                    while (*val != '\\0' && di + 1u < cap) { dst[di++] = *val++; }")
    L.append("                }")
    L.append("                i = e2 + 1u;")
    L.append("                continue;")
    L.append("            }")
    L.append("        }")
    L.append("        if (di + 1u < cap) { dst[di++] = tpl[i]; }")
    L.append("        i++;")
    L.append("    }")
    L.append("    if (di < cap) { dst[di] = '\\0'; }")
    L.append("}")
    L.append("")

    # ---------- switch 状态机 ----------
    L.append("/* ---- step：每次调用执行一个动作（非阻塞） ---- */")
    L.append("uint8_t %s_mini_step(void)" % name)
    L.append("{")
    L.append("    if (s_wait != 0u)")
    L.append("    {")
    L.append("        int p = SCL_AsyncPoll();")
    L.append("        if (p == 0) { return 1u; }")
    L.append("        s_wait = 0u;")
    L.append("        s_st = s_pend;")
    L.append("    }")
    L.append("    if (s_st >= MINI_NST) { return 0u; }")
    L.append("    if (s_steps++ >= MINI_STEP_LIMIT) { s_fault = 1u; goto mini_done; }")
    L.append("    switch (s_st)")
    L.append("    {")
    for idx, it in enumerate(insts):
        for ln in _mini_case_c(idx, it):
            L.append("    " + ln)
    L.append("    default:")
    L.append("        s_st = MINI_NST;")
    L.append("        break;")
    L.append("    }")
    L.append("mini_done:")
    L.append("    if (s_fault != 0u)")
    L.append("    {")
    L.append("        s_fault = 0u;")
    L.append("        s_st = MINI_NST;")
    L.append("        return 0u;")
    L.append("    }")
    L.append("    return (s_st < MINI_NST) ? 1u : 0u;")
    L.append("}")
    L.append("")
    L.append("void %s_mini_start(void)" % name)
    L.append("{")
    L.append("    SCL_Ret_Set(0);")
    L.append("    s_st = 0u;")
    L.append("    s_steps = 0u;")
    L.append("    s_wait = 0u;")
    L.append("    s_pend = 0u;")
    L.append("    s_fault = 0u;")
    L.append("    s_retst = 0u;")
    L.append("}")
    L.append("")
    L.append("uint8_t %s_mini_busy(void)" % name)
    L.append("{")
    L.append("    return (s_st < MINI_NST) ? 1u : 0u;")
    L.append("}")
    L.append("")
    return "\n".join(L)


def _mini_case_c(idx, it):
    L = ["case %d:" % idx, "{"]
    k = it["kind"]
    if k == "vardecl":
        tyc = {"bool": "SCL_T_BOOL", "int": "SCL_T_INT",
               "flag": "SCL_T_FLAG", "string": "SCL_T_STR"}[it["type"]]
        L.append('    if (Mini_Set(%s, %s, %s, %s) != 0) { s_fault = 1u; goto mini_done; }'
                 % (_cstr(it["name"]), tyc, _cstr(it["val"]), "1u" if it["const"] else "0u"))
        L.append("    s_st = %du;" % (idx + 1))
    elif k == "freeall":
        L.append("    { unsigned j;")
        L.append("      for (j = 0u; j < MINI_VN; j++)")
        L.append("      { if ((s_vs[j].tag & 0x01u) == 0u) { s_vs[j].txt[0] = '\\0'; *s_vs[j].def = 0u;")
        L.append("        if ((s_vs[j].tag & 0x02u) == 0u) { (void)SCL_VarFree(s_vs[j].name); } } }")
        L.append("    s_st = %du;" % (idx + 1))
    elif k == "free1":
        L.append("    Mini_Free(%s);" % _cstr(it["name"]))
        L.append("    s_st = %du;" % (idx + 1))
    elif k == "jump":
        L.append("    s_st = %du;" % it["st"])
    elif k == "jumpa":
        L.append("    if (SCL_Ret_Get() != 0)")
        L.append("    { SCL_Ret_Set(0); s_st = %du; }" % it["st"])
        L.append("    else")
        L.append("    { SCL_Ret_Set(0); s_st = %du; }" % (idx + 1))
    elif k == "callf":
        L.append("    s_retst = %du;" % (idx + 1))
        L.append("    s_st = %du;" % it["st"])
    elif k == "retf":
        L.append("    if (s_retst != 0u) { s_st = s_retst; s_retst = 0u; }")
        L.append("    else { s_st = MINI_NST; }")
    elif k == "op":
        L.extend(_op_c(idx, it))
    elif k == "call":
        args = it["args"]
        narg = len(args)
        if narg > 0:
            L.append("    const char *av[%d];" % narg)
            L.append("    scl_invoke_arg_t ia[%d];" % narg)
            bufi = 0
            for ai, (ty, tx) in enumerate(args):
                if ty == T_STR and "${" in tx:
                    L.append("    char bx_%d[48];" % bufi)
                    L.append("    Mini_Exp(%s, bx_%d, sizeof(bx_%d));" % (_cstr(tx), bufi, bufi))
                    L.append("    av[%d] = bx_%d;" % (ai, bufi))
                    bufi += 1
                else:
                    L.append("    av[%d] = %s;" % (ai, _cstr(tx)))
                tn = {T_BOOL: "SCL_T_BOOL", T_INT: "SCL_T_INT",
                      T_FLAG: "SCL_T_FLAG", T_STR: "SCL_T_STR"}[ty]
                L.append("    ia[%d].text = av[%d];" % (ai, ai))
                L.append("    ia[%d].type = %s;" % (ai, tn))
            L.append("    {")
            L.append("        uint8_t r = SCL_CmdInvoke(%s, %d, ia);" % (_cstr(it["name"]), narg))
        else:
            L.append("    {")
            L.append("        uint8_t r = SCL_CmdInvoke(%s, 0, NULL);" % _cstr(it["name"]))
        L.append("        if (r == 0u) { s_fault = 1u; goto mini_done; }")
        L.append("        if (r == 2u) { s_wait = 1u; s_pend = %du; }" % (idx + 1))
        L.append("        else { s_st = %du; }" % (idx + 1))
        L.append("    }")
    else:
        L.append("    s_fault = 1u; goto mini_done;")
    L.append("    break;")
    L.append("}")
    return L


def _op_c(idx, it):
    L = []
    op = it["op"]
    toks = it["toks"]
    nxt = idx + 1
    def _num(tok, var):
        L.append("        %s = Mini_NumTok(%s, &ok);" % (var, _cstr(tok)))
        L.append("        if (!ok) { s_fault = 1u; goto mini_done; }")
    if op in _INT_BIN:
        L.append("    { int ok;")
        if len(toks) < 3:
            L.append("      s_fault = 1u; goto mini_done; }")
        else:
            a, b, dst = toks[0], toks[1], toks[2]
            _num(a, "int32_t a")
            _num(b, "int32_t b")
            opc = {"iadd": "a + b", "isub": "a - b", "imul": "a * b",
                   "iand": "a & b", "ior": "a | b", "ixor": "a ^ b",
                   "shl": "(int32_t)((uint32_t)a << (b & 31))",
                   "shr": "(int32_t)((uint32_t)a >> (b & 31))"}.get(op)
            if opc is None:  # idiv / imod
                L.append("        if (b == 0) { s_fault = 1u; goto mini_done; }")
                opc = "a / b" if op == "idiv" else "a % b"
            L.append("        char tb[MINI_VLEN];")
            L.append("        Mini_Fmt(%s, tb, sizeof(tb));" % opc)
            L.append('        if (Mini_Set(%s, SCL_T_INT, tb, 0u) != 0) { s_fault = 1u; goto mini_done; }'
                     % _cstr(dst))
            L.append("    }")
        L.append("    s_st = %du;" % nxt)
    elif op in _INT_UN:
        L.append("    { int ok;")
        if len(toks) < 2:
            L.append("      s_fault = 1u; goto mini_done; }")
        else:
            a, dst = toks[0], toks[1]
            _num(a, "int32_t a")
            L.append("        char tb[MINI_VLEN];")
            L.append("        Mini_Fmt(%s, tb, sizeof(tb));" % ("-a" if op == "ineg" else "~a"))
            L.append('        if (Mini_Set(%s, SCL_T_INT, tb, 0u) != 0) { s_fault = 1u; goto mini_done; }'
                     % _cstr(dst))
            L.append("    }")
        L.append("    s_st = %du;" % nxt)
    elif op in _CMP:
        L.append("    { int ok;")
        if len(toks) < 2:
            L.append("      s_fault = 1u; goto mini_done; }")
        else:
            _num(toks[0], "int32_t a")
            _num(toks[1], "int32_t b")
            cond = {"ieq": "a == b", "ine": "a != b", "igt": "a > b",
                    "ige": "a >= b", "ilt": "a < b", "ile": "a <= b"}[op]
            L.append("        SCL_Ret_Set((%s) ? 1 : 0);" % cond)
            L.append("    }")
        L.append("    s_st = %du;" % nxt)
    elif op in _BOOL2:
        L.append("    {")
        if len(toks) < 2:
            L.append("      s_fault = 1u; goto mini_done; }")
        else:
            L.append("        uint8_t a = Mini_TruthTok(%s);" % _cstr(toks[0]))
            L.append("        uint8_t b = Mini_TruthTok(%s);" % _cstr(toks[1]))
            L.append("        SCL_Ret_Set((%s) ? 1 : 0);"
                     % ("(a && b)" if op == "band" else "(a || b)"))
            L.append("    }")
        L.append("    s_st = %du;" % nxt)
    elif op in _BOOL1:
        L.append("    {")
        if len(toks) < 1:
            L.append("      s_fault = 1u; goto mini_done; }")
        else:
            L.append("        uint8_t a = Mini_TruthTok(%s);" % _cstr(toks[0]))
            L.append("        SCL_Ret_Set(%s);" % ("a ? 0 : 1" if op == "bnot" else "a ? 1 : 0"))
            L.append("    }")
        L.append("    s_st = %du;" % nxt)
    elif op in _STRCMP:
        L.append("    { int eq;")
        if len(toks) < 2:
            L.append("      s_fault = 1u; goto mini_done; }")
        else:
            L.append("        eq = Mini_Eq(Mini_TokText(%s), Mini_TokText(%s)) ? 1 : 0;"
                     % (_cstr(toks[0]), _cstr(toks[1])))
            L.append("        SCL_Ret_Set(%s);" % ("eq" if op == "seq" else "eq ? 0 : 1"))
            L.append("    }")
        L.append("    s_st = %du;" % nxt)
    else:
        L.append("    s_fault = 1u; goto mini_done;")
    return L


def main(argv=None):
    ap = argparse.ArgumentParser(description="mini-scl：脚本/指令链 → switch 状态机 C")
    ap.add_argument("input", help="输入 .s2c（现代语法）或指令链文本（--chain）")
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--name", default=None)
    ap.add_argument("--chain", action="store_true")
    ap.add_argument("--ret-setter", default="setret")
    ap.add_argument("--var-max", type=int, default=4)
    ap.add_argument("--name-max", type=int, default=8)
    ap.add_argument("--value-max", type=int, default=15)
    args = ap.parse_args(argv)

    try:
        with open(args.input, "r", encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        print("scl-mini: 无法读取 %s: %s" % (args.input, e), file=sys.stderr)
        return 2

    if args.chain:
        chain = src
        note = "%s (指令链文本)" % args.input
    else:
        try:
            chain, warns = translate(src, ret_setter=args.ret_setter,
                                     var_max=args.var_max, name_max=args.name_max,
                                     value_max=args.value_max, const_fold=True)
        except S2CError as e:
            loc = ("%s:%d:%d: " % (args.input, e.line, e.col)) if e.line else ""
            print("scl-mini: %serror: %s" % (loc, e.msg), file=sys.stderr)
            return 1
        for w in warns:
            print("scl-mini: 警告: " + w, file=sys.stderr)
        note = "%s (现代语法 .s2c)" % args.input

    if args.name:
        name = args.name
    else:
        base = os.path.basename(args.input)
        name = "".join(ch for ch in base.split(".")[0] if ch.isalnum() or ch == "_") or "prog"

    try:
        text = emit_mini_c(name, chain, note)
    except MiniError as e:
        print("scl-mini: error: %s" % e, file=sys.stderr)
        return 1

    if args.output:
        try:
            with open(args.output, "w", encoding="utf-8") as f:
                f.write(text)
        except OSError as e:
            print("scl-mini: 无法写入 %s: %s" % (args.output, e), file=sys.stderr)
            return 2
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
