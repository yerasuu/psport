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
#ifndef _WIN32
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define BLOCK UINT64_C(0x10000)
#define RESERVE_ALIGN UINT64_C(0x200000)
#define PROT_CPU_EXEC 4
#define MAX_EXTENTS 1024
typedef struct { uint64_t phys, size; } Extent;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Extent extents[MAX_EXTENTS]; /* free pool memory, sorted by phys, never adjacent */
static size_t extent_count;
static uint64_t expanded_bytes, free_bytes;
static size_t expands, reserves, commits, decommits;

/* Lock held; 0 when the free list is full. */
static int give_back(uint64_t phys, uint64_t size) {
    size_t i=0;
    while (i<extent_count && extents[i].phys<phys) ++i;
    const int joins_left=i>0 && extents[i-1].phys+extents[i-1].size==phys;
    const int joins_right=i<extent_count && phys+size==extents[i].phys;
    if (joins_left && joins_right) {
        extents[i-1].size+=size+extents[i].size;
        for (size_t j=i;j+1<extent_count;++j) extents[j]=extents[j+1];
        --extent_count;
    } else if (joins_left) {
        extents[i-1].size+=size;
    } else if (joins_right) {
        extents[i].phys=phys; extents[i].size+=size;
    } else {
        if (extent_count==MAX_EXTENTS) return 0;
        for (size_t j=extent_count;j>i;--j) extents[j]=extents[j-1];
        extents[i]=(Extent){phys,size};
        ++extent_count;
    }
    free_bytes+=size;
    return 1;
}
static void released(uint64_t phys, uint64_t size) {
    if (!give_back(phys,size))
        fprintf(stderr,"Runtime: memory pool free list full: %#" PRIx64 "+%#" PRIx64 " is lost to the pool\n",phys,size);
}
static void take(uint64_t size) {
    extents[0].phys+=size; extents[0].size-=size; free_bytes-=size;
    if (!extents[0].size) {
        for (size_t j=0;j+1<extent_count;++j) extents[j]=extents[j+1];
        --extent_count;
    }
}

static ABI int32_t pool_expand(uint64_t low, uint64_t high, uint64_t size, uint64_t alignment, uint64_t *out) {
    if (high<=low || !size || size%BLOCK || alignment%BLOCK || !out) return INVALID;
    if (high-low<size) return NO_MEMORY;
    int64_t phys;
    pthread_mutex_lock(&lock);
    int32_t result=extent_count==MAX_EXTENTS ? NO_MEMORY
                                             : runtime_memory_pool_expand(low,high,size,alignment ? alignment : BLOCK,&phys);
    if (!result) { give_back((uint64_t)phys,size); expanded_bytes+=size; ++expands; }
    pthread_mutex_unlock(&lock);
    if (result) {
        printf("Runtime: memory pool expand by %#" PRIx64 " in [%#" PRIx64 ", %#" PRIx64 ") failed (error %#x)\n",
               size,low,high,(unsigned)result);
        return NO_MEMORY;
    }
    *out=(uint64_t)phys;
    printf("Runtime: memory pool expanded by %#" PRIx64 " bytes at direct memory %#" PRIx64 "\n",size,(uint64_t)phys);
    return 0;
}
static ABI int32_t pool_reserve(void *address, uint64_t size, uint64_t alignment, int flags, void **out) {
    if (!out || !size || size%RESERVE_ALIGN || (alignment & (alignment-1))) return INVALID;
    void *at=address;
    const int32_t result=runtime_memory_pool_reserve(&at,size,flags,alignment ? alignment : RESERVE_ALIGN);
    if (result) {
        printf("Runtime: memory pool reserve of %#" PRIx64 " bytes at %p failed (error %#x)\n",size,address,(unsigned)result);
        return result;
    }
    __atomic_add_fetch(&reserves,1,__ATOMIC_RELAXED);
    *out=at;
    printf("Runtime: memory pool reserved %p, size=%#" PRIx64 "\n",at,size);
    return 0;
}
static ABI int32_t pool_commit(void *address, uint64_t size, int type, int prot, int flags) {
    (void)flags;
    if (!address || !size || size%BLOCK || (prot & PROT_CPU_EXEC) || (prot & ~0x37) ||
        type<0 || type>10)
        return INVALID;
    const uintptr_t start=(uintptr_t)address;
    pthread_mutex_lock(&lock);
    int32_t result=free_bytes<size ? NO_MEMORY : 0;
    uint64_t done=0;
    while (!result && done<size) {
        const uint64_t piece=extents[0].size<size-done ? extents[0].size : size-done;
        result=runtime_memory_pool_commit(start+done,piece,prot,type,extents[0].phys);
        if (!result) { take(piece); done+=piece; }
    }
    if (result && done) runtime_memory_pool_decommit(start,done,released);
    if (!result) ++commits;
    const uint64_t pool_free=free_bytes;
    pthread_mutex_unlock(&lock);
    if (result)
        printf("Runtime: memory pool commit of %#" PRIx64 " bytes at %p failed (error %#x), pool free=%#" PRIx64 "\n",
               size,address,(unsigned)result,pool_free);
    return result;
}
static ABI int32_t pool_decommit(void *address, uint64_t size, int flags) {
    (void)flags;
    if (!address || !size || size%BLOCK) return INVALID;
    pthread_mutex_lock(&lock);
    const int32_t result=runtime_memory_pool_decommit((uintptr_t)address,size,released);
    if (!result) ++decommits;
    pthread_mutex_unlock(&lock);
    if (result) printf("Runtime: memory pool decommit of %#" PRIx64 " bytes at %p failed (error %#x)\n",size,address,(unsigned)result);
    return result;
}

static const RuntimeExport exports[]={
    {"sceKernelMemoryPoolExpand",pool_expand}, {"sceKernelMemoryPoolReserve",pool_reserve},
    {"sceKernelMemoryPoolCommit",pool_commit}, {"sceKernelMemoryPoolDecommit",pool_decommit},
};
uintptr_t runtime_mempool_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_mempool_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: memory pool expands=%zu (%#" PRIx64 " bytes), reserves=%zu, commits=%zu, decommits=%zu, "
           "committed=%#" PRIx64 "\n",
           expands,expanded_bytes,reserves,commits,decommits,expanded_bytes-free_bytes);
    pthread_mutex_unlock(&lock);
}
#else
uintptr_t runtime_mempool_resolve(const char *name) { (void)name; return 0; }
void runtime_mempool_report(void) {}
#endif
