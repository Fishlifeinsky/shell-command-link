# scl.c 模块化拆分（运行器 / 编译器 / 输入解析 各自独立）

> 状态：**执行中**（步骤 1 `0c6a49b`、2 `950d94b`、3 `5d0e03e`、4 `e8cac3f` 已闭环）
> 场景分类：运行时/解释器 ＋ 构建工具
> 提出者 / 日期：用户指示 / 2026-09-10
> 关联：`scl/Src/scl.c`（迁移前 3400+ 行单文件）、`scl/Src/scl_priv.h`

---

## 1. 场景与动机

`scl.c` 现在把**字符串/格式化工具、消息、返回值、命令注册表、invoke/异步、RunLine、
文本编译器（token 化/label/参数缓存）、字节码执行器（Step/Loop/算子 case）、
命令描述/help/usage、初始化**全塞在一个 TU 里，3400+ 行。任何一处改动都要在巨型文件里定位，
也让"按功能裁剪"只能靠 `#if` 拼。

目标：按职责拆成多个模块，**行为零变化**（纯搬运），让运行器 / 编译器 / 输入解析各自独立。

## 2. 现状（已核实）

`scl.c` 内的职责块（按现有分区注释）：

| 职责块 | 典型符号 |
|---|---|
| 移植输出/消息 | `Scl_Msg`、`Scl_VMsg`、`Scl_MsgErr`、`Scl_FmtI32`、`Scl_PutU32*` |
| 字符串工具 | `Scl_StrLen/Eq/IsSp/IsNm/IsAl/IsDigit/Up/ParseI32Len` |
| 返回值 | `SCL_Ret_Set/Get`、`Scl_RetTake` |
| 命令注册表 | `s_cmd_head`、`SCL_RegisterCmd`、`Scl_CmdFindName/Op`、`SCL_CmdHead` |
| 编程式调用/异步 | `SCL_CmdInvoke`、`SCL_AsyncBusy/Poll`、`SCL_RunLine` |
| 文本编译 | token 化、label 表、参数缓存、`SCL_Run` 的编译阶段 |
| 字节码执行 | `Scl_StepOnce`、`SCL_Loop/Idle/Abort`、算子 `Scl_DoArith/Cmp/Bool/StrCmp` |
| 描述/help | `Scl_DescCheck`、`Scl_DescPrint*`、`Scl_DoHelp*`、`Scl_HelpDocFind` |
| 初始化 | `SCL_InitEx`、`SCL_Init`、`Scl_Finish`、`Scl_DynamicRelease` |
| 变量列表打印 | `Scl_VarList`、`Scl_DoVarRaw/FreeRaw/CacheRaw` |

状态全部是 `scl.c` 内的 `static`（`s_bc/s_arg/s_pc/s_fn_back/s_steps/s_wait_cmd/s_abort/
s_busy/s_ret/s_prog/s_label_*` 等）→ **这是拆分的主要障碍**。

## 3. 方案

### 3.1 目标文件划分

```
scl/Src/
├── scl.c          # 对外 API 薄封装 + 初始化编排（保留原文件名，避免动调用方）
├── scl_core.c     # 消息/格式化/字符串工具/返回值
├── scl_cmd.c      # 命令注册表 + 按名/按 opc 查找 + CmdInvoke/异步
├── scl_line.c     # SCL_RunLine：一行文本 → argc/argv 分派（"指令输入解析"）
├── scl_compile.c  # 指令链文本 → 字节码（token 化、label、参数缓存）
├── scl_exec.c     # 字节码运行器（Step/Loop/分支/算子）
└── scl_desc.c     # 命令描述/help/usage（CMDDESC）
```

### 3.2 状态集中（关键前置）

新增 `scl/Src/scl_vm.h`：把所有跨模块共享的运行状态收进**一个结构体**（`extern s_vm;` 或
`Scl_Vm(void)` 返回指针），例如：

```c
typedef struct {
    /* 编译期 */
    uint8_t  bc[SCL_CFG_BC_MAX]; uint16_t bc_len;
    uint8_t  argbuf[SCL_CFG_ARG_CACHE_MAX]; uint16_t arg_len;
    /* 执行期 */
    uint16_t pc, fn_back; uint32_t steps;
    const scl_cmd_t *wait_cmd;
    uint8_t  busy, abort; uint8_t ret;
    scl_prog_t prog;
    /* 标签表 */ ...
} scl_vm_t;
extern scl_vm_t s_vm;
```

