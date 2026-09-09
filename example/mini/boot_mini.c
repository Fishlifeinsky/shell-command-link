/* ===============================================================
 * SCL mini 自包含状态机程序：boot（由 tools/scl_mini_c.py / scl_emit_c.py --mini 生成，勿手改）
 * 来源：example/mcu_template/boot.s2c (现代语法 .s2c)
 * 用法：SCL_Init(); <注册所需命令>; boot_mini_start();
 *       然后周期调用 boot_mini_step()（每次一个动作，非阻塞）；返回 0 表示完成。
 * 特性：脚本变量为 static 文本缓冲；用户可见变量写入时 SCL_VarSetT 镜像，
 *       外部可用 SCL_VarGet 读取（观感与解释器一致）。
 * 裁剪：本文件自包含步进，不再需要文本编译器/字节码解释器/label 表。
 * ===============================================================
 */
#include "scl.h"

#define MINI_VLEN  16u          /* 变量文本缓冲（含 '\0'） */
#define MINI_VN    3u          /* 脚本变量个数 */
#define MINI_NST   20u          /* 状态数（超界=完成） */
#define MINI_STEP_LIMIT 1000000u
/* 静态助手可能在某个程序里用不到：抑制未用告警（GCC/Clang；其它编译器为空） */
#if defined(__GNUC__) || defined(__clang__)
#define MINI_UNUSED __attribute__((unused))
#else
#define MINI_UNUSED
#endif

/* ---- 脚本变量（static；用户可见者写入即镜像到 SCL） ---- */
typedef struct { const char *name; char *txt; uint8_t *def; uint8_t tag; } mini_vref_t;
/* tag: bit0=const 只读；bit1=内部临时变量（不镜像） */
static char m_r[MINI_VLEN];
static uint8_t m_r_def;
static char m_i[MINI_VLEN];
static uint8_t m_i_def;
static char m_sum[MINI_VLEN];
static uint8_t m_sum_def;
static mini_vref_t s_vs[MINI_VN] = {
    { "r", m_r, &m_r_def, 0x00 },
    { "i", m_i, &m_i_def, 0x00 },
    { "sum", m_sum, &m_sum_def, 0x00 },
};

/* ---- 运行状态 ---- */
static uint16_t s_st;      /* 当前状态 */
static uint32_t s_steps;   /* 已推进动作数（步限保护） */
static uint8_t  s_wait;    /* 异步命令等待中 */
static uint16_t s_pend;    /* 异步完成后进入的状态 */
static uint8_t  s_fault;   /* 运行错误（如除零/写 const） */
static uint16_t s_retst;   /* callf 返回点（单层子程序） */

/* ---- 无 libc 小助手 ---- */
static MINI_UNUSED unsigned Mini_Len(const char *s)
{
    unsigned n = 0u;
    while (s[n] != '\0') { n++; }
    return n;
}

static MINI_UNUSED uint8_t Mini_Eq(const char *a, const char *b)
{
    unsigned i;
    for (i = 0u; a[i] != '\0' && b[i] != '\0'; i++)
    {
        if (a[i] != b[i]) { return 0u; }
    }
    return (a[i] == b[i]) ? 1u : 0u;
}

