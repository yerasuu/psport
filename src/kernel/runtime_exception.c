/* libkernel exception handlers (libkernel_unity): the guest's handler runs on the target thread,
 * on that thread's own stack, with a FreeBSD ucontext of the interrupted registers. Unity's
 * IL2CPP garbage collector suspends each thread this way (sceKernelRaiseException with SIGUSR1)
 * and scans its stack from the saved rsp. Guest code already runs on the host thread with GS at
 * its TCB, so the handler is called straight from the host signal handler. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define GUEST_SIGNALS 128
#define GUEST_SIGUSR1 30
/* How long a thread in runtime_exception_wait sleeps before it looks for a raise held back, with a
 * handler installed and without one (only a handler installed during the sleep can need it). */
#define RAISE_SLICE_NS 2000000
#define IDLE_SLICE_NS 100000000
#define MC_FLAG_MASK 0x7u
#define MC_FPFMT_XMM 0x10002
#define MC_FPOWNED_FPU 0x20001
#define GUEST_EFAULT 14
#define GUEST_EINVAL 22
#define FXSAVE_SW_RESERVED 464
#define XSAVE_HEADER 512
#define XSAVE_YMM_END 832

typedef void (ABI *GuestHandler)(int, void *);
/* FreeBSD amd64 mcontext_t and ucontext_t as the PS4 kernel passes them (shadPS4's exception.h). */
typedef struct {
    uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
    uint32_t trapno;
    uint16_t fs, gs;
    uint64_t addr;
    uint32_t flags;
    uint16_t es, ds;
    uint64_t err, rip, cs, rflags, rsp, ss, len, fpformat, ownedfp, lbrfrom, lbrto, aux1, aux2;
    uint64_t fpstate[104];
    uint64_t fsbase, gsbase, spare[6];
} GuestMcontext;
typedef struct {
    uint32_t sigmask[4];
    uint32_t reserved[12];
    GuestMcontext mcontext;
    uint64_t link;
    uint64_t stack_sp, stack_size;
    uint32_t stack_flags, stack_align;
    uint32_t flags, spare[4], tail[3];
} GuestUcontext;
_Static_assert(sizeof(GuestMcontext)==0x480,"FreeBSD mcontext_t size");
_Static_assert(sizeof(GuestUcontext)==0x500,"PS4 ucontext_t size");
/* PS4Util.prx hands this field to IL2CPP's suspend callback as the thread's stack pointer. */
_Static_assert(offsetof(GuestUcontext,mcontext.rsp)==0xf8,"ucontext rsp offset");

static _Atomic(GuestHandler) s_handlers[GUEST_SIGNALS];
/* Runtime locks this thread holds, is taking or sleeps on in runtime_exception_wait, and a raise
 * held back until it lets go. */
static _Thread_local unsigned s_held;
static _Thread_local volatile sig_atomic_t s_is_deferred;
static _Atomic unsigned s_c_raised, s_c_delivered;
/* The errnos deliver puts back when the guest's handler returns, for a handler that leaves by
 * sigreturn instead. */
typedef struct { int is_active, host_errno; int32_t guest_errno; } Delivery;
static _Thread_local Delivery s_delivery;

/* The signals libkernel lets a game handle (shadPS4's orbis_allowed_signals). */
static int allowed(int sig) { return sig==1 || sig==4 || sig==8 || sig==10 || sig==11 || sig==GUEST_SIGUSR1; }

void runtime_exception_hold(void) {
    ++s_held;
    atomic_signal_fence(memory_order_seq_cst);
}
void runtime_exception_release(void) {
    atomic_signal_fence(memory_order_seq_cst);
    --s_held;
    atomic_signal_fence(memory_order_seq_cst);
    if (!s_held && s_is_deferred) pthread_kill(pthread_self(),RUNTIME_EXCEPTION_SIGNAL);
}
void runtime_exception_lock(pthread_mutex_t *p_lock) {
    runtime_exception_hold();
    pthread_mutex_lock(p_lock);
}
void runtime_exception_unlock(pthread_mutex_t *p_lock) {
    pthread_mutex_unlock(p_lock);
    runtime_exception_release();
}
int runtime_exception_wait(pthread_cond_t *p_cond, pthread_mutex_t *p_lock, const struct timespec *p_end) {
    if (s_is_deferred && s_held==1) { runtime_exception_unlock(p_lock); runtime_exception_lock(p_lock); return 0; }
    struct timespec slice;
    ps_host_time_read(PS_HOST_CLOCK_MONOTONIC,&slice);
    slice.tv_nsec+=atomic_load(&s_handlers[GUEST_SIGUSR1]) ? RAISE_SLICE_NS : IDLE_SLICE_NS;
    slice.tv_sec+=slice.tv_nsec/1000000000; slice.tv_nsec%=1000000000;
    int last=p_end && (p_end->tv_sec<slice.tv_sec || (p_end->tv_sec==slice.tv_sec && p_end->tv_nsec<=slice.tv_nsec));
    int e=pthread_cond_clockwait(p_cond,p_lock,CLOCK_MONOTONIC,last ? p_end : &slice);
    return e==ETIMEDOUT && !last ? 0 : e;
}

