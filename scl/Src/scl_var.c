/**
  ******************************************************************************
  * @file    scl_var.c
  * @brief   SCL 会话变量表与管理（从 scl.c 拆分，多模块之一）
  ******************************************************************************
  */
#include "scl_priv.h"

/* ---- 会话变量表（本模块持有；core/env 经 scl_priv.h 只读访问） ---- */
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
scl_var_t *s_vars = NULL;
#else
scl_var_t s_vars[SCL_CFG_VAR_MAX];
#endif

uint8_t Scl_VarInit(void)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    uint16_t i;
    s_vars = (scl_var_t *)Scl_MemAlloc(sizeof(scl_var_t) * SCL_CFG_VAR_MAX);
    if (s_vars == NULL) { return 0u; }
    for (i = 0u; i < SCL_CFG_VAR_MAX; i++)
    {
        s_vars[i].name = NULL;
        s_vars[i].value = NULL;
        s_vars[i].name_cap = 0u;
        s_vars[i].value_cap = 0u;
        s_vars[i].used = 0u;
        s_vars[i].ro = 0u;
        s_vars[i].type = 0u;
    }
#else
    SCL_VarFreeAll();
#endif
    return 1u;
}

void Scl_VarShutdown(void)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    uint16_t i;
    if (s_vars == NULL) { return; }
    for (i = 0u; i < SCL_CFG_VAR_MAX; i++)
    {
        Scl_MemFree(s_vars[i].name);
        Scl_MemFree(s_vars[i].value);
    }
    Scl_MemFree(s_vars);
    s_vars = NULL;
#endif
}

static void Scl_VarClearSlot(scl_var_t *slot)
{
    if (slot == NULL) { return; }
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    Scl_MemFree(slot->name);
    Scl_MemFree(slot->value);
    slot->name = NULL;
    slot->value = NULL;
    slot->name_cap = 0u;
    slot->value_cap = 0u;
#else
    slot->name[0] = '\0';
    slot->value[0] = '\0';
#endif
    slot->used = 0u;
    slot->type = 0u;
    slot->ro = 0u;
}

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

/* 类型化写入核心（脚本 'var' 与 C 命令共用）。ro=1 建立只读常量：
   已有常量不可被任何覆盖（返回 -5）；普通变量可被覆盖为常量或普通。 */
static int Scl_VarSetCoreEx(const char *name, uint8_t type, const char *val, uint8_t ro)
{
    char norm[SCL_CFG_VAR_VALUE_MAX];
    int  idx;
    int  r;
    uint16_t i;

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
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
        s_vars[idx].name = (char *)Scl_MemAlloc((size_t)Scl_StrLen(name) + 1u);
        s_vars[idx].value = (char *)Scl_MemAlloc((size_t)Scl_StrLen(norm) + 1u);
        if ((s_vars[idx].name == NULL) || (s_vars[idx].value == NULL))
        {
            Scl_VarClearSlot(&s_vars[idx]);
            return -1;
        }
        s_vars[idx].name_cap = (uint16_t)(Scl_StrLen(name) + 1u);
        s_vars[idx].value_cap = (uint16_t)(Scl_StrLen(norm) + 1u);
#endif
        s_vars[idx].used = 1u;
        s_vars[idx].ro   = ro;
        for (i = 0u; name[i] != '\0'; i++)
        {
            s_vars[idx].name[i] = name[i];
        }
        s_vars[idx].name[i] = '\0';
    }
    else if (s_vars[idx].ro != 0u)
    {
        return -5;   /* 只读常量不可覆盖 */
    }
    else
    {
        s_vars[idx].ro = ro;   /* 普通变量可升级为 const */
    }
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    if (idx >= 0)
    {
        uint16_t need = (uint16_t)(Scl_StrLen(norm) + 1u);
        char *nv = (char *)Scl_MemRealloc(s_vars[idx].value, need);
        if (nv == NULL) { return -1; }
        s_vars[idx].value = nv;
        s_vars[idx].value_cap = need;
    }
#endif
    s_vars[idx].type = type;
    for (i = 0u; norm[i] != '\0'; i++)
    {
        s_vars[idx].value[i] = norm[i];
    }
    s_vars[idx].value[i] = '\0';
    return 0;
}

/* 普通写入（可变变量） */
static int Scl_VarSetCore(const char *name, uint8_t type, const char *val)
{
    return Scl_VarSetCoreEx(name, type, val, 0u);
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

/* 显式类型建立只读常量（一经建立不可覆盖/释放，生命周期同会话变量） */
int SCL_VarSetConst(const char *name, uint8_t type, const char *val)
{
    return Scl_VarSetCoreEx(name, type, val, 1u);
}

/* 查询是否为只读常量（会话变量表） */
int SCL_VarIsConst(const char *name)
{
    int idx = Scl_VarFind(name);
    return ((idx >= 0) && (s_vars[idx].ro != 0u)) ? 1 : 0;
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
    if (s_vars[idx].ro != 0u)
    {
        return -2;   /* 只读常量不可释放 */
    }
    Scl_VarClearSlot(&s_vars[idx]);
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
            Scl_VarClearSlot(&s_vars[i]);
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

uint8_t Scl_VarGc(void)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used != 0u && s_vars[i].value != NULL)
        {
            uint16_t need = (uint16_t)(Scl_StrLen(s_vars[i].value) + 1u);
            char *nv = (char *)Scl_MemRealloc(s_vars[i].value, need);
            if (nv != NULL)
            {
                s_vars[i].value = nv;
                s_vars[i].value_cap = need;
            }
        }
    }
#endif
    return 1u;
}

uint8_t Scl_VarGcZombie(void)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    int i;
    for (i = 0; i < (int)SCL_CFG_VAR_MAX; i++)
    {
        if (s_vars[i].used == 0u)
        {
            Scl_VarClearSlot(&s_vars[i]);
        }
    }
#endif
    return 1u;
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
