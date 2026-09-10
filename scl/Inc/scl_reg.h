/**
  ******************************************************************************
  * @file    scl_reg.h
  * @brief   命令 / 静态变量的"声明宏"——供 scl/tool/scl_gen_list.py 扫描收集，
  *          生成 scl/cmd/scl_cmd_list.c（命令数组 + 变量数组 + 注册入口）。
  *
  *          用法（每个命令一个 .c，放 scl/cmd/ 下）：
  *            static void Cmd_echo(int argc, char *argv[]) { ... }
  *            static const scl_arg_spec_t a_echo_txt[] = { { "text", SCL_T_STR, 0u, "文本" } };
  *            SCL_CMD_DEFINE(echo, Cmd_echo, NULL, "打印参数", a_echo_txt);
  *
  *          约定（生成器依赖，必须遵守）：
  *            - 第一个参数是**命令名**，必须是简单标识符（[A-Za-z_][A-Za-z0-9_]*）
  *            - 宏参数保持括号平衡；不要用宏拼出命令名
  ******************************************************************************
  */

#ifndef __SCL_REG_H__
#define __SCL_REG_H__

#include "scl.h"

/* 参数模板项数（_args 为数组名；无参数模板请用 *_NA 宏） */
#define SCL_ARGCNT(_args)   ((int)(sizeof(_args) / sizeof((_args)[0])))

/* ============================ 命令 ============================ */

#if (SCL_CFG_CMDDESC_EN != 0u)

/* 带参数模板：定义命令节点 + 命令描述（均由注册表引用，故为外部链接）
   desc 末字段 doc 置 NULL（多行手册可后续用 SCL_CmdRegisterDesc 手工补） */
#define SCL_CMD_DEFINE(_name, _fn, _sync, _help, _args)                    \
    scl_cmd_t s_cmd_##_name;                                               \
    const scl_cmd_desc_t s_desc_##_name = {                                \
        #_name, _help, _args, SCL_ARGCNT(_args), _fn, _sync, NULL }

/* 无参数模板（sizeof(NULL) 非法，故单独一个宏，项数写 0） */
#define SCL_CMD_DEFINE_NA(_name, _fn, _sync, _help)                        \
    scl_cmd_t s_cmd_##_name;                                               \
    const scl_cmd_desc_t s_desc_##_name = { #_name, _help, NULL, 0, _fn, _sync, NULL }

#else   /* CMDDESC 关闭：只定义节点，描述字段不存在 */

#define SCL_CMD_DEFINE(_name, _fn, _sync, _help, _args)                    \
    scl_cmd_t s_cmd_##_name

#define SCL_CMD_DEFINE_NA(_name, _fn, _sync, _help)                        \
    scl_cmd_t s_cmd_##_name

#endif

/* ============================ 静态变量 ============================ */

/**
  * @brief  定义一个 static 型脚本变量（mini 态唯一支持的变量来源）
  * @param  _name 脚本中的变量名（简单标识符）
  * @param  _type SCL_T_BOOL / SCL_T_INT / SCL_T_FLAG / SCL_T_STR
  * @param  _get  取值函数（返回文本；由命令文件实现）
  * @param  _set  写值函数（0=成功；负=拒绝）
  * @note   **不允许初始化**：初值由宿主用 SCL_VarSet/SCL_VarSetT 注入。
  *         若在宏里写初值，生成器会报错（每次 start 重置宿主值）。
  */
#define SCL_VAR_DEFINE(_name, _type, _get, _set)                           \
    const scl_var_bind_t s_bind_##_name = { #_name, _type, _get, _set }

#endif /* __SCL_REG_H__ */
