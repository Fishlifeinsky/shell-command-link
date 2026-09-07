/**
  ******************************************************************************
  * @file    scl.c
  * @brief   SCL（Shell-Command-Link）简易指令链脚本库 —— 实现（字节码执行模型 v4）
  *
  *          总体模型（用户 2026-09-07 确认）：
  *            - 运行时"指令链文本"为汇编式：
  *                cmd a b ...     普通式业务命令
  *                var/free/help   内置命令
  *                label <名>      设置跳转点（不产字节）
  *                jump [-a] <名>  -b/默认=无条件跳；-a=G_RETURN 为真才跳(读后清零)
  *              运行时不再提供 if/while 文本（由上层编译器降级为 label/jump）
  *            - SCL_Run() 把文本**编译成字节码**后立即返回并置 busy：
  *                每条指令固定 4 字节 = opc(2B,大端) + argOff(2B,大端)
  *                参数写入"参数字节缓存"：[len(1)][参数原文]；无参 argOff=0
  *                label → 登记到 label 表 (名 → 下一条指令字节偏移)
  *                jump  → 编译期把名解析为目标偏移写入 argOff 槽
  *            - 命令在 SCL_RegisterCmd() 时自动分配 opcode（自 0x0100 起）
  *            - 主循环周期调 SCL_Loop() 逐条解释执行字节码；业务命令分同步/异步：
  *                异步命令 handler 立即返回，库在指令边界轮询其 sync 回调完成后再
  *                执行下一条
  *            - 全程静态内存、无 malloc、无 OS/HAL/libc 依赖
  *
  *          编译期错误（label 重名/未定义、未知命令、超限）→ 拒绝该链并保持空闲。
  *          执行期错误/abort → 收尾（自动 free 全部变量、清 G_RETURN）。
  ******************************************************************************
  */

/* 本模块头文件 */
#include "scl.h"

/* 变参消息 */
#include <stdarg.h>

/* ========================== 移植输出 ========================== */

#if (SCL_CFG_MSG_EN == 1u)
extern void SCL_Port_PutChar(char c);
#endif

/* ========================== 编译期校验 ========================== */

#if ((SCL_CFG_VAR_NAME_MAX) < 1u)
#error "SCL_CFG_VAR_NAME_MAX must >= 1"
#endif
#if ((SCL_CFG_VAR_VALUE_MAX) < 2u)
#error "SCL_CFG_VAR_VALUE_MAX must >= 2 (1 char + '\\0')"
#endif
#if ((SCL_CFG_ARG_LEN_MAX) < 2u)
#error "SCL_CFG_ARG_LEN_MAX must >= 2"
#endif
#if ((SCL_CFG_BC_MAX) < 8u) || ((SCL_CFG_BC_MAX) % 4u != 0u)
#error "SCL_CFG_BC_MAX must >= 8 and multiple of 4"
#endif

/* ========================== opcode 常量 ========================== */

enum
{
    SCL_OP_NONE    = 0x0000u,  /* 保留 */
    SCL_OP_HELP    = 0x0001u,  /* help（示例：\x00\x01） */
    SCL_OP_VAR     = 0x0002u,  /* var */
    SCL_OP_FREE    = 0x0003u,  /* free */
    SCL_OP_JUMP    = 0x0004u,  /* jump 无条件 */
    SCL_OP_JUMPA   = 0x0005u,  /* jump -a 有条件 */
    SCL_OP_CMD_BASE = 0x0100u  /* 注册命令 opcode 起点（自动递增） */
};

/* 编译中间伪指令 */
enum
{
    SCL_P_LABEL  = 0xFF00u,   /* label */
    SCL_P_JUMP   = 0xFF01u,   /* jump 无条件 */
    SCL_P_JUMPA  = 0xFF02u    /* jump -a */
};

/* ========================== 静态状态 ========================== */

static uint8_t s_inited = 0u;      /* 首次自动初始化标记 */

/* ---- 命令链表（注册即自动分配 opcode） ---- */
static scl_cmd_t *s_cmd_head = NULL;
static uint16_t  s_next_opc  = (uint16_t)SCL_OP_CMD_BASE;

/* ---- 变量表 ---- */
typedef struct
{
    char     name[SCL_CFG_VAR_NAME_MAX + 1u];
    char     value[SCL_CFG_VAR_VALUE_MAX];
    uint8_t  used;
} scl_var_t;

static scl_var_t s_vars[SCL_CFG_VAR_MAX];

/* ---- 字节码程序（编译产物，跨 tick 持久） ---- */
static uint8_t  s_bc[SCL_CFG_BC_MAX];       /* 每条指令 4 字节 */
static uint16_t s_bc_len = 0u;              /* 有效字节数（4 的倍数） */
static uint16_t s_pc     = 0u;              /* 程序计数器（解释执行） */

/* ---- 参数字节缓存：[len][参数原文...]，argOff 指向该 len 字节 ---- */
static uint8_t  s_argc[SCL_CFG_ARG_CACHE_MAX];
static uint16_t s_arg_len = 0u;

