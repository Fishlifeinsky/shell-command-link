# 变量来源模型：static / const / var（两态语义）

> 状态：**提案中**（等确认）
> 场景分类：变量与绑定 ＋ 编译链（s2c 前端 / mini 生成器）
> 提出者 / 日期：agent 提案 / 2026-09-10（依据用户决策整理）
> 关联：`scl/Src/scl_var.c`（mini/normal 两臂）、`scl/Inc/scl.h`（Var API）、
> `tools/scl_script2chain.py`、`tools/scl_mini_c.py`、`tools/scl_emit_c.py`

---

## 1. 场景与动机

现在脚本变量只有两类：**会话变量 `var`**（占槽、可 `free`）与 **`const`**（编译期折叠）。
mini 态把 `var` 裁掉后，外部值只能靠宿主手写 `SCL_VarBind` 绑定，脚本里**无法声明**
"这个变量是宿主提供的静态存储"。

需求（用户指定）：

1. 脚本可直接声明 **`static int a`** —— 由宿主注入的值，落到生成代码的类型化静态存储；
2. **mini 只支持 static 这一种来源**，且 **static 不允许初始化**；声明 `var` 必须**编译期报错**；
3. **`const` 两态都支持**；
4. **`var` 只在 normal 支持**：可 `free`，开启动态内存时按需分配；
5. 原有的"读变量/写变量"API 要能同时访问 **static / const / var** 三种来源。

## 2. 现状（已核实）

| 来源 | 语法 | mini | normal | 存储 | 代码位置 |
|---|---|---|---|---|---|
| `var` | `var int a = 0` | 生成器可解析但不占槽（实际靠宿主绑定） | ✅ 会话槽表 `s_vars[SCL_CFG_VAR_MAX]` | RAM | `scl_var.c` 普通臂 |
| `const` | `const int a = 5`（顶层） | ✅ 折叠 | ✅ 折叠（`const_fold`） | 无 | `scl_script2chain.py:267,922` |
| `static` | — | — | — | — | **尚不存在**（仅宿主手写 `SCL_VarBind`） |

查找优先级（现状）：normal 的 `SCL_VarSet/Get` **先查绑定表**（`Scl_BindFind`），未命中再查会话表
（`scl_var.c:458-520`）；mini 只查绑定表，未命中返回 `-1`/`NULL`。

## 3. 方案：三种来源 × 两态

| 来源 | 脚本语法 | mini | normal | 存储 | `free` | 初始化 |
|---|---|---|---|---|---|---|
| **static** | `static int a` | ✅ | ✅ | 生成 C 静态变量 + 绑定路由 | no-op | **禁止** |
| **const** | `const int a = 5` | ✅ | ✅ | 编译期折叠（无存储） | no-op | **必须** |
| **var** | `var int a = 0` | ❌ **编译期报错** | ✅ | 会话槽（可动态内存） | ✅ | 可选/必须 |

### 3.1 API 统一

`SCL_VarGet` / `SCL_VarSet` / `SCL_VarSetT` / `SCL_VarType` / `SCL_VarIsConst` /
`SCL_VarFree` / `SCL_VarFreeAll` / `SCL_VarEnum` / `SCL_VarCount` 三种来源通吃：

| API | static | const | var |
|---|---|---|---|
| `VarGet` | ✅ 走绑定 getter | ✅ 返回常量文本 | ✅ 会话槽 |
| `VarSet(T)` | ✅ 走绑定 setter | ❌ 返回 `-5`（已是 const） | ✅ 会话槽 |
| `VarType` | ✅ 绑定项类型 | ✅ 常量类型 | ✅ 槽类型 |
| `VarIsConst` | 0 | 1 | 0 |
| `VarFree` | no-op（无生命周期） | no-op | ✅ 释放 |
| `VarEnum/Count` | ✅ 计入 | ✅ 计入 | ✅ 计入 |

**查找优先级（两态一致）**：`static(绑定) > var(会话) > env`；`const` 在文本路径编译期已折叠，
运行期查询应能命中其类型与只读标记。

### 3.2 mini 的约束（**编译期强制**，由生成器报错）

1. `static int a = 0;` → 报错：
   `mini: static 变量不允许初始化；初值请由宿主 SCL_VarSet() 注入`
   **原因**：生成的状态机若带初值，每次 `start` 都会把宿主注入值重置（既有教训见
   `test-ms41929` 的 `doc/arc/scl-mini-mode-migration.md` §3.3）。
2. `var ...` → 报错：
   `mini 不支持 var（会话变量）；请改用 static，或由宿主绑定`
3. `const` → 允许，编译期折叠。

### 3.3 normal 的语义

- `static`：**不占会话槽**，编译期产出"静态引用"语句；运行期只从绑定表取。未绑定 → 运行期报错；
- `var`：沿用会话槽；`free` 释放；`SCL_CFG_DYNAMIC_MEM_EN=1` 时按需分配；
- `const`：折叠（不变）。

### 3.4 编译链改动

