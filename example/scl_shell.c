/**
  ******************************************************************************
  * @file    scl_shell.c
  * @brief   SCL 交互式命令 Shell 实现（详见 scl_shell.h）
  *
  *          设计：面向串口的逐字节状态机 —— 收满一行后（空闲时）交 SCL_Run()，
  *          命令完成后打印提示符。全部静态内存，无 libc。
  ******************************************************************************
  */

#include "scl_shell.h"

#if (SCL_EX_SHELL_EN == 1u)

#include "scl.h"

/* ========================== 内部状态 ========================== */

static void (*s_out)(char) = NULL;      /* 输出回调 */

static char s_line[SCL_EX_SHELL_LINE_MAX];   /* 当前编辑行 */
static unsigned s_len = 0u;                  /* 行已用字节数（光标恒在尾部） */

static char s_edit[SCL_EX_SHELL_LINE_MAX];   /* 进入历史浏览前暂存的新行 */
static char s_hist[SCL_EX_SHELL_HIST_MAX][SCL_EX_SHELL_LINE_MAX];
static int  s_hist_cnt = 0;                  /* 历史条数 */
static int  s_hist_pos = -1;                 /* 当前浏览位置（-1=正在输入新行） */

static unsigned s_esc = 0u;                  /* CSI 状态机：0=无 1=ESC 2=ESC[ */
static unsigned s_was_busy = 0u;             /* 上次 Poll 时是否 busy（提示符沿触发） */
static unsigned s_quit = 0u;                 /* quit/exit 请求 */

static const char s_prompt[] = "scl> ";
static const char s_busy_msg[] = "[busy: 命令执行中，已忽略]\r\n";

/* ========================== 小工具 ========================== */

static unsigned Sh_StrLen(const char *s)
{
    unsigned n = 0u;
    while (s[n] != '\0') { n++; }
    return n;
}

static void Sh_Puts(const char *s)
{
    unsigned i;
    for (i = 0u; s[i] != '\0'; i++)
    {
        if (s_out != NULL) { s_out(s[i]); }
    }
}

static void Sh_Putc(char c)
{
    if (s_out != NULL) { s_out(c); }
}

static int Sh_IsSp(char c)
{
    return (c == ' ') || (c == '\t');
}

/* 重绘当前行（ANSI：回车+清行+提示+行） */
static void Sh_Redraw(void)
{
    unsigned i;
    Sh_Puts("\r\x1b[2K");
    Sh_Puts(s_prompt);
    for (i = 0u; i < s_len; i++)
    {
        Sh_Putc(s_line[i]);
    }
}

/* 回车换行 */
static void Sh_Newline(void)
{
    Sh_Puts("\r\n");
}

/* 把当前行压入历史（去重最近一条；满则丢最旧） */
static void Sh_HistPush(void)
{
    int i;
    if (s_len == 0u)
    {
        return;
    }
    /* 与最近一条相同则不重复记录 */
    if (s_hist_cnt > 0)
    {
        unsigned k;
        unsigned ok = (Sh_StrLen(s_hist[s_hist_cnt - 1]) == s_len);
        for (k = 0u; ok && (k < s_len); k++)
        {
            if (s_hist[s_hist_cnt - 1][k] != s_line[k]) { ok = 0u; }
        }
        if (ok) { return; }
    }
    if (s_hist_cnt < (int)SCL_EX_SHELL_HIST_MAX)
    {
        s_hist_cnt++;
    }
    else
    {
        /* 满：整体前移丢弃最旧 */
        for (i = 0; i < (int)SCL_EX_SHELL_HIST_MAX - 1; i++)
        {
            unsigned k;
            for (k = 0u; k < SCL_EX_SHELL_LINE_MAX; k++)
            {
                s_hist[i][k] = s_hist[i + 1][k];
            }
        }
    }
    {
        unsigned k;
        for (k = 0u; k < s_len; k++) { s_hist[s_hist_cnt - 1][k] = s_line[k]; }
        s_hist[s_hist_cnt - 1][s_len] = '\0';
    }
}

