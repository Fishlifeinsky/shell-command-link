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

/* ========================== opcode 常量 ==========================
   内建指令 opcode 枚举已随编译/执行核移入 scl_exec.c（本文件不再直接使用）。 */

/* ========================== 静态状态 ========================== */

uint8_t s_inited = 0u;             /* 首次自动初始化标记（scl_exec.c 的 Run/Loop 读） */

/* ---- 命令链表（注册即自动分配 opcode） ----
   非 static：注册/查找/调用逻辑在 scl_cmd.c（声明见 scl_priv.h） ---- */
scl_cmd_t *s_cmd_head = NULL;
uint16_t   s_next_opc  = (uint16_t)SCL_CFG_OP_CMD_BASE;

/* ---- 脚本命令链表（s_scmd_head）：随 SCL_Scmd_* 移入 scl_exec.c（extern 见 scl_priv.h） ---- */


/* ---- 编译/执行态（s_prog / s_bc / s_argc / s_labels / s_pc / s_fn_back / s_steps）----
   已整段随执行核移入 scl_exec.c，并由 Scl_ExecInit/Release/Reset 管理。 */

/* ---- 条件标志 G_RETURN（执行核读写，故此处定义 + scl_priv.h 声明） ---- */
uint8_t s_ret = 0u;

/* 会话变量保留标记：置 1 后脚本结束不自动释放变量（供交互 shell/长会话）
   （scl_var.c 的 VarKeep 写、此处 Finish 读） */
uint8_t s_keep_vars = 0u;

/* ---- 执行状态（执行核读写，故非 static；声明见 scl_priv.h） ---- */
uint8_t  s_busy = 0u;
volatile uint8_t s_abort = 0u;
scl_cmd_t *s_wait_cmd = NULL;                /* 正在异步等待的命令（scl_cmd.c 读写） */

/* ---- 动态内存分配与统计 ----
   实现已移至 scl/Src/scl_mem.c；本文件只保留调用点。
   SCL_InitEx 通过 Scl_MemSetAllocator() 注入分配器并清零统计。 */

/* ---- 参数工作缓冲 ----
   s_argb/s_argv/s_argt 由命令直调（scl_cmd.c）与编译/执行（scl_exec.c）共用
   → 外部链接（声明见 scl_priv.h）；容量宏也在那里。 */
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
char (*s_argb)[SCL_CFG_ARG_LEN_MAX] = NULL;
char **s_argv = NULL;
uint8_t *s_argt = NULL;
#else
char s_argb[SCL_CFG_ARG_MAX][SCL_CFG_ARG_LEN_MAX];
char *s_argv[SCL_CFG_ARG_MAX];
uint8_t s_argt[SCL_CFG_ARG_MAX];       /* 当前命令各参数 type（SCL_ArgType 用） */
#endif

/* 文本/数值小工具实现见 scl/Src/scl_core.c（原型在 scl_priv.h） */

/* Scl_LitType（字面量归类）随编译链移入 scl/Src/scl_exec.c。 */

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

/* Scl_RetTake（G_RETURN 取值并清零）随执行核移入 scl/Src/scl_exec.c。 */

/* 命令注册表、按名查找、编程式调用（含异步）、一行解析 SCL_RunLine 均已移至
   scl/Src/scl_cmd.c；按 opcode 查找与 ${} 展开则随执行核移入 scl/Src/scl_exec.c。 */

/* ===================== 参数字节缓存：type 块编解码（v0.2） =====================
   写入原语（BlkPut* / Area* / ArgStore*）与读回原语、编译链已整体移入
   scl/Src/scl_exec.c（同一编译单元，避免参数缓存内部原语被迫外置）。 */

/* 参数字节缓存读回（Scl_ArgLoad / Scl_BlkText / Scl_ArgRestoreAt / Scl_DoCallName）
   随执行核移入 scl/Src/scl_exec.c。 */

/* ============ 编译链（保留字表 / 子句切分 / 两遍编译） ============
   已整体移入 scl/Src/scl_exec.c（与执行器同一编译单元，见该文件头说明）。 */

