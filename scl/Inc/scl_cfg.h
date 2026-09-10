/**
  ******************************************************************************
  * @file    scl_cfg.h
  * @brief   SCL（Shell-Command-Link）可移植配置头
  *
  *          所有"占用/能力"相关的编译期裁剪宏集中在此，用户可：
  *            1) 直接修改本文件默认值；
  *            2) 或在编译命令行用 -DSCL_CFG_XXX=yyy 覆盖（本文件所有宏均带 #ifndef 保护）。
  *
  *          说明：
   *            - 默认静态内存、无 malloc；可选动态模式由应用提供 allocator；
  *            - 关闭 SCL_CFG_MSG_EN 后，库内消息输出整段裁掉，连移植函数
  *              SCL_Port_PutChar 都可不实现；
  *            - 占用预算见 doc/arc/shell-command-link-design.md §5.4。
  ******************************************************************************
  */

#ifndef __SCL_CFG_H__
#define __SCL_CFG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 变量相关 ============================ */

/* 变量槽数量（v0.2 类型化后默认提到 4，可用 -D 裁剪回 2） */
#ifndef SCL_CFG_VAR_MAX
#define SCL_CFG_VAR_MAX          4u
#endif

/* 变量名最大字符数（不含结尾 '\0'） */
#ifndef SCL_CFG_VAR_NAME_MAX
#define SCL_CFG_VAR_NAME_MAX     8u
#endif

/* 变量值缓冲字节数（含结尾 '\0'，即最多可存 15 字符） */
#ifndef SCL_CFG_VAR_VALUE_MAX
#define SCL_CFG_VAR_VALUE_MAX    16u
#endif

/* ============================ 主开关：普通态 / mini 态（二选一） ============================ */

/* SCL 只有两种形态，由本主开关决定（无需/不建议再手设其它裁剪宏）：
   0=普通态：文本指令链 SCL_Run + 预编译 SCL_RunProg + env + 描述 + 全消息；
   1=mini 态：解释器收敛为最简 argc/argv 解析（SCL_RunLine/SCL_CmdInvoke），
     自动派生：RUN_TEXT=0、RUN_PROG=0、ENV=0、SCMD=0、消息默认级=ERR；
     描述(CMDDESC)仍保留。各宏仍可用 -D...=x 单独覆盖。
   【后果】置 1 换体积：库不再含文本编译/解释器与预编译程序支持
     （ARM -O2 实测 mini 档 Flash 约 4.7 KB / RAM 约 0.45 KB），
     但动态文本脚本、const 预编译程序、env 与脚本命令均不可用。 */
#ifndef SCL_CFG_MINI_EN
#define SCL_CFG_MINI_EN         0u
#endif

/* 注册表开关：1=使用 scl/cmd/scl_cmd_list.c（由 scl/tool/scl_gen_list.py 生成的
   "命令数组 + 静态变量数组 + SCL_RegList_Init"），SCL_Init 时自动注册命令与静态变量；
   0=关闭自动注册（需自行调用 SCL_RegisterCmd/SCL_VarBind）。
   生成物以**弱符号** SCL_RegList_Init() 接入：未链接该文件时也不报错（静默跳过）。
   这是本库唯一的注册方式（不再手写各模块的 Xxx_Register）。
   【后果】置 0：不再自动注册，命令与静态变量必须由宿主逐个 SCL_RegisterCmd/
     SCL_VarBindOne 手工登记（命令仍可用，但漏登记不会被任何机制提醒）。 */
#ifndef SCL_CFG_REG_LIST_EN
#define SCL_CFG_REG_LIST_EN     1u
#endif

/* 业务命令 opcode 起点：注册表命令按 `BASE + 表内下标` 分配；
   手工 SCL_RegisterCmd 从 `BASE + SCL_CFG_CMD_RESERVE` 起继续。
   （生成物 scl_cmd_list.c 也用它，故必须放在公共配置头）
   【后果】改大/改小只影响 opcode 编号，不影响已烧录的 const 预编译程序
     （它们按命令名调用，不落 opcode）；但与宿主侧硬编码的编号会不一致。 */
#ifndef SCL_CFG_OP_CMD_BASE
#define SCL_CFG_OP_CMD_BASE     0x0100u
#endif

/* 注册表（scl/cmd/scl_cmd_list.c）命令占用的 opcode 区间长度。
   注册表命令 opcode = SCL_CFG_OP_CMD_BASE + 表内下标，故必须给注册表留出足够区间；
   手工 SCL_RegisterCmd 的命令从 BASE + 本值 起继续分配，两者不冲突。
   【后果】设小了：生成器会在构建期 #error 提示调大（不会运行期越界）；
     设大了：只浪费 opcode 编号空间，不占 Flash/RAM。 */
