#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#ifndef _WIN32
#include <setjmp.h>
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *g_p_runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
/* sceSystemServiceLoadExec (probe.c): psport starts again on its own command line with sz_path, a
 * guest path of the package, as the main executable and pp_arguments (NULL-terminated, may be NULL)
 * after it; returns only when the exec failed. */
void runtime_load_exec(const char *sz_path, char *const *pp_arguments);
/* psport's own command line, which runtime_load_exec starts again from; kept. */
void runtime_restart_configure(int c_argv, char **pp_argv);
#endif
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *sz_name, int is_data);
/* The libraries after the runtime's own exports (runtime_resolve.c), in a fixed order. */
uintptr_t runtime_library_resolve(const char *sz_name);
/* Equal NIDs whatever their "#<library>#<module>" scope: one title's scope letters are not another's. */
int runtime_same_nid(const char *sz_a, const char *sz_b);
/* Linked modules' dynamic TLS ids run from 2 (1 is the eboot's static TLS) below this. */
#define RUNTIME_TLS_MODULES 32
void runtime_report(void);
/* Ends the process after a STOP: the call report, then _exit, since exit() from a game thread runs the
 * GPU library's destructors while its threads are still running and never returns. */
__attribute__((noreturn)) void runtime_stop(int status);
void runtime_finalize(void *p_dso);
uintptr_t runtime_mutex_resolve(const char *sz_name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *sz_name);
void runtime_memory_report(void);
/* Printed by runtime_calls_report on every exit, the fault handler and std::_Exit included, so they
 * take no locks and write to stdout directly. */
void runtime_memory_flexible_report(void);
int runtime_memory_is_mapped(uintptr_t va_range, uint64_t cb_range);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t va_range, uint64_t cb_range);
void runtime_memory_note_cpu_write(uintptr_t va_range, uint64_t cb_range);
void runtime_memory_prepare_cpu_write(uintptr_t va_range, uint64_t cb_range);
/* libkernel memory pools (runtime_mempool.c) over the runtime's direct memory and mapping table. */
uintptr_t runtime_mempool_resolve(const char *sz_name);
void runtime_mempool_report(void);
/* libkernel asynchronous file I/O (runtime_aio.c) on worker threads over the runtime's file layer. */
uintptr_t runtime_aio_resolve(const char *sz_name);
void runtime_aio_report(void);
int32_t runtime_memory_pool_expand(uint64_t low, uint64_t high, uint64_t cb_range, uint64_t alignment, int64_t *p_phys);
int32_t runtime_memory_pool_reserve(void **pp_inout, uint64_t cb_range, int flags, uint64_t alignment);
/* Maps [phys, phys+size) of direct memory over a pool reservation; EINVAL unless all of the range is one. */
int32_t runtime_memory_pool_commit(uintptr_t va_range, uint64_t cb_range, int prot, int type, uint64_t phys);
/* Turns committed or reserved pool range back into a pool reservation; release receives each piece of
 * direct memory that was committed there, under the runtime's lock. */
int32_t runtime_memory_pool_decommit(uintptr_t va_range, uint64_t cb_range, void (*pfn_release)(uint64_t phys, uint64_t cb_range));
/* psport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
/* psport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
const char *runtime_import_name(const char *sz_name);
/* Arguments (bit N: argument N) the export writes through a pointer, per shadPS4's prototypes; 0
 * when it has none or the NID is not listed. */
unsigned runtime_import_outputs(const char *sz_nid);
/* How the export reports a failure, per shadPS4's prototypes (import_returns.inc): 1 a 32-bit SCE
 * error code, 2 and 3 -1 with errno as a 32-bit or 64-bit result, 4 a 64-bit SCE error code; 0 when
 * its result says nothing about failure or it takes arguments on the stack. */
unsigned runtime_import_failure(const char *sz_nid);
/* 1 when the export reports a state its caller polls or waits for with this code (an SCE code, or
 * the errno of a POSIX export), per import_statuses.inc: counted apart from the failures. */
int runtime_import_status(const char *sz_nid, uint32_t code);
/* Calls nothing answers for real or that fail (runtime_calls.c): logged once per import and error
 * code, counted, and summed up by runtime_calls_report (also into PS_CALL_REPORT). */
