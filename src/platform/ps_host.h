/* The host platform layer (decision #540): what psport asks of the operating system, behind one
 * small C ABI with an implementation per OS in src/platform/<os>/ (src/platform/common/ when it is
 * the same on every OS that builds it), so that the runtime and the loader name what they need
 * instead of a Linux call. Included from C and C++ (libpsport-gpu.so
 * reaches these through -rdynamic). Linux is implemented, Windows too (src/platform/win32/, not run
 * with a game yet); host_features.inc says what each OS has. */
#ifndef PS_HOST_H
#define PS_HOST_H
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "ps_host_audio.h"
#include "ps_host_gpu_memory.h"
#include "ps_host_input.h"
#include "ps_host_window.h"
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

/* psport --list-features: every entry as JSON on stdout, with this host's state; a probe that found
 * the feature missing makes it UNAVAILABLE with what it found as the reason. */
void ps_host_features_list(void);
/* Once at launch, before the game's code runs: the "Host features:" line and the exit summary. */
void ps_host_features_log(void);
/* A FEATURE_PROBED entry's run-time answer, from the code that probes it (the GPU library once the
 * Vulkan device is known, psport_gpu_probe_host_features): logged and kept for the exit summary.
 * sz_result must outlive the process (a string literal or static storage); later answers for the
 * same feature are ignored and said. */
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
/* The host's time zone rule in force at that UTC second, east of UTC in seconds: its standard
 * offset and the daylight saving on top of it (0 outside it, negative in zones whose standard time
 * is their summer one), so that local time is UTC plus both. 0, or the host's errno. */
int ps_host_time_zone_at(int64_t utc_sec, int32_t *p_out_standard_sec, int32_t *p_out_dst_sec);
/* The UTC second as calendar fields in the host's local time zone, or in UTC: 0, or the host's errno. */
int ps_host_time_local_fields(int64_t utc_sec, struct tm *p_out);
int ps_host_time_utc_fields(int64_t utc_sec, struct tm *p_out);
/* The UTC second the calendar fields name (tm_wday and tm_yday ignored, the others normalised as
 * timegm does): 0, or the host's errno (EOVERFLOW when it is not representable). */
int ps_host_time_utc_seconds(const struct tm *p_fields, int64_t *p_out_utc_sec);

/* ---- randomness ---- */
/* Fills the buffer from the host's cryptographic source: 0, or the host's errno. */
int ps_host_random_bytes(void *p_buffer, size_t cb_buffer);

/* ---- paths ---- */
/* The running executable's absolute path, NUL-terminated, with '/' between folders: its length, or
 * -1 when the host cannot say or it does not fit in cb_out. */
long ps_host_exe_path(char *sz_out, size_t cb_out);
/* The folder shadPS4 keeps its user data in when it is not run portable (Windows: %APPDATA%\\shadPS4),
 * NUL-terminated: its length, or -1 when the host has none or it does not fit in cb_out (Linux:
 * there is only <working folder>/user). */
long ps_host_shadps4_user_dir(char *sz_out, size_t cb_out);
/* This process's command line as UTF-8, arguments joined by single spaces and NUL-terminated (no
 * quoting: the arguments' own spaces stay): its length, or -1 when the host cannot say or it does
 * not fit in cb_out. Read-only view for options that code outside main's parser needs (--user-id). */
long ps_host_command_line(char *sz_out, size_t cb_out);

/* ---- dynamic libraries ---- */
typedef struct PsHostLibrary PsHostLibrary;
/* Loads a host library with every symbol bound now and kept out of the process's own scope, so that
 * its dependencies cannot interpose on psport's. NULL on failure, with the host's reason in
 * sz_out_error when it is given. */
PsHostLibrary *ps_host_dl_open(const char *sz_path, char *sz_out_error, size_t cb_out_error);
void *ps_host_dl_sym(PsHostLibrary *p_library, const char *sz_symbol);
/* A library the process already holds, without loading it (a tool that hooks the process from its
 * start, which a later load would miss): sz_name is its base name without the host's prefix and
 * suffix ("renderdoc" for librenderdoc.so and renderdoc.dll). NULL when it is not loaded. */
PsHostLibrary *ps_host_dl_find_loaded(const char *sz_name);
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
/* CPU time the process or the calling thread spent in user mode and in the kernel: 0, or the
 * host's errno. */
int ps_host_thread_cpu_times(int is_whole_process, struct timespec *p_out_user, struct timespec *p_out_system);
/* Hardware threads this process may run on (its affinity, so that taskset can stand in for a
 * smaller machine), or 0 when the host cannot say. */
