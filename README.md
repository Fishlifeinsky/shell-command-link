# SCL（Shell-Command-Link）简易指令链脚本库

嵌入式友好的「shell + 指令链脚本」C 库：**静态内存、无 malloc、无 OS/HAL/libc 依赖**，仅需提供一个字符输出函数。

## 特性速览

- **多类型变量**：`var <type> x=val`，type ∈ `bool`/`int`/`flag`/`string`
  （默认 4 槽 / 名 ≤8 / 值 16B，全部可裁剪）
- **const 只读常量**：`var const <type> x=val`（现代源写 `const [type] x=val`）——
  声明后不可覆盖/释放/作写回目标；`SCL_VarSetConst/IsConst`；列表带 `const` 标记
- **类型化参数字节缓存**：命令参数入缓存即解析成 type 块（bool/int/flag/string），
  type 开头、无空格分隔；命令可用 `SCL_ArgType()` 区分参数类型
- 命令实参内 `${x}` 取值（支持拼接）；C 命令可用 `SCL_VarGet()/VarSet()/VarSetT()/VarType()`
- **命令描述注册辅助**（参考 ESP-IDF console/argtable3）：声明命令 help + 参数模板，
  注册后自动参数数量/类型校验并输出 usage，`help` 汇总带说明；`SCL_ParseInt` 无 libc 取数
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
- **交互 Shell**（example/scl_shell）：串口 REPL —— 行编辑/历史 ↑↓/Tab 补全
  命令与 `${var}`；`SCL_VarKeep(1)` 会话变量跨命令保留
- **环境变量缓冲（持久配置）**：默认配置表装载 → 脚本 `${}`/命令/运算只读可见；
  `Scl_Env_Set` 修改；`Scl_Env_Save/Load` 序列化固化到用户自有存储(EEPROM/Flash/文件)
  与恢复；坏存储自动回退默认（`SCL_CFG_ENV_MAX` 可裁剪）
- **MCU 串口模拟**（example/sim_uart）：把 PC 终端当串口体验/调试
- **脚本结束自动全释放**（默认）；置 `SCL_VarKeep(1)` 后保留为会话变量
- **汇编式控制流**：`label 名` 设跳转点；`jump [-a] 名`（-a=G_RETURN 真跳）
- **文本→字节码**：指令固定 4 字节（opc + 参数偏移），命令注册自动分配 opcode
- **异步/跨主循环步进**：命令可带同步信号回调，适配"命令耗时等待硬件"
- 一条 `SCL_Run` + 主循环周期调 `SCL_Loop()` 即可驱动

## 文档

| 文档 | 内容 |
|------|------|
| `doc/arc/shell-command-link-design.md` | 设计（含左右脑互博、内存估算、变更记录） |
| `doc/arc/v02-typed-vars-design.md` | **v0.2 设计**：类型化参数缓存/多类型变量/int·bool 运算指令 |
| `doc/arc/v03-brainstorm.md` / `v03-plan.md` | **v0.3 头脑风暴/计划**：完整表达式·for·位运算·循环保护·交互终端 |
| `doc/arc/script2chain-design.md` | 现代脚本→指令链 转译器设计（语法与映射） |
| `doc/arc/scl-const-prog.md` | **v0.3**：预编译只读程序（s2c→C，省 RAM）设计 |
| `doc/arc/scl-shell-sim.md` | **v0.3**：交互 Shell + MCU 串口模拟 + VarKeep |
| `doc/arc/scl-env-buffer.md` | **v0.3**：环境变量缓冲（默认装载/固化/恢复） |
| `doc/arc/scl-cmddesc.md` | **v0.3**：多模块源码 + 命令描述注册辅助（argtable3 风格） |
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
example/                      PC 示例（main.c 全量测试 / chain_runner 回喂 /
                              scl_shell 交互 Shell / sim_uart 串口模拟 / s2c/*.s2c）
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

## 构建示例（PC 测试）

```bash
gcc -O2 -Wall -Wextra -I scl/Inc -I scl/Src -I example \
    scl/Src/scl.c scl/Src/scl_var.c scl/Src/scl_env.c \
    example/scl_port.c example/demo_cmds.c \
    example/scl_shell.c example/main.c -o build/scl_test        # 全量测试(含 Shell)

# 交互终端：把 PC 终端当 MCU 串口（echo/var/Tab 补全/↑↓ 历史/quit）
gcc -O2 -Wall -Wextra -I scl/Inc -I scl/Src -I example \
    scl/Src/scl.c scl/Src/scl_var.c scl/Src/scl_env.c \
    example/scl_port.c example/demo_cmds.c \
    example/scl_shell.c example/sim_uart.c -o build/sim_uart && ./build/sim_uart
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
while (n < 3) {                     # 比较表达式 n<3 → ilt + label/jump
    n = n + 1                       # 算术赋值 → iadd n 1 n
    echo("n=${n}")
}
for (var int i=0; i < 5; i = i + 1) {   # for(init;cond;step) 标准 while 语义
    if (i == 2) { continue }        # continue → step
    if (i == 4) { break }           # break 退出循环
    echo("i=${i}")
}
if (mode == 1 && running) { echo("ok") } else { echo("ng") }  # && || ! 括号短路
if ((n & 0x1) == 1) { echo("odd") }     # 位运算字面量
x = (a & 0xFF) | 0x10               # 多运算符算术/位：a+b*2、a<<2|1 …（临时变量自动管理）
if { echo("沿用G_RETURN") }         # 无条件 if 也可用
fn not_done() { demo_inc() }        # 用户函数作条件（内联）
ret(1)                              # 置 G_RETURN（默认映射 setret，可配）
```

> v0.3：条件/赋值完整表达式（`&& || !`、括号、`+ - * / %`、`& | ^ ~ << >>`）、
> `for` 与 `break/continue` 均已支持；循环有 `SCL_CFG_STEP_LIMIT` 步进保护兜底。

## 裁剪

编译期宏集中在 `scl/Inc/scl_cfg.h`（`#ifndef` 保护，可用 `-DSCL_CFG_XXX=yyy` 覆盖）：
变量槽数/名长/值长、脚本缓冲/字节码/参数字节缓存/label 表容量、参数上限、
消息开关 `SCL_CFG_MSG_EN`（关掉后连 `SCL_Port_PutChar` 都可省略）。

> 保留关键字（不可注册为业务命令）：`var` `free` `help` `label` `jump`
> 及内置运算指令 `iadd` `isub` `imul` `idiv` `imod` `ineg` `ieq` `ine` `igt`
> `ige` `ilt` `ile` `band` `bor` `bnot` `btest`（`if/while` 已由上层编译器降级）。
