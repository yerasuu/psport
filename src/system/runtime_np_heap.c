// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceNpCommon's kernel memory, NpHeap and sce::np::Object's allocator-taking operators, as the
 * console's module does them (docs/missing-hle-spec.md E in decanter, kb #1253): the Np libraries
 * build their own heap over flexible memory and allocate their objects through it.
 * The console's NpHeap is a libc mspace placed in the guest buffer; here a first-fit allocator over
 * that buffer keeps its block table host-side, so a guest overrun cannot silently corrupt it, and
 * frees of addresses it never handed out are refused and counted. The mspace value the guest's
 * heap holds is the buffer's 16-byte aligned base, looked up in that table. */
#define _GNU_SOURCE
#include "runtime_np.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP_COMMON_INVALID_ARGUMENT ((int32_t)0x80558005)
#define NP_COMMON_OUT_OF_MEMORY ((int32_t)0x8055800A)
#define CB_KERNEL_MEMORY_ALIGNMENT UINT64_C(0x4000)
#define CB_OBJECT_HEADER UINT64_C(0x10)
#define CB_BLOCK_ALIGNMENT UINT64_C(16)
#define CB_HEAP_MIN UINT64_C(64)
#define CB_HEAP_MAX (UINT64_C(1) << 36)
#define CB_NAME 32
#define C_HEAPS_MAX 64
/* Past it a free block is handed out whole instead of split, so the table stays bounded. */
#define C_BLOCKS_MAX 16384
#define C_LOGGED_FAILURES 16
#define KERNEL_PROT_READ_WRITE 3

typedef struct NpKernelMemory {
    uint64_t cb_memory;
    uint64_t reserved;
    void *p_memory;
} NpKernelMemory;
typedef struct NpHeapStat {
    uint64_t cb_pool, cb_max_in_use, cb_in_use;
} NpHeapStat;
typedef struct NpAllocator {
    void *(ABI *pfn_malloc)(uint64_t cb_block, void *p_user);
    void *(ABI *pfn_realloc)(void *p_block, uint64_t cb_block, void *p_user);
    void (ABI *pfn_free)(void *p_block, void *p_user);
    void *p_user;
} NpAllocator;
typedef int32_t (ABI *MapNamedFlexible)(void **pp_inout, uint64_t cb_range, int prot, int flags, const char *sz_name);
typedef int32_t (ABI *Unmap)(void *p_address, uint64_t cb_range);

typedef struct HeapBlock {
    uint64_t off_block, cb_block;
    bool is_used;
} HeapBlock;
typedef struct Heap {
    uintptr_t va_base;
    uint64_t cb_heap, cb_live, cb_max_live;
    HeapBlock *p_blocks;
    size_t c_blocks, c_block_capacity;
    char sz_name[CB_NAME];
} Heap;
static pthread_mutex_t s_heaps_lock = PTHREAD_MUTEX_INITIALIZER;
static Heap s_heaps[C_HEAPS_MAX];

static uint64_t s_c_heaps_created, s_c_heaps_destroyed, s_c_allocations, s_c_frees, s_c_reallocations, s_c_failures;
static uint64_t s_cb_live, s_c_kernel_maps, s_c_kernel_unmaps, s_c_objects_created, s_c_objects_deleted;

