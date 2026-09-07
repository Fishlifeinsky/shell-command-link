# SCL 语法与接口规格（doc/spec）

> SCL = Shell-Command-Link：嵌入式友好的"指令链脚本"库。
> 完整设计见 `doc/arc/shell-command-link-design.md`；调用/执行流程见 `doc/arc/instruction-chain-flow.md`；测试报告见 `doc/other/scl-test-report.md`。

## 1. 一句话

把一段由 `;` 分隔、可含变量/条件/循环的"脚本"交给 `SCL_Run()`，之后在主循环里周期调用 `SCL_Loop()`，库按步执行；脚本结束自动释放全部变量。

## 2. 保留关键字（不能注册为业务命令）

`var` `free` `help` `label` `jump`

> `if`/`while` 已不属运行时指令（由上层编译器降级为 label/jump），直接写会被当未知命令。

## 3. 脚本语法（汇编式 + 字节码编译）

```
<脚本>     ::= <子句> { ';' <子句> }            // 引号内 ';' 不分割
<子句>     ::= 变量指令 | 释放指令 | 命令调用
             | 'label' <名>                     // 设置跳转点（不产字节）
             | 'jump' ['-a'] <名>               // -a=有条件(G_RETURN 真跳,读后清零)；默认/'-b'=无条件
<变量指令> ::= 'var' <名> '=' <值>              // 名/值均可配上限；值支持引号与 ${}
             | 'var' | 'var' <名> | 'var free'
<释放指令> ::= 'free' [<名>]
<命令调用> ::= <命令名> { ' ' <实参> }          // 普通式：空白分隔
<实参>     ::= [ '"' | "'" ] <文本，可含 ${name}> [ '"' | "'" ]
```

- **命令调用只有普通式** `cmd a b`；含空格的参数用引号包成整体。函数式 `cmd(a,b)` 不解析（未知命令）。
- 引号支持 `"..."` 与 `'...'`，交替嵌套（同类型不嵌套）。
- 值里可用 `${name}` 引用变量；变量未定义展开为空并提示。

### 3.1 执行模型（v4：文本 → 字节码）

`SCL_Run(text)` 一次性把文本**编译成字节码**后置忙并返回：

| 项 | 格式 |
|----|------|
| 指令 | 每条固定 **4 字节** = `opc(2B,大端) + argOff(2B,大端)` |
| 参数 | 存“参数字节缓存”：`[len(1)][参数原文]`；`argOff` 指向其起点；无参 `argOff=0` |
| label | 不产字节，登记到 label 表：名 → 下一条指令字节偏移 |
| jump | 编译期把 label 名解析为目标偏移写入 argOff 槽 |
| opcode | 内置固定：help=1 / var=2 / free=3 / jump=4 / jump -a=5；注册命令在 `SCL_RegisterCmd` 时**自动分配**（自 0x0100 起） |

例：`help xxx` 编译为 `00 01 56 78`（`00 01`=help 的 opcode，`56 78`=参数在缓存中的偏移）。

编译错误（未知命令 / label 重名或未定义 / jump 缺目标 / 超限）→ 拒绝该链并保持空闲。

## 4. G_RETURN（条件标志）

- 默认 `false`；脚本开始/结束清零。
- C 命令写入：`SCL_Ret_Set(v)`；命令内原样读：`SCL_Ret_Get()`。
- `jump -a` **读取即清零**（“每次读完重置”）。例：命令置真后 `jump -a L` 跳到 L；再用一次 `jump -a` 会读到已清空的 false。

## 5. 控制流（label/jump）

- `label 名`：给下一条指令做标记；`jump 名`（或 `jump -b 名`）=无条件跳转。
- `jump -a 名`：读 G_RETURN，为真 → 跳到 `名` 处；为假 → 顺序执行下一条。
- do-while 循环范式：
  ```
  label L_top
  <body>
  <条件命令>      ; 写 G_RETURN
  jump -a L_top   ; 真 → 再跑一轮
  ```
- if/else 范式（由编译器生成）：
  ```
  <条件命令>; jump -a L_t; <else>; jump L_end; label L_t; <then>; label L_end
  ```
- label 数量上限 `SCL_CFG_LABEL_MAX`（默认 16），标签名 ≤ `SCL_CFG_LABEL_NAME_MAX`。

## 6. 脚本生命周期 / 异步步进

```
SCL_Run("...")   // 编译文本→字节码，置忙，立即返回；一次只跑一个
主循环: SCL_Loop() // 每周期执行一条指令（或轮询异步命令完成）
SCL_Idle()       // 1=空闲（字节码全部执行完）
SCL_Abort()      // 可从中断里调用，请求中止（变量仍自动释放）
```

- 业务命令分**同步/异步**：注册节点带 `sync` 回调即为异步。handler 立即返回发起操作；库每 Loop 轮询 `sync(false)`，返回 true 后调 `sync(true)` 清除，再执行下一条。`G_RETURN` 可在 handler 或 sync 回调里设置。
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
