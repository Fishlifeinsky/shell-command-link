#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scl_mini_c.py —— mini-scl：把脚本/指令链编译成"类型化 C 状态机"（v2）

设计目标（小型化）：
  - 脚本变量 = 生成 .c 里的**类型化 static**（int32_t / bool / flag / 字符串缓冲），
    不是文本会话变量、不镜像到会话槽。
  - 运算符直接用 C：算术/比较/逻辑在 case 里就是 m_a + m_b、m_i < 3、m_b && m_c，
    不再走文本解析/格式化。
  - 每个变量生成 getter/setter，经 SCL_VarBind() 注册进 SCL 的绑定路由：
    外部 C 用 SCL_VarGet/SCL_VarSet(T) 时命中绑定 → 落到这些 static（不占会话槽、
    无生命周期管理，因此不需要 free）。内部临时变量（__ 前缀）不注册。
  - 生成 `<name>_mini_register()`：注册命令 + 绑定变量；
        `<name>_mini_start()` / `_step()`（非阻塞）/ `_busy()`。
    命令注册后外部（或薄分发）可直接按名触发该 s2c 运行。
  - 通用小助手只在用到时生成极少量；需要 SCL_CFG_MINI_EN=1 编译（绑定路由在库内）。
  - 参数发射：整串恰好是单个 ${name} 时直通（不再经 char[48] 局部缓冲 + 逐段拼接）。
    因此绑定 getter（SCL_VarBind 注册）**必须每变量独立存储**，不得多变量共用同一
    缓冲：同一调用点可能同时持有多个 getter 指针（如 drv("${a}", "${b}")）。

用法：
  python tools/scl_mini_c.py boot.s2c -o boot_mini.c [--name boot] [--cmd boot]
  python tools/scl_mini_c.py --chain boot.chain -o boot_mini.c
  # 也可 scl_emit_c.py --mini 调同一入口
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scl_script2chain import translate, S2CError  # noqa: E402

# ---------- 与 scl_emit_c.py 一致的字面量/类型 ----------
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
INT_BIN = {"iadd", "isub", "imul", "idiv", "imod", "iand", "ior", "ixor", "shl", "shr"}
INT_UN = {"ineg", "inot"}
CMP = {"ieq", "ine", "igt", "ige", "ilt", "ile"}
BOOL2 = {"band", "bor"}
BOOL1 = {"bnot", "btest"}
STRCMP = {"seq", "sneq"}

_TYPEC = {"int": "SCL_T_INT", "bool": "SCL_T_BOOL",
          "flag": "SCL_T_FLAG", "string": "SCL_T_STR"}


class MiniError(Exception):
    pass


def _is_sp(c):
    return c in " \t\r\n"


def _is_digit(c):
    return "0" <= c <= "9"


def _parse_i32(s):
    if not s:
        return None
    if len(s) > 2 and s[0] == "0" and s[1] in "xXbB":
        base = 16 if s[1] in "xX" else 2
        v = 0
        for ch in s[2:]:
            if _is_digit(ch):
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
        if not _is_digit(ch):
            return None
        d = ord(ch) - 48
        if v > (lim - d) // 10:
            return None
        v = v * 10 + d
    return (-v) if neg else v


def _lit_type(tok):
    """token 字面量类型：'int'/'bool'/'flag'/'str'/'num'？返回 None 表示是引用。"""
    if len(tok) == 2 and tok[0] == "-" and ("a" <= tok[1] <= "z" or "A" <= tok[1] <= "Z"):
        return "flag"
    low = tok.lower()
    if low == "true" or low == "false":
        return "bool"
    if _parse_i32(tok) is not None:
        return "int"
    return "str"


def _lit_val(tok):
    """字面量的 C 值/文本描述。"""
    ty = _lit_type(tok)
    if ty == "int":
        return ("int", str(_parse_i32(tok)))
    if ty == "bool":
        return ("bool", "1" if tok.lower() == "true" else "0")
    if ty == "flag":
        return ("flag", tok)
    return ("str", tok)


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


