/**
  ******************************************************************************
  * @file    main.c
  * @brief   PC 侧 SCL 全量测试（v0.2：类型化参数缓存 + bool/int/flag/string 变量 + 内置运算）
  *
  *          覆盖：
  *            1) 基础：变量、${} 展开、普通式命令、引号参数、编译拒绝
  *            1b) 类型化变量（bool/int/flag/string）与类型校验
  *            1c) 内置 int/bool 运算指令（iadd/idiv/ineg/ilt/band/btest 等）
  *            2) label/jump：无条件跳、jump -a 条件跳（真跳/假不跳/读后清零）
  *            3) do-while 循环（label + 条件 jump）终止性
  *            4) 异步命令 wait（指令边界等待）
  *            5) 可靠性：结构错误、随机模糊输入不崩溃可恢复
  *            6) 重复性、速度与大小信息
  *
  *          编译（示例）：
  *            gcc -O2 -Wall -Wextra -I scl/Inc -I example \
  *                scl/Src/scl.c example/scl_port.c example/demo_cmds.c \
  *                example/main.c -o build/scl_test
  ******************************************************************************
  */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "scl.h"
#include "scl_port.h"
#include "demo_cmds.h"

/* ========================== 测试基础设施 ========================== */

#define MAX_TICKS      3000000
#define FUZZ_MAX_TICKS 30000

static char g_cap[1u << 16];
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
        else      { g_fail++; printf("  [FAIL] %s\n", msg); } \
    } while (0)

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

static int RunToIdle(const char *chain)
{
    int t = 0;
    if (SCL_Run(chain) == 0u)
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

static int RunCap(const char *chain, char *buf, int cap)
{
    int ticks;
    Scl_CapBegin(buf, cap);
    ticks = RunToIdle(chain);
    Scl_CapEnd();
    return ticks;
}

static void Section(const char *title)
{
    printf("\n===== %s =====\n", title);
}

/* 确定性伪随机 */
static uint32_t s_rng = 0x9E3779B9u;
static uint32_t Rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

/* ========================== 1. 基础 ========================== */

static void TestBasics(void)
{
    int t;
    Section("1. 基础/变量/命令");

    t = RunCap("var string a=hello; echo ${a}", g_cap, sizeof(g_cap));
    CHECK(t >= 0 && strstr(g_cap, "echo hello") != NULL, "${a} 展开取值");
    CHECK(SCL_VarCount() == 0, "脚本结束自动释放变量");

    t = RunCap("var int a=1;var int b=2;var int c=3;var int d=4;var int e=5",
               g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "已满") != NULL, "变量槽>4 被拒(执行校验)");

    t = RunCap("var int a=1;var int b=2;free a;var int c=3;echo ${c}",
               g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "已满") == NULL && strstr(g_cap, "echo 3") != NULL,
          "free 后槽可复用");

    t = RunCap("var string t=\"a b\"; echo [${t}]", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo [a b]") != NULL, "var 值含空格(引号)");

    t = RunCap("echo \"x y\" z", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo x y z") != NULL, "普通式引号参数");

    /* v0.3：以 '#' 开头的整句在编译期作为注释跳过 */
    t = RunCap("# full comment;echo ok;# lead comment;echo tail", g_cap, sizeof(g_cap));
    CHECK(CountStr(g_cap, "echo ok") == 1 && CountStr(g_cap, "echo tail") == 1,
          "整句 # 注释被跳过");

    /* 编译拒绝：未知命令 / label 重名 / 未定义 / 缺目标 */
    CHECK(SCL_Run("nosuch 1") == 0u, "未知命令被编译拒绝");
    CHECK(SCL_Idle(), "编译拒绝后保持空闲");
    CHECK(SCL_Run("label A;label A;echo x") == 0u, "label 重名被拒");
    CHECK(SCL_Run("jump Lx;echo y") == 0u, "jump 未定义 label 被拒");
    CHECK(SCL_Run("jump -a;echo y") == 0u, "jump 缺目标被拒");
    CHECK(SCL_Run("") == 0u, "空链被拒");
}

/* ========================== 1b. 类型化变量 ========================== */

static void TestVarsTyped(void)
{
    int t;
    Section("1b. 类型化变量 bool/int/flag/string");

    t = RunCap("var bool b=true;var int i=-12;var flag f=-x;var string s=abc;var",
               g_cap, sizeof(g_cap));
    CHECK(t >= 0 && strstr(g_cap, "b : bool = true") != NULL, "bool 变量声明与列表");
    CHECK(strstr(g_cap, "i : int = -12") != NULL, "int 变量（负数）");
    CHECK(strstr(g_cap, "f : flag = -x") != NULL, "flag 变量");
    CHECK(strstr(g_cap, "s : string = abc") != NULL, "string 变量");

    CHECK(SCL_VarSetT("k", SCL_T_INT, "42") == 0, "VarSetT int 设置");
    CHECK(SCL_VarType("k") == SCL_T_INT, "VarType 返回 int");
    CHECK(strcmp(SCL_VarGet("k"), "42") == 0, "VarGet 文本为 42");
    CHECK(SCL_VarSetT("k", SCL_T_INT, "zz") != 0, "VarSetT int 非数字拒绝");
    SCL_VarFree("k");
    CHECK(SCL_VarSet("z", "7") == 0 && SCL_VarType("z") == SCL_T_INT,
          "VarSet 自动推断 int");
    CHECK(SCL_VarSet("z", "true") == 0 && SCL_VarType("z") == SCL_T_BOOL,
          "VarSet 自动推断 bool(覆盖)");
    SCL_VarFree("z");

    t = RunCap("var int bad=zz;echo after", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "var: 参数错误") != NULL && strstr(g_cap, "echo after") != NULL,
          "int 值非数字拒绝且继续执行");
    t = RunCap("var flag ff=xx", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "var: 参数错误") != NULL, "flag 值需 -x 被拒");
    t = RunCap("var old=1", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "缺少类型") != NULL, "v0.1 缺类型 var 被提示");

    t = RunCap("var bool b=true;var int i=7;var flag f=-x;echo ${b}/${i}/${f}",
               g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo true/7/-x") != NULL, "${} 多类型变量文本展开");
}

