/**
  ******************************************************************************
  * @file    b_cmds1.c
  * @brief   big_demo：设备命令 —— 老化炉（power/heat/fan/door/soak/temp/is_warm）
  *                                       电机（m_pps/m_rel/m_abs/m_home/m_stop/m_pos/m_busy）
  *                                       IO/显示（led/buzz/relay/rly/btn）
  *
  *          异步命令（soak/m_rel/m_abs/m_home）带 sync：SCL_Loop 在等待中轮询，
  *          完成时置 G_RETURN=1（对脚本而言"等待结束"信号）。
  ******************************************************************************
  */

#include "b_cmds.h"
#include "b_sim.h"

/* 本模块命令以 CMDDESC 参数模板方式注册（需 SCL_CFG_CMDDESC_EN=1） */
#if (SCL_EX_BIG_EN == 1u) && (SCL_CFG_CMDDESC_EN != 0u)

/* ========================== 前置 ========================== */
static void Cmd_power(int argc, char *argv[]);
static void Cmd_heat(int argc, char *argv[]);
static void Cmd_fan(int argc, char *argv[]);
static void Cmd_door(int argc, char *argv[]);
static void Cmd_soak(int argc, char *argv[]);
static void Cmd_temp(int argc, char *argv[]);
static void Cmd_chamber(int argc, char *argv[]);
static void Cmd_iswarm(int argc, char *argv[]);
static void Cmd_mpps(int argc, char *argv[]);
static void Cmd_mrel(int argc, char *argv[]);
static void Cmd_mabs(int argc, char *argv[]);
static void Cmd_mhome(int argc, char *argv[]);
static void Cmd_mstop(int argc, char *argv[]);
static void Cmd_mpos(int argc, char *argv[]);
static void Cmd_mbusy(int argc, char *argv[]);
static void Cmd_led(int argc, char *argv[]);
static void Cmd_buzz(int argc, char *argv[]);
static void Cmd_relay(int argc, char *argv[]);
static void Cmd_rly(int argc, char *argv[]);
static void Cmd_btn(int argc, char *argv[]);
static void Cmd_doorclosed(int argc, char *argv[]);
static void Cmd_isfan(int argc, char *argv[]);
static void Cmd_warmto(int argc, char *argv[]);

static scl_cmd_t s_c_power, s_c_heat, s_c_fan, s_c_door, s_c_soak;
static scl_cmd_t s_c_temp, s_c_chamber, s_c_iswarm;
static scl_cmd_t s_c_mpps, s_c_mrel, s_c_mabs, s_c_mhome, s_c_mstop;
static scl_cmd_t s_c_mpos, s_c_mbusy;
static scl_cmd_t s_c_led, s_c_buzz, s_c_relay, s_c_rly, s_c_btn;
static scl_cmd_t s_c_doorclosed, s_c_isfan, s_c_warmto;