def _clauses(chain):
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
    if not (nm and (nm[0] == "_" or ("a" <= nm[0] <= "z") or ("A" <= nm[0] <= "Z"))):
        raise MiniError("var 名不合法: %r" % nm)
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
            if not nm or nm in labels:
                raise MiniError("label 名不合法/重名: %r" % nm)
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
            insts.append({"kind": "callf", "tgt": tgt})
            continue
        if head == "retf":
            insts.append({"kind": "retf"})
            continue
        opw = next((w for (w, _o) in OPWORDS if head == w), None)
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
            continue            # mini 变量为 static，无生命周期 → free 是空操作
        if head in ("help", "cache"):
            raise MiniError("mini: meta '%s' 在自包含模式不支持" % head)
        if not (0 < len(head) < 256):
            raise MiniError("命令名不合法: %r" % head)
        insts.append({"kind": "call", "name": head, "toks": _toks(raw)})
    return insts, labels


# ---------------- C 发射 ----------------

def _var_cname(nm):
    return "m_" + nm


def _var_getter(nm):
    return "m_%s_get" % nm


def _op_num_expr(tok, sym):
    """数值上下文的 C 表达式：脚本 int 变量→原生；字面量→常量；其它→运行时解析。
       返回 (code_lines, expr) —— code_lines 用于额外局部语句（一般为空）。"""
    s = sym.get(tok)
    if s is not None:
        if s["type"] == "int":
            return ([], _var_cname(tok))
        # 非 int 脚本变量出现在数值上下文（类型不匹配，尽量兼容）：取其文本再解析
        return ([], "SCL_ParseInt(%s(), 0)" % _var_getter(tok))
    ty = _lit_type(tok)
    if ty == "int":
        return ([], "(%s)" % str(_parse_i32(tok)))
    if ty == "bool":
        return ([], "1" if tok.lower() == "true" else "0")
    if ty == "flag":
        raise MiniError("flag 出现在数值上下文: %r" % tok)
    # 引用外部（${name} 或裸名，如 env 'baud'）：SCL_VarGet 读文本再解析
    return ([], "SCL_ParseInt(Mini_ExtS(%s), 0)" % _cstr(tok if _ext_name(tok) is None else _ext_name(tok)))


def _ext_name(tok):
    """取 ${name} 的 name；裸名原样。非引用返回 None。"""
    if tok.startswith("${") and tok.endswith("}"):
        n = tok[2:-1]
        return n
    return None


def _op_truth_expr(tok, sym, extra):
    """真值上下文的 C 表达式（原生 typed；外部文本用 Mini_TruthS）。"""
    s = sym.get(tok)
    if s is not None:
        t = s["type"]
        if t == "bool":
            return "(%s != 0u)" % _var_cname(tok)
        if t == "int":
            return "(%s != 0)" % _var_cname(tok)
        if t == "flag":
            return "1"            # flag 已定义即为真（无生命周期）
        return "(%s[0] != '\\0')" % _var_cname(tok)
    ty = _lit_type(tok)
    if ty == "bool":
        return "1" if tok.lower() == "true" else "0"
    if ty == "int":
        return "(%s != 0)" % str(_parse_i32(tok))
    if ty == "flag":
        return "1"
    # ${name} 或裸名/裸文本 → 一律当外部变量读（解释器 TruthText 先查变量再按字面）
    return "Mini_TruthS(Mini_ExtS(%s))" % _cstr(tok if _ext_name(tok) is None else _ext_name(tok))


def _op_text_expr(tok, sym, extra):
    """文本上下文的 C 表达式（指针，可经 getter/字面量/外部）。"""
    s = sym.get(tok)
    if s is not None:
        return _var_getter(tok) + "()"
    nm = _ext_name(tok)
    if nm is not None:
        return "Mini_ExtS(%s)" % _cstr(nm)
    return _cstr(tok)


def _build_arg_text(tok, sym):
    """命令参数 token → (pre_lines, text_expr, type_c)。处理 ${} 内部脚本变量引用。
       注意：裸变量名参数按字面量（解释器不展开裸名）；只有 ${name} 才取变量文本。"""
    nm = _ext_name(tok)
    ty = _lit_type(tok)
    if nm is None and ty != "str":
        return ([], _cstr(tok), {"int": "SCL_T_INT", "bool": "SCL_T_BOOL",
                                  "flag": "SCL_T_FLAG"}[ty])
    if "${" not in tok:
        return ([], _cstr(tok), "SCL_T_STR")
    # 快路径：整串恰好是单个 ${name}（无字面量拼接）→ 直接给表达式，
    # 免去 char bx[48] 局部缓冲 + Mini_AppendS 逐段拼接（纯浪费，且占栈）。
    if (nm is not None) and (tok.count("${") == 1):
        if sym.get(nm) is not None:
            return ([], _var_getter(nm) + "()", "SCL_T_STR")
        return ([], "Mini_ExtS(%s)" % _cstr(nm), "SCL_T_STR")
    # 含 ${...}：编译期拆片；脚本变量用其 getter，否则外部 Mini_ExtS
    pre = []
    b = _build_arg_exp(tok, sym, pre)
    return (pre, b, "SCL_T_STR")