static void deliver(int sig, siginfo_t *p_info, void *p_raw) {
    (void)sig; (void)p_info;
    GuestHandler pfn_handler=atomic_load(&s_handlers[GUEST_SIGUSR1]);
    if (!pfn_handler) { s_is_deferred=0; return; }
    if (s_held) { s_is_deferred=1; return; }
    s_is_deferred=0;
    ucontext_t *p_host=p_raw;
    greg_t *p_registers=p_host->uc_mcontext.gregs;
    /* The handler may call back into the runtime, which sets both errnos. */
    int host_errno=errno;
    int32_t guest_errno=*runtime_errno();
    GuestUcontext context;
    memset(&context,0,sizeof(context));
    GuestMcontext *p_mcontext=&context.mcontext;
    p_mcontext->rdi=(uint64_t)p_registers[REG_RDI]; p_mcontext->rsi=(uint64_t)p_registers[REG_RSI]; p_mcontext->rdx=(uint64_t)p_registers[REG_RDX]; p_mcontext->rcx=(uint64_t)p_registers[REG_RCX];
    p_mcontext->r8=(uint64_t)p_registers[REG_R8]; p_mcontext->r9=(uint64_t)p_registers[REG_R9]; p_mcontext->rax=(uint64_t)p_registers[REG_RAX]; p_mcontext->rbx=(uint64_t)p_registers[REG_RBX];
    p_mcontext->rbp=(uint64_t)p_registers[REG_RBP]; p_mcontext->r10=(uint64_t)p_registers[REG_R10]; p_mcontext->r11=(uint64_t)p_registers[REG_R11]; p_mcontext->r12=(uint64_t)p_registers[REG_R12];
    p_mcontext->r13=(uint64_t)p_registers[REG_R13]; p_mcontext->r14=(uint64_t)p_registers[REG_R14]; p_mcontext->r15=(uint64_t)p_registers[REG_R15];
    p_mcontext->rip=(uint64_t)p_registers[REG_RIP]; p_mcontext->rsp=(uint64_t)p_registers[REG_RSP]; p_mcontext->rflags=(uint64_t)p_registers[REG_EFL];
    p_mcontext->cs=(uint64_t)p_registers[REG_CSGSFS]&0xffff;
    p_mcontext->trapno=(uint32_t)p_registers[REG_TRAPNO]; p_mcontext->err=(uint64_t)p_registers[REG_ERR];
    p_mcontext->len=sizeof(*p_mcontext);
    /* fxsave layout as FreeBSD's savefpu begins: a collector may find pointers spilled to xmm. */
    if (p_host->uc_mcontext.fpregs) {
        memcpy(p_mcontext->fpstate,p_host->uc_mcontext.fpregs,512);
        p_mcontext->fpformat=0x10002; /* _MC_FPFMT_XMM */
        p_mcontext->ownedfp=0x20001;  /* _MC_FPOWNED_FPU */
    }
    /* The guest's FS base is its TCB, which the runtime keeps in GS. */
    unsigned long tcb=0;
    if (!syscall(SYS_arch_prctl,ARCH_GET_GS,&tcb)) { p_mcontext->fsbase=tcb; p_mcontext->flags=0x2; /* _MC_HASBASES */ }
    if (!atomic_fetch_add(&s_c_delivered,1)) {
        static const char s_sz_first[]="Runtime: first guest exception delivered to its handler\n";
        /* write, not stdio: this runs in a signal handler. */
        ssize_t written=write(2,s_sz_first,sizeof(s_sz_first)-1); (void)written;
    }
    s_delivery=(Delivery){1,host_errno,guest_errno};
    pfn_handler(GUEST_SIGUSR1,&context);
    s_delivery.is_active=0;
    /* As on the console, register changes the handler makes take effect on return. The FPU area
     * is a copy for the handler to read and is not restored. */
    p_registers[REG_RDI]=(greg_t)p_mcontext->rdi; p_registers[REG_RSI]=(greg_t)p_mcontext->rsi; p_registers[REG_RDX]=(greg_t)p_mcontext->rdx; p_registers[REG_RCX]=(greg_t)p_mcontext->rcx;
    p_registers[REG_R8]=(greg_t)p_mcontext->r8; p_registers[REG_R9]=(greg_t)p_mcontext->r9; p_registers[REG_RAX]=(greg_t)p_mcontext->rax; p_registers[REG_RBX]=(greg_t)p_mcontext->rbx;
    p_registers[REG_RBP]=(greg_t)p_mcontext->rbp; p_registers[REG_R10]=(greg_t)p_mcontext->r10; p_registers[REG_R11]=(greg_t)p_mcontext->r11; p_registers[REG_R12]=(greg_t)p_mcontext->r12;
    p_registers[REG_R13]=(greg_t)p_mcontext->r13; p_registers[REG_R14]=(greg_t)p_mcontext->r14; p_registers[REG_R15]=(greg_t)p_mcontext->r15;
    p_registers[REG_RIP]=(greg_t)p_mcontext->rip; p_registers[REG_RSP]=(greg_t)p_mcontext->rsp; p_registers[REG_EFL]=(greg_t)p_mcontext->rflags;
    *runtime_errno()=guest_errno;
    errno=host_errno;
}

