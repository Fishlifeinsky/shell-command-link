/**
  ******************************************************************************
  * @file    cmd_demo_inc.c
  * @brief   demo_inc：计数 +1 并打印；G_RETURN = (计数 < 目标)
  ******************************************************************************
  */
#include "scl_reg.h"
#include "cmd_util.h"

static void Cmd_demo_inc(int argc, char *argv[]);

static void Cmd_demo_inc(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SclCmd_DemoCnt++;
    SclCmd_Talk("cnt=%d\n", SclCmd_DemoCnt);
    SCL_Ret_Set((SclCmd_DemoCnt < SclCmd_DemoTgt) ? 1 : 0);
}

SCL_CMD_DEFINE_NA(demo_inc, Cmd_demo_inc, NULL, "计数+1 并置 G_RETURN");