/* ---- label 表：label 名 → 下一条指令字节偏移 ---- */
typedef struct
{
    char     name[SCL_CFG_LABEL_NAME_MAX + 1u];
    uint16_t off;
} scl_label_t;
static scl_label_t s_labels[SCL_CFG_LABEL_MAX];
static uint8_t     s_label_cnt = 0u;

/* ---- 条件标志 G_RETURN ---- */
static uint8_t s_ret = 0u;

/* ---- 执行状态 ---- */
static uint8_t  s_busy = 0u;
static volatile uint8_t s_abort = 0u;
static scl_cmd_t *s_wait_cmd = NULL;         /* 正在异步等待的命令 */

/* ---- 参数工作缓冲 ---- */
#define SCL_RAW_MAX 256u                     /* 单条指令参数原文上限（含 '\0'） */
static char  s_raw[SCL_RAW_MAX];             /* 从缓存取出的参数原文 */
static char  s_argb[SCL_CFG_ARG_MAX][SCL_CFG_ARG_LEN_MAX];
static char *s_argv[SCL_CFG_ARG_MAX];

/* ========================== 文本小工具（不依赖 libc） ========================== */

static uint16_t Scl_StrLen(const char *s)
{
    uint16_t n = 0u;
    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

static uint8_t Scl_IsSp(char c)
{
    return ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n')) ? 1u : 0u;
}

static uint8_t Scl_IsNm(char c)
{
    if ((c >= 'a') && (c <= 'z')) { return 1u; }
    if ((c >= 'A') && (c <= 'Z')) { return 1u; }
    if ((c >= '0') && (c <= '9')) { return 1u; }
    if (c == '_') { return 1u; }
    return 0u;
}

static uint8_t Scl_EqN(const char *a, const char *b, uint16_t n)
{
    uint16_t i;
    for (i = 0u; i < n; i++)
    {
        if (a[i] != b[i])
        {
            return 0u;
        }
    }
    return 1u;
}

static uint8_t Scl_StrEq(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0'))
    {
        if (*a != *b)
        {
            return 0u;
        }
        a++;
        b++;
    }
    return ((*a == '\0') && (*b == '\0')) ? 1u : 0u;
}

/* 十进制/十六进制输出与消息（含内部小格式化 %s %c %d %u %x） */
static void Scl_PutU32(uint32_t v)
{
    char tmp[10u];
    int  i = 0;
    if (v == 0u)
    {
#if (SCL_CFG_MSG_EN == 1u)
        SCL_Port_PutChar('0');
#endif
        return;
    }
    while (v > 0u)
    {
        tmp[i] = (char)('0' + (v % 10u));
        v /= 10u;
        i++;
    }
    while (i > 0)
    {
        i--;
#if (SCL_CFG_MSG_EN == 1u)
        SCL_Port_PutChar(tmp[i]);
#endif
    }
}

#if (SCL_CFG_MSG_EN == 1u)
static void Scl_VMsg(const char *fmt, va_list ap)
{
    while (*fmt != '\0')
    {
        if (*fmt == '%')
        {
            fmt++;
            if (*fmt == 's')
            {
                const char *s = va_arg(ap, const char *);
                if (s == NULL) { s = "(null)"; }
                while (*s != '\0') { SCL_Port_PutChar(*s); s++; }
            }
            else if (*fmt == 'c')
            {
                SCL_Port_PutChar((char)va_arg(ap, int));
            }
            else if (*fmt == 'd')
            {
                int v = va_arg(ap, int);
                if (v < 0)
                {
                    SCL_Port_PutChar('-');
                    Scl_PutU32((uint32_t)(-(int32_t)v));
                }
                else
                {
                    Scl_PutU32((uint32_t)v);
                }
            }
            else if (*fmt == 'u')
            {
                Scl_PutU32((uint32_t)va_arg(ap, unsigned int));
            }
            else if (*fmt == 'x')
            {
                uint32_t v = (uint32_t)va_arg(ap, unsigned int);
                int shift;
                uint8_t started = 0u;
                for (shift = 28; shift >= 0; shift -= 4)
                {
                    uint8_t d = (uint8_t)((v >> shift) & 0x0Fu);
                    if ((d != 0u) || started || (shift == 0))
                    {
                        started = 1u;
                        SCL_Port_PutChar((d < 10u) ? (char)('0' + d) : (char)('A' + (d - 10u)));
                    }
                }
            }
            else if (*fmt == '%')
            {
                SCL_Port_PutChar('%');
            }
            fmt++;
        }
        else
        {
            SCL_Port_PutChar(*fmt);
            fmt++;
        }
    }
}
#endif /* SCL_CFG_MSG_EN */

static void Scl_Msg(const char *fmt, ...)
{
#if (SCL_CFG_MSG_EN == 1u)
    va_list ap;
    va_start(ap, fmt);
    Scl_VMsg(fmt, ap);
    va_end(ap);
#else
    (void)fmt;
#endif
}

static void Scl_MsgErr(const char *fmt, ...)
{
#if (SCL_CFG_MSG_EN == 1u)
    va_list ap;
    Scl_Msg("scl: ");
    va_start(ap, fmt);
    Scl_VMsg(fmt, ap);
    va_end(ap);
    Scl_Msg("\r\n");
#else
    (void)fmt;
#endif
}

/* ========================== 变量实现 ========================== */

static int Scl_VarFind(const char *name)
{
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if ((s_vars[i].used != 0u) && Scl_StrEq(s_vars[i].name, name))
        {
            return i;
        }
    }
    return -1;
}