void runtime_calls_configure(const char (*p_names)[128], uint64_t c_names);
void runtime_calls_unimplemented(uint32_t id);
ABI int64_t runtime_calls_stub(uint32_t id, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);
/* A call to import id with these six arguments returned result, which kind says may be a failure;
 * returns result. */
ABI int64_t runtime_calls_failed(uint32_t id, const uintptr_t *p_arguments, int64_t result, uint32_t kind);
/* An HLE function logging "(STUBBED)" was called the first time; *count keeps counting its calls. */
void runtime_calls_hle_stub(const char *sz_function, const uint64_t *p_count);
void runtime_calls_report(void);
uintptr_t runtime_rwlock_resolve(const char *sz_name);
void runtime_rwlock_report(void);
uintptr_t runtime_barrier_resolve(const char *sz_name);
void runtime_barrier_report(void);
void runtime_set_module_tls(uint64_t module, const void *p_data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *p_param);
/* The guest's command line: pp_arguments[0] is the executable's guest path; NULL-terminated, kept. */
void runtime_set_arguments(char **pp_arguments, int c_arguments);
uintptr_t runtime_thread_resolve(const char *sz_name);
uintptr_t runtime_sched_resolve(const char *sz_name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *p_data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
/* Sends a host signal to a guest thread (ScePthread handle): 0, or 0x80020003 when the handle names
 * no live thread. */
int32_t runtime_thread_kill(void *p_thread, int host_signal);
/* The guest's sigprocmask bookkeeping for the calling thread (sigreturn's uc_sigmask). */
void runtime_kernel_set_signal_mask(const uint32_t bits[4]);
uintptr_t runtime_exception_resolve(const char *sz_name);
void runtime_exception_report(void);
#ifndef _WIN32
#include <pthread.h>
/* The host signal a guest exception raise arrives on. SIGUSR1 stays free for host libraries;
 * SIGUSR2, SIGALRM and SIGPROF are probe.c's thread dumps, watchdog and the wait sampler. */
#define RUNTIME_EXCEPTION_SIGNAL (SIGRTMIN+3)
/* For runtime locks a guest exception handler takes again (kernel semaphores, event flags): a raise
 * that lands on the lock's holder is delivered once it lets go, or the handler would wait on its
 * own thread. */
void runtime_exception_lock(pthread_mutex_t *p_lock);
void runtime_exception_unlock(pthread_mutex_t *p_lock);
/* The same around a runtime lock of another kind (runtime_memory's rwlock): hold before taking it,
 * release after letting it go. */
void runtime_exception_hold(void);
void runtime_exception_release(void);
/* pthread_cond_wait, or a wait until end on CLOCK_MONOTONIC, under such a lock. glibc's condition
 * wait has no point at which the thread is known not to hold the mutex, so a raise stays held back
 * while it sleeps; the sleep is cut into slices and returns 0 early, as a spurious wakeup, to deliver
 * one. */
int runtime_exception_wait(pthread_cond_t *p_cond, pthread_mutex_t *p_lock, const struct timespec *p_end);
#endif
/* The calling thread runs guest code on this stack from now on (the guest main stack): what
 * scePthreadAttrGet reports, which garbage collectors scan up to. */
void runtime_thread_set_stack(void *p_addr, uint64_t cb_stack);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *sz_name);
void runtime_sema_report(void);
uintptr_t runtime_eventflag_resolve(const char *sz_name);
uintptr_t runtime_ulobjmgr_resolve(const char *sz_name);
uintptr_t runtime_ult_resolve(const char *sz_name);
void runtime_eventflag_report(void);
void runtime_eventflag_created_report(void);
void runtime_content_configure(const uint32_t values[5]);
void runtime_content_title(const char *sz_title);
uintptr_t runtime_content_resolve(const char *sz_name);
void runtime_content_report(void);
typedef struct { const char *sz_name; void *p_function; } RuntimeExport;
/* What an export table lists for a function the runtime does not implement but answers with
 * success: runtime_lookup reports each import resolved to it (g_runtime_last_stub). */
