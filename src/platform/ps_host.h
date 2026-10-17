/* The host platform layer (decision #540): what psport asks of the operating system, behind one
 * small C ABI with an implementation per OS in src/platform/<os>/, so that the runtime and the
 * loader name what they need instead of a Linux call. Included from C and C++ (libpsport-gpu.so
 * reaches these through -rdynamic). Only Linux is implemented; host_features.inc says what each
 * OS has. */
#ifndef PS_HOST_H
#define PS_HOST_H
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- the capability table (host_features.inc) ---- */
typedef enum {
    PS_HOST_FEATURE_STATE_IMPLEMENTED,
    PS_HOST_FEATURE_STATE_EXPERIMENTAL,
    PS_HOST_FEATURE_STATE_UNAVAILABLE,
} PsHostFeatureState;

typedef uint16_t PsHostFeatureId;
enum {
#define FEATURE(id, area, sz_desc) PS_HOST_FEATURE_##id,
#define FEATURE_HOST(id, os, state, sz_reason, sz_ref)
#define FEATURE_PROBED(id)
#include "host_features.inc"
#undef FEATURE
#undef FEATURE_HOST
#undef FEATURE_PROBED
    PS_HOST_FEATURE_COUNT
};

/* psport --list-features: every entry as JSON on stdout, with this host's state. */
void ps_host_features_list(void);
/* Once at launch, before the game's code runs: the "Host features:" line and the exit summary. */
void ps_host_features_log(void);
/* A FEATURE_PROBED entry's run-time answer, from the code that probes it (the GPU library once the
 * Vulkan device is known): logged and kept for the exit summary. sz_result must outlive the
 * process (a string literal); later answers for the same feature are ignored and said. */
void ps_host_feature_probed(PsHostFeatureId id, int is_available, const char *sz_result);
/* The exit summary line, rebuilt at each publication so that the call report can print it from the
 * fault and watchdog handlers. */
const char *ps_host_features_summary(void);

/* ---- time ---- */
typedef enum {
    PS_HOST_CLOCK_MONOTONIC,
    PS_HOST_CLOCK_REALTIME,
    /* Realtime at the scheduler tick's resolution, cheaper to read. */
    PS_HOST_CLOCK_REALTIME_COARSE,
    PS_HOST_CLOCK_THREAD_CPU,
    PS_HOST_CLOCK_PROCESS_CPU,
} PsHostClock;

/* 0, or the host's errno. */
int ps_host_time_read(PsHostClock clock, struct timespec *p_out_time);
int ps_host_time_resolution(PsHostClock clock, struct timespec *p_out_time);
/* The monotonic and realtime clocks in nanoseconds, for callers that cannot fail: both clocks
 * always read on every host psport supports. */
uint64_t ps_host_time_monotonic_ns(void);
uint64_t ps_host_time_realtime_ns(void);
/* One sleep on the monotonic clock: 0 once it elapsed, EINTR when a signal cut it short (with what
 * was left in *p_out_remaining_ns when given), another errno on failure. Safe in signal handlers. */
int ps_host_time_sleep_ns(uint64_t ns, uint64_t *p_out_remaining_ns);
/* Sleeps until the monotonic clock reaches deadline_ns: 0, EINTR or another errno. */
int ps_host_time_sleep_until_ns(uint64_t deadline_ns);

/* ---- randomness ---- */
/* Fills the buffer from the host's cryptographic source: 0, or the host's errno. */
int ps_host_random_bytes(void *p_buffer, size_t cb_buffer);

/* ---- paths ---- */
/* The running executable's absolute path, NUL-terminated: its length, or -1 when the host cannot
 * say or it does not fit in cb_out. */
long ps_host_exe_path(char *sz_out, size_t cb_out);

/* ---- dynamic libraries ---- */
typedef struct PsHostLibrary PsHostLibrary;
/* Loads a host library with every symbol bound now and kept out of the process's own scope, so that
 * its dependencies cannot interpose on psport's. NULL on failure, with the host's reason in
 * sz_out_error when it is given. */
