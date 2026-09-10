# opcode 改为"注册表内下标"

> 状态：**已闭环**（2026-09-10，`dd3721c`）
> 场景分类：运行时/解释器 ＋ 生成器
> 提出者 / 日期：用户指示 / 2026-09-10
> 关联：`scl/Src/scl.c`（`s_next_opc`、`SCL_RegisterCmd`、`Scl_CmdFindOp`）、
> `scl/cmd/scl_cmd_list.c`（生成表）、`tools/scl_emit_c.py`（const prog）

---

## 1. 场景与动机

现在 opcode 是**注册时自增**分配的（`s_next_opc++`），与"命令在注册表里的位置"没有显式关系；
查找 opcode 要**线性遍历链表**（`Scl_CmdFindOp`）。

改为：**opcode = 命令在注册表数组里的下标 + `SCL_OP_CMD_BASE`**。

- 分配规则从"隐式自增"变成"由表顺序决定"，**可预测、可静态推导**（生成器按文件名排序 → 顺序确定）
- 查表从 O(n) 遍历变 O(1)
- 与"表即真源"的注册表机制天然一致

## 2. 现状（已核实）

| 项 | 现状 | 位置 |
|---|---|---|
| 分配 | `cmd->opc = s_next_opc++`，初值 `SCL_OP_CMD_BASE` | `scl.c:120-121`、`SCL_RegisterCmd` |
| 查找 | `Scl_CmdFindOp(opc)` 遍历 `s_cmd_head` 链表 | `scl.c:1128` |
| 文本路径 | 编译器给业务命令发一条"按 opc 调用"指令 | 运行时编译 |
| const prog | 业务命令统一编译为 `CALLN(0x28)` **按名调用** | `tools/scl_emit_c.py:285` |
| 表来源 | `scl/cmd/scl_cmd_list.c`（生成器按文件名排序） | `scl/tool/scl_gen_list.py` |

> 关键：**const prog 不依赖 opc**（按名调用），所以 opc 语义变化**不会破坏已编译的 Flash 程序**。

## 3. 方案

1. `SCL_RegisterCmd` 不再自增：opcode 由**表内下标**给出 —— 注册表初始化时按数组顺序注册，
   第 `i` 条即 `SCL_OP_CMD_BASE + i`；
2. 新增一张**RAM 指针数组** `s_cmd_by_opc[SCL_CFG_CMD_MAX]`（生成器可知命令数 → 由生成表传出
   `SCL_REG_CMD_COUNT`，或运行期动态上限 `SCL_CFG_CMD_MAX`），`Scl_CmdFindOp` 直接 `s_cmd_by_opc[opc - BASE]`；
3. 手工 `SCL_RegisterCmd`（非注册表路径）仍可用，但**不再分配新 opcode**（返回失败或走"仅按名调用"），
   避免与表下标冲突 —— 需要明确策略（见 §9 决策）。
4. 生成器输出 `const int scl_cmd_list_n`（已有）+ 可选 `SCL_REG_CMD_COUNT` 供编译期定数组大小。

### 3.1 实际落地方案（与上表的差异）

本轮**只落了"opcode = 表内下标"这一半**，另加一个**保留区**解决手工注册冲突：

- 新增 `SCL_CFG_OP_CMD_BASE`（0x0100，**公共配置头**，生成物也要用）——
  原因：`SCL_OP_CMD_BASE` 原本是 `scl.c` 里的**私有枚举成员**，生成物 `scl_cmd_list.c`
  只 `#include "scl.h"`，引用它是编译不过的（首轮回归就红在这里，见 §10）；
- 新增 `SCL_CFG_CMD_RESERVE`（默认 64）：注册表占 `BASE .. BASE+63`，
  **手工 `SCL_RegisterCmd` 从 `BASE + RESERVE` 起自增** —— 两条路径彻底不冲突（比"禁止手工注册"更温和）；
- `SCL_RegisterCmd` 改为**只在 `opc == 0` 时分配** opcode，不覆盖表已给的编号；
- 生成物输出 `SCL_REG_CMD_COUNT`，并 `#if > SCL_CFG_CMD_RESERVE` → 构建期 `#error`（不越界）。

**未做（有意）**：`s_cmd_by_opc[]` 指针数组 + `Scl_CmdFindOp` 改 O(1)、
`scl_cmd_t.next` 退役。理由：命令数少，O(n) 遍历代码比 4 B/命令更划算；
且链表退役会牵动 desc/遍历 API，放到 `scl-module-split.md` 里一并评估更合适。

## 4. 影响面

| 维度 | 影响 |
|---|---|
| 静态 RAM | 新增指针数组 4B/命令；若保留链表则 `next` 字段可退役（**相抵**） |
| Flash | 略降（去掉线性遍历代码，O(1) 查表） |
| 公共 API | `SCL_RegisterCmd` 语义变（不再自动分配 opc）；`SCL_CmdHead` 保留 |
| 配置开关 | 可选新增 `SCL_CFG_CMD_MAX`（不用生成器时定数组） |
| 普通态 / mini 态 | 两态通用；mini 只用按名调用，影响面更小 |
| 生成物与工具链 | 生成器多输出一个"命令数"常量；`scl.cmake` 无需改 |
| 向后兼容性 | **opc 数值含义变化**（不落盘，故不影响 const prog）；手工注册方需适配 |

