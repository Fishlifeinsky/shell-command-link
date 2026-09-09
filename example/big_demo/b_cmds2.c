/**
  ******************************************************************************
  * @file    b_cmds2.c
  * @brief   big_demo：流程/批次/统计/校准/诊断命令
  *
  *          lot_begin/part_pass/part_fail/lot_end  批次与判定
  *          stat_reset                             累计统计复位
  *          pwm_calc                               温控策略（业务命令演示脚本调它）
  *          diag_all/about/b_echo                  诊断与通用输出
  ******************************************************************************
  */

#include "b_cmds.h"
#include "b_sim.h"

/* 本模块命令以 CMDDESC 参数模板方式注册（需 SCL_CFG_CMDDESC_EN=1） */
#if (SCL_EX_BIG_EN == 1u) && (SCL_CFG_CMDDESC_EN != 0u)

/* ========================== 前置 ========================== */
static void Cmd_lotbegin(int argc, char *argv[]);
static void Cmd_partpass(int argc, char *argv[]);
static void Cmd_partfail(int argc, char *argv[]);
static void Cmd_lotend(int argc, char *argv[]);
static void Cmd_statreset(int argc, char *argv[]);
static void Cmd_pwmcalc(int argc, char *argv[]);
static void Cmd_diagall(int argc, char *argv[]);
static void Cmd_about(int argc, char *argv[]);
static void Cmd_becho(int argc, char *argv[]);
static void Cmd_setret(int argc, char *argv[]);

static scl_cmd_t s_c_lotbegin, s_c_partpass, s_c_partfail, s_c_lotend;
static scl_cmd_t s_c_statreset, s_c_pwmcalc, s_c_diagall, s_c_about, s_c_becho;
static scl_cmd_t s_c_setret;

