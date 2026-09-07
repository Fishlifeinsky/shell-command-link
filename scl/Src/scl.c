/**
  ******************************************************************************
  * @file    scl.c
  * @brief   SCL（Shell-Command-Link）简易指令链脚本库 —— 实现
  *
  *          执行模型（异步/跨主循环步进，用户已确认）：
  *            - SCL_Run() 把脚本拷贝进内部程序缓冲后立即返回并置忙（busy）；
  *            - 主循环周期调用 SCL_Loop()，每调用推进"一个动作"（执行一条子句，
  *              或轮询正在异步等待的业务命令是否完成）；
  *            - 业务命令分同步/异步：异步命令 handler 立即返回，库在命令边界
  *              轮询其 sync(false)，完成后 sync(true) 清除，再继续下一条；
  *            - 程序文本在执行期间"只读不改写"，执行状态（游标+帧栈）全部静态，
  *              跨 tick 存活；不 C 递归，天然可挂起。
  *
  *          结构与关键字：
  *            - 子句：以 ';' 分隔（引号内 ';' 不分割，引号保护）；去首尾空白
  *            - 变量：'var name=value' / 'var'（列表+剩余位）/ 'var name'（查单个）/
  *                    'var free'（全释放）；'free [name]'；脚本结束自动全释放
  *            - 展开：命令实参内 ${name} 取值（支持拼接），缺失变量展开为空并提示
  *            - 条件 G_RETURN：默认 false；if / while -e 读取即清零；SCL_Ret_Set() 写
  *            - if：-t/-f 值可为双引号多指令子链或单条命令；真/假各选其一支（可省略）
  *            - while：'while -b; <body>; while -e [N]'
  *                -e 读到 G_RETURN==false 退出；==true 回跳 body（do-while）；
  *                N=显式迭代上限；SCL_CFG_WHILE_MAX 兜底防死循环
  *            - 命令调用：普通式 'cmd a b'；函数式 'cmd_xxx(a,b,...)'（'(' 紧邻命令名）
  *
  *          保留关键字：if / while / var / free / help（不能注册为业务命令）
  *
  *          移植：输出经 SCL_Port_PutChar()（用户实现）；SCL_CFG_MSG_EN=0 时可裁掉
  ******************************************************************************
  */

/* 本模块头文件 */
#include "scl.h"

/* 变参消息 */
#include <stdarg.h>

/* ========================== 移植输出 ========================== */

/* 库内消息统一经 SCL_Port_PutChar() 逐字符输出（用户实现） */
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
#if ((SCL_CFG_SCRIPT_MAX) < 16u)
#error "SCL_CFG_SCRIPT_MAX must >= 16"
#endif

/* ========================== 静态状态 ========================== */

/* 首次自动初始化标记 */
static uint8_t s_inited = 0u;

/* ---- 命令链表头 ---- */
static scl_cmd_t *s_cmd_head = NULL;

/* ---- 变量表 ---- */
typedef struct
{
    char     name[SCL_CFG_VAR_NAME_MAX + 1u];  /* 变量名（含 '\0'） */
    char     value[SCL_CFG_VAR_VALUE_MAX];     /* 变量值缓冲（含 '\0'） */
    uint8_t  used;                             /* 1=已使用 */
} scl_var_t;

static scl_var_t s_vars[SCL_CFG_VAR_MAX];      /* 变量槽（无 malloc） */

/* ---- 内部程序缓冲（异步跨 tick 需持久保存文本，执行期间只读） ---- */
static char s_prog[SCL_CFG_SCRIPT_MAX];        /* 脚本文本拷贝 */
static const char *s_prog_end;                 /* 程序有效文本结尾（不含 '\0'） */
static const char *s_cur;                      /* 当前扫描游标 */

/* ---- 条件标志 G_RETURN ---- */
static uint8_t s_ret;                          /* 0=假；1=真；if/while -e 读后清零 */

/* ---- 执行状态 ---- */
static uint8_t  s_busy;                        /* 1=正在执行脚本 */
static volatile uint8_t s_abort;               /* 1=请求中断（可从中断上下文置位） */
static scl_cmd_t *s_wait_cmd;                  /* 正在异步等待的命令节点（NULL=无） */

/* ---- 执行帧栈（显式，不 C 递归） ---- */
typedef enum
{
    SCL_FR_WHILE = 0u,   /* while 循环帧 */
    SCL_FR_SEG   = 1u    /* 段帧（if 分支子链等内层文本区间） */
} scl_frame_kind_t;

typedef struct
{
    scl_frame_kind_t kind;     /* 帧类型 */
    const char *body;          /* WHILE: 每次迭代起点(-b 子句之后)；SEG: 段文本起点 */
    const char *end;           /* WHILE: -e 子句结束点(退出后续点，即扫描界)；
                                  SEG : 段文本结束位置(不包含，扫描界) */
    const char *e_start;       /* WHILE: 匹配 -e 子句起点(定位用) */
    const char *resume;        /* 本帧结束后的续点 */
    uint32_t    iter;          /* WHILE: 已迭代次数 */
} scl_frame_t;

