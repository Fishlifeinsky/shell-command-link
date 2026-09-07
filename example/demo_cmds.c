/**
  ******************************************************************************
  * @file    demo_cmds.c
  * @brief   PC 示例：基础演示命令（可裁剪 SCL_EX_CMDS_EN=0 裁掉）
  *
  *          提供命令（演示 + 测试用，全部同步/异步两类示例）：
  *            - echo <text...>   同步：打印参数（验证展开/引号/类型化参数）
  *            - setret <0|1>     同步：显式写 G_RETURN（S2C ret 糖目标）
  *            - noop             同步：什么都不做（性能/压力用）
  *            - demo_reset [n]   同步：计数清零并设目标（默认 3）
  *            - demo_inc         同步：计数+1 并打印；G_RETURN=(计数<目标)
  *            - wait <n>         异步：模拟耗时操作，n 个 Loop 后完成并置 G_RETURN=真
  *          （int 运算/比较由库内置指令承担：iadd/isub/.../ieq/ilt 等，见 scl.h）
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
static void Cmd_noop(int argc, char *argv[]);
static void Cmd_demo_reset(int argc, char *argv[]);
static void Cmd_demo_inc(int argc, char *argv[]);
static void Cmd_wait(int argc, char *argv[]);
static bool Sync_wait(bool clear);

/* 命令节点（静态存储；字段由注册函数填充，name/fn/sync 不随脚本生命周期变化） */
static scl_cmd_t s_cmd_echo;
static scl_cmd_t s_cmd_setret;
static scl_cmd_t s_cmd_noop;
static scl_cmd_t s_cmd_demo_reset;
static scl_cmd_t s_cmd_demo_inc;
static scl_cmd_t s_cmd_wait;

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

#if (SCL_CFG_CMDDESC_EN != 0u)
/* 命令描述（argtable3 风格：help + 参数模板，注册后自动校验/usage/help 汇总） */
static const scl_arg_spec_t a_setret_val[] = { { "value", SCL_T_INT, 0u, "0/1，写 G_RETURN" } };
static const scl_arg_spec_t a_reset_n[]    = { { "n",     SCL_T_INT, 1u, "目标计数（默认 3）" } };
static const scl_arg_spec_t a_wait_n[]     = { { "n",     SCL_T_INT, 0u, "模拟耗时 Loop 数" } };

static const scl_cmd_desc_t s_desc_echo       = { "echo",       "打印参数（支持展开）", NULL, 0, Cmd_echo,       NULL };
static const scl_cmd_desc_t s_desc_setret     = { "setret",     "写条件标志 G_RETURN", a_setret_val, 1, Cmd_setret,     NULL };
static const scl_cmd_desc_t s_desc_noop       = { "noop",       "空操作（压力/空跑）", NULL, 0, Cmd_noop,       NULL };
static const scl_cmd_desc_t s_desc_demo_reset = { "demo_reset", "计数清零并设目标",     a_reset_n, 1, Cmd_demo_reset, NULL };
static const scl_cmd_desc_t s_desc_demo_inc   = { "demo_inc",   "计数+1 并置 G_RETURN", NULL, 0, Cmd_demo_inc,   NULL };
static const scl_cmd_desc_t s_desc_wait       = { "wait",       "模拟耗时（异步）",     a_wait_n, 1, Cmd_wait,       Sync_wait };
#endif

void Scl_Demo_Register(void)
{
#if (SCL_CFG_CMDDESC_EN != 0u)
    SCL_CmdRegisterDesc(&s_cmd_echo, &s_desc_echo);
    SCL_CmdRegisterDesc(&s_cmd_setret, &s_desc_setret);
    SCL_CmdRegisterDesc(&s_cmd_noop, &s_desc_noop);
    SCL_CmdRegisterDesc(&s_cmd_demo_reset, &s_desc_demo_reset);
    SCL_CmdRegisterDesc(&s_cmd_demo_inc, &s_desc_demo_inc);
    SCL_CmdRegisterDesc(&s_cmd_wait, &s_desc_wait);
#else
    /* CMDDESC 关闭：退回普通注册 */
    s_cmd_echo.name = "echo";        s_cmd_echo.fn = Cmd_echo;        s_cmd_echo.sync = NULL;
    s_cmd_setret.name = "setret";    s_cmd_setret.fn = Cmd_setret;    s_cmd_setret.sync = NULL;
    s_cmd_noop.name = "noop";        s_cmd_noop.fn = Cmd_noop;        s_cmd_noop.sync = NULL;
    s_cmd_demo_reset.name = "demo_reset"; s_cmd_demo_reset.fn = Cmd_demo_reset;
    s_cmd_demo_reset.sync = NULL;
    s_cmd_demo_inc.name = "demo_inc"; s_cmd_demo_inc.fn = Cmd_demo_inc; s_cmd_demo_inc.sync = NULL;
    s_cmd_wait.name = "wait";        s_cmd_wait.fn = Cmd_wait;        s_cmd_wait.sync = Sync_wait;
    SCL_RegisterCmd(&s_cmd_echo);
    SCL_RegisterCmd(&s_cmd_setret);
    SCL_RegisterCmd(&s_cmd_noop);
    SCL_RegisterCmd(&s_cmd_demo_reset);
    SCL_RegisterCmd(&s_cmd_demo_inc);
    SCL_RegisterCmd(&s_cmd_wait);
#endif
}

#endif /* SCL_EX_CMDS_EN */
