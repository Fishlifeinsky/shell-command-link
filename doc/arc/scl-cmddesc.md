# SCL v0.3：多模块源码 + 命令描述注册辅助（CMDDesc，argtable3 风格）

> 类别：架构/使用 · 状态：已实现 · 文件：`scl/Src/*`、`scl/Inc/scl.h`、`example/demo_cmds.c`、`example/main.c`(第 9 节)

## 1. 源码多模块（可独立编译/裁剪）

SCL 源码按主题拆成三个编译单元 + 一个内部共享头：

| 文件 | 职责 | 裁剪 |
| --- | --- | --- |
| `scl/Src/scl.c` | 核心：文本工具/编译/执行/命令注册/异步/内置命令 | `RUN_TEXT/RUN_PROG/CMDDESC/...` |
| `scl/Src/scl_var.c` | 会话变量表与管理（读回退 FindAny） | `SCL_CFG_VAR_*` |
| `scl/Src/scl_env.c` | 环境变量缓冲（持久配置） | `SCL_CFG_ENV_EN` |
| `scl/Src/scl_priv.h` | 内部共享类型/状态/函数声明 | —（勿在应用层使用） |

要点：
- **公共头只有 `scl.h`**；多文件对外行为不变（API/裁剪宏一致）。
- 三个 .c **必须一起编译链接**（var/env 依赖 core 的工具函数，core 依赖
  var/env 的读回退与状态）。
- 想进一步"按需取文件"时：`SCL_CFG_ENV_EN=0` 可不编 `scl_env.c`（把声明引用裁掉），
  `RUN_TEXT_EN=0` 裁掉动态编译；会话变量/环境变量与执行器仍是强耦合的最小集合。
- 构建示例与 CMake 已纳入全部源文件（见 README/example/CMakeLists.txt）。

## 2. 命令描述注册辅助（CMDDesc，参考 ESP-IDF console + argtable3）

SCL 命令原本只注册 `{name, fn, sync}`，参数语义要自己解析、帮助要手写。
现在可像 argtable3 一样**声明式描述命令**：一行帮助 + 参数模板，注册后库自动：

- **参数数量/类型校验**：调用前比对模板，不符输出错误并打印 `usage`，中止脚本；
- **帮助增强**：`help` 自动列出每条命令的帮助文本与模板概要（`usage` 行）；
- **统一注册**：`SCL_CmdRegisterDesc()` 一条 API。

### 声明式命令示例（example/demo_cmds.c）

```c
/* 参数模板：脚本按位置匹配（模板 STR 接受任意；INT 接受整数或可解析文本；
   BOOL/FLAG 要求脚本字面量类型一致） */
static const scl_arg_spec_t a_wait_n[] = {
    { "n", SCL_T_INT, 0u, "模拟耗时 Loop 数" },   /* name,type,opt(0必选),help */
};

static const scl_cmd_desc_t s_desc_wait = {
    "wait",                 /* 命令名 */
    "模拟耗时（异步）",      /* 一行帮助（help 显示） */
    a_wait_n, 1,            /* 参数模板 + 条数 */
    Cmd_wait, Sync_wait,    /* 命令实现 + 异步同步回调 */
};

void Scl_Demo_Register(void)
{
    ...
    SCL_CmdRegisterDesc(&s_cmd_wait, &s_desc_wait);   /* 节点静态存储 + 描述 */
}
```

脚本侧自动获得校验：
```text
wait 100          → OK（异步执行）
wait              → scl: 命令 'wait': 缺少参数（至少 1 个）
                    usage: wait <n:int>
wait 1 2          → scl: 命令 'wait': 参数过多（最多 1 个）  + usage
wait abc          → scl: 命令 'wait': 参数 1 'abc' 期望 int   + usage
```

`help` 输出示例：
```text
scl: 已注册命令:
  wait (异步) - 模拟耗时（异步）
      usage: wait <n:int>
  demo_reset - 计数清零并设目标
      usage: demo_reset [n:int]
```

### API 速览（scl.h，`SCL_CFG_CMDDESC_EN` 裁剪）

- `SCL_CmdRegisterDesc(scl_cmd_t *node, const scl_cmd_desc_t *desc)`
  按描述注册（自动填 name/fn/sync/desc 并挂链）。
- `scl_cmd_desc_t { name; help; args; arg_cnt; fn; sync }`
- `scl_arg_spec_t  { name; type(SCL_T_*); opt; help }`
  模板约定：`args==NULL` → 不限参（如 `echo`）；`arg_cnt>0` → 按模板校验；
  模板必选数 = `opt==0` 的条数（缺必选/多给/类型不符都拒绝 + usage）。
- `int32_t SCL_ParseInt(const char *s, int32_t def)`：命令内把 argv 文本转数字
  （十进制/0x/0b；失败给默认），无需 libc `atoi`。
- 关闭 `SCL_CFG_CMDDESC_EN=0`：`scl_cmd_t` 无 desc 字段、无校验/usage，
  退回普通注册（兼容老代码）。

### 与 ESP-IDF console 的对照

| ESP-IDF / argtable3 | SCL 对应 |
| --- | --- |
| `esp_console_cmd_register` | `SCL_CmdRegisterDesc`（普通仍用 `SCL_RegisterCmd`） |
| 命令带 `help` | `scl_cmd_desc_t.help`（`help` 自动汇总） |
| `argtable3` 参数条目 | `scl_arg_spec_t[]` 位置模板（脚本参数已类型化） |
| `arg_parse` 校验/usage | 库在调用前自动校验并输出 `usage` |
| `arg_int` 取值 | 直接 `argv[idx]` + `SCL_ParseInt`/`SCL_ArgType` |
| `arg_end` 错误提示 | 库输出 `命令 'x': 参数 n 'v' 期望 int` 并中止脚本 |

## 3. 验证

`scl_test` 第 9 节：help 带描述与 usage、缺参/错类型/过多参数被拒、合法执行、
拒绝后状态干净、`SCL_ParseInt` 解析。基线：scl_test PASS=127 / s2c_test PASS=75；
`SCL_CFG_CMDDESC_EN=0` 变体 PASS=108（裁剪正确）。

## 4. `help <cmd>` 单命令明细 + Shell 联动（esp_console 风格）

`help` 原本只做全览；参数（`help <cmd>`）此前被编译缓存但执行端未用。本次接入：

- **执行端**：`help` 的 raw 参数传入 `Scl_DoHelpRaw()` —— 无参=全览（输出不变）；
  带命令名=单命令明细，查找顺序：注册命令 → 内置元命令 → 内置运算（分组），
  大小写不敏感，未知命令提示“用 help 查看全部”。
- **注册命令明细**：有 `desc` 输出 `名字（异步）— 帮助` + `usage:` + 逐参
  `name<type> [必选/可选]  帮助`；无 `desc` 回落普通说明（不依赖 CMDDESC 也能用）。
- **内置文档表**：`var/free/help/label/jump` 元命令与 `iadd/…/seq` 等运算分组
  各给一行到数行中文说明（ROM 只读，条件不变）。
- **Shell 联动**（example/scl_shell.c，CMDDESC=1 时）：
  - Tab 多候选时**逐行**列出候选，注册命令带 desc 则每行附一行帮助；
  - 参数位（首命令完整且带参数模板）按 Tab 输出该命令 `usage:` 行（不插文本）。

测试：第 9 节新增 `help <cmd>` 8 例；Shell 第 7 节新增 desc 联动 3 例（desc 宏内）。