#if (SCL_CFG_RUN_TEXT_EN != 0u)
/* （内置运算保留字表、Scl_OpWord、Scl_NextClause、Scl_Compile 见 scl_exec.c） */

/* 编译（两遍直接扫描文本，不保留中间指令表）与子句切分 Scl_NextClause
   已移至 scl/Src/scl_exec.c。 */

/* Scl_Compile（两遍扫描产生字节码）实现见 scl/Src/scl_exec.c。 */
#endif /* SCL_CFG_RUN_TEXT_EN */

/* ========================== 内置命令：help / var / free ==========================
   Scl_TypeOfName、help 输出（DoHelp/帮助表/HelpDocFind/HelpOne/DoHelpRaw）
   与 var/free/cache 指令实现均随执行核移入 scl/Src/scl_exec.c。 */

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

/* 命令描述层的实现（usage 行 / 参数模板校验 / 描述注册）在 scl/Src/scl_desc.c；
   help 专用的明细打印随 help 一起在 scl/Src/scl_exec.c。 */

/* 变量列表与 var / free / cache 指令的实现随内置命令移入 scl/Src/scl_exec.c。 */

/* Scl_Finish（收尾：清执行态/释放变量/G_RETURN）随执行核移入 scl/Src/scl_exec.c。 */

/* 内置运算指令执行（算术/比较/字符串/布尔）与字节码解释器 Scl_StepOnce
   随执行核移入 scl/Src/scl_exec.c。 */

/* ========================== 公共接口实现 ========================== */

#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
static void Scl_DynamicRelease(void)
{
    Scl_EnvShutdown();
    Scl_VarShutdown();
    Scl_ExecRelease();                    /* 执行核缓冲（s_bc/s_argc/s_labels/s_raw/s_cmdname） */
    Scl_MemFree(s_argb);  s_argb = NULL;  /* 参数工作缓冲（命令直调与编译/执行共用） */
    Scl_MemFree(s_argv);  s_argv = NULL;
    Scl_MemFree(s_argt);  s_argt = NULL;
}
#endif /* SCL_CFG_DYNAMIC_MEM_EN */

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
    /* 执行核缓冲（s_bc/s_argc/s_labels/s_raw/s_cmdname）由执行核自行分配 */
    if ((Scl_ExecInit() == 0u) ||
        ((s_argb = (char (*)[SCL_CFG_ARG_LEN_MAX])Scl_MemAlloc(SCL_CFG_ARG_BUF_BYTES)) == NULL) ||
        ((s_argv = (char **)Scl_MemAlloc(sizeof(char *) * SCL_CFG_ARG_MAX)) == NULL) ||
        ((s_argt = (uint8_t *)Scl_MemAlloc(SCL_CFG_ARG_MAX)) == NULL) ||
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
    s_next_opc  = (uint16_t)(SCL_CFG_OP_CMD_BASE + SCL_CFG_CMD_RESERVE);
#if ((SCL_CFG_SCMD_EN != 0u) && (SCL_CFG_RUN_PROG_EN != 0u))
    s_scmd_head = NULL;
#endif
    s_busy      = 0u;
    s_abort     = 0u;
    s_wait_cmd  = NULL;
    Scl_ExecReset();          /* 执行核态（s_prog/s_pc/s_fn_back/s_steps 与各缓冲长度） */
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

/* SCL_Run（动态文本编译并启动）与 SCL_RunProg（装载 const 程序并启动）
   随执行核移入 scl/Src/scl_exec.c。 */

#if ((SCL_CFG_RUN_TEXT_EN) != 0u) || ((SCL_CFG_RUN_PROG_EN) != 0u)

/* 普通态：SCL_Loop / SCL_Idle / SCL_Abort 与解释器同属执行核（要直接操作执行态
   并调用 Scl_Finish），故随执行核移入 scl/Src/scl_exec.c。 */

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

/* ============================ 脚本命令（s2c 编译产物注册成命令） ============================
   实现在 scl/Src/scl_exec.c（要用执行的 ${} 展开与 SCL_RunProg）；s_scmd_head 也在那里定义。 */
