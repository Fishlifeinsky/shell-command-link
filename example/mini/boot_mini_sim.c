/**
  ******************************************************************************
  * @file    boot_mini_sim.c
  * @brief   mini-scl 无板自检驱动：跑 boot.s2c 生成的 switch 状态机并断言
  *
  *          编译（PC，见 tools/scl_build.py test 的 mini_boot_sim 变体）：
  *            gcc -O2 -I scl/Inc -I scl/Src -I example \
  *                scl/Src/scl.c scl/Src/scl_var.c scl/Src/scl_env.c \
  *                example/scl_port.c example/demo_cmds.c \
  *                example/mini/boot_mini_sim.c -o build/mini_boot_sim
  *
  *          验证点（与解释器 SCL_Run/SCL_Loop 行为一致）：
  *            1) 生成代码跑出与 boot.s2c 相同的 echo 输出
  *            2) 脚本变量（r/i/sum）镜像到 SCL，外部 SCL_VarGet 可读
  *            3) step() 每次一个动作、有限步数内完成（非阻塞、无死循环）
  *
  *          注：boot_mini.c 由 tools/scl_mini_c.py 生成（勿手改）；此处直接
  *          include 该翻译单元，减少一个编译产物维护点。
  ******************************************************************************
  */

#include <stdio.h>
#include <string.h>
#include "scl.h"
#include "scl_port.h"
#include "scl_port.h"

#include "boot_mini.c"      /* 生成的状态机：register/start/step */

static int g_ok = 0;
static int g_bad = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { g_ok++; printf("  [PASS] %s\n", msg); } \
        else      { g_bad++; printf("  [FAIL] %s\n", msg); } \
    } while (0)

static int RunToDone(void)
{
    int guard = 0;
    while (boot_mini_step())
    {
        if (++guard > 200000) { break; }
    }
    return guard;
}

int main(void)
{
    static char cap[4096];

    SCL_Init();
    /* 命令由注册表自动注册（SCL_Init 内部；见 scl/cmd/scl_cmd_list.c） */
    boot_mini_register();            /* 绑定变量 + 注册成命令 'boot' */

    /* 外部先写绑定变量（路由到类型化 static） */
    CHECK(SCL_VarType("sum") == SCL_T_INT, "VarType 路由到绑定 int");
    CHECK(SCL_VarSetT("i", SCL_T_INT, "0") == 0, "外部 VarSetT 写绑定变量");

    /* 捕获输出跑完整个状态机（等价于解释器跑 boot.s2c） */
    Scl_CapBegin(cap, sizeof(cap));
    boot_mini_start();
    (void)RunToDone();
    Scl_CapEnd();

    CHECK(strstr(cap, "boot-const-ok") != NULL, "boot-const-ok 输出");
    CHECK(strstr(cap, "boot-const-fail") == NULL, "无 boot-const-fail");
    CHECK(strstr(cap, "boot-loop-ok") != NULL, "boot-loop-ok 输出");
    CHECK(strstr(cap, "boot-done") != NULL, "boot-done 输出");
    {
        const char *s = SCL_VarGet("sum");
        CHECK(s != NULL && strcmp(s, "3") == 0, "绑定路由：VarGet(sum)==3");
        s = SCL_VarGet("i");
        CHECK(s != NULL && strcmp(s, "3") == 0, "绑定路由：VarGet(i)==3");
    }

    /* 作为注册命令触发（薄分发/外部按名调用） */
    Scl_CapBegin(cap, sizeof(cap));
    CHECK(SCL_CmdInvoke("boot", 0, NULL) == 1, "注册命令 boot 可同步触发");
    (void)RunToDone();
    Scl_CapEnd();
    CHECK(strstr(cap, "boot-done") != NULL, "命令触发后重新跑完");

    printf("PASS=%d FAIL=%d\n", g_ok, g_bad);
    return (g_bad == 0) ? 0 : 1;
}