unsigned ps_host_thread_cpu_count(void);
/* The calling thread gives up the rest of its time slice to another ready thread. */
void ps_host_thread_yield(void);
/* The calling thread runs only on otherwise idle cores, or at the lowest priority when the host
 * has no such class: 0, or the host's errno when neither could be set. */
int ps_host_thread_make_background(void);

/* ---- guest TLS ---- */
/* The guest's thread pointer, the TCB its code reaches through a segment register: GS, since
 * the host's own TLS keeps FS. Sets the calling thread's: 0, or the host's errno. */
int ps_host_tls_set_base(void *p_base);
/* The calling thread's guest thread pointer: 0, or the host's errno. Safe in signal handlers. */
int ps_host_tls_base(uintptr_t *p_out_va_base);
/* Reserves what ps_host_tls_set_base needs on every thread: once at launch, before the host loads
 * other libraries, which could take it first. 0, or the host's errno. */
int ps_host_tls_init(void);
/* The displacement the loader writes into the guest's thread pointer loads (`mov r64, fs:[0]`,
 * rewritten to GS) so that they read the TCB pointer: 0 where GS's base is the TCB itself, whose
 * first word points to itself. Valid once ps_host_tls_init succeeded. */
int32_t ps_host_tls_tcb_offset(void);

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
/* The same calls on a stdio stream's file, whose fileno is the C runtime's descriptor and need not
 * be one of psport's. */
int ps_host_fs_stat_stream(FILE *p_stream, PsHostStat *p_out);
int ps_host_fs_sync_stream(FILE *p_stream);
int ps_host_fs_truncate_stream(FILE *p_stream, int64_t cb_file);
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
/* Writes the file's data back, without the metadata a later read does not need: 0, or the host's
 * errno. */
int ps_host_fs_sync_data(int h_file);
/* A pipe whose p_out_h_files[0] reads what p_out_h_files[1] writes, both close-on-exec: 0, or the
 * host's errno. */
int ps_host_fs_pipe(int p_out_h_files[2]);
/* flock's whole-file advisory locks: SHARED or EXCLUSIVE (with NONBLOCK, EWOULDBLOCK instead of
 * waiting), or UNLOCK. 0, or the host's errno. */
enum { PS_HOST_LOCK_SHARED = 1, PS_HOST_LOCK_EXCLUSIVE = 2, PS_HOST_LOCK_NONBLOCK = 4, PS_HOST_LOCK_UNLOCK = 8 };
int ps_host_fs_lock(int h_file, unsigned operation);
/* What a pipe, socket or other stream can do now, without waiting: the events asked for (IN, OUT)
 * that hold, with HANGUP and ERROR whenever they do, and the bytes a read gets (IN) or a write has
 * room for (OUT, 0 when the host cannot say). 0, or the host's errno (EBADF: not open). */
enum { PS_HOST_POLL_IN = 1, PS_HOST_POLL_OUT = 2, PS_HOST_POLL_HANGUP = 4, PS_HOST_POLL_ERROR = 8 };
int ps_host_fs_poll(int h_file, unsigned events, unsigned *p_out_events, int64_t *p_out_cb_ready);

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

/* A stdio stream on a host path, sz_mode as fopen's: NULL with the host's errno in errno. */
FILE *ps_host_fs_open_stream(const char *sz_path, const char *sz_mode);

/* What ps_host_fs_walk visits: sz_path is the root as given or a path below it joined with '/',
 * valid until the callback returns. kind comes from the entry itself, links never followed, and is
 * UNKNOWN (stat not filled) when the entry vanished or could not be examined; is_unlisted marks a
 * folder whose entries could not be read, visited without them. */
typedef struct {
    const char *sz_path;
    PsHostEntryKind kind;
    int is_unlisted;
    int depth;
    PsHostStat stat;
} PsHostWalkEntry;
/* POST_ORDER visits a folder after its entries, so that a callback may remove them. */
enum { PS_HOST_WALK_POST_ORDER = 1 };
/* Calls pfn_entry with the root and everything below it, each folder's entries in the folder's own
 * order. pfn_entry returns 0 to go on or an errno to stop the walk with it. 0, that errno, or the
 * host's errno when the root or a folder could not be read (ENOENT: no root). */
int ps_host_fs_walk(const char *sz_root, unsigned flags, int (*pfn_entry)(const PsHostWalkEntry *p_entry, void *p_context),
                    void *p_context);
