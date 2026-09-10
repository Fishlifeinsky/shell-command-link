# mini-scl：两态精简内核（架构）

> 更新：本次"激进精简"后，mini 态已真正移除**会话变量表**与**字节码/文本解释器**两条大链。
> ARM(Cortex-M4, -O2) 实测 Flash≈4.9KB / RAM≈0.45KB（-Os 下 Flash≈3.6KB），达成 <5KB 目标。

## 1. 两态总开关

整个库只有 **一个主开关** `SCL_CFG_MINI_EN`（`scl/Inc/scl_cfg.h`）：

| 开关值 | 语义 | 派生默认值 |
| --- | --- | --- |
| `0`（默认） | 普通态：完整解释器 + env + desc + msg | `RUN_TEXT_EN=1`、`RUN_PROG_EN=1`、`ENV_EN=1`、`SCMD_EN=1`、`MSG_LVL=0xFF` |
| `1` | mini 态：最简 argc/argv 分派内核 | `RUN_TEXT_EN=0`、`RUN_PROG_EN=0`、`ENV_EN=0`、`SCMD_EN=0`、`MSG_LVL=ERR` |

- 派生默认写在 `scl_cfg.h`，用户仍可用 `-D` 覆盖（如 mini 下强行开 `ENV_EN`）。
- 普通态若两个解释器都被关成 0 会 `#error`（要求必须至少留一个）；mini 态不走该断言。

## 2. mini 态保留 / 裁掉的内容

| 类别 | 普通态 | mini 态 | 说明 |
| --- | --- | --- | --- |
| 命令表注册 `SCL_RegisterCmd` | ✅ | ✅ | s2c/生成代码把命令注册进表 |
| 命令触发 `SCL_CmdInvoke/AsyncBusy/AsyncPoll` | ✅ | ✅ | 同步/异步两种触发 |
| 行文本分派 `SCL_RunLine` | ✅ | ✅ | 最简 argc/argv 拆分后查表调用，1=完成 2=异步等待 0=拒绝 |
| 返回值 `SCL_Ret_Set/Get` | ✅ | ✅ | 前一条命令设置的结果码/文本 |
| 变量 API `SCL_VarGet/Set/SetT/Type/Enum...` | ✅ | ✅ | **mini 全部走静态变量绑定路由** |
| 会话变量表 `s_vars` | ✅ | ❌ | mini 不在库内开表；不定义、无生命周期管理 |
| `SCL_VarBind/SCL_VarBindClear` | ✅ | ✅ | 生成代码把"类型化 C 静态变量"注册进 `s_binds[]` |
| 文本解释器 `SCL_Run(text)` | ✅ | ❌ | `RUN_TEXT_EN=0` 整链裁剪 |
| 预编译程序解释器 `SCL_RunProg` | ✅ | ❌ | `RUN_PROG_EN=0` 整链裁剪 |
| `SCL_Loop/Idle/Abort` | 走解释器 | 仅轮询 | mini 下 `SCL_Loop` 只调 `SCL_AsyncPoll()`，`Idle`=异步不忙，`Abort` 无解释器可中断 |
| 环境变量 `SCL_Env*` | ✅ | ❌ | `ENV_EN=0`；`scl_env.c` mini 下基本为空 |
| 描述 `SCL_CFG_CMDDESC_EN` | 默认 1 | 默认 1 | 帮助文本保留（可 `-D` 关） |
| 消息 | 运行时全局 `SCL_MsgLvl` | 同左 | `SCL_CFG_MSG_LVL` 决定默认级；`Scl_MsgErr` 需 ≥ERR、`Scl_Msg` 需 ≥INFO 才真正输出 |

## 3. mini 变量的"绑定路由"

mini 不使用库内会话槽，而是把**生成代码里的类型化 C 静态变量**注册进绑定表：

```
scl_bind_t { name, type(SCL_T_*), get(), set() }  ->  s_binds[SCL_CFG_VAR_BIND_MAX]
```

- `SCL_VarBind(tab,n)` 由生成代码（`*_mini_register()`）在 `SCL_Init` 后调用；
- `SCL_VarGet/SCL_VarSet(T)` 先查 `s_binds[]`，命中即路由到对应 `get/set`，落到**静态存储**；
- 未绑定名字返回 `-1/NULL/0`（不报错）；
- `SCL_VarEnum/Count/Keep/Free(All)` 提供一致的查询视图；`Free` 系对绑定变量为 no-op；
- `Scl_VarGc/Zombie` mini 为空实现（没有表可回收）。

好处：外部/宿主代码仍可用与普通态一致的旧 API 读写"看似会话变量"的变量，但存储、类型都由生成代码拥有。

## 4. 执行模型（mini）

1. 上电：`SCL_Init()` → 清零绑定表 → 各命令注册（生成代码或用户表）。
2. 触发：
   - 同步：`SCL_CmdInvoke("name", argc, argv)` 返回 `0=拒绝 / 1=完成 / 2=异步`；
   - 异步：返回 2 后由 `SCL_AsyncBusy()` 判忙、`SCL_AsyncPoll()` 推进（或直接调 `SCL_Loop()`，mini 下等价于 poll）；
   - 或文本行：`SCL_RunLine("name a b")` 内部局部栈缓冲拆 argc/argv 后走同一条分派。
3. 返回值经 `SCL_Ret_Set/Get` 传递。

