/* Fault, watchdog and termination signal handlers: what the log shows when the process stops. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include "main.h"

#ifndef _WIN32
/* Termination signals SDL does not take (--cpu-only): the call summary, then the default action. */
static void shutdown_signal(int sig) {
    runtime_calls_report();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif
#ifndef _WIN32
static void dump_frames(ucontext_t *p_ucontext);
static void fault(int sig, siginfo_t *p_info, void *p_context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (g_is_gpu_enabled && sig == SIGSEGV && psport_gpu_handle_fault(p_context, p_info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && g_p_runtime_fault_recover) {
        sigjmp_buf *p_recover = g_p_runtime_fault_recover;
        g_p_runtime_fault_recover = NULL;
        pthread_sigmask(SIG_SETMASK, &((ucontext_t *)p_context)->uc_sigmask, NULL);
        siglongjmp(*p_recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *p_ucontext = p_context;
    uintptr_t rip = (uintptr_t)p_ucontext->uc_mcontext.gregs[REG_RIP];
    char sz_line[512];
    Dl_info where;
    if (rip - (uintptr_t)g_p_image < 0x10000000)
        snprintf(sz_line, sizeof(sz_line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)g_p_image), p_info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(sz_line, sizeof(sz_line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", p_info->si_addr);
    else
        snprintf(sz_line, sizeof(sz_line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, p_info->si_addr);
    { ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_; }
    if (g_is_gpu_enabled) psport_gpu_dump_guest_writes(p_context);
    /* Guest code keeps frame pointers: its call chain says which caller handed over the bad
     * value, where the host backtrace below stops at the first guest frame. */
    if (rip - (uintptr_t)g_p_image < 0x10000000) dump_frames(p_ucontext);
    /* A call through a null or stale pointer (an import PS_PERMISSIVE answered with 0, a symbol
     * lookup that failed): the return address on the stack names the guest caller. */
    else if (!(dladdr((void *)rip, &where) && where.dli_fname)) {
        uintptr_t caller = 0;
        struct iovec local = {&caller, sizeof(caller)}, remote = {(void *)p_ucontext->uc_mcontext.gregs[REG_RSP], sizeof(caller)};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)sizeof(caller) &&
            caller - (uintptr_t)g_p_image < 0x10000000) {
            snprintf(sz_line, sizeof(sz_line), "  called from guest offset 0x%lx\n", (unsigned long)(caller - (uintptr_t)g_p_image));
            { ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_; }
            p_ucontext->uc_mcontext.gregs[REG_RIP] = (greg_t)caller;
            dump_frames(p_ucontext);
        }
    }
    /* Outside the image and any shared object (generated code, a freed mapping): the mapping
     * from /proc/self/maps, and the thread. */
    if (rip - (uintptr_t)g_p_image >= 0x10000000 && !(dladdr((void *)rip, &where) && where.dli_fname)) {
        char sz_thread[32] = "?";
        pthread_getname_np(pthread_self(), sz_thread, sizeof(sz_thread));
        snprintf(sz_line, sizeof(sz_line), "  thread %s; mapping of RIP: ", sz_thread);
        { ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_; }
        FILE *p_maps = fopen("/proc/self/maps", "r");
        int is_found = 0;
        while (p_maps && fgets(sz_line, sizeof(sz_line), p_maps)) {
            unsigned long from, to;
            if (sscanf(sz_line, "%lx-%lx", &from, &to) == 2 && rip >= from && rip < to) {
                ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_;
                is_found = 1;
                break;
            }
        }
        if (p_maps) fclose(p_maps);
        if (!is_found) { ssize_t written_=write(2, "none\n", 5); (void)written_; }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(sz_line, sizeof(sz_line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(sz_line, sizeof(sz_line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, sz_line, strlen(sz_line)); (void)written_;
    }
    runtime_calls_report();
    _exit(128 + sig);
}
#endif
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t s_exe_base;
static void write_hex(char *sz_out, uint64_t v) {
    const char sz_digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) sz_out[i] = sz_digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *p_ucontext) {
    char sz_line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)p_ucontext->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)p_ucontext->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char sz_arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(sz_arg+6,tid); write_hex(sz_arg+27,(uint64_t)p_ucontext->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,sz_arg,sizeof(sz_arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(sz_line+6,tid); write_hex(sz_line+27,rip); write_hex(sz_line+59,rip-(uintptr_t)g_p_image); write_hex(sz_line+90,rip-s_exe_base);
        { ssize_t written_=write(2,sz_line,sizeof(sz_line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *p_info, void *p_context) { (void)sig; (void)p_info; dump_frames(p_context); }
static void watchdog(int sig, siginfo_t *p_info, void *p_context) {
    (void)p_info;
    const char sz_head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,sz_head,sizeof(sz_head)-1); (void)written_; }
    dump_frames(p_context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char sz_buffer[4096];
    long cb_read;
    pid_t self=gettid();
    while (dir>=0 && (cb_read=syscall(SYS_getdents64,dir,sz_buffer,sizeof(sz_buffer)))>0)
        for (long at=0; at<cb_read;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char sz_name[]; } *p_entry=(void *)(sz_buffer+at);
            pid_t tid=(pid_t)strtol(p_entry->sz_name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=p_entry->reclen;
        }
    usleep(100000);
    runtime_calls_report();
    _exit(128 + sig);
}
#ifndef _WIN32
void install_fault_handlers(unsigned timeout_seconds) {
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask); sigaddset(&sa.sa_mask, RUNTIME_EXCEPTION_SIGNAL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) s_exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
}
void install_shutdown_signals(void) {
    static const int s_shutdown_signals[] = {SIGTERM, SIGINT, SIGHUP};
    for (size_t i = 0; i < sizeof(s_shutdown_signals) / sizeof(*s_shutdown_signals); ++i) {
        struct sigaction current;
        if (!sigaction(s_shutdown_signals[i], NULL, &current) && current.sa_handler == SIG_DFL)
            signal(s_shutdown_signals[i], shutdown_signal);
    }
}
#endif