/* 把给定字符串装入编辑行 */
static void Sh_LineSet(const char *src)
{
    unsigned n = Sh_StrLen(src);
    unsigned k;
    if (n >= SCL_EX_SHELL_LINE_MAX) { n = SCL_EX_SHELL_LINE_MAX - 1u; }
    for (k = 0u; k < n; k++) { s_line[k] = src[k]; }
    s_len = n;
    s_line[n] = '\0';
    Sh_Redraw();
}

/* ========================== 历史浏览 ========================== */

static void Sh_HistUp(void)
{
    if (s_hist_cnt == 0)
    {
        return;
    }
    if (s_hist_pos < 0)
    {
        /* 保存当前编辑行 */
        unsigned k;
        for (k = 0u; k <= s_len; k++) { s_edit[k] = s_line[k]; }
        s_hist_pos = s_hist_cnt - 1;
    }
    else if (s_hist_pos > 0)
    {
        s_hist_pos--;
    }
    else
    {
        return;   /* 已在最旧 */
    }
    Sh_LineSet(s_hist[s_hist_pos]);
}

static void Sh_HistDown(void)
{
    if (s_hist_cnt == 0)
    {
        return;
    }
    if (s_hist_pos < 0)
    {
        return;   /* 已在编辑态 */
    }
    s_hist_pos++;
    if (s_hist_pos >= s_hist_cnt)
    {
        s_hist_pos = -1;
        Sh_LineSet(s_edit);
    }
    else
    {
        Sh_LineSet(s_hist[s_hist_pos]);
    }
}

/* ========================== 补全 ========================== */

/* 保留字/内置运算（与 scl.c 一致的可用输入） */
static const char *const s_keywords[] = {
    "help", "var", "free", "label", "jump",
    "iadd", "isub", "imul", "idiv", "imod", "ineg",
    "ieq", "ine", "igt", "ige", "ilt", "ile",
    "band", "bor", "bnot", "btest",
    "iand", "ior", "ixor", "inot", "shl", "shr", "seq", "sneq"
};

#if (SCL_CFG_CMDDESC_EN == 1u)
/* 字节串比较（含结尾 \0；供命令查找，shell 无 libc） */
static unsigned Sh_EqN(const char *a, const char *b, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; i++)
    {
        if (a[i] != b[i]) { return 0u; }
    }
    return 1u;
}

/* 在注册命令链表中按精确名找命令（无则 NULL） */
static const scl_cmd_t *Sh_FindCmd(const char *name)
{
    const scl_cmd_t *nd;
    for (nd = SCL_CmdHead(); nd != NULL; nd = nd->next)
    {
        if (Sh_EqN(nd->name, name, Sh_StrLen(name) + 1u)) { return nd; }
    }
    return NULL;
}

/* 类型常量 → 类型名（本地映射, 与 scl.c 一致） */
static const char *Sh_TypeName(uint8_t t)
{
    switch (t)
    {
    case SCL_T_BOOL: return "bool";
    case SCL_T_INT:  return "int";
    case SCL_T_FLAG: return "flag";
    default:         return "string";
    }
}

/* 打印命令 usage 行（参数模板；esp_console 风格提示） */
static void Sh_PrintUsage(const scl_cmd_t *nd)
{
    const scl_cmd_desc_t *d = nd->desc;
    int i;
    Sh_Puts("usage: ");
    Sh_Puts(nd->name);
    if ((d != NULL) && (d->args != NULL))
    {
        for (i = 0; i < d->arg_cnt; i++)
        {
            const scl_arg_spec_t *a = &d->args[i];
            Sh_Putc((a->opt != 0u) ? '[' : '<');
            Sh_Puts(a->name);
            Sh_Putc(':');
            Sh_Puts(Sh_TypeName(a->type));
            Sh_Putc((a->opt != 0u) ? ']' : '>');
        }
    }
    Sh_Newline();
}
#endif /* SCL_CFG_CMDDESC_EN */

