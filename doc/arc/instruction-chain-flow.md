# 当前指令链调用流程（现状 v3，doc/arc）

> 对应实现：`scl/Src/scl.c`。描述**当前已实现**的指令链调用/执行流程
> （含 v2 移除函数式调用、v3 仅 SCL 层语义不变、modern 层 while 为 do-while 等历史变更后的现状）。
> 设计背景见 `shell-command-link-design.md`；指令链语法见 `doc/spec/scl-spec.md`。

## 0. 总览

```
用户/上层应用
   │  提供一条指令链（脚本字符串，如 "var sp=100;while -b;step ${sp};while -e"）
   ▼
SCL_Run(chain) ──拷贝进内部缓冲、置 busy、清 G_RETURN──► 立即返回(不执行)
   ▼
主循环周期调用  SCL_Loop()   （每调用推进"一个动作"）
   │   ├─ 有异步命令在等? ──► 轮询 sync(false)/sync(true)，完成后继续
   │   └─ 否则 Scl_StepOnce() 推进一条子句（或结束一个区域）
   ▼
SCL_Idle()==1 ──► 脚本执行完：自动释放全部变量、G_RETURN 清零
```

要点：
- **一次只跑一条链**：busy 期间再 `SCL_Run` 会被拒绝并提示。
- **异步/跨主循环步进**：链不阻塞；每周期只做一个动作。
- 全程静态内存、无 malloc；解析不建 AST，执行时按文本游标推进。

## 1. 提交：SCL_Run(const char *script)

1. 首次未初始化则 `SCL_Init()`（清命令链表/变量/G_RETURN）。
2. 若 `busy` → 报“busy: 有脚本正在执行”，返回 0 拒绝。
3. 长度校验：> `SCL_CFG_SCRIPT_MAX-1` → 报“脚本过长”，返回 0。
4. 把脚本**拷贝**进内部 `s_prog[]`（执行期间只读；调用方缓冲可立即复用）。
5. 复位执行状态：`s_cur`=起点、帧栈空、无异步等待、清 abort、`G_RETURN=0`、`busy=1`。
6. 返回 1。**此函数不执行任何指令**。

## 2. 主循环：SCL_Loop()

每周期一次（主循环/tick/调度里调用）：

1. `busy==0` → 直接返回（空闲）。
2. 若 `SCL_Abort()` 被置位 → 打印 aborted → `Scl_Finish(1)` 收尾（变量仍自动释放）。
3. 若正在**异步等待**某命令（`s_wait_cmd!=NULL`）：
   - 调 `cmd->sync(false)` 查询：未完成 → 返回（下个周期再查）；
   - 完成 → 调 `cmd->sync(true)` 清除标志、清等待节点 → 返回（下一周期继续下一条）。
4. 否则 `Scl_StepOnce()` 推进一个动作。

## 3. 单步：Scl_StepOnce()

先定位“当前区域的扫描界 `bnd`”（栈顶帧的 end；无帧=程序结尾），然后：

- 用引号保护的子句扫描取**下一条子句** [cs,ce) 与续点 nx：
  - 若**区域已结束**：
    - 栈顶是段帧（if 分支）→ 弹帧、`s_cur`=续点（回到 if 之后继续）；
    - 栈顶是 while 帧 → 结构异常报错（理论上 -b 时已配对）；
    - **无帧（顶层）→ 脚本自然完成** `Scl_Finish(0)`。
- 有子句则按首关键字分派：

| 首字 | 处理 |
|------|------|
| `if` | 读并清零 G_RETURN，取 `-t`/`-f` 分支 → 压“段帧”执行分支子链 |
| `while` | `-b` 压循环帧；`-e` 读 G_RETURN 判定退出/回跳 |
| `var` | 建/列/查/释放变量 |
| `free` | 释放指定/全部变量 |
| `help` | 列出已注册命令 |
| 其它 | 当作**业务命令**（普通式）分派 |

## 4. 内置关键字细节

### 4.1 业务命令（普通式，`cmd a b`）
1. 命令名 = 子句首 token（到空白为止；**不再有 `cmd(a,b)` 函数式**，写了会被当未知命令）。
2. 解析实参（空白分隔；`"..."`/`'...'` 引号参数可含空格；参数里做 `${name}` 展开；
   变量未定义展开为空并提示）。
3. 查命令链表：找不到 → “未知命令 xxx”并跳过该子句（不中断）。
4. 调用：
   - **同步**（`sync==NULL`）：`fn(argc, argv)` 执行完即完；
   - **异步**（`sync!=NULL`）：先设等待节点，再 `fn(argc, argv)` 发起操作并立即返回，
     之后由 `SCL_Loop` 轮询其 `sync` 完成才继续下一条。
5. 子句被消费，游标前移。

### 4.2 if（读 G_RETURN 后清零）
- 读 `G_RETURN`（“读后重置”）。
- 真 → 执行 `-t` 后的分支；假 → 执行 `-f` 后的分支；缺省分支则跳过。
- 分支值可为双引号多指令子链（内部再解析），或单条命令。
- 执行分支 = 压**段帧**（分支文本区间），结束自动回到 if 之后。

### 4.3 while（SCL 层为 do-while）
- `while -b`：先查嵌套深度上限；向前扫描定位配对 `while -e`；压**循环帧**（body 起点、-e 位置、迭代计数）。
- `while -e [N]`：读并清零 G_RETURN——
  - `G_RETURN==false` → 退出循环（弹帧，继续其后指令）；
  - `G_RETURN==true` → 回跳 body（do-while），每圈迭代计数 +1；
  - `N` 为显式迭代上限，否则 `SCL_CFG_WHILE_MAX`（默认 100000）兜底防死循环。
- 若/while 帧共用显式帧栈，深度 ≤ `SCL_CFG_NEST_MAX`，可互相嵌套。

## 5. 变量与 G_RETURN 生命周期

- 变量：静态槽（默认 2），`var`/`free`/`SCL_VarSet` 管理；脚本内可读写。
- `G_RETURN`：默认 false；脚本开始/结束清零；`if`/`while -e` 读取即清零；
  命令侧用 `SCL_Ret_Set(v)` 写、`SCL_Ret_Get()` 读。
- **结束（任何路径：正常/abort/结构错误）都自动 `free` 全部变量并清 G_RETURN**。

## 6. 结束收尾 Scl_Finish(reason)

清 busy、等待节点、帧栈、游标；释放全部变量；G_RETURN 清零；打印 `[scl done]` 或 `[scl abort]`。

## 7. C 侧集成骨架

```c
#include "scl.h"
void SCL_Port_PutChar(char c);          /* 移植：输出单字符 */

void OnChain(const char *chain) {
    if (!SCL_Run(chain)) { /* busy / 过长 / 空 */ }
}
void Main_Loop(void) {
    SCL_Loop();                          /* 每周期推进 */
    if (SCL_Idle()) { /* 上一脚本完成，可提交下一条 */ }
}
/* 异步命令示例（注册时带 sync 回调）：
   static scl_cmd_t c = { "step", CmdStep, SyncStep, NULL }; SCL_RegisterCmd(&c); */
```

## 8. 与上层工具的关系

- 现代语法脚本（`.s2c`）由 `tools/scl_script2chain.py` 转成**一条 SCL 指令链**（普通式、引号交替），
  再走上面的流程执行。
- modern 层 `while(cond)` 为 do-while 语义，直译成 `while -b; body; cond; while -e`，
  与 SCL 层 do-while 一致。