/* Removes the tree at sz_path, links removed rather than followed, the root too unless is_root_kept;
 * stops at the first entry that cannot be removed. 0, or the host's errno (ENOENT: no root). */
int ps_host_fs_remove_tree(const char *sz_path, int is_root_kept);

/* ---- the address space ---- */
/* What a mapping allows. Write and execute access may imply read on some hosts. */
enum { PS_HOST_VM_NONE = 0, PS_HOST_VM_READ = 1, PS_HOST_VM_WRITE = 2, PS_HOST_VM_EXECUTE = 4 };
/* Where a mapping goes: with neither REPLACE nor EXACT, p_at is a hint (NULL: anywhere). */
enum {
    /* At p_at, replacing whatever the process had mapped there. */
    PS_HOST_VM_REPLACE = 1,
    /* At p_at, or nowhere (EEXIST) when something is mapped there. */
    PS_HOST_VM_EXACT = 2,
    /* Address space only: pages are committed when first touched and not charged up front, so that
     * tables over the guest's whole 40-bit range cost what is used. */
    PS_HOST_VM_SPARSE = 4,
};
size_t ps_host_vm_page_size(void);
/* The range the guest's REPLACE and EXACT mappings go to, taken before the host can place anything
 * there: once at launch, before the first of them. Windows reserves it as placeholders, since a
 * fixed mapping can replace nothing else there; Linux needs nothing. 0, or the host's errno. */
int ps_host_vm_reserve_arena(void *p_begin, size_t cb_range);
/* Private zeroed memory: its address, or NULL with the host's errno in errno. */
void *ps_host_vm_map(void *p_at, size_t cb_range, unsigned access, unsigned flags);
/* A view of a section or file from off_file: every view of the same bytes aliases them and writes
 * reach the file. NULL with the host's errno in errno (EACCES: the file does not allow access). */
void *ps_host_vm_map_shared(void *p_at, size_t cb_range, unsigned access, unsigned flags, int h_file,
                            uint64_t off_file);
/* 0, or the host's errno. */
int ps_host_vm_unmap(void *p_range, size_t cb_range);
/* Changes the access of mapped pages: 0, or the host's errno. Safe in signal handlers, where the
 * write traps lift their protection. */
int ps_host_vm_protect(void *p_range, size_t cb_range, unsigned access);
enum {
    /* Starts writing the range back without waiting for it. */
    PS_HOST_VM_FLUSH_ASYNC = 1,
    /* Later reads see what other views and the file hold. */
    PS_HOST_VM_FLUSH_INVALIDATE = 2,
};
/* Writes a shared view's changes back to its file: 0, or the host's errno. */
int ps_host_vm_flush(void *p_range, size_t cb_range, unsigned flags);
/* Memory that is not a file, which ps_host_vm_map_shared views at several addresses (the guest's
 * direct memory): cb_section zeroed bytes, the descriptor in *p_out_h_section, closed with
 * ps_host_fs_close. 0, or the host's errno with nothing left open. */
int ps_host_vm_section_create(const char *sz_name, uint64_t cb_section, int *p_out_h_section);
/* The bytes read zero again and their storage goes back to the host: 0, or the host's errno. */
int ps_host_vm_section_discard(int h_section, uint64_t off_section, uint64_t cb_range);
/* malloc hands out addresses below 1 TiB from now on, so that host objects the guest holds (thread
 * handles, TLS blocks, its argv) fit the 40-bit pointers PS4 code packs. Once at launch, before the
 * first allocation the guest may see. */
void ps_host_vm_keep_heap_low(void);

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
 * the BSD socket calls of ps_host_sockets.h, except what follows. ps_host_net_init comes before the
 * first socket and may be called again at no cost: 0, or the host's errno. */
int ps_host_net_init(void);
/* family, type and protocol as the host's headers number them; close-on-exec, and non-blocking when
 * asked: the descriptor, or minus the host's errno. */
int ps_host_net_socket(int family, int type, int protocol, int is_nonblocking);
/* The connection's socket, close-on-exec, and its peer's address: its descriptor, or minus the
 * host's errno. */
int ps_host_net_accept(int h_socket, void *p_out_address, uint32_t *p_cb_address);
/* A connected pair of local (AF_UNIX) sockets, close-on-exec, non-blocking when asked: 0, or the
 * host's errno (EOPNOTSUPP where the host has no socket pairs). */
