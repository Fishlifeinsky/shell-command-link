# SCL v0.3：预编译只读程序（s2c → C，减少 RAM）

> 类别：架构设计 · 状态：已实现（v0.3）· 文件：`scl/Src/scl.c`、`tools/scl_emit_c.py`

## 1. 要解决的问题

SCL 常规用法 `SCL_Run(文本)` 在运行时把脚本文本**编译进 RAM**：

| 动态项 | 默认大小 | 说明 |
| --- | --- | --- |
| `s_bc` 字节码 | `SCL_CFG_BC_MAX`(512B) | 每条指令 4B |
| `s_argc` 参数缓存 | `SCL_CFG_ARG_CACHE_MAX`(256B) | type 块 |
| `s_ins` 中间指令 | `BC_MAX/4` 条 × ~12B | 编译中间表 |
| `s_labels` label 表 | `LABEL_MAX` × ~24B | 编译期用 |
| 变量表 + 工作缓冲 | 小 | 运行必需 |

对 **MCU**：脚本通常是固定的（上电自检序列 / 生产测试 / 演示流程），
每次运行时重新在 RAM 编译是浪费 —— 若脚本不变，编译产物本该只读、可放 Flash。

## 2. 方案：Flash 预编译只读程序

把脚本**在开发期（PC 工具）编译成 const 数组**，随固件烧进 Flash；
运行时 `SCL_RunProg()` 直接解释，**不占字节码/参数缓存/label/中间表 RAM**。

```c
/* 由 tools/scl_emit_c.py 生成 */
static const uint8_t scl_boot_bc[]   = { ... };   /* Flash：字节码 */
static const uint8_t scl_boot_argc[] = { ... };   /* Flash：参数缓存 */
const scl_prog_t scl_boot_prog = {
    scl_boot_bc, sizeof(scl_boot_bc),
    scl_boot_argc, sizeof(scl_boot_argc)
};

/* 用户代码（命令注册完成后启动） */
SCL_Init();
Scl_Demo_Register();            /* 注册脚本用到的业务命令 */
SCL_RunProg(&scl_boot_prog);    /* 之后周期调用 SCL_Loop() 至 SCL_Idle() */
```

### 关键设计点

1. **执行器统一走"当前程序"描述 `scl_prog_t`**
   动态（`SCL_Run`）编译产物挂到 `s_prog`；预编译程序（`SCL_RunProg`）直接把
   const 程序挂上。解释器只读 `s_prog`，因此动态/只读共用同一执行路径。

2. **label 在编译期回填为绝对字节偏移**
   `jump` 指令的 argOff 直接是目标指令偏移（运行时跳转不需查表）。
   因此 Flash 程序**不需要 label 表**。

3. **注册命令 → CALLN（按名调用，opcode 0x28）**
   动态编译把命令名解析成运行时注册的 opcode（`SCL_RegisterCmd` 按注册顺序
   自动分配），这是运行期才知道的；Flash 程序是编译期产物、不能依赖注册顺序。
   因此预编译程序里**业务命令统一编码为 CALLN**：
   ```text
   参数区 = [total(1)] [STR 命令名] [参数 type 块...]
   ```
   执行时按名字查命令链表（命令数少，开销可忽略）后调用。
   内置元指令（`var/free/help`）与运算指令（`iadd..sneq`）opcode 编译期固定，不变。

4. **`${name}` 变量引用仍在运行时展开**
   参数 STR 块保留 `${}` 原文，解释时按当前变量展开（与动态一致）。
   普通 `var` 变量照旧运行时查变量表（变量表仍占 RAM）。

5. **顶层 `const` 直接折叠进 Flash（v0.3b）**
   脚本里 `const X = ...` 是**编译期常量**：`scl_emit_c`（.s2c 输入，转译时
   `const_fold=True`）把所有对常量的读取（算术/比较/`${}`/字符串比较等）
   折叠成字面量编进字节码与参数缓存——**不产 `var const` 指令、不占运行变量槽**，
   值随程序放 Flash。动态 `SCL_Run` 的 const 仍是运行时只读变量，互不影响。

### 编码格式（与 scl.c 动态编译完全一致）

- 字节码：每条指令 4B = `opc(2B 大端) + argOff(2B 大端)`；
  `argOff==0` = 无参（参数缓存第 0 字节为哨兵）。
