# SCL 语法与接口规格（doc/spec）

> SCL = Shell-Command-Link：嵌入式友好的"指令链脚本"库。
> 完整设计见 `doc/arc/shell-command-link-design.md`；测试报告见 `doc/other/scl-test-report.md`。

## 1. 一句话

把一段由 `;` 分隔、可含变量/条件/循环的"脚本"交给 `SCL_Run()`，之后在主循环里周期调用 `SCL_Loop()`，库按步执行；脚本结束自动释放全部变量。

## 2. 保留关键字（不能注册为业务命令）

`var` `free` `if` `while` `help`

## 3. 脚本语法

```
<脚本>     ::= <子句> { ';' <子句> }            // 引号内 ';' 不分割
<子句>     ::= 变量指令 | 释放指令 | 流程指令 | 命令调用
<变量指令> ::= 'var' <名> '=' <值>              // 名/值均可配上限；值支持引号与 ${}
             | 'var'                            // 列出变量与剩余空位
             | 'var' <名>                       // 查单个变量
             | 'var free'                       // 全释放（同 free）
<释放指令> ::= 'free' [<名>]                    // 无参=全释放
<流程指令> ::= 'if' ['-t' <分支>] ['-f' <分支>] // 读 G_RETURN（读后清零）
             | 'while' '-b'                     // 循环体起点
             | 'while' '-e' [<N>]               // 判定退出；N=显式迭代上限
<分支>     ::= '"' <脚本> '"' | "'" <脚本> "'"  // 引号内=多指令子链
             | <单条命令>                       // 无引号=单条命令（不含空格）
<命令调用> ::= <命令名> { ' ' <实参> }          // 普通式：空白分隔（函数式调用已移除）
<实参>     ::= [ '"' | "'" ] <文本，可含 ${name}> [ '"' | "'" ]
```

- 命令调用只有**普通式**：`cmd a b`；参数以空白分隔，含空格的参数用 `"..."` 或 `'...'` 包裹成整体。
  旧函数式写法 `cmd(a,b)` 不再解析（会当作未知命令报错）。
- 引号支持 `"..."` 与 `'...'`，可**交替嵌套**（如外层 `"..."` 内层 `'...'`），用于在分支子链里再写带引号的 `if`。同类型不嵌套。
- 值里可用 `${name}` 引用其它变量（`var b=${a}`、命令实参内拼接 `${a}deg` 都支持）。变量未定义展开为空并打印提示。

## 4. G_RETURN（条件标志）

- 默认 `false`；脚本开始/结束清零。
- C 命令写入：`SCL_Ret_Set(v)`；命令内原样读：`SCL_Ret_Get()`。
- `if`、`while -e` **读取即清零**（"每次读完重置"）。因此想让 `while -e N` 稳定循环 N 圈，body 内**每圈都要重新置 `G_RETURN=true`**（例如 `while -b; setret 1; cmd...; while -e 3`）。

## 5. if / while 语义（已确认）

- `if -t "A" -f "B"`：`G_RETURN==true` 执行 A；`==false` 执行 B；`-t/-f` 可只写一个、可同时存在。
- `while -b; <body>; while -e`：do-while。`-e` 读到 `G_RETURN==false` 退出；`==true` 回跳 body 继续。
- `while -e N`：带迭代上限 N（`G_RETURN==true` 时 N 圈强制退出）；无 N 时由 `SCL_CFG_WHILE_MAX`（默认 100000）兜底防死循环。
- if/while 可互相嵌套，总深度 ≤ `SCL_CFG_NEST_MAX`（默认 3）；超深报错中止。
- 结构错误（如 `-e` 无 `-b`、`-b` 无 `-e`、引号/括号未闭合）报错并中止当前脚本（变量仍自动释放）。

## 6. 脚本生命周期 / 异步步进

```
SCL_Run("...")   // 拷贝脚本，置忙，立即返回；一次只跑一个
主循环: SCL_Loop() // 每周期推进一个动作（一条子句，或轮询异步命令完成）
SCL_Idle()       // 1=空闲（脚本全部结束）
SCL_Abort()      // 可从中断里调用，请求中止（变量仍自动释放）
```

- 业务命令分**同步/异步**：注册节点带 `sync` 回调即为异步。handler 立即返回发起操作；库每 Loop 轮询 `sync(false)`，返回 true 后调 `sync(true)` 清除，再继续下一条。`G_RETURN` 可在 handler 或 sync 回调里设置。
- 命令处理函数/回调必须快速、非阻塞；需要耗时的操作在命令内部自管状态推进。

## 7. 变量

- 上限 `SCL_CFG_VAR_MAX`（默认 2）；名长 ≤ `SCL_CFG_VAR_NAME_MAX`（默认 8）；值缓冲 `SCL_CFG_VAR_VALUE_MAX`（默认 16 B，最多 15 字符）。
- C 接口：`SCL_VarGet(name)` / `SCL_VarSet(name,val)` / `SCL_VarFree(name)` / `SCL_VarFreeAll()` / `SCL_VarCount()` / `SCL_VarFreeCount()`。
- 脚本结束自动 `free` 全部变量（含中止/出错路径）。

## 8. 移植接口（用户唯一需要做的）

| 项 | 内容 |
|----|------|
| 必须 | `void SCL_Port_PutChar(char c)` 输出单字符（库内消息用） |
| 可选 | `SCL_CFG_MSG_EN=0` 编译时裁掉全部消息 → 连上面函数都可省略 |
| 依赖 | 无 malloc / 无 OS / 无 HAL / 无 libc |

其它裁剪宏全在 `scl/Inc/scl_cfg.h`（`#ifndef` 保护，可用 `-D` 覆盖）。

## 9. 最小集成示例（伪代码）

```c
#include "scl.h"

// 1) 用户实现移植输出（例：接到串口/重定向到现有 printf）
void SCL_Port_PutChar(char c) { uart_putchar(c); }

// 2) 静态定义命令节点（业务层），并注册
static void Cmd_Step(int argc, char *argv[]) { /* 发指令… */ SCL_Ret_Set(done); }
static scl_cmd_t s_cmd_step = { "step", Cmd_Step, NULL, NULL };
//   （异步命令：再给 .sync 赋完成检测回调）

void App_Init(void) { SCL_RegisterCmd(&s_cmd_step); }

// 3) 收到一"行脚本"就交给库
void OnLine(const char *line) { if (!SCL_Run(line)) log("busy/超长"); }

// 4) 主循环周期推进
void Main_Loop(void) { SCL_Loop(); }
```

脚本示例：
```
var sp=100
while -b
  step ${sp} 1               // 普通式命令；内部完成后 G_RETURN 反映状态
  if -t "log ok" -f "step ${sp} -1;log retry"
  setret 1                   // 供 while -e 每圈判定为"继续"
while -e 5
echo finish
```
