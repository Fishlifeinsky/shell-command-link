# scl.c 模块化拆分（运行器 / 编译器 / 输入解析 各自独立）

> 状态：**提案中**
> 场景分类：运行时/解释器 ＋ 构建工具
> 提出者 / 日期：用户指示 / 2026-09-10
> 关联：`scl/Src/scl.c`（现 3400+ 行单文件）、`scl/Src/scl_priv.h`

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

| 指标 | 拆分前 | 拆分后 |
|---|---|---|
| Flash（默认档 -O2） | 21354 | 待填 |
| RAM | 2014 | 待填 |
| mini Flash | 4946 | 待填 |

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