static int Scl_VarNameOk(const char *name)
{
    uint16_t n = 0u;
    if ((name[0] == '\0') || (!((name[0] == '_') ||
        ((name[0] >= 'a') && (name[0] <= 'z')) ||
        ((name[0] >= 'A') && (name[0] <= 'Z')))))
    {
        return 0;
    }
    while (name[n] != '\0')
    {
        if (!Scl_IsNm(name[n]))
        {
            return 0;
        }
        n++;
    }
    return (n <= SCL_CFG_VAR_NAME_MAX) ? 1 : 0;
}

int SCL_VarSet(const char *name, const char *val)
{
    int      idx;
    uint16_t vlen;

    if ((name == NULL) || (name[0] == '\0'))
    {
        return -4;
    }
    if (!Scl_VarNameOk(name))
    {
        return -2;
    }
    if (val == NULL)
    {
        val = "";
    }
    vlen = Scl_StrLen(val);
    if (vlen >= SCL_CFG_VAR_VALUE_MAX)
    {
        return -3;
    }
    idx = Scl_VarFind(name);
    if (idx < 0)
    {
        for (idx = 0; idx < (int)SCL_CFG_VAR_MAX; idx++)
        {
            if (s_vars[idx].used == 0u)
            {
                break;
            }
        }
        if (idx >= (int)SCL_CFG_VAR_MAX)
        {
            return -1;
        }
        s_vars[idx].used = 1u;
        {
            uint16_t i;
            for (i = 0u; name[i] != '\0'; i++)
            {
                s_vars[idx].name[i] = name[i];
            }
            s_vars[idx].name[i] = '\0';
        }
    }
    {
        uint16_t i;
        for (i = 0u; i < vlen; i++)
        {
            s_vars[idx].value[i] = val[i];
        }
        s_vars[idx].value[vlen] = '\0';
    }
    return 0;
}

const char *SCL_VarGet(const char *name)
{
    int idx = Scl_VarFind(name);
    return (idx < 0) ? NULL : s_vars[idx].value;
}

int SCL_VarFree(const char *name)
{
    int idx = Scl_VarFind(name);
    if (idx < 0)
    {
        return -1;
    }
    s_vars[idx].used = 0u;
    s_vars[idx].name[0] = '\0';
    s_vars[idx].value[0] = '\0';
    return 0;
}

int SCL_VarFreeAll(void)
{
    int n = 0;
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u)
        {
            s_vars[i].used = 0u;
            s_vars[i].name[0] = '\0';
            s_vars[i].value[0] = '\0';
            n++;
        }
    }
    return n;
}

int SCL_VarCount(void)
{
    int n = 0;
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u)
        {
            n++;
        }
    }
    return n;
}

int SCL_VarFreeCount(void)
{
    return (int)SCL_CFG_VAR_MAX - SCL_VarCount();
}

/* ========================== 条件标志 G_RETURN ========================== */

void SCL_Ret_Set(int v)
{
    s_ret = (v != 0) ? 1u : 0u;
}

int SCL_Ret_Get(void)
{
    return (s_ret != 0u) ? 1 : 0;
}

static int Scl_RetTake(void)
{
    int r = (s_ret != 0u) ? 1 : 0;
    s_ret = 0u;
    return r;
}

/* ========================== 命令注册（自动分配 opcode） ========================== */

void SCL_RegisterCmd(scl_cmd_t *cmd)
{
    scl_cmd_t **pp;

    if (cmd == NULL)
    {
        return;
    }
    cmd->opc  = s_next_opc;              /* 自动分配字节码 opcode */
    s_next_opc = (uint16_t)(s_next_opc + 1u);
    cmd->next = NULL;
    pp = &s_cmd_head;
    while (*pp != NULL)
    {
        pp = &((*pp)->next);
    }
    *pp = cmd;
}

static scl_cmd_t *Scl_CmdFindName(const char *name, uint16_t len)
{
    scl_cmd_t *node;
    for (node = s_cmd_head; node != NULL; node = node->next)
    {
        uint16_t nl = Scl_StrLen(node->name);
        if ((nl == len) && Scl_EqN(node->name, name, len))
        {
            return node;
        }
    }
    return NULL;
}

static scl_cmd_t *Scl_CmdFindOp(uint16_t opc)
{
    scl_cmd_t *node;
    for (node = s_cmd_head; node != NULL; node = node->next)
    {
        if (node->opc == opc)
        {
            return node;
        }
    }
    return NULL;
}

/* ========================== ${} 展开拷贝 ========================== */