ABI int32_t runtime_stub(void);
extern int g_runtime_last_stub;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *sz_nid);
uintptr_t runtime_lookup(const RuntimeExport *p_table, size_t c_exports, const char *sz_nid);
uintptr_t runtime_kernel_resolve(const char *sz_name);
void runtime_process_configure(uint32_t sdk_version, uint32_t psf_attributes);
uintptr_t runtime_file_resolve(const char *sz_name);
uintptr_t runtime_services_resolve(const char *sz_name);
/* sceSystemServiceParamGetInt's language (PS_LANGUAGE, default 1 = English US). */
int runtime_system_language(void);
uintptr_t runtime_playgo_resolve(const char *sz_name);
/* Absent peripherals: USB devices and the PlayStation Camera (runtime_devices.c). */
uintptr_t runtime_devices_resolve(const char *sz_name);
/* Remote Play, Share Play, video recording and core dumps, none of them running (runtime_capture.c). */
uintptr_t runtime_capture_resolve(const char *sz_name);
/* PlayStation VR with no headset connected: Hmd, VrTracker, SocialScreen (runtime_vr.c). */
uintptr_t runtime_vr_resolve(const char *sz_name);
uintptr_t runtime_voice_resolve(const char *sz_name);
/* Offline libSceNet sockets (runtime_socket.c), consulted by runtime_services_resolve. */
uintptr_t runtime_socket_resolve(const char *sz_name);
/* libSceHttp offline (runtime_http.c), consulted by runtime_services_resolve. */
uintptr_t runtime_http_resolve(const char *sz_name);
/* libSceNpTrophy (runtime_trophy.c), consulted by runtime_services_resolve. */
uintptr_t runtime_trophy_resolve(const char *sz_name);
/* libSceJson (runtime_json.c), consulted by runtime_services_resolve. */
uintptr_t runtime_json_resolve(const char *sz_name);
/* PSN libraries offline (runtime_np.c), consulted by runtime_services_resolve. */
uintptr_t runtime_np_resolve(const char *sz_name);
int32_t *runtime_net_errno(void);
/* The console's wired MAC address (runtime_socket.c). */
void runtime_net_ether_address(uint8_t p_out_address[6]);
/* Online play (runtime_net.c, PS_ONLINE=1): the online module answers the network and PSN imports. */
void runtime_net_configure(const char *sz_app0, const char *sz_serial, const char *sz_title, const char *sz_version);
int runtime_net_enabled(void);
int runtime_net_symbol(const char *sz_symbol);
uintptr_t runtime_net_resolve(const char *sz_name);
/* Guest threads for host code (the online module's NP threads); 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **pp_thread, void *(ABI *pfn_entry)(void *), void *p_argument, uint64_t stack, const char *sz_name);
int32_t runtime_thread_join_spawned(void *p_thread, void **pp_result);
void runtime_file_report(void);
void runtime_file_unmounted_report(void);
void runtime_file_configure(const char *sz_app0, const char *sz_user);
int runtime_file_mount(const char *sz_guest, const char *sz_host);
void runtime_file_unmount(const char *sz_guest);
int runtime_file_translate(const char *sz_guest, char *sz_out, size_t cb_out);
int64_t runtime_file_open(const char *sz_path, int flags, int mode);
int64_t runtime_file_close(int h_fd);
int64_t runtime_file_read(int h_fd, void *p_buffer, uint64_t cb_buffer);
int64_t runtime_file_pread(int h_fd, void *p_buffer, uint64_t cb_buffer, int64_t off_file);
int runtime_file_shared_host(int h_fd);
/* A host descriptor (a socket) as a guest descriptor in the file table, which owns it from then on;
 * the descriptor, or -EMFILE with the table full (the caller still owns h_host then). */