PsHostLibrary *ps_host_dl_open(const char *sz_path, char *sz_out_error, size_t cb_out_error);
void *ps_host_dl_sym(PsHostLibrary *p_library, const char *sz_symbol);
/* What a code address belongs to, for fault reports and samples: the loaded file (NULL when none
 * holds it) and the nearest exported symbol (NULL when none). The strings belong to the host loader
 * and stay valid while that file is loaded. */
typedef struct {
    const char *sz_file, *sz_symbol;
    uintptr_t va_base, va_symbol;
} PsHostCodeAddress;
/* Nonzero when the address lies in a loaded file; *p_out is zeroed otherwise. Usable in a fault
 * handler that is ending the process. */
int ps_host_dl_address(const void *p_code, PsHostCodeAddress *p_out);

/* ---- threads ---- */
/* The calling thread's id as the host's tools and thread signals name it. Safe in signal handlers. */
int32_t ps_host_thread_id(void);
/* Sleeps while the 32-bit word at p_word holds expected, until a wake on that address, the
 * absolute deadline on clock (MONOTONIC or REALTIME; NULL waits without one) or a signal. A
 * private wait pairs only with wakes from this process, which is cheaper. 0 when woken, EAGAIN
 * when the word did not hold expected, ETIMEDOUT, EINTR, or another errno. */
int ps_host_wait_on_address(const void *p_word, uint32_t expected, int is_private, PsHostClock clock,
                            const struct timespec *p_deadline);
/* Wakes up to c_waiters threads waiting on p_word: the number woken, or minus the host's errno. */
int ps_host_wake_address(const void *p_word, int c_waiters, int is_private);
/* Names the calling thread for the host's tools, ps_host_thread_name and ps_host_thread_find; the
 * host may keep only the first 15 characters. */
void ps_host_thread_set_name(const char *sz_name);
/* The calling thread's name as the host keeps it: 0, or the host's errno. */
int ps_host_thread_name(char *sz_out, size_t cb_out);
/* The id of a thread of this process whose name starts with sz_prefix, or 0 when none does. */
int32_t ps_host_thread_find(const char *sz_prefix);
/* Calls pfn_thread with each thread of this process, without allocating, so that the watchdog may
 * walk them from its signal handler: 0, or the host's errno when they cannot be listed. */
int ps_host_thread_each(void (*pfn_thread)(int32_t tid, void *p_context), void *p_context);
/* The calling thread's stack, its lowest address and size: 0, or the host's errno. */
int ps_host_thread_stack(uintptr_t *p_out_va_low, size_t *p_out_cb_stack);
/* Hardware threads this process may run on (its affinity, so that taskset can stand in for a
 * smaller machine), or 0 when the host cannot say. */
unsigned ps_host_thread_cpu_count(void);
/* The calling thread runs only on otherwise idle cores, or at the lowest priority when the host
 * has no such class: 0, or the host's errno when neither could be set. */
int ps_host_thread_make_background(void);

/* ---- files and folders ---- */
/* Calls pfn_entry with each name in the folder but "." and "..", without allocating, so that a
 * signal handler may list one (the watchdog lists /proc/self/task): 0, or the host's errno when the
 * folder cannot be read. */
int ps_host_list_dir(const char *sz_path, void (*pfn_entry)(const char *sz_name, void *p_context), void *p_context);

/* Host files are int descriptors (0-2 being the process's stdio), opened close-on-exec. Calls that
 * can fail return 0 or the host's errno; reads, writes and seeks a count or offset, or minus the
 * host's errno. Paths are host paths: the guest's mounts and FreeBSD's flags are runtime_file.c's. */
enum {
    PS_HOST_OPEN_READ = 0,
    PS_HOST_OPEN_WRITE = 1,
    PS_HOST_OPEN_READ_WRITE = 2,
    PS_HOST_OPEN_ACCESS_MASK = 3,
    PS_HOST_OPEN_NONBLOCK = 1 << 2,
    PS_HOST_OPEN_APPEND = 1 << 3,
    PS_HOST_OPEN_SYNC = 1 << 4,
    PS_HOST_OPEN_CREATE = 1 << 5,
    PS_HOST_OPEN_TRUNCATE = 1 << 6,
    PS_HOST_OPEN_EXCLUSIVE = 1 << 7,
    PS_HOST_OPEN_DIRECTORY = 1 << 8,
};
/* The traditional Unix mode word, whose type bits and permissions FreeBSD and Linux share; a host
 * without one fills it in. */
