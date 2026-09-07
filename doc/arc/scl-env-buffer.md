# SCL v0.3：环境变量缓冲（持久配置：默认装载 / 固化 / 恢复）

> 类别：架构/使用 · 状态：已实现 · 文件：`scl/Inc/scl_cfg.h`、`scl/Src/scl.c`、`example/main.c`(第 8 节)

## 1. 要解决的问题

设备/固件常需要一组**跨重启、可修改的配置**（SSID、波特率、开关…）。
SCL 会话变量随脚本结束释放，不适合当配置。本特性新增**环境变量缓冲**：

- 是独立于会话变量的**持久配置表**（静态槽，跨脚本恒在，`SCL_CFG_ENV_MAX` 可裁剪）；
- **默认先从默认配置表装载**（用户 const 表，可放 Flash）；
- 支持把缓冲**序列化固化到用户自己的存储区**（EEPROM/Flash/文件），
  以及**从存储区读回写进缓冲**（恢复）；
- 脚本/命令读路径可直接取到 env（`${name}`、`SCL_VarGet`、算术/真值操作数），
  会话变量同名时优先（可临时屏蔽）。

## 2. 数据流

```
 用户存储区(EEPROM/文件)  <--固化--  Scl_Env_Save(buf,len)   环境变量缓冲 s_env[]
        ^                            (字节流导出)              (静态槽)
        └------ 恢复 ----  Scl_Env_Load(buf,len)                       │
                                 (字节流写入)               脚本 ${}/VarGet/运算 只读
  默认配置表(const 表) ---- Reset 装载 ------>  (无存储时回退)          │
   Scl_Env_RegisterDefault + Scl_Env_Reset                           修改: Scl_Env_Set
```

典型启动流程：
```c
static const scl_env_def_t defs[] = {
    { "ssid", SCL_T_STR,  "SCL-AP" },
    { "baud", SCL_T_INT,  "115200" },
    { "dhcp", SCL_T_BOOL, "true"   },
    { "log",  SCL_T_FLAG, "-v"     },
};
/* 上电：先试用户存储，损坏/无 → 默认 */
Scl_Env_RegisterDefault(defs, 4);
if (my_eeprom_read(buf, &len) != OK || Scl_Env_Load(buf, len) != 0) {
    Scl_Env_Reset();                 /* 无存储/坏存储 → 默认配置 */
}
/* 修改并固化（用户写自己的存储区） */
Scl_Env_Set("baud", SCL_T_INT, "9600");
len = Scl_Env_Save(buf, sizeof(buf));
my_eeprom_write(buf, len);           /* 固化到用户的 EEPROM/Flash */
```

## 3. API（scl.h，`SCL_CFG_ENV_EN` 裁剪）

| API | 作用 |
| --- | --- |
| `Scl_Env_RegisterDefault(tab,n)` | 注册默认配置 const 表（Flash 友好） |
| `Scl_Env_Reset()` | 清空后用默认表重装缓冲（返回条数；无表 -1） |
| `Scl_Env_Set(name,type,val)` | 类型化写入/覆盖一条（校验规范化） |
| `Scl_Env_Save(buf,cap)` | 序列化 → 字节数（供固化到用户存储） |
| `Scl_Env_Load(buf,len)` | 反序列化 → 写缓冲（坏数据清空并返回负） |
| `Scl_Env_FreeAll/Count/Enum` | 清空 / 计数 / 遍历 |

序列化格式：`'S''C''L''E' + ver(1) + n(1) + 每项[type][nlen][name][vlen][val]`；
含魔数/版本/长度校验，坏存储会拒绝（不回填脏数据）。

## 4. 与脚本/命令的关系（读路径回退）

实现上把"名字 → 可读值"统一为 `Scl_VarFindAny()`：
**先查会话变量，再查 env**。因此：

- `echo ${baud}`、比较、算术指令操作数、`SCL_VarGet/Type` 都能读到 env；
- 会话变量同名优先：脚本里 `var int baud=0` 临时屏蔽 env，脚本结束会话释放后
  读回 env 值（一个"本地覆盖"机制）；
- 写路径（`var`/`free`/运算写回）仍只作用于会话变量，env 只能经 `Scl_Env_Set`
  修改 —— 避免脚本误改持久配置。

## 5. 裁剪

- `SCL_CFG_ENV_EN=0`：裁掉 env 表与读回退（会话变量行为回到原样）。
- `SCL_CFG_ENV_MAX`：env 槽数（默认 8，≤255）。
- 名/值上限复用 `SCL_CFG_VAR_NAME_MAX` / `SCL_CFG_VAR_VALUE_MAX`。

## 6. 验证（example/main.c 第 8 节，基线 scl_test PASS=97）

- 默认装载 → VarGet/Type 可见；脚本 `${}` 读取；跨脚本持久
- 算术指令直接读 env int 操作数
- 会话变量屏蔽同名 env、会话结束恢复
- 修改 → `Save` 固化到模拟存储 → 清空 → `Load` 恢复（存储值优先）
- 坏存储被拒绝 → 回退默认装载
