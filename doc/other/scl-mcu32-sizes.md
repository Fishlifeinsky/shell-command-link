# SCL 库 32 位资源占用（ARM Cortex-M，实测）

> 类别：测试报告（other） · 测量日期：2026-09-08
> 工具链：Arm GNU Toolchain 14.2.Rel1（arm-none-eabi-gcc 14.2.1），目标 Cortex-M4 / Thumb-2
> 编译命令（以默认档为例，产物为 .o 后解析 ELF 节表）：
> ```
> arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -std=c99 -O2 -Wall -Wextra \
>     -I scl/Inc -I scl/Src -c scl/Src/scl.c -o scl.o
>     （scl_var.c / scl_env.c 同）
> ```
> 测量：Python 解析 ELF 节表（本机 binutils 非白名单读不到明文产物）。

口径：
- Flash(ROM) = .text(代码) + .rodata(常量) + .data(初值，烧录进 Flash、运行时拷 RAM)
- RAM(运行态) = .data + .bss（不含应用栈/堆；库全程无 malloc）

## 1. 默认全功能配置（scl_cfg.h 默认值，-O2，2026-09-08 优化后基线）

| 模块 | .text | .rodata | .data | .bss | Flash | RAM |
|------|------:|--------:|------:|-----:|------:|-----:|
| scl.o（核心：编译/执行/命令/异步/内置 help/desc） | 10 304 | 4 200 | 2 | 1 532 | 14 506 | 1 534 |
| scl_var.o（会话变量） | 1 496 | 18 | 0 | 112 | 1 514 | 112 |
| scl_env.o（环境变量缓冲） | 1 028 | 0 | 0 | 230 | 1 028 | 230 |
| **库合计** | 12 828 | 4 218 | 2 | 1 874 | **17 048** | **1 876** |

## 2. 各裁剪档对比（库合计 Flash/RAM，由 `tools/scl_build.py sizes` 复现）

| 配置 | Flash | RAM |
|------|------:|-----:|
| 默认全功能（-O2） | 17 048 B | 1 876 B |
| 默认（-Os，省 Flash） | 13 751 B | 1 874 B |
| 档 B 平衡·小动态（-O2） | 17 028 B | 1 212 B |
| 档 A 最小 RAM·固定脚本（-O2） | 14 028 B | 540 B |
| 档 A + 关消息 `MSG_EN=0`（量产） | **7 769 B** | **540 B** |

档位宏集合见 `doc/other/scl-config-profiles.md`；模块分解见上表三模块。

## 3. 说明

- **RAM 大头是 .bss 运行缓冲**：2026-09-08 优化后默认档已从 3.6 KB 降到 ≈1.9 KB——
  主要省项：去掉编译中间指令表 `s_ins`（两遍重扫文本，省 ~1.5 KB）、元指令缓冲
  `SCL_RAW_MAX` 256→64（省 192 B）。裁成档 A（`SCL_CFG_RUN_TEXT_EN=0` 走 `SCL_RunProg`
  const 程序，编译器与 RAM 大缓存整段裁掉）RAM 再降至 ≈0.5 KB。
- **ROM 优化**：关消息（`MSG_EN=0`）连 help 全览/文档表一并裁掉，档 A 上 Flash
  从 ≈9.7 KB 降到 **≈7.8 KB**（.rodata 大幅下降）；`-Os` 较 `-O2` 默认档省 ≈3.3 KB Flash。
- 变量表：档 A(VAR_MAX=2) ≈56 B、档 B/默认(VAR_MAX=4) ≈112 B（scl_var.o .bss）。
- env 缓冲：按 `SCL_CFG_ENV_MAX`(默认 8)×槽大小在 scl_env.o .bss（此处 230 B）。
- 不含帧栈/中断栈（由应用分配）；库函数调用链无动态内存。

## 4. 可复现

一键：`python tools/scl_build.py sizes`（自动探测 arm-none-eabi，编译各档 .o 并解析 ELF 节表）；
`python tools/scl_build.py check` 可复查各裁剪开关零告警。本机 binutils 非白名单读不到明文，
故用 Python 解析 ELF 节表。PC 侧 x86-64 对照与速度/可靠性报告见 `doc/other/scl-test-report.md`
（x86 指令密度高于 Thumb-2，MCU 实际代码体积通常更小）。

> 附：测量中发现并修复一处可移植性缺陷 —— 代码用 `NULL` 但仅依赖 stdio.h 间接提供；
> 严格工具链（arm-none-eabi + -std=c99）下未声明。已在 `scl/Inc/scl.h` 补 `#include <stddef.h>`。