各模块通过 `scl_vm.h` 访问，`scl_priv.h` 继续放跨模块函数原型（按模块再分节）。

### 3.3 顺序（**先无风险、后有风险**）

1. 先做 `cmd / core / desc`（无共享执行状态）；
2. 再做 `line`（只依赖 cmd）；
3. 最后做 `compile / exec`（共享 VM 状态最多），此步引入 `scl_vm.h`；
4. `scl.c` 收敛为 API 薄壳 + init 编排。

## 4. 影响面

| 维度 | 影响 |
|---|---|
| 静态 RAM | **不变**（状态只是换了归属 TU） |
| Flash | **不变**（纯搬运；符号重排可能带来极小抖动，验收标准 ≤ ±2%） |
| 公共 API | 完全不变（`scl.h` 不动） |
| 配置开关 | 不变；但裁剪粒度变细（可按模块排除编译，如不要 help 就不编 `scl_desc.c`） |
| 普通态 / mini 态 | 两态通用；mini 只保留 `core/cmd/line` 相关，`compile/exec` 本就被 `#if` 裁掉 |
| 生成物与工具链 | `scl_build.py`、`scl.cmake`、`example/CMakeLists.txt` 源列表同步（用 glob 可自动） |
| 向后兼容性 | 源码级无破坏；若有人 `#include "scl.c"` 需改（检查后几乎没人这么干） |

## 5. 执行步骤

1. 建 `scl_vm.h`，把共享状态**先原地**改成结构体成员（不换文件，先跑绿）；
2. 按 §3.3 顺序逐块搬文件，每搬一块跑一次全量回归；
3. 每步提交一次（`change:`），并在提交信息里附"ELF 尺寸前后对比"；
4. 更新 `scl_priv.h` 分节注释 + `doc/arc/` 架构文档（新增模块图）。

## 6. 验证（可直接复制执行）

```powershell
python tools/scl_build.py test      # 每搬一块都要全绿
python tools/scl_build.py check     # 裁剪矩阵零告警
python tools/scl_build.py sizes     # 与拆分前对比（≤ ±2%）
arm-none-eabi-size <各 .o>          # 模块级归属确认
```

期望数据：

| 指标 | 拆分前 | 拆分后（步骤 2） |
|---|---|---|
| Flash（默认档 -O2） | 21362 | **19968** |
| RAM | 2014 | 2013 |
| mini Flash | 4954 | 4760 |

## 7. 闭环标准

- [ ] 编译：普通态 + mini 态零错误零告警
- [ ] 回归：`tools/scl_build.py test` 全绿（每步）
- [ ] 体积：前后实测，Flash 变化 ≤ ±2%（超出需解释）
- [ ] 文档：`doc/arc/` 增模块划分图与职责表
- [ ] 提交：`change:` 分批提交
- [ ] 回填：本文件补数据与提交号

## 8. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| 大搬迁引入回归 | 搬运中漏了 `static`/顺序 | 按 §3.3 分步 + 每步回归；单步回滚成本低 |
| 状态结构体化引入间接寻址开销 | 结构体成员可能比裸 static 略慢 | 实测尺寸/回归；必要时 `#define` 映射回裸变量 |
| mini 裁剪边界 | 拆分后 `#if` 位置变化导致 mini 变大 | 用 `sizes` 的 mini 档卡住（±100B） |
| 构建脚本漏加文件 | glob 遗漏 | 源列表改 glob + `scl_build.py` 里断言文件数 |

## 9. 决策（实现者定）

1. 保留 `scl.c` 文件名（对外无感）还是改名 `scl_api.c`？（建议**保留**）
2. 共享状态用 `extern s_vm` 还是访问器 `Scl_Vm()`？（建议 `extern`，嵌入式少一层调用）
3. 是否同时把 `scl/cmd/` 的手写命令并入同一构建单元？（建议**保持独立**）

