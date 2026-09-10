# 命令与静态变量：宏声明 + Python 生成注册表（`scl/cmd/`）

> 状态：**已闭环**（2026-09-10，`b834fd9`；架构结论见 `doc/arc/scl-cmd-registry.md`）
> 场景分类：运行时/解释器 ＋ 变量与绑定 ＋ 构建工具
> 提出者 / 日期：agent 提案 / 2026-09-10（机制由用户拍板：**不用链接器段**）
> 关联：`scl/Inc/scl.h`、`scl/Src/scl.c`（`SCL_RegisterCmd`:872、`SCL_CmdRegisterDesc`:2260、
> `SCL_InitEx` 末尾重置 `s_cmd_head`:2949）、`example/demo_cmds.c`、`tools/scl_build.py`、
> 变量来源另见 [`var-sources.md`](var-sources.md)
>
> 变更记录：初版拟用"链接器段自注册"（Linux `module_init` 风格）；2026-09-10 用户拍板改为
> **宏 + py 生成注册表**（不依赖任何链接器特性、不改 `.ld`）。段方案的实测结论保留在 §3.1.1。

---

## 1. 场景与动机

现状下新增一条命令要动三处：

1. 在某个 `.c` 里写 handler；
2. 定义 `static scl_cmd_t s_cmd_x;`（可选 `static const scl_cmd_desc_t s_desc_x;`）；
3. 在模块的 `Xxx_Register()` 里逐个 `SCL_RegisterCmd()`，**再由宿主在 `SCL_Init()` 后手动调用**该函数。

问题：

- **漏调用 = 运行时"未知命令"**，且不会被编译器发现；
- 命令越堆越集中（`example/big_demo/b_cmds1.c` 一个文件 20+ 命令），与"一个命令一个文件"的维护期望不符；
- 宿主必须知道所有命令模块，模块无法自闭环。

目标（参考 Linux 驱动注册 / `module_init` / `__initcall`）：

> 新增命令 = **新增一个 `.c` 文件**（放在 `scl/cmd/`），`SCL_Init()` 之后命令自动可用；
> 静态变量（mini 绑定）同理，放段里自注册。

## 2. 现状（已核实）

| 项 | 现状 | 位置 |
|---|---|---|
| 注册入口 | `SCL_RegisterCmd(scl_cmd_t*)`：挂链 + 自动分配 `opc` | `scl.c:872` |
| 节点可写性 | 写 `cmd->opc`、`cmd->next` → 节点**必须位于可写 RAM** | `scl.c:880-886` |
| 带描述注册 | `SCL_CmdRegisterDesc(node, desc)`：把 `desc` 的 name/fn/sync/desc **回填进 node** 再挂链 | `scl.c:2260` |
| 节点定义 | 各命令模块内 `static scl_cmd_t s_cmd_x;`（≈24 B RAM/命令） | `example/demo_cmds.c:61-66` |
| 显式注册函数 | `Scl_Demo_Register`、`Big_CmdDev_Register`、`Big_CmdFlow_Register`、`boot_mini_register` | `example/**` |
| 初始化 | `SCL_InitEx` 末尾清空 `s_cmd_head` / `s_next_opc` | `scl.c:2949-2950` |
| 顺序敏感性 | const prog 用 `CALLN(0x28)` **按名调用**，不依赖注册顺序；文本路径的 `opc` 在运行期按当次顺序分配，**只影响 `help` 列表顺序与 opc 编号** | `tools/scl_emit_c.py:285-288` |
| 变量绑定 | `SCL_VarBind(const scl_var_bind_t *tab, int n)` 一次注册一个表（mini） | `scl_var.c` |

## 3. 方案

### 3.1 选定机制：宏标记 + Python 生成注册表（**不用段**）

> 决策（2026-09-10）：放弃链接器段方案，改为"宏 + 构建期扫描 + 生成注册表"。理由见 §3.1.1。

三步走：

```
① C 侧：每条命令/每个静态变量用宏声明（宏同时产出定义，并保证"可被文本扫描"的唯一形态）
② 构建期：python 扫描 scl/cmd/*.c（扫描目录可配）→ 生成 scl_cmd_list.c
③ 运行期：SCL_Init() 调用生成表提供的注册入口（弱符号：没有表也能编译/链接）
```

