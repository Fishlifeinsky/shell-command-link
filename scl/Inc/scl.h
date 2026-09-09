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
  *            - 默认全程静态内存、无 malloc；可选动态模式使用应用 allocator
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
#include <stddef.h>   /* NULL（正规来源；严格工具链如 arm-none-eabi 下 stdio 不保证提供） */

/* 可选动态内存提供者；ctx 由应用持有，库不解释其内容。 */
typedef void *(*scl_alloc_fn)(void *ctx, size_t size);
typedef void *(*scl_realloc_fn)(void *ctx, void *ptr, size_t size);
typedef void (*scl_free_fn)(void *ctx, void *ptr);

typedef struct scl_allocator
{
  scl_alloc_fn   alloc;
  scl_realloc_fn realloc;
  scl_free_fn    free;
  void          *ctx;
} scl_allocator_t;

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
#if (SCL_CFG_CMDDESC_EN != 0u)
typedef struct scl_cmd_desc scl_cmd_desc_t;   /* 命令描述（见下，可含 help+参数模板） */
#endif
typedef struct scl_cmd
{
    const char          *name;   /* 命令名（不能为保留关键字） */
    scl_cmd_handler_t    fn;     /* 处理函数 */
    scl_sync_t           sync;   /* 同步信号回调（NULL=同步命令；非 NULL=异步需等待） */
    struct scl_cmd      *next;   /* 链表下一节点（由 SCL_RegisterCmd 维护） */
    uint16_t             opc;    /* 字节码 opcode（SCL_RegisterCmd 自动分配，勿手填） */
#if (SCL_CFG_CMDDESC_EN != 0u)
    const scl_cmd_desc_t *desc;  /* 命令描述（可选：NULL=无描述/无模板校验） */
#endif
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

#if ((SCL_CFG_SCMD_EN != 0u) && (SCL_CFG_RUN_PROG_EN != 0u))
/**
  * @brief  脚本命令：把 s2c 编译产物（const 程序）注册成命令行可直接调用的命令。
  * @note   调用形态：仅整行顶层（命令行输入 `name 参数...`，SCL 空闲时）执行；
  *         运行时把参数按位置注入为变量 arg0..argN（上限 SCL_CFG_VAR_MAX），
  *         再 SCL_RunProg() 跑该脚本；脚本里用 ${arg0}..（可 alias 起名）读取。
  *         生成器 tools/scl_emit_c.py --cmd <name> 会输出节点与注册函数。
  * @see    doc/arc/scl-scmd.md
  */
typedef struct scl_scmd
{
    const char       *name;   /* 命令名（命令行输入；不能为保留关键字） */
    const scl_prog_t *prog;   /* 预编译 const 程序（生成器产物） */
    struct scl_scmd  *next;   /* 链表（SCL_Scmd_Register 维护） */
} scl_scmd_t;

void SCL_Scmd_Register(scl_scmd_t *cmd);
const scl_scmd_t *SCL_Scmd_Find(const char *name);
const scl_scmd_t *SCL_Scmd_Head(void);
/**
  * @brief  执行一行 'name 参数...'（脚本命令入口）
  * @retval 1=已接受并置忙；0=busy 拒/参数非法；2=命令名不是脚本命令（可回退 SCL_Run）
  * @note   参数注入 arg0.. 后 SCL_RunProg；busy 时沿用拒绝语义
  */
uint8_t SCL_Scmd_RunText(const char *line);
#endif

/* ============================ 生命周期 ============================ */

/**
  * @brief  初始化库：清命令链表/变量/G_RETURN/执行状态
  * @note   未显式调用时，首次 SCL_Run 前会自动初始化一次
  */
void SCL_Init(void);

/**
  * @brief  使用应用提供的 allocator 初始化。
  * @param  allocator 动态模式必填；静态模式可传 NULL
  * @retval 1=成功；0=allocator 不完整或最小工作区分配失败
  */
uint8_t SCL_InitEx(const scl_allocator_t *allocator);

/** 动态/静态缓存统计（动态模式统计 allocator 实际用量，静态模式统计配置容量） */
typedef struct scl_cache_info
{
    size_t current;
    size_t peak;
    size_t capacity;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t gc_count;
    uint32_t zombie_count;
} scl_cache_info_t;

uint8_t SCL_CacheInfo(scl_cache_info_t *info);
uint8_t SCL_CacheGc(void);
uint8_t SCL_CacheGcZombie(void);

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

#if (SCL_CFG_CMDDESC_EN != 0u)

/* ============================ 命令描述注册辅助（argtable3 风格） ============================ */

/**
  * @brief  参数模板项（类 argtable3：声明命令参数的类型/可选性/帮助）
  * @note   脚本调用按位置匹配模板；模板 STR 接受任意参数（文本），
  *         INT 接受整型或可解析整数的文本，BOOL/FLAG 要求类型一致
  */
typedef struct scl_arg_spec
{
    const char *name;    /* 参数名（usage/help 显示） */
    uint8_t     type;    /* SCL_T_BOOL/INT/FLAG/STR */
    uint8_t     opt;     /* 0=必选；1=可选 */
    const char *help;    /* 说明（可 NULL） */
} scl_arg_spec_t;

/**
  * @brief  命令描述（声明式：名字 + 一行帮助 + 参数模板 + 可选多行详细说明）
  * @note   args=NULL     → 不限制参数、不校验（如 echo 变参 / drv flag 型命令）；
  *         args!=NULL 且 arg_cnt>0 → 按模板校验（缺必选/多给/类型不符即拒绝并打印 usage）；
  *         args!=NULL 且 arg_cnt==0 → 要求无参数；
  *         doc：多行详细说明（可含 \r\n），仅 help <cmd> 明细输出，help 全览与
  *         参数校验都不受影响（给 flag 型命令写手册、又不想启用校验时用）
  */
typedef struct scl_cmd_desc
{
    const char           *name;    /* 命令名 */
    const char           *help;    /* 一行帮助（help 命令显示，可 NULL） */
    const scl_arg_spec_t *args;    /* 参数模板数组（NULL=不限，不校验） */
    int                   arg_cnt; /* 模板条数 */
    scl_cmd_handler_t     fn;      /* 命令实现（校验通过后调用，参数在 argv） */
    scl_sync_t            sync;    /* 异步同步回调（NULL=同步） */
    const char           *doc;     /* v0.4a：多行详细说明（可含 \r\n，仅 help <cmd> 显示，NULL=无） */
} scl_cmd_desc_t;

/**
  * @brief  按描述注册命令（等价"填节点 + 自动校验 + help 关联"）
  * @param  node 命令节点（静态/全局存储，生命周期贯穿运行期）
  * @param  desc 命令描述（静态 const；注册后不得改动）
  * @note   校验失败：输出 usage + 错误消息，并中断当前脚本（等同运行错误）。
  *         内置 help 会自动列出带 help 文本的命令与模板概要
  */
void SCL_CmdRegisterDesc(scl_cmd_t *node, const scl_cmd_desc_t *desc);

#endif /* SCL_CFG_CMDDESC_EN */

/* ============================ 数字解析（命令实现内取值用，无 libc） ============================ */

/**
  * @brief  十进制/0x/0b 文本 → int32（供命令内把 argv 转数字，无需 atoi）
  * @param  s   文本（NULL 或非法 → 返回 def）
  * @param  def 解析失败返回值
  */
int32_t SCL_ParseInt(const char *s, int32_t def);

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
  * @retval 0=成功；-1=变量已满无空槽；-2=变量名非法/过长；-3=值过长/非法；-4=变量名为空；
  *         -5=同名已存在且为只读常量（const）
  */
int SCL_VarSetT(const char *name, uint8_t type, const char *val);

/**
  * @brief  建立只读常量（const）：声明后不可覆盖/free/作为写回目标
  * @param  name 变量名；type 同 SCL_VarSetT；val 值（校验/规范化）
  * @retval 同 SCL_VarSetT（-5=已是 const 常量不可再覆盖）
  * @note   生命周期与会话变量一致（脚本结束按 VarKeep 语义释放）
  */
int SCL_VarSetConst(const char *name, uint8_t type, const char *val);

/**
  * @brief  查询某变量是否为只读常量（const）
  * @param  name 变量名
  * @retval 1=是常量；0=否/不存在
  */
int SCL_VarIsConst(const char *name);

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

/**
  * @brief  会话变量保留开关：置 1 后脚本结束不再自动释放全部变量
  *         （供交互 shell / 长会话让 var 跨命令存活；仍可用 free 显式释放）
  * @param  keep 0=关闭（默认，一次性脚本跑完自动释放）；非 0=开启
  * @retval 旧值（便于恢复）
  */
int SCL_VarKeep(int keep);

/**
  * @brief  只读命令链表头（供遍历/补全/调试）
  * @retval 首节点指针；无命令返回 NULL
  * @note   链表由各命令模块静态定义 + SCL_RegisterCmd 挂入
  */
const scl_cmd_t *SCL_CmdHead(void);

#if (SCL_CFG_MINI_EN != 0u)

/* ============================ mini：外部静态变量绑定路由（SCL_CFG_MINI_EN） ============================ */

/**
  * @brief  绑定变量的文本 getter（取当前值文本；未定义返回 NULL）
  */
typedef const char *(*scl_var_get_t)(void);

/**
  * @brief  绑定变量的文本 setter（写类型化静态存储）
  * @param  text 规范文本（int→十进制；bool→true/false；flag→"-x"；string→原文）
  * @retval 0=成功；-5=只读(const)；-3=值非法
  */
typedef int (*scl_var_set_t)(const char *text);

/**
  * @brief  一个"外部绑定变量"：由 mini 生成代码把其 static 变量注册进 SCL，
  *         SCL_VarGet/SCL_VarSet(T) 等会路由到这里（先于会话/env）。
  * @note   不占会话槽、无生命周期管理 → 无需 free。name 需符合变量名规则
  */
typedef struct scl_var_bind
{
    const char   *name;   /* 变量名 */
    uint8_t       type;   /* SCL_T_BOOL/INT/FLAG/STR */
    scl_var_get_t get;    /* 取文本（未定义返回 NULL） */
    scl_var_set_t set;    /* 写类型化存储 */
} scl_var_bind_t;

/**
  * @brief  注册一批绑定变量（mini 生成代码在 <name>_mini_register() 里调用）
  * @param  tab 描述数组（表项为 const，表项生命周期需贯穿运行期）
  * @param  n   条数
  * @retval 成功注册条数（容量 SCL_CFG_VAR_BIND_MAX 满则截断）
  * @note   同名重复注册视为更新（覆盖旧绑定）
  */
int SCL_VarBind(const scl_var_bind_t *tab, int n);

/**
  * @brief  清空绑定路由表
  */
void SCL_VarBindClear(void);

#endif /* SCL_CFG_MINI_EN */

/* ============================ 命令编程式调用（供自包含生成代码 / 宿主直接调命令） ============================ */

/**
  * @brief  单个调用参数：文本 + 类型（库会拷进工作缓冲；text 只需在调用期间有效）
  * @note   type 取 SCL_T_BOOL/INT/FLAG/STR；传 0 视为 STR。
  *         int 用十进制文本；bool 用 true/false；flag 用 "-x"；string 用原文
  */
typedef struct scl_invoke_arg
{
    const char *text;   /* 参数文本 */
    uint8_t     type;   /* SCL_T_BOOL/INT/FLAG/STR（0=按 STR） */
} scl_invoke_arg_t;

/**
  * @brief  编程式按名调用一条注册命令（不经字节码/解释器）
  * @param  name  命令名（不能为保留关键字对应的元命令）
  * @param  argc  参数个数
  * @param  argv  参数数组（可为 NULL 当 argc==0）
  * @retval 0=命令不存在/参数非法(已打印)/desc 校验拒绝；1=已同步执行完；
  *         2=已发起异步命令（需周期调 SCL_AsyncPoll() 等待完成）
  * @note   供"自包含生成代码"（如把脚本编译成 switch 状态机的 mini 模式）与
  *         宿主 C 代码直接调命令使用，行为与解释器 CALLN 一致（含 desc 模板校验）。
  *         文本长度受 SCL_CFG_ARG_LEN_MAX 限制，超长会被截断/拒绝。
  */
uint8_t SCL_CmdInvoke(const char *name, int argc, const scl_invoke_arg_t *argv);

/**
  * @brief  是否有异步命令在等待完成（供生成代码轮询）
  * @retval 1=有；0=无
  */
uint8_t SCL_AsyncBusy(void);

/**
  * @brief  推进一次异步等待轮询
  * @retval 1=等待的命令已完成并清除（可继续下一步）；0=仍在进行；
  *         -1=当前无异步等待
  */
int SCL_AsyncPoll(void);

/**
  * @brief  按索引遍历已用变量名（供 shell 补全 / 调试）
  * @param  idx  序号（0 起）
  * @param  name 输出缓冲（含结尾 '\0'）
  * @param  cap  缓冲容量
  * @retval 0=找到；-1=越界/无更多/参数非法
  */
int SCL_VarEnum(int idx, char *name, int cap);

#if (SCL_CFG_ENV_EN != 0u)

/* ============================ 环境变量缓冲（持久配置） ============================ */

/**
  * @brief  默认配置项（用户 const 表；name 需同变量名规则）
  * @note   val 为规范文本：bool→true/false；int→十进制(可 0x/0b)；flag→"-x"；string→原文
  */
typedef struct scl_env_def
{
    const char *name;
    uint8_t     type;   /* SCL_T_BOOL/INT/FLAG/STR */
    const char *val;
} scl_env_def_t;

/**
  * @brief  注册默认配置表（const，可放 Flash）
  * @param  tab 默认配置数组；NULL 表示清除注册
  * @param  n   条数
  */
void Scl_Env_RegisterDefault(const scl_env_def_t *tab, int n);

/**
  * @brief  用默认配置表重建 env 缓冲（先清空再逐条装载）
  * @retval 装载成功条数；-1=无默认表（已清空）
  */
int Scl_Env_Reset(void);

/**
  * @brief  显式类型写入/覆盖一条 env（值按 type 校验并规范化）
  * @retval 0=成功；-1=槽满；-2=名非法/过长；-3=值非法/过长；-4=名为空
  */
int Scl_Env_Set(const char *name, uint8_t type, const char *val);

/**
  * @brief  序列化 env 缓冲到字节流（供用户固化到自有存储区）
  * @param  buf 目标缓冲
  * @param  cap 容量
  * @retval 写入字节数；负=空间不足
  * @note   格式：'S''C''L''E' + ver(1) + n(1) + 每项[type][nlen][name][vlen][val]
  */
int Scl_Env_Save(uint8_t *buf, int cap);

/**
  * @brief  从字节流装载 env 缓冲（用户从自有存储读回后调用）
  * @param  buf 序列化数据（须 Scl_Env_Save 生成）
  * @param  len 数据长度
  * @retval 0=成功；负=格式错/超容
  */
int Scl_Env_Load(const uint8_t *buf, int len);

/** @brief 清空 env 缓冲，返回释放条数 */
int Scl_Env_FreeAll(void);

/** @brief 已用 env 条数 */
int Scl_Env_Count(void);

/** @brief 按索引遍历 env 名（idx 0 起）。0=找到；-1=结束/非法 */
int Scl_Env_Enum(int idx, char *name, int cap);

#endif /* SCL_CFG_ENV_EN */

#ifdef __cplusplus
}
#endif

#endif /* __SCL_H__ */
