# SCL v0.3：const 只读常量

> 类别：架构/使用 · 状态：已实现 · 文件：`scl/Src/scl_var.c`、`scl/Src/scl.c`、`tools/scl_script2chain.py`、`example/main.c`(第 10 节)

## 1. 是什么

给变量加"只读"属性：**声明一次后不可再覆盖、不可释放、不可作为写回目标**，
适合放换算系数/设备常量/默认参数，防止脚本或命令误改。

## 2. 三种用法

链式（运行时脚本/串口 shell）：
```text
var const int LIMIT=10        # 显式类型常量
var const string TAG="SCL"    # string 常量
var const                    # （配合 var 列表：显示 const 标记）
```

C API：
```c
SCL_VarSetConst("PI", SCL_T_INT, "314");   /* 建立只读常量（校验/规范化） */
SCL_VarIsConst("PI");                      /* 1 */
SCL_VarSetT("PI", SCL_T_INT, "3");         /* -5：常量不可覆盖 */
SCL_VarFree("PI");                         /* -2：常量不可释放 */
```

现代脚本（S2C，仅顶层）：
```s2c
const int LIMIT = 10          # 编译为链：var const int LIMIT=10
const RATE = 0b1010           # 省略类型 → 推断 int
```
（`if`/循环/`fn` 体内不允许 `const`，报错提示"仅允许顶层"。）

## 3. 行为与保护

- **读不受限**：`${LIMIT}`、比较、算术/真值操作数、`SCL_VarGet/Type` 都能用常量。
- **写被拦**：
  - 再次 `var int LIMIT=..` / `SCL_VarSetT` → 拒绝（`-5`，消息"const 常量，不可覆盖"）；
  - `free LIMIT` / `SCL_VarFree` → 拒绝（`-2`，消息"const 常量，不可释放"）；
  - 算术写回 `iadd LIMIT 1 LIMIT` → 拒绝（消息"目标常量只读"）。
- **普通→const 升级**：`var int x=1` 后再 `var const int x=9` 允许（升级并锁定）；锁定后不可再改。
- **生命周期**：与会话变量一致 —— 脚本结束按 `SCL_VarKeep` 语义整体释放；
  `SCL_VarFreeAll()`（脚本收尾/库复位）会连常量一起清理（系统级清理不受单条保护约束）。
- `var` 列表/单查会显示 `const` 前缀，如 `  const LIMIT : int = 10`。

## 4. 实现要点

- `scl_var_t` 增 `ro`（read-only）标志（scl_priv.h，env 项恒 0）。
- `Scl_VarSetCoreEx(name,type,val,ro)`：新建置 ro；已存在 const → `-5`；
  普通可被 const 覆盖（升级）。
- 新 API：`SCL_VarSetConst()` / `SCL_VarIsConst()`；`SCL_VarFree` 对 const 返回 `-2`。
- 链式 `var` 指令解析支持 `const` 前缀；DoFreeRaw/DoArith 写回分别提示。
- S2C：`const` 入语法字；仅顶层解析为 `constvar` 语句 → 输出 `var const ...`。

## 5. const 折叠：真常量直接放 Flash（v0.3b）

对**固定脚本**（`tools/scl_emit_c.py` 编成 const C 程序 → `SCL_RunProg` 执行，
见 `scl-const-prog.md`），顶层 `const` 走"真常量"路径：

- 声明 `const int LIM = 5` **不需要 var、不降级为 `var const`**，不产运行指令；
- 脚本里对 `LIM` 的**读取引用**（算术 `r = LIM + 1`、比较 `r == LIM`、
  `${LIM}` 参数、字符串常量比较、条件真值等）在 **PC 编译期折叠为字面量**，
  值直接编进 Flash 的字节码/参数缓存；
- 运行时**不占变量槽**（`SCL_CFG_VAR_MAX` 不计），没有"建只读变量"这一步。

动态 `SCL_Run`（文本在 MCU 上即时编译）的 `const` 仍是上文的"运行时会话
只读变量"（`var const`）。两条路径互不影响：`scl_script2chain.translate`
默认不折叠；`scl_emit_c` 编 Flash 程序时以 `const_fold=True` 折叠。

折叠类型矩阵（混用会编译期报错）：
| const 类型 | 可折叠语境 |
|---|---|
| `int` | 算术/比较操作数、btest 真值、`${}` 参数、int 赋值 |
| `bool` | 条件真值/`!` 取反、`${}` 参数、bool 赋值 |
| `flag` | 条件真值(btest)、字符串比较、`${}` 参数 |
| `string` | 命令参数 `{}`、字符串比较(seq/sneq)、string 赋值 |

限制（编译期报错）：const 值不能引用变量；const 名不可再 `var`/赋值/同名声明；
string 常量不能参与 int 运算等错配语境会被拦截。

## 6. 验证

`scl_test` 第 10 节（11 用例）：声明+列表标记、`${}` 读取、覆盖拒、写回拒、free 拒、
普通→const 升级锁定、C API 返回码与清理。基线：scl_test PASS=116 / s2c_test PASS=71。
