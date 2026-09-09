/**
  ******************************************************************************
  * @file    b_cmds.h
  * @brief   big_demo：业务命令库头（含共享打印 B_Talk 与分组注册入口）
  *
  *          命令分层：
  *            Big_CmdDev_Register()  设备命令（老化炉/电机/IO）— b_cmds1.c
  *            Big_CmdFlow_Register() 批次/统计/流程/工具  — b_cmds2.c
  *          需要 SCL_CFG_CMDDESC_EN=1（默认开；desc 提供参数模板校验/help/usage）
  ******************************************************************************
  */

#ifndef __B_CMDS_H__
#define __B_CMDS_H__

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "scl.h"
#include "scl_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本演示命令总开关：0 可整体裁掉 */
#ifndef SCL_EX_BIG_EN
#define SCL_EX_BIG_EN 1u
#endif

#if (SCL_EX_BIG_EN == 1u)

/* ---- 共享打印：走 SCL_Port_PutChar（同时进捕获缓冲，便于断言） ---- */
static inline void B_Talk(const char *fmt, ...)
{
    char    b[128];
    va_list ap;
    int     i, n;

    va_start(ap, fmt);
    n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 1) { n = (int)sizeof(b) - 1; }
    for (i = 0; i < n; i++)
    {
        SCL_Port_PutChar(b[i]);
    }
}

/* 无 libc 字符串比较（全等，含结尾） */
static inline int B_Eq(const char *a, const char *b)
{
    if (a == b) { return 1; }
    if ((a == 0) || (b == 0)) { return 0; }
    while ((*a != '\0') && (*b != '\0'))
    {
        if (*a != *b) { return 0; }
        a++;
        b++;
    }
    return (*a == *b) ? 1 : 0;
}

/* 设备命令（老化炉/电机/IO）；流程命令（批次/统计/校准/诊断/工具） */
void Big_CmdDev_Register(void);
void Big_CmdFlow_Register(void);

#endif /* SCL_EX_BIG_EN */

#ifdef __cplusplus
}
#endif

#endif /* __B_CMDS_H__ */