_ARGCTR = [0]


def _build_arg_exp(tok, sym, out_lines):
    """把含 ${} 的 token 拆成拼接语句；返回 (text_expr)。分配 bx 名字。"""
    _ARGCTR[0] += 1
    k = _ARGCTR[0]
    b = "bx%d" % k
    z = "zx%d" % k
    out_lines.append("    char %s[48]; unsigned %s = 0u;" % (b, z))
    i = 0
    n = len(tok)
    while i < n:
        if tok[i] == "$" and i + 1 < n and tok[i + 1] == "{":
            j = tok.find("}", i + 2)
            if j < 0:
                raise MiniError("${ 未闭合")
            name = tok[i + 2:j]
            s = sym.get(name)
            if s is not None:
                out_lines.append("    Mini_AppendS(%s, &%s, sizeof(%s), %s());"
                                 % (b, z, b, _var_getter(name)))
            else:
                out_lines.append("    Mini_AppendS(%s, &%s, sizeof(%s), Mini_ExtS(%s));"
                                 % (b, z, b, _cstr(name)))
            i = j + 1
            continue
        j = i
        while j < n and not (tok[j] == "$" and j + 1 < n and tok[j + 1] == "{"):
            j += 1
        lit = tok[i:j]
        if lit:
            out_lines.append("    Mini_AppendS(%s, &%s, sizeof(%s), %s);"
                             % (b, z, b, _cstr(lit)))
        i = j
    out_lines.append("    %s[%s < sizeof(%s) ? %s : sizeof(%s) - 1u] = '\\0';"
                     % (b, z, b, z, b))
    return b


