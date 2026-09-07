/**
  ******************************************************************************
  * @file    scl.c
  * @brief   SCL（Shell-Command-Link）简易指令链脚本库 —— 实现（v0.2 类型化参数缓存）
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
  *                参数写入"参数字节缓存"并**同步解析成类型块**（type 开头，无空格）：
  *                  BOOL=0x01+v(1) | INT=0x02+4B 大端 | FLAG=0x03+c(1) | STR=0x04+len(1)+bytes
  *                元指令（var/free/help/label/jump）参数按整段 STR 块存原文内部解析；
  *                业务命令/运算指令参数按字面量类型化；无参 argOff=0（保留缓存第 0 字节）
  *                label → 登记到 label 表 (名 → 下一条指令字节偏移)
  *                jump  → 编译期把名解析为目标偏移写入 argOff 槽
  *            - 命令在 SCL_RegisterCmd() 时自动分配 opcode（自 0x0100 起）
  *            - 变量类型化：bool/int/flag/string，槽存 type + 规范化文本（见 scl.h）
  *            - 内置 int/bool 运算指令（保留关键字，见 scl.h 注释）
  *            - 主循环周期调 SCL_Loop() 逐条解释执行字节码；业务命令分同步/异步：
  *                异步命令 handler 立即返回，库在指令边界轮询其 sync 回调完成后再
  *                执行下一条
  *            - 全程静态内存、无 malloc、无 OS/HAL/libc 依赖
  *
  *          编译期错误（label 重名/未定义、未知命令、类型/超限）→ 拒绝该链并保持空闲。
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

    /* 内置 int/bool 运算指令（保留关键字，参考 C；0x0010 段与注册命令 0x0100+ 不冲突） */
    SCL_OP_IADD = 0x0010u,  /* iadd  a b dst   : int a+b → dst */
    SCL_OP_ISUB = 0x0011u,  /* isub  a b dst   : int a-b → dst */
    SCL_OP_IMUL = 0x0012u,  /* imul  a b dst   : int a*b → dst */
    SCL_OP_IDIV = 0x0013u,  /* idiv  a b dst   : int a/b（0 除报错）→ dst */
    SCL_OP_IMOD = 0x0014u,  /* imod  a b dst   : int a%%b → dst */
    SCL_OP_INEG = 0x0015u,  /* ineg  a dst     : int -a → dst */
    SCL_OP_IEQ  = 0x0016u,  /* ieq  a b : G_RETURN = (a==b) */
    SCL_OP_INE  = 0x0017u,  /* ine  a b : G_RETURN = (a!=b) */
    SCL_OP_IGT  = 0x0018u,  /* igt  a b : G_RETURN = (a>b)  */
    SCL_OP_IGE  = 0x0019u,  /* ige  a b : G_RETURN = (a>=b) */
    SCL_OP_ILT  = 0x001Au,  /* ilt  a b : G_RETURN = (a<b)  */
    SCL_OP_ILE  = 0x001Bu,  /* ile  a b : G_RETURN = (a<=b) */
    SCL_OP_BAND = 0x001Cu,  /* band a b : G_RETURN = 布尔与 */
    SCL_OP_BOR  = 0x001Du,  /* bor  a b : G_RETURN = 布尔或 */
    SCL_OP_BNOT = 0x001Eu,  /* bnot a   : G_RETURN = 布尔非 */
    SCL_OP_BTEST = 0x001Fu, /* btest a  : G_RETURN = 操作数真值（int≠0/bool/flag 已定义） */

    /* v0.3：int 位运算/移位（保留字，参考 C 的 & | ^ ~ << >>） */
    SCL_OP_IAND = 0x0020u,  /* iand a b dst : a & b  → dst */
    SCL_OP_IOR  = 0x0021u,  /* ior  a b dst : a | b  → dst */
    SCL_OP_IXOR = 0x0022u,  /* ixor a b dst : a ^ b  → dst */
    SCL_OP_INOT = 0x0023u,  /* inot a dst   : ~a     → dst */
    SCL_OP_SHL  = 0x0024u,  /* shl  a b dst : a << b → dst */
    SCL_OP_SHR  = 0x0025u,  /* shr  a b dst : a >> b → dst */
    SCL_OP_SEQ  = 0x0026u,  /* seq  a b   : G_RETURN = (文本 a==b) */
    SCL_OP_SNEQ = 0x0027u,  /* sneq a b   : G_RETURN = (文本 a!=b) */

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

/* ---- 变量表：类型化（name + type + 规范化文本值） ---- */
typedef struct
{
    char     name[SCL_CFG_VAR_NAME_MAX + 1u];
    uint8_t  type;                       /* SCL_T_BOOL/INT/FLAG/STR */
    char     value[SCL_CFG_VAR_VALUE_MAX]; /* 规范化文本（bool true/false；int 十进制；flag "-x"；string 原文） */
    uint8_t  used;
} scl_var_t;

static scl_var_t s_vars[SCL_CFG_VAR_MAX];

/* ---- 字节码程序（编译产物，跨 tick 持久） ---- */
static uint8_t  s_bc[SCL_CFG_BC_MAX];       /* 每条指令 4 字节 */
static uint16_t s_bc_len = 0u;              /* 有效字节数（4 的倍数） */
static uint16_t s_pc     = 0u;              /* 程序计数器（解释执行） */
static uint32_t s_steps  = 0u;              /* 本脚本已执行步数（步进保护） */

/* ---- 参数字节缓存：每条指令参数区 = [total(1)][type 块序列]，argOff 指向 total 字节。
     type 块（type 开头，无空格分隔）：
       BOOL=0x01+v(1) | INT=0x02+4B 大端 | FLAG=0x03+c(1) | STR=0x04+len(1)+bytes
     total=块序列字节数（不含自身）；argOff==0 表示无参数（保留缓存第 0 字节） ---- */
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
static char  s_raw[SCL_RAW_MAX];             /* 从缓存取出的参数原文（STR 块） */
static char  s_argb[SCL_CFG_ARG_MAX][SCL_CFG_ARG_LEN_MAX];
static char *s_argv[SCL_CFG_ARG_MAX];
static uint8_t s_argt[SCL_CFG_ARG_MAX];       /* 当前命令各参数 type（SCL_ArgType 用） */

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

