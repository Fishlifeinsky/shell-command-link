/**
  ******************************************************************************
  * @file    scl_core.c
  * @brief   SCL 核心基础件：文本/数值小工具 + 消息输出（全程无 libc 依赖）
  *
  *          由 scl.c 拆出（模块化）：
  *            - 文本：长度 / 空白与标识符判定 / 比较（含不区分大小写）
  *            - 数值：Scl_ParseI32Len（10/16/2 进制）、Scl_FmtI32（十进制）
  *            - 消息：Scl_Msg / Scl_MsgErr（%s %c %d %u %x，支持 0 与宽度修饰）
  *              与 SCL_MsgLvl 运行级门控；SCL_CFG_MSG_EN=0 时整段裁掉
  *
  *          这些函数被编译链、运行器、描述/help 共用，故不带 static，
  *          原型统一放 scl_priv.h。实现与迁移前逐行一致（行为零变化）。
  ******************************************************************************
  */

#include "scl.h"
#include "scl_priv.h"

/* 变参消息 */
#include <stdarg.h>

/* ========================== 移植输出 ========================== */

#if (SCL_CFG_MSG_EN == 1u)
extern void SCL_Port_PutChar(char c);
#endif

/* ========================== 文本小工具（不依赖 libc） ========================== */

uint16_t Scl_StrLen(const char *s)
{
    uint16_t n = 0u;
    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

uint8_t Scl_IsSp(char c)
{
    return ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n')) ? 1u : 0u;
}

uint8_t Scl_IsNm(char c)
{
    if ((c >= 'a') && (c <= 'z')) { return 1u; }
    if ((c >= 'A') && (c <= 'Z')) { return 1u; }
    if ((c >= '0') && (c <= '9')) { return 1u; }
    if (c == '_') { return 1u; }
    return 0u;
}

uint8_t Scl_EqN(const char *a, const char *b, uint16_t n)
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

uint8_t Scl_StrEq(const char *a, const char *b)
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

char Scl_Up(char c)
{
    return ((c >= 'a') && (c <= 'z')) ? (char)(c - 32) : c;
}

uint8_t Scl_IsAl(char c)
{
    return (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))) ? 1u : 0u;
}

uint8_t Scl_IsDigit(char c)
{
    return ((c >= '0') && (c <= '9')) ? 1u : 0u;
}

/* 不区分大小写比较前 n 字符 */
uint8_t Scl_EqIN(const char *a, const char *b, uint16_t n)
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
int Scl_ParseI32Len(const char *s, uint16_t len, int32_t *out)
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
uint16_t Scl_FmtI32(int32_t val, char *dst, uint16_t cap)
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

/* ========================== 消息输出（MSG_EN 关则整段裁掉） ========================== */

#if (SCL_CFG_MSG_EN == 1u)

/* 十进制输出（Scl_VMsg 内部小格式化 %d/%u 用；MSG_EN=0 整段裁掉，无死代码） */
static void Scl_PutU32(uint32_t v)
{
    char tmp[10u];
    int  i = 0;
    if (v == 0u)
    {
        SCL_Port_PutChar('0');
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
        SCL_Port_PutChar(tmp[i]);
    }
}

static void Scl_PutU32Padded(uint32_t v, unsigned int width, int zero_pad, int base_hex)
{
    char tmp[16u];
    unsigned int i = 0u;
    unsigned int digits = 0u;
    uint32_t x = v;

    if (v == 0u)
    {
        digits = 1u;
    }
    else
    {
        while (x > 0u)
        {
            uint32_t d = x % ((base_hex) ? 16u : 10u);
            tmp[i++] = (char)((d < 10u) ? ('0' + d) : ('A' + (d - 10u)));
            x /= ((base_hex) ? 16u : 10u);
        }
        digits = i;
    }

    if (width > digits)
    {
        unsigned int pad = width - digits;
        while (pad-- > 0u)
        {
            SCL_Port_PutChar(zero_pad ? '0' : ' ');
        }
    }

    if (v == 0u)
    {
        SCL_Port_PutChar('0');
        return;
    }

    while (i > 0u)
    {
        i--;
        SCL_Port_PutChar(tmp[i]);
    }
}

