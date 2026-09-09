#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scl_emit_c.py —— SCL 预编译程序生成器（把脚本"编译成 C"，减少 RAM）

输入现代语法脚本(.s2c)（或 --chain 的指令链文本），输出 C 源：
  - const 字节码数组（每条指令 4 字节：opc(2B 大端)+argOff(2B 大端)）
  - const 参数字节缓存（type 块序列）
  - const scl_prog_t 程序描述
把数组放只读存储（Flash）后，运行时用 SCL_RunProg() 直接解释，
不再占用 RAM 的字节码/参数缓存/label 表 —— 固定脚本最省 RAM 的用法。

编码规则与 scl/Src/scl.c 的 Scl_Compile / Scl_ArgStore* 完全一致：
  - opcode：HELP=1 VAR=2 FREE=3 JUMP=4 JUMPA=5；int/bool 运算 0x10..0x27
  - 注册业务命令统一编译为 CALLN(0x28) "按名调用"：
       参数区 = [total][STR 命令名][参数 type 块...]（无需匹配运行时命令注册顺序）
  - label 在编译期回填为绝对字节偏移（运行时不查 label 表）
  - ${name} 变量引用保留在 STR 块原文，运行时展开
  - const 折叠（v0.3b，仅 .s2c 输入）：顶层 const 声明编译期折叠成字面量，
    不产 var 指令、不占运行变量槽（SCL_CFG_VAR_MAX 不计）；值直接编进 Flash
    字节码/参数缓存 —— 即"const 值直接放 Flash"、无需运行时 RAM 变量。
  - type 块：BOOL=01+v | INT=02+4B 大端 | FLAG=03+c | STR=04+len+bytes
  - 保留字（大小写敏感，同 C）：label/jump/var/free/help + iadd..sneq 运算字

用法：
  python tools/scl_emit_c.py demo.s2c -o demo_prog.c [--name demo4]
  python tools/scl_emit_c.py --chain boot.chain -o boot_prog.c [--name boot]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scl_script2chain import translate, S2CError  # noqa: E402

# ---------- opcode（与 scl.c 枚举一致，改动需同步） ----------
T_BOOL = 0x01
T_INT = 0x02
T_FLAG = 0x03
T_STR = 0x04

OP_HELP, OP_VAR, OP_FREE, OP_JUMP, OP_JUMPA = 1, 2, 3, 4, 5
OP_CACHE = 0x2B
OP_CALLN = 0x28
OP_CALLF = 0x29   # callf <label>：运行时子程序调用（fn 非内联）
OP_RETF  = 0x2A   # retf：跳回全局 fn_back

# 内置运算保留字 → opcode（与 scl.c s_opwords[] 顺序/拼写一致，大小写敏感）
OPWORDS = [
    ("iadd", 0x10), ("isub", 0x11), ("imul", 0x12), ("idiv", 0x13), ("imod", 0x14), ("ineg", 0x15),
    ("ieq", 0x16), ("ine", 0x17), ("igt", 0x18), ("ige", 0x19), ("ilt", 0x1A), ("ile", 0x1B),
    ("band", 0x1C), ("bor", 0x1D), ("bnot", 0x1E), ("btest", 0x1F),
    ("iand", 0x20), ("ior", 0x21), ("ixor", 0x22), ("inot", 0x23), ("shl", 0x24), ("shr", 0x25),
    ("seq", 0x26), ("sneq", 0x27),
]
META_OPC = {"help": OP_HELP, "var": OP_VAR, "free": OP_FREE, "cache": OP_CACHE}


class EncError(Exception):
    """编码错误（带消息）。"""


# ---------- 基础工具 ----------
def _is_sp(c):
    return c in " \t\r\n"


def _is_ascii_alpha(c):
    return ("a" <= c <= "z") or ("A" <= c <= "Z")


def _is_ascii_digit(c):
    return "0" <= c <= "9"


def _parse_i32(s):
    """复刻 Scl_ParseI32Len：返回 int32 或 None。"""
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
            v -= 0x100000000  # 0xFFFFFFFF → -1（同 C 强转）
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
    """复刻 Scl_LitType：返回 (type, payload_bytes)。payload 不含 type 头。"""
    if len(tok) == 2 and tok[0] == "-" and _is_ascii_alpha(tok[1]):
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
    raw = tok.encode("utf-8")
    if len(raw) > 255:
        raise EncError("参数过长(>255B): %r" % tok)
    return (T_STR, raw)


