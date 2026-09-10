/**
  ******************************************************************************
  * @file    scl_priv.h
  * @brief   SCL 内部共享头（多模块拆分用；库用户勿依赖）
  *
  *          v0.3 源码按主题拆分为多个编译单元（全部必须一起编译链接）：
  *            scl/Src/scl.c      —— 核心：编译/执行/内置命令/初始化编排
  *            scl/Src/scl_cmd.c  —— 命令注册表、按名查找、编程式调用与异步、一行解析
  *            scl/Src/scl_core.c —— 文本/数值小工具 + 消息输出（无 libc）
  *            scl/Src/scl_mem.c  —— 动态内存分配器与用量统计（SCL_CFG_DYNAMIC_MEM_EN）
  *            scl/Src/scl_var.c  —— 会话变量表与管理（SCL_CFG_VAR_*）
  *            scl/Src/scl_env.c  —— 环境变量缓冲（SCL_CFG_ENV_EN）
  *
  *          拆分进度与验收（行为零变化）见 doc/idea/scl-module-split.md。
  *
  *          本头声明跨文件共享的类型/状态/内部函数；其余内部符号仍为 static。
  *          公共 API 见 scl.h（scl.c 拆分的对外行为不变）。
  ******************************************************************************
  */

#ifndef __SCL_PRIV_H__
#define __SCL_PRIV_H__

#include "scl.h"

/* ========================== 共享类型 ========================== */

/* 变量/env 表项（会话变量与 env 同构：名 + 类型 + 规范化文本值） */
typedef struct
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
  char    *name;
  char    *value;
  uint16_t name_cap;
  uint16_t value_cap;
#else
    char     name[SCL_CFG_VAR_NAME_MAX + 1u];
    char     value[SCL_CFG_VAR_VALUE_MAX]; /* 规范化文本 */
#endif
  uint8_t  type;                        /* SCL_T_BOOL/INT/FLAG/STR */
    uint8_t  used;
    uint8_t  ro;                          /* 1=只读常量（const）：不可覆盖/free/作为写回目标 */
} scl_var_t;

/* ========================== 共享状态（各模块定义） ========================== */

#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
extern scl_var_t *s_vars;                              /* scl_var.c 定义 */
#else
extern scl_var_t s_vars[SCL_CFG_VAR_MAX];              /* scl_var.c 定义 */
#endif

extern uint8_t s_keep_vars;                            /* scl.c 定义；scl_var.c 写 */

/* 命令链表与异步等待（逻辑在 scl_cmd.c，定义仍在 scl.c 以保证 InitEx 重置简单） */
extern scl_cmd_t *s_cmd_head;                          /* 命令链表头 */
extern uint16_t   s_next_opc;                          /* 手工注册的 opcode 递增游标 */
extern scl_cmd_t *s_wait_cmd;                          /* 正在异步等待的命令 */

/* 参数工作缓冲（scl_cmd.c 的命令直调路径与 scl.c 的编译链共用）。
   s_raw / s_cmdname 只在 scl.c 内使用，故留在那里保持 static（未用档位可自动消除）。 */
#define SCL_RAW_MAX      64u     /* 元指令参数原文上限（含 '\0'） */
#define SCL_CMDNAME_MAX  32u     /* CALLN 命令名缓冲长度（含 '\0'） */
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
extern char   (*s_argb)[SCL_CFG_ARG_LEN_MAX];
extern char   **s_argv;
extern uint8_t *s_argt;
#else
extern char     s_argb[SCL_CFG_ARG_MAX][SCL_CFG_ARG_LEN_MAX];
extern char    *s_argv[SCL_CFG_ARG_MAX];
extern uint8_t  s_argt[SCL_CFG_ARG_MAX];
#endif

#if (SCL_CFG_ENV_EN != 0u)
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
extern scl_var_t *s_env;                               /* scl_env.c 定义 */
#else
extern scl_var_t s_env[SCL_CFG_ENV_MAX];               /* scl_env.c 定义 */
#endif
extern const scl_env_def_t *s_env_def;                 /* scl_env.c 定义 */
extern uint16_t            s_env_def_n;
#endif

/* ========================== 文本/数值/消息（scl_core.c 提供） ========================== */

uint16_t Scl_StrLen(const char *s);
uint8_t  Scl_IsSp(char c);
uint8_t  Scl_IsNm(char c);
uint8_t  Scl_EqN(const char *a, const char *b, uint16_t n);
uint8_t  Scl_StrEq(const char *a, const char *b);
char     Scl_Up(char c);
uint8_t  Scl_IsAl(char c);
uint8_t  Scl_IsDigit(char c);
uint8_t  Scl_EqIN(const char *a, const char *b, uint16_t n);
int      Scl_ParseI32Len(const char *s, uint16_t len, int32_t *out);
uint16_t Scl_FmtI32(int32_t val, char *dst, uint16_t cap);

/* 消息输出（级别门控与实现见 scl_core.c）。
   关闭消息时在本头内联定义**空实现**，而非宏：
     - 保留实参求值语义（用宏会让实参消失，触发 -Wunused-but-set-variable）
     - static inline 空体可被完全消除 → 调用点零开销（等价于拆分前的同 TU 内联）
     - static inline 未被使用时不会产生 -Wunused-function */
#if (SCL_CFG_MSG_EN != 0u)
void     Scl_Msg(const char *fmt, ...);
void     Scl_MsgErr(const char *fmt, ...);
#else
static inline void Scl_Msg(const char *fmt, ...)    { (void)fmt; }
static inline void Scl_MsgErr(const char *fmt, ...) { (void)fmt; }
#endif

/* ========================== 命令注册与调用（scl_cmd.c 提供） ========================== */

scl_cmd_t *Scl_CmdFindName(const char *name, uint16_t len);

#if (SCL_CFG_CMDDESC_EN != 0u)
/* 命令参数模板校验：返回 0=通过；非 0=拒绝（已打印提示） */
int        Scl_DescCheck(const scl_cmd_t *nd, int argc);
#endif

/* ========================== 会话变量内部（scl_var.c 提供） ========================== */

int      Scl_VarNameOk(const char *name);   /* env 装载时复用 */
uint8_t  Scl_VarInit(void);
void     Scl_VarShutdown(void);
int      Scl_VarNorm(uint8_t type, const char *val, char *out, uint16_t cap);
int32_t  Scl_VarNum(const char *name, uint8_t *ok);     /* core 算术/比较用 */
uint8_t  Scl_VarTruth(const char *name);                /* core btest/布尔用 */
uint8_t  Scl_VarGc(void);
uint8_t  Scl_VarGcZombie(void);

#if (SCL_CFG_ENV_EN != 0u)
uint8_t Scl_EnvInit(void);
void    Scl_EnvShutdown(void);
#endif

void     Scl_MemSetAllocator(const scl_allocator_t *a);
void    *Scl_MemAlloc(size_t size);
void    *Scl_MemRealloc(void *ptr, size_t size);
void     Scl_MemFree(void *ptr);
void     Scl_MemGcCount(void);
size_t   Scl_MemCurrent(void);
size_t   Scl_MemPeak(void);
size_t   Scl_MemCapacity(void);
uint32_t Scl_MemAllocCount(void);
uint32_t Scl_MemFreeCount(void);
uint32_t Scl_MemGcTotal(void);

#endif /* __SCL_PRIV_H__ */