static void summary(char *sz_out, size_t cb_out) {
    snprintf(sz_out, cb_out,
             "Runtime: summary Np heaps created %" PRIu64 " destroyed %" PRIu64 ", allocations %" PRIu64 " frees %" PRIu64
             " reallocations %" PRIu64 ", live %" PRIu64 " bytes, failures %" PRIu64 "; kernel memory maps %" PRIu64
             " unmaps %" PRIu64 "; np::Object new %" PRIu64 " delete %" PRIu64 "\n",
             __atomic_load_n(&s_c_heaps_created, __ATOMIC_RELAXED), __atomic_load_n(&s_c_heaps_destroyed, __ATOMIC_RELAXED),
             __atomic_load_n(&s_c_allocations, __ATOMIC_RELAXED), __atomic_load_n(&s_c_frees, __ATOMIC_RELAXED),
             __atomic_load_n(&s_c_reallocations, __ATOMIC_RELAXED), __atomic_load_n(&s_cb_live, __ATOMIC_RELAXED),
             __atomic_load_n(&s_c_failures, __ATOMIC_RELAXED), __atomic_load_n(&s_c_kernel_maps, __ATOMIC_RELAXED),
             __atomic_load_n(&s_c_kernel_unmaps, __ATOMIC_RELAXED), __atomic_load_n(&s_c_objects_created, __ATOMIC_RELAXED),
             __atomic_load_n(&s_c_objects_deleted, __ATOMIC_RELAXED));
}
static void note_use(void) {
    static int s_has_summary;
    if (!__atomic_exchange_n(&s_has_summary, 1, __ATOMIC_ACQ_REL)) runtime_calls_add_summary(summary);
}
/* The summary counts every failure; the first ones also show here, in order. */
static void log_failure(const char *sz_call, const char *sz_reason, uintptr_t va_argument) {
    note_use();
    const uint64_t c_failures = __atomic_add_fetch(&s_c_failures, 1, __ATOMIC_RELAXED);
    if (c_failures <= C_LOGGED_FAILURES)
        printf("Runtime: %s: %s (0x%" PRIxPTR "; Np heap failure %" PRIu64 "%s)\n", sz_call, sz_reason, va_argument, c_failures,
               c_failures == C_LOGGED_FAILURES ? ", later ones only counted in the summary" : "");
}

static bool read_guest(void *p_out, uintptr_t va_guest, uint64_t cb_read) {
    return va_guest && va_guest + cb_read > va_guest && runtime_memory_read_guest(p_out, va_guest, cb_read) == (int64_t)cb_read;
}
static bool is_guest_range(uintptr_t va_range, uint64_t cb_range) {
    unsigned char probe;
    return cb_range && read_guest(&probe, va_range, 1) && read_guest(&probe, va_range + cb_range - 1, 1);
}
static void read_guest_name(const char *sz_guest, char sz_out[CB_NAME]) {
    sz_out[0] = 0;
    if (!sz_guest) return;
    for (size_t i = 0; i + 1 < CB_NAME; ++i) {
        if (!read_guest(&sz_out[i], (uintptr_t)sz_guest + i, 1)) sz_out[i] = 0;
        if (!sz_out[i]) return;
    }
    sz_out[CB_NAME - 1] = 0;
}

static Heap *find_heap(uintptr_t va_mspace) {
    for (size_t i = 0; i < C_HEAPS_MAX; ++i)
        if (va_mspace && s_heaps[i].va_base == va_mspace) return &s_heaps[i];
    return NULL;
}
static void release_heap(Heap *p_heap) {
    __atomic_sub_fetch(&s_cb_live, p_heap->cb_live, __ATOMIC_RELAXED);
    free(p_heap->p_blocks);
    *p_heap = (Heap){0};
}
/* Room for one more block, within C_BLOCKS_MAX. */
static bool reserve_block(Heap *p_heap) {
    if (p_heap->c_blocks < p_heap->c_block_capacity) return true;
    if (p_heap->c_block_capacity >= C_BLOCKS_MAX) return false;
    const size_t c_grown = p_heap->c_block_capacity * 2 < C_BLOCKS_MAX ? p_heap->c_block_capacity * 2 : C_BLOCKS_MAX;
    HeapBlock *p_grown = realloc(p_heap->p_blocks, c_grown * sizeof(*p_grown));
    if (!p_grown) return false;
    p_heap->p_blocks = p_grown;
    p_heap->c_block_capacity = c_grown;
    return true;
}
static void insert_block(Heap *p_heap, size_t i_block, HeapBlock block) {
    memmove(p_heap->p_blocks + i_block + 1, p_heap->p_blocks + i_block, (p_heap->c_blocks - i_block) * sizeof(*p_heap->p_blocks));
    p_heap->p_blocks[i_block] = block;
    ++p_heap->c_blocks;
}
static void remove_block(Heap *p_heap, size_t i_block) {
    memmove(p_heap->p_blocks + i_block, p_heap->p_blocks + i_block + 1, (p_heap->c_blocks - i_block - 1) * sizeof(*p_heap->p_blocks));
    --p_heap->c_blocks;
}
/* Keeps cb_block of block i_block and hands the rest to a free block after it, merged with the
 * next one when that is free too; the block stays whole when the table is full. */