def _blk_str(data):
    if len(data) > 255:
        raise EncError("字符串块过长(>255B)")
    return bytes([T_STR, len(data)]) + data


def _type_args(raw):
    """复刻 Scl_ArgStoreTyped：把参数原文切成字面量块（引号整段 STR；裸 token 类型化）。
    返回块 bytes 列表；无参返回 []。"""
    blocks = []
    i, n = 0, len(raw)
    while True:
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
                raise EncError("引号未闭合")
            blocks.append(_blk_str(raw[i:j].encode("utf-8")))
            i = j + 1
        else:
            j = i
            while j < n and not _is_sp(raw[j]) and raw[j] not in ('"', "'"):
                j += 1
            ty, payload = _lit_payload(raw[i:j])
            if ty == T_BOOL or ty == T_FLAG:
                blocks.append(bytes([ty]) + payload)
            elif ty == T_INT:
                blocks.append(bytes([ty]) + payload)
            else:
                blocks.append(_blk_str(payload))
            i = j
    return blocks


# ---------- 指令链切分（复刻 Scl_Compile 子句扫描） ----------
def _clauses(chain):
    """把链文本切成子句原文列表（保留内部空白；引号内 ';' 不分割）。"""
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
    """返回 (head, head_end_index, 参数原文)。去两端空白。head '#' 注释返回 (None,None,None)。"""
    n = len(clause)
    hs = 0
    he = 0
    while he < n and not _is_sp(clause[he]):
        he += 1
    if hs < he and clause[hs] == "#":
        return (None, None, None)
    rs = he
    while rs < n and _is_sp(clause[rs]):
        rs += 1
    re = n
    while re > rs and _is_sp(clause[re - 1]):
        re -= 1
    return (clause[hs:he], rs, clause[rs:re])


# ---------- 主编码 ----------
def encode_chain(chain):
    """指令链文本 → (bc:list[int], argc:list[int])。抛 EncError。
    bc 每条指令 4 字节：opc BE + aoff BE；argc 为参数字节缓存（第 0 字节哨兵）。"""
    cache = [0]                       # 第 0 字节哨兵（argOff==0 表示无参）
    bc = []
    insts = []                        # 每项：{opc, raw, kind, ...}
    labels = {}                       # label 名 → 指令偏移(icnt*4)

    # ---- 第一遍：收集指令与 label ----
    for cl in _clauses(chain):
        head, _rs, raw = _split_head(cl)
        if head is None:
            continue                  # '#' 整句注释
        if len(insts) >= 0x4000:
            raise EncError("指令过多")
        if head == "label":
            nm = raw
            if not nm:
                raise EncError("label: 名不合法")
            if nm in labels:
                raise EncError("label 重名: %s" % nm)
            labels[nm] = len(insts) * 4
            continue                  # label 不产指令
        if head == "jump":
            # jump [-a|-b] <名>
            parts = raw.split(None, 1)
            cond = 0
            if parts and parts[0].startswith("-") and len(parts[0]) == 2 and parts[0][1] in "ab":
                cond = 1 if parts[0][1] == "a" else 0
                tgt = parts[1].strip() if len(parts) > 1 else ""
            else:
                tgt = raw
            if not tgt:
                raise EncError("jump: 缺少目标 label")
            insts.append({"opc": OP_JUMPA if cond else OP_JUMP, "kind": "jump", "tgt": tgt})
            continue
        if head == "callf":
            # callf <名>：目标必须已/将登记 label
            tgt = raw.strip()
            if not tgt:
                raise EncError("callf: 缺少目标 label")
            insts.append({"opc": OP_CALLF, "kind": "callf", "tgt": tgt})
            continue
        if head == "retf":
            insts.append({"opc": OP_RETF, "kind": "retf"})
            continue
        opw = None
        for (w, o) in OPWORDS:
            if head == w:
                opw = o
                break
        if opw is not None:
            insts.append({"opc": opw, "kind": "typed", "raw": raw})
            continue
        meta = META_OPC.get(head)
        if meta is not None:
            insts.append({"opc": meta, "kind": "meta", "raw": raw})
            continue
        # 其余视为注册业务命令 → CALLN（按名调用）
        if not (0 < len(head) < 256):
            raise EncError("命令名不合法: %r" % head)
        insts.append({"opc": OP_CALLN, "kind": "call", "name": head, "raw": raw})

    if not insts:
        raise EncError("无指令")

    def area_open():
        off = len(cache)
        cache.append(None)            # total 占位
        return off

    def area_close(off):
        total = len(cache) - off - 1
        if total > 254:
            raise EncError("参数区超长(>254B)")
        cache[off] = total

    # ---- 第二遍：生成参数缓存与字节码 ----
    for it in insts:
        if it["kind"] == "meta":
            raw = it["raw"]
            if raw == "":
                aoff = 0
            else:
                off = area_open()
                cache.extend(_blk_str(raw.encode("utf-8")))
                area_close(off)
                aoff = off
        elif it["kind"] == "typed":
            blocks = _type_args(it["raw"])
            if not blocks:
                aoff = 0
            else:
                off = area_open()
                for b in blocks:
                    cache.extend(b)
                area_close(off)
                aoff = off
        elif it["kind"] == "call":
            off = area_open()
            cache.extend(_blk_str(it["name"].encode("utf-8")))  # STR 命令名（首块）
            for b in _type_args(it["raw"]):
                cache.extend(b)
            area_close(off)
            aoff = off
        elif it["kind"] == "callf":
            tgt = labels.get(it["tgt"])
            if tgt is None:
                raise EncError("callf: label '%s' 未定义" % it["tgt"])
            aoff = tgt
        elif it["kind"] == "retf":
            aoff = 0
        else:                          # jump
            tgt = labels.get(it["tgt"])
            if tgt is None:
                raise EncError("jump: label '%s' 未定义" % it["tgt"])
            aoff = tgt
        bc.append((it["opc"] >> 8) & 0xFF)
        bc.append(it["opc"] & 0xFF)
        bc.append((aoff >> 8) & 0xFF)
        bc.append(aoff & 0xFF)

    if len(bc) > 0xFFFF:
        raise EncError("字节码超长")
    return bc, cache