static int Scl_ExpandCopy(const char *src, const char *end,
                          char *dst, uint16_t cap)
{
    uint16_t di = 0u;
    const char *p = src;

    while (p < end)
    {
        char c = *p;
        if ((c == '$') && (p + 1 < end) && (p[1] == '{'))
        {
            const char *ns = p + 2u;
            const char *q2 = ns;
            while ((q2 < end) && Scl_IsNm(*q2))
            {
                q2++;
            }
            if ((q2 == ns) || (q2 >= end) || (*q2 != '}'))
            {
                Scl_MsgErr("${}: 语法错误");
                if (di + 1u >= cap) { return -2; }
                dst[di++] = '$';
                p++;
                continue;
            }
            {
                char nb[SCL_CFG_VAR_NAME_MAX + 1u];
                uint16_t nn = (uint16_t)(q2 - ns);
                uint16_t i;
                if (nn > SCL_CFG_VAR_NAME_MAX)
                {
                    Scl_MsgErr("${}: 变量名过长");
                    return -1;
                }
                for (i = 0u; i < nn; i++) { nb[i] = ns[i]; }
                nb[nn] = '\0';
                {
                    const char *val = SCL_VarGet(nb);
                    if (val == NULL)
                    {
                        Scl_MsgErr("${%s}: 变量未定义", nb);
                    }
                    else
                    {
                        while ((*val != '\0') && (di + 1u < cap))
                        {
                            dst[di++] = *val;
                            val++;
                        }
                    }
                }
            }
            p = q2 + 1u;
            continue;
        }
        if (di + 1u >= cap)
        {
            return -2;
        }
        dst[di++] = c;
        p++;
    }
    if (di >= cap)
    {
        return -2;
    }
    dst[di] = '\0';
    return 0;
}

/* ========================== 参数原文 → argv（引号 + 展开） ========================== */

