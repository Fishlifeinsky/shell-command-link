/**
  ******************************************************************************
  * @file    scl.h
  * @brief   SCL（Shell-Command-Link）简易指令链脚本库 —— 公共接口
  *
  *          功能：
  *            - 执行"指令链"（脚本文本）：'cmd a b' 普通式命令 + 汇编式控制流
  *                label <名>            // 设置跳转点（不产字节，登记名→字节偏移）
  *                jump [-a|-b] <名>     // 跳转：-b/默认=无条件；-a=G_RETURN 为真才跳(读后清零)
  *            - SCL_Run() 先把指令链文本**编译成字节码**（每条指令 4 字节：
  *                opc(2B) + argOff(2B)，参数存 [len][原文] 缓存；label 表登记跳转点），
  *                然后主循环解释执行；无 if/while 文本（由上层编译器降级为 label/jump）
  *            - 命令在 SCL_RegisterCmd 时自动分配 opcode
  *            - 变量：'var name=value'（数量/名长/值长由 scl_cfg.h 裁剪）、${name} 取值、
  *              'free' 释放、脚本结束自动释放全部变量
  *            - 条件标志 G_RETURN：默认 false，脚本开始/结束清零；被 jump -a 读取后自动清零；
  *              C 命令用 SCL_Ret_Set() 写入
  *            - 执行模型：异步/跨主循环步进。SCL_Run() 编译后立即返回并置 busy；
  *              主循环周期调 SCL_Loop() 逐条解释执行；命令可带"同步信号回调"
  *              （handler 启动异步操作后立即返回，库每 Loop 轮询 sync(false)，
  *              完成后再 sync(true) 清除并执行下一条）
  *            - 全程静态内存、无 malloc；无 OS / HAL / libc 依赖
  *
  *          保留关键字（不能注册为业务命令）：var / free / help / label / jump
  *            （if / while 已由上层编译器降级，运行时不再提供）
  *
  *          移植接口（用户提供）：
  *            void SCL_Port_PutChar(char c);   // 输出单字符（SCL_CFG_MSG_EN=0 时可不实现）
  ******************************************************************************
  */

#ifndef __SCL_H__
#define __SCL_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 可裁剪配置 */
#include "scl_cfg.h"

#include <stdint.h>
#include <stdbool.h>

/* ============================ 类型定义 ============================ */

/**
  * @brief  命令处理函数类型
  * @param  argc 参数个数（不含命令名）
  * @param  argv 参数指针数组（指向库内展开后实参，仅在本次调用有效，勿保存）
  * @note   无论同步/异步命令，处理函数都应"立即发起并快速返回"；
  *         需耗时的操作在函数内部自行推进，完成与否通过 sync 回调告知库
  */
typedef void (*scl_cmd_handler_t)(int argc, char *argv[]);

/**
  * @brief  同步信号回调类型（异步命令完成检测）
  * @param  clear false=查询命令是否已完成（返回 true 表示已完成）；
  *               true =完成确认后再次调用以清除标志
  * @retval true=已完成；false=仍在进行
  * @note   同步命令该字段为 NULL（处理函数执行完即完成，无需等待）
  */
typedef bool (*scl_sync_t)(bool clear);

/* 命令链表节点：由命令所属模块静态定义，注册后挂入命令链表 */
typedef struct scl_cmd
{
    const char          *name;   /* 命令名（不能为保留关键字） */
    scl_cmd_handler_t    fn;     /* 处理函数 */
    scl_sync_t           sync;   /* 同步信号回调（NULL=同步命令；非 NULL=异步需等待） */
    struct scl_cmd      *next;   /* 链表下一节点（由 SCL_RegisterCmd 维护） */
    uint16_t             opc;    /* 字节码 opcode（SCL_RegisterCmd 自动分配，勿手填） */
} scl_cmd_t;

/* ============================ 生命周期 ============================ */

/**
  * @brief  初始化库：清命令链表/变量/G_RETURN/执行状态
  * @note   未显式调用时，首次 SCL_Run 前会自动初始化一次
  */
void SCL_Init(void);

/**
  * @brief  提交一条指令链（脚本）开始执行
  * @param  script 以 '\0' 结尾的脚本字符串（内部拷贝，调用后缓冲可复用）
  * @retval 1=已接受并置忙；0=忙中拒绝 / 脚本超长 / 为空
  * @note   一次只运行一个脚本；完成后变量自动全部释放、G_RETURN 清零
  */
uint8_t SCL_Run(const char *script);

/**
  * @brief  主循环周期调用：推进脚本执行（含异步命令的完成轮询）
  * @note   未 busy 时直接返回；脚本执行中每调用一步执行一个动作，
  *         业务命令耗时时会在命令边界等待其 sync 回调
  */
void SCL_Loop(void);

/**
  * @brief  查询是否空闲（无脚本执行/无异步等待）
  * @retval 1=空闲；0=正在执行
  */
uint8_t SCL_Idle(void);

/**
  * @brief  请求中断当前脚本（可从中断/其它上下文调用）
  * @note   库在子句边界/异步轮询间隙响应；中断后变量仍会自动全部释放
  */
void SCL_Abort(void);

/* ============================ 命令注册 ============================ */

/**
  * @brief  注册命令节点到命令链表
  * @param  cmd 命令节点指针（节点必须为静态/全局，生命周期贯穿运行期）
  * @note   各模块在自身初始化时调用；命令名不得为保留关键字 if/while/var/free/help
  */
void SCL_RegisterCmd(scl_cmd_t *cmd);

/* ============================ 条件标志 G_RETURN ============================ */

/**
  * @brief  写 G_RETURN（供命令处理函数 / sync 回调调用，供后续 if / while -e 判断）
  * @param  v 0=置假；非 0=置真
  */
void SCL_Ret_Set(int v);

/**
  * @brief  读 G_RETURN（命令内原样读，不清零）
  * @retval 1=真；0=假
  */
int SCL_Ret_Get(void);

/* ============================ 变量接口（供 C 命令使用） ============================ */

/**
  * @brief  取变量值
  * @param  name 变量名
  * @retval 值字符串指针（库内静态存储，脚本结束/释放后失效）；不存在返回 NULL
  */
const char *SCL_VarGet(const char *name);

/**
  * @brief  设置/覆盖变量（供脚本 'var' 与 C 命令共用）
  * @param  name 变量名（[A-Za-z_][A-Za-z0-9_]*，长度 <= SCL_CFG_VAR_NAME_MAX）
  * @param  val  值（长度 <= SCL_CFG_VAR_VALUE_MAX-1）
  * @retval 0=成功；-1=变量已满无空槽；-2=变量名非法/过长；-3=值过长；-4=变量名为空
  */
int SCL_VarSet(const char *name, const char *val);

/**
  * @brief  释放指定变量（不存在则无操作）
  * @param  name 变量名
  * @retval 0=已释放；-1=不存在
  */
int SCL_VarFree(const char *name);

/**
  * @brief  释放全部变量
  * @retval 释放个数
  */
int SCL_VarFreeAll(void);

/**
  * @brief  查询剩余空变量槽数
  * @retval 空槽数量
  */
int SCL_VarFreeCount(void);

/**
  * @brief  查询已用变量槽数
  * @retval 已用数量
  */
int SCL_VarCount(void);

#ifdef __cplusplus
}
#endif

#endif /* __SCL_H__ */
