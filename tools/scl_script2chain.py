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
  - while：  while (条件) { }       （标准语义：先判再跑；continue→重判、break→退出）
  - do：     do { } while (条件)    （先跑一次再判；continue→重判、break→退出）
  - when：   when [(主语)] { 匹配[,匹配] -> 体; else -> 体 }（Kotlin 风格；无主语=守卫链）
  - for：     已移除（用 var 初始化 + while/do 改写；旧脚本会得到明确报错）
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
SYNTAX_WORDS = {"else", "fn", "ret", "true", "false", "const",
                "do", "when", "for", "alias"}   # 语法字


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

        if ch in "(){};,":
            toks.append(Tok(ch, ch, l0, c0))
            adv()
            continue

        # 比较/赋值/逻辑/算术/位/负号/箭头（两字符合并；单字符各自成 token）
        if ch in "=<>!-+*/%&|^~":
            two = src[i:i + 2]
            if two in ("==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "->"):
                toks.append(Tok(two, two, l0, c0))
                adv(2)
            else:
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
            return self.parse_var(top=top)
        if kw == "const":
            if not top:
                raise S2CError("const 常量声明仅允许顶层", t.line, t.col)
            return self.parse_var(is_const=True, top=top)
        if kw == "alias":
            if not top:
                raise S2CError("alias 别名声明仅允许顶层", t.line, t.col)
            return self.parse_alias()
        if kw == "free":
            return self.parse_free()
        if kw == "if":
            return self.parse_if()
        if kw == "while":
            return self.parse_while()
        if kw == "do":
            return self.parse_do()
        if kw == "when":
            return self.parse_when()
        if kw == "for":
            raise S2CError("for 已移除：请用 var 初始化 + while(标准先判) 或 do{..}while(先跑一次) 改写",
                           t.line, t.col)
        if kw == "break" or kw == "continue":
            self.next()
            return (kw,)
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

        # 命令/函数调用 name(args) 或 赋值语句 name = expr
        self.next()
        if self.at("=", text="="):
            self.next()
            self.skip_nl()
            return ("assign", kw, self.parse_expr())
        if self.at("(", text="("):
            self.next()
            args = self.parse_args_after_open()
        else:
            raise S2CError("命令 %r 后需要 '('（如 %s(...)）" % (kw, kw), t.line, t.col)
        return ("call", kw, args)

    def parse_var(self, is_const=False, top=False):
        # 支持 'var <type> <name>=<value>'、顶层 'const <type> <name>=<value>'、
        # 以及顶层 'var const <type> <name>=<value>'（type 可省略→自动推断）
        self.next()   # 消费 var / const
        if (not is_const) and self.at("ID", "const"):
            # 'var const ...'：只读常量声明，仅允许顶层（与 const 关键字一致）
            if not top:
                t = self.cur()
                raise S2CError("const 常量声明仅允许顶层", t.line, t.col)
            is_const = True
            self.next()
        t = self.cur()
        typ = None
        if t.kind == "ID" and t.text in ("bool", "int", "flag", "string"):
            typ = t.text
            self.next()
            t = self.cur()
        name_tok = t
        if name_tok.kind != "ID":
            raise S2CError("var 后需要变量名", name_tok.line, name_tok.col)
        if name_tok.text in SYNTAX_WORDS or name_tok.text in RESERVED_CMD:
            raise S2CError("变量名不能为保留字 %r" % name_tok.text, name_tok.line, name_tok.col)
        self.next()
        self.skip_nl()
        self.expect(text="=", what="'='")
        self.skip_nl()
        value = self.gather_value()
        if is_const:
            return ("constvar", name_tok.text, typ, value)
        return ("var", name_tok.text, typ, value)

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
            if t.kind == "-" or t.text == "-":   # 负数字面量 / flag 的 '-x'
                parts.append("-")
                self.next()
                continue
            break
        return "".join(parts)

    # ---- 赋值语句右侧表达式（v0.3：完整算术/位，优先级递归） ----
    def parse_expr(self):
        return self.parse_bit_or()

    def parse_bit_or(self):
        left = self.parse_bit_xor()
        while self.at("|", text="|"):
            self.next()
            right = self.parse_bit_xor()
            left = ("bin", "|", left, right)
        return left

    def parse_bit_xor(self):
        left = self.parse_bit_and()
        while self.at("^", text="^"):
            self.next()
            right = self.parse_bit_and()
            left = ("bin", "^", left, right)
        return left

    def parse_bit_and(self):
        left = self.parse_shift()
        while self.at("&", text="&"):
            self.next()
            right = self.parse_shift()
            left = ("bin", "&", left, right)
        return left

    def parse_shift(self):
        left = self.parse_add()
        while self.at("<<", text="<<") or self.at(">>", text=">>"):
            op = self.next().text
            right = self.parse_add()
            left = ("bin", op, left, right)
        return left

    def parse_add(self):
        left = self.parse_mul()
        while self.at("+", text="+") or self.at("-", text="-"):
            op = self.next().text
            self.skip_nl()
            right = self.parse_mul()
            left = ("bin", op, left, right)
        return left

    def parse_mul(self):
        left = self.parse_unary()
        while self.at("*", text="*") or self.at("/", text="/") or self.at("%", text="%"):
            op = self.next().text
            self.skip_nl()
            right = self.parse_unary()
            left = ("bin", op, left, right)
        return left

    def parse_unary(self):
        if self.at("-", text="-"):
            self.next()
            x = self.parse_unary()
            if x[0] == "num":
                return ("num", "-" + x[1])   # 负字面量折叠
            return ("neg", x)
        if self.at("~", text="~"):
            self.next()
            return ("notb", self.parse_unary())
        if self.at("+", text="+"):
            self.next()
            return self.parse_unary()
        return self.parse_primary()

    def parse_primary(self):
        t = self.cur()
        if t.kind == "(" or t.text == "(":
            self.next()
            inner = self.parse_expr()
            self.expect(text=")", what="')'")
            return inner
        if t.kind == "NUM":
            self.next()
            return ("num", t.text)
        if t.kind == "STR":
            self.next()
            return ("str", t.text)
        if t.kind == "ID":
            if t.text in ("true", "false"):
                self.next()
                return ("bool", t.text == "true")
            if t.text in RESERVED_CMD or t.text in SYNTAX_WORDS:
                raise S2CError("赋值右值不能为关键字 %r" % t.text, t.line, t.col)
            name = t.text
            self.next()
            if self.at("(", text="("):
                raise S2CError("赋值右值暂不支持函数调用结果", t.line, t.col)
            return ("id", name)
        raise S2CError("赋值右值无法解析 %r" % t.text, t.line, t.col)

    def parse_alias(self):
        """alias <name> <target>：编译期别名——之后对 name 的引用等价于 target。
        仅顶层（与 const 一致）。target 可为普通变量/另一别名/const 常量名。"""
        self.next()   # 消费 alias
        n = self.cur()
        if n.kind != "ID":
            raise S2CError("alias 后需要别名（identifier）", n.line, n.col)
        name = n.text
        self.next()
        self.skip_nl()
        t = self.cur()
        if t.kind != "ID":
            raise S2CError("alias %r 后需要目标变量名" % name, t.line, t.col)
        target = t.text
        self.next()
        return ("alias", name, target)

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

    CMP_OPS = ("==", "!=", "<=", ">=", "<", ">")

    def parse_cond(self):
        """条件表达式（完整逻辑层，v0.3）：or → and → not → 比较 → 原子/括号"""
        return self.parse_cond_or()

    def parse_cond_or(self):
        left = self.parse_cond_and()
        while self.at("||", text="||"):
            self.next()
            right = self.parse_cond_and()
            left = ("or", left, right)
        return left

    def parse_cond_and(self):
        left = self.parse_cond_not()
        while self.at("&&", text="&&"):
            self.next()
            right = self.parse_cond_not()
            left = ("and", left, right)
        return left

    def parse_cond_not(self):
        if self.at("!", text="!"):
            self.next()
            inner = self.parse_cond_not()
            return ("not", inner)
        return self.parse_cond_cmp()

    def parse_cond_cmp(self):
        left = self.parse_cond_atom()
        t = self.cur()
        if t.kind in self.CMP_OPS:
            op = self.next().text
            right = self.parse_cond_atom()
            if left[0] == "call" or right[0] == "call":
                raise S2CError("比较两侧不能是命令调用", t.line, t.col)
            return ("cmp", op, left, right)
        return left

    def parse_cond_atom(self):
        """原子：括号(按内容自动区分布尔组/算术)、true·false、命令调用、变量、字面量"""
        t = self.cur()
        if t.kind == "(" or t.text == "(":
            if self._paren_has_bool(self.p):
                self.next()
                inner = self.parse_cond()          # 布尔组：含 &&/||/!/比较
            else:
                self.next()
                inner = self.parse_expr()          # 算术/位组：如 (i&1)、(a+b)
            self.expect(text=")", what="')'")
            return inner
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
                return ("call", name, args)
            return ("var", name)
        if t.kind == "NUM":
            self.next()
            return ("lit", t.text)
        if t.kind == "STR":
            self.next()
            return ("str", t.text)
        if t.text == "-":
            self.next()
            u = self.cur()
            if u.kind == "NUM":
                self.next()
                return ("lit", "-" + u.text)
            raise S2CError("条件负号后需要数字", t.line, t.col)
        raise S2CError("条件无法解析 %r" % t.text, t.line, t.col)

    def _paren_has_bool(self, open_pos):
        """lookahead：'(' 内是否含布尔结构（&& || ! 或 比较符）→ 是则按布尔组解析，
        否则按算术/位表达式（如 (i & 1) 作为比较两侧的操作数）。不移动 self.p"""
        k = open_pos + 1
        n = len(self.ts)
        depth = 1
        while k < n:
            tk = self.ts[k]
            if tk.kind == "(":
                depth += 1
            elif tk.kind == ")":
                depth -= 1
                if depth == 0:
                    break
            elif depth == 1 and (tk.kind in ("&&", "||", "!") or
                                 tk.kind in self.CMP_OPS):
                return True
            k += 1
        return False

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

    # ---- v0.4 do { body } while(cond)：先跑一次再判（原 while 的 do-while 语义显式化） ----
    def parse_do(self):
        self.next()                       # do
        body = self.parse_block()
        self.skip_nl()
        if not self.at("ID", "while"):
            t = self.cur()
            raise S2CError("do 需要 do { } while(条件)", t.line, t.col)
        self.next()
        self.skip_nl()
        self.expect(text="(", what="'('")
        cond = self.parse_cond()
        self.expect(text=")", what="')'")
        return ("do", cond, body)

    # ---- when（参考 Kotlin；主语可选）----
    def parse_when(self):
        self.next()                       # when
        subj = None
        if self.at("(", text="("):
            self.next()
            self.skip_nl()
            subj = self.parse_when_atom("when 主语")
            self.skip_nl()
            self.expect(text=")", what="')'")
        self.expect(text="{", what="'{'")
        arms = []
        saw_else = False
        self.skip_sep()
        while not self.at("}", text="}"):
            if self.at("EOF"):
                t = self.cur()
                raise S2CError("when 块未闭合（缺 '}'）", t.line, t.col)
            if self.at("ID", "else"):
                if saw_else:
                    t = self.cur()
                    raise S2CError("when 只能有一个 else 分支", t.line, t.col)
                saw_else = True
                self.next()
                self.skip_nl()
                if not self.at("->", text="->"):
                    t = self.cur()
                    raise S2CError("when 的 else 分支需要 '->'", t.line, t.col)
                self.next()
                arms.append(("else", None, self.parse_when_body()))
            elif subj is None:
                # 无主语：守卫链（依次判条件）
                cond = self.parse_cond()
                self.skip_nl()
                self.expect(text="->", what="'->'")
                arms.append(("guard", cond, self.parse_when_body()))
            else:
                # 有主语：匹配值（可多个，逗号分隔）
                matchers = []
                while not self.at("->", text="->"):
                    if self.at("}", text="}") or self.at("EOF"):
                        t = self.cur()
                        raise S2CError("when 分支缺少 '->'", t.line, t.col)
                    matchers.append(self.parse_when_atom("匹配值"))
                    if self.at(",", text=","):
                        self.next()
                        self.skip_nl()
                self.next()               # ->
                arms.append(("match", matchers, self.parse_when_body()))
            self.skip_sep()
        self.next()                       # }
        if not arms:
            t = self.cur()
            raise S2CError("when 至少需要一个分支", t.line, t.col)
        return ("when", subj, arms)

    def parse_when_atom(self, what):
        """when 主语/匹配值：变量/字面量/字符串/${}引用/flag(-x)/bool —— 原子，不支持复合表达式"""
        t = self.cur()
        if t.kind == "ID":
            if t.text in ("true", "false"):
                self.next()
                return ("bool", t.text == "true")
            if t.text in RESERVED_CMD or t.text in SYNTAX_WORDS:
                raise S2CError("%s不能为关键字 %r" % (what, t.text), t.line, t.col)
            self.next()
            return ("var", t.text)
        if t.kind == "NUM":
            self.next()
            return ("lit", t.text)
        if t.kind == "STR":
            self.next()
            return ("str", t.text)
        if t.kind == "VARREF":
            self.next()
            return ("varref", t.text)
        if t.kind == "-" or t.text == "-":
            self.next()
            u = self.cur()
            if u.kind in ("NUM", "ID"):
                self.next()
                return ("lit", "-" + u.text)
            raise S2CError("%s：负号后需要数字/标识" % what, t.line, t.col)
        raise S2CError("%s无法解析 %r（仅支持变量/字面量/字符串）" % (what, t.text),
                      t.line, t.col)

    def parse_when_body(self):
        """when 分支体：{块} 或单语句（其后须 换行/; / }）"""
        if self.at("{", text="{"):
            return self.parse_block()
        body = [self.parse_statement()]
        t = self.cur()
        if t.kind in ("NL", ";", "}"):
            return body
        raise S2CError("when 分支体后需要换行/分号或 {} 块", t.line, t.col)

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

# v0.2：表达式 → 运行时内置运算指令（scl.h）
INT_OPWORD = {"+": "iadd", "-": "isub", "*": "imul", "/": "idiv", "%": "imod",
              "&": "iand", "|": "ior", "^": "ixor", "<<": "shl", ">>": "shr"}
UNARY_OPWORD = {"neg": "ineg", "notb": "inot"}
CMP_OPWORD = {"==": "ieq", "!=": "ine", "<": "ilt", "<=": "ile",
              ">": "igt", ">=": "ige"}


def infer_var_type(value):
    """var 未显式类型时推断：true/false→bool；-x→flag；十进制/0x/0b→int；其余→string"""
    v = value
    if v.lower() in ("true", "false"):
        return "bool"
    body = v[1:] if v.startswith("-") else v
    if body[:2].lower() in ("0x", "0b"):
        rest = body[2:]
        if rest and all(c in "0123456789abcdefABCDEF" for c in rest):
            return "int"
        return "string"
    if body.isdigit():
        return "int"
    if len(v) == 2 and v[0] == "-" and v[1].isalpha():
        return "flag"
    return "string"


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
        self._fn_runtime = set()   # v0.3d：转运行时子程序（callf/retf）的函数名
        self._fn_label = {}        # 运行时函数名 → 子程序入口 label
        self._used_runtime = []    # 实际发射过 callf 的运行时函数名（顺序生成体）
        self._in_runtime = False   # 正在编译运行时函数体（内部 fn 调用一律内联）
        self.vtypes = {}      # v0.2：变量名 → bool/int/flag/string（编译期跟踪）
        self._tmpn = 0        # v0.3：隐藏临时变量序号（__t0..）
        self._loop = []       # v0.3：循环上下文栈 {break:, continue:}（for/while 的 break/continue）
        self._tmp_stack = []  # v0.3：当前表达式的隐藏临时变量列表（用后 free）
        self.const_fold = False  # v0.3b：const 折叠模式（真常量，不产 var，编进 Flash 程序）
        self.consts = {}         # const_fold 时：常量名 → (type, 规范化值文本)
        self.aliases = {}        # v0.3c：编译期变量别名 name → target（引用处替换）

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

    # ---- v0.3b/c：const 折叠 + alias 编译期替换 ----
    def declare_alias(self, name, target):
        """登记编译期别名 name → target（alias <name> <target>）。"""
        if name in SYNTAX_WORDS or name in RESERVED_CMD:
            raise S2CError("别名不能为保留字 %r" % name)
        if name in self.aliases:
            raise S2CError("别名 %r 重复定义" % name)
        if name in self.consts:
            raise S2CError("别名 %r 与 const 常量同名" % name)
        if name in self.vtypes:
            raise S2CError("别名 %r 与已声明的变量/常量同名" % name)
        if name == target:
            raise S2CError("别名不能指向自己: %r" % name)
        if target in RESERVED_CMD:
            raise S2CError("别名目标不能为保留字 %r" % target)
        self.aliases[name] = target

    def resolve(self, name):
        """沿别名链解析变量名到最终名（编译期别名替换）。无别名原样返回。"""
        steps = 0
        while name in self.aliases:
            name = self.aliases[name]
            steps += 1
            if steps > len(self.aliases):
                raise S2CError("别名循环引用")
        return name

    def declare_const(self, name, typ, value):
        """预扫登记顶层 const（const_fold 模式）。校验名/值/类型，不占变量槽。"""
        if len(name) > self.name_max:
            raise S2CError("常量名 %r 过长（>%d）" % (name, self.name_max))
        if name in RESERVED_CMD or name in SYNTAX_WORDS:
            raise S2CError("常量名不能为保留字 %r" % name)
        if "${" in value:
            raise S2CError("const 常量值不能引用变量: %r" % value)
        if len(value) > self.value_max:
            raise S2CError("常量 %s 的字面值过长（>%d）：%r" % (name, self.value_max, value))
        if typ is None:
            typ = infer_var_type(value)
        if name in self.consts:
            raise S2CError("常量 %r 重复声明" % name)
        if name in self.aliases:
            raise S2CError("常量 %r 与别名同名" % name)
        self.consts[name] = (typ, value)

    def const_of(self, name):
        """name 是否为 const 常量（先过别名）；是则返回 (type, 值文本)，否则 None。"""
        if not self.const_fold:
            return None
        return self.consts.get(self.resolve(name))

    def fold_varref(self, text):
        """展开/折叠参数文本里的 ${name}：
        - 别名(alias) → 编译期替换为目标名（恒生效）
        - const 常量（const_fold 模式）→ 折叠为字面量值
        - 其余 ${name} 保留原样（运行时展开）"""
        if "${" not in text:
            return text
        out = []
        i = 0
        n = len(text)
        while i < n:
            j = text.find("${", i)
            if j < 0:
                out.append(text[i:])
                break
            k = text.find("}", j + 2)
            if k < 0:
                out.append(text[i:])
                break
            name = text[j + 2:k]
            rn = self.resolve(name)
            e = self.consts.get(rn) if (self.const_fold and rn in self.consts) else None
            out.append(text[i:j])
            if e is not None:
                out.append(e[1])
            elif rn != name:
                out.append("${" + rn + "}")
            else:
                out.append(text[j:k + 1])
            i = k + 1
        return "".join(out)

    def _const_int_lit(self, name):
        """const 名 → 可作 int 运算/数值比较操作数的字面量文本；非 int 常量返回 None。"""
        e = self.consts.get(self.resolve(name)) if self.const_fold else None
        if e is None:
            return None
        if e[0] == "int":
            return e[1]
        return None

    def emit_call(self, name, args):
        """SCL 普通式调用：'cmd a b'（参数里 ${别名/常量} 编译期替换）"""
        if not args:
            return name
        args = [self.fold_varref(a) for a in args]
        return name + " " + " ".join(self.quote_lit(a) for a in args)

    # ---- v0.3d：fn 运行时子程序分析（无参 + 体>3 + 调用>3 才转 callf/retf，否则内联） ----
    def _count_calls(self, sts):
        """统计整个程序（顶层 + 所有 fn 体）里对用户 fn 的调用次数。"""
        cnt = {}
        def walk(s):
            for st in s:
                k = st[0]
                if k == "call":
                    if st[1] in self.fns:
                        cnt[st[1]] = cnt.get(st[1], 0) + 1
                elif k == "if":
                    walk(st[2])
                    if st[3] is not None:
                        walk(st[3])
                elif k == "while":
                    walk(st[2])
                elif k == "do":
                    walk(st[2])
                elif k == "when":
                    for (_k, _a, b) in st[2]:
                        walk(b)
        for st in sts:
            if st[0] == "fndef":
                walk(st[3])
            else:
                walk([st])
        return cnt

    def _body_blocked(self, body):
        """运行时函数体禁止：var/constvar/alias/free/break/continue/ret_set
        （这些依赖调用点上下文或变量生命周期，转子程序会失真）。含则回退内联。"""
        def walk(s):
            for st in s:
                k = st[0]
                if k in ("var", "constvar", "alias", "free",
                         "break", "continue", "ret_set"):
                    return True
                if k == "if":
                    if walk(st[2]):
                        return True
                    if st[3] is not None and walk(st[3]):
                        return True
                elif k == "while":
                    if walk(st[2]):
                        return True
                elif k == "do":
                    if walk(st[2]):
                        return True
                elif k == "when":
                    for (_k, _a, b) in st[2]:
                        if walk(b):
                            return True
            return False
        return walk(body)

    def _analyze_fns(self, stmts):
        """fn → 内联 or 运行时子程序。规则（无参前提下）：
        体语句>3 且 全程序调用>3 → 运行时（callf/retf，体只存一份）；否则内联。
        带参 fn 恒内联；体含 var/free/break 等禁止节点则回退内联。"""
        calls = self._count_calls(stmts)
        for name, fd in self.fns.items():
            _, _, params, body = fd
            if params:
                continue                       # 带参 fn：参数须克隆替换，只能内联
            if len(body) <= 3:
                continue
            if calls.get(name, 0) <= 3:
                continue
            if self._body_blocked(body):
                continue                       # 保守回退：内联语义保持原状
            self._fn_runtime.add(name)

    def _fn_lab(self, name):
        """取/分配运行时函数入口 label。"""
        if name not in self._fn_label:
            self._fn_label[name] = self.new_label()
        return self._fn_label[name]

    # ---- 主流程：产生线性代码列表 ----
    def compile(self, stmts):
        lines = []
        self._analyze_fns(stmts)
        self.emit_stmts(stmts, lines, set(), [])
        # 主流程之后：为每个实际 callf 过的运行时函数追加唯一子程序体
        for name in self._used_runtime:
            fd = self.fns[name]
            _, _, _params, body = fd
            bl = []
            self._in_runtime = True
            try:
                self.emit_stmts(body, bl, set(), [name])
            finally:
                self._in_runtime = False
            lines.append("label " + self._fn_label[name])
            lines.extend(bl)
            lines.append("retf")
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
                if self._in_runtime:
                    # 运行时函数体内部：一律内联（无嵌套 callf/retf）
                    self.expand_fn(name, args, lines, vs, exp_stack)
                elif name in self._fn_runtime:
                    # 无参大函数多调用点 → 转运行时子程序调用（体末尾唯一一份）
                    if name not in self._used_runtime:
                        self._used_runtime.append(name)
                    lines.append("callf " + self._fn_lab(name))
                else:
                    self.expand_fn(name, args, lines, vs, exp_stack)
                return
            if name in RESERVED_CMD:
                raise S2CError("不能调用保留命令 %r（SCL 内置）" % name)
            lines.append(self.emit_call(name, args))
        elif k == "var":
            lines.append(self.emit_var(st[1], st[2], st[3], vs))
        elif k == "constvar":
            if self.const_fold:
                pass   # 真常量已预扫登记（declare_const）；不产 var 指令、不占变量槽
            else:
                lines.append(self.emit_var(st[1], st[2], st[3], vs, is_const=True))
        elif k == "alias":
            self.declare_alias(st[1], st[2])   # 编译期别名，不产链
        elif k == "free":
            lines.append(self.emit_free(st[1], vs))
        elif k == "ret_set":
            lines.append(self.emit_call(self.ret_setter, [st[1]]))
        elif k == "assign":
            self.emit_assign(st[1], st[2], lines, vs)
        elif k == "if":
            self.emit_if(st, lines, vs, exp_stack)
        elif k == "while":
            self.emit_while(st, lines, vs, exp_stack)
        elif k == "do":
            self.emit_do(st, lines, vs, exp_stack)
        elif k == "when":
            self.emit_when(st, lines, vs, exp_stack)
        elif k in ("break", "continue"):
            if not self._loop:
                raise S2CError("%s 只能在循环内使用" % k)
            tag = "break" if k == "break" else "continue"
            lines.append("jump " + self._loop[-1][tag])
        elif k == "fndef":
            pass   # 定义不输出
        else:
            raise S2CError("未知 AST 节点 %r" % (k,))

    def emit_var(self, name, typ, value, vs, is_const=False):
        if name in self.aliases:
            raise S2CError("不能声明与别名同名的变量 %r（alias %s）" % (name, self.aliases[name]))
        if self.const_fold and name in self.consts:
            raise S2CError("const 常量 %r 不可重新 var 声明" % name)
        value = self.fold_varref(value)   # 初值里的 ${别名/常量} 编译期替换
        if len(name) > self.name_max:
            raise S2CError("变量名 %r 过长（>%d）" % (name, self.name_max))
        if name in RESERVED_CMD or name in SYNTAX_WORDS:
            raise S2CError("变量名不能为保留字 %r" % name)
        if "${" not in value and len(value) > self.value_max:
            raise S2CError("变量 %s 的字面值过长（>%d）：%r" % (name, self.value_max, value))
        vs.add(name)
        if len(vs) > self.var_max:
            raise S2CError("同时存活的变量超过 %d 个（含 %s）" % (self.var_max, name))
        if typ is None:
            typ = infer_var_type(value)   # v0.2：现代层允许省略类型（编译器推断）
        self.vtypes[name] = typ
        return ("var const " if is_const else "var ") + \
               "%s %s=%s" % (typ, name, self.quote_lit(value))

    def emit_free(self, name, vs):
        if name is not None:
            name = self.resolve(name)
            if name not in vs:
                raise S2CError("释放未定义的变量 %r" % name)
            vs.discard(name)
            self.vtypes.pop(name, None)
            return "free " + name
        vs.clear()
        self.vtypes.clear()
        return "free"

    # ---- 赋值语句：name = expr（v0.3：完整算术/位，多运算符，用隐藏临时变量 __t） ----
    def emit_assign(self, name, expr, lines, vs):
        if self.const_fold and name in self.consts:
            raise S2CError("const 常量 %r 不可赋值" % name)
        typ = self.vtypes.get(name)
        if typ is None:
            raise S2CError("赋值目标 %r 需先用 var 声明" % name)
        k = expr[0]
        self._tmp_stack = []
        try:
            if typ == "string":
                if k == "str":
                    lines.append("var string %s=%s" % (name, self.quote_lit(expr[1])))
                elif k == "id":
                    c = self.const_of(expr[1])
                    if c is not None:
                        lines.append("var string %s=%s" % (name, self.quote_lit(c[1])))
                    else:
                        lines.append("var string %s=${%s}" % (name, self.resolve(expr[1])))
                else:
                    raise S2CError("string 变量 %r 只能赋字符串/变量" % name)
                return
            if typ == "bool":
                if k == "bool":
                    lines.append("var bool %s=%s" % (name, "true" if expr[1] else "false"))
                elif k == "id":
                    c = self.const_of(expr[1])
                    if c is not None:
                        if c[0] == "bool":
                            lines.append("var bool %s=%s" % (name, c[1]))
                        else:
                            raise S2CError("bool 变量 %r 不能赋 %s 常量 %r"
                                           % (name, c[0], expr[1]))
                    else:
                        lines.append("var bool %s=${%s}" % (name, self.resolve(expr[1])))
                elif k == "num":
                    lines.append("var bool %s=%s" % (name, expr[1]))
                else:
                    raise S2CError("bool 变量 %r 不支持该赋值" % name)
                return
            if typ != "int":
                raise S2CError("算术写回目标 %r 应为 int（当前 %s）" % (name, typ))
            self.emit_arith_to(name, expr, lines, vs)
        finally:
            for t in self._tmp_stack:
                lines.append("free " + t)
                vs.discard(t)
                self.vtypes.pop(t, None)
            self._tmp_stack = []

    def alloc_temp(self, vs):
        n = self._tmpn
        self._tmpn += 1
        nm = "__t%d" % n
        vs.add(nm)
        self.vtypes[nm] = "int"
        self._tmp_stack.append(nm)
        return nm

    def emit_arith_to(self, dst, expr, lines, vs):
        """把表达式 expr 求值为 int 写入变量 dst（int）"""
        k = expr[0]
        if k == "num":
            lines.append("var int %s=%s" % (dst, expr[1]))
            return
        if k == "bool":
            lines.append("var int %s=%s" % (dst, "1" if expr[1] else "0"))
            return
        if k == "id":
            c = self.const_of(expr[1])
            if c is not None:
                if c[0] == "int":
                    lines.append("var int %s=%s" % (dst, c[1]))
                else:
                    raise S2CError("int 运算不能使用 %s 常量 %r" % (c[0], expr[1]))
                return
            lines.append("var int %s=${%s}" % (dst, self.resolve(expr[1])))
            return
        if k in ("neg", "notb"):
            src = self.emit_operand(expr[1], lines, vs)
            lines.append("%s %s %s" % (UNARY_OPWORD[k], src, dst))
            return
        if k == "bin":
            _, op, L, R = expr
            if op not in INT_OPWORD:
                raise S2CError("不支持的运算符 %r" % op)
            lt = self.emit_operand(L, lines, vs)
            rt = self.emit_operand(R, lines, vs)
            lines.append("%s %s %s %s" % (INT_OPWORD[op], lt, rt, dst))
            return
        if k == "str":
            raise S2CError("字符串不能参与 int 运算")
        raise S2CError("未知表达式节点 %r" % (expr,))

    def emit_operand(self, expr, lines, vs):
        """返回可作操作数的文本（变量名/字面量）；复合子式先求到隐藏临时变量"""
        k = expr[0]
        if k == "id":
            c = self.const_of(expr[1])
            if c is not None:
                if c[0] == "int":
                    return c[1]          # 常量折叠为数字字面量
                raise S2CError("int 运算不能使用 %s 常量 %r" % (c[0], expr[1]))
            return self.resolve(expr[1])
        if k == "num":
            return expr[1]
        if k == "bool":
            return "1" if expr[1] else "0"
        if k == "str":
            raise S2CError("字符串不能参与 int 运算")
        if len(self._tmp_stack) >= 4:
            raise S2CError("单表达式临时变量过多（>4），请拆开写")
        t = self.alloc_temp(vs)
        self.emit_arith_to(t, expr, lines, vs)
        return t

    # ---- 条件 → 一组会产生 G_RETURN 的代码行（v0.3 支持 && || ! 括号短路） ----
    def emit_cond(self, cond, out, vs, exp_stack):
        k = cond[0]
        if k == "bool":
            out.append(self.emit_call(self.ret_setter, ["1" if cond[1] else "0"]))
            return
        if k == "call":
            name, args = cond[1], cond[2]
            if name in self.fns:
                self.expand_fn(name, args, out, vs, exp_stack)
                return
            if name in RESERVED_CMD:
                raise S2CError("条件不能调用保留命令 %r" % name)
            out.append(self.emit_call(name, args))
            return
        if k == "var":
            c = self.const_of(cond[1])
            if c is not None:
                if c[0] == "bool":
                    out.append(self.emit_call(self.ret_setter,
                                              ["1" if c[1].lower() == "true" else "0"]))
                elif c[0] in ("int", "flag"):
                    out.append("btest " + c[1])   # 常量真值 → G_RETURN
                else:
                    raise S2CError("字符串常量 %r 不能直接作条件真值" % cond[1])
                return
            out.append("btest " + self.resolve(cond[1]))  # 变量真值 → G_RETURN
            return
        if k == "lit":
            out.append("btest " + cond[1])     # 字面量真值 → G_RETURN
            return
        if k == "cmp":
            _, op, L, R = cond
            if op not in CMP_OPWORD:
                raise S2CError("不支持比较符 %r" % op)
            # v0.3：字符串/flag 类型 → seq/sneq 文本比较
            if self._is_str(L) or self._is_str(R):
                if op not in ("==", "!="):
                    raise S2CError("字符串/flag 仅支持 == / != 比较")
                if not (self._is_str(L) and self._is_str(R)):
                    raise S2CError("字符串/flag 与数值不能直接比较")
                lt = self._str_text(L)
                rt = self._str_text(R)
                out.append(("seq" if op == "==" else "sneq") + " " + lt + " " + rt)
                return
            old = self._tmp_stack
            self._tmp_stack = []
            lt = self._cond_val(L, out, vs)
            rt = self._cond_val(R, out, vs)
            out.append("%s %s %s" % (CMP_OPWORD[op], lt, rt))
            for x in self._tmp_stack:
                out.append("free " + x)
                vs.discard(x)
                self.vtypes.pop(x, None)
            self._tmp_stack = old
            return
        if k == "not":
            x = cond[1]
            if x[0] == "bool":
                out.append(self.emit_call(self.ret_setter, ["0" if x[1] else "1"]))
                return
            c = self.const_of(x[1]) if x[0] == "var" else None
            if c is not None and c[0] == "bool":
                out.append(self.emit_call(self.ret_setter,
                                          ["0" if c[1].lower() == "true" else "1"]))
                return
            tx = self.cond_text(x)
            if tx is not None:      # 原子（变量/字面量）→ 直接 bnot，最短
                out.append("bnot " + tx)
                return
            # 复合子式：先求 x → G_RETURN，再用跳转反转
            self.emit_cond(x, out, vs, exp_stack)
            l1 = self.new_label()
            l2 = self.new_label()
            out.append("jump -a " + l1)                      # x 真 → L1
            out.append(self.emit_call(self.ret_setter, ["1"]))  # x 假 → not=1
            out.append("jump " + l2)
            out.append("label " + l1)
            out.append(self.emit_call(self.ret_setter, ["0"]))  # x 真 → not=0
            out.append("label " + l2)
            return
        if k == "and":
            a, b = cond[1], cond[2]
            l1 = self.new_label()
            l2 = self.new_label()
            self.emit_cond(a, out, vs, exp_stack)            # G_RETURN=a
            out.append("jump -a " + l1)                      # a 真 → 求 b
            out.append(self.emit_call(self.ret_setter, ["0"]))  # a 假 → 0
            out.append("jump " + l2)
            out.append("label " + l1)
            self.emit_cond(b, out, vs, exp_stack)            # 结果 = b
            out.append("label " + l2)
            return
        if k == "or":
            a, b = cond[1], cond[2]
            l1 = self.new_label()
            l2 = self.new_label()
            self.emit_cond(a, out, vs, exp_stack)            # G_RETURN=a
            out.append("jump -a " + l1)                      # a 真 → 短路真
            self.emit_cond(b, out, vs, exp_stack)            # a 假 → 结果=b
            out.append("jump " + l2)
            out.append("label " + l1)
            out.append(self.emit_call(self.ret_setter, ["1"]))  # 真短路
            out.append("label " + l2)
            return
        raise S2CError("未知条件节点 %r" % (k,))

    def _is_str(self, n):
        """节点是否为字符串语境：引号字面量，或 string/flag 类型变量（含 string/flag 常量）"""
        if n[0] == "str":
            return True
        if n[0] == "var":
            c = self.const_of(n[1])
            if c is not None:
                return c[0] in ("string", "flag")
            return self.vtypes.get(self.resolve(n[1])) in ("string", "flag")
        return False

    def _str_text(self, n):
        if n[0] == "str":
            return self.quote_lit(n[1])   # 含空格需引号
        if n[0] == "var":
            c = self.const_of(n[1])
            if c is not None:
                return self.quote_lit(c[1])   # 常量文本（含空格需引号）
            return "${" + self.resolve(n[1]) + "}"  # 运行时按变量文本展开
        raise S2CError("字符串比较操作数不支持 %r" % (n,))

    def cond_text(self, x):
        if x[0] == "var":
            c = self.const_of(x[1])
            if c is not None:
                # int 常量作比较操作数 → 数字字面量；bool/flag/string 走各自分支
                return c[1] if c[0] == "int" else None
            return self.resolve(x[1])
        if x[0] == "lit":
            return x[1]
        if x[0] == "bool":
            return "1" if x[1] else "0"
        return None

    def _cond_val(self, node, out, vs):
        """把条件比较两侧的节点化为可比较操作数文本：
        变量/字面量直接用；算术/位复合式先求到隐藏临时变量 __tN（由调用方统一 free）"""
        t = self.cond_text(node)
        if t is not None:
            return t
        name = self.alloc_temp(vs)
        self.emit_arith_to(name, node, out, vs)
        return name

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

    # ---- while(cond) 转译：标准先判再跑（continue→条件, break→出口） ----
    def emit_while(self, st, lines, vs, exp_stack):
        _, cond, body = st
        lc = self.new_label()    # 条件 / 回跳（continue 落此）
        lb = self.new_label()    # body 入口
        le = self.new_label()    # 出口（break 落此）
        condl = []
        if cond[0] == "bool":
            if cond[1]:
                self.warnings.append("while(true) 无终止条件，将循环到外部强制中止（步进保护兜底）")
                condl.append(self.emit_call(self.ret_setter, ["1"]))
            else:
                condl.append(self.emit_call(self.ret_setter, ["0"]))
        else:
            self.emit_cond(cond, condl, vs, exp_stack)
        lines.append("label " + lc)
        lines.extend(condl)
        lines.append("jump -a " + lb)
        lines.append("jump " + le)
        lines.append("label " + lb)
        self._loop.append({"break": le, "continue": lc})
        lines.extend(self.code(body, vs, exp_stack))
        self._loop.pop()
        lines.append("jump " + lc)
        lines.append("label " + le)

    # ---- do {body} while(cond) 转译：先跑一次再判（continue→条件, break→出口） ----
    def emit_do(self, st, lines, vs, exp_stack):
        _, cond, body = st
        lbody = self.new_label()
        lcond = self.new_label()   # 条件（continue 落此：重判）
        lexit = self.new_label()   # 出口（break 落此）
        lines.append("label " + lbody)
        self._loop.append({"break": lexit, "continue": lcond})
        lines.extend(self.code(body, vs, exp_stack))
        self._loop.pop()
        lines.append("label " + lcond)
        if cond[0] == "bool":
            if cond[1]:
                lines.append(self.emit_call(self.ret_setter, ["1"]))
            else:
                lines.append(self.emit_call(self.ret_setter, ["0"]))
        else:
            condl = []
            self.emit_cond(cond, condl, vs, exp_stack)
            lines.extend(condl)
        lines.append("jump -a " + lbody)
        lines.append("label " + lexit)

    # ---- when（Kotlin 风格；主语可选；命中分支执行后自动结束，无 fallthrough） ----
    def emit_when(self, st, lines, vs, exp_stack):
        _, subj, arms = st
        wend = self.new_label()
        entries = []               # (body, 入口 label)
        pend = None                # else body
        # 探测序列：guard → 条件序列；match → 主语 与各匹配值比较（均跳各自臂入口）
        for kind, a, body in arms:
            if kind == "else":
                pend = body
                continue
            lbl = self.new_label()
            if kind == "guard":
                condl = []
                self.emit_cond(a, condl, vs, exp_stack)
                lines.extend(condl)
                lines.append("jump -a " + lbl)
            else:   # match：主语与每个匹配值比较，命中即跳该臂
                for m in a:
                    op, lt, rt = self._when_cmp(subj, m)
                    lines.append("%s %s %s" % (op, lt, rt))
                    lines.append("jump -a " + lbl)
            entries.append((body, lbl))
        # 兜底：有 else → 顺序执行 else body；无 else → 直接跳 when 尾
        if pend is not None:
            lines.extend(self.code(pend, vs, exp_stack))
        lines.append("jump " + wend)
        # 各命中臂体（执行后统一跳 when 尾，无 fallthrough）
        for body, lbl in entries:
            lines.append("label " + lbl)
            lines.extend(self.code(body, vs, exp_stack))
            lines.append("jump " + wend)
        lines.append("label " + wend)

    def _when_cmp(self, a, b):
        """when 主语 vs 匹配值：返回 (op, 左文本, 右文本)。字符串/flag→seq 文本；数值→ieq"""
        if self._when_is_str(a) or self._when_is_str(b):
            if not (self._when_is_str(a) and self._when_is_str(b)):
                raise S2CError("when 匹配：字符串/flag 与数值不能混用比较")
            return ("seq", self._when_text(a), self._when_text(b))
        return ("ieq", self._when_num(a), self._when_num(b))

    def _when_is_str(self, n):
        k = n[0]
        if k in ("str", "varref"):
            return True
        if k == "var":
            return self.vtypes.get(n[1]) in ("string", "flag")
        if k == "lit":
            v = n[1]
            return not (v.lstrip("-").isdigit() or v[:2].lower() in ("0x", "0b"))
        return False

    def _when_text(self, n):
        k = n[0]
        if k in ("var", "varref"):
            return "${" + n[1] + "}"
        if k in ("str", "lit"):
            return self.quote_lit(n[1])
        raise S2CError("when 文本比较不支持该节点")

    def _when_num(self, n):
        k = n[0]
        if k == "var":
            return n[1]
        if k == "lit":
            return n[1]
        if k == "bool":
            return "1" if n[1] else "0"
        raise S2CError("when 数值比较不支持该节点")

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
                nm, typ, val = st[1], st[2], st[3]
                out.append(("var", nm, typ, sub.get(val, val)))
            elif k == "assign":
                out.append(("assign", st[1], self.clone_expr(st[2], sub)))
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
            elif k == "do":
                cond, body = st[1], st[2]
                out.append(("do", self.clone_cond(cond, sub), self.clone_sub(body, sub)))
            elif k == "when":
                _, subj, arms = st
                oa = []
                for kind, a, body in arms:
                    if kind == "else":
                        oa.append(("else", None, self.clone_sub(body, sub)))
                    elif kind == "guard":
                        oa.append(("guard", self.clone_cond(a, sub),
                                   self.clone_sub(body, sub)))
                    else:
                        oa.append(("match",
                                   [Compiler.clone_when_node(m, sub) for m in a],
                                   self.clone_sub(body, sub)))
                out.append(("when", Compiler.clone_when_node(subj, sub), oa))
            elif k in ("break", "continue"):
                out.append(st)
            else:
                raise S2CError("fn 体内不支持该语句 %r" % (k,))
        return out

    @classmethod
    def clone_when_node(cls, node, sub):
        """when 主语/匹配值克隆（fn 实参替换：形参变量 → 实参字面量）"""
        if node is None:
            return None
        if node[0] == "var":
            nm = node[1]
            return ("lit", sub[nm]) if nm in sub else node
        return node

    @classmethod
    def clone_cond(cls, cond, sub):
        if cond is None:
            return None
        k = cond[0]
        if k == "bool":
            return cond
        if k == "call":
            return ("call", cond[1], [sub.get(a, a) for a in cond[2]])
        if k == "var":
            nm = cond[1]
            return ("lit", sub[nm]) if nm in sub else cond
        if k == "lit":
            return cond
        if k == "str":
            s = cond[1]
            return ("str", sub[s]) if s in sub else cond
        if k == "not":
            return ("not", cls.clone_cond(cond[1], sub))
        if k in ("and", "or"):
            return (k, cls.clone_cond(cond[1], sub), cls.clone_cond(cond[2], sub))
        if k == "cmp":
            _, op, L, R = cond
            return ("cmp", op, cls.clone_cond(L, sub), cls.clone_cond(R, sub))
        return cond

    @classmethod
    def clone_expr(cls, expr, sub):
        """赋值右侧表达式树克隆（fn 实参替换）"""
        k = expr[0]
        if k == "id":
            nm = expr[1]
            return ("id", sub[nm]) if nm in sub else expr
        if k in ("num", "str", "bool"):
            return expr
        if k == "bin":
            _, op, L, R = expr
            return ("bin", op, cls.clone_expr(L, sub), cls.clone_expr(R, sub))
        if k in ("neg", "notb"):
            return (k, cls.clone_expr(expr[1], sub))
        return expr


# ============================ 顶层接口 ============================

def translate(source, ret_setter="setret", max_len=512,
              var_max=4, name_max=8, value_max=15, const_fold=False):
    """源码 → (chain, warnings)。抛 S2CError。

    const_fold=True：const 折叠模式（供预编译 Flash 程序 scl_emit_c 使用）——
    顶层 const 只登记为编译期常量，不产 var 指令、不占变量槽；引用处折叠成字面量。
    """
    toks = tokenize(source)
    parser = Parser(toks)
    stmts = parser.parse_program()

    comp = Compiler(ret_setter=ret_setter, var_max=var_max,
                    name_max=name_max, value_max=value_max)
    comp.const_fold = const_fold
    for st in stmts:
        if st[0] == "fndef":
            if st[1] in comp.fns:
                raise S2CError("函数 %r 重复定义" % st[1])
            comp.fns[st[1]] = st
        elif const_fold and st[0] == "constvar":
            comp.declare_const(st[1], st[2], st[3])   # 预扫登记（不产 var const）
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
    ap.add_argument("--var-max", type=int, default=4, help="同时存活变量上限（默认 4）")
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
