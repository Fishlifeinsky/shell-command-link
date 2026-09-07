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
  *                opc(2B) + argOff(2B)），然后主循环解释执行；
  *                参数在入缓存时**同步解析成类型块**（type 开头，无空格间隔）：
  *                  BOOL=0x01+v(1) | INT=0x02+4B 大端 | FLAG=0x03+c(1) | STR=0x04+len(1)+bytes
  *                元指令（var/free/help/label/jump）参数按整段 STR 块存原文内部解析；
  *                业务命令参数按字面量类型化；label 表登记跳转点
  *            - 命令在 SCL_RegisterCmd 时自动分配 opcode
  *            - 变量：类型化声明 'var <type> <name>=<value>'，type ∈ bool/int/flag/string
  *              （bool→true/false；int→十进制；flag→-x；string→原文）；${name} 取值、
  *              'free' 释放、脚本结束自动释放全部变量；数量/名长/值长由 scl_cfg.h 裁剪
  *            - 内置 int/bool 运算指令（参考 C，保留关键字）：
  *                int 算术: iadd isub imul idiv imod ineg    —— op a b dst（结果写回变量）
  *                int 比较: ieq ine igt ige ilt ile          —— op a b（结果进 G_RETURN）
  *                bool    : band bor bnot                    —— op a [b]（结果进 G_RETURN）
  *                btest 变量真值装载（bool/int≠0/flag 已定义） → G_RETURN
  *            - 条件标志 G_RETURN：默认 false，脚本开始/结束清零；被 jump -a 读取后自动清零；
  *              C 命令用 SCL_Ret_Set() 写入
  *            - 执行模型：异步/跨主循环步进。SCL_Run() 编译后立即返回并置 busy；
  *              主循环周期调 SCL_Loop() 逐条解释执行；命令可带"同步信号回调"
  *              （handler 启动异步操作后立即返回，库每 Loop 轮询 sync(false)，
  *              完成后再 sync(true) 清除并执行下一条）
  *            - 全程静态内存、无 malloc；无 OS / HAL / libc 依赖
  *
  *          保留关键字（不能注册为业务命令）：var / free / help / label / jump
  *            以及内置运算指令 iadd/isub/imul/idiv/imod/ineg/ieq/ine/igt/ige/ilt/ile/
  *            band/bor/bnot/btest（if / while 已由上层编译器降级，运行时不再提供）
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

/* ============================ 类型常量 ============================ */

/* 参数/变量类型（参数字节块 type 首字节，亦作变量 type 字段） */
enum
{
    SCL_T_BOOL = 0x01u,   /* bool ：块=type+值(1B 0/1)；变量存 true/false 文本 */
    SCL_T_INT  = 0x02u,   /* int  ：块=type+值(4B 大端)；变量存十进制文本 */
    SCL_T_FLAG = 0x03u,   /* flag ：块=type+字符(1B, 如 'x' 即 -x)；变量存 "-x" 文本 */
    SCL_T_STR  = 0x04u    /* string：块=type+len(1B)+bytes；变量存原文 */
};

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

/**
  * @brief  预编译只读脚本程序（由上层工具把脚本编译为 const 数组后静态定义）
  * @note   数据放在只读存储区（Flash/ROM），执行时 SCL_RunProg() 直接解释，
  *         不占用 RAM 字节码/参数缓存/label 表 —— 脚本固定不变时最省 RAM 的用法。
  *         bc   ：字节码，每条指令 4 字节 = opc(2B 大端) + argOff(2B 大端)；
  *                注册命令调用使用"按名调用"指令（命令名存于参数区，无需匹配
  *                运行时注册顺序，见 doc）；
  *         argc ：参数字节缓存（type 块序列，argOff==0 表示无参，缓存第 0 字节为哨兵）
  * @see    tools/ 下生成器 / doc/arc/scl-const-prog.md
  */
typedef struct scl_prog
{
    const uint8_t *bc;       /* 字节码（Flash） */
    uint16_t       bc_len;   /* 字节码字节数（4 的倍数） */
    const uint8_t *argc;     /* 参数字节缓存（Flash） */
    uint16_t       arg_len;  /* 参数字节缓存字节数 */
} scl_prog_t;

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
  * @note   一次只运行一个脚本；完成后变量自动全部释放、G_RETURN 清零。
  *         仅当 SCL_CFG_RUN_TEXT_EN=1 时可用（运行时编译，占 RAM）
  */
#if (SCL_CFG_RUN_TEXT_EN != 0u)
uint8_t SCL_Run(const char *script);
#endif

#if (SCL_CFG_RUN_PROG_EN != 0u)
/**
  * @brief  开始执行预编译只读程序（const 数据，放 Flash）
  * @param  prog 程序描述；bc/argc 必须非空且为合法预编译产物
  * @retval 1=已接受并置忙；0=忙中拒绝 / 参数非法
  * @note   语义与 SCL_Run 相同（变量/G_RETURN/异步命令/步进保护）；
  *         须在命令注册完成后再启动。运行期间不占用字节码/参数缓存 RAM
  */
uint8_t SCL_RunProg(const scl_prog_t *prog);
#endif

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

/* ============================ 参数类型查询（命令调用期间） ============================ */

/**
  * @brief  查询当前正在调用的命令第 idx 个参数的类型（供 C 命令区分 bool/int/flag/string）
  * @param  idx 参数序号（0 起）
  * @retval SCL_T_BOOL/SCL_T_INT/SCL_T_FLAG/SCL_T_STR；idx 越界或非命令上下文返回 0
  * @note   仅在命令处理函数被调用期间有效（同 argv）
  */
int SCL_ArgType(int idx);

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
  * @brief  取变量值（文本形态：bool→true/false；int→十进制；flag→"-x"；string→原文）
  * @param  name 变量名
  * @retval 值字符串指针（库内静态存储，脚本结束/释放后失效）；不存在返回 NULL
  */
const char *SCL_VarGet(const char *name);

/**
  * @brief  查询变量类型
  * @param  name 变量名
  * @retval SCL_T_BOOL/INT/FLAG/STR；不存在返回 0
  */
uint8_t SCL_VarType(const char *name);

/**
  * @brief  设置/覆盖变量（供 C 命令使用，类型由值自动推断：
  *         true/false→bool；-x 单字符→flag；十进制→int；其余→string）
  * @param  name 变量名（[A-Za-z_][A-Za-z0-9_]*，长度 <= SCL_CFG_VAR_NAME_MAX）
  * @param  val  值
  * @retval 0=成功；-1=变量已满无空槽；-2=变量名非法/过长；-3=值过长/非法；-4=变量名为空
  */
int SCL_VarSet(const char *name, const char *val);

/**
  * @brief  显式类型设置/覆盖变量（供脚本 'var' 与 C 命令共用，类型明确）
  * @param  name 变量名（同 SCL_VarSet）
  * @param  type SCL_T_BOOL/SCL_T_INT/SCL_T_FLAG/SCL_T_STR
  * @param  val  值（按 type 校验并规范化：bool 接受 true/false/1/0；int 接受十进制；
  *              flag 接受 "-x"；string 接受任意文本）
  * @retval 0=成功；-1=变量已满无空槽；-2=变量名非法/过长；-3=值过长/非法；-4=变量名为空
  */
int SCL_VarSetT(const char *name, uint8_t type, const char *val);

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
