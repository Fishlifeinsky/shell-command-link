/**
  ******************************************************************************
  * @file    chain_runner.c
  * @brief   PC 回喂工具：把一条指令链文件喂给 SCL 跑完
  *
  *          用途：验证 "现代脚本 -> 指令链" 转译结果能在真实 SCL 上执行。
  *          用法：chain_runner <chain文件>
  *            读入整条链 → SCL_Run → 主循环推到空闲。
  *            退出码：0=完成；2=SCL_Run 拒绝；3=超时（防死循环）。
  *
  *          编译（建议放大脚本缓冲，供较长的现代脚本转译结果）：
  *            gcc -O2 -DSCL_CFG_SCRIPT_MAX=2048 -I scl/Inc -I scl/Src -I example \
  *                scl/Src/scl.c scl/Src/scl_var.c scl/Src/scl_env.c \
  *                example/scl_port.c example/demo_cmds.c \
  *                example/chain_runner.c -o build/chain_runner
  ******************************************************************************
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scl.h"
#include "scl_port.h"
#include "scl_port.h"

int main(int argc, char *argv[])
{
    FILE *f;
    long fsz;
    char *buf;
    long guard = 0;

    if (argc < 2)
    {
        printf("usage: chain_runner <chain_file>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL)
    {
        printf("[RUN-NOFILE]\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz > (long)SCL_CFG_SCRIPT_MAX - 1)
    {
        fsz = (long)SCL_CFG_SCRIPT_MAX - 1;
    }
    buf = (char *)malloc((size_t)fsz + 1u);
    if (buf == NULL)
    {
        fclose(f);
        printf("[RUN-NOMEM]\n");
        return 2;
    }
    if (fread(buf, 1u, (size_t)fsz, f) != (size_t)fsz)
    {
        /* 读到少于预期：按实际已读即可 */
    }
    buf[fsz] = '\0';
    fclose(f);

    SCL_Init();
    /* 命令由注册表在 SCL_Init 内自动注册（scl/cmd/scl_cmd_list.c） */

    if (SCL_Run(buf) == 0u)
    {
        printf("[RUN-REJECT]\n");
        free(buf);
        return 2;
    }
    while (!SCL_Idle())
    {
        SCL_Loop();
        guard++;
        if (guard > 5000000L)
        {
            SCL_Abort();
            while (!SCL_Idle())
            {
                SCL_Loop();
            }
            printf("[RUN-TIMEOUT]\n");
            free(buf);
            return 3;
        }
    }
    printf("[RUN-OK]\n");
    free(buf);
    return 0;
}
