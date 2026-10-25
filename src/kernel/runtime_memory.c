/* PS4 virtual memory: the region table and the storage behind direct (physical) and flexible
 * memory (the guest's calls: runtime_memory_kernel.c). One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. PS_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh or the title's profile sets it). */
uint64_t pool_size_bytes(void) {
    static uint64_t s_cb_pool;
    if (!s_cb_pool) {
        const char *sz_env = runtime_env("PS_DMEM_MB", NULL);
        uint64_t mb = sz_env ? strtoull(sz_env, NULL, 10) : 0;
        if (mb < 5056 || mb > 16384) {
            if (sz_env && *sz_env)
                fprintf(stderr,"Runtime: PS_DMEM_MB=%s is outside 5056-16384 MiB: direct memory stays at the retail 5056 MiB\n",sz_env);
            mb = 5056;
        }
        s_cb_pool = mb * 1024 * 1024;
    }
    return s_cb_pool;
}

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
static pthread_rwlock_t s_lock=PTHREAD_RWLOCK_INITIALIZER;
static __thread unsigned s_exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
uint64_t g_table_generation;
void write_lock(void) {
    runtime_exception_hold();
    if (!s_exclusive_depth++) { pthread_rwlock_wrlock(&s_lock); __atomic_add_fetch(&g_table_generation,1,__ATOMIC_ACQ_REL); }
}
void write_unlock(void) {
    if (!--s_exclusive_depth) { __atomic_add_fetch(&g_table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&s_lock); }
    runtime_exception_release();
}
void read_lock(void) { runtime_exception_hold(); if (!s_exclusive_depth) pthread_rwlock_rdlock(&s_lock); }
void read_unlock(void) { if (!s_exclusive_depth) pthread_rwlock_unlock(&s_lock); runtime_exception_release(); }
/* For signal handlers, which must not wait: 0 when a writer holds the lock, or this thread does
 * (it was interrupted while changing the table). */
int read_trylock(void) { return !s_exclusive_depth && !pthread_rwlock_tryrdlock(&s_lock); }
void read_tryunlock(void) { pthread_rwlock_unlock(&s_lock); }
Vma *g_p_vmas; size_t g_c_vmas; static size_t s_c_vma_capacity;
static int s_h_pool=-1;
/* psport: direct memory in GPU-visible system memory the GPU library allocates (dma-buf chunks,
 * or imported host memory): the GPU reads the game's data in place instead of the CPU copying it
 * into staging buffers first. Chunks are allocated when the game allocates direct memory in them;
 * a chunk the GPU library cannot provide stays in the memfd. */
#define CHUNK (UINT64_C(256) << 20)
/* 0 with *pp_out_share for GPU memory the chunk's views map, 0 with NULL when the GPU imported the chunk's
 * memfd in place, or an errno when the chunk stays in the memfd. */
typedef int (*GuestChunkAlloc)(uint64_t phys, uint64_t cb_range, PsHostGpuShare **pp_out_share);
static GuestChunkAlloc s_pfn_chunk_alloc;
/* Where a chunk lives: not decided, the memfd, the memfd the GPU imported (host memory:
 * VK_EXT_external_memory_host), or GPU memory exported to the host (a dma-buf) in p_share. */
typedef enum { CHUNK_UNDECIDED, CHUNK_MEMFD, CHUNK_IMPORTED, CHUNK_SHARED } ChunkPlace;
typedef struct { ChunkPlace place; PsHostGpuShare *p_share; } ChunkSource;
static ChunkSource *s_p_chunks; /* per CHUNK of direct memory */
/* Released memory in a chunk the GPU uses is cleared, not punched out: the GPU holds those pages. */
#define GPU_CHUNK(source) ((source).place==CHUNK_IMPORTED || (source).place==CHUNK_SHARED)
/* Drivers whose dma-buf maps at offset 0 only (NVIDIA): a chunk per direct allocation instead of
 * the CHUNK grid. The game maps each of its allocations once, whole, so its mappings and the
 * backing view start at offset 0 of their chunk. */
