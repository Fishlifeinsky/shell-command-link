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

## 1. 默认全功能配置（scl_cfg.h 默认值，-O2）

| 模块 | .text | .rodata | .data | .bss | Flash | RAM |
|------|------:|--------:|------:|-----:|------:|-----:|
| scl.o（核心：编译/执行/命令/异步/内置 help/desc） | 10 352 | 4 216 | 2 | 3 260 | 14 570 | 3 262 |
| scl_var.o（会话变量） | 1 496 | 18 | 0 | 112 | 1 514 | 112 |
| scl_env.o（环境变量缓冲） | 1 028 | 0 | 0 | 230 | 1 028 | 230 |
| **库合计** | 12 876 | 4 234 | 2 | 3 602 | **17 112** | **3 604** |

## 2. 各裁剪档对比（库合计）

| 配置 | .text | .rodata | .data | .bss | Flash | RAM |
|------|------:|--------:|------:|-----:|------:|-----:|
| 默认全功能（-O2） | 12 876 | 4 234 | 2 | 3 602 | 17 112 | 3 604 |
| 默认（-Os，省 Flash） | 9 672 | 3 997 | 2 | 3 602 | 13 671 | 3 604 |
| 档 B 平衡·小动态（-O2） | 12 864 | 4 234 | 2 | 2 170 | 17 100 | 2 172 |
| 档 A 最小 RAM·固定脚本（-O2） | 10 404 | 3 654 | 2 | 730 | 14 060 | 732 |
| 档 A + 关消息 `MSG_EN=0`（量产） | 8 180 | 1 490 | 2 | 730 | **9 672** | **732** |

档位宏集合见 `doc/other/scl-config-profiles.md`；对象级别各 .o 组成即上面三模块。

## 3. 说明

- **RAM 大头是 .bss 运行缓冲**：默认档（脚本 512 + 字节码 512 + 参数缓存 256 + label/中间表/argv 等）≈3.6 KB；
  裁成档 A（`SCL_CFG_RUN_TEXT_EN=0` 走 `SCL_RunProg` const 程序，编译器与 RAM 大缓存整段裁掉）RAM 降至 ≈0.7 KB。
- **ROM 大头**：关消息（`SCL_CFG_MSG_EN=0`）在档 A 上再省 ≈4.4 KB Flash（文本 .rodata 大降）；`-Os` 较 `-O2` 默认档省 ≈3.4 KB Flash。
- 变量表：档 A(VAR_MAX=2) ≈56 B、档 B/默认(VAR_MAX=4) ≈112 B（scl_var.o .bss）。
- env 缓冲：按 `SCL_CFG_ENV_MAX`(默认 8)×槽大小在 scl_env.o .bss（此处 230 B）。
- 不含帧栈/中断栈（由应用分配）；库函数调用链无动态内存。

## 4. 可复现

各档 .o 已可自行编译后解析；PC 侧 x86-64 对照与速度/可靠性报告见 `doc/other/scl-test-report.md`
（x86 指令密度高于 Thumb-2，MCU 实际代码体积通常更小）。

> 附：测量中发现并修复一处可移植性缺陷 —— 代码用 `NULL` 但仅依赖 stdio.h 间接提供；
> 严格工具链（arm-none-eabi + -std=c99）下未声明。已在 `scl/Inc/scl.h` 补 `#include <stddef.h>`。
