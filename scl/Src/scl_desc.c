/**
  ******************************************************************************
  * @file    scl_desc.c
  * @brief   SCL 命令描述层：类型名、usage 打印、参数模板校验、描述注册
  *
  *          由 scl.c 拆出（模块化）。**help 文本输出仍在 scl.c**：
  *          它只被解释器（Scl_StepOnce）使用，留在原处可继续享受
  *          "static 且未被引用即整段消除"（mini 态不付体积）。
  *
  *          本文件内容：
  *            - Scl_TypeName：类型 → 名字（scl.c 的变量命令与 help 也用）
  *            - Scl_DescPrintUsage：usage 行（help 全览/明细与校验失败都用）
  *            - Scl_DescArgOk / Scl_DescCheck：按模板校验命令参数
  *            - SCL_CmdRegisterDesc：按描述注册命令（注册表生成物调用）
  ******************************************************************************
  */

#include "scl.h"
#include "scl_priv.h"

/* ========================== 类型名 ↔ type ========================== */

const char *Scl_TypeName(uint8_t t)
{
    switch (t)
    {
    case SCL_T_BOOL: return "bool";
    case SCL_T_INT:  return "int";
    case SCL_T_FLAG: return "flag";
    default:         return "string";
    }
}

#if (SCL_CFG_CMDDESC_EN != 0u)

/* ========================== 命令描述注册辅助（argtable3 风格） ========================== */

/* 打印 usage 行（<必选:类型> [可选:类型] ...）；help（在 scl.c）也要用 → 外部链接 */
void Scl_DescPrintUsage(const scl_cmd_t *nd)
{
    const scl_cmd_desc_t *d = nd->desc;
    int i;
    Scl_Msg("usage: %s", nd->name);
    if ((d != NULL) && (d->args != NULL))
    {
        for (i = 0; i < d->arg_cnt; i++)
        {
            const scl_arg_spec_t *a = &d->args[i];
            if (a->opt != 0u) { Scl_Msg(" ["); }
            else              { Scl_Msg(" <"); }
            Scl_Msg("%s:%s", a->name, Scl_TypeName(a->type));
            if (a->opt != 0u) { Scl_Msg("]"); }
            else              { Scl_Msg(">"); }
        }
    }
    Scl_Msg("\r\n");
}

/* 单参数与模板匹配：0=通过。string 模板接受任意；int 模板接受 int 或可解析的文本；
   bool/flag 模板要求类型一致 */
static int Scl_DescArgOk(const scl_arg_spec_t *a, uint8_t have, const char *text)
{
    if (a->type == SCL_T_STR) { return 0; }
    if (a->type == SCL_T_INT)
    {
        int32_t v;
        if (have == SCL_T_INT) { return 0; }
        if ((have == SCL_T_STR) &&
            (Scl_ParseI32Len(text, Scl_StrLen(text), &v) == 0))
        {
            return 0;
        }
        return 1;
    }
    return (have == a->type) ? 0 : 1;
}

/* 按模板校验命令参数（s_argt/s_argv 为当前已还原参数）。
   返回 0=通过；负=拒绝（已打印提示）。
   被 SCL_CmdInvoke（scl_cmd.c）与解释器（scl.c）共用，故为外部链接。 */
int Scl_DescCheck(const scl_cmd_t *nd, int argc)
{
    const scl_cmd_desc_t *d = nd->desc;
    int minreq = 0;
    int i;

    if ((d == NULL) || (d->args == NULL))
    {
        return 0;   /* 无模板：不限制 */
    }
    for (i = 0; i < d->arg_cnt; i++)
    {
        if (d->args[i].opt == 0u) { minreq++; }
    }
    if (argc < minreq)
    {
        Scl_MsgErr("命令 '%s': 缺少参数（至少 %d 个）", nd->name, minreq);
        Scl_DescPrintUsage(nd);
        return -1;
    }
    if (argc > d->arg_cnt)
    {
        Scl_MsgErr("命令 '%s': 参数过多（最多 %d 个）", nd->name, d->arg_cnt);
        Scl_DescPrintUsage(nd);
        return -2;
    }
    for (i = 0; i < argc; i++)
    {
        if (Scl_DescArgOk(&d->args[i], s_argt[i], s_argv[i]) != 0)
        {
            Scl_MsgErr("命令 '%s': 参数 %d '%s' 期望 %s",
                       nd->name, i + 1, s_argv[i],
                       Scl_TypeName(d->args[i].type));
            Scl_DescPrintUsage(nd);
            return -3;
        }
    }
    return 0;
}

/* 按描述注册命令（填节点 name/fn/sync/desc 后挂链）；注册表生成物调用 */
void SCL_CmdRegisterDesc(scl_cmd_t *node, const scl_cmd_desc_t *desc)
{
    if ((node == NULL) || (desc == NULL))
    {
        return;
    }
    node->name = desc->name;
    node->fn   = desc->fn;
    node->sync = desc->sync;
    node->desc = desc;
    SCL_RegisterCmd(node);
}
#endif /* SCL_CFG_CMDDESC_EN */