/* 收集匹配 prefix 的命令名（注册命令 + 保留字）到 cand；
   返回匹配数。带 *nunique 供歧义提示（只记首个匹配名） */
static int Sh_CollectCands(const char *prefix, char cand[][SCL_EX_SHELL_LINE_MAX],
                           int cand_max)
{
    int n = 0;
    unsigned plen = Sh_StrLen(prefix);
    const scl_cmd_t *nd;

    for (nd = SCL_CmdHead(); nd != NULL; nd = nd->next)
    {
        if (n >= cand_max) { break; }
        if ((Sh_StrLen(nd->name) >= plen) &&
            (Sh_StrLen(nd->name) < SCL_EX_SHELL_LINE_MAX))
        {
            unsigned k;
            unsigned eq = 1u;
            for (k = 0u; k < plen; k++)
            {
                if (nd->name[k] != prefix[k]) { eq = 0u; break; }
            }
            if (eq)
            {
                unsigned j;
                for (j = 0u; j <= Sh_StrLen(nd->name); j++) { cand[n][j] = nd->name[j]; }
                n++;
            }
        }
    }
    {
        unsigned i;
        for (i = 0u; i < (unsigned)(sizeof(s_keywords) / sizeof(s_keywords[0])); i++)
        {
            if (n >= cand_max) { break; }
            if (Sh_StrLen(s_keywords[i]) >= plen)
            {
                unsigned k;
                unsigned eq = 1u;
                for (k = 0u; k < plen; k++)
                {
                    if (s_keywords[i][k] != prefix[k]) { eq = 0u; break; }
                }
                if (eq)
                {
                    unsigned j;
                    for (j = 0u; j <= Sh_StrLen(s_keywords[i]); j++)
                    {
                        cand[n][j] = s_keywords[i][j];
                    }
                    n++;
                }
            }
        }
    }
    return n;
}

/* 计算一组候选的最长公共前缀（用于单次按 Tab 尽量补全） */
static unsigned Sh_CommonPrefix(char cand[][SCL_EX_SHELL_LINE_MAX], int n)
{
    unsigned i = 0u;
    char c0 = cand[0][0];
    while (c0 != '\0')
    {
        int j;
        for (j = 1; j < n; j++)
        {
            if (cand[j][i] != c0) { return i; }
        }
        i++;
        c0 = cand[0][i];
    }
    return i;
}

