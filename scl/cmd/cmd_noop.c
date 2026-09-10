/**
  ******************************************************************************
  * @file    cmd_noop.c
  * @brief   noop：空操作（压力/空跑测试）
  ******************************************************************************
  */
#include "scl_reg.h"

static void Cmd_noop(int argc, char *argv[]);

static void Cmd_noop(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
}

SCL_CMD_DEFINE_NA(noop, Cmd_noop, NULL, "空操作（压力/空跑）");
