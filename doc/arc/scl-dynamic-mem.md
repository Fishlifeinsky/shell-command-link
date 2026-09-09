# SCL 可选动态内存与 cache

## 1. 配置

默认仍是静态内存，保持原有固件 ABI 和行为：

```c
#define SCL_CFG_DYNAMIC_MEM_EN 0u
```

打开动态模式：

```text
-DSCL_CFG_DYNAMIC_MEM_EN=1
```

应用必须提供完整的 `alloc/realloc/free + ctx`：

```c
static scl_allocator_t mem = {
    App_Alloc, App_Realloc, App_Free, app_ctx
};

if (SCL_InitEx(&mem) == 0) {
    /* allocator 不完整或最小工作区不足 */
}
```

动态模式不依赖 libc；函数可以连接到 RTOS heap、用户 arena 或 MCU 专用内存池。
`ctx` 原样传回三个回调。

## 2. 分配策略

- `SCL_InitEx()` 分配解释器最小工作区：动态文本的字节码、参数缓存、label 表，
  以及运行期间复用的 `raw/argv` 工作区。
- 变量槽元数据在初始化时建立，但变量名和值**首次使用时按实际长度分配**。
- 变量值覆盖时按新长度 `realloc`；释放变量时立即释放名字和值。
- 预编译 `SCL_RunProg()` 仍直接使用 Flash 的 `bc/argc`，不复制脚本数据。
- `SCL_Init()` 在动态模式没有 allocator 时会失败；应用应改用 `SCL_InitEx()`。

动态 allocator 的统计不含回调自身的内部开销，`current`/`peak` 按用户 payload
计数，不含库内部的块头。

## 3. cache 指令与 API

指令链中可直接使用：

```text
cache          # 当前、峰值、容量、alloc/free、GC、zombie 统计
cache max      # 查看峰值（同 cache 输出）
cache gc       # 按当前字符串长度收缩变量值缓冲
cache zombie   # 清理未使用变量槽中的残留缓冲
```

C 侧对应：

```c
scl_cache_info_t info;
SCL_CacheInfo(&info);
SCL_CacheGc();
SCL_CacheGcZombie();
```

字段说明：

| 字段 | 含义 |
|---|---|
| `current` | 当前 allocator payload 用量 |
| `peak` | 初始化以来的最大 payload 用量 |
| `capacity` | 静态模式的配置容量；动态回调未提供总容量时为 0 |
| `alloc_count/free_count` | 分配/释放次数 |
| `gc_count` | `cache gc` 或 `cache zombie` 次数 |
| `zombie_count` | 可回收但仍挂在未使用槽上的变量块数量 |

当前实现变量释放立即归还块，因此正常情况下 `zombie_count=0`；该入口为 arena
或未来延迟回收策略保留。

## 4. 约束

- 动态模式仍受 `SCL_CFG_BC_MAX`、`SCL_CFG_ARG_CACHE_MAX`、`SCL_CFG_VAR_MAX`
  等逻辑上限约束；动态分配改变的是物理占用，不是脚本上限。
- allocator 回调必须可重入到库所需程度，且返回的块在 `realloc/free` 中保持一致。
- `SCL_CacheGc()` 不是并发安全的；应在脚本空闲或应用已暂停脚本时调用。