def emit_mini_c(name, chain, source_note, cmd_name=None):
    insts, labels = _mini_insts(chain)
    if not insts:
        raise MiniError("无指令")
    n = len(insts)
    for it in insts:
        if it["kind"] in ("jump", "jumpa", "callf"):
            if it["tgt"] not in labels:
                raise MiniError("%s: label '%s' 未定义" % (it["kind"], it["tgt"]))
            it["st"] = labels[it["tgt"]]

    # 符号表：脚本变量（内部 __ 前缀不绑定/不镜像）
    sym = {}
    for it in insts:
        if it["kind"] == "vardecl":
            old = sym.get(it["name"])
            if old is None or (it["const"] and not old["const"]):
                sym[it["name"]] = {"type": it["type"], "const": it["const"]}
    bind_vars = [nm for nm in sorted(sym) if not nm.startswith("__")]

    _ARGCTR[0] = 0
    L = []
    L.append("/* ===============================================================")
    L.append(" * SCL mini v2 类型化状态机：%s（由 tools/scl_mini_c.py 生成，勿手改）" % name)
    L.append(" * 来源：%s" % source_note)
    L.append(" * 用法：SCL_Init(); <注册所需命令>; %s_mini_register();" % name)
    L.append(" *       %s_mini_start(); 然后周期调 %s_mini_step()（每次一动作，非阻塞）。" % (name, name))
    L.append(" * 变量=类型化 static；SCL_VarBind 绑定 → 外部 SCL_VarGet/Set 路由到 static。")
    L.append(" * 绑定 getter 必须每变量独立存储（同一调用点可能同时持有多个 getter 指针）。")
    L.append(" * 需 SCL_CFG_MINI_EN=1 编译（绑定路由在库内）。")
    L.append(" * ===============================================================")
    L.append(" */")
    L.append('#include "scl.h"')
    L.append("")
    L.append("#define MINI_NST %du" % n)
    L.append("#define MINI_STEP_LIMIT 1000000u")
    L.append("#if defined(__GNUC__) || defined(__clang__)")
    L.append("#define MINI_UNUSED __attribute__((unused))")
    L.append("#else")
    L.append("#define MINI_UNUSED")
    L.append("#endif")
    L.append("")

    # 需要用到的小助手（按需；多余的无害，MINI_UNUSED 抑制告警 + 编译器裁掉）
    need = {"cp": False, "eq": False, "itoa": False,
            "appends": False, "exts": False, "truths": False}
    any_bool = False
    any_strcmp = False
    for it in insts:
        if it["kind"] == "vardecl":
            if it["type"] == "string":
                need["cp"] = True
        elif it["kind"] == "call":
            for t in it["toks"]:
                if "${" in t:
                    need["appends"] = True
                    if sym.get(_ext_name(t)) is None:
                        need["exts"] = True
        elif it["kind"] == "op":
            op = it["op"]
            if op in BOOL1 or op in BOOL2:
                any_bool = True
            if op in STRCMP:
                any_strcmp = True
            for t in it["toks"]:
                if _ext_name(t) is not None:
                    need["exts"] = True
                elif sym.get(t) is None and _lit_type(t) not in ("int", "bool", "flag"):
                    # 裸名可能是 env/绑定（如 iadd r baud r 的 baud）
                    if op in (CMP | INT_BIN | INT_UN | BOOL2 | BOOL1):
                        need["exts"] = True
                    elif op in STRCMP:
                        if _lit_type(t) != "str":
                            need["exts"] = True
    if any(sym[nm]["type"] == "int" for nm in sym):
        need["itoa"] = True
    if any(sym[nm]["type"] == "string" for nm in sym):
        need["cp"] = True
    if any_bool:
        need["truths"] = True
    if any_strcmp:
        need["eq"] = True
    if need["truths"]:
        need["eq"] = True      # Mini_TruthS 用 Mini_Eq
    if need["appends"]:
        need["cp"] = True      # 供字符串变量缓冲写入用（保守保留；实际用 AppendS）
    if need["eq"]:
        L.append("static MINI_UNUSED int Mini_Eq(const char *a, const char *b)")
        L.append("{ unsigned i = 0u;")
        L.append("  for (; a[i] != '\\0' && b[i] != '\\0'; i++) { if (a[i] != b[i]) { return 0; } }")
        L.append("  return (a[i] == b[i]) ? 1 : 0; }")
        L.append("")
    if need["cp"]:
        L.append("static MINI_UNUSED void Mini_Cp(char *d, const char *s, unsigned cap)")
        L.append("{ unsigned i = 0u; while (i + 1u < cap && s[i] != '\\0') { d[i] = s[i]; i++; }")
        L.append("  if (cap) { d[i] = '\\0'; } }")
        L.append("")
    if need["itoa"]:
        L.append("static MINI_UNUSED void Mini_Itoa(int32_t v, char *d, unsigned cap)")
        L.append("{ char t[12]; unsigned k = 0u, off = 0u, i; uint32_t u;")
        L.append("  if (v < 0) { off = 1u; u = (uint32_t)(-(v + 1)) + 1u; } else { u = (uint32_t)v; }")
        L.append("  if (u == 0u) { t[k++] = '0'; }")
        L.append("  while (u) { t[k++] = (char)('0' + (u % 10u)); u /= 10u; }")
        L.append("  if (off && off < cap) { d[0] = '-'; }")
        L.append("  for (i = 0u; i < k && off + i + 1u < cap; i++) { d[off + i] = t[k - 1u - i]; }")
        L.append("  d[(off + (k < cap - off ? k : cap - 1u - off))] = '\\0'; }")
        L.append("")
    if need["appends"]:
        L.append("static MINI_UNUSED void Mini_AppendS(char *d, unsigned *idx, unsigned cap, const char *s)")
        L.append("{ while (*s && *idx + 1u < cap) { d[(*idx)++] = *s++; } }")
        L.append("")
    if need["exts"]:
        L.append("static MINI_UNUSED const char *Mini_ExtS(const char *nm)")
        L.append("{ const char *v = SCL_VarGet(nm); return (v != NULL) ? v : \"\"; }")
        L.append("")
    if need["truths"]:
        L.append("static MINI_UNUSED int Mini_TruthS(const char *s)")
        L.append("{ if (s[0] == '\\0') { return 0; }")
        L.append("  if (Mini_Eq(s, \"false\")) { return 0; }")
        L.append("  if (Mini_Eq(s, \"0\")) { return 0; }")
        L.append("  return 1; }")
        L.append("")

    # 类型化变量 + getter/setter + 绑定表 + 命令注册
    L.append("/* ---- 类型化变量（绑定路由到外部） ---- */")
    if sym:
        for nm in sorted(sym):
            t = sym[nm]["type"]
            c = _var_cname(nm)
            g = _var_getter(nm)
            if t == "int":
                L.append("static int32_t %s;" % c)
                L.append("static MINI_UNUSED const char *%s(void){ static char b[12]; Mini_Itoa(%s, b, sizeof b); return b; }"
                         % (g, c))
                L.append("static MINI_UNUSED int %s_set(const char *s){ %s = SCL_ParseInt((s != NULL) ? s : \"\", 0); return 0; }"
                         % (nm, c))
            elif t == "bool":
                L.append("static uint8_t %s;" % c)
                L.append("static MINI_UNUSED const char *%s(void){ return %s ? \"true\" : \"false\"; }" % (g, c))
                L.append("static MINI_UNUSED int %s_set(const char *s){ if (s == NULL) return -3;" % nm)
                L.append("  if (s[0] == 't' || s[0] == 'T' || s[0] == '1') { %s = 1u; }" % c)
                L.append("  else if (s[0] == 'f' || s[0] == 'F' || s[0] == '0') { %s = 0u; }" % c)
                L.append("  else { return -3; } return 0; }")
            elif t == "flag":
                L.append("static char %s;" % c)
                L.append("static MINI_UNUSED const char *%s(void){ static char b[3]; b[0]='-'; b[1]=%s; b[2]='\\0'; return b; }" % (g, c))
                L.append("static MINI_UNUSED int %s_set(const char *s){ if (s == NULL) return -3;" % nm)
                L.append("  if (s[0] == '-' && s[1] != '\\0' && s[2] == '\\0') { %s = s[1]; return 0; }" % c)
                L.append("  return -3; }")
            else:  # string：缓冲 16B（同解释器值上限）
                L.append("static char %s[16];" % c)
                L.append("static MINI_UNUSED const char *%s(void){ return %s; }" % (g, c))
                L.append("static MINI_UNUSED int %s_set(const char *s){ Mini_Cp(%s, (s != NULL) ? s : \"\", sizeof(%s)); return 0; }"
                         % (nm, c, c))
    else:
        L.append("/* 无脚本变量 */")
    L.append("")

    # 绑定表
    if bind_vars:
        L.append("/* ---- 变量绑定：SCL_VarBind 路由（命中 SCL_VarGet/Set） ---- */")
        L.append("static const scl_var_bind_t s_bind[] = {")
        for nm in bind_vars:
            s = sym[nm]
            L.append('    { %s, %s, %s, %s },'
                     % (_cstr(nm), _TYPEC[s["type"]], _var_getter(nm), "%s_set" % nm))
        L.append("};")
        L.append("")

    # 状态
    L.append("/* ---- 运行状态 ---- */")
    L.append("static uint16_t s_st;")
    L.append("static uint32_t s_steps;")
    L.append("static uint8_t  s_wait;")
    L.append("static uint16_t s_pend;")
    L.append("static uint8_t  s_fault;")
    L.append("static uint16_t s_retst;")
    L.append("")

    L.append("uint8_t %s_mini_step(void)" % name)
    L.append("{")
    L.append("    if (s_wait != 0u)")
    L.append("    { int p = SCL_AsyncPoll();")
    L.append("      if (p == 0) { return 1u; }")
    L.append("      s_wait = 0u; s_st = s_pend; }")
    L.append("    if (s_st >= MINI_NST) { return 0u; }")
    L.append("    if (s_steps++ >= MINI_STEP_LIMIT) { s_fault = 1u; goto mini_done; }")
    L.append("    switch (s_st)")
    L.append("    {")
    for idx, it in enumerate(insts):
        for ln in _case(idx, it, sym):
            L.append("    " + ln)
    L.append("    default: s_st = MINI_NST; break;")
    L.append("    }")
    L.append("mini_done:")
    L.append("    if (s_fault != 0u) { s_fault = 0u; s_st = MINI_NST; return 0u; }")
    L.append("    return (s_st < MINI_NST) ? 1u : 0u;")
    L.append("}")
    L.append("")

    L.append("void %s_mini_start(void)" % name)
    L.append("{")
    L.append("    SCL_Ret_Set(0); s_st = 0u; s_steps = 0u;")
    L.append("    s_wait = 0u; s_pend = 0u; s_fault = 0u; s_retst = 0u;")
    L.append("}")
    L.append("uint8_t %s_mini_busy(void)" % name)
    L.append("{ return (s_st < MINI_NST) ? 1u : 0u; }")
    L.append("")

    # register：命令 + 变量绑定
    L.append("/* ---- 注册：把 s2c 注册成命令 + 绑定变量到 SCL ---- */")
    L.append("static void %s_mini_cmd(int argc, char *argv[])" % name)
    L.append("{ (void)argc; (void)argv; %s_mini_start(); }" % name)
    L.append("static scl_cmd_t s_%s_cmd = { %s, %s_mini_cmd, NULL, NULL, 0, NULL };" % (name, _cstr(cmd_name or name), name))
    L.append("void %s_mini_register(void)" % name)
    L.append("{")
    L.append("    SCL_RegisterCmd(&s_%s_cmd);" % name)
    if bind_vars:
        L.append("    (void)SCL_VarBind(s_bind, %d);" % len(bind_vars))
    L.append("}")
    L.append("")
    return "\n".join(L)


