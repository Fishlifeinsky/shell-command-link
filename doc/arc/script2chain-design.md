# Script2Chain：现代语法脚本 → SCL 指令链（Python 工具）设计

- 状态：v1 已实现（2026-09-07）
- 工具：`tools/scl_script2chain.py`（纯标准库，无第三方依赖）
- 测试：`tools/s2c_test.py` + `example/chain_runner.c`（真实回喂 C 库执行）

## 1. 目标

让用户用**可读的现代语法**（多行、注释、大括号块、括号式函数调用）写"指令脚本"，
再由本工具**转译成单行 SCL 指令链**，可直接喂给 `SCL_Run()`。
> 说明：SCL 指令链已移除函数式调用，故转译器输出一律为 SCL **普通式** `cmd a b`；
> 现代源语法仍用 `cmd(a, b)` 书写（更好看），由本工具负责“去括号”。

```s2c
# 现代语法（demo2.s2c 简化）
var sp = 100

fn not_done() {       # 用户自定义"判定函数"：每圈执行并置 G_RETURN
    demo_inc()
}

while (not_done()) {  # do-while：body 先跑一次再判（直译 SCL）
    echo("step", ${sp})
}
```

转译结果（示意，普通式 do-while）：
```
var sp=100;demo_reset 3;while -b;echo step ${sp};demo_inc;while -e;echo loop-end
```

## 2. 约束与映射原则

SCL 控制流只有 `G_RETURN`（命令写入、`if`/`while -e` 读取即清零）。因此现代脚本的
`if/while` **条件不是表达式求值**，而是"一段会产生 G_RETURN 的子链"：

| 现代写法 | 语义 | 转译 |
|----------|------|------|
| `if (fn(...)) {A} else {B}` | 先跑条件链，G_RETURN 真→A、假→B | `<cond>; if -t "A" -f "B"` |
| `if {A} else {B}`（无条件） | 沿用**当前** G_RETURN | `if -t "A" -f "B"` |
| `while (fn(...)) {B}` | **do-while**（body 先跑一次再判） | `while -b;B;<c1>;while -e` |
| `ret(1)` / `true` / `false` | 置 G_RETURN（映射到目标命令） | `<ret_setter> 1/0` |

- 输出命令统一为 SCL **普通式**：`cmd a b`；含空格的参数自动加引号。括号只在 modern 源语法里出现，转译时被剥掉。
- `while(cond){body}` 现为 **do-while 语义**（用户 2026-09-07 确认）：body 先跑一次，之后每圈先跑 body 再判 cond，
  cond 为真继续、为假退出 → 直译 `while -b; body; cond; while -e`，**无"先判"门控**。
  `while(false)` 仍会执行一次 body；`while(true)` 依赖 `SCL_CFG_WHILE_MAX` 兜底退出（转译时给警告）。
- 用户自定义 `fn name(args){...}`：**编译期内联展开**（textual inline），调用点替换为函数体，
  参数按字面替换。原因：SCL 无运行时函数/调用栈且占用低。fn 体可作为条件（末句置 G_RETURN）。
- `if (cond)` 为主；`if {}`（无参）仍支持，用于"上一条命令刚写完 G_RETURN，立即据此分支"的常见电机测法。

## 3. 引号处理（关键）

SCL 分支用引号括子链，支持 `"`/`'` **交替嵌套**（引号栈）。生成器按“嵌套深度奇偶”选引号：

- 顶层（深度 0）打开分支用 `"`；
- 深度 1（已在 `"..."` 内）再开分支用 `'`；
- 深度 2 再用 `"` … 依次交替。
- 命令实参需要引号时，同样选用“与当前最内层包裹引号不同”的类型。

因此只要字符串内不含两个引号（且不跨层），生成的链 SCL 都能正确解析。

## 4. 静态校验（转译时给出错误/警告）

- 保留关键字不可作命令名：`if while var free help`，以及语法字 `else fn true false ret`。
- 变量：名 ≤8；**同时存活 ≤2**；字面值 ≤15 字符（含 `${}` 时运行时才定，仅提示）。
- 单条链长度超 `--max-len`（默认 256，对应 SCL_CFG_SCRIPT_MAX）→ 警告；
  长脚本可放大目标宏 `SCL_CFG_SCRIPT_MAX` 或 `--max-len`。
- 未闭合注释/引号/括号、缺 `)` 等给出文件行列。
- fn 递归/循环 → 报错。

## 5. 产物

| 文件 | 说明 |
|------|------|
| `tools/scl_script2chain.py` | 转换器（CLI：文件或 stdin；`--ret-setter` / `--max-len` / `-o`） |
| `tools/s2c_test.py` | 测试：现代脚本 → 期望链比对 + 真实回喂 |
| `example/chain_runner.c` | 小工具：读一条链文件 → SCL_Run 跑完（配合 demo 命令） |
| `example/s2c/*.s2c` | 示例现代脚本 |

CLI：
```
python tools/scl_script2chain.py example/s2c/demo1.s2c            # 打印指令链
python tools/scl_script2chain.py example/s2c/demo1.s2c -o out.chain
python tools/scl_script2chain.py --ret-setter setret < in.s2c
```
