/**
  ******************************************************************************
  * @file    demo_cmds.c
  * @brief   PC 示例：基础演示命令（可裁剪 SCL_EX_CMDS_EN=0 裁掉）
  *
  *          提供命令（演示 + 测试用，全部同步/异步两类示例）：
  *            - echo <text...>   同步：打印参数（验证展开/函数式/引号）
  *            - setret <0|1>     同步：显式写 G_RETURN
  *            - cmp <a> <b>      同步：G_RETURN = (a==b)（if 用）
  *            - add <a> <b>      同步：打印 a+b（函数式 add(a,b) 用）
  *            - noop             同步：什么都不做（性能/压力用）
  *            - demo_reset [n]   同步：计数清零并设目标（默认 3）
  *            - demo_inc         同步：计数+1 并打印；G_RETURN=(计数<目标)
  *            - wait <n>         异步：模拟耗时操作，n 个 Loop 后完成并置 G_RETURN=真
  ******************************************************************************
  */

#include "demo_cmds.h"

#if (SCL_EX_CMDS_EN == 1u)

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>   /* atoi */
#include <stdbool.h>
#include "scl.h"
#include "scl_port.h"

/* ========================== 演示输出（走 SCL_Port_PutChar，可被测试捕获） ========================== */

static void DemoTalk(const char *fmt, ...)
{
    char    b[128];
    va_list ap;
    int     i;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 1)
    {
        n = (int)sizeof(b) - 1;
    }
    for (i = 0; i < n; i++)
    {
        SCL_Port_PutChar(b[i]);
    }
}

/* ========================== 命令节点静态定义 ========================== */

static void Cmd_echo(int argc, char *argv[]);
static void Cmd_setret(int argc, char *argv[]);
static void Cmd_cmp(int argc, char *argv[]);
static void Cmd_add(int argc, char *argv[]);
static void Cmd_noop(int argc, char *argv[]);
static void Cmd_demo_reset(int argc, char *argv[]);
static void Cmd_demo_inc(int argc, char *argv[]);
static void Cmd_wait(int argc, char *argv[]);
static bool Sync_wait(bool clear);

static scl_cmd_t s_cmd_echo       = { "echo",       Cmd_echo,       NULL, NULL };
static scl_cmd_t s_cmd_setret     = { "setret",     Cmd_setret,     NULL, NULL };
static scl_cmd_t s_cmd_cmp        = { "cmp",        Cmd_cmp,        NULL, NULL };
static scl_cmd_t s_cmd_add        = { "add",        Cmd_add,        NULL, NULL };
static scl_cmd_t s_cmd_noop       = { "noop",       Cmd_noop,       NULL, NULL };
static scl_cmd_t s_cmd_demo_reset = { "demo_reset", Cmd_demo_reset, NULL, NULL };
static scl_cmd_t s_cmd_demo_inc   = { "demo_inc",   Cmd_demo_inc,   NULL, NULL };
static scl_cmd_t s_cmd_wait       = { "wait",       Cmd_wait,       Sync_wait, NULL };

/* ========================== 演示命令状态 ========================== */

static int s_demo_cnt   = 0;   /* demo_inc 计数 */
static int s_demo_tgt   = 3;   /* demo_inc 目标 */
static int s_wait_remain = 0;  /* wait 剩余 Loop 数 */

/* ========================== 命令实现 ========================== */

/* echo：打印每个参数（空格分隔） */
static void Cmd_echo(int argc, char *argv[])
{
    int i;
    DemoTalk("echo");
    for (i = 0; i < argc; i++)
    {
        DemoTalk(" %s", argv[i]);
    }
    DemoTalk("\n");
}

/* setret：显式写 G_RETURN */
static void Cmd_setret(int argc, char *argv[])
{
    (void)argc;
    SCL_Ret_Set((argv[0] != NULL) ? atoi(argv[0]) : 0);
}

/* cmp：G_RETURN = (a==b) */
static void Cmd_cmp(int argc, char *argv[])
{
    (void)argc;
    SCL_Ret_Set((atoi(argv[0]) == atoi(argv[1])) ? 1 : 0);
}

/* add：打印 a+b（支持 add(2,3) 函数式） */
static void Cmd_add(int argc, char *argv[])
{
    (void)argc;
    DemoTalk("add=%d\n", atoi(argv[0]) + atoi(argv[1]));
}

/* noop：什么都不做（同步压力测试） */
static void Cmd_noop(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
}

/* demo_reset [n]：计数清零并设目标（默认 3） */
static void Cmd_demo_reset(int argc, char *argv[])
{
    s_demo_cnt = 0;
    s_demo_tgt = (argc > 0) ? atoi(argv[0]) : 3;
    if (s_demo_tgt < 0) { s_demo_tgt = 0; }
}

/* demo_inc：计数+1 打印；G_RETURN = (计数 < 目标) */
static void Cmd_demo_inc(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    s_demo_cnt++;
    DemoTalk("cnt=%d\n", s_demo_cnt);
    SCL_Ret_Set((s_demo_cnt < s_demo_tgt) ? 1 : 0);
}

/* wait：异步示例——handler 立即返回，模拟耗时 s_wait_remain 个 Loop */
static void Cmd_wait(int argc, char *argv[])
{
    (void)argc;
    s_wait_remain = atoi(argv[0]);
    if (s_wait_remain < 0) { s_wait_remain = 0; }
    SCL_Ret_Set(0);   /* 完成前为假 */
}

/* wait 的同步信号：每 Loop 减一，减到 0 即完成，完成后置 G_RETURN=真 */
static bool Sync_wait(bool clear)
{
    if (clear)
    {
        return true;
    }
    if (s_wait_remain > 0)
    {
        s_wait_remain--;
        return false;
    }
    SCL_Ret_Set(1);   /* 完成 → G_RETURN=真（供后续 if -t 判断） */
    return true;
}

/* ========================== 注册 ========================== */

void Scl_Demo_Register(void)
{
    SCL_RegisterCmd(&s_cmd_echo);
    SCL_RegisterCmd(&s_cmd_setret);
    SCL_RegisterCmd(&s_cmd_cmp);
    SCL_RegisterCmd(&s_cmd_add);
    SCL_RegisterCmd(&s_cmd_noop);
    SCL_RegisterCmd(&s_cmd_demo_reset);
    SCL_RegisterCmd(&s_cmd_demo_inc);
    SCL_RegisterCmd(&s_cmd_wait);
}

#endif /* SCL_EX_CMDS_EN */
