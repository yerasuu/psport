/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <asm/prctl.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    /* Lowest address of the stack: the game's own (scePthreadAttrSetstack) or, once the
     * thread runs, the one it got. libSceUlt builds fibers on it. */
    void *stack_addr;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
    /* The creator's signal mask, which the thread takes once its TCB is in place. */
    sigset_t start_mask;
    jmp_buf exit_jump;
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
static void set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); runtime_stop(21); }
}
/* Build TCB/static TLS for the calling host thread and point GS at it. */
/* The stack this host thread runs on, kept in the attributes scePthreadAttrGet copies. */
static void record_stack(GuestThread *t) {
    if (t->attr.stack_addr) return;
    pthread_attr_t self;
    void *addr=NULL; size_t size=0;
    if (pthread_getattr_np(pthread_self(),&self)) return;
    if (!pthread_attr_getstack(&self,&addr,&size)) { t->attr.stack_addr=addr; t->attr.stack=size; }
    pthread_attr_destroy(&self);
}
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=aligned_alloc(64,(total+63)&~(size_t)63);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    set_gs(tcb);
    current=t;
}
static GuestThread *new_thread(void) {
    GuestThread *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    runtime_exception_lock(&lock); t->next=threads; threads=t; runtime_exception_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); record_stack(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
}
void runtime_thread_set_stack(void *addr, uint64_t size) {
    GuestThread *t=runtime_thread_current();
    t->attr.stack_addr=addr; t->attr.stack=size;
}
static GuestThread *find_thread(void *handle) {
    GuestThread *found=NULL;
    runtime_exception_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle) { found=t; break; }
    runtime_exception_unlock(&lock);
    return found;
}
/* Under the table's lock, so the host thread cannot be joined and its pthread_t reused between
 * the lookup and the kill. */
int32_t runtime_thread_kill(void *handle, int host_signal) {
    int e=ESRCH;
    runtime_exception_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next)
        if (t==handle) { if (!t->finished && t->host) e=pthread_kill(t->host,host_signal); break; }
    runtime_exception_unlock(&lock);
    return e==ESRCH ? ERR(3) : e ? ERR(22) : 0;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    runtime_exception_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    runtime_exception_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    runtime_exception_lock(&lock); a->next=attributes; attributes=a; runtime_exception_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    runtime_exception_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    runtime_exception_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_set_stack_region(ThreadAttr **slot,void *addr,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !addr || size<16384) return ERR(22);
    a->stack_addr=addr; a->stack=size; return 0;
}
static ABI int32_t attr_get_stack_region(ThreadAttr **slot,void **addr,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !addr || !size) return ERR(22);
    *addr=a->stack_addr; *size=a->stack; return 0;
}
static ABI int32_t attr_get_stack_addr(ThreadAttr **slot,void **addr) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !addr) return ERR(22);
    *addr=a->stack_addr; return 0;
}
/* libSceFiber checks a fiber's bounds against its thread's stack: the lowest address and size,
 * as pthread_attr_getstack gives them. */
static ABI int32_t thread_get_stack(void *thread,void **addr,uint64_t *size) {
    GuestThread *t=thread ? find_thread(thread) : runtime_thread_current();
    if (!t || !addr || !size) return 22;
    *addr=t->attr.stack_addr; *size=t->attr.stack;
    return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
}
static void *host_start(void *p) {
    GuestThread *t=p;
    attach(t);
    pthread_sigmask(SIG_SETMASK,&t->start_mask,NULL);
    record_stack(t);
    set_host_name(t->name);
    if (!setjmp(t->exit_jump)) t->result=t->entry(t->argument);
    runtime_thread_keys_cleanup();
    runtime_exception_lock(&lock); t->finished=1; ++exited; runtime_exception_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    /* Blocked until host_start has set GS: a signal handler that runs guest code on the thread
     * before then would find no TCB. */
    /* Faults stay open: one taken while blocked kills the process instead of reaching the GPU's
     * write tracking (bbgpu_handle_fault), and pthread_create writes into the guest's stack. */
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all,SIGSEGV); sigdelset(&all,SIGBUS); sigdelset(&all,SIGILL); sigdelset(&all,SIGFPE);
    sigdelset(&all,SIGTRAP);
    pthread_sigmask(SIG_SETMASK,&all,&t->start_mask);
    int e=EINVAL;
    if (t->attr.stack_addr) {
        /* The game's own memory, as the console runs it. glibc keeps its thread record and the
         * process's static TLS at its top and refuses a stack too small for them. */
        pthread_attr_setstack(&host,t->attr.stack_addr,t->attr.stack);
        e=pthread_create(&t->host,&host,host_start,t);
        if (e==EINVAL) {
            printf("Runtime: guest thread '%s': its own %llu-byte stack is too small for the host; "
                   "it runs on one of the runtime's\n",t->name,(unsigned long long)t->attr.stack);
            /* The attributes then name the stack it really runs on (record_stack). */
            t->attr.stack_addr=NULL;
            pthread_attr_destroy(&host);
            pthread_attr_init(&host);
        }
    }
    if (e==EINVAL && !t->attr.stack_addr) {
        /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
        size_t stack_bytes=(size_t)stack+STACK_MARGIN;
        void *stack_memory=runtime_low_map(stack_bytes,PROT_READ|PROT_WRITE);
        if (stack_memory) {
            pthread_attr_setstack(&host,stack_memory,stack_bytes);
            /* Known before the thread runs: libSceUlt reads it (scePthreadAttrGet) right after
             * creating its workers, and builds their fibers' contexts on it. */
            t->attr.stack_addr=stack_memory; t->attr.stack=stack_bytes;
        } else pthread_attr_setstacksize(&host,stack_bytes);
        e=pthread_create(&t->host,&host,host_start,t);
    }
    pthread_attr_destroy(&host);
    pthread_sigmask(SIG_SETMASK,&t->start_mask,NULL);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); runtime_stop(21); }
    if (t->detached) pthread_detach(t->host);
    runtime_exception_lock(&lock); ++created; runtime_exception_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    if (t==current) return ERR(11);
    if (t->detached || t->joined) return ERR(22);
    t->joined=1;
    void *value=NULL;
    int e=pthread_join(t->host,&value);
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); runtime_stop(21); }
    if (result) *result=t->result;
    runtime_exception_lock(&lock); ++joined_count; runtime_exception_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); runtime_stop(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    longjmp(t->exit_jump,1);
}
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio; return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    if (!find_thread(t)) return ERR(3);
    t->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!find_thread(t)) return ERR(3);
    if (!mask) return ERR(22);
    *mask=t->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) set_host_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }
