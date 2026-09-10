/* ===============================================================
 * SCL 注册表：scl_cmd_list.c（由 scl/tool/scl_gen_list.py 生成，勿手改）
 * 命令 6 条、静态变量 0 个
 *   cmd  demo_inc         <- scl/cmd/cmd_demo_inc.c:17
 *   cmd  demo_reset       <- scl/cmd/cmd_demo_reset.c:22
 *   cmd  echo             <- scl/cmd/cmd_echo.c:20
 *   cmd  noop             <- scl/cmd/cmd_noop.c:12
 *   cmd  setret           <- scl/cmd/cmd_setret.c:17
 *   cmd  wait             <- scl/cmd/cmd_wait.c:42
 * =============================================================== */
#include "scl.h"
#if (SCL_CFG_CMDDESC_EN != 0u)
extern const scl_cmd_desc_t s_desc_demo_inc;
extern const scl_cmd_desc_t s_desc_demo_reset;
extern const scl_cmd_desc_t s_desc_echo;
extern const scl_cmd_desc_t s_desc_noop;
extern const scl_cmd_desc_t s_desc_setret;
extern const scl_cmd_desc_t s_desc_wait;
#endif
extern scl_cmd_t s_cmd_demo_inc;
extern scl_cmd_t s_cmd_demo_reset;
extern scl_cmd_t s_cmd_echo;
extern scl_cmd_t s_cmd_noop;
extern scl_cmd_t s_cmd_setret;
extern scl_cmd_t s_cmd_wait;

scl_cmd_t * const scl_cmd_list[] = {
    &s_cmd_demo_inc,
    &s_cmd_demo_reset,
    &s_cmd_echo,
    &s_cmd_noop,
    &s_cmd_setret,
    &s_cmd_wait,
};
const int scl_cmd_list_n = 6;

const scl_var_bind_t * const * const scl_var_list = NULL;   /* 空表：NULL 指针 */
const int scl_var_list_n = 0;

#if (SCL_CFG_CMDDESC_EN != 0u)
static const scl_cmd_desc_t * const scl_desc_list[] = {
    &s_desc_demo_inc,
    &s_desc_demo_reset,
    &s_desc_echo,
    &s_desc_noop,
    &s_desc_setret,
    &s_desc_wait,
};
#endif

void SCL_RegList_Init(void)
{
    int i;
    for (i = 0; i < scl_cmd_list_n; i++)
    {
        scl_cmd_t *nd = scl_cmd_list[i];
        if (nd == NULL) { continue; }
#if (SCL_CFG_CMDDESC_EN != 0u)
        SCL_CmdRegisterDesc(nd, scl_desc_list[i]);
#else
        SCL_RegisterCmd(nd);
#endif
    }
    for (i = 0; i < scl_var_list_n; i++)
    {
        SCL_VarBindOne(scl_var_list[i]);
    }
}