static ABI int32_t install_handler(int sig, GuestHandler pfn_handler) {
    if (!allowed(sig)) return ERR(22);
    GuestHandler pfn_none=NULL;
    if (!atomic_compare_exchange_strong(&s_handlers[sig],&pfn_none,pfn_handler)) return ERR(35);
    if (sig==GUEST_SIGUSR1) {
        /* SA_RESTART so that file reads and futex waits the signal interrupts resume by themselves;
         * the runtime's sleeps retry on EINTR. Not SA_ONSTACK: the collector scans the stack the
         * handler runs on as the thread's own. */
        struct sigaction action={0};
        action.sa_sigaction=deliver;
        action.sa_flags=SA_SIGINFO|SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(RUNTIME_EXCEPTION_SIGNAL,&action,NULL)) { atomic_store(&s_handlers[sig],NULL); return ERR(runtime_guest_errno(errno)); }
    }
    printf("Runtime: guest exception handler for signal %d installed at %p%s\n",sig,(void *)pfn_handler,
           sig==GUEST_SIGUSR1 ? "" : " (host faults are not forwarded to it)");
    return 0;
}
static ABI int32_t remove_handler(int sig) {
    if (!allowed(sig)) return ERR(22);
    atomic_store(&s_handlers[sig],NULL);
    return 0;
}
static ABI int32_t raise_exception(void *p_thread, int sig) {
    if (sig!=GUEST_SIGUSR1) return ERR(22);
    int32_t result=runtime_thread_kill(p_thread,RUNTIME_EXCEPTION_SIGNAL);
    if (!result && !atomic_fetch_add(&s_c_raised,1)) {
        /* write, not stdio: the thread may stop in the handler while it holds stdout's lock. */
        char sz_line[96];
        int cb_line=snprintf(sz_line,sizeof(sz_line),"Runtime: first guest exception raised (signal %d) to thread %p\n",sig,p_thread);
        ssize_t written=write(2,sz_line,(size_t)cb_line); (void)written;
    }
    return result;
}

/* FreeBSD's sigreturn: the whole context through the host's rt_sigreturn, which sets rsp, rip,
 * rflags, the FPU and the signal mask in one step, so nothing is written below the restored rsp
 * (an interrupted leaf function's red zone survives). */