- 参数缓存 type 块：
  `BOOL=01+v | INT=02+4B 大端 | FLAG=03+c | STR=04+len+bytes`
- opcode：`HELP=1 VAR=2 FREE=3 JUMP=4 JUMPA=5`；int/bool 运算 `0x10..0x27`；
  `CALLN=0x28`（仅预编译程序使用）。
- 命令字/保留字大小写敏感（同 C 端 `Scl_EqN`）；`true/false` 不区分大小写；
  数字支持十进制（含负号）与 `0x/0b` 前缀；`-x`(x 为字母) 为 flag。

## 3. 裁剪配置

`doc/../scl_cfg.h` 新增两个执行源开关：

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `SCL_CFG_RUN_TEXT_EN` | 1 | `SCL_Run` 动态文本编译（RAM 大缓存 + 编译器代码） |
| `SCL_CFG_RUN_PROG_EN` | 1 | `SCL_RunProg` 预编译程序（Flash，几乎不占 RAM） |

- **裁剪建议**
  - 脚本固定 → `-DSCL_CFG_RUN_TEXT_EN=0`：编译器函数
    （`Scl_Compile`/`Scl_ArgStore*`/opword 表/中间表）与 `s_bc/s_argc/s_labels/s_ins`
    整段裁掉，RAM/ROM 双省。
  - 仍需动态脚本 → 保留双开（代码略大但灵活）。
  - 两者都关 → 编译期 `#error`。

### RAM/ROM 粗略对比（默认裁剪宏）

| 形态 | 典型 RAM（运行脚本所需额外） | ROM |
| --- | --- | --- |
| 动态 `SCL_Run`（默认） | 字节码 512 + 参数 256 + 中间表 ~1.5K + label ~0.4K | 编译器代码 |
| 预编译 `RunProg` + `RUN_TEXT_EN=0` | 仅工作缓冲 + 变量表（~几百 B，随执行逐条使用） | 省去编译器代码 |
| `RunProg` + 双开 | 与动态同构的数组已裁，仅工作缓冲 | 略大 |

> 变量表 `SCL_CFG_VAR_MAX`/`VAR_NAME_MAX`/`VAR_VALUE_MAX` 与参数工作缓冲
> `s_argb/s_argv/s_argt/s_raw` 是运行必需的 RAM，仍按需裁剪。

## 4. 工具：`tools/scl_emit_c.py`

把现代语法脚本（.s2c）或已降级指令链文本，编译成上述 const C 源。

```bash
# 现代语法 demo4_control.s2c → demo4_prog.c（含 scl_demo4_prog）
python tools/scl_emit_c.py example/s2c/demo4_control.s2c -o build/demo4_prog.c --name demo4

# 直接编译指令链文本（不经过现代语法解析）
python tools/scl_emit_c.py boot.chain -o build/boot_prog.c --name boot --chain
```

生成文件含：
- `static const uint8_t scl_<name>_bc[]`：字节码（opc+argOff 大端）
- `static const uint8_t scl_<name>_argc[]`：参数缓存
- `const scl_prog_t scl_<name>_prog`：程序描述（`#include "scl.h"`）

Python 端编码逻辑与 `scl.c` 的 `Scl_Compile`/`Scl_ArgStore*` 一一对应复刻，
并在 `s2c_test.py` 中通过"生成→编译→`SCL_RunProg` 实跑→与文本路径输出比对"
持续防回归（含 `RUN_TEXT_EN=0` 裁剪变体）。

## 5. MCU 集成提示

- 生成数组所在 C 文件与 `scl.h` 一起编译即可；用链接脚本/编译器属性
  （如 Keil 放只读、GCC `__attribute__((section(".rodata")))`）确保进 Flash。
- 命令在 `main` 里先 `Scl_Xxx_Register()` 注册，再 `SCL_RunProg()`；
  多个固定脚本可各生成一个 prog，按需启动。
- 异步命令（带 sync 回调）在 `RunProg` 下行为与动态一致。

## 6. 相关

- `scl/Inc/scl.h`：`scl_prog_t` / `SCL_RunProg` 接口
- `scl/Inc/scl_cfg.h`：`SCL_CFG_RUN_TEXT_EN` / `SCL_CFG_RUN_PROG_EN`
- `tools/s2c_test.py`：第 4 节 emit-c 回喂（含 min 变体）
- 测试基线：scl_test 69 / s2c_test 64 全绿