/* 最小变参格式化：%s %c %d %u %x %%（可带 0 与宽度修饰），其余字符原样输出 */
static void Scl_VMsg(const char *fmt, va_list ap)
{
    while (*fmt != '\0')
    {
        if (*fmt == '%')
        {
            const char *p = fmt + 1u;
            unsigned int width = 0u;
            int zero_pad = 0;
            if (*p == '0')
            {
                zero_pad = 1;
                p++;
            }
            while ((*p >= '0') && (*p <= '9'))
            {
                width = width * 10u + (unsigned int)(*p - '0');
                p++;
            }

            if (*p == 's')
            {
                const char *s = va_arg(ap, const char *);
                if (s == NULL) { s = "(null)"; }
                while (*s != '\0') { SCL_Port_PutChar(*s); s++; }
                fmt = p + 1u;
            }
            else if (*p == 'c')
            {
                SCL_Port_PutChar((char)va_arg(ap, int));
                fmt = p + 1u;
            }
            else if (*p == 'd')
            {
                int v = va_arg(ap, int);
                if (v < 0)
                {
                    SCL_Port_PutChar('-');
                    if (width > 1u)
                    {
                        unsigned int n = (unsigned int)(-(int32_t)v);
                        unsigned int digits = 1u;
                        uint32_t x = n;
                        while (x >= 10u)
                        {
                            x /= 10u;
                            digits++;
                        }
                        if (width > digits)
                        {
                            while (width-- > digits) { SCL_Port_PutChar(zero_pad ? '0' : ' '); }
                        }
                    }
                    Scl_PutU32((uint32_t)(-(int32_t)v));
                }
                else
                {
                    if (width > 1u)
                    {
                        unsigned int digits = 1u;
                        uint32_t x = (uint32_t)v;
                        while (x >= 10u)
                        {
                            x /= 10u;
                            digits++;
                        }
                        if (width > digits)
                        {
                            while (width-- > digits) { SCL_Port_PutChar(zero_pad ? '0' : ' '); }
                        }
                    }
                    Scl_PutU32((uint32_t)v);
                }
                fmt = p + 1u;
            }
            else if (*p == 'u')
            {
                unsigned int v = (unsigned int)va_arg(ap, unsigned int);
                if (width > 1u)
                {
                    unsigned int digits = 1u;
                    uint32_t x = (uint32_t)v;
                    while (x >= 10u)
                    {
                        x /= 10u;
                        digits++;
                    }
                    if (width > digits)
                    {
                        while (width-- > digits) { SCL_Port_PutChar(zero_pad ? '0' : ' '); }
                    }
                }
                Scl_PutU32((uint32_t)v);
                fmt = p + 1u;
            }
            else if (*p == 'x')
            {
                uint32_t v = (uint32_t)va_arg(ap, unsigned int);
                if (width > 0u)
                {
                    unsigned int digits = 1u;
                    uint32_t x = v;
                    while (x >= 16u)
                    {
                        x /= 16u;
                        digits++;
                    }
                    if (width > digits)
                    {
                        while (width-- > digits) { SCL_Port_PutChar('0'); }
                    }
                }
                {
                    int shift;
                    uint8_t started = 0u;
                    for (shift = 28; shift >= 0; shift -= 4)
                    {
                        uint8_t d = (uint8_t)((v >> shift) & 0x0Fu);
                        if ((d != 0u) || started || (shift == 0) || (width == 0u))
                        {
                            started = 1u;
                            SCL_Port_PutChar((d < 10u) ? (char)('0' + d) : (char)('A' + (d - 10u)));
                        }
                    }
                }
                fmt = p + 1u;
            }
            else if (*p == '%')
            {
                SCL_Port_PutChar('%');
                fmt = p + 1u;
            }
            else
            {
                SCL_Port_PutChar('%');
                fmt = fmt + 1u;
            }
        }
        else
        {
            SCL_Port_PutChar(*fmt);
            fmt++;
        }
    }
}
#endif /* SCL_CFG_MSG_EN */

/* ============================ 消息级别（全局，运行期可调） ============================ */

uint8_t SCL_MsgLvl = (uint8_t)SCL_CFG_MSG_LVL;

void SCL_MsgLevelSet(uint8_t lvl)
{
    SCL_MsgLvl = lvl;
}

uint8_t SCL_MsgLevelGet(void)
{
    return SCL_MsgLvl;
}

/* 信息级输出（低于 SCL_MSG_INFO 且未通过门控则丢弃）
   注意：本函数仅在 MSG_EN=1 时存在；MSG_EN=0 时 scl_priv.h 已把它们宏化为空，
   故定义前必须 #undef，否则下面的函数名会被宏替换。 */
#if (SCL_CFG_MSG_EN == 1u)
#undef Scl_Msg
#undef Scl_MsgErr

void Scl_Msg(const char *fmt, ...)
{
    if (SCL_MsgLvl < (uint8_t)SCL_MSG_INFO) { return; }   /* 运行级门控 */
    {
        va_list ap;
        va_start(ap, fmt);
        Scl_VMsg(fmt, ap);
        va_end(ap);
    }
}

/* 错误级输出（自带 "scl: " 前缀与 CRLF） */
void Scl_MsgErr(const char *fmt, ...)
{
    if (SCL_MsgLvl < (uint8_t)SCL_MSG_ERR) { return; }    /* 错误级门控 */
    {
        va_list ap;
        Scl_Msg("scl: ");
        va_start(ap, fmt);
        Scl_VMsg(fmt, ap);
        va_end(ap);
        Scl_Msg("\r\n");
    }
}
#endif /* SCL_CFG_MSG_EN */
