/**
  ******************************************************************************
  * @file    b_env.c
  * @brief   big_demo：运行参数默认表（env，const 可放 Flash；由 C 注册）
  *          脚本（diagnose）经 ${tgt_ov}.. 读取 → 体现 C 配置 / 脚本消费。
  ******************************************************************************
  */

#include "scl.h"

#if (SCL_CFG_ENV_EN != 0u)

static const scl_env_def_t s_env_def[] = {
    { "tgt_ov", SCL_T_INT, "60"  },   /* 老化目标温度 ℃ */
    { "sw_step", SCL_T_INT, "120" },  /* 每件扫位步数 */
    { "buzzer", SCL_T_INT, "3"   },   /* 完成提示蜂鸣次数 */
};

void Big_Env_Register(void)
{
    Scl_Env_RegisterDefault(s_env_def,
                            (int)(sizeof(s_env_def) / sizeof(s_env_def[0])));
    Scl_Env_Reset();   /* 用默认表装载 env 缓冲（脚本 ${} / C SCL_VarGet 即可读） */
}

#endif /* SCL_CFG_ENV_EN */
