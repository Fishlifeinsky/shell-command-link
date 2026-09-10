# 命令 / 静态变量注册机制（宏声明 + 生成注册表）

> 结论性文档（架构）。决策与实测过程见 `doc/idea/cmd-modular-registration.md`、
> `doc/idea/opcode-from-list-index.md`。
> 适用版本：2026-09-10 起（破坏性变更：`example/demo_cmds.c` 已删除）。

---

## 1. 解决什么问题

嵌入式工程里"加一条命令"原本要在三处动手：写 handler、写 `scl_cmd_desc_t`、
在某个 `Xxx_Register()` 里手工挂上去。命令一多就出现"漏注册 / 漏填描述 / 顺序漂移"。

现在的模型：**声明即注册**。命令文件里用宏声明，Python 扫描生成一张表，
库初始化时按表自动注册。

**不用链接器段**（`.scl_cmd` 之类）：段方案需要改 `.ld`、段名带点、`--gc-sections` 会误删、
IAR/Keil/GCC 写法不一。实测结论留在 `doc/idea/cmd-modular-registration.md` §3.1.1。
本机制**不依赖任何链接器特性**，只依赖"一个 .c 文件被编译进工程"。

## 2. 目录与角色

| 路径 | 角色 |
|---|---|
| `scl/Inc/scl_reg.h` | 声明宏（唯一的"写法约定"入口） |
| `scl/cmd/*.c` | 用户填充：每个命令/变量一个文件 |
| `scl/cmd/scl_cmd_list.c` | **生成物**（`scl/tool/scl_gen_list.py` 产出），入库 |
| `scl/tool/scl_gen_list.py` | 扫描器 + 代码生成器 |
| `scl/scl.cmake` | `scl_collect()` 收集 `Src/*.c` + `cmd/*.c`；`scl_regen()` 重新生成 |

## 3. 写法（`scl_reg.h`）

```c
/* 命令（带参数模板） */
static void Cmd_echo(int argc, char *argv[]) { ... }
static const scl_arg_spec_t a_echo_txt[] = { { "text", SCL_T_STR, 0u, "文本" } };
SCL_CMD_DEFINE(echo, Cmd_echo, NULL, "打印参数", a_echo_txt);

/* 命令（无参数模板） */
SCL_CMD_DEFINE_NA(noop, Cmd_noop, NULL, "空操作");

/* 静态变量（mini 态唯一支持的变量来源；**不允许初始化**） */
SCL_VAR_DEFINE(demo_cnt, SCL_T_INT, Get_demo_cnt, Set_demo_cnt);
```

约束（生成器依赖，破坏即构建期报错）：

1. 宏**第一个参数是名字**，必须是简单标识符 `[A-Za-z_][A-Za-z0-9_]*`；
2. 宏参数保持**括号平衡**，不要用宏拼名字；
3. `SCL_VAR_DEFINE` **不写初值**——初值由宿主用 `SCL_VarSet/SCL_VarSetT` 注入
   （每次 `SCL_Start` 重置宿主值，写死在宏里会被覆盖，故禁止）；
4. 节点/描述/绑定的符号名前缀固定为 `s_cmd_` / `s_desc_` / `s_bind_`（重名由链接器兜底报错）。

`SCL_CFG_CMDDESC_EN=0` 时宏退化为纯节点定义（无 desc），命令仍可按名调用。

## 4. 生成物结构（`scl/cmd/scl_cmd_list.c`）

文件头注释列出**扫描清单**（命令名 + 来源文件:行号），便于"静默漏扫"排查。随后依次输出：

| 内容 | 说明 |
|---|---|
| `extern` 声明 | 每条命令的 `s_cmd_*`（有 desc 时加 `s_desc_*`） |
| `scl_cmd_list[]` | 命令节点指针数组，**按命令名排序** → 顺序确定可推导 |
| `scl_cmd_list_n` | 条数 |
| `scl_var_list` / `scl_var_list_n` | 变量绑定指针数组；**空表时为 `NULL`**（省一个元素，用户要求） |
| `scl_desc_list[]` | desc 指针数组（仅 `CMDDESC_EN`），`static` |
| `#define SCL_REG_CMD_COUNT` + `#if > SCL_CFG_CMD_RESERVE` `#error` | 区间校验 |
| `void SCL_RegList_Init(void)` | 注册入口（库侧弱符号调用） |

## 5. opcode 分配模型（本机制的关键约定）

```
opcode 空间
 0x0000 .. 0x00FF   内建指令（SCL_OP_* 枚举，见 scl.c）
 0x0100 .. 0x013F   注册表命令：opcode = SCL_CFG_OP_CMD_BASE + 表内下标
 0x0140 ..          手工 SCL_RegisterCmd 自增分配（BASE + SCL_CFG_CMD_RESERVE 起）
```

- **注册表命令的 opcode 由表内下标决定**，不由注册顺序自增 —— 可静态推导、与表一致；
- `SCL_RegisterCmd` **只在 `cmd->opc == 0` 时**才分配 opcode，因此不会覆盖表已给的编号；
- 表条数超过 `SCL_CFG_CMD_RESERVE` → **构建期 `#error`**，不会运行期越界；
- 命令名排序 → 增删命令会**整体位移**后续 opcode。这是刻意的：
  **opcode 不是持久契约**。