enum {
    PS_HOST_MODE_TYPE_MASK = 0170000,
    PS_HOST_MODE_DIRECTORY = 0040000,
    PS_HOST_MODE_REGULAR = 0100000,
    PS_HOST_MODE_LINK = 0120000,
};
#define PS_HOST_MODE_IS_DIRECTORY(mode) (((mode) & PS_HOST_MODE_TYPE_MASK) == PS_HOST_MODE_DIRECTORY)
#define PS_HOST_MODE_IS_REGULAR(mode) (((mode) & PS_HOST_MODE_TYPE_MASK) == PS_HOST_MODE_REGULAR)
#define PS_HOST_MODE_IS_LINK(mode) (((mode) & PS_HOST_MODE_TYPE_MASK) == PS_HOST_MODE_LINK)
typedef struct {
    uint64_t dev, ino;
    uint32_t mode, c_links, cb_block;
    int64_t cb_size, c_blocks;
    struct timespec atime, mtime, ctime;
} PsHostStat;
/* ps_host_fs_access's mode: any of these, or 0 to ask whether the path exists. */
enum { PS_HOST_ACCESS_READ = 4, PS_HOST_ACCESS_WRITE = 2, PS_HOST_ACCESS_EXECUTE = 1 };

/* A descriptor, or minus the host's errno; mode applies to a file the call creates. */
int ps_host_fs_open(const char *sz_path, unsigned flags, unsigned mode);
int ps_host_fs_close(int h_file);
int64_t ps_host_fs_read(int h_file, void *p_buffer, size_t cb_buffer);
int64_t ps_host_fs_pread(int h_file, void *p_buffer, size_t cb_buffer, int64_t off_file);
/* Safe in signal handlers, which write their reports to descriptors 1 and 2 with it. */
int64_t ps_host_fs_write(int h_file, const void *p_buffer, size_t cb_buffer);
int64_t ps_host_fs_pwrite(int h_file, const void *p_buffer, size_t cb_buffer, int64_t off_file);
/* whence as fseek's: SEEK_SET, SEEK_CUR, SEEK_END. */
int64_t ps_host_fs_seek(int h_file, int64_t off_file, int whence);
int ps_host_fs_sync(int h_file);
int ps_host_fs_truncate(int h_file, int64_t cb_file);
int ps_host_fs_truncate_path(const char *sz_path, int64_t cb_file);
/* What the path names, following links (ps_host_fs_stat_link: the link itself). */
int ps_host_fs_stat(const char *sz_path, PsHostStat *p_out);
int ps_host_fs_stat_link(const char *sz_path, PsHostStat *p_out);
int ps_host_fs_stat_file(int h_file, PsHostStat *p_out);
int ps_host_fs_access(const char *sz_path, int mode);
int ps_host_fs_make_dir(const char *sz_path, unsigned mode);
/* Creates a folder no one else has: sz_template ends in XXXXXX, which the call replaces in place. */
int ps_host_fs_make_unique_dir(char *sz_template);
int ps_host_fs_remove_dir(const char *sz_path);
int ps_host_fs_remove(const char *sz_path);
/* Replaces sz_to in one step when it exists, so that a reader sees the old file or the new one. */
int ps_host_fs_rename(const char *sz_from, const char *sz_to);
int ps_host_fs_symlink(const char *sz_target, const char *sz_link);
/* Bytes an unprivileged writer may still use on the file system holding the path. */
int ps_host_fs_free_space(const char *sz_path, uint64_t *p_out_cb_free);
/* The absolute path with every link and "." or ".." resolved; the path must exist. */
int ps_host_fs_real_path(const char *sz_path, char *sz_out, size_t cb_out);
/* Access then modification time; NULL sets both to now. */
int ps_host_fs_set_times(const char *sz_path, const struct timespec *p_times);
int ps_host_fs_set_times_file(int h_file, const struct timespec *p_times);
int ps_host_fs_set_mode(const char *sz_path, unsigned mode);
int ps_host_fs_set_mode_file(int h_file, unsigned mode);
/* The access mode, PS_HOST_OPEN_NONBLOCK and PS_HOST_OPEN_APPEND as the descriptor has them now;
 * ps_host_fs_set_status changes only those two. */
