/* ===============================================================
 * SCL mini v2 类型化状态机：boot（由 tools/scl_mini_c.py 生成，勿手改）
 * 来源：example/mcu_template/boot.s2c (现代语法 .s2c)
 * 用法：SCL_Init(); <注册所需命令>; boot_mini_register();
 *       boot_mini_start(); 然后周期调 boot_mini_step()（每次一动作，非阻塞）。
 * 变量=类型化 static；SCL_VarBind 绑定 → 外部 SCL_VarGet/Set 路由到 static。
 * 需 SCL_CFG_MINI_EN=1 编译（绑定路由在库内）。
 * ===============================================================
 */
#include "scl.h"

#define MINI_NST 20u
#define MINI_STEP_LIMIT 1000000u
#if defined(__GNUC__) || defined(__clang__)
#define MINI_UNUSED __attribute__((unused))
#else
#define MINI_UNUSED
#endif

static MINI_UNUSED void Mini_Itoa(int32_t v, char *d, unsigned cap)
{ char t[12]; unsigned k = 0u, off = 0u, i; uint32_t u;
  if (v < 0) { off = 1u; u = (uint32_t)(-(v + 1)) + 1u; } else { u = (uint32_t)v; }
  if (u == 0u) { t[k++] = '0'; }
  while (u) { t[k++] = (char)('0' + (u % 10u)); u /= 10u; }
  if (off && off < cap) { d[0] = '-'; }
  for (i = 0u; i < k && off + i + 1u < cap; i++) { d[off + i] = t[k - 1u - i]; }
  d[(off + (k < cap - off ? k : cap - 1u - off))] = '\0'; }

/* ---- 类型化变量（绑定路由到外部） ---- */
static int32_t m_i;
static MINI_UNUSED const char *m_i_get(void){ static char b[12]; Mini_Itoa(m_i, b, sizeof b); return b; }
static MINI_UNUSED int i_set(const char *s){ m_i = SCL_ParseInt((s != NULL) ? s : "", 0); return 0; }
static int32_t m_r;
static MINI_UNUSED const char *m_r_get(void){ static char b[12]; Mini_Itoa(m_r, b, sizeof b); return b; }
static MINI_UNUSED int r_set(const char *s){ m_r = SCL_ParseInt((s != NULL) ? s : "", 0); return 0; }
static int32_t m_sum;
static MINI_UNUSED const char *m_sum_get(void){ static char b[12]; Mini_Itoa(m_sum, b, sizeof b); return b; }
static MINI_UNUSED int sum_set(const char *s){ m_sum = SCL_ParseInt((s != NULL) ? s : "", 0); return 0; }

/* ---- 变量绑定：SCL_VarBind 路由（命中 SCL_VarGet/Set） ---- */
static const scl_var_bind_t s_bind[] = {
    { "i", SCL_T_INT, m_i_get, i_set },
    { "r", SCL_T_INT, m_r_get, r_set },
    { "sum", SCL_T_INT, m_sum_get, sum_set },
};

/* ---- 运行状态 ---- */
static uint16_t s_st;
static uint32_t s_steps;
static uint8_t  s_wait;
static uint16_t s_pend;
static uint8_t  s_fault;
static uint16_t s_retst;

