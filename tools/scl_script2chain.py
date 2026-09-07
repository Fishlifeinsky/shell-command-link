#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scl_script2chain.py — 现代语法脚本 -> SCL 指令链 转译器（纯标准库，v4）

设计详见 doc/arc/script2chain-design.md。

现代语法（S2C）：
  - 注释： # ...   // ...   /* ... */
  - 语句以换行或 ';' 分隔；块用大括号 { }
  - 变量：  var 名 = 值            （名<=8、存活<=2、字面值<=15，编译期校验）
  - 释放：  free            / free 名
  - 命令调用： name(a, b, c)        （实参可为裸词/数字/字符串/ ${var} ）
  - 置返回： ret(1|0|true|false)    （映射到目标“置 G_RETURN 命令”，见 --ret-setter）
  - if：     if (条件) { } [else { }]（可 else if）
             无条件形式： if { } else { }   —— 沿用当前 G_RETURN
  - while：  while (条件) { }       （do-while 语义：body 先跑一次再判）
  - 函数：   fn 名 (参1,参2) { 语句 }   —— 编译期内联展开（文字替换），可用作条件

SCL 运行时（v4）已把 if/while 文本移除，控制流改用汇编式：
    label <名>       设置跳转点
    jump [-a] <名>   -b/默认=无条件跳；-a=G_RETURN 为真才跳(读后清零)
本转译器负责把高级 if/while **下翻译**成 label/jump 线性汇编输出。
"""

import sys
import argparse

# ============================ 保留字 ============================

RESERVED_CMD = {"if", "while", "var", "free", "help", "label", "jump"}  # 运行时/关键字
SYNTAX_WORDS = {"else", "fn", "ret", "true", "false"}                    # 语法字


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

    while i < n:
        ch = src[i]

        if ch == "\n":
            toks.append(Tok("NL", "\n", line, col))
            adv()
            continue
        if ch in " \t\r":
            adv()
            continue
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
            adv()
            toks.append(Tok("STR", "".join(buf), l0, c0))
            continue

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

        if ch.isdigit() or ch.isalpha() or ch == "_":
            buf = []
            while i < n and (src[i].isalnum() or src[i] == "_"):
                buf.append(src[i])
                adv()
            text = "".join(buf)
            kind = "NUM" if text[0].isdigit() else "ID"
            toks.append(Tok(kind, text, l0, c0))
            continue

        if ch in "(){};,=":
            toks.append(Tok(ch, ch, l0, c0))
            adv()
            continue

        raise S2CError("无法识别的字符 %r" % ch, line, col)

    toks.append(Tok("EOF", "", line, col))
    return toks


# ============================ AST ============================
#   ('call', name, [arg...])           命令/函数调用
#   ('var', name, value)               变量赋值
#   ('free', name|None)                释放
#   ('ret_set', '1'|'0')               置 G_RETURN
#   ('if', cond, then_list, else_list)  cond: None 或 ('call'..) 或 ('bool',b)
#   ('while', cond, body_list)
#   ('fndef', name, params, body)      函数定义（不输出）


# ============================ 解析器 ============================

class Parser:
    def __init__(self, toks):
        self.ts = toks
        self.p = 0
        self.in_block_depth = 0

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
        while self.at("NL") or self.at(";"):
            self.next()

    def skip_nl(self):
        while self.at("NL"):
            self.next()

    def parse_program(self):
        stmts = []
        self.skip_sep()
        while not self.at("EOF"):
            stmts.append(self.parse_statement(top=True))
            self.expect_stmt_end()
            self.skip_sep()
        return stmts

    def expect_stmt_end(self):
        t = self.cur()
        if t.kind in ("NL", "EOF"):
            return
        if t.kind == "}":
            return
        if t.kind == ";":
            return
        raise S2CError("语句之间需要分隔（换行或 ';'）", t.line, t.col)

    def parse_block(self):
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
        self.next()
        return stmts

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
            self.next()
            args = self.parse_args_after_open()
        else:
            raise S2CError("命令 %r 后需要 '('（如 %s(...)）" % (kw, kw), t.line, t.col)
        return ("call", kw, args)

    def parse_var(self):
        t = self.next()
        name_tok = self.cur()
        if name_tok.kind != "ID":
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

    def parse_free(self):
        self.next()
        t = self.cur()
        if t.kind == "ID" and t.text not in SYNTAX_WORDS and t.text not in RESERVED_CMD:
            name = t.text
            self.next()
            return ("free", name)
        return ("free", None)

    def parse_if(self):
        self.next()
        cond = None
        if self.at("(", text="("):
            self.next()
            cond = self.parse_cond()
            self.expect(text=")", what="')'")
        then_list = self.parse_block()
        else_list = None
        # else / else if 检查：允许 '}' 与 'else' 之间有换行；
        # 若无 else 则把已跳过的换行还原（留给外层当语句分隔）
        save = self.p
        self.skip_nl()
        t = self.cur()
        if t.kind == "ID" and t.text == "else":
            self.next()
            self.skip_nl()
            if self.at("ID", "if"):
                inner = self.parse_if()
                else_list = [inner]
            else:
                else_list = self.parse_block()
        else:
            self.p = save
        return ("if", cond, then_list, else_list)

    def parse_cond(self):
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
                self.next()
                args = self.parse_args_after_open()
            else:
                args = []
            return ("call", name, args)
        raise S2CError("条件需要为命令调用或 true/false", t.line, t.col)

    def parse_while(self):
        self.next()
        if not self.at("(", text="("):
            t = self.cur()
            raise S2CError("while 需要条件 while(条件){...}", t.line, t.col)
        self.next()
        cond = self.parse_cond()
        self.expect(text=")", what="')'")
        body = self.parse_block()
        return ("while", cond, body)

    def parse_fndef(self):
        t = self.next()
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
        self.next()
        self.in_block_depth += 1
        try:
            body = self.parse_block()
        finally:
            self.in_block_depth -= 1
        return ("fndef", name, params, body)

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
            t = self.cur()
            if t.kind == "EOF":
                raise S2CError("缺 ')'（括号未闭合）", t.line, t.col)
            if t.kind not in (",", ")", "NL"):
                raise S2CError("实参中出现不允许的内容 %r" % t.text, t.line, t.col)
            args.append(v)
            self.skip_nl()
            if self.at(",", text=","):
                self.next()
                continue
            self.expect(text=")", what="')'")
            break
        return args


# ============================ 编译器（输出 label/jump 线性汇编） ============================

class Compiler:
    def __init__(self, ret_setter="setret", var_max=2,
                 name_max=8, value_max=15):
        self.ret_setter = ret_setter
        self.var_max = var_max
        self.name_max = name_max
        self.value_max = value_max
        self.fns = {}
        self.warnings = []
        self._label_seq = 0

    # ---- 标签 / 引号 ----
    def new_label(self):
        self._label_seq += 1
        return "L%d" % self._label_seq

    def quote_lit(self, text):
        """给参数加引号（按需）。SCL 运行时只按空白分词，故含空白/';'等需引号。"""
        need = False
        if text == "":
            need = True
        else:
            for ch in " \t;\"'\\":
                if ch in text:
                    need = True
                    break
        if not need:
            return text
        if '"' not in text:
            return '"' + text + '"'
        if "'" not in text:
            return "'" + text + "'"
        raise S2CError("字符串同时含 ' 与 \"，无法表示: %r" % text)

    def emit_call(self, name, args):
        """SCL 普通式调用：'cmd a b'"""
        if not args:
            return name
        return name + " " + " ".join(self.quote_lit(a) for a in args)

    # ---- 主流程：产生线性代码列表 ----
    def compile(self, stmts):
        lines = []
        self.emit_stmts(stmts, lines, set(), [])
        return ";".join(lines)

    def emit_stmts(self, stmts, lines, vs, exp_stack):
        for st in stmts:
            self.emit_stmt(st, lines, vs, exp_stack)

    def code(self, stmts, vs, exp_stack):
        lines = []
        self.emit_stmts(stmts, lines, vs, exp_stack)
        return lines

    def emit_stmt(self, st, lines, vs, exp_stack):
        k = st[0]
        if k == "call":
            name, args = st[1], st[2]
            if name in self.fns:
                self.expand_fn(name, args, lines, vs, exp_stack)
                return
            if name in RESERVED_CMD:
                raise S2CError("不能调用保留命令 %r（SCL 内置）" % name)
            lines.append(self.emit_call(name, args))
        elif k == "var":
            lines.append(self.emit_var(st[1], st[2], vs))
        elif k == "free":
            lines.append(self.emit_free(st[1], vs))
        elif k == "ret_set":
            lines.append(self.emit_call(self.ret_setter, [st[1]]))
        elif k == "if":
            self.emit_if(st, lines, vs, exp_stack)
        elif k == "while":
            self.emit_while(st, lines, vs, exp_stack)
        elif k == "fndef":
            pass   # 定义不输出
        else:
            raise S2CError("未知 AST 节点 %r" % (k,))

    def emit_var(self, name, value, vs):
        if len(name) > self.name_max:
            raise S2CError("变量名 %r 过长（>%d）" % (name, self.name_max))
        if name in RESERVED_CMD or name in SYNTAX_WORDS:
            raise S2CError("变量名不能为保留字 %r" % name)
        if "${" not in value and len(value) > self.value_max:
            raise S2CError("变量 %s 的字面值过长（>%d）：%r" % (name, self.value_max, value))
        vs.add(name)
        if len(vs) > self.var_max:
            raise S2CError("同时存活的变量超过 %d 个（含 %s）" % (self.var_max, name))
        return "var " + name + "=" + self.quote_lit(value)

    def emit_free(self, name, vs):
        if name is not None:
            if name not in vs:
                raise S2CError("释放未定义的变量 %r" % name)
            vs.discard(name)
            return "free " + name
        vs.clear()
        return "free"

    # ---- 条件 → 一组会产生 G_RETURN 的代码行 ----
    def emit_cond(self, cond, out, vs, exp_stack):
        if cond[0] == "bool":
            out.append(self.emit_call(self.ret_setter, ["1" if cond[1] else "0"]))
            return
        name, args = cond[1], cond[2]
        if name in self.fns:
            self.expand_fn(name, args, out, vs, exp_stack)
            return
        if name in RESERVED_CMD:
            raise S2CError("条件不能调用保留命令 %r" % name)
        out.append(self.emit_call(name, args))

    # ---- if 转译（线性 label/jump） ----
    def emit_if(self, st, lines, vs, exp_stack):
        _, cond, thenl, elsel = st
        has_then = bool(thenl)
        has_else = bool(elsel)

        condlines = []
        if cond is not None:
            self.emit_cond(cond, condlines, vs, exp_stack)

        if not has_then and not has_else:
            lines.extend(condlines)   # 保留条件副作用
            return

        if has_then and has_else:
            # cond; jump -a Lt; <else>; jump Le; label Lt; <then>; label Le
            lt = self.new_label()
            le = self.new_label()
            lines.extend(condlines)
            lines.append("jump -a " + lt)
            lines.extend(self.code(elsel, vs, exp_stack))
            lines.append("jump " + le)
            lines.append("label " + lt)
            lines.extend(self.code(thenl, vs, exp_stack))
            lines.append("label " + le)
        elif has_then:
            # 只有 then：真→Lt 执行，假→跳到结束跳过
            lt = self.new_label()
            le = self.new_label()
            lines.extend(condlines)
            lines.append("jump -a " + lt)
            lines.append("jump " + le)
            lines.append("label " + lt)
            lines.extend(self.code(thenl, vs, exp_stack))
            lines.append("label " + le)
        else:
            # 只有 else：真→结束(跳过 else)，假→执行 else
            le = self.new_label()
            lines.extend(condlines)
            lines.append("jump -a " + le)
            lines.extend(self.code(elsel, vs, exp_stack))
            lines.append("label " + le)

    # ---- while 转译（do-while：body 先跑再判；label+条件 jump） ----
    def emit_while(self, st, lines, vs, exp_stack):
        _, cond, body = st
        lt = self.new_label()
        bodyl = self.code(body, vs, exp_stack)
        condl = []
        if cond[0] == "bool":
            if cond[1]:
                self.warnings.append(
                    "while(true) 为 do-while 且无终止条件，将循环到外部强制中止")
                condl.append(self.emit_call(self.ret_setter, ["1"]))
            else:
                condl.append(self.emit_call(self.ret_setter, ["0"]))
        else:
            self.emit_cond(cond, condl, vs, exp_stack)

        lines.append("label " + lt)
        lines.extend(bodyl)
        lines.extend(condl)
        lines.append("jump -a " + lt)

    # ---- 函数内联 ----
    def expand_fn(self, name, args, out, vs, exp_stack):
        if name in exp_stack:
            raise S2CError("函数递归/循环调用：%s" % " -> ".join(exp_stack + [name]))
        fd = self.fns[name]
        _, _, params, body = fd
        if len(args) != len(params):
            raise S2CError("函数 %s 需要 %d 个实参，给了 %d 个" %
                           (name, len(params), len(args)))
        sub = dict(zip(params, args))
        body2 = self.clone_sub(body, sub)
        self.emit_stmts(body2, out, vs, exp_stack + [name])

    def clone_sub(self, stmts, sub):
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
                out.append(st)
            elif k == "ret_set":
                out.append(st)
            elif k == "if":
                cond, thenl, elsel = st[1], st[2], st[3]
                out.append(("if", self.clone_cond(cond, sub),
                            self.clone_sub(thenl, sub),
                            self.clone_sub(elsel, sub) if elsel is not None else None))
            elif k == "while":
                cond, body = st[1], st[2]
                out.append(("while", self.clone_cond(cond, sub), self.clone_sub(body, sub)))
            else:
                raise S2CError("fn 体内不支持该语句 %r" % (k,))
        return out

    @staticmethod
    def clone_cond(cond, sub):
        if cond is None:
            return None
        if cond[0] == "bool":
            return cond
        name, args = cond[1], cond[2]
        return ("call", name, [sub.get(a, a) for a in args])


# ============================ 顶层接口 ============================

def translate(source, ret_setter="setret", max_len=512,
              var_max=2, name_max=8, value_max=15):
    """源码 → (chain, warnings)。抛 S2CError。"""
    toks = tokenize(source)
    parser = Parser(toks)
    stmts = parser.parse_program()

    comp = Compiler(ret_setter=ret_setter, var_max=var_max,
                    name_max=name_max, value_max=value_max)
    for st in stmts:
        if st[0] == "fndef":
            if st[1] in comp.fns:
                raise S2CError("函数 %r 重复定义" % st[1])
            comp.fns[st[1]] = st
    chain = comp.compile(stmts)
    return chain, comp.warnings


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="现代语法脚本 -> SCL 指令链（S2C 转译器，输出 label/jump 汇编）")
    ap.add_argument("input", nargs="?", default="-",
                    help="输入 .s2c 文件；缺省或 '-' 从 stdin 读")
    ap.add_argument("-o", "--output", default=None,
                    help="输出文件（缺省打印到 stdout）")
    ap.add_argument("--ret-setter", default="setret",
                    help="置 G_RETURN 命令名（ret()/true/false 映射到它，默认 setret）")
    ap.add_argument("--max-len", type=int, default=512,
                    help="单条链最大长度告警阈值（默认 512，对齐 SCL_CFG_SCRIPT_MAX）")
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