static void split_block(Heap *p_heap, size_t i_block, uint64_t cb_block) {
    HeapBlock *p_block = &p_heap->p_blocks[i_block];
    const uint64_t cb_rest = p_block->cb_block - cb_block;
    if (!cb_rest) return;
    if (i_block + 1 < p_heap->c_blocks && !p_heap->p_blocks[i_block + 1].is_used) {
        p_block->cb_block = cb_block;
        p_heap->p_blocks[i_block + 1].off_block -= cb_rest;
        p_heap->p_blocks[i_block + 1].cb_block += cb_rest;
        return;
    }
    if (!reserve_block(p_heap)) return;
    p_block = &p_heap->p_blocks[i_block];
    p_block->cb_block = cb_block;
    insert_block(p_heap, i_block + 1, (HeapBlock){p_block->off_block + cb_block, cb_rest, false});
}
static uint64_t block_size_for(uint64_t cb_request) {
    if (cb_request > CB_HEAP_MAX) return 0;
    return cb_request ? (cb_request + CB_BLOCK_ALIGNMENT - 1) & ~(CB_BLOCK_ALIGNMENT - 1) : CB_BLOCK_ALIGNMENT;
}
static void *heap_allocate_locked(Heap *p_heap, uint64_t cb_request) {
    const uint64_t cb_block = block_size_for(cb_request);
    if (!cb_block) return NULL;
    for (size_t i_block = 0; i_block < p_heap->c_blocks; ++i_block) {
        if (p_heap->p_blocks[i_block].is_used || p_heap->p_blocks[i_block].cb_block < cb_block) continue;
        split_block(p_heap, i_block, cb_block);
        HeapBlock *p_block = &p_heap->p_blocks[i_block];
        p_block->is_used = true;
        p_heap->cb_live += p_block->cb_block;
        if (p_heap->cb_live > p_heap->cb_max_live) p_heap->cb_max_live = p_heap->cb_live;
        __atomic_add_fetch(&s_cb_live, p_block->cb_block, __ATOMIC_RELAXED);
        return (void *)(p_heap->va_base + p_block->off_block);
    }
    return NULL;
}
/* The index of the used block starting at va_block, or c_blocks; *p_out_reason says why not. */
static size_t find_used_block(const Heap *p_heap, uintptr_t va_block, const char **p_out_reason) {
    *p_out_reason = "address outside the heap";
    if (va_block < p_heap->va_base || va_block - p_heap->va_base >= p_heap->cb_heap) return p_heap->c_blocks;
    const uint64_t off_block = va_block - p_heap->va_base;
    size_t low = 0, high = p_heap->c_blocks;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (p_heap->p_blocks[mid].off_block < off_block) low = mid + 1;
        else high = mid;
    }
    *p_out_reason = "address the heap never handed out";
    if (low == p_heap->c_blocks || p_heap->p_blocks[low].off_block != off_block) return p_heap->c_blocks;
    *p_out_reason = "block already free";
    return p_heap->p_blocks[low].is_used ? low : p_heap->c_blocks;
}
static void heap_free_locked(Heap *p_heap, size_t i_block) {
    HeapBlock *p_block = &p_heap->p_blocks[i_block];
    p_block->is_used = false;
    p_heap->cb_live -= p_block->cb_block;
    __atomic_sub_fetch(&s_cb_live, p_block->cb_block, __ATOMIC_RELAXED);
    if (i_block + 1 < p_heap->c_blocks && !p_heap->p_blocks[i_block + 1].is_used) {
        p_block->cb_block += p_heap->p_blocks[i_block + 1].cb_block;
        remove_block(p_heap, i_block + 1);
    }
    if (i_block > 0 && !p_heap->p_blocks[i_block - 1].is_used) {
        p_heap->p_blocks[i_block - 1].cb_block += p_heap->p_blocks[i_block].cb_block;
        remove_block(p_heap, i_block);
    }
}

