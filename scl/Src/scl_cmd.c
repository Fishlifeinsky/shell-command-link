/**
  ******************************************************************************
  * @file    scl_cmd.c
  * @brief   SCL 命令注册表、编程式调用（mini / 宿主直调）与"一行 → 命令"解析
  *
  *          由 scl.c 拆出（模块化）：
  *            - SCL_RegisterCmd / SCL_CmdHead：注册表链表维护
  *              （opcode 分配规则：注册表按下标，手工注册走保留区，见 scl.c 注释）
  *            - Scl_CmdFindName：按名查找（按 opcode 查找只在 scl.c 内部用）
  *
  *          （${} 展开 Scl_ExpandCopy 也只被 scl.c 用，故留在那里）
  *            - SCL_CmdInvoke / SCL_AsyncBusy / SCL_AsyncPoll：编程式调用与异步等待
  *            - SCL_RunLine：一行文本（空白分隔、支持引号）→ 类型化 argv → 调用
  *
  *          实现与迁移前逐行一致（含参数类型推断与错误提示原文）。
  ******************************************************************************
  */

#include "scl.h"
#include "scl_priv.h"

/* ========================== 命令注册 ==========================
   opcode 由**注册表（list 下标）**决定：scl/cmd/scl_cmd_list.c 在注册前按顺序
   写 `nd->opc = SCL_OP_CMD_BASE + i`；本函数**不覆盖已分配的 opcode**。
   手工注册（opc==0）时才从"保留区之后"继续分配，避免与注册表区间冲突。 */

void SCL_RegisterCmd(scl_cmd_t *cmd)
{
    scl_cmd_t **pp;

    if (cmd == NULL)
    {
        return;
    }
    if (cmd->opc == 0u)                  /* 0=未分配（手工注册路径） */
    {
        cmd->opc  = s_next_opc;
        s_next_opc = (uint16_t)(s_next_opc + 1u);
    }
    cmd->next = NULL;
    pp = &s_cmd_head;
    while (*pp != NULL)
    {
        pp = &((*pp)->next);
    }
    *pp = cmd;
}

/* 只读：返回命令链表头（供遍历/补全/调试） */
const scl_cmd_t *SCL_CmdHead(void)
{
    return s_cmd_head;
}

int SCL_ArgType(int idx)
{
    if ((idx < 0) || (idx >= (int)SCL_CFG_ARG_MAX))
    {
        return 0;
    }
    return (int)s_argt[idx];
}

/* ========================= 命令查找（链表） ========================= */

scl_cmd_t *Scl_CmdFindName(const char *name, uint16_t len)
{
    scl_cmd_t *node;
    for (node = s_cmd_head; node != NULL; node = node->next)
    {
        uint16_t nl = Scl_StrLen(node->name);
        if ((nl == len) && Scl_EqN(node->name, name, len))
        {
            return node;
        }
    }
    return NULL;
}

/* 按 opcode 查找：仅解释执行/预编译程序路径需要（mini 态不编，省 Flash） */
/* Scl_CmdFindOp（按 opcode 查找）与 Scl_ExpandCopy（${} 展开）只被 scl.c 的
   编译链/执行路径使用，故留在 scl.c 内部作为 static，未用档位可被整段消除。 */

/* ============================ 命令编程式调用（mini / 宿主直调） ============================ */

uint8_t SCL_CmdInvoke(const char *name, int argc, const scl_invoke_arg_t *argv)
{
    scl_cmd_t *nd;
    uint16_t nl;
    int ai;

    if (name == NULL)
    {
        return 0u;
    }
    nl = Scl_StrLen(name);
    if (nl == 0u)
    {
        return 0u;
    }
    if (argc < 0)
    {
        return 0u;
    }
    if (argc > (int)SCL_CFG_ARG_MAX)
    {
        Scl_MsgErr("命令 '%s': 参数过多(>%d)", name, (int)SCL_CFG_ARG_MAX);
        return 0u;
    }
    if ((argc > 0) && (argv == NULL))
    {
        return 0u;
    }

    nd = Scl_CmdFindName(name, nl);
    if (nd == NULL)
    {
        Scl_MsgErr("未知命令 '%s'", name);
        return 0u;
    }

    /* 拷入工作缓冲并记录各参数类型（同解释器 Scl_ArgRestore 后的状态） */
    for (ai = 0; ai < argc; ai++)
    {
        const char *tx = argv[ai].text;
        uint16_t len;
        uint16_t k;
        if (tx == NULL) { tx = ""; }
        len = Scl_StrLen(tx);
        if (len >= SCL_CFG_ARG_LEN_MAX)
        {
            len = (uint16_t)(SCL_CFG_ARG_LEN_MAX - 1u);
        }
        for (k = 0u; k < len; k++) { s_argb[ai][k] = tx[k]; }
        s_argb[ai][len] = '\0';
        s_argt[ai] = (uint8_t)((argv[ai].type == 0u) ? SCL_T_STR : argv[ai].type);
        s_argv[ai] = s_argb[ai];
    }

#if (SCL_CFG_CMDDESC_EN != 0u)
    if (Scl_DescCheck(nd, argc) != 0)
    {
        return 0u;   /* 模板校验拒绝（已打印 usage/错误） */
    }
#endif
    if (nd->sync != NULL)
    {
        s_wait_cmd = nd;   /* 异步：登记等待再发起（同解释器） */
    }
    nd->fn(argc, s_argv);
    return (nd->sync != NULL) ? 2u : 1u;
}