int runtime_file_adopt(int h_host);
/* The host descriptor behind a guest one, or -1. */
int runtime_file_host(int h_fd);
int64_t runtime_file_write(int h_fd, const void *p_buffer, uint64_t cb_buffer);
int64_t runtime_file_pwrite(int h_fd, const void *p_buffer, uint64_t cb_buffer, int64_t off_file);
int64_t runtime_file_lseek(int h_fd, int64_t off_file, int whence);
int64_t runtime_file_stat(const char *sz_path, void *p_guest_stat);
int64_t runtime_file_fstat(int h_fd, void *p_guest_stat);
int64_t runtime_file_getdents(int h_fd, char *sz_buffer, uint64_t cb_buffer, int64_t *p_base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t cb_range, int prot);
uintptr_t runtime_ajm_resolve(const char *sz_name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *sz_name);
void runtime_audio_report(void);
/* libSceAudioOut's own calls, for the libraries that output through its ports (libSceAudio3d). */
int32_t runtime_audio_init(void);
int32_t runtime_audio_open(int32_t user, int32_t type, int32_t i_port, uint32_t length, uint32_t freq, uint32_t param);
int32_t runtime_audio_output(int32_t h_handle, const void *p_data);
int32_t runtime_audio_close(int32_t h_handle);
/* Channels of the host's default playback device: 2 without one. */
int runtime_audio_device_channels(void);
uintptr_t runtime_audio3d_resolve(const char *sz_name);
void runtime_audio3d_report(void);
uintptr_t runtime_pad_resolve(const char *sz_name);
uintptr_t runtime_gesture_resolve(const char *sz_name);
void runtime_pad_report(void);
uintptr_t runtime_mouse_resolve(const char *sz_name);
void runtime_mouse_report(void);
uintptr_t runtime_keyboard_resolve(const char *sz_name);
void runtime_keyboard_report(void);
/* 1 while the game has libSceImeKeyboard open: the host keyboard is its, not the pad's. */
int runtime_keyboard_taken(void);
/* libSceIme's on-screen keyboard (runtime_ime.c), consulted by runtime_keyboard_resolve. update hands
 * its due events to handler (the one it was opened with when NULL) and returns 0 while it is closed. */
uintptr_t runtime_ime_resolve(const char *sz_name);
int runtime_ime_update(void *p_handler);
/* The panel's size in pixels of a 1920x1080 screen for an OrbisImeType. */
void runtime_ime_panel_size(uint32_t type, uint32_t *p_width, uint32_t *p_height);
/* UTF-16 text of the IME and its dialog to UTF-8 and back (runtime_services.c): at most limit units
 * are read; max units and a terminator are written. */
size_t utf16_to_utf8(const uint16_t *p_in, size_t limit, char *sz_out, size_t cb_out);
void utf8_to_utf16(const char *sz_in, uint16_t *p_out, uint32_t max);
uintptr_t runtime_rtc_resolve(const char *sz_name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *sz_title);
/* The title id the game's saves are kept under (INSTALL_DIR_SAVEDATA, else TITLE_ID). */
const char *runtime_savedata_title(void);
uintptr_t runtime_savedata_resolve(const char *sz_name);
/* Fills an OrbisSaveDataParam (zeroed when the save has none); -1 when the save does not exist. */
int runtime_savedata_param(int32_t user, const char *sz_title, const char *sz_dir, void *p_param);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *sz_name);
/* What the loader prelinked (src/loader/loader_link.c), at image offsets; [0] is the eboot. Deferred modules (loaded by
 * the game with sceKernelLoadStartModule) start there, the others at boot through
 * runtime_module_start, which runs an initializer at most once. */
typedef struct { uint64_t va_module_segment, cb_module_segment, prot; } RuntimeModuleSegment;
typedef struct { char sz_nid[16]; uint64_t va_export; } RuntimeModuleExport;
typedef struct {
    char sz_name[128];
    uint64_t base, cb_module, init, va_tls, tls_memsz, tls_filesz, tls_module, deferred;
    uint64_t eh_frame_hdr, cb_eh_frame_hdr, eh_frame, cb_eh_frame, c_segments;
    RuntimeModuleSegment segments[4];
    uint64_t c_exports;
    RuntimeModuleExport *p_exports;
} RuntimeModule;
/* state: one byte per module, nonzero once started; owned by the caller for the process. */
void runtime_modules_configure(unsigned char *p_image, RuntimeModule *p_modules, size_t c_modules, unsigned char *p_state);
/* 1 when this call ran the initializer (its value in *result), 0 when it had run already. */
int runtime_module_start(size_t h_handle, uint64_t args, const void *p_argp, int32_t *p_result);
uintptr_t runtime_module_resolve(const char *sz_name);
#endif