/* The allocator sceNpHeapGetAllocator hands out: p_user is the guest's SceNpHeap, whose first word
 * is the mspace. A heap that was never initialized or was destroyed answers NULL, as the console's. */
static Heap *lock_heap_of(const char *sz_call, void *p_user) {
    uintptr_t va_mspace = 0;
    if (!read_guest(&va_mspace, (uintptr_t)p_user, sizeof(va_mspace))) {
        log_failure(sz_call, "unreadable SceNpHeap", (uintptr_t)p_user);
        return NULL;
    }
    pthread_mutex_lock(&s_heaps_lock);
    Heap *p_heap = find_heap(va_mspace);
    if (p_heap) return p_heap;
    pthread_mutex_unlock(&s_heaps_lock);
    log_failure(sz_call, va_mspace ? "SceNpHeap holds no live mspace" : "SceNpHeap not initialized", va_mspace);
    return NULL;
}
static ABI void *heap_malloc(uint64_t cb_request, void *p_user) {
    Heap *p_heap = lock_heap_of("SceNpHeap malloc", p_user);
    if (!p_heap) return NULL;
    void *p_block = heap_allocate_locked(p_heap, cb_request);
    pthread_mutex_unlock(&s_heaps_lock);
    if (!p_block) {
        log_failure("SceNpHeap malloc", "heap exhausted", (uintptr_t)cb_request);
        return NULL;
    }
    __atomic_add_fetch(&s_c_allocations, 1, __ATOMIC_RELAXED);
    return p_block;
}
static ABI void heap_free(void *p_block, void *p_user) {
    if (!p_block) return;
    Heap *p_heap = lock_heap_of("SceNpHeap free", p_user);
    if (!p_heap) return;
    const char *sz_reason;
    const size_t i_block = find_used_block(p_heap, (uintptr_t)p_block, &sz_reason);
    const bool is_found = i_block < p_heap->c_blocks;
    if (is_found) heap_free_locked(p_heap, i_block);
    pthread_mutex_unlock(&s_heaps_lock);
    if (!is_found) {
        log_failure("SceNpHeap free", sz_reason, (uintptr_t)p_block);
        return;
    }
    __atomic_add_fetch(&s_c_frees, 1, __ATOMIC_RELAXED);
}
/* dlmalloc's realloc, which the console's mspace is: a NULL block allocates, a zero size frees. */
static ABI void *heap_realloc(void *p_block, uint64_t cb_request, void *p_user) {
    if (!p_block) return heap_malloc(cb_request, p_user);
    if (!cb_request) {
        heap_free(p_block, p_user);
        return NULL;
    }
    Heap *p_heap = lock_heap_of("SceNpHeap realloc", p_user);
    if (!p_heap) return NULL;
    const char *sz_reason = "heap exhausted";
    const uint64_t cb_block = block_size_for(cb_request);
    const size_t i_block = find_used_block(p_heap, (uintptr_t)p_block, &sz_reason);
    void *p_result = NULL;
    if (i_block < p_heap->c_blocks && cb_block) {
        HeapBlock *p_old = &p_heap->p_blocks[i_block];
        const uint64_t cb_old = p_old->cb_block;
        HeapBlock *p_next = i_block + 1 < p_heap->c_blocks ? &p_heap->p_blocks[i_block + 1] : NULL;
        if (cb_block <= cb_old) {
            split_block(p_heap, i_block, cb_block);
            p_result = p_block;
        } else if (p_next && !p_next->is_used && cb_old + p_next->cb_block >= cb_block) {
            const uint64_t cb_grow = cb_block - cb_old;
            p_old->cb_block = cb_block;
            p_next->off_block += cb_grow;
            if (!(p_next->cb_block -= cb_grow)) remove_block(p_heap, i_block + 1);
            p_result = p_block;
        }
        if (p_result) {
            const uint64_t cb_new = p_heap->p_blocks[i_block].cb_block;
            p_heap->cb_live = p_heap->cb_live - cb_old + cb_new;
            if (p_heap->cb_live > p_heap->cb_max_live) p_heap->cb_max_live = p_heap->cb_live;
            if (cb_new >= cb_old) __atomic_add_fetch(&s_cb_live, cb_new - cb_old, __ATOMIC_RELAXED);
            else __atomic_sub_fetch(&s_cb_live, cb_old - cb_new, __ATOMIC_RELAXED);
        } else if ((p_result = heap_allocate_locked(p_heap, cb_request))) {
            runtime_memory_prepare_cpu_write((uintptr_t)p_result, cb_old);
            memcpy(p_result, p_block, cb_old);
            runtime_memory_note_cpu_write((uintptr_t)p_result, cb_old);
            heap_free_locked(p_heap, find_used_block(p_heap, (uintptr_t)p_block, &sz_reason));
        }
    }
    pthread_mutex_unlock(&s_heaps_lock);
    if (!p_result) {
        log_failure("SceNpHeap realloc", sz_reason, (uintptr_t)p_block);
        return NULL;
    }
    __atomic_add_fetch(&s_c_reallocations, 1, __ATOMIC_RELAXED);
    return p_result;
}

