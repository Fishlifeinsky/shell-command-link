/**
  ******************************************************************************
  * @file    b_sys.h
  * @brief   big_demo：系统级注册聚合（env 默认表 + 脚本命令）
  *          供 main.c 一次性初始化，避免重复 extern。
  ******************************************************************************
  */

#ifndef __B_SYS_H__
#define __B_SYS_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 env 默认运行参数表（tgt_ov/sw_step/buzzer，脚本 diagnose 读取） */
void Big_Env_Register(void);

/* 注册全部脚本命令（gen/ 下 scl_emit_c 生成的 selftest/profile/run_lot/diagnose） */
void Big_Scmd_RegisterAll(void);

#ifdef __cplusplus
}
#endif

#endif /* __B_SYS_H__ */
