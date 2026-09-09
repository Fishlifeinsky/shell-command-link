/**
  ******************************************************************************
  * @file    b_sim.h
  * @brief   big_demo：设备模拟器（老化炉 + 步进电机 + IO + 批次统计）状态与接口
  *
  *          这是"固件侧真实外设"的占位模拟：所有命令操作的是这里的状态，
  *          C 层主状态机与 SCL 脚本看到的是同一份事实，保证混合控制一致。
  *          移植到真机时，把 b_sim_* 换成寄存器/HAL 即可，命令/脚本无需改动。
  ******************************************************************************
  */

#ifndef __B_SIM_H__
#define __B_SIM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 老化炉箱体 ---- */
typedef struct
{
    int  on;     /* 运行开关 */
    int  set;    /* 目标温度 ℃ */
    int  cur;    /* 当前温度 ℃（按 pwm 向 set 收敛） */
    int  pwm;    /* 加热占空 0..100 */
    int  fan;    /* 风机 0/1 */
    int  door;   /* 门 0=关 1=开 */
    long timer;  /* 保温剩余 tick（soak） */
} b_oven_t;

/* ---- 步进电机（相对/绝对/复位，异步到完成） ---- */
typedef struct
{
    int  busy;   /* 正在运行 */
    long pos;    /* 当前位置（步） */
    long tgt;    /* 目标位置（绝对，busy 时有效） */
    long remain; /* 剩余步 */
    int  pps;    /* 速率 步/tick */
    int  mode;   /* 0=idle 1=abs 2=rel */
} b_mot_t;

/* ---- IO / 显示 ---- */
typedef struct
{
    int  relay[4];  /* 4 路继电器 */
    int  led;       /* 0=off 1=on 2=blink */
    int  buzz;      /* 蜂鸣剩余次（tick 递减） */
    int  btn_start; /* 启动按钮 1=按下 */
    int  door_io;   /* 门限位 1=关闭到位 */
} b_io_t;

/* ---- 批次/统计 ---- */
typedef struct
{
    long total;   /* 累计件数 */
    long ok;      /* 良品 */
    long ng;      /* 不良 */
    long sn;      /* 下一个序列号 */
    int  lot;     /* 当前批次容量 */
    int  cur;     /* 本批已做件数 */
    int  lot_ok;  /* 本批良品 */
    int  lot_ng;  /* 本批不良 */
} b_lot_t;

/* ---- 全局单例（命令与 C 主状态机共用） ---- */
typedef struct
{
    b_oven_t oven;
    b_mot_t  mot;
    b_io_t   io;
    b_lot_t  lot;
    int      tick;      /* 运行 tick（只增） */
    int      in_script; /* 1=正在跑脚本/链（混合控制演示用） */
} b_sim_t;

/* 单例访问 */
b_sim_t *b_sim_get(void);

/* 初始化设备：上电默认状态（门开、温 25℃、风机停…） */
void b_sim_init(void);

/* 推进一个 tick：温度向 set 收敛、电机按 pps 走步、蜂鸣/保温倒计时。
   由库的 SCL_Loop/异步 sync 轮询与 C 主循环调用。 */
void b_sim_step(void);

#ifdef __cplusplus
}
#endif

#endif /* __B_SIM_H__ */
