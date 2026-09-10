---
name: scl
description: "适用于 Shell-Command-Link 嵌入式脚本库的开发：修改运行时解释器、命令注册、脚本编译器、环境变量、const 程序生成，或将 SCL 脚本与 C 代码集成到这个仓库中。任何修改提案都必须先在 doc/idea/ 建立'执行+闭环'文档，写清场景、影响面与验证标准，改完回填实测数据与提交号。"
---

# Shell-Command-Link (SCL)

## 改动提案规约（强制，先读这条）

**任何 agent 想对本库提出更改，必须先在 `doc/idea/` 建立一份"执行 + 闭环"文档**，
然后才能动代码。目录与规范见本仓库 `doc/idea/README.md`（模板 `doc/idea/_TEMPLATE.md`）。

流程四步（缺一不可）：

```
① 提案  doc/idea/<主题>.md，按 doc/idea/_TEMPLATE.md 填写
        必填：场景与动机、现状、方案、影响面、验证命令、闭环标准
② 执行  改代码；跨多个函数/多仓库的改动，先给用户计划并等待确认
③ 验证  跑闭环标准里的命令，记录可复现命令 + 实测数据
④ 闭环  回填实测数据与提交号；结论沉淀到 doc/arc | doc/debug | doc/spec
```

要点：

- 命名：`小写短横线主题.md`；一份文档只讲一个场景/变更点
- 状态标记：`提案中` → `执行中` → `已闭环`；未采纳写 `已否决` / `搁置`
- **影响面表**必须填：静态 RAM / Flash / API / 配置开关 / 普通态与 mini 态 / 生成物 / 兼容性
- **闭环标准**必须可勾选：编译零告警、回归全绿、体积前后实测数字、文档落点、提交分类
- 只改文档不改行为（纯 `md` 提交）可不新建 idea 文档，但仍须遵守 `doc/` 分类

## 一句话概括

SCL 是一个面向 C 项目的轻量级嵌入式脚本运行时。它把命令 shell、类型变量、控制流脚本、以及可选的 Flash 只读程序整合在一起，让应用可以在不依赖完整运行时和 libc 的前提下执行紧凑脚本逻辑。

## 仓库结构与定位

这个仓库分成三层：

- 核心运行时：`scl/Inc/scl.h`、`scl/Inc/scl_cfg.h`、`scl/Inc/scl_reg.h`，
  源码（`scl/Src/`）按职责分为 8 个模块：`scl.c`(初始化编排) / `scl_exec.c`(编译链+执行核) /
  `scl_cmd.c`(注册表·调用·一行解析) / `scl_desc.c`(描述层) / `scl_core.c`(文本·消息) /
  `scl_mem.c`(动态内存) / `scl_var.c`(变量) / `scl_env.c`(env)；模块表见 `doc/arc/scl-modules.md`
- 脚本编译/转译：`tools/scl_script2chain.py`、`tools/scl_emit_c.py`
- 示例与集成：`example/`、`example/big_demo/`、`example/mcu_template/`

设计目标是：低 ROM/RAM 占用、无标准库依赖、默认静态内存，必要时支持动态内存分配。

## 核心概念

### 1. 命令与 shell 执行

SCL 提供 shell 风格的命令分发器，命令会被注册并由脚本文本或交互输入调用。

- 对外入口包括 `SCL_Init`、`SCL_InitEx`、`SCL_Run`、`SCL_Loop`、`SCL_RunProg`
- 命令通过 `SCL_RegisterCmd` 等 API 注册
- 内置执行流程使用 `G_RETURN` 作为条件结果和跳转判断
- `jump -a` 会读取返回标志，并在读取后自动清零

### 2. 变量与类型状态

变量设计紧凑、适合嵌入式环境，可在配置下使用静态或动态分配。

- 类型包括 bool、int、flag、string 等
- 常用接口：`SCL_VarSet`、`SCL_VarSetT`、`SCL_VarGet`、`SCL_VarFree`、`SCL_VarKeep`
- `const` 值应当在编译期折叠，不应当被当做可写运行时槽位
- 会话变量可在显式保留时跨命令保留

### 3. 文本脚本与编译链程序

运行时支持两种执行模式：

- 文本编译路径：`SCL_Run("...")` 把脚本文本编译成内部指令链
- 只读 const 路径：`tools/scl_emit_c.py` 把脚本生成 Flash 友好的字节码，随后用 `SCL_RunProg()` 执行，不需要每个脚本单独占用 RAM 缓冲