/* ========================== 1c. 内置运算指令 ========================== */

static void TestArith(void)
{
    int t;
    Section("1c. 内置 int/bool 运算指令");

    RunCap("var int n=0;iadd n 1 n;echo n=${n}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo n=1") != NULL, "iadd 写回变量");

    RunCap("var int a=7;var int b=3;isub a b a;imul a 2 a;echo a=${a}",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=8") != NULL, "isub/imul 链式写回");

    RunCap("var int a=10;var int b=3;idiv a b a;imod b 2 b;echo a=${a} b=${b}",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=3 b=1") != NULL, "idiv/imod 写回");

    RunCap("var int a=5;ineg a a;echo a=${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=-5") != NULL, "ineg 取负写回");

    RunCap("var int x=5;ilt x 10;jump -a L;echo no;label L;echo yes",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo yes") != NULL && strstr(g_cap, "echo no") == NULL,
          "ilt 真→jump -a");
    RunCap("var int x=5;ieq x 6;jump -a L;echo ne;label L;echo end",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo ne") != NULL, "ieq 假不跳");

    RunCap("var bool p=true;var bool q=false;btest p;bnot p;jump -a L1;echo nf;label L1;echo end1",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo nf") != NULL, "bnot(p)=false 不跳");
    RunCap("var bool p=true;var bool q=false;btest p;band p q;jump -a L;echo andf;label L;echo end",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo andf") != NULL, "band(p,q)=false 不跳");
    RunCap("var bool p=true;bor p true;jump -a L;echo no;label L;echo oryes",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo oryes") != NULL && strstr(g_cap, "echo no") == NULL,
          "bor(p,true) 真→jump");

    t = RunCap("var int a=1;idiv a 0 a;echo after", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "除数为 0") != NULL && strstr(g_cap, "echo after") != NULL,
          "idiv 除 0 报错后继续");
    CHECK(SCL_VarCount() == 0 && SCL_Idle(), "运算错误后状态干净");
}

/* ========================== 1d. 位运算 / 进制字面量 / 步进保护 ========================== */

static void TestBitHex(void)
{
    Section("1d. 位运算 / 进制字面量 / 步进保护");

    RunCap("var int a=0xf;var int b=0b11;echo a=${a} b=${b}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=15 b=3") != NULL, "var 支持 0x/0b 字面量");
    RunCap("echo 0x10", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo 16") != NULL, "命令参数 0x 字面量 → int");

    RunCap("var int a=5;iand a 3 a;echo a=${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=1") != NULL, "iand (5&3=1)");
    RunCap("var int a=5;ixor a 3 a;echo a=${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=6") != NULL, "ixor (5^3=6)");
    RunCap("var int a=5;ior a 2 a;echo a=${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=7") != NULL, "ior  (5|2=7)");
    RunCap("var int a=1;shl a 4 a;echo a=${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=16") != NULL, "shl (1<<4=16)");
    RunCap("var int a=0x80000000;shr a 4 a;echo a=${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=134217728") != NULL, "shr 逻辑右移");
    RunCap("var int a=0xFFFFFFFF;inot a a;echo a=${a}", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo a=0") != NULL, "inot (~0xFFFFFFFF=0)");

    RunCap("label L;setret 1;jump -a L", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "步进超限") != NULL && SCL_Idle() && SCL_VarCount() == 0,
          "死循环被步进保护中断且状态干净");
}