int ps_host_fs_status(int h_file, unsigned *p_out_flags);
int ps_host_fs_set_status(int h_file, unsigned flags);
/* 1 or 0, or minus the host's errno. */
int ps_host_fs_close_on_exec(int h_file);
int ps_host_fs_set_close_on_exec(int h_file, int is_set);

typedef struct PsHostDir PsHostDir;
typedef enum {
    PS_HOST_ENTRY_UNKNOWN,
    PS_HOST_ENTRY_REGULAR,
    PS_HOST_ENTRY_DIRECTORY,
    PS_HOST_ENTRY_LINK,
    PS_HOST_ENTRY_OTHER,
} PsHostEntryKind;
/* sz_name stays valid until the next call on the same listing. kind is what the folder records
 * without a lookup: a link stays a link, and some file systems answer UNKNOWN (ps_host_dir_stat
 * says more). */
typedef struct {
    const char *sz_name;
    PsHostEntryKind kind;
} PsHostDirEntry;
/* NULL with errno set when the folder cannot be read. */
PsHostDir *ps_host_dir_open(const char *sz_path);
/* 1 with the next entry ("." and ".." included where the host lists them), 0 at the end. */
int ps_host_dir_next(PsHostDir *p_dir, PsHostDirEntry *p_out);
/* An entry of the listing, following links. */
int ps_host_dir_stat(PsHostDir *p_dir, const char *sz_name, PsHostStat *p_out);
void ps_host_dir_close(PsHostDir *p_dir);

/* ---- the address space ---- */
typedef struct {
    uintptr_t va_begin, va_end;
    int is_readable, is_writable, is_executable;
    /* The host's own line for the mapping (offsets, file), for logs. */
    const char *sz_description;
} PsHostMapping;
/* Calls pfn_mapping with each mapping in address order until it returns nonzero, without
 * allocating, so that a fault handler may look one up: 0, or the host's errno when the list
 * cannot be read. */
int ps_host_list_mappings(int (*pfn_mapping)(const PsHostMapping *p_mapping, void *p_context), void *p_context);

/* ---- sockets ---- */
/* Sockets are int descriptors like files (runtime_file.c keeps both in the guest's table) and use
 * the BSD socket calls, except what follows. ps_host_net_init comes before the first socket and
 * may be called again at no cost: 0, or the host's errno. */
int ps_host_net_init(void);
int ps_host_net_close(int h_socket);
/* The last failed socket call's error on this thread, as an errno value. */
int ps_host_net_error(void);
int ps_host_net_set_nonblocking(int h_socket, int is_nonblocking);
/* Bytes waiting to be read and not yet sent; each is left as it was when the host cannot say. */
void ps_host_net_queued(int h_socket, int32_t *p_out_cb_receive, int32_t *p_out_cb_send);
/* RFC 793's states in the order FreeBSD numbers them (TCPS_*), which the guest reads. */
typedef enum {
    PS_HOST_TCP_CLOSED,
    PS_HOST_TCP_LISTEN,
    PS_HOST_TCP_SYN_SENT,
    PS_HOST_TCP_SYN_RECEIVED,
    PS_HOST_TCP_ESTABLISHED,
    PS_HOST_TCP_CLOSE_WAIT,
    PS_HOST_TCP_FIN_WAIT_1,
    PS_HOST_TCP_CLOSING,
    PS_HOST_TCP_LAST_ACK,
    PS_HOST_TCP_FIN_WAIT_2,
    PS_HOST_TCP_TIME_WAIT,
} PsHostTcpState;
/* A stream socket's state; CLOSED when the host cannot say. */
PsHostTcpState ps_host_net_tcp_state(int h_socket);

