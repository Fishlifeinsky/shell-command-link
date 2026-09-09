# SCL v0.3c：脚本命令（s2c 编译产物注册成命令，命令行直接调）

> 类别：架构/使用 · 状态：已实现 · 文件：`tools/scl_script2chain.py`、`tools/scl_emit_c.py`、
> `scl/Src/scl.c`、`example/s2c/demo_focus.s2c`

## 1. 是什么

把一段 `.s2c` 脚本**编译成预编译 const 程序后，注册成一条"命令行命令"**：
在命令行（shell / 串口，SCL 空闲时）直接输入

```text
focus 512 3
```

就执行该脚本，并把 `512`、`3` 作为参数注入脚本（成为变量 `arg0`、`arg1`）。
宿主代码只调一次生成的注册函数即可接入，无需为每个脚本手写 C 命令。

## 2. 参数：直接是变量 arg0..argN

- 不声明形参：命令的第 i 个参数执行时注入为会话变量 `arg<i>`（上限 `SCL_CFG_VAR_MAX`），
  脚本里用 `${arg0}`、`${arg1}`… 读取；文本推断类型（数字→int、`-x`→flag、其余→string）。
- 执行入口 `SCL_Scmd_RunText("focus 512 3")`：
  1. 空闲检查（busy 时拒绝并提示，沿用 `SCL_Run` 语义）；
  2. 切参数（引号可含空白），逐个 `${}` 展开后注入 `arg0..argN`；
  3. `SCL_RunProg()` 执行 const 程序；
  4. 生成器在字节码末尾自动追加**无参 `free`**：脚本命令引入的变量（含 `arg*`）
     执行完自动释放，干净返回。
- 超过 `SCL_CFG_VAR_MAX` 个参数 → 提示"参数过多"，仍注入前 N 个执行。

## 3. 变量别名（编译期，零运行时成本）

```s2c
alias pps arg0        # 之后对 pps 的引用 === arg0
alias rep arg1
var int i = 0
while (i < rep) { i = i + 1 }
echo("cnt=${i} pps=${pps}")
```

- `alias <新名> <目标>`：**编译期**把所有对 `新名` 的引用替换成 `目标`（参数 `${}`、
  表达式操作数、条件比较、字符串比较、`free` 等）。
- 目标可以是：普通运行时变量（如 `arg0`）、另一别名（链式 `alias b a`）、或
  `const` 常量名（会继续 const 折叠）。
- 仅顶层允许；别名须在使用前声明；别名不可与已声明的变量/常量/const 同名，
  不可自指/成环（编译期报错）。
- 别名**不占变量槽、不产链指令**——纯编译期符号替换。

## 4. 生成与接入

```bash
# .s2c → C（含 const 程序 + 尾部 free + scmd 节点与注册函数）
python tools/scl_emit_c.py example/s2c/demo_focus.s2c -o build/focus_prog.c \
       --name focus --cmd focus
```

生成 C 含：
- `const scl_prog_t scl_focus_prog`（bc/argc，Flash）
- `static scl_scmd_t s_scmd_focus = { "focus", &scl_focus_prog, NULL };`
- `void Scl_Scmd_Register_focus(void)`：调 `SCL_Scmd_Register(&s_scmd_focus)`

固件接入：

```c
SCL_Init();
Scl_Demo_Register();              /* 脚本用到的业务命令 */
Scl_Scmd_Register_focus();        /* 注册脚本命令 */
/* 命令行每行：先试脚本命令，非脚本命令回退普通 SCL_Run */
uint8_t r = SCL_Scmd_RunText(line);
if (r == 2u) { SCL_Run(line); }   /* 2=不是脚本命令 */
```

## 5. 命令链 API（`scl/Inc/scl.h`，`SCL_CFG_SCMD_EN` 可裁，需 `RUN_PROG_EN`）

| API | 说明 |
|---|---|
| `SCL_Scmd_Register(scl_scmd_t *cmd)` | 注册脚本命令节点（静态/全局持有） |
| `SCL_Scmd_Find(name)` / `SCL_Scmd_Head()` | 查/遍历（shell 补全、help） |
| `SCL_Scmd_RunText(line)` | 执行 `name 参数...`；1=接受、0=busy/非法、2=非脚本命令 |

`scl_scmd_t = { name, const scl_prog_t *prog, next }`。

## 6. 定位与限制

- **仅整行顶层命令**：脚本命令在"命令行输入、SCL 空闲"时启动整段程序执行；
  它**不是**普通同步命令（普通命令的 fn 在链执行中被同步回调，无法再启动一段程序）。
  因此脚本命令**不可嵌在其它链中间**（`a; focus x; b` 不支持，busy 语义）。
- 与 `fn` 的关系：`fn` 仍是编译期内联；脚本命令是把**整段脚本**变成一个可复用的
  命令行入口。二者互补。
- 命令名不能为保留关键字；脚本自身仍可用 `const`（折叠）、`alias`、循环/条件/`${}`。
- 示例/自测：`example/s2c/demo_focus.s2c`；`tools/s2c_test.py` 第 5 节端到端
  （生成 → 注册 → `SCL_Scmd_RunText` → 比对注入/循环/释放/busy 拒/未知命令名）。
  基线：s2c_test PASS=104 全绿。
