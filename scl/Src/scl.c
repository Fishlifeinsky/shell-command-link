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
  *                callf <名>      运行时子程序调用：保存返回点(下一条)到 fn_back 再跳
  *                retf            子程序返回：无条件跳回 fn_back（v0.3d 单层，无嵌套）
  *              运行时不再提供 if/while 文本（由上层编译器降级为 label/jump/callf/retf）
  *            - SCL_Run() 把文本**编译成字节码**后立即返回并置 busy：
  *                每条指令固定 4 字节 = opc(2B,大端) + argOff(2B,大端)
  *                参数写入"参数字节缓存"并**同步解析成类型块**（type 开头，无空格）：
  *                  BOOL=0x01+v(1) | INT=0x02+4B 大端 | FLAG=0x03+c(1) | STR=0x04+len(1)+bytes
  *                元指令（var/free/help/label/jump）参数按整段 STR 块存原文内部解析；
  *                业务命令/运算指令参数按字面量类型化；无参 argOff=0（保留缓存第 0 字节）
  *                label → 登记到 label 表 (名 → 下一条指令字节偏移)
  *                jump/callf → 编译期把名解析为目标偏移写入 argOff 槽；retf 无参
  *            - 命令 opcode：注册表命令 = SCL_CFG_OP_CMD_BASE + 表内下标；
  *              手工 SCL_RegisterCmd 自 BASE + SCL_CFG_CMD_RESERVE 起递增
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
#include "scl_priv.h"

/* 变参消息 */
#include <stdarg.h>

/* （文本/数值小工具与消息输出已移至 scl/Src/scl_core.c） */

/* ========================== 注册表（生成物，弱符号接入） ========================== */
/* scl/cmd/scl_cmd_list.c 由 scl/tool/scl_gen_list.py 生成，提供：
       void SCL_RegList_Init(void);   —— 逐个注册命令与静态变量
   未链接该文件时弱符号为 NULL，静默跳过（不影响库单独编译）。 */
#if (SCL_CFG_REG_LIST_EN != 0u)
#if defined(__GNUC__)
#define SCL_REG_WEAK __attribute__((weak))
#elif defined(__ICCARM__) || defined(__CC_ARM) || defined(__ARMCC_VERSION)
#define SCL_REG_WEAK __weak
#else
#define SCL_REG_WEAK
#endif
extern void SCL_RegList_Init(void) SCL_REG_WEAK;
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

    /* v0.3：预编译程序"按名调用"注册命令（argOff 指向参数区 = [total][STR 命令名][参数块...]；
       用于 SCL_RunProg 的 Flash 只读程序，免依赖运行时命令注册顺序） */
    SCL_OP_CALLN = 0x0028u,

    /* v0.3d：运行时子程序（S2C fn 非内联）——全局 fn_back 保存单层返回点，无嵌套/递归 */
    SCL_OP_CALLF = 0x0029u,   /* callf <label>：保存返回点(下一条)到 fn_back，跳转到 label */
    SCL_OP_RETF  = 0x002Au,   /* retf：无条件跳回 fn_back（aoff 忽略） */
    SCL_OP_CACHE = 0x002Bu,   /* cache [max|gc|zombie]：缓存统计与 GC */

    SCL_OP_CMD_BASE = SCL_CFG_OP_CMD_BASE  /* 注册命令 opcode 起点（注册表按下标分配） */
};

/* ========================== 静态状态 ========================== */

static uint8_t s_inited = 0u;      /* 首次自动初始化标记 */

/* ---- 命令链表（注册即自动分配 opcode） ----
   非 static：注册/查找/调用逻辑在 scl_cmd.c（声明见 scl_priv.h） ---- */
scl_cmd_t *s_cmd_head = NULL;
uint16_t   s_next_opc  = (uint16_t)SCL_OP_CMD_BASE;

#if ((SCL_CFG_SCMD_EN != 0u) && (SCL_CFG_RUN_PROG_EN != 0u))
/* ---- 脚本命令链表（s2c 编译产物注册成命令，SCL_Scmd_*） ---- */
static scl_scmd_t *s_scmd_head = NULL;
#endif


/* ---- 当前执行程序：bc/argc 只读访问（可指向 RAM 动态编译产物，或 Flash const 预编译程序）。
     SCL_Run 动态路径把 s_bc/s_argc 挂到 s_prog；SCL_RunProg 直接挂 const 程序 */
static scl_prog_t s_prog;                 /* 激活程序；bc==NULL 表示未装载 */

#if (SCL_CFG_RUN_TEXT_EN != 0u)
/* ---- 字节码程序（SCL_Run 动态编译产物，跨 tick 持久） ---- */
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
static uint8_t *s_bc = NULL;
#else
static uint8_t  s_bc[SCL_CFG_BC_MAX];       /* 每条指令 4 字节 */
#endif
static uint16_t s_bc_len = 0u;              /* 有效字节数（4 的倍数） */
#endif
static uint16_t s_pc     = 0u;              /* 程序计数器（解释执行） */
static uint16_t s_fn_back = 0u;             /* 运行时子程序返回点（callf 保存 / retf 跳回） */
static uint32_t s_steps  = 0u;              /* 本脚本已执行步数（步进保护） */

#if (SCL_CFG_RUN_TEXT_EN != 0u)
/* ---- 参数字节缓存：每条指令参数区 = [total(1)][type 块序列]，argOff 指向 total 字节。
     type 块（type 开头，无空格分隔）：
       BOOL=0x01+v(1) | INT=0x02+4B 大端 | FLAG=0x03+c(1) | STR=0x04+len(1)+bytes
     total=块序列字节数（不含自身）；argOff==0 表示无参数（保留缓存第 0 字节） ---- */
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
static uint8_t *s_argc = NULL;
#else
static uint8_t  s_argc[SCL_CFG_ARG_CACHE_MAX];
#endif
static uint16_t s_arg_len = 0u;

/* ---- label 表：label 名 → 下一条指令字节偏移（仅编译期用，编译后回填为绝对偏移） ---- */
typedef struct
{
    char     name[SCL_CFG_LABEL_NAME_MAX + 1u];
    uint16_t off;
} scl_label_t;
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
static scl_label_t *s_labels = NULL;
#else
static scl_label_t s_labels[SCL_CFG_LABEL_MAX];
#endif
static uint8_t     s_label_cnt = 0u;
#endif

/* ---- 条件标志 G_RETURN ---- */
static uint8_t s_ret = 0u;

/* 会话变量保留标记：置 1 后脚本结束不自动释放变量（供交互 shell/长会话）
   （scl_var.c 的 VarKeep 写、此处 Finish 读） */
uint8_t s_keep_vars = 0u;

/* ---- 执行状态 ---- */
static uint8_t  s_busy = 0u;
static volatile uint8_t s_abort = 0u;
scl_cmd_t *s_wait_cmd = NULL;                /* 正在异步等待的命令（scl_cmd.c 读写） */

/* ---- 动态内存分配与统计 ----
   实现已移至 scl/Src/scl_mem.c；本文件只保留调用点。
   SCL_InitEx 通过 Scl_MemSetAllocator() 注入分配器并清零统计。 */