/* ---- processes ---- */
/* Closes descriptors h_first to h_last (-1: every one from h_first on), or with is_on_exec only marks
 * them close-on-exec, so that a failed exec leaves them working: 0, or the host's errno. Safe
 * between fork and exec. */
int ps_host_close_from(int h_first, int h_last, int is_on_exec);
/* Replaces the process with sz_file (looked up in PATH when it has no slash): returns only when that
 * fails, with the host's errno (errno holds it too). */
int ps_host_exec(const char *sz_file, char *const *pp_argv);
/* Replaces the process with a new run of psport's own executable, the same file even when its path
 * changed since: returns only when that fails, with the host's errno (errno holds it too). */
int ps_host_exec_self(char *const *pp_argv);

/* ---- faults and thread interrupts ---- */
/* What a fault or an interrupt handler is handed: the interrupted thread's registers, which it may
 * change for when the thread resumes, and what faulted. Valid only during the handler. Handlers run
 * in signal context: only what is safe in a signal handler, and the thread may hold any lock. */
typedef struct PsHostFaultContext PsHostFaultContext;
typedef enum {
    PS_HOST_FAULT_ACCESS,
    PS_HOST_FAULT_BUS,
    PS_HOST_FAULT_ILLEGAL,
    PS_HOST_FAULT_ARITHMETIC,
    /* An int3: the instruction pointer is past it. */
    PS_HOST_FAULT_BREAKPOINT,
    PS_HOST_FAULT_KIND_COUNT,
} PsHostFaultKind;
typedef enum {
    PS_HOST_FAULT_UNKNOWN,
    PS_HOST_FAULT_READ,
    PS_HOST_FAULT_WRITE,
    PS_HOST_FAULT_EXECUTE,
} PsHostFaultAccess;
typedef enum {
    PS_HOST_REG_RAX,
    PS_HOST_REG_RBX,
    PS_HOST_REG_RCX,
    PS_HOST_REG_RDX,
    PS_HOST_REG_RSI,
    PS_HOST_REG_RDI,
    PS_HOST_REG_RBP,
    PS_HOST_REG_RSP,
    PS_HOST_REG_R8,
    PS_HOST_REG_R9,
    PS_HOST_REG_R10,
    PS_HOST_REG_R11,
    PS_HOST_REG_R12,
    PS_HOST_REG_R13,
    PS_HOST_REG_R14,
    PS_HOST_REG_R15,
    PS_HOST_REG_RIP,
    PS_HOST_REG_RFLAGS,
    PS_HOST_REG_COUNT,
} PsHostRegister;
/* What the CPU recorded beyond the general registers, for the guest's own exception context. */
typedef struct {
    uint32_t trap_number;
    uint64_t error_code;
    uint16_t code_selector;
    /* The 512-byte fxsave image of the FPU and SSE state, NULL when the host gives none. */
    const void *p_fxsave;
} PsHostFaultCpu;

PsHostFaultKind ps_host_fault_kind(const PsHostFaultContext *p_context);
/* The host's signal number, for reports and exit codes. */
int ps_host_fault_signal(const PsHostFaultContext *p_context);
/* The address an access, bus or arithmetic fault names. */
void *ps_host_fault_address(const PsHostFaultContext *p_context);
/* What the faulting access did, for access faults; UNKNOWN for the rest. */
PsHostFaultAccess ps_host_fault_access(const PsHostFaultContext *p_context);
uint64_t ps_host_fault_register(const PsHostFaultContext *p_context, PsHostRegister reg);
void ps_host_fault_set_register(PsHostFaultContext *p_context, PsHostRegister reg, uint64_t value);
void ps_host_fault_cpu(const PsHostFaultContext *p_context, PsHostFaultCpu *p_out);
/* Leaves the handler for a recovery point a speculative reader set with sigsetjmp (its second
 * argument 0), with the signal mask the interrupted code had. */