static ABI int32_t heap_init(uint64_t *p_heap, void *p_base, uint64_t cb_base, const char *sz_name) {
    note_use();
    uint64_t va_mspace;
    if (!read_guest(&va_mspace, (uintptr_t)p_heap, sizeof(va_mspace))) {
        log_failure("sceNpHeapInit", "unreadable SceNpHeap", (uintptr_t)p_heap);
        return NP_COMMON_OUT_OF_MEMORY;
    }
    const uintptr_t va_base = ((uintptr_t)p_base + CB_BLOCK_ALIGNMENT - 1) & ~(uintptr_t)(CB_BLOCK_ALIGNMENT - 1);
    const uint64_t cb_skipped = va_base - (uintptr_t)p_base;
    const uint64_t cb_heap = cb_base > cb_skipped ? (cb_base - cb_skipped) & ~(CB_BLOCK_ALIGNMENT - 1) : 0;
    if (!p_base || va_base < (uintptr_t)p_base || cb_heap < CB_HEAP_MIN || cb_heap > CB_HEAP_MAX || !is_guest_range(va_base, cb_heap)) {
        log_failure("sceNpHeapInit", "buffer missing, unmapped or outside the size limits", (uintptr_t)p_base);
        return NP_COMMON_OUT_OF_MEMORY;
    }
    char sz_heap_name[CB_NAME];
    read_guest_name(sz_name, sz_heap_name);
    HeapBlock *p_blocks = malloc(64 * sizeof(*p_blocks));
    if (!p_blocks) {
        log_failure("sceNpHeapInit", "host allocation failed", (uintptr_t)p_base);
        return NP_COMMON_OUT_OF_MEMORY;
    }
    p_blocks[0] = (HeapBlock){0, cb_heap, false};
    size_t c_replaced = 0;
    Heap *p_slot = NULL;
    pthread_mutex_lock(&s_heaps_lock);
    /* The console's mspace is rebuilt over whatever the buffer held, so an older heap over the same
     * memory is gone from then on. */
    for (size_t i = 0; i < C_HEAPS_MAX; ++i) {
        Heap *p_old = &s_heaps[i];
        if (p_old->va_base && p_old->va_base < va_base + cb_heap && va_base < p_old->va_base + p_old->cb_heap) {
            release_heap(p_old);
            ++c_replaced;
        }
        if (!p_old->va_base && !p_slot) p_slot = p_old;
    }
    if (p_slot) {
        *p_slot = (Heap){va_base, cb_heap, 0, 0, p_blocks, 1, 64, {0}};
        memcpy(p_slot->sz_name, sz_heap_name, sizeof(sz_heap_name));
    }
    pthread_mutex_unlock(&s_heaps_lock);
    if (c_replaced) {
        __atomic_add_fetch(&s_c_heaps_destroyed, c_replaced, __ATOMIC_RELAXED);
        log_failure("sceNpHeapInit", "live heap over the same buffer dropped", va_base);
    }
    if (!p_slot) {
        free(p_blocks);
        log_failure("sceNpHeapInit", "heap table full", (uintptr_t)p_base);
        return NP_COMMON_OUT_OF_MEMORY;
    }
    *p_heap = va_base;
    __atomic_add_fetch(&s_c_heaps_created, 1, __ATOMIC_RELAXED);
    printf("Runtime: Np heap '%s' at 0x%" PRIxPTR ", %" PRIu64 " bytes\n", sz_heap_name, va_base, cb_heap);
    return 0;
}
static ABI void heap_destroy(uint64_t *p_heap) {
    uint64_t va_mspace;
    if (!read_guest(&va_mspace, (uintptr_t)p_heap, sizeof(va_mspace))) {
        log_failure("sceNpHeapDestroy", "unreadable SceNpHeap", (uintptr_t)p_heap);
        return;
    }
    if (!va_mspace) return;
    Heap destroyed = {0};
    pthread_mutex_lock(&s_heaps_lock);
    Heap *p_heap_found = find_heap(va_mspace);
    if (p_heap_found) {
        destroyed = *p_heap_found;
        release_heap(p_heap_found);
    }
    pthread_mutex_unlock(&s_heaps_lock);
    *p_heap = 0;
    if (!destroyed.va_base) {
        log_failure("sceNpHeapDestroy", "SceNpHeap holds no live mspace", va_mspace);
        return;
    }
    __atomic_add_fetch(&s_c_heaps_destroyed, 1, __ATOMIC_RELAXED);
    printf("Runtime: Np heap '%s' at 0x%" PRIxPTR " destroyed, %" PRIu64 " bytes still allocated\n", destroyed.sz_name,
           destroyed.va_base, destroyed.cb_live);
}
static ABI void heap_get_allocator(uint64_t *p_heap, NpAllocator *p_out_allocator) {
    if (!is_guest_range((uintptr_t)p_out_allocator, sizeof(*p_out_allocator))) {
        log_failure("sceNpHeapGetAllocator", "unreadable SceNpAllocator", (uintptr_t)p_out_allocator);
        return;
    }
    *p_out_allocator = (NpAllocator){heap_malloc, heap_realloc, heap_free, p_heap};
}
/* The heap-first forms libraries such as libSceNpAuth call directly, over the same mspace. */
static ABI void *heap_malloc_in(uint64_t *p_heap, uint64_t cb_request) { return heap_malloc(cb_request, p_heap); }
static ABI void *heap_realloc_in(uint64_t *p_heap, void *p_block, uint64_t cb_request) {
    return heap_realloc(p_block, cb_request, p_heap);
}
static ABI void heap_free_in(uint64_t *p_heap, void *p_block) { heap_free(p_block, p_heap); }
/* The console fills it from sceLibcMspaceMallocStats: the pool is the mspace's maxSystemSize, and
 * the stat is left untouched when that call fails. */
