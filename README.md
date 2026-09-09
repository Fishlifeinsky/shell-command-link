# SCL（Shell-Command-Link）简易指令链脚本库

嵌入式友好的「shell + 指令链脚本」C 库：**默认静态内存、无 malloc、无 OS/HAL/libc 依赖**，
也可选用应用提供的 allocator 动态管理运行缓冲；仅需提供一个字符输出函数。

## 特性速览

- **多类型变量**：`var <type> x=val`，type ∈ `bool`/`int`/`flag`/`string`
  （默认 4 槽 / 名 ≤8 / 值 16B，全部可裁剪）
- **const 只读常量**：`var const <type> x=val`（现代源顶层写 `const [type] x=val`
  或 `var const [type] x=val`）——声明后不可覆盖/释放/作写回目标；
  `SCL_VarSetConst/IsConst`；列表带 `const` 标记
- **类型化参数字节缓存**：命令参数入缓存即解析成 type 块（bool/int/flag/string），
  type 开头、无空格分隔；命令可用 `SCL_ArgType()` 区分参数类型
- 命令实参内 `${x}` 取值（支持拼接）；C 命令可用 `SCL_VarGet()/VarSet()/VarSetT()/VarType()`
- **命令描述注册辅助**（参考 ESP-IDF console/argtable3）：声明命令 help + 参数模板，
  注册后自动参数数量/类型校验并输出 usage，`help` 汇总带说明；`SCL_ParseInt` 无 libc 取数
- **`help [cmd]` 单命令明细**：`help` 全览；`help <cmd>`（含内置元命令/运算，大小写不敏感）
  输出该命令 help+usage+逐参必选/可选说明（esp_console 风格）
- 源码**多模块**：`scl.c`(核心) + `scl_var.c`(会话变量) + `scl_env.c`(env)，内部 `scl_priv.h` 共享
- `free` 释放 / `var` 查剩余空位；**脚本跑完自动全释放**
- 全局条件标志 `G_RETURN`：命令/比较指令写、`jump -a` 读（**读后自动清零**）
- 普通调用 `cmd a b`（空白分隔；含空格的参数用引号包裹为整体）
- **内置 int/bool 运算指令**（参考 C，保留字）：
  `iadd isub imul idiv imod ineg`（写回变量）/ `ieq ine igt ige ilt ile`
  `band bor bnot btest`（结果进 G_RETURN）/ `iand ior ixor inot shl shr`（位运算，写回变量）
  `seq sneq`（字符串/flag 相等·不等比较 → G_RETURN）
- **预编译只读程序（s2c→C，省 RAM）**：`scl_emit_c.py` 把脚本编译成 const
  字节码放 Flash，`SCL_RunProg()` 直接执行，**不占 RAM 字节码/参数缓存/label**；
  可 `SCL_CFG_RUN_TEXT_EN=0` 裁掉动态编译器（固定脚本最省 RAM 用法）
- **环境变量缓冲（持久配置）**：默认配置表装载 → 脚本 `${}`/命令/运算只读可见；
  `Scl_Env_Set` 修改；`Scl_Env_Save/Load` 序列化固化到用户自有存储(EEPROM/Flash/文件)
  与恢复；坏存储自动回退默认（`SCL_CFG_ENV_MAX` 可裁剪）
- **可选动态内存**：`SCL_CFG_DYNAMIC_MEM_EN=1` 时由应用提供 `alloc/realloc/free + ctx`，
  `SCL_InitEx()` 初始化最小运行缓冲，变量名和值按需分配；`cache`/`cache gc`/
  `cache zombie` 查询和回收缓存
- **MCU(STM32) 移植模板**（example/mcu_template）：真实工程样板 —— HAL UART 移植层
  + main 集成骨架（env 固化 + const 自检程序）+ PC 无板自检（mcu_boot_sim）
- **脚本结束自动全释放**（默认）；置 `SCL_VarKeep(1)` 后保留为会话变量
- **汇编式控制流**：`label 名` 设跳转点；`jump [-a] 名`（-a=G_RETURN 真跳）
- **文本→字节码**：指令固定 4 字节（opc + 参数偏移），命令注册自动分配 opcode
- **异步/跨主循环步进**：命令可带同步信号回调，适配"命令耗时等待硬件"
- 一条 `SCL_Run` + 主循环周期调 `SCL_Loop()` 即可驱动

## 占用与裁剪建议

按当前默认配置用 gcc `-O2` 编译 `scl.c`、`scl_var.c`、`scl_env.c` 的对象文件，
SCL 核心约为：代码 `.text` 22.4 KB、只读数据 `.rdata` 5.0 KB、静态 RAM `.bss`
约 2.1 KB。该结果是 PC 编译器口径，Cortex-M 的 Thumb-2 结果应以目标工具链为准。

