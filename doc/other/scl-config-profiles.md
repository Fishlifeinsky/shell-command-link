# SCL v0.3：配置裁剪档模板（内存预算与推荐值）

> 类别：使用指南 · 文件：`scl/Inc/scl_cfg.h`

SCL 所有占用/能力宏集中在 `scl_cfg.h`（均 `#ifndef` 保护），
直接用编译命令行 `-DSCL_CFG_XXX=...` 覆盖即可，无需改库头。

## 1. 参数速查

| 宏 | 默认 | 含义 |
| --- | --- | --- |
| `SCL_CFG_VAR_MAX` | 4 | 变量槽数 |
| `SCL_CFG_VAR_NAME_MAX` | 8 | 变量名长 |
| `SCL_CFG_VAR_VALUE_MAX` | 16 | 变量值缓冲 |
| `SCL_CFG_SCRIPT_MAX` | 512 | 动态脚本文本上限 |
| `SCL_CFG_BC_MAX` | 512 | 字节码缓冲(4 倍数) |
| `SCL_CFG_ARG_CACHE_MAX` | 256 | 参数字节缓存 |
| `SCL_CFG_LABEL_MAX` | 16 | label 表 |
| `SCL_CFG_LABEL_NAME_MAX` | 16 | label 名长 |
| `SCL_CFG_STEP_LIMIT` | 100000 | 步数保护(0=关) |
| `SCL_CFG_ARG_MAX` | 8 | 单命令参数个数 |
| `SCL_CFG_ARG_LEN_MAX` | 32 | 单参数展开后长度 |
| `SCL_CFG_MSG_EN` | 1 | 消息输出(0=裁全部输出) |
| `SCL_CFG_RUN_TEXT_EN` | 1 | 动态文本编译(SCL_Run) |
| `SCL_CFG_RUN_PROG_EN` | 1 | 预编译只读程序(SCL_RunProg) |

## 2. 三档模板

### 档 A：最小 RAM —— 固定脚本（MCU 量产/上电自检）
```bash
-DSCL_CFG_RUN_TEXT_EN=0      # 裁动态编译器与 RAM 大缓存
-DSCL_CFG_RUN_PROG_EN=1      # 用 scl_emit_c 生成 const 程序 → SCL_RunProg
-DSCL_CFG_VAR_MAX=2 -DSCL_CFG_VAR_NAME_MAX=8 -DSCL_CFG_VAR_VALUE_MAX=16
-DSCL_CFG_ARG_MAX=4  -DSCL_CFG_ARG_LEN_MAX=24
-DSCL_CFG_MSG_EN=1           # 保留错误消息（调试期）；量产可改 0 再省 ROM
```
RAM 大致：仅变量表 + 运行工作缓冲（约几百 B），bc/参数/label/中间表全在 Flash/裁掉。
工具：`python tools/scl_emit_c.py boot.s2c -o boot_prog.c`（见 doc/arc/scl-const-prog.md）。

### 档 B：平衡 —— 少量动态脚本 + 交互 shell（MCU 常见）
```bash
-DSCL_CFG_RUN_TEXT_EN=1 -DSCL_CFG_RUN_PROG_EN=1
-DSCL_CFG_SCRIPT_MAX=256 -DSCL_CFG_BC_MAX=256 -DSCL_CFG_ARG_CACHE_MAX=128
-DSCL_CFG_LABEL_MAX=8 -DSCL_CFG_VAR_MAX=4
-DSCL_CFG_ARG_MAX=6 -DSCL_CFG_ARG_LEN_MAX=24
```
（可再开 `SCL_EX_SHELL_EN` 用 example/scl_shell 做串口 REPL。）

### 档 C：全功能 —— PC / 调试 / 演示（本仓库默认与测试）
```bash
-DSCL_CFG_SCRIPT_MAX=2048 -DSCL_CFG_BC_MAX=2048 -DSCL_CFG_ARG_CACHE_MAX=1024
-DSCL_CFG_LABEL_MAX=64 -DSCL_CFG_VAR_MAX=8
```
（scl_test / s2c_test 即用此档。）

## 3. 内存预算（大致，A/B/C 对照）

| 项 | A(RunProg 纯只读) | B(小动态) | C(全功能) |
| --- | --- | --- | --- |
| 字节码 bc | Flash(const) | 256B | 2048B |
| 参数缓存 argc | Flash(const) | 128B | 1024B |
| 中间指令表 ins | 无(编译器裁) | ~384B | ~1.5K+ |
| label 表 | 无 | ~256B | ~1.5K |
| 变量表(按 VAR_MAX×~24B) | ~48B | ~96B | ~192B |
| 命令工作缓冲(argv/raw) | ~小 | 按 ARG 裁剪 | ~600B |
| 编译/动态代码 ROM | 无 | 有 | 有 |

> 说明：A 档固定脚本的字节码/参数放 Flash（`scl_emit_c.py` 产物），
> 因此运行时 RAM 只剩变量 + 工作缓冲，是 MCU 上最省的一种用法。

## 4. 其它裁剪

- `SCL_CFG_MSG_EN=0`：库内全部提示整段裁掉，连 `SCL_Port_PutChar` 都可省略。
- 例程层开关：`SCL_EX_CMDS_EN`(demo 命令)、`SCL_EX_SHELL_EN`(交互 shell) 置 0 裁掉。
