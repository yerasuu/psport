/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700
#define DEFAULT_GUARD (16*1024)

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int is_detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    /* Lowest address of the stack: the game's own (scePthreadAttrSetstack) or, once the
     * thread runs, the one it got. libSceUlt builds fibers on it. */
    void *p_stack_addr;
    struct ThreadAttr *p_next;
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
    int detached, finished, joined, host_owned, is_cancel_requested;
    /* The creator's interrupt mask, which the thread takes once its TCB is in place. */
    PsHostInterruptMask start_mask;
    /* Known once the TCB is in place; interrupts sent before then wait in early_interrupts. */
    int32_t tid;
    unsigned early_interrupts;
    PsHostRecoveryPoint exit_jump;
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *s_p_attributes;
static GuestThread *s_p_threads;
static _Thread_local GuestThread *s_p_current;
static _Thread_local int32_t s_guest_errno;
static _Thread_local void (*s_pfn_exit_hook)(void);
static const unsigned char *s_p_tls_template;
static uint64_t s_tls_filesz, s_tls_memsz, s_tls_align=16;
static size_t s_c_created, s_c_joined, s_c_exited, s_c_exit_calls;
#define C_EXIT_CALLS_LOGGED 32
static size_t s_c_cancel_requests, s_c_cancels_taken;
static size_t s_c_high_records;

void runtime_set_main_tls(const void *p_data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    s_p_tls_template=p_data; s_tls_filesz=filesz; s_tls_memsz=memsz; s_tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (s_tls_memsz+s_tls_align-1)&~(s_tls_align-1); }
static void set_gs(void *p_base) {
    const int error=ps_host_tls_set_base(p_base);
    if (error) { errno=error; perror("STOP: arch_prctl(ARCH_SET_GS)"); runtime_stop(21); }
}
/* Build TCB/static TLS for the calling host thread and point GS at it. */
/* The stack this host thread runs on, kept in the attributes scePthreadAttrGet copies. */
static void record_stack(GuestThread *p_thread) {
    if (p_thread->attr.p_stack_addr) return;
    uintptr_t va_low; size_t cb_stack;
    if (!ps_host_thread_stack(&va_low,&cb_stack)) { p_thread->attr.p_stack_addr=(void *)va_low; p_thread->attr.stack=cb_stack; }
}
static void attach(GuestThread *p_thread) {
    uint64_t off_tls=tls_offset();
    size_t cb_block=off_tls+256;
    /* Never freed, so aligned by hand: UCRT has no aligned_alloc whose memory free() takes. */
    unsigned char *p_raw=malloc(cb_block+63);
    if (!p_raw) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    unsigned char *p_block=(unsigned char *)(((uintptr_t)p_raw+63)&~(uintptr_t)63);
    memset(p_block,0,cb_block);
    if (s_tls_filesz) memcpy(p_block,s_p_tls_template,s_tls_filesz);
    uint64_t *p_tcb=(uint64_t *)(p_block+off_tls);
    static uint64_t s_dtv[3];
    p_tcb[0]=(uint64_t)(uintptr_t)p_tcb;         /* tcb_self */
    p_tcb[1]=(uint64_t)(uintptr_t)s_dtv;         /* tcb_dtv (static module only) */
    p_tcb[2]=(uint64_t)(uintptr_t)p_thread;           /* tcb_thread */
    p_thread->tls_block=p_block; p_thread->tcb=p_tcb;
    set_gs(p_tcb);
    s_p_current=p_thread;
}
/* A thread's record is its ScePthread handle, and games pack handles into 40-bit fields: a
 * hybrid lock that masks its owner to 0xffffffffff and compares it with the whole handle never
 * matches its own thread when the handle is above 40 bits, and the thread sleeps forever (a
 * loading screen that never ended, bbport #193). Records are carved from low memory in 256 KiB
 * pieces; records are never freed. */
