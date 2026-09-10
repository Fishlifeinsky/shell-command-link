# 命令与静态变量改为"一命令一文件 + 段自注册"（Linux 驱动风格）

> 状态：**提案中**（等确认后再执行）
> 场景分类：运行时/解释器 ＋ 变量与绑定 ＋ 构建工具
> 提出者 / 日期：agent 提案 / 2026-09-10
> 关联：`scl/Inc/scl.h`、`scl/Src/scl.c`（`SCL_RegisterCmd`:872、`SCL_CmdRegisterDesc`:2260、
> `SCL_InitEx` 末尾重置 `s_cmd_head`:2949）、`example/demo_cmds.c`、`tools/scl_build.py`

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

### 3.1 段自注册机制（GNU ld）

新增 `scl/Inc/scl_reg.h`：

```c
/* 段名必须是合法 C 标识符，GNU ld 才会自动生成 __start_/__stop_ 符号 */
#define SCL_SEC_CMDS   __attribute__((used, section("scl_cmds")))
#define SCL_SEC_BINDS  __attribute__((used, section("scl_binds")))

/* 把一个命令节点指针放进段里（只做登记，不产生函数调用） */
#define SCL_CMDREG_ONE(_sym) \
    static scl_cmd_t * const __scl_cmdreg_##_sym SCL_SEC_CMDS = &(s_cmd_##_sym)
```

`scl.c` 在 `SCL_InitEx` 的 `s_cmd_head = NULL; s_next_opc = ...;` **之后**遍历：

```c
#if (SCL_CFG_CMD_AUTOREG_EN != 0u)
extern scl_cmd_t * const __start_scl_cmds[];
extern scl_cmd_t * const __stop_scl_cmds[];
{
    scl_cmd_t * const *it;
    for (it = __start_scl_cmds; it < __stop_scl_cmds; it++)
    {
        scl_cmd_t *nd = *it;
        if (nd == NULL) { continue; }
#if (SCL_CFG_CMDDESC_EN != 0u)
        if (nd->desc != NULL) { SCL_CmdRegisterDesc(nd, nd->desc); continue; }
#endif
        SCL_RegisterCmd(nd);
    }
}
#endif
```

### 3.2 一行定义一个命令

```c
/* 带参数模板 */
#define SCL_CMD_DEFINE(_name, _fn, _sync, _help, _args)                  \
    static scl_cmd_t s_cmd_##_name;                                      \
    static const scl_cmd_desc_t s_desc_##_name = {                       \
        #_name, _help, _args, (int)(sizeof(_args) / sizeof((_args)[0])), \
        _fn, _sync };                                                    \
    static scl_cmd_t * const s_cmdreg_##_name SCL_SEC_CMDS = &s_cmd_##_name

/* 无参数模板（sizeof(NULL) 非法 → 单独宏，项数写 0） */
#define SCL_CMD_DEFINE_NA(_name, _fn, _sync, _help)                      \
    static scl_cmd_t s_cmd_##_name;                                      \
    static const scl_cmd_desc_t s_desc_##_name = { #_name, _help, NULL, 0u, _fn, _sync }; \
    static scl_cmd_t * const s_cmdreg_##_name SCL_SEC_CMDS = &s_cmd_##_name
```

配合构造函数把 `node->desc` 指向字面 desc（节点是 RAM，回填不算额外成本）：

```c
#define SCL_CMD_DEFINE(_name, ...) \
    ... \
    static void __scl_descinit_##_name(void) __attribute__((constructor)); \
    static void __scl_descinit_##_name(void) { s_cmd_##_name.desc = &s_desc_##_name; }
```

> 不用 `constructor` 也可以：把 desc 指针直接放进段里（段元素改为
> `{ scl_cmd_t *node; const scl_cmd_desc_t *desc; }`），初始化时成对使用。
> **推荐后者**（无构造器依赖，IAR/Keil 也能退化为查表），见 §3.6。

### 3.3 静态变量同样处理

```c
/* 单个绑定变量登记进 scl_binds 段 */
#define SCL_VAR_DEFINE(_name, _type, _get, _set)                          \
    static const scl_var_bind_t s_bind_##_name = { #_name, _type, _get, _set }; \
    static const scl_var_bind_t * const __scl_varreg_##_name SCL_SEC_BINDS = &s_bind_##_name
```

