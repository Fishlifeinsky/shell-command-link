# Script2Chain：现代语法脚本 → SCL 指令链（Python 工具）设计

- 状态：v4 已实现（2026-09-07）
- 工具：`tools/scl_script2chain.py`（纯标准库，无第三方依赖）
- 测试：`tools/s2c_test.py` + `example/chain_runner.c`（真实回喂 C 库执行）
- ⚠️ v4：SCL 运行时改为字节码 + label/jump，if/while 文本已移除；本转译器负责把高级
  if/while **下翻译**成 label/jump 线性汇编输出（普通式命令 `cmd a b`）。

## 1. 目标

让用户用**可读的现代语法**（多行、注释、大括号块、括号式函数调用）写"指令脚本"，
再由本工具**转译成单行 SCL 指令链**，可直接喂给 `SCL_Run()`。
> 说明：SCL 指令链已移除函数式调用与 if/while 文本；转译器输出普通式 `cmd a b` +
> `label/jump` 汇编；现代源语法仍用 `cmd(a, b)`/`if/while` 书写，由本工具降级。

```s2c
# 现代语法（示例）
var sp = 100
var int i = 0
while (i < 3) {           # while：标准先判再跑
    echo("step", ${sp})
    i = i + 1
}
do {                      # do{..}while：先跑一次再判
    demo_inc()
} while (i > 0)
when (i) {                # when：主语值匹配（Kotlin 风格；无主语=守卫链）
    1    -> echo("one")
    2, 3 -> echo("two/three")
    else -> echo("other")
}
```

转译结果（示意，label/jump 汇编）：
```
# while(标准先判)：<cond> 求 G_RETURN，真→body、假→出口；body 尾回跳重判
label L1; <cond>; jump -a L2; jump L3; label L2; <body>; jump L1; label L3
# do{..}while(先跑一次)：body 先执行，再到条件判定，真→回 body
label L1; <body>; label L2; <cond>; jump -a L1; label L3
# when(主语) 值匹配：依次比较，命中跳对应臂体，执行后跳 when 尾（无 fallthrough）
ieq <subj> 1; jump -a L_a; ieq <subj> 2; jump -a L_b; <else>; jump L_end;
label L_a; <arm1>; jump L_end; label L_b; <arm2>; jump L_end; label L_end
```

## 2. 约束与映射原则

SCL 控制流只有 `G_RETURN`（命令写入、`jump -a` 读取即清零）。现代脚本的 `if/while`
**条件不是表达式求值**，而是"一段会产生 G_RETURN 的子链"：

| 现代写法 | 语义 | 转译（汇编） |
|----------|------|--------------|
| `if (fn(...)) {A} else {B}` | 先跑条件链，真→A、假→B | `<cond>; jump -a L_A; B; jump L_E; label L_A; A; label L_E` |
| `if {A} else {B}`（无条件） | 沿用**当前** G_RETURN | `jump -a L_A; B; jump L_E; label L_A; A; label L_E` |
| `while (cond) {B}` | **标准先判**（先判再跑；continue→重判、break→出口） | `label L_c; <cond>; jump -a L_b; jump L_e; label L_b; B; jump L_c; label L_e` |
| `do {B} while (cond)` | **先跑一次再判**（continue→条件、break→出口） | `label L_b; B; label L_c; <cond>; jump -a L_b; label L_e` |
| `when (subj) { m1[,m2] -> A; else -> B }` | 主语值匹配，命中执行该臂后**自动结束**（无 fallthrough） | `<subj==m>; jump -a L_A; <else B>; jump L_e; label L_A; A; jump L_e; label L_e` |
| `when { cond -> A; else -> B }` | 守卫链（依次判条件，等价 else-if） | `<cond>; jump -a L_A; <else B>; jump L_e; label L_A; A; jump L_e; label L_e` |
| `ret(1)` / `true` / `false` | 置 G_RETURN（映射到目标命令） | `<ret_setter> 1/0` |

- 输出命令统一为 SCL **普通式** `cmd a b`；含空格的参数自动加引号；标签名由工具自动生成（`L1`、`L2`…）。
- `while(cond)` 为**标准先判**：先判 cond，假则不执行 body（`while(false)` 正确跳过）；
  `continue` 跳回条件处重判，`break` 跳到循环出口。`while(true)` 无终止条件（建议给条件命令，转译给警告）。
- `do{body}while(cond)` 为**先跑一次再判**（原 while 的 do-while 语义显式化；`demo2_while.s2c` 用此写法）。
- `for` 已移除（v0.4）：用 `var` 初始化 + `while`/`do` 表达，旧脚本得到“for 已移除”明确报错。
- `when`：主语可选。有主语时分支为**匹配值**（原子：变量/数字/字符串/flag/${}，可逗号多值）；
  无主语时分支为**守卫条件**（完整条件表达式）。`else ->` 兜底可省略；匹配值 string/flag 用 `seq` 文本比较、数值用 `ieq`。
- 用户自定义 `fn name(args){...}`：**编译期内联展开**（textual inline），调用点替换为函数体，
  参数按字面替换。fn 体可作为条件（末句置 G_RETURN）。fn 内支持 if/while/do/when。
- `if (cond)` 为主；`if {}`（无参）仍支持，用于"上一条命令刚写完 G_RETURN，立即据此分支"。

## 3. 引号处理

输出是**线性文本**（不再把分支包成引号子链），因此不再需要按嵌套深度交替引号：
实参含空白/`;` 等时按需用 `"..."` 或 `'...'` 包裹即可。

- 顶层（深度 0）打开分支用 `"`；
- 深度 1（已在 `"..."` 内）再开分支用 `'`；
- 深度 2 再用 `"` … 依次交替。
- 命令实参需要引号时，同样选用“与当前最内层包裹引号不同”的类型。

因此只要字符串内不含两个引号（且不跨层），生成的链 SCL 都能正确解析。

## 4. 静态校验（转译时给出错误/警告）

- 保留关键字不可作命令名：`if while var free help`，以及语法字 `else fn true false ret const do when for`
  （`for` 已移除，作语法字给迁移报错）。
- 变量：名 ≤8；**同时存活 ≤4**（默认，CLI `--var-max` 可调，对应目标 `SCL_CFG_VAR_MAX`）；
  字面值 ≤15 字符（含 `${}` 时运行时才定，仅提示）。
- 常量声明：顶层 `const <type> x=v` 或 `var const <type> x=v`（别名等价，类型可省略推断）；
  块内/循环内禁（转译期报“仅允许顶层”，避免循环重入重复定义）。
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