static ABI int32_t thread_get_id(void) { return (int32_t)gettid(); }

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }
static ABI int32_t posix_detach(GuestThread *t) { return posix(thread_detach(t)); }
static _Thread_local int cancel_state, cancel_type;
static ABI int32_t posix_set_cancel_state(int state,int *old) {
    if (state!=0 && state!=1) return 22;
    if (old) *old=cancel_state;
    cancel_state=state; return 0;
}
static ABI int32_t posix_set_cancel_type(int type,int *old) {
    if (type!=0 && type!=2) return 22;
    if (old) *old=cancel_type;
    cancel_type=type; return 0;
}

int32_t runtime_thread_spawn(void **out,void *(ABI *entry)(void *),void *argument,uint64_t stack,const char *name) {
    ThreadAttr *attr=NULL;
    int32_t r=attr_init(&attr);
    if (!r && stack) r=attr_set_stack(&attr,stack);
    if (!r) r=create((GuestThread **)out,&attr,entry,argument,name);
    if (attr) attr_destroy(&attr);
    return posix(r);
}
int32_t runtime_thread_join_spawned(void *thread,void **result) {
    return posix(thread_join((GuestThread *)thread,result));
}

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ",thread_self}, {"EotR8a3ASf4",thread_self},
        {"9BcDykPmo1I",guest_error},
        {"nsYoNRywwNg",attr_init}, {"62KCwEMmzcM",attr_destroy},
        {"x1X76arYMxU",attr_get}, {"8+s5BzZjxSg",attr_affinity},
        {"UTXzJbWhhTE",attr_set_stack}, {"-Wreprtu0Qs",attr_set_detach},
        {"4+h9EzwKF4I",attr_set_policy}, {"DzES9hQF4f4",attr_set_param},
        {"3qxgM4ezETA",attr_set_affinity}, {"eXbUSpEaTsA",attr_set_inherit},
        {"JaRMy+QcpeU",attr_get_detach}, {"-fA+7ZlGDQs",attr_get_stack},
        {"FXPWHNk8Of0",attr_get_param}, {"El+cQ20DynU",attr_set_guard},
        {"Bvn74vj6oLo",attr_set_stack_region}, {"Ru36fiTtJzA",attr_get_stack_addr},
        {"-quPa4SEJUw",attr_get_stack_region},
        {"ruZ9hhQ8oUk",thread_get_stack},
        {"6UgtwV+0zb4",thread_create}, {"onNY9Byn-W8",thread_join},
        {"4qGrR6eoP9Y",thread_detach}, {"3kg7rT0NQIs",thread_exit},
        {"T72hz6ffq08",thread_yield}, {"1tKyG7RlMJo",thread_get_prio},
        {"W0Hpm2X0uPE",thread_set_prio}, {"bt3CTBKmGyI",thread_set_affinity}, {"rcrVFJsQWRY",thread_get_affinity},
        {"GBUY7ywdULE",thread_rename}, {"3PtV6p3QNX4",thread_equal}, {"EI-5-jlq2dE",thread_get_id},
        {"wtkt-teR1so",posix_attr_init}, {"zHchY8ft5pk",posix_attr_destroy},
        {"E+tyo3lp5Lw",posix_attr_set_detach}, {"2Q0z6rnBrTE",posix_attr_set_stack},
        {"euKRgm0Vn2M",posix_attr_set_param}, {"OxhIB8LB-PQ",posix_create},
        {"Jmi+9w9u0E4",posix_create_name}, {"h9CcP3J0oVM",posix_join},
        {"+U1R4WtXvoc",posix_detach}, {"FJrT5LuUBAU",thread_exit}, {"7Xl257M4VNI",thread_equal},
        {"lZzFeSxPl08",posix_set_cancel_state}, {"2dEhvvjlq30",posix_set_cancel_type},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (runtime_same_nid(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_thread_report(void) {
    runtime_exception_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    runtime_exception_unlock(&lock);
}
#else
uintptr_t runtime_thread_resolve(const char *name) { (void)name; return 0; }
void runtime_thread_report(void) {}
void runtime_thread_attach_main(void) {}
void runtime_thread_set_stack(void *addr, uint64_t size) { (void)addr; (void)size; }
void runtime_set_main_tls(const void *d,uint64_t f,uint64_t m,uint64_t a) { (void)d;(void)f;(void)m;(void)a; }
#endif
