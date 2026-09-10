/**
  ******************************************************************************
  * @file    scl_mem.c
  * @brief   SCL 可选动态内存分配器与用量统计（SCL_CFG_DYNAMIC_MEM_EN）
  *
  *          由 scl.c 拆出（模块化）：本文件只负责
  *            - 用宿主注入的 scl_allocator_t 做"带头分配"（头里记 size，供 free/capacity）
  *            - 统计 current/peak/alloc/free/gc 计数，供 SCL_CacheInfo 查询
  *          关掉 SCL_CFG_DYNAMIC_MEM_EN 时全部退化为空实现（库改用静态缓冲，
  *          容量由 Scl_MemCapacity 报告）。
  *
  *          不依赖 libc / HAL / OS。
  ******************************************************************************
  */

#include "scl.h"
#include "scl_priv.h"

/* ========================== 分配头与统计状态 ========================== */

#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
typedef struct
{
    size_t size;                          /* 用户请求字节数（不含本头） */
} scl_mem_hdr_t;

static scl_allocator_t s_allocator;       /* 宿主注入的分配器 */
static size_t   s_mem_current = 0u;       /* 当前占用（不含头） */
static size_t   s_mem_peak    = 0u;       /* 峰值占用 */
static uint32_t s_mem_alloc_count = 0u;   /* 累计分配次数 */
static uint32_t s_mem_free_count  = 0u;   /* 累计释放次数 */
static uint32_t s_mem_gc_count    = 0u;   /* 累计 GC 次数 */
#endif

/**
  * @brief  注入分配器并清零统计（由 SCL_InitEx 在释放旧内存之后调用）
  * @param  a 分配器指针（调用方已校验 alloc/realloc/free 齐全）
  * @note   与迁移前行为一致：先赋值分配器，再把统计计数清零。
  */
void Scl_MemSetAllocator(const scl_allocator_t *a)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    if (a != NULL) { s_allocator = *a; }
    s_mem_current = 0u;
    s_mem_peak = 0u;
    s_mem_alloc_count = 0u;
    s_mem_free_count = 0u;
    s_mem_gc_count = 0u;
#else
    (void)a;
#endif
}

/**
  * @brief  分配器是否就绪（静态内存模式下恒为就绪）
  */
static uint8_t Scl_MemReady(void)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    return (s_allocator.alloc != NULL) && (s_allocator.free != NULL) ? 1u : 0u;
#else
    return 1u;
#endif
}

/**
  * @brief  分配 size 字节（带 1 个 size_t 头部）
  * @return 用户区指针；失败返回 NULL
  */
void *Scl_MemAlloc(size_t size)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    scl_mem_hdr_t *h;
    if (!Scl_MemReady() || size == 0u) { return NULL; }
    h = (scl_mem_hdr_t *)s_allocator.alloc(s_allocator.ctx, sizeof(*h) + size);
    if (h == NULL) { return NULL; }
    h->size = size;
    s_mem_current += size;
    if (s_mem_current > s_mem_peak) { s_mem_peak = s_mem_current; }
    s_mem_alloc_count++;
    return (void *)(h + 1);
#else
    (void)size;
    return NULL;
#endif
}

/**
  * @brief  重分配（ptr==NULL 等价于分配；size==0 等价于释放）
  * @note   分配器未提供 realloc 时返回 NULL（不退化，保持调用方可预期）
  */
void *Scl_MemRealloc(void *ptr, size_t size)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    scl_mem_hdr_t *h;
    size_t old;
    if (ptr == NULL) { return Scl_MemAlloc(size); }
    if (size == 0u) { Scl_MemFree(ptr); return NULL; }
    if (s_allocator.realloc == NULL) { return NULL; }
    h = ((scl_mem_hdr_t *)ptr) - 1;
    old = h->size;
    h = (scl_mem_hdr_t *)s_allocator.realloc(s_allocator.ctx, h, sizeof(*h) + size);
    if (h == NULL) { return NULL; }
    h->size = size;
    s_mem_current = s_mem_current - old + size;
    if (s_mem_current > s_mem_peak) { s_mem_peak = s_mem_current; }
    return (void *)(h + 1);
#else
    (void)ptr; (void)size;
    return NULL;
#endif
}

/**
  * @brief  释放（NULL 安全）
  */
void Scl_MemFree(void *ptr)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    scl_mem_hdr_t *h;
    if (ptr == NULL || !Scl_MemReady()) { return; }
    h = ((scl_mem_hdr_t *)ptr) - 1;
    if (s_mem_current >= h->size) { s_mem_current -= h->size; }
    s_mem_free_count++;
    s_allocator.free(s_allocator.ctx, h);
#else
    (void)ptr;
#endif
}

/**
  * @brief  GC 计数 +1（由变量表 GC 成功时调用）
  */
void Scl_MemGcCount(void)
{
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    s_mem_gc_count++;
#endif
}

/* ========================== 用量查询（无动态内存时给静态容量） ========================== */

size_t Scl_MemCurrent(void) { return
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    s_mem_current
#else
    0u
#endif
; }
size_t Scl_MemPeak(void) { return
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    s_mem_peak
#else
    0u
#endif
; }
size_t Scl_MemCapacity(void) { return
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    0u
#else
    (size_t)SCL_CFG_BC_MAX + SCL_CFG_ARG_CACHE_MAX +
    (size_t)SCL_CFG_VAR_MAX * (SCL_CFG_VAR_NAME_MAX + 1u + SCL_CFG_VAR_VALUE_MAX) +
    64u + SCL_CFG_ARG_BUF_BYTES
#endif
; }
uint32_t Scl_MemAllocCount(void) { return
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    s_mem_alloc_count
#else
    0u
#endif
; }
uint32_t Scl_MemFreeCount(void) { return
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    s_mem_free_count
#else
    0u
#endif
; }
uint32_t Scl_MemGcTotal(void) { return
#if (SCL_CFG_DYNAMIC_MEM_EN != 0u)
    s_mem_gc_count
#else
    0u
#endif
; }

/**
  * @brief  公共接口：缓存用量快照（见 scl.h）
  */
uint8_t SCL_CacheInfo(scl_cache_info_t *info)
{
    if (info == NULL) { return 0u; }
    info->current = Scl_MemCurrent();
    info->peak = Scl_MemPeak();
    info->capacity = Scl_MemCapacity();
    info->alloc_count = Scl_MemAllocCount();
    info->free_count = Scl_MemFreeCount();
    info->gc_count = Scl_MemGcTotal();
    info->zombie_count = 0u;
    return 1u;
}

/**
  * @brief  公共接口：回收失效变量（成功则计入 GC 次数）
  */
uint8_t SCL_CacheGc(void)
{
    uint8_t r = Scl_VarGc();
    if (r != 0u) { Scl_MemGcCount(); }
    return r;
}

/**
  * @brief  公共接口：回收僵尸变量（成功则计入 GC 次数）
  */
uint8_t SCL_CacheGcZombie(void)
{
    uint8_t r = Scl_VarGcZombie();
    if (r != 0u) { Scl_MemGcCount(); }
    return r;
}
