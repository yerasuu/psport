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
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("BB_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < 5056 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
uint64_t table_generation;
void write_lock(void) {
    runtime_exception_hold();
    if (!exclusive_depth++) { pthread_rwlock_wrlock(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL); }
}
void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&lock); }
    runtime_exception_release();
}
void read_lock(void) { runtime_exception_hold(); if (!exclusive_depth) pthread_rwlock_rdlock(&lock); }
void read_unlock(void) { if (!exclusive_depth) pthread_rwlock_unlock(&lock); runtime_exception_release(); }
Vma *vmas; size_t vma_count; static size_t vma_capacity;
static int pool_fd=-1;
/* bbport: direct memory in GPU-visible system memory the GPU library allocates (dma-buf chunks,
 * BB_GUEST_GPU_MEMORY=1): the GPU reads the game's data in place instead of the CPU copying it
 * into staging buffers first. Chunks are allocated when the game allocates direct memory in them;
 * a chunk the GPU library cannot provide stays in the memfd. */
#define CHUNK (UINT64_C(256) << 20)
typedef int (*GuestChunkAlloc)(uint64_t phys, uint64_t size);
static GuestChunkAlloc chunk_alloc;
static int *chunk_fds; /* per CHUNK of direct memory: -1 not decided, -2 memfd, -3 memfd the GPU
                        * imported (host memory: VK_EXT_external_memory_host), else a dma-buf fd */
#define CHUNK_IMPORTED (-3)
/* Released memory in a chunk the GPU uses is cleared, not punched out: the GPU holds those pages. */
#define GPU_CHUNK(fd) ((fd)>=0 || (fd)==CHUNK_IMPORTED)
/* Drivers whose dma-buf maps at offset 0 only (NVIDIA): a chunk per direct allocation instead of
 * the CHUNK grid. The game maps each of its allocations once, whole, so its mappings and the
 * backing view start at offset 0 of their chunk. */
unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
static int whole_chunks;
typedef struct { uint64_t phys, size; int fd; } Region;
#define MAX_REGIONS 256
static Region regions[MAX_REGIONS];
static size_t region_count;
/* The region holding phys (sorted by phys), or NULL; *next: start of the next region above. */
static const Region *region_at(uint64_t phys, uint64_t *next) {
    *next=POOL_SIZE;
    for (size_t i=0;i<region_count;++i) {
        if (phys<regions[i].phys) { *next=regions[i].phys; return NULL; }
        if (phys<regions[i].phys+regions[i].size) return &regions[i];
    }
    return NULL;
}
static void add_region(uint64_t phys, uint64_t size) {
    if (region_count==MAX_REGIONS) return;
    int fd=chunk_alloc(phys,size);
    if (fd>=0 && mmap(backing_base+phys,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==MAP_FAILED) {
        close(fd);
        fd=-1;
    }
    size_t i=region_count;
    while (i>0 && regions[i-1].phys>phys) { regions[i]=regions[i-1]; --i; }
    regions[i]=(Region){phys,size,fd>=0 || fd==CHUNK_IMPORTED ? fd : -2};
    ++region_count;
}
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
GpuRange hook_map, hook_unmap, hook_invalidate;
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; size_t pending_count;
uint64_t flexible_bytes;

/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
int pool(void) {
    if (pool_fd>=0) return 0;
    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    chunk_fds=malloc((POOL_SIZE/CHUNK+1)*sizeof(int));
    if (!chunk_fds) return -1;
    for (uint64_t c=0;c<=POOL_SIZE/CHUNK;++c) chunk_fds[c]=-1;
    return 0;
}
/* Size of direct memory chunk c (the last one may be shorter). */
static uint64_t chunk_size(uint64_t c) {
    const uint64_t start=c*CHUNK;
    return POOL_SIZE-start<CHUNK ? POOL_SIZE-start : CHUNK;
}
/* Decides where the chunks of [phys, phys+size) of direct memory live (lock held). */
void ensure_chunks(uint64_t phys, uint64_t size) {
    if (!chunk_fds || phys>=POOL_SIZE) return;
    if (whole_chunks) {
        if (!chunk_alloc) return;
        const uint64_t end=phys+size<POOL_SIZE ? phys+size : POOL_SIZE;
        for (uint64_t at=phys;at<end;) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            if (r) { at=r->phys+r->size; continue; }
            const uint64_t gap_end=next<end ? next : end;
            add_region(at,gap_end-at);
            at=gap_end;
        }
        return;
    }
    const uint64_t last=(phys+size-1<POOL_SIZE ? phys+size-1 : POOL_SIZE-1)/CHUNK;
    for (uint64_t c=phys/CHUNK;c<=last;++c) {
        if (chunk_fds[c]!=-1) continue;
        int fd=chunk_alloc ? chunk_alloc(c*CHUNK,chunk_size(c)) : -1;
        if (fd>=0 && mmap(backing_base+c*CHUNK,chunk_size(c),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==MAP_FAILED) {
            close(fd);
            fd=-1;
        }
        chunk_fds[c]=fd>=0 || fd==CHUNK_IMPORTED ? fd : -2;
    }
}
/* mmap of direct or flexible memory at phys, split where it crosses chunks. */
static void *map_phys(uintptr_t address, uint64_t size, int prot, uint64_t phys) {
    if (!chunk_fds || phys>=POOL_SIZE)
        return mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys);
    ensure_chunks(phys,size);
    for (uint64_t done=0;whole_chunks && done<size;) {
        const uint64_t at=phys+done;
        uint64_t next;
        const Region *r=region_at(at,&next);
        const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
        void *p = r && r->fd>=0
            ? mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,r->fd,(off_t)(at-r->phys))
            : mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)at);
        if (p==MAP_FAILED) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,r && r->fd>=0 ? "a GPU-visible dma-buf (whole-allocation chunk)" : "the memfd",strerror(errno));
            return MAP_FAILED;
        }
        done+=n;
        if (done==size) return (void *)address;
    }
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done, c=at/CHUNK;
        const uint64_t room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        void *p = chunk_fds[c]>=0
            ? mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,chunk_fds[c],(off_t)(at-c*CHUNK))
            : mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)at);
        if (p==MAP_FAILED) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,chunk_fds[c]>=0 ? "a GPU-visible dma-buf chunk" : "the memfd",strerror(errno));
            return MAP_FAILED;
        }
        done+=n;
    }
    return (void *)address;
}
/* Returns released physical memory to zero: hole punching in the memfd, a clear in a chunk. */
void zero_phys(uint64_t phys, uint64_t size) {
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done;
        if (!chunk_fds || at>=POOL_SIZE) {
            fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)(size-done));
            return;
        }
        if (whole_chunks) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
            if (r && GPU_CHUNK(r->fd)) memset(backing_base+at,0,n);
            else fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)n);
            done+=n;
            continue;
        }
        const uint64_t c=at/CHUNK, room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        if (GPU_CHUNK(chunk_fds[c])) memset(backing_base+at,0,n);
        else fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)n);
        done+=n;
    }
}
/* bbport: the GPU library provides direct memory chunks (see CHUNK). */
void runtime_memory_set_guest_chunk_whole(int whole) { whole_chunks=whole; }
/* bbport: the host address of direct memory phys in the backing view (host memory import). */
void *runtime_memory_backing_pointer(uint64_t phys) { return backing_base ? backing_base+phys : NULL; }
void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size)) {
    write_lock();
    chunk_alloc=alloc;
    write_unlock();
}
void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); runtime_stop(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT || right.kind==KIND_FILE) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
static int32_t place_backed(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                            int kind, int type, uint64_t phys, int file) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    void *mapped = kind==KIND_RESERVED
        ? mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0)
        : kind==KIND_FILE ? mmap((void *)address,size,host_prot(prot),MAP_SHARED|MAP_FIXED,file,(off_t)phys)
        : map_phys(address,size,host_prot(prot),phys);
    if (mapped==MAP_FAILED) return kind==KIND_FILE && errno==EACCES ? ACCESS : NO_MEMORY;
    drop_range(address,address+size);
    trap_forget(address,address+size); /* a new mapping starts without traps */
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys,0})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    *inout=mapped;
    return 0;
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
              int kind, int type, uint64_t phys) {
    return place_backed(inout,size,prot,flags,alignment,kind,type,phys,-1);
}
/* A shared mapping of a host file at offset: every mapping of the same file aliases it, and
 * writes reach the file. */
int32_t place_file(void **inout, uint64_t size, int prot, int flags, int file, uint64_t offset) {
    return place_backed(inout,size,prot,flags,PAGE,KIND_FILE,0,offset,file);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
}
#else
void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }
uintptr_t runtime_memory_resolve(const char *name) { (void)name; return 0; }
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) { (void)address; (void)size; return 0; }
void runtime_memory_report(void) { puts("Runtime: Windows direct-memory backend not implemented"); }
#endif
