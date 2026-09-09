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

/* 动态内存模式：1=由 SCL_InitEx 提供的 allocator 管理运行缓冲；0=静态数组。
   动态模式下变量名/值按需分配，解释器工作区在初始化时分配。 */
#ifndef SCL_CFG_DYNAMIC_MEM_EN
#define SCL_CFG_DYNAMIC_MEM_EN   0u
#endif

/* 动态模式下保留的最小变量槽数量；变量名/值仍按首次使用懒分配。 */
#ifndef SCL_CFG_DYNAMIC_VAR_MAX
#define SCL_CFG_DYNAMIC_VAR_MAX  SCL_CFG_VAR_MAX
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
   1=支持（占 RAM：字节码/参数缓存/label 表/中间指令表）；
   0=裁剪整段（省 RAM，需用 Flash 预编译程序 SCL_RunProg 执行固定脚本） */
#ifndef SCL_CFG_RUN_TEXT_EN
#define SCL_CFG_RUN_TEXT_EN      1u
#endif

/* 预编译只读程序支持：SCL_RunProg() 直接解释 const 程序（数据放 Flash，几乎不占 RAM）。
   1=支持；0=裁剪 */
#ifndef SCL_CFG_RUN_PROG_EN
#define SCL_CFG_RUN_PROG_EN      1u
#endif

#if ((SCL_CFG_RUN_TEXT_EN) == 0u) && ((SCL_CFG_RUN_PROG_EN) == 0u)
#error "SCL_CFG_RUN_TEXT_EN 与 SCL_CFG_RUN_PROG_EN 至少需一个为 1"
#endif

/* 脚本命令（s2c 编译产物注册成命令）总开关：1=SCL_Scmd_* 可用；0=裁掉。
   执行用 SCL_RunProg（预编译 const 程序），故需 SCL_CFG_RUN_PROG_EN=1 */
#ifndef SCL_CFG_SCMD_EN
#define SCL_CFG_SCMD_EN      1u
#endif

/* ============================ 环境变量缓冲（持久配置） ============================ */

/* 环境变量缓冲总开关：1=支持（默认配置表装载/用户存储装载、序列化固化导出）。
   环境变量在脚本读路径（${}、VarGet、运算/真值操作数）可见（会话变量优先）；
   0=裁掉该表 */
#ifndef SCL_CFG_ENV_EN
#define SCL_CFG_ENV_EN        1u
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
   错误输出 usage、help 汇总带说明；0=裁掉（scl_cmd_t 无 desc 字段） */
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

/* 消息输出开关：0=关闭（库内所有提示整段裁掉，可不实现 SCL_Port_PutChar） */
#ifndef SCL_CFG_MSG_EN
#define SCL_CFG_MSG_EN           1u
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SCL_CFG_H__ */
