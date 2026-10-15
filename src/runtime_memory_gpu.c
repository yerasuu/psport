/* GPU library interface (gpu/shim/psport_gpu.cpp): GPU hooks, write watches and traps, backing view
 * reads and writes, and the region lookups of its per-draw queries. */
#define _GNU_SOURCE
#include "runtime_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * PS_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with PS_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (PS_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (PS_COPY_THREADS),
 * 262144 with PS_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/psport_toggles.h). */
uint64_t g_runtime_disabled_optimizations;
/* psport: a second number in PS_TOGGLE_FILE: temporary experiment bits (PsToggle::Experiment). */
uint64_t g_runtime_experiment_bits;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
static void *toggle_watcher(void *p_path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *p_file=fopen(p_path,"r");
        unsigned long long value=0, experiment=0;
        if (p_file) { if (fscanf(p_file,"%llu %llu",&value,&experiment)<1) value=0; fclose(p_file); }
        __atomic_store_n(&g_runtime_experiment_bits,(uint64_t)experiment,__ATOMIC_RELEASE);
        if (value!=last) {
            __atomic_store_n(&g_runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
    }
    return NULL;
}
static GpuRange s_pfn_hook_note_write; /* psport: data written into GPU memory (runtime_memory_note_write) */
void runtime_memory_set_gpu_hooks(GpuRange pfn_map, GpuRange pfn_unmap, GpuRange pfn_invalidate) {
    const char *sz_toggles=getenv("PS_TOGGLE_FILE");
    static pthread_t s_watcher;
    if (sz_toggles && !s_watcher) pthread_create(&s_watcher,NULL,toggle_watcher,(void *)sz_toggles);
    write_lock();
    g_pfn_hook_map=pfn_map; g_pfn_hook_unmap=pfn_unmap; g_pfn_hook_invalidate=pfn_invalidate;
    for (size_t i=0;i<g_c_vmas;++i) if (g_p_vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,g_p_vmas[i].start,g_p_vmas[i].end-g_p_vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* psport: the CPU is about to write [address, address+size) without the guest's code doing it
 * (a file read into it): the GPU side is told first (Rasterizer::InvalidateMemory), for the parts
 * the GPU can access, as a driver is told of a resource update. Its write tracking then has
 * nothing to catch there. */
void runtime_memory_set_note_write_hook(GpuRange pfn_note) { s_pfn_hook_note_write=pfn_note; }
void runtime_memory_note_write(uintptr_t va_range, uint64_t cb_range) {
    if (!g_pfn_hook_invalidate || !cb_range) return;
    uintptr_t starts[8], ends[8];
    int c_ranges=0;
    read_lock();
    for (size_t i=vma_index(va_range); i<g_c_vmas && g_p_vmas[i].start<va_range+cb_range && c_ranges<8; ++i) {
        if (g_p_vmas[i].kind==KIND_RESERVED || !(g_p_vmas[i].prot & 0x30)) continue;
        starts[c_ranges]=g_p_vmas[i].start>va_range ? g_p_vmas[i].start : va_range;
        ends[c_ranges]=g_p_vmas[i].end<va_range+cb_range ? g_p_vmas[i].end : va_range+cb_range;
        ++c_ranges;
    }
    read_unlock();
    GpuRange pfn_hook=s_pfn_hook_note_write ? s_pfn_hook_note_write : g_pfn_hook_invalidate;
    for (int i=0;i<c_ranges;++i) pfn_hook(starts[i],ends[i]-starts[i]);
}
/* psport PS_GUEST_IN_PLACE without page protection: pages whose CPU writes the GPU side must hear
 * of (a VRAM copy of them, a texture made from them), one bit per 4 KiB page below 2^40. The GPU
 * side sets and clears them where it used to protect; the writers it cannot see coming (libc
 * imports, file reads, hooked game code) check them after writing and tell it, as a driver is
 * told of a resource update. */
#define WATCH_LIMIT (1ull<<40)
static _Atomic(uint64_t) *s_p_write_watch;
static GpuRange s_pfn_hook_cpu_write;
void runtime_memory_set_cpu_write_hook(GpuRange pfn_hook) {
    if (!s_p_write_watch) {
        void *p_bits=mmap(NULL,WATCH_LIMIT>>15,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if (p_bits==MAP_FAILED) { perror("psport: write watch bitmap"); return; }
        s_p_write_watch=p_bits;
    }
    s_pfn_hook_cpu_write=pfn_hook;
}
void runtime_memory_set_write_watch(uintptr_t va_range, uint64_t cb_range, int watch) {
    if (!s_p_write_watch || !cb_range || va_range>=WATCH_LIMIT) return;
    uint64_t first=va_range>>12, last=(va_range+cb_range-1)>>12;
    if (last>=(WATCH_LIMIT>>12)) last=(WATCH_LIMIT>>12)-1;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (watch) atomic_fetch_or_explicit(&s_p_write_watch[word],mask,memory_order_release);
        else atomic_fetch_and_explicit(&s_p_write_watch[word],~mask,memory_order_release);
    }
}
void runtime_memory_note_cpu_write(uintptr_t va_range, uint64_t cb_range) {
    if (!s_p_write_watch || !cb_range || va_range>=WATCH_LIMIT || va_range+cb_range>WATCH_LIMIT) return;
    const uint64_t first=va_range>>12, last=(va_range+cb_range-1)>>12;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (atomic_load_explicit(&s_p_write_watch[word],memory_order_acquire) & mask) {
            s_pfn_hook_cpu_write(va_range,cb_range);
            return;
        }
    }
}
/* psport: write traps the GPU side keeps on guest pages, by reason (bits: a VRAM copy of the page,
 * an image made from it, a command buffer of a frame in flight, ...). A page with any reason is read-only for the game even where its
 * own protection allows writes; the fault goes to the GPU library's handlers, and each clears
 * only its own reason. One owner for these protections: the layer's VRAM copies and the texture
 * cache trap the same pages, and one's unprotect must not drop the other's trap. Reasons from 16 up
 * trap reads too (no access: the page's data is newer elsewhere, a VRAM copy). Lock order: the
 * runtime's lock, then s_trap_mutex (as kernel_mprotect, which keeps the traps). */
#define TRAP_LIMIT (1ull<<40)
#define TRAP_NO_ACCESS 0xf0u
static uint8_t *s_p_trap_reasons; /* one byte per 4 KiB page below TRAP_LIMIT (reserved, not committed) */
static pthread_mutex_t s_trap_mutex=PTHREAD_MUTEX_INITIALIZER;
/* A page's trap level: 0 none, 1 read-only, 2 no access. */
static int trap_level(uint8_t reasons) { return !reasons ? 0 : (reasons & TRAP_NO_ACCESS) ? 2 : 1; }
/* The pages [a, b) to the game's protection, without write at level 1, without access at 2
 * (runtime lock held). */
static void trap_apply(uintptr_t a, uintptr_t b, int level) {
    for (size_t i=vma_index(a); i<g_c_vmas && g_p_vmas[i].start<b; ++i) {
        if (g_p_vmas[i].kind==KIND_RESERVED) continue;
        const uintptr_t s=g_p_vmas[i].start>a ? g_p_vmas[i].start : a, e=g_p_vmas[i].end<b ? g_p_vmas[i].end : b;
        const int prot=host_prot(g_p_vmas[i].prot);
        mprotect((void *)s,e-s,level==2 ? PROT_NONE : level ? (prot & ~PROT_WRITE) : prot);
    }
}
/* No traps in [start, end) any more: a mapping replaced or gone (runtime lock held). */
void trap_forget(uintptr_t start, uintptr_t end) {
    if (!s_p_trap_reasons || start>=TRAP_LIMIT) return;
    if (end>TRAP_LIMIT) end=TRAP_LIMIT;
    pthread_mutex_lock(&s_trap_mutex);
    memset(s_p_trap_reasons+(start>>12),0,((end+4095)>>12)-(start>>12));
    pthread_mutex_unlock(&s_trap_mutex);
}
/* Pages with traps in [start, end) read-only (or no access) again after the game changed their
 * protection (runtime lock held). */
void trap_reapply(uintptr_t start, uintptr_t end) {
    if (!s_p_trap_reasons || start>=TRAP_LIMIT) return;
    if (end>TRAP_LIMIT) end=TRAP_LIMIT;
    pthread_mutex_lock(&s_trap_mutex);
    uintptr_t run=0; int run_level=0;
    for (uintptr_t p=start>>12; p<(end+4095)>>12; ++p) {
        const int level=trap_level(s_p_trap_reasons[p]);
        if (run && level!=run_level) { trap_apply(run,p<<12,run_level); run=0; }
        if (level && !run) { run=p<<12; run_level=level; }
    }
    if (run) trap_apply(run,end,run_level);
    pthread_mutex_unlock(&s_trap_mutex);
}
void runtime_memory_trap(uintptr_t va_range, uint64_t cb_range, unsigned reason, int is_on) {
    if (!cb_range || va_range>=TRAP_LIMIT) return;
    if (!s_p_trap_reasons) {
        void *p_bytes=mmap(NULL,TRAP_LIMIT>>12,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if (p_bytes==MAP_FAILED) { perror("psport: write trap table"); return; }
        uint8_t *p_expected=NULL;
        if (!__atomic_compare_exchange_n(&s_p_trap_reasons,&p_expected,(uint8_t *)p_bytes,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            munmap(p_bytes,TRAP_LIMIT>>12);
    }
    uintptr_t end=va_range+cb_range>TRAP_LIMIT ? TRAP_LIMIT : va_range+cb_range;
    read_lock();
    pthread_mutex_lock(&s_trap_mutex);
    /* Runs of pages whose trap level changes, one mprotect each. */
    uintptr_t run=0; int run_level=0;
    const uintptr_t first=va_range>>12, last=(end-1)>>12;
    for (uintptr_t p=first; p<=last; ++p) {
        const uint8_t old=s_p_trap_reasons[p], now=is_on ? (uint8_t)(old|reason) : (uint8_t)(old & ~reason);
        if (old!=now) s_p_trap_reasons[p]=now;
        const int level=trap_level(now), change=trap_level(old)!=level;
        if (run && (!change || level!=run_level)) { trap_apply(run,p<<12,run_level); run=0; }
        if (change && !run) { run=p<<12; run_level=level; }
    }
    if (run) trap_apply(run,(last+1)<<12,run_level);
    pthread_mutex_unlock(&s_trap_mutex);
    read_unlock();
}
/* psport: a libc write about to land on pages with write traps: the GPU side is told first and
 * lifts its traps, instead of the write faulting (a fault handler waits for the draws the GPU
 * thread has queued; libc copies into small textures every frame made the game's frames late).
 * Its announcement after the write (runtime_memory_note_cpu_write) stays as it was. */
void runtime_memory_prepare_cpu_write(uintptr_t va_range, uint64_t cb_range) {
    const uint8_t *p_table=__atomic_load_n(&s_p_trap_reasons,__ATOMIC_ACQUIRE);
    if (!p_table || !cb_range || va_range>=TRAP_LIMIT || !s_pfn_hook_cpu_write) return;
    uint64_t last=(va_range+cb_range-1)>>12;
    if (last>=(TRAP_LIMIT>>12)) last=(TRAP_LIMIT>>12)-1;
    for (uint64_t p=va_range>>12; p<=last; ++p)
        if (__atomic_load_n(&p_table[p],__ATOMIC_RELAXED)) { s_pfn_hook_cpu_write(va_range,cb_range); return; }
}
unsigned runtime_memory_trap_reasons(uintptr_t va_guest) {
    const uint8_t *p_table=__atomic_load_n(&s_p_trap_reasons,__ATOMIC_ACQUIRE);
    return p_table && va_guest<TRAP_LIMIT ? __atomic_load_n(&p_table[va_guest>>12],__ATOMIC_ACQUIRE) : 0;
}
/* psport: where direct memory at a guest address lives (PS_GUEST_IN_PLACE: the GPU uses the game's
 * memory where it is): its offset in direct memory and the end of the mapping, which is contiguous
 * there. 0 for anything else (flexible memory, reserved or unmapped addresses). */
int runtime_memory_direct_phys(uintptr_t va_guest, uint64_t *p_phys, uintptr_t *p_end) {
    read_lock();
    const size_t i=vma_index(va_guest);
    const int is_ok = i<g_c_vmas && g_p_vmas[i].start<=va_guest && va_guest<g_p_vmas[i].end && g_p_vmas[i].kind==KIND_DIRECT;
    if (is_ok) {
        *p_phys=g_p_vmas[i].phys+(va_guest-g_p_vmas[i].start);
        *p_end=g_p_vmas[i].end;
    }
    read_unlock();
    return is_ok;
}
/* psport: protection and direct memory type of the mapping at address (type -1: not direct
 * memory); 0 when nothing is mapped there. */
int runtime_memory_vma_info(uintptr_t va_guest, int *p_prot, int *p_type, uintptr_t *p_end) {
    read_lock();
    const size_t i=vma_index(va_guest);
    const int is_ok = i<g_c_vmas && g_p_vmas[i].start<=va_guest && g_p_vmas[i].kind!=KIND_RESERVED;
    if (is_ok) {
        *p_prot=g_p_vmas[i].prot;
        *p_type=g_p_vmas[i].kind==KIND_DIRECT ? g_p_vmas[i].type : -1;
        *p_end=g_p_vmas[i].end;
    }
    read_unlock();
    return is_ok;
}
/* Writes through the backing view; 0 when part of the range has no backing. */
/* Guest-visible writes from the GPU side (fence labels, WriteData, downloads): every byte is
 * stored once. glibc's memcpy copies 4-7 bytes as two overlapping 4-byte stores (the same
 * 4 bytes twice for a 4-byte label): a thread preempted between them stored the label again
 * after the guest had seen it and freed the block that held it (a free-list link ending in
 * 0x00000004: guest fault 0x263b8e7). Aligned 2, 4 and 8-byte pieces are single stores. */
static void store_once(unsigned char *p_dst, const unsigned char *p_src, uint64_t cb_copy) {
    if (cb_copy > 64) { memcpy(p_dst,p_src,cb_copy); return; }
    while (cb_copy) {
        const uintptr_t at=(uintptr_t)p_dst;
        if (cb_copy>=8 && !(at&7)) { uint64_t v; memcpy(&v,p_src,8); __atomic_store_n((uint64_t *)p_dst,v,__ATOMIC_RELEASE); p_dst+=8; p_src+=8; cb_copy-=8; }
        else if (cb_copy>=4 && !(at&3)) { uint32_t v; memcpy(&v,p_src,4); __atomic_store_n((uint32_t *)p_dst,v,__ATOMIC_RELEASE); p_dst+=4; p_src+=4; cb_copy-=4; }
        else if (cb_copy>=2 && !(at&1)) { uint16_t v; memcpy(&v,p_src,2); __atomic_store_n((uint16_t *)p_dst,v,__ATOMIC_RELEASE); p_dst+=2; p_src+=2; cb_copy-=2; }
        else { __atomic_store_n(p_dst,*p_src,__ATOMIC_RELEASE); ++p_dst; ++p_src; --cb_copy; }
    }
}
static unsigned char *backing_of(const Vma *p_vma, uintptr_t at) {
    return p_vma->kind==KIND_FILE ? p_vma->p_view+(at-p_vma->start) : g_p_backing_base+p_vma->phys+(at-p_vma->start);
}
static uint64_t s_c_viewless_file_writes;
int runtime_memory_write_backing(uintptr_t va_range, const void *p_data, uint64_t cb_range) {
    read_lock();
    int is_ok=1;
    for (uintptr_t at=va_range, end=va_range+cb_range; at<end && is_ok;) {
        size_t i=vma_index(at);
        if (i==g_c_vmas || g_p_vmas[i].start>at || g_p_vmas[i].kind==KIND_RESERVED) { is_ok=0; break; }
        if (g_p_vmas[i].kind==KIND_FILE && !g_p_vmas[i].p_view) {
            if (!__atomic_fetch_add(&s_c_viewless_file_writes,1,__ATOMIC_RELAXED)) {
                printf("Runtime: GPU write to file mapping %#" PRIxPTR " without a host view: dropped\n",at);
                runtime_calls_hle_stub("GPU write to a file mapping without a host view",&s_c_viewless_file_writes);
            }
            is_ok=0; break;
        }
        uint64_t cb_chunk=(g_p_vmas[i].end<end ? g_p_vmas[i].end : end)-at;
        store_once(backing_of(&g_p_vmas[i],at),(const unsigned char *)p_data+(at-va_range),cb_chunk);
        at+=cb_chunk;
    }
    read_unlock();
    return is_ok;
}
/* Reads through the backing view: the host's copies of guest memory (uploads), which the GPU's
 * read protection (PS_READBACKS=2: GPU-written pages) must not stop. A fault there would ask the
 * GPU thread for a readback while it waits for these very copies (a hang at start). Gaps read
 * as zeros. */
void runtime_memory_read_backing(uintptr_t va_range, void *p_data, uint64_t cb_range) {
    unsigned char *p_out=p_data;
    read_lock();
    for (uintptr_t at=va_range, end=va_range+cb_range; at<end;) {
        const size_t i=vma_index(at);
        uint64_t cb_chunk;
        if (i<g_c_vmas && g_p_vmas[i].start<=at && g_p_vmas[i].kind!=KIND_RESERVED) {
            cb_chunk=(g_p_vmas[i].end<end ? g_p_vmas[i].end : end)-at;
            memcpy(p_out+(at-va_range),g_p_vmas[i].kind==KIND_FILE && !g_p_vmas[i].p_view ? (const void *)at : backing_of(&g_p_vmas[i],at),cb_chunk);
        } else {
            const uintptr_t next=i==g_c_vmas ? end : g_p_vmas[i].start>at ? g_p_vmas[i].start : g_p_vmas[i].end;
            cb_chunk=(next<end ? next : end)-at;
            memset(p_out+(at-va_range),0,cb_chunk);
        }
        at+=cb_chunk;
    }
    read_unlock();
}
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; } CachedRegion;
static __thread CachedRegion s_region_cache[8];
static __thread unsigned s_i_region_cache_next;
static const CachedRegion *cached_region(uintptr_t va_guest) {
    uint64_t generation=__atomic_load_n(&g_table_generation,__ATOMIC_ACQUIRE);
    if (generation & 1) return NULL;
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *p_cached_region=&s_region_cache[i];
        if (p_cached_region->generation==generation && p_cached_region->end && p_cached_region->start<=va_guest && va_guest<p_cached_region->end) return p_cached_region;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&g_table_generation,__ATOMIC_ACQUIRE);
    if (i==g_c_vmas || (generation & 1)) return;
    s_region_cache[s_i_region_cache_next++ % 8]=(CachedRegion){generation,g_p_vmas[i].start,g_p_vmas[i].end,g_p_vmas[i].kind};
}
/* The mapping table's generation (odd while it changes): callers cache regions against it. */
const uint64_t *runtime_memory_generation(void) { return &g_table_generation; }
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t va_range, uint64_t cb_range) {
    const CachedRegion *p_cached_region=(g_runtime_disabled_optimizations & 1) ? NULL : cached_region(va_range);
    if (p_cached_region && p_cached_region->kind!=KIND_RESERVED && cb_range<=p_cached_region->end-va_range) return cb_range;
    read_lock();
    { size_t i=vma_index(va_range); if (i<g_c_vmas && g_p_vmas[i].start<=va_range) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(va_range); i<g_c_vmas && length<cb_range; ++i) {
        if (g_p_vmas[i].start>va_range+length || g_p_vmas[i].kind==KIND_RESERVED) break;
        length=g_p_vmas[i].end-va_range;
    }
    read_unlock();
    return length<cb_range ? length : cb_range;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t va_guest, uintptr_t *p_start, uintptr_t *p_end, int *p_mapped) {
    const CachedRegion *p_cached_region=(g_runtime_disabled_optimizations & 1) ? NULL : cached_region(va_guest);
    if (p_cached_region) { *p_start=p_cached_region->start; *p_end=p_cached_region->end; *p_mapped=p_cached_region->kind!=KIND_RESERVED; return 1; }
    read_lock();
    size_t i=vma_index(va_guest);
    int is_found=i<g_c_vmas;
    if (is_found && g_p_vmas[i].start<=va_guest) {
        cache_region(i);
        *p_start=g_p_vmas[i].start; *p_end=g_p_vmas[i].end; *p_mapped=g_p_vmas[i].kind!=KIND_RESERVED;
    } else if (is_found) {
        *p_start=i ? g_p_vmas[i-1].end : 0; *p_end=g_p_vmas[i].start; *p_mapped=0;
    }
    read_unlock();
    return is_found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
void runtime_memory_gpu_protect(uintptr_t va_range, uint64_t cb_range, int read, int write) {
    read_lock();
    for (size_t i=vma_index(va_range); i<g_c_vmas && g_p_vmas[i].start<va_range+cb_range; ++i) {
        if (g_p_vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=g_p_vmas[i].start>va_range ? g_p_vmas[i].start : va_range;
        uintptr_t b=g_p_vmas[i].end<va_range+cb_range ? g_p_vmas[i].end : va_range+cb_range;
        int guest=host_prot(g_p_vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        mprotect((void *)a,b-a,guest & want);
    }
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t va_range, uint64_t cb_range) {
    const CachedRegion *p_cached_region=(g_runtime_disabled_optimizations & 1) ? NULL : cached_region(va_range);
    if (p_cached_region && p_cached_region->kind!=KIND_RESERVED && cb_range<=p_cached_region->end-va_range) return 1;
    read_lock();
    int result=covered(va_range,va_range+cb_range,0);
    read_unlock();
    return result;
}
#endif