/* ===================== 类型/整数小工具（v0.2，无 libc） ===================== */

static char Scl_Up(char c)
{
    return ((c >= 'a') && (c <= 'z')) ? (char)(c - 32) : c;
}

static uint8_t Scl_IsAl(char c)
{
    return (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))) ? 1u : 0u;
}

static uint8_t Scl_IsDigit(char c)
{
    return ((c >= '0') && (c <= '9')) ? 1u : 0u;
}

/* 不区分大小写比较前 n 字符 */
static uint8_t Scl_EqIN(const char *a, const char *b, uint16_t n)
{
    uint16_t i;
    for (i = 0u; i < n; i++)
    {
        if (Scl_Up(a[i]) != Scl_Up(b[i]))
        {
            return 0u;
        }
    }
    return 1u;
}

/* 解析整段整数（十进制可带 '-'；0x/0X 十六进制；0b/0B 二进制）。
   成功返回 0 并写 *out；非法/溢出返回 -1 */
static int Scl_ParseI32Len(const char *s, uint16_t len, int32_t *out)
{
    uint16_t i = 0u;
    int neg = 0;
    uint32_t v = 0u;
    uint32_t lim;

    if ((len == 0u) || (s == NULL))
    {
        return -1;
    }
    /* 0x 十六进制 / 0b 二进制 前缀 */
    if ((len > 2u) && (s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X') ||
                                        (s[1] == 'b') || (s[1] == 'B')))
    {
        uint32_t base = ((s[1] == 'x') || (s[1] == 'X')) ? 16u : 2u;
        uint32_t acc = 0u;
        uint16_t j;
        for (j = 2u; j < len; j++)
        {
            char c = s[j];
            uint32_t d;
            if ((c >= '0') && (c <= '9')) { d = (uint32_t)(c - '0'); }
            else if ((c >= 'a') && (c <= 'f')) { d = (uint32_t)(c - 'a' + 10u); }
            else if ((c >= 'A') && (c <= 'F')) { d = (uint32_t)(c - 'A' + 10u); }
            else { return -1; }
            if (d >= base) { return -1; }
            if (acc > (0xFFFFFFFFu - d) / base) { return -1; }   /* 溢出 */
            acc = acc * base + d;
        }
        *out = (int32_t)acc;   /* 0xFFFFFFFF → -1 亦允许 */
        return 0;
    }
    if (s[0] == '-')
    {
        neg = 1;
        i = 1u;
        if (i >= len)
        {
            return -1;
        }
    }
    lim = neg ? 2147483648u : 2147483647u;
    for (; i < len; i++)
    {
        uint32_t d;
        if (!Scl_IsDigit(s[i]))
        {
            return -1;
        }
        d = (uint32_t)(s[i] - '0');
        if (v > (lim - d) / 10u)
        {
            return -1;   /* 溢出 */
        }
        v = v * 10u + d;
    }
    if (neg)
    {
        *out = (int32_t)(0u - v);   /* INT32_MIN 亦正确 */
    }
    else
    {
        *out = (int32_t)v;
    }
    return 0;
}

/* 整数格式化为十进制写入 dst（cap 含结尾 '\0'）。成功返回长度；失败返回 0 */
static uint16_t Scl_FmtI32(int32_t val, char *dst, uint16_t cap)
{
    uint32_t u = (uint32_t)val;
    char tmp[11u];
    uint16_t n = 0u;
    uint8_t i = 0u;
    int neg = (val < 0) ? 1 : 0;

    if (neg)
    {
        if (n + 1u >= cap) { return 0u; }
        dst[n++] = '-';
        u = 0u - u;   /* 模 2^32 取绝对值，INT32_MIN 亦正确 */
    }
    if (u == 0u)
    {
        if (n + 1u >= cap) { return 0u; }
        dst[n++] = '0';
    }
    else
    {
        while (u > 0u)
        {
            tmp[i] = (char)('0' + (u % 10u));
            u /= 10u;
            i++;
        }
        while (i > 0u)
        {
            i--;
            if (n + 1u >= cap) { return 0u; }
            dst[n++] = tmp[i];
        }
    }
    if (n + 1u > cap) { return 0u; }
    dst[n] = '\0';
    return n;
}

/* 编译期字面量 token 归类（命令参数）：返回 SCL_T_*；
   FLAG→*iv=字符代码；BOOL→0/1；INT→数值；无法定类一律 STR（*iv 忽略） */
