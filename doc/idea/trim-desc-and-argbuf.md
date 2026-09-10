# 通过裁剪 desc 与参数缓冲进一步压缩体积

> 状态：**提案中**（未执行）
> 场景分类：配置裁剪
> 提出者 / 日期：agent 提案 / 2026-09-10
> 关联：`scl/Inc/scl_cfg.h`、`doc/arc/scl-mini.md` §6

---

## 1. 场景与动机

场景：mini 档（`SCL_CFG_MINI_EN=1`）已压到 ARM `-O2` 约 4.9 KB / `-Os` 约 3.6 KB
（仅核心三文件）。若目标 MCU 的 Flash 更紧，需要继续往下压。

动机：剩余体积的大头是**中文字符串帮助表（`SCL_CFG_CMDDESC_EN`）**与
**静态参数缓冲**（`SCL_CFG_ARG_MAX × SCL_CFG_ARG_LEN_MAX`）。

## 2. 现状

- 帮助/描述：`SCL_CFG_CMDDESC_EN=1` 时保留 `s_help_meta` / `s_help_ops` 与
  `Scl_DescPrintUsage` / `Scl_DescPrintDetail` / `Scl_DescCheck` 等（含中文字符串常量）。
- 参数缓冲：`s_argb[SCL_CFG_ARG_MAX][SCL_CFG_ARG_LEN_MAX]` + `s_argv` + `s_argt`
  （默认 `8 × 32 = 256 B` 静态 RAM，不含 `s_argv`/`s_argt`）。

## 3. 方案

### 3.1 关闭 desc（`-DSCL_CFG_CMDDESC_EN=0`）

- 收益：砍掉帮助文本与模板校验代码；
- 代价：`help` 命令与参数模板校验失效（`SCL_CmdInvoke` 不再拒绝非法参数）。

### 3.2 收紧参数容量

- `SCL_CFG_ARG_MAX`：按实际命令最大参数个数设置（如 4）；
- `SCL_CFG_ARG_LEN_MAX`：按实际参数最大文本长度设置（如 16）；
- 收益：RAM 线性下降；Flash 侧对循环展开略有影响。

### 3.3 组合建议

| 档位 | CMDDESC | ARG_MAX | ARG_LEN_MAX | 适用 |
|---|---|---|---|---|
| 标准 mini | 1 | 8 | 32 | 需要 help/校验 |
| 精简 mini | 0 | 4 | 16 | 无需 help，参数短 |
| 极限 | 0 | 2 | 12 | 单点脚本、无交互 |

## 4. 影响面

| 维度 | 影响 |
|---|---|
| 静态 RAM | 3.2 可省 ≈ 100~200 B（按上表） |
| Flash | 3.1 预计省数百字节~1 KB（取决于帮助文本量） |
| 公共 API / ABI | 无变化，但 `SCL_CmdInvoke` 行为变化（不再拒绝非法参数） |
| 配置开关 | 复用现有开关，无需新增 |
| 普通态 / mini 态 | 主要影响 mini；普通态同样受益于 3.2 |
| 生成物与工具链 | 无 |
| 向后兼容性 | 配置级不兼容（帮助能力消失），需在项目中显式声明 |

## 5. 执行步骤

1. 在具体工程中加 `-D` 覆盖（无需改库）；
2. 若作为“官方档位”，在 `doc/other/scl-config-profiles.md` 增补档位并在
   `tools/scl_build.py` 的 `PROFILES` 中登记以便持续测量；
3. 记录是否有命令依赖 `desc`（`SCL_CmdRegisterDesc`）导致行为变化。

## 6. 验证（可直接复制执行）

```powershell
python tools/scl_build.py test                     # 回归
python tools/scl_build.py check                    # 裁剪矩阵零告警
python tools/scl_build.py sizes                    # 各档 Flash/RAM
# 单独档位：
# gcc -O2 -DSCL_CFG_MINI_EN=1 -DSCL_CFG_CMDDESC_EN=0 -DSCL_CFG_ARG_MAX=4 -DSCL_CFG_ARG_LEN_MAX=16 -fsyntax-only ...
```

期望数据（待填）：

| 指标 | 标准 mini | 精简 mini | 极限 |
|---|---|---|---|
| Flash (`-O2`) | | | |
| RAM | | | |

## 7. 闭环标准

- [ ] 编译：普通态 + mini 态零错误（含新档位零告警）
- [ ] 回归：`tools/scl_build.py test` 全绿
- [ ] 体积：三档 Flash/RAM 实测数字
- [ ] 文档：档位写入 `doc/other/scl-config-profiles.md`，结论摘要入 `doc/arc/scl-mini.md`
- [ ] 提交：`change:` + `md:`
- [ ] 回填：本文件补数据与提交号

## 8. 风险与回退

- 风险：关 desc 后非法参数不再被拦截，可能把错误推迟到运行期（表现为命令自己报错或静默）。
- 风险：`ARG_LEN_MAX` 过小会**截断**长参数（如路径），需确认实际参数上限。
- 回退：改配置开关即可，无需回滚代码。

## 9. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 建档 | 待评估 | — |