uint8_t SCL_AsyncBusy(void)
{
    return (s_wait_cmd != NULL) ? 1u : 0u;
}

int SCL_AsyncPoll(void)
{
    if (s_wait_cmd == NULL)
    {
        return -1;
    }
    if (s_wait_cmd->sync != NULL)
    {
        if (s_wait_cmd->sync(false))
        {
            s_wait_cmd->sync(true);
            s_wait_cmd = NULL;
            return 1;
        }
        return 0;
    }
    s_wait_cmd = NULL;
    return 1;
}

/* ============ 最简"解释器"：一行 → 命令名 + argc/argv → 按名执行 ============ */

uint8_t SCL_RunLine(const char *line)
{
    char tb[SCL_CFG_ARG_BUF_BYTES];   /* token 文本区（仅调用时占栈，避免常驻 RAM） */
    char nm[SCL_CMDNAME_MAX];
    scl_invoke_arg_t ia[SCL_CFG_ARG_MAX];
    int32_t iv;
    uint16_t nml;
    uint16_t tbi = 0u;
    int argc = 0;
    const char *p;

    if (line == NULL)
    {
        return 0u;
    }
    if (SCL_AsyncBusy() != 0u)
    {
        Scl_MsgErr("busy: 上一条异步命令未完成");
        return 0u;
    }
    p = line;
    while (Scl_IsSp(*p)) { p++; }
    if (*p == '\0') { return 0u; }

    /* 命令名（首个空白前；不支持引号命令名） */
    nml = 0u;
    while ((p[nml] != '\0') && !Scl_IsSp(p[nml]) && (nml + 1u < (uint16_t)sizeof(nm)))
    {
        nml++;
    }
    if (nml == 0u)
    {
        return 0u;
    }
    if ((p[nml] != '\0') && !Scl_IsSp(p[nml]))
    {
        Scl_MsgErr("命令名过长");
        return 0u;
    }
    {
        uint16_t k;
        for (k = 0u; k < nml; k++) { nm[k] = p[k]; }
        nm[nml] = '\0';
    }
    if (Scl_CmdFindName(nm, nml) == NULL)
    {
        Scl_MsgErr("未知命令 '%s'", nm);
        return 0u;
    }
    p += nml;

    /* 参数：空白分隔；引号内可有空白（整段 STR，类型不识别） */
    for (;;)
    {
        char q = 0;
        uint16_t start;
        uint16_t len;
        uint8_t ty;
        while (*p != '\0' && Scl_IsSp(*p)) { p++; }
        if (*p == '\0') { break; }
        if (argc >= (int)SCL_CFG_ARG_MAX)
        {
            Scl_MsgErr("参数过多(>%d)", (int)SCL_CFG_ARG_MAX);
            return 0u;
        }
        start = tbi;
        if ((*p == '"') || (*p == '\''))
        {
            q = *p;
            p++;
        }
        while (*p != '\0')
        {
            if (q != 0)
            {
                if (*p == q) { p++; break; }
            }
            else if (Scl_IsSp(*p))
            {
                break;
            }
            if (tbi + 1u < (uint16_t)sizeof(tb)) { tb[tbi++] = *p; }
            p++;
        }
        tb[tbi] = '\0';
        len = (uint16_t)(tbi - start);
        tbi++;   /* 越过 NUL，为下个 token 让位 */
        ty = SCL_T_STR;
        if ((q == 0) && (len == 4u) && Scl_EqIN(&tb[start], "true", 4u)) { ty = SCL_T_BOOL; }
        else if ((q == 0) && (len == 5u) && Scl_EqIN(&tb[start], "false", 5u)) { ty = SCL_T_BOOL; }
        else if ((q == 0) && (len == 2u) && (tb[start] == '-') && Scl_IsAl(tb[start + 1u]))
        {
            ty = SCL_T_FLAG;
        }
        else if ((q == 0) && (len > 0u) && (Scl_ParseI32Len(&tb[start], len, &iv) == 0))
        {
            ty = SCL_T_INT;
        }
        ia[argc].text = &tb[start];
        ia[argc].type = ty;
        argc++;
    }

    return SCL_CmdInvoke(nm, argc, (argc > 0) ? ia : NULL);
}

/* ${} 展开拷贝（Scl_ExpandCopy）保留在 scl.c 内部（static），见文件头说明。 */