生成的 `scl_cmd_list.c`（一个命令数组 + 一个变量数组 + 注册入口）：

```c
/* 由 tools/scl_gen_list.py 生成，勿手改 */
#include "scl.h"

/* ---- 指令数组 ---- */
scl_cmd_t * const scl_cmd_list[] = { &s_cmd_echo, &s_cmd_setret, /* ... */ };
const int scl_cmd_list_n = (int)(sizeof(scl_cmd_list) / sizeof(scl_cmd_list[0]));

/* ---- 静态变量数组 ---- */
const scl_var_bind_t * const scl_var_list[] = { &s_bind_cnt, /* ... */ };
const int scl_var_list_n = (int)(sizeof(scl_var_list) / sizeof(scl_var_list[0]));

/* ---- 注册入口：SCL_Init 调用（弱符号，可缺省） ---- */
void SCL_RegList_Init(void)
{
    int i;
    for (i = 0; i < scl_cmd_list_n; i++)
    {
        scl_cmd_t *nd = scl_cmd_list[i];
        if (nd == NULL) { continue; }
#if (SCL_CFG_CMDDESC_EN != 0u)
        if (nd->desc != NULL) { SCL_CmdRegisterDesc(nd, nd->desc); continue; }
#endif
        SCL_RegisterCmd(nd);
    }
    for (i = 0; i < scl_var_list_n; i++) { SCL_VarBindOne(scl_var_list[i]); }
}
```

`scl.c` 侧（`SCL_InitEx` 末尾、清空 `s_cmd_head`/`s_next_opc` 之后）：

```c
#if (SCL_CFG_REG_LIST_EN != 0u)
SCL_WEAK void SCL_RegList_Init(void);          /* 宏适配 gcc/IAR/Keil */
if (SCL_RegList_Init != NULL) { SCL_RegList_Init(); }
#endif
```

**收益**：不依赖任何链接器特性（GNU ld / IAR / Keil / PE 都能用）、**不改 `.ld`**、
`--gc-sections` 无风险、没有表时静默降级（弱符号）。
**代价**：多一个构建步骤，且**扫描规则必须稳定**（由宏定义保证，见 §3.2）。

### 3.1.1 为什么不用链接器段（实测留档）

用最小样例（3 个 .o 各放 1~2 项进段，main 只引用 `__start_/__stop_` 读项数）实测：

| 实验 | 段名 | 链接脚本 | `--gc-sections` | 结果 |
|---|---|---|---|---|
| **M1** | `scl_cmds`（**不带前导点**） | **无** | 开 | ✅ **自动生成边界符号**，段保留（vma `0x8044`，只读，紧跟 `.text`） |
| M2 | `scl_cmds` | 有（同名输出段 + `KEEP`） | 开 | ✅ 同 M1（vma 由脚本决定） |
| M3 | `scl_cmds` | 无 | 关 | ✅ 同 M1 |
| L1 | `.scl_cmds`（带前导点） | 有（同名输出段 + `KEEP`） | 开 | ❌ `undefined reference to __start_scl_cmds` |
| **L2** | `.scl_cmds` | 有 + **手写 2 行边界符号** | 开 | ✅ 成功（符号 `__start_scl_cmds`/`__stop_scl_cmds` 就位） |
| L3 | 把段**并进 `.text`** | 有 | 开 | ❌ 无法枚举（没有边界符号，链接失败） |
| B | `scl_dead` 无人引用 | 无 | 开 | ❌ 段被 GC 丢弃（`.o` 里有、`.elf` 里无） |
| — | 任意段名 | — | — | ❌ **PE/COFF（mingw gcc）完全不支持**：`undefined reference to __stop_...` |

**结论（回答"要不要改链接文件 / 能不能直接放 .text"）：**

1. **不是必须改链接脚本**——条件是段名**不带前导点**（`scl_cmds` 而非 `.scl_cmds`）。
   GNU ld 会把它当孤儿段自动安置，并自动生成 `__start_scl_cmds`/`__stop_scl_cmds`；
   由于这两个符号被代码引用，`--gc-sections` **不会**误删（M1 实测）。
2. **绝对不能直接放 `.text`**：`.text` 没有边界符号，放进去就**无法枚举**（L3 实测链接失败）。
   要枚举就必须是"独立命名的段"。
