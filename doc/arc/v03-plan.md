# SCL v0.3 实施计划（已圈选 2026-09-07）

范围（用户确认）：**P0 五项 + 交互终端提前到 v0.3**。
分两批交付，每批可独立回归 + git 提交；全部完成后打 `v0.3` tag。

---

## 批次 1：核心语言 + 可靠性（运行时 + S2C + 测试）

### 1.1 运行时（scl_cfg.h / scl.h / scl.c）

- **A1 循环/步进保护**：新宏 `SCL_CFG_STEP_LIMIT`（0=关，默认 100000）；
  `Scl_StepOnce`/`SCL_Loop` 计步，超限报错并 `Scl_Finish(1)`。
- **A2 int 位运算/移位**（保留字，参考 C，`& | ^ ~ << >>`）：
  `iand ior ixor inot shl shr`，opcode 段 0x20–0x25；
  算术形态：`iand/ior/ixor/shl/shr a b dst`、`inot a dst`（写回 int 变量）；
  S2C 映射 `& | ^ ~ << >>`（注意 `&`/`|` 与 bool 逻辑区分：bool 用 `&&`/`||`）。
- **A3 hex/二进制字面量**：`0x1F` / `0b101`
  - C：`Scl_ParseI32Len`/`Scl_LitType`/`Scl_VarNorm` 支持前缀解析；
  - 溢出/非法值报错照旧。
- opcode 布局核对：现有 0x10–0x1F（IADD…BTEST）满，新增用 0x20+；与注册命令 0x0100+ 不冲突。

### 1.2 S2C（scl_script2chain.py）

- **词法**：`&`、`|`、`~`、`<<`、`>>`、`^` 运算符 token；数字字面量 hex/0b 归类。
- **B1 完整表达式**（新表达式引擎，可嵌套 + 优先级 + 括号）：
  - 逻辑/比较：`&& || !` 与 `== != < <= > >=` → **短路 label/jump 降级**（不占变量槽）；
  - 算术/位：`+ - * / %` 与 `& | ^ ~ << >>` → 需要中间值时编译器分配**隐藏临时变量**（`__t0…`，int，
    用后 `free`；会临时占用变量槽，文档注明）；
  - 右侧表达式写回：`a = <expr>`。
- **B2 `for(init;cond;step){…}`** → 降级 label + jump（do-while 化）。
- **B3 `break` / `continue`** → 编译期跳到当前循环结束/续点标签（纯编译期）。
- 保留 v0.2 报错升级为“已支持”的文案更新；旧“v0.3 暂不支持”用例改为新功能正用例。

### 1.3 测试与裁剪

- `main.c`：位运算/移位/hex/循环保护 新用例；
- `s2c_test.py`：表达式/for/break/continue/优先级 精确链 + 回喂；
- 回归目标：scl_test / s2c_test 全绿。

## 批次 2：交互终端 + 裁剪档 + MCU 示例（example 扩展层，整层可裁）

### 2.1 交互终端（历史/补全/行编辑）

- 新 `example/scl_shell.c`（+`scl_shell.h`）：
  - REPL：提示符读一行当“指令链”跑（复用 `SCL_Run`/`SCL_Loop`），`Ctrl+C` 中断（`SCL_Abort`）；
  - 行编辑：退格/方向/Home/End/清行；
  - **历史**：↑↓ 环形缓冲（`SCL_EX_HIST_MAX`）；
  - **补全**：Tab 前缀匹配 命令链表 + 保留字 + `${变量}`；
- 宏：`SCL_EX_SHELL_EN` / `SCL_EX_LINE_EN` / `SCL_EX_HIST_EN` / `SCL_EX_COMPL_EN`（默认关）；
- PC 输入后端：Windows `_getch`（conio）示例；抽象出读字符以便 MCU 轮询替换。

### 2.2 裁剪配置档模板

- 提供“最小档/全功能档”两组宏清单（文档 + 可选头），并更新内存预算表；
- 合入尺寸/速度报告脚本（P1 D4，顺带）。

### 2.3 MCU 移植示例

- 一个真单片机风格移植示例（UART 轮询 GetChar/PutChar）+ shell 演示 + 最小档配置。

## 交付与风险

- 每批：全量回归（scl_test + s2c_test）→ 文档 → git 提交 →（批次 2 完成后 push + tag v0.3）。
- 风险/注意：
  - 表达式引擎是 S2C 最大改动，用“短路 + 隐藏临时变量”保小内存；var_max 注意临时变量占用；
  - 交互终端在 Windows/PC 与 MCU 的输入后端差异 → 抽象层隔离；
  - 全部新功能保持 `#ifndef` 可裁，默认最小档行为不变。