#ifndef SCL_CFG_CMD_RESERVE
#define SCL_CFG_CMD_RESERVE     64u
#endif

/* 静态变量绑定表开关：1=保留"名字→getter/setter"路由表（static 型变量可用）；
   0=编译期关掉该表（数组与登记循环整段不生成）。
   【后果】置 0：普通态实测 RAM −128 B / Flash −216 B；
     代价是 static 型变量不可用（SCL_VarBind / SCL_VarBindOne 变空操作、查表恒不命中）。
   普通态默认 1（不用 static 变量的工程可置 0 换 RAM）；mini 态必须 1。 */
#ifndef SCL_CFG_VAR_BIND_EN
#define SCL_CFG_VAR_BIND_EN     1u
#endif

#if (SCL_CFG_MINI_EN != 0u) && (SCL_CFG_VAR_BIND_EN == 0u)
#error "mini 态必须保留绑定表（SCL_CFG_VAR_BIND_EN=1）"
#endif

/* 绑定表容量（项数）：**由上面开关直接推导**，不再是独立配置项。
   普通态固定 8；该值为 0 时实现侧用零长数组（不占 RAM，登记与查找恒不命中）。
   static 变量条数超过容量时登记被截断（SCL_VarBindOne 返回 0），需在集成阶段发现。 */
#if (SCL_CFG_VAR_BIND_EN != 0u)
#define SCL_CFG_VAR_BIND_MAX    8u
#else
#define SCL_CFG_VAR_BIND_MAX    0u
#endif

/* 消息运行级默认值：运行时把 SCL_MsgLvl 设为 SCL_CFG_MSG_LVL（枚举见 scl.h）。
   0=全静默；SCL_MSG_ALL(0xFF)=全量。编译不裁剪消息代码，仅按级别运行开关。
   普通态默认全量；mini 态默认仅错误。 */
#ifndef SCL_CFG_MSG_LVL
#if (SCL_CFG_MINI_EN != 0u)
#define SCL_CFG_MSG_LVL         1u      /* mini：默认只出错误 */
#else
#define SCL_CFG_MSG_LVL         0xFFu   /* 普通：默认全量 */
#endif
#endif

/* 动态内存模式：1=由 SCL_InitEx 注入的 allocator 管理运行缓冲；0=静态数组（默认）。
   【后果】开 1：变量名/值与解释器工作区改由 allocator 按需分配，宿主必须
     提供 alloc/realloc/free（无 malloc 的系统需自行适配）；
     关 0：全部走编译期静态缓冲，RAM 占用固定、无碎片、无宿主依赖。
   它只改变"内存来源"，不改变任何对外语义。 */
#ifndef SCL_CFG_DYNAMIC_MEM_EN
#define SCL_CFG_DYNAMIC_MEM_EN   0u
#endif

/* ============================ 文本 / 字节码 / 参数缓存 ============================ */

/* 单条指令链文本的最大长度（含结尾 '\0'）：SCL_Run() 读取并编译，
   超长拒绝。文本不长期驻留（编译成字节码后即丢弃）。 */
#ifndef SCL_CFG_SCRIPT_MAX
#define SCL_CFG_SCRIPT_MAX       512u
#endif

/* 字节码缓冲字节数：每条指令固定 4 字节(opc2 + argOff2)，最多 BC_MAX/4 条 */
#ifndef SCL_CFG_BC_MAX
#define SCL_CFG_BC_MAX           512u
#endif

/* 参数字节缓存：每条带参指令存 [len(1)][参数原文...]；argOff 指向其起点 */
#ifndef SCL_CFG_ARG_CACHE_MAX
#define SCL_CFG_ARG_CACHE_MAX    256u
#endif

/* label 表容量（label 指令不产字节，只登记名字→字节偏移） */
#ifndef SCL_CFG_LABEL_MAX
#define SCL_CFG_LABEL_MAX        16u
#endif

/* label 名最大长度（不含结尾 '\0'） */
#ifndef SCL_CFG_LABEL_NAME_MAX
#define SCL_CFG_LABEL_NAME_MAX   16u
#endif

/* 每脚本最大解释步数（防死循环；0=关闭）。超限自动中断并报错 */
#ifndef SCL_CFG_STEP_LIMIT
#define SCL_CFG_STEP_LIMIT       100000u
#endif

/* ============================ 执行源（可裁剪） ============================ */

/* 动态文本脚本支持：SCL_Run() 读取文本 → 运行时编译进 RAM 字节码/参数缓存执行。
   普通态默认 1；mini 态由主开关派生为 0（只留最简 argc/argv 入口） */