int ps_host_net_socketpair(int type, int protocol, int is_nonblocking, int p_out_h_sockets[2]);
/* 0, or the host's errno: EINPROGRESS while a non-blocking socket still connects. */
int ps_host_net_connect(int h_socket, const void *p_address, uint32_t cb_address);
/* struct iovec's layout, which FreeBSD's msghdr hands over too. */
typedef struct {
    void *p_base;
    size_t cb_length;
} PsHostIoVector;
/* What a send or receive asks; a broken connection never raises a signal. */
enum {
    PS_HOST_NET_OUT_OF_BAND = 1,
    PS_HOST_NET_PEEK = 2,
    PS_HOST_NET_WAIT_ALL = 4,
    PS_HOST_NET_DONT_WAIT = 8,
};
/* What a receive found, beyond the bytes. */
enum {
    PS_HOST_NET_TRUNCATED = 1,
    PS_HOST_NET_CONTROL_TRUNCATED = 2,
    PS_HOST_NET_GOT_OUT_OF_BAND = 4,
};
/* To p_to when not NULL: the bytes sent, or minus the host's errno. */
int64_t ps_host_net_send(int h_socket, const PsHostIoVector *p_vectors, int c_vectors, const void *p_to, uint32_t cb_to,
                         unsigned flags);
/* The sender's address in p_out_from, *p_cb_from its room then its size (0 when it has none or
 * p_out_from is NULL): the bytes received, or minus the host's errno. */
int64_t ps_host_net_receive(int h_socket, const PsHostIoVector *p_vectors, int c_vectors, void *p_out_from,
                            uint32_t *p_cb_from, unsigned flags, unsigned *p_out_flags);
enum {
    PS_HOST_NET_POLL_IN = 1,
    PS_HOST_NET_POLL_OUT = 2,
    PS_HOST_NET_POLL_PRIORITY = 4,
    /* Only in revents. */
    PS_HOST_NET_POLL_ERROR = 8,
    PS_HOST_NET_POLL_HANG_UP = 16,
    PS_HOST_NET_POLL_INVALID = 32,
};
typedef struct {
    int h_socket;
    unsigned events, revents;
} PsHostPollEntry;
/* Waits until one entry is ready or timeout_ns passes (negative: no timeout): how many are, or minus
 * the host's errno. */
int ps_host_net_poll(PsHostPollEntry *p_entries, int c_entries, int64_t timeout_ns);
/* ps_host_fs_poll for a socket, whose descriptor is not a file's on every host. */
int ps_host_net_ready(int h_socket, unsigned events, unsigned *p_out_events, int64_t *p_out_cb_ready);
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
/* A helper process removes the folder once this process has ended, whichever way it ends (a fault,
 * a kill, an exec into something else): 0, or the host's errno (ENOSYS where it has no such helper),
 * the folder left in place then. */
int ps_host_remove_tree_at_exit(const char *sz_path);
/* pfn_exit runs at exit() with its status, after the handlers registered later (atexit order). */
int ps_host_on_exit(void (*pfn_exit)(int status));
/* Ends the process with status at once, from a fault handler too: no atexit handlers, no stdio
 * flush and no library teardown, since other threads may hold any lock when it is called. */
__attribute__((noreturn)) void ps_host_exit_at_once(int status);
/* Descriptors 1 and 2 go through pipes to a helper process, which writes each chunk on to where they
 * pointed before and hands it to pfn_chunk (is_error: it came through 2), then calls pfn_end once
 * both pipes are closed (this process and whatever inherited them ended, or ps_host_output_untee)
 * and exits. The helper ignores the termination signals so that it outlives this process to drain
 * them, and the callbacks run in a forked child: only what is safe in a signal handler. Called
 * before any thread starts: 0, or the host's errno (nothing changed then). */
int ps_host_output_tee(void (*pfn_chunk)(int is_error, const char *p_data, size_t cb_data), void (*pfn_end)(void));
/* Points 1 and 2 back where they pointed before ps_host_output_tee, so that its helper sees the
 * pipes closed and ends; nothing without one. */
void ps_host_output_untee(void);
/* Points h_target at what h_file names (h_target < 0: a new close-on-exec descriptor): the
 * descriptor, or minus the host's errno. */
int ps_host_fs_duplicate(int h_file, int h_target);
/* The machine's network name, NUL-terminated: 0, or the host's errno. */
int ps_host_host_name(char *sz_out, size_t cb_out);
/* Sets an environment variable of this process, leaving one already set when !is_overwrite: 0, or
 * the host's errno. Not thread-safe against readers of the environment, as setenv. */
int ps_host_env_set(const char *sz_name, const char *sz_value, int is_overwrite);
/* The value of an environment variable of this process, or NULL when unset; valid until the next
 * ps_host_env_set. */
