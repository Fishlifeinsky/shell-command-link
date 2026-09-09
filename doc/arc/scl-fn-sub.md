# SCL fn：内联 vs 运行时子程序（callf/retf）

v0.3d。现代脚本 `fn` 默认**编译期内联**（调用处整段展开、体复制 N 份）。
当函数"又大又被调很多次"时，内联会让 Flash 里的字节码重复膨胀。为此提供
**运行时子程序**：函数体只在链末尾存一份，调用处只发一条 `callf`，返回用
一条 `retf`。代价是最小运行期支持（两条新指令 + 1 个返回点变量）。

## 1. 决策规则（S2C 编译期自动选择）

对每个 `fn`（函数体 AST 顶层语句数为 N，全程序静态调用点数为 C）：

| 条件 | 生成方式 | 说明 |
|------|----------|------|
| 带参（`params != []`） | 恒内联 | 参数须在调用处克隆替换成实参，只有内联能做 |
| N ≤ 3 或 C ≤ 3 | 内联 | 体小/调用少，内联更省，也保语义简单 |
| N > 3 且 C > 3 | **运行时子程序** | 体只存一份，调用处 `callf <L>` |

"调用点数"按**静态 AST**统计（含其它 fn 体内的调用），与循环运行次数无关：
源码写一处调用即计 1。阈值不暴露成配置，写死 3/3（小而美，别加开关）。

**回退保护**：若体（含嵌套 if/while/when 子树）出现
`var/constvar/alias/free/break/continue/ret_set` 任一节点——这些依赖调用点
变量上下文或循环跳转语义，转子程序会失真——则**回退内联**（warning 级，
行为与旧版完全一致，只是不省 Flash）。

## 2. 链形态

```
; 调用处（N 个）
callf L1
...
callf L1
; 主流程其余
echo done
; —— 链末尾，运行时子程序体（唯一一份）——
label L1
...
retf
```

动态（`SCL_Run` 文本）与预编译（`scl_emit_c` → const C）两路都支持
`callf <label>` / `retf`：
- `scl.c` 第二遍编译把 `callf` 目标名查 label 表回填成绝对字节偏移（同 jump）；
  `retf` 无参。
- `scl_emit_c.encode_chain` 同样在第一遍收集 `callf` 目标、第二遍回填；
  `retf` 记 opc、aoff=0。

## 3. 运行语义（scl.c）

- 新增 opcode：`SCL_OP_CALLF = 0x0029`、`SCL_OP_RETF = 0x002A`（0x29/0x2A，
  紧接 `CALLN=0x28` 之后，不冲突注册命令 0x100+）。
- 全局 `s_fn_back`（uint16）：
  - `callf <off>`：校验 `off < bc_len`；`s_fn_back = 下一条指令偏移`；
    `s_pc = off`。
  - `retf`：若 `s_fn_back==0 || s_fn_back >= bc_len` → 报错并 `Finish(1)`；
    否则 `s_pc = s_fn_back`。
  - 程序启动（`SCL_Run` / `SCL_RunProg`）与 `SCL_Init` 都清 `s_fn_back=0`。
- **单层返回、无嵌套/递归**：运行时函数体内部出现的其它 `fn` 调用一律强制
  内联（不产生嵌套 `callf`），运行时函数体内自调用经 `expand_fn` 的递归
  检查报"递归/循环调用"。

## 4. 使用约束（对用户）

- 运行时化只对**无参**、**语句上下文**调用生效：
  - `if/while/when` 条件里调用 `fn` → 恒内联（条件要产出 `G_RETURN`）。
  - 带参 `fn` → 恒内联。
- 函数体内不要写 `var/free/break/continue`——编译器检测到会自动回退内联。
- 函数体赋值目标须为已在顶层 `var` 声明的脚本变量。
- 直观收益：几千次重复调用的"大"无参动作（如加热/传输/自检流程）只占一份
  Flash 体积，运行开销只是每次 `callf/retf` 各 1 拍。

## 5. 验证

- `tools/s2c_test.py`：新增单元（无参体>3 调用>3 → callf/retf 精确链；
  体≤3 或带参 → 仍内联）、动态回喂（SCL_Run 执行 4 次子程序结果正确）、
  预编译回喂（SCL_RunProg + encode_chain 回填）用例。
- `example/big_demo`：`seg` 等带参/小函数仍走内联，全量回归 PASS=19。