unsigned char *g_p_backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
static int s_has_whole_chunks;
typedef struct { uint64_t phys, cb_range; ChunkSource source; } Region;
#define MAX_REGIONS 256
static Region s_regions[MAX_REGIONS];
static size_t s_c_regions;
/* The region holding phys (sorted by phys), or NULL; *next: start of the next region above. */
static const Region *region_at(uint64_t phys, uint64_t *p_next) {
    *p_next=POOL_SIZE;
    for (size_t i=0;i<s_c_regions;++i) {
        if (phys<s_regions[i].phys) { *p_next=s_regions[i].phys; return NULL; }
        if (phys<s_regions[i].phys+s_regions[i].cb_range) return &s_regions[i];
    }
    return NULL;
}
/* Asks the GPU library for the chunk [phys, phys+size) (lock held): exported GPU memory replaces the
 * backing view there, and the chunk stays in the memfd when either fails. */
static ChunkSource alloc_chunk(uint64_t phys, uint64_t cb_range) {
    PsHostGpuShare *p_share=NULL;
    if (!s_pfn_chunk_alloc || s_pfn_chunk_alloc(phys,cb_range,&p_share)) return (ChunkSource){CHUNK_MEMFD,NULL};
    if (!p_share) return (ChunkSource){CHUNK_IMPORTED,NULL};
    if (!ps_host_gpu_memory_map(p_share,g_p_backing_base+phys,cb_range,PS_HOST_VM_READ|PS_HOST_VM_WRITE,PS_HOST_VM_REPLACE,0)) {
        ps_host_gpu_memory_release(p_share);
        return (ChunkSource){CHUNK_MEMFD,NULL};
    }
    return (ChunkSource){CHUNK_SHARED,p_share};
}
static void add_region(uint64_t phys, uint64_t cb_range) {
    if (s_c_regions==MAX_REGIONS) return;
    const ChunkSource source=alloc_chunk(phys,cb_range);
    size_t i=s_c_regions;
    while (i>0 && s_regions[i-1].phys>phys) { s_regions[i]=s_regions[i-1]; --i; }
    s_regions[i]=(Region){phys,cb_range,source};
    ++s_c_regions;
}
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t s_flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (libpsport-gpu): notified outside the lock, in order, after each operation. */
GpuRange g_pfn_hook_map, g_pfn_hook_unmap, g_pfn_hook_invalidate;
typedef struct { int kind; uintptr_t va_range; uint64_t cb_range; } PendingHook;
static PendingHook s_pending[64]; size_t g_c_pending_hooks;
uint64_t g_cb_flexible;

