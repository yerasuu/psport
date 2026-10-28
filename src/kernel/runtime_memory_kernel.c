// SPDX-License-Identifier: GPL-2.0-or-later
/* libkernel memory calls: direct memory allocation and mapping, flexible memory, mmap,
 * virtual ranges, protection, queries, batch maps and the memory pools' side (runtime_mempool.c). */
#define _GNU_SOURCE
#include "runtime_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <errno.h>
#define LIMIT 4096
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct { uint64_t start, cb_block; int type, used; } Block;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
typedef struct { uint64_t start, end; int32_t memory_type; } DirectQueryInfo;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");
_Static_assert(sizeof(DirectQueryInfo)==24,"PS4 direct memory query info layout");
static Block s_blocks[LIMIT];
static size_t s_c_allocations, s_c_maps, s_c_reservations, s_c_flexible_maps, s_c_protects, s_c_queries;
static uint64_t s_cb_live, s_cb_flexible_mapped;
/* Some titles map flexible memory dozens of times a second: past these, maps are counted. */
enum { LISTED_FLEXIBLE_MAPS=8 };

static int trace_memory(void) { static int s_is_enabled=-1; if (s_is_enabled<0) s_is_enabled=runtime_env("PS_TRACE_MEM",NULL)!=NULL; return s_is_enabled; }
static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (s_blocks[i].used && phys>=s_blocks[i].start && phys-s_blocks[i].start<s_blocks[i].cb_block) return s_blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t cb_range) {
    for (uint64_t at=phys; at<phys+cb_range;) {
        int is_found=0;
        for (int i=0;i<LIMIT;++i) if (s_blocks[i].used && at>=s_blocks[i].start && at-s_blocks[i].start<s_blocks[i].cb_block) {
            at=s_blocks[i].start+s_blocks[i].cb_block; is_found=1; break;
        }
        if (!is_found) return 0;
    }
    return 1;
}