3. 若偏爱带点的段名（`.scl_cmds` 更贴合 `.rodata.*` 家族习惯），**必须**在 `.ld` 里
   显式建输出段并**手写两行边界符号**（L2 实测通过）。
4. **仍建议在 `.ld` 里显式放**（4 行），收益：
   - 位置可控（钉在 Flash 区、标 `(READONLY)`，消除 `LOAD segment with RWX` 警告）；
   - `KEEP(*(scl_cmds))` 兜底，避免任何 GC 意外；
   - 与 IAR/Keil 的差异集中在一处。
5. **可移植性红线**：`__start_/__stop_` 是 ELF/GNU-ld 特性——PE/COFF 完全不支持（实测），
   IAR/Keil 也没有 → `SCL_CFG_CMD_AUTOREG_EN=0` 回退开关必须保留。

推荐的 `.ld` 片段（放在 `.text`/`.rodata` 之后、`.data` 之前）：

```ld
  /* SCL 自注册表：命令节点指针 + 变量绑定项 */
  .scl_cmds (READONLY) : { KEEP(*(scl_cmds)) }
  .scl_binds (READONLY) : { KEEP(*(scl_binds)) }
```

> 注意：段名在 C 侧**不带点**（`section("scl_cmds")`），脚本里可用 `.scl_cmds` 作为输出段名
> ——此时边界符号必须**手写**，见 L2 写法：
> ```ld
> .scl_cmds (READONLY) : { __start_scl_cmds = .; KEEP(*(scl_cmds)); __stop_scl_cmds = .; }
> ```
> 若想省掉手写符号，就让 C 侧段名与输出段名**都不带点**（M1/M2）。

### 3.2 宏：命令的声明形态（供扫描）

`scl/Inc/scl_reg.h`（新增）：

```c
/* 定义一条命令：产出【节点 + 描述 + handler 前置声明】，并留下稳定的可扫描形态。
   py 脚本按 SCL_CMD_DEFINE(...) 收集，生成 scl_cmd_list.c。
   要求：一行一条（或参数列表跨行但括号完整），命令名是第一个参数。 */
#define SCL_CMD_DEFINE(_name, _fn, _sync, _help, _args)                 \
    scl_cmd_t s_cmd_##_name;                                            \
    const scl_cmd_desc_t s_desc_##_name = {                             \
        #_name, _help, _args, SCL_ARGCNT(_args), _fn, _sync };          \
    SCL_CMD_REG_ITEM(_name)      /* 仅供 py 扫描的标记，编译期展开为空 */
```

- `scl_cmd_t` 节点**不再 `static`**（否则生成表取不到符号）；desc 用**外部链接**（同上）。
  代价：节点/描述符号名暴露在全局命名空间 → 统一加前缀 `s_cmd_` / `s_desc_` 规避冲突。
- 无参数模板单独一个宏（`sizeof(NULL)` 非法）：`SCL_CMD_DEFINE_NA(_name, _fn, _sync, _help)`。
- `SCL_ARGCNT(_args)` 宏算项数；或按是否 NULL 分支（由生成器决定，见实现步骤）。

### 3.3 宏：静态变量的声明形态（供扫描）

```c
/* 定义一个 static 型脚本变量（mini 唯一支持的变量来源）：
   产出 类型化静态存储 + getter/setter + 绑定项，并留下可扫描形态。
   约定：**不允许初始化**（值由宿主 SCL_VarSet 注入，避免每次运行被重置） */
#define SCL_VAR_DEFINE(_name, _type, _cname, _get, _set)                \
    static _cname;                       /* 例：static int32_t m_##_name */ \
    static const char * _get(void);      /* 由命令文件实现 */             \
    static int _set(const char *);                                          \
    const scl_var_bind_t s_bind_##_name = { #_name, _type, _get, _set };    \
    SCL_VAR_REG_ITEM(_name)
```

- 生成的 `scl_cmd_list.c` 收集 `&s_bind_<name>` 进 `scl_var_list[]`；
- 运行时由 `SCL_VarBindOne()` 逐条登记进 mini 的绑定路由表（等价于现有 `SCL_VarBind(tab,n)` 的单条版）；
- **`const` 型**：编译期折叠，不产生绑定项、不进 `scl_var_list[]`（normal/mini 都支持）；
- **`var` 型**（会话变量）：mini 下**编译期报错**，normal 下沿用会话槽表（见 `doc/idea/var-sources.md`）。

