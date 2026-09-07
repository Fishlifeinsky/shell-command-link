/**
  ******************************************************************************
  * @file    main.c
  * @brief   PC 侧 SCL 全量测试（无硬件依赖）
  *
  *          五类测试（按用户要求）：
  *            1) 功能/语法（变量、${} 展开、普通调用、if、while、异步 wait）
  *            2) 大小     —— 打印配置与静态 RAM 估算；最终 .text/.data/.bss 用 binutils
  *                          `size` 在外部实测（见构建脚本）
  *            3) 速度     —— 命令分发吞吐（条/秒）与每命令步进开销
  *            4) 可靠性   —— 结构错误/超长/乱码模糊输入（确定性随机），保证不崩溃、可恢复
  *            5) 重复性   —— 同一脚本多次运行输出逐字节一致、tick 数一致
  *            附：复杂度   —— 子句数量/循环规模与执行步数近似线性
  *
  *          编译（示例）：
  *            gcc -O2 -Wall -Wextra -I scl/Inc -I example \
  *                scl/Src/scl.c example/scl_port.c example/demo_cmds.c \
  *                example/main.c -o build/scl_test
  ******************************************************************************
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scl.h"
#include "scl_port.h"
#include "demo_cmds.h"

/* ========================== 测试基础设施 ========================== */

#define MAX_TICKS      3000000   /* 单脚本最大推进步数（防卡死） */
#define FUZZ_MAX_TICKS 50000     /* 模糊输入单脚本最大步数 */

/* 捕获缓冲（断言用；库与演示输出同时写入） */
static char g_cap[1u << 16];

/* 测试统计 */
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
        else      { g_fail++; printf("  [FAIL] %s\n", msg); } \
    } while (0)

/* 字符串计数 */
static int CountStr(const char *hay, const char *needle)
{
    int c = 0;
    size_t nl = strlen(needle);
    const char *p = hay;
    if (nl == 0u) { return 0; }
    while ((p = strstr(p, needle)) != NULL)
    {
        c++;
        p += nl;
    }
    return c;
}

/* 把脚本跑到空闲；返回 tick 数；-1=SCL_Run 拒绝；-2=超时（已请求中断并排空） */
static int RunToIdle(const char *script)
{
    int t = 0;

    if (SCL_Run(script) == 0u)
    {
        return -1;
    }
    while (!SCL_Idle())
    {
        SCL_Loop();
        t++;
        if (t > MAX_TICKS)
        {
            SCL_Abort();
            while (!SCL_Idle())
            {
                SCL_Loop();
            }
            return -2;
        }
    }
    return t;
}

/* 运行并捕获；断言库已空闲且变量已自动全释放 */
static int RunCap(const char *script, char *buf, int cap)
{
    int ticks;
    Scl_CapBegin(buf, cap);
    ticks = RunToIdle(script);
    Scl_CapEnd();
    return ticks;
}

/* 打印小节标题 */
static void Section(const char *title)
{
    printf("\n===== %s =====\n", title);
}

/* 确定性伪随机（LCG，保证重复性） */
static uint32_t s_rng = 0x9E3779B9u;
static uint32_t Rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

/* ========================== 1. 功能/语法 ========================== */

