# SCL MCU 移植模板（STM32 参考工程）

本目录把 SCL 的整套能力收束成一个**可直接照抄的 MCU 工程模板**：

- `scl_stm32_port.h/.c` —— STM32 HAL 串口移植层（输出 + env 固化用户存储）
- `mcu_main_template.c` —— 并入 CubeMX 工程的 `main()` 骨架（env 固化 + const 自检程序 + 主循环）
- `boot.s2c` —— 开机自检脚本示例（现代语法；固定脚本 → const 程序最省 RAM）
- `mcu_boot_sim.c` —— **PC 可编译的整体骨架自检**（先无板验证 main 骨架逻辑正确）

> 库本身对 MCU 的唯一硬要求是提供 `void SCL_Port_PutChar(char c)`（`SCL_CFG_MSG_EN=0` 时可省略）；
> env 固化是把 `Scl_Env_Save` 的缓冲写进你的 Flash/EEPROM。

---

## 1. 架构

```mermaid
flowchart LR
    subgraph MCU[MCU 主循环 while(1)]
        B[SCL_Loop 推进脚本] --- C[SCL 库核心]
        E[(用户 Flash/EEPROM)] <-->|Scl_Env_Save/Load| ENV[env 缓冲]
        BOOT[const 自检程序 SCL_RunProg] --> C
    end
    TX[串口 TX] --- C
    C -->|SCL_Port_PutChar| TX
    ENV --> C
```

启动时序（`main` 骨架，详见 `mcu_main_template.c`）：

1. 系统时钟/GPIO/UART 由 CubeMX 生成；
2. `SCL_Init()` → 注册业务命令（`SCL_CmdRegisterDesc`）；
3. `Scl_Env_RegisterDefault(默认配置表, n)` → `Scl_Env_Load(存储)`（无存储/坏存储回退默认）；
4. 上电自检：`SCL_RunProg(&scl_boot_prog)` 执行 const 程序；
5. `while(1){ SCL_Loop(); 周期任务(); }`；
6. 配置被改动后调用 `Scl_Env_Save` 固化到你的存储。

---

## 2. 文件清单与作用

| 文件 | 作用 | 是否参与本仓库编译 |
|---|---|---|
| `scl_stm32_port.h/.c` | STM32 HAL UART 移植层（库只要求 PutChar；env 固化用用户存储样板） | 否（需 STM32 HAL） |
| `mcu_main_template.c` | `main()` 的 SCL 集成骨架（含 env 固化与 const 自检整合） | 否（并入 CubeMX 工程） |
| `boot.s2c` | 示例现代脚本（编译成指令链 → const 程序） | ——（被 emit 工具读取） |
| `mcu_boot_sim.c` | 同一套“集成逻辑”的 PC 无板自检（见 §4） | 是（PC gcc 可跑） |

---

## 3. STM32（CubeIDE/Keil）接入步骤

1. **建工程**：CubeMX 生成带 UART1（115200 8N1，开中断）的基础工程。
2. **拷库**：把 `scl/` 整个目录拷入工程（`Inc/` 头文件 + `Src/` 8 个模块源 + 需要时 `cmd/`），加入编译；
   `scl_priv.h` 是内部头，不用包含，但**三个 .c 必须一起编译链接**。
3. **拷模板**：`scl_stm32_port.c/.h` → 工程；按你的句柄把
   `SCL_MCU_HUART`（`scl_stm32_port.h` 顶部）改成你的 `huart1`/`huart2`。
4. **main.c 并入骨架**：把 `mcu_main_template.c` 中 `App_Scl_Init()` 与主循环里
   `App_Scl_Poll()` 的内容并入 CubeMX 生成的 `main()`（或单独成模块调用）。
5. **裁剪**：按 `doc/other/scl-config-profiles.md` 调整 `scl_cfg.h`
   （纯固定脚本 → `SCL_CFG_RUN_TEXT_EN=0` + `SCL_CFG_RUN_PROG_EN=1` 最省 RAM）。
6. **生成 const 自检程序**（脚本不变时）：
   ```bash
   python tools/scl_script2chain.py example/mcu_template/boot.s2c -o build/boot.chain
   python tools/scl_emit_c.py example/mcu_template/boot.s2c -o build/boot_prog.c
   ```
   把生成的 `boot_prog.c`（含 `const scl_prog_t scl_boot_prog`）拷入工程，
   在 `main()` 里 `extern const scl_prog_t scl_boot_prog;` 后 `SCL_RunProg(&scl_boot_prog)`。
   （想先验证内容正确，可先用 §4 的 PC 自检。）

7. **env 固化**：`scl_stm32_port.c` 底部给出“用户存储”样板；把 `Scl_Env_Save` 的缓冲写入
   内部 Flash 一个扇区 / 外挂 EEPROM，上电用 `Scl_Env_Load` 读回（坏数据自动回退默认）。

---

## 4. PC 无板自检（推荐先跑）

无需开发板即可验证“main 骨架 + env 固化 + const 自检程序”这一整套集成逻辑：

```bash
gcc -pipe -O2 -Wall -Wextra -I scl/Inc -I scl/Src -I example \
    scl/Src/*.c \
    example/scl_port.c example/demo_cmds.c \
    example/mcu_template/mcu_boot_sim.c -o build/mcu_boot_sim
python -c "import subprocess;print(subprocess.run(['build/mcu_boot_sim'],capture_output=True).stdout.decode('utf-8','replace'))"
```

自检内容（headless，非交互）：
- env 默认配置装载 → 脚本可读；
- 上电“自检程序”（常量 boot 链等价物）执行到 `OK`；
- 修改 env → `Scl_Env_Save` 固化到“用户存储”（内存模拟）；
- 模拟重启清空 → `Scl_Env_Load` 恢复 → 校验固化值优先。

> 有真实板后：把 `boot.s2c` 换成 `scl_emit_c.py` 产物 + `SCL_RunProg`，
> 把“用户存储”换成你的 Flash/EEPROM。
