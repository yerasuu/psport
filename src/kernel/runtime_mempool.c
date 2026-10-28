// SPDX-License-Identifier: GPL-2.0-or-later
/* libkernel memory pools: sceKernelMemoryPoolExpand takes direct memory out of
 * the direct pool for the process's memory pool, sceKernelMemoryPoolReserve
 * reserves a virtual range for it, and commits map the pool's memory into that
 * range in 64 KiB blocks, wherever free blocks are, until decommits give them
 * back. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define BLOCK UINT64_C(0x10000)
#define RESERVE_ALIGN UINT64_C(0x200000)
#define PROT_CPU_EXEC 4
#define MAX_EXTENTS 1024
typedef struct { uint64_t phys, cb_range; } Extent;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static Extent s_extents[MAX_EXTENTS]; /* free pool memory, sorted by phys, never adjacent */
static size_t s_c_extents;
static uint64_t s_cb_expanded, s_cb_free;
static size_t s_c_expands, s_c_reserves, s_c_commits, s_c_decommits;

/* Lock held; 0 when the free list is full. */
static int give_back(uint64_t phys, uint64_t cb_range) {
    size_t i=0;
    while (i<s_c_extents && s_extents[i].phys<phys) ++i;
    const int joins_left=i>0 && s_extents[i-1].phys+s_extents[i-1].cb_range==phys;
    const int joins_right=i<s_c_extents && phys+cb_range==s_extents[i].phys;
    if (joins_left && joins_right) {
        s_extents[i-1].cb_range+=cb_range+s_extents[i].cb_range;
        for (size_t j=i;j+1<s_c_extents;++j) s_extents[j]=s_extents[j+1];
        --s_c_extents;
    } else if (joins_left) {
        s_extents[i-1].cb_range+=cb_range;
    } else if (joins_right) {
        s_extents[i].phys=phys; s_extents[i].cb_range+=cb_range;
    } else {
        if (s_c_extents==MAX_EXTENTS) return 0;
        for (size_t j=s_c_extents;j>i;--j) s_extents[j]=s_extents[j-1];
        s_extents[i]=(Extent){phys,cb_range};
        ++s_c_extents;
    }
    s_cb_free+=cb_range;
    return 1;
}
static void released(uint64_t phys, uint64_t cb_range) {
    if (!give_back(phys,cb_range))
        fprintf(stderr,"Runtime: memory pool free list full: %#" PRIx64 "+%#" PRIx64 " is lost to the pool\n",phys,cb_range);
}
static void take(uint64_t cb_range) {
    s_extents[0].phys+=cb_range; s_extents[0].cb_range-=cb_range; s_cb_free-=cb_range;
    if (!s_extents[0].cb_range) {
        for (size_t j=0;j+1<s_c_extents;++j) s_extents[j]=s_extents[j+1];
        --s_c_extents;
    }
}