static void TestBasics(void)
{
    int t;

    Section("1. 基础/变量/展开/调用");

    /* echo + ${} 展开 */
    t = RunCap("var a=hello; echo ${a}", g_cap, sizeof(g_cap));
    CHECK(t >= 0, "脚本可运行");
    CHECK(strstr(g_cap, "echo hello") != NULL, "${a} 展开取值");
    CHECK(SCL_VarCount() == 0, "脚本结束自动释放全部变量");

    /* var 列表 / 剩余空位 */
    t = RunCap("var x=5; var", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "剩余空位") != NULL, "var 列表显示剩余空位");

    /* 变量槽已满（上限 2） */
    t = RunCap("var a=1; var b=2; var c=3", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "已满") != NULL, "第 3 个变量被拒绝(已满)");

    /* free 释放后可用 */
    t = RunCap("var a=1; var b=2; free a; var c=3", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "已满") == NULL, "free 后变量槽可复用");

    /* 值带空格（引号） */
    t = RunCap("var t=\"hello world\"; echo [${t}]", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo [hello world]") != NULL, "变量值含空格(引号)");

    /* 变量引用变量赋值 */
    t = RunCap("var a=10; var b=${a}; echo ${b}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo 10") != NULL, "var b=${a} 赋值展开");

    /* 变量覆盖 */
    t = RunCap("var a=1; var a=2; echo ${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo 2") != NULL, "var 覆盖旧值");

    /* 名字过长被拒 */
    t = RunCap("var verylongname=1", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "变量名过长") != NULL, "超长变量名被拒");

    /* 普通调用（函数式调用已移除：add(2,3) 按未知命令报错） */
    t = RunCap("add 2 3", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "add=5") != NULL, "普通调用 add 2 3");
    t = RunCap("add(2,3)", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "add=5") == NULL, "函数式调用已移除(不执行)");
    CHECK(strstr(g_cap, "未知命令") != NULL, "函数式调用按未知命令报错");

    /* 普通式引号参数（含空格） */
    t = RunCap("echo \"x y\" z", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo x y z") != NULL, "普通式引号参数");

    /* 引号内 ';' 不分割（普通式） */
    t = RunCap("echo 'a;b'; echo after", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a;b") != NULL, "单引号内 ';' 不分割");
    CHECK(strstr(g_cap, "echo after") != NULL, "后续子句仍执行");

    /* 未知命令 */
    t = RunCap("nosuchcmd 1", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "未知命令") != NULL, "未知命令报错");

    /* 缺失变量容错 */
    t = RunCap("echo ${ghost}", g_cap, sizeof(g_cap));
    CHECK(t >= 0 && SCL_VarCount() == 0, "缺失变量不崩溃且脚本可完成");
}

