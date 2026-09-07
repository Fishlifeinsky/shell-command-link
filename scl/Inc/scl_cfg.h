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

/* 变量槽数量（用户确认默认：2） */
#ifndef SCL_CFG_VAR_MAX
#define SCL_CFG_VAR_MAX          2u
#endif

/* 变量名最大字符数（不含结尾 '\0'） */
#ifndef SCL_CFG_VAR_NAME_MAX
#define SCL_CFG_VAR_NAME_MAX     8u
#endif

/* 变量值缓冲字节数（含结尾 '\0'，即最多可存 15 字符） */
#ifndef SCL_CFG_VAR_VALUE_MAX
#define SCL_CFG_VAR_VALUE_MAX    16u
#endif

/* ============================ 脚本/程序缓冲 ============================ */

/* 内部程序文本缓冲字节数（含结尾 '\0'）：异步跨主循环步进需要，
   SCL_Run() 会把整条脚本拷贝进来，调用方缓冲可即刻复用。
   单条脚本超过该值将被拒绝。 */
#ifndef SCL_CFG_SCRIPT_MAX
#define SCL_CFG_SCRIPT_MAX       256u
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

/* ============================ 流程控制 ============================ */

/* 执行帧栈深度上限（if 分支子链 / while 嵌套 共用，含 while 与段帧） */
#ifndef SCL_CFG_NEST_MAX
#define SCL_CFG_NEST_MAX         3u
#endif

/* while 总迭代兜底保护：G_RETURN 一直为真且未显式给上限时，达到此值强制退出防死循环 */
#ifndef SCL_CFG_WHILE_MAX
#define SCL_CFG_WHILE_MAX        100000u
#endif

/* ============================ 输出 ============================ */

/* 消息输出开关：0=关闭（库内所有提示整段裁掉，可不实现 SCL_Port_PutChar） */
#ifndef SCL_CFG_MSG_EN
#define SCL_CFG_MSG_EN           1u
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SCL_CFG_H__ */