/* ---- 参数工作缓冲 ----
   s_argb/s_argv/s_argt 由 scl_cmd.c 的命令直调路径共用 → 外部链接（声明见 scl_priv.h）；
   s_raw/s_cmdname 只在本文件用 → 保持 static，未被引用的档位会被编译器自动
   消除（全局符号做不到这一点，mini 会白付几十字节）。容量宏见 scl_priv.h。 */
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
static char *s_raw = NULL;
char (*s_argb)[SCL_CFG_ARG_LEN_MAX] = NULL;
char **s_argv = NULL;
uint8_t *s_argt = NULL;
#else
static char s_raw[SCL_RAW_MAX];
char s_argb[SCL_CFG_ARG_MAX][SCL_CFG_ARG_LEN_MAX];
char *s_argv[SCL_CFG_ARG_MAX];
uint8_t s_argt[SCL_CFG_ARG_MAX];       /* 当前命令各参数 type（SCL_ArgType 用） */
#endif
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
static char *s_cmdname = NULL;         /* CALLN：动态工作区 */
#else
static char s_cmdname[SCL_CMDNAME_MAX]; /* CALLN：当前按名调用命令名（运行工作区） */
#endif

/* 文本/数值小工具实现见 scl/Src/scl_core.c（原型在 scl_priv.h） */

/* 编译期字面量 token 归类（命令参数）：返回 SCL_T_*；
   FLAG→*iv=字符代码；BOOL→0/1；INT→数值；无法定类一律 STR（*iv 忽略） */
#if (SCL_CFG_RUN_TEXT_EN != 0u)
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
#endif /* SCL_CFG_RUN_TEXT_EN */

/* 消息格式化/输出实现见 scl/Src/scl_core.c */

/* ============================ 消息级别（全局，运行期可调） ============================ */

/* 消息级别（SCL_MsgLvl / SCL_MsgLevelSet / SCL_MsgLevelGet）与 Scl_Msg / Scl_MsgErr
   实现均已移至 scl/Src/scl_core.c；本文件只调用。 */


/* (会话变量与 env 实现拆分至 scl_var.c / scl_env.c，见 scl_priv.h) */

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

/* 命令注册表、按名/按 opcode 查找、编程式调用（含异步）、
   一行解析 SCL_RunLine 与 ${} 展开 Scl_ExpandCopy 均已移至 scl/Src/scl_cmd.c
   （原型见 scl_priv.h）。 */

/* SCL_RunLine / Scl_CmdFindName / SCL_CmdInvoke / SCL_Async* 的实现见 scl_cmd.c。
   下面两个只被本文件的编译链与执行路径使用，保持 static：未被引用的档位
   （如 mini）会被编译器整段消除，不占体积。 */

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

/* ${} 展开拷贝：成功 0；-1 变量名过长；-2 目标缓冲不足（已尽量写入） */
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

#if (SCL_CFG_RUN_TEXT_EN != 0u)
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
#endif /* SCL_CFG_RUN_TEXT_EN */

/* 运行时（元指令）：从 aoff 读取整段原文（参数区须为单个 STR 块，不做 ${} 展开）。
   无参数/不符返回 NULL */
static const char *Scl_ArgLoad(uint16_t aoff)
{
    uint16_t total;
    uint16_t o;
    uint16_t len;
    uint16_t i;

    if (aoff == 0u) { return NULL; }
    if (aoff >= s_prog.arg_len) { return NULL; }
    total = s_prog.argc[aoff];
    if ((uint16_t)(aoff + 1u + total) > s_prog.arg_len) { return NULL; }
    o = aoff + 1u;
    if (total == 0u) { return ""; }
    if ((s_prog.argc[o] != SCL_T_STR) || (total < 2u)) { return NULL; }
    len = s_prog.argc[o + 1u];
    if ((uint16_t)(2u + len) > total) { return NULL; }
    if (len >= SCL_RAW_MAX) { return NULL; }
    for (i = 0u; i < len; i++) { s_raw[i] = (char)s_prog.argc[o + 2u + i]; }
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

    if (o >= s_prog.arg_len) { return 0xFFFFu; }
    t = s_prog.argc[o];
    if (t == SCL_T_BOOL)
    {
        if ((uint16_t)(o + 2u) > s_prog.arg_len) { return 0xFFFFu; }
        if (s_prog.argc[o + 1u] != 0u)
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
        if ((uint16_t)(o + 5u) > s_prog.arg_len) { return 0xFFFFu; }
        v = (int32_t)(((uint32_t)s_prog.argc[o + 1u] << 24) |
                      ((uint32_t)s_prog.argc[o + 2u] << 16) |
                      ((uint32_t)s_prog.argc[o + 3u] << 8) |
                      (uint32_t)s_prog.argc[o + 4u]);
        if (Scl_FmtI32(v, dst, cap) == 0u) { return 0xFFFFu; }
        return (uint16_t)(o + 5u);
    }
    if (t == SCL_T_FLAG)
    {
        if ((uint16_t)(o + 2u) > s_prog.arg_len) { return 0xFFFFu; }
        if (cap < 3u) { return 0xFFFFu; }
        dst[0] = '-'; dst[1] = (char)s_prog.argc[o + 1u]; dst[2] = '\0';
        return (uint16_t)(o + 2u);
    }
    if (t == SCL_T_STR)
    {
        const char *sb;
        if ((uint16_t)(o + 2u) > s_prog.arg_len) { return 0xFFFFu; }
        len = s_prog.argc[o + 1u];
        if ((uint16_t)(o + 2u + len) > s_prog.arg_len) { return 0xFFFFu; }
        sb = (const char *)&s_prog.argc[o + 2u];
        if (Scl_ExpandCopy(sb, sb + len, dst, cap) < 0) { return 0xFFFFu; }
        return (uint16_t)(o + 2u + len);
    }
    (void)i;
    (void)len;
    return 0xFFFFu;   /* 未知 type */
}

/* 运行时：从参数区中段 o 开始还原参数至 end（type 块 → 文本；STR 展开 ${}；记录各参数 type）。
   返回 argc；负=错误 */
static int Scl_ArgRestoreAt(uint16_t o, uint16_t end)
{
    int ai = 0;

    while (o < end)
    {
        uint16_t nxt;
        if (ai >= (int)SCL_CFG_ARG_MAX) { return -3; }
        nxt = Scl_BlkText(o, s_argb[ai], SCL_CFG_ARG_LEN_MAX);
        if (nxt == 0xFFFFu) { return -4; }
        s_argt[ai] = (uint8_t)s_prog.argc[o];   /* 参数原始 type */
        s_argv[ai] = s_argb[ai];
        ai++;
        if (nxt <= o) { break; }   /* 防死循环 */
        o = nxt;
    }
    return ai;
}

/* 运行时：还原某指令参数区为 argv（type 块 → 文本；STR 展开 ${}；记录各参数 type）。
   返回 argc；0=无参数；负=错误 */
static int Scl_ArgRestore(uint16_t aoff)
{
    uint16_t total;
    uint16_t end;

    if (aoff == 0u) { return 0; }
    if (aoff >= s_prog.arg_len) { return -1; }
    total = s_prog.argc[aoff];
    if ((uint16_t)(aoff + 1u + total) > s_prog.arg_len) { return -1; }
    end = (uint16_t)(aoff + 1u + total);
    return Scl_ArgRestoreAt(aoff + 1u, end);
}

/* 运行时：执行"按名调用"注册命令（预编译 const 程序 SCL_RunProg 专用；不依赖命令注册顺序）。
   aoff 指向参数区 = [total][STR 命令名][参数 type 块...]；命令名块不做 ${} 展开。
   内部查命令链表并调用；返回 0=已执行；负=错误（-1 格式错；-2 未知命令） */
