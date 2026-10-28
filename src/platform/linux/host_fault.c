// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux: faults, thread interrupts and the signals behind them (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#define FXSAVE_SW_RESERVED 464
#define XSAVE_HEADER 512
#define XSAVE_YMM_END 832
#define MAX_HANDLERS 4

struct PsHostFaultContext {
    int signal;
    siginfo_t *p_info;
    ucontext_t *p_ucontext;
};

static const int s_fault_signals[PS_HOST_FAULT_KIND_COUNT] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
static const int s_gregs[PS_HOST_REG_COUNT] = {
    REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_RBP, REG_RSP, REG_R8,
    REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RIP, REG_EFL,
};
static _Atomic(PsHostFaultHandler) s_fault_handlers[PS_HOST_FAULT_KIND_COUNT][MAX_HANDLERS];
static _Atomic int s_c_fault_handlers[PS_HOST_FAULT_KIND_COUNT];
static unsigned s_fault_flags[PS_HOST_FAULT_KIND_COUNT];
static _Atomic(PsHostInterruptHandler) s_interrupt_handlers[PS_HOST_INTERRUPT_COUNT];
static void (*s_pfn_termination_report)(int sig);
static pthread_mutex_t s_install_lock = PTHREAD_MUTEX_INITIALIZER;

/* SIGUSR1 stays free for host libraries. */
int ps_host_interrupt_signal(PsHostInterrupt interrupt) {
    switch (interrupt) {
    case PS_HOST_INTERRUPT_GUEST_EXCEPTION: return SIGRTMIN + 3;
    case PS_HOST_INTERRUPT_THREAD_DUMP: return SIGUSR2;
    case PS_HOST_INTERRUPT_WATCHDOG: return SIGALRM;
    case PS_HOST_INTERRUPT_SAMPLE: return SIGPROF;
    case PS_HOST_INTERRUPT_COUNT: break;
    }
    return 0;
}

PsHostFaultKind ps_host_fault_kind(const PsHostFaultContext *p_context) {
    for (int kind = 0; kind < PS_HOST_FAULT_KIND_COUNT; ++kind)
        if (s_fault_signals[kind] == p_context->signal) return (PsHostFaultKind)kind;
    return PS_HOST_FAULT_KIND_COUNT;
}

int ps_host_fault_signal(const PsHostFaultContext *p_context) { return p_context->signal; }

void *ps_host_fault_address(const PsHostFaultContext *p_context) {
    return p_context->p_info ? p_context->p_info->si_addr : NULL;
}

/* The page fault's error code: bit 1 a write, bit 4 an instruction fetch. */
PsHostFaultAccess ps_host_fault_access(const PsHostFaultContext *p_context) {
    if (p_context->signal != SIGSEGV) return PS_HOST_FAULT_UNKNOWN;
    const greg_t error = p_context->p_ucontext->uc_mcontext.gregs[REG_ERR];
    return error & 0x10 ? PS_HOST_FAULT_EXECUTE : error & 0x2 ? PS_HOST_FAULT_WRITE : PS_HOST_FAULT_READ;
}

uint64_t ps_host_fault_register(const PsHostFaultContext *p_context, PsHostRegister reg) {
    return (uint64_t)p_context->p_ucontext->uc_mcontext.gregs[s_gregs[reg]];
}

void ps_host_fault_set_register(PsHostFaultContext *p_context, PsHostRegister reg, uint64_t value) {
    p_context->p_ucontext->uc_mcontext.gregs[s_gregs[reg]] = (greg_t)value;
}

void ps_host_fault_cpu(const PsHostFaultContext *p_context, PsHostFaultCpu *p_out) {
    const mcontext_t *p_machine = &p_context->p_ucontext->uc_mcontext;
    *p_out = (PsHostFaultCpu){
        .trap_number = (uint32_t)p_machine->gregs[REG_TRAPNO],
        .error_code = (uint64_t)p_machine->gregs[REG_ERR],
        .code_selector = (uint16_t)((uint64_t)p_machine->gregs[REG_CSGSFS] & 0xffff),
        .p_fxsave = p_machine->fpregs,
    };
}

void ps_host_fault_recover(const PsHostFaultContext *p_context, PsHostRecoveryPoint *p_point) {
    pthread_sigmask(SIG_SETMASK, &p_context->p_ucontext->uc_sigmask, NULL);
    siglongjmp(*p_point, 1);
}

