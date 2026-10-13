/* libkernel memory calls: direct memory allocation and mapping, flexible memory, mmap,
 * virtual ranges, protection, queries, batch maps and the memory pools' side (runtime_mempool.c). */
#define _GNU_SOURCE
#include "runtime_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#ifndef _WIN32
#define LIMIT 4096
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
typedef struct { uint64_t start, end; int32_t memory_type; } DirectQueryInfo;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");
_Static_assert(sizeof(DirectQueryInfo)==24,"PS4 direct memory query info layout");
static Block blocks[LIMIT];
static size_t allocations, maps, reservations, flexible_maps, protects, queries;
static uint64_t live_bytes;

static int trace_memory(void) { static int v=-1; if (v<0) v=getenv("BB_TRACE_MEM")!=NULL; return v; }
static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

static ABI uint64_t direct_size(void) { return POOL_SIZE; }
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) return NO_SPACE;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    ensure_chunks(start,size);
    ++allocations; live_bytes += size;
    write_unlock();
    if (trace_memory()) printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
/* The main pool is the whole direct memory the port offers: newer SDKs allocate from it without a
 * search range. */
static ABI int32_t direct_allocate_main(uint64_t size, uint64_t alignment, int type, int64_t *out) {
    return direct_allocate(0, (int64_t)POOL_SIZE, size, alignment, type, out);
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (result) printf("Runtime: mapping direct memory %#" PRIx64 ", size=%" PRIu64 " failed (error %#x)\n", (uint64_t)physical, size, (unsigned)result);
    else if (trace_memory()) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (munmap((void *)a,b-a)) return INVALID;
    }
    drop_range(start,end);
    trap_forget(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static int32_t release_direct(uint64_t start, uint64_t size, int32_t unallocated) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return unallocated; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    zero_phys(start,size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) { return release_direct(start,size,INVALID); }
static ABI int32_t direct_checked_release(uint64_t start, uint64_t size) { return release_direct(start,size,NOT_FOUND); }
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
/* mmap (sceKernelMmap, and POSIX mmap over it): the game's libc grows its malloc heap with
 * anonymous private mappings once the heap it started with is full (seen after minutes of area
 * streaming). They come from flexible memory as sceKernelMapFlexibleMemory does, at the hint
 * when it is free, as FreeBSD places them. MAP_VOID reserves the range as
 * sceKernelReserveVirtualRange does. A file is mapped as a copy of its contents, read-only
 * (IL2CPP maps global-metadata.dat that way); a writable shared one would need writing back and
 * is refused. */
enum { MAP_SHARED_FLAG=1, MAP_VOID_FLAG=0x100, MAP_ANON_FLAG=0x1000 };
static _Atomic uint64_t heap_growths;
uint64_t runtime_heap_growths(void) { return atomic_load(&heap_growths); }
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot);
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment);
static int32_t map_file(void **inout, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    if ((flags & MAP_SHARED_FLAG) && (prot & 2)) return ERR(19); /* ENODEV */
    if (offset<0 || offset%PAGE) return INVALID;
    const uint64_t length=align_up(size,PAGE);
    const int32_t result=map_flexible(inout,length,(prot & 0x37)|3,flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE));
    if (result) return result;
    const int64_t read=runtime_file_pread(fd,*inout,size,offset);
    if (read<0) {
        direct_unmap(*inout,length);
        return ERR(runtime_guest_errno((int)-read));
    }
    if (!(prot & 2)) kernel_mprotect(*inout,length,prot & 0x37);
    printf("Runtime: mmap of file descriptor %d: %#" PRIx64 " bytes from offset %#" PRIx64 " copied to %p\n",
           fd,(uint64_t)read,(uint64_t)offset,*inout);
    return 0;
}
static ABI int32_t kernel_mmap(void *addr, uint64_t size, int prot, int flags, int fd, int64_t offset, void **out) {
    if (!out || !size) return INVALID;
    void *at=addr;
    int32_t result;
    if (flags & MAP_VOID_FLAG)
        result=reserve_range(&at,align_up(size,PAGE),flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE),PAGE);
    else if (!(flags & MAP_ANON_FLAG))
        result=fd<0 ? ERR(9) /* EBADF */ : map_file(&at,size,prot,flags,fd,offset);
    else if (fd!=-1 || offset)
        result=INVALID;
    else
        result=map_flexible(&at,align_up(size,PAGE),prot & 0x37,flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE));
    if (trace_memory() || result)
        printf("Runtime: mmap %p+%#" PRIx64 " prot=0x%x flags=0x%x fd=%d offset=%#" PRIx64 " -> %p (0x%x)\n",
               addr,size,prot,flags,fd,(uint64_t)offset,result ? NULL : at,(unsigned)result);
    if (!result) *out=at;
    return result;
}
static ABI void *posix_mmap(void *addr, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    void *out=NULL;
    const int32_t result=kernel_mmap(addr,size,prot,flags,fd,offset,&out);
    if ((flags & MAP_ANON_FLAG) && !(flags & MAP_VOID_FLAG)) {
        atomic_fetch_add(&heap_growths,1);
        uint64_t sites[3];
        runtime_guest_call_sites(sites);
        printf("Runtime: mmap %#" PRIx64 " bytes for the game (its heap grows) from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
               size,sites[0],sites[1],sites[2]);
    }
    if (result) { *runtime_errno()=result & 0xffff; return (void *)-1; }
    return out;
}
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    /* MAP_FIXED replaces what is mapped there (games decommit by reserving over their mappings),
     * so the GPU forgets the range first, as for an unmap. */
    if ((flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE))==MAP_FIXED_FLAG && hook_unmap && *inout && !((uintptr_t)*inout%PAGE))
        hook_unmap((uintptr_t)*inout,size);
    write_lock();
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    if (!result) ++reservations;
    write_unlock();
    if (!result && trace_memory()) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    if (mprotect((void *)start,end-start,host_prot(prot))) return INVALID;
    trap_reapply(start,end); /* pages with write traps stay read-only */
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.pooled ? 8u : 0) |
                (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
/* The allocation holding offset (flags 1: else the first one above it), reported to the end of
 * the run of allocations that follow it without a gap and with its type, as libkernel merges them. */
static ABI int32_t direct_memory_query(uint64_t offset, int flags, DirectQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    if (offset>=POOL_SIZE) return ACCESS;
    read_lock();
    int found=-1;
    for (int i=0;i<LIMIT;++i) {
        if (!blocks[i].used) continue;
        if (offset>=blocks[i].start && offset-blocks[i].start<blocks[i].size) { found=i; break; }
        if (flags==1 && blocks[i].start>offset && (found<0 || blocks[i].start<blocks[found].start)) found=i;
    }
    if (found<0) { read_unlock(); return ACCESS; }
    const int type=blocks[found].type;
    uint64_t end=blocks[found].start+blocks[found].size;
    for (int grew=1; grew;) {
        grew=0;
        for (int i=0;i<LIMIT;++i)
            if (blocks[i].used && blocks[i].start==end && blocks[i].type==type) { end+=blocks[i].size; grew=1; }
    }
    info->start=blocks[found].start; info->end=end; info->memory_type=type;
    read_unlock();
    return 0;
}
/* Locking only keeps pages resident, which the guest cannot observe: the range is checked and
 * left as it is. */
static ABI int32_t lock_range(const void *address, uint64_t size) {
    if (!address || !size || (uintptr_t)address+size<(uintptr_t)address) return INVALID;
    return 0;
}
/* Range names only label mappings for the console's debugging tools, so they are checked and
 * dropped; VirtualQuery names a range by its kind. */
static ABI int32_t set_virtual_range_name(const void *address, uint64_t size, const char *name) {
    (void)address; (void)size;
    if (!name) return (int32_t)UINT32_C(0x8002000e); /* EFAULT */
    if (strnlen(name,32)>=32) return (int32_t)UINT32_C(0x8002003f); /* ENAMETOOLONG */
    return 0;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static ABI int32_t available_flexible_size(uint64_t *out) {
    if (!out) return INVALID;
    read_lock();
    *out=FLEXIBLE_SIZE-flexible_bytes;
    read_unlock();
    return 0;
}
static ABI int32_t configured_flexible_size(uint64_t *out) {
    if (!out) return INVALID;
    *out=FLEXIBLE_SIZE;
    return 0;
}
/* The start of the first allocation in [at, end), else end (lock held). */
static uint64_t next_block_start(uint64_t at, uint64_t end) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && blocks[i].start>=at && blocks[i].start<end) end=blocks[i].start;
    return end;
}
/* The largest free range of direct memory in [low, high) once its start is aligned, as libkernel
 * answers it. */