#ifndef SCL_CFG_RUN_TEXT_EN
#if (SCL_CFG_MINI_EN != 0u)
#define SCL_CFG_RUN_TEXT_EN      0u
#else
#define SCL_CFG_RUN_TEXT_EN      1u
#endif
#endif

/* 预编译只读程序支持：SCL_RunProg() 直接解释 const 程序（数据放 Flash，几乎不占 RAM）。
   普通态默认 1；mini 态派生为 0 */
#ifndef SCL_CFG_RUN_PROG_EN
#if (SCL_CFG_MINI_EN != 0u)
#define SCL_CFG_RUN_PROG_EN      0u
#else
#define SCL_CFG_RUN_PROG_EN      1u
#endif
#endif

/* 普通态至少需要一个解释器；mini 态允许全关（只留最简 argc/argv 解释器） */
#if ((SCL_CFG_RUN_TEXT_EN) == 0u) && ((SCL_CFG_RUN_PROG_EN) == 0u) && ((SCL_CFG_MINI_EN) == 0u)
#error "普通态需 SCL_CFG_RUN_TEXT_EN 与 SCL_CFG_RUN_PROG_EN 至少一个为 1；mini 态请置 SCL_CFG_MINI_EN=1"
#endif

/* 脚本命令（s2c 编译产物注册成命令）总开关：1=SCL_Scmd_* 可用；0=裁掉。
   执行用 SCL_RunProg（预编译 const 程序），故需 SCL_CFG_RUN_PROG_EN=1；mini 态派生为 0 */
#ifndef SCL_CFG_SCMD_EN
#if (SCL_CFG_MINI_EN != 0u)
#define SCL_CFG_SCMD_EN      0u
#else
#define SCL_CFG_SCMD_EN      1u
#endif
#endif

/* ============================ 环境变量缓冲（持久配置） ============================ */

/* 环境变量缓冲总开关：1=支持（默认配置表装载/用户存储装载、序列化固化导出）。
   环境变量在脚本读路径（${}、VarGet、运算/真值操作数）可见（会话变量优先）；
   0=裁掉该表；SCL_CFG_MINI_EN 最简档默认裁掉（可用 -D 覆盖为 1）。
   【后果】置 0：Scl_Env_* 全部不可用，持久配置需宿主自行维护；
     写路径不受影响（脚本只能写会话变量）。 */
#ifndef SCL_CFG_ENV_EN
#if (SCL_CFG_MINI_EN != 0u)
#define SCL_CFG_ENV_EN        0u
#else
#define SCL_CFG_ENV_EN        1u
#endif
#endif

/* env 槽数量（<=255，序列化字段为 1 字节） */
#ifndef SCL_CFG_ENV_MAX
#define SCL_CFG_ENV_MAX       8u
#endif

#if ((SCL_CFG_ENV_EN) != 0u) && ((SCL_CFG_ENV_MAX) > 255u)
#error "SCL_CFG_ENV_MAX must <= 255"
#endif

/* ============================ 命令描述注册辅助（argtable3 风格） ============================ */

/* 命令描述总开关：1=命令可带描述（help + 参数模板），注册后自动校验参数、
   错误输出 usage、help 汇总带说明；0=裁掉（scl_cmd_t 无 desc 字段）。
   【后果】置 0：省下描述结构与帮助文本，但没有参数模板校验、
     help 只能列出命令名（参数个数/类型不再提示）。 */
#ifndef SCL_CFG_CMDDESC_EN
#define SCL_CFG_CMDDESC_EN     1u
#endif

/* ============================ 命令参数 ============================ */

/* 单条命令最大参数个数（argv 数组长度） */
#ifndef SCL_CFG_ARG_MAX
#define SCL_CFG_ARG_MAX          8u
#endif

/* 单个参数（${} 展开后）最大长度（含结尾 '\0'） */
#ifndef SCL_CFG_ARG_LEN_MAX
#define SCL_CFG_ARG_LEN_MAX      32u
#endif

/* 命令参数工作缓冲总字节数（内部推导，勿改） */
#define SCL_CFG_ARG_BUF_BYTES    ((SCL_CFG_ARG_MAX) * (SCL_CFG_ARG_LEN_MAX))

/* ============================ 输出 ============================ */

/* 消息输出开关：0=关闭（库内所有提示整段裁掉，可不实现 SCL_Port_PutChar）
   【后果】置 0：不再有任何库内文字输出（含错误提示），
     消息相关函数退化为头内联空实现（调用点零开销）；
     出错只能靠返回值/状态区分，是嵌入式量产档常用的省体积手段。 */
#ifndef SCL_CFG_MSG_EN
#define SCL_CFG_MSG_EN           1u
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SCL_CFG_H__ */