生成器 `tools/scl_mini_c.py`（`scl_emit_c.py --mini` 惰性导入）产出 `<name>_mini_register/start/step`：
类型化静态变量 `m_<name>`、原生 C 运算符、绑定 `s_bind[]`，与 `SCL_CmdInvoke`/`SCL_AsyncPoll` 无缝对接。

## 5. 裁剪如何实现（本次关键）

- `scl_var.c` 按 `SCL_CFG_MINI_EN` 上下分两态：
  - 顶部 mini 模块：绑定路由 + `SCL_VarBind/...` 全部无会话表实现；
  - `#else` 收住原有会话变量表代码（普通态专用）；
  - 删除夹在普通态里的旧 mini 绑定块（两态都不会编译的死代码）。
- `scl.c`：`SCL_Loop/SCL_Idle/SCL_Abort` 在 `RUN_TEXT_EN=0 && RUN_PROG_EN=0`（即 mini）时降级为只轮询 `SCL_AsyncPoll`，
  使解释器静态函数链**整体无引用**，`-O2` 死代码剔除自动去掉，Flash 大幅下降。
- 尺寸矩阵 `tools/scl_build.py sizes` 新增 `mini-scl(O2)/mini-scl(-Os)` 两档。

## 6. 实测尺寸（ARM Cortex-M4，仅 scl.c+var.c+env.c）

| 档 | Flash | RAM |
| --- | --- | --- |
| 默认全功能 (O2) | 20782 B | 1882 B |
| mini-scl (O2) | **4882 B** | **447 B** |
| mini-scl (-Os) | **3600 B** | **447 B** |

> 说明：Flash 含中文字符串帮助表（`CMDDESC_EN=1`）与运行时消息框架。若需再压，可关
> `SCL_CFG_CMDDESC_EN`（帮助）或调 `SCL_CFG_MSG_LVL`（消息），两者是剩余体积的主要贡献者。

## 7. 相关文件

- `scl/Inc/scl_cfg.h`：主开关与派生默认值
- `scl/Inc/scl.h`：绑定 API、消息级别、invoke/异步 API、`SCL_RunLine`（均在 MINI guard 下/通用）
- `scl/Src/scl.c`：命令注册、invoke/async、`SCL_RunLine`、`SCL_Loop` mini 降级
- `scl/Src/scl_var.c`：mini 绑定路由模块（顶部）+ 普通态会话表（`#else`）
- `tools/scl_mini_c.py` / `tools/scl_emit_c.py --mini`：mini 状态机生成器
- `example/mini/boot_mini.c` / `boot_mini_sim.c`：生成样例与自检 driver（10/10 PASS）

---

## 8. 生成器体积优化（v2.1）

生成物大小不只取决于库，**生成器的发射策略**同样是关键项。已落地的两项：

### 8.1 单引用参数直通（主要收益）

旧行为：任何含 `${}` 的参数一律走"局部缓冲 + 逐段拼接"，即使整串就是单个 `${name}`：

```c
char bx4[48]; unsigned zx4 = 0u;                            /* 48B 栈 */
Mini_AppendS(bx4, &zx4, sizeof(bx4), Mini_ExtS("dist"));    /* 纯搬运 */
bx4[zx4 < sizeof(bx4) ? zx4 : sizeof(bx4) - 1u] = '\0';
av[1] = bx4;
```

新行为：整串恰好是单个 `${name}`（无字面量）时直接给表达式，免缓冲、免拼接：

```c
ia[1].text = Mini_ExtS("dist");
```

### 8.2 去掉 `const char *av[]` 中转

`ia[i].text` 直接赋值，不再经一层指针数组（少 N 个指针的栈与赋值）。

### 8.3 实测（`measure_flow.s2c`：22 状态、8 外部注入变量）

同参数对照编译（`arm-none-eabi-size`）：

| 优化级别 | `.text`（生成物） | `step()` | 生成 C 源码 |
|---|---|---|---|
| `-O2` 优化前 | 4147 | 3588 | 17338 B |
| `-O2` 优化后 | **2755** | **2196（−39%）** | **12294 B** |
| `-Os` 优化前 | 2890 | 2412 | 17338 B |
| `-Os` 优化后 | **2186** | **1708（−29%）** | 12294 B |

### 8.4 约束（重要）

直通后，同一调用点可能**同时持有多个 getter 返回的指针**（旧实现是拿到值立刻快照到局部缓冲）。
因此绑定 getter **必须每个变量独立存储**，不得多变量共用同一个 `static char` 缓冲，
否则形如 `drv("${a}", "${b}")` 的调用会取到同一个（最后求值的）值。

- 生成器为脚本内变量生成的 getter 天然每变量独立；
- 宿主注入变量（`SCL_VarBind`）需遵守同一条约定（见 `example/` 与工程侧 `scl-mini-mode-migration` 文档）。

### 8.5 仍然"按名调用"的理由

生成代码目前仍走 `SCL_CmdInvoke("name", argc, ia)` 而非直接调 `handler`：

| 方案 | 省 | 付 |
|---|---|---|
| 按名调用（现状） | — | `SCL_CmdInvoke` 564B + `Scl_CmdFindName` 72B + `Scl_StrEq` 42B（一次性） |
| 直接调回调 | 上述 ≈ 0.7 KB 一次性 | 生成器需拿到命令 C 符号（名字→符号耦合）；argv 仍需**可写**缓冲（handler 会原地改）；失去 `nd->sync != NULL` 的异步判定与 desc 模板校验 |

即：直接调用是**一次性 ~0.5KB 级**的收益，而 8.1 这类发射策略优化是**随脚本规模线性**的收益，
后者才是主要矛盾。