| 工具 | 改动 |
|---|---|
| `tools/scl_script2chain.py` | 新增 `static` 关键字：顶层声明 `static <type> <name>`（**不接受 `=`**），产出语句 `("staticvar", name, type)`；`var`/`const` 行为不变 |
| `tools/scl_mini_c.py` | 接受 `staticvar`/`constvar`；**拒绝 `var`**、**拒绝带初值的 static**；为每个 static 产出 `SCL_VAR_DEFINE(...)`（见 `cmd-modular-registration.md` §3.3） |
| `tools/scl_emit_c.py` | `staticvar` 编译为"静态引用"，不占参数字节缓存变量槽；`var` 行为不变 |

## 4. 影响面

| 维度 | 影响 |
|---|---|
| 静态 RAM（mini） | 不变：static 变量本来就需要存储，现在由宿主手写、改后由脚本声明后生成 |
| 静态 RAM（normal） | 不变：新增 `staticvar` 只是不占会话槽；会话槽表大小不变 |
| Flash | 每 static 变量：`getter/setter` + 绑定项（与现状宿主手写等价） |
| 公共 API | 无破坏性变化；新增 `SCL_VarBindOne()`（注册表用） |
| 配置开关 | 复用 `SCL_CFG_VAR_BIND_MAX`（static 变量计入该上限，需按脚本调大） |
| 普通态 / mini 态 | 两态语义差异集中在 §3.2/§3.3，需在 `doc/spec/` 写明 |
| 生成物与工具链 | s2c 前端 + mini 生成器 + const prog 生成器三处都要支持 `static` |
| 向后兼容性 | 现有 `var`/`const` 脚本不受影响；**mini 下原本用 `var` 的脚本会变成编译错误**（属于有意的行为收紧） |

## 5. 执行步骤

1. `scl_script2chain.py`：加 `static` 关键字与 `staticvar` 语句；
2. `scl_mini_c.py`：`staticvar` → `SCL_VAR_DEFINE`；`var` 报错；static 带初值报错；
3. `scl_var.c`：`SCL_VarBindOne()`；核对三来源 API 分派与优先级；
4. `scl_emit_c.py`：`staticvar` 生成路径；
5. 示例：`example/mini/` 改用 `static`（原 `var cnt`）；`example/demo` 加三种来源混用样例；
6. 文档：`doc/spec/` 写明语法与两态差异。

## 6. 验证（可直接复制执行）

```powershell
python tools/scl_build.py test            # 回归（含 mini_boot_sim）
# 反向用例：mini 下应报错
python tools/scl_mini_c.py bad_var.s2c   -o out.c   # 期望: 报错 "mini 不支持 var"
python tools/scl_mini_c.py bad_init.s2c  -o out.c   # 期望: 报错 "static 不允许初始化"
```

期望结果：正向用例编译通过且变量可被 `SCL_VarSet/Get` 读写；反向用例**编译期报错**（非运行期）。

## 7. 闭环标准

- [ ] 编译：普通态 + mini 态零错误零告警
- [ ] 回归：`tools/scl_build.py test` 全绿
- [ ] 反向用例：mini 下 `var` 与带初值 static 均**编译期**报错（附实际报错文本）
- [ ] 体积：默认档/mini 档 Flash 与 RAM 前后实测数字
- [ ] 文档：语法与两态差异入 `doc/spec/`，机制入 `doc/arc/`
- [ ] 提交：`add:`（语法与生成）+ `change:`（示例迁移）+ `md:`
- [ ] 回填：本文件补实测数据与提交号

## 8. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| 现有 mini 脚本被拒 | 原先用 `var` 的脚本升级后编译失败 | 迁到 `static`；或给生成器加 `--allow-var` 兼容开关（短期过渡） |
| static 未绑定 | normal 下脚本引用未绑定的 static | 运行期明确报错（而非静默空值）；错误信息带上变量名 |
| 绑定表容量 | static 变量计入 `SCL_CFG_VAR_BIND_MAX`（默认 8）易满 | 文档提示按脚本调大；生成器在变量数超限时**构建期报警** |
| 语义分裂 | 同一脚本两态行为不同，易误用 | `doc/spec/` 用一张表讲清；生成器在各态给出对应报错 |

## 9. 待确认

1. mini 下 `var` 报错的**现有脚本兼容**：直接报错，还是先给 `--allow-var` 过渡开关？
2. `static` 允许的类型：`int/bool/flag/string` 全支持？（mini 绑定表四种都支持）
3. 同名冲突：`static a` 与 `var a` 并存时，**static 优先**（沿用现状）可以吗？
4. `const` 是否只允许顶层（现状），还是也允许函数内声明？
5. `static` 在 normal 的文本路径：未绑定应"编译期无法校验 → 运行期报错"，接受吗？

## 10. 闭环记录

| 日期 | 动作 | 结果 / 实测数据 | 提交 |
|---|---|---|---|
| 2026-09-10 | 建档（仅规划，未改代码） | 待确认 §9 | — |
