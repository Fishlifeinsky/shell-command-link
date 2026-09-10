/**
  ******************************************************************************
  * @file    test_cmd_opc.c
  * @brief   断言：注册表命令的 opcode 由"表内下标"决定（连续且递增）
  *
  *          注册表按数组顺序注册（下标 0..n-1），故链表顺序即表顺序，
  *          于是 `第 i 条命令的 opc == 第 0 条的 opc + i`。
  *          这条断言就是"opcode 不再依赖注册时自增"的闭环证据。
  ******************************************************************************
  */
#include <stdio.h>

#include "scl.h"
#include "scl_port.h"

int main(void)
{
    const scl_cmd_t *nd;
    int      i    = 0;
    int      bad  = 0;
    uint16_t base = 0u;

    SCL_Init();

    for (nd = SCL_CmdHead(); nd != NULL; nd = nd->next, i++)
    {
        if (i == 0)
        {
            base = nd->opc;
        }
        else if (nd->opc != (uint16_t)(base + (uint16_t)i))
        {
            printf("[FAIL] %s opc=0x%04X 期望=0x%04X\n",
                   nd->name, (unsigned)nd->opc, (unsigned)(base + (uint16_t)i));
            bad++;
        }
    }

    if (i == 0)
    {
        printf("[FAIL] 没有注册任何命令（注册表未链接？）\n");
        return 1;
    }

    printf("[%s] 注册表命令 %d 条，opc 连续（基址 0x%04X）\n",
           (bad == 0) ? "PASS" : "FAIL", i, (unsigned)base);
    return (bad == 0) ? 0 : 1;
}