static ABI int32_t pool_expand(uint64_t low, uint64_t high, uint64_t cb_range, uint64_t alignment, uint64_t *p_out) {
    if (high<=low || !cb_range || cb_range%BLOCK || alignment%BLOCK || !p_out) return INVALID;
    if (high-low<cb_range) return NO_MEMORY;
    int64_t phys;
    pthread_mutex_lock(&s_lock);
    int32_t result=s_c_extents==MAX_EXTENTS ? NO_MEMORY
                                             : runtime_memory_pool_expand(low,high,cb_range,alignment ? alignment : BLOCK,&phys);
    if (!result) { give_back((uint64_t)phys,cb_range); s_cb_expanded+=cb_range; ++s_c_expands; }
    pthread_mutex_unlock(&s_lock);
    if (result) {
        printf("Runtime: memory pool expand by %#" PRIx64 " in [%#" PRIx64 ", %#" PRIx64 ") failed (error %#x)\n",
               cb_range,low,high,(unsigned)result);
        return NO_MEMORY;
    }
    *p_out=(uint64_t)phys;
    printf("Runtime: memory pool expanded by %#" PRIx64 " bytes at direct memory %#" PRIx64 "\n",cb_range,(uint64_t)phys);
    return 0;
}
static ABI int32_t pool_reserve(void *p_address, uint64_t cb_range, uint64_t alignment, int flags, void **pp_out) {
    if (!pp_out || !cb_range || cb_range%RESERVE_ALIGN || (alignment & (alignment-1))) return INVALID;
    void *p_at=p_address;
    const int32_t result=runtime_memory_pool_reserve(&p_at,cb_range,flags,alignment ? alignment : RESERVE_ALIGN);
    if (result) {
        printf("Runtime: memory pool reserve of %#" PRIx64 " bytes at %p failed (error %#x)\n",cb_range,p_address,(unsigned)result);
        return result;
    }
    __atomic_add_fetch(&s_c_reserves,1,__ATOMIC_RELAXED);
    *pp_out=p_at;
    printf("Runtime: memory pool reserved %p, size=%#" PRIx64 "\n",p_at,cb_range);
    return 0;
}
static ABI int32_t pool_commit(void *p_address, uint64_t cb_range, int type, int prot, int flags) {
    (void)flags;
    if (!p_address || !cb_range || cb_range%BLOCK || (prot & PROT_CPU_EXEC) || (prot & ~0x37) ||
        type<0 || type>10)
        return INVALID;
    const uintptr_t start=(uintptr_t)p_address;
    pthread_mutex_lock(&s_lock);
    int32_t result=s_cb_free<cb_range ? NO_MEMORY : 0;
    uint64_t done=0;
    while (!result && done<cb_range) {
        const uint64_t piece=s_extents[0].cb_range<cb_range-done ? s_extents[0].cb_range : cb_range-done;
        result=runtime_memory_pool_commit(start+done,piece,prot,type,s_extents[0].phys);
        if (!result) { take(piece); done+=piece; }
    }
    if (result && done) runtime_memory_pool_decommit(start,done,released);
    if (!result) ++s_c_commits;
    const uint64_t pool_free=s_cb_free;
    pthread_mutex_unlock(&s_lock);
    if (result)
        printf("Runtime: memory pool commit of %#" PRIx64 " bytes at %p failed (error %#x), pool free=%#" PRIx64 "\n",
               cb_range,p_address,(unsigned)result,pool_free);
    return result;
}
static ABI int32_t pool_decommit(void *p_address, uint64_t cb_range, int flags) {
    (void)flags;
    if (!p_address || !cb_range || cb_range%BLOCK) return INVALID;
    pthread_mutex_lock(&s_lock);
    const int32_t result=runtime_memory_pool_decommit((uintptr_t)p_address,cb_range,released);
    if (!result) ++s_c_decommits;
    pthread_mutex_unlock(&s_lock);
    if (result) printf("Runtime: memory pool decommit of %#" PRIx64 " bytes at %p failed (error %#x)\n",cb_range,p_address,(unsigned)result);
    return result;
}

static const RuntimeExport s_exports[]={
    {"sceKernelMemoryPoolExpand",pool_expand}, {"sceKernelMemoryPoolReserve",pool_reserve},
    {"sceKernelMemoryPoolCommit",pool_commit}, {"sceKernelMemoryPoolDecommit",pool_decommit},
};
uintptr_t runtime_mempool_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_mempool_report(void) {
    if (!runtime_exception_report_lock(&s_lock,"memory pool")) return;
    printf("Runtime: memory pool expands=%zu (%#" PRIx64 " bytes), reserves=%zu, commits=%zu, decommits=%zu, "
           "committed=%#" PRIx64 "\n",
           s_c_expands,s_cb_expanded,s_c_reserves,s_c_commits,s_c_decommits,s_cb_expanded-s_cb_free);
    runtime_exception_unlock(&s_lock);
}