/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
int pool(void) {
    if (s_h_pool>=0) return 0;
    if (ps_host_vm_section_create("psport-guest-memory",POOL_SIZE+FLEX_SPAN,&s_h_pool)) return -1;
    void *p_view=ps_host_vm_map_shared(NULL,POOL_SIZE+FLEX_SPAN,PS_HOST_VM_READ|PS_HOST_VM_WRITE,PS_HOST_VM_SPARSE,s_h_pool,0);
    if (!p_view) return -1;
    g_p_backing_base=p_view;
    s_p_chunks=calloc(POOL_SIZE/CHUNK+1,sizeof(*s_p_chunks));
    if (!s_p_chunks) return -1;
    return 0;
}
/* Size of direct memory chunk c (the last one may be shorter). */
static uint64_t chunk_size(uint64_t c) {
    const uint64_t start=c*CHUNK;
    return POOL_SIZE-start<CHUNK ? POOL_SIZE-start : CHUNK;
}
/* Decides where the chunks of [phys, phys+size) of direct memory live (lock held). */
void ensure_chunks(uint64_t phys, uint64_t cb_range) {
    if (!s_p_chunks || phys>=POOL_SIZE) return;
    if (s_has_whole_chunks) {
        if (!s_pfn_chunk_alloc) return;
        const uint64_t end=phys+cb_range<POOL_SIZE ? phys+cb_range : POOL_SIZE;
        for (uint64_t at=phys;at<end;) {
            uint64_t next;
            const Region *p_region=region_at(at,&next);
            if (p_region) { at=p_region->phys+p_region->cb_range; continue; }
            const uint64_t gap_end=next<end ? next : end;
            add_region(at,gap_end-at);
            at=gap_end;
        }
        return;
    }
    const uint64_t last=(phys+cb_range-1<POOL_SIZE ? phys+cb_range-1 : POOL_SIZE-1)/CHUNK;
    for (uint64_t c=phys/CHUNK;c<=last;++c) {
        if (s_p_chunks[c].place==CHUNK_UNDECIDED) s_p_chunks[c]=alloc_chunk(c*CHUNK,chunk_size(c));
    }
}
/* A view of direct or flexible memory at phys, split where it crosses chunks. */
static void *map_phys(uintptr_t va_range, uint64_t cb_range, unsigned access, uint64_t phys) {
    if (!s_p_chunks || phys>=POOL_SIZE)
        return ps_host_vm_map_shared((void *)va_range,cb_range,access,PS_HOST_VM_REPLACE,s_h_pool,phys);
    ensure_chunks(phys,cb_range);
    for (uint64_t done=0;s_has_whole_chunks && done<cb_range;) {
        const uint64_t at=phys+done;
        uint64_t next;
        const Region *p_region=region_at(at,&next);
        const uint64_t room=p_region ? p_region->phys+p_region->cb_range-at : next-at, cb_chunk=cb_range-done<room ? cb_range-done : room;
        const int is_shared=p_region && p_region->source.place==CHUNK_SHARED;
        void *p_mapping = is_shared
            ? ps_host_gpu_memory_map(p_region->source.p_share,(void *)(va_range+done),cb_chunk,access,PS_HOST_VM_REPLACE,at-p_region->phys)
            : ps_host_vm_map_shared((void *)(va_range+done),cb_chunk,access,PS_HOST_VM_REPLACE,s_h_pool,at);
        if (!p_mapping) {
            static int s_has_reported;
            if (s_has_reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,cb_chunk,is_shared ? "a GPU-visible dma-buf (whole-allocation chunk)" : "the memfd",strerror(errno));
            return NULL;
        }
        done+=cb_chunk;
        if (done==cb_range) return (void *)va_range;
    }
    for (uint64_t done=0;done<cb_range;) {
        const uint64_t at=phys+done, c=at/CHUNK;
        const uint64_t room=c*CHUNK+chunk_size(c)-at, cb_chunk=cb_range-done<room ? cb_range-done : room;
        const int is_shared=s_p_chunks[c].place==CHUNK_SHARED;
        void *p_mapping = is_shared
            ? ps_host_gpu_memory_map(s_p_chunks[c].p_share,(void *)(va_range+done),cb_chunk,access,PS_HOST_VM_REPLACE,at-c*CHUNK)
            : ps_host_vm_map_shared((void *)(va_range+done),cb_chunk,access,PS_HOST_VM_REPLACE,s_h_pool,at);
        if (!p_mapping) {
            static int s_has_reported;
            if (s_has_reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,cb_chunk,is_shared ? "a GPU-visible dma-buf chunk" : "the memfd",strerror(errno));
            return NULL;
        }
        done+=cb_chunk;
    }
    return (void *)va_range;
}
/* Returns released physical memory to zero: hole punching in the memfd, a clear in a chunk. */
void zero_phys(uint64_t phys, uint64_t cb_range) {
    for (uint64_t done=0;done<cb_range;) {
        const uint64_t at=phys+done;
        if (!s_p_chunks || at>=POOL_SIZE) {
            ps_host_vm_section_discard(s_h_pool,at,cb_range-done);
            return;
        }
        if (s_has_whole_chunks) {
            uint64_t next;
            const Region *p_region=region_at(at,&next);
            const uint64_t room=p_region ? p_region->phys+p_region->cb_range-at : next-at, cb_chunk=cb_range-done<room ? cb_range-done : room;
            if (p_region && GPU_CHUNK(p_region->source)) memset(g_p_backing_base+at,0,cb_chunk);
            else ps_host_vm_section_discard(s_h_pool,at,cb_chunk);
            done+=cb_chunk;
            continue;
        }
        const uint64_t c=at/CHUNK, room=c*CHUNK+chunk_size(c)-at, cb_chunk=cb_range-done<room ? cb_range-done : room;
        if (GPU_CHUNK(s_p_chunks[c])) memset(g_p_backing_base+at,0,cb_chunk);
        else ps_host_vm_section_discard(s_h_pool,at,cb_chunk);
        done+=cb_chunk;
    }
}
/* psport: the GPU library provides direct memory chunks (see CHUNK). */
void runtime_memory_set_guest_chunk_whole(int whole) { s_has_whole_chunks=whole; }
/* psport: the host address of direct memory phys in the backing view (host memory import). */
void *runtime_memory_backing_pointer(uint64_t phys) { return g_p_backing_base ? g_p_backing_base+phys : NULL; }
/* Only when a share failed, so that the summary of a run without failures stays as it was. */
static void share_summary(char *sz_out, size_t cb_out) {
    PsHostGpuMemoryStats stats;
    ps_host_gpu_memory_stats(&stats);
    const PsHostGpuShareCounts *p_export=&stats.export_fd, *p_import=&stats.import_range;
    if (!p_export->c_failures && !p_import->c_failures) { if (cb_out) *sz_out=0; return; }
    snprintf(sz_out,cb_out,"Runtime: summary direct memory shared with the GPU: export_fd %" PRIu64 " (%" PRIu64 " MiB) %" PRIu64
             " failed (last: %s), import_range %" PRIu64 " (%" PRIu64 " MiB) %" PRIu64 " failed (last: %s)\n",
             p_export->c_shares,p_export->cb_shared>>20,p_export->c_failures,p_export->c_failures ? strerror(p_export->last_error) : "none",
             p_import->c_shares,p_import->cb_shared>>20,p_import->c_failures,p_import->c_failures ? strerror(p_import->last_error) : "none");
}
void runtime_memory_set_guest_chunk_allocator(GuestChunkAlloc pfn_alloc) {
    static int s_has_summary;
    write_lock();
    s_pfn_chunk_alloc=pfn_alloc;
    write_unlock();
    if (pfn_alloc && !__atomic_exchange_n(&s_has_summary,1,__ATOMIC_ACQ_REL)) runtime_calls_add_summary(share_summary);
}
void queue_hook(int kind, uintptr_t va_range, uint64_t cb_range) {
    GpuRange pfn_hook = kind==HOOK_MAP ? g_pfn_hook_map : kind==HOOK_UNMAP ? g_pfn_hook_unmap : g_pfn_hook_invalidate;
    if (!pfn_hook) return;
    if (g_c_pending_hooks==sizeof(s_pending)/sizeof(*s_pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); runtime_stop(21); }
    s_pending[g_c_pending_hooks++]=(PendingHook){kind,va_range,cb_range};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!g_c_pending_hooks) { write_unlock(); return; }
        PendingHook h=s_pending[0];
        memmove(s_pending,s_pending+1,--g_c_pending_hooks*sizeof(*s_pending));
        GpuRange pfn_hook = h.kind==HOOK_MAP ? g_pfn_hook_map : h.kind==HOOK_UNMAP ? g_pfn_hook_unmap : g_pfn_hook_invalidate;
        write_unlock();
        if (pfn_hook) pfn_hook(h.va_range,h.cb_range);
    }
}
static int flex_test(uint64_t page) { return (int)((s_flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t c_pages, int used) {
    for (uint64_t p=first;p<first+c_pages;++p) {
        if (used) s_flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else s_flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
uint64_t flex_alloc(uint64_t cb_range) {
    uint64_t pages=cb_range/PAGE, c_span_pages=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<c_span_pages;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
void flex_free(uint64_t phys, uint64_t cb_range) {
    flex_set((phys-POOL_SIZE)/PAGE,cb_range/PAGE,0);
    ps_host_vm_section_discard(s_h_pool,phys,cb_range);
}
size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=g_c_vmas;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (g_p_vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (g_c_vmas==s_c_vma_capacity) {
        size_t c_capacity=s_c_vma_capacity ? s_c_vma_capacity*2 : 256;
        Vma *p_next=realloc(g_p_vmas,c_capacity*sizeof(*g_p_vmas));
        if (!p_next) return -1;
        g_p_vmas=p_next; s_c_vma_capacity=c_capacity;
    }
    memmove(g_p_vmas+at+1,g_p_vmas+at,(g_c_vmas-at)*sizeof(*g_p_vmas));
    g_p_vmas[at]=v; ++g_c_vmas; return 0;
}
static void vma_erase(size_t at, size_t c_vmas) {
    memmove(g_p_vmas+at,g_p_vmas+at+c_vmas,(g_c_vmas-at-c_vmas)*sizeof(*g_p_vmas)); g_c_vmas-=c_vmas;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==g_c_vmas || g_p_vmas[i].start>=a) return 0;
    Vma right=g_p_vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT || right.kind==KIND_FILE) right.phys+=a-g_p_vmas[i].start;
    if (right.p_view) right.p_view+=a-g_p_vmas[i].start;
    g_p_vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
size_t carve(uintptr_t start, uintptr_t end, int *p_error) {
    *p_error = split_at(start) || split_at(end);
    return vma_index(start);
}
int covered(uintptr_t start, uintptr_t end, int should_allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==g_c_vmas || g_p_vmas[i].start>at || (!should_allow_reserved && g_p_vmas[i].kind==KIND_RESERVED)) return 0;
        at=g_p_vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<g_c_vmas && g_p_vmas[i].start<end; ++i)
        if (!ignore_reserved || g_p_vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t cb_range, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+cb_range<=USER_MAX; ++i) {
        if (i==g_c_vmas || g_p_vmas[i].start>=at+cb_range) return at;
        at=align_up(g_p_vmas[i].end,alignment);
    }
    return 0;
}
void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), c_dropped=0;
    while (i+c_dropped<g_c_vmas && g_p_vmas[i+c_dropped].end<=end && g_p_vmas[i+c_dropped].start>=start) {
        if (g_p_vmas[i+c_dropped].kind==KIND_FLEXIBLE) {
            g_cb_flexible-=g_p_vmas[i+c_dropped].end-g_p_vmas[i+c_dropped].start;
            flex_free(g_p_vmas[i+c_dropped].phys,g_p_vmas[i+c_dropped].end-g_p_vmas[i+c_dropped].start);
        }
        if (g_p_vmas[i+c_dropped].p_view) ps_host_vm_unmap(g_p_vmas[i+c_dropped].p_view,g_p_vmas[i+c_dropped].end-g_p_vmas[i+c_dropped].start);
        ++c_dropped;
    }
    vma_erase(i,c_dropped);
}
static int32_t place_backed(void **pp_inout, uint64_t cb_range, int prot, int flags, uint64_t alignment,
                            int kind, int type, uint64_t phys, int file) {
    uintptr_t va_range=(uintptr_t)*pp_inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!va_range || va_range%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(va_range,va_range+cb_range,1)) return NO_MEMORY;
    } else {
        va_range=find_free(va_range,cb_range,alignment);
        if (!va_range) return NO_MEMORY;
    }
    void *p_mapped = kind==KIND_RESERVED
        ? ps_host_vm_map((void *)va_range,cb_range,PS_HOST_VM_NONE,PS_HOST_VM_REPLACE|PS_HOST_VM_SPARSE)
        : kind==KIND_FILE ? ps_host_vm_map_shared((void *)va_range,cb_range,host_prot(prot),PS_HOST_VM_REPLACE,file,phys)
        : map_phys(va_range,cb_range,host_prot(prot),phys);
    if (!p_mapped) return kind==KIND_FILE && errno==EACCES ? ACCESS : NO_MEMORY;
    unsigned char *p_view=NULL;
    if (kind==KIND_FILE) {
        p_view=ps_host_vm_map_shared(NULL,cb_range,PS_HOST_VM_READ|PS_HOST_VM_WRITE,0,file,phys);
        if (!p_view) {
            printf("Runtime: file mapping %p+%#" PRIx64 " has no host view (%s): the GPU's writes to it are dropped\n",
                   p_mapped,cb_range,strerror(errno));
        }
    }
    drop_range(va_range,va_range+cb_range);
    trap_forget(va_range,va_range+cb_range); /* a new mapping starts without traps */
    if (vma_insert(vma_index(va_range),(Vma){va_range,va_range+cb_range,kind,prot,type,phys,0,p_view})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) g_cb_flexible+=cb_range;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,va_range,cb_range);
    *pp_inout=p_mapped;
    return 0;
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
int32_t place(void **pp_inout, uint64_t cb_range, int prot, int flags, uint64_t alignment,
              int kind, int type, uint64_t phys) {
    return place_backed(pp_inout,cb_range,prot,flags,alignment,kind,type,phys,-1);
}
/* A shared mapping of a host file at offset: every mapping of the same file aliases it, and
 * writes reach the file. */
int32_t place_file(void **pp_inout, uint64_t cb_range, int prot, int flags, int file, uint64_t off_file) {
    return place_backed(pp_inout,cb_range,prot,flags,PAGE,KIND_FILE,0,off_file,file);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t s_low_next=LOW_MIN;
int runtime_memory_reserve_arena(void) {
    return ps_host_vm_reserve_arena((void *)(uintptr_t)LOW_MIN,(size_t)(USER_MAX-LOW_MIN));
}
void *runtime_low_map(size_t cb_range, unsigned access) {
    cb_range=align_up(cb_range,PAGE);
    write_lock();
    void *p_mapping=NULL;
    while (s_low_next+cb_range<=USER_MIN) {
        p_mapping=ps_host_vm_map((void *)s_low_next,cb_range,access,PS_HOST_VM_EXACT);
        s_low_next+=cb_range+PAGE; /* unmapped gap catches overruns */
        if (p_mapping) break;
    }
    write_unlock();
    return p_mapping;
}