static ABI int32_t guest_sigreturn(const GuestUcontext *p_context) {
    if (!p_context) { *runtime_errno()=GUEST_EFAULT; return -1; }
    const GuestMcontext *p_mcontext=&p_context->mcontext;
    if ((p_mcontext->flags & ~MC_FLAG_MASK) || (p_mcontext->cs & 3)!=3) { *runtime_errno()=GUEST_EINVAL; return -1; }
    ucontext_t frame;
    memset(&frame,0,sizeof(frame));
    greg_t *p_regs=frame.uc_mcontext.gregs;
    p_regs[REG_RDI]=(greg_t)p_mcontext->rdi; p_regs[REG_RSI]=(greg_t)p_mcontext->rsi;
    p_regs[REG_RDX]=(greg_t)p_mcontext->rdx; p_regs[REG_RCX]=(greg_t)p_mcontext->rcx;
    p_regs[REG_R8]=(greg_t)p_mcontext->r8; p_regs[REG_R9]=(greg_t)p_mcontext->r9;
    p_regs[REG_RAX]=(greg_t)p_mcontext->rax; p_regs[REG_RBX]=(greg_t)p_mcontext->rbx;
    p_regs[REG_RBP]=(greg_t)p_mcontext->rbp; p_regs[REG_R10]=(greg_t)p_mcontext->r10;
    p_regs[REG_R11]=(greg_t)p_mcontext->r11; p_regs[REG_R12]=(greg_t)p_mcontext->r12;
    p_regs[REG_R13]=(greg_t)p_mcontext->r13; p_regs[REG_R14]=(greg_t)p_mcontext->r14;
    p_regs[REG_R15]=(greg_t)p_mcontext->r15; p_regs[REG_RIP]=(greg_t)p_mcontext->rip;
    p_regs[REG_RSP]=(greg_t)p_mcontext->rsp; p_regs[REG_EFL]=(greg_t)p_mcontext->rflags;
    /* The host's own user selectors: the guest runs in the same code segment, whatever it saved. */
    uint16_t code_selector, stack_selector;
    __asm__("mov %%cs,%0" : "=r"(code_selector));
    __asm__("mov %%ss,%0" : "=r"(stack_selector));
    p_regs[REG_CSGSFS]=(greg_t)((uint64_t)code_selector|(uint64_t)stack_selector<<48);
    /* With the FPU not owned the kernel resets it, as FreeBSD does for _MC_FPOWNED_NONE. A valid
     * xsave header that holds AVX state brings the YMM upper halves back too. */
    _Alignas(64) uint8_t fpu[XSAVE_YMM_END+64];
    if (p_mcontext->ownedfp==MC_FPOWNED_FPU && p_mcontext->fpformat==MC_FPFMT_XMM) {
        memset(fpu,0,sizeof(fpu));
        memcpy(fpu,p_mcontext->fpstate,XSAVE_YMM_END);
        uint64_t xstate_bv, xcomp_bv;
        memcpy(&xstate_bv,fpu+XSAVE_HEADER,8);
        memcpy(&xcomp_bv,fpu+XSAVE_HEADER+8,8);
        memset(fpu+FXSAVE_SW_RESERVED,0,XSAVE_HEADER-FXSAVE_SW_RESERVED);
        if ((xstate_bv & 4) && !(xstate_bv & ~UINT64_C(7)) && !xcomp_bv) {
            const struct _fpx_sw_bytes sw_bytes={.magic1=FP_XSTATE_MAGIC1,.extended_size=XSAVE_YMM_END+4,
                                                 .xstate_bv=7,.xstate_size=XSAVE_YMM_END};
            const uint32_t magic2=FP_XSTATE_MAGIC2;
            memcpy(fpu+FXSAVE_SW_RESERVED,&sw_bytes,sizeof(sw_bytes));
            memcpy(fpu+XSAVE_YMM_END,&magic2,4);
        }
        frame.uc_mcontext.fpregs=(fpregset_t)fpu;
    }
    /* Only the guest's SIGUSR1 has a host signal behind it; the others keep their host state. */
    pthread_sigmask(SIG_SETMASK,NULL,&frame.uc_sigmask);
    if (p_context->sigmask[0] & 1u<<(GUEST_SIGUSR1-1)) sigaddset(&frame.uc_sigmask,RUNTIME_EXCEPTION_SIGNAL);
    else sigdelset(&frame.uc_sigmask,RUNTIME_EXCEPTION_SIGNAL);
    /* rt_sigreturn applies uc_stack as the alternate signal stack. */
    sigaltstack(NULL,&frame.uc_stack);
    runtime_kernel_set_signal_mask(p_context->sigmask);
    if (s_delivery.is_active) {
        s_delivery.is_active=0;
        *runtime_errno()=s_delivery.guest_errno;
        errno=s_delivery.host_errno;
    }
    /* A signal taken between the stack switch and the syscall would push its frame over fpu. */
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK,&all,NULL);
    __asm__ volatile("mov %0,%%rsp\n\tmov $15,%%eax\n\tsyscall\n\tud2" : : "r"(&frame) : "memory");
    __builtin_unreachable();
}
/* libunwind's x86-64 setcontext as built for FreeBSD: the FPU from fxsave when the context owns
 * it, rip pushed on the restored stack, rcx and rdi last. r10, r11, rflags and the signal mask
 * stay as they are. */
