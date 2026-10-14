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
/* Ends the process after a STOP: the call report, then _exit, since exit() from a game thread runs the
 * GPU library's destructors while its threads are still running and never returns. */
__attribute__((noreturn)) void runtime_stop(int status);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
/* Printed by runtime_calls_report on every exit, the fault handler and std::_Exit included, so they
 * take no locks and write to stdout directly. */
void runtime_memory_flexible_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size);
/* libkernel memory pools (runtime_mempool.c) over the runtime's direct memory and mapping table. */
uintptr_t runtime_mempool_resolve(const char *name);
void runtime_mempool_report(void);
/* libkernel asynchronous file I/O (runtime_aio.c) on worker threads over the runtime's file layer. */
uintptr_t runtime_aio_resolve(const char *name);
void runtime_aio_report(void);
int32_t runtime_memory_pool_expand(uint64_t low, uint64_t high, uint64_t size, uint64_t alignment, int64_t *phys);
int32_t runtime_memory_pool_reserve(void **inout, uint64_t size, int flags, uint64_t alignment);
/* Maps [phys, phys+size) of direct memory over a pool reservation; EINVAL unless all of the range is one. */
int32_t runtime_memory_pool_commit(uintptr_t address, uint64_t size, int prot, int type, uint64_t phys);
/* Turns committed or reserved pool range back into a pool reservation; release receives each piece of
 * direct memory that was committed there, under the runtime's lock. */
int32_t runtime_memory_pool_decommit(uintptr_t address, uint64_t size, void (*release)(uint64_t phys, uint64_t size));
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
/* How the export reports a failure, per shadPS4's prototypes (import_returns.inc): 1 a 32-bit SCE
 * error code, 2 and 3 -1 with errno as a 32-bit or 64-bit result, 4 a 64-bit SCE error code; 0 when
 * its result says nothing about failure or it takes arguments on the stack. */
unsigned runtime_import_failure(const char *nid);
/* 1 when the export reports a state its caller polls or waits for with this code (an SCE code, or
 * the errno of a POSIX export), per import_statuses.inc: counted apart from the failures. */
int runtime_import_status(const char *nid, uint32_t code);
/* Calls nothing answers for real or that fail (runtime_calls.c): logged once per import and error
 * code, counted, and summed up by runtime_calls_report (also into PS_CALL_REPORT). */
void runtime_calls_configure(const char (*names)[128], uint64_t count);
void runtime_calls_unimplemented(uint32_t id);
ABI int64_t runtime_calls_stub(uint32_t id, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);
/* A call to import id with these six arguments returned result, which kind says may be a failure;
 * returns result. */
ABI int64_t runtime_calls_failed(uint32_t id, const uintptr_t *arguments, int64_t result, uint32_t kind);
/* An HLE function logging "(STUBBED)" was called the first time; *count keeps counting its calls. */
void runtime_calls_hle_stub(const char *function, const uint64_t *count);
void runtime_calls_report(void);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
uintptr_t runtime_barrier_resolve(const char *sz_name);
void runtime_barrier_report(void);
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
/* The same around a runtime lock of another kind (runtime_memory's rwlock): hold before taking it,
 * release after letting it go. */
void runtime_exception_hold(void);
void runtime_exception_release(void);
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
uintptr_t runtime_ult_resolve(const char *name);
void runtime_eventflag_report(void);
void runtime_eventflag_created_report(void);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
void runtime_content_title(const char *title);
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
/* sceSystemServiceParamGetInt's language (PS_LANGUAGE, default 1 = English US). */
int runtime_system_language(void);
uintptr_t runtime_playgo_resolve(const char *name);
/* Absent peripherals: USB devices and the PlayStation Camera (runtime_devices.c). */
uintptr_t runtime_devices_resolve(const char *sz_name);
/* Remote Play, Share Play, video recording and core dumps, none of them running (runtime_capture.c). */
uintptr_t runtime_capture_resolve(const char *sz_name);
uintptr_t runtime_voice_resolve(const char *name);
/* Offline libSceNet sockets (runtime_socket.c), consulted by runtime_services_resolve. */
uintptr_t runtime_socket_resolve(const char *name);
/* libSceJson (runtime_json.c), consulted by runtime_services_resolve. */
uintptr_t runtime_json_resolve(const char *sz_name);
/* PSN libraries offline (runtime_np.c), consulted by runtime_services_resolve. */
uintptr_t runtime_np_resolve(const char *sz_name);
int32_t *runtime_net_errno(void);
/* Online play (runtime_net.c, PS_ONLINE=1): the online module answers the network and PSN imports. */
void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version);
int runtime_net_enabled(void);
int runtime_net_symbol(const char *symbol);
uintptr_t runtime_net_resolve(const char *name);
/* Guest threads for host code (the online module's NP threads); 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **thread, void *(ABI *entry)(void *), void *argument, uint64_t stack, const char *name);
int32_t runtime_thread_join_spawned(void *thread, void **result);
void runtime_file_report(void);
void runtime_file_unmounted_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int runtime_file_shared_host(int fd);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_pwrite(int fd, const void *buffer, uint64_t size, int64_t offset);
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
/* libSceAudioOut's own calls, for the libraries that output through its ports (libSceAudio3d). */
int32_t runtime_audio_init(void);
int32_t runtime_audio_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param);
int32_t runtime_audio_output(int32_t handle, const void *data);
int32_t runtime_audio_close(int32_t handle);
/* Channels of the host's default playback device: 2 without one. */
int runtime_audio_device_channels(void);
uintptr_t runtime_audio3d_resolve(const char *name);
void runtime_audio3d_report(void);
uintptr_t runtime_pad_resolve(const char *name);
uintptr_t runtime_gesture_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_mouse_resolve(const char *name);
void runtime_mouse_report(void);
uintptr_t runtime_keyboard_resolve(const char *name);
void runtime_keyboard_report(void);
/* 1 while the game has libSceImeKeyboard open: the host keyboard is its, not the pad's. */
int runtime_keyboard_taken(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
/* The title id the game's saves are kept under (INSTALL_DIR_SAVEDATA, else TITLE_ID). */
const char *runtime_savedata_title(void);
uintptr_t runtime_savedata_resolve(const char *name);
/* Fills an OrbisSaveDataParam (zeroed when the save has none); -1 when the save does not exist. */
int runtime_savedata_param(int32_t user, const char *title, const char *dir, void *param);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);
/* What the loader prelinked (src/loader_link.c), at image offsets; [0] is the eboot. Deferred modules (loaded by
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
