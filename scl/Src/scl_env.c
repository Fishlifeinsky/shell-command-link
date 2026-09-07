/** ... header ... */
#include "scl_priv.h"

#if (SCL_CFG_ENV_EN != 0u)
/* ---- env 缓冲与默认配置表（本模块持有；读回退经 scl_priv.h 共享） ---- */
scl_var_t s_env[SCL_CFG_ENV_MAX];
const scl_env_def_t *s_env_def = NULL;
uint16_t            s_env_def_n = 0u;

/* ========================== 环境变量缓冲（持久配置） ========================== */

static int Scl_EnvFind(const char *name)
{
    int i;
    for (i = 0; i < (int)SCL_CFG_ENV_MAX; i++)
    {
        if ((s_env[i].used != 0u) && Scl_StrEq(s_env[i].name, name))
        {
            return i;
        }
    }
    return -1;
}

/* 类型化写入一条 env（校验+规范化；覆盖已有；槽满/名错/值错返回负） */
static int Scl_EnvSetCore(const char *name, uint8_t type, const char *val)
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
        return -3;
    }
    idx = Scl_EnvFind(name);
    if (idx < 0)
    {
        for (idx = 0; idx < (int)SCL_CFG_ENV_MAX; idx++)
        {
            if (s_env[idx].used == 0u)
            {
                break;
            }
        }
        if (idx >= (int)SCL_CFG_ENV_MAX)
        {
            return -1;
        }
        s_env[idx].used = 1u;
        for (i = 0u; name[i] != '\0'; i++)
        {
            s_env[idx].name[i] = name[i];
        }
        s_env[idx].name[i] = '\0';
    }
    s_env[idx].type = type;
    for (i = 0u; norm[i] != '\0'; i++)
    {
        s_env[idx].value[i] = norm[i];
    }
    s_env[idx].value[i] = '\0';
    return 0;
}

void Scl_Env_RegisterDefault(const scl_env_def_t *tab, int n)
{
    s_env_def = tab;
    s_env_def_n = (uint16_t)((n < 0) ? 0 : n);
}

int Scl_Env_Reset(void)
{
    int i;
    int ok = 0;
    Scl_Env_FreeAll();
    if ((s_env_def == NULL) || (s_env_def_n == 0u))
    {
        return -1;
    }
    for (i = 0; i < (int)s_env_def_n; i++)
    {
        if (Scl_EnvSetCore(s_env_def[i].name, s_env_def[i].type,
                           s_env_def[i].val) == 0)
        {
            ok++;
        }
    }
    return ok;
}

int Scl_Env_Set(const char *name, uint8_t type, const char *val)
{
    return Scl_EnvSetCore(name, type, val);
}

int Scl_Env_FreeAll(void)
{
    int n = 0;
    int i;
    for (i = 0; i < (int)SCL_CFG_ENV_MAX; i++)
    {
        if (s_env[i].used != 0u)
        {
            s_env[i].used = 0u;
            s_env[i].type = 0u;
            s_env[i].name[0] = '\0';
            s_env[i].value[0] = '\0';
            n++;
        }
    }
    return n;
}

int Scl_Env_Count(void)
{
    int n = 0;
    int i;
    for (i = 0; i < (int)SCL_CFG_ENV_MAX; i++)
    {
        if (s_env[i].used != 0u)
        {
            n++;
        }
    }
    return n;
}

int Scl_Env_Enum(int idx, char *name, int cap)
{
    int n = 0;
    int i;
    if ((name == NULL) || (cap <= 0) || (idx < 0))
    {
        return -1;
    }
    for (i = 0; i < (int)SCL_CFG_ENV_MAX; i++)
    {
        if (s_env[i].used != 0u)
        {
            if (n == idx)
            {
                uint16_t k;
                uint16_t nl = Scl_StrLen(s_env[i].name);
                if ((uint16_t)(cap - 1) < nl) { return -1; }
                for (k = 0u; k < nl; k++) { name[k] = s_env[i].name[k]; }
                name[nl] = '\0';
                return 0;
            }
            n++;
        }
    }
    return -1;
}

/* 序列化 env 缓冲：'S''C''L''E' + ver(1) + n(1) + 每项[type][nlen][name][vlen][val]。
   返回字节数；负=空间不足 */
int Scl_Env_Save(uint8_t *buf, int cap)
{
    int o = 0;
    int i;
    if ((buf == NULL) || (cap < 6))
    {
        return -1;
    }
    buf[o++] = 'S';
    buf[o++] = 'C';
    buf[o++] = 'L';
    buf[o++] = 'E';
    buf[o++] = 1u;                    /* version */
    buf[o++] = (uint8_t)Scl_Env_Count();
    for (i = 0; i < (int)SCL_CFG_ENV_MAX; i++)
    {
        uint16_t nl;
        uint16_t vl;
        uint16_t k;
        if (s_env[i].used == 0u) { continue; }
        nl = Scl_StrLen(s_env[i].name);
        vl = Scl_StrLen(s_env[i].value);
        if (o + 2 + (int)nl + 1 + (int)vl > cap) { return -1; }
        buf[o++] = s_env[i].type;
        buf[o++] = (uint8_t)nl;
        for (k = 0u; k < nl; k++) { buf[o++] = (uint8_t)s_env[i].name[k]; }
        buf[o++] = (uint8_t)vl;
        for (k = 0u; k < vl; k++) { buf[o++] = (uint8_t)s_env[i].value[k]; }
    }
    return o;
}

/* 反序列化装载 env 缓冲（失败会清空缓冲并返回负） */
int Scl_Env_Load(const uint8_t *buf, int len)
{
    int o = 0;
    int i;
    if ((buf == NULL) || (len < 6))
    {
        return -1;
    }
    if ((buf[0] != 'S') || (buf[1] != 'C') || (buf[2] != 'L') || (buf[3] != 'E'))
    {
        return -1;
    }
    if (buf[4] != 1u)
    {
        return -1;   /* 未知版本 */
    }
    {
        int n = (int)buf[5];
        if (n > (int)SCL_CFG_ENV_MAX)
        {
            return -1;
        }
        o = 6;
        Scl_Env_FreeAll();
        for (i = 0; i < n; i++)
        {
            uint8_t  ty;
            uint16_t nl;
            uint16_t vl;
            char nm[SCL_CFG_VAR_NAME_MAX + 1u];
            char vl2[SCL_CFG_VAR_VALUE_MAX];
            uint16_t k;
            if (o + 2 > len) { Scl_Env_FreeAll(); return -1; }
            ty = buf[o++];
            nl = buf[o++];
            if ((nl > SCL_CFG_VAR_NAME_MAX) || ((o + (int)nl + 1) > len))
            {
                Scl_Env_FreeAll();
                return -1;
            }
            for (k = 0u; k < nl; k++) { nm[k] = (char)buf[o++]; }
            nm[nl] = '\0';
            vl = buf[o++];
            if ((vl >= SCL_CFG_VAR_VALUE_MAX) || ((o + (int)vl) > len))
            {
                Scl_Env_FreeAll();
                return -1;
            }
            for (k = 0u; k < vl; k++) { vl2[k] = (char)buf[o++]; }
            vl2[vl] = '\0';
            if (Scl_EnvSetCore(nm, ty, vl2) != 0)
            {
                Scl_Env_FreeAll();
                return -1;
            }
        }
    }
    return 0;
}
#endif /* SCL_CFG_ENV_EN */