static ABI __attribute__((noreturn)) void unwind_setcontext(const GuestUcontext *p_context) {
    s_delivery.is_active=0;
    if (p_context->mcontext.ownedfp==MC_FPOWNED_FPU && p_context->mcontext.fpformat==MC_FPFMT_XMM)
        __asm__ volatile("fxrstor %0" : : "m"(*(const uint8_t (*)[512])p_context->mcontext.fpstate));
    __asm__ volatile("mov %c[r8](%%rdi),%%r8\n\t"
                     "mov %c[r9](%%rdi),%%r9\n\t"
                     "mov %c[rbx](%%rdi),%%rbx\n\t"
                     "mov %c[rbp](%%rdi),%%rbp\n\t"
                     "mov %c[r12](%%rdi),%%r12\n\t"
                     "mov %c[r13](%%rdi),%%r13\n\t"
                     "mov %c[r14](%%rdi),%%r14\n\t"
                     "mov %c[r15](%%rdi),%%r15\n\t"
                     "mov %c[rsi](%%rdi),%%rsi\n\t"
                     "mov %c[rdx](%%rdi),%%rdx\n\t"
                     "mov %c[rax](%%rdi),%%rax\n\t"
                     "mov %c[rsp](%%rdi),%%rsp\n\t"
                     "pushq %c[rip](%%rdi)\n\t"
                     "mov %c[rcx](%%rdi),%%rcx\n\t"
                     "mov %c[rdi](%%rdi),%%rdi\n\t"
                     "ret"
                     :
                     : "D"(p_context), [r8] "i"(offsetof(GuestUcontext,mcontext.r8)),
                       [r9] "i"(offsetof(GuestUcontext,mcontext.r9)), [rbx] "i"(offsetof(GuestUcontext,mcontext.rbx)),
                       [rbp] "i"(offsetof(GuestUcontext,mcontext.rbp)), [r12] "i"(offsetof(GuestUcontext,mcontext.r12)),
                       [r13] "i"(offsetof(GuestUcontext,mcontext.r13)), [r14] "i"(offsetof(GuestUcontext,mcontext.r14)),
                       [r15] "i"(offsetof(GuestUcontext,mcontext.r15)), [rsi] "i"(offsetof(GuestUcontext,mcontext.rsi)),
                       [rdx] "i"(offsetof(GuestUcontext,mcontext.rdx)), [rax] "i"(offsetof(GuestUcontext,mcontext.rax)),
                       [rsp] "i"(offsetof(GuestUcontext,mcontext.rsp)), [rip] "i"(offsetof(GuestUcontext,mcontext.rip)),
                       [rcx] "i"(offsetof(GuestUcontext,mcontext.rcx)), [rdi] "i"(offsetof(GuestUcontext,mcontext.rdi))
                     : "memory");
    __builtin_unreachable();
}

static const RuntimeExport s_exports[]={
    {"sceKernelInstallExceptionHandler",install_handler},
    {"sceKernelRemoveExceptionHandler",remove_handler},
    {"sceKernelRaiseException",raise_exception},
    {"sigreturn",guest_sigreturn}, {"__Ux86_64_setcontext",unwind_setcontext},
};
uintptr_t runtime_exception_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_exception_report(void) {
    unsigned r=atomic_load(&s_c_raised);
    if (r) printf("Runtime: exceptions raised=%u, delivered=%u\n",r,atomic_load(&s_c_delivered));
}
#else
uintptr_t runtime_exception_resolve(const char *name) { (void)name; return 0; }
void runtime_exception_report(void) {}
#endif