其中约 5 KB 的只读数据主要是 `SCL_CFG_MSG_EN` 打开的中文帮助文档和错误提示，
不是每条命令的描述本身。实测只关闭 `SCL_CFG_MSG_EN` 后，`scl.c` 的 `.rdata`
从约 5.0 KB 降到约 0.9 KB，`.text` 也从约 18.6 KB 降到约 13.7 KB；只关闭
`SCL_CFG_CMDDESC_EN` 只减少约 0.2 KB `.rdata` 和约 1.1 KB `.text`。

固定脚本的量产配置建议使用预编译程序：

```text
-DSCL_CFG_RUN_TEXT_EN=0   # 去掉 MCU 端动态文本编译器
-DSCL_CFG_MSG_EN=0        # 去掉 help/错误输出及其中文字符串
-DSCL_CFG_ENV_EN=0        # 不需要持久环境变量时关闭
-DSCL_CFG_SCMD_EN=0       # 不使用脚本注册命令时关闭
```

该最小配置只保留 `SCL_RunProg()` 解释器，PC 对象级实测 `scl.c` 约为 `.text`
8.2 KB、`.rdata` 0.3 KB、`.bss` 0.6 KB；最终 Flash/RAM 仍应以 MCU 链接地图为准。

## 文档

| 文档 | 内容 |
|------|------|
| `doc/arc/shell-command-link-design.md` | 设计（含左右脑互博、内存估算、变更记录） |
| `doc/arc/v02-typed-vars-design.md` | **v0.2 设计**：类型化参数缓存/多类型变量/int·bool 运算指令 |
| `doc/arc/v03-brainstorm.md` / `v03-plan.md` | **v0.3 头脑风暴/计划**：完整表达式·for·位运算·循环保护·交互终端 |
| `doc/arc/script2chain-design.md` | 现代脚本→指令链 转译器设计（语法与映射） |
| `doc/arc/scl-const-prog.md` | **v0.3**：预编译只读程序（s2c→C，省 RAM）设计 |
| `doc/arc/scl-env-buffer.md` | **v0.3**：环境变量缓冲（默认装载/固化/恢复） |
| `doc/arc/scl-cmddesc.md` | **v0.3**：多模块源码 + 命令描述注册辅助（argtable3 风格） |
| `doc/arc/scl-dynamic-mem.md` | **v0.4**：可选动态内存、变量懒分配与 cache/GC |
| `doc/arc/scl-const.md` | **v0.3**：const 只读常量（链式/C API/现代源） |
| `doc/other/scl-config-profiles.md` | **v0.3**：配置裁剪档模板（min/平衡/full + 内存预算） |
| `doc/spec/scl-spec.md` | 语法/API/移植/裁剪规格 + 集成示例 |
| `doc/other/scl-test-report.md` | 全量测试报告（大小/速度/可靠性/重复性/复杂度） |
| `doc/user/prompt-shell-command-link.md` | 原始需求存档 |

## 目录

```
scl/Inc/scl.h scl_cfg.h       库公共接口 + 可裁剪配置
scl/Src/scl.c scl_var.c scl_env.c   核心(编译/执行/命令/异步) · 会话变量 · 环境变量缓冲(多模块)
scl/Src/scl_priv.h             多模块内部共享头（勿在应用层使用）
tools/scl_script2chain.py     现代语法脚本 → SCL 指令链（Python 转译器）
tools/scl_emit_c.py           SCL 脚本 → const C 程序（Flash 只读，省 RAM）
tools/s2c_test.py             转译器测试（精确比对 + 真实回喂 + emit-c）
tools/scl_build.py            统一构建/测试/尺寸（PC 全量 + ARM 裁剪矩阵/尺寸）
example/                      PC 示例（main.c 全量测试 / chain_runner 回喂 /
                              s2c/*.s2c）
example/mcu_template/         MCU(STM32) 移植模板（HAL port + main 骨架 + PC 自检）
```

## 快速集成

```c
#include "scl.h"

void SCL_Port_PutChar(char c) { my_uart_put(c); }   /* 移植：输出单字符 */
static void Cmd_Step(int argc, char *argv[]) { /* ... */ SCL_Ret_Set(done); }
static scl_cmd_t s_cmd_step = { "step", Cmd_Step, NULL, NULL };

int main(void) {
    SCL_RegisterCmd(&s_cmd_step);
    /* label/jump 汇编式（do-while 范式）：step 完成后置 G_RETURN=false 即退出 */
    SCL_Run("var int sp=100; label L1; step ${sp} 1; setret 0; jump -a L1");
    for (;;) { SCL_Loop(); }   /* 主循环推进 */
}
```

> v0.2 破坏性变更：运行时 `var` 需显式类型（v0.1 的 `var x=v` 改为 `var <type> x=v`）；
> 现代源层可省略类型（编译器自动推断）。旧指令链需补类型后方可运行。

## 构建 / 测试（推荐：一键脚本）