static ABI uint64_t direct_size(void) { return POOL_SIZE; }
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t cb_range,
                                  uint64_t alignment, int type, int64_t *p_out) {
    if (!alignment) alignment = PAGE;
    if (!p_out || low < 0 || high < 0 || !cb_range || cb_range % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || cb_range > end - (uint64_t)low) return NO_SPACE;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!s_blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || cb_range > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = s_blocks[i];
            if (b.used && start < b.start+b.cb_block && b.start < start+cb_range) {
                start = align_up(b.start+b.cb_block, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    s_blocks[slot] = (Block){start, cb_range, type, 1}; *p_out = (int64_t)start;
    ensure_chunks(start,cb_range);
    ++s_c_allocations; s_cb_live += cb_range;
    write_unlock();
    if (trace_memory()) printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, cb_range, type);
    return 0;
}
/* The main pool is the whole direct memory the port offers: newer SDKs allocate from it without a
 * search range. */
static ABI int32_t direct_allocate_main(uint64_t cb_range, uint64_t alignment, int type, int64_t *p_out) {
    return direct_allocate(0, (int64_t)POOL_SIZE, cb_range, alignment, type, p_out);
}
static int32_t map_direct_locked(void **pp_out, uint64_t cb_range, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!pp_out || physical < 0 || (uint64_t)physical % PAGE || !cb_range || cb_range % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,cb_range)) return INVALID;
    int32_t result=place(pp_out,cb_range,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++s_c_maps;
    return result;
}
static ABI int32_t direct_map(void **pp_out, uint64_t cb_range, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(pp_out,cb_range,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (result) printf("Runtime: mapping direct memory %#" PRIx64 ", size=%" PRIu64 " failed (error %#x)\n", (uint64_t)physical, cb_range, (unsigned)result);
    else if (trace_memory()) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *pp_out, cb_range, prot);
    return result;
}
static ABI int32_t direct_map_named(void **pp_out, uint64_t cb_range, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *sz_name) {
    (void)sz_name; return direct_map(pp_out,cb_range,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t cb_range) {
    if (start%PAGE || !cb_range) return INVALID;
    uint64_t end=start+align_up(cb_range,PAGE);
    for (size_t i=vma_index(start); i<g_c_vmas && g_p_vmas[i].start<end; ++i)
        if (g_p_vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=g_p_vmas[i].start>start ? g_p_vmas[i].start : start, b=g_p_vmas[i].end<end ? g_p_vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<g_c_vmas && g_p_vmas[i].start<end; ++i) {
        uintptr_t a=g_p_vmas[i].start>start ? g_p_vmas[i].start : start, b=g_p_vmas[i].end<end ? g_p_vmas[i].end : end;
        if (ps_host_vm_unmap((void *)a,b-a)) return INVALID;
    }
    drop_range(start,end);
    trap_forget(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *p_address, uint64_t cb_range) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (g_pfn_hook_unmap && !((uintptr_t)p_address%PAGE) && cb_range) g_pfn_hook_unmap((uintptr_t)p_address,align_up(cb_range,PAGE));
    write_lock();
    size_t before=g_c_pending_hooks;
    int32_t result=unmap_locked((uintptr_t)p_address,cb_range);
    g_c_pending_hooks=before; /* already notified above */
    write_unlock();
    return result;
}
static int32_t release_direct(uint64_t start, uint64_t cb_range, int32_t unallocated) {
    if (start % PAGE || cb_range % PAGE) return INVALID;
    if (!cb_range) return 0;
    write_lock();
    if (!direct_range_allocated(start,cb_range)) { write_unlock(); return unallocated; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<g_c_vmas;) {
        Vma v=g_p_vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+cb_range && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+cb_range ? v.phys+length : start+cb_range;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *p_block=&s_blocks[i];
        if (!p_block->used || p_block->start>=start+cb_range || start>=p_block->start+p_block->cb_block) continue;
        uint64_t a=p_block->start>start ? p_block->start : start, e=p_block->start+p_block->cb_block<start+cb_range ? p_block->start+p_block->cb_block : start+cb_range;
        Block left=*p_block, right=*p_block;
        left.cb_block=a-p_block->start; right.start=e; right.cb_block=p_block->start+p_block->cb_block-e;
        p_block->used=0;
        if (left.cb_block) *p_block=left;
        if (right.cb_block) for (int j=0;j<LIMIT;++j) if (!s_blocks[j].used) { s_blocks[j]=right; break; }
        s_cb_live-=e-a;
    }
    zero_phys(start,cb_range); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}
static ABI int32_t direct_release(uint64_t start, uint64_t cb_range) { return release_direct(start,cb_range,INVALID); }
static ABI int32_t direct_checked_release(uint64_t start, uint64_t cb_range) { return release_direct(start,cb_range,NOT_FOUND); }
static int32_t map_flexible_locked(void **pp_inout, uint64_t cb_range, int prot, int flags) {
    if (!pp_inout || !cb_range || cb_range%PAGE || (prot & ~0x37)) return INVALID;
    if (g_cb_flexible+cb_range>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(cb_range);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(pp_inout,cb_range,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,cb_range);
    else { ++s_c_flexible_maps; s_cb_flexible_mapped+=cb_range; }
    return result;
}
static ABI int32_t map_flexible(void **pp_inout, uint64_t cb_range, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(pp_inout,cb_range,prot,flags);
    const size_t c_flexible_maps=s_c_flexible_maps;
    const uint64_t in_use=g_cb_flexible;
    write_unlock();
    flush_hooks();
    if (result) fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                        cb_range, (unsigned)result, in_use);
    else if (c_flexible_maps<=LISTED_FLEXIBLE_MAPS || trace_memory())
        printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *pp_inout, cb_range, prot);
    else if (!(c_flexible_maps&(c_flexible_maps-1)))
        printf("Runtime: flexible memory maps=%zu so far, in use=%" PRIu64 "; only the first %d are listed (PS_TRACE_MEM=1: each)\n",
               c_flexible_maps, in_use, LISTED_FLEXIBLE_MAPS);
    return result;
}
static ABI int32_t map_flexible_named(void **pp_inout, uint64_t cb_range, int prot, int flags, const char *sz_name) {
    (void)sz_name; return map_flexible(pp_inout,cb_range,prot,flags);
}
static ABI int32_t release_flexible(void *p_address, uint64_t cb_range) { return direct_unmap(p_address,cb_range); }
/* mmap (sceKernelMmap, and POSIX mmap over it): the game's libc grows its malloc heap with
 * anonymous private mappings once the heap it started with is full (seen after minutes of area
 * streaming). They come from flexible memory as sceKernelMapFlexibleMemory does, at the hint
 * when it is free, as FreeBSD places them. MAP_VOID reserves the range as
 * sceKernelReserveVirtualRange does. A shared file mapping is the host file's (emulators alias
 * one file at several addresses); a private one is a copy of its contents (IL2CPP maps
 * global-metadata.dat that way). */