## 10. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 建档（仅规划） | — | — |
| 2026-09-10 | 步骤 1：抽出 `scl_mem.c`（分配器 + 用量统计），新增 `Scl_MemSetAllocator()` 替掉 InitEx 里的 6 行直写；构建/测试源列表改 glob | 全绿；RAM 不变，Flash **+16~22 B**（跨 TU 不再内联） | `0c6a49b` |
| 2026-09-10 | 步骤 2：抽出 `scl_core.c`（文本/数值小工具 + 消息输出） | 全绿；裁剪矩阵回到全零警告；体积见 §10.2 | `950d94b` |
| 2026-09-10 | 步骤 3：抽出 `scl_cmd.c`（命令注册/查找/编程式调用/一行解析） | 全绿；一轮体积回归后定下外置口径（见 §10.3） | `5d0e03e` |
| 2026-09-10 | 步骤 4：抽出 `scl_desc.c`（类型名/usage/模板校验/描述注册） | 全绿；help 文本按 §10.3 口径留在 `scl.c` | `e8cac3f` |

### 10.4 步骤 4 的体积账单

| 档位 | 步骤 3 | 步骤 4 | 变化 |
|---|---|---|---|
| 默认全功能 O2 | 19922 | 19919 | −3 |
| 档A 最小RAM | 16784 | 16761 | −23 |
| 档A+关消息 | 9362 | **9511** | **+149（+1.6%）** |
| 档B 平衡 | 19954 | 19947 | −7 |
| mini O2 | 4762 | 4774 | +12 |
| mini -Os | 3712 | 3684 | −28 |

`档A+关消息` 的 +149 B 是跨 TU 的固定代价：`Scl_TypeName` 与 `Scl_DescPrintUsage`
在关消息档虽几乎无输出，但已经不能像同 TU 时那样被内联消掉。
仍在提案 §4 的 ≤ ±2% 门槛内。

> help 文本（约 1 KB 的字符串表 + 输出逻辑）**没有**外置——它只被解释器调用，
> 一旦挪到新编译单元就会变成"永远不会被消除的全局符号"，mini 会直接胖一圈。

---

## 11. 步骤 5/6 的收敛方案（与 §3.1 的差异，已定）

§3.1 原计划把 `scl_compile.c` 与 `scl_exec.c` **分开**。按 §10.3 的实测口径重新评估后，
**两者合并为一个编译单元 `scl_exec.c`**：

1. 编译链与执行器共享参数缓存（`s_argc`/`s_arg_len`）与读回原语
   （`Scl_ArgLoad`/`Scl_BlkText`/`Scl_ArgRestore`/`Scl_DoCallName`/`Scl_CmdFindOp`）；
2. 这些原语目前都是 `static`，**在 mini 态仍会被编译**（引用点位于无条件的公共段）；
   一旦为了跨文件而外置，就再也不能被消除 —— 这正是 §10.3 那次 **+383 B / +101 B** 的根因；
3. 所以：

| 文件 | 内容 |
|---|---|
| `scl/Src/scl_exec.c`（待建） | 编译（字面量归类/label/参数缓存）、执行（Step/算子/`SCL_Loop`）、内置命令（`var`/`free`/`cache`/`help`）、`SCL_Run`/`SCL_RunProg`、执行态与 `Scl_Finish` |
| `scl/Src/scl.c`（收敛后） | opcode 常量、注册表弱符号接入、`SCL_InitEx`/`SCL_Init`、脚本命令（`SCL_Scmd_*`）、`SCL_Ret_*`，以及执行核的初始化/释放入口 |

4. **执行态跟着使用它的模块走**：`s_bc`/`s_bc_len`/`s_pc`/`s_fn_back`/`s_steps`/
   `s_argc`/`s_arg_len`/`s_labels`/`s_label_cnt`/`s_raw` 整体搬入 `scl_exec.c`，
   并整段包在 `#if ((SCL_CFG_RUN_TEXT_EN) != 0u) || ((SCL_CFG_RUN_PROG_EN) != 0u)` 内——
   mini 态整段不编译，天然不付体积；`scl.c` 通过 `Scl_ExecInit()` / `Scl_ExecRelease()`
   驱动分配与释放（替代现在 `SCL_InitEx` 直接摆弄这些变量）。

> 验收口径不变：每步 `python tools/scl_build.py all` 全绿、裁剪矩阵零告警、
> 体积变化落在 §4 的 ≤ ±2%（mini ±100 B）内；超出必须在 §10 记录并给出原因。

### 10.3 关键取舍：外置的代价是"再也不会被消除"

