/**
  ******************************************************************************
  * @file    mcu_boot_sim.c
  * @brief   MCU main 骨架的 PC 无板自检（headless，非交互）
  *
  *          与 example/mcu_template/ 里的 STM32 模板(mcu_main_template.c) 走同一条
  *          “集成时序”：Init → env 默认装载 → 上电自检脚本 → 改 env → 固化 →
  *          模拟重启 Load 恢复。先在 PC 上把这条逻辑跑绿，再上板只换"串口/存储"。
  *
  *          编译：
  *            gcc -pipe -O2 -Wall -Wextra -I scl/Inc -I scl/Src -I example \
  *                scl/Src/scl.c scl/Src/scl_var.c scl/Src/scl_env.c \
  *                example/scl_port.c example/demo_cmds.c \
  *                example/mcu_template/mcu_boot_sim.c -o build/mcu_boot_sim
  ******************************************************************************
  */

#include <stdio.h>
#include <string.h>

#include "scl.h"
#include "scl_port.h"
#include "scl_port.h"

/* ==================== 模拟"用户自己的存储区"（上板换成 Scl_Store_Write/Read） ==================== */
static uint8_t s_store[512];
static int     s_store_len = 0;

/* env 默认配置（与 mcu_main_template.c 一致） */
static const scl_env_def_t s_env_defs[] = {
    { "mode", SCL_T_STR,  "auto"    },
    { "baud", SCL_T_INT,  "115200"  },
    { "log",  SCL_T_FLAG, "-v"      },
};

static int s_fail = 0;

static void Check(int cond, const char *msg)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) { s_fail++; }
}

static void RunToIdle(const char *chain)
{
    if (SCL_Run(chain) != 0u)
    {
        while (!SCL_Idle()) { SCL_Loop(); }
    }
}

/* ==================== 集成时序（对应 mcu_main_template.c 的 App_* ） ==================== */

static void App_Boot_LoadEnv(void)
{
    /* 从"用户存储"读回固化值；无数据/坏数据 → Reset 回退默认 */
    Scl_Env_RegisterDefault(s_env_defs, 3);
    if (s_store_len > 0)
    {
        Scl_Env_Load(s_store, s_store_len);
    }
    if (Scl_Env_Count() == 0)
    {
        Scl_Env_Reset();
    }
}

static void App_SelfTest(void)
{
    /* 上电自检脚本（真机把 boot.s2c 编译成 const 后用 SCL_RunProg 执行，语义相同） */
    char cap[512];
    Scl_CapBegin(cap, sizeof(cap));
    RunToIdle("var const int LIM=5;var int r=0;var int sum=0;iadd r LIM r;iadd r 1 r;"
              "iadd sum 1 sum;iadd sum 2 sum;iadd sum 3 sum;"
              "ieq r 6;jump -a C1;echo boot-const-fail;jump C2;label C1;echo boot-const-ok;"
              "label C2;ieq sum 6;jump -a C3;echo boot-sum-fail;jump C4;label C3;echo boot-sum-ok;"
              "label C4;echo boot-done");
    Scl_CapEnd();
    Check(strstr(cap, "boot-const-ok") != NULL &&
          strstr(cap, "boot-sum-ok")   != NULL &&
          strstr(cap, "boot-done")     != NULL &&
          strstr(cap, "fail")          == NULL,
          "上电自检脚本（const 语义）全通过");
}

static void App_ConfigPersist(void)
{
    uint8_t snap[512];
    int len;

    /* 运行期脚本读到 env 默认值 */
    {
        char cap[256];
        Scl_CapBegin(cap, sizeof(cap));
        RunToIdle("echo baud=${baud}");
        Scl_CapEnd();
        Check(strstr(cap, "echo baud=115200") != NULL, "env 默认值可被脚本读到");
    }

    /* 用户改动 env → 固化到"用户存储" */
    Check(Scl_Env_Set("baud", SCL_T_INT, "9600") == 0, "运行期修改 env(baud=9600)");
    len = Scl_Env_Save(snap, (int)sizeof(snap));
    Check(len > 0, "Scl_Env_Save 序列化成功");
    memcpy(s_store, snap, (size_t)len);
    s_store_len = len;

    /* 模拟重启：清空缓冲 → 从"用户存储"恢复 */
    Scl_Env_FreeAll();
    Check(Scl_Env_Count() == 0, "模拟重启后 env 缓冲为空");
    App_Boot_LoadEnv();
    Check(strcmp(SCL_VarGet("baud"), "9600") == 0,
          "重启 Load 恢复：固化值 9600 优先于默认 115200");
    Check(strcmp(SCL_VarGet("mode"), "auto") == 0, "未改动项保持默认");

    /* 坏存储 → Load 拒绝（不清入）→ Reset 回退默认 */
    Scl_Env_FreeAll();
    Check(Scl_Env_Load(s_store, 3) == -1, "坏存储被 Load 拒绝");
    Check(Scl_Env_Count() == 0, "坏存储后缓冲为空（拒绝不清入）");
    Check(Scl_Env_Reset() == 3, "坏存储后 Reset 回退默认装载");
    Check(strcmp(SCL_VarGet("baud"), "115200") == 0, "坏存储后回退默认配置");
}

int main(void)
{
    printf("=== SCL MCU main 骨架 · PC 无板自检 ===\n");
    SCL_Init();
    /* 命令由注册表自动注册（SCL_Init 内部；见 scl/cmd/scl_cmd_list.c） */

    App_Boot_LoadEnv();          /* main: App_Scl_Init 的 env 部分 */
    App_SelfTest();              /* main: SCL_RunProg(&scl_boot_prog) 的等价验证 */
    App_ConfigPersist();         /* main: 周期固化 env + 重启恢复 */

    printf("===== 汇总 =====\nPASS=%d  FAIL=%d\n",
           (s_fail == 0) ? 8 : 8 - s_fail, s_fail);
    return (s_fail == 0) ? 0 : 1;
}