static ABI int32_t heap_get_stat(uint64_t *p_heap, NpHeapStat *p_out_stat) {
    if (!is_guest_range((uintptr_t)p_out_stat, sizeof(*p_out_stat))) {
        log_failure("sceNpHeapGetStat", "unwritable SceNpHeapStat", (uintptr_t)p_out_stat);
        return NP_COMMON_INVALID_ARGUMENT;
    }
    Heap *p_found = lock_heap_of("sceNpHeapGetStat", p_heap);
    if (!p_found) return NP_COMMON_INVALID_ARGUMENT;
    const NpHeapStat stat = {p_found->cb_heap, p_found->cb_max_live, p_found->cb_live};
    pthread_mutex_unlock(&s_heaps_lock);
    *p_out_stat = stat;
    return 0;
}

static int32_t allocate_kernel_memory(const char *sz_call, uint64_t cb_memory, const char *sz_name, void **pp_out_memory,
                                      NpKernelMemory *p_memory) {
    note_use();
    if (!p_memory) {
        log_failure(sz_call, "NULL SceNpKernelMemory", 0);
        return NP_COMMON_INVALID_ARGUMENT;
    }
    if (!is_guest_range((uintptr_t)p_memory, sizeof(*p_memory))) {
        log_failure(sz_call, "unreadable SceNpKernelMemory", (uintptr_t)p_memory);
        return NP_COMMON_INVALID_ARGUMENT;
    }
    const MapNamedFlexible pfn_map = (MapNamedFlexible)runtime_memory_resolve("mL8NDH86iQI");
    if (!pfn_map) {
        log_failure(sz_call, "sceKernelMapNamedFlexibleMemory not resolved", 0);
        return NP_COMMON_INVALID_ARGUMENT;
    }
    p_memory->cb_memory = cb_memory;
    p_memory->reserved = 0;
    const int32_t result = pfn_map(&p_memory->p_memory, cb_memory, KERNEL_PROT_READ_WRITE, 0, sz_name);
    char sz_memory_name[CB_NAME];
    read_guest_name(sz_name, sz_memory_name);
    if (result < 0) {
        log_failure(sz_call, "sceKernelMapNamedFlexibleMemory failed", (uintptr_t)(uint32_t)result);
        return result;
    }
    if (pp_out_memory) *pp_out_memory = p_memory->p_memory;
    __atomic_add_fetch(&s_c_kernel_maps, 1, __ATOMIC_RELAXED);
    printf("Runtime: Np kernel memory '%s' at %p, %" PRIu64 " bytes\n", sz_memory_name, p_memory->p_memory, cb_memory);
    return result;
}
/* A size within 0x4000 of the top wraps to 0 here, which the map refuses, where the console's
 * rounding wraps to a small size. */
