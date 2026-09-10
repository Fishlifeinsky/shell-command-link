/**
  ******************************************************************************
  * @file    cmd_demo_reset.c
  * @brief   demo_reset [n]：计数清零并设目标（默认 3）
  ******************************************************************************
  */
#include "scl_reg.h"
#include "cmd_util.h"

#include <stdlib.h>   /* atoi */

static void Cmd_demo_reset(int argc, char *argv[]);
static const scl_arg_spec_t a_reset_n[] = { { "n", SCL_T_INT, 1u, "目标计数（默认 3）" } };

static void Cmd_demo_reset(int argc, char *argv[])
{
    SclCmd_DemoCnt = 0;
    SclCmd_DemoTgt = (argc > 0) ? atoi(argv[0]) : 3;
    if (SclCmd_DemoTgt < 0)
    {
        SclCmd_DemoTgt = 0;
    }
}

SCL_CMD_DEFINE(demo_reset, Cmd_demo_reset, NULL, "计数清零并设目标", a_reset_n);
