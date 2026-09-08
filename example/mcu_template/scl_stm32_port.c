/**
  ******************************************************************************
  * @file    scl_stm32_port.c
  * @brief   SCL → STM32(HAL) 移植层实现（模板；需并入 STM32 工程编译）
  *
  *          内容：
  *            1) SCL_Port_PutChar —— 库消息输出（阻塞串口发送）
  *            2) Scl_Stm32_UartStartRx / HAL_UART_RxCpltCallback
  *               —— 单字节中断接收 → Scl_Shell_Feed（收完自动续收）
  *            3) Scl_Store_Write/Read —— env 固化的"用户存储"样板
  *               （内部 Flash 单扇区示意，按芯片/需求替换）
  *
  *          说明：模板按 STM32F1/F4 常见 HAL 编写；具体串口/Flash 型号不同时
  *          只改本文件两处（句柄宏在 .h、Flash 地址/擦除扇区在下方）即可。
  ******************************************************************************
  */

#include "scl_stm32_port.h"

/* ---- 可选：仅当跑交互 Shell 时需要；不跑 Shell 可裁掉这段 ---- */
#if (SCL_EX_SHELL_EN == 1u)
#include "scl_shell.h"
#endif

/* ==================== 1. 输出单字符（库要求） ==================== */

void SCL_Port_PutChar(char c)
{
    /* 阻塞发 1 字节。shell 输出量小，简单可靠；如需非阻塞可换 DMA+标志 */
    HAL_UART_Transmit(&SCL_MCU_HUART, (uint8_t *)&c, 1u, 10u);
}

/* ==================== 2. 串口中断接收 → Shell ==================== */

/* 接收缓冲（单字节续收即可；Shell 内部自组行/历史） */
static volatile uint8_t s_rx_byte = 0u;

void Scl_Stm32_UartStartRx(void)
{
    /* 启动一次单字节接收；完成后在回调里续收 */
    HAL_UART_Receive_IT(&SCL_MCU_HUART, (uint8_t *)&s_rx_byte, 1u);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart != &SCL_MCU_HUART)
    {
        return;
    }
#if (SCL_EX_SHELL_EN == 1u)
    Scl_Shell_Feed((int)s_rx_byte);   /* 逐字节喂交互 Shell */
#endif
    HAL_UART_Receive_IT(&SCL_MCU_HUART, (uint8_t *)&s_rx_byte, 1u);   /* 续收 */
}

/* ==================== 3. env 固化：用户存储（样板） ==================== */

/* 按你的芯片与分区改：env 数据固定放一个 Flash 扇区（此处以 F1 高地址示意）。
   生产建议：写前先备份旧值→擦除→编程；容量不足/掉电用"魔数+长度+校验"自愈
   （SCL 侧 Scl_Env_Save/Load 已带魔数/版本/长度校验，坏数据 Load 会拒绝回退默认）。 */
#define SCL_MCU_STORE_ADDR   0x0803F000u   /* 例：内部 Flash 最后 4KB 扇区 */
#define SCL_MCU_STORE_MAX    1024u         /* 例：单扇区内最多放 1KB env 数据 */

int Scl_Store_Write(const uint8_t *buf, int len)
{
    /* TODO(用户)：本示例不实际烧 Flash，避免模板误操作你的板子。
       真实实现参考（F1/F4 HAL）：
         uint32_t n = 0; FLASH_EraseInitTypeDef er;
         if (len <= 0 || len > SCL_MCU_STORE_MAX) return -1;
         HAL_FLASH_Unlock();
         er.TypeErase = FLASH_TYPEERASE_PAGES; er.PageAddress = SCL_MCU_STORE_ADDR;
         er.NbPages = 1; HAL_FLASHEx_Erase(&er, &n);
         for (int i = 0; i < len; i += 2)
             HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, SCL_MCU_STORE_ADDR + i,
                               (uint16_t)(buf[i] | (uint16_t)buf[i + 1] << 8));
         HAL_FLASH_Lock();
       */
    (void)buf;
    (void)len;
    return -1;   /* 未实现时返回 <=0，调用方不应误以为已固化 */
}

int Scl_Store_Read(uint8_t *buf, int cap)
{
    /* TODO(用户)：把 Flash 数据拷回 buf（先读前若干字节看是否被写过）。
       示例：memcpy(buf, (const void *)SCL_MCU_STORE_ADDR, cap)；返回 cap。 */
    (void)buf;
    (void)cap;
    return -1;   /* 未实现时返回 <=0，调用方回退默认配置 */
}