static int Scl_DoCallName(uint16_t aoff)
{
    uint16_t total;
    uint16_t o;
    uint16_t nl;
    uint16_t k;
    int  argc;
    scl_cmd_t *nd;

    if (aoff == 0u) { return -1; }
    if (aoff >= s_prog.arg_len) { return -1; }
    total = s_prog.argc[aoff];
    if ((uint16_t)(aoff + 1u + total) > s_prog.arg_len) { return -1; }
    o = aoff + 1u;
    if (total == 0u) { return -1; }                       /* 至少应有命令名 */
    if (s_prog.argc[o] != SCL_T_STR) { return -1; }      /* 首块须为 STR 命令名 */
    nl = s_prog.argc[o + 1u];
    if ((uint16_t)(o + 2u + nl) > (uint16_t)(aoff + 1u + total)) { return -1; }
    if ((nl == 0u) || (nl >= SCL_CMDNAME_MAX)) { return -1; }
    for (k = 0u; k < nl; k++) { s_cmdname[k] = (char)s_prog.argc[o + 2u + k]; }
    s_cmdname[nl] = '\0';
    o = (uint16_t)(o + 2u + nl);

    nd = Scl_CmdFindName(s_cmdname, nl);
    if (nd == NULL)
    {
        Scl_MsgErr("未知命令 '%s'", s_cmdname);
        return -2;
    }
    argc = Scl_ArgRestoreAt(o, (uint16_t)(aoff + 1u + total));
    if (argc < 0)
    {
        Scl_MsgErr("命令参数错误(%d)", argc);
        return -1;
    }
    if (nd->sync != NULL)
    {
        s_wait_cmd = nd;   /* 异步：先登记等待，再发起 */
    }
    nd->fn(argc, s_argv);
    return 0;
}

/* ============ 内置 int/bool 运算指令保留字表（参考 C） ============ */
#if (SCL_CFG_RUN_TEXT_EN != 0u)

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

/* 编译（两遍直接扫描文本，不保留中间指令表 —— 省 RAM：原 s_ins[BC/4] 静态表 ~KB 级）。
   第一遍登记 label 表并统计指令数；第二遍从文本重扫逐条生成字节码/参数缓存；
   两遍共用 Scl_NextClause 保证子句切分完全一致。 */
#define SCL_BC_INSTR_MAX (SCL_CFG_BC_MAX / 4u)

/* 一条待编译子句的规范化视图 */
typedef struct
{
    const char *hs;      /* 首词（命令/关键字）起点 */
    uint16_t    hlen;    /* 首词长度 */
    const char *rs;      /* 参数区起点（去前导空白） */
    const char *re;      /* 参数区终点（已去尾空白，不含） */
} scl_clause_t;

/* 从 *pp 取下一条"有效子句"（自动跳过空子句与 # 注释）。
   返回 0=得到子句并填充 cl；1=文本结束。成功时 *pp 已越过该子句（含其后的 ';'） */
static uint8_t Scl_NextClause(const char **pp, scl_clause_t *cl)
{
    const char *p = *pp;
    for (;;)
    {
        const char *cs;
        const char *ce;
        const char *hs;
        const char *he;
        const char *rs;
        const char *re;
        char q = 0;

        while ((*p == ';') || Scl_IsSp(*p)) { p++; }
        if (*p == '\0') { return 1u; }
        cs = p;
        while (*p != '\0')
        {
            if ((*p == ';') && (q == 0)) { break; }
            if ((*p == '"') || (*p == '\''))
            {
                if (q == 0) { q = *p; }
                else if (q == *p) { q = 0; }
            }
            p++;
        }
        ce = p;
        if (*p == ';') { p++; }
        hs = cs;
        he = cs;
        while ((he < ce) && !Scl_IsSp(*he)) { he++; }
        /* 整句注释：以 '#' 开头的子句跳过（值内 '#' 不受影响） */
        if ((hs < he) && (*hs == '#')) { continue; }
        rs = he;
        re = ce;
        while ((rs < re) && Scl_IsSp(*rs)) { rs++; }
        while ((re > rs) && Scl_IsSp(*(re - 1))) { re--; }
        cl->hs   = hs;
        cl->hlen = (uint16_t)(he - hs);
        cl->rs   = rs;
        cl->re   = re;
        *pp = p;
        return 0u;
    }
}