static GuestThread *allocate_low_record(void) {
    static pthread_mutex_t s_pool_lock=PTHREAD_MUTEX_INITIALIZER;
    static unsigned char *s_p_pool;
    static size_t s_cb_left;
    const size_t cb_record=(sizeof(GuestThread)+63)&~(size_t)63, cb_piece=256*1024;
    void *p_record=NULL;
    pthread_mutex_lock(&s_pool_lock);
    if (s_cb_left<cb_record) {
        s_p_pool=runtime_low_map(cb_piece,PS_HOST_VM_READ|PS_HOST_VM_WRITE);
        s_cb_left=s_p_pool ? cb_piece : 0;
    }
    if (s_cb_left>=cb_record) { p_record=s_p_pool; s_p_pool+=cb_record; s_cb_left-=cb_record; }
    pthread_mutex_unlock(&s_pool_lock);
    return p_record;
}
static GuestThread *new_thread(void) {
    GuestThread *p_thread=allocate_low_record();
    if (!p_thread) {
        fputs("Runtime: no low memory for a thread record; its handle may not fit 40 bits\n",stderr);
        __atomic_add_fetch(&s_c_high_records,1,__ATOMIC_RELAXED);
        p_thread=calloc(1,sizeof(*p_thread));
    }
    if (!p_thread) return NULL;
    p_thread->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=DEFAULT_GUARD,.affinity=0x7f};
    return p_thread;
}
static void publish(GuestThread *p_thread) {
    runtime_exception_lock(&s_lock); p_thread->next=s_p_threads; s_p_threads=p_thread; runtime_exception_unlock(&s_lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (s_p_current) return s_p_current;
    GuestThread *p_thread=new_thread();
    if (!p_thread) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    p_thread->host=pthread_self(); p_thread->host_owned=1; p_thread->tid=ps_host_thread_id();
    snprintf(p_thread->name,sizeof(p_thread->name),"host");
    attach(p_thread); record_stack(p_thread); publish(p_thread);
    return p_thread;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *sz_name) {
    GuestThread *p_thread=runtime_thread_current();
    snprintf(p_thread->name,sizeof(p_thread->name),"%s",sz_name ? sz_name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *p_thread=runtime_thread_current();
    snprintf(p_thread->name,sizeof(p_thread->name),"main");
}
void runtime_thread_set_stack(void *p_addr, uint64_t cb_stack) {
    GuestThread *p_thread=runtime_thread_current();
    p_thread->attr.p_stack_addr=p_addr; p_thread->attr.stack=cb_stack;
}
static GuestThread *find_thread(void *p_handle) {
    GuestThread *p_found=NULL;
    runtime_exception_lock(&s_lock);
    for (GuestThread *p_thread=s_p_threads;p_thread;p_thread=p_thread->next) if (p_thread==p_handle) { p_found=p_thread; break; }
    runtime_exception_unlock(&s_lock);
    return p_found;
}
/* Under the table's lock, so the host thread cannot be joined and its id reused between
 * the lookup and the kill. */
int32_t runtime_thread_kill(void *p_handle, PsHostInterrupt interrupt) {
    int e=ESRCH;
    runtime_exception_lock(&s_lock);
    for (GuestThread *p_thread=s_p_threads;p_thread;p_thread=p_thread->next) {
        if (p_thread!=p_handle) continue;
        if (p_thread->finished || !p_thread->host) break;
        if (p_thread->tid) e=ps_host_interrupt_thread(p_thread->tid,interrupt);
        else { p_thread->early_interrupts|=1u<<interrupt; e=0; }
        break;
    }
    runtime_exception_unlock(&s_lock);
    return e==ESRCH ? ERR(3) : e ? ERR(22) : 0;
}
static ThreadAttr *find_attr(ThreadAttr **pp_slot) {
    if (!pp_slot || !*pp_slot) return NULL;
    ThreadAttr *p_found=NULL;
    runtime_exception_lock(&s_lock);
    for (ThreadAttr *p_thread_attr=s_p_attributes;p_thread_attr;p_thread_attr=p_thread_attr->p_next) if (p_thread_attr==*pp_slot && p_thread_attr->magic==ATTR_MAGIC) { p_found=p_thread_attr; break; }
    runtime_exception_unlock(&s_lock);
    return p_found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &s_guest_errno; }
int32_t *runtime_errno(void) { return &s_guest_errno; }

static ABI int32_t attr_init(ThreadAttr **pp_out) {
    if (!pp_out) return ERR(22);
    ThreadAttr *p_thread_attr=calloc(1,sizeof(*p_thread_attr));
    if (!p_thread_attr) return ERR(12);
    *p_thread_attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=DEFAULT_GUARD,.affinity=0x7f};
    runtime_exception_lock(&s_lock); p_thread_attr->p_next=s_p_attributes; s_p_attributes=p_thread_attr; runtime_exception_unlock(&s_lock);
    *pp_out=p_thread_attr; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **pp_slot) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr) return ERR(22);
    runtime_exception_lock(&s_lock);
    ThreadAttr **pp_link=&s_p_attributes;
    while (*pp_link!=p_thread_attr) pp_link=&(*pp_link)->p_next;
    *pp_link=p_thread_attr->p_next; p_thread_attr->magic=0;
    runtime_exception_unlock(&s_lock);
    free(p_thread_attr); *pp_slot=NULL; return 0;
}
static ABI int32_t attr_get(void *h_thread,ThreadAttr **pp_out) {
    ThreadAttr *p_thread_attr=find_attr(pp_out);
    if (!p_thread_attr || !h_thread) return ERR(22);
    /* A thread that ended, joined or not, is no longer found (kb #691). */
    GuestThread *p_thread=find_thread(h_thread);
    if (!p_thread || __atomic_load_n(&p_thread->finished,__ATOMIC_ACQUIRE)) return ERR(3);
    ThreadAttr *p_next=p_thread_attr->p_next;
    *p_thread_attr=p_thread->attr; p_thread_attr->magic=ATTR_MAGIC; p_thread_attr->p_next=p_next;
    p_thread_attr->is_detached=p_thread->detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **pp_slot,uint64_t cb_stack) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || cb_stack<16384) return ERR(22);
    p_thread_attr->stack=cb_stack; return 0;
}
static ABI int32_t attr_set_stack_region(ThreadAttr **pp_slot,void *p_addr,uint64_t cb_stack) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_addr || cb_stack<16384) return ERR(22);
    p_thread_attr->p_stack_addr=p_addr; p_thread_attr->stack=cb_stack; return 0;
}
static ABI int32_t attr_get_stack_region(ThreadAttr **pp_slot,void **pp_addr,uint64_t *p_size) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !pp_addr || !p_size) return ERR(22);
    *pp_addr=p_thread_attr->p_stack_addr; *p_size=p_thread_attr->stack; return 0;
}
static ABI int32_t attr_get_stack_addr(ThreadAttr **pp_slot,void **pp_addr) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !pp_addr) return ERR(22);
    *pp_addr=p_thread_attr->p_stack_addr; return 0;
}
/* libSceFiber checks a fiber's bounds against its thread's stack: the lowest address and size,
 * as pthread_attr_getstack gives them. */