/* ========================== 2. label / jump ========================== */

static void TestJump(void)
{
    Section("2. label / jump");

    RunCap("label L1;echo one;jump L2;echo two;label L2;echo three",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo one") != NULL, "无条件跳前执行");
    CHECK(strstr(g_cap, "echo two") == NULL, "无条件 jump 跳过中间");
    CHECK(strstr(g_cap, "echo three") != NULL, "跳到目标后继续");

    RunCap("setret 1;jump -a L1;echo no;label L1;echo yes", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo yes") != NULL, "jump -a 真跳执行目标");
    CHECK(strstr(g_cap, "echo no") == NULL, "jump -a 真跳不落中间");

    RunCap("setret 0;jump -a L1;echo fall;label L1;echo end", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo fall") != NULL, "jump -a 假不跳(顺序执行)");
    CHECK(strstr(g_cap, "echo end") != NULL, "jump -a 假不跳仍到后续");

    /* 读后清零：第一次 -a 读真并清零，第二次 -a 读到假 → 不跳 */
    RunCap("setret 1;jump -a L1;echo na;label L1;echo mid;jump -a L2;echo nb;label L2;echo fin",
           g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo mid") != NULL, "-a 读取后 G_RETURN 已用");
    CHECK(strstr(g_cap, "echo nb") != NULL, "第二次 -a 读到假(读后清零)");
    CHECK(strstr(g_cap, "echo na") == NULL, "第一次真跳未落中间");

    /* do-while 循环：label + demo_inc 条件跳，能终止 */
    {
        int tk = RunCap("demo_reset 2;label L;echo go;demo_inc;jump -a L;echo over",
                        g_cap, sizeof(g_cap));
        CHECK(tk > 0 && CountStr(g_cap, "echo go") == 2, "do-while body 执行 2 次");
        CHECK(strstr(g_cap, "echo over") != NULL, "循环终止到后续");
        CHECK(SCL_VarCount() == 0, "循环结束变量自动释放");
    }
}

/* ========================== 3. 异步命令 ========================== */

static void TestAsync(void)
{
    Section("3. 异步命令（wait）");

    Scl_CapBegin(g_cap, sizeof(g_cap));
    CHECK(SCL_Run("wait 3;echo AFTER") == 1u, "提交含异步链");
    SCL_Loop();
    SCL_Loop();
    CHECK(strstr(g_cap, "echo AFTER") == NULL, "wait 未完成前不执行后续");
    while (!SCL_Idle())
    {
        SCL_Loop();
    }
    Scl_CapEnd();
    CHECK(strstr(g_cap, "echo AFTER") != NULL, "wait 完成后执行后续");

    /* 异步完成置 G_RETURN=true，供 jump -a */
    RunCap("wait 1;jump -a L1;echo no;label L1;echo done", g_cap, sizeof(g_cap));
    CHECK(strstr(g_cap, "echo done") != NULL && strstr(g_cap, "echo no") == NULL,
          "异步完成后 G_RETURN=true 供 jump -a");
    CHECK(SCL_VarCount() == 0, "异步链结束变量已释放");
}

/* ========================== 4. 可靠性 ========================== */

static void FuzzBuild(char *buf, int cap)
{
    static const char *tok[] = {
        "var", "free", "help", "label", "jump", "-a", "-b",
        "echo", "add", "setret", "wait", "cmp", "noop",
        "1", "0", "3", "x", "a", "L1", "L2", ";", "\"", "'", "${a}",
        "=", " ", " ", "\t", "[", "]", "!", "@", "(", ")"
    };
    int i;
    int n = (int)(Rnd() % 30u) + 1;
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
    Section("4. 可靠性");
    {
        const char *cases[] = {
            "echo \"unclosed",
            "label A;label A;echo x",
            "jump NOPE",
            "jump -z L1",
            "var int toolongname123=1",
            "var int x=1234567890123456",
            "var old=1",
            "var flag f=xx",
            "echo a b c d e f g h i j k l",
            "jump -a",
            "label ;echo x",
        };
        int i;
        int bad = 0;
        for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
        {
            int t = RunCap(cases[i], g_cap, sizeof(g_cap));
            if (t < -1 || !SCL_Idle() || SCL_VarCount() != 0)
            {
                bad++;
                printf("  [FAIL] 错误用例状态不干净: %s\n", cases[i]);
            }
        }
        CHECK(bad == 0, "结构错误用例不崩溃且状态干净");
    }
    {
        int i;
        int ok = 1;
        for (i = 0; i < 600; i++)
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
            if ((i % 60) == 59)
            {
                Scl_CapBegin(g_cap, sizeof(g_cap));
                RunToIdle("echo recover");
                Scl_CapEnd();
                if (strstr(g_cap, "echo recover") == NULL || !SCL_Idle())
                {
                    ok = 0;
                }
            }
        }
        CHECK(ok == 1, "600 次模糊输入不崩溃、可恢复");
    }
}

/* ========================== 5. 重复性 / 速度 / 大小 ========================== */

static void TestMisc(void)
{
    int i;
    int t;
    Section("5. 重复性/速度/大小");

    {
        const char *chain = "demo_reset 4;label L;echo x;demo_inc;jump -a L";
        char out[3][2048];
        int ticks[3];
        int same = 1;
        for (i = 0; i < 3; i++)
        {
            ticks[i] = RunCap(chain, out[i], sizeof(out[i]));
        }
        for (i = 1; i < 3; i++)
        {
            if (strcmp(out[0], out[i]) != 0 || ticks[0] != ticks[i])
            {
                same = 0;
            }
        }
        CHECK(same == 1, "重复性：3 次运行输出与 tick 一致");
    }

    /* 速度：2000 圈 demo_inc 循环 ×3 次 */
    {
        const char *chain = "demo_reset 2000;label L;demo_inc;jump -a L";
        clock_t c0 = clock();
        int total = 0;
        for (i = 0; i < 3; i++)
        {
            t = RunToIdle(chain);
            total += (t > 0) ? t : 0;
        }
        {
            clock_t c1 = clock();
            double sec = (double)(c1 - c0) / CLOCKS_PER_SEC;
            printf("  2000 圈×3 次 总步进 %d tick, 用时 %.3f s\n", total, sec);
        }
        CHECK(total > 0, "速度测试可完成");
    }

    printf("  配置: VAR=%u BC=%u ARG_CACHE=%u LABEL=%u\n",
           (unsigned)SCL_CFG_VAR_MAX, (unsigned)SCL_CFG_BC_MAX,
           (unsigned)SCL_CFG_ARG_CACHE_MAX, (unsigned)SCL_CFG_LABEL_MAX);
}

/* ========================== 主流程 ========================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== SCL v0.2 (类型化参数缓存 + bool/int/flag/string 变量 + 内置运算) ===\n");
    SCL_Init();
#if (SCL_EX_CMDS_EN == 1u)
    Scl_Demo_Register();
#else
    printf("(SCL_EX_CMDS_EN=0)\n");
#endif

    TestBasics();
    TestVarsTyped();
    TestArith();
    TestBitHex();
    TestJump();
    TestAsync();
    TestReliability();
    TestMisc();

    printf("\n===== 汇总 =====\n");
    printf("PASS=%d  FAIL=%d\n", g_pass, g_fail);
    printf("最终状态: idle=%d var=%d (应 1/0)\n", (int)SCL_Idle(), SCL_VarCount());
    return (g_fail == 0) ? 0 : 1;
}
