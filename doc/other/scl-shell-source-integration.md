# SCL Shell 源码级接入说明

## 背景

此前 Shell 逻辑只存在于 example 目录里，库主体仍缺少正式的源文件入口。用户要求将其纳入 SCL 源码树，并保持可裁剪、可选启用的模式。

## 结论

- Shell 实现正式归档到 `scl/Inc/scl_shell.h` 与 `scl/Src/scl_shell.c`。
- 默认开关 `SCL_EX_SHELL_EN` 控制整个 Shell 模块是否编译；关闭后无实现、无静态缓冲、无额外依赖。
- 细粒度开关继续保留：ANSI、编辑、历史、补全、光标、单词导航、行导航等均可单独关闭。
- 运行时仍依赖 `SCL_Run()` / `SCL_Loop()` / `SCL_VarKeep()`，保持与库原有语义一致。

## 实际设计

1. 公开接口在 `scl/Inc` 中暴露，作为库对外 API。
2. 实现位于 `scl/Src`，便于库构建脚本、CMake、手工 gcc 编译统一引入。
3. example 目录保留示例代码，但不再是唯一的实现来源。
4. 构建脚本和 README 同步更新：默认的 PC 测试与交互终端都使用库内 `scl_shell.c`。

## 裁剪方式

```c
#define SCL_EX_SHELL_EN 0u
#define SCL_EX_SHELL_HISTORY_EN 0u
#define SCL_EX_SHELL_COMPLETION_EN 0u
```

这保证 Shell 能像其他 SCL 功能一样在编译期裁剪，不影响最小化 MCU 方案。

## 兼容性

- example 下的 `scl_shell.c` 仍可保留作为历史兼容文件；
- 但核心构建和默认链接已切换为 `scl/Src/scl_shell.c`；
- 这样既满足“放到 scl 源码里面”，又避免破坏旧示例工程。
