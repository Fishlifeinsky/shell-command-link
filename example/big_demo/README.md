# big_demo：SCL 复杂项目（C 语言混合控制）

一个"足够复杂、可直接编译运行、自测全绿"的示例工程：**老化炉产线测试台**。
演示 SCL v0.2/v0.3/v0.3b/v0.3c 在真实规模下的用法，以及 **C 宿主层 ↔ 命令库 ↔
脚本命令**三层混合控制的分工与协作。

## 规模一览

| 项 | 数量/内容 |
|---|---|
| 指令（业务命令） | **32 个**（老化炉 11 / 电机 8 / IO 5 / 批次·统计 4 / 温控策略 1 / 诊断工具 3），均带 `scl_cmd_desc` 参数模板 |
| 脚本 | **4 个 .s2c**（`selftest` / `profile` / `run_lot` / `diagnose`），合计约 300 行，各自编译成 **const 脚本命令**（Flash） |
| 常量 | 曲线档位/阈值等 `const`（编译期折叠，不占变量） |
| 变量 | 脚本 `var` + 命令参数 `arg0..` + `alias` 编译期别名 + C 注入/`env` 运行参数 |
| 存储 | `b_sim` 设备模拟器（箱体/电机/IO/批次）+ `env` 默认运行参数表 |
| C 混合 | `main.c` 状态机：直接 `SCL_Run(链)`、`SCL_VarSet/Get`、`env` 装载、触发脚本命令、读 `b_sim` 决策、自测断言 |

## 文件结构

```
example/big_demo/
├── b_sim.h / b_sim.c        设备模拟器（温度收敛/电机走步/IO/批次）——"事实源"
├── b_cmds.h                 命令库头（B_Talk / B_Eq / 注册入口）
├── b_cmds1.c                老化炉 + 电机 + IO 命令（22）
├── b_cmds2.c                批次/统计/校准/诊断/工具（含 setret，10）
├── b_env.c                  运行参数默认表（env，C 注册、脚本消费）
├── b_scripts.c              聚合注册 4 个脚本命令
├── main.c                   C 混合控制 + 自测断言（PASS/FAIL）
├── scripts/*.s2c           脚本源码（现代语法：const/alias/var/fn/when/while/…）
├── gen/sc_*.c               生成物：scl_emit_c --cmd 输出（const 程序，勿手改）
└── build_big.py             一键：生成脚本命令 C + 编译 + 运行
```

## 构建 / 运行 / 自测

```bash
python example/big_demo/build_big.py            # 生成 + 编译 + 运行（exit 0 = 全绿）
python example/big_demo/build_big.py --no-run   # 只生成 + 编译
# 或用 README 等价 gcc 命令行（见 main.c 顶部注释）
```

预期自测：**PASS=19 FAIL=0**（覆盖：C 变量 API、链文本、自检 6/6、env 读回、
温循升温到位、批次 4 件=3良1不良、脚本命令 busy 拒、诊断读 env 等）。

## 三层混合控制（本项目想示范的）

```
C 宿主层     main.c：设备 tick 驱动、注册、决策、断言
    │  直接 SCL_Run("链文本") / SCL_VarSet·Get / 触发 SCL_Scmd_RunText
命令库       b_cmds*.c：32 个带 desc 命令（脚本与 C 都调用它们操作 b_sim）
脚本命令     4 个 .s2c（const 程序 + 尾 free）：selftest / profile 0..2 / run_lot n / diagnose
```

要点：**脚本的"结果"落在命令副作用（`b_sim.lot` 等）与 env/输出上**，
C 在脚本之间直接读回做下一步决策——这正是"C 与脚本分工"的推荐姿势：
- 脚本管**节拍/流程/判定的语义**（易改、放 Flash）；
- C 管**设备驱动、跨流程决策、配置**；
- 数据/状态在 `b_sim`/`env` 上"一处更新、双方可读"，避免两层各自状态不一致。

## 脚本命令用法（命令行 / C）

```c
SCL_Init();
b_sim_init();
Big_CmdDev_Register();  Big_CmdFlow_Register();
Big_Env_Register();     Big_Scmd_RegisterAll();

SCL_Scmd_RunText("selftest");      // 开机自检
SCL_Scmd_RunText("profile 0");     // 温循档位（恒温 60℃）
SCL_Scmd_RunText("run_lot 4");     // 批量 4 件（第 3 件注入 NG → 3 良 1 不良）
SCL_Scmd_RunText("diagnose");      // 诊断 + 读 env 运行参数
while (!SCL_Idle()) { b_sim_step(); SCL_Loop(); }   // 主循环：模拟器 tick + 解释器步进
```

脚本内可用：顶层 `const`（折叠进 Flash）、`alias <名> argN`（命令参数起名）、
`var`、`fn`（内联）、`if/else`、`while/do`、`when`、`${}`、算术/比较/逻辑、取模等；
命令参数一律 `${变量/常量}`，表达式/比较用裸名。

## 真机移植提示

- 把 `b_sim.c` 换成真实 HAL/寄存器（命令与脚本不需改）；
- `warm_to` 目前是同步"一步到位"（模拟器）；真机可改成异步命令（sync 每 tick 升温
  1℃，脚本调用不变）；
- `env` 表可放 Flash；固件侧按需用 `Scl_Env_Save/Load` 固化；
- 脚本命令的 const 程序与命令库一起编译即入 Flash，运行期不占字节码 RAM。
