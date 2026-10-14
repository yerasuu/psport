/* Shared by the runtime_memory*.c files: the region table and its lock, the direct and flexible
 * memory backing, and the queued GPU notifications. Hidden, so that the host libraries the binary
 * exports its symbols to (-rdynamic) never bind to these. */
#ifndef BB_RUNTIME_MEMORY_H
#define BB_RUNTIME_MEMORY_H
#include "runtime.h"
#ifndef _WIN32
#include <sys/mman.h>
#pragma GCC visibility push(hidden)
uint64_t pool_size_bytes(void);
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define NOT_FOUND ((int32_t)UINT32_C(0x80020002))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE, KIND_FILE };
/* view: a KIND_FILE region's second host mapping of its file, at start (NULL when the file could
 * not be mapped writable): the GPU's backing reads and writes, which the guest's own protection of
 * the region (read traps, read-only prot) must not stop. */
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; int pooled; unsigned char *view; } Vma;
void write_lock(void);
void write_unlock(void);
void read_lock(void);
void read_unlock(void);
extern uint64_t table_generation;
extern Vma *vmas; extern size_t vma_count;
extern unsigned char *backing_base;
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
extern GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
extern size_t pending_count;
extern uint64_t flexible_bytes;
static inline uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static inline int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
int pool(void);
void ensure_chunks(uint64_t phys, uint64_t size);
void zero_phys(uint64_t phys, uint64_t size);
void queue_hook(int kind, uintptr_t address, uint64_t size);
void flush_hooks(void);
uint64_t flex_alloc(uint64_t size);
void flex_free(uint64_t phys, uint64_t size);
size_t vma_index(uintptr_t a);
size_t carve(uintptr_t start, uintptr_t end, int *error);
int covered(uintptr_t start, uintptr_t end, int allow_reserved);
void drop_range(uintptr_t start, uintptr_t end);
int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment, int kind, int type, uint64_t phys);
int32_t place_file(void **inout, uint64_t size, int prot, int flags, int file, uint64_t offset);
/* bbport: write traps (runtime_memory_trap, runtime_memory_gpu.c), kept across the game's own changes. */
void trap_reapply(uintptr_t start, uintptr_t end);
void trap_forget(uintptr_t start, uintptr_t end);
#pragma GCC visibility pop
#endif
#endif