static int Scl_TokenizeRaw(const char *s)
{
    int ai = 0;
    const char *p = s;

    while (1)
    {
        const char *ab;
        const char *ae;
        char q = 0;

        while (*p != '\0')
        {
            if (!Scl_IsSp(*p)) { break; }
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        if ((*p == '"') || (*p == '\''))
        {
            q = *p;
            p++;
            ab = p;
            while ((*p != '\0') && (*p != q))
            {
                p++;
            }
            if (*p == '\0')
            {
                return -4;   /* 引号未闭合 */
            }
            ae = p;
            p++;   /* 跳过闭引号 */
        }
        else
        {
            ab = p;
            while ((*p != '\0') && !Scl_IsSp(*p))
            {
                /* 引号字符在裸 token 内也按普通字符处理 */
                if ((*p == '"') || (*p == '\''))
                {
                    break;
                }
                p++;
            }
            ae = p;
        }
        if (ai >= (int)SCL_CFG_ARG_MAX)
        {
            return -3;   /* 参数过多 */
        }
        {
            int r = Scl_ExpandCopy(ab, ae, s_argb[ai], SCL_CFG_ARG_LEN_MAX);
            if (r < 0)
            {
                return r;
            }
        }
        s_argv[ai] = s_argb[ai];
        ai++;
    }
    return ai;
}

/* ========================== 编译：文本 → 字节码 ========================== */

/* 从缓存取参数原文到 s_raw（含 '\0'）；aoff==0 表示无参数返回 NULL */
static const char *Scl_ArgLoad(uint16_t aoff)
{
    uint16_t len;
    uint16_t i;

    if (aoff == 0u)
    {
        return NULL;
    }
    if (aoff >= s_arg_len)
    {
        return NULL;
    }
    len = s_argc[aoff];
    if ((uint16_t)(aoff + 1u + len) > s_arg_len)
    {
        return NULL;
    }
    for (i = 0u; i < len; i++)
    {
        s_raw[i] = (char)s_argc[aoff + 1u + i];
    }
    s_raw[len] = '\0';
    return s_raw;
}

/* 把一个参数原文加入参数缓存；返回偏移（0 表示无参数）。
   注意：偏移 0 被保留作"无参"哨兵，故首个记录从偏移 1 开始 */
static uint16_t Scl_ArgStore(const char *raw, uint16_t rawlen)
{
    uint16_t off;
    uint16_t i;

    if (rawlen == 0u)
    {
        return 0u;
    }
    if (rawlen > 255u)
    {
        return 0xFFFFu;   /* 超长标记 */
    }
    off = s_arg_len;
    if (off == 0u)
    {
        s_arg_len = 1u;   /* 保留第 0 字节作哨兵 */
        off = 1u;
    }
    if ((uint16_t)(s_arg_len + 1u + rawlen) > SCL_CFG_ARG_CACHE_MAX)
    {
        return 0xFFFFu;
    }
    s_argc[off] = (uint8_t)rawlen;
    for (i = 0u; i < rawlen; i++)
    {
        s_argc[off + 1u + i] = (uint8_t)raw[i];
    }
    s_arg_len = (uint16_t)(s_arg_len + 1u + rawlen);
    return off;
}

/* 编译中间指令记录（先收集再回填，静态数组） */
#define SCL_BC_INSTR_MAX (SCL_CFG_BC_MAX / 4u)

typedef struct
{
    uint16_t opc;           /* 真 opcode 或 SCL_P_* 伪指令 */
    uint16_t cond;          /* jump: 0=无条件 -b, 1=-a */
    const char *raw;        /* 参数原文指针（指向待编译文本内，编译期有效） */
    uint16_t rawlen;
} scl_ins_t;

static scl_ins_t s_ins[SCL_BC_INSTR_MAX];

/* 编译文本为字节码（s_bc/s_argc/s_labels）。成功返回 1 */
static uint8_t Scl_Compile(const char *script)
{
    const char *p = script;
    uint16_t icnt = 0u;      /* 指令数（label 不计） */
    uint16_t i;
    uint8_t  ok = 1u;

    s_label_cnt = 0u;
    s_arg_len   = 0u;

    /* ---- 第一遍：扫描子句，收集指令与 label ---- */
    while (ok && (*p != '\0'))
    {
        const char *cs;
        const char *ce;
        char q = 0;

        /* 跳过分隔 ';' 与空白 */
        while ((*p == ';') || Scl_IsSp(*p))
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        cs = p;
        /* 扫描到顶层 ';'（引号内 ';' 不分割） */
        while (*p != '\0')
        {
            if ((*p == ';') && (q == 0))
            {
                break;
            }
            if ((*p == '"') || (*p == '\''))
            {
                if (q == 0)
                {
                    q = *p;
                }
                else if (q == *p)
                {
                    q = 0;
                }
            }
            p++;
        }
        ce = p;
        if (*p == ';')
        {
            p++;
        }

        /* 提取子句首 token（命令名/关键字） */
        {
            const char *hs = cs;
            const char *he = cs;
            while ((he < ce) && !Scl_IsSp(*he))
            {
                he++;
            }
            /* 参数原文 = head 之后，去两端空白 */
            {
                const char *rs = he;
                const char *re = ce;
                while ((rs < re) && Scl_IsSp(*rs)) { rs++; }
                while ((re > rs) && Scl_IsSp(*(re - 1))) { re--; }

                if (icnt >= SCL_BC_INSTR_MAX)
                {
                    Scl_MsgErr("指令过多(>%d)", (int)SCL_BC_INSTR_MAX);
                    ok = 0;
                    break;
                }
                if (Scl_EqN(hs, "label", 5u) && ((uint16_t)(he - hs) == 5u))
                {
                    /* label <名>：登记名 → 当前指令计数*4 */
                    const char *nb = rs;
                    uint16_t nlen = (uint16_t)(re - rs);
                    uint8_t dup = 0u;
                    if ((nlen == 0u) || (nlen > SCL_CFG_LABEL_NAME_MAX))
                    {
                        Scl_MsgErr("label: 名不合法");
                        ok = 0;
                        break;
                    }
                    if (s_label_cnt >= (uint8_t)SCL_CFG_LABEL_MAX)
                    {
                        Scl_MsgErr("label 过多(>%d)", (int)SCL_CFG_LABEL_MAX);
                        ok = 0;
                        break;
                    }
                    for (i = 0u; i < s_label_cnt; i++)
                    {
                        if ((Scl_StrLen(s_labels[i].name) == nlen) &&
                            Scl_EqN(s_labels[i].name, nb, nlen))
                        {
                            dup = 1u;
                            break;
                        }
                    }
                    if (dup != 0u)
                    {
                        Scl_MsgErr("label 重名");
                        ok = 0;
                        break;
                    }
                    s_labels[s_label_cnt].off = (uint16_t)(icnt * 4u);
                    {
                        uint16_t j;
                        for (j = 0u; j < nlen; j++)
                        {
                            s_labels[s_label_cnt].name[j] = nb[j];
                        }
                        s_labels[s_label_cnt].name[nlen] = '\0';
                    }
                    s_label_cnt++;
                    continue;   /* label 不占指令 */
                }
                else if (Scl_EqN(hs, "jump", 4u) && ((uint16_t)(he - hs) == 4u))
                {
                    /* jump [-a|-b] <名> */
                    const char *w = rs;
                    const char *wn;
                    uint8_t cond = 0u;
                    const char *tgt = rs;
                    uint16_t tlen;

                    while ((w < re) && !Scl_IsSp(*w)) { w++; }   /* 第一个 token 末尾 */
                    wn = w;
                    while ((wn < re) && Scl_IsSp(*wn)) { wn++; } /* 第二个 token 起点 */
                    if (((uint16_t)(w - rs) == 2u) && (rs[0] == '-'))
                    {
                        if (rs[1] == 'a')
                        {
                            cond = 1u;
                        }
                        else if (rs[1] == 'b')
                        {
                            cond = 0u;
                        }
                        else
                        {
                            Scl_MsgErr("jump: 未知模式");
                            ok = 0;
                            break;
                        }
                        tgt  = wn;
                        tlen = (uint16_t)(re - wn);   /* re 已去尾空白 */
                    }
                    else
                    {
                        tgt  = rs;
                        tlen = (uint16_t)(w - rs);    /* 无模式：整体为 label 名 */
                    }
                    if (tlen == 0u)
                    {
                        Scl_MsgErr("jump: 缺少目标 label");
                        ok = 0;
                        break;
                    }
                    s_ins[icnt].opc    = (cond != 0u) ? SCL_P_JUMPA : SCL_P_JUMP;
                    s_ins[icnt].cond   = cond;
                    s_ins[icnt].raw    = tgt;
                    s_ins[icnt].rawlen = tlen;
                    icnt++;
                }
                else
                {
                    /* 内置或注册命令 */
                    uint16_t opc = 0u;
                    if (Scl_EqN(hs, "help", 4u) && ((uint16_t)(he - hs) == 4u))
                    {
                        opc = SCL_OP_HELP;
                    }
                    else if (Scl_EqN(hs, "var", 3u) && ((uint16_t)(he - hs) == 3u))
                    {
                        opc = SCL_OP_VAR;
                    }
                    else if (Scl_EqN(hs, "free", 4u) && ((uint16_t)(he - hs) == 4u))
                    {
                        opc = SCL_OP_FREE;
                    }
                    else
                    {
                        scl_cmd_t *nd = Scl_CmdFindName(hs, (uint16_t)(he - hs));
                        if (nd == NULL)
                        {
                            char nbuf[24u];
                            uint16_t nn = ((uint16_t)(he - hs) < 23u) ? (uint16_t)(he - hs) : 23u;
                            uint16_t k;
                            for (k = 0u; k < nn; k++) { nbuf[k] = hs[k]; }
                            nbuf[nn] = '\0';
                            Scl_MsgErr("未知命令 '%s'", nbuf);
                            ok = 0;
                            break;
                        }
                        opc = nd->opc;
                    }
                    s_ins[icnt].opc    = opc;
                    s_ins[icnt].cond   = 0u;
                    s_ins[icnt].raw    = rs;
                    s_ins[icnt].rawlen = (uint16_t)(re - rs);
                    icnt++;
                }
            }
        }
    }

    if (!ok)
    {
        return 0u;
    }
    if (icnt == 0u)
    {
        return 0u;   /* 无指令 */
    }
    if ((uint16_t)(icnt * 4u) > SCL_CFG_BC_MAX)
    {
        Scl_MsgErr("字节码超长");
        return 0u;
    }

    /* ---- 第二遍：生成字节码与参数缓存，解析 jump 目标 ---- */
    s_bc_len = 0u;
    for (i = 0u; i < icnt; i++)
    {
        uint16_t opc = s_ins[i].opc;
        uint16_t aoff = 0u;

        if ((opc == SCL_P_JUMP) || (opc == SCL_P_JUMPA))
        {
            /* 解析目标 label */
            uint8_t found = 0u;
            uint8_t k;
            for (k = 0u; k < s_label_cnt; k++)
            {
                if ((Scl_StrLen(s_labels[k].name) == s_ins[i].rawlen) &&
                    Scl_EqN(s_labels[k].name, s_ins[i].raw, s_ins[i].rawlen))
                {
                    aoff = s_labels[k].off;
                    found = 1u;
                    break;
                }
            }
            if (found == 0u)
            {
                char nbuf[SCL_CFG_LABEL_NAME_MAX + 1u];
                uint16_t nn = (s_ins[i].rawlen < SCL_CFG_LABEL_NAME_MAX) ?
                              s_ins[i].rawlen : SCL_CFG_LABEL_NAME_MAX;
                uint16_t k;
                for (k = 0u; k < nn; k++) { nbuf[k] = s_ins[i].raw[k]; }
                nbuf[nn] = '\0';
                Scl_MsgErr("jump: label '%s' 未定义", nbuf);
                return 0u;
            }
            opc = (s_ins[i].opc == SCL_P_JUMP) ? SCL_OP_JUMP : SCL_OP_JUMPA;
        }
        else
        {
            aoff = Scl_ArgStore(s_ins[i].raw, s_ins[i].rawlen);
            if (aoff == 0xFFFFu)
            {
                Scl_MsgErr("参数字节缓存不足/超长");
                return 0u;
            }
        }
        /* 写 4 字节：opc(大端) + aoff(大端) */
        s_bc[s_bc_len]     = (uint8_t)(opc >> 8);
        s_bc[s_bc_len + 1u] = (uint8_t)(opc & 0xFFu);
        s_bc[s_bc_len + 2u] = (uint8_t)(aoff >> 8);
        s_bc[s_bc_len + 3u] = (uint8_t)(aoff & 0xFFu);
        s_bc_len = (uint16_t)(s_bc_len + 4u);
    }
    return 1u;
}

/* ========================== 内置命令：help / var / free ========================== */

static void Scl_DoHelp(void)
{
    scl_cmd_t *node;
    Scl_Msg("scl: 内置: var / free / help / label / jump\r\n");
    Scl_Msg("scl: 已注册命令:\r\n");
    for (node = s_cmd_head; node != NULL; node = node->next)
    {
        Scl_Msg("  %s%s (opc 0x%04x)\r\n",
                node->name, (node->sync != NULL) ? " (异步)" : "",
                (unsigned int)node->opc);
    }
}

/* 变量列表 */
static void Scl_VarList(void)
{
    int i;
    Scl_Msg("scl: 变量(%d/%d)\r\n", SCL_VarCount(), (int)SCL_CFG_VAR_MAX);
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u)
        {
            Scl_Msg("  %s = %s\r\n", s_vars[i].name, s_vars[i].value);
        }
    }
    Scl_Msg("scl: 剩余空位 %d\r\n", SCL_VarFreeCount());
}

