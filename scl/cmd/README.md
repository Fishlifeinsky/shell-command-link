# scl/cmd：命令源码目录

本目录放**命令实现**，一个命令一个 `.c` 文件；注册靠宏 + 生成表，**不需要手写 `Xxx_Register()`**。

## 新增一条命令

1. 在本目录新建 `cmd_<名字>.c`：

```c
#include "scl_reg.h"
#include "cmd_util.h"          /* 需要打印时用 SclCmd_Talk */

static void Cmd_foo(int argc, char *argv[]);
static const scl_arg_spec_t a_foo_x[] = { { "x", SCL_T_INT, 0u, "参数说明" } };

static void Cmd_foo(int argc, char *argv[])
{
    (void)argv;
    SclCmd_Talk("foo %d\n", argc);
}

SCL_CMD_DEFINE(foo, Cmd_foo, NULL, "一行帮助", a_foo_x);   /* 无参数模板用 SCL_CMD_DEFINE_NA */
```

2. 重新生成注册表并编译：

```powershell
python scl/tool/scl_gen_list.py scl/cmd -o scl/cmd/scl_cmd_list.c
```

3. 生成器会打印扫描清单（文件 → 命令名），**命令数不对就是漏扫**，别忽略这条日志。

## 规则（生成器依赖）

| 项 | 要求 |
|---|---|
| 宏 | `SCL_CMD_DEFINE` / `SCL_CMD_DEFINE_NA` / `SCL_VAR_DEFINE`，见 `scl/Inc/scl_reg.h` |
| 首参数 | 必须是简单标识符（`[A-Za-z_][A-Za-z0-9_]*`），即命令名/变量名 |
| 括号 | 宏调用内括号必须平衡；不要用宏拼命令名 |
| 重名 | 同名命令/变量会**构建期报错** |
| 顺序 | 生成表按**文件名排序**（决定 `help` 顺序与 opc 编号） |

## 相关文件

- `scl_cmd_list.c`：**生成物**（命令数组 + 变量数组 + `SCL_RegList_Init`），勿手改
- `../tool/scl_gen_list.py`：生成器
- `../scl.cmake`：CMake 集成入口（输出 `SCL_SRC_LIST` / `SCL_INCLUDE_DIRS`）
