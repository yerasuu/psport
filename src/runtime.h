#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#ifndef _WIN32
#include <setjmp.h>
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#endif
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
/* Equal NIDs whatever their "#<library>#<module>" scope: one title's scope letters are not another's. */
int runtime_same_nid(const char *a, const char *b);
/* Linked modules' dynamic TLS ids run from 2 (1 is the eboot's static TLS) below this. */
#define RUNTIME_TLS_MODULES 32
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size);
/* bbport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
/* bbport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
const char *runtime_import_name(const char *name);
/* Arguments (bit N: argument N) the export writes through a pointer, per shadPS4's prototypes; 0
 * when it has none or the NID is not listed. */
unsigned runtime_import_outputs(const char *nid);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
uintptr_t runtime_thread_resolve(const char *name);
uintptr_t runtime_sched_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
/* Sends a host signal to a guest thread (ScePthread handle): 0, or 0x80020003 when the handle names
 * no live thread. */
int32_t runtime_thread_kill(void *thread, int host_signal);
uintptr_t runtime_exception_resolve(const char *name);
void runtime_exception_report(void);
#ifndef _WIN32
#include <pthread.h>
/* The host signal a guest exception raise arrives on. SIGUSR1 stays free for host libraries;
 * SIGUSR2, SIGALRM and SIGPROF are probe.c's thread dumps, watchdog and the wait sampler. */
#define RUNTIME_EXCEPTION_SIGNAL (SIGRTMIN+3)
/* For runtime locks a guest exception handler takes again (kernel semaphores, event flags): a raise
 * that lands on the lock's holder is delivered once it lets go, or the handler would wait on its
 * own thread. */
void runtime_exception_lock(pthread_mutex_t *lock);
void runtime_exception_unlock(pthread_mutex_t *lock);
/* pthread_cond_wait, or a wait until end on CLOCK_MONOTONIC, under such a lock. glibc's condition
 * wait has no point at which the thread is known not to hold the mutex, so a raise stays held back
 * while it sleeps; the sleep is cut into slices and returns 0 early, as a spurious wakeup, to deliver
 * one. */
int runtime_exception_wait(pthread_cond_t *cond, pthread_mutex_t *lock, const struct timespec *end);
#endif
/* The calling thread runs guest code on this stack from now on (the guest main stack): what
 * scePthreadAttrGet reports, which garbage collectors scan up to. */
void runtime_thread_set_stack(void *addr, uint64_t size);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
uintptr_t runtime_eventflag_resolve(const char *name);
uintptr_t runtime_ulobjmgr_resolve(const char *name);
void runtime_eventflag_report(void);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
/* What an export table lists for a function the runtime does not implement but answers with
 * success: runtime_lookup reports each import resolved to it (runtime_last_stub). */
ABI int32_t runtime_stub(void);
extern int runtime_last_stub;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
void runtime_process_configure(uint32_t sdk_version, uint32_t psf_attributes);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* Online play (runtime_net.c, BB_ONLINE=1): the online module answers the network and PSN imports. */
void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version);
int runtime_net_enabled(void);
int runtime_net_symbol(const char *symbol);
uintptr_t runtime_net_resolve(const char *name);
/* Guest threads for host code (the online module's NP threads); 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **thread, void *(ABI *entry)(void *), void *argument, uint64_t stack, const char *name);
int32_t runtime_thread_join_spawned(void *thread, void **result);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);
/* What link_modules.py prelinked, at image offsets; [0] is the eboot. Deferred modules (loaded by
 * the game with sceKernelLoadStartModule) start there, the others at boot through
 * runtime_module_start, which runs an initializer at most once. */
typedef struct { uint64_t address, size, prot; } RuntimeModuleSegment;
typedef struct { char nid[16]; uint64_t address; } RuntimeModuleExport;
typedef struct {
    char name[128];
    uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module, deferred;
    uint64_t eh_frame_hdr, eh_frame_hdr_size, eh_frame, eh_frame_size, segment_count;
    RuntimeModuleSegment segments[4];
    uint64_t export_count;
    RuntimeModuleExport *exports;
} RuntimeModule;
/* state: one byte per module, nonzero once started; owned by the caller for the process. */
void runtime_modules_configure(unsigned char *image, RuntimeModule *modules, size_t count, unsigned char *state);
/* 1 when this call ran the initializer (its value in *result), 0 when it had run already. */
int runtime_module_start(size_t handle, uint64_t args, const void *argp, int32_t *result);
uintptr_t runtime_module_resolve(const char *name);
#endif
