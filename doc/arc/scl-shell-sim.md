# SCL v0.3：交互 Shell 与 MCU 串口模拟（会话变量 VarKeep）

> 类别：架构/示例 · 状态：已实现（v0.3）
> 文件：`example/scl_shell.{h,c}`、`example/sim_uart.c`、`example/main.c`(第 7 节)

## 1. 是什么

给 SCL 配一个**串口命令行 Shell**（REPL），像连终端一样逐条输入链式命令：
- 行编辑：回车执行、Backspace 删尾、Ctrl-U 清行
- 历史：`↑`/`↓`（ANSI `ESC[A/B`）浏览与重放
- `Tab` 补全：行首补**注册命令名/保留字**；`${` 开头补**变量名**
- 输入 `quit` / `exit` 置退出请求
- busy（上一条命令未执行完）时输入新命令 → 忽略并提示，保证串口语义不乱

Shell 面向 **MCU 串口**设计：串口每收到 1 字节调 `Scl_Shell_Feed()`，
主循环调 `Scl_Shell_Poll()`；全部输出走初始化回调（通常就是 UART 发送）。
无 malloc / 无 libc，静态内存。

## 2. 会话变量：`SCL_VarKeep()`

SCL 默认语义：**一次脚本跑完自动释放全部变量**（适合一次性烧录脚本）。
但交互终端里用户期望 `var int a=1` 之后，后续每条命令都能用 `${a}` ——
即变量属于"会话"，跨命令存活，直到显式 `free`。

为此库新增：
```c
int SCL_VarKeep(int keep);   /* 1=脚本结束不再自动释放变量；返回旧值；默认 0 */
```
`Scl_Shell_Init()` 内部置 `SCL_VarKeep(1)`（会话模式）；`free` 仍可显式释放。
一次性 `SCL_Run` 调用方不受影响（默认 keep=0，跑完自动清空）。

## 3. 接口

```c
void Scl_Shell_Init(void (*out)(char));  /* out 通常是 SCL_Port_PutChar / UART 发送 */
void Scl_Shell_Feed(int ch);             /* 串口 RX 1 字节 */
void Scl_Shell_Poll(void);               /* 主循环周期调用（命令完成补打印提示符） */
int  Scl_Shell_QuitReq(void);            /* quit/exit 后返回 1 */
```
- 裁剪：`SCL_EX_SHELL_EN=0` 整段裁掉；容量宏 `SCL_EX_SHELL_LINE_MAX`(96)、
  `SCL_EX_SHELL_HIST_MAX`(8)。
- 依赖：`SCL_CFG_RUN_TEXT_EN=1`（Shell 用 `SCL_Run` 执行）；补全用到
  `SCL_CmdHead()` / `SCL_VarEnum()`。

配套新增只读 API（scl.h）：
- `const scl_cmd_t *SCL_CmdHead(void);`  命令链表遍历（补全/调试）
- `int SCL_VarEnum(int idx, char *name, int cap);` 变量名遍历

## 4. MCU 串口模拟（example/sim_uart.c）

把 PC 终端当"MCU 的串口"来体验：
```bash
gcc -O2 -pipe -Wall -Wextra -I scl/Inc -I example \
    scl/Src/scl.c example/scl_port.c example/demo_cmds.c \
    example/scl_shell.c example/sim_uart.c -o build/sim_uart
./build/sim_uart
```
运行后像连上 MCU 一样逐键输入：`echo hi`、`var int n=5`、`echo n=${n}`、
`help`、`demo_reset 3;demo_inc`、Tab 补全、↑ 历史、`quit` 退出。
Windows 用 conio 取键；Linux 用 raw 终端。方向键统一归一化成 ANSI 序列喂 Shell。

**MCU 真实移植**只需替换 I/O 来源：
- RX：UART 接收中断/轮询 → `Scl_Shell_Feed(byte)`
- TX：`SCL_Port_PutChar` 实现为 UART 发送（库消息与 Shell 共用）

自动化验证（example/main.c 第 7 节）：用**注入字节流**模拟串口对话
（执行/变量跨命令/三种 Tab 补全/历史 ↑ 重放/busy 忽略/quit），
如同把一串串口抓包喂给 MCU。基线：scl_test PASS=127。

### Tab 补全与 CMDDESC 联动（`SCL_CFG_CMDDESC_EN=1` 时）

- **多候选逐行带 desc**：Tab 出多个候选时每个候选一行；若候选是注册命令且带
  `desc->help`，行尾附“ — 帮助文本”（如 `demo_reset — 计数清零并设目标`），
  关键字/变量候选保持原名输出。
- **参数位 usage 提示**：命令行已处于参数位置（首命令完整）且该命令带参数模板时，
  按 Tab 只提示 `usage: <cmd> <name:type>…` 一行（不插入文本、不改行），
  供回忆参数名/顺序；无模板命令静默。

两者辅助函数（`Sh_EqN/Sh_FindCmd/Sh_TypeName/Sh_PrintUsage`）仅在 CMDDESC=1 时编译，
关宏不影响 Shell 其它功能。

## 5. 相关

- 预编译只读程序（更省 RAM 的固定脚本方案）：`doc/arc/scl-const-prog.md`
- 命令描述/`help <cmd>` 明细：`doc/arc/scl-cmddesc.md`
- MCU(STM32) 移植模板：`example/mcu_template/README.md`
- 配置裁剪档：见 `scl_cfg.h` 与 doc/other/scl-config-profiles.md