static void Sh_Complete(void)
{
    unsigned wi = s_len;
    int is_var = 0;
    unsigned plen;
    unsigned i;
    char cand[16][SCL_EX_SHELL_LINE_MAX];
    int n;

    /* 取最后一个词的起点（含空白前） */
    while ((wi > 0u) && !Sh_IsSp(s_line[wi - 1u])) { wi--; }

    /* 变量引用：当前词以 "${" 开头 → 补 ${var} */
    is_var = ((s_len - wi) >= 2u) && (s_line[wi] == '$') && (s_line[wi + 1u] == '{');
    if (is_var)
    {
        char prefix[SCL_EX_SHELL_LINE_MAX];
        n = 0;
        plen = s_len - wi - 2u;   /* "${" 之后的部分 */
        for (i = 0u; i < plen; i++) { prefix[i] = s_line[wi + 2u + i]; }
        prefix[plen] = '\0';
        {
            int vi = 0;
            char nm[SCL_EX_SHELL_LINE_MAX];
            while (n < 16)
            {
                if (SCL_VarEnum(vi, nm, (int)sizeof(nm)) != 0) { break; }
                vi++;
                if (Sh_StrLen(nm) >= plen)
                {
                    unsigned k;
                    unsigned eq = 1u;
                    for (k = 0u; k < plen; k++)
                    {
                        if (nm[k] != prefix[k]) { eq = 0u; break; }
                    }
                    if (eq)
                    {
                        unsigned j;
                        for (j = 0u; j <= Sh_StrLen(nm); j++) { cand[n][j] = nm[j]; }
                        n++;
                    }
                }
            }
        }
    }
    else if (wi == 0u)
    {
        /* 行首词 → 补命令名/保留字 */
        char prefix[SCL_EX_SHELL_LINE_MAX];
        plen = s_len - wi;
        for (i = 0u; i < plen; i++) { prefix[i] = s_line[wi + i]; }
        prefix[plen] = '\0';
        n = Sh_CollectCands(prefix, cand, 16);
    }
    else
    {
        /* 参数位置且非 ${：若首命令完整且带参数模板 → 提示 usage（不插入文本） */
#if (SCL_CFG_CMDDESC_EN == 1u)
        {
            unsigned k = 0u;
            const scl_cmd_t *hnd;
            while ((k < wi) && !Sh_IsSp(s_line[k])) { k++; }
            if ((k > 0u) && (k < SCL_EX_SHELL_LINE_MAX))
            {
                char hb[SCL_EX_SHELL_LINE_MAX];
                for (i = 0u; i < k; i++) { hb[i] = s_line[i]; }
                hb[k] = '\0';
                hnd = Sh_FindCmd(hb);
                if ((hnd != NULL) && (hnd->desc != NULL) &&
                    (hnd->desc->args != NULL) && (hnd->desc->arg_cnt > 0))
                {
                    Sh_Newline();
                    Sh_PrintUsage(hnd);
                    Sh_Redraw();
                }
            }
        }
#endif
        return;   /* 参数位置且非 ${：不补全 */
    }

    if (n <= 0)
    {
        Sh_Putc(0x07u);   /* 响铃 */
        return;
    }
    if (n == 1)
    {
        /* 唯一候选：替换整词 */
        unsigned nlen = Sh_StrLen(cand[0]);
        unsigned extra = (is_var ? (nlen + 2u) : nlen);
        if ((wi + extra) >= SCL_EX_SHELL_LINE_MAX)
        {
            Sh_Putc(0x07u);
            return;
        }
        /* 删旧词：保留 wi 前内容 */
        if (is_var)
        {
            s_line[wi] = '$';
            s_line[wi + 1u] = '{';
            for (i = 0u; i < nlen; i++) { s_line[wi + 2u + i] = cand[0][i]; }
            s_len = wi + 2u + nlen;
        }
        else
        {
            for (i = 0u; i < nlen; i++) { s_line[wi + i] = cand[0][i]; }
            s_len = wi + nlen;
        }
        s_line[s_len] = '\0';
        Sh_Redraw();
        return;
    }

    /* 多个候选：先补公共前缀（若比当前长） */
    if (!is_var)
    {
        unsigned cp = Sh_CommonPrefix(cand, n);
        unsigned cur = s_len - wi;
        if ((cp > cur) && (wi + cp) < SCL_EX_SHELL_LINE_MAX)
        {
            for (i = cur; i < cp; i++) { s_line[wi + i] = cand[0][i]; }
            s_len = wi + cp;
            s_line[s_len] = '\0';
            Sh_Redraw();
        }
    }
    /* 列出候选：命令候选逐行（带 desc 一行帮助）；变量候选行内空格分隔 */
    Sh_Newline();
    for (i = 0u; i < (unsigned)n; i++)
    {
        if (is_var)
        {
            Sh_Puts("${");
            Sh_Puts(cand[i]);
            Sh_Puts("}");
            Sh_Putc(' ');
        }
        else
        {
            const char *h = NULL;
#if (SCL_CFG_CMDDESC_EN == 1u)
            const scl_cmd_t *m = Sh_FindCmd(cand[i]);
            if ((m != NULL) && (m->desc != NULL) && (m->desc->help != NULL))
            {
                h = m->desc->help;
            }
#endif
            Sh_Puts(cand[i]);
            if (h != NULL)
            {
                Sh_Puts(" — ");
                Sh_Puts(h);
            }
            Sh_Newline();
        }
    }
    Sh_Newline();
    Sh_Redraw();
}

/* ========================== 执行 / 提示 ========================== */