def it_decl(nm, insts, sym):
    for it in insts:
        if it["kind"] == "vardecl" and it["name"] == nm:
            return it
    return None


def _var_cap(decl, need):
    """字符串变量缓冲容量（含 NUL）。"""
    if decl is None:
        return 16
    v = decl["val"]
    # 考虑 ${} 展开到最长引用值：给足 48 与字面量较长者
    cap = len(v) + 1
    # 让后续用于 echo 展开的最小余量
    if "${" not in v:
        cap = max(cap, 2)
    if cap < 16:
        cap = 16
    if cap > 48:
        cap = 48
    return cap


def _case(idx, it, sym):
    L = ["case %d:" % idx, "{"]
    k = it["kind"]
    nxt = idx + 1
    if k == "vardecl":
        t = it["type"]
        nm = it["name"]
        c = _var_cname(nm)
        if t == "int":
            L.append("    %s = %s;" % (c, it["val"]))
        elif t == "bool":
            L.append("    %s = %s;" % (c, "1u" if it["val"] == "true" else "0u"))
        elif t == "flag":
            L.append("    %s = %s;" % (c, _cstr(it["val"][1])))
        else:
            L.append("    Mini_Cp(%s, %s, sizeof(%s));" % (c, _cstr(it["val"]), c))
        L.append("    s_st = %du;" % nxt)
    elif k == "jump":
        L.append("    s_st = %du;" % it["st"])
    elif k == "jumpa":
        L.append("    if (SCL_Ret_Get() != 0) { SCL_Ret_Set(0); s_st = %du; }" % it["st"])
        L.append("    else { SCL_Ret_Set(0); s_st = %du; }" % nxt)
    elif k == "callf":
        L.append("    s_retst = %du; s_st = %du;" % (nxt, it["st"]))
    elif k == "retf":
        L.append("    if (s_retst != 0u) { s_st = s_retst; s_retst = 0u; }")
        L.append("    else { s_st = MINI_NST; }")
    elif k == "op":
        L.extend(_op(idx, it, sym))
    elif k == "call":
        L.extend(_call(idx, it, sym))
    else:
        L.append("    s_fault = 1u; goto mini_done;")
    L.append("    break;")
    L.append("}")
    return L


