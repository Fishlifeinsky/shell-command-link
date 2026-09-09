/**
  ******************************************************************************
  * @file    scl_priv.h
  * @brief   SCL 内部共享头（多模块拆分用；库用户勿依赖）
  *
  *          v0.3 源码按主题拆分为三个编译单元（全部必须一起编译链接）：
  *            scl/Src/scl.c      —— 核心：编译/执行/命令注册/异步/内置命令/文本工具
  *            scl/Src/scl_var.c  —— 会话变量表与管理（SCL_CFG_VAR_*）
  *            scl/Src/scl_env.c  —— 环境变量缓冲（SCL_CFG_ENV_EN）
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

#if (SCL_CFG_ENV_EN != 0u)
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
extern scl_var_t *s_env;                               /* scl_env.c 定义 */
#else
extern scl_var_t s_env[SCL_CFG_ENV_MAX];               /* scl_env.c 定义 */
#endif
extern const scl_env_def_t *s_env_def;                 /* scl_env.c 定义 */
extern uint16_t            s_env_def_n;
#endif

/* ========================== 文本/数值工具（scl.c 提供） ========================== */

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
