/**
  ******************************************************************************
  * @file    scl_var.c
  * @brief   SCL 会话变量表与管理（从 scl.c 拆分，多模块之一）
  ******************************************************************************
  */
#include "scl_priv.h"

/* ---- 会话变量表（本模块持有；core/env 经 scl_priv.h 只读访问） ---- */
scl_var_t s_vars[SCL_CFG_VAR_MAX];

/* ========================== 变量实现（v0.2 类型化） ========================== */

static int Scl_VarFind(const char *name)
{
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if ((s_vars[i].used != 0u) && Scl_StrEq(s_vars[i].name, name))
        {
            return i;
        }
    }
    return -1;
}

/* 读路径查找：先会话变量、后环境变量（ENV_EN）。返回表项指针或 NULL。
   会话变量优先，因此脚本 'var' 可临时屏蔽同名 env；写路径仍只走会话 Scl_VarFind */
static scl_var_t *Scl_VarFindAny(const char *name)
{
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if ((s_vars[i].used != 0u) && Scl_StrEq(s_vars[i].name, name))
        {
            return &s_vars[i];
        }
    }
#if (SCL_CFG_ENV_EN != 0u)
    for (i = 0; i < (int)SCL_CFG_ENV_MAX; i++)
    {
        if ((s_env[i].used != 0u) && Scl_StrEq(s_env[i].name, name))
        {
            return &s_env[i];
        }
    }
#endif
    return NULL;
}

int Scl_VarNameOk(const char *name)
{
    uint16_t n = 0u;
    if ((name[0] == '\0') || (!((name[0] == '_') ||
        ((name[0] >= 'a') && (name[0] <= 'z')) ||
        ((name[0] >= 'A') && (name[0] <= 'Z')))))
    {
        return 0;
    }
    while (name[n] != '\0')
    {
        if (!Scl_IsNm(name[n]))
        {
            return 0;
        }
        n++;
    }
    return (n <= SCL_CFG_VAR_NAME_MAX) ? 1 : 0;
}

/* 按值推断类型：true/false→bool；-x 单字符→flag；十进制→int；其余→string */
static uint8_t Scl_InferType(const char *val)
{
    uint16_t len = Scl_StrLen(val);
    int32_t  iv;
    if (len == 0u) { return SCL_T_STR; }
    if (Scl_EqIN(val, "true", 4u) || Scl_EqIN(val, "false", 5u)) { return SCL_T_BOOL; }
    if ((len == 2u) && (val[0] == '-') && Scl_IsAl(val[1])) { return SCL_T_FLAG; }
    if (Scl_ParseI32Len(val, len, &iv) == 0) { return SCL_T_INT; }
    return SCL_T_STR;
}

/* 按类型校验并把值规范化为文本写入 out（cap 含 '\0'）。0=成功；非 0=值非法 */
int Scl_VarNorm(uint8_t type, const char *val, char *out, uint16_t cap)
{
    uint16_t len;
    uint16_t i;

    if (val == NULL) { val = ""; }
    len = Scl_StrLen(val);

    if (type == SCL_T_BOOL)
    {
        uint8_t on = 0u;
        if ((len == 4u && Scl_EqIN(val, "true", 4u)) || (len == 1u && val[0] == '1'))
        {
            on = 1u;
        }
        else if ((len == 5u && Scl_EqIN(val, "false", 5u)) || (len == 1u && val[0] == '0'))
        {
            on = 0u;
        }
        else
        {
            return -1;
        }
        if (on != 0u) { out[0] = 't'; out[1] = 'r'; out[2] = 'u'; out[3] = 'e'; out[4] = '\0'; }
        else          { out[0] = 'f'; out[1] = 'a'; out[2] = 'l'; out[3] = 's'; out[4] = 'e'; out[5] = '\0'; }
        return 0;
    }
    if (type == SCL_T_INT)
    {
        int32_t iv;
        if (Scl_ParseI32Len(val, len, &iv) != 0) { return -1; }
        if (Scl_FmtI32(iv, out, cap) == 0u) { return -1; }
        return 0;
    }
    if (type == SCL_T_FLAG)
    {
        if ((len != 2u) || (val[0] != '-') || !Scl_IsAl(val[1])) { return -1; }
        out[0] = '-'; out[1] = val[1]; out[2] = '\0';
        return 0;
    }
    /* SCL_T_STR */
    if (len >= cap) { return -1; }
    for (i = 0u; i < len; i++) { out[i] = val[i]; }
    out[len] = '\0';
    return 0;
}

/* 类型化写入核心（脚本 'var' 与 C 命令共用） */
static int Scl_VarSetCore(const char *name, uint8_t type, const char *val)
{
    char norm[SCL_CFG_VAR_VALUE_MAX];
    int  idx;
    int  r;

    if ((name == NULL) || (name[0] == '\0'))
    {
        return -4;
    }
    if (!Scl_VarNameOk(name))
    {
        return -2;
    }
    r = Scl_VarNorm(type, val, norm, (uint16_t)sizeof(norm));
    if (r != 0)
    {
        return -3;   /* 值非法/过长 */
    }
    idx = Scl_VarFind(name);
    if (idx < 0)
    {
        uint16_t i;
        for (idx = 0; idx < (int)SCL_CFG_VAR_MAX; idx++)
        {
            if (s_vars[idx].used == 0u)
            {
                break;
            }
        }
        if (idx >= (int)SCL_CFG_VAR_MAX)
        {
            return -1;   /* 满 */
        }
        s_vars[idx].used = 1u;
        for (i = 0u; name[i] != '\0'; i++)
        {
            s_vars[idx].name[i] = name[i];
        }
        s_vars[idx].name[i] = '\0';
    }
    s_vars[idx].type = type;
    {
        uint16_t i;
        for (i = 0u; norm[i] != '\0'; i++)
        {
            s_vars[idx].value[i] = norm[i];
        }
        s_vars[idx].value[i] = '\0';
    }
    return 0;
}

