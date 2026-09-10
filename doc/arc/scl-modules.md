# SCL 源码模块划分（职责 / 依赖 / 裁剪边界）

> 类别：架构 · 状态：已落地（2026-09-10） · 相关提案：`doc/idea/scl-module-split.md`

---

## 1. 模块清单与职责

| 文件 | 行数 | 职责 | 关键对外符号 |
|---|---|---|---|
| `scl/Src/scl.c` | ~330 | 编译期校验、命令/静态变量注册表**状态**、初始化编排、mini 态主循环 | `SCL_InitEx` `SCL_Init` `SCL_RegisterCmd` `SCL_CmdHead` |
| `scl/Src/scl_exec.c` | ~2190 | **编译链 + 字节码执行核**：字面量归类、label、参数字节缓存、两遍编译、解释执行、算子、收尾、内置命令（var/free/cache/help）、`SCL_Run`/`SCL_RunProg`、普通态 `SCL_Loop/Idle/Abort`、脚本命令 `SCL_Scmd_*` | `SCL_Run` `SCL_RunProg` `SCL_Loop` `SCL_Scmd_*` `Scl_StepOnce` `Scl_ExecInit/Release/Reset` |
| `scl/Src/scl_cmd.c` | ~330 | 命令注册表链表维护、按名查找、编程式调用（`SCL_CmdInvoke`）与异步等待、一行文本解析 `SCL_RunLine` | `SCL_RegisterCmd` `SCL_CmdInvoke` `SCL_AsyncPoll` `SCL_RunLine` |
| `scl/Src/scl_desc.c` | ~140 | 命令描述层：类型名、usage 行、参数模板校验、描述注册 | `SCL_CmdRegisterDesc` `Scl_DescCheck` `Scl_TypeName` |
| `scl/Src/scl_core.c` | ~480 | 无 libc 基础件：文本/数值小工具、消息输出与级别门控 | `Scl_StrLen` `Scl_ParseI32Len` `Scl_FmtI32` `Scl_Msg` `Scl_MsgErr` |
| `scl/Src/scl_mem.c` | ~190 | 动态内存分配器与用量统计（`SCL_CacheInfo` 等） | `Scl_MemAlloc` `Scl_MemSetAllocator` |
| `scl/Src/scl_var.c` | ~640 | 会话变量表 + 静态变量绑定路由表（两态各一臂） | `SCL_VarGet/Set/T/Type/Free/Count` `SCL_VarBind*` |
| `scl/Src/scl_env.c` | — | 环境变量缓冲（持久配置的装载/序列化） | `Scl_Env_*` `Scl_Env_Load/Save` |
| `scl/cmd/*.c` | — | 用户命令（一命令一文件）+ 生成的注册表 `scl_cmd_list.c` | `SCL_RegList_Init`（弱符号） |

公共头：`scl/Inc/scl.h`（对外 API）、`scl/Inc/scl_cfg.h`（配置中心）、`scl/Inc/scl_reg.h`（声明宏）。
内部头：`scl/Src/scl_priv.h`（跨模块共享状态与函数原型，库用户勿依赖）。

## 2. 依赖方向

```
            scl.h / scl_cfg.h / scl_reg.h        （对外）
                     ↑
   scl.c ────────────┼────────────→ scl_exec.c ──→ scl_cmd.c
     │               │                   │              │
     │               │                   ↓              ↓
     └───────────────┴──────────→ scl_desc.c ──→ scl_core.c ──→ scl_mem.c
                                     │
                          scl_var.c / scl_env.c
```

- **单向、无环**：`core/mem` 最底层 → `desc/cmd` → `exec` → `scl.c`（编排）；
- `scl_priv.h` 是唯一的跨模块接口面，公共 API 一律经 `scl.h`。

## 3. 两态与开关的裁剪边界

| 模块/内容 | 条件 |
|---|---|
| 编译链、解释器、内置命令、help、`SCL_Run` | `SCL_CFG_RUN_TEXT_EN` |
| `SCL_RunProg`、`CALLN` 按名调用 | `SCL_CFG_RUN_PROG_EN` |
| 执行核整体（含执行态缓冲、`SCL_Loop` 普通态分支） | `RUN_TEXT_EN \|\| RUN_PROG_EN`（mini 态整段不参与） |
| 脚本命令 `SCL_Scmd_*` | `SCL_CFG_SCMD_EN && SCL_CFG_RUN_PROG_EN` |
| 绑定路由表（`s_binds`） | `SCL_CFG_VAR_BIND_EN`（关闭时容量为 0 = 零长数组） |
| 消息输出与执行核的耦合点 | `SCL_CFG_MSG_EN`（关闭时 `Scl_Msg/MsgErr` 为头内联空实现） |
| 描述层 | `SCL_CFG_CMDDESC_EN` |

## 4. 共享状态一览（定义处 → 声明处）

| 状态 | 定义 | 使用者 |
|---|---|---|
| `s_cmd_head` / `s_next_opc` / `s_wait_cmd` | `scl.c` | `scl_cmd.c`、`scl_exec.c` |
| `s_cmd_head` 相关注册表接口 | — | 生成物 `scl_cmd_list.c` |
| `s_argb` / `s_argv` / `s_argt` | `scl.c` | `scl_cmd.c`（直调）、`scl_exec.c`（编译/执行） |
| `s_inited` / `s_busy` / `s_abort` / `s_ret` | `scl.c` | `scl_exec.c` |
| `s_keep_vars` | `scl.c` | `scl_var.c` |
| `s_scmd_head` | `scl_exec.c` | `scl.c`（InitEx 复位） |
| 执行态（`s_prog`/`s_bc`/`s_argc`/`s_labels`/`s_pc`/…） | `scl_exec.c`（static） | 仅执行核内部；由 `Scl_ExecInit/Release/Reset` 管理 |
| `s_binds` / `s_bind_cnt` | `scl_var.c`（static） | 仅变量模块内部 |
| `s_vars` / `s_env` | `scl_var.c` / `scl_env.c` | 各模块经 `scl_priv.h` |

## 5. 拆分判据（实测总结）

1. **判断"该不该外置"看引用面**，不是"逻辑上属于谁"：同一 TU 内 `static` 且未被引用的符号
   会被编译器整段消除；一旦外置就永久占体积（实测 mini **+383 B / +101 B**，§10.3）。
2. **共享内部原语的模块必须同 TU**：编译链与执行器共用参数字节缓存与读回原语，
   拆成两个文件会迫使这些原语外置 → 故合并为 `scl_exec.c`（§11）。
3. **"关开关即空实现"的函数用头内联空实现**（`static inline`），不要用宏：
   宏会让实参消失，触发 `-Wunused-but-set-variable` 且可能造成体积回归（§10.1）。
4. **状态跟着使用它的模块走**，并由该模块提供 init/release/reset 三个接口给编排层；
   这样 mini 态可以整段不编译，天然不付体积。

## 6. 构建集成

- `scl/scl.cmake`：`scl_collect()` 用 `file(GLOB)` 收集 `Src/*.c` + `cmd/*.c`，
  新增模块**无需改 CMake**；`scl_apply_defs(target)` 补 `SCL_CFG_REG_LIST_EN=1`；
  `scl_regen()` 重新生成注册表。
- `tools/scl_build.py` / `tools/s2c_test.py` / `example/big_demo/build_big.py`：
  库源列表统一改为 `sorted(Src/*.c)` 的 glob 形式。