def _op(idx, it, sym):
    """生成一条运算指令的 case 语句（不含 break；s_st 由本函数推进）。"""
    L = []
    op = it["op"]
    toks = it["toks"]
    nxt = idx + 1

    if op in INT_BIN:
        if len(toks) < 3:
            raise MiniError("%s: 需要 3 参 (a b dst)" % op)
        a, b, dst = toks
        _p, ea = _op_num_expr(a, sym)
        _p, eb = _op_num_expr(b, sym)
        opr = {"iadd": "a + b", "isub": "a - b", "imul": "a * b",
               "iand": "a & b", "ior": "a | b", "ixor": "a ^ b",
               "shl": "(int32_t)((uint32_t)a << (b & 31))",
               "shr": "(int32_t)((uint32_t)a >> (b & 31))"}.get(op)
        L.append("{ int32_t a = %s; int32_t b = %s; int32_t r;" % (ea, eb))
        if opr is None:
            L.append("  if (b == 0) { s_fault = 1u; goto mini_done; }")
            opr = "a / b" if op == "idiv" else "a % b"
        L.append("  r = %s; %s = r; }" % (opr, _var_cname(dst)))
    elif op in INT_UN:
        if len(toks) < 2:
            raise MiniError("%s: 需要 2 参 (a dst)" % op)
        a, dst = toks
        _p, ea = _op_num_expr(a, sym)
        L.append("{ int32_t a = %s; %s = %s; }" % (ea, _var_cname(dst),
                                                   "-a" if op == "ineg" else "~a"))
    elif op in CMP:
        if len(toks) < 2:
            raise MiniError("%s: 需要 2 参 (a b)" % op)
        a, b = toks
        _p, ea = _op_num_expr(a, sym)
        _p, eb = _op_num_expr(b, sym)
        opr = {"ieq": "==", "ine": "!=", "igt": ">", "ige": ">=",
               "ilt": "<", "ile": "<="}[op]
        L.append("SCL_Ret_Set(((%s) %s (%s)) ? 1 : 0);" % (ea, opr, eb))
    elif op in BOOL2:
        if len(toks) < 2:
            raise MiniError("%s: 需要 2 参 (a b)" % op)
        a, b = toks[:2]
        ea = _op_truth_expr(a, sym, L)
        eb = _op_truth_expr(b, sym, L)
        if op == "band":
            L.append("SCL_Ret_Set((%s) && (%s) ? 1 : 0);" % (ea, eb))
        else:
            L.append("SCL_Ret_Set((%s) || (%s) ? 1 : 0);" % (ea, eb))
    elif op in BOOL1:
        if len(toks) < 1:
            raise MiniError("%s: 需要 1 参 (a)" % op)
        a = toks[0]
        ea = _op_truth_expr(a, sym, L)
        if op == "bnot":
            L.append("SCL_Ret_Set((%s) ? 0 : 1);" % ea)
        else:
            L.append("SCL_Ret_Set((%s) ? 1 : 0);" % ea)
    elif op in STRCMP:
        if len(toks) < 2:
            raise MiniError("%s: 需要 2 参 (a b)" % op)
        a, b = toks
        ea = _op_text_expr(a, sym, L)
        eb = _op_text_expr(b, sym, L)
        L.append("SCL_Ret_Set(Mini_Eq(%s, %s) ? %s : %s);"
                 % (ea, eb, "1" if op == "seq" else "0", "0" if op == "seq" else "1"))
    else:
        raise MiniError("mini: 不支持运算 %s" % op)

    L.append("s_st = %du;" % nxt)
    return L


