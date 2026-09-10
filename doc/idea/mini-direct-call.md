# mini 生成代码改为直接调用 handler（跳过按名查找）

> 状态：**搁置**（已量化收益，等需求方决定是否值得）
> 场景分类：生成器
> 提出者 / 日期：agent 提案 / 2026-09-10
> 关联：`tools/scl_mini_c.py`（`_call`）、`scl/Src/scl.c`（`SCL_CmdInvoke`）、
> `doc/arc/scl-mini.md` §8.5

---

## 1. 场景与动机

场景：脚本（s2c）在构建期生成 C 状态机，运行期反复触发命令（如 `drv("${mot}","-v","${pps}","pps")`）。

现状下生成代码写的是：

```c
scl_invoke_arg_t ia[4];
ia[0].text = ...; ia[0].type = SCL_T_STR;
...
uint8_t r = SCL_CmdInvoke("drv", 4, ia);   /* 按名查找 → 拷贝参数 → 校验 → 调用 */
```

动机：`SCL_CmdInvoke` 需要**按名线性查找**并做参数拷贝。若生成器在编译期就知道目标命令的
C 符号，理论上可以直接 `drv(4, argv)`，省掉这条路径。

## 2. 现状（已实测）

`arm-none-eabi-size` / `arm-none-eabi-nm` 实测（Cortex-M3，`-O2`）：

| 符号 | 大小 |
|---|---|
| `SCL_CmdInvoke` | 564 B |
| `Scl_CmdFindName` | 72 B |
| `Scl_StrEq` | 42 B |
| 合计（一次性） | **≈ 678 B** |

生成代码侧：每个调用点仍需准备 `ia[]`（N 个 4B 结构）并分支 `r==0/2`，改为直接调用
**并不会**让每个调用点变小。

## 3. 方案

### 方案 A：生成器加 `--direct`，宿主提供 name→handler 映射

- 生成器输出 `drv(4, argv)` 形式的直接调用；
- 名字→符号映射由宿主提供（宏表或生成器参数传入）。

**代价：**

1. `handler(int argc, char *argv[])` 的 `argv` 必须是**可写**缓冲（handler 会原地改参数），
   所以仍需要一份可写拷贝——库里的 `s_argb[][SCL_CFG_ARG_LEN_MAX]` 就是干这个的。
   直接把字符串字面量传进去会写坏 Flash，因此**省不掉参数拷贝**。
2. 生成器与命令 C 符号**耦合**：现在命令由宿主 `SCL_RegisterCmd` 注册，生成器只认名字。
3. 丢失两项库能力：
   - 异步判定（`nd->sync != NULL` → 返回 `2`）；
   - `desc` 参数模板校验（`Scl_DescCheck`）。
4. 需要在生成代码里自建 dispatch（switch/jump table），本身也要几十~上百字节。

### 方案 B（已采用）：优化参数发射策略

不做直接调用，而是去掉生成代码里的冗余（单引用 `${name}` 直通、去 `av[]` 中转），
已落地：生成物 `.text` 4147→2755（`-O2`），`step()` 3588→2196（−39%）。见 `doc/arc/scl-mini.md` §8。

**结论**：方案 A 是**一次性 ~0.5 KB 级**收益且丢能力；方案 B 是**随脚本规模线性**的收益且零风险。
故当前选 B，A 搁置。

### 方案 C（若要继续压，优先于 A）

- 关 `SCL_CFG_CMDDESC_EN`（省帮助/模板校验表，是剩余 Flash 大头）；
- 收紧 `SCL_CFG_ARG_MAX` / `SCL_CFG_ARG_LEN_MAX`（静态 RAM 按乘积缩小）。

## 4. 影响面（方案 A）

| 维度 | 影响 |
|---|---|
| 静态 RAM | 不变（仍需可写参数缓冲，或略增） |
| Flash | 约 −400 ~ −600 B（一次性） |
| 公共 API / ABI | 无（库 API 不变），但生成物与命令符号强耦合 |
| 配置开关 | 需新增生成器开关（如 `--direct`） |
| 普通态 / mini 态 | 仅 mini 生成物受影响 |
| 生成物与工具链 | 生成器需新增映射参数；宿主需维护 name→符号表 |
| 向后兼容性 | 生成物不兼容（需重新生成），库本身兼容 |

## 5. 执行步骤（若启动）

1. `tools/scl_mini_c.py` 增加 `--direct` 与 `--map <file>`（name→符号表）；
2. 生成代码改为：把参数写入**生成器自有的可写缓冲**（或复用 `SCL_ArgBuf` 类接口）后直接调用；
3. 异步命令需宿主显式声明（如 `--async drv`），生成器据此保留 `s_wait` 分支；
4. 文档与示例同步。

## 6. 验证（可直接复制执行）

```powershell
# 生成 + 编译 + 体积对比
python build\_exp_mini_fast.py        # 实验脚本（若已清理需重建）
python tools/scl_build.py test        # 回归（含 mini_boot_sim）
python tools/scl_build.py sizes       # mini 档 .o 尺寸
```

期望：mini 档 Flash 下降 ≥ 400 B，且 `mini_boot_sim` 仍 10/10 PASS。

## 7. 闭环标准

- [ ] 编译：普通态 + mini 态零错误
- [ ] 回归：`tools/scl_build.py test` 全绿
- [ ] 体积：给出 `-O2`/`-Os` 前后 `.text` 与 ELF 数字
- [ ] 文档：结论落到 `doc/arc/scl-mini.md`
- [ ] 提交：`change:` 类型
- [ ] 回填：本文件补数据与提交号

## 8. 风险与回退

- 风险：`argv` 可写性若处理不当 → 写坏 Flash/常量区（必须拷贝到 RAM 缓冲）。
- 风险：丢失 `desc` 校验后，参数错误从"运行期拒绝并打印 usage"变成"静默错误"。
- 回退：不传 `--direct` 即回到现状；库侧无需改动，回退成本低。

## 9. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 量化评估 | 按名调用路径 ≈678 B 一次性；采用方案 B 替代 | `100bf99` |
