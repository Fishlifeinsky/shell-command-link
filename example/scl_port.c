/**
  ******************************************************************************
  * @file    scl_port.c
  * @brief   PC 示例：SCL 移植接口实现（PutChar -> stdout + 可选捕获）
  ******************************************************************************
  */

#include <stdio.h>
#include "scl_port.h"

/* ---- 捕获缓冲状态 ---- */
static char *g_cap     = NULL;   /* 捕获缓冲（NULL=未捕获） */
static int   g_cap_i   = 0;      /* 已写字节数 */
static int   g_cap_cap = 0;      /* 捕获缓冲容量 */

void SCL_Port_PutChar(char c)
{
    /* 捕获 */
    if ((g_cap != NULL) && (g_cap_i < g_cap_cap - 1))
    {
        g_cap[g_cap_i] = c;
        g_cap_i++;
        g_cap[g_cap_i] = '\0';
    }
    /* 输出到 stdout */
    putchar(c);
}

void Scl_CapBegin(char *buf, int cap)
{
    g_cap     = buf;
    g_cap_i   = 0;
    g_cap_cap = cap;
    if (cap > 0)
    {
        buf[0] = '\0';
    }
}

void Scl_CapEnd(void)
{
    g_cap     = NULL;
    g_cap_i   = 0;
    g_cap_cap = 0;
}

int Scl_CapLen(void)
{
    return g_cap_i;
}