/* var 指令：raw = 'name=value' / ''（列表）/ 'name'（查单）/ 'free'（全释放） */
static void Scl_DoVarRaw(const char *raw)
{
    const char *eq = NULL;
    const char *q;
    int has_eq = 0;

    if (raw == NULL)
    {
        raw = "";
    }
    q = raw;
    while (*q != '\0')
    {
        if (*q == '=')
        {
            has_eq = 1;
            eq = q;
            break;
        }
        q++;
    }

    if (has_eq != 0)
    {
        const char *nb = raw;
        const char *ne = eq;
        const char *vb = eq + 1;
        const char *ve = raw + Scl_StrLen(raw);
        char namebuf[SCL_CFG_VAR_NAME_MAX + 1u];
        char valbuf[SCL_CFG_VAR_VALUE_MAX];
        uint16_t i;
        int r;

        while ((nb < ne) && Scl_IsSp(*nb)) { nb++; }
        while ((ne > nb) && Scl_IsSp(*(ne - 1))) { ne--; }
        while ((vb < ve) && Scl_IsSp(*vb)) { vb++; }
        while ((ve > vb) && Scl_IsSp(*(ve - 1))) { ve--; }
        if ((uint16_t)(ne - nb) > SCL_CFG_VAR_NAME_MAX)
        {
            Scl_MsgErr("var: 变量名过长");
            return;
        }
        for (i = 0u; i < (uint16_t)(ne - nb); i++) { namebuf[i] = nb[i]; }
        namebuf[(uint16_t)(ne - nb)] = '\0';
        if ((uint16_t)(ve - vb) >= 2u)
        {
            if (((*vb == '"') || (*vb == '\'')) && (*(ve - 1) == *vb))
            {
                vb++;
                ve--;
            }
        }
        {
            int er = Scl_ExpandCopy(vb, ve, valbuf, SCL_CFG_VAR_VALUE_MAX);
            if (er < 0)
            {
                Scl_MsgErr("var: 值无效");
                return;
            }
        }
        r = SCL_VarSet(namebuf, valbuf);
        if (r == 0)
        {
            Scl_Msg("scl: var %s = %s (剩余空位 %d)\r\n",
                    namebuf, valbuf, SCL_VarFreeCount());
        }
        else if (r == -1)
        {
            Scl_MsgErr("var: 变量已满(%d 个)", (int)SCL_CFG_VAR_MAX);
        }
        else
        {
            Scl_MsgErr("var: 参数错误(%d)", r);
        }
        return;
    }

    /* 无 '=' → 命令形式 */
    if (raw[0] == '\0')
    {
        Scl_VarList();
    }
    else if (Scl_StrEq(raw, "free"))
    {
        int n = SCL_VarFreeAll();
        Scl_Msg("scl: 释放全部 %d 个变量（剩余空位 %d）\r\n", n, SCL_VarFreeCount());
    }
    else
    {
        const char *vn = SCL_VarGet(raw);
        if (vn == NULL)
        {
            Scl_MsgErr("var: %s 未定义", raw);
        }
        else
        {
            Scl_Msg("scl: var %s = %s\r\n", raw, vn);
        }
    }
}

