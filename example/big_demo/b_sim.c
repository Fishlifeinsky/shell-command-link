/**
  ******************************************************************************
  * @file    b_sim.c
  * @brief   big_demo：设备模拟器实现（温度收敛/电机走步/倒计时）
  ******************************************************************************
  */

#include "b_sim.h"

static b_sim_t s_sim;

b_sim_t *b_sim_get(void)
{
    return &s_sim;
}

void b_sim_init(void)
{
    b_sim_t *s = &s_sim;
    s->oven.on  = 0;
    s->oven.set = 25;
    s->oven.cur = 25;
    s->oven.pwm = 0;
    s->oven.fan = 0;
    s->oven.door = 1;   /* 门开（上电待装料） */
    s->oven.timer = 0;

    s->mot.busy  = 0;
    s->mot.pos   = 0;
    s->mot.tgt   = 0;
    s->mot.remain = 0;
    s->mot.pps   = 50;
    s->mot.mode  = 0;

    s->io.relay[0] = s->io.relay[1] = s->io.relay[2] = s->io.relay[3] = 0;
    s->io.led  = 0;
    s->io.buzz = 0;
    s->io.btn_start = 0;
    s->io.door_io   = 1;

    s->lot.total = 0;
    s->lot.ok = 0;
    s->lot.ng = 0;
    s->lot.sn = 1000;
    s->lot.lot = 0;
    s->lot.cur = 0;
    s->lot.lot_ok = 0;
    s->lot.lot_ng = 0;

    s->tick = 0;
    s->in_script = 0;
}

void b_sim_step(void)
{
    b_sim_t *s = &s_sim;

    s->tick++;

    /* 温度收敛规则（可读、确定）：
         - 断电 / 门开        → 向环境(25℃)回落；
         - 门关但风机未开     → 无强制对流，不控温（温度保持）；
         - 门关 + 风机开      → pwm>0 每 tick 升温 1℃ 直至 set；pwm=0 每 tick 冷却 1℃ */
    if ((s->oven.on == 0) || (s->oven.door != 0))
    {
        if (s->oven.cur > 25) { s->oven.cur--; }
    }
    else if (s->oven.fan == 0)
    {
        /* 无对流：保持 */
    }
    else if (s->oven.pwm > 0)
    {
        if (s->oven.cur < s->oven.set) { s->oven.cur++; }
    }
    else
    {
        if (s->oven.cur > s->oven.set) { s->oven.cur--; }
    }

    /* 电机：busy 时按 pps 走步到目标 */
    if (s->mot.busy != 0)
    {
        long step = (s->mot.pps > 0) ? s->mot.pps : 1;
        if (step > s->mot.remain) { step = s->mot.remain; }
        if (s->mot.tgt > s->mot.pos) { s->mot.pos += step; }
        else if (s->mot.tgt < s->mot.pos) { s->mot.pos -= step; }
        s->mot.remain -= step;
        if (s->mot.remain <= 0)
        {
            s->mot.busy = 0;
            s->mot.mode = 0;
            s->mot.remain = 0;
            s->mot.pos = s->mot.tgt;
        }
    }

    /* 倒计时：保温、蜂鸣 */
    if (s->oven.timer > 0)
    {
        s->oven.timer--;
    }
    if (s->io.buzz > 0)
    {
        s->io.buzz--;
    }
}