static ABI int32_t thread_get_stack(void *h_thread,void **pp_addr,uint64_t *p_size) {
    GuestThread *p_thread=h_thread ? find_thread(h_thread) : runtime_thread_current();
    if (!p_thread || !pp_addr || !p_size) return 22;
    *pp_addr=p_thread->attr.p_stack_addr; *p_size=p_thread->attr.stack;
    return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **pp_slot,uint64_t *p_size) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_size) return ERR(22);
    *p_size=p_thread_attr->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **pp_slot,int state) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || (state!=0 && state!=1)) return ERR(22);
    p_thread_attr->is_detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **pp_slot,int *p_state) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_state) return ERR(22);
    *p_state=p_thread_attr->is_detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **pp_slot,int policy) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || policy<1 || policy>3) return ERR(22);
    p_thread_attr->policy=policy; return 0;
}
/* The console checks only attr and *attr before reading attr+4 (kb #691); a null out here is EINVAL
 * instead of a fault. */
static ABI int32_t attr_get_inherit(ThreadAttr **pp_slot,int *p_inherit) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_inherit) return ERR(22);
    *p_inherit=p_thread_attr->inherit; return 0;
}
static ABI int32_t attr_get_policy(ThreadAttr **pp_slot,int *p_policy) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_policy) return ERR(22);
    *p_policy=p_thread_attr->policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **pp_slot,int inherit) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || (inherit!=0 && inherit!=4)) return ERR(22);
    p_thread_attr->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **pp_slot,const int *p_param) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_param) return ERR(22);
    p_thread_attr->prio=*p_param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **pp_slot,int *p_param) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_param) return ERR(22);
    *p_param=p_thread_attr->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **pp_slot,uint64_t mask) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr) return ERR(22);
    p_thread_attr->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **pp_slot,uint64_t *p_mask) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_mask) return ERR(22);
    *p_mask=p_thread_attr->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **pp_slot,uint64_t cb_guard) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr) return ERR(22);
    p_thread_attr->guard=cb_guard; return 0;
}
static ABI int32_t attr_get_guard(ThreadAttr **pp_slot,uint64_t *p_size) {
    ThreadAttr *p_thread_attr=find_attr(pp_slot);
    if (!p_thread_attr || !p_size) return ERR(22);
    *p_size=p_thread_attr->guard; return 0;
}

