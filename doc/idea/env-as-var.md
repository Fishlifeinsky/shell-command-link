# ENV 并入 var 语义（init 时创建，和 var 同一层）

> 状态：**提案中**
> 场景分类：变量与绑定 ＋ 配置裁剪
> 提出者 / 日期：用户指示 / 2026-09-10
> 关联：`scl/Src/scl_env.c`、`scl/Src/scl_var.c`、`scl/Inc/scl.h`（`Scl_Env_*`）

---

## 1. 场景与动机

现在 env 是**自成一层**的独立表：有自己的存储、自己的 `Scl_Env_*` API、自己的序列化。
但它与 `var` 的语义高度重复 —— 都是"名字 → 类型化文本值"。

按用户指示统一：**ENV 就是 var，只不过在 init 时被创建**（默认表批量声明），此后与 var
**共用同一张表、同一批读写 API**。区别只剩两点：
1. env 项**由默认表在初始化时创建**（var 是运行期脚本声明）；
2. env 项**可序列化**（持久化配置）。

## 2. 现状（已核实）

| 项 | 现状 |
|---|---|
| 存储 | `scl_env.c` 独立表（`SCL_CFG_ENV_MAX` 项） |
| API | `Scl_Env_RegisterDefault` / `Reset` / `Set` / `Save` / `Load` / `FreeAll` / `Count` / `Enum` |
| 定义结构 | `scl_env_def_t { name, type, value }`（默认表） |
| 查找优先级 | 会话 var **>** env |
| 裁剪 | `SCL_CFG_ENV_EN`（mini 态默认 0） |

## 3. 方案

1. **入库**：`Scl_Env_RegisterDefault(tab,n)` 改为"在 `Scl_VarInit` 之后，把默认表逐条
   `SCL_VarSetT` 建为会话变量"，并给这些项打上 `env` 标志（`scl_var_t` 增 1 bit）。
2. **同层**：`Scl_Env_Set/Count/Enum` 直接转发到 var 表（`SCL_VarSetT` / `SCL_VarCount` / 只枚举 env 标志项）。
3. **优先级**：env 不再是"另一层"，因此**自然并入 var**，唯一优先级仍是 `static(绑定) > var(含 env)`。
   —— 这条语义变化需要在 `doc/spec/` 写明（原来 env 排在 var 之后）。
4. **序列化**：`Scl_Env_Save/Load` 遍历 var 表，只处理 `env` 标志项；
   **保持字段顺序/格式不变**（兼容已有固化数据）。
5. **容量**：var 表需容纳 `VAR_MAX + ENV_MAX`；建议 `SCL_CFG_VAR_MAX` 语义变为"用户脚本变量槽"，
   另加 `SCL_CFG_ENV_MAX` 预留，init 时按 `VAR_MAX + ENV_MAX` 建表（或直接把两者相加写进配置）。
6. **裁剪**：`SCL_CFG_ENV_EN=0` 时不装载默认表、不编 `Scl_Env_*` 序列化代码（保持现有裁剪能力）。

## 4. 影响面

| 维度 | 影响 |
|---|---|
| 静态 RAM | **下降**：省掉整张 env 表（`ENV_MAX × 槽大小`）；但 var 表要按 `VAR_MAX+ENV_MAX` 开 → 净变化需实测 |
| Flash | 下降（env 独立路径的重复代码消失；序列化代码保留） |
| 公共 API | `Scl_Env_*` 签名不变（语义变薄封装）；**查找优先级变化**（env 从"第二层"变为 var 同层） |
| 配置开关 | `SCL_CFG_ENV_EN` 保留；可能需要新增 `SCL_CFG_VAR_TOTAL_MAX` 或自动推导 |
| 普通态 / mini 态 | mini 默认无 env，不受影响；普通态行为变化（见 §8 风险） |
| 生成物与工具链 | 无 |
| 向后兼容性 | **序列化格式兼容**；但"env 与 var 同名时的优先级"变化属于行为变更（需在 spec 中标注） |

## 5. 执行步骤

1. `scl_var_t` 增 `env` 标志位（复用现有位域/字节）；
2. `scl_env.c` 重写为 var 的薄封装（保留全部 `Scl_Env_*` 签名）；
3. `Scl_VarInit` 后调用默认表装载；
4. 枚举/计数/序列化按 `env` 标志过滤；
5. 回归 + 序列化 round-trip 用例 + 尺寸对比。

## 6. 验证（可直接复制执行）

```powershell
python tools/scl_build.py test      # 全量回归（含 env 相关用例）
python tools/scl_build.py sizes     # 前后 Flash/RAM（预期 RAM 下降）
# 新增用例：
#   1) env 默认表在 SCL_Init 后可读（SCL_VarGet 命中）
#   2) SCL_VarSet 可改 env 项（同层语义）
#   3) Save→Load round-trip 后值一致
```

期望数据：

| 指标 | 改前 | 改后 |
|---|---|---|
| 默认档 Flash / RAM | 21354 / 2014 | 待填 |
| 档A（关 RUN_TEXT）Flash / RAM | 17812 / 678 | 待填 |

## 7. 闭环标准

- [ ] 编译：普通态 + mini 态零错误零告警
- [ ] 回归：`tools/scl_build.py test` 全绿
- [ ] 用例：env 读/写/枚举/序列化 round-trip 全过（新增）
- [ ] 体积：前后实测数字
- [ ] 文档：语义与优先级写入 `doc/spec/`，机制入 `doc/arc/`
- [ ] 提交：`change:`
- [ ] 回填：本文件补数据与提交号

## 8. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| 优先级变化 | 原来 var 优先于 env；并入后 env 就是 var（同名即冲突/覆盖） | 默认表装载时**跳过已存在的 var 名**；文档写明 |
| 固化数据兼容 | `Scl_Env_Load` 的字段依赖名字顺序 | 保持 Save 输出顺序 = 默认表顺序（不变） |
| 槽位不足 | var 表要同时装脚本 var 与 env | 提供 `SCL_CFG_VAR_MAX`/`ENV_MAX` 相加的显式宏 + 构建期 `#error` |
| 裁剪退化 | 合并后难以只砍 env | 保留 `SCL_CFG_ENV_EN` 分支 |

## 9. 决策（实现者定）

1. var 表容量：自动 `VAR_MAX + ENV_MAX`，还是新增一个显式 `SCL_CFG_VAR_TOTAL_MAX`？（建议自动 + 可覆盖）
2. 同名冲突：env 跳过已有 var，还是 var 声明时报错？（建议**env 跳过**，保证旧工程行为）
3. `Scl_Env_*` 是否保留为公开 API（薄封装）？（建议**保留**，避免大范围改调用方）

## 10. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 建档（仅规划） | — | — |
