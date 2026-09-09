/**
  ******************************************************************************
  * @file    scl_stm32_port.h
  * @brief   SCL → STM32(HAL) 移植层：声明（模板）
  *
  *          SCL 对 MCU 的最低要求仅是"输出单字符"（SCL_Port_PutChar）；
  *          env 固化需要"用户存储读/写"。本头把这些集中声明，实现见 scl_stm32_port.c。
  *
  *          使用：
  *            1) 把 SCL_MCU_HUART 改成你工程里的串口句柄（如 huart1 / huart2）；
  *            2) 若用内部 Flash 存 env，按你芯片改存储地址宏（见 .c 内样板）；
  *            3) main 里周期调用 SCL_Loop() 推进脚本即可。
  ******************************************************************************
  */

#ifndef __SCL_STM32_PORT_H__
#define __SCL_STM32_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "scl.h"

/* ==================== 用户按工程修改 ==================== */
/* CubeMX 生成的串口句柄（替换成你的，如 huart2） */
#include "main.h"               /* CubeMX 生成的声明：extern UART_HandleTypeDef huart1; 等 */
#define SCL_MCU_HUART   huart1

/* env 固化的"用户存储"读写接口（由你在 .c 里按 Flash/EEPROM 实现）：
   返回写入/读出的字节数；失败/越界返回 <=0 */
int  Scl_Store_Write(const uint8_t *buf, int len);
int  Scl_Store_Read(uint8_t *buf, int cap);

/* ==================== 本层导出 ==================== */

/* 库要求的输出单字符（实现 = HAL_UART_Transmit 阻塞发送） */
void SCL_Port_PutChar(char c);

#ifdef __cplusplus
}
#endif

#endif /* __SCL_STM32_PORT_H__ */