uint8_t boot_mini_step(void)
{
    if (s_wait != 0u)
    { int p = SCL_AsyncPoll();
      if (p == 0) { return 1u; }
      s_wait = 0u; s_st = s_pend; }
    if (s_st >= MINI_NST) { return 0u; }
    if (s_steps++ >= MINI_STEP_LIMIT) { s_fault = 1u; goto mini_done; }
    switch (s_st)
    {
    case 0:
    {
        m_r = 0;
        s_st = 1u;
        break;
    }
    case 1:
    {
        m_i = 0;
        s_st = 2u;
        break;
    }
    case 2:
    {
        m_sum = 0;
        s_st = 3u;
        break;
    }
    case 3:
    {
    { int32_t a = (5); int32_t b = (1); int32_t r;
      r = a + b; m_r = r; }
    s_st = 4u;
        break;
    }
    case 4:
    {
    SCL_Ret_Set(((m_r) == ((6))) ? 1 : 0);
    s_st = 5u;
        break;
    }
    case 5:
    {
        if (SCL_Ret_Get() != 0) { SCL_Ret_Set(0); s_st = 8u; }
        else { SCL_Ret_Set(0); s_st = 6u; }
        break;
    }
    case 6:
    {
    const char *av[1];
    scl_invoke_arg_t ia[1];
    av[0] = "boot-const-fail";
    ia[0].text = av[0];
    ia[0].type = SCL_T_STR;
    { uint8_t r = SCL_CmdInvoke("echo", 1, ia);
      if (r == 0u) { s_fault = 1u; goto mini_done; }
      if (r == 2u) { s_wait = 1u; s_pend = 7u; }
      else { s_st = 7u; } }
        break;
    }
    case 7:
    {
        s_st = 9u;
        break;
    }
    case 8:
    {
    const char *av[1];
    scl_invoke_arg_t ia[1];
    av[0] = "boot-const-ok";
    ia[0].text = av[0];
    ia[0].type = SCL_T_STR;
    { uint8_t r = SCL_CmdInvoke("echo", 1, ia);
      if (r == 0u) { s_fault = 1u; goto mini_done; }
      if (r == 2u) { s_wait = 1u; s_pend = 9u; }
      else { s_st = 9u; } }
        break;
    }
    case 9:
    {
    SCL_Ret_Set(((m_i) < ((3))) ? 1 : 0);
    s_st = 10u;
        break;
    }
    case 10:
    {
        if (SCL_Ret_Get() != 0) { SCL_Ret_Set(0); s_st = 12u; }
        else { SCL_Ret_Set(0); s_st = 11u; }
        break;
    }
    case 11:
    {
        s_st = 15u;
        break;
    }
    case 12:
    {
    { int32_t a = m_sum; int32_t b = m_i; int32_t r;
      r = a + b; m_sum = r; }
    s_st = 13u;
        break;
    }
    case 13:
    {
    { int32_t a = m_i; int32_t b = (1); int32_t r;
      r = a + b; m_i = r; }
    s_st = 14u;
        break;
    }
    case 14:
    {
        s_st = 9u;
        break;
    }
    case 15:
    {
    SCL_Ret_Set(((m_sum) == ((3))) ? 1 : 0);
    s_st = 16u;
        break;
    }
    case 16:
    {
        if (SCL_Ret_Get() != 0) { SCL_Ret_Set(0); s_st = 18u; }
        else { SCL_Ret_Set(0); s_st = 17u; }
        break;
    }
    case 17:
    {
        s_st = 19u;
        break;
    }
    case 18:
    {
    const char *av[1];
    scl_invoke_arg_t ia[1];
    av[0] = "boot-loop-ok";
    ia[0].text = av[0];
    ia[0].type = SCL_T_STR;
    { uint8_t r = SCL_CmdInvoke("echo", 1, ia);
      if (r == 0u) { s_fault = 1u; goto mini_done; }
      if (r == 2u) { s_wait = 1u; s_pend = 19u; }
      else { s_st = 19u; } }
        break;
    }
    case 19:
    {
    const char *av[1];
    scl_invoke_arg_t ia[1];
    av[0] = "boot-done";
    ia[0].text = av[0];
    ia[0].type = SCL_T_STR;
    { uint8_t r = SCL_CmdInvoke("echo", 1, ia);
      if (r == 0u) { s_fault = 1u; goto mini_done; }
      if (r == 2u) { s_wait = 1u; s_pend = 20u; }
      else { s_st = 20u; } }
        break;
    }
    default: s_st = MINI_NST; break;
    }
mini_done:
    if (s_fault != 0u) { s_fault = 0u; s_st = MINI_NST; return 0u; }
    return (s_st < MINI_NST) ? 1u : 0u;
}

void boot_mini_start(void)
{
    SCL_Ret_Set(0); s_st = 0u; s_steps = 0u;
    s_wait = 0u; s_pend = 0u; s_fault = 0u; s_retst = 0u;
}
uint8_t boot_mini_busy(void)
{ return (s_st < MINI_NST) ? 1u : 0u; }

/* ---- 注册：把 s2c 注册成命令 + 绑定变量到 SCL ---- */
static void boot_mini_cmd(int argc, char *argv[])
{ (void)argc; (void)argv; boot_mini_start(); }
static scl_cmd_t s_boot_cmd = { "boot", boot_mini_cmd, NULL, NULL, 0, NULL };
void boot_mini_register(void)
{
    SCL_RegisterCmd(&s_boot_cmd);
    (void)SCL_VarBind(s_bind, 3);
}
