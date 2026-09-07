# 用户需求存档：shell + 简单 shell 指令链库

> 归档原始需求（2026-09-07），供开发与评审对照。设计细节见 `doc/arc/shell-command-link-design.md`。

## 用户原话（要点摘录）

我要创建一个 shell + 简单 shell 指令链，参考工程：`C:\Users\dev04\Desktop\work\stm32\test-ms41929`。

### 变量
- 创建变量（定义宏，数量有限为 2，存储为字符串 16 byte）：
  - `var xxxx=xxx` 创建一个名为 `xxxx` 存储 `xxx` 的变量
- 使用变量：在其他指令调用 `${xxxx}` 取 xxxx 的值
- 其他指令（C 命令处理函数）也可以通过接口调用变量
- 变量支持 free；可查看剩余空变量数量；脚本运行完自动 free 所有变量

### 全局标志
- 存在一个全局标志位 `G_RETURN`，默认 false，每次读完重置

### 指令调用
- 除普通调用外，还支持函数式调用：`cmd_xxx(xxx,xxx...)`

### 执行方式
- if：
  - 命令结构 `cmd....; if -t xxxx -f xxxx`
  - `cmd...` 写 G_RETURN，`if` 读取；`-t` 后面字符串在 G_RETURN 为 true 时执行，`-f` 后面在 false 时执行；`-t` 与 `-f` 可同时存在
- while：
  - 命令结构 `while -b; xxxx; cmd...; while -e`
  - `cmd...` 为一个写 G_RETURN 的指令，`while` 指令判断是否退出

### 约束
- 可改成其他更好用的形式，但要求占用低
- 写成一个库，提供移植接口
- 先做左右脑互博（设计推演）再动手
