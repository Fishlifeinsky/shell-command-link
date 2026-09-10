/**
  ******************************************************************************
  * @file    cmd_util.h
  * @brief   scl/cmd/ 下命令的公共助手（格式化输出）
  ******************************************************************************
  */
#ifndef __SCL_CMD_UTIL_H__
#define __SCL_CMD_UTIL_H__

/**
  * @brief  按格式输出文本（内部格式化后逐字符走 SCL_Port_PutChar）
  * @note   走移植层单字符输出，测试/串口都能捕获；不需要 libc 的重定向
  */
void SclCmd_Talk(const char *fmt, ...);

/* 演示命令共享状态（cmd_demo_reset.c 定义，cmd_demo_inc.c 使用） */
extern int SclCmd_DemoCnt;
extern int SclCmd_DemoTgt;

/* wait 命令的异步剩余计数（cmd_wait.c 定义/使用） */
extern int SclCmd_WaitRemain;

#endif /* __SCL_CMD_UTIL_H__ */