/* 编译文本为字节码（s_bc/s_argc/s_labels）。成功返回 1 */
static uint8_t Scl_Compile(const char *script)
{
    const char *p;
    uint16_t icnt = 0u;      /* 指令数（label 不计） */
    uint16_t i;
    scl_clause_t cl;

    s_label_cnt = 0u;
    s_arg_len   = 0u;

    /* ---- 第一遍：登记 label 表并统计指令数（不产中间表，省 RAM） ---- */
    p = script;
    while (Scl_NextClause(&p, &cl) == 0u)
    {
        if (icnt >= SCL_BC_INSTR_MAX)
        {
            Scl_MsgErr("指令过多(>%d)", (int)SCL_BC_INSTR_MAX);
            return 0u;
        }
        if ((cl.hlen == 5u) && Scl_EqN(cl.hs, "label", 5u))
        {
            /* label <名>：登记名 → 当前指令计数*4 */
            uint16_t nlen = (uint16_t)(cl.re - cl.rs);
            uint8_t dup = 0u;
            if ((nlen == 0u) || (nlen > SCL_CFG_LABEL_NAME_MAX))
            {
                Scl_MsgErr("label: 名不合法");
                return 0u;
            }
            if (s_label_cnt >= (uint8_t)SCL_CFG_LABEL_MAX)
            {
                Scl_MsgErr("label 过多(>%d)", (int)SCL_CFG_LABEL_MAX);
                return 0u;
            }
            for (i = 0u; i < s_label_cnt; i++)
            {
                if ((Scl_StrLen(s_labels[i].name) == nlen) &&
                    Scl_EqN(s_labels[i].name, cl.rs, nlen))
                {
                    dup = 1u;
                    break;
                }
            }
            if (dup != 0u)
            {
                Scl_MsgErr("label 重名");
                return 0u;
            }
            s_labels[s_label_cnt].off = (uint16_t)(icnt * 4u);
            {
                uint16_t j;
                for (j = 0u; j < nlen; j++)
                {
                    s_labels[s_label_cnt].name[j] = cl.rs[j];
                }
                s_labels[s_label_cnt].name[nlen] = '\0';
            }
            s_label_cnt++;
            continue;   /* label 不占指令 */
        }
        icnt++;   /* label 之外均占一条指令（jump/运算/元命令/业务命令） */
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

    /* ---- 第二遍：重扫文本，逐条生成字节码与参数缓存（label 无字节码，跳过） ---- */
    s_bc_len   = 0u;
    s_arg_len  = 0u;
    p = script;
    while (Scl_NextClause(&p, &cl) == 0u)
    {
        uint16_t opc;
        uint16_t aoff = 0u;

        if ((cl.hlen == 5u) && Scl_EqN(cl.hs, "label", 5u))
        {
            continue;   /* label 不产生字节码 */
        }
        if ((cl.hlen == 4u) && Scl_EqN(cl.hs, "jump", 4u))
        {
            /* jump [-a|-b] <名>：解析模式与目标并回填绝对偏移 */
            const char *w = cl.rs;
            const char *wn;
            uint8_t cond = 0u;
            const char *tgt;
            uint16_t tlen;
            uint8_t found = 0u;
            uint8_t k;

            while ((w < cl.re) && !Scl_IsSp(*w)) { w++; }   /* 首 token 末尾 */
            wn = w;
            while ((wn < cl.re) && Scl_IsSp(*wn)) { wn++; } /* 次 token 起点 */
            if (((uint16_t)(w - cl.rs) == 2u) && (cl.rs[0] == '-'))
            {
                if (cl.rs[1] == 'a')
                {
                    cond = 1u;
                }
                else if (cl.rs[1] == 'b')
                {
                    cond = 0u;
                }
                else
                {
                    Scl_MsgErr("jump: 未知模式");
                    return 0u;
                }
                tgt  = wn;
                tlen = (uint16_t)(cl.re - wn);   /* re 已去尾空白 */
            }
            else
            {
                tgt  = cl.rs;
                tlen = (uint16_t)(w - cl.rs);    /* 无模式：整体为 label 名 */
            }
            if (tlen == 0u)
            {
                Scl_MsgErr("jump: 缺少目标 label");
                return 0u;
            }
            for (k = 0u; k < s_label_cnt; k++)
            {
                if ((Scl_StrLen(s_labels[k].name) == tlen) &&
                    Scl_EqN(s_labels[k].name, tgt, tlen))
                {
                    aoff = s_labels[k].off;
                    found = 1u;
                    break;
                }
            }
            if (found == 0u)
            {
                char nbuf[SCL_CFG_LABEL_NAME_MAX + 1u];
                uint16_t nn = (tlen < SCL_CFG_LABEL_NAME_MAX) ?
                              tlen : SCL_CFG_LABEL_NAME_MAX;
                for (k = 0u; k < nn; k++) { nbuf[k] = tgt[k]; }
                nbuf[nn] = '\0';
                Scl_MsgErr("jump: label '%s' 未定义", nbuf);
                return 0u;
            }
            opc = (cond != 0u) ? SCL_OP_JUMPA : SCL_OP_JUMP;
        }
        else if ((cl.hlen == 5u) && Scl_EqN(cl.hs, "callf", 5u))
        {
            /* callf <label>：运行时子程序调用——保存返回点再跳转（fn 非内联） */
            const char *w  = cl.rs;
            const char *tgt;
            uint16_t tlen;
            uint16_t k;
            uint8_t  found = 0u;

            while ((w < cl.re) && !Scl_IsSp(*w)) { w++; }
            tgt  = cl.rs;
            tlen = (uint16_t)(w - cl.rs);
            if (tlen == 0u)
            {
                Scl_MsgErr("callf: 缺少目标 label");
                return 0u;
            }
            for (k = 0u; k < s_label_cnt; k++)
            {
                if ((Scl_StrLen(s_labels[k].name) == tlen) &&
                    Scl_EqN(s_labels[k].name, tgt, tlen))
                {
                    aoff = s_labels[k].off;
                    found = 1u;
                    break;
                }
            }
            if (found == 0u)
            {
                char nbuf[SCL_CFG_LABEL_NAME_MAX + 1u];
                uint16_t nn = (tlen < SCL_CFG_LABEL_NAME_MAX) ?
                              tlen : SCL_CFG_LABEL_NAME_MAX;
                uint16_t k2;
                for (k2 = 0u; k2 < nn; k2++) { nbuf[k2] = tgt[k2]; }
                nbuf[nn] = '\0';
                Scl_MsgErr("callf: label '%s' 未定义", nbuf);
                return 0u;
            }
            opc = SCL_OP_CALLF;
        }
        else if ((cl.hlen == 4u) && Scl_EqN(cl.hs, "retf", 4u))
        {
            opc = SCL_OP_RETF;   /* 无参：aoff=0 */
        }
        else
        {
            /* 内置运算指令 / 内置元命令 / 注册命令 */
            opc = Scl_OpWord(cl.hs, cl.hlen);   /* 运算指令优先（保留字） */
            if (opc == 0u)
            {
                if ((cl.hlen == 4u) && Scl_EqN(cl.hs, "help", 4u))
                {
                    opc = SCL_OP_HELP;
                }
                else if ((cl.hlen == 3u) && Scl_EqN(cl.hs, "var", 3u))
                {
                    opc = SCL_OP_VAR;
                }
                else if ((cl.hlen == 4u) && Scl_EqN(cl.hs, "free", 4u))
                {
                    opc = SCL_OP_FREE;
                }
                else if ((cl.hlen == 5u) && Scl_EqN(cl.hs, "cache", 5u))
                {
                    opc = SCL_OP_CACHE;
                }
                else
                {
                    scl_cmd_t *nd = Scl_CmdFindName(cl.hs, cl.hlen);
                    if (nd == NULL)
                    {
                        char nbuf[24u];
                        uint16_t nn = (cl.hlen < 23u) ? cl.hlen : 23u;
                        uint16_t k;
                        for (k = 0u; k < nn; k++) { nbuf[k] = cl.hs[k]; }
                        nbuf[nn] = '\0';
                        Scl_MsgErr("未知命令 '%s'", nbuf);
                        return 0u;
                    }
                    opc = nd->opc;
                }
            }
            if ((opc == SCL_OP_HELP) || (opc == SCL_OP_VAR) ||
                (opc == SCL_OP_FREE) || (opc == SCL_OP_CACHE))
            {
                /* 元指令：整段原文按单个 STR 块存（var/free/help 内部自行解析） */
                aoff = Scl_ArgStoreMeta(cl.rs, (uint16_t)(cl.re - cl.rs));
                if (aoff == 0xFFFFu)
                {
                    Scl_MsgErr("参数字节缓存不足/超长");
                    return 0u;
                }
            }
            else
            {
                /* 业务命令/运算指令：字面量类型化存储 */
                aoff = Scl_ArgStoreTyped(cl.rs, cl.re);
                if (aoff == 0xFFFFu)
                {
                    Scl_MsgErr("参数字节缓存不足/参数超长/引号未闭合");
                    return 0u;
                }
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
#endif /* SCL_CFG_RUN_TEXT_EN */

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

#if (SCL_CFG_CMDDESC_EN != 0u)
static void Scl_DescPrintUsage(const scl_cmd_t *nd);     /* 前向：供 help 输出模板概要 */
#endif
#if ((SCL_CFG_CMDDESC_EN != 0u) && (SCL_CFG_MSG_EN == 1u))
static void Scl_DescPrintDetail(const scl_cmd_t *nd);    /* 前向：help <cmd> 命令明细（消息输出用） */
#endif

#if (SCL_CFG_MSG_EN == 1u)
/* ============ help 输出（全览/单命令文档表）：仅消息开时编译，关消息整段裁掉省 ROM ============ */
static void Scl_DoHelp(void)
{
    scl_cmd_t *node;
    Scl_Msg("scl: 内置元命令: var / free / help / cache / label / jump\r\n");
    Scl_Msg("scl: 内置运算: iadd isub imul idiv imod ineg | ieq ine igt ige ilt ile\r\n");
    Scl_Msg("scl:            band bor bnot btest | iand ior ixor inot shl shr | seq sneq\r\n");
    Scl_Msg("scl: var <type> <name>=<value>, type = bool/int/flag/string\r\n");
    Scl_Msg("scl: 已注册命令:\r\n");
    for (node = s_cmd_head; node != NULL; node = node->next)
    {
        Scl_Msg("  %s%s (opc 0x%04x)", node->name,
                (node->sync != NULL) ? " (异步)" : "",
                (unsigned int)node->opc);
#if (SCL_CFG_CMDDESC_EN != 0u)
        if ((node->desc != NULL) && (node->desc->help != NULL))
        {
            Scl_Msg(" - %s", node->desc->help);
        }
#endif
        Scl_Msg("\r\n");
#if (SCL_CFG_CMDDESC_EN != 0u)
        if ((node->desc != NULL) && (node->desc->args != NULL) &&
            (node->desc->arg_cnt > 0))
        {
            Scl_Msg("      ");
            Scl_DescPrintUsage(node);   /* 模板概要（usage 行） */
        }
#endif
    }
}

/* ========================== help <cmd>：单命令明细 ========================== */

/* 帮助表条目：name 匹配时输出 doc（doc 行尾已含 \r\n） */
typedef struct
{
    const char *name;
    const char *doc;
} scl_help_doc_t;

/* 内置元命令帮助（var/free/help/label/jump） */
static const scl_help_doc_t s_help_meta[] =
{
    { "var",
      "变量/常量管理\r\n"
      "  var free                      释放全部变量（剩余空位见输出）\r\n"
      "  var <type> <name>=<value>     声明变量, type = bool/int/flag/string\r\n"
      "  var const <type> <name>=<v>   声明只读常量（不可覆盖/释放）\r\n"
      "  var <name>                    查询单个变量\r\n"
      "  ${<name>}                     在其它指令参数中引用变量值" },
    { "free",
      "释放变量\r\n"
      "  free              释放全部变量\r\n"
      "  free <name>       释放单个变量（const 常量拒绝）" },
    { "help",
      "帮助\r\n"
      "  help              列出全部命令\r\n"
      "  help <cmd>        查看单条命令用法（业务命令带参数模板明细）" },
    { "label",
      "设置跳转点（脚本编译期登记, 供 jump 跳转）\r\n"
      "  label <name>" },
    { "jump",
      "跳转（汇编式控制流）\r\n"
      "  jump <name>         无条件跳转\r\n"
            "  jump -a <name>      G_RETURN 为真时跳转（读后清零）" },
        { "cache",
            "缓存统计与垃圾回收\r\n"
            "  cache                查看当前/峰值/容量/GC统计\r\n"
            "  cache max            查看峰值（与 cache 相同）\r\n"
            "  cache gc             收缩变量值缓冲\r\n"
            "  cache zombie         清理未使用变量槽残留缓冲" }
};

/* 内置运算指令帮助（分组, 组内各 token 均命中同组说明） */
static const scl_help_doc_t s_help_ops[] =
{
    { "iadd isub imul idiv imod ineg", "int 算术：op a b → dst（结果写回 dst 变量）" },
    { "ieq ine igt ige ilt ile",       "int 比较：op a b → G_RETURN" },
    { "band bor bnot",                 "bool 逻辑：op a [b] → G_RETURN" },
    { "btest",                         "取变量真值 → G_RETURN（bool/非0 int/已定义 flag）" },
    { "iand ior ixor inot shl shr",    "int 位运算/移位：op a b → dst（结果写回变量）" },
    { "seq sneq",                      "string/flag 相等/不等比较 → G_RETURN（变量自动展开）" }
};

/* 判断 name 是否命中表条目（条目名内以空格分隔的任一 token） */
static const char *Scl_HelpDocFind(const scl_help_doc_t *tbl, uint16_t n,
                                   const char *name, uint16_t len)
{
    uint16_t i;
    for (i = 0u; i < n; i++)
    {
        const char *p = tbl[i].name;
        while (*p != '\0')
        {
            const char *tb = p;
            while ((*p != '\0') && !Scl_IsSp(*p)) { p++; }
            if (((uint16_t)(p - tb) == len) && Scl_EqIN(tb, name, len))
            {
                return tbl[i].doc;
            }
            while (Scl_IsSp(*p)) { p++; }
        }
    }
    return NULL;
}

/* help <cmd>：单命令帮助（注册命令优先, 其次内置元命令/运算） */
static void Scl_HelpOne(const char *nm, uint16_t nl)
{
    const scl_cmd_t *node;
    const char *doc;

    /* 1) 注册命令（大小写不敏感查找, 便于交互输入） */
    for (node = s_cmd_head; node != NULL; node = node->next)
    {
        uint16_t L = Scl_StrLen(node->name);
        if ((L == nl) && Scl_EqIN(node->name, nm, nl))
        {
#if (SCL_CFG_CMDDESC_EN != 0u)
            if (node->desc != NULL)
            {
                Scl_DescPrintDetail(node);
            }
            else
#endif
            {
                Scl_Msg("%s%s\r\n", node->name,
                        (node->sync != NULL) ? "（异步）" : "");
                Scl_Msg("  普通注册命令（无参数模板描述）\r\n");
            }
            return;
        }
    }
    /* 2) 内置元命令 / 内置运算 */
    doc = Scl_HelpDocFind(s_help_meta,
                          (uint16_t)(sizeof(s_help_meta) / sizeof(s_help_meta[0])),
                          nm, nl);
    if (doc == NULL)
    {
        doc = Scl_HelpDocFind(s_help_ops,
                              (uint16_t)(sizeof(s_help_ops) / sizeof(s_help_ops[0])),
                              nm, nl);
    }
    if (doc != NULL)
    {
        Scl_Msg("%s\r\n", doc);
        return;
    }
    {
        char nbuf[32u];
        uint16_t nn = (nl < 31u) ? nl : 31u;
        uint16_t k;
        for (k = 0u; k < nn; k++) { nbuf[k] = nm[k]; }
        nbuf[nn] = '\0';
        Scl_MsgErr("help: 未知命令 '%s'（help 查看全部）", nbuf);
    }
}

/* help [cmd]：无参 → 全部概览；有参 → 单命令明细 */
static void Scl_DoHelpRaw(const char *raw)
{
    const char *p;
    const char *nb;

    if ((raw == NULL) || (raw[0] == '\0'))
    {
        Scl_DoHelp();
        return;
    }
    p = raw;
    while (Scl_IsSp(*p)) { p++; }
    nb = p;
    while ((*p != '\0') && !Scl_IsSp(*p)) { p++; }
    Scl_HelpOne(nb, (uint16_t)(p - nb));
}
#endif /* SCL_CFG_MSG_EN：help 文档/输出整段 */

/* 数字解析（命令内取值用，无 libc）：文本 → int32；失败返回 def */
int32_t SCL_ParseInt(const char *s, int32_t def)
{
    int32_t v;
    if ((s != NULL) && (Scl_ParseI32Len(s, Scl_StrLen(s), &v) == 0))
    {
        return v;
    }
    return def;
}

#if (SCL_CFG_CMDDESC_EN != 0u)
/* ========================== 命令描述注册辅助（argtable3 风格） ========================== */

/* 打印 usage 行（<必选:类型> [可选:类型] ...） */
static void Scl_DescPrintUsage(const scl_cmd_t *nd)
{
    const scl_cmd_desc_t *d = nd->desc;
    int i;
    Scl_Msg("usage: %s", nd->name);
    if ((d != NULL) && (d->args != NULL))
    {
        for (i = 0; i < d->arg_cnt; i++)
        {
            const scl_arg_spec_t *a = &d->args[i];
            if (a->opt != 0u) { Scl_Msg(" ["); }
            else              { Scl_Msg(" <"); }
            Scl_Msg("%s:%s", a->name, Scl_TypeName(a->type));
            if (a->opt != 0u) { Scl_Msg("]"); }
            else              { Scl_Msg(">"); }
        }
    }
    Scl_Msg("\r\n");
}

#if (SCL_CFG_MSG_EN == 1u)
/* 打印单命令完整明细（help <cmd>；esp_console 风格：help+usage+逐参数说明+多行 doc） */
static void Scl_DescPrintDetail(const scl_cmd_t *nd)
{
    const scl_cmd_desc_t *d = nd->desc;
    int i;
    Scl_Msg("%s%s", nd->name, (nd->sync != NULL) ? "（异步）" : "");
    if ((d != NULL) && (d->help != NULL))
    {
        Scl_Msg(" — %s", d->help);
    }
    Scl_Msg("\r\n");
    Scl_DescPrintUsage(nd);
    if ((d != NULL) && (d->args != NULL) && (d->arg_cnt > 0))
    {
        for (i = 0; i < d->arg_cnt; i++)
        {
            const scl_arg_spec_t *a = &d->args[i];
            Scl_Msg("    %s<%s>%s", a->name, Scl_TypeName(a->type),
                    (a->opt != 0u) ? " [可选]" : " [必选]");
            if (a->help != NULL) { Scl_Msg("  %s", a->help); }
            Scl_Msg("\r\n");
        }
    }
    /* v0.4a：多行详细说明（仅 help <cmd> 展示；不影响 help 全览与参数校验） */
    if ((d != NULL) && (d->doc != NULL))
    {
        Scl_Msg("%s\r\n", d->doc);
    }
}
#endif /* SCL_CFG_MSG_EN：DescPrintDetail 仅供 help 输出 */

/* 单参数与模板匹配：0=通过。string 模板接受任意；int 模板接受 int 或可解析的文本；
   bool/flag 模板要求类型一致 */
static int Scl_DescArgOk(const scl_arg_spec_t *a, uint8_t have, const char *text)
{
    if (a->type == SCL_T_STR) { return 0; }
    if (a->type == SCL_T_INT)
    {
        int32_t v;
        if (have == SCL_T_INT) { return 0; }
        if ((have == SCL_T_STR) &&
            (Scl_ParseI32Len(text, Scl_StrLen(text), &v) == 0))
        {
            return 0;
        }
        return 1;
    }
    return (have == a->type) ? 0 : 1;
}

/* 按模板校验命令参数（s_argt/s_argv 为当前已还原参数）。0=通过；负=拒绝（已打印） */
/* 参数模板校验（SCL_CmdInvoke 与解释器共用；声明见 scl_priv.h） */
int Scl_DescCheck(const scl_cmd_t *nd, int argc)
{
    const scl_cmd_desc_t *d = nd->desc;
    int minreq = 0;
    int i;

    if ((d == NULL) || (d->args == NULL))
    {
        return 0;   /* 无模板：不限制 */
    }
    for (i = 0; i < d->arg_cnt; i++)
    {
        if (d->args[i].opt == 0u) { minreq++; }
    }
    if (argc < minreq)
    {
        Scl_MsgErr("命令 '%s': 缺少参数（至少 %d 个）", nd->name, minreq);
        Scl_DescPrintUsage(nd);
        return -1;
    }
    if (argc > d->arg_cnt)
    {
        Scl_MsgErr("命令 '%s': 参数过多（最多 %d 个）", nd->name, d->arg_cnt);
        Scl_DescPrintUsage(nd);
        return -2;
    }
    for (i = 0; i < argc; i++)
    {
        if (Scl_DescArgOk(&d->args[i], s_argt[i], s_argv[i]) != 0)
        {
            Scl_MsgErr("命令 '%s': 参数 %d '%s' 期望 %s",
                       nd->name, i + 1, s_argv[i],
                       Scl_TypeName(d->args[i].type));
            Scl_DescPrintUsage(nd);
            return -3;
        }
    }
    return 0;
}

/* 按描述注册命令（填节点 name/fn/sync/desc 后挂链） */
void SCL_CmdRegisterDesc(scl_cmd_t *node, const scl_cmd_desc_t *desc)
{
    if ((node == NULL) || (desc == NULL))
    {
        return;
    }
    node->name = desc->name;
    node->fn   = desc->fn;
    node->sync = desc->sync;
    node->desc = desc;
    SCL_RegisterCmd(node);
}
#endif /* SCL_CFG_CMDDESC_EN */

/* 变量列表 */
static void Scl_VarList(void)
{
    int i;
    Scl_Msg("scl: 变量(%d/%d)\r\n", SCL_VarCount(), (int)SCL_CFG_VAR_MAX);
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u)
        {
            Scl_Msg("  %s%s : %s = %s\r\n",
                    (s_vars[i].ro != 0u) ? "const " : "",
                    s_vars[i].name,
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
    uint8_t isc = 0u;   /* 前缀 const（只读常量） */

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

    /* 首 token（可选 const 前缀）：var const <type> <name>=<value> */
    p = raw;
    while (Scl_IsSp(*p)) { p++; }
    {
        const char *tb = p;
        const char *te = p;
        while ((*te != '\0') && !Scl_IsSp(*te)) { te++; }
        isc = ((uint16_t)(te - tb) == 5u) && Scl_EqN(tb, "const", 5u);
        if (isc != 0u)
        {
            p = te;
            while (Scl_IsSp(*p)) { p++; }
            tb = p;
            te = p;
            while ((*te != '\0') && !Scl_IsSp(*te)) { te++; }
        }
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
        if (isc != 0u)
        {
            r = SCL_VarSetConst(namebuf, typ, valbuf);
        }
        else
        {
            r = SCL_VarSetT(namebuf, typ, valbuf);
        }
        if (r == 0)
        {
            Scl_Msg("scl: var %s%s : %s = %s (剩余空位 %d)\r\n",
                    (isc != 0u) ? "const " : "",
                    namebuf, Scl_TypeName(typ), valbuf, SCL_VarFreeCount());
        }
        else if (r == -1)
        {
            Scl_MsgErr("var: 变量已满(%d 个)", (int)SCL_CFG_VAR_MAX);
        }
        else if (r == -5)
        {
            Scl_MsgErr("var: '%s' 是 const 常量，不可覆盖", namebuf);
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
        int fr = SCL_VarFree(raw);
        if (fr == 0)
        {
            Scl_Msg("scl: free %s（剩余空位 %d）\r\n", raw, SCL_VarFreeCount());
        }
        else if (fr == -2)
        {
            Scl_MsgErr("free: '%s' 是 const 常量，不可释放", raw);
        }
        else
        {
            Scl_MsgErr("free: %s 不存在", raw);
        }
    }
}

static void Scl_DoCacheRaw(const char *raw)
{
    scl_cache_info_t info;
    if ((raw != NULL) && Scl_StrEq(raw, "gc"))
    {
        SCL_CacheGc();
    }
    else if ((raw != NULL) && Scl_StrEq(raw, "zombie"))
    {
        SCL_CacheGcZombie();
    }
    if (SCL_CacheInfo(&info) == 0u) { return; }
    Scl_Msg("scl: cache current=%u peak=%u capacity=%u alloc=%u free=%u gc=%u zombie=%u\r\n",
            (unsigned int)info.current, (unsigned int)info.peak,
            (unsigned int)info.capacity, (unsigned int)info.alloc_count,
            (unsigned int)info.free_count, (unsigned int)info.gc_count,
            (unsigned int)info.zombie_count);
}

/* ========================== 收尾 ========================== */

static void Scl_Finish(int reason)
{
    s_busy     = 0u;
    s_wait_cmd = NULL;
    s_pc       = 0u;
    s_steps    = 0u;
    s_prog.bc  = NULL;
    s_prog.bc_len = 0u;
    s_prog.argc = NULL;
    s_prog.arg_len = 0u;
#if (SCL_CFG_RUN_TEXT_EN != 0u)
    s_bc_len   = 0u;
    s_arg_len  = 0u;
    s_label_cnt = 0u;
#endif
    s_abort    = 0u;
    if (s_keep_vars == 0u)
    {
        SCL_VarFreeAll();   /* 默认：脚本结束自动释放全部变量 */
    }
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
        if (SCL_VarIsConst(dst) != 0)
        {
            Scl_MsgErr("算术: 目标常量 '%s' 只读，不可写回", dst);
        }
        else
        {
            Scl_MsgErr("算术: 目标变量 '%s' 无效或已满", dst);
        }
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

    if ((s_prog.bc == NULL) || (s_pc >= s_prog.bc_len))
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
    opc = (uint16_t)(((uint16_t)s_prog.bc[s_pc] << 8) | s_prog.bc[s_pc + 1u]);
    aoff = (uint16_t)(((uint16_t)s_prog.bc[s_pc + 2u] << 8) | s_prog.bc[s_pc + 3u]);
    next = (uint16_t)(s_pc + 4u);

    switch (opc)
    {
    case SCL_OP_JUMP:
        if (aoff >= s_prog.bc_len)
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
            if (aoff >= s_prog.bc_len)
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

    case SCL_OP_CALLF:
        if (aoff >= s_prog.bc_len)
        {
            Scl_MsgErr("callf: 目标越界");
            Scl_Finish(1);
            return;
        }
        s_fn_back = next;      /* 保存返回点（下一条指令偏移） */
        s_pc = aoff;
        return;

    case SCL_OP_RETF:
        if ((s_fn_back == 0u) || (s_fn_back >= s_prog.bc_len))
        {
            Scl_MsgErr("retf: 无有效返回点（fn_back 未设置）");
            Scl_Finish(1);
            return;
        }
        s_pc = s_fn_back;      /* 跳回调用处之后 */
        return;

    case SCL_OP_HELP:
#if (SCL_CFG_MSG_EN == 1u)
        Scl_DoHelpRaw(Scl_ArgLoad(aoff));   /* help [cmd]：空=全览; 带名=单命令明细 */
#endif
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

    case SCL_OP_CACHE:
        Scl_DoCacheRaw(Scl_ArgLoad(aoff));
        s_pc = next;
        return;

    default:
        if (opc == SCL_OP_CALLN)
        {
            /* 预编译程序按名调用注册命令 */
            if (Scl_DoCallName(aoff) != 0)
            {
                Scl_Finish(1);
                return;
            }
            s_pc = next;
            return;
        }
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
#if (SCL_CFG_CMDDESC_EN != 0u)
                /* 带描述的"表格式命令"：调用前按参数模板校验（不符已打印 usage） */
                if (Scl_DescCheck(nd, argc) != 0)
                {
                    Scl_Finish(1);
                    return;
                }
#endif
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

static void Scl_DynamicRelease(void)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    Scl_EnvShutdown();
    Scl_VarShutdown();
#if (SCL_CFG_RUN_TEXT_EN != 0u)
    Scl_MemFree(s_bc);    s_bc = NULL;
    Scl_MemFree(s_argc);  s_argc = NULL;
    Scl_MemFree(s_labels); s_labels = NULL;
#endif
    Scl_MemFree(s_raw);   s_raw = NULL;
    Scl_MemFree(s_argb);  s_argb = NULL;
    Scl_MemFree(s_argv);  s_argv = NULL;
    Scl_MemFree(s_argt);  s_argt = NULL;
    Scl_MemFree(s_cmdname); s_cmdname = NULL;
#endif
}

uint8_t SCL_InitEx(const scl_allocator_t *allocator)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    uint16_t i;
    if ((allocator == NULL) || (allocator->alloc == NULL) ||
        (allocator->realloc == NULL) || (allocator->free == NULL))
    {
        s_inited = 0u;
        return 0u;
    }
    if (s_inited != 0u) { Scl_DynamicRelease(); }
    /* 注入分配器并清零统计（实现见 scl_mem.c） */
    Scl_MemSetAllocator(allocator);
#if (SCL_CFG_RUN_TEXT_EN != 0u)
    s_bc = (uint8_t *)Scl_MemAlloc(SCL_CFG_BC_MAX);
    s_argc = (uint8_t *)Scl_MemAlloc(SCL_CFG_ARG_CACHE_MAX);
    s_labels = (scl_label_t *)Scl_MemAlloc(sizeof(scl_label_t) * SCL_CFG_LABEL_MAX);
#endif
    s_raw = (char *)Scl_MemAlloc(SCL_RAW_MAX);
    s_argb = (char (*)[SCL_CFG_ARG_LEN_MAX])Scl_MemAlloc(SCL_CFG_ARG_BUF_BYTES);
    s_argv = (char **)Scl_MemAlloc(sizeof(char *) * SCL_CFG_ARG_MAX);
    s_argt = (uint8_t *)Scl_MemAlloc(SCL_CFG_ARG_MAX);
    s_cmdname = (char *)Scl_MemAlloc(SCL_CMDNAME_MAX);
#if (SCL_CFG_RUN_TEXT_EN != 0u)
    if ((s_bc == NULL) || (s_argc == NULL) || (s_labels == NULL))
#else
    if (0)
#endif
    {
        Scl_DynamicRelease();
        return 0u;
    }
    if ((s_raw == NULL) || (s_argb == NULL) || (s_argv == NULL) || (s_argt == NULL) ||
        (s_cmdname == NULL) ||
        (Scl_VarInit() == 0u)
    #if (SCL_CFG_ENV_EN != 0u)
        || (Scl_EnvInit() == 0u)
    #endif
        )
    {
        Scl_DynamicRelease();
        return 0u;
    }
    for (i = 0u; i < SCL_CFG_ARG_MAX; i++)
    {
        s_argv[i] = &s_argb[i][0];
    }
#else
    (void)allocator;
    Scl_VarInit();
#if (SCL_CFG_ENV_EN != 0u)
    Scl_EnvInit();
#endif
#endif
    s_cmd_head  = NULL;
    /* 手工注册从"注册表预留区间之后"开始；注册表命令的 opcode 由表下标直接给出 */
    s_next_opc  = (uint16_t)(SCL_OP_CMD_BASE + SCL_CFG_CMD_RESERVE);
#if ((SCL_CFG_SCMD_EN != 0u) && (SCL_CFG_RUN_PROG_EN != 0u))
    s_scmd_head = NULL;
#endif
    s_busy      = 0u;
    s_abort     = 0u;
    s_wait_cmd  = NULL;
    s_pc        = 0u;
    s_fn_back   = 0u;
    s_prog.bc   = NULL;
    s_prog.bc_len = 0u;
    s_prog.argc = NULL;
    s_prog.arg_len = 0u;
#if (SCL_CFG_RUN_TEXT_EN != 0u)
    s_bc_len    = 0u;
    s_arg_len   = 0u;
    s_label_cnt = 0u;
#endif
    s_ret       = 0u;
    s_keep_vars = 0u;

#if (SCL_CFG_REG_LIST_EN != 0u)
    /* 自动注册：命令 + 静态变量（生成表；弱符号，缺表时跳过） */
    if (SCL_RegList_Init != NULL)
    {
        SCL_RegList_Init();
    }
#endif

    s_inited    = 1u;
    return 1u;
}

void SCL_Init(void)
{
    (void)SCL_InitEx(NULL);
}

#if (SCL_CFG_RUN_TEXT_EN != 0u)
uint8_t SCL_Run(const char *script)
{
    uint16_t i;

    if (s_inited == 0u)
    {
        SCL_Init();
        if (s_inited == 0u) { return 0u; }
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

    /* 装载动态编译产物到当前程序描述（RAM） */
    s_prog.bc     = s_bc;
    s_prog.bc_len = s_bc_len;
    s_prog.argc   = s_argc;
    s_prog.arg_len = s_arg_len;

    /* 启动执行 */
    s_pc      = 0u;
    s_fn_back = 0u;
    s_steps   = 0u;
    s_wait_cmd = NULL;
    s_abort   = 0u;
    s_ret     = 0u;
    s_busy    = 1u;
    return 1u;
}
#endif /* SCL_CFG_RUN_TEXT_EN */

#if (SCL_CFG_RUN_PROG_EN != 0u)
uint8_t SCL_RunProg(const scl_prog_t *prog)
{
    if (s_inited == 0u)
    {
        SCL_Init();
        if (s_inited == 0u) { return 0u; }
    }
    if (s_busy != 0u)
    {
        Scl_MsgErr("busy: 有脚本正在执行");
        return 0u;
    }
    if ((prog == NULL) || (prog->bc == NULL) || (prog->argc == NULL))
    {
        return 0u;
    }
    if ((prog->bc_len == 0u) || ((prog->bc_len % 4u) != 0u) || (prog->arg_len == 0u))
    {
        return 0u;   /* 非法程序描述 */
    }

    /* 装载只读程序（Flash const），运行期不占用字节码/参数缓存 RAM */
    s_prog = *prog;

    /* 启动执行 */
    s_pc      = 0u;
    s_fn_back = 0u;
    s_steps   = 0u;
    s_wait_cmd = NULL;
    s_abort   = 0u;
    s_ret     = 0u;
    s_busy    = 1u;
    return 1u;
}
#endif /* SCL_CFG_RUN_PROG_EN */

#if ((SCL_CFG_RUN_TEXT_EN) != 0u) || ((SCL_CFG_RUN_PROG_EN) != 0u)

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

#else
/* ============ mini 态：无字节码解释器；SCL_Loop 仅推进异步命令等待 ============ */

void SCL_Loop(void)
{
    if (s_inited == 0u)
    {
        SCL_Init();
    }
    (void)SCL_AsyncPoll();
}

uint8_t SCL_Idle(void)
{
    return (SCL_AsyncBusy() != 0u) ? 0u : 1u;
}

void SCL_Abort(void)
{
    (void)SCL_AsyncPoll();   /* 无解释器可中断；仅清等待 */
}

#endif

/* ============================ 脚本命令（s2c 编译产物注册成命令） ============================ */

#if ((SCL_CFG_SCMD_EN != 0u) && (SCL_CFG_RUN_PROG_EN != 0u))

void SCL_Scmd_Register(scl_scmd_t *cmd)
{
    if ((cmd == NULL) || (cmd->name == NULL) || (cmd->prog == NULL))
    {
        return;
    }
    cmd->next = s_scmd_head;
    s_scmd_head = cmd;
}

const scl_scmd_t *SCL_Scmd_Find(const char *name)
{
    scl_scmd_t *p;
    if (name == NULL)
    {
        return NULL;
    }
    for (p = s_scmd_head; p != NULL; p = p->next)
    {
        if (Scl_StrEq(p->name, name))
        {
            return p;
        }
    }
    return NULL;
}

/* 头遍历（供 help/补全展示；NULL 表示空） */
const scl_scmd_t *SCL_Scmd_Head(void)
{
    return s_scmd_head;
}

/* 格式化 "arg<n>" 变量名到 buf（idx < 1000） */
static void Scl_Scmd_ArgName(uint16_t idx, char *buf)
{
    uint16_t p = 3u;
    buf[0] = 'a'; buf[1] = 'r'; buf[2] = 'g';
    if (idx >= 100u) { buf[p++] = (char)('0' + (idx / 100u)); }
    if (idx >= 10u)  { buf[p++] = (char)('0' + ((idx / 10u) % 10u)); }
    buf[p++] = (char)('0' + (idx % 10u));
    buf[p] = '\0';
}

/* 执行一行 'name 参数...'：注入 argv 为 arg0..argN，再 SCL_RunProg。 */
uint8_t SCL_Scmd_RunText(const char *line)
{
    const scl_scmd_t *sc;
    const char *p;
    char  name[SCL_CFG_ARG_LEN_MAX];
    char  anm[SCL_CFG_VAR_NAME_MAX + 1u];
    char  abuf[SCL_CFG_ARG_LEN_MAX];
    uint16_t k;
    int   ai;

    if (s_inited == 0u)
    {
        SCL_Init();
    }
    if (line == NULL)
    {
        return 0u;
    }
    p = line;
    while ((*p != '\0') && Scl_IsSp(*p)) { p++; }
    if (*p == '\0') { return 0u; }
    k = 0u;
    while ((*p != '\0') && !Scl_IsSp(*p) && (k + 1u < (uint16_t)sizeof(name)))
    {
        name[k++] = *p++;
    }
    name[k] = '\0';

    sc = SCL_Scmd_Find(name);
    if (sc == NULL)
    {
        return 2u;                 /* 非脚本命令 → 调用方回退普通 SCL_Run */
    }
    if (s_busy != 0u)
    {
        Scl_MsgErr("busy: 有脚本正在执行");
        return 0u;
    }

    ai = 0;
    for (;;)
    {
        const char *seg;
        const char *segend;
        char q;
        int  isq;

        while ((*p != '\0') && Scl_IsSp(*p)) { p++; }
        if (*p == '\0') { break; }
        if (ai >= (int)SCL_CFG_VAR_MAX)
        {
            Scl_MsgErr("脚本命令参数过多（上限 SCL_CFG_VAR_MAX=%u）", SCL_CFG_VAR_MAX);
            break;
        }
        isq = 0;
        q = '\0';
        if ((*p == '"') || (*p == '\''))
        {
            q  = *p;
            p++;
            isq = 1;
        }
        seg = p;
        if (isq != 0)
        {
            while ((*p != '\0') && (*p != q)) { p++; }
            segend = p;
            if (*p == q) { p++; }          /* 越过闭合引号 */
        }
        else
        {
            while ((*p != '\0') && !Scl_IsSp(*p)) { p++; }
            segend = p;
        }
        /* 参数段 [seg, segend) → ${} 展开 → arg<ai> 注入 */
        if (Scl_ExpandCopy(seg, segend, abuf, (uint16_t)sizeof(abuf)) < 0)
        {
            abuf[0] = '\0';
        }
        Scl_Scmd_ArgName((uint16_t)ai, anm);
        if (SCL_VarSet(anm, abuf) != 0)
        {
            Scl_MsgErr("scmd: 注入参数变量 %s 失败（类型/超长/槽满）", anm);
        }
        ai++;
    }
    return SCL_RunProg(sc->prog);
}

#endif /* SCL_CFG_SCMD_EN && SCL_CFG_RUN_PROG_EN */
