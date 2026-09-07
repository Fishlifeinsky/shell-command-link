# SCL（Shell-Command-Link）简易指令链脚本库

嵌入式友好的「shell + 指令链脚本」C 库：**静态内存、无 malloc、无 OS/HAL/libc 依赖**，仅需提供一个字符输出函数。

## 特性速览

- **多类型变量**：`var <type> x=val`，type ∈ `bool`/`int`/`flag`/`string`
  （默认 4 槽 / 名 ≤8 / 值 16B，全部可裁剪）
- **类型化参数字节缓存**：命令参数入缓存即解析成 type 块（bool/int/flag/string），
  type 开头、无空格分隔；命令可用 `SCL_ArgType()` 区分参数类型
- 命令实参内 `${x}` 取值（支持拼接）；C 命令可用 `SCL_VarGet()/VarSet()/VarSetT()/VarType()`
- `free` 释放 / `var` 查剩余空位；**脚本跑完自动全释放**
- 全局条件标志 `G_RETURN`：命令/比较指令写、`jump -a` 读（**读后自动清零**）
- 普通调用 `cmd a b`（空白分隔；含空格的参数用引号包裹为整体）
- **内置 int/bool 运算指令**（参考 C，保留字）：
  `iadd isub imul idiv imod ineg`（写回变量）/ `ieq ine igt ige ilt ile`
  `band bor bnot btest`（结果进 G_RETURN）
- **汇编式控制流**：`label 名` 设跳转点；`jump [-a] 名`（-a=G_RETURN 真跳）
- **文本→字节码**：指令固定 4 字节（opc + 参数偏移），命令注册自动分配 opcode
- **异步/跨主循环步进**：命令可带同步信号回调，适配"命令耗时等待硬件"
- 一条 `SCL_Run` + 主循环周期调 `SCL_Loop()` 即可驱动

## 文档

| 文档 | 内容 |
|------|------|
| `doc/arc/shell-command-link-design.md` | 设计（含左右脑互博、内存估算、变更记录） |
| `doc/arc/v02-typed-vars-design.md` | **v0.2 设计**：类型化参数缓存/多类型变量/int·bool 运算指令 |
| `doc/arc/script2chain-design.md` | 现代脚本→指令链 转译器设计（语法与映射） |
| `doc/spec/scl-spec.md` | 语法/API/移植/裁剪规格 + 集成示例 |
| `doc/other/scl-test-report.md` | 全量测试报告（大小/速度/可靠性/重复性/复杂度） |
| `doc/user/prompt-shell-command-link.md` | 原始需求存档 |

## 目录

```
scl/Inc/scl.h scl_cfg.h       库公共接口 + 可裁剪配置
scl/Src/scl.c                 解析器/执行器/变量/if/while/异步
tools/scl_script2chain.py     现代语法脚本 → SCL 指令链（Python 转译器）
tools/s2c_test.py             转译器测试（精确比对 + 真实回喂）
example/                      PC 示例（main.c 全量测试 / chain_runner.c 回喂工具 / s2c/*.s2c 现代脚本）
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
gcc -O2 -Wall -Wextra -I scl/Inc -I example \
    scl/Src/scl.c example/scl_port.c example/demo_cmds.c example/main.c \
    -o build/scl_test
# 或用 CMake：cmake -S example -B build && cmake --build build
```

## 现代脚本 → 指令链（Python 工具）

可用更可读的语法写脚本，再转成单行 SCL 指令链喂给 `SCL_Run()`：

```bash
python tools/scl_script2chain.py example/s2c/demo1_if.s2c     # 打印指令链
python tools/scl_script2chain.py in.s2c -o out.chain          # 写文件
python tools/scl_script2chain.py --ret-setter setret < in.s2c # 改置返回命令名
python tools/s2c_test.py                                      # 转译器测试(含真实回喂)
```

现代语法速览（详见 `doc/arc/script2chain-design.md`，示例见 `example/s2c/`）：

```s2c
# 注释支持 # // /* */
var int sp = 100                    # 显式类型；省略则编译器推断（默认 4 槽/名≤8/值≤15）
var bool running = true
var flag f = -x
var int n = 0
while (n < 3) {                     # do-while：比较表达式 n<3 → ilt + label/jump
    n = n + 1                       # 算术赋值 → iadd n 1 n
    echo("n=${n}")
}
if (mode == 1) { echo("ok") } else { echo("ng") }   # 原子比较表达式
if (!running) { echo("halt") }     # 一元取反
if { echo("沿用G_RETURN") }         # 无条件 if 也可用
fn not_done() { demo_inc() }        # 用户函数作条件（内联）
while (not_done()) { echo("step", ${sp}) }
ret(1)                              # 置 G_RETURN（默认映射 setret，可配）
```

> 条件表达式范围：原子条件（比较/`!b`/变量/字面量）已支持；
> `&& ||` 与多运算符算术留待 v0.3（会报明确错误）。

## 裁剪

编译期宏集中在 `scl/Inc/scl_cfg.h`（`#ifndef` 保护，可用 `-DSCL_CFG_XXX=yyy` 覆盖）：
变量槽数/名长/值长、脚本缓冲/字节码/参数字节缓存/label 表容量、参数上限、
消息开关 `SCL_CFG_MSG_EN`（关掉后连 `SCL_Port_PutChar` 都可省略）。

> 保留关键字（不可注册为业务命令）：`var` `free` `help` `label` `jump`
> 及内置运算指令 `iadd` `isub` `imul` `idiv` `imod` `ineg` `ieq` `ine` `igt`
> `ige` `ilt` `ile` `band` `bor` `bnot` `btest`（`if/while` 已由上层编译器降级）。