> 持久性：const prog（`tools/scl_emit_c.py`）把业务命令编译为 `CALLN(0x28)`**按名调用**，
> 不落 opcode。因此 **opcode 语义变化不会破坏已烧录的 Flash 程序**。

## 6. 初始化时序

```
SCL_InitEx()
   ...清状态...
   s_cmd_head = NULL;
   s_next_opc = SCL_CFG_OP_CMD_BASE + SCL_CFG_CMD_RESERVE;   /* 手工注册区起点 */
#if SCL_CFG_REG_LIST_EN
   if (SCL_RegList_Init != NULL) SCL_RegList_Init();          /* 弱符号，缺表则跳过 */
#endif
   ...
   s_inited = 1;
```

- 库侧用**弱符号**引用 `SCL_RegList_Init`：`gcc/Clang` → `__attribute__((weak))`，
  `IAR/Keil` → `__weak`（宏 `SCL_REG_WEAK`）。**没有注册表的工程只是少注册几条命令，不会链接失败**；
- 弱引用**必须在 `s_inited = 1` 之前**调用（否则 `SCL_RegisterCmd` 会因"未初始化"拒绝）；
- `SCL_CFG_REG_LIST_EN=0` 时整段编译掉（纯手工注册的老用法）。

## 7. 静态变量绑定表

`SCL_VarBindOne()` 把 `s_bind_*` 收进库内定长表 `s_binds[SCL_CFG_VAR_BIND_MAX]`：

- 容量**不可单独配置**：由 `SCL_CFG_VAR_BIND_EN` 推导（普通态 8 项；开关置 0 时
  整表不生成、登记与查找退化为空操作，实现侧不会出现零长数组）；
- 超出容量时后续登记被截断（`SCL_VarBindOne` 返回 0），需在集成阶段发现。

- 查找优先级：**static（绑定表）> var（会话表）> env**；
- 关掉 `SCL_CFG_VAR_BIND_EN`（默认 1）→ 绑定表整体编译掉，
  实测 **RAM −128 B / Flash −216 B**；此时 static 变量不可用；
- **mini 态不允许关**（mini 只支持 static 变量，`#error` 拦住）。

## 8. 两态差异

| | 普通态 | mini 态 |
|---|---|---|
| 文本解析 | 有（`RUN_TEXT_EN`） | 无（派生为 0） |
| 参数模板/desc | 有（可关） | 无 |
| 命令调用 | 按名 + 按 opcode | **只按名** |
| 变量来源 | static / const / var | **只 static** |
| 注册表 | 同一张表，同一入口 | 同左（少了 desc 分支） |

## 9. 失败模式与排查

| 现象 | 原因 | 排查 |
|---|---|---|
| 命令"存在但找不到" | 扫描器漏扫 / 文件没进构建 | 看 `scl_cmd_list.c` 头部清单是否有该条 |
| 构建期 `#error 超过 SCL_CFG_CMD_RESERVE` | 命令数超区间 | 调大 `SCL_CFG_CMD_RESERVE` |
| 构建期"无法解析宏" | 宏参数用了宏拼接 / 非简单标识符 | 改回字面标识符 |
| 链接期重名 | 两条命令同名 | 生成器已判重报错；重名符号由链接器兜底 |
| 客户端调用 `Xxx_Register()` 又开了注册表 | 新旧混用 | 删掉手工注册（迁移见 `doc/idea/cmd-modular-registration.md` §5） |
| opcode 编号变了 | 增删/改名命令 | 预期行为；按名调用不受影响 |

## 10. 实测（2026-09-10，ARM Cortex-M4，`arm-none-eabi-size`）

口径：Flash = `.text+.rodata+.data`；RAM = `.data+.bss`；全库编译。

| 档位 | 机制引入前 | 机制引入后 | opcode 下标化后 |
|---|---|---|---|
| 默认全功能 O2 | Flash 20782 / RAM 1882 | 21354 / 2014 | **21362 / 2014** |
| 默认关绑定表 O2 | — | 21146 / 1886 | **21146 / 1886** |
| mini O2 | 4882 / 447 | 4946 / 447 | **4954 / 447** |
| mini -Os | 3600 / — | 3640 / — | **3644 / —** |

结论：注册机制本身代价约 **+560 B Flash / +130 B RAM**（普通态，含 desc 与绑定表；
关掉 `VAR_BIND_EN` 可回收 128 B RAM / 216 B Flash）；opcode 下标化再付 **+4~8 B Flash**，
RAM 零变化 —— 换来"编号可静态推导 + 与表一致 + 手工注册不再冲突"。

## 11. 相关

- 提案与实测过程：`doc/idea/cmd-modular-registration.md`、`doc/idea/opcode-from-list-index.md`
- 变量来源模型：`doc/idea/var-sources.md`
- mini 态裁剪：`doc/arc/scl-mini.md`
- 构建入口：`scl/scl.cmake`、`tools/scl_build.py`