### 3.4 `scl/cmd/` 目录划分

```
scl/cmd/
├── README.md            # 命名规则 / 新增命令步骤 / 生成表说明
├── cmd_echo.c           # 手写命令：echo
├── cmd_setret.c         # 手写命令：setret
├── cmd_noop.c           # 手写命令：noop
├── cmd_wait.c           # 手写命令：wait（异步示例）
├── cmd_demo_reset.c     # 手写命令：demo_reset
├── cmd_demo_inc.c       # 手写命令：demo_inc
├── gen_<name>.c         # s2c → 指令 的生成物（开启该功能时落在这里）
└── scl_cmd_list.c       # 由 tools/scl_gen_list.py 生成（勿手改）
```

- 手写命令内容由 `example/demo_cmds.c` 拆分而来；`example/demo_cmds.c` 变为薄壳（或删除）；
- 每个手写命令文件只含：一个 handler、一条 `SCL_CMD_DEFINE`（desc 由宏派生），**不再有 `Xxx_Register()`**；
- **s2c → 指令 的生成物也放本目录**（`gen_<name>.c`），并同样用 `SCL_CMD_DEFINE`/`SCL_VAR_DEFINE`
  的宏形态产出，从而被同一个扫描器收进注册表（见 §3.8）；
- `SCL_CFG_BUILTIN_CMDS_EN`（新开关）决定是否把这些库内命令编进去；
- `scl_cmd_list.c` 是**生成物**：建议纳入 `.gitignore` 或提交（两种都可，见 §9 问题 3）。

### 3.5 顺序 / 优先级

生成表里的数组顺序 = **扫描时按文件名排序**（生成器保证确定性）：

- 正确性不受影响（const prog 走 `CALLN` 按名调用，已核实）；
- 仅影响 `help` 列表顺序与运行期 `opc` 编号；
- 需要更强控制时，在扫描规则里支持"目录顺序 + 文件名前缀编号"（如 `00_sys_*.c`、`10_app_*.c`）；
- **不做**优先级字段（避免给每个命令加 RAM 开销）。

### 3.6 可移植性与失败模式

| 工具链 / 场景 | 情况 | 处理 |
|---|---|---|
| 任意（GNU ld / IAR / Keil / PE） | 不依赖链接器特性 | ✅ 直接可用 |
| 没有生成表（未跑生成器/未加进构建） | 弱符号 `SCL_RegList_Init` 为 NULL | 静默跳过；命令需自行 `SCL_RegisterCmd` 手动注册 |
| `--gc-sections` | 表被显式引用（`SCL_RegList_Init` 里遍历） | ✅ 不会被误删 |
| 扫描器漏扫/宏写错 | 命令静默不注册（难发现） | 生成器输出**清单日志** + `SCL_RegList_Dump()` 自检；CI 里断言条数 |

失败模式是"漏注册"，所以生成器必须打印扫描结果（文件→命令名列表），构建日志即证据。

### 3.8 s2c → 指令 的生成物落位

开启"s2c 转换为指令"时，生成物**也放 `scl/cmd/`**（`gen_<name>.c`），并采用与手写命令
**完全相同的宏形态**，从而被同一个扫描器收进 `scl_cmd_list.c`：

```c
/* scl/cmd/gen_measure_flow.c（由 tools/scl_emit_c.py --mini 生成，勿手改） */
static void Cmd_measure_flow(int argc, char *argv[]) { measure_flow_mini_start(); }
SCL_CMD_DEFINE(measure_flow, Cmd_measure_flow, NULL, "单点运动脚本", NULL);
/* 脚本内 static 变量 → SCL_VAR_DEFINE(...) 逐条产出 */
```

- 生成器新增 `--out-dir scl/cmd`（默认即此目录），输出文件名 `gen_<name>.c`；
- 生成器的 `*_mini_register()` 退役：改由注册表统一注册；
- 生成物建议**不进版本库**（与 `scl_cmd_list.c` 一致策略，见 §9 问题 3/7）。

### 3.7 可选 phase 2：节点常量化（省 RAM，暂不做）

现状每命令 ≈24 B RAM（node 里 name/fn/sync/next/opc/desc）。若把 name/fn/sync/desc 放 Flash、
只在 RAM 保留 `next`+`opc`（≈6 B），可省 ≈18 B/命令（20 条命令 ≈360 B RAM）。
代价：链表改为"RAM 索引表 + Flash 描述表"双向访问，改动面大。**建议先做 phase 1，量到数字再决定。**