`SCL_InitEx` 中遍历 `__start_scl_binds/__stop_scl_binds`，逐个登记（等价于 `SCL_VarBind` 单条版；
需在 `scl_var.c` 增加 `Scl_VarBindOne()` 内部入口，mini 态生效）。

### 3.4 `scl/cmd/` 目录划分

```
scl/cmd/
├── README.md          # 命名/新增命令的步骤
├── cmd_echo.c         # echo
├── cmd_setret.c       # setret
├── cmd_noop.c         # noop
├── cmd_wait.c         # wait（异步示例）
├── cmd_demo_reset.c   # demo_reset
└── cmd_demo_inc.c     # demo_inc
```

- 内容由 `example/demo_cmds.c` 拆分而来；`example/demo_cmds.c` 变为薄壳（或删除并改宿主直接依赖库内命令）；
- 每个文件只含：一个 handler、一份 desc、一条 `SCL_CMD_DEFINE`，**不再有 `Xxx_Register()`**；
- 通过 `SCL_CFG_BUILTIN_CMDS_EN`（新开关）决定是否把这些命令编进库。

### 3.5 顺序 / 优先级

单段 + **链接顺序**决定注册顺序：

- 正确性不受影响（const prog 按名调用，已核实）；
- 仅影响 `help` 列表顺序与运行期 `opc` 编号；
- 需要固定顺序时：构建脚本按**文件名排序**传入 `.c`，或用两级段
  （`scl_cmds.0` 核心 / `scl_cmds.1` 业务）在初始化里先遍历低优先级段。
- **待定**：是否需要"优先级字段"（Linux `subsys_initcall` 风格）。当前建议不做，靠文件名排序。

### 3.6 可移植性回退（必须做）

段机制依赖 GNU ld 的 `__start_/__stop_` 与链接脚本保留段：

| 工具链 | 情况 | 处理 |
|---|---|---|
| GNU ld（arm-none-eabi-gcc / mingw） | 支持 `__start_/__stop_` | 直接可用；`--gc-sections` 下建议在 `.ld` 里 `KEEP(*(scl_cmds))` |
| IAR | 无 `__start_/__stop_`，用 `__section_begin/__section_end` | 提供 `SCL_CFG_CMD_AUTOREG_EN=0` 回退 |
| Keil/ARMCC | 用 `--keep` 或属性段 | 同上回退 |

回退路径：`SCL_CFG_CMD_AUTOREG_EN=0` 时宏不再产生段项，库改为遍历一张
**显式命令表**（`SCL_CMD_TABLE` 数组，由构建时或手工列出）。即"自注册是便利，不是唯一路径"。

### 3.7 可选 phase 2：节点常量化（省 RAM，暂不做）

现状每命令 ≈24 B RAM（node 里 name/fn/sync/next/opc/desc）。若把 name/fn/sync/desc 放 Flash、
只在 RAM 保留 `next`+`opc`（≈6 B），可省 ≈18 B/命令（20 条命令 ≈360 B RAM）。
代价：链表改为"RAM 索引表 + Flash 描述表"双向访问，改动面大。**建议先做 phase 1，量到数字再决定。**

## 4. 影响面（phase 1）

| 维度 | 影响 |
|---|---|
| 静态 RAM | 每命令多 1 个段指针项（4 B，在 Flash 段里）+ 可能的 desc 指针；**RAM 节点不变** → RAM ≈ 不变 |
| Flash | 每命令 +4 B（段项）+ 少量遍历代码（≈40~60 B，一次性） |
| 公共 API | 新增宏与 `scl_reg.h`；`SCL_RegisterCmd`/`SCL_CmdRegisterDesc` **保持不变**（手动注册仍可用） |
| 配置开关 | 新增 `SCL_CFG_CMD_AUTOREG_EN`（段自注册）、`SCL_CFG_BUILTIN_CMDS_EN`（库内命令） |
| 普通态 / mini 态 | 两态通用；mini 侧变量绑定也改为段登记 |
| 生成物与工具链 | `tools/scl_build.py` 源列表加 `scl/cmd/*.c`；`example/CMakeLists.txt`、`big_demo`、`mcu_template` 同步；生成器的 `*_mini_register()` 可后续改为自注册 |
| 向后兼容性 | 默认 `AUTOREG_EN=0` 时行为与现在完全一致；开 `=1` 后宿主**不再需要**调用各 `Xxx_Register()`（但重复注册会导致命令重复挂链，需在文档中说明） |

