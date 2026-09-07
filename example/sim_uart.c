/**
  ******************************************************************************
  * @file    sim_uart.c
  * @brief   PC 模拟"MCU + 串口"：把本机终端当成 MCU 的 UART
  *
  *          运行本程序后，可像连着 MCU 串口一样逐键输入 SCL 链式命令：
  *            - 回车执行；←退格删除；↑/↓ 历史；Tab 补全命令名/保留字/${变量}
  *            - 内置命令：help / var / free / label / jump / echo / setret ...
  *            - 输入 quit 或 exit 退出
  *
  *          工作原理（模拟一个 MCU 主循环）：
  *            UART RX（键盘/stdin 单字符） → Scl_Shell_Feed()   逐字节
  *            SCL_Loop()                                       推进脚本
  *            Scl_Shell_Poll()                                 打印提示
  *            UART TX（屏幕/stdout）         ← SCL_Port_PutChar 逐字符
  *
  *          MCU 真实移植：把"键盘读"换成 UART 接收中断/轮询喂 Scl_Shell_Feed，
  *                        SCL_Port_PutChar 换成 UART 发送即可。
  *
  *          编译（Windows/MinGW）：
  *            gcc -O2 -pipe -Wall -Wextra -I scl/Inc -I example \
  *                scl/Src/scl.c example/scl_port.c example/demo_cmds.c \
  *                example/scl_shell.c example/sim_uart.c -o build/sim_uart
  *          （Linux）:
  *            gcc -O2 -pipe -Wall -Wextra -I scl/Inc -I example \
  *                scl/Src/scl.c example/scl_port.c example/demo_cmds.c \
  *                example/scl_shell.c example/sim_uart.c -o build/sim_uart
  ******************************************************************************
  */

#include <stdio.h>

#include "scl.h"
#include "scl_port.h"
#include "demo_cmds.h"
#include "scl_shell.h"

#if (SCL_EX_SHELL_EN == 1u)

#ifdef _WIN32
#include <conio.h>

/* Windows：conio 键盘（方向键为 0/0xE0 前缀 + 扫描码） */
static int Sim_GetKey(void)
{
    if (!_kbhit())
    {
        return -1;
    }
    {
        int c = _getch();
        if ((c == 0) || (c == 0xE0))
        {
            int k = _getch();
            if (k == 72) { return -100; }   /* ↑ */
            if (k == 80) { return -101; }   /* ↓ */
            return -1;                      /* 其它功能键忽略 */
        }
        return c;
    }
}

#else
#include <termios.h>
#include <unistd.h>
#include <poll.h>

/* POSIX：stdin 置 raw 模式逐字符读（方向键由 Scl_Shell 内部解析 ANSI CSI） */
static struct termios Sim_old;

static void Sim_RawOn(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &Sim_old);
    t = Sim_old;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void Sim_RawOff(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &Sim_old);
}

static int Sim_GetKey(void)
{
    struct pollfd pfd;
    unsigned char b;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) <= 0)
    {
        return -1;
    }
    if (read(STDIN_FILENO, &b, 1u) != 1)
    {
        return -1;
    }
    return (int)b;
}
#endif /* _WIN32 */

static void Sim_FeedDirUp(void)
{
    Scl_Shell_Feed(0x1Bu);
    Scl_Shell_Feed('[');
    Scl_Shell_Feed('A');
}

static void Sim_FeedDirDown(void)
{
    Scl_Shell_Feed(0x1Bu);
    Scl_Shell_Feed('[');
    Scl_Shell_Feed('B');
}

int main(void)
{
    SCL_Init();
    Scl_Demo_Register();
    Scl_Shell_Init(SCL_Port_PutChar);   /* 输出 = 串口 TX（本演示走 stdout） */

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n=== SCL 交互终端（模拟 MCU 串口）===\n");
    printf("命令: echo/var/help/free/label/jump/setret/demo_inc/wait ...\n");
    printf("      Tab 补全 · ↑/↓ 历史 · quit 退出\n");

#ifndef _WIN32
    Sim_RawOn();
#endif

    /* ---- MCU 主循环：轮询串口 RX + 推进库 ---- */
    for (;;)
    {
        int k;
        while ((k = Sim_GetKey()) >= 0)
        {
            Scl_Shell_Feed(k);          /* UART RX 1 字节 → Shell */
        }
        if (k == -100)
        {
            Sim_FeedDirUp();            /* ↑ 已由平台层归一化 */
        }
        else if (k == -101)
        {
            Sim_FeedDirDown();
        }
        SCL_Loop();                     /* 推进脚本（含异步命令） */
        Scl_Shell_Poll();               /* 完成 → 打印提示符 */
        if (Scl_Shell_QuitReq())
        {
            printf("\nbye\r\n");
            break;
        }
    }

#ifndef _WIN32
    Sim_RawOff();
#endif
    return 0;
}

#else /* SCL_EX_SHELL_EN == 0 */
int main(void)
{
    printf("SCL_EX_SHELL_EN=0（交互 Shell 已裁剪）\n");
    return 0;
}
#endif
