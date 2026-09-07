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
  *            - 库全程静态内存、无 malloc、无 OS、无 HAL、无 libc 依赖；
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
