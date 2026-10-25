/* libkernel exception handlers (libkernel_unity): the guest's handler runs on the target thread,
 * on that thread's own stack, with a FreeBSD ucontext of the interrupted registers. Unity's
 * IL2CPP garbage collector suspends each thread this way (sceKernelRaiseException with SIGUSR1)
 * and scans its stack from the saved rsp. Guest code already runs on the host thread with GS at
 * its TCB, so the handler is called straight from the host signal handler. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define GUEST_SIGNALS 128
#define GUEST_SIGUSR1 30
/* How long a thread in runtime_exception_wait sleeps before it looks for a raise held back, with a
 * handler installed and without one (only a handler installed during the sleep can need it). */
#define RAISE_SLICE_NS 2000000
#define IDLE_SLICE_NS 100000000
#define REPORT_LOCK_WAIT_NS 100000000
#define MC_FLAG_MASK 0x7u
#define MC_FPFMT_XMM 0x10002
#define MC_FPOWNED_FPU 0x20001
#define GUEST_EFAULT 14
#define GUEST_EINVAL 22

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
    if (!s_held && s_is_deferred) ps_host_interrupt_thread(ps_host_thread_id(),PS_HOST_INTERRUPT_GUEST_EXCEPTION);
}
void runtime_exception_lock(pthread_mutex_t *p_lock) {
    runtime_exception_hold();
    pthread_mutex_lock(p_lock);
}
void runtime_exception_unlock(pthread_mutex_t *p_lock) {
    pthread_mutex_unlock(p_lock);
    runtime_exception_release();
}
int runtime_exception_report_lock(pthread_mutex_t *p_lock, const char *sz_report) {
    runtime_exception_hold();
    struct timespec end;
    int error=ps_host_time_read(PS_HOST_CLOCK_MONOTONIC,&end);
    if (!error) {
        end.tv_nsec+=REPORT_LOCK_WAIT_NS;
        end.tv_sec+=end.tv_nsec/1000000000; end.tv_nsec%=1000000000;
        error=pthread_mutex_clocklock(p_lock,CLOCK_MONOTONIC,&end);
        if (!error) return 1;
    }
    runtime_exception_release();
    if (error==ETIMEDOUT)
        printf("Runtime: %s report skipped: its lock stayed held for %d ms, another thread keeps it at exit\n",
               sz_report,REPORT_LOCK_WAIT_NS/1000000);
    else printf("Runtime: %s report skipped: its lock could not be taken: %s\n",sz_report,strerror(error));
    return 0;
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

/* GuestMcontext's general registers in PsHostRegister's order. */
static const size_t s_off_registers[PS_HOST_REG_COUNT]={
    offsetof(GuestMcontext,rax), offsetof(GuestMcontext,rbx), offsetof(GuestMcontext,rcx), offsetof(GuestMcontext,rdx),
    offsetof(GuestMcontext,rsi), offsetof(GuestMcontext,rdi), offsetof(GuestMcontext,rbp), offsetof(GuestMcontext,rsp),
    offsetof(GuestMcontext,r8), offsetof(GuestMcontext,r9), offsetof(GuestMcontext,r10), offsetof(GuestMcontext,r11),
    offsetof(GuestMcontext,r12), offsetof(GuestMcontext,r13), offsetof(GuestMcontext,r14), offsetof(GuestMcontext,r15),
    offsetof(GuestMcontext,rip), offsetof(GuestMcontext,rflags),
};
static void deliver(PsHostFaultContext *p_host) {
    GuestHandler pfn_handler=atomic_load(&s_handlers[GUEST_SIGUSR1]);
    if (!pfn_handler) { s_is_deferred=0; return; }
    if (s_held) { s_is_deferred=1; return; }
    s_is_deferred=0;
    /* The handler may call back into the runtime, which sets both errnos. */
    int host_errno=errno;
    int32_t guest_errno=*runtime_errno();
    GuestUcontext context;
    memset(&context,0,sizeof(context));
    GuestMcontext *p_mcontext=&context.mcontext;
    for (int reg=0;reg<PS_HOST_REG_COUNT;++reg) {
        const uint64_t value=ps_host_fault_register(p_host,(PsHostRegister)reg);
        memcpy((char *)p_mcontext+s_off_registers[reg],&value,sizeof(value));
    }
    PsHostFaultCpu cpu;
    ps_host_fault_cpu(p_host,&cpu);
    p_mcontext->cs=cpu.code_selector;
    p_mcontext->trapno=cpu.trap_number; p_mcontext->err=cpu.error_code;
    p_mcontext->len=sizeof(*p_mcontext);
    /* fxsave layout as FreeBSD's savefpu begins: a collector may find pointers spilled to xmm. */
    if (cpu.p_fxsave) {
        memcpy(p_mcontext->fpstate,cpu.p_fxsave,512);
        p_mcontext->fpformat=0x10002; /* _MC_FPFMT_XMM */
        p_mcontext->ownedfp=0x20001;  /* _MC_FPOWNED_FPU */
    }
    /* The guest's FS base is its TCB, which the runtime keeps in GS. */
    uintptr_t tcb=0;
    if (!ps_host_tls_base(&tcb)) { p_mcontext->fsbase=tcb; p_mcontext->flags=0x2; /* _MC_HASBASES */ }
    if (!atomic_fetch_add(&s_c_delivered,1)) {
        static const char s_sz_first[]="Runtime: first guest exception delivered to its handler\n";
        /* write, not stdio: this runs in a signal handler. */
        (void)ps_host_fs_write(2,s_sz_first,sizeof(s_sz_first)-1);
    }
    s_delivery=(Delivery){1,host_errno,guest_errno};
    pfn_handler(GUEST_SIGUSR1,&context);
    s_delivery.is_active=0;
    /* As on the console, register changes the handler makes take effect on return. The FPU area
     * is a copy for the handler to read and is not restored. */
    for (int reg=0;reg<PS_HOST_REG_COUNT;++reg) {
        uint64_t value;
        memcpy(&value,(const char *)p_mcontext+s_off_registers[reg],sizeof(value));
        ps_host_fault_set_register(p_host,(PsHostRegister)reg,value);
    }
    *runtime_errno()=guest_errno;
    errno=host_errno;
}

static ABI int32_t install_handler(int sig, GuestHandler pfn_handler) {
    if (!allowed(sig)) return ERR(22);
    GuestHandler pfn_none=NULL;
    if (!atomic_compare_exchange_strong(&s_handlers[sig],&pfn_none,pfn_handler)) return ERR(35);
    if (sig==GUEST_SIGUSR1) {
        /* Restarting, so that file reads and futex waits the interrupt cuts short resume by
         * themselves; the runtime's sleeps retry on EINTR. On the thread's own stack: the collector
         * scans the stack the handler runs on as the thread's. */
        const int error=ps_host_interrupt_install(PS_HOST_INTERRUPT_GUEST_EXCEPTION,deliver,PS_HOST_INTERRUPT_RESTART);
        if (error) { atomic_store(&s_handlers[sig],NULL); return ERR(runtime_guest_errno(error)); }
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
    int32_t result=runtime_thread_kill(p_thread,PS_HOST_INTERRUPT_GUEST_EXCEPTION);
    if (!result && !atomic_fetch_add(&s_c_raised,1)) {
        /* write, not stdio: the thread may stop in the handler while it holds stdout's lock. */
        char sz_line[96];
        int cb_line=snprintf(sz_line,sizeof(sz_line),"Runtime: first guest exception raised (signal %d) to thread %p\n",sig,p_thread);
        (void)ps_host_fs_write(2,sz_line,(size_t)cb_line);
    }
    return result;
}

/* FreeBSD's sigreturn: the whole context in one step (ps_host_resume), so nothing is written below
 * the restored rsp (an interrupted leaf function's red zone survives). */
static ABI int32_t guest_sigreturn(const GuestUcontext *p_context) {
    if (!p_context) { *runtime_errno()=GUEST_EFAULT; return -1; }
    const GuestMcontext *p_mcontext=&p_context->mcontext;
    if ((p_mcontext->flags & ~MC_FLAG_MASK) || (p_mcontext->cs & 3)!=3) { *runtime_errno()=GUEST_EINVAL; return -1; }
    uint64_t registers[PS_HOST_REG_COUNT];
    for (int reg=0;reg<PS_HOST_REG_COUNT;++reg) memcpy(&registers[reg],(const char *)p_mcontext+s_off_registers[reg],sizeof(registers[reg]));
    /* With the FPU not owned the host resets it, as FreeBSD does for _MC_FPOWNED_NONE. */
    const int is_fpu_owned=p_mcontext->ownedfp==MC_FPOWNED_FPU && p_mcontext->fpformat==MC_FPFMT_XMM;
    /* Only the guest's SIGUSR1 has a host signal behind it; the others keep their host state. */
    const int is_exception_masked=(p_context->sigmask[0] & 1u<<(GUEST_SIGUSR1-1))!=0;
    runtime_kernel_set_signal_mask(p_context->sigmask);
    if (s_delivery.is_active) {
        s_delivery.is_active=0;
        *runtime_errno()=s_delivery.guest_errno;
        errno=s_delivery.host_errno;
    }
    ps_host_resume(registers,is_fpu_owned ? p_mcontext->fpstate : NULL,RUNTIME_EXCEPTION_SIGNAL,is_exception_masked);
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
int runtime_exception_is_delivering(void) { return s_delivery.is_active; }
int runtime_exception_is_holding(void) { return s_held!=0; }
