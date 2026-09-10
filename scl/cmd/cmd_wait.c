/**
  ******************************************************************************
  * @file    cmd_wait.c
  * @brief   wait <n>：异步示例——handler 立即返回，n 个 Loop 后完成并置 G_RETURN=真
  * @note    sync 回调非 NULL 时，库把命令登记为"异步等待"（SCL_CmdInvoke 返回 2，
  *          由 SCL_AsyncPoll/SCL_Loop 推进），是异步命令的参考实现
  ******************************************************************************
  */
#include "scl_reg.h"
#include "cmd_util.h"

#include <stdlib.h>   /* atoi */
#include <stdbool.h>

static void Cmd_wait(int argc, char *argv[]);
static bool Sync_wait(bool clear);
#if (SCL_CFG_CMDDESC_EN != 0u)
static const scl_arg_spec_t a_wait_n[] = { { "n", SCL_T_INT, 0u, "模拟耗时 Loop 数" } };
#endif

/* handler：立即发起（记录剩余 Loop 数）后返回 */
static void Cmd_wait(int argc, char *argv[])
{
    (void)argc;
    SclCmd_WaitRemain = atoi(argv[0]);
    if (SclCmd_WaitRemain < 0)
    {
        SclCmd_WaitRemain = 0;
    }
    SCL_Ret_Set(0);   /* 完成前为假 */
}

/* 同步信号：每 Loop 减一，减到 0 即完成，完成后置 G_RETURN=真 */
static bool Sync_wait(bool clear)
{
    if (clear)
    {
        return true;
    }
    if (SclCmd_WaitRemain > 0)
    {
        SclCmd_WaitRemain--;
        return false;
    }
    SCL_Ret_Set(1);
    return true;
}

SCL_CMD_DEFINE(wait, Cmd_wait, Sync_wait, "模拟耗时（异步）", a_wait_n);