enum { MAP_SHARED_FLAG=1, MAP_VOID_FLAG=0x100, MAP_ANON_FLAG=0x1000 };
static _Atomic uint64_t s_c_heap_growths;
uint64_t runtime_heap_growths(void) { return atomic_load(&s_c_heap_growths); }
static ABI int32_t kernel_mprotect(const void *p_address, uint64_t cb_range, int prot);
static ABI int32_t reserve_range(void **pp_inout, uint64_t cb_range, int flags, uint64_t alignment);
static int32_t map_file(void **pp_inout, uint64_t cb_range, int prot, int flags, int h_fd, int64_t off_file) {
    if (off_file<0 || off_file%PAGE) return INVALID;
    if (flags & MAP_SHARED_FLAG) {
        const int host=runtime_file_shared_host(h_fd);
        if (host<0) return ERR(9); /* EBADF */
        write_lock();
        const int32_t result=place_file(pp_inout,align_up(cb_range,PAGE),prot & 0x37,flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE),
                                        host,(uint64_t)off_file);
        write_unlock();
        flush_hooks();
        return result;
    }
    const uint64_t length=align_up(cb_range,PAGE);
    const int32_t result=map_flexible(pp_inout,length,(prot & 0x37)|3,flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE));
    if (result) return result;
    const int64_t read=runtime_file_pread(h_fd,*pp_inout,cb_range,off_file);
    if (read<0) {
        direct_unmap(*pp_inout,length);
        return ERR(runtime_guest_errno((int)-read));
    }
    if (!(prot & 2)) kernel_mprotect(*pp_inout,length,prot & 0x37);
    printf("Runtime: mmap of file descriptor %d: %#" PRIx64 " bytes from offset %#" PRIx64 " copied to %p\n",
           h_fd,(uint64_t)read,(uint64_t)off_file,*pp_inout);
    return 0;
}
static ABI int32_t kernel_mmap(void *p_addr, uint64_t cb_range, int prot, int flags, int h_fd, int64_t off_file, void **pp_out) {
    if (!pp_out || !cb_range) return INVALID;
    void *p_at=p_addr;
    int32_t result;
    if (flags & MAP_VOID_FLAG)
        result=reserve_range(&p_at,align_up(cb_range,PAGE),flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE),PAGE);
    else if (!(flags & MAP_ANON_FLAG))
        result=h_fd<0 ? ERR(9) /* EBADF */ : map_file(&p_at,cb_range,prot,flags,h_fd,off_file);
    else if (h_fd!=-1 || off_file)
        result=INVALID;
    else
        result=map_flexible(&p_at,align_up(cb_range,PAGE),prot & 0x37,flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE));
    if (trace_memory() || result)
        printf("Runtime: mmap %p+%#" PRIx64 " prot=0x%x flags=0x%x fd=%d offset=%#" PRIx64 " -> %p (0x%x)\n",
               p_addr,cb_range,prot,flags,h_fd,(uint64_t)off_file,result ? NULL : p_at,(unsigned)result);
    if (!result) *pp_out=p_at;
    return result;
}
static ABI void *posix_mmap(void *p_addr, uint64_t cb_range, int prot, int flags, int h_fd, int64_t off_file) {
    void *p_out=NULL;
    const int32_t result=kernel_mmap(p_addr,cb_range,prot,flags,h_fd,off_file,&p_out);
    if ((flags & MAP_ANON_FLAG) && !(flags & MAP_VOID_FLAG)) {
        atomic_fetch_add(&s_c_heap_growths,1);
        uint64_t sites[3];
        runtime_guest_call_sites(sites);
        printf("Runtime: mmap %#" PRIx64 " bytes for the game (its heap grows) from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
               cb_range,sites[0],sites[1],sites[2]);
    }
    if (result) { *runtime_errno()=result & 0xffff; return (void *)-1; }
    return p_out;
}
static ABI int32_t reserve_range(void **pp_inout, uint64_t cb_range, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!pp_inout || !cb_range || cb_range%PAGE || !valid_alignment(alignment)) return INVALID;
    /* MAP_FIXED replaces what is mapped there (games decommit by reserving over their mappings),
     * so the GPU forgets the range first, as for an unmap. */
    if ((flags & (MAP_FIXED_FLAG|MAP_NO_OVERWRITE))==MAP_FIXED_FLAG && g_pfn_hook_unmap && *pp_inout && !((uintptr_t)*pp_inout%PAGE))
        g_pfn_hook_unmap((uintptr_t)*pp_inout,cb_range);
    write_lock();
    int32_t result=place(pp_inout,cb_range,0,flags,alignment,KIND_RESERVED,0,0);
    if (!result) ++s_c_reservations;
    write_unlock();
    if (!result && trace_memory()) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *pp_inout, cb_range);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t cb_range, int prot, int type) {
    uintptr_t end=start+align_up(cb_range,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    if (ps_host_vm_protect((void *)start,end-start,host_prot(prot))) return INVALID;
    trap_reapply(start,end); /* pages with write traps stay read-only */
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<g_c_vmas && g_p_vmas[i].start<end; ++i) { g_p_vmas[i].prot=prot; if (type>=0) g_p_vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++s_c_protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *p_address, uint64_t cb_range, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)p_address,cb_range,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *p_address, void **pp_start, void **pp_end, uint32_t *p_prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)p_address);
    if (i==g_c_vmas || g_p_vmas[i].start>(uintptr_t)p_address || g_p_vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && g_p_vmas[first-1].end==g_p_vmas[first].start && g_p_vmas[first-1].kind!=KIND_RESERVED && g_p_vmas[first-1].prot==g_p_vmas[i].prot) --first;
    while (last+1<g_c_vmas && g_p_vmas[last+1].start==g_p_vmas[last].end && g_p_vmas[last+1].kind!=KIND_RESERVED && g_p_vmas[last+1].prot==g_p_vmas[i].prot) ++last;
    if (pp_start) *pp_start=(void *)g_p_vmas[first].start;
    if (pp_end) *pp_end=(void *)g_p_vmas[last].end;
    if (p_prot) *p_prot=(uint32_t)g_p_vmas[i].prot;
    __atomic_add_fetch(&s_c_queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *p_address, int flags, VirtualQueryInfo *p_info, uint64_t cb_info) {
    if (!p_info || cb_info!=sizeof(*p_info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)p_address);
    if (i==g_c_vmas || (g_p_vmas[i].start>(uintptr_t)p_address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=g_p_vmas[i];
    memset(p_info,0,sizeof(*p_info));
    p_info->start=v.start; p_info->end=v.end;
    p_info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    p_info->protection=v.prot; p_info->memory_type=v.type;
    p_info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.pooled ? 8u : 0) |
                (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(p_info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : v.kind==KIND_FILE ? "file" : "flexible");
    __atomic_add_fetch(&s_c_queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *p_type, void **pp_start, void **pp_end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (s_blocks[i].used && phys>=s_blocks[i].start && phys-s_blocks[i].start<s_blocks[i].cb_block) {
        if (p_type) *p_type=s_blocks[i].type;
        if (pp_start) *pp_start=(void *)(uintptr_t)s_blocks[i].start;
        if (pp_end) *pp_end=(void *)(uintptr_t)(s_blocks[i].start+s_blocks[i].cb_block);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
/* The allocation holding offset (flags 1: else the first one above it), reported to the end of
 * the run of allocations that follow it without a gap and with its type, as libkernel merges them. */
static ABI int32_t direct_memory_query(uint64_t off_direct, int flags, DirectQueryInfo *p_info, uint64_t cb_info) {
    if (!p_info || cb_info!=sizeof(*p_info)) return INVALID;
    if (off_direct>=POOL_SIZE) return ACCESS;
    read_lock();
    int i_found=-1;
    for (int i=0;i<LIMIT;++i) {
        if (!s_blocks[i].used) continue;
        if (off_direct>=s_blocks[i].start && off_direct-s_blocks[i].start<s_blocks[i].cb_block) { i_found=i; break; }
        if (flags==1 && s_blocks[i].start>off_direct && (i_found<0 || s_blocks[i].start<s_blocks[i_found].start)) i_found=i;
    }
    if (i_found<0) { read_unlock(); return ACCESS; }
    const int type=s_blocks[i_found].type;
    uint64_t end=s_blocks[i_found].start+s_blocks[i_found].cb_block;
    for (int grew=1; grew;) {
        grew=0;
        for (int i=0;i<LIMIT;++i)
            if (s_blocks[i].used && s_blocks[i].start==end && s_blocks[i].type==type) { end+=s_blocks[i].cb_block; grew=1; }
    }
    p_info->start=s_blocks[i_found].start; p_info->end=end; p_info->memory_type=type;
    read_unlock();
    return 0;
}
/* Locking only keeps pages resident, which the guest cannot observe: the range is checked and
 * left as it is. */
static ABI int32_t lock_range(const void *p_address, uint64_t cb_range) {
    if (!p_address || !cb_range || (uintptr_t)p_address+cb_range<(uintptr_t)p_address) return INVALID;
    return 0;
}
/* Range names only label mappings for the console's debugging tools, so they are checked and
 * dropped; VirtualQuery names a range by its kind. */
static ABI int32_t set_virtual_range_name(const void *p_address, uint64_t cb_range, const char *sz_name) {
    (void)p_address; (void)cb_range;
    if (!sz_name) return (int32_t)UINT32_C(0x8002000e); /* EFAULT */
    if (strnlen(sz_name,32)>=32) return (int32_t)UINT32_C(0x8002003f); /* ENAMETOOLONG */
    return 0;
}
/* Range names are not kept (sceKernelSetVirtualRangeName checks its name only), so there is none to clear. */
static ABI int32_t clear_virtual_range_name(const void *p_address, uint64_t cb_range) {
    (void)p_address; (void)cb_range;
    return 0;
}
static ABI int32_t batch_map2(BatchEntry *p_entries, int c_entries, int *p_processed, int flags) {
    if (!p_entries || c_entries<0) return INVALID;
    int32_t result=0; int is_done=0;
    write_lock();
    for (; is_done<c_entries; ++is_done) {
        BatchEntry *p_batch_entry=&p_entries[is_done];
        switch (p_batch_entry->operation) {
        case 0: result=map_direct_locked(&p_batch_entry->start,p_batch_entry->length,(uint8_t)p_batch_entry->prot,flags,(int64_t)p_batch_entry->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)p_batch_entry->start,p_batch_entry->length); break;
        case 2: result=protect_locked((uintptr_t)p_batch_entry->start,p_batch_entry->length,(uint8_t)p_batch_entry->prot,-1); break;
        case 3: result=map_flexible_locked(&p_batch_entry->start,p_batch_entry->length,(uint8_t)p_batch_entry->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)p_batch_entry->start,p_batch_entry->length,(uint8_t)p_batch_entry->prot,p_batch_entry->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (p_processed) *p_processed=is_done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *p_entries, int c_entries, int *p_processed) {
    return batch_map2(p_entries,c_entries,p_processed,MAP_FIXED_FLAG);
}
static ABI int32_t available_flexible_size(uint64_t *p_out) {
    if (!p_out) return INVALID;
    read_lock();
    *p_out=FLEXIBLE_SIZE-g_cb_flexible;
    read_unlock();
    return 0;
}
static ABI int32_t configured_flexible_size(uint64_t *p_out) {
    if (!p_out) return INVALID;
    *p_out=FLEXIBLE_SIZE;
    return 0;
}
/* The start of the first allocation in [at, end), else end (lock held). */
static uint64_t next_block_start(uint64_t at, uint64_t end) {
    for (int i=0;i<LIMIT;++i)
        if (s_blocks[i].used && s_blocks[i].start>=at && s_blocks[i].start<end) end=s_blocks[i].start;
    return end;
}
/* The largest free range of direct memory in [low, high) once its start is aligned, as libkernel
 * answers it. */
static ABI int32_t available_direct_size(uint64_t low, uint64_t high, uint64_t alignment, uint64_t *p_phys, uint64_t *p_size) {
    if (!alignment) alignment=PAGE;
    if (!p_phys || !p_size || !valid_alignment(alignment)) return INVALID;
    const uint64_t end=high<POOL_SIZE ? high : POOL_SIZE;
    uint64_t best_start=0, cb_best=0;
    read_lock();
    for (uint64_t at=low; at<end;) {
        int inside=0;
        for (int i=0;i<LIMIT;++i) if (s_blocks[i].used && at>=s_blocks[i].start && at-s_blocks[i].start<s_blocks[i].cb_block) {
            at=s_blocks[i].start+s_blocks[i].cb_block; inside=1; break;
        }
        if (inside) continue;
        const uint64_t gap_end=next_block_start(at,end), start=align_up(at,alignment);
        if (start<gap_end && gap_end-start>cb_best) { best_start=start; cb_best=gap_end-start; }
        at=gap_end;
    }
    read_unlock();
    if (!cb_best) return NO_MEMORY;
    *p_phys=best_start; *p_size=cb_best;
    return 0;
}
/* Memory pools (runtime_mempool.c): a pool reservation is a reserved range marked pooled, and
 * committing maps direct memory the pool expanded into there, marked pooled too. */
int32_t runtime_memory_pool_expand(uint64_t low, uint64_t high, uint64_t cb_range, uint64_t alignment, int64_t *p_phys) {
    return direct_allocate((int64_t)low,(int64_t)high,cb_range,alignment,0,p_phys);
}
static void mark_pooled(uintptr_t start, uintptr_t end) {
    for (size_t i=vma_index(start); i<g_c_vmas && g_p_vmas[i].start<end; ++i) g_p_vmas[i].pooled=1;
}
static int all_pooled(uintptr_t start, uintptr_t end, int kind) {
    if (!covered(start,end,1)) return 0;
    for (size_t i=vma_index(start); i<g_c_vmas && g_p_vmas[i].start<end; ++i)
        if (!g_p_vmas[i].pooled || (kind && g_p_vmas[i].kind!=kind)) return 0;
    return 1;
}
int32_t runtime_memory_pool_reserve(void **pp_inout, uint64_t cb_range, int flags, uint64_t alignment) {
    int32_t result=reserve_range(pp_inout,cb_range,flags,alignment);
    if (result) return result;
    write_lock();
    mark_pooled((uintptr_t)*pp_inout,(uintptr_t)*pp_inout+cb_range);
    write_unlock();
    return 0;
}
int32_t runtime_memory_pool_commit(uintptr_t va_range, uint64_t cb_range, int prot, int type, uint64_t phys) {
    write_lock();
    if (pool() || !all_pooled(va_range,va_range+cb_range,KIND_RESERVED)) { write_unlock(); return INVALID; }
    void *p_at=(void *)va_range;
    int32_t result=place(&p_at,cb_range,prot,MAP_FIXED_FLAG,PAGE,KIND_DIRECT,type,phys);
    if (!result) { mark_pooled(va_range,va_range+cb_range); ++s_c_maps; }
    write_unlock();
    flush_hooks();
    return result;
}
int32_t runtime_memory_pool_decommit(uintptr_t va_range, uint64_t cb_range, void (*pfn_release)(uint64_t phys, uint64_t cb_range)) {
    read_lock();
    int is_ok=!(va_range%PAGE) && cb_range && all_pooled(va_range,va_range+cb_range,0);
    read_unlock();
    if (!is_ok) return INVALID;
    if (g_pfn_hook_unmap) g_pfn_hook_unmap(va_range,cb_range);
    write_lock();
    if (!all_pooled(va_range,va_range+cb_range,0)) { write_unlock(); return INVALID; }
    for (size_t i=vma_index(va_range); i<g_c_vmas && g_p_vmas[i].start<va_range+cb_range; ++i) {
        if (g_p_vmas[i].kind!=KIND_DIRECT) continue;
        const uintptr_t a=g_p_vmas[i].start>va_range ? g_p_vmas[i].start : va_range;
        const uintptr_t b=g_p_vmas[i].end<va_range+cb_range ? g_p_vmas[i].end : va_range+cb_range;
        const uint64_t phys=g_p_vmas[i].phys+(a-g_p_vmas[i].start);
        zero_phys(phys,b-a);
        pfn_release(phys,b-a);
    }
    void *p_at=(void *)va_range;
    int32_t result=place(&p_at,cb_range,0,MAP_FIXED_FLAG,PAGE,KIND_RESERVED,0,0);
    if (!result) mark_pooled(va_range,va_range+cb_range);
    write_unlock();
    return result;
}
/* FreeBSD's msync flags: MS_ASYNC 1, MS_INVALIDATE 2, MS_SYNC 0. Shared file mappings reach their
 * file; anonymous memory has nothing to write back. */
static int sync_range(uintptr_t va_start,uint64_t cb_range,int32_t flags) {
    if ((va_start&4095) || (flags&~3) || (flags&1 && flags&2)) return EINVAL;
    if (!runtime_memory_is_mapped(va_start,cb_range)) return ENOMEM;
    return ps_host_vm_flush((void *)va_start,cb_range,(flags&1 ? PS_HOST_VM_FLUSH_ASYNC : 0u)|(flags&2 ? PS_HOST_VM_FLUSH_INVALIDATE : 0u));
}
static ABI int32_t kernel_msync(void *p_start,uint64_t cb_range,int32_t flags) {
    const int error=sync_range((uintptr_t)p_start,cb_range,flags);
    return error ? ERR(runtime_guest_errno(error)) : 0;
}
static ABI int32_t posix_msync(void *p_start,uint64_t cb_range,int32_t flags) {
    const int error=sync_range((uintptr_t)p_start,cb_range,flags);
    if (!error) return 0;
    *runtime_errno()=runtime_guest_errno(error);
    return -1;
}
/* struct { void *address; uint64_t size; }: the segment the kernel sets aside in a module for libc's
 * internal heap. Modules here get none, and libc then keeps that heap in ordinary memory (it checks
 * the address for NULL). */
static ABI int32_t module_segment_info(uint64_t *p_out_segment) {
    if (!p_out_segment) return INVALID;
    p_out_segment[0]=p_out_segment[1]=0;
    return 0;
}
static const RuntimeExport s_exports[]={
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
    {"sceKernelClearVirtualRangeName",clear_virtual_range_name},
    {"sceKernelMlock",lock_range},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
    {"mmap",posix_mmap}, {"sceKernelMmap",kernel_mmap},
    {"sceKernelAvailableFlexibleMemorySize",available_flexible_size},
    {"sceKernelConfiguredFlexibleMemorySize",configured_flexible_size},
    {"sceKernelAvailableDirectMemorySize",available_direct_size},
    {"sceKernelMsync",kernel_msync}, {"msync",posix_msync},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",module_segment_info},
};
uintptr_t runtime_memory_resolve(const char *sz_name) {
    return RUNTIME_LOOKUP(s_exports,sz_name);
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           s_c_allocations, s_c_maps, s_cb_live, POOL_SIZE);
    printf("Runtime: virtual ranges reserved=%zu\n", s_c_reservations);
    printf("Runtime: memory protects=%zu, queries=%zu, regions=%zu\n", s_c_protects, s_c_queries, g_c_vmas);
}
void runtime_memory_flexible_report(void) {
    char sz_line[160];
    const int used=snprintf(sz_line,sizeof(sz_line),"Runtime: flexible memory maps=%zu, mapped=%" PRIu64 ", in use=%" PRIu64 "\n",
                            __atomic_load_n(&s_c_flexible_maps,__ATOMIC_RELAXED),
                            __atomic_load_n(&s_cb_flexible_mapped,__ATOMIC_RELAXED),
                            __atomic_load_n(&g_cb_flexible,__ATOMIC_RELAXED));
    if (used>0 && ps_host_fs_write(1,sz_line,(size_t)used)<0) return;
}