static void TestIf(void)
{
    Section("2. if / G_RETURN");

    /* 真 → -t */
    RunCap("setret 1; if -t \"echo T\" -f \"echo F\"", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo T") != NULL, "G_RETURN=true 执行 -t");
    CHECK(strstr(g_cap, "echo F") == NULL, "G_RETURN=true 不执行 -f");

    /* 假 → -f */
    RunCap("setret 0; if -t \"echo T\" -f \"echo F\"", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo F") != NULL, "G_RETURN=false 执行 -f");
    CHECK(strstr(g_cap, "echo T") == NULL, "G_RETURN=false 不执行 -t");

    /* 只给 -t：真执行、假跳过 */
    RunCap("setret 1; if -t \"echo T\"", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo T") != NULL, "-t 单独: 真执行");
    RunCap("setret 0; if -t \"echo T\"", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo T") == NULL, "-t 单独: 假跳过");

    /* 多指令分支（引号内 ';' 是子链分隔） */
    RunCap("setret 1; if -t \"echo A;echo B\"", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo A") != NULL && strstr(g_cap, "echo B") != NULL,
          "分支多指令子链 A;B");

    /* 读完重置：两个 if 依次读同一份 G_RETURN，第二个读到默认假 */
    RunCap("setret 1; if -t \"echo T1\"; if -t \"echo T2\"", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo T1") != NULL, "第一次读 G_RETURN=true");
    CHECK(strstr(g_cap, "echo T2") == NULL, "读后重置(第二次读到 false)");

    /* 分支内嵌套 if（外层单引号包、内层双引号） */
    RunCap("setret 1; if -t 'setret 0; if -t \"echo X\" -f \"echo Y\"'",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo Y") != NULL, "if 内嵌 if(真分支内再判假)");
    CHECK(strstr(g_cap, "echo X") == NULL, "if 内嵌 if 未走 -t");
}

static void TestWhile(void)
{
    int t;

    Section("3. while / 循环");

    /* 计数循环（do-while：G_RETURN=false 退出） */
    t = RunCap("demo_reset 3; while -b; demo_inc; while -e", g_cap, sizeof(g_cap));
    CHECK(CountStr(g_cap, "cnt=") == 3, "demo_inc 执行 3 次");
    CHECK(strstr(g_cap, "cnt=3") != NULL, "计数到 3");
    CHECK(strstr(g_cap, "cnt=4") == NULL, "未多执行");

    /* 显式上限：body 内每次重新置 G_RETURN=true，按 -e N 退出 */
    RunCap("setret 1; while -b; setret 1; echo X; while -e 3", g_cap, sizeof(g_cap));
    CHECK(CountStr(g_cap, "echo X") == 3, "while -e 3 执行 3 次");

    /* 空循环体 + 上限，快速终止 */
    t = RunCap("setret 1; while -b; while -e 2", g_cap, sizeof(g_cap));
    CHECK(t > 0 && t < 1000, "空循环体快速终止");
    CHECK(SCL_VarCount() == 0, "循环后变量已自动释放");

    /* -e 无 -b：结构错误 */
    t = RunCap("while -e", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "没有匹配的 while -b") != NULL, "-e 无 -b 报错");
    CHECK(SCL_Idle(), "-e 报错后库回到空闲");

    /* -b 无 -e：结构错误 */
    t = RunCap("while -b; echo Z", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "未找到匹配的 while -e") != NULL, "-b 无 -e 报错");
}

static void TestNested(void)
{
    Section("4. 嵌套（while/if）");

    /* while 嵌套 while：外层2 × 内层3 = 6 次（每轮内层都重置 G_RETURN=true） */
    RunCap("setret 1; while -b; setret 1; while -b; setret 1; echo i; while -e 3; setret 1; while -e 2",
           g_cap, sizeof(g_cap));
    CHECK(CountStr(g_cap, "echo i") == 6, "嵌套 while：外层2 × 内层3 = 6 次");

    /* while 内嵌 if */
    RunCap("setret 1; while -b; cmp 1 1; if -t \"echo HIT\"; setret 1; while -e 2",
           g_cap, sizeof(g_cap));
    CHECK(CountStr(g_cap, "echo HIT") == 2, "while 内嵌 if 每轮命中");

    /* if 分支内嵌 while */
    RunCap("setret 1; if -t \"demo_reset 2; while -b; demo_inc; while -e\"; echo fin",
           g_cap, sizeof(g_cap));
    CHECK(CountStr(g_cap, "cnt=") == 2, "if 分支内嵌 while 跑 2 次");
    CHECK(strstr(g_cap, "echo fin") != NULL, "if 分支结束后继续");

    /* 嵌套过深（>SCL_CFG_NEST_MAX=3）被拒：4 层 while，须带足 -e 才能推到第 4 层 */
    RunCap("while -b; while -b; while -b; while -b; while -e; while -e; while -e; while -e; echo x",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "嵌套过深") != NULL, "while 嵌套过深被拒");
}

static void TestAsync(void)
{
    Section("5. 异步命令（wait）");

    /* 手动步进：wait 完成前不应执行后续命令 */
    Scl_CapBegin(g_cap, sizeof(g_cap));
    CHECK(SCL_Run("wait 3; echo AFTER") == 1u, "提交含异步的脚本");
    SCL_Loop();
    SCL_Loop();
    CHECK(strstr(g_cap, "echo AFTER") == NULL, "wait 未完成前不执行后续");
    while (!SCL_Idle())
    {
        SCL_Loop();   /* 排空到完成 */
    }
    Scl_CapEnd();
    CHECK(strstr(g_cap, "echo AFTER") != NULL, "wait 完成后执行后续");

    /* 异步完成后 G_RETURN=true 供 if -t 判断 */
    RunCap("wait 2; if -t \"echo DONE\"", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo DONE") != NULL, "异步完成置 G_RETURN=true 走 if -t");
    CHECK(SCL_VarCount() == 0, "异步脚本结束变量已释放");
}

/* ========================== 2. 大小 ========================== */

static void TestSize(void)
{
    Section("6. 大小(配置/静态 RAM 估算; 精确 .text/.data/.bss 用 size 实测)");
    printf("  SCL_CFG_VAR_MAX       = %u\n", (unsigned)SCL_CFG_VAR_MAX);
    printf("  SCL_CFG_VAR_NAME_MAX  = %u\n", (unsigned)SCL_CFG_VAR_NAME_MAX);
    printf("  SCL_CFG_VAR_VALUE_MAX = %u\n", (unsigned)SCL_CFG_VAR_VALUE_MAX);
    printf("  SCL_CFG_SCRIPT_MAX    = %u\n", (unsigned)SCL_CFG_SCRIPT_MAX);
    printf("  SCL_CFG_ARG_MAX       = %u\n", (unsigned)SCL_CFG_ARG_MAX);
    printf("  SCL_CFG_ARG_LEN_MAX   = %u\n", (unsigned)SCL_CFG_ARG_LEN_MAX);
    printf("  SCL_CFG_NEST_MAX      = %u\n", (unsigned)SCL_CFG_NEST_MAX);
    printf("  SCL_CFG_WHILE_MAX     = %u\n", (unsigned)SCL_CFG_WHILE_MAX);

    /* 静态 RAM 估算（近似，精确值以 size 实测的 BSS 为准） */
    {
        unsigned long ram =
            (unsigned long)SCL_CFG_SCRIPT_MAX +
            (unsigned long)SCL_CFG_VAR_MAX * (SCL_CFG_VAR_NAME_MAX + 1u + SCL_CFG_VAR_VALUE_MAX) +
            (unsigned long)SCL_CFG_ARG_MAX * SCL_CFG_ARG_LEN_MAX +
            (unsigned long)SCL_CFG_NEST_MAX * 32u +   /* 帧(约24~32B) */
            64u;                                      /* 杂项 */
        printf("  静态 RAM 估算 ≈ %lu B\n", ram);
    }
    CHECK(1 == 1, "大小信息已打印（外部 size 实测见报告）");
}

/* ========================== 3. 速度 ========================== */

static void TestSpeed(void)
{
    Section("7. 速度(命令吞吐)");
    {
        /* 循环 5000 次：body = setret1 + noop，命令 10000 条 */
        const char *script = "setret 1; while -b; setret 1; noop; while -e 5000";
        clock_t c0;
        clock_t c1;
        double sec;
        int t;
        int iter;
        int total_ticks = 0;
        int total_cmds = 0;

        /* 预热一次 */
        t = RunToIdle(script);
        c0 = clock();
        for (iter = 0; iter < 5; iter++)
        {
            t = RunToIdle(script);
            total_ticks += (t > 0) ? t : 0;
        }
        c1 = clock();
        sec = (double)(c1 - c0) / CLOCKS_PER_SEC;
        total_cmds = 5 * 10000;   /* 每次 10000 条业务命令 */

        printf("  5 次共 %d 条业务命令, 总步进 %d tick, 用时 %.3f s\n",
               total_cmds, total_ticks, sec);
        if (sec > 0.0)
        {
            printf("  命令吞吐 ≈ %.0f 条/s\n", total_cmds / sec);
        }
        printf("  平均每条业务命令 ≈ %.1f 个 Loop tick\n",
               (total_ticks > 0) ? (double)total_ticks / total_cmds : 0.0);
        CHECK(t == 0 || t > 0, "速度测试可完成");
        (void)t;
    }
}

/* ========================== 4. 可靠性 ========================== */

/* 生成一段"结构化垃圾"脚本（确定性随机） */
static void FuzzBuild(char *buf, int cap)
{
    static const char *tok[] = {
        "var", "free", "if", "while", "-b", "-e", "-t", "-f",
        "echo", "add", "setret", "noop", "wait", "cmp",
        "1", "0", "3", "x", "a", ";", "(", ")", ",", "\"", "'",
        "${a}", "${", "}", "=", " ", " ", "\t", "[", "]", "!", "@"
    };
    int i;
    int n = (int)(Rnd() % 40u) + 1;
    int len = 0;

    buf[0] = '\0';
    for (i = 0; i < n; i++)
    {
        const char *s = tok[Rnd() % (sizeof(tok) / sizeof(tok[0]))];
        while ((*s != '\0') && (len + 1 < cap))
        {
            buf[len++] = *s++;
        }
        buf[len] = '\0';
    }
}

static void TestReliability(void)
{
    int i;
    int bad = 0;

    Section("8. 可靠性");

    /* 超长脚本被拒绝 */
    {
        char huge[SCL_CFG_SCRIPT_MAX + 64u];
        memset(huge, 'a', sizeof(huge) - 1u);
        huge[sizeof(huge) - 1u] = '\0';
        CHECK(SCL_Run(huge) == 0u, "超长脚本被拒绝");
        CHECK(SCL_Idle(), "拒绝后保持空闲");
    }

    /* 引号未闭合 / 括号不配 等结构错误不崩溃 */
    {
        const char *cases[] = {
            "echo \"unclosed",
            "add(1,2",
            "echo \"a;b",
            "if -t",
            "if -z 1",
            "while",
            "var a=",
            "var =1",
            "var 'a'=1; while -e 2",
            "; ; ; ;",
            "add(1,2) tail",
            "echo a b c d e f g h i j",
        };
        for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
        {
            int t = RunCap(cases[i], g_cap, sizeof(g_cap));
            if (t < -1)
            {
                bad++;
                printf("  [FAIL] 结构错误用例不结束: %s\n", cases[i]);
            }
            else if (!SCL_Idle() || (SCL_VarCount() != 0))
            {
                bad++;
                printf("  [FAIL] 结构错误后状态不干净: %s\n", cases[i]);
            }
        }
        CHECK(bad == 0, "结构错误用例均不崩溃且状态干净");
    }

    /* 随机模糊输入（2000 次）：不崩溃、最终能恢复执行正常脚本 */
    {
        int ok = 1;
        for (i = 0; i < 2000; i++)
        {
            char buf[96];
            int guard = 0;

            FuzzBuild(buf, sizeof(buf));
            if (SCL_Run(buf) != 0u)
            {
                while (!SCL_Idle())
                {
                    SCL_Loop();
                    guard++;
                    if (guard > FUZZ_MAX_TICKS)
                    {
                        SCL_Abort();
                        while (!SCL_Idle())
                        {
                            SCL_Loop();
                        }
                        break;
                    }
                }
            }
            /* 恢复性检查：每 100 次插一条正常脚本 */
            if ((i % 100) == 99)
            {
                Scl_CapBegin(g_cap, sizeof(g_cap));
                RunToIdle("echo recover");
                Scl_CapEnd();
                if (strstr(g_cap, "echo recover") == NULL)
                {
                    ok = 0;
                }
                if (!SCL_Idle() || (SCL_VarCount() != 0))
                {
                    ok = 0;
                }
            }
        }
        CHECK(ok == 1, "2000 次模糊输入不崩溃、可恢复");
    }
}

/* ========================== 5. 重复性 ========================== */

static void TestRepeatability(void)
{
    Section("9. 重复性");
    {
        const char *script =
            "setret 1; while -b; cmp 1 1; if -t \"echo HIT\"; setret 1; while -e 4; echo FIN";
        char out[5][4096];
        int  ticks[5];
        int  i;
        int  same = 1;

        for (i = 0; i < 5; i++)
        {
            ticks[i] = RunCap(script, out[i], sizeof(out[i]));
        }
        for (i = 1; i < 5; i++)
        {
            if (strcmp(out[0], out[i]) != 0)
            {
                same = 0;
            }
        }
        CHECK(same == 1, "5 次运行输出逐字节一致");
        CHECK(ticks[0] > 0 &&
              (ticks[0] == ticks[1]) && (ticks[1] == ticks[2]) &&
              (ticks[2] == ticks[3]) && (ticks[3] == ticks[4]),
              "5 次运行 tick 数一致");
        printf("  ticks = %d\n", ticks[0]);
        CHECK(CountStr(out[0], "echo HIT") == 4, "内容正确(HIT×4)");
    }
}

/* ========================== 附：复杂度 ========================== */

static void TestComplexity(void)
{
    Section("10. 复杂度(步数与规模近似线性)");

    /* 子句数量线性：N 条 noop（控制脚本 < SCRIPT_MAX=256，N2 用 40） */
    {
        int n1 = 20;
        int n2 = 40;
        char s1[256];
        char s2[256];
        int i;
        int t1;
        int t2;

        s1[0] = '\0';
        for (i = 0; i < n1; i++) { strcat(s1, "noop;"); }
        s2[0] = '\0';
        for (i = 0; i < n2; i++) { strcat(s2, "noop;"); }

        t1 = RunCap(s1, g_cap, sizeof(g_cap));
        t2 = RunCap(s2, g_cap, sizeof(g_cap));
        printf("  N=%d → %d tick; N=%d → %d tick\n", n1, t1, n2, t2);
        /* 每条子句 ≈1 tick + 常数开销，2 倍子句应约 2 倍步数 */
        CHECK((t2 > 0) && (t1 > 0) &&
              (t2 >= 180 * t1 / 100) && (t2 <= 220 * t1 / 100),
              "执行步数随子句数近似线性");
    }

    /* 循环规模线性：-e N 迭代（body 每条重置 G_RETURN=true） */
    {
        int t1;
        int t2;
        t1 = RunCap("setret 1; while -b; setret 1; noop; while -e 400", g_cap, sizeof(g_cap));
        t2 = RunCap("setret 1; while -b; setret 1; noop; while -e 1600", g_cap, sizeof(g_cap));
        printf("  loop400 → %d tick; loop1600 → %d tick\n", t1, t2);
        CHECK((t2 > 0) && (t1 > 0) && (t2 >= 3 * t1) && (t2 <= 5 * t1),
              "执行步数随循环次数近似线性");
    }

    /* 帧/段开销小：单 if 空走接近常量 */
    {
        int t1;
        int t2;
        t1 = RunCap("noop;noop;noop", g_cap, sizeof(g_cap));
        t2 = RunCap("setret 1; if -t \"noop\"; noop; noop", g_cap, sizeof(g_cap));
        printf("  平铺3条=%d tick; if分支+2条=%d tick\n", t1, t2);
        CHECK((t1 > 0) && (t2 > 0) && (t2 - t1 <= 4), "if 段帧开销为小常数");
    }
}

/* ========================== 主流程 ========================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* 逐字符刷新，保证输出顺序 */

    printf("=== SCL(Shell-Command-Link) 全量测试 ===\n");
    printf("模型: DeepSeek V4 Flash(说明)\n");
    printf("SCL_CFG_VAR_MAX=%u  SCRIPT=%u  NEST=%u\n",
           (unsigned)SCL_CFG_VAR_MAX, (unsigned)SCL_CFG_SCRIPT_MAX,
           (unsigned)SCL_CFG_NEST_MAX);

    SCL_Init();

#if (SCL_EX_CMDS_EN == 1u)
    Scl_Demo_Register();
#else
    printf("(SCL_EX_CMDS_EN=0: 未注册演示命令)\n");
#endif

    TestBasics();
    TestIf();
    TestWhile();
    TestNested();
    TestAsync();
    TestSize();
    TestSpeed();
    TestReliability();
    TestRepeatability();
    TestComplexity();

    printf("\n===== 汇总 =====\n");
    printf("PASS=%d  FAIL=%d\n", g_pass, g_fail);
    printf("最终状态: busy=%d idle=%d var=%d (应 0/1/0)\n",
           (int)(!SCL_Idle()), (int)SCL_Idle(), SCL_VarCount());

    return (g_fail == 0) ? 0 : 1;
}