static ABI int32_t allocate_kernel_memory_aligned(uint64_t cb_memory, const char *sz_name, void **pp_out_memory,
                                                  NpKernelMemory *p_memory) {
    const uint64_t cb_aligned = (cb_memory + CB_KERNEL_MEMORY_ALIGNMENT - 1) & ~(CB_KERNEL_MEMORY_ALIGNMENT - 1);
    return allocate_kernel_memory("sceNpAllocateKernelMemoryWithAlignment", cb_aligned >= cb_memory ? cb_aligned : 0, sz_name,
                                  pp_out_memory, p_memory);
}
static ABI int32_t allocate_kernel_memory_unaligned(uint64_t cb_memory, const char *sz_name, void **pp_out_memory,
                                                    NpKernelMemory *p_memory) {
    return allocate_kernel_memory("sceNpAllocateKernelMemoryNoAlignment", cb_memory, sz_name, pp_out_memory, p_memory);
}
/* The console ignores the unmap's result and answers 0. */
static ABI int32_t free_kernel_memory(NpKernelMemory *p_memory) {
    if (!p_memory) {
        log_failure("sceNpFreeKernelMemory", "NULL SceNpKernelMemory", 0);
        return NP_COMMON_INVALID_ARGUMENT;
    }
    if (!is_guest_range((uintptr_t)p_memory, sizeof(*p_memory))) {
        log_failure("sceNpFreeKernelMemory", "unreadable SceNpKernelMemory", (uintptr_t)p_memory);
        return NP_COMMON_INVALID_ARGUMENT;
    }
    if (p_memory->p_memory) {
        const Unmap pfn_unmap = (Unmap)runtime_memory_resolve("cQke9UuBQOk");
        const int32_t result = pfn_unmap ? pfn_unmap(p_memory->p_memory, p_memory->cb_memory) : NP_COMMON_INVALID_ARGUMENT;
        if (result) log_failure("sceNpFreeKernelMemory", "sceKernelMunmap failed", (uintptr_t)p_memory->p_memory);
        else __atomic_add_fetch(&s_c_kernel_unmaps, 1, __ATOMIC_RELAXED);
    }
    p_memory->p_memory = NULL;
    return 0;
}

