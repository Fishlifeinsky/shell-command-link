/**
  ******************************************************************************
  * @file    main.c
  * @brief   big_demo：C 语言混合控制 + 全流程自测（PC 可编译运行）
  *
  *          展示"复杂项目"的三层协作：
  *            C 宿主层   ：注册命令/env/脚本命令；直接 SCL_Run(链) / SCL_VarSet·Get；
  *                          驱动模拟器 tick；读命令副作用做决策与断言。
  *            命令库     ：30+ 业务命令（老化炉/电机/IO/批次/校准/诊断，带 desc 模板）
  *            脚本命令   ：4 个 .s2c 编译成 const 程序（selftest/profile/run_lot/diagnose）
  *                          命令行/链可直接触发，参数 arg0.. + alias + const 折叠。
  *
  *          混合控制的"事实源" = b_sim 单例：C 与脚本都作用于它，读回一致。
  *
  *  编译：
  *    gcc -O2 -Wall -Wextra -I scl/Inc -I scl/Src -I example -I example/big_demo \
    *        -DSCL_CFG_DYNAMIC_MEM_EN=1 -DSCL_CFG_VAR_MAX=12 -DSCL_CFG_ARG_MAX=12 \
  *        scl/Src/scl.c scl/Src/scl_var.c scl/Src/scl_env.c \
  *        example/scl_port.c \
  *        example/big_demo/b_sim.c example/big_demo/b_cmds1.c \
  *        example/big_demo/b_cmds2.c example/big_demo/b_env.c \
  *        example/big_demo/b_scripts.c example/big_demo/main.c \
  *        example/big_demo/gen/sc_selftest.c example/big_demo/gen/sc_profile.c \
  *        example/big_demo/gen/sc_run_lot.c example/big_demo/gen/sc_diagnose.c \
  *        -o build/big_demo
  ******************************************************************************
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scl.h"
#include "scl_port.h"
#include "b_sim.h"
#include "b_cmds.h"
#include "b_sys.h"

/* ========================== 自测基础设施 ========================== */

#define BIG_TICKS_MAX  1000000L
static char g_cap[1u << 16];
static int g_pass = 0;
static int g_fail = 0;

static void *Big_Alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void *Big_Realloc(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    return realloc(ptr, size);
}

static void Big_Free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

#define CHK(cond, msg) \
    do { \
        if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
        else      { g_fail++; printf("  [FAIL] %s\n", msg); } \
    } while (0)

/* 驱动：模拟器每 tick 推进 + 解释器步进，直到空闲 */
static int Big_Drive(void)
{
    long t = 0L;
    while (SCL_Idle() == 0u)
    {
        b_sim_step();          /* 温度/电机/计时器推进（真实 MCU 主循环同样调用） */
        SCL_Loop();
        t++;
        if (t > BIG_TICKS_MAX)
        {
            SCL_Abort();
            while (SCL_Idle() == 0u) { SCL_Loop(); }
            return -1;         /* 超时：防死循环 */
        }
    }
    return 0;
}

/* 执行一行（脚本命令优先，否则回退普通链 SCL_Run）并驱动到空闲；返回并捕获 */
static int Big_RunLine(const char *line, const char **endcap)
{
    Scl_CapBegin(g_cap, (int)sizeof(g_cap));
    {
        uint8_t r = SCL_Scmd_RunText(line);
        if (r == 2u)            /* 非脚本命令 → 当普通链跑 */
        {
            if (SCL_Run(line) == 0u) { Scl_CapEnd(); return -2; }
        }
    }
    {
        int drv = Big_Drive();
        Scl_CapEnd();
        return drv;
    }
    (void)endcap;
}

static int CapHas(const char *needle)
{
    return (strstr(g_cap, needle) != NULL) ? 1 : 0;
}

/* ========================== 主流程：C 语言混合控制 ========================== */