static void on_fault(int sig, siginfo_t *p_info, void *p_raw) {
    PsHostFaultContext context = {sig, p_info, p_raw};
    const PsHostFaultKind kind = ps_host_fault_kind(&context);
    for (int i = atomic_load(&s_c_fault_handlers[kind]); i-- > 0;) {
        const PsHostFaultHandler pfn_handler = atomic_load(&s_fault_handlers[kind][i]);
        if (pfn_handler && pfn_handler(&context)) return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int ps_host_fault_install(PsHostFaultKind kind, PsHostFaultHandler pfn_handler, unsigned flags) {
    pthread_mutex_lock(&s_install_lock);
    const int c_handlers = atomic_load(&s_c_fault_handlers[kind]);
    if (c_handlers == MAX_HANDLERS) {
        pthread_mutex_unlock(&s_install_lock);
        return ENOSPC;
    }
    s_fault_flags[kind] |= flags;
    struct sigaction action = {0};
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | (s_fault_flags[kind] & PS_HOST_FAULT_NESTED ? SA_NODEFER : 0);
    sigemptyset(&action.sa_mask);
    if (s_fault_flags[kind] & PS_HOST_FAULT_HOLD_GUEST_EXCEPTION)
        sigaddset(&action.sa_mask, ps_host_interrupt_signal(PS_HOST_INTERRUPT_GUEST_EXCEPTION));
    atomic_store(&s_fault_handlers[kind][c_handlers], pfn_handler);
    atomic_store(&s_c_fault_handlers[kind], c_handlers + 1);
    const int error = sigaction(s_fault_signals[kind], &action, NULL) ? errno : 0;
    if (error) atomic_store(&s_c_fault_handlers[kind], c_handlers);
    pthread_mutex_unlock(&s_install_lock);
    return error;
}

static void on_interrupt(int sig, siginfo_t *p_info, void *p_raw) {
    PsHostFaultContext context = {sig, p_info, p_raw};
    for (int interrupt = 0; interrupt < PS_HOST_INTERRUPT_COUNT; ++interrupt) {
        if (ps_host_interrupt_signal((PsHostInterrupt)interrupt) != sig) continue;
        const PsHostInterruptHandler pfn_handler = atomic_load(&s_interrupt_handlers[interrupt]);
        if (pfn_handler) pfn_handler(&context);
        return;
    }
}

int ps_host_interrupt_install(PsHostInterrupt interrupt, PsHostInterruptHandler pfn_handler, unsigned flags) {
    atomic_store(&s_interrupt_handlers[interrupt], pfn_handler);
    struct sigaction action = {0};
    action.sa_sigaction = on_interrupt;
    action.sa_flags = SA_SIGINFO | (flags & PS_HOST_INTERRUPT_RESTART ? SA_RESTART : 0);
    sigemptyset(&action.sa_mask);
    return sigaction(ps_host_interrupt_signal(interrupt), &action, NULL) ? errno : 0;
}

_Static_assert(sizeof(sigset_t) <= sizeof(((PsHostInterruptMask *)0)->opaque), "sigset_t fits PsHostInterruptMask");
void ps_host_interrupts_hold(int is_holding_all, PsHostInterruptMask *p_out_previous) {
    sigset_t held, previous;
    if (is_holding_all) {
        sigfillset(&held);
        for (int i = 0; i < PS_HOST_FAULT_KIND_COUNT; ++i) sigdelset(&held, s_fault_signals[i]);
    } else {
        sigemptyset(&held);
    }
    pthread_sigmask(SIG_SETMASK, &held, &previous);
    if (p_out_previous) memcpy(p_out_previous->opaque, &previous, sizeof(previous));
}

void ps_host_interrupts_restore(const PsHostInterruptMask *p_mask) {
    sigset_t mask;
    memcpy(&mask, p_mask->opaque, sizeof(mask));
    pthread_sigmask(SIG_SETMASK, &mask, NULL);
}

int ps_host_interrupt_thread(int32_t tid, PsHostInterrupt interrupt) {
    return syscall(SYS_tgkill, getpid(), (pid_t)tid, ps_host_interrupt_signal(interrupt)) ? errno : 0;
}

void ps_host_watchdog_arm(unsigned seconds) { alarm(seconds); }

static void on_termination(int sig) {
    s_pfn_termination_report(sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* SDL takes these unless it runs without video (--cpu-only): only a default action is replaced. */
void ps_host_on_termination(void (*pfn_report)(int sig)) {
    static const int s_signals[] = {SIGTERM, SIGINT, SIGHUP};
    s_pfn_termination_report = pfn_report;
    for (size_t i = 0; i < sizeof(s_signals) / sizeof(*s_signals); ++i) {
        struct sigaction current;
        if (!sigaction(s_signals[i], NULL, &current) && current.sa_handler == SIG_DFL) signal(s_signals[i], on_termination);
    }
}

/* Through rt_sigreturn, which sets rsp, rip, rflags, the FPU and the signal mask at once. */
void ps_host_resume(const uint64_t p_registers[PS_HOST_REG_COUNT], const void *p_fpu, int exception_signal,
                    int is_exception_masked) {
    ucontext_t frame;
    memset(&frame, 0, sizeof(frame));
    for (int reg = 0; reg < PS_HOST_REG_COUNT; ++reg) frame.uc_mcontext.gregs[s_gregs[reg]] = (greg_t)p_registers[reg];
    /* The host's own user selectors: the code resumed runs in the same code segment. */
    uint16_t code_selector, stack_selector;
    __asm__("mov %%cs,%0" : "=r"(code_selector));
    __asm__("mov %%ss,%0" : "=r"(stack_selector));
    frame.uc_mcontext.gregs[REG_CSGSFS] = (greg_t)((uint64_t)code_selector | (uint64_t)stack_selector << 48);
    /* Without fpregs the kernel resets the FPU. A valid xsave header holding AVX state brings the
     * YMM upper halves back too, which the kernel takes only with its software bytes in the
     * fxsave area's reserved part and its second magic at the end. */
    _Alignas(64) uint8_t fpu[XSAVE_YMM_END + 64];
    if (p_fpu) {
        memset(fpu, 0, sizeof(fpu));
        memcpy(fpu, p_fpu, XSAVE_YMM_END);
        uint64_t xstate_bv, xcomp_bv;
        memcpy(&xstate_bv, fpu + XSAVE_HEADER, 8);
        memcpy(&xcomp_bv, fpu + XSAVE_HEADER + 8, 8);
        memset(fpu + FXSAVE_SW_RESERVED, 0, XSAVE_HEADER - FXSAVE_SW_RESERVED);
        if ((xstate_bv & 4) && !(xstate_bv & ~UINT64_C(7)) && !xcomp_bv) {
            const struct _fpx_sw_bytes sw_bytes = {.magic1 = FP_XSTATE_MAGIC1,
                                                   .extended_size = XSAVE_YMM_END + 4,
                                                   .xstate_bv = 7,
                                                   .xstate_size = XSAVE_YMM_END};
            const uint32_t magic2 = FP_XSTATE_MAGIC2;
            memcpy(fpu + FXSAVE_SW_RESERVED, &sw_bytes, sizeof(sw_bytes));
            memcpy(fpu + XSAVE_YMM_END, &magic2, 4);
        }
        frame.uc_mcontext.fpregs = (fpregset_t)fpu;
    }
    pthread_sigmask(SIG_SETMASK, NULL, &frame.uc_sigmask);
    if (is_exception_masked) sigaddset(&frame.uc_sigmask, exception_signal);
    else sigdelset(&frame.uc_sigmask, exception_signal);
    /* rt_sigreturn applies uc_stack as the alternate signal stack. */
    sigaltstack(NULL, &frame.uc_stack);
    /* A signal taken between the stack switch and the syscall would push its frame over fpu. */
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    __asm__ volatile("mov %0,%%rsp\n\tmov $15,%%eax\n\tsyscall\n\tud2" : : "r"(&frame) : "memory");
    __builtin_unreachable();
}

int64_t ps_host_read_memory(void *p_out, const void *p_from, size_t cb_copy) {
    const struct iovec local = {p_out, cb_copy}, remote = {(void *)p_from, cb_copy};
    const ssize_t cb_read = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return cb_read < 0 ? -errno : cb_read;
}

int64_t ps_host_write_memory(void *p_to, const void *p_from, size_t cb_copy) {
    const struct iovec local = {(void *)p_from, cb_copy}, remote = {p_to, cb_copy};
    const ssize_t cb_written = process_vm_writev(getpid(), &local, 1, &remote, 1, 0);
    return cb_written < 0 ? -errno : cb_written;
}

int ps_host_backtrace(void **pp_frames, int c_frames) {
    void *frames[65];
    const int depth = backtrace(frames, c_frames < 64 ? c_frames + 1 : 65);
    const int c_stored = depth > 0 ? depth - 1 : 0;
    memcpy(pp_frames, frames + 1, (size_t)c_stored * sizeof(*frames));
    return c_stored;
}

int ps_host_fault_backtrace(const PsHostFaultContext *p_context, void **pp_frames, int c_frames) {
    void *frames[64];
    const int depth = backtrace(frames, 64);
    const void *p_rip = (const void *)p_context->p_ucontext->uc_mcontext.gregs[REG_RIP];
    int i_first = 0;
    while (i_first < depth && frames[i_first] != p_rip) ++i_first;
    if (i_first == depth) i_first = 0;
    int c_stored = 0;
    for (; c_stored < c_frames && i_first + c_stored < depth; ++c_stored) pp_frames[c_stored] = frames[i_first + c_stored];
    return c_stored;
}