static int ClampI(int v, int lo, int hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/* ========================== 老化炉 ========================== */

static void Cmd_power(int argc, char *argv[])
{
    (void)argc;
    b_sim_get()->oven.on = B_Eq(argv[0], "on") ? 1 : 0;
    B_Talk("power %s\n", (b_sim_get()->oven.on != 0) ? "on" : "off");
}

static void Cmd_heat(int argc, char *argv[])
{
    (void)argc;
    b_sim_get()->oven.pwm = ClampI(SCL_ParseInt(argv[0], 0), 0, 100);
    B_Talk("heat pwm=%d%%\n", b_sim_get()->oven.pwm);
}

static void Cmd_fan(int argc, char *argv[])
{
    (void)argc;
    b_sim_get()->oven.fan = B_Eq(argv[0], "on") ? 1 : 0;
    B_Talk("fan %s\n", (b_sim_get()->oven.fan != 0) ? "on" : "off");
}

static void Cmd_door(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    s->oven.door = B_Eq(argv[0], "open") ? 1 : 0;
    s->io.door_io = (s->oven.door == 0) ? 1 : 0;   /* 门限位与门一致 */
    B_Talk("door %s\n", (s->oven.door != 0) ? "open" : "close");
}

/* soak <tick>：保温等待，timer 递减至 0 完成 */
static bool Sync_soak(bool clear)
{
    b_sim_t *s = b_sim_get();
    if (clear)
    {
        return true;
    }
    if (s->oven.timer > 0)
    {
        return false;                       /* 继续等 */
    }
    SCL_Ret_Set(1);
    return true;
}

static void Cmd_soak(int argc, char *argv[])
{
    (void)argc;
    b_sim_get()->oven.timer = ClampI(SCL_ParseInt(argv[0], 0), 0, 100000);
    SCL_Ret_Set(0);
    B_Talk("soak %ld tick\n", b_sim_get()->oven.timer);
}

static void Cmd_temp(int argc, char *argv[])
{
    (void)argc; (void)argv;
    B_Talk("temp cur=%d set=%d\n", b_sim_get()->oven.cur, b_sim_get()->oven.set);
}

static void Cmd_chamber(int argc, char *argv[])
{
    (void)argc; (void)argv;
    b_sim_t *s = b_sim_get();
    B_Talk("chamber pwr=%s fan=%s door=%s cur=%d set=%d pwm=%d\n",
           (s->oven.on != 0) ? "on" : "off",
           (s->oven.fan != 0) ? "on" : "off",
           (s->oven.door != 0) ? "open" : "closed",
           s->oven.cur, s->oven.set, s->oven.pwm);
}

/* is_warm：达温条件（通电 + 门关 + cur>=set）→ G_RETURN */
static void Cmd_iswarm(int argc, char *argv[])
{
    (void)argc; (void)argv;
    b_sim_t *s = b_sim_get();
    SCL_Ret_Set(((s->oven.on != 0) && (s->oven.door == 0) &&
                 (s->oven.cur >= s->oven.set)) ? 1 : 0);
}

/* ========================== 步进电机 ========================== */

/* 电机异步通用完成判定：remain 走完即到位 */
static bool Sync_mot(bool clear)
{
    b_sim_t *s = b_sim_get();
    if (clear)
    {
        return true;
    }
    if ((s->mot.busy != 0) && (s->mot.remain > 0))
    {
        return false;
    }
    SCL_Ret_Set(1);
    return true;
}

static void Cmd_mpps(int argc, char *argv[])
{
    (void)argc;
    b_sim_get()->mot.pps = ClampI(SCL_ParseInt(argv[0], 50), 1, 1000);
    B_Talk("m_pps %d\n", b_sim_get()->mot.pps);
}

static void Cmd_mrel(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    long st = SCL_ParseInt(argv[0], 0);
    s->mot.tgt = s->mot.pos + st;
    s->mot.remain = (st < 0) ? -st : st;
    s->mot.mode = 2;
    s->mot.busy = 1;
    SCL_Ret_Set(0);
    B_Talk("m_rel %ld -> %ld\n", st, s->mot.tgt);
}

static void Cmd_mabs(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    long p = ClampI((int)SCL_ParseInt(argv[0], 0), 0, 1000000);
    s->mot.tgt = p;
    s->mot.remain = (p > s->mot.pos) ? (p - s->mot.pos) : (s->mot.pos - p);
    s->mot.mode = 1;
    s->mot.busy = 1;
    SCL_Ret_Set(0);
    B_Talk("m_abs %ld (remain %ld)\n", p, s->mot.remain);
}

static void Cmd_mhome(int argc, char *argv[])
{
    (void)argc; (void)argv;
    b_sim_t *s = b_sim_get();
    (void)s;
    s->mot.tgt = 0;
    s->mot.remain = s->mot.pos;
    s->mot.mode = 1;
    s->mot.busy = 1;
    SCL_Ret_Set(0);
    B_Talk("m_home\n");
}

static void Cmd_mstop(int argc, char *argv[])
{
    (void)argc; (void)argv;
    b_sim_t *s = b_sim_get();
    s->mot.busy = 0;
    s->mot.remain = 0;
    s->mot.mode = 0;
    B_Talk("m_stop (pos=%ld)\n", s->mot.pos);
}

static void Cmd_mpos(int argc, char *argv[])
{
    (void)argc; (void)argv;
    B_Talk("m_pos %ld%s\n", b_sim_get()->mot.pos,
           (b_sim_get()->mot.busy != 0) ? " (moving)" : "");
}

static void Cmd_mbusy(int argc, char *argv[])
{
    (void)argc; (void)argv;
    SCL_Ret_Set((b_sim_get()->mot.busy != 0) ? 1 : 0);
}

/* ========================== IO / 显示 ========================== */

static void Cmd_led(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    if (B_Eq(argv[0], "blink")) { s->io.led = 2; }
    else if (B_Eq(argv[0], "on")) { s->io.led = 1; }
    else { s->io.led = 0; }
    B_Talk("led %d\n", s->io.led);
}

static void Cmd_buzz(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    s->io.buzz = ClampI(SCL_ParseInt(argv[0], 0), 0, 100);
    B_Talk("buzz %d\n", s->io.buzz);
}

static void Cmd_relay(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    int idx = ClampI(SCL_ParseInt(argv[0], 0), 0, 3);
    s->io.relay[idx] = B_Eq(argv[1], "on") ? 1 : 0;
    B_Talk("relay %d %s\n", idx, (s->io.relay[idx] != 0) ? "on" : "off");
}

/* rly <idx>：读继电器 → G_RETURN */
static void Cmd_rly(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    int idx = ClampI(SCL_ParseInt(argv[0], 0), 0, 3);
    SCL_Ret_Set(s->io.relay[idx] != 0);
}

/* btn：启动按钮按下？→ G_RETURN（由 C 主状态机在脚本需要时置位） */
static void Cmd_btn(int argc, char *argv[])
{
    (void)argc; (void)argv;
    SCL_Ret_Set((b_sim_get()->io.btn_start != 0) ? 1 : 0);
}

/* door_closed：门关闭到位？→ G_RETURN */
static void Cmd_doorclosed(int argc, char *argv[])
{
    (void)argc; (void)argv;
    SCL_Ret_Set((b_sim_get()->io.door_io != 0) ? 1 : 0);
}

/* is_fan：风机运行？→ G_RETURN */
static void Cmd_isfan(int argc, char *argv[])
{
    (void)argc; (void)argv;
    SCL_Ret_Set((b_sim_get()->oven.fan != 0) ? 1 : 0);
}

/* warm_to <set>：上电并以 100% 占空同步加热至目标（模拟器内部完成，脚本无需轮询）。
   真机可将此改为异步（每 tick 升 1℃，SCL 异步 sync 驱动），脚本侧调用不变。 */
static void Cmd_warmto(int argc, char *argv[])
{
    b_sim_t *s = b_sim_get();
    (void)argc;
    s->oven.on  = 1;
    s->oven.set = ClampI(SCL_ParseInt(argv[0], 0), -40, 200);
    s->oven.pwm = 100;
    while (s->oven.cur < s->oven.set) { s->oven.cur++; }
    B_Talk("warm_to cur=%d set=%d\n", s->oven.cur, s->oven.set);
}

/* ========================== 注册 ========================== */

static const scl_arg_spec_t a_power[]  = { { "sw",  SCL_T_STR, 0u, "on/off" } };
static const scl_arg_spec_t a_heat[]   = { { "pct", SCL_T_INT, 0u, "0..100" } };
static const scl_arg_spec_t a_fan[]    = { { "sw",  SCL_T_STR, 0u, "on/off" } };
static const scl_arg_spec_t a_door[]   = { { "act", SCL_T_STR, 0u, "open/close" } };
static const scl_arg_spec_t a_soak[]   = { { "tick",SCL_T_INT, 0u, "保温等待 tick" } };
static const scl_arg_spec_t a_mpps[]   = { { "pps", SCL_T_INT, 0u, "1..1000 步/tick" } };
static const scl_arg_spec_t a_mrel[]   = { { "st",  SCL_T_INT, 0u, "相对步数(可负)" } };
static const scl_arg_spec_t a_mabs[]   = { { "pos", SCL_T_INT, 0u, "绝对位置" } };
static const scl_arg_spec_t a_led[]    = { { "st",  SCL_T_STR, 0u, "on/off/blink" } };
static const scl_arg_spec_t a_buzz[]   = { { "n",   SCL_T_INT, 0u, "响 n tick" } };
static const scl_arg_spec_t a_relay[]  = { { "idx", SCL_T_INT, 0u, "0..3" },
                                            { "sw",  SCL_T_STR, 0u, "on/off" } };
static const scl_arg_spec_t a_rly[]    = { { "idx", SCL_T_INT, 0u, "0..3" } };
static const scl_arg_spec_t a_warmto[] = { { "set", SCL_T_INT, 0u, "目标 ℃" } };

static const scl_cmd_desc_t s_d_power   = { "power",  "老化炉电源",   a_power, 1, Cmd_power,   NULL };
static const scl_cmd_desc_t s_d_heat    = { "heat",   "加热占空",     a_heat,  1, Cmd_heat,    NULL };
static const scl_cmd_desc_t s_d_fan     = { "fan",    "风机",         a_fan,   1, Cmd_fan,     NULL };
static const scl_cmd_desc_t s_d_door    = { "door",   "门",           a_door,  1, Cmd_door,    NULL };
static const scl_cmd_desc_t s_d_soak    = { "soak",   "保温等待(异步)",a_soak,  1, Cmd_soak,    Sync_soak };
static const scl_cmd_desc_t s_d_temp    = { "temp",   "读当前温度",   NULL, 0, Cmd_temp,      NULL };
static const scl_cmd_desc_t s_d_chamber = { "chamber","箱体状态",     NULL, 0, Cmd_chamber,   NULL };
static const scl_cmd_desc_t s_d_iswarm  = { "is_warm","达温条件",     NULL, 0, Cmd_iswarm,    NULL };
static const scl_cmd_desc_t s_d_mpps    = { "m_pps",  "电机速率",     a_mpps, 1, Cmd_mpps,     NULL };
static const scl_cmd_desc_t s_d_mrel    = { "m_rel",  "相对移动(异步)",a_mrel, 1, Cmd_mrel,    Sync_mot };
static const scl_cmd_desc_t s_d_mabs    = { "m_abs",  "绝对定位(异步)",a_mabs, 1, Cmd_mabs,    Sync_mot };
static const scl_cmd_desc_t s_d_mhome   = { "m_home", "回零(异步)",    NULL, 0, Cmd_mhome,    Sync_mot };
static const scl_cmd_desc_t s_d_mstop   = { "m_stop", "停止电机",      NULL, 0, Cmd_mstop,    NULL };
static const scl_cmd_desc_t s_d_mpos    = { "m_pos",  "读位置",        NULL, 0, Cmd_mpos,     NULL };
static const scl_cmd_desc_t s_d_mbusy   = { "m_busy", "电机忙条件",    NULL, 0, Cmd_mbusy,    NULL };
static const scl_cmd_desc_t s_d_led     = { "led",    "状态灯",        a_led,  1, Cmd_led,     NULL };
static const scl_cmd_desc_t s_d_buzz    = { "buzz",   "蜂鸣",          a_buzz, 1, Cmd_buzz,    NULL };
static const scl_cmd_desc_t s_d_relay   = { "relay",  "写继电器",      a_relay,2, Cmd_relay,   NULL };
static const scl_cmd_desc_t s_d_rly     = { "rly",    "读继电器条件",  a_rly,  1, Cmd_rly,     NULL };
static const scl_cmd_desc_t s_d_btn     = { "btn",    "启动按钮条件",  NULL, 0, Cmd_btn,      NULL };
static const scl_cmd_desc_t s_d_doorclosed = { "door_closed","门关闭到位条件",NULL, 0, Cmd_doorclosed, NULL };
static const scl_cmd_desc_t s_d_isfan     = { "is_fan", "风机运行条件",  NULL, 0, Cmd_isfan,    NULL };
static const scl_cmd_desc_t s_d_warmto    = { "warm_to",  "升温到位",     a_warmto,1, Cmd_warmto,    NULL };

typedef struct { scl_cmd_t *node; const scl_cmd_desc_t *desc; } dev_cmd_t;
static const dev_cmd_t s_devs[] = {
    { &s_c_power,   &s_d_power   }, { &s_c_heat,  &s_d_heat  }, { &s_c_fan, &s_d_fan },
    { &s_c_door,    &s_d_door    }, { &s_c_soak,  &s_d_soak  }, { &s_c_temp, &s_d_temp },
    { &s_c_chamber, &s_d_chamber }, { &s_c_iswarm,&s_d_iswarm}, { &s_c_mpps, &s_d_mpps },
    { &s_c_mrel,    &s_d_mrel    }, { &s_c_mabs,  &s_d_mabs  }, { &s_c_mhome, &s_d_mhome },
    { &s_c_mstop,   &s_d_mstop   }, { &s_c_mpos,  &s_d_mpos  }, { &s_c_mbusy, &s_d_mbusy },
    { &s_c_led,     &s_d_led     }, { &s_c_buzz,  &s_d_buzz  }, { &s_c_relay, &s_d_relay },
    { &s_c_rly,     &s_d_rly     }, { &s_c_btn,   &s_d_btn   },
    { &s_c_doorclosed, &s_d_doorclosed }, { &s_c_isfan, &s_d_isfan },
    { &s_c_warmto,    &s_d_warmto    },
};

void Big_CmdDev_Register(void)
{
    unsigned i;
    for (i = 0u; i < (unsigned)(sizeof(s_devs) / sizeof(s_devs[0])); i++)
    {
        SCL_CmdRegisterDesc(s_devs[i].node, s_devs[i].desc);
    }
}

#endif /* SCL_EX_BIG_EN && CMDDESC */