/* free 指令：raw = ''（全释放）或 'name' */
static void Scl_DoFreeRaw(const char *raw)
{
    if ((raw == NULL) || (raw[0] == '\0'))
    {
        int n = SCL_VarFreeAll();
        Scl_Msg("scl: free 全部 %d 个变量（剩余空位 %d）\r\n", n, SCL_VarFreeCount());
    }
    else
    {
        if (SCL_VarFree(raw) == 0)
        {
            Scl_Msg("scl: free %s（剩余空位 %d）\r\n", raw, SCL_VarFreeCount());
        }
        else
        {
            Scl_MsgErr("free: %s 不存在", raw);
        }
    }
}

/* ========================== 收尾 ========================== */

static void Scl_Finish(int reason)
{
    s_busy     = 0u;
    s_wait_cmd = NULL;
    s_pc       = 0u;
    s_bc_len   = 0u;
    s_arg_len  = 0u;
    s_label_cnt = 0u;
    s_abort    = 0u;
    SCL_VarFreeAll();
    s_ret      = 0u;
    if (reason == 0)
    {
        Scl_Msg("[scl done]\r\n");
    }
    else
    {
        Scl_Msg("[scl abort]\r\n");
    }
}

/* ========================== 解释执行 ========================== */

/* 推进一个动作：解码并执行一条指令（jump/label 不占指令，label 已在此阶段无作用） */
static void Scl_StepOnce(void)
{
    uint16_t opc;
    uint16_t aoff;
    uint16_t next;

    if (s_pc >= s_bc_len)
    {
        Scl_Finish(0);   /* 程序末尾 → 自然完成 */
        return;
    }
    opc = (uint16_t)(((uint16_t)s_bc[s_pc] << 8) | s_bc[s_pc + 1u]);
    aoff = (uint16_t)(((uint16_t)s_bc[s_pc + 2u] << 8) | s_bc[s_pc + 3u]);
    next = (uint16_t)(s_pc + 4u);

    switch (opc)
    {
    case SCL_OP_JUMP:
        if (aoff >= s_bc_len)
        {
            Scl_MsgErr("jump: 目标越界");
            Scl_Finish(1);
            return;
        }
        s_pc = aoff;
        return;

    case SCL_OP_JUMPA:
        if (Scl_RetTake() != 0)
        {
            if (aoff >= s_bc_len)
            {
                Scl_MsgErr("jump -a: 目标越界");
                Scl_Finish(1);
                return;
            }
            s_pc = aoff;
        }
        else
        {
            s_pc = next;
        }
        return;

    case SCL_OP_HELP:
        Scl_DoHelp();
        s_pc = next;
        return;

    case SCL_OP_VAR:
        Scl_DoVarRaw(Scl_ArgLoad(aoff));
        s_pc = next;
        return;

    case SCL_OP_FREE:
        Scl_DoFreeRaw(Scl_ArgLoad(aoff));
        s_pc = next;
        return;

    default:
        {
            scl_cmd_t *nd = Scl_CmdFindOp(opc);
            if (nd == NULL)
            {
                Scl_MsgErr("未知 opcode 0x%04x", (unsigned int)opc);
                Scl_Finish(1);
                return;
            }
            {
                const char *raw = Scl_ArgLoad(aoff);
                int argc = (raw != NULL) ? Scl_TokenizeRaw(raw) : 0;
                if (argc < 0)
                {
                    Scl_MsgErr("命令参数错误(%d)", argc);
                    s_pc = next;
                    return;
                }
                if (nd->sync != NULL)
                {
                    s_wait_cmd = nd;   /* 异步：先登记等待，再发起 */
                }
                nd->fn(argc, s_argv);
                s_pc = next;
            }
        }
        return;
    }
}