/* 自动推断类型设置（C 命令便捷用） */
int SCL_VarSet(const char *name, const char *val)
{
    if (val == NULL) { val = ""; }
    return Scl_VarSetCore(name, Scl_InferType(val), val);
}

/* 显式类型设置（v0.2；'var' 脚本路径） */
int SCL_VarSetT(const char *name, uint8_t type, const char *val)
{
    return Scl_VarSetCore(name, type, val);
}

uint8_t SCL_VarType(const char *name)
{
    scl_var_t *e = Scl_VarFindAny(name);
    return (e == NULL) ? 0u : e->type;
}

const char *SCL_VarGet(const char *name)
{
    scl_var_t *e = Scl_VarFindAny(name);
    return (e == NULL) ? NULL : e->value;
}

int SCL_VarFree(const char *name)
{
    int idx = Scl_VarFind(name);
    if (idx < 0)
    {
        return -1;
    }
    s_vars[idx].used = 0u;
    s_vars[idx].type = 0u;
    s_vars[idx].name[0] = '\0';
    s_vars[idx].value[0] = '\0';
    return 0;
}

int SCL_VarFreeAll(void)
{
    int n = 0;
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u)
        {
            s_vars[i].used = 0u;
            s_vars[i].type = 0u;
            s_vars[i].name[0] = '\0';
            s_vars[i].value[0] = '\0';
            n++;
        }
    }
    return n;
}

int SCL_VarCount(void)
{
    int n = 0;
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u)
        {
            n++;
        }
    }
    return n;
}

int SCL_VarFreeCount(void)
{
    return (int)SCL_CFG_VAR_MAX - SCL_VarCount();
}

/* 会话变量保留：keep!=0 时脚本结束不自动释放变量（供交互 shell/长会话使用）。
   返回旧值；默认 0（一次脚本跑完自动释放全部变量） */
int SCL_VarKeep(int keep)
{
    int old = (s_keep_vars != 0u) ? 1 : 0;
    s_keep_vars = (keep != 0) ? 1u : 0u;
    return old;
}

/* 按索引遍历已用变量名（idx 从 0 起）。成功 0 并写 name；越界/失败 -1 */
int SCL_VarEnum(int idx, char *name, int cap)
{
    int n = 0;
    int i;
    if ((name == NULL) || (cap <= 0) || (idx < 0))
    {
        return -1;
    }
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u)
        {
            if (n == idx)
            {
                uint16_t k;
                uint16_t nl = Scl_StrLen(s_vars[i].name);
                if ((uint16_t)(cap - 1) < nl) { return -1; }
                for (k = 0u; k < nl; k++) { name[k] = s_vars[i].name[k]; }
                name[nl] = '\0';
                return 0;
            }
            n++;
        }
    }
    return -1;
}


/* 变量数值化（供运算/条件指令）：
   int→值；bool→1/0；flag→已定义即 1；string→整段解析失败按 0（ok=0） */
int32_t Scl_VarNum(const char *name, uint8_t *ok)
{
    scl_var_t *e = Scl_VarFindAny(name);
    int32_t iv = 0;
    if (e == NULL)
    {
        if (ok != NULL) { *ok = 0u; }
        return 0;
    }
    switch (e->type)
    {
    case SCL_T_INT:
        if (Scl_ParseI32Len(e->value, Scl_StrLen(e->value), &iv) != 0)
        {
            if (ok != NULL) { *ok = 0u; }
            return 0;
        }
        break;
    case SCL_T_BOOL:
        iv = (Scl_EqIN(e->value, "true", 4u)) ? 1 : 0;
        break;
    case SCL_T_FLAG:
        iv = (e->value[0] != '\0') ? 1 : 0;
        break;
    default: /* STR：尝试整段数字 */
        if (Scl_ParseI32Len(e->value, Scl_StrLen(e->value), &iv) != 0)
        {
            if (ok != NULL) { *ok = 0u; }
            return 0;
        }
        break;
    }
    if (ok != NULL) { *ok = 1u; }
    return iv;
}

/* 变量真值（供 btest/布尔运算）：
   int→非 0；bool→true；flag→已定义；string→非空且非 false/0 */
uint8_t Scl_VarTruth(const char *name)
{
    scl_var_t *e = Scl_VarFindAny(name);
    uint8_t len;
    if (e == NULL)
    {
        return 0u;
    }
    switch (e->type)
    {
    case SCL_T_INT:
        return (e->value[0] == '-') ? 1u :
               ((e->value[0] == '0') && (e->value[1] == '\0')) ? 0u : 1u;
    case SCL_T_BOOL:
        return (Scl_EqIN(e->value, "true", 4u)) ? 1u : 0u;
    case SCL_T_FLAG:
        return (e->value[0] != '\0') ? 1u : 0u;
    default:
        len = (uint8_t)Scl_StrLen(e->value);
        if ((len == 0u) || Scl_EqIN(e->value, "false", 5u) ||
            ((len == 1u) && (e->value[0] == '0')))
        {
            return 0u;
        }
        return 1u;
    }
}
