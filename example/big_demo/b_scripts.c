/**
  ******************************************************************************
  * @file    b_scripts.c
  * @brief   big_demo：聚合注册由 scl_emit_c --cmd 生成的脚本命令。
  *          gen/sc_*.c 由 tools/scl_emit_c.py 生成（const 程序 + 尾 free +
  *          scl_scmd_t 节点 + Scl_Scmd_Register_<name>），勿手改。
  ******************************************************************************
  */

#include "scl.h"
#include "b_sys.h"

#if ((SCL_CFG_SCMD_EN != 0u) && (SCL_CFG_RUN_PROG_EN != 0u))

/* 生成器提供的注册函数（见 gen/sc_*.c 尾部） */
void Scl_Scmd_Register_selftest(void);
void Scl_Scmd_Register_profile(void);
void Scl_Scmd_Register_run_lot(void);
void Scl_Scmd_Register_diagnose(void);

void Big_Scmd_RegisterAll(void)
{
    Scl_Scmd_Register_selftest();
    Scl_Scmd_Register_profile();
    Scl_Scmd_Register_run_lot();
    Scl_Scmd_Register_diagnose();
}

#endif