static scl_frame_t s_frames[SCL_CFG_NEST_MAX]; /* 帧栈（含 while 与段帧） */
static uint8_t     s_frame_cnt;                /* 已用帧数 */

/* 命令实参工作缓冲（展开后实参，本次调用有效） */
static char  s_argb[SCL_CFG_ARG_MAX][SCL_CFG_ARG_LEN_MAX];
static char *s_argv[SCL_CFG_ARG_MAX];          /* argv 指针（指向 s_argb） */

/* ========================== 内部小工具（不依赖 libc） ========================== */

/* 字符串长度（不含 '\0'） */
static uint16_t Scl_StrLen(const char *s)
{
    uint16_t n = 0u;
    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

/* 判断空白字符（空格/Tab/回车/换行，均视为分隔） */
static uint8_t Scl_IsSp(char c)
{
    return ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n')) ? 1u : 0u;
}

/* 变量名字符：字母/数字/下划线 */
static uint8_t Scl_IsNm(char c)
{
    if ((c >= 'a') && (c <= 'z')) { return 1u; }
    if ((c >= 'A') && (c <= 'Z')) { return 1u; }
    if ((c >= '0') && (c <= '9')) { return 1u; }
    if (c == '_') { return 1u; }
    return 0u;
}

/* 前 n 字节与字符串逐字节相等（不比较 '\0' 结尾本身） */
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

/* 完整字符串相等 */
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

/* 十进制无符号数输出（内部） */
static void Scl_PutU32(uint32_t v)
{
    char  tmp[10u];   /* 十进制最多 10 位 */
    int   i = 0;

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

/* 十六进制无符号数输出（内部） */
static void Scl_PutHex(uint32_t v)
{
    int    shift;
    uint8_t started = 0u;

    for (shift = 28; shift >= 0; shift -= 4)
    {
        uint8_t d = (uint8_t)((v >> shift) & 0x0Fu);
        if ((d != 0u) || started || (shift == 0))
        {
            started = 1u;
#if (SCL_CFG_MSG_EN == 1u)
            SCL_Port_PutChar((d < 10u) ? (char)('0' + d) : (char)('A' + (d - 10u)));
#endif
        }
    }
}

/* 带格式消息输出：支持 %s %c %d %u %x（内部实现，无 libc 依赖） */
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
                char c = (char)va_arg(ap, int);
                SCL_Port_PutChar(c);
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
                Scl_PutHex((uint32_t)va_arg(ap, unsigned int));
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

/* 带前缀消息输出（用户不可见/内部） */
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

/* 错误提示（带 "scl: " 前缀） */
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

/* 查找变量槽下标（不存在返回 -1） */
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

/* 名字合法性：非空、首字符字母/下划线、其余字母/数字/下划线、不超长 */
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
    int  idx;
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
        return -3;   /* 值过长（需预留 '\0'） */
    }

    idx = Scl_VarFind(name);
    if (idx < 0)
    {
        /* 新建：找空槽 */
        for (idx = 0; idx < (int)SCL_CFG_VAR_MAX; idx++)
        {
            if (s_vars[idx].used == 0u)
            {
                break;
            }
        }
        if (idx >= (int)SCL_CFG_VAR_MAX)
        {
            return -1;   /* 已满 */
        }
        s_vars[idx].used = 1u;
        /* 复制名字（长度已在合法性校验内保证 <= NAME_MAX） */
        {
            uint16_t i;
            for (i = 0u; name[i] != '\0'; i++)
            {
                s_vars[idx].name[i] = name[i];
            }
            s_vars[idx].name[i] = '\0';
        }
    }
    /* 复制值 */
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
    if (idx < 0)
    {
        return NULL;
    }
    return s_vars[idx].value;
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

/* 读取并清零（if / while -e 专用："每次读完重置"） */
static int Scl_RetTake(void)
{
    int r = (s_ret != 0u) ? 1 : 0;
    s_ret = 0u;
    return r;
}

/* ========================== 命令链表 ========================== */

void SCL_RegisterCmd(scl_cmd_t *cmd)
{
    scl_cmd_t **pp;

    if (cmd == NULL)
    {
        return;
    }
    cmd->next = NULL;
    /* 追加到链表尾 */
    pp = &s_cmd_head;
    while (*pp != NULL)
    {
        pp = &((*pp)->next);
    }
    *pp = cmd;
}

/* 按名字长度在链表中查找命令（head 非 '\0' 结尾，用长度比较） */
static scl_cmd_t *Scl_CmdFindN(const char *name, uint16_t len)
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

/* ========================== 文本扫描小工具 ========================== */

/* 跳过空白 */
static const char *Scl_SkipSp(const char *p, const char *end)
{
    while ((p < end) && Scl_IsSp(*p))
    {
        p++;
    }
    return p;
}

/* 回退末尾空白，返回"去尾空白"后的终点 */
static const char *Scl_TrimEnd(const char *p, const char *end)
{
    while ((end > p) && Scl_IsSp(*(end - 1)))
    {
        end--;
    }
    return end;
}

/* ========================== 引号栈（单/双引号交替嵌套支持） ========================== */

/* 引号状态栈：遇到 '"' 或 '\'' 时，若与栈顶同类型则闭合弹栈，否则压栈。
   这样支持 "..." 内嵌 '...'（及反向）的交替嵌套；同类型不做嵌套（按配对处理）。
   用于扫描时保护 ';'/','/'='/')' 不被引号内的同类字符误分割。 */
typedef struct
{
    char st[4u];   /* 引号栈（类型字符，深度上限 4） */
    int  top;      /* 栈顶索引（0=空） */
} scl_qs_t;

static void Scl_QS_Init(scl_qs_t *q)
{
    q->top = 0;
}

static void Scl_QS_Char(scl_qs_t *q, char c)
{
    if ((c != '"') && (c != '\''))
    {
        return;
    }
    if ((q->top > 0) && (q->st[q->top - 1] == c))
    {
        q->top--;   /* 同类型闭合 */
        return;
    }
    if (q->top < 4)
    {
        q->st[q->top++] = c;   /* 压栈（不同类型内嵌） */
    }
}

static int Scl_QS_Open(const scl_qs_t *q)
{
    return (q->top > 0) ? 1 : 0;
}

/**
  * @brief  读取一段引号字符串（支持 '"' 与 '\'' 及交替嵌套）
  * @param  pp  指向开引号；成功后推进到闭引号之后
  * @param  cb / ce 引号内容边界（不含两端引号；可为空）
  * @retval 1=成功；0=未闭合
  */
static int Scl_TakeQuoted(const char **pp, const char *bnd,
                          const char **cb, const char **ce)
{
    const char *p = *pp;
    char open = *p;
    scl_qs_t qs;

    Scl_QS_Init(&qs);
    Scl_QS_Char(&qs, open);
    p++;
    *cb = p;
    while (p < bnd)
    {
        char c = *p;
        Scl_QS_Char(&qs, c);
        if (!Scl_QS_Open(&qs))
        {
            *ce = p;        /* 闭引号位置（内容不含它） */
            *pp = p + 1u;
            return 1;
        }
        p++;
    }
    return 0;   /* 未闭合 */
}

/* 子句首 token 长度：到空白 / '(' / 终点停止 */
static uint16_t Scl_HeadLen(const char *cs, const char *ce)
{
    const char *p = cs;
    while (p < ce)
    {
        char c = *p;
        if ((c == '(') || Scl_IsSp(c))
        {
            break;
        }
        p++;
    }
    return (uint16_t)(p - cs);
}

/* 首 token 是否与字符串 str 相同（长度需完全一致） */
static uint8_t Scl_HeadIs(const char *cs, const char *ce, const char *str)
{
    uint16_t hl = Scl_HeadLen(cs, ce);
    uint16_t sl = Scl_StrLen(str);
    return ((hl == sl) && Scl_EqN(cs, str, sl)) ? 1u : 0u;
}

/**
  * @brief  在 [p, bnd) 内取下一子句（引号保护：引号内 ';' 不分割）
  * @param  st 子句文本起点（已去前导空白/分隔 ';'）
  * @param  en 子句文本终点（不含其终止 ';'）
  * @param  nx 子句之后下一扫描点（已跳过终止 ';'）
  * @retval 1=找到；0=区域结束（只有空白/分隔）
  */
static int Scl_FindClause(const char *p, const char *bnd,
                          const char **st, const char **en, const char **nx)
{
    const char *s = p;
    scl_qs_t qs;

    Scl_QS_Init(&qs);

    /* 跳过分隔 ';' 与空白（此处必为引号外） */
    while (s < bnd)
    {
        if (*s == ';') { s++; continue; }
        if (Scl_IsSp(*s)) { s++; continue; }
        break;
    }
    if (s >= bnd)
    {
        return 0;
    }
    *st = s;
    /* 扫描到顶层 ';' 或 bnd（引号栈保护：引号内 ';' 不分割） */
    while (s < bnd)
    {
        Scl_QS_Char(&qs, *s);
        if ((*s == ';') && (!Scl_QS_Open(&qs))) { break; }
        s++;
    }
    *en = s;
    if (s < bnd)
    {
        s++;   /* 跳过终止 ';' */
    }
    *nx = s;
    return 1;
}

/**
  * @brief  ${name} 展开拷贝：[src, end) 内容拷到 dst（容量 cap 含 '\0'）
  * @retval 0=成功；-1=语法错误（未闭合/空名/名超长）；-2=超长
  * @note   缺失变量：展开为空并打印提示（容错，不终止）
  */
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
            const char *ns = p + 2u;   /* 名字起点 */
            const char *q2 = ns;

            while ((q2 < end) && Scl_IsNm(*q2))
            {
                q2++;
            }
            if ((q2 == ns) || (q2 >= end) || (*q2 != '}'))
            {
                /* 空名 / 未闭合：把 '$' 当普通字符拷出（容错） */
                Scl_MsgErr("${}: 语法错误");
                if (di + 1u >= cap) { return -2; }
                dst[di++] = '$';
                p++;
                continue;
            }
            /* 取名字段到局部缓冲 */
            {
                char  nb[SCL_CFG_VAR_NAME_MAX + 1u];
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
            p = q2 + 1u;   /* 跳过 '}' */
            continue;
        }

        if (di + 1u >= cap)
        {
            return -2;   /* 超长 */
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

/* ========================== 实参解析 ========================== */

/* 追加一个参数到 s_argb（内部，argv 指针复用同一缓冲区） */
static int Scl_ArgAdd(const char *b, const char *e, int ai)
{
    /* 去两端空白 */
    while ((b < e) && Scl_IsSp(*b)) { b++; }
    while ((e > b) && Scl_IsSp(*(e - 1))) { e--; }
    /* 两端同类型引号剥除（支持 " 与 '） */
    if ((e - b >= 2) && ((*b == '"') || (*b == '\'')) && (*(e - 1) == *b))
    {
        b++;
        e--;
    }
    if (ai >= (int)SCL_CFG_ARG_MAX)
    {
        return -3;   /* 参数过多 */
    }
    {
        int r = Scl_ExpandCopy(b, e, s_argb[ai], SCL_CFG_ARG_LEN_MAX);
        if (r < 0)
        {
            return r;
        }
    }
    s_argv[ai] = s_argb[ai];
    return 0;
}

/* 普通式实参解析：'cmd a b ...'（空白分隔，支持 " 与 ' 引号字符串） */
static int Scl_ArgsNorm(const char *cs, const char *ce, int *out_argc)
{
    const char *p = cs + Scl_HeadLen(cs, ce);
    int ai = 0;

    while (1)
    {
        const char *ab;
        const char *ae;
        int r;

        p = Scl_SkipSp(p, ce);
        if (p >= ce)
        {
            break;
        }
        if ((*p == '"') || (*p == '\''))
        {
            const char *cb;
            const char *cc;
            if (!Scl_TakeQuoted(&p, ce, &cb, &cc))
            {
                return -4;   /* 引号未闭合 */
            }
            ab = cb;
            ae = cc;
        }
        else
        {
            ab = p;
            while ((p < ce) && !Scl_IsSp(*p)) { p++; }
            ae = p;
        }
        r = Scl_ArgAdd(ab, ae, ai);
        if (r < 0)
        {
            return r;
        }
        ai++;
    }
    *out_argc = ai;
    return 0;
}

/* 函数式实参解析：'cmd_xxx(a,b,...)'，逗号分隔（引号栈保护） */
static int Scl_ArgsFunc(const char *cs, const char *ce, int *out_argc)
{
    const char *p = cs + Scl_HeadLen(cs, ce);  /* p 指向 '(' */
    const char *cl;                            /* 匹配 ')' */
    const char *cur;
    scl_qs_t qs;
    int ai = 0;

    if ((p >= ce) || (*p != '('))
    {
        return -5;
    }
    p++;
    /* 找匹配 ')'（引号栈保护；不支持括号嵌套） */
    cl = p;
    Scl_QS_Init(&qs);
    while (cl < ce)
    {
        Scl_QS_Char(&qs, *cl);
        if ((*cl == ')') && (!Scl_QS_Open(&qs))) { break; }
        cl++;
    }
    if (cl >= ce)
    {
        return -4;   /* 缺 ')' */
    }
    /* ')' 后应只有空白 */
    {
        const char *tail = Scl_SkipSp(cl + 1, ce);
        if (tail < ce)
        {
            return -5;   /* ')' 后仍有内容 */
        }
    }

    cur = p;
    while (cur < cl)
    {
        const char *ab;
        const char *ae;
        int r;

        cur = Scl_SkipSp(cur, cl);
        if (cur >= cl)
        {
            break;   /* 空参数段忽略 */
        }
        ab = cur;
        Scl_QS_Init(&qs);
        while (cur < cl)
        {
            Scl_QS_Char(&qs, *cur);
            if ((*cur == ',') && (!Scl_QS_Open(&qs))) { break; }
            cur++;
        }
        ae = cur;
        if (cur < cl)
        {
            cur++;   /* 跳过 ',' */
        }
        r = Scl_ArgAdd(ab, ae, ai);
        if (r < 0)
        {
            return r;
        }
        ai++;
    }
    *out_argc = ai;
    return 0;
}

/* ========================== 变量相关内置命令 ========================== */

/* 变量列表 + 剩余空位 */
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

/* 'var' 内置：var name=value / var（列表）/ var name（查单个）/ var free（全释放） */
static void Scl_DoVar(const char *cs, const char *ce, const char *nx)
{
    const char *p = cs + Scl_HeadLen(cs, ce);
    const char *eq = NULL;
    const char *q;
    int has_eq = 0;
    scl_qs_t qs;

    p = Scl_SkipSp(p, ce);

    /* 找顶层 '='（引号栈保护） */
    q = p;
    Scl_QS_Init(&qs);
    while (q < ce)
    {
        Scl_QS_Char(&qs, *q);
        if ((*q == '=') && (!Scl_QS_Open(&qs))) { has_eq = 1; eq = q; break; }
        q++;
    }

    if (has_eq != 0)
    {
        /* var name=value */
        const char *nb = p;
        const char *ne = eq;
        const char *vb = eq + 1;
        const char *ve = ce;
        char namebuf[SCL_CFG_VAR_NAME_MAX + 1u];
        char valbuf[SCL_CFG_VAR_VALUE_MAX];
        uint16_t i;
        int r;

        while ((nb < ne) && Scl_IsSp(*nb)) { nb++; }
        ne = Scl_TrimEnd(nb, ne);
        vb = Scl_SkipSp(vb, ve);
        ve = Scl_TrimEnd(vb, ve);
        if ((ve - vb >= 2) && ((*vb == '"') || (*vb == '\'')) && (*(ve - 1) == *vb))
        {
            vb++;
            ve--;
        }
        /* 名字长度校验 */
        if ((uint16_t)(ne - nb) > SCL_CFG_VAR_NAME_MAX)
        {
            Scl_MsgErr("var: 变量名过长");
            s_cur = nx;
            return;
        }
        for (i = 0u; i < (uint16_t)(ne - nb); i++) { namebuf[i] = nb[i]; }
        namebuf[(uint16_t)(ne - nb)] = '\0';
        /* 值做 ${} 展开（如 var a=${b}） */
        {
            int er = Scl_ExpandCopy(vb, ve, valbuf, SCL_CFG_VAR_VALUE_MAX);
            if (er < 0)
            {
                Scl_MsgErr("var: 值无效");
                s_cur = nx;
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
    }
    else
    {
        /* 无 '=' → 命令形式 */
        const char *t = Scl_SkipSp(p, ce);
        if (t >= ce)
        {
            Scl_VarList();   /* var */
        }
        else if ((uint16_t)(ce - t) >= 4u && Scl_EqN(t, "free", 4u))
        {
            /* 'var free' → 全释放 */
            const char *tail = Scl_SkipSp(t + 4, ce);
            if (tail >= ce)
            {
                int n = SCL_VarFreeAll();
                Scl_Msg("scl: 释放全部 %d 个变量（剩余空位 %d）\r\n",
                        n, SCL_VarFreeCount());
            }
            else
            {
                Scl_MsgErr("var: 无法识别的子命令");
            }
        }
        else
        {
            /* 'var name' → 查单个 */
            const char *te = t;
            const char *vn;
            while ((te < ce) && !Scl_IsSp(*te)) { te++; }
            /* 取名字段到局部缓冲 */
            {
                char namebuf[SCL_CFG_VAR_NAME_MAX + 1u];
                uint16_t i;
                uint16_t nn = (uint16_t)(te - t);
                if (nn > SCL_CFG_VAR_NAME_MAX) { nn = SCL_CFG_VAR_NAME_MAX; }
                for (i = 0u; i < nn; i++) { namebuf[i] = t[i]; }
                namebuf[nn] = '\0';
                vn = SCL_VarGet(namebuf);
                if (vn == NULL)
                {
                    Scl_MsgErr("var: %s 未定义", namebuf);
                }
                else
                {
                    Scl_Msg("scl: var %s = %s\r\n", namebuf, vn);
                }
            }
        }
    }
    s_cur = nx;
}

/* 'free' 内置：free [name]（无参=全释放） */
static void Scl_DoFree(const char *cs, const char *ce, const char *nx)
{
    const char *p = cs + Scl_HeadLen(cs, ce);
    const char *t;

    p = Scl_SkipSp(p, ce);
    t = p;
    if (t >= ce)
    {
        int n = SCL_VarFreeAll();
        Scl_Msg("scl: free 全部 %d 个变量（剩余空位 %d）\r\n",
                n, SCL_VarFreeCount());
    }
    else
    {
        const char *te = t;
        char namebuf[SCL_CFG_VAR_NAME_MAX + 1u];
        uint16_t i;
        uint16_t nn;
        int r;

        while ((te < ce) && !Scl_IsSp(*te)) { te++; }
        nn = (uint16_t)(te - t);
        if (nn > SCL_CFG_VAR_NAME_MAX) { nn = SCL_CFG_VAR_NAME_MAX; }
        for (i = 0u; i < nn; i++) { namebuf[i] = t[i]; }
        namebuf[nn] = '\0';
        r = SCL_VarFree(namebuf);
        if (r == 0)
        {
            Scl_Msg("scl: free %s（剩余空位 %d）\r\n", namebuf, SCL_VarFreeCount());
        }
        else
        {
            Scl_MsgErr("free: %s 不存在", namebuf);
        }
    }
    s_cur = nx;
}

/* 'help' 内置：列出保留命令与已注册命令 */
static void Scl_DoHelp(void)
{
    scl_cmd_t *node;
    Scl_Msg("scl: 内置: var / free / if / while / help\r\n");
    Scl_Msg("scl: 已注册命令:\r\n");
    for (node = s_cmd_head; node != NULL; node = node->next)
    {
        Scl_Msg("  %s%s\r\n", node->name, (node->sync != NULL) ? " (异步)" : "");
    }
}

/* ========================== 结束/清理 ========================== */

/* 脚本结束统一清理：自动释放全部变量、清 G_RETURN、清执行状态 */
static void Scl_Finish(int reason)
{
    s_busy      = 0u;
    s_wait_cmd  = NULL;
    s_frame_cnt = 0u;
    s_cur       = NULL;
    s_prog_end  = NULL;
    s_abort     = 0u;
    SCL_VarFreeAll();
    s_ret       = 0u;
    if (reason == 0)
    {
        Scl_Msg("[scl done]\r\n");
    }
    else
    {
        Scl_Msg("[scl abort]\r\n");
    }
}

/* 当前区域扫描界：帧栈顶帧 end，无帧时为程序结尾 */
static const char *Scl_RegionBnd(void)
{
    if (s_frame_cnt > 0u)
    {
        return s_frames[s_frame_cnt - 1u].end;
    }
    return s_prog_end;
}

/* ========================== 流程控制：if ========================== */

/**
  * @brief  解析 if 子句并执行分支
  * @note   'if [-t 分支] [-f 分支]'，分支可为双引号多指令子链或单条命令；
  *         G_RETURN 真走 -t、假走 -f；读取即清零
  */
static void Scl_DoIf(const char *cs, const char *ce, const char *nx)
{
    const char *p = cs + Scl_HeadLen(cs, ce);
    const char *t_b = NULL, *t_e = NULL;
    const char *f_b = NULL, *f_e = NULL;
    int ret;

    while (1)
    {
        p = Scl_SkipSp(p, ce);
        if (p >= ce)
        {
            break;
        }
        if (*p == '-')
        {
            p++;
            if ((p < ce) && ((*p == 't') || (*p == 'f')))
            {
                char which = *p;
                const char *vb;
                const char *ve;
                p++;
                p = Scl_SkipSp(p, ce);
                if (p >= ce)
                {
                    Scl_MsgErr("if: -%c 缺少分支", which);
                    Scl_Finish(1);
                    return;
                }
                if ((*p == '"') || (*p == '\''))
                {
                    const char *cb;
                    const char *cc;
                    if (!Scl_TakeQuoted(&p, ce, &cb, &cc))
                    {
                        Scl_MsgErr("if: 引号未闭合");
                        Scl_Finish(1);
                        return;
                    }
                    vb = cb;
                    ve = cc;
                }
                else
                {
                    vb = p;
                    while ((p < ce) && !Scl_IsSp(*p)) { p++; }
                    ve = p;
                }
                if (which == 't') { t_b = vb; t_e = ve; }
                else              { f_b = vb; f_e = ve; }
                continue;
            }
            Scl_MsgErr("if: 未知选项");
            Scl_Finish(1);
            return;
        }
        Scl_MsgErr("if: 多余内容");
        Scl_Finish(1);
        return;
    }

    /* 读 G_RETURN（读后清零） */
    ret = Scl_RetTake();

    /* 选定分支 */
    {
        const char *bb = NULL;
        const char *be = NULL;
        if (ret != 0)
        {
            bb = t_b;
            be = t_e;
        }
        else
        {
            bb = f_b;
            be = f_e;
        }
        if ((bb == NULL) || (bb >= be))
        {
            s_cur = nx;   /* 无分支或空分支：跳过 */
            return;
        }
        /* 压"段帧"执行分支子链 */
        if (s_frame_cnt >= (uint8_t)SCL_CFG_NEST_MAX)
        {
            Scl_MsgErr("if: 嵌套过深(>%d)", (int)SCL_CFG_NEST_MAX);
            Scl_Finish(1);
            return;
        }
        {
            scl_frame_t *f = &s_frames[s_frame_cnt];
            f->kind   = SCL_FR_SEG;
            f->body   = bb;
            f->end    = be;      /* 段文本结束（扫描界） */
            f->e_start= bb;
            f->resume = nx;      /* if 子句之后继续 */
            f->iter   = 0u;
            s_frame_cnt++;
        }
        s_cur = bb;
    }
}

/* ========================== 流程控制：while ========================== */

/**
  * @brief  从 pos 起在当前区域内扫描定位与 'while -b' 配对的 'while -e'
  * @retval 1=找到（*eb= -e 子句起点；*en= -e 子句之后的扫描点）；0=未找到
  */
static int Scl_FindWhileEnd(const char *pos, const char *bnd,
                            const char **eb, const char **en)
{
    const char *q = pos;
    uint32_t depth = 0u;

    while (1)
    {
        const char *st, *ce2, *nx2;
        if (!Scl_FindClause(q, bnd, &st, &ce2, &nx2))
        {
            return 0;   /* 区域到头仍未找到 */
        }
        if (st < ce2)
        {
            if (Scl_HeadIs(st, ce2, "while"))
            {
                const char *sp = st + 5u;   /* 跳过 "while" */
                sp = Scl_SkipSp(sp, ce2);
                if ((sp + 1 < ce2) && (sp[0] == '-') && ((sp[1] == 'b') || (sp[1] == 'e')))
                {
                    if (sp[1] == 'b')
                    {
                        depth++;
                    }
                    else
                    {
                        if (depth == 0u)
                        {
                            *eb = st;
                            *en = nx2;
                            return 1;
                        }
                        depth--;
                    }
                }
            }
        }
        q = nx2;
    }
}

/**
  * @brief  处理 while 子句：-b 开始（压循环帧）；-e 判定退出（读 G_RETURN）
  * @note   语义（已确认）：G_RETURN==false 退出；==true 回跳 body（do-while）
  */
static void Scl_DoWhile(const char *cs, const char *ce, const char *nx)
{
    const char *p = cs + 5u;   /* 跳过 "while" */

    p = Scl_SkipSp(p, ce);
    if ((p + 1 >= ce) || (p[0] != '-') || ((p[1] != 'b') && (p[1] != 'e')))
    {
        Scl_MsgErr("while: 需要 -b 或 -e");
        s_cur = nx;
        return;
    }

    if (p[1] == 'b')
    {
        /* ----- while -b：先查嵌套深度，再定位配对 -e 并压循环帧 ----- */
        const char *eb;
        const char *en;
        const char *bnd = Scl_RegionBnd();

        if (s_frame_cnt >= (uint8_t)SCL_CFG_NEST_MAX)
        {
            Scl_MsgErr("while: 嵌套过深(>%d)", (int)SCL_CFG_NEST_MAX);
            Scl_Finish(1);
            return;
        }
        if (!Scl_FindWhileEnd(nx, bnd, &eb, &en))
        {
            Scl_MsgErr("while -b: 未找到匹配的 while -e");
            Scl_Finish(1);
            return;
        }
        {
            scl_frame_t *f = &s_frames[s_frame_cnt];
            f->kind    = SCL_FR_WHILE;
            f->body    = nx;       /* 迭代体起点（-b 子句之后） */
            f->end     = en;       /* -e 子句结束点 = 退出续点 */
            f->e_start = eb;       /* 匹配 -e 子句起点 */
            f->resume  = en;
            f->iter    = 0u;
            s_frame_cnt++;
        }
        s_cur = nx;
    }
    else
    {
        /* ----- while -e：判定退出 ----- */
        scl_frame_t *f;
        uint32_t cap = 0u;
        uint32_t limit;
        const char *q;
        int exit = 0;

        if ((s_frame_cnt == 0u) || (s_frames[s_frame_cnt - 1u].kind != SCL_FR_WHILE))
        {
            Scl_MsgErr("while -e: 没有匹配的 while -b");
            Scl_Finish(1);
            return;
        }
        f = &s_frames[s_frame_cnt - 1u];
        if (f->e_start != cs)
        {
            /* 防御：位置不匹配（理论上不应出现） */
            Scl_MsgErr("while -e: 结构错位");
            Scl_Finish(1);
            return;
        }

        /* 可选迭代上限：'-e N' */
        q = p + 2u;   /* 跳过 "-e" */
        q = Scl_SkipSp(q, ce);
        if (q < ce)
        {
            /* 解析十进制数字 */
            cap = 0u;
            while ((q < ce) && (*q >= '0') && (*q <= '9'))
            {
                cap = cap * 10u + (uint32_t)(*q - '0');
                if (cap > SCL_CFG_WHILE_MAX) { cap = SCL_CFG_WHILE_MAX; }
                q++;
            }
            if ((q < ce) && !Scl_IsSp(*q))
            {
                Scl_MsgErr("while -e: 上限须为数字");
            }
        }

        /* 读 G_RETURN（读后清零） */
        f->iter++;
        limit = (cap > 0u) ? cap : (uint32_t)SCL_CFG_WHILE_MAX;
        if (Scl_RetTake() == 0)
        {
            exit = 1;   /* G_RETURN==false → 退出 */
        }
        else if (f->iter >= limit)
        {
            exit = 1;   /* 达到显式上限 / 兜底上限 → 退出 */
            if (cap == 0u)
            {
                Scl_MsgErr("while: 达到最大迭代 %u，强制退出", (unsigned int)SCL_CFG_WHILE_MAX);
            }
        }

        if (exit != 0)
        {
            s_cur = f->resume;
            s_frame_cnt--;
        }
        else
        {
            s_cur = f->body;   /* 回跳继续（do-while） */
        }
    }
}

/* ========================== 业务命令分发 ========================== */

/* 校验帧类型合法性：帧顶 WHILE 且 s_cur 到帧的 -e（防御）—— 用于步进调度 */
static void Scl_DispatchCmd(const char *cs, const char *ce, const char *nx)
{
    uint16_t hl = Scl_HeadLen(cs, ce);
    const char *after = cs + hl;
    int is_func = ((after < ce) && (*after == '(')) ? 1 : 0;
    int argc = 0;
    int r;
    scl_cmd_t *nd;

    /* 解析实参 */
    if (is_func != 0)
    {
        r = Scl_ArgsFunc(cs, ce, &argc);
    }
    else
    {
        r = Scl_ArgsNorm(cs, ce, &argc);
    }
    if (r < 0)
    {
        Scl_MsgErr("命令参数错误(%d)", r);
        s_cur = nx;
        return;
    }

    /* 查命令 */
    nd = Scl_CmdFindN(cs, hl);
    if (nd == NULL)
    {
        char nbuf[24u];
        uint16_t i;
        uint16_t nn = (hl < 23u) ? hl : 23u;
        for (i = 0u; i < nn; i++) { nbuf[i] = cs[i]; }
        nbuf[nn] = '\0';
        Scl_MsgErr("未知命令 '%s'", nbuf);
        s_cur = nx;
        return;
    }

    /* 异步命令先登记等待节点，再调用 handler 发起操作 */
    if (nd->sync != NULL)
    {
        s_wait_cmd = nd;
    }
    nd->fn(argc, s_argv);

    /* 子句已消费，游标前移（异步命令完成前不继续下一条） */
    s_cur = nx;
}

/* ========================== 步进执行 ========================== */

/* 每周期推进一个动作：执行一条子句，或结束一个已到界的段帧 */
static void Scl_StepOnce(void)
{
    const char *bnd = Scl_RegionBnd();
    const char *cs, *ce, *nx;

    if (!Scl_FindClause(s_cur, bnd, &cs, &ce, &nx))
    {
        /* 当前区域结束 */
        if (s_frame_cnt > 0u)
        {
            scl_frame_t *f = &s_frames[s_frame_cnt - 1u];
            if (f->kind == SCL_FR_WHILE)
            {
                /* 防御：循环体异常（-e 缺失应由 -b 扫描时发现） */
                Scl_MsgErr("while: 循环结构异常");
                Scl_Finish(1);
                return;
            }
            s_cur = f->resume;   /* 段帧（if 分支）结束，回到外层续点 */
            s_frame_cnt--;
        }
        else
        {
            /* 顶层结束 → 自然完成（自动释放变量） */
            Scl_Finish(0);
        }
        return;
    }

    if (cs >= ce)
    {
        s_cur = nx;   /* 空子句（防御） */
        return;
    }

    /* 关键字分发 */
    if (Scl_HeadIs(cs, ce, "if"))
    {
        Scl_DoIf(cs, ce, nx);
    }
    else if (Scl_HeadIs(cs, ce, "while"))
    {
        Scl_DoWhile(cs, ce, nx);
    }
    else if (Scl_HeadIs(cs, ce, "var"))
    {
        Scl_DoVar(cs, ce, nx);
    }
    else if (Scl_HeadIs(cs, ce, "free"))
    {
        Scl_DoFree(cs, ce, nx);
    }
    else if (Scl_HeadIs(cs, ce, "help"))
    {
        Scl_DoHelp();
        s_cur = nx;
    }
    else
    {
        Scl_DispatchCmd(cs, ce, nx);
    }
}

/* ========================== 公共接口实现 ========================== */

void SCL_Init(void)
{
    s_cmd_head  = NULL;
    s_busy      = 0u;
    s_abort     = 0u;
    s_wait_cmd  = NULL;
    s_frame_cnt = 0u;
    s_cur       = NULL;
    s_prog_end  = NULL;
    s_ret       = 0u;
    SCL_VarFreeAll();
    s_inited    = 1u;
}

uint8_t SCL_Run(const char *script)
{
    uint16_t len;
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

    /* 拷贝进内部程序缓冲 */
    len = 0u;
    while ((script[len] != '\0') && (len + 1u < SCL_CFG_SCRIPT_MAX))
    {
        len++;
    }
    if (script[len] != '\0')
    {
        Scl_MsgErr("脚本过长(>%d)", (int)(SCL_CFG_SCRIPT_MAX - 1u));
        return 0u;
    }
    if (len == 0u)
    {
        return 0u;   /* 空脚本 */
    }
    for (i = 0u; i < len; i++)
    {
        s_prog[i] = script[i];
    }
    s_prog[len] = '\0';

    /* 启动执行 */
    s_prog_end  = s_prog + len;
    s_cur       = s_prog;
    s_frame_cnt = 0u;
    s_wait_cmd  = NULL;
    s_abort     = 0u;
    s_ret       = 0u;      /* 脚本开始清一次（防历史残留） */
    s_busy      = 1u;
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
                if (s_wait_cmd->sync != NULL)
                {
                    s_wait_cmd->sync(true);   /* 清除标志 */
                }
                s_wait_cmd = NULL;
            }
        }
        else
        {
            s_wait_cmd = NULL;   /* 防御 */
        }
        return;
    }

    /* 推进一个动作 */
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
