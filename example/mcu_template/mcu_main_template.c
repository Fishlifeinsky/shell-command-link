/**
  ******************************************************************************
  * @file    mcu_main_template.c
  * @brief   SCL → STM32 main 集成骨架（模板；并入 CubeMX 工程编译）
  *
  *          把它编译进 STM32 工程后，在 CubeMX 生成的 main() 里加入：
  *
  *             int main(void)
  *             {
  *                 HAL_Init(); SystemClock_Config(); MX_GPIO_Init(); MX_USARTx_UART_Init();
  *                 App_Scl_Init();              // <-- SCL 相关初始化（本文件）
  *                 Scl_Stm32_UartStartRx();     // <-- 串口中断接收（见 scl_stm32_port.c）
  *                 while (1)
  *                 {
  *                     App_Scl_Poll();          // <-- SCL_Loop + Shell 提示
  *                     App_Scl_Tick1s();        // <-- 例：周期性把改动后的 env 固化
  *                     /* ...你的其它周期任务... */
  *                 }
  *             }
  *
  *          本文件只写 SCL 侧逻辑，不碰具体外设（外设都经 scl_stm32_port 抽象）。
  ******************************************************************************
  */

#include "scl_stm32_port.h"     /* 内含 scl.h 与 SCL_MCU_HUART 等 */
#include "scl_shell.h"          /* 交互 Shell（可裁剪） */

/* ==================== 例：env 默认配置表（放 Flash 只读） ==================== */
static const scl_env_def_t s_env_defs[] = {
    { "mode", SCL_T_STR,  "auto"    },
    { "baud", SCL_T_INT,  "115200"  },
    { "log",  SCL_T_FLAG, "-v"      },
};

/* ==================== 例：上电自检常量程序（由 boot.s2c 经 scl_emit_c 生成） ====================
   生成（PC 上）：
     python tools/scl_emit_c.py example/mcu_template/boot.s2c -o build/boot_prog.c
   把 build/boot_prog.c 拷入工程后在此引用。若不用 const，也可直接用 SCL_Run 文本。 */
extern const scl_prog_t scl_boot_prog;    /* boot_prog.c 提供（名字可改） */

/* ==================== 初始化（上电/复位调用一次） ==================== */

void App_Scl_Init(void)
{
    /* 1) 库初始化 + 注册业务命令（各业务模块自己注册，见 SCL_CmdRegisterDesc） */
    SCL_Init();
    /* 示例：你的业务命令注册，例如 App_Cmds_Register(); */

    /* 2) 注册 env 默认配置并装载：
          - 用户存储里有固化数据 → Load 恢复（优先于默认）
          - 无数据/坏数据      → Reset 回退默认配置 */
    Scl_Env_RegisterDefault(s_env_defs, 3);
    {
        uint8_t store[512];
        int n = Scl_Store_Read(store, (int)sizeof(store));   /* 从 Flash/EEPROM 读回 */
        if (n > 0)
        {
            Scl_Env_Load(store, n);      /* 坏数据内部自动拒绝并留空（由上层回退默认） */
        }
        if (Scl_Env_Count() == 0)
        {
            Scl_Env_Reset();             /* 首次上电/坏存储：从默认配置装载 */
        }
    }

    /* 3) 交互 Shell（可裁剪；不需要交互可整段去掉） */
    Scl_Shell_Init(SCL_Port_PutChar);    /* 提示符输出走同一个 PutChar */

    /* 4) 上电自检：跑固定 const 程序（省 RAM；也可 SCL_Run 文本） */
    if (SCL_RunProg(&scl_boot_prog) == 0u)
    {
        /* 程序非法/忙中：串口输出错误（PutChar 已在库内打消息） */
    }
}

/* ==================== 主循环周期调用 ==================== */

void App_Scl_Poll(void)
{
    SCL_Loop();            /* 推进脚本（含异步命令轮询） */
    Scl_Shell_Poll();      /* 命令完成后打印提示符（无 Shell 则空实现） */
}

/* ==================== 周期任务：固化改动后的 env ==================== */

void App_Scl_Tick1s(void)
{
    /* 例：每秒把当前 env 缓冲固化一次（只有被改过才写，避免频繁擦 Flash）。
       生产建议：记录"脏"标志，仅在 Scl_Env_Set 后置位，这里只写一次并清标志。 */
    static uint8_t store[512];
    int n = Scl_Env_Save(store, (int)sizeof(store));
    if (n > 0)
    {
        Scl_Store_Write(store, n);       /* 写到 Flash/EEPROM */
    }
}
