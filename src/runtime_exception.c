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
#include <signal.h>
#include <stdatomic.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define GUEST_SIGNALS 128
#define GUEST_SIGUSR1 30
/* SIGUSR1 stays free for host libraries; SIGUSR2, SIGALRM and SIGPROF are probe.c's thread dumps,
 * watchdog and the wait sampler. */
#define HOST_SIGNAL (SIGRTMIN+3)

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

static _Atomic(GuestHandler) handlers[GUEST_SIGNALS];
static _Atomic unsigned raised, delivered;

/* The signals libkernel lets a game handle (shadPS4's orbis_allowed_signals). */
static int allowed(int sig) { return sig==1 || sig==4 || sig==8 || sig==10 || sig==11 || sig==GUEST_SIGUSR1; }

static void deliver(int sig, siginfo_t *info, void *raw) {
    (void)sig; (void)info;
    GuestHandler handler=atomic_load(&handlers[GUEST_SIGUSR1]);
    if (!handler) return;
    ucontext_t *host=raw;
    greg_t *r=host->uc_mcontext.gregs;
    /* The handler may call back into the runtime, which sets both errnos. */
    int host_errno=errno;
    int32_t guest_errno=*runtime_errno();
    GuestUcontext context;
    memset(&context,0,sizeof(context));
    GuestMcontext *m=&context.mcontext;
    m->rdi=(uint64_t)r[REG_RDI]; m->rsi=(uint64_t)r[REG_RSI]; m->rdx=(uint64_t)r[REG_RDX]; m->rcx=(uint64_t)r[REG_RCX];
    m->r8=(uint64_t)r[REG_R8]; m->r9=(uint64_t)r[REG_R9]; m->rax=(uint64_t)r[REG_RAX]; m->rbx=(uint64_t)r[REG_RBX];
    m->rbp=(uint64_t)r[REG_RBP]; m->r10=(uint64_t)r[REG_R10]; m->r11=(uint64_t)r[REG_R11]; m->r12=(uint64_t)r[REG_R12];
    m->r13=(uint64_t)r[REG_R13]; m->r14=(uint64_t)r[REG_R14]; m->r15=(uint64_t)r[REG_R15];
    m->rip=(uint64_t)r[REG_RIP]; m->rsp=(uint64_t)r[REG_RSP]; m->rflags=(uint64_t)r[REG_EFL];
    m->cs=(uint64_t)r[REG_CSGSFS]&0xffff;
    m->trapno=(uint32_t)r[REG_TRAPNO]; m->err=(uint64_t)r[REG_ERR];
    m->len=sizeof(*m);
    /* fxsave layout as FreeBSD's savefpu begins: a collector may find pointers spilled to xmm. */
    if (host->uc_mcontext.fpregs) {
        memcpy(m->fpstate,host->uc_mcontext.fpregs,512);
        m->fpformat=0x10002; /* _MC_FPFMT_XMM */
        m->ownedfp=0x20001;  /* _MC_FPOWNED_FPU */
    }
    /* The guest's FS base is its TCB, which the runtime keeps in GS. */
    unsigned long tcb=0;
    if (!syscall(SYS_arch_prctl,ARCH_GET_GS,&tcb)) { m->fsbase=tcb; m->flags=0x2; /* _MC_HASBASES */ }
    if (!atomic_fetch_add(&delivered,1)) {
        static const char first[]="Runtime: first guest exception delivered to its handler\n";
        /* write, not stdio: this runs in a signal handler. */
        ssize_t written=write(2,first,sizeof(first)-1); (void)written;
    }
    handler(GUEST_SIGUSR1,&context);
    /* As on the console, register changes the handler makes take effect on return. The FPU area
     * is a copy for the handler to read and is not restored. */
    r[REG_RDI]=(greg_t)m->rdi; r[REG_RSI]=(greg_t)m->rsi; r[REG_RDX]=(greg_t)m->rdx; r[REG_RCX]=(greg_t)m->rcx;
    r[REG_R8]=(greg_t)m->r8; r[REG_R9]=(greg_t)m->r9; r[REG_RAX]=(greg_t)m->rax; r[REG_RBX]=(greg_t)m->rbx;
    r[REG_RBP]=(greg_t)m->rbp; r[REG_R10]=(greg_t)m->r10; r[REG_R11]=(greg_t)m->r11; r[REG_R12]=(greg_t)m->r12;
    r[REG_R13]=(greg_t)m->r13; r[REG_R14]=(greg_t)m->r14; r[REG_R15]=(greg_t)m->r15;
    r[REG_RIP]=(greg_t)m->rip; r[REG_RSP]=(greg_t)m->rsp; r[REG_EFL]=(greg_t)m->rflags;
    *runtime_errno()=guest_errno;
    errno=host_errno;
}

static ABI int32_t install_handler(int sig, GuestHandler handler) {
    if (!allowed(sig)) return ERR(22);
    GuestHandler none=NULL;
    if (!atomic_compare_exchange_strong(&handlers[sig],&none,handler)) return ERR(35);
    if (sig==GUEST_SIGUSR1) {
        /* SA_RESTART so that file reads and futex waits the signal interrupts resume by themselves;
         * the runtime's sleeps retry on EINTR. Not SA_ONSTACK: the collector scans the stack the
         * handler runs on as the thread's own. */
        struct sigaction action={0};
        action.sa_sigaction=deliver;
        action.sa_flags=SA_SIGINFO|SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(HOST_SIGNAL,&action,NULL)) { atomic_store(&handlers[sig],NULL); return ERR(runtime_guest_errno(errno)); }
    }
    printf("Runtime: guest exception handler for signal %d installed at %p%s\n",sig,(void *)handler,
           sig==GUEST_SIGUSR1 ? "" : " (host faults are not forwarded to it)");
    return 0;
}
static ABI int32_t remove_handler(int sig) {
    if (!allowed(sig)) return ERR(22);
    atomic_store(&handlers[sig],NULL);
    return 0;
}
static ABI int32_t raise_exception(void *thread, int sig) {
    if (sig!=GUEST_SIGUSR1) return ERR(22);
    int32_t result=runtime_thread_kill(thread,HOST_SIGNAL);
    if (!result && !atomic_fetch_add(&raised,1))
        printf("Runtime: first guest exception raised (signal %d) to thread %p\n",sig,thread);
    return result;
}

static const RuntimeExport exports[]={
    {"sceKernelInstallExceptionHandler",install_handler},
    {"sceKernelRemoveExceptionHandler",remove_handler},
    {"sceKernelRaiseException",raise_exception},
};
uintptr_t runtime_exception_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_exception_report(void) {
    unsigned r=atomic_load(&raised);
    if (r) printf("Runtime: exceptions raised=%u, delivered=%u\n",r,atomic_load(&delivered));
}
#else
uintptr_t runtime_exception_resolve(const char *name) { (void)name; return 0; }
void runtime_exception_report(void) {}
#endif
