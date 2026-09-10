/**
  ******************************************************************************
  * @file    cmd_setret.c
  * @brief   setret <0|1>：显式写条件标志 G_RETURN（S2C ret 糖的目标）
  ******************************************************************************
  */
#include "scl_reg.h"

#include <stdlib.h>   /* atoi */

static void Cmd_setret(int argc, char *argv[]);
static const scl_arg_spec_t a_setret_val[] = { { "value", SCL_T_INT, 0u, "0/1，写 G_RETURN" } };

static void Cmd_setret(int argc, char *argv[])
{
    (void)argc;
    SCL_Ret_Set((argv[0] != NULL) ? atoi(argv[0]) : 0);
}

SCL_CMD_DEFINE(setret, Cmd_setret, NULL, "写条件标志 G_RETURN", a_setret_val);
