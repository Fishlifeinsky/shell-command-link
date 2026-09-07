/**
  ******************************************************************************
  * @file    scl_shell.h
  * @brief   SCL 交互式命令 Shell（REPL：行编辑 + 历史 + Tab 补全）
  *
  *          example 层、可裁剪（SCL_EX_SHELL_EN=0 裁掉）。面向 MCU 串口：
  *            串口每收到 1 字节 → Scl_Shell_Feed(ch)；
  *            主循环周期调 Scl_Shell_Poll()（负责命令完成后打印提示符）。
  *            输出全部经初始化时给出的 out 回调（MCU 上通常是 UART 发送）。
  *
  *          按键支持（面向 ANSI 终端 / 串口助手）：
  *            Enter(CR/LF) 执行；Backspace(0x08/0x7F) 删尾；Ctrl-U(0x15) 清行；
  *            ↑(ESC[A) / ↓(ESC[B) 历史；Tab 补全（命令名/保留字/${变量}）。
  *            行 "quit" / "exit" 置退出请求（Scl_Shell_QuitReq 可查）。
  *
  *          注意：Shell 用 SCL_Run() 执行链式命令，要求 SCL_CFG_RUN_TEXT_EN=1；
  *                命令执行中（busy）回车的新命令会被忽略并提示。
  *                Shell 为"会话模式"：初始化时置 SCL_VarKeep(1)，var 定义的变量
  *                跨命令保留（可用 free 显式释放；库复位/关 keep 才整体清空）。
  *
  *          内存：静态（行缓冲 SCL_EX_SHELL_LINE_MAX、历史
  *                SCL_EX_SHELL_HIST_MAX × 行长），无 malloc / 无 libc。
  ******************************************************************************
  */

#ifndef __SCL_SHELL_H__
#define __SCL_SHELL_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 演示 Shell 总开关：0 可整体裁掉（不编译实现，也无 RAM） */
#ifndef SCL_EX_SHELL_EN
#define SCL_EX_SHELL_EN 1u
#endif

#if (SCL_EX_SHELL_EN == 1u)

/* 行缓冲最大长度（含结尾 '\0'） */
#ifndef SCL_EX_SHELL_LINE_MAX
#define SCL_EX_SHELL_LINE_MAX    96u
#endif

/* 历史条数（环形：满时丢弃最旧） */
#ifndef SCL_EX_SHELL_HIST_MAX
#define SCL_EX_SHELL_HIST_MAX    8u
#endif

/**
  * @brief  初始化 Shell（清历史/行；打印一次提示符）
  * @param  out 输出单字符回调（可为 SCL_Port_PutChar 或 MCU UART 发送）
  */
void Scl_Shell_Init(void (*out)(char));

/**
  * @brief  输入 1 字节（串口中断/轮询每收 1 字节调用一次）
  * @param  ch 收到的字节（可打印字符或编辑控制键）
  */
void Scl_Shell_Feed(int ch);

/**
  * @brief  主循环周期调用（命令执行完成后自动打印提示符）
  * @note   必须由用户主循环同时周期调用 SCL_Loop() 推进脚本
  */
void Scl_Shell_Poll(void);

/**
  * @brief  查询是否收到退出请求（输入 quit / exit）
  * @retval 1=请求退出；0=无
  */
int Scl_Shell_QuitReq(void);

#endif /* SCL_EX_SHELL_EN */

#ifdef __cplusplus
}
#endif

#endif /* __SCL_SHELL_H__ */