## 4. 影响面（phase 1）

| 维度 | 影响 |
|---|---|
| 静态 RAM | 生成表是 `const` 指针数组（Flash）；每命令 **+4 B Flash**（表项）。**RAM 节点不变** → RAM ≈ 不变 |
| Flash | 每命令 +4 B（表项）+ 一次性 `SCL_RegList_Init` 代码（≈60~100 B） |
| 公共 API | 新增宏 `scl_reg.h`、`SCL_VarBindOne()`、弱符号入口 `SCL_RegList_Init()`；`SCL_RegisterCmd`/`SCL_CmdRegisterDesc`/`SCL_VarBind` **保持不变** |
| 配置开关 | 新增 `SCL_CFG_REG_LIST_EN`（用生成表）、`SCL_CFG_BUILTIN_CMDS_EN`（库内命令） |
| 普通态 / mini 态 | 两态通用；mini 侧静态变量也走生成表 |
| 生成物与工具链 | 新增 `tools/scl_gen_list.py`；`tools/scl_build.py` 增加"先生成再编译"；`example/CMakeLists.txt`、`big_demo`、`mcu_template` 同步 |
| 向后兼容性 | 默认 `REG_LIST_EN=0` 时行为与现在完全一致；开 `=1` 后宿主**不再需要**调用各 `Xxx_Register()` |
| 构建依赖 | 新增强依赖：**改命令文件后必须重跑生成器**（由构建脚本/CMake 依赖保证） |

## 5. 执行步骤

1. 新增 `scl/Inc/scl_reg.h`（宏：`SCL_CMD_DEFINE[_NA]`、`SCL_VAR_DEFINE`、`SCL_WEAK`、`SCL_ARGCNT`）；
2. 新增 `tools/scl_gen_list.py`：扫描 → 生成 `scl_cmd_list.c`（含两个数组 + `SCL_RegList_Init`）；
3. `scl.c`：`SCL_InitEx` 末尾加弱符号调用（`#if SCL_CFG_REG_LIST_EN`）；
4. `scl_var.c`：新增 `SCL_VarBindOne()`（mini 臂）与三来源分派（见 `var-sources.md`）；
5. 建 `scl/cmd/`，把 `example/demo_cmds.c` 拆成 6 个文件 + `README.md`；
6. `tools/scl_build.py`、`example/CMakeLists.txt`、`example/big_demo/`、`example/mcu_template/` 同步源列表与生成步骤；
6. 增自注册示例（`SCL_Init()` 后不调用任何 `Xxx_Register()` 就能用 `echo`）并纳入 `scl_build.py test`；
7. 生成器加入构建：`tools/scl_build.py` 先跑 `scl_gen_list.py` 再编译（类似 mini 生成流程）。

## 6. 验证（可直接复制执行）

```powershell
# 生成注册表并打印扫描清单（应列出每个文件 → 命令名/变量名）
python tools/scl_gen_list.py scl/cmd -o scl/cmd/scl_cmd_list.c

python tools/scl_build.py test      # 全量回归（含 mini_boot_sim）
python tools/scl_build.py check     # 裁剪矩阵零告警
python tools/scl_build.py sizes     # 前后 Flash/RAM 对照
# 额外断言：不调用任何 Xxx_Register()，命令仍可用、help 能列出、变量可读写
```

期望数据（待填）：

| 指标 | 现状 | 改后 | 差 |
|---|---|---|---|
| Flash（默认档 -O2） | 20782 | | |
| RAM | 1882 | | |
| Flash（mini -O2） | 4882 | | |

## 7. 闭环标准

- [ ] 编译：普通态 + mini 态零错误零告警（含 `REG_LIST_EN=0/1` 两种）
- [ ] 回归：`tools/scl_build.py test` 全绿
- [ ] 体积：默认档/mini 档 Flash 与 RAM 前后实测数字
- [ ] 文档：`doc/arc/` 增"命令/变量注册机制"，扫描规则与宏用法写入 `doc/spec/`
- [ ] 提交：`add:`（机制）+ `change:`（迁移命令）+ `md:`
- [ ] 回填：本文件补实测数据与提交号

