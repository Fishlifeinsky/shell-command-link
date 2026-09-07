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

## 5. 验证

`scl_test` 第 10 节（11 用例）：声明+列表标记、`${}` 读取、覆盖拒、写回拒、free 拒、
普通→const 升级锁定、C API 返回码与清理。基线：scl_test PASS=116 / s2c_test PASS=71。