static ABI int32_t available_direct_size(uint64_t low, uint64_t high, uint64_t alignment, uint64_t *phys, uint64_t *size) {
    if (!alignment) alignment=PAGE;
    if (!phys || !size || !valid_alignment(alignment)) return INVALID;
    const uint64_t end=high<POOL_SIZE ? high : POOL_SIZE;
    uint64_t best_start=0, best_size=0;
    read_lock();
    for (uint64_t at=low; at<end;) {
        int inside=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; inside=1; break;
        }
        if (inside) continue;
        const uint64_t gap_end=next_block_start(at,end), start=align_up(at,alignment);
        if (start<gap_end && gap_end-start>best_size) { best_start=start; best_size=gap_end-start; }
        at=gap_end;
    }
    read_unlock();
    if (!best_size) return NO_MEMORY;
    *phys=best_start; *size=best_size;
    return 0;
}
/* Memory pools (runtime_mempool.c): a pool reservation is a reserved range marked pooled, and
 * committing maps direct memory the pool expanded into there, marked pooled too. */
int32_t runtime_memory_pool_expand(uint64_t low, uint64_t high, uint64_t size, uint64_t alignment, int64_t *phys) {
    return direct_allocate((int64_t)low,(int64_t)high,size,alignment,0,phys);
}
static void mark_pooled(uintptr_t start, uintptr_t end) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) vmas[i].pooled=1;
}
static int all_pooled(uintptr_t start, uintptr_t end, int kind) {
    if (!covered(start,end,1)) return 0;
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!vmas[i].pooled || (kind && vmas[i].kind!=kind)) return 0;
    return 1;
}
int32_t runtime_memory_pool_reserve(void **inout, uint64_t size, int flags, uint64_t alignment) {
    int32_t result=reserve_range(inout,size,flags,alignment);
    if (result) return result;
    write_lock();
    mark_pooled((uintptr_t)*inout,(uintptr_t)*inout+size);
    write_unlock();
    return 0;
}
int32_t runtime_memory_pool_commit(uintptr_t address, uint64_t size, int prot, int type, uint64_t phys) {
    write_lock();
    if (pool() || !all_pooled(address,address+size,KIND_RESERVED)) { write_unlock(); return INVALID; }
    void *at=(void *)address;
    int32_t result=place(&at,size,prot,MAP_FIXED_FLAG,PAGE,KIND_DIRECT,type,phys);
    if (!result) { mark_pooled(address,address+size); ++maps; }
    write_unlock();
    flush_hooks();
    return result;
}
int32_t runtime_memory_pool_decommit(uintptr_t address, uint64_t size, void (*release)(uint64_t phys, uint64_t size)) {
    read_lock();
    int ok=!(address%PAGE) && size && all_pooled(address,address+size,0);
    read_unlock();
    if (!ok) return INVALID;
    if (hook_unmap) hook_unmap(address,size);
    write_lock();
    if (!all_pooled(address,address+size,0)) { write_unlock(); return INVALID; }
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind!=KIND_DIRECT) continue;
        const uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        const uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        const uint64_t phys=vmas[i].phys+(a-vmas[i].start);
        zero_phys(phys,b-a);
        release(phys,b-a);
    }
    void *at=(void *)address;
    int32_t result=place(&at,size,0,MAP_FIXED_FLAG,PAGE,KIND_RESERVED,0,0);
    if (!result) mark_pooled(address,address+size);
    write_unlock();
    return result;
}
static const RuntimeExport exports[]={
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelAllocateMainDirectMemory",direct_allocate_main},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelCheckedReleaseDirectMemory",direct_checked_release},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelDirectMemoryQuery",direct_memory_query}, {"sceKernelSetVirtualRangeName",set_virtual_range_name},
    {"sceKernelMlock",lock_range},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
    {"mmap",posix_mmap}, {"sceKernelMmap",kernel_mmap},
    {"sceKernelAvailableFlexibleMemorySize",available_flexible_size},
    {"sceKernelConfiguredFlexibleMemorySize",configured_flexible_size},
    {"sceKernelAvailableDirectMemorySize",available_direct_size},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E#p#J")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA#p#J")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA#p#J")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8#p#J")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk#p#J")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: virtual ranges reserved=%zu\n", reservations);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
}
#endif