```bash
python tools/scl_build.py            # all：PC 全量测试 + 裁剪矩阵 + ARM 尺寸
python tools/scl_build.py test       # 仅 PC 全量（默认档/desc关/MCU骨架/s2c）
python tools/scl_build.py check      # ARM 裁剪开关零告警矩阵（需 arm-none-eabi）
python tools/scl_build.py sizes      # ARM(Cortex-M4) 各裁剪档 Flash/RAM（需 arm-none-eabi）
```

脚本用 Python `subprocess` 编译并运行（兼容透明加密环境：编译产物只有白名单进程能读明文），
自动探测 `gcc`/`arm-none-eabi-gcc`，缺 ARM 工具时 check/sizes 自动跳过。
各裁剪档宏集合见 `doc/other/scl-config-profiles.md`。

## 构建示例（PC 测试，手工）

```bash
gcc -O2 -Wall -Wextra -I scl/Inc -I scl/Src -I example \
    scl/Src/scl.c scl/Src/scl_var.c scl/Src/scl_env.c \
    example/scl_port.c example/demo_cmds.c \
    example/main.c -o build/scl_test        # 全量测试
```

## 固定脚本最省 RAM：预编译只读程序（s2c→C）

脚本不变时，用工具编译成 const C 放 Flash，运行时 `SCL_RunProg()` 直接执行：

```bash
python tools/scl_emit_c.py boot.s2c -o boot_prog.c   # 生成 const 字节码 C 源
# 固件里：SCL_Init(); <注册命令>; SCL_RunProg(&scl_boot_prog); 周期 SCL_Loop()
# MCU 裁剪：编译加 -DSCL_CFG_RUN_TEXT_EN=0 连动态编译器一起裁掉
```

详见 `doc/arc/scl-const-prog.md`。

## 现代脚本 → 指令链（Python 工具）

可用更可读的语法写脚本，再转成单行 SCL 指令链喂给 `SCL_Run()`：

```bash
python tools/scl_script2chain.py example/s2c/demo1_if.s2c     # 打印指令链
python tools/scl_script2chain.py in.s2c -o out.chain          # 写文件
python tools/scl_script2chain.py --ret-setter setret < in.s2c # 改置返回命令名
python tools/s2c_test.py                                      # 转译器测试(含真实回喂)
python tools/scl_emit_c.py demo.s2c -o demo_prog.c           # 现代脚本→const C 程序
```

现代语法速览（详见 `doc/arc/script2chain-design.md`，示例见 `example/s2c/`）：

```s2c
# 注释支持 # // /* */
var int sp = 100                    # 显式类型；省略则推断（bool/int/flag/string，0x/0b→int）
var bool running = true
var flag f = -x
var int n = 0
while (n < 3) {                     # 标准 while：先判再跑 → ilt + label/jump
    n = n + 1                       # 算术赋值 → iadd n 1 n
    echo("n=${n}")
}
do {                                # do{..}while：先跑一次再判
    n = n + 1
} while (n < 3)
when (n) {                          # when 值匹配（Kotlin 风格；主语可选）
    1, 2 -> echo("small")
    3    -> echo("three")
    else -> echo("big")
}
when { n == 3 -> echo("guard") }    # 无主语 = 守卫链（依次判条件）
if (mode == 1 && running) { echo("ok") } else { echo("ng") }  # && || ! 括号短路
if ((n & 0x1) == 1) { echo("odd") }     # 位运算字面量
x = (a & 0xFF) | 0x10               # 多运算符算术/位：a+b*2、a<<2|1 …（临时变量自动管理）
if { echo("沿用G_RETURN") }         # 无条件 if 也可用
fn not_done() { demo_inc() }        # 用户函数作条件（内联）
ret(1)                              # 置 G_RETURN（默认映射 setret，可配）
```

> v0.3：条件/赋值完整表达式（`&& || !`、括号、`+ - * / %`、`& | ^ ~ << >>`）均已支持；
> v0.4：`while` 改标准先判、新增 `do{}while` 与 `when`（Kotlin 风格），`for` 已移除
>（用 `var` 初始化 + `while` 改写）；`break/continue` 作用于最近 while/do；循环有 `SCL_CFG_STEP_LIMIT` 兜底。

## 裁剪

编译期宏集中在 `scl/Inc/scl_cfg.h`（`#ifndef` 保护，可用 `-DSCL_CFG_XXX=yyy` 覆盖）：
变量槽数/名长/值长、脚本缓冲/字节码/参数字节缓存/label 表容量、参数上限、
消息开关 `SCL_CFG_MSG_EN`（关掉后连 `SCL_Port_PutChar` 都可省略）。

> 保留关键字（不可注册为业务命令）：`var` `free` `help` `label` `jump`
> 及内置运算指令 `iadd` `isub` `imul` `idiv` `imod` `ineg` `ieq` `ine` `igt`
> `ige` `ilt` `ile` `band` `bor` `bnot` `btest`（`if/while` 已由上层编译器降级）。