## 5. 执行步骤

1. 生成器：输出 `scl_cmd_list_n` 已有；新增 `#define SCL_REG_CMD_COUNT n`；
2. `scl.c`：新增 `s_cmd_by_opc[]` + 注册表路径按下标写 opc；`Scl_CmdFindOp` 改 O(1)；
3. 手工注册路径：opc 保持"未分配"（按名调用仍可用），并在文档写明；
4. 回归 + 断言测试（第 i 条命令的 opc == BASE+i）。

## 6. 验证（可直接复制执行）

```powershell
python tools/scl_build.py test      # 全量回归
python tools/scl_build.py sizes     # 前后 Flash/RAM
# 新增断言用例：注册后逐条比对 opc == SCL_OP_CMD_BASE + 下标
```

期望：回归全绿；`sizes` 变化在 ±100B 内（链表 vs 数组相抵）。

> 实测：普通档 +8 B、`-Os` +4 B、RAM 不变 —— **符合预期**（见 §10）。

## 7. 闭环标准（已全部达成，见 §10）

- [x] 编译：普通态 + mini 态零错误零告警
- [x] 回归：`tools/scl_build.py test` 全绿
- [x] 断言：opc == BASE + 表下标（`example/test_cmd_opc.c`，纳入回归）
- [x] 体积：前后实测数字（§10）
- [x] 文档：结论入 `doc/arc/scl-cmd-registry.md`
- [x] 提交：`change:`
- [x] 回填：本文件补数据与提交号

## 8. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| 手工注册与表混用 | 两条路径的 opc 分配冲突 | 手工注册不分配 opc（仅按名）；或要求先注册表后手工 |
| 表顺序变化 | 换文件名/加命令 → opc 编号整体位移 | 不落盘故安全；文档写明"opc 非持久契约" |
| 数组上限 | 不用生成器时需 `SCL_CFG_CMD_MAX` | 默认给足（如 32），超限构建期报错 |

## 9. 决策（已由实现者定）

1. 手工 `SCL_RegisterCmd` 是否允许继续分配 opc？
   → **允许，但走保留区**：注册表占 `BASE..BASE+RESERVE-1`，手工注册从 `BASE+RESERVE` 起自增。
   比"禁止"更温和，且两区间物理隔离、不会撞号。
2. 是否借机让 `scl_cmd_t.next` 退役（链表→数组）？→ **本轮不做**（理由见 §3.1）。
3. `SCL_OP_CMD_BASE` 保持现值？→ **是**，但**上提为公共宏 `SCL_CFG_OP_CMD_BASE`**（生成物必须可见）。

## 10. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 建档（仅规划） | — | — |
| 2026-09-10 | 实现：`SCL_CFG_OP_CMD_BASE` / `SCL_CFG_CMD_RESERVE` / 生成物按下标写 opc / `SCL_RegisterCmd` 条件分配 / 断言用例 | 首轮回归 **红**：`SCL_OP_CMD_BASE` 为 `scl.c` 私有枚举，生成物不可见（编译错） | — |
| 2026-09-10 | 修复：把 opcode 起点上提为公共宏 `SCL_CFG_OP_CMD_BASE` | 回归 **全绿** | `dd3721c` |

**最终闭环数据（`dd3721c`）：**

| 项 | 结果 |
|---|---|
| 编译（普通态 + mini + 7 组裁剪开关） | 全零警告 |
| 全量回归 | **全绿**：`scl_test 115/0`、`mcu_boot_sim 8/0`、`mini_boot_sim 10/0`、`s2c_test 111/0` |
| opc 断言 | `example/test_cmd_opc.c`：链表顺序 == 表顺序，且 `opc == BASE + i` 连续，**纳入回归** |
| 尺寸（ARM `-O2`，口径 Flash=`.text+.rodata+.data`，RAM=`.data+.bss`） | 默认档 `21354 → 21362`（+8 B）/ RAM `2014` 不变；关绑定表 `21138 → 21146`（+8）；`-Os` `16864 → 16868`（+4）；档A `17812 → 17820`；档A+关消息 `10191 → 10199`；档B `21366 → 21374`；mini `4946 → 4954`（+8）、mini `-Os` `3640 → 3644`（+4） |
| 文档 | 结论入 `doc/arc/scl-cmd-registry.md`（§5 opcode 分配模型、§10 实测） |
| 提交 | `change: opcode 改为注册表下标(BASE+i), 手工注册走保留区` |

> 改动量：RAM **零变化**，Flash **+4~8 B**（来自 `SCL_RegisterCmd` 的 `opc == 0` 判空），
> 落在提案 §6 的 ±100 B 期望内。

**遗留（未做）：**

1. `s_cmd_by_opc[]` O(1) 查表 + `scl_cmd_t.next` 退役 —— 与 `scl-module-split.md` 合并评估；
2. 旧的外部工程若手工注册命令并**假定 opc 从 0x0100 连续**，需要同步（opc 非持久契约，按名调用不受影响）。
