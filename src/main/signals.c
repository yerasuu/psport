// SPDX-License-Identifier: GPL-2.0-or-later
/* Fault, watchdog and termination signal handlers: what the log shows when the process stops. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"

/* Fault and watchdog reports go to stderr and, in debug mode, to faults.txt too (opened by the
 * first report, from the handler). */
static int s_h_faults = -1;
static void write_report(const char *p_text, size_t cb_text) {
    (void)ps_host_fs_write(2, p_text, cb_text);
    int h_faults = __atomic_load_n(&s_h_faults, __ATOMIC_ACQUIRE);
    if (h_faults < 0) {
        const int h_opened = runtime_debug_open("faults.txt");
        if (h_opened < 0) return;
        int expected = -1;
        if (__atomic_compare_exchange_n(&s_h_faults, &expected, h_opened, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) h_faults = h_opened;
        else { ps_host_fs_close(h_opened); h_faults = expected; }
    }
    (void)ps_host_fs_write(h_faults, p_text, cb_text);
}
static void dump_frames(const PsHostFaultContext *p_context);
/* The exit summary a closed window writes (runtime_report: guest threads, locks, memory, calls),
 * also from the watchdog and termination handlers: each report takes its lock with a bound, so one
 * held by the thread a handler interrupted is skipped and named instead of hanging the exit. */
static void report_exit(const char *sz_how, int exit_code) {
    runtime_report();
    runtime_debug_finish(sz_how, exit_code);
}
typedef struct { uintptr_t va; int is_found; } MappingSearch;
static int write_mapping_of(const PsHostMapping *p_mapping, void *p_context) {
    MappingSearch *p_search = p_context;
    if (p_search->va < p_mapping->va_begin || p_search->va >= p_mapping->va_end) return 0;
    write_report(p_mapping->sz_description, strlen(p_mapping->sz_description));
    write_report("\n", 1);
    p_search->is_found = 1;
    return 1;
}
/* PS_FAULT_DUMP: guest memory a fault report shows, parsed once at startup so that a malformed
 * value is reported while the launch can still be fixed, not when the process is already dying. */
enum { FAULT_DUMP_MAX_RANGES = 16, FAULT_DUMP_MAX_BYTES = 64 * 1024 };
typedef struct {
    uintptr_t va_address;
    int64_t off_from_pointer;
    size_t cb_dump, cb_pointer;
} FaultDumpRange;
static FaultDumpRange s_fault_dumps[FAULT_DUMP_MAX_RANGES];
static size_t s_c_fault_dumps;
static int parse_number(const char **psz_text, uint64_t *p_out_value) {
    char *sz_end;
    errno = 0;
    *p_out_value = strtoull(*psz_text, &sz_end, 0);
    if (sz_end == *psz_text || errno || **psz_text == '-' || **psz_text == '+') return 0;
    *psz_text = sz_end;
    return 1;
}
static const char *parse_fault_dump(const char *sz_item, FaultDumpRange *p_out_range) {
    uint64_t value;
    *p_out_range = (FaultDumpRange){0};
    const int is_indirect = *sz_item == '*';
    if (is_indirect) ++sz_item;
    if (!parse_number(&sz_item, &value)) return "expected an address";
    p_out_range->va_address = value;
    if (is_indirect) {
        p_out_range->cb_pointer = 8;
        if (*sz_item == '/') {
            ++sz_item;
            if (!parse_number(&sz_item, &value) || (value != 4 && value != 8)) return "pointer width must be /4 or /8";
            p_out_range->cb_pointer = value;
        }
        while (*sz_item == '+' || *sz_item == '-') {
            const int is_negative = *sz_item++ == '-';
            if (!parse_number(&sz_item, &value) || value > INT64_MAX) return "expected an offset after + or -";
            p_out_range->off_from_pointer += is_negative ? -(int64_t)value : (int64_t)value;
        }
    }
    if (*sz_item++ != ':') return "expected :<length>";
    if (!parse_number(&sz_item, &value) || !value) return "expected a non-zero length";
    if (*sz_item) return "unexpected text after the length";
    if (value > FAULT_DUMP_MAX_BYTES) return "length above 64 KiB";
    p_out_range->cb_dump = value;
    return NULL;
}
static void configure_fault_dumps(void) {
    const char *sz_value = runtime_env("PS_FAULT_DUMP", "");
    size_t cb_total = 0;
    while (*sz_value) {
        const size_t cb_item = strcspn(sz_value, ",");
        char sz_item[128];
        FaultDumpRange range;
        const char *sz_problem = NULL;
        snprintf(sz_item, sizeof(sz_item), "%.*s", (int)cb_item, sz_value);
        if (cb_item >= sizeof(sz_item)) sz_problem = "entry too long";
        else if (s_c_fault_dumps == FAULT_DUMP_MAX_RANGES) sz_problem = "more than 16 ranges";
        else sz_problem = parse_fault_dump(sz_item, &range);
        if (!sz_problem && cb_total + range.cb_dump > FAULT_DUMP_MAX_BYTES) sz_problem = "ranges above 64 KiB in total";
        if (sz_problem) fprintf(stderr, "Runtime: PS_FAULT_DUMP entry '%s' ignored: %s\n", sz_item, sz_problem);
        else {
            s_fault_dumps[s_c_fault_dumps++] = range;
            cb_total += range.cb_dump;
        }
        sz_value += cb_item;
        if (*sz_value == ',') ++sz_value;
    }
    if (s_c_fault_dumps)
        fprintf(stderr, "Runtime: PS_FAULT_DUMP: %zu ranges, %zu bytes dumped on a fault\n", s_c_fault_dumps, cb_total);
}
static void write_unreadable(uintptr_t va_begin, size_t cb_unreadable) {
    char sz_line[96];
    snprintf(sz_line, sizeof(sz_line), "  %016lx: unreadable (%zu bytes)\n", (unsigned long)va_begin, cb_unreadable);
    write_report(sz_line, strlen(sz_line));
}
/* Reads go through runtime_memory_read_guest_signal row by row, so an unmapped page only costs its rows. */
static void dump_fault_ranges(void) {
    char sz_line[160];
    for (size_t i_range = 0; i_range < s_c_fault_dumps; ++i_range) {
        const FaultDumpRange *p_range = &s_fault_dumps[i_range];
        uintptr_t va_dump = p_range->va_address;
        if (p_range->cb_pointer) {
            uint64_t pointer = 0;
            if (runtime_memory_read_guest_signal(&pointer, p_range->va_address, p_range->cb_pointer) !=
                (int64_t)p_range->cb_pointer) {
                snprintf(sz_line, sizeof(sz_line), "Fault dump *0x%lx/%zu: pointer unreadable\n",
                         (unsigned long)p_range->va_address, p_range->cb_pointer);
                write_report(sz_line, strlen(sz_line));
                continue;
            }
            va_dump = (uintptr_t)(pointer + (uint64_t)p_range->off_from_pointer);
            const int64_t off = p_range->off_from_pointer;
            snprintf(sz_line, sizeof(sz_line), "Fault dump *0x%lx/%zu = 0x%lx, %c0x%lx -> 0x%lx, %zu bytes:\n",
                     (unsigned long)p_range->va_address, p_range->cb_pointer, (unsigned long)pointer, off < 0 ? '-' : '+',
                     (unsigned long)(off < 0 ? -(uint64_t)off : (uint64_t)off), (unsigned long)va_dump, p_range->cb_dump);
        } else
            snprintf(sz_line, sizeof(sz_line), "Fault dump 0x%lx, %zu bytes:\n", (unsigned long)va_dump, p_range->cb_dump);
        write_report(sz_line, strlen(sz_line));
        uintptr_t va_unreadable = 0;
        size_t cb_unreadable = 0;
        for (size_t off_row = 0; off_row < p_range->cb_dump; off_row += 16) {
            const uintptr_t va_row = va_dump + off_row;
            const size_t cb_row = p_range->cb_dump - off_row < 16 ? p_range->cb_dump - off_row : 16;
            unsigned char row[16];
            const int64_t cb_read = runtime_memory_read_guest_signal(row, va_row, cb_row);
            if (cb_read <= 0) {
                if (!cb_unreadable) va_unreadable = va_row;
                cb_unreadable += cb_row;
                continue;
            }
            if (cb_unreadable) write_unreadable(va_unreadable, cb_unreadable);
            cb_unreadable = 0;
            int cb_line = snprintf(sz_line, sizeof(sz_line), "  %016lx:", (unsigned long)va_row);
            for (size_t i = 0; i < cb_row; ++i)
                cb_line += (int)i < cb_read ? snprintf(sz_line + cb_line, sizeof(sz_line) - (size_t)cb_line, " %02x", row[i])
                                            : snprintf(sz_line + cb_line, sizeof(sz_line) - (size_t)cb_line, " ??");
            sz_line[cb_line++] = '\n';
            write_report(sz_line, (size_t)cb_line);
        }
        if (cb_unreadable) write_unreadable(va_unreadable, cb_unreadable);
    }
}
/* The UTC time of day and the thread, so that a fault is placed against the launcher's own log (a
 * stop it sent, a window it closed). */
static void write_fault_moment(void) {
    const uint64_t ms_of_day = ps_host_time_realtime_ns() / 1000000u % 86400000u;
    char sz_thread[32] = "?";
    ps_host_thread_name(sz_thread, sizeof(sz_thread));
    char sz_line[96];
    snprintf(sz_line, sizeof(sz_line), "  at %02u:%02u:%02u.%03u UTC, thread %s\n", (unsigned)(ms_of_day / 3600000u),
             (unsigned)(ms_of_day / 60000u % 60u), (unsigned)(ms_of_day / 1000u % 60u), (unsigned)(ms_of_day % 1000u),
             sz_thread);
    write_report(sz_line, strlen(sz_line));
}
static int fault(PsHostFaultContext *p_context) {
    const PsHostFaultKind kind = ps_host_fault_kind(p_context);
    void *p_address = ps_host_fault_address(p_context);
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (g_is_gpu_enabled && kind == PS_HOST_FAULT_ACCESS && psport_gpu_handle_fault(p_context, p_address)) return 1;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((kind == PS_HOST_FAULT_ACCESS || kind == PS_HOST_FAULT_BUS) && g_p_runtime_fault_recover) {
        PsHostRecoveryPoint *p_recover = g_p_runtime_fault_recover;
        g_p_runtime_fault_recover = NULL;
        ps_host_fault_recover(p_context, p_recover);
    }
    /* The process is terminating: the loader's address lookup and snprintf are acceptable here. */
    const int sig = ps_host_fault_signal(p_context);
    uintptr_t rip = (uintptr_t)ps_host_fault_register(p_context, PS_HOST_REG_RIP);
    /* A fault inside a red-zone guard trampoline is reported against the original guest instruction
     * (#803), not the relocated copy outside the image. */
    if (g_is_gpu_enabled) rip = (uintptr_t)psport_gpu_red_zone_site(rip);
    char sz_line[512];
    PsHostCodeAddress where;
    if (rip - (uintptr_t)g_p_image < 0x10000000)
        snprintf(sz_line, sizeof(sz_line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)g_p_image), p_address);
    else if (ps_host_dl_address((void *)rip, &where) && where.sz_file)
        snprintf(sz_line, sizeof(sz_line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.sz_file,
                 (unsigned long)(rip - where.va_base), where.sz_symbol ? where.sz_symbol : "?", p_address);
    else
        snprintf(sz_line, sizeof(sz_line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, p_address);
    write_report(sz_line, strlen(sz_line));
    write_fault_moment();
    dump_fault_ranges();
    if (g_is_gpu_enabled) psport_gpu_dump_guest_writes(p_context);
    /* Guest code keeps frame pointers: its call chain says which caller handed over the bad
     * value, where the host backtrace below stops at the first guest frame. */
    if (rip - (uintptr_t)g_p_image < 0x10000000) dump_frames(p_context);
    /* A call through a null or stale pointer (an import PS_PERMISSIVE answered with 0, a symbol
     * lookup that failed): the return address on the stack names the guest caller. */
    else if (!(ps_host_dl_address((void *)rip, &where) && where.sz_file)) {
        uintptr_t caller = 0;
        if (runtime_memory_read_guest_signal(&caller, (uintptr_t)ps_host_fault_register(p_context, PS_HOST_REG_RSP), sizeof(caller)) ==
                (int64_t)sizeof(caller) &&
            caller - (uintptr_t)g_p_image < 0x10000000) {
            snprintf(sz_line, sizeof(sz_line), "  called from guest offset 0x%lx\n", (unsigned long)(caller - (uintptr_t)g_p_image));
            write_report(sz_line, strlen(sz_line));
            ps_host_fault_set_register(p_context, PS_HOST_REG_RIP, caller);
            dump_frames(p_context);
        }
    }
    /* Outside the image and any shared object (generated code, a freed mapping): the mapping
     * the host lists, and the thread. */
    if (rip - (uintptr_t)g_p_image >= 0x10000000 && !(ps_host_dl_address((void *)rip, &where) && where.sz_file)) {
        char sz_thread[32] = "?";
        ps_host_thread_name(sz_thread, sizeof(sz_thread));
        snprintf(sz_line, sizeof(sz_line), "  thread %s; mapping of RIP: ", sz_thread);
        write_report(sz_line, strlen(sz_line));
        MappingSearch search = {rip, 0};
        ps_host_list_mappings(write_mapping_of, &search);
        if (!search.is_found) write_report("none\n", 5);
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    const int depth = ps_host_fault_backtrace(p_context, frames, 32);
    for (int i = 0; i < depth; ++i) {
        if (ps_host_dl_address(frames[i], &where) && where.sz_file)
            snprintf(sz_line, sizeof(sz_line), "  #%d %s+0x%lx (%s)\n", i, where.sz_file,
                     (unsigned long)((uintptr_t)frames[i] - where.va_base), where.sz_symbol ? where.sz_symbol : "?");
        else
            snprintf(sz_line, sizeof(sz_line), "  #%d %p\n", i, frames[i]);
        write_report(sz_line, strlen(sz_line));
    }
    if (g_is_gpu_enabled) psport_gpu_flush_caches(0);
    runtime_calls_report();
    runtime_debug_finish("fault", 128 + sig);
    snprintf(sz_line, sizeof(sz_line), "Runtime: fault report written, the process ends with code %d\n", 128 + sig);
    write_report(sz_line, strlen(sz_line));
    ps_host_exit_at_once(128 + sig);
}
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets when inside the
 * image). Reads go through runtime_memory_read_guest_signal so bad frames cannot fault. */
static uintptr_t s_exe_base;
static void write_hex(char *sz_out, uint64_t v) {
    const char sz_digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) sz_out[i] = sz_digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(const PsHostFaultContext *p_context) {
    char sz_line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)ps_host_fault_register(p_context,PS_HOST_REG_RIP), rbp=(uintptr_t)ps_host_fault_register(p_context,PS_HOST_REG_RBP);
    uint64_t tid=(uint64_t)ps_host_thread_id();
    /* First argument register: the lock address when a thread waits on a futex. */
    char sz_arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(sz_arg+6,tid); write_hex(sz_arg+27,ps_host_fault_register(p_context,PS_HOST_REG_RDI));
    write_report(sz_arg,sizeof(sz_arg)-1);
    for (int depth=0; depth<24; ++depth) {
        write_hex(sz_line+6,tid); write_hex(sz_line+27,rip); write_hex(sz_line+59,rip-(uintptr_t)g_p_image); write_hex(sz_line+90,rip-s_exe_base);
        write_report(sz_line,sizeof(sz_line)-1);
        uintptr_t frame[2];
        if (!rbp || runtime_memory_read_guest_signal(frame,rbp,sizeof(frame))!=(int64_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(PsHostFaultContext *p_context) { dump_frames(p_context); }
static void dump_other_thread(int32_t tid, void *p_context) {
    (void)p_context;
    if (tid!=ps_host_thread_id()) { ps_host_interrupt_thread(tid,PS_HOST_INTERRUPT_THREAD_DUMP); ps_host_time_sleep_ns(20000000,NULL); }
}
static void watchdog(PsHostFaultContext *p_context) {
    const char sz_head[]="STOP: watchdog timeout; thread stacks:\n";
    write_report(sz_head,sizeof(sz_head)-1);
    dump_frames(p_context);
    ps_host_thread_each(dump_other_thread,NULL);
    ps_host_time_sleep_ns(100000000,NULL);
    if (g_is_gpu_enabled) psport_gpu_flush_caches(0);
    report_exit("watchdog timeout", 128 + ps_host_fault_signal(p_context));
    ps_host_exit_at_once(128 + ps_host_fault_signal(p_context));
}
void install_fault_handlers(unsigned timeout_seconds) {
    static const PsHostFaultKind s_kinds[] = {PS_HOST_FAULT_ACCESS, PS_HOST_FAULT_ILLEGAL, PS_HOST_FAULT_BUS,
                                              PS_HOST_FAULT_ARITHMETIC};
    for (size_t i = 0; i < sizeof(s_kinds) / sizeof(*s_kinds); ++i)
        ps_host_fault_install(s_kinds[i], fault, PS_HOST_FAULT_HOLD_GUEST_EXCEPTION);
    configure_fault_dumps();
    PsHostCodeAddress self;
    if (ps_host_dl_address((void *)main,&self)) s_exe_base=self.va_base;
    ps_host_interrupt_install(PS_HOST_INTERRUPT_THREAD_DUMP, thread_dump, PS_HOST_INTERRUPT_RESTART);
    ps_host_interrupt_install(PS_HOST_INTERRUPT_WATCHDOG, watchdog, 0);
    ps_host_watchdog_arm(timeout_seconds); /* 0 disables the watchdog */
}
/* Termination requests SDL does not take (--cpu-only): the exit summary, then the default action. */
static void report_termination(int sig) { report_exit("termination signal", 128 + sig); }
/* PS_STOP_FILE: a launcher with no SIGTERM to send (Windows, where a Job Object can only be killed and
 * WM_CLOSE was seen not to end Bloodborne, #1274) asks for an orderly end by creating this file. The
 * window thread then ends the game as when its window closes; if it has not within STOP_GRACE_NS the
 * summary is written from here and the process ends, before the launcher's own grace runs out. */
#define STOP_POLL_NS 250000000ull
#define STOP_GRACE_NS 5000000000ull
static void *watch_stop_file(void *p_path) {
    const char *sz_path = p_path;
    ps_host_thread_set_name("psport:stop");
    int error;
    while ((error = ps_host_fs_access(sz_path, 0)) == ENOENT) ps_host_time_sleep_ns(STOP_POLL_NS, NULL);
    if (error) {
        printf("Runtime: PS_STOP_FILE %s cannot be watched (%s): a launcher's stop request ends the game "
               "without its exit summary\n", sz_path, strerror(error));
        return NULL;
    }
    printf("Runtime: stop requested by the launcher (%s exists), the game ends\n", sz_path);
    if (g_is_gpu_enabled) {
        const int error_quit = psport_gpu_request_quit();
        if (error_quit)
            printf("Runtime: the window thread could not be asked to quit (%s), ending from the stop watcher\n",
                   strerror(error_quit));
        else {
            ps_host_time_sleep_ns(STOP_GRACE_NS, NULL);
            printf("Runtime: the window thread has not ended the game %llu s after the stop request, ending from "
                   "the stop watcher\n", STOP_GRACE_NS / 1000000000ull);
        }
        psport_gpu_flush_caches(0);
        report_exit("stop request (window thread did not end)", 143);
        fflush(NULL);
        ps_host_exit_at_once(143);
    }
    report_exit("stop request", 0);
    fflush(NULL);
    ps_host_exit_at_once(0);
}
void install_shutdown_signals(void) {
    ps_host_on_termination(report_termination);
    const char *sz_stop_file = runtime_env("PS_STOP_FILE", NULL);
    if (!sz_stop_file || !*sz_stop_file) return;
    pthread_t h_watcher;
    const int error = pthread_create(&h_watcher, NULL, watch_stop_file, (void *)sz_stop_file);
    if (error) {
        printf("Runtime: PS_STOP_FILE not watched, its thread could not start (%s): a launcher's stop request ends "
               "the game without its exit summary\n", strerror(error));
        return;
    }
    pthread_detach(h_watcher);
}
