/**
  ******************************************************************************
  * @file    demo_cmds.h
  * @brief   PC 示例：基础演示命令（可裁剪）
  ******************************************************************************
  */

#ifndef __DEMO_CMDS_H__
#define __DEMO_CMDS_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 演示命令总开关：0 可整体裁掉（此时库无任何业务命令也可跑测试） */
#ifndef SCL_EX_CMDS_EN
#define SCL_EX_CMDS_EN 1u
#endif

#if (SCL_EX_CMDS_EN == 1u)
void Scl_Demo_Register(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DEMO_CMDS_H__ */