static void *host_start(void *p_argument) {
    GuestThread *p_thread=p_argument;
    attach(p_thread);
    runtime_exception_lock(&s_lock);
    p_thread->tid=ps_host_thread_id();
    const unsigned early_interrupts=p_thread->early_interrupts;
    p_thread->early_interrupts=0;
    runtime_exception_unlock(&s_lock);
    ps_host_interrupts_restore(&p_thread->start_mask);
    for (int interrupt=0;interrupt<PS_HOST_INTERRUPT_COUNT;++interrupt)
        if (early_interrupts&(1u<<interrupt)) ps_host_interrupt_thread(p_thread->tid,(PsHostInterrupt)interrupt);
    record_stack(p_thread);
    ps_host_thread_set_name(p_thread->name);
    if (PS_HOST_RECOVERY_SET(p_thread->exit_jump)) {
        runtime_exception_lock(&s_lock); const size_t i_exit_call=s_c_exit_calls++; runtime_exception_unlock(&s_lock);
        if (i_exit_call<C_EXIT_CALLS_LOGGED)
            printf("Runtime: guest thread '%s' left through scePthreadExit (result %p)%s\n",p_thread->name,
                   p_thread->result,i_exit_call+1==C_EXIT_CALLS_LOGGED ? "; further ones are only counted" : "");
    } else p_thread->result=p_thread->entry(p_thread->argument);
    if (s_pfn_exit_hook) s_pfn_exit_hook();
    runtime_thread_keys_cleanup();
    runtime_exception_lock(&s_lock); p_thread->finished=1; ++s_c_exited; runtime_exception_unlock(&s_lock);
    return p_thread->result;
}
static int32_t create(GuestThread **pp_out,ThreadAttr **pp_attr_slot,GuestEntry pfn_entry,void *p_argument,const char *sz_name) {
    if (!pp_out || !pfn_entry) return ERR(22);
    ThreadAttr *p_thread_attr=NULL;
    if (pp_attr_slot && !(p_thread_attr=find_attr(pp_attr_slot))) return ERR(22);
    GuestThread *p_thread=new_thread();
    if (!p_thread) return ERR(12);
    if (p_thread_attr) { p_thread->attr=*p_thread_attr; p_thread->attr.p_next=NULL; }
    p_thread->entry=pfn_entry; p_thread->argument=p_argument; p_thread->detached=p_thread->attr.is_detached;
    snprintf(p_thread->name,sizeof(p_thread->name),"%s",sz_name ? sz_name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=p_thread->attr.stack<MIN_STACK ? MIN_STACK : p_thread->attr.stack;
    publish(p_thread);
    /* Publish the handle before the thread can run and inspect itself. */
    *pp_out=p_thread;
    /* Blocked until host_start has set GS: a signal handler that runs guest code on the thread
     * before then would find no TCB. */
    /* Faults stay open: one taken while blocked kills the process instead of reaching the GPU's
     * write tracking (psport_gpu_handle_fault), and pthread_create writes into the guest's stack. */
    ps_host_interrupts_hold(1,&p_thread->start_mask);
    int e=EINVAL;
    if (p_thread->attr.p_stack_addr) {
        /* The game's own memory, as the console runs it. glibc keeps its thread record and the
         * process's static TLS at its top and refuses a stack too small for them. */
        pthread_attr_setstack(&host,p_thread->attr.p_stack_addr,p_thread->attr.stack);
        e=pthread_create(&p_thread->host,&host,host_start,p_thread);
        if (e==EINVAL) {
            printf("Runtime: guest thread '%s': its own %llu-byte stack is too small for the host; "
                   "it runs on one of the runtime's\n",p_thread->name,(unsigned long long)p_thread->attr.stack);
            /* The attributes then name the stack it really runs on (record_stack). */
            p_thread->attr.p_stack_addr=NULL;
            pthread_attr_destroy(&host);
            pthread_attr_init(&host);
        }
    }
    if (e==EINVAL && !p_thread->attr.p_stack_addr) {
        /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
        size_t cb_stack=(size_t)stack+STACK_MARGIN;
        void *p_stack_memory=runtime_low_map(cb_stack,PS_HOST_VM_READ|PS_HOST_VM_WRITE);
        if (p_stack_memory) {
            pthread_attr_setstack(&host,p_stack_memory,cb_stack);
            /* Known before the thread runs: libSceUlt reads it (scePthreadAttrGet) right after
             * creating its workers, and builds their fibers' contexts on it. */
            p_thread->attr.p_stack_addr=p_stack_memory; p_thread->attr.stack=cb_stack;
        } else pthread_attr_setstacksize(&host,cb_stack);
        e=pthread_create(&p_thread->host,&host,host_start,p_thread);
    }
    pthread_attr_destroy(&host);
    ps_host_interrupts_restore(&p_thread->start_mask);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); runtime_stop(21); }
    if (p_thread->detached) pthread_detach(p_thread->host);
    runtime_exception_lock(&s_lock); ++s_c_created; runtime_exception_unlock(&s_lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",p_thread->name,(unsigned long long)stack,p_thread->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **pp_out,ThreadAttr **pp_attr,GuestEntry pfn_entry,void *p_argument,const char *sz_name) {
    return create(pp_out,pp_attr,pfn_entry,p_argument,sz_name);
}
static ABI int32_t thread_join(GuestThread *p_thread,void **pp_result) {
    if (!find_thread(p_thread) || p_thread->host_owned) return ERR(3);
    if (p_thread==s_p_current) return ERR(11);
    if (p_thread->detached || p_thread->joined) return ERR(22);
    p_thread->joined=1;
    void *p_value=NULL;
    int e=pthread_join(p_thread->host,&p_value);
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); runtime_stop(21); }
    if (pp_result) *pp_result=p_thread->result;
    runtime_exception_lock(&s_lock); ++s_c_joined; runtime_exception_unlock(&s_lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *p_thread) {
    if (!find_thread(p_thread)) return ERR(3);
    if (p_thread->detached) return ERR(22);
    p_thread->detached=1;
    if (!p_thread->host_owned) pthread_detach(p_thread->host);
    return 0;
}
/* FreeBSD's struct pthread_cleanup, laid over the _pthread_cleanup_info the guest's
 * pthread_cleanup_push macro keeps on its stack. */