固定逻辑优先用 const 程序；动态脚本和开发调试优先走文本执行。

### 4. 脚本编译器行为

`tools/scl_script2chain.py` 是把简洁的 S2C 语法翻译成内部指令链的 Python 编译器。

- `const` 常量尽量在编译期折叠
- alias 在编译期展开，不要依赖运行时反射查找
- 函数体可能被内联，或按启发式变成 `callf` / `retf` 形式
- 现代源语法更适合可读写，而底层指令链保留给高压缩/低层控制

## 关键设计约束

### 尽量压缩内存

这个库偏好：

- 默认静态分配
- 编译期常量折叠
- 固定大小的表和缓存
- 通过配置开关启用动态分配
- 通过 `scl/Inc/scl_cfg.h` 做裁剪

不要在不配置的情况下引入“运行时 heap 依赖”式特性，默认行为必须保持小体积和可预测性。

### 优先编译期决策

如果数据在编译期就知道，优先做编译期消除和展开，而不是在运行时增加复杂逻辑。

典型例子：

- `const` 直接落成字面量程序
- alias 在生成链时替换
- 小函数和高频函数尽量内联，而不是全走运行时子程序栈

### 遵守解释器模型

SCL 是一个小型 VM 风格解释器，而不是通用脚本引擎：

- 语句短小且确定
- 控制流依赖 label / jump
- `G_RETURN` 是标准条件标志
- 运行过程由 `SCL_Loop()` 分步推进

不要引入广泛的反射、对象化或复杂动态机制，除非能明显符合嵌入式低占用设计。

## 重点文件

开发时优先从这些文件入手：

- `README.md`：总体概览与用法
- `doc/idea/README.md`：**改动提案规约（强制）**，改动前必读
- `doc/spec/scl-spec.md`：语法与 API 约定
- `doc/arc/scl-const-prog.md`：只读 const 程序说明
- `doc/arc/scl-dynamic-mem.md`：可选动态内存与 cache 行为
- `scl/Inc/scl_cfg.h`：编译期配置
- `scl/Inc/scl.h`：公共 API
- `tools/scl_script2chain.py`：S2C 编译器
- `tools/scl_emit_c.py`：const 程序生成器
- `example/big_demo/`：端到端验证项目

## 常见工作内容

### 添加命令

1. 实现对应的 C 处理函数
2. 用 `SCL_RegisterCmd` 或项目中的命令注册入口注册
3. 若命令描述开启，则补充 help/usage 描述
4. 用 S2C 或示例工程做验证

### 调整脚本语言行为

1. 先查看 `tools/scl_script2chain.py`
2. 修改解析器/展开器规则
3. 保持 alias 和 const 折叠在编译期完成
4. 运行 `python tools/s2c_test.py` 验证

### 把脚本转成 Flash 程序

1. 执行 `python tools/scl_emit_c.py <script> -o <output.c>`
2. 把生成程序接入固件
3. 通过 `SCL_RunProg()` 执行
4. 最小 RAM 配置下维持 `SCL_CFG_RUN_TEXT_EN=0`

### 做内存敏感修改

- 优先编译期折叠
- 仅在必要时启用 `SCL_CFG_DYNAMIC_MEM_EN`
- 控制表大小和缓存上限
- 在改 allocator 逻辑前检查 `cache` / GC 行为

## 验证命令

在修改解释器或编译器时，优先使用仓库自带验证：

```bash
python tools/s2c_test.py
python example/big_demo/build_big.py
```

其余构建/测试入口见 `README.md` 和脚本工具本身。

## 修改时的基本原则

- **提出任何更改前，先在 `doc/idea/` 建立"执行 + 闭环"文档**（见本文件开头规约）
- 保持无 libc、无 OS、无标准库的嵌入式设计
- 让运行时语义保持简单和可预测
- 优先显式配置，而不是隐式运行期魔法
- 同时兼容文本执行和 const Flash 程序
- 如果新增复杂度，先考虑能否通过编译期降级/展开解决

## 成功标准

一个好的改动通常满足以下条件：

- 仍保留很小的内存占用
- 脚本行为仍然确定且稳定
- 编译期优化仍然有效
- 示例和测试继续通过
- 不引入隐式的 libc / OS / heap 依赖
- 对应的 `doc/idea/<主题>.md` 已回填实测数据与提交号（状态置为"已闭环"）