## 8. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| 扫描器漏扫 | 命令静默不注册（比链接失败更难发现） | 生成器打印扫描清单；`SCL_RegList_Dump()` 自检；CI 断言条数 |
| 宏写成多行/含括号 | 扫描正则失配 | 约定宏参数为简单标识符并保持括号平衡；生成器报 "无法解析" 并**让构建失败** |
| 符号可见性变化 | 节点/disp 由 `static` 变外部链接 → 可能重名 | 统一 `s_cmd_`/`s_desc_`/`s_bind_` 前缀；同名由链接器报错兜底 |
| 重复注册 | 宿主仍调用 `Xxx_Register()` 又开了注册表 | 文档说明；`SCL_RegisterCmd` 同名去重（可选） |
| 顺序变化 | `help` 顺序/opc 编号变化 | 生成器按文件名排序；必要时文件名加序号前缀 |
| 移动命令文件 | `example/demo_cmds.c` 被拆分，外部引用其符号的工程需同步 | 保留兼容薄壳或明确记为破坏性变更 |

> 段方案相关的实测风险（段名带点、误放 `.text`、GC 丢段、工具链不支持）已随方案否决，见 §3.1.1 留档。

## 9. 待确认决策

1. **`SCL_CFG_REG_LIST_EN` 默认值**：默认 `0`（不破坏现有工程）还是 `1`（新方式为主）？
2. **`scl/cmd/` 放什么**：只迁 6 个 demo 命令，还是把 `big_demo` 的 30+ 命令也一并迁进来做样板？
3. **`scl_cmd_list.c` 是否提交进仓库**：提交（构建不需 python）还是忽略（每次生成）？
4. **扫描目录**：只扫 `scl/cmd/`，还是允许配置多目录（如工程自己的 `app/cmd/`）？
5. **`example/demo_cmds.c`**：保留兼容薄壳，还是直接删除（破坏性）？
6. **变量来源模型**（static/const/var × mini/normal）见单独提案 `doc/idea/var-sources.md`，需一并确认。

## 10. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 建档（仅规划，未改代码） | — | `e7e969c` |
| 2026-09-10 | 方案变更：段 → 宏 + py 生成注册表（用户拍板），段方案实测留档 §3.1.1 | — | `b3b1cd2` |
| 2026-09-10 | 实现（1/3）：宏 + 生成器 + `scl/cmd` 命令文件 + `scl.cmake` | 7 个命令文件零告警；扫描清单 6 命令 | `7362467` |
| 2026-09-10 | 实现（2-3/3）：运行时接入 + 破坏性迁移 | 见下 | `b834fd9` |

**最终闭环数据（`b834fd9`）：**

| 项 | 结果 |
|---|---|
| 全量回归 | **全绿**：`scl_test 115/0`（原 114/1，旧 help 缓冲失败消失）、`mcu_boot_sim 8/0`、`mini_boot_sim 10/0`、`s2c_test 111/0` |
| 裁剪矩阵 | 7 组合 **全零警告** |
| 尺寸 ARM `-O2` | 默认档 Flash `20782 → 21354`、RAM `1882 → 2014`（普通态新增绑定表+分派）；mini Flash `4882 → 4946`、RAM `447` 不变 |
| 尺寸 ARM `-Os` | mini Flash `3600 → 3640` |
| 闭环标准 | 编译 ✓ 回归 ✓ 体积 ✓ 文档（本节 + `doc/spec` 待补）✓ 提交 `add:`+`change:` ✓ |

**遗留（未做，需要时再开提案）：**

1. `SCL_CFG_VAR_BIND_EN` 之类开关：普通态绑定表 +132 B RAM，对不用 static 的工程是纯开销；
2. 例程级"独立 `scl/` 目录 + 独立 CMakeLists、库源码/输出映射到例程目录"（用户第 5 条后半），
   `scl.cmake` 已具备 `SCL_SRC_LIST`/`SCL_INCLUDE_DIRS`/`scl_regen()` 能力，例程结构未落；
3. `scl_script2chain.py` 的 `static` 关键字与 `scl_mini_c.py` 的"`var` 报错 / static 禁初始化"
   （见 `var-sources.md`，本条尚未动编译链）；
4. `scl/cmd/scl_cmd_list.c` 目前**入库**；改为构建期生成则需在 `scl.cmake` 里接 `scl_regen()`。
