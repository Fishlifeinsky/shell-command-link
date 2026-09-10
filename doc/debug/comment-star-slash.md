# 注释里的 `*/` 提前终止块注释（几十条 stray 报错的真凶）

> 类别：调试记录 · 现象/原因/办法 · 发生日期：2026-09-10 · 关联：`scl/Inc/scl_cfg.h`

## 1. 现象

给 `scl_cfg.h` 补"后果"注释后，全量编译一次报出**几十条**互相矛盾的错误，且**全部指向同一行**：

```
scl/Inc/scl_cfg.h:88:70: error: stray '\343' in program
scl/Inc/scl_cfg.h:88:72: error: expected '=', ',', ';', 'asm' or '__attribute__' before '\U000067e5...'
scl/Inc/scl_cfg.h:88:47: error: unknown type name 'SCL_VarBindOne'
scl/Inc/scl_cfg.h:89:15: error: invalid suffix "（不用" on integer constant
scl/Inc/scl_cfg.h:89:70: error: stray '\343' in program
```

特征：**行号集中、错误类型五花八门、错误位置落在中文上**。

## 2. 原因

注释里写了这样一串符号：

```c
     代价是 static 型变量不可用（SCL_VarBind*/SCL_VarBindOne 变空操作、查表恒不命中）。
/*                                        ^^ 这两个字符提前结束了块注释 */
```

`SCL_VarBind*` 紧跟 `/` 构成了 `*/`，**块注释在这里就结束了**；
后面的 `SCL_VarBindOne 变空操作…）` 和下一行中文全部被当作 C 代码解析，
于是产生大量 `stray '\343'`（中文 UTF-8 首字节）与"未知类型名"。

在本仓库里它格外容易被误判成**透明加密**问题（历史上确实出现过 gcc 读到未解密字节而
报 `stray`）。两者的区别：

| | 注释提前终止 | 透明加密误读 |
|---|---|---|
| 错误行号 | 集中在**同一行**及紧随其后 | 散落在文件各处，行号跳跃 |
| 报错内容 | 中文被当标识符/后缀 | 随机字节被当 `stray`，位置列号很大 |
| 复现性 | **100% 复现** | 重跑就好 |

## 3. 办法

1. 注释里列举符号时，用**空格**分隔而不是紧跟斜杠：
   `SCL_VarBind / SCL_VarBindOne`；
2. `Foo*` 这种通配写法后面**不要**紧跟 `/`（例如别写 `Foo*/bar`）；
3. 出现"几十条 stray 且行号集中"时，先按此排查，别急着怀疑加密。

一键自查（全库找 `*/` 后紧跟标识符/中文的位置）：

```bash
grep -rnP '\*/[A-Za-z_(]' scl/          # 命中即为可疑（正常的注释结尾后面是空白或换行）
```

> 同类坑：`doc/` 里写 Markdown 不受影响；只有 C 注释才需要这条约束。