# ---------- C 源生成 ----------
def emit_c(name, bc, argc, source_note, cmd=None):
    """生成 C 源文本（数组 + 程序描述）。
    cmd 非 None：额外生成脚本命令节点与注册函数（Scl_Scmd_Register_<name>）。"""
    L = []
    L.append("/* ===============================================================")
    L.append(" * SCL 预编译只读程序：%s（由 tools/scl_emit_c.py 生成，勿手改）" % name)
    L.append(" * 来源：%s" % source_note)
    L.append(" * 用法：SCL_Init(); <注册所需命令>; SCL_RunProg(&scl_%s_prog);" % name)
    L.append(" *       然后周期调用 SCL_Loop() 直到 SCL_Idle()。")
    L.append(" * 数据应放只读存储（Flash），运行期不占用字节码/参数缓存 RAM。")
    L.append(" * ===============================================================")
    L.append(" */")
    L.append('#include "scl.h"')
    L.append("")
    L.append("/* 字节码：每条指令 4 字节 = opc(2B 大端) + argOff(2B 大端) */")
    L.append("static const uint8_t scl_%s_bc[] = {" % name)
    for i in range(0, len(bc), 12):
        row = ", ".join("0x%02X" % b for b in bc[i:i + 12])
        L.append("    %s," % row)
    L.append("};")
    L.append("")
    L.append("/* 参数字节缓存：第 0 字节哨兵；argOff 指向参数区 total 字节 */")
    L.append("static const uint8_t scl_%s_argc[] = {" % name)
    for i in range(0, len(argc), 12):
        row = ", ".join("0x%02X" % b for b in argc[i:i + 12])
        L.append("    %s," % row)
    L.append("};")
    L.append("")
    L.append("/* 程序描述（const，放 Flash） */")
    L.append("const scl_prog_t scl_%s_prog = {" % name)
    L.append("    scl_%s_bc,  (uint16_t)sizeof(scl_%s_bc)," % (name, name))
    L.append("    scl_%s_argc, (uint16_t)sizeof(scl_%s_argc)" % (name, name))
    L.append("};")
    if cmd:
        L.append("")
        L.append("/* ============ 脚本命令：注册为命令行命令 '%s'（SCL_Scmd） ============ */" % cmd)
        L.append("/* 用法：SCL_Init(); <注册业务命令>; Scl_Scmd_Register_%s();" % name)
        L.append(" *      之后命令行输入：%s 参数... （注入 arg0..argN，执行本脚本；见 scl.h）" % cmd)
        L.append(" *      程序尾部已含 free：执行完自动释放本命令引入的变量（含 arg*） */")
        L.append("static scl_scmd_t s_scmd_%s = { \"%s\", &scl_%s_prog, NULL };" % (name, cmd, name))
        L.append("void Scl_Scmd_Register_%s(void)" % name)
        L.append("{")
        L.append("    SCL_Scmd_Register(&s_scmd_%s);" % name)
        L.append("}")
    L.append("")
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="SCL 预编译程序生成器：脚本/指令链 → C 源（const 字节码，省 RAM）")
    ap.add_argument("input", help="输入 .s2c（现代语法）或指令链文本（--chain）")
    ap.add_argument("-o", "--output", default=None, help="输出 C 文件（缺省打印 stdout）")
    ap.add_argument("--name", default=None, help="生成标识名（缺省取输入文件名去扩展）")
    ap.add_argument("--chain", action="store_true",
                    help="输入已是指令链文本（跳过现代语法解析）")
    ap.add_argument("--cmd", default=None,
                    help="生成脚本命令注册（命令名，如 focus）：尾部加 free、输出 scmd 节点与 "
                         "Scl_Scmd_Register_<name>()；命令行输入 '<cmd> 参数...' 直接执行本脚本")
    ap.add_argument("--mini", action="store_true",
                    help="mini-scl：不生成字节码+解释器，改为生成自包含 switch 状态机 C "
                         "(<name>_mini_start/step)，运行时不再需要文本编译/字节码解释/label 表")
    # 现代语法解析参数（对齐 scl_script2chain.translate）
    ap.add_argument("--ret-setter", default="setret")
    ap.add_argument("--var-max", type=int, default=4)
    ap.add_argument("--name-max", type=int, default=8)
    ap.add_argument("--value-max", type=int, default=15)
    args = ap.parse_args(argv)

    try:
        with open(args.input, "r", encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        print("emit-c: 无法读取 %s: %s" % (args.input, e), file=sys.stderr)
        return 2

    if args.chain:
        chain = src
        note = "%s (指令链文本)" % args.input
    else:
        try:
            # const_fold=True：顶层 const 折叠为编译期字面量（不产 var const 指令、
            # 不占 RAM 变量槽；引用处直接编进 Flash 字节码/参数缓存）
            chain, warns = translate(src, ret_setter=args.ret_setter,
                                     var_max=args.var_max, name_max=args.name_max,
                                     value_max=args.value_max, const_fold=True)
        except S2CError as e:
            loc = ("%s:%d:%d: " % (args.input, e.line, e.col)) if e.line else ""
            print("emit-c: %serror: %s" % (loc, e.msg), file=sys.stderr)
            return 1
        for w in warns:
            print("emit-c: 警告: " + w, file=sys.stderr)
        note = "%s (现代语法 .s2c)" % args.input

    if args.name:
        name = args.name
    else:
        base = os.path.basename(args.input)
        name = "".join(ch for ch in base.split(".")[0] if ch.isalnum() or ch == "_") or "prog"

    if args.mini:
        if args.cmd is not None:
            print("emit-c: error: --mini 与 --cmd 互斥（mini 为自包含程序，不注册为命令）",
                  file=sys.stderr)
            return 1
        try:
            import scl_mini_c
        except Exception as e:                      # noqa: BLE001
            print("emit-c: error: 无法加载 scl_mini_c 模块: %s" % e, file=sys.stderr)
            return 1
        try:
            text = scl_mini_c.emit_mini_c(name, chain, note)
        except scl_mini_c.MiniError as e:
            print("emit-c: error: %s" % e, file=sys.stderr)
            return 1
    else:
        try:
            bc, argc = encode_chain(chain)
        except EncError as e:
            print("emit-c: error: %s" % e, file=sys.stderr)
            return 1
        cmd = args.cmd
        if cmd is not None:
            # 脚本命令：字节码末尾追加一条无参 free（执行完自动释放 arg* 与本命令变量）
            bc = bc + [0x00, META_OPC["free"] & 0xFF, 0x00, 0x00]
        text = emit_c(name, bc, argc, note, cmd=cmd)

    if args.output:
        try:
            with open(args.output, "w", encoding="utf-8") as f:
                f.write(text)
        except OSError as e:
            print("emit-c: 无法写入 %s: %s" % (args.output, e), file=sys.stderr)
            return 2
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
