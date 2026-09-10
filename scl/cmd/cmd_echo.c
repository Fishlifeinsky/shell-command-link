/**
  ******************************************************************************
  * @file    cmd_echo.c
  * @brief   echo <text...>：同步打印参数（验证展开/引号/类型化参数）
  * @note    scl/cmd/ 下一个命令一个文件；注册靠 SCL_CMD_DEFINE 宏 + 生成表
  ******************************************************************************
  */
#include "scl_reg.h"
#include "cmd_util.h"

static void Cmd_echo(int argc, char *argv[]);

static void Cmd_echo(int argc, char *argv[])
{
    int i;

    SclCmd_Talk("echo");
    for (i = 0; i < argc; i++)
    {
        SclCmd_Talk(" %s", argv[i]);
    }
    SclCmd_Talk("\n");
}

/* 参数个数不定 → 不带参数模板（运行时不需要 desc 校验） */
SCL_CMD_DEFINE_NA(echo, Cmd_echo, NULL, "打印参数（支持展开）");