static void Sh_RunLine(void)
{
    Sh_Newline();
    /* quit / exit 退出请求 */
    if ((s_len == 4u) && (s_line[0] == 'q') && (s_line[1] == 'u') &&
        (s_line[2] == 'i') && (s_line[3] == 't'))
    {
        s_quit = 1u;
        s_len = 0u;
        s_line[0] = '\0';
        s_hist_pos = -1;
        return;
    }
    if ((s_len == 4u) && (s_line[0] == 'e') && (s_line[1] == 'x') &&
        (s_line[2] == 'i') && (s_line[3] == 't'))
    {
        s_quit = 1u;
        s_len = 0u;
        s_line[0] = '\0';
        s_hist_pos = -1;
        return;
    }
    Sh_HistPush();
    if (s_len != 0u)
    {
        if (SCL_Idle())
        {
            s_line[s_len] = '\0';
            if (SCL_Run(s_line) == 0u)
            {
                Sh_Puts("[run 拒绝]\r\n");
            }
            s_was_busy = 1u;   /* 待命令完成后打印提示 */
        }
        else
        {
            Sh_Puts(s_busy_msg);
        }
    }
    s_len = 0u;
    s_line[0] = '\0';
    s_hist_pos = -1;
}

/* ========================== 公共接口 ========================== */

void Scl_Shell_Init(void (*out)(char))
{
    unsigned i;
    s_out = out;
    s_len = 0u;
    s_line[0] = '\0';
    s_hist_cnt = 0;
    s_hist_pos = -1;
    s_esc = 0u;
    s_was_busy = 0u;
    s_quit = 0u;
    for (i = 0u; i < SCL_EX_SHELL_LINE_MAX; i++) { s_edit[i] = '\0'; }
    SCL_VarKeep(1);   /* 会话模式：var 跨命令保留（free 显式释放） */
    Sh_Puts(s_prompt);   /* 首提示符 */
}

void Scl_Shell_Feed(int ch)
{
    if (s_esc == 1u)
    {
        s_esc = 0u;
        if (ch == '[') { s_esc = 2u; }
        return;
    }
    if (s_esc == 2u)
    {
        s_esc = 0u;
        if (ch == 'A') { Sh_HistUp(); }
        else if (ch == 'B') { Sh_HistDown(); }
        /* C/D/E/F 左右/End/Home 略（光标恒在尾部） */
        return;
    }
    if (ch == 0x1Bu)   /* ESC */
    {
        s_esc = 1u;
        return;
    }
    if ((ch == '\r') || (ch == '\n'))
    {
        Sh_RunLine();
        return;
    }
    if ((ch == 0x08u) || (ch == 0x7Fu))   /* Backspace */
    {
        if (s_len > 0u)
        {
            s_len--;
            s_line[s_len] = '\0';
            Sh_Puts("\b \b");
        }
        return;
    }
    if (ch == 0x15u)   /* Ctrl-U：清行 */
    {
        while (s_len > 0u)
        {
            s_len--;
        }
        s_line[0] = '\0';
        Sh_Redraw();
        return;
    }
    if (ch == '\t')
    {
        Sh_Complete();
        return;
    }
    if ((ch >= 0x20) && (ch <= 0x7E))
    {
        if (s_len + 1u < SCL_EX_SHELL_LINE_MAX)
        {
            s_line[s_len++] = (char)ch;
            s_line[s_len] = '\0';
            Sh_Putc((char)ch);
        }
        return;
    }
    /* 其它控制字符忽略 */
}

void Scl_Shell_Poll(void)
{
    unsigned busy = (SCL_Idle() == 0u) ? 1u : 0u;
    if ((s_was_busy != 0u) && (busy == 0u))
    {
        Sh_Puts(s_prompt);   /* 命令完成 → 提示 */
    }
    s_was_busy = busy;
}

int Scl_Shell_QuitReq(void)
{
    return (s_quit != 0u) ? 1 : 0;
}

#endif /* SCL_EX_SHELL_EN */
