# v0.2 设计：类型化参数缓存 + 多类型变量 + int/bool 运算指令（草案·待确认）

- 版本：v0.2（基于 v0.1 字节码 + label/jump 运行时）
- 状态：**已确认（默认项 A/B/C/D 全部采用），v0.2 实施中**
- 用户已确认（2026-09-07）：
  1. 参数 type 块：**type 开头、仅 string 带 len**
  2. 变量类型：**显式声明** `var bool b=true` / `var int i=1`
  3. 现代源运算：**直接写表达式**（`a+b` / `n<3` / `a&&b`），编译器降级成指令
  4. flag：**变量也可声明 flag**

---

## 1. 目标

- 参数入缓存时即做类型化解析（不再依赖空格在运行时切分）；
- 变量带类型：bool / string / int / flag；
- 新增一组参考 C 的 int/bool 内置运算指令；
- S2C 现代层把 var/表达式降级为新的文本链/指令。

## 2. 参数 type 块规范（运行期参数字节缓存）

每条带参指令的参数区 = 若干连续 **type 块**，`argOff` 指向第一个块的 type 字节；
`argOff==0` 仍为“无参”哨兵（保留缓存第 0 字节）。

| type | 名称   | 编码                                    | 长度 |
|------|--------|-----------------------------------------|------|
| 0x01 | BOOL   | `01` + `v`(1)                           | 2B   |
| 0x02 | INT    | `02` + `v`(4B 大端，有符号 int32)         | 5B   |
| 0x03 | FLAG   | `03` + `c`(1，`-x` 的字符 x，不含 '-')    | 2B   |
| 0x04 | STR    | `04` + `len`(1B) + `bytes`               | len+2B |

- 定长类型（bool/int/flag）无需 len，按已知宽度顺序推进即可定位下一块；
- string 变长，靠自身 len 界定，故 type 之后**再接一个 len**；
- 取消原 `[len(1)][原文]` + 空格分隔方式；查找/遍历参数只按 type 头。

## 3. 编译期参数解析规则（文本 → type 块）

对**业务命令 / 运算指令**的参数做字面量类型化（token 切分沿用引号分组，`${}` 不在此刻展开）：

| 源 token 形态 | 归类 | 说明 |
|---|---|---|
| `-x`（短横线 + 单字符） | FLAG | 存字符 `x`；多字符 `-abc` 按 STR 处理 |
| `true` / `false` | BOOL | 不区分大小写 |
| 十进制整数 `[-]?[0-9]+` | INT | 存 int32；溢出报错 |
| 其它（含 `${x}`、含空格引号串、普通串） | STR | 原文保留，运行时展开 `${}` 再文本化 |

> 元指令（`var/free/help/label/jump`）参数仍按 **STR 块整存原文**，由各自内部解析
> （`var` 内含 `type name = value` 结构、`jump` 的 `-a/-b` 模式位等不适用通用类型化）。（已确认 A）

## 4. 变量：显式类型存储

运行时链语法（破坏性变更，v0.1 的 `var name=value` 需补类型）：

```
var <type> <name> = <value>     // type: bool | int | flag | string
var                            // 列表（带类型列）
var <name>                     // 查单个
var free                       // 释放全部（同 free）
free [name]
```

- 变量槽存储：`name` + `type` + 规范化文本值（固定缓冲，沿用无 malloc）；
  - bool 规范化 `true`/`false`；int 十进制文本；flag 存 `-x`；string 原样；
- `SCL_VarGet(name)` 仍返回文本 → `${x}` 语义不变，老命令兼容；
- 新增类型化读写接口给 C 命令 / 运算指令取数（如按 type 返回 int/bool 真值）；
- flag 变量：`var flag f = -x`，真值 = 已定义且值非空；
- 变量数量默认：VAR_MAX 2→4（已确认 B）。

## 5. 内置运算指令（参考 C；注册为库内置命令，占保留关键字）

统一遵循 v0.1 的真值约定：**比较/逻辑结果 → G_RETURN（供 jump -a）**；
算术结果写回指定变量（`op a b dst`，dst 覆盖为 int）。

| 类别 | 指令（词） | 参数 | 结果 |
|---|---|---|---|
| int 算术 | `iadd isub imul idiv imod` | `a b dst`（a/b=变量名或 int 字面量） | 写回 dst(int) |
| int 一元 | `ineg` | `a dst` | 写回 dst |
| int 比较 | `ieq ine igt ige ilt ile` | `a b` | → G_RETURN |
| bool 逻辑 | `band bor` | `a b`（bool 变量/字面量） | → G_RETURN |
| bool 一元 | `bnot` | `a` | → G_RETURN |
| 真值装载 | `btest` | 变量名 | → G_RETURN（int≠0 / bool true / flag 定义） |

- 空操作数解析：变量名 → 取变量文本再按语义转数；字面量在编译期已按 INT/BOOL 块编码；
- S2C 现代层映射（示例）：`n=n+1` → `iadd n 1 n`；`n<3` → `ilt n 3`；`a&&b` 见下。

## 6. S2C 现代层改动（范围压缩，默认）

- `var` 支持类型：`var bool b = true`、`var int n = 0`、`var flag f = -x`、`var string s = "hi"`；
  不带类型报错（提示补类型）；
- **条件表达式先支持“原子条件”**：比较（`a<b` 等）、布尔变量、`!b`、`true/false`、int/bool/flag 变量真值
  → 编译器降级为比较/逻辑指令后接 `jump -a`（完全复用 v0.1 label/jump 骨架）；
- **暂不支持 `&& ||` 及括号复合表达式**（需临时变量/短路，放 v0.3）（已确认 C）；
- 赋值语句 `n = n + 1`（整型）→ `iadd` 系指令；`n = 5` 属 var/Set 范畴。

## 7. 兼容性破坏 & 影响文件

- 破坏：v0.1 脚本 `var x=v` → `var string x=v`（或按值给类型）；demo .s2c / 测试 / 文档同步改；
- demo_cmds：删除 `add`、`cmp`（由内置运算符承担），保留 `setret`（S2C 的 `ret(1)` 糖仍用它）（已确认 D）；
- 新增内置运算符为保留关键字（不能注册为业务命令名），命令注册跳过这些名字；
- 涉及：`scl_cfg.h`（VAR_MAX、类型相关）、`scl.h`（类型化接口）、`scl.c`（参数解析/存储/新指令）、
  `demo_cmds.c/h`、`scl_script2chain.py`（var 类型 + 原子条件/赋值表达式降级）、
  `s2c_test.py`、`main.c`、3 个 `.s2c`、spec/设计/README 文档；
- 回归目标：scl_test 与 s2c_test 全绿后 git 提交并推送（commit 类型 `change`）。

## 8. 实施顺序

1. `scl_cfg.h`/`scl.h`：类型常量、变量槽结构扩展、接口原型；
2. `scl.c`：参数 type 块编解码、编译期字面量类型化、var 类型解析、类型化变量接口；
3. `scl.c`：注册内置运算指令 + 执行；
4. S2C：var 类型 + 原子条件/赋值表达式降级；
5. 示例 `.s2c`、`demo_cmds`、`main.c`、`s2c_test.py` 更新；
6. 文档（spec/arc/README/本设计定稿）→ 回归 → 提交并推送。