/* Each object is preceded by 0x10 bytes whose first word points at the allocator it came from, the
 * caller's SceNpAllocator itself, which delete frees it through. */
static ABI void *object_new(uint64_t cb_object, NpAllocator *p_allocator) {
    NpAllocator allocator;
    if (!read_guest(&allocator, (uintptr_t)p_allocator, sizeof(allocator)) || !allocator.pfn_malloc) {
        log_failure("sce::np::Object::operator new", "unreadable SceNpAllocator or no malloc", (uintptr_t)p_allocator);
        return NULL;
    }
    if (cb_object > UINT64_MAX - CB_OBJECT_HEADER) {
        log_failure("sce::np::Object::operator new", "size overflows", (uintptr_t)cb_object);
        return NULL;
    }
    unsigned char *p_block = allocator.pfn_malloc(cb_object + CB_OBJECT_HEADER, allocator.p_user);
    if (!p_block) {
        log_failure("sce::np::Object::operator new", "allocator returned NULL", (uintptr_t)p_allocator);
        return NULL;
    }
    *(NpAllocator **)p_block = p_allocator;
    __atomic_add_fetch(&s_c_objects_created, 1, __ATOMIC_RELAXED);
    return p_block + CB_OBJECT_HEADER;
}
static ABI void object_delete(void *p_object) {
    if (!p_object) return;
    const uintptr_t va_block = (uintptr_t)p_object - CB_OBJECT_HEADER;
    uintptr_t va_allocator = 0;
    NpAllocator allocator;
    if (!read_guest(&va_allocator, va_block, sizeof(va_allocator)) || !read_guest(&allocator, va_allocator, sizeof(allocator)) ||
        !allocator.pfn_free) {
        log_failure("sce::np::Object::operator delete", "object header names no readable allocator", (uintptr_t)p_object);
        return;
    }
    allocator.pfn_free((void *)va_block, allocator.p_user);
    __atomic_add_fetch(&s_c_objects_deleted, 1, __ATOMIC_RELAXED);
}
/* The console's placement forms free through the object's header too, not the allocator passed. */
static ABI void object_delete_placed(void *p_object, void *p_allocator) {
    (void)p_allocator;
    object_delete(p_object);
}

static const RuntimeExport s_exports[] = {
    {"sceNpAllocateKernelMemoryWithAlignment", allocate_kernel_memory_aligned},
    {"sceNpAllocateKernelMemoryNoAlignment", allocate_kernel_memory_unaligned},
    {"sceNpFreeKernelMemory", free_kernel_memory},
    {"sceNpHeapInit", heap_init}, {"sceNpHeapDestroy", heap_destroy}, {"sceNpHeapGetAllocator", heap_get_allocator},
    {"_sceNpHeapMalloc", heap_malloc_in}, {"_sceNpHeapRealloc", heap_realloc_in}, {"_sceNpHeapFree", heap_free_in},
    {"sceNpHeapGetStat", heap_get_stat},
    {"_ZN3sce2np6ObjectnwEmR14SceNpAllocator", object_new}, {"_ZN3sce2np6ObjectnaEmR14SceNpAllocator", object_new},
    {"_ZN3sce2np6ObjectdlEPv", object_delete}, {"_ZN3sce2np6ObjectdaEPv", object_delete},
    {"_ZN3sce2np6ObjectdlEPvR14SceNpAllocator", object_delete_placed},
    {"_ZN3sce2np6ObjectdaEPvR14SceNpAllocator", object_delete_placed},
    {"_ZN3sce2np6ObjectdlEPvR16SceNpAllocatorEx", object_delete_placed},
    {"_ZN3sce2np6ObjectdaEPvR16SceNpAllocatorEx", object_delete_placed},
};
uintptr_t runtime_np_heap_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports, sz_name); }