同一 TU 内的 `static` 函数/变量，**未被引用时编译器会整段消除**；
一旦变成跳模块的全局符号，就再也不会被消除。步骤 3 实测：

1. 先试着把 `s_raw`/`s_cmdname` 也外置并按档位开关裁剪 → **mini 直接编译不过**：
   它们的引用点（`Scl_BlkText`/`Scl_DoCallName` 等公共段函数）在 mini 态**仍然会被编译**；
2. 换成"外置 + 裁掉引用不到的档位" → mini **+383 B Flash / +101 B RAM**：
   `Scl_ExpandCopy` 284 B、`Scl_CmdFindOp` 28 B、`s_raw` 64 B、`s_cmdname` 32 B；
3. 最终做法：**只外置真正跳模块使用的符号**
   （`s_argb/s_argv/s_argt`、`s_cmd_head/s_next_opc/s_wait_cmd`、`Scl_CmdFindName`、`Scl_DescCheck`），
   其余（`Scl_CmdFindOp`、`Scl_ExpandCopy`、`s_raw`、`s_cmdname`）**留在 `scl.c` 保持 static**，
   继续享受"未引用即消除"。mini 回落（`4760 → 4762`，+2 B）。

> 结论：判断一个符号"该不该外置"的第一依据不是"逻辑上属于哪个模块"，
> 而是**"谁会跳文件引用它"**；否则会白付体积。
>
> 附带教训：这类问题在默认档看不出来（它本来就用到所有函数），
> **只有 mini 档 / 关消息档的 `sizes` 数字能暴露**。

### 10.2 逐步体积（ARM Cortex-M4，Flash=`.text+.rodata+.data`，RAM=`.data+.bss`）

基准取步骤 1 之前（即 opcode 下标化后的 `dd3721c`）：

| 档位 | 拆分前 | 步骤 1（scl_mem） | 步骤 2（scl_core） | 步骤 2 vs 拆分前 |
|---|---|---|---|---|
| 默认全功能 O2 | 21362 | 21378 | **19968** | **−1394（−6.5%）** |
| 默认-关绑定表 O2 | 21146 | 21162 | **19752** | −1394 |
| 默认 -Os | 16868 | 16890 | **17080** | +212（+1.3%） |
| 档A 最小RAM O2 | 17820 | 17840 | **16802** | −1018 |
| 档A+关消息 O2 | 10199 | 10195 | **9392** | −807 |
| 档B 平衡 O2 | 21374 | 21390 | **20004** | −1370 |
| mini O2 | 4954 | 4958 | **4760** | −194（−3.9%） |
| mini -Os | 3644 | 3646 | **3660** | +16（+0.4%） |
| RAM（默认档） | 2014 | 2014 | **2013** | −1 |

解释：默认档 −6.5% 是**单 TU 内联膨胀消失**的结果——
原先 `scl.c` 一个编译单元里，变参函数 `Scl_Msg` 被内联到数十个调用点，
每个调用点都带一份参数装配与格式化代码；拆成独立编译单元后只保留一份。
−Os 档 +1.3%、mini −Os +0.4% 则是短小工具函数不再自动内联的代价。
两者都属提案 §4 所说的"符号重排带来的抖动"，且整体是变小方向。

### 10.1 拆分中发现的体积规律（重要）

跨 TU 拆分会让**原本被内联消除的空函数**变成真实调用：

- `SCL_CFG_MSG_EN=0` 时，`Scl_Msg/Scl_MsgErr` 原本是同一 TU 内的 static 空函数，
  调用点被完全消除；拆出后每个调用点都变成"压参 + call"，
  实测 **档A+关消息 Flash `10195 → 12938`（+2743 B，+27%）**；
- 修法：`scl_priv.h` 在 `MSG_EN=0` 时把两者**宏化为 `((void)0)`**
  （实现文件定义前 `#undef`），恢复"调用点零开销"。

同类的还有 `SCL_CFG_DYNAMIC_MEM_EN=0` 下的 `Scl_MemReady`（静态模式无此概念）——
直接将该函数整体包进 `#if DYNAMIC_MEM_EN`，消除 `-Wunused-function`。

> 结论：**凡"关掉某开关后变成空实现"的内部函数，跨模块后都要用宏（而非空函数体）来表达**，
> 否则体积会反向增长。后续步骤（`desc` / `exec`）同样要检查。
