# idea：改动提案（执行 + 闭环）

> 本目录是**改动提案的唯一入口**。任何 agent（或人）在调用/修改本库之前，若要提出更改，
> **必须先在这里落一份"执行 + 闭环"文档**，然后才能动代码。

## 1. 为什么要有这个目录

这个库是嵌入式取向的（低 ROM/RAM、无 libc 依赖、多档配置裁剪），一次"看起来很小"的改动
很容易在别处付出体积或语义代价。所以要求：**先写清楚场景与闭环标准，再改代码**，
并把实测数据回填到同一份文档里。

## 2. 强制流程

```
① 提案  → 在 doc/idea/ 新建 <主题>.md，按 _TEMPLATE.md 填写
           必须写清：场景、动机、现状、方案、影响面、闭环标准
② 执行  → 改代码；跨多个函数/多仓库的改动先向用户给出计划并确认
③ 验证  → 跑"闭环标准"里列出的命令，记录**可复现的命令 + 实测数据**
④ 闭环  → 回填实测数据与提交号；结论沉淀到 doc/arc | doc/debug | doc/spec
           （本文件保留提案与闭环记录，作为决策留痕）
```

状态标记（写在文档首行）：`提案中` → `执行中` → `已闭环`；未采纳的写 `已否决` / `搁置`。

## 3. 命名与分类

- 文件名：`小写短横线主题.md`（如 `mini-direct-call.md`）
- 一份文档 = 一个场景/一个变更点，不要把多个无关改动塞进同一份
- 场景分类（写在文档头部）：
  - `运行时/解释器`：`scl/Src/scl.c`、`SCL_Run`、`SCL_RunProg`、`SCL_Loop`
  - `变量与绑定`：`scl/Src/scl_var.c`、`SCL_VarBind`、类型与生命周期
  - `生成器`：`tools/scl_mini_c.py`、`tools/scl_emit_c.py`
  - `编译链`：`tools/scl_script2chain.py`（S2C 语法/展开/折叠）
  - `配置裁剪`：`scl/Inc/scl_cfg.h`、mini/普通两态、各 `*_EN` 开关
  - `集成/移植`：`example/`、`example/mcu_template/`、宿主适配
  - `构建工具`：`tools/scl_build.py`、验证矩阵与尺寸测量

## 4. 必填章节

`_TEMPLATE.md` 里的章节都是必填，尤其是这三项：

1. **影响面表**：静态 RAM / Flash / API / 配置开关 / 普通态与 mini 态 / 生成物 / 兼容性
2. **验证命令**：必须可直接复制执行（写死命令，不写"跑一下测试"）
3. **闭环标准**：可勾选的清单，含体积前后实测数字与文档落点

## 5. 闭环的定义

一份提案只有同时满足下面全部条件才算**已闭环**：

- [ ] 编译：普通态与 mini 态都能通过（`tools/scl_build.py check` 或等价命令）
- [ ] 回归：仓库自带测试全绿（`tools/scl_build.py test`，含 `mini_boot_sim`）
- [ ] 体积：给出改前/改后的实测数字（`arm-none-eabi-size`，注明优化级别与口径）
- [ ] 文档：结论已落到 `doc/arc`（架构）或 `doc/debug`（现象/原因/办法）或 `doc/spec`（规则）
- [ ] 提交：按仓库约定提交（`add`/`fix`/`change`/`md`），说明写清楚
- [ ] 回填：本文件补上实测数据、提交号、遗留问题

> 只改文档不改行为（如纯 `md` 提交）可以不新建 idea 文档，但仍需遵守 `doc/` 分类。

## 6. 索引

| 文档 | 场景分类 | 状态 | 一句话 |
|---|---|---|---|
| [cmd-modular-registration.md](cmd-modular-registration.md) | 运行时/解释器 + 变量与绑定 | **提案中** | 命令/静态变量用宏声明 + py 扫描生成 `scl_cmd_list.c`，放 `scl/cmd/` |
| [opcode-from-list-index.md](opcode-from-list-index.md) | 运行时/解释器 + 生成器 | **提案中** | opcode 改为"注册表内下标"，查表 O(1)、分配可静态推导 |
| [scl-module-split.md](scl-module-split.md) | 运行时/解释器 + 构建工具 | **提案中** | 把 `scl.c` 拆成 core/cmd/line/compile/exec/desc（行为零变化） |
| [env-as-var.md](env-as-var.md) | 变量与绑定 + 配置裁剪 | **提案中** | ENV 并入 var 语义：init 时创建，与 var 同一层同一批 API |
| [var-sources.md](var-sources.md) | 变量与绑定 + 编译链 | **提案中** | 变量来源 static/const/var 的三来源两态语义（mini 只支持 static 且禁初始化） |
| [mini-direct-call.md](mini-direct-call.md) | 生成器 | 搁置 | 生成代码直接调 handler 而非 `SCL_CmdInvoke` 按名查找 |
| [trim-desc-and-argbuf.md](trim-desc-and-argbuf.md) | 配置裁剪 | 提案中 | 关 `CMDDESC` / 收紧 `ARG_MAX`、`ARG_LEN_MAX` 进一步压体积 |

> **实施顺序（已定）**：先做两个"行为零变化"的（`opcode-from-list-index` → `scl-module-split`），
> 再落语义变更的 `env-as-var`；`var-sources` 的编译链部分随生成器一起改。

## 7. 相关

- 模板：[`_TEMPLATE.md`](_TEMPLATE.md)
- 架构结论：`doc/arc/`
- 问题排查：`doc/debug/`
- 编码规则：`doc/spec/`
- 仓库整体约定：`README.md`、`.github/copilot-instructions.md`
