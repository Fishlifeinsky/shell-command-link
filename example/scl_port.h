/**
  ******************************************************************************
  * @file    scl_port.h
  * @brief   PC 示例：SCL 移植接口 + 测试捕获辅助
  *
  *          移植要求（库对用户唯一要求）：实现 void SCL_Port_PutChar(char c)
  *          用于库内消息输出；SCL_CFG_MSG_EN=0 时可完全省略。
  *          本文件（示例层）提供捕获能力，便于断言库输出。
  ******************************************************************************
  */

#ifndef __SCL_PORT_H__
#define __SCL_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 移植：输出单字符（库消息 + 演示命令共用，同时写 stdout 与捕获缓冲） */
void SCL_Port_PutChar(char c);

/* 测试辅助：开启/关闭捕获（库与演示输出同时写入 buf，便于断言） */
void Scl_CapBegin(char *buf, int cap);
void Scl_CapEnd(void);
int  Scl_CapLen(void);

#ifdef __cplusplus
}
#endif

#endif /* __SCL_PORT_H__ */