/* ========================== 公共接口实现 ========================== */

void SCL_Init(void)
{
    s_cmd_head  = NULL;
    s_next_opc  = (uint16_t)SCL_OP_CMD_BASE;
    s_busy      = 0u;
    s_abort     = 0u;
    s_wait_cmd  = NULL;
    s_pc        = 0u;
    s_bc_len    = 0u;
    s_arg_len   = 0u;
    s_label_cnt = 0u;
    s_ret       = 0u;
    SCL_VarFreeAll();
    s_inited    = 1u;
}

uint8_t SCL_Run(const char *script)
{
    uint16_t i;

    if (s_inited == 0u)
    {
        SCL_Init();
    }
    if (s_busy != 0u)
    {
        Scl_MsgErr("busy: 有脚本正在执行");
        return 0u;
    }
    if (script == NULL)
    {
        return 0u;
    }
    /* 文本长度防御检查（文本不驻留，编译即弃） */
    i = 0u;
    while ((script[i] != '\0') && (i + 1u < SCL_CFG_SCRIPT_MAX))
    {
        i++;
    }
    if (script[i] != '\0')
    {
        Scl_MsgErr("脚本过长(>%d)", (int)(SCL_CFG_SCRIPT_MAX - 1u));
        return 0u;
    }
    if (i == 0u)
    {
        return 0u;   /* 空脚本 */
    }
    /* 编译（失败保持空闲） */
    if (Scl_Compile(script) == 0u)
    {
        s_bc_len = 0u;
        s_arg_len = 0u;
        s_label_cnt = 0u;
        return 0u;
    }

    /* 启动执行 */
    s_pc      = 0u;
    s_wait_cmd = NULL;
    s_abort   = 0u;
    s_ret     = 0u;
    s_busy    = 1u;
    return 1u;
}

void SCL_Loop(void)
{
    if (s_inited == 0u)
    {
        SCL_Init();
    }
    if (s_busy == 0u)
    {
        return;
    }
    /* 响应中断请求 */
    if (s_abort != 0u)
    {
        Scl_MsgErr("aborted");
        Scl_Finish(1);
        return;
    }
    /* 异步等待轮询 */
    if (s_wait_cmd != NULL)
    {
        if (s_wait_cmd->sync != NULL)
        {
            if (s_wait_cmd->sync(false))
            {
                s_wait_cmd->sync(true);
                s_wait_cmd = NULL;
            }
        }
        else
        {
            s_wait_cmd = NULL;
        }
        return;
    }
    /* 推进一条指令 */
    Scl_StepOnce();
}

uint8_t SCL_Idle(void)
{
    return (s_busy == 0u) ? 1u : 0u;
}

void SCL_Abort(void)
{
    s_abort = 1u;
}