def _call(idx, it, sym):
    """生成一次命令调用的 case 语句（不含 break；含 ${} 拆片到局部缓冲）。"""
    L = []
    nxt = idx + 1
    toks = it["toks"]
    narg = len(toks)
    exprs = []
    types = []
    for t in toks:
        pr, tx, ty = _build_arg_text(t, sym)
        for ln in pr:
            L.append(ln.strip("\n"))
        exprs.append(tx)
        types.append(ty)
    if narg > 0:
        # 直接填 scl_invoke_arg_t[]，不再经 const char *av[] 中转（少一层数组与赋值）
        L.append("scl_invoke_arg_t ia[%d];" % narg)
        for i in range(narg):
            L.append("ia[%d].text = %s;" % (i, exprs[i]))
            L.append("ia[%d].type = %s;" % (i, types[i]))
        L.append("{ uint8_t r = SCL_CmdInvoke(%s, %d, ia);" % (_cstr(it["name"]), narg))
    else:
        L.append("{ uint8_t r = SCL_CmdInvoke(%s, 0, NULL);" % _cstr(it["name"]))
    L.append("  if (r == 0u) { s_fault = 1u; goto mini_done; }")
    L.append("  if (r == 2u) { s_wait = 1u; s_pend = %du; }" % nxt)
    L.append("  else { s_st = %du; } }" % nxt)
    return L


def main(argv=None):
    ap = argparse.ArgumentParser(description="mini-scl v2：脚本/指令链 → 类型化 switch 状态机 C")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--name", default=None)
    ap.add_argument("--cmd", default=None, help="注册成命令行指令的名字（缺省=--name/文件名）")
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
        text = emit_mini_c(name, chain, note, cmd_name=args.cmd)
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