__attribute__((noreturn)) void ps_host_fault_recover(const PsHostFaultContext *p_context, sigjmp_buf *p_point);

/* Nonzero when the handler took the fault: the thread resumes with the registers it left. Zero
 * passes it on to the handler installed before it, and past the first the host's default action
 * ends the process. */
typedef int (*PsHostFaultHandler)(PsHostFaultContext *p_context);
enum {
    /* The same kind may fault again while the handler runs (a breakpoint the handler steps over). */
    PS_HOST_FAULT_NESTED = 1,
    /* The guest exception interrupt waits while the handler runs. */
    PS_HOST_FAULT_HOLD_GUEST_EXCEPTION = 2,
};
/* Adds pfn_handler in front of the handlers of kind (at most 4 each): 0, or the host's errno. Each
 * call's flags add to the earlier ones. */
int ps_host_fault_install(PsHostFaultKind kind, PsHostFaultHandler pfn_handler, unsigned flags);

/* Interrupts: a thread of this process stopped where it runs to call a handler with its registers. */
typedef enum {
    /* sceKernelRaiseException's SIGUSR1, delivered to the guest's handler. */
    PS_HOST_INTERRUPT_GUEST_EXCEPTION,
    /* The watchdog's request to a thread to print its stack. */
    PS_HOST_INTERRUPT_THREAD_DUMP,
    /* The watchdog itself, when its timeout runs out (ps_host_watchdog_arm). */
    PS_HOST_INTERRUPT_WATCHDOG,
    /* The wait profile's sample of one thread. */
    PS_HOST_INTERRUPT_SAMPLE,
    PS_HOST_INTERRUPT_COUNT,
} PsHostInterrupt;
typedef void (*PsHostInterruptHandler)(PsHostFaultContext *p_context);
enum {
    /* Calls the interrupt cut short (file reads, futex waits) start again by themselves. */
    PS_HOST_INTERRUPT_RESTART = 1,
};
/* Replaces the interrupt's handler: 0, or the host's errno. */
int ps_host_interrupt_install(PsHostInterrupt interrupt, PsHostInterruptHandler pfn_handler, unsigned flags);
/* Interrupts one thread of this process: 0, or the host's errno. Safe in signal handlers. */
int ps_host_interrupt_thread(int32_t tid, PsHostInterrupt interrupt);
/* The host signal behind an interrupt, for the code that masks it around a thread's start and in
 * sigreturn's frame. */
int ps_host_interrupt_signal(PsHostInterrupt interrupt);
/* The watchdog interrupt arrives once after seconds (0 disarms it), on whichever thread the host
 * picks. */
void ps_host_watchdog_arm(unsigned seconds);
/* pfn_report runs once when the process is asked to end (a terminal's ^C, a launcher's stop) and
 * the host's default action follows; a request someone else already handles is left to them. */
void ps_host_on_termination(void (*pfn_report)(void));

/* Resumes the calling thread with these registers (RSP, RIP and RFLAGS included) in one step, so
 * that nothing is written below the restored stack pointer. p_fpu is an 832-byte xsave image
 * (legacy area, header, the YMM upper halves when its header says so) or NULL to reset the FPU;
 * exception_signal is masked when is_exception_masked, else unmasked. */
__attribute__((noreturn)) void ps_host_resume(const uint64_t p_registers[PS_HOST_REG_COUNT], const void *p_fpu,
                                              int exception_signal, int is_exception_masked);

/* Copies from memory of this process without faulting: a page without read access ends the copy.
 * The bytes copied, or minus the host's errno when none could be. Safe in signal handlers. */
int64_t ps_host_read_memory(void *p_out, const void *p_from, size_t cb_copy);
/* The interrupted code's call chain from the host's unwind information, starting at the faulting
 * instruction (at the handler's own frames when the unwinder cannot see past them): how many
 * return addresses were stored. For a fault report: not promised safe in a signal handler. */
int ps_host_fault_backtrace(const PsHostFaultContext *p_context, void **pp_frames, int c_frames);

#ifdef __cplusplus
}
#endif
#endif