typedef struct CleanupHandler {
    struct CleanupHandler *p_prev;
    void (ABI *pfn_routine)(void *);
    void *p_routine_arg;
    int32_t onheap;
} CleanupHandler;
static _Thread_local CleanupHandler *s_p_cleanup_handlers;
static ABI void push_cleanup_handler(void (ABI *pfn_routine)(void *),void *p_argument,CleanupHandler *p_handler) {
    p_handler->pfn_routine=pfn_routine; p_handler->p_routine_arg=p_argument; p_handler->onheap=0;
    p_handler->p_prev=s_p_cleanup_handlers; s_p_cleanup_handlers=p_handler;
}
static ABI void pop_cleanup_handler(int32_t should_execute) {
    CleanupHandler *p_top=s_p_cleanup_handlers;
    if (!p_top) return;
    s_p_cleanup_handlers=p_top->p_prev;
    if (should_execute) p_top->pfn_routine(p_top->p_routine_arg);
}
static ABI __attribute__((noreturn)) void thread_exit(void *p_value) {
    GuestThread *p_thread=runtime_thread_current();
    if (p_thread->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); runtime_stop(21); }
    while (s_p_cleanup_handlers) pop_cleanup_handler(1);
    p_thread->result=p_value;
    /* No host unwinder: guest frames have no unwind data, and on Windows the C runtime's longjmp
     * unwinds through them (RtlUnwindEx), which ends the process with STATUS_BAD_FUNCTION_TABLE. */
    PS_HOST_RECOVERY_JUMP(p_thread->exit_jump);
}
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *p_thread,int *p_prio) {
    if (!find_thread(p_thread)) return ERR(3);
    if (!p_prio) return ERR(22);
    *p_prio=p_thread->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *p_thread,int prio) {
    if (!find_thread(p_thread)) return ERR(3);
    p_thread->attr.prio=prio; return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *p_thread,uint64_t mask) {
    if (!find_thread(p_thread)) return ERR(3);
    p_thread->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *p_thread,uint64_t *p_mask) {
    if (!find_thread(p_thread)) return ERR(3);
    if (!p_mask) return ERR(22);
    *p_mask=p_thread->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *p_thread,const char *sz_name) {
    if (!find_thread(p_thread)) return ERR(3);
    if (!sz_name) return ERR(22);
    snprintf(p_thread->name,sizeof(p_thread->name),"%s",sz_name);
    if (p_thread==s_p_current) ps_host_thread_set_name(p_thread->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *p_a,GuestThread *p_b) { return p_a==p_b; }
static ABI int32_t thread_get_id(void) { return ps_host_thread_id(); }

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **pp_thread_attr) { return posix(attr_init(pp_thread_attr)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **pp_thread_attr) { return posix(attr_destroy(pp_thread_attr)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **pp_thread_attr,int s) { return posix(attr_set_detach(pp_thread_attr,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **pp_thread_attr,uint64_t s) { return posix(attr_set_stack(pp_thread_attr,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **pp_thread_attr,const int *p_param) { return posix(attr_set_param(pp_thread_attr,p_param)); }
static ABI int32_t posix_attr_set_policy(ThreadAttr **pp_thread_attr,int p) { return posix(attr_set_policy(pp_thread_attr,p)); }
static ABI int32_t posix_attr_get_policy(ThreadAttr **pp_thread_attr,int *p_policy) { return posix(attr_get_policy(pp_thread_attr,p_policy)); }
static ABI int32_t posix_attr_get_inherit(ThreadAttr **pp_thread_attr,int *p_inherit) { return posix(attr_get_inherit(pp_thread_attr,p_inherit)); }
static ABI int32_t posix_attr_set_inherit(ThreadAttr **pp_thread_attr,int inherit) { return posix(attr_set_inherit(pp_thread_attr,inherit)); }
static ABI int32_t posix_create(GuestThread **pp_thread,ThreadAttr **pp_thread_attr,GuestEntry pfn_entry,void *p_arg) { return posix(create(pp_thread,pp_thread_attr,pfn_entry,p_arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **pp_thread,ThreadAttr **pp_thread_attr,GuestEntry pfn_entry,void *p_arg,const char *sz_name) { return posix(create(pp_thread,pp_thread_attr,pfn_entry,p_arg,sz_name)); }
static ABI int32_t posix_join(GuestThread *p_thread,void **pp_result) { return posix(thread_join(p_thread,pp_result)); }
static ABI int32_t posix_detach(GuestThread *p_thread) { return posix(thread_detach(p_thread)); }
static _Thread_local int s_cancel_state, s_cancel_type;
static ABI int32_t posix_set_cancel_state(int state,int *p_old) {
    if (state!=0 && state!=1) return 22;
    if (p_old) *p_old=s_cancel_state;
    s_cancel_state=state; return 0;
}
static ABI int32_t posix_set_cancel_type(int type,int *p_old) {
    if (type!=0 && type!=2) return 22;
    if (p_old) *p_old=s_cancel_type;
    s_cancel_type=type; return 0;
}
/* Deferred cancellation: a request is taken when the target reaches pthread_testcancel. The
 * blocking calls that are cancellation points on FreeBSD (waits, sleeps, I/O) do not take it, and
 * asynchronous cancellation is held to the next pthread_testcancel too; the report counts both. */
static ABI int32_t thread_cancel(GuestThread *p_thread) {
    if (!find_thread(p_thread) || p_thread->host_owned) return ERR(3);
    runtime_exception_lock(&s_lock);
    const int is_first_request=!s_c_cancel_requests && !p_thread->is_cancel_requested;
    if (!p_thread->is_cancel_requested) ++s_c_cancel_requests;
    p_thread->is_cancel_requested=1;
    runtime_exception_unlock(&s_lock);
    if (is_first_request)
        printf("Runtime: guest thread '%s' asked to cancel; it ends at its next pthread_testcancel\n",p_thread->name);
    return 0;
}
static ABI int32_t posix_cancel(GuestThread *p_thread) { return posix(thread_cancel(p_thread)); }
static ABI void thread_test_cancel(void) {
    GuestThread *p_thread=s_p_current;
    if (!p_thread || p_thread->host_owned || !p_thread->is_cancel_requested || s_cancel_state) return;
    runtime_exception_lock(&s_lock); ++s_c_cancels_taken; runtime_exception_unlock(&s_lock);
    thread_exit((void *)1); /* PTHREAD_CANCELED */
}

/* Suspending a thread and reading its user context need it stopped only in guest code, as the
 * console's kernel stops it on its way back to user mode; the runtime has no hook where a thread
 * leaves its own code for the game's, and stopped elsewhere it may hold any host lock its suspender
 * then waits for. Until there is one these fail with ENOSYS, counted. */
static uint64_t s_c_user_context_calls;
static int32_t user_context_unavailable(const char *sz_function) {
    if (!__atomic_fetch_add(&s_c_user_context_calls,1,__ATOMIC_RELAXED)) {
        printf("Runtime: %s not available (threads are not suspended): ENOSYS\n",sz_function);
        runtime_calls_hle_stub("pthread_suspend/resume/get/set_user_context_np (ENOSYS)",&s_c_user_context_calls);
    }
    return 78;
}
static ABI int32_t posix_suspend_context(void *h_thread) { (void)h_thread; return user_context_unavailable("pthread_suspend_user_context_np"); }
static ABI int32_t posix_resume_context(void *h_thread) { (void)h_thread; return user_context_unavailable("pthread_resume_user_context_np"); }
static ABI int32_t posix_get_context(void *h_thread,void *p_out_context) {
    (void)h_thread; (void)p_out_context;
    return user_context_unavailable("pthread_get_user_context_np");
}
static ABI int32_t posix_set_context(void *h_thread,const void *p_context) {
    (void)h_thread; (void)p_context;
    return user_context_unavailable("pthread_set_user_context_np");
}

int32_t runtime_thread_spawn(void **pp_out,void *(ABI *pfn_entry)(void *),void *p_argument,uint64_t stack,const char *sz_name) {
    ThreadAttr *p_attr=NULL;
    int32_t r=attr_init(&p_attr);
    if (!r && stack) r=attr_set_stack(&p_attr,stack);
    if (!r) r=create((GuestThread **)pp_out,&p_attr,pfn_entry,p_argument,sz_name);
    if (p_attr) attr_destroy(&p_attr);
    return posix(r);
}
void runtime_thread_set_exit_hook(void (*pfn_hook)(void)) { s_pfn_exit_hook=pfn_hook; }
int32_t runtime_thread_join_spawned(void *p_thread,void **pp_result) {
    return posix(thread_join((GuestThread *)p_thread,pp_result));
}

uintptr_t runtime_thread_resolve(const char *sz_name) {
    static const struct { const char *sz_nid; void *p_function; } s_table[]={
        {"aI+OeCz8xrQ",thread_self}, {"EotR8a3ASf4",thread_self},
        {"9BcDykPmo1I",guest_error},
        {"nsYoNRywwNg",attr_init}, {"62KCwEMmzcM",attr_destroy},
        {"x1X76arYMxU",attr_get}, {"8+s5BzZjxSg",attr_affinity},
        {"UTXzJbWhhTE",attr_set_stack}, {"-Wreprtu0Qs",attr_set_detach},
        {"4+h9EzwKF4I",attr_set_policy}, {"DzES9hQF4f4",attr_set_param},
        {"3qxgM4ezETA",attr_set_affinity}, {"eXbUSpEaTsA",attr_set_inherit},
        {"JaRMy+QcpeU",attr_get_detach}, {"-fA+7ZlGDQs",attr_get_stack},
        {"FXPWHNk8Of0",attr_get_param}, {"El+cQ20DynU",attr_set_guard},
        {"txHtngJ+eyc",attr_get_guard},
        {"NMyIQ9WgWbU",attr_get_policy}, {"lpMP8HhkBbg",attr_get_inherit},
        {"RtLRV-pBTTY",posix_attr_get_policy}, {"oLjPqUKhzes",posix_attr_get_inherit}, {"7ZlAakEf0Qg",posix_attr_set_inherit},
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
        {"euKRgm0Vn2M",posix_attr_set_param}, {"JarMIy8kKEY",posix_attr_set_policy}, {"OxhIB8LB-PQ",posix_create},
        {"Jmi+9w9u0E4",posix_create_name}, {"h9CcP3J0oVM",posix_join},
        {"+U1R4WtXvoc",posix_detach}, {"FJrT5LuUBAU",thread_exit}, {"7Xl257M4VNI",thread_equal},
        {"lZzFeSxPl08",posix_set_cancel_state}, {"2dEhvvjlq30",posix_set_cancel_type},
        {"qBDmpCyGssE",thread_cancel}, {"0D4-FVvEikw",posix_cancel},
        {"LapIb799SSE",thread_test_cancel}, {"nYBrkGDqxh8",thread_test_cancel},
        {"1xvtUVx1-Sg",push_cleanup_handler}, {"iWsFlYMf3Kw",pop_cleanup_handler},
        {"cfjAjVTFG6A",posix_suspend_context}, {"QRdE7dBfNks",posix_resume_context},
        {"YkGOXpJEtO8",posix_get_context}, {"el9stmu6290",posix_set_context},
        {"4e9dMKt+UYA",posix_suspend_context}, {"BYM3L--ojzI",posix_resume_context},
    };
    for (size_t i=0;i<sizeof(s_table)/sizeof(*s_table);++i)
        if (runtime_same_nid(sz_name,s_table[i].sz_nid)) return (uintptr_t)s_table[i].p_function;
    return 0;
}
void runtime_thread_report(void) {
    if (!runtime_exception_report_lock(&s_lock,"thread")) return;
    printf("Runtime: guest threads created=%zu, exited=%zu (through scePthreadExit %zu), joined=%zu\n",s_c_created,
           s_c_exited,s_c_exit_calls,s_c_joined);
    if (s_c_cancel_requests)
        printf("Runtime: thread cancellation: %zu requested, %zu taken at pthread_testcancel (blocking "
               "cancellation points and asynchronous cancellation are not honoured)\n",s_c_cancel_requests,
               s_c_cancels_taken);
    if (s_c_high_records)
        printf("Runtime: %zu thread records outside low memory (handles above 40 bits)\n",s_c_high_records);
    runtime_exception_unlock(&s_lock);
}
