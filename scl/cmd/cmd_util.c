/**
  ******************************************************************************
  * @file    cmd_util.c
  * @brief   scl/cmd/ 下命令的公共助手实现
  ******************************************************************************
  */
#include "cmd_util.h"
#include "scl.h"

#include <stdarg.h>
#include <stdio.h>

/* 移植接口：与 scl.c 内部声明的同一符号（用户实现的单字符输出） */
extern void SCL_Port_PutChar(char c);

/* 演示命令共享状态定义（其它命令文件按 extern 使用） */
int SclCmd_DemoCnt = 0;
int SclCmd_DemoTgt = 3;
int SclCmd_WaitRemain = 0;

void SclCmd_Talk(const char *fmt, ...)
{
    char    b[128];
    va_list ap;
    int     n;
    int     i;

    va_start(ap, fmt);
    n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0)
    {
        return;
    }
    if (n > (int)sizeof(b) - 1)
    {
        n = (int)sizeof(b) - 1;
    }
    for (i = 0; i < n; i++)
    {
        SCL_Port_PutChar(b[i]);
    }
}