static MINI_UNUSED void Mini_Copy(char *dst, const char *src, unsigned cap)
{
    unsigned i = 0u;
    while ((i + 1u < cap) && (src[i] != '\0')) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static MINI_UNUSED int32_t Mini_Num(const char *s, int *ok)
{
    int32_t v = 0;
    int neg = 0;
    int base = 10;
    int i = 0;
    *ok = 1;
    if (s == NULL) { *ok = 0; return 0; }
    if (s[i] == '-') { neg = 1; i++; }
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
    else if (s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) { base = 2; i += 2; }
    if (s[i] == '\0') { *ok = 0; return 0; }
    while (s[i] != '\0')
    {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9') { d = c - '0'; }
        else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
        else { *ok = 0; return 0; }
        if (d >= base) { *ok = 0; return 0; }
        if (v > (0x7FFFFFFF - d) / base) { *ok = 0; return 0; }
        v = v * base + d;
        i++;
    }
    return neg ? -v : v;
}

static MINI_UNUSED void Mini_Fmt(int32_t val, char *dst, unsigned cap)
{
    char t[12];
    unsigned k = 0u;
    uint32_t u;
    unsigned i;
    unsigned off;
    if (val < 0) { off = 1u; u = (uint32_t)(-(val + 1)) + 1u; }
    else { off = 0u; u = (uint32_t)val; }
    if (u == 0u) { t[k++] = '0'; }
    while (u != 0u) { t[k++] = (char)('0' + (u % 10u)); u /= 10u; }
    if (off != 0u && off < cap) { dst[0] = '-'; }
    for (i = 0u; i < k && off + i + 1u < cap; i++)
    {
        dst[off + i] = t[k - 1u - i];
    }
    dst[(off + (k < cap - off ? k : cap - 1u - off))] = '\0';
}

static MINI_UNUSED int Mini_VFind(const char *name)
{
    unsigned i;
    for (i = 0u; i < MINI_VN; i++)
    {
        if (s_vs[i].name != NULL && Mini_Eq(s_vs[i].name, name)) { return (int)i; }
    }
    return -1;
}

static MINI_UNUSED int Mini_Set(const char *name, uint8_t type, const char *txt, uint8_t isconst)
{
    int i = Mini_VFind(name);
    if (i < 0) { return -1; }
    if ((s_vs[i].tag & 0x01u) != 0u && isconst == 0u) { return -1; }
    Mini_Copy(s_vs[i].txt, txt, MINI_VLEN);
    *s_vs[i].def = 1u;
    if ((s_vs[i].tag & 0x02u) == 0u)
    {
        if (isconst != 0u) { (void)SCL_VarSetConst(name, type, txt); }
        else { (void)SCL_VarSetT(name, type, txt); }
    }
    return 0;
}

static MINI_UNUSED void Mini_Free(const char *name)
{
    int i = Mini_VFind(name);
    if (i < 0) { return; }
    if ((s_vs[i].tag & 0x01u) != 0u) { return; }
    s_vs[i].txt[0] = '\0';
    *s_vs[i].def = 0u;
    if ((s_vs[i].tag & 0x02u) == 0u) { (void)SCL_VarFree(name); }
}

static MINI_UNUSED int32_t Mini_NumTok(const char *tok, int *ok)
{
    int i;
    const char *v;
    *ok = 1;
    i = Mini_VFind(tok);
    if (i >= 0)
    {
        if (*s_vs[i].def == 0u) { *ok = 0; return 0; }
        return Mini_Num(s_vs[i].txt, ok);
    }
    if (Mini_Eq(tok, "true")) { return 1; }
    if (Mini_Eq(tok, "false")) { return 0; }
    if (tok[0] == '$' && tok[1] == '{')
    {
        const char *nm = tok + 2;
        unsigned len = Mini_Len(nm);
        char nb[24]; unsigned k;
        if (len == 0u || nm[len - 1u] != '}') { *ok = 0; return 0; }
        for (k = 0u; k + 1u < len && k + 1u < sizeof(nb); k++) { nb[k] = nm[k]; }
        nb[k] = '\0';
        v = SCL_VarGet(nb);
        if (v == NULL) { *ok = 0; return 0; }
        if (Mini_Eq(v, "true")) { return 1; }
        if (Mini_Eq(v, "false")) { return 0; }
        return Mini_Num(v, ok);
    }
    v = SCL_VarGet(tok);
    if (v != NULL)
    {
        if (Mini_Eq(v, "true")) { return 1; }
        if (Mini_Eq(v, "false")) { return 0; }
        return Mini_Num(v, ok);
    }
    return Mini_Num(tok, ok);
}

static MINI_UNUSED uint8_t Mini_TruthTok(const char *tok)
{
    int i = Mini_VFind(tok);
    const char *v;
    int ok;
    if (i >= 0)
    {
        if (*s_vs[i].def == 0u) { return 0u; }
        v = s_vs[i].txt;
    }
    else if (tok[0] == '$' && tok[1] == '{')
    {
        const char *nm = tok + 2;
        unsigned len = Mini_Len(nm);
        char nb[24]; unsigned k;
        if (len == 0u || nm[len - 1u] != '}') { return 0u; }
        for (k = 0u; k + 1u < len && k + 1u < sizeof(nb); k++) { nb[k] = nm[k]; }
        nb[k] = '\0';
        v = SCL_VarGet(nb);
        if (v == NULL) { return 0u; }
    }
    else
    {
        v = SCL_VarGet(tok);
        if (v == NULL) { v = tok; }
    }
    if (Mini_Eq(v, "true")) { return 1u; }
    if (Mini_Eq(v, "false")) { return 0u; }
    ok = 0;
    if (v[0] != '\0' && Mini_Num(v, &ok) != 0 && ok) { return 1u; }
    if (ok) { return 0u; }
    if (v[0] == '\0') { return 0u; }
    if (Mini_Eq(v, "0")) { return 0u; }
    return 1u;
}

static MINI_UNUSED const char *Mini_TokText(const char *tok)
{
    int i = Mini_VFind(tok);
    const char *v;
    if (i >= 0) { return (*s_vs[i].def != 0u) ? s_vs[i].txt : ""; }
    if (tok[0] == '$' && tok[1] == '{')
    {
        const char *nm = tok + 2;
        unsigned len = Mini_Len(nm);
        static char nb[24]; unsigned k;
        if (len == 0u || nm[len - 1u] != '}') { return ""; }
        for (k = 0u; k + 1u < len && k + 1u < sizeof(nb); k++) { nb[k] = nm[k]; }
        nb[k] = '\0';
        v = SCL_VarGet(nb);
        return (v != NULL) ? v : "";
    }
    v = SCL_VarGet(tok);
    return (v != NULL) ? v : tok;
}

static MINI_UNUSED void Mini_Exp(const char *tpl, char *dst, unsigned cap)
{
    unsigned di = 0u;
    unsigned i = 0u;
    while (tpl[i] != '\0')
    {
        if (tpl[i] == '$' && tpl[i + 1] == '{')
        {
            unsigned s2 = i + 2u;
            unsigned e2 = s2;
            const char *val;
            while (tpl[e2] != '\0' && tpl[e2] != '}') { e2++; }
            if (tpl[e2] == '}')
            {
                char nb[24];
                unsigned k = 0u;
                unsigned j;
                while (s2 + k < e2 && k + 1u < sizeof(nb)) { nb[k] = tpl[s2 + k]; k++; }
                nb[k] = '\0';
                j = (unsigned)Mini_VFind(nb);
                val = (j < MINI_VN && *s_vs[j].def != 0u) ? s_vs[j].txt : SCL_VarGet(nb);
                if (val != NULL)
                {
                    while (*val != '\0' && di + 1u < cap) { dst[di++] = *val++; }
                }
                i = e2 + 1u;
                continue;
            }
        }
        if (di + 1u < cap) { dst[di++] = tpl[i]; }
        i++;
    }
    if (di < cap) { dst[di] = '\0'; }
}

/* ---- step：每次调用执行一个动作（非阻塞） ---- */
uint8_t boot_mini_step(void)
{
    if (s_wait != 0u)
    {
        int p = SCL_AsyncPoll();
        if (p == 0) { return 1u; }
        s_wait = 0u;
        s_st = s_pend;
    }
    if (s_st >= MINI_NST) { return 0u; }
    if (s_steps++ >= MINI_STEP_LIMIT) { s_fault = 1u; goto mini_done; }
    switch (s_st)
    {
    case 0:
    {
        if (Mini_Set("r", SCL_T_INT, "0", 0u) != 0) { s_fault = 1u; goto mini_done; }
        s_st = 1u;
        break;
    }
    case 1:
    {
        if (Mini_Set("i", SCL_T_INT, "0", 0u) != 0) { s_fault = 1u; goto mini_done; }
        s_st = 2u;
        break;
    }
    case 2:
    {
        if (Mini_Set("sum", SCL_T_INT, "0", 0u) != 0) { s_fault = 1u; goto mini_done; }
        s_st = 3u;
        break;
    }
    case 3:
    {
        { int ok;
            int32_t a = Mini_NumTok("5", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            int32_t b = Mini_NumTok("1", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            char tb[MINI_VLEN];
            Mini_Fmt(a + b, tb, sizeof(tb));
            if (Mini_Set("r", SCL_T_INT, tb, 0u) != 0) { s_fault = 1u; goto mini_done; }
        }
        s_st = 4u;
        break;
    }
    case 4:
    {
        { int ok;
            int32_t a = Mini_NumTok("r", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            int32_t b = Mini_NumTok("6", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            SCL_Ret_Set((a == b) ? 1 : 0);
        }
        s_st = 5u;
        break;
    }
    case 5:
    {
        if (SCL_Ret_Get() != 0)
        { SCL_Ret_Set(0); s_st = 8u; }
        else
        { SCL_Ret_Set(0); s_st = 6u; }
        break;
    }
    case 6:
    {
        const char *av[1];
        scl_invoke_arg_t ia[1];
        av[0] = "boot-const-fail";
        ia[0].text = av[0];
        ia[0].type = SCL_T_STR;
        {
            uint8_t r = SCL_CmdInvoke("echo", 1, ia);
            if (r == 0u) { s_fault = 1u; goto mini_done; }
            if (r == 2u) { s_wait = 1u; s_pend = 7u; }
            else { s_st = 7u; }
        }
        break;
    }
    case 7:
    {
        s_st = 9u;
        break;
    }
    case 8:
    {
        const char *av[1];
        scl_invoke_arg_t ia[1];
        av[0] = "boot-const-ok";
        ia[0].text = av[0];
        ia[0].type = SCL_T_STR;
        {
            uint8_t r = SCL_CmdInvoke("echo", 1, ia);
            if (r == 0u) { s_fault = 1u; goto mini_done; }
            if (r == 2u) { s_wait = 1u; s_pend = 9u; }
            else { s_st = 9u; }
        }
        break;
    }
    case 9:
    {
        { int ok;
            int32_t a = Mini_NumTok("i", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            int32_t b = Mini_NumTok("3", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            SCL_Ret_Set((a < b) ? 1 : 0);
        }
        s_st = 10u;
        break;
    }
    case 10:
    {
        if (SCL_Ret_Get() != 0)
        { SCL_Ret_Set(0); s_st = 12u; }
        else
        { SCL_Ret_Set(0); s_st = 11u; }
        break;
    }
    case 11:
    {
        s_st = 15u;
        break;
    }
    case 12:
    {
        { int ok;
            int32_t a = Mini_NumTok("sum", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            int32_t b = Mini_NumTok("i", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            char tb[MINI_VLEN];
            Mini_Fmt(a + b, tb, sizeof(tb));
            if (Mini_Set("sum", SCL_T_INT, tb, 0u) != 0) { s_fault = 1u; goto mini_done; }
        }
        s_st = 13u;
        break;
    }
    case 13:
    {
        { int ok;
            int32_t a = Mini_NumTok("i", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            int32_t b = Mini_NumTok("1", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            char tb[MINI_VLEN];
            Mini_Fmt(a + b, tb, sizeof(tb));
            if (Mini_Set("i", SCL_T_INT, tb, 0u) != 0) { s_fault = 1u; goto mini_done; }
        }
        s_st = 14u;
        break;
    }
    case 14:
    {
        s_st = 9u;
        break;
    }
    case 15:
    {
        { int ok;
            int32_t a = Mini_NumTok("sum", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            int32_t b = Mini_NumTok("3", &ok);
            if (!ok) { s_fault = 1u; goto mini_done; }
            SCL_Ret_Set((a == b) ? 1 : 0);
        }
        s_st = 16u;
        break;
    }
    case 16:
    {
        if (SCL_Ret_Get() != 0)
        { SCL_Ret_Set(0); s_st = 18u; }
        else
        { SCL_Ret_Set(0); s_st = 17u; }
        break;
    }
    case 17:
    {
        s_st = 19u;
        break;
    }
    case 18:
    {
        const char *av[1];
        scl_invoke_arg_t ia[1];
        av[0] = "boot-loop-ok";
        ia[0].text = av[0];
        ia[0].type = SCL_T_STR;
        {
            uint8_t r = SCL_CmdInvoke("echo", 1, ia);
            if (r == 0u) { s_fault = 1u; goto mini_done; }
            if (r == 2u) { s_wait = 1u; s_pend = 19u; }
            else { s_st = 19u; }
        }
        break;
    }
    case 19:
    {
        const char *av[1];
        scl_invoke_arg_t ia[1];
        av[0] = "boot-done";
        ia[0].text = av[0];
        ia[0].type = SCL_T_STR;
        {
            uint8_t r = SCL_CmdInvoke("echo", 1, ia);
            if (r == 0u) { s_fault = 1u; goto mini_done; }
            if (r == 2u) { s_wait = 1u; s_pend = 20u; }
            else { s_st = 20u; }
        }
        break;
    }
    default:
        s_st = MINI_NST;
        break;
    }
mini_done:
    if (s_fault != 0u)
    {
        s_fault = 0u;
        s_st = MINI_NST;
        return 0u;
    }
    return (s_st < MINI_NST) ? 1u : 0u;
}

void boot_mini_start(void)
{
    SCL_Ret_Set(0);
    s_st = 0u;
    s_steps = 0u;
    s_wait = 0u;
    s_pend = 0u;
    s_fault = 0u;
    s_retst = 0u;
}

uint8_t boot_mini_busy(void)
{
    return (s_st < MINI_NST) ? 1u : 0u;
}