int main(void)
{
    b_sim_t *S;

    printf("==== big_demo: SCL 复杂项目（C 混合控制）====\n");

    /* ---- 0. 初始化：库 + 设备模拟 + 命令 + env + 脚本命令 ---- */
    {
        scl_allocator_t mem = { Big_Alloc, Big_Realloc, Big_Free, NULL };
        CHK(SCL_InitEx(&mem) != 0u, "动态 allocator 初始化");
    }
    printf("  [TRACE] allocator ready\n");
    b_sim_init();
    printf("  [TRACE] simulator ready\n");
    Big_CmdDev_Register();        /* 老化炉/电机/IO（22 命令） */
    printf("  [TRACE] device commands ready\n");
    Big_CmdFlow_Register();       /* 批次/统计/校准/诊断/工具（9 命令） */
    printf("  [TRACE] flow commands ready\n");
    Big_Env_Register();           /* env 默认运行参数 */
    printf("  [TRACE] env ready\n");
    Big_Scmd_RegisterAll();       /* 4 个 .s2c → 脚本命令 */
    printf("  [TRACE] script commands ready\n");
    S = b_sim_get();

    /* ---- 1. C 层直接控制：用变量 API + 链文本 ---- */
    printf("-- [C] 直接链控制 + 变量 API --\n");
    SCL_VarSetT("c_from_c", SCL_T_INT, "7");          /* C 设变量（脚本结束自动释放前，先读） */
    {
        const char *v = SCL_VarGet("c_from_c");
        CHK((v != NULL) && (strcmp(v, "7") == 0), "C SCL_VarSet/Get 直读变量");
    }
    Scl_CapBegin(g_cap, (int)sizeof(g_cap));
    if (SCL_Run("b_echo c_from_c=${c_from_c};led on;buzz 1") != 0u)   /* C 直接跑链文本 */
    {
        Big_Drive();
    }
    Scl_CapEnd();
    CHK(CapHas("c_from_c=7"), "C 链文本读回 C 变量(${})");

    /* ---- 2. 脚本命令：开机自检（脚本内部 const/alias/电机/IO 断言） ---- */
    printf("-- [C] 触发脚本命令 selftest --\n");
    CHK(Big_RunLine("selftest", NULL) == 0, "selftest 执行完成(未超时)");
    CHK(CapHas("selftest OK"), "自检全部通过(6/6)");
    CHK(S->mot.pos == 0, "C 读 b_sim：自检后电机已归零");
    CHK((S->oven.fan != 0) && (S->io.relay[0] != 0), "C 读 b_sim：IO 置位保持");

    /* ---- 3. C 读 env（脚本 diagnose 会再读一次，双路径验证） ---- */
    {
        const char *v = SCL_VarGet("tgt_ov");
        CHK((v != NULL) && (strcmp(v, "60") == 0), "C 经 env 回退读运行参数 tgt_ov=60");
    }

    /* ---- 4. 脚本命令：温循档位 0（恒温 60℃，脚本内部 pwm/等温/保温） ---- */
    printf("-- [C] 触发脚本命令 profile 0 --\n");
    CHK(Big_RunLine("profile 0", NULL) == 0, "profile 0 执行完成");
    CHK(CapHas("profile done"), "温循档位正常结束");
    CHK(CapHas("warm_to cur=60 set=60"), "C 捕获：箱温已升温到位 60℃");
    CHK(S->oven.on == 0, "C 读 b_sim：脚本已停加热断电(末段散热属预期)");

    /* ---- 5. 脚本命令：批量 4 件（第 3 件注入 NG，恒温批其余 PASS） ---- */
    printf("-- [C] 触发脚本命令 run_lot 4 --\n");
    CHK(Big_RunLine("run_lot 4", NULL) == 0, "run_lot 4 执行完成");
    CHK(CapHas("BATCH-DONE"), "批次脚本完成标记");
    CHK(S->lot.total == 4 && S->lot.ok == 3 && S->lot.ng == 1,
        "C 读 b_sim：批次 4 件 = 3 良 + 1 不良");
    CHK(S->lot.lot == 0, "C 读 b_sim：lot_end 已关闭本批");

    /* ---- 6. 脚本命令：诊断（打印全状态 + env 参数） ---- */
    printf("-- [C] 触发脚本命令 diagnose --\n");
    CHK(Big_RunLine("diagnose", NULL) == 0, "diagnose 执行完成");
    CHK(CapHas("tgt=60"), "脚本 ${} 读到 env 参数 tgt=60");

    /* ---- 7. 忙时拒绝（脚本命令正在执行时再触发 → busy） ---- */
    printf("-- [C] busy 冲突演示 --\n");
    {
        uint8_t r1 = SCL_Scmd_RunText("selftest");      /* 触发一段（会跑完） */
        uint8_t r2 = SCL_Scmd_RunText("profile 1");     /* 运行中再触发 → 拒 */
        CHK((r1 == 1u) && (r2 == 0u), "脚本执行中重复触发被拒绝(busy)");
        Big_Drive();
    }

    /* ---- 8. C 与脚本长期混合后汇总统计 ---- */
    printf("-- [C] 汇总 --\n");
    {
        CHK(S->lot.ok == 3 && S->lot.ng == 1 && S->lot.total == 4,
            "累计统计只来自 run_lot(4 件)");
        printf("     环境运行参数已由脚本消费（见上 tgt=60 sweep=120）\n");
    }

    /* ---- 9. 动态缓存：当前/峰值/max/变量 GC/僵尸 GC ---- */
    printf("-- [C] 动态缓存统计与 GC --\n");
    CHK(Big_RunLine("cache", NULL) == 0, "cache 查询完成");
    CHK(CapHas("cache current=") && CapHas("peak=") && CapHas("alloc="),
        "cache 输出当前/峰值/分配统计");
    CHK(Big_RunLine("cache max", NULL) == 0, "cache max 查询完成");
    CHK(Big_RunLine("cache gc", NULL) == 0, "cache gc 完成");
    CHK(CapHas("gc=") && CapHas("zombie="), "cache gc 输出回收统计");
    CHK(Big_RunLine("cache zombie", NULL) == 0, "cache zombie 完成");

    printf("\n==== big_demo 自测汇总: PASS=%d FAIL=%d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