static uint8_t Scl_LitType(const char *s, uint16_t len, int32_t *iv)
{
    if ((len == 2u) && (s[0] == '-') && Scl_IsAl(s[1]))
    {
        *iv = s[1];            /* -x → flag */
        return SCL_T_FLAG;
    }
    if (len == 4u && Scl_EqIN(s, "true", 4u))
    {
        *iv = 1;
        return SCL_T_BOOL;
    }
    if (len == 5u && Scl_EqIN(s, "false", 5u))
    {
        *iv = 0;
        return SCL_T_BOOL;
    }
    if (len >= 1u)
    {
        int32_t v;
        if (Scl_ParseI32Len(s, len, &v) == 0)
        {
            *iv = v;
            return SCL_T_INT;
        }
    }
    return SCL_T_STR;
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

/* ========================== 变量实现（v0.2 类型化） ========================== */

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

/* 按值推断类型：true/false→bool；-x 单字符→flag；十进制→int；其余→string */
static uint8_t Scl_InferType(const char *val)
{
    uint16_t len = Scl_StrLen(val);
    int32_t  iv;
    if (len == 0u) { return SCL_T_STR; }
    if (Scl_EqIN(val, "true", 4u) || Scl_EqIN(val, "false", 5u)) { return SCL_T_BOOL; }
    if ((len == 2u) && (val[0] == '-') && Scl_IsAl(val[1])) { return SCL_T_FLAG; }
    if (Scl_ParseI32Len(val, len, &iv) == 0) { return SCL_T_INT; }
    return SCL_T_STR;
}

/* 按类型校验并把值规范化为文本写入 out（cap 含 '\0'）。0=成功；非 0=值非法 */
static int Scl_VarNorm(uint8_t type, const char *val, char *out, uint16_t cap)
{
    uint16_t len;
    uint16_t i;

    if (val == NULL) { val = ""; }
    len = Scl_StrLen(val);

    if (type == SCL_T_BOOL)
    {
        uint8_t on = 0u;
        if ((len == 4u && Scl_EqIN(val, "true", 4u)) || (len == 1u && val[0] == '1'))
        {
            on = 1u;
        }
        else if ((len == 5u && Scl_EqIN(val, "false", 5u)) || (len == 1u && val[0] == '0'))
        {
            on = 0u;
        }
        else
        {
            return -1;
        }
        if (on != 0u) { out[0] = 't'; out[1] = 'r'; out[2] = 'u'; out[3] = 'e'; out[4] = '\0'; }
        else          { out[0] = 'f'; out[1] = 'a'; out[2] = 'l'; out[3] = 's'; out[4] = 'e'; out[5] = '\0'; }
        return 0;
    }
    if (type == SCL_T_INT)
    {
        int32_t iv;
        if (Scl_ParseI32Len(val, len, &iv) != 0) { return -1; }
        if (Scl_FmtI32(iv, out, cap) == 0u) { return -1; }
        return 0;
    }
    if (type == SCL_T_FLAG)
    {
        if ((len != 2u) || (val[0] != '-') || !Scl_IsAl(val[1])) { return -1; }
        out[0] = '-'; out[1] = val[1]; out[2] = '\0';
        return 0;
    }
    /* SCL_T_STR */
    if (len >= cap) { return -1; }
    for (i = 0u; i < len; i++) { out[i] = val[i]; }
    out[len] = '\0';
    return 0;
}

/* 类型化写入核心（脚本 'var' 与 C 命令共用） */
static int Scl_VarSetCore(const char *name, uint8_t type, const char *val)
{
    char norm[SCL_CFG_VAR_VALUE_MAX];
    int  idx;
    int  r;

    if ((name == NULL) || (name[0] == '\0'))
    {
        return -4;
    }
    if (!Scl_VarNameOk(name))
    {
        return -2;
    }
    r = Scl_VarNorm(type, val, norm, (uint16_t)sizeof(norm));
    if (r != 0)
    {
        return -3;   /* 值非法/过长 */
    }
    idx = Scl_VarFind(name);
    if (idx < 0)
    {
        uint16_t i;
        for (idx = 0; idx < (int)SCL_CFG_VAR_MAX; idx++)
        {
            if (s_vars[idx].used == 0u)
            {
                break;
            }
        }
        if (idx >= (int)SCL_CFG_VAR_MAX)
        {
            return -1;   /* 满 */
        }
        s_vars[idx].used = 1u;
        for (i = 0u; name[i] != '\0'; i++)
        {
            s_vars[idx].name[i] = name[i];
        }
        s_vars[idx].name[i] = '\0';
    }
    s_vars[idx].type = type;
    {
        uint16_t i;
        for (i = 0u; norm[i] != '\0'; i++)
        {
            s_vars[idx].value[i] = norm[i];
        }
        s_vars[idx].value[i] = '\0';
    }
    return 0;
}

/* 自动推断类型设置（C 命令便捷用） */
int SCL_VarSet(const char *name, const char *val)
{
    if (val == NULL) { val = ""; }
    return Scl_VarSetCore(name, Scl_InferType(val), val);
}

/* 显式类型设置（v0.2；'var' 脚本路径） */
int SCL_VarSetT(const char *name, uint8_t type, const char *val)
{
    return Scl_VarSetCore(name, type, val);
}

uint8_t SCL_VarType(const char *name)
{
    int idx = Scl_VarFind(name);
    return (idx < 0) ? 0u : s_vars[idx].type;
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
    s_vars[idx].type = 0u;
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
            s_vars[i].type = 0u;
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

/* 变量数值化（供运算/条件指令）：
   int→值；bool→1/0；flag→已定义即 1；string→整段解析失败按 0（ok=0） */
static int32_t Scl_VarNum(const char *name, uint8_t *ok)
{
    int idx = Scl_VarFind(name);
    int32_t iv = 0;
    if (idx < 0)
    {
        if (ok != NULL) { *ok = 0u; }
        return 0;
    }
    switch (s_vars[idx].type)
    {
    case SCL_T_INT:
        if (Scl_ParseI32Len(s_vars[idx].value, Scl_StrLen(s_vars[idx].value), &iv) != 0)
        {
            if (ok != NULL) { *ok = 0u; }
            return 0;
        }
        break;
    case SCL_T_BOOL:
        iv = (Scl_EqIN(s_vars[idx].value, "true", 4u)) ? 1 : 0;
        break;
    case SCL_T_FLAG:
        iv = (s_vars[idx].value[0] != '\0') ? 1 : 0;
        break;
    default: /* STR：尝试整段数字 */
        if (Scl_ParseI32Len(s_vars[idx].value, Scl_StrLen(s_vars[idx].value), &iv) != 0)
        {
            if (ok != NULL) { *ok = 0u; }
            return 0;
        }
        break;
    }
    if (ok != NULL) { *ok = 1u; }
    return iv;
}

/* 变量真值（供 btest/布尔运算）：
   int→非 0；bool→true；flag→已定义；string→非空且非 false/0 */
static uint8_t Scl_VarTruth(const char *name)
{
    int idx = Scl_VarFind(name);
    uint8_t len;
    if (idx < 0)
    {
        return 0u;
    }
    switch (s_vars[idx].type)
    {
    case SCL_T_INT:
        return (s_vars[idx].value[0] == '-') ? 1u :
               ((s_vars[idx].value[0] == '0') && (s_vars[idx].value[1] == '\0')) ? 0u : 1u;
    case SCL_T_BOOL:
        return (Scl_EqIN(s_vars[idx].value, "true", 4u)) ? 1u : 0u;
    case SCL_T_FLAG:
        return (s_vars[idx].value[0] != '\0') ? 1u : 0u;
    default:
        len = (uint8_t)Scl_StrLen(s_vars[idx].value);
        if ((len == 0u) || Scl_EqIN(s_vars[idx].value, "false", 5u) ||
            ((len == 1u) && (s_vars[idx].value[0] == '0')))
        {
            return 0u;
        }
        return 1u;
    }
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

int SCL_ArgType(int idx)
{
    if ((idx < 0) || (idx >= (int)SCL_CFG_ARG_MAX))
    {
        return 0;
    }
    return (int)s_argt[idx];
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

/* ===================== 参数字节缓存：type 块编解码（v0.2） ===================== */

/* 参数区布局：argOff 指向 [total(1)]，其后为若干 type 块；total=块序列字节数。
   type 块：BOOL=01+v(1) / INT=02+4B大端 / FLAG=03+c(1) / STR=04+len(1)+bytes
   缓存第 0 字节保留哨兵；argOff==0 表示无参数。 */

/* 块写入原语（追加到缓存尾，成功 0；缓存不足/超长返回 -1，不推进） */
static int Scl_BlkPutBool(uint8_t v)
{
    if ((uint16_t)(s_arg_len + 2u) > SCL_CFG_ARG_CACHE_MAX) { return -1; }
    s_argc[s_arg_len++] = SCL_T_BOOL;
    s_argc[s_arg_len++] = (uint8_t)((v != 0u) ? 1u : 0u);
    return 0;
}

static int Scl_BlkPutInt(int32_t v)
{
    uint32_t u = (uint32_t)v;
    if ((uint16_t)(s_arg_len + 5u) > SCL_CFG_ARG_CACHE_MAX) { return -1; }
    s_argc[s_arg_len++] = SCL_T_INT;
    s_argc[s_arg_len++] = (uint8_t)(u >> 24);
    s_argc[s_arg_len++] = (uint8_t)(u >> 16);
    s_argc[s_arg_len++] = (uint8_t)(u >> 8);
    s_argc[s_arg_len++] = (uint8_t)(u & 0xFFu);
    return 0;
}

static int Scl_BlkPutFlag(char c)
{
    if ((uint16_t)(s_arg_len + 2u) > SCL_CFG_ARG_CACHE_MAX) { return -1; }
    s_argc[s_arg_len++] = SCL_T_FLAG;
    s_argc[s_arg_len++] = (uint8_t)c;
    return 0;
}

static int Scl_BlkPutStr(const char *data, uint16_t len)
{
    uint16_t i;
    if (len > 255u) { return -1; }
    if ((uint16_t)(s_arg_len + 2u + len) > SCL_CFG_ARG_CACHE_MAX) { return -1; }
    s_argc[s_arg_len++] = SCL_T_STR;
    s_argc[s_arg_len++] = (uint8_t)len;
    for (i = 0u; i < len; i++) { s_argc[s_arg_len++] = (uint8_t)data[i]; }
    return 0;
}

/* 开启一个参数区：返回区头偏移（total 字节位置）；缓存不足返回 0xFFFF */
static uint16_t Scl_AreaBegin(void)
{
    uint16_t off;
    if (s_arg_len == 0u)
    {
        s_arg_len = 1u;   /* 保留第 0 字节作"无参"哨兵 */
    }
    off = s_arg_len;
    if ((uint16_t)(off + 1u) >= SCL_CFG_ARG_CACHE_MAX)
    {
        return 0xFFFFu;
    }
    s_arg_len = (uint16_t)(s_arg_len + 1u);   /* total 占位 */
    return off;
}

/* 结束参数区：回填 total；超长回退到 off 返回 -1 */
static int Scl_AreaEnd(uint16_t off)
{
    uint16_t total = (uint16_t)(s_arg_len - off - 1u);
    if (total > 254u)
    {
        s_arg_len = off;
        return -1;
    }
    s_argc[off] = (uint8_t)total;
    return 0;
}

/* 编译期（元指令）：把整段原文存成"单个 STR 块"参数区。返回区偏移；空参 0；失败 0xFFFF */
static uint16_t Scl_ArgStoreMeta(const char *raw, uint16_t rawlen)
{
    uint16_t off;
    if (rawlen == 0u) { return 0u; }
    off = Scl_AreaBegin();
    if (off == 0xFFFFu) { return 0xFFFFu; }
    if (Scl_BlkPutStr(raw, rawlen) != 0) { s_arg_len = off; return 0xFFFFu; }
    if (Scl_AreaEnd(off) != 0) { return 0xFFFFu; }
    return off;
}

/* 编译期（业务命令/运算指令）：把参数文本 [s,end) 切字面量参数并类型化存储。
   返回区偏移；无参 0；失败 0xFFFF */
static uint16_t Scl_ArgStoreTyped(const char *s, const char *end)
{
    uint16_t off;
    uint8_t  any = 0u;
    const char *p = s;

    if (end == NULL) { end = s; }
    off = Scl_AreaBegin();
    if (off == 0xFFFFu) { return 0xFFFFu; }

    while (1)
    {
        const char *ab;
        const char *ae;
        uint8_t ty;

        while ((p < end) && Scl_IsSp(*p)) { p++; }
        if (p >= end) { break; }

        if ((*p == '"') || (*p == '\''))
        {
            char q = *p;
            p++;
            ab = p;
            while ((p < end) && (*p != q)) { p++; }
            if (p >= end) { s_arg_len = off; return 0xFFFFu; }   /* 引号未闭合 */
            ae = p;
            p++;   /* 跳过闭引号 */
            /* 引号内容一律 string（即使内容是数字） */
            if (Scl_BlkPutStr(ab, (uint16_t)(ae - ab)) != 0)
            {
                s_arg_len = off;
                return 0xFFFFu;
            }
        }
        else
        {
            int32_t iv;
            ab = p;
            while ((p < end) && !Scl_IsSp(*p))
            {
                if ((*p == '"') || (*p == '\'')) { break; }
                p++;
            }
            ae = p;
            ty = Scl_LitType(ab, (uint16_t)(ae - ab), &iv);
            if (ty == SCL_T_BOOL)
            {
                if (Scl_BlkPutBool((uint8_t)iv) != 0) { s_arg_len = off; return 0xFFFFu; }
            }
            else if (ty == SCL_T_INT)
            {
                if (Scl_BlkPutInt(iv) != 0) { s_arg_len = off; return 0xFFFFu; }
            }
            else if (ty == SCL_T_FLAG)
            {
                if (Scl_BlkPutFlag((char)iv) != 0) { s_arg_len = off; return 0xFFFFu; }
            }
            else
            {
                if (Scl_BlkPutStr(ab, (uint16_t)(ae - ab)) != 0)
                {
                    s_arg_len = off;
                    return 0xFFFFu;
                }
            }
        }
        any = 1u;
        /* 注：不要在此处 break —— 裸 token 结束处 ae==p 属正常（下一轮顶部跳空白再取下一参）。
           防死循环：本轮必须推进 p（裸 token 至少 1 字符；引号分支必越过闭引号） */
    }

    if (any == 0u)
    {
        s_arg_len = off;
        return 0u;
    }
    if (Scl_AreaEnd(off) != 0)
    {
        s_arg_len = off;
        return 0xFFFFu;
    }
    return off;
}

/* 运行时（元指令）：从 aoff 读取整段原文（参数区须为单个 STR 块，不做 ${} 展开）。
   无参数/不符返回 NULL */
static const char *Scl_ArgLoad(uint16_t aoff)
{
    uint16_t total;
    uint16_t o;
    uint16_t len;
    uint16_t i;

    if (aoff == 0u) { return NULL; }
    if (aoff >= s_arg_len) { return NULL; }
    total = s_argc[aoff];
    if ((uint16_t)(aoff + 1u + total) > s_arg_len) { return NULL; }
    o = aoff + 1u;
    if (total == 0u) { return ""; }
    if ((s_argc[o] != SCL_T_STR) || (total < 2u)) { return NULL; }
    len = s_argc[o + 1u];
    if ((uint16_t)(2u + len) > total) { return NULL; }
    if (len >= SCL_RAW_MAX) { return NULL; }
    for (i = 0u; i < len; i++) { s_raw[i] = (char)s_argc[o + 2u + i]; }
    s_raw[len] = '\0';
    return s_raw;
}

/* 运行时：把 aoff 处一个 type 块转为文本写入 dst（cap 含 '\0'；STR 做 ${} 展开）。
   返回下一块偏移；越界/失败返回 0xFFFF */
static uint16_t Scl_BlkText(uint16_t o, char *dst, uint16_t cap)
{
    uint8_t  t;
    uint16_t len;
    uint16_t i;

    if (o >= s_arg_len) { return 0xFFFFu; }
    t = s_argc[o];
    if (t == SCL_T_BOOL)
    {
        if ((uint16_t)(o + 2u) > s_arg_len) { return 0xFFFFu; }
        if (s_argc[o + 1u] != 0u)
        {
            if (cap < 5u) { return 0xFFFFu; }
            dst[0] = 't'; dst[1] = 'r'; dst[2] = 'u'; dst[3] = 'e'; dst[4] = '\0';
        }
        else
        {
            if (cap < 6u) { return 0xFFFFu; }
            dst[0] = 'f'; dst[1] = 'a'; dst[2] = 'l'; dst[3] = 's'; dst[4] = 'e'; dst[5] = '\0';
        }
        return (uint16_t)(o + 2u);
    }
    if (t == SCL_T_INT)
    {
        int32_t v;
        if ((uint16_t)(o + 5u) > s_arg_len) { return 0xFFFFu; }
        v = (int32_t)(((uint32_t)s_argc[o + 1u] << 24) |
                      ((uint32_t)s_argc[o + 2u] << 16) |
                      ((uint32_t)s_argc[o + 3u] << 8) |
                      (uint32_t)s_argc[o + 4u]);
        if (Scl_FmtI32(v, dst, cap) == 0u) { return 0xFFFFu; }
        return (uint16_t)(o + 5u);
    }
    if (t == SCL_T_FLAG)
    {
        if ((uint16_t)(o + 2u) > s_arg_len) { return 0xFFFFu; }
        if (cap < 3u) { return 0xFFFFu; }
        dst[0] = '-'; dst[1] = (char)s_argc[o + 1u]; dst[2] = '\0';
        return (uint16_t)(o + 2u);
    }
    if (t == SCL_T_STR)
    {
        const char *sb;
        if ((uint16_t)(o + 2u) > s_arg_len) { return 0xFFFFu; }
        len = s_argc[o + 1u];
        if ((uint16_t)(o + 2u + len) > s_arg_len) { return 0xFFFFu; }
        sb = (const char *)&s_argc[o + 2u];
        if (Scl_ExpandCopy(sb, sb + len, dst, cap) < 0) { return 0xFFFFu; }
        return (uint16_t)(o + 2u + len);
    }
    (void)i;
    (void)len;
    return 0xFFFFu;   /* 未知 type */
}

/* 运行时：还原某指令参数区为 argv（type 块 → 文本；STR 展开 ${}；记录各参数 type）。
   返回 argc；0=无参数；负=错误 */
static int Scl_ArgRestore(uint16_t aoff)
{
    uint16_t total;
    uint16_t end;
    uint16_t o;
    int ai = 0;

    if (aoff == 0u) { return 0; }
    if (aoff >= s_arg_len) { return -1; }
    total = s_argc[aoff];
    if ((uint16_t)(aoff + 1u + total) > s_arg_len) { return -1; }
    end = (uint16_t)(aoff + 1u + total);
    o = aoff + 1u;
    while (o < end)
    {
        uint16_t nxt;
        if (ai >= (int)SCL_CFG_ARG_MAX) { return -3; }
        nxt = Scl_BlkText(o, s_argb[ai], SCL_CFG_ARG_LEN_MAX);
        if (nxt == 0xFFFFu) { return -4; }
        s_argt[ai] = (uint8_t)s_argc[o];   /* 参数原始 type */
        s_argv[ai] = s_argb[ai];
        ai++;
        if (nxt <= o) { break; }   /* 防死循环 */
        o = nxt;
    }
    return ai;
}

/* ============ 内置 int/bool 运算指令保留字表（参考 C） ============ */

typedef struct
{
    const char *name;
    uint8_t     len;
    uint16_t    opc;
} scl_opword_t;

static const scl_opword_t s_opwords[] =
{
    { "iadd", 4u,  SCL_OP_IADD  }, { "isub", 4u,  SCL_OP_ISUB  },
    { "imul", 4u,  SCL_OP_IMUL  }, { "idiv", 4u,  SCL_OP_IDIV  },
    { "imod", 4u,  SCL_OP_IMOD  }, { "ineg", 4u,  SCL_OP_INEG  },
    { "ieq",  3u,  SCL_OP_IEQ   }, { "ine",  3u,  SCL_OP_INE   },
    { "igt",  3u,  SCL_OP_IGT   }, { "ige",  3u,  SCL_OP_IGE   },
    { "ilt",  3u,  SCL_OP_ILT   }, { "ile",  3u,  SCL_OP_ILE   },
    { "band", 4u,  SCL_OP_BAND  }, { "bor",  3u,  SCL_OP_BOR   },
    { "bnot", 4u,  SCL_OP_BNOT  }, { "btest",5u,  SCL_OP_BTEST },
    { "iand", 4u,  SCL_OP_IAND  }, { "ior",  3u,  SCL_OP_IOR   },
    { "ixor", 4u,  SCL_OP_IXOR  }, { "inot", 4u,  SCL_OP_INOT  },
    { "shl",  3u,  SCL_OP_SHL   }, { "shr",  3u,  SCL_OP_SHR   },
    { "seq",  3u,  SCL_OP_SEQ   }, { "sneq", 4u,  SCL_OP_SNEQ  }
};

/* 按名字查内置运算指令 opcode；不是返回 0 */
static uint16_t Scl_OpWord(const char *s, uint16_t n)
{
    uint8_t k;
    for (k = 0u; k < (uint8_t)(sizeof(s_opwords) / sizeof(s_opwords[0])); k++)
    {
        if ((uint16_t)s_opwords[k].len == n && Scl_EqN(s_opwords[k].name, s, n))
        {
            return s_opwords[k].opc;
        }
    }
    return 0u;
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
            /* v0.3：整句注释 —— 以 '#' 开头的子句在编译期跳过（值内 '#' 不受影响） */
            if ((hs < he) && (*hs == '#'))
            {
                continue;
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
                    /* 内置运算指令 / 内置元命令 / 注册命令 */
                    uint16_t opc = 0u;
                    opc = Scl_OpWord(hs, (uint16_t)(he - hs));   /* 运算指令优先（保留字） */
                    if (opc == 0u)
                    {
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
        else if ((opc == SCL_OP_HELP) || (opc == SCL_OP_VAR) || (opc == SCL_OP_FREE))
        {
            /* 元指令：整段原文按单个 STR 块存（var/free/help 内部自行解析） */
            aoff = Scl_ArgStoreMeta(s_ins[i].raw, s_ins[i].rawlen);
            if (aoff == 0xFFFFu)
            {
                Scl_MsgErr("参数字节缓存不足/超长");
                return 0u;
            }
        }
        else
        {
            /* 业务命令/运算指令：字面量类型化存储 */
            aoff = Scl_ArgStoreTyped(s_ins[i].raw, s_ins[i].raw + s_ins[i].rawlen);
            if (aoff == 0xFFFFu)
            {
                Scl_MsgErr("参数字节缓存不足/参数超长/引号未闭合");
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

/* 类型名 ↔ type */
static const char *Scl_TypeName(uint8_t t)
{
    switch (t)
    {
    case SCL_T_BOOL: return "bool";
    case SCL_T_INT:  return "int";
    case SCL_T_FLAG: return "flag";
    default:         return "string";
    }
}

/* 按名字取类型（bool/int/flag/string），不是返回 0 */
static uint8_t Scl_TypeOfName(const char *s, uint16_t n)
{
    if (n == 4u && Scl_EqIN(s, "bool", 4u))   { return SCL_T_BOOL; }
    if (n == 3u && Scl_EqIN(s, "int", 3u))    { return SCL_T_INT; }
    if (n == 4u && Scl_EqIN(s, "flag", 4u))   { return SCL_T_FLAG; }
    if (n == 6u && Scl_EqIN(s, "string", 6u)) { return SCL_T_STR; }
    return 0u;
}

static void Scl_DoHelp(void)
{
    scl_cmd_t *node;
    Scl_Msg("scl: 内置元命令: var / free / help / label / jump\r\n");
    Scl_Msg("scl: 内置运算: iadd isub imul idiv imod ineg | ieq ine igt ige ilt ile\r\n");
    Scl_Msg("scl:            band bor bnot btest | iand ior ixor inot shl shr | seq sneq\r\n");
    Scl_Msg("scl: var <type> <name>=<value>, type = bool/int/flag/string\r\n");
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
            Scl_Msg("  %s : %s = %s\r\n", s_vars[i].name,
                    Scl_TypeName(s_vars[i].type), s_vars[i].value);
        }
    }
    Scl_Msg("scl: 剩余空位 %d\r\n", SCL_VarFreeCount());
}

/* var 指令（v0.2 显式类型）：
     var                      列表
     var free                 释放全部
     var <type> <name>=<value> 声明/覆盖（type ∈ bool/int/flag/string）
     var <name>               查询单个
   缺类型（如 var x=1）→ 报错提示补类型 */
static void Scl_DoVarRaw(const char *raw)
{
    const char *p;
    uint8_t typ;

    if (raw == NULL)
    {
        raw = "";
    }
    if (raw[0] == '\0')
    {
        Scl_VarList();
        return;
    }
    if (Scl_StrEq(raw, "free"))
    {
        int n = SCL_VarFreeAll();
        Scl_Msg("scl: 释放全部 %d 个变量（剩余空位 %d）\r\n", n, SCL_VarFreeCount());
        return;
    }

    /* 首 token */
    p = raw;
    while (Scl_IsSp(*p)) { p++; }
    {
        const char *tb = p;
        const char *te = p;
        while ((*te != '\0') && !Scl_IsSp(*te)) { te++; }
        typ = Scl_TypeOfName(tb, (uint16_t)(te - tb));
    }

    if (typ != 0u)
    {
        /* ---- 声明路径：type name = value ---- */
        const char *eq;
        const char *rs = p;
        const char *q;
        const char *nb, *ne, *vb, *ve;
        char namebuf[SCL_CFG_VAR_NAME_MAX + 1u];
        char valbuf[SCL_CFG_VAR_VALUE_MAX];
        uint16_t i;
        int r;

        (void)rs;
        q = p;
        while ((*q != '\0') && (*q != '=')) { q++; }
        eq = (*q == '=') ? q : NULL;
        if (eq == NULL)
        {
            Scl_MsgErr("var: 语法 var <type> <name>=<value>");
            return;
        }
        nb = p;
        ne = eq;
        while ((nb < ne) && Scl_IsSp(*nb)) { nb++; }
        while ((ne > nb) && Scl_IsSp(*(ne - 1))) { ne--; }
        /* 跳过 name 前多余空白后，name 起点应越过类型词 */
        {
            const char *t = nb;
            while ((t < ne) && !Scl_IsSp(*t)) { t++; }   /* 跳过类型词 */
            while ((t < ne) && Scl_IsSp(*t)) { t++; }
            nb = t;
        }
        if ((nb >= ne) || ((uint16_t)(ne - nb) > SCL_CFG_VAR_NAME_MAX))
        {
            Scl_MsgErr("var: 变量名不合法");
            return;
        }
        for (i = 0u; i < (uint16_t)(ne - nb); i++) { namebuf[i] = nb[i]; }
        namebuf[(uint16_t)(ne - nb)] = '\0';

        vb = eq + 1;
        ve = eq + Scl_StrLen(eq);
        while ((vb < ve) && Scl_IsSp(*vb)) { vb++; }
        while ((ve > vb) && Scl_IsSp(*(ve - 1))) { ve--; }
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
        r = SCL_VarSetT(namebuf, typ, valbuf);
        if (r == 0)
        {
            Scl_Msg("scl: var %s : %s = %s (剩余空位 %d)\r\n",
                    namebuf, Scl_TypeName(typ), valbuf, SCL_VarFreeCount());
        }
        else if (r == -1)
        {
            Scl_MsgErr("var: 变量已满(%d 个)", (int)SCL_CFG_VAR_MAX);
        }
        else
        {
            Scl_MsgErr("var: 参数错误(%d, 值须匹配 %s)", r, Scl_TypeName(typ));
        }
        return;
    }

    /* ---- 非类型开头 ---- */
    {
        const char *q = raw;
        while ((*q != '\0') && (*q != '=')) { q++; }
        if (*q == '=')
        {
            Scl_MsgErr("var: 缺少类型, 如 var int i=1");
            return;
        }
    }
    {
        const char *vn = SCL_VarGet(raw);
        uint8_t vt = SCL_VarType(raw);
        if (vn == NULL)
        {
            Scl_MsgErr("var: %s 未定义", raw);
        }
        else
        {
            Scl_Msg("scl: var %s : %s = %s\r\n", raw, Scl_TypeName(vt), vn);
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
    s_steps    = 0u;
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

/* ========================== 内置运算指令执行（int/bool，参考 C） ========================== */

/* 操作数 → 数值：变量名取变量值；否则按字面量（true=1/false=0/十进制/其余 0+ok=0） */
static int32_t Scl_NumText(const char *tok, uint8_t *ok)
{
    int32_t  iv;
    uint16_t len;
    if (ok != NULL) { *ok = 1u; }
    if (SCL_VarType(tok) != 0u) { return Scl_VarNum(tok, ok); }
    len = Scl_StrLen(tok);
    if (len == 4u && Scl_EqIN(tok, "true", 4u))  { return 1; }
    if (len == 5u && Scl_EqIN(tok, "false", 5u)) { return 0; }
    if (Scl_ParseI32Len(tok, len, &iv) == 0) { return iv; }
    if (ok != NULL) { *ok = 0u; }
    return 0;
}

/* 操作数 → 真值：变量名取变量真值；否则按字面量 */
static uint8_t Scl_TruthText(const char *tok)
{
    int32_t  iv;
    uint16_t len;
    if (SCL_VarType(tok) != 0u) { return Scl_VarTruth(tok); }
    len = Scl_StrLen(tok);
    if (len == 4u && Scl_EqIN(tok, "true", 4u))  { return 1u; }
    if (len == 5u && Scl_EqIN(tok, "false", 5u)) { return 0u; }
    if (Scl_ParseI32Len(tok, len, &iv) == 0) { return (iv != 0) ? 1u : 0u; }
    if (len == 0u) { return 0u; }
    if ((len == 1u) && (tok[0] == '0')) { return 0u; }
    return 1u;
}

/* 算术（结果写回目标变量）：iadd/isub/imul/idiv/imod a b dst；ineg a dst */
static void Scl_DoArith(uint16_t opc, int argc, char *argv[])
{
    int32_t a = 0;
    int32_t b = 0;
    int32_t r = 0;
    uint8_t oka = 1u;
    uint8_t okb = 1u;
    const char *dst;
    char nb[SCL_CFG_VAR_VALUE_MAX];

    if ((opc == SCL_OP_INEG) || (opc == SCL_OP_INOT))
    {
        const char *nm = (opc == SCL_OP_INEG) ? "ineg" : "inot";
        if (argc < 2) { Scl_MsgErr("%s: 需要 2 参 (a dst)", nm); return; }
        a = Scl_NumText(argv[0], &oka);
        dst = argv[1];
        r = (opc == SCL_OP_INEG) ? -a : (~a);
    }
    else
    {
        if (argc < 3) { Scl_MsgErr("算术: 需要 3 参 (a b dst)"); return; }
        a = Scl_NumText(argv[0], &oka);
        b = Scl_NumText(argv[1], &okb);
        dst = argv[2];
        if (!oka || !okb) { Scl_MsgErr("算术: 操作数非数值"); return; }
        switch (opc)
        {
        case SCL_OP_IADD: r = a + b; break;
        case SCL_OP_ISUB: r = a - b; break;
        case SCL_OP_IMUL: r = a * b; break;
        case SCL_OP_IDIV:
            if (b == 0) { Scl_MsgErr("idiv: 除数为 0"); return; }
            r = a / b;
            break;
        case SCL_OP_IMOD:
            if (b == 0) { Scl_MsgErr("imod: 除数为 0"); return; }
            r = a % b;
            break;
        case SCL_OP_IAND: r = a & b; break;
        case SCL_OP_IOR:  r = a | b; break;
        case SCL_OP_IXOR: r = a ^ b; break;
        case SCL_OP_SHL:  r = (int32_t)((uint32_t)a << (b & 31)); break;
        case SCL_OP_SHR:  r = (int32_t)((uint32_t)a >> (b & 31)); break;  /* 逻辑右移 */
        default: return;
        }
    }
    if (!oka) { Scl_MsgErr("算术: 操作数非数值"); return; }
    if (Scl_FmtI32(r, nb, (uint16_t)sizeof(nb)) == 0u) { Scl_MsgErr("算术: 结果溢出"); return; }
    if (SCL_VarSetT(dst, SCL_T_INT, nb) != 0)
    {
        Scl_MsgErr("算术: 目标变量 '%s' 无效或已满", dst);
    }
}

/* int 比较（结果 → G_RETURN）：ieq/ine/igt/ige/ilt/ile a b */
static void Scl_DoCmp(uint16_t opc, int argc, char *argv[])
{
    int32_t a;
    int32_t b;
    uint8_t oka;
    uint8_t okb;
    uint8_t r = 0u;

    if (argc < 2) { Scl_MsgErr("比较: 需要 2 参 (a b)"); return; }
    a = Scl_NumText(argv[0], &oka);
    b = Scl_NumText(argv[1], &okb);
    if (!oka || !okb) { Scl_MsgErr("比较: 操作数非数值"); return; }
    switch (opc)
    {
    case SCL_OP_IEQ: r = (a == b); break;
    case SCL_OP_INE: r = (a != b); break;
    case SCL_OP_IGT: r = (a > b);  break;
    case SCL_OP_IGE: r = (a >= b); break;
    case SCL_OP_ILT: r = (a < b);  break;
    case SCL_OP_ILE: r = (a <= b); break;
    default: return;
    }
    SCL_Ret_Set(r ? 1 : 0);
}

/* 字符串相等/不等（结果 → G_RETURN）：seq / sneq a b（文本比较，参数可含空格引号） */
static void Scl_DoStrCmp(uint16_t opc, int argc, char *argv[])
{
    int eq;
    if (argc < 2)
    {
        Scl_MsgErr("%s: 需要 2 参 (a b)", (opc == SCL_OP_SEQ) ? "seq" : "sneq");
        return;
    }
    eq = Scl_StrEq(argv[0], argv[1]) ? 1 : 0;
    SCL_Ret_Set((opc == SCL_OP_SEQ) ? eq : (eq ? 0 : 1));
}

/* bool 逻辑（结果 → G_RETURN）：band/bor a b；bnot/btest a */
static void Scl_DoBool(uint16_t opc, int argc, char *argv[])
{
    uint8_t r = 0u;
    if (opc == SCL_OP_BAND)
    {
        if (argc < 2) { Scl_MsgErr("band: 需要 2 参 (a b)"); return; }
        r = (Scl_TruthText(argv[0]) != 0u) && (Scl_TruthText(argv[1]) != 0u);
    }
    else if (opc == SCL_OP_BOR)
    {
        if (argc < 2) { Scl_MsgErr("bor: 需要 2 参 (a b)"); return; }
        r = (Scl_TruthText(argv[0]) != 0u) || (Scl_TruthText(argv[1]) != 0u);
    }
    else if (opc == SCL_OP_BNOT)
    {
        if (argc < 1) { Scl_MsgErr("bnot: 需要 1 参 (a)"); return; }
        r = (Scl_TruthText(argv[0]) == 0u);
    }
    else /* BTEST */
    {
        if (argc < 1) { Scl_MsgErr("btest: 需要 1 参 (a)"); return; }
        r = Scl_TruthText(argv[0]);
    }
    SCL_Ret_Set(r ? 1 : 0);
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
    /* 步进保护（SCL_CFG_STEP_LIMIT=0 关闭）：防 while(true) 死循环 */
    s_steps++;
#if (SCL_CFG_STEP_LIMIT != 0u)
    if (s_steps > (uint32_t)SCL_CFG_STEP_LIMIT)
    {
        Scl_MsgErr("步进超限(>%u)，已中断（防死循环）", (unsigned int)SCL_CFG_STEP_LIMIT);
        Scl_Finish(1);
        return;
    }
#endif
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
        if ((opc == SCL_OP_SEQ) || (opc == SCL_OP_SNEQ))
        {
            int argc = Scl_ArgRestore(aoff);
            if (argc < 0) { Scl_MsgErr("字符串比较参数错误(%d)", argc); }
            else { Scl_DoStrCmp(opc, argc, s_argv); }
            s_pc = next;
            return;
        }
        if (((opc >= SCL_OP_IADD) && (opc <= SCL_OP_BTEST)) ||
            ((opc >= SCL_OP_IAND) && (opc <= SCL_OP_SHR)))
        {
            /* 内置运算指令（无独立命令节点） */
            int argc = Scl_ArgRestore(aoff);
            if (argc < 0)
            {
                Scl_MsgErr("运算参数错误(%d)", argc);
            }
            else if (((opc >= SCL_OP_IADD) && (opc <= SCL_OP_INEG)) ||
                     ((opc >= SCL_OP_IAND) && (opc <= SCL_OP_SHR)))
            {
                Scl_DoArith(opc, argc, s_argv);
            }
            else if ((opc >= SCL_OP_IEQ) && (opc <= SCL_OP_ILE))
            {
                Scl_DoCmp(opc, argc, s_argv);
            }
            else
            {
                Scl_DoBool(opc, argc, s_argv);
            }
            s_pc = next;
            return;
        }
        {
            scl_cmd_t *nd = Scl_CmdFindOp(opc);
            if (nd == NULL)
            {
                Scl_MsgErr("未知 opcode 0x%04x", (unsigned int)opc);
                Scl_Finish(1);
                return;
            }
            {
                int argc = Scl_ArgRestore(aoff);
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
    s_steps   = 0u;
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