const char *ps_host_env_get(const char *sz_name);
/* This process's id, as the host's tools name it. */
int32_t ps_host_process_id(void);

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
/* PS_HOST_RECOVERY_SET marks a recovery point (a macro, since it must run in the reader's own
 * frame): 0 when set, nonzero when a fault or PS_HOST_RECOVERY_JUMP comes back to it. */
#ifdef _WIN32
/* Hand-written (src/platform/win32/host_fault.c), since the C runtime's longjmp unwinds through SEH,
 * which stops at guest frames that have no unwind data. */
typedef uint64_t PsHostRecoveryPoint[32];
__attribute__((returns_twice)) int ps_host_recovery_set(PsHostRecoveryPoint point);
__attribute__((noreturn)) void ps_host_recovery_jump(PsHostRecoveryPoint point, int value);
#define PS_HOST_RECOVERY_SET(point) ps_host_recovery_set(point)
#define PS_HOST_RECOVERY_JUMP(point) ps_host_recovery_jump(point, 1)
#else
typedef sigjmp_buf PsHostRecoveryPoint;
#define PS_HOST_RECOVERY_SET(point) sigsetjmp(point, 0)
#define PS_HOST_RECOVERY_JUMP(point) siglongjmp(point, 1)
#endif
/* Leaves the handler for a recovery point a speculative reader set with PS_HOST_RECOVERY_SET,
 * with the signal mask the interrupted code had. */
__attribute__((noreturn)) void ps_host_fault_recover(const PsHostFaultContext *p_context,
                                                     PsHostRecoveryPoint *p_point);

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
/* Which interrupts the calling thread holds back, as a signal mask (Linux's sigset_t), so that a
 * new thread holds them until its TCB is in place. */
typedef struct {
    uint64_t opaque[16];
} PsHostInterruptMask;
/* Holds every interrupt on the calling thread, faults excepted (with is_holding_all, so that one
 * taken meanwhile still reaches its handler), or none: the mask it had in *p_out_previous when not
 * NULL. A thread created meanwhile starts with the same mask where the host passes it on. */
void ps_host_interrupts_hold(int is_holding_all, PsHostInterruptMask *p_out_previous);
/* Puts back a mask ps_host_interrupts_hold saved; what arrived while held is delivered once open. */
void ps_host_interrupts_restore(const PsHostInterruptMask *p_mask);
/* The host signal behind an interrupt, for the code that masks it around a thread's start and in
 * sigreturn's frame. */
int ps_host_interrupt_signal(PsHostInterrupt interrupt);
/* The watchdog interrupt arrives once after seconds (0 disarms it), on whichever thread the host
 * picks. */
void ps_host_watchdog_arm(unsigned seconds);
/* pfn_report runs once, with the signal, when the process is asked to end (a terminal's ^C, a
 * launcher's stop) and the host's default action follows; a request someone else already handles is left to them. */
void ps_host_on_termination(void (*pfn_report)(int sig));

/* Resumes the calling thread with these registers (RSP, RIP and RFLAGS included) in one step, so
 * that nothing is written below the restored stack pointer. p_fpu is an 832-byte xsave image
 * (legacy area, header, the YMM upper halves when its header says so) or NULL to reset the FPU;
 * exception_signal is masked when is_exception_masked, else unmasked. */
__attribute__((noreturn)) void ps_host_resume(const uint64_t p_registers[PS_HOST_REG_COUNT], const void *p_fpu,
                                              int exception_signal, int is_exception_masked);

/* Copies from memory of this process without faulting: a page without read access ends the copy.
 * The bytes copied, or minus the host's errno when none could be. Safe in signal handlers. */
int64_t ps_host_read_memory(void *p_out, const void *p_from, size_t cb_copy);
/* Copies into memory of this process without faulting: a page without write access ends the copy.
 * The bytes copied, or minus the host's errno when none could be. */
int64_t ps_host_write_memory(void *p_to, const void *p_from, size_t cb_copy);
/* The calling thread's call chain, starting at the caller: how many return addresses were stored. */
int ps_host_backtrace(void **pp_frames, int c_frames);
/* The interrupted code's call chain from the host's unwind information, starting at the faulting
 * instruction (at the handler's own frames when the unwinder cannot see past them): how many
 * return addresses were stored. For a fault report: not promised safe in a signal handler. */
int ps_host_fault_backtrace(const PsHostFaultContext *p_context, void **pp_frames, int c_frames);

#ifdef __cplusplus
}
#endif
#endif
