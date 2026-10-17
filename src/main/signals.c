/* Fault, watchdog and termination signal handlers: what the log shows when the process stops. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "main.h"

#ifndef _WIN32
static void dump_frames(const PsHostFaultContext *p_context);
typedef struct { uintptr_t va; int is_found; } MappingSearch;
static int write_mapping_of(const PsHostMapping *p_mapping, void *p_context) {
    MappingSearch *p_search = p_context;
    if (p_search->va < p_mapping->va_begin || p_search->va >= p_mapping->va_end) return 0;
    (void)ps_host_fs_write(2, p_mapping->sz_description, strlen(p_mapping->sz_description));
    (void)ps_host_fs_write(2, "\n", 1);
    p_search->is_found = 1;
    return 1;
}
static int fault(PsHostFaultContext *p_context) {
    const PsHostFaultKind kind = ps_host_fault_kind(p_context);
    void *p_address = ps_host_fault_address(p_context);
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (g_is_gpu_enabled && kind == PS_HOST_FAULT_ACCESS && psport_gpu_handle_fault(p_context, p_address)) return 1;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((kind == PS_HOST_FAULT_ACCESS || kind == PS_HOST_FAULT_BUS) && g_p_runtime_fault_recover) {
        sigjmp_buf *p_recover = g_p_runtime_fault_recover;
        g_p_runtime_fault_recover = NULL;
        ps_host_fault_recover(p_context, p_recover);
    }
    /* The process is terminating: the loader's address lookup and snprintf are acceptable here. */
    const int sig = ps_host_fault_signal(p_context);
    uintptr_t rip = (uintptr_t)ps_host_fault_register(p_context, PS_HOST_REG_RIP);
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
    (void)ps_host_fs_write(2, sz_line, strlen(sz_line));
    if (g_is_gpu_enabled) psport_gpu_dump_guest_writes(p_context);
    /* Guest code keeps frame pointers: its call chain says which caller handed over the bad
     * value, where the host backtrace below stops at the first guest frame. */
    if (rip - (uintptr_t)g_p_image < 0x10000000) dump_frames(p_context);
    /* A call through a null or stale pointer (an import PS_PERMISSIVE answered with 0, a symbol
     * lookup that failed): the return address on the stack names the guest caller. */
    else if (!(ps_host_dl_address((void *)rip, &where) && where.sz_file)) {
        uintptr_t caller = 0;
        if (ps_host_read_memory(&caller, (const void *)ps_host_fault_register(p_context, PS_HOST_REG_RSP), sizeof(caller)) ==
                (int64_t)sizeof(caller) &&
            caller - (uintptr_t)g_p_image < 0x10000000) {
            snprintf(sz_line, sizeof(sz_line), "  called from guest offset 0x%lx\n", (unsigned long)(caller - (uintptr_t)g_p_image));
            (void)ps_host_fs_write(2, sz_line, strlen(sz_line));
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
        (void)ps_host_fs_write(2, sz_line, strlen(sz_line));
        MappingSearch search = {rip, 0};
        ps_host_list_mappings(write_mapping_of, &search);
        if (!search.is_found) (void)ps_host_fs_write(2, "none\n", 5);
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
        (void)ps_host_fs_write(2, sz_line, strlen(sz_line));
    }
    if (g_is_gpu_enabled) psport_gpu_flush_caches(0);
    runtime_calls_report();
    _exit(128 + sig);
}
#endif
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets when inside the
 * image). Reads go through ps_host_read_memory so bad frames cannot fault. */
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
    (void)ps_host_fs_write(2, sz_arg,sizeof(sz_arg)-1);
    for (int depth=0; depth<24; ++depth) {
        write_hex(sz_line+6,tid); write_hex(sz_line+27,rip); write_hex(sz_line+59,rip-(uintptr_t)g_p_image); write_hex(sz_line+90,rip-s_exe_base);
        (void)ps_host_fs_write(2, sz_line,sizeof(sz_line)-1);
        uintptr_t frame[2];
        if (!rbp || ps_host_read_memory(frame,(const void *)rbp,sizeof(frame))!=(int64_t)sizeof(frame)) break;
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
    (void)ps_host_fs_write(2, sz_head,sizeof(sz_head)-1);
    dump_frames(p_context);
    ps_host_thread_each(dump_other_thread,NULL);
    ps_host_time_sleep_ns(100000000,NULL);
    if (g_is_gpu_enabled) psport_gpu_flush_caches(0);
    runtime_calls_report();
    _exit(128 + ps_host_fault_signal(p_context));
}
#ifndef _WIN32
void install_fault_handlers(unsigned timeout_seconds) {
    static const PsHostFaultKind s_kinds[] = {PS_HOST_FAULT_ACCESS, PS_HOST_FAULT_ILLEGAL, PS_HOST_FAULT_BUS,
                                              PS_HOST_FAULT_ARITHMETIC};
    for (size_t i = 0; i < sizeof(s_kinds) / sizeof(*s_kinds); ++i)
        ps_host_fault_install(s_kinds[i], fault, PS_HOST_FAULT_HOLD_GUEST_EXCEPTION);
    PsHostCodeAddress self;
    if (ps_host_dl_address((void *)main,&self)) s_exe_base=self.va_base;
    ps_host_interrupt_install(PS_HOST_INTERRUPT_THREAD_DUMP, thread_dump, PS_HOST_INTERRUPT_RESTART);
    ps_host_interrupt_install(PS_HOST_INTERRUPT_WATCHDOG, watchdog, 0);
    ps_host_watchdog_arm(timeout_seconds); /* 0 disables the watchdog */
}
/* Termination requests SDL does not take (--cpu-only): the call summary, then the default action. */
void install_shutdown_signals(void) { ps_host_on_termination(runtime_calls_report); }
#endif