## 5. 执行步骤

1. 新增 `scl/Inc/scl_reg.h`（段/宏定义，含 `AUTOREG_EN` 分支）；
2. `scl.c`：在 `SCL_InitEx` 末尾加段遍历（`#if AUTOREG_EN`）；
3. `scl_var.c`（mini 臂）：加 `Scl_VarBindOne()` 与绑定段遍历；
4. 建 `scl/cmd/`，把 `example/demo_cmds.c` 拆成 6 个文件 + `README.md`；
5. `tools/scl_build.py`、`example/CMakeLists.txt`、`example/big_demo/`、`example/mcu_template/` 同步源列表；
6. 提供 `.ld` 片段文档（`KEEP(*(scl_cmds))` / `KEEP(*(scl_binds))`）；
7. 增一个自注册示例（`SCL_Init()` 后直接 `echo`）并纳入 `scl_build.py test`。

## 6. 验证（可直接复制执行）

```powershell
python tools/scl_build.py test      # 全量回归（含 mini_boot_sim）
python tools/scl_build.py check     # 裁剪矩阵零告警
python tools/scl_build.py sizes     # 前后 Flash/RAM 对照
# 额外断言：开 AUTOREG_EN 后不调用任何 Xxx_Register()，命令仍可用、help 能列出
```

期望数据（待填）：

| 指标 | 现状 | phase 1 | 差 |
|---|---|---|---|
| Flash（默认档 -O2） | 20782 | | |
| RAM | 1882 | | |
| Flash（mini -O2） | 4882 | | |

## 7. 闭环标准

- [ ] 编译：普通态 + mini 态零错误零告警（含 `AUTOREG_EN=0/1` 两种）
- [ ] 回归：`tools/scl_build.py test` 全绿
- [ ] 体积：默认档/mini 档 Flash 与 RAM 前后实测数字
- [ ] 文档：`doc/arc/` 增"命令自注册机制"，`.ld` 片段写入 `doc/spec/`
- [ ] 提交：`add:`（机制）+ `change:`（迁移命令）+ `md:`
- [ ] 回填：本文件补实测数据与提交号

## 8. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| `--gc-sections` 丢段 | 未加 `KEEP()` 且 `__start_/__stop_` 未被识别为根时，命令全丢 | `.ld` 加 `KEEP`；或 `AUTOREG_EN=0` |
| 工具链不支持段符号 | IAR/Keil | `AUTOREG_EN=0` 走显式表 |
| 重复注册 | 宿主仍调用 `Xxx_Register()` 且又开了自注册 → 同命令挂两次 | 文档说明；`SCL_RegisterCmd` 增加同名去重（可选） |
| 顺序变化 | `help` 顺序/opc 编号变化 | 文件名排序约定；必要时两级段 |
| 移动命令文件 | `example/demo_cmds.c` 被拆分，外部引用其符号的工程需同步 | 保留兼容薄壳或明确记为破坏性变更 |

## 9. 待确认决策

1. **`SCL_CFG_CMD_AUTOREG_EN` 默认值**：默认 `0`（不破坏现有工程）还是默认 `1`（新方式为主）？
2. **`scl/cmd/` 放什么**：只放"机制 + 6 个 demo 命令"，还是把 `big_demo` 的 30+ 命令也一并迁进来做样板？
3. **顺序策略**：接受"链接顺序 + 文件名排序"，还是要引入优先级字段/两级段？
4. **phase 2（节点常量化省 RAM）**：本次一起做，还是先只做 phase 1 量数据？
5. **`example/demo_cmds.c`**：保留兼容薄壳，还是直接删除（破坏性）？

## 10. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 建档（仅规划，未改代码） | 待确认 §9 | — |
