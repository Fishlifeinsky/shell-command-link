#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scl_script2chain.py — 现代语法脚本 -> SCL 指令链 转译器（纯标准库）

设计详见 doc/arc/script2chain-design.md。

现代语法（S2C，v1）：
  - 注释： # ...   // ...   /* ... */
  - 语句以换行或 ';' 分隔；块用大括号 { }
  - 变量：  var 名 = 值            （名<=8、存活<=2、字面值<=15，编译期校验）
  - 释放：  free            / free 名
  - 命令调用： name(a, b, c)        （实参可为裸词/数字/字符串/ ${var} ）
  - 置返回： ret(1|0|true|false)    （映射到目标“置 G_RETURN 命令”，见 --ret-setter）
  - 布尔句： true / false           （同上）
  - if：     if (条件) { } [else if (条件) { }] [else { }]
             无条件形式： if { } else { }   —— 沿用当前 G_RETURN
             条件 = 命令/自定义 fn 调用（产生 G_RETURN）或 true/false
  - while：  while (条件) { }       （C 语义：先判后跑）
  - 函数：   fn 名 (参1,参2) { 语句 }   —— 编译期内联展开（文字替换），可用作条件

输出：单行 SCL 指令链（保留关键字的引号交替由工具自动处理）。
"""

import sys
import argparse

# ============================ 保留字 ============================

RESERVED_CMD = {"if", "while", "var", "free", "help"}          # SCL 保留命令
SYNTAX_WORDS = {"else", "fn", "ret", "true", "false"}          # 语法字（不可当命令/变量名）


# ============================ 错误 ============================

class S2CError(Exception):
    """带位置信息的编译错误"""

    def __init__(self, msg, line=0, col=0):
        super().__init__(msg)
        self.msg = msg
        self.line = line
        self.col = col


# ============================ 词法 ============================

class Tok:
    __slots__ = ("kind", "text", "line", "col")

    def __init__(self, kind, text, line, col):
        self.kind = kind      # ID NUM STR VARREF 或单字符符号; 'NL'
        self.text = text
        self.line = line
        self.col = col

    def __repr__(self):
        return "Tok(%s,%r,%d)" % (self.kind, self.text, self.line)


def tokenize(src):
    """把源码切为 token 列表。换行生成 'NL'；注释剥离；引号/块注释支持跨行。"""
    toks = []
    i = 0
    n = len(src)
    line = 1
    col = 1

    def adv(k=1):
        nonlocal i, line, col
        for _ in range(k):
            if i < n and src[i] == "\n":
                line += 1
                col = 1
            else:
                col += 1
            i += 1

    def peek(c=None):
        return c if c is not None and False else None

    while i < n:
        ch = src[i]

        # 换行
        if ch == "\n":
            toks.append(Tok("NL", "\n", line, col))
            adv()
            continue
        # 空白
        if ch in " \t\r":
            adv()
            continue
        # 注释
        if ch == "#":
            while i < n and src[i] != "\n":
                adv()
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                adv()
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "*":
            adv(2)
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                adv()
            if i < n:
                adv(2)
            else:
                raise S2CError("块注释未闭合", line, col)
            continue

        l0, c0 = line, col

        # 字符串字面量（'...' 或 "..."，无转义）
        if ch in "\"'":
            q = ch
            adv()
            buf = []
            while i < n and src[i] != q:
                if src[i] == "\n":
                    raise S2CError("字符串未闭合", line, col)
                buf.append(src[i])
                adv()
            if i >= n:
                raise S2CError("字符串未闭合", l0, c0)
            adv()  # 闭引号
            toks.append(Tok("STR", "".join(buf), l0, c0))
            continue

        # ${var} 变量引用
        if ch == "$" and i + 1 < n and src[i + 1] == "{":
            adv(2)
            buf = []
            while i < n and src[i] != "}":
                buf.append(src[i])
                adv()
            if i >= n:
                raise S2CError("${} 未闭合", l0, c0)
            adv()
            if not buf:
                raise S2CError("${} 变量名为空", l0, c0)
            toks.append(Tok("VARREF", "".join(buf), l0, c0))
            continue
        if ch == "$":
            raise S2CError("意外的 '$'（变量引用请用 ${name}）", line, col)

        # 数字 / 标识符
        if ch.isdigit() or ch.isalpha() or ch == "_":
            buf = []
            while i < n and (src[i].isalnum() or src[i] == "_"):
                buf.append(src[i])
                adv()
            text = "".join(buf)
            kind = "NUM" if text[0].isdigit() else "ID"
            toks.append(Tok(kind, text, l0, c0))
            continue

        # 单字符符号
        if ch in "(){};,=":
            toks.append(Tok(ch, ch, l0, c0))
            adv()
            continue

        raise S2CError("无法识别的字符 %r" % ch, line, col)

    toks.append(Tok("EOF", "", line, col))
    return toks


# ============================ AST ============================
# 节点用元组表示：
#   ('call', name, [arg...])           命令/函数调用（arg 为内容字符串）
#   ('var', name, value)               变量赋值
#   ('free', name|None)                释放
#   ('ret_set', '1'|'0')               置 G_RETURN
#   ('if', cond, then_list, else_list)  cond: None 或 ('call'..) 或 ('bool',b)
#   ('while', cond, body_list)
#   ('fndef', name, params, body)      函数定义（不输出）


# ============================ 解析器 ============================

class Parser:
    def __init__(self, toks, ret_kw="ret"):
        self.ts = toks
        self.p = 0
        self.ret_kw = ret_kw  # 保留（ret 语句用的关键字）
        self.in_block_depth = 0  # 是否在函数/块内（fn 只允许顶层定义）

    # ---- token 游标 ----
    def cur(self):
        return self.ts[self.p]

    def at(self, kind=None, text=None):
        t = self.cur()
        if kind is not None and t.kind != kind:
            return False
        if text is not None and t.text != text:
            return False
        return True

    def next(self):
        t = self.ts[self.p]
        if t.kind != "EOF":
            self.p += 1
        return t

    def expect(self, kind=None, text=None, what=""):
        t = self.cur()
        if (kind is not None and t.kind != kind) or (text is not None and t.text != text):
            raise S2CError("期望 %s，实际为 %r" % (what or (text or kind), t.text), t.line, t.col)
        return self.next()

    def skip_sep(self):
        """跳过 NL 与 ';'（语句分隔）"""
        while self.at("NL") or self.at(";"):
            self.next()

    def skip_nl(self):
        while self.at("NL"):
            self.next()

    # ---- 语句分隔与程序 ----
    def parse_program(self):
        stmts = []
        self.skip_sep()
        while not self.at("EOF"):
            stmts.append(self.parse_statement(top=True))
            self.expect_stmt_end()
            self.skip_sep()
        return stmts

    def expect_stmt_end(self):
        """语句结束后必须跟分隔符 / '}' / EOF"""
        t = self.cur()
        if t.kind in ("NL", "EOF"):
            return
        if t.kind == "}" or (t.kind == "RB"):
            return
        if t.kind == ";" and t.text == ";":
            return
        raise S2CError("语句之间需要分隔（换行或 ';'）", t.line, t.col)

    def parse_block(self):
        """解析 { stmt* }，返回语句列表（大括号已被消费）"""
        self.expect(text="{", what="'{'")
        stmts = []
        self.skip_sep()
        while not self.at("}", text="}"):
            if self.at("EOF"):
                t = self.cur()
                raise S2CError("块未闭合（缺 '}'）", t.line, t.col)
            stmts.append(self.parse_statement())
            self.expect_stmt_end()
            self.skip_sep()
        self.next()  # '}'
        return stmts

    # ---- 语句分派 ----
    def parse_statement(self, top=False):
        t = self.cur()
        if t.kind != "ID":
            raise S2CError("无法解析的语句开始 %r" % t.text, t.line, t.col)
        kw = t.text

        if kw == "var":
            return self.parse_var()
        if kw == "free":
            return self.parse_free()
        if kw == "if":
            return self.parse_if()
        if kw == "while":
            return self.parse_while()
        if kw == "fn":
            if not top:
                raise S2CError("fn 定义仅允许顶层", t.line, t.col)
            return self.parse_fndef()
        if kw == "true" or kw == "false":
            self.next()
            return ("ret_set", "1" if kw == "true" else "0")
        if kw == "ret":
            # ret(1|0|true|false)：映射到目标"置 G_RETURN 命令"
            self.next()
            self.skip_nl()
            self.expect(text="(", what="'('")
            self.skip_nl()
            t = self.cur()
            if t.kind not in ("NUM", "ID"):
                raise S2CError("ret 参数应为 1/0/true/false", t.line, t.col)
            v = t.text
            self.next()
            self.skip_nl()
            self.expect(text=")", what="')'")
            if v == "true":
                return ("ret_set", "1")
            if v == "false":
                return ("ret_set", "0")
            if v not in ("0", "1"):
                raise S2CError("ret 参数应为 1/0/true/false，实际 %r" % v, t.line, t.col)
            return ("ret_set", v)
        if kw in RESERVED_CMD or kw in SYNTAX_WORDS:
            raise S2CError("此处不允许使用关键字 %r" % kw, t.line, t.col)

        # 命令/函数调用：name(args)
        self.next()
        if self.at("(", text="("):
            self.next()   # 消费 '('
            args = self.parse_args_after_open()
        else:
            raise S2CError("命令 %r 后需要 '('（如 %s(...)）" % (kw, kw), t.line, t.col)
        return ("call", kw, args)

    # ---- var ----
    def parse_var(self):
        t = self.next()  # 'var'
        name_tok = self.cur()
        if name_tok.kind not in ("ID",):
            raise S2CError("var 后需要变量名", name_tok.line, name_tok.col)
        if name_tok.text in SYNTAX_WORDS or name_tok.text in RESERVED_CMD:
            raise S2CError("变量名不能为保留字 %r" % name_tok.text, name_tok.line, name_tok.col)
        self.next()
        self.skip_nl()
        self.expect(text="=", what="'='")
        self.skip_nl()
        value = self.gather_value()
        return ("var", name_tok.text, value)

    def gather_value(self):
        """收集一个值/实参的文本内容：连续 STR/ID/NUM/VARREF（到分隔符为止，可跨 NL 于括号内）"""
        parts = []
        while True:
            t = self.cur()
            if t.kind in ("STR", "ID", "NUM", "VARREF"):
                if t.kind == "VARREF":
                    parts.append("${" + t.text + "}")
                else:
                    parts.append(t.text)
                self.next()
                continue
            break
        return "".join(parts)

    # ---- free ----
    def parse_free(self):
        self.next()
        # 可带一个名字（同名 token 后即分隔）
        t = self.cur()
        if t.kind == "ID" and t.text not in SYNTAX_WORDS and t.text not in RESERVED_CMD:
            name = t.text
            self.next()
            return ("free", name)
        return ("free", None)

    # ---- if ----
    def parse_if(self):
        self.next()  # 'if'
        cond = None
        if self.at("(", text="("):
            self.next()
            cond = self.parse_cond()
            self.expect(text=")", what="')'")
        then_list = self.parse_block()
        else_list = None

        # 可选的 else / else if
        self.skip_nl()
        # 允许 '}' 与 'else' 之间无分号
        t = self.cur()
        if t.kind == "ID" and t.text == "else":
            self.next()
            self.skip_nl()
            if self.at("ID", "if"):
                inner = self.parse_if()  # else if → else 块内放一个 if 语句
                else_list = [inner]
            else:
                else_list = self.parse_block()
        return ("if", cond, then_list, else_list)

    def parse_cond(self):
        """解析 if/while 的条件：命令/函数调用 或 true/false"""
        t = self.cur()
        if t.kind == "ID":
            if t.text == "true":
                self.next()
                return ("bool", True)
            if t.text == "false":
                self.next()
                return ("bool", False)
            if t.text in RESERVED_CMD or t.text in SYNTAX_WORDS:
                raise S2CError("条件不能为关键字 %r" % t.text, t.line, t.col)
            name = t.text
            self.next()
            if self.at("(", text="("):
                self.next()   # 消费 '('
                args = self.parse_args_after_open()
            else:
                args = []
            return ("call", name, args)
        raise S2CError("条件需要为命令调用或 true/false", t.line, t.col)

    # ---- while ----
    def parse_while(self):
        self.next()  # 'while'
        if not self.at("(", text="("):
            t = self.cur()
            raise S2CError("while 需要条件 while(条件){...}", t.line, t.col)
        self.next()
        cond = self.parse_cond()
        self.expect(text=")", what="')'")
        body = self.parse_block()
        return ("while", cond, body)

    # ---- fn ----
    def parse_fndef(self):
        t = self.next()  # 'fn'
        name_tok = self.cur()
        if name_tok.kind != "ID":
            raise S2CError("fn 后需要函数名", name_tok.line, name_tok.col)
        name = name_tok.text
        if name in RESERVED_CMD or name in SYNTAX_WORDS:
            raise S2CError("函数名不能为保留字 %r" % name, name_tok.line, name_tok.col)
        self.next()
        self.skip_nl()
        self.expect(text="(", what="'('")
        params = []
        self.skip_nl()
        while not self.at(")", text=")"):
            pt = self.cur()
            if pt.kind != "ID":
                raise S2CError("参数应为标识符", pt.line, pt.col)
            if pt.text in SYNTAX_WORDS or pt.text in RESERVED_CMD:
                raise S2CError("参数不能为保留字 %r" % pt.text, pt.line, pt.col)
            params.append(pt.text)
            self.next()
            self.skip_nl()
            if self.at(",", text=","):
                self.next()
                self.skip_nl()
        self.next()  # ')'
        self.in_block_depth += 1
        try:
            body = self.parse_block()
        finally:
            self.in_block_depth -= 1
        return ("fndef", name, params, body)

    # ---- 实参（'(' 已被消费）----
    def parse_args_after_open(self):
        args = []
        self.skip_nl()
        if self.at(")", text=")"):
            self.next()
            return args
        while True:
            self.skip_nl()
            if self.at(")", text=")"):
                self.next()
                break
            v = self.gather_value()
            # gather_value 可能因到达 '(' 等停止 → 那是不允许的
            t = self.cur()
            if t.kind == "EOF":
                raise S2CError("缺 ')'（括号未闭合）", t.line, t.col)
            if t.kind not in (",", ")", "NL") and not (t.kind == "NL"):
                raise S2CError("实参中出现不允许的内容 %r" % t.text, t.line, t.col)
            args.append(v)
            self.skip_nl()
            if self.at(",", text=","):
                self.next()
                continue
            self.expect(text=")", what="')'")
            break
        return args


# ============================ 编译器 ============================

class Compiler:
    def __init__(self, ret_setter="setret", max_len=256, var_max=2,
                 name_max=8, value_max=15):
        self.ret_setter = ret_setter
        self.max_len = max_len
        self.var_max = var_max
        self.name_max = name_max
        self.value_max = value_max
        self.fns = {}          # name -> ('fndef',...)
        self.warnings = []

    # ---- 引号工具 ----
    @staticmethod
    def other(q):
        return "'" if q == '"' else '"'

    def wrap_q(self, text, depth):
        """把子链文本用引号包成 SCL 分支值；depth=当前文本已处引号层数"""
        q = '"' if (depth % 2 == 0) else "'"
        if q in text:
            raise S2CError(
                "生成的子链包含与包裹引号冲突的字符 %s（内容过长或含引号）" % q)
        return q + text + q

    def quote_lit(self, text, depth):
        """给实参文本加引号（按需）；depth=当前所在引号层数（0=顶层）"""
        need = self.need_quote(text)
        if not need:
            return text
        if depth == 0:
            # 顶层：优先 "，若内容含 " 则用 '
            if '"' not in text:
                return '"' + text + '"'
            if "'" not in text:
                return "'" + text + "'"
            raise S2CError("字符串同时含 ' 与 \"，无法在顶层表示: %r" % text)
        # 非顶层：只能用“与当前最内层包裹引号相反”的类型
        q = self.other('"' if (depth % 2 == 1) else "'")
        if q in text:
            raise S2CError("该字符串在嵌套层无法安全表示（含引号 %s）: %r" % (q, text))
        return q + text + q

    @staticmethod
    def need_quote(text):
        if text == "":
            return True
        for ch in " \t,;()\"'\\\n\r":
            if ch in text:
                return True
        return False

    # ---- 主流程 ----
    def compile(self, stmts):
        chain = self.emit_stmts(stmts, 0, var_state=set())
        return chain

    def emit_stmts(self, stmts, depth, var_state, exp_stack=None):
        out = []
        vs = set(var_state)  # 本块复制（不污染外层；块间 var 生命周期按语句序累计在外层）
        for st in stmts:
            seg = self.emit_stmt(st, depth, vs, exp_stack or [])
            if seg:
                out.append(seg)
        return ";".join(out)

    def emit_stmt(self, st, depth, vs, exp_stack):
        k = st[0]
        if k == "call":
            name, args = st[1], st[2]
            if name in self.fns:
                return self.expand_fn(name, args, depth, vs, exp_stack)
            if name in RESERVED_CMD:
                raise S2CError("不能调用保留命令 %r（SCL 内置）" % name)
            return self.emit_call(name, args, depth)
        if k == "var":
            return self.emit_var(st[1], st[2], depth, vs)
        if k == "free":
            return self.emit_free(st[1], depth, vs)
        if k == "ret_set":
            return self.emit_call(self.ret_setter, [st[1]], depth)
        if k == "if":
            return self.emit_if(st, depth, vs, exp_stack)
        if k == "while":
            return self.emit_while(st, depth, vs, exp_stack)
        if k == "fndef":
            # 定义不输出（已在收集阶段放入 self.fns）
            return ""
        raise S2CError("未知 AST 节点 %r" % (k,))

    def emit_call(self, name, args, depth):
        parts = []
        for a in args:
            parts.append(self.quote_lit(a, depth))
        return name + "(" + ",".join(parts) + ")"

    def emit_var(self, name, value, depth, vs):
        if len(name) > self.name_max:
            raise S2CError("变量名 %r 过长（>%d）" % (name, self.name_max))
        if name in RESERVED_CMD or name in SYNTAX_WORDS:
            raise S2CError("变量名不能为保留字 %r" % name)
        # 字面值长度（不含 ${}）校验
        if "${" not in value and len(value) > self.value_max:
            raise S2CError("变量 %s 的字面值过长（>%d）：%r" % (name, self.value_max, value))
        vs.add(name)
        if len(vs) > self.var_max:
            raise S2CError("同时存活的变量超过 %d 个（含 %s）" % (self.var_max, name))
        return "var " + name + "=" + self.quote_lit(value, depth)

    def emit_free(self, name, depth, vs):
        if name is not None:
            if name not in vs:
                raise S2CError("释放未定义的变量 %r" % name)
            vs.discard(name)
            return "free " + name
        vs.clear()
        return "free"

    # ---- 函数内联展开 ----
    def expand_fn(self, name, args, depth, vs, exp_stack):
        if name in exp_stack:
            raise S2CError("函数递归/循环调用：%s" % " -> ".join(exp_stack + [name]))
        fd = self.fns[name]
        _, _, params, body = fd
        if len(args) != len(params):
            raise S2CError("函数 %s 需要 %d 个实参，给了 %d 个" %
                           (name, len(params), len(args)))
        sub = dict(zip(params, args))
        body2 = self.clone_sub(body, sub)
        seg = self.emit_stmts(body2, depth, vs, exp_stack + [name])
        return seg

    def clone_sub(self, stmts, sub):
        """深拷贝语句列表，并把'叶子内容 == 参数名'的实参/值替换为实参文本"""
        out = []
        for st in stmts:
            k = st[0]
            if k == "call":
                name, args = st[1], st[2]
                out.append(("call", name, [sub.get(a, a) for a in args]))
            elif k == "var":
                nm, val = st[1], st[2]
                out.append(("var", nm, sub.get(val, val)))
            elif k == "free":
                nm = st[1]
                out.append(("free", nm))
            elif k == "ret_set":
                out.append(st)
            elif k == "if":
                cond, thenl, elsel = st[1], st[2], st[3]
                c2 = self.clone_cond(cond, sub)
                t2 = self.clone_sub(thenl, sub)
                e2 = self.clone_sub(elsel, sub) if elsel is not None else None
                out.append(("if", c2, t2, e2))
            elif k == "while":
                cond, body = st[1], st[2]
                c2 = self.clone_cond(cond, sub)
                b2 = self.clone_sub(body, sub)
                out.append(("while", c2, b2))
            else:
                raise S2CError("fn 体内不支持该语句 %r" % (k,))
        return out

    @staticmethod
    def clone_cond(cond, sub):
        if cond is None:
            return None
        if cond[0] == "bool":
            return cond
        # call
        name, args = cond[1], cond[2]
        return ("call", name, [sub.get(a, a) for a in args])

    # ---- if 转译 ----
    def emit_if(self, st, depth, vs, exp_stack):
        _, cond, thenl, elsel = st
        has_then = bool(thenl)
        has_else = bool(elsel)

        cond_chain = ""
        if cond is not None:
            cond_chain = self.emit_cond(cond, depth, vs, exp_stack)

        # 两分支都空 → 只留条件（副作用保留）
        if not has_then and not has_else:
            return cond_chain

        q = '"' if (depth % 2 == 0) else "'"

        pieces = []
        if cond_chain:
            pieces.append(cond_chain)

        # then
        then_text = self.emit_stmts(thenl, depth + 1, vs, exp_stack) if has_then else ""
        # else：else 块里可能含嵌套 if（else if）→ 作为一条链文本
        else_text = ""
        if has_else:
            else_text = self.emit_stmts(elsel, depth + 1, vs, exp_stack)

        # 组装单条 if 子句
        clause = "if"
        if has_then:
            clause += " -t " + self.wrap_q(then_text, depth)
        if has_else and else_text:
            clause += " -f " + self.wrap_q(else_text, depth)
        pieces.append(clause)
        return ";".join(pieces)

    def emit_cond(self, cond, depth, vs, exp_stack):
        """条件 → 一段会产生 G_RETURN 的链"""
        if cond[0] == "bool":
            return self.emit_call(self.ret_setter, ["1" if cond[1] else "0"], depth)
        # call（可能是用户 fn）
        name, args = cond[1], cond[2]
        if name in self.fns:
            return self.expand_fn(name, args, depth, vs, exp_stack)
        if name in RESERVED_CMD:
            raise S2CError("条件不能调用保留命令 %r" % name)
        return self.emit_call(name, args, depth)

    # ---- while 转译（C 语义：门控 do-while） ----
    def emit_while(self, st, depth, vs, exp_stack):
        _, cond, body = st

        if cond[0] == "bool":
            if not cond[1]:
                return ""   # while(false) 不执行
            # while(true)：恒真（用置位器每圈维持 true，直到 SCL 兜底上限）
            self.warnings.append("while(true) 需依赖 SCL_CFG_WHILE_MAX 兜底退出，建议用带条件的 while")
            ctext = self.emit_call(self.ret_setter, ["1"], depth)
        else:
            ctext = self.emit_cond(cond, depth, vs, exp_stack)

        # 门控 do-while：cond; if -t "while -b; body; cond; while -e"
        body_text = self.emit_stmts(body, depth + 1, vs, exp_stack)
        inner_cond = self.emit_cond(cond, depth + 1, vs, exp_stack) \
            if cond[0] != "bool" else self.emit_call(self.ret_setter, ["1"], depth + 1)

        loop = "while -b"
        if body_text:
            loop += ";" + body_text
        loop += ";" + inner_cond + ";while -e"

        # 恒真时无需门控
        if cond[0] == "bool" and cond[1]:
            return loop

        # 门控：cond 先判，真才进循环
        q = '"' if (depth % 2 == 0) else "'"
        return ";".join([ctext, "if -t " + self.wrap_q(loop, depth)])


# ============================ 顶层接口 ============================

def translate(source, ret_setter="setret", max_len=256,
              var_max=2, name_max=8, value_max=15):
    """源码 → (chain, warnings)。抛 S2CError。"""
    toks = tokenize(source)
    parser = Parser(toks)
    stmts = parser.parse_program()

    comp = Compiler(ret_setter=ret_setter, max_len=max_len,
                    var_max=var_max, name_max=name_max, value_max=value_max)
    # 收集 fn（顶层定义），并按定义顺序排到 fns 表
    for st in stmts:
        if st[0] == "fndef":
            if st[1] in comp.fns:
                raise S2CError("函数 %r 重复定义" % st[1])
            comp.fns[st[1]] = st
    chain = comp.compile(stmts)
    return chain, comp.warnings


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="现代语法脚本 -> SCL 指令链（S2C 转译器）")
    ap.add_argument("input", nargs="?", default="-",
                    help="输入 .s2c 文件；缺省或 '-' 从 stdin 读")
    ap.add_argument("-o", "--output", default=None,
                    help="输出文件（缺省打印到 stdout）")
    ap.add_argument("--ret-setter", default="setret",
                    help="置 G_RETURN 命令名（ret()/true/false 映射到它，默认 setret）")
    ap.add_argument("--max-len", type=int, default=256,
                    help="单条链最大长度告警阈值（默认 256，对齐 SCL_CFG_SCRIPT_MAX）")
    ap.add_argument("--var-max", type=int, default=2, help="同时存活变量上限（默认 2）")
    ap.add_argument("--name-max", type=int, default=8, help="变量名长度上限（默认 8）")
    ap.add_argument("--value-max", type=int, default=15, help="变量字面值长度上限（默认 15）")
    args = ap.parse_args(argv)

    if args.input == "-":
        src = sys.stdin.read()
    else:
        try:
            with open(args.input, "r", encoding="utf-8") as f:
                src = f.read()
        except OSError as e:
            print("s2c: 无法读取 %s: %s" % (args.input, e), file=sys.stderr)
            return 2

    try:
        chain, warns = translate(src, ret_setter=args.ret_setter,
                                 max_len=args.max_len,
                                 var_max=args.var_max,
                                 name_max=args.name_max,
                                 value_max=args.value_max)
    except S2CError as e:
        loc = ("%s:%d:%d: " % (args.input, e.line, e.col)) if e.line else ""
        print("s2c: %serror: %s" % (loc, e.msg), file=sys.stderr)
        return 1

    if len(chain) > args.max_len:
        print("s2c: 警告: 指令链长 %d > %d（可放大 SCL_CFG_SCRIPT_MAX 或 --max-len）"
              % (len(chain), args.max_len), file=sys.stderr)
    for w in warns:
        print("s2c: 警告: " + w, file=sys.stderr)

    if args.output:
        try:
            with open(args.output, "w", encoding="utf-8") as f:
                f.write(chain + "\n")
        except OSError as e:
            print("s2c: 无法写入 %s: %s" % (args.output, e), file=sys.stderr)
            return 2
    else:
        print(chain)
    return 0


if __name__ == "__main__":
    sys.exit(main())