static int ClampI(int v, int lo, int hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/* ========================== 批次 / 判定 ========================== */

/* lot_begin <n>：开启容量为 n 的批次，复位本批计数 */
static void Cmd_lotbegin(int argc, char *argv[])
{
    (void)argc;
    b_lot_t *l = &b_sim_get()->lot;
    int n = ClampI(SCL_ParseInt(argv[0], 0), 1, 10000);
    l->lot = n;
    l->cur = 0;
    l->lot_ok = 0;
    l->lot_ng = 0;
    B_Talk("lot_begin %d (起始序号 %ld)\n", n, l->sn);
}

/* 单件判定（ok/ng 共用内部逻辑，tag=0/1） */
static void PartJudge(int tag, const char *extra)
{
    b_sim_t *s = b_sim_get();
    b_lot_t *l = &s->lot;
    long sn;

    if (l->lot <= 0)
    {
        B_Talk("error: 未 lot_begin\n");
        SCL_Ret_Set(0);
        return;
    }
    if (l->cur >= l->lot)
    {
        B_Talk("error: 批次已满 (%d)\n", l->lot);
        SCL_Ret_Set(0);
        return;
    }
    sn = l->sn;
    l->sn++;
    l->cur++;
    l->total++;
    if (tag != 0)
    {
        l->ok++;
        l->lot_ok++;
    }
    else
    {
        l->ng++;
        l->lot_ng++;
    }
    B_Talk("part %s sn=%ld (%d/%d)%s\n",
           (tag != 0) ? "PASS" : "NG ", sn, l->cur, l->lot,
           (extra != 0) ? extra : "");
    SCL_Ret_Set(1);
}

static void Cmd_partpass(int argc, char *argv[])
{
    (void)argc; (void)argv;
    PartJudge(1, NULL);
}

static void Cmd_partfail(int argc, char *argv[])
{
    PartJudge(0, (argc > 0) ? argv[0] : NULL);
}

/* lot_end：本批汇总并关闭批次 */
static void Cmd_lotend(int argc, char *argv[])
{
    (void)argc; (void)argv;
    b_lot_t *l = &b_sim_get()->lot;
    if (l->lot <= 0)
    {
        B_Talk("lot_end: 当前无批次\n");
        return;
    }
    B_Talk("lot_end %d 件: ok=%d ng=%d (累计 ok=%ld ng=%ld)\n",
           l->lot, l->lot_ok, l->lot_ng, l->ok, l->ng);
    l->lot = 0;
}

/* stat_reset：复位累计统计（保留批次计数? 只清 total/ok/ng/sn 归位 1000） */
static void Cmd_statreset(int argc, char *argv[])
{
    (void)argc; (void)argv;
    b_lot_t *l = &b_sim_get()->lot;
    l->total = 0;
    l->ok = 0;
    l->ng = 0;
    l->sn = 1000;
    B_Talk("stat_reset ok\n");
}

/* ========================== 温控策略（业务命令） ========================== */

/* pwm_calc <set>：按温差计算加热占空并写入（简单比例：1℃→10%） */
static void Cmd_pwmcalc(int argc, char *argv[])
{
    (void)argc;
    b_sim_t *s = b_sim_get();
    int set = ClampI(SCL_ParseInt(argv[0], 0), -40, 200);
    int pwm;
    if (s->oven.cur < set)
    {
        pwm = ClampI((set - s->oven.cur) * 10, 0, 100);
    }
    else
    {
        pwm = 0;
    }
    s->oven.pwm = pwm;
    s->oven.set = set;
    B_Talk("pwm_calc set=%d cur=%d -> pwm=%d\n", set, s->oven.cur, pwm);
}

/* ========================== 诊断 / 工具 ========================== */

static void Cmd_diagall(int argc, char *argv[])
{
    (void)argc; (void)argv;
    b_sim_t *s = b_sim_get();
    B_Talk("[diag] oven on=%d cur=%d set=%d pwm=%d fan=%d door=%d timer=%ld\n",
           s->oven.on, s->oven.cur, s->oven.set, s->oven.pwm,
           s->oven.fan, s->oven.door, s->oven.timer);
    B_Talk("[diag] mot busy=%d pos=%ld tgt=%ld remain=%ld pps=%d\n",
           s->mot.busy, s->mot.pos, s->mot.tgt, s->mot.remain, s->mot.pps);
    B_Talk("[diag] io led=%d buzz=%d rly=%d%d%d%d btn=%d doorio=%d\n",
           s->io.led, s->io.buzz,
           s->io.relay[0], s->io.relay[1], s->io.relay[2], s->io.relay[3],
           s->io.btn_start, s->io.door_io);
    B_Talk("[diag] lot n=%d cur=%d ok=%d ng=%d | cum total=%ld ok=%ld ng=%ld sn=%ld\n",
           s->lot.lot, s->lot.cur, s->lot.lot_ok, s->lot.lot_ng,
           s->lot.total, s->lot.ok, s->lot.ng, s->lot.sn);
}

static void Cmd_about(int argc, char *argv[])
{
    (void)argc; (void)argv;
    B_Talk("big_demo v0.3c (SCL 老化炉产线测试台)\n");
}

static void Cmd_becho(int argc, char *argv[])
{
    int i;
    B_Talk("b_echo");
    for (i = 0; i < argc; i++)
    {
        B_Talk(" %s", argv[i]);
    }
    B_Talk("\n");
}

/* setret <0|1>：显式写 G_RETURN（s2c 的 ret/!条件反转目标命令） */
static void Cmd_setret(int argc, char *argv[])
{
    (void)argc;
    SCL_Ret_Set((SCL_ParseInt(argv[0], 0) != 0) ? 1 : 0);
}

/* ========================== 注册 ========================== */

static const scl_arg_spec_t a_lotbegin[] = { { "n", SCL_T_INT, 0u, "批次容量(件)" } };
static const scl_arg_spec_t a_partfail[] = { { "msg", SCL_T_STR, 1u, "不良原因(可选)" } };
static const scl_arg_spec_t a_pwmcalc[]  = { { "set", SCL_T_INT, 0u, "目标 ℃" } };
static const scl_arg_spec_t a_setret[]   = { { "v", SCL_T_INT, 0u, "0/1" } };

static const scl_cmd_desc_t s_d_lotbegin = { "lot_begin", "开启批次",    a_lotbegin, 1, Cmd_lotbegin, NULL };
static const scl_cmd_desc_t s_d_partpass = { "part_pass", "单件判定良",  NULL, 0, Cmd_partpass, NULL };
static const scl_cmd_desc_t s_d_partfail = { "part_fail", "单件判定不良",a_partfail, 1, Cmd_partfail, NULL };
static const scl_cmd_desc_t s_d_lotend   = { "lot_end",   "批次汇总",    NULL, 0, Cmd_lotend,   NULL };
static const scl_cmd_desc_t s_d_statreset= { "stat_reset","复位统计",    NULL, 0, Cmd_statreset,NULL };
static const scl_cmd_desc_t s_d_pwmcalc  = { "pwm_calc",  "温控占空计算",a_pwmcalc, 1, Cmd_pwmcalc, NULL };
static const scl_cmd_desc_t s_d_diagall  = { "diag_all",  "全状态诊断",  NULL, 0, Cmd_diagall,  NULL };
static const scl_cmd_desc_t s_d_about    = { "about",     "版本信息",    NULL, 0, Cmd_about,    NULL };
static const scl_cmd_desc_t s_d_becho    = { "b_echo",    "通用打印",    NULL, 0, Cmd_becho,    NULL };
static const scl_cmd_desc_t s_d_setret   = { "setret",    "写 G_RETURN", a_setret, 1, Cmd_setret,   NULL };

typedef struct { scl_cmd_t *node; const scl_cmd_desc_t *desc; } flow_cmd_t;
static const flow_cmd_t s_flows[] = {
    { &s_c_lotbegin, &s_d_lotbegin }, { &s_c_partpass, &s_d_partpass },
    { &s_c_partfail, &s_d_partfail }, { &s_c_lotend,   &s_d_lotend   },
    { &s_c_statreset,&s_d_statreset}, { &s_c_pwmcalc,  &s_d_pwmcalc  },
    { &s_c_diagall,  &s_d_diagall  }, { &s_c_about,    &s_d_about    },
    { &s_c_becho,    &s_d_becho    },
    { &s_c_setret,   &s_d_setret   },
};

void Big_CmdFlow_Register(void)
{
    unsigned i;
    for (i = 0u; i < (unsigned)(sizeof(s_flows) / sizeof(s_flows[0])); i++)
    {
        SCL_CmdRegisterDesc(s_flows[i].node, s_flows[i].desc);
    }
}

#endif /* SCL_EX_BIG_EN && CMDDESC */
