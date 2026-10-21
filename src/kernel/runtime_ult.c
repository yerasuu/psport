/* libSceUlt: user-level threads. The console switches its user-level threads on a runtime's worker
 * threads in user space; here each one runs on a host thread of its own and the runtime's worker
 * count is a set of slots: a user-level thread holds one while it runs and gives it up while it
 * waits in a ULT primitive, joins or yields, so one waiting on another never holds the other back.
 * The synchronization objects live on the host beside the guest's opaque objects. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>

#define ERR_NULL ((int32_t)0x80810001)
#define ERR_ALIGNMENT ((int32_t)0x80810002)
#define ERR_RANGE ((int32_t)0x80810003)
#define ERR_INVALID ((int32_t)0x80810004)
#define ERR_PERMISSION ((int32_t)0x80810005)
#define ERR_STATE ((int32_t)0x80810006)
#define ERR_BUSY ((int32_t)0x80810007)
#define ERR_AGAIN ((int32_t)0x80810008)
#define ERR_NOT_INITIALIZED ((int32_t)0x8081000A)
#define END_OF_LIST UINT32_MAX
#define MAX_READERS 0xFFFF
#define LONG_WAIT_S 10
#define MIN_STACK 0x4000

typedef int32_t (ABI *UltEntry)(uint64_t);
typedef struct {
    char signature[12];
    uint32_t build;
    uint64_t stack, affinity;
    int32_t priority, inherit;
    unsigned char reserved[88];
} RuntimeOptParam;
_Static_assert(sizeof(RuntimeOptParam)==128,"SceUltUlthreadRuntimeOptParam layout");
typedef struct {
    char signature[12];
    uint32_t build;
    uint32_t attribute;
    unsigned char reserved[108];
} OptParam;
_Static_assert(sizeof(OptParam)==128,"SceUlt*OptParam layout");
static const char s_sz_signature[]="ubilamtl\0cig";

typedef struct Ulthread {
    const void *p_object;
    struct Runtime *p_runtime;
    UltEntry pfn_entry;
    uint64_t argument, serial, slot_ticket;
    void *p_thread;
    const char *sz_blocked_in;
    struct Ulthread *p_joining;
    int32_t status;
    int has_finished, has_joiner, has_ticket;
    struct Ulthread *p_next;
} Ulthread;
typedef struct Runtime {
    const void *p_object;
    uint64_t cb_stack, c_slot_tickets, c_slot_grants;
    uint32_t c_max, c_live;
    pthread_cond_t slots, done;
    struct Runtime *p_next;
} Runtime;

/* A caller queued on a mutex or a semaphore, on its own stack until it is granted. */
typedef struct Waiter {
    uint64_t owner;
    int32_t c_resources;
    int is_granted;
    struct Waiter *p_next;
} Waiter;
typedef struct {
    Waiter *p_head, *p_tail;
} WaiterQueue;
typedef struct {
    const char *sz_function;
    const void *p_object;
    const Ulthread *p_target;
    struct timespec start;
    int is_reported;
} Block;

enum { KIND_MUTEX=1, KIND_CONDITION, KIND_SEMAPHORE, KIND_RWLOCK, KIND_QUEUE, KIND_WAITING_POOL, KIND_DATA_POOL };
typedef struct UltObject {
    const void *p_object;
    int kind;
    struct UltObject *p_next;
} UltObject;
typedef struct {
    UltObject header;
    uint32_t c_threads, c_sync_objects, c_sync_used, c_waiters, c_users;
} WaitingPool;
typedef struct {
    UltObject header;
    WaitingPool *p_pool;
    uint64_t owner;
    uint32_t c_recursion, c_waiters, c_conditions;
    int is_recursive;
    WaiterQueue waiters;
    pthread_cond_t granted;
} Mutex;
typedef struct {
    UltObject header;
    Mutex *p_mutex;
    uint64_t c_waits, c_signals;
    uint32_t c_waiters;
    pthread_cond_t signaled;
} Condition;
/* The read locks one caller holds on a reader-writer lock. */
typedef struct ReadHold {
    uint64_t owner;
    uint32_t c_holds;
    struct ReadHold *p_next;
} ReadHold;
typedef struct {
    UltObject header;
    WaitingPool *p_pool;
    uint32_t c_readers, c_waiters, c_writers_waiting;
    int is_writing;
    ReadHold *p_holds;
    pthread_cond_t released;
} ReaderWriterLock;
typedef struct {
    UltObject header;
    WaitingPool *p_pool;
    int32_t c_resources;
    uint32_t c_waiters;
    WaiterQueue waiters;
    pthread_cond_t granted;
} Semaphore;
typedef struct {
    UltObject header;
    WaitingPool *p_pool;
    uint64_t cb_data;
    uint32_t c_data, c_queues, c_queues_used, c_waiters, i_free;
    unsigned char *p_data;
    uint32_t *p_i_next;
    pthread_cond_t freed;
} DataPool;
typedef struct {
    UltObject header;
    WaitingPool *p_pool;
    DataPool *p_data_pool;
    uint64_t cb_data;
    uint32_t i_head, i_tail, c_waiters;
    pthread_cond_t pushed;
} Queue;

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static Runtime *s_p_runtimes;
static Ulthread *s_p_ulthreads;
static UltObject *s_p_objects;
static int s_is_initialized;
static uint64_t s_c_serials;
static _Thread_local Ulthread *s_p_current;
static _Thread_local sigjmp_buf s_exit_jump;
static size_t s_c_created, s_c_destroyed, s_c_recreated, s_c_waits, s_c_long_waits, s_c_no_pool, s_c_try_misses,
    s_c_failed;

/* The first failures are named so a title that trips over a code shows which call returned it. */
static int32_t counted(const char *sz_function,int32_t result) {
    if (result==ERR_AGAIN) __atomic_add_fetch(&s_c_try_misses,1,__ATOMIC_RELAXED);
    else if (result<0 && __atomic_add_fetch(&s_c_failed,1,__ATOMIC_RELAXED)<=16)
        fprintf(stderr,"ULT: %s returned 0x%08x\n",sz_function,(uint32_t)result);
    return result;
}
static int32_t check_pointer(const void *p_object) {
    return !p_object ? ERR_NULL : (uintptr_t)p_object&7 ? ERR_ALIGNMENT : 0;
}
static int is_opt_valid(const OptParam *p_opt,uint32_t max_attribute) {
    if (!p_opt) return 1;
    if (memcmp(p_opt->signature,s_sz_signature,sizeof(p_opt->signature)) || p_opt->attribute>max_attribute) return 0;
    for (size_t i=0;i<sizeof(p_opt->reserved);++i)
        if (p_opt->reserved[i]) return 0;
    return 1;
}
/* A serial rather than the Ulthread's address, which a later one may get once this one is freed. */
static uint64_t caller_owner(void) {
    return s_p_current ? s_p_current->serial<<1 : (uint64_t)pthread_self()|1;
}

static Runtime **find_runtime(const void *p_object) {
    Runtime **pp_runtime=&s_p_runtimes;
    while (*pp_runtime && (*pp_runtime)->p_object!=p_object) pp_runtime=&(*pp_runtime)->p_next;
    return pp_runtime;
}
static Ulthread **find_ulthread(const void *p_object) {
    Ulthread **pp_ulthread=&s_p_ulthreads;
    while (*pp_ulthread && (*pp_ulthread)->p_object!=p_object) pp_ulthread=&(*pp_ulthread)->p_next;
    return pp_ulthread;
}
static UltObject **find_slot(const void *p_object) {
    UltObject **pp_entry=&s_p_objects;
    while (*pp_entry && (*pp_entry)->p_object!=p_object) pp_entry=&(*pp_entry)->p_next;
    return pp_entry;
}
static void *find_object(const void *p_object,int kind) {
    UltObject *p_entry=*find_slot(p_object);
    return p_entry && p_entry->kind==kind ? p_entry : NULL;
}
/* The console keeps an object inside the guest's memory, so a game may create one again where a
 * previous one lay without destroying it; the old shadow stays allocated because other objects may
 * still point at it. */
static void register_object(UltObject *p_entry,const void *p_object,int kind) {
    UltObject **pp_entry=find_slot(p_object);
    if (*pp_entry) {
        if (++s_c_recreated<=16) fprintf(stderr,"ULT: object %p created again without being destroyed\n",p_object);
        *pp_entry=(*pp_entry)->p_next;
    }
    *p_entry=(UltObject){.p_object=p_object,.kind=kind,.p_next=s_p_objects};
    s_p_objects=p_entry;
    ++s_c_created;
}
static void unregister_object(UltObject *p_entry) {
    *find_slot(p_entry->p_object)=p_entry->p_next;
    ++s_c_destroyed;
}
static int32_t take_sync_object(WaitingPool *p_pool) {
    if (p_pool->c_sync_used>=p_pool->c_sync_objects) return ERR_BUSY;
    ++p_pool->c_sync_used;
    return 0;
}
/* An object created without a waiting pool cannot block; the console's answer then is still a
 * question for the firmware, so each case is named. */
static int32_t reserve_waiter(WaitingPool *p_pool,const char *sz_function,const void *p_object) {
    if (!p_pool) {
        if (++s_c_no_pool<=16) fprintf(stderr,"ULT: %s on %p must wait but has no waiting pool\n",sz_function,p_object);
        return ERR_BUSY;
    }
    if (p_pool->c_waiters>=p_pool->c_threads) return ERR_BUSY;
    ++p_pool->c_waiters;
    return 0;
}
static void enqueue_waiter(WaiterQueue *p_queue,Waiter *p_waiter) {
    if (p_queue->p_tail) p_queue->p_tail->p_next=p_waiter;
    else p_queue->p_head=p_waiter;
    p_queue->p_tail=p_waiter;
}
static Waiter *dequeue_waiter(WaiterQueue *p_queue) {
    Waiter *p_waiter=p_queue->p_head;
    if (p_waiter && !(p_queue->p_head=p_waiter->p_next)) p_queue->p_tail=NULL;
    return p_waiter;
}

static void wait_slot(Runtime *p_runtime,uint64_t ticket) {
    while (ticket>=p_runtime->c_slot_grants) runtime_exception_wait(&p_runtime->slots,&s_lock,NULL);
}
static void take_slot(Runtime *p_runtime) { wait_slot(p_runtime,p_runtime->c_slot_tickets++); }
static void release_slot(Runtime *p_runtime) {
    ++p_runtime->c_slot_grants;
    pthread_cond_broadcast(&p_runtime->slots);
}
static void begin_block(Block *p_block,const char *sz_function,const void *p_object) {
    *p_block=(Block){.sz_function=sz_function,.p_object=p_object};
    ps_host_time_read(PS_HOST_CLOCK_MONOTONIC,&p_block->start);
    ++s_c_waits;
    if (s_p_current) {
        s_p_current->sz_blocked_in=sz_function;
        release_slot(s_p_current->p_runtime);
    }
}
/* A wait that never ends (a lost wakeup in the game, a join cycle through other primitives) is
 * named once instead of hanging silently. */
static void wait_on(Block *p_block,pthread_cond_t *p_cond) {
    runtime_exception_wait(p_cond,&s_lock,NULL);
    if (p_block->is_reported) return;
    struct timespec now;
    ps_host_time_read(PS_HOST_CLOCK_MONOTONIC,&now);
    if (now.tv_sec-p_block->start.tv_sec<LONG_WAIT_S) return;
    p_block->is_reported=1;
    ++s_c_long_waits;
    const Ulthread *p_target=p_block->p_target;
    fprintf(stderr,"ULT: %s on %p has waited %d s%s%s\n",p_block->sz_function,p_block->p_object,LONG_WAIT_S,
            p_target ? ", its target is in " : "",
            !p_target ? "" : p_target->sz_blocked_in ? p_target->sz_blocked_in : "guest code");
}
static void end_block(void) {
    if (!s_p_current) return;
    take_slot(s_p_current->p_runtime);
    s_p_current->sz_blocked_in=NULL;
}

/* Also reached when the guest leaves by scePthreadExit or a cancel, past run_ulthread's frame. */
static void finish_ulthread(void) {
    Ulthread *p_ulthread=s_p_current;
    if (!p_ulthread) return;
    s_p_current=NULL;
    runtime_exception_lock(&s_lock);
    release_slot(p_ulthread->p_runtime);
    p_ulthread->has_finished=1;
    pthread_cond_broadcast(&p_ulthread->p_runtime->done);
    runtime_exception_unlock(&s_lock);
}
static ABI void *run_ulthread(void *p_argument) {
    Ulthread *p_ulthread=p_argument;
    Runtime *p_runtime=p_ulthread->p_runtime;
    runtime_exception_lock(&s_lock);
    while (!p_ulthread->has_ticket || p_ulthread->slot_ticket>=p_runtime->c_slot_grants)
        runtime_exception_wait(&p_runtime->slots,&s_lock,NULL);
    runtime_exception_unlock(&s_lock);
    s_p_current=p_ulthread;
    runtime_thread_set_exit_hook(finish_ulthread);
    /* No host unwinder: guest frames have no registered FDEs, so sceUltUlthreadExit jumps back here. */
    if (!sigsetjmp(s_exit_jump,1)) p_ulthread->status=p_ulthread->pfn_entry(p_ulthread->argument);
    return NULL;
}

static ABI int32_t ult_initialize(void) {
    return counted(__func__,__atomic_exchange_n(&s_is_initialized,1,__ATOMIC_RELAXED) ? ERR_STATE : 0);
}
static ABI int32_t ult_finalize(void) {
    return counted(__func__,__atomic_exchange_n(&s_is_initialized,0,__ATOMIC_RELAXED) ? 0 : ERR_NOT_INITIALIZED);
}
static ABI int32_t opt_initialize(OptParam *p_opt,uint32_t build) {
    int32_t result=check_pointer(p_opt);
    if (result) return counted(__func__,result);
    *p_opt=(OptParam){.build=build};
    memcpy(p_opt->signature,s_sz_signature,sizeof(p_opt->signature));
    return 0;
}
static ABI int32_t runtime_opt_initialize(RuntimeOptParam *p_opt,uint32_t build) {
    if (!p_opt) return ERR_NULL;
    if ((uintptr_t)p_opt&7) return ERR_ALIGNMENT;
    *p_opt=(RuntimeOptParam){.build=build,.stack=MIN_STACK,.affinity=0x3f,.priority=700,.inherit=4};
    memcpy(p_opt->signature,s_sz_signature,sizeof(p_opt->signature));
    return 0;
}
static ABI uint64_t runtime_work_area_size(uint32_t c_max,uint32_t c_workers) {
    (void)c_max;
    return (uint64_t)c_workers*sizeof(void *);
}
static ABI int32_t runtime_create(void *p_object,const char *sz_name,uint32_t c_max,uint32_t c_workers,void *p_work_area,
                                  const RuntimeOptParam *p_opt,uint32_t build) {
    (void)build;
    if (!p_object || !sz_name || !p_work_area) return ERR_NULL;
    if ((uintptr_t)p_object&7 || (uintptr_t)p_work_area&7) return ERR_ALIGNMENT;
    if (p_opt && memcmp(p_opt->signature,s_sz_signature,sizeof(p_opt->signature))) return ERR_INVALID;
    if (!c_max || !c_workers) return ERR_RANGE;
    Runtime *p_runtime=malloc(sizeof(*p_runtime));
    if (!p_runtime) return ERR_AGAIN;
    *p_runtime=(Runtime){.p_object=p_object,.cb_stack=p_opt ? p_opt->stack : MIN_STACK,.c_slot_grants=c_workers,.c_max=c_max};
    pthread_cond_init(&p_runtime->slots,NULL);
    pthread_cond_init(&p_runtime->done,NULL);
    runtime_exception_lock(&s_lock);
    if (*find_runtime(p_object)) {
        runtime_exception_unlock(&s_lock);
        pthread_cond_destroy(&p_runtime->slots);
        pthread_cond_destroy(&p_runtime->done);
        free(p_runtime);
        return ERR_STATE;
    }
    p_runtime->p_next=s_p_runtimes; s_p_runtimes=p_runtime;
    runtime_exception_unlock(&s_lock);
    return 0;
}
static ABI int32_t runtime_destroy(void *p_object) {
    if (!p_object) return ERR_NULL;
    runtime_exception_lock(&s_lock);
    Runtime **pp_runtime=find_runtime(p_object), *p_runtime=*pp_runtime;
    if (!p_runtime || p_runtime->c_live) {
        runtime_exception_unlock(&s_lock);
        return p_runtime ? ERR_BUSY : ERR_STATE;
    }
    *pp_runtime=p_runtime->p_next;
    runtime_exception_unlock(&s_lock);
    pthread_cond_destroy(&p_runtime->slots);
    pthread_cond_destroy(&p_runtime->done);
    free(p_runtime);
    return 0;
}
static ABI int32_t ulthread_create(void *p_object,const char *sz_name,UltEntry pfn_entry,uint64_t argument,void *p_context,
                                   uint64_t cb_context,void *p_runtime,const void *p_opt,uint32_t build) {
    (void)p_context; (void)p_opt; (void)build;
    if (!p_object || !sz_name || !pfn_entry || !p_runtime) return ERR_NULL;
    if ((uintptr_t)p_object&7 || (uintptr_t)p_runtime&7) return ERR_ALIGNMENT;
    Ulthread *p_ulthread=malloc(sizeof(*p_ulthread));
    if (!p_ulthread) return ERR_AGAIN;
    runtime_exception_lock(&s_lock);
    Runtime *p_owner=*find_runtime(p_runtime);
    int32_t result=!p_owner || *find_ulthread(p_object) ? ERR_STATE : p_owner->c_live>=p_owner->c_max ? ERR_AGAIN : 0;
    if (!result) {
        *p_ulthread=(Ulthread){.p_object=p_object,.p_runtime=p_owner,.pfn_entry=pfn_entry,.argument=argument,
                               .serial=++s_c_serials,.p_next=s_p_ulthreads};
        s_p_ulthreads=p_ulthread;
        ++p_owner->c_live;
    }
    runtime_exception_unlock(&s_lock);
    if (result) {
        free(p_ulthread);
        return result;
    }
    /* The context the game hands over holds the console's stack for it, so the host thread gets at
     * least that much; guest threads refuse less than MIN_STACK. */
    uint64_t cb_stack=cb_context>p_owner->cb_stack ? cb_context : p_owner->cb_stack;
    if (cb_stack<MIN_STACK) cb_stack=MIN_STACK;
    int is_spawned=!runtime_thread_spawn(&p_ulthread->p_thread,run_ulthread,p_ulthread,cb_stack,sz_name);
    runtime_exception_lock(&s_lock);
    /* The ticket is drawn only for a thread that runs, so a failed one never holds up the queue. */
    if (is_spawned) {
        p_ulthread->slot_ticket=p_owner->c_slot_tickets++;
        p_ulthread->has_ticket=1;
        pthread_cond_broadcast(&p_owner->slots);
    } else {
        *find_ulthread(p_object)=p_ulthread->p_next;
        --p_owner->c_live;
    }
    runtime_exception_unlock(&s_lock);
    if (is_spawned) return 0;
    free(p_ulthread);
    return counted(__func__,ERR_AGAIN);
}
static void *release_joined(Ulthread *p_ulthread,int32_t *p_out_status) {
    void *p_thread=p_ulthread->p_thread;
    if (p_out_status) *p_out_status=p_ulthread->status;
    *find_ulthread(p_ulthread->p_object)=p_ulthread->p_next;
    --p_ulthread->p_runtime->c_live;
    free(p_ulthread);
    return p_thread;
}
/* Joining itself, or one whose chain of joins leads back to the caller, would never return. */
static int is_join_cycle(const Ulthread *p_target) {
    for (const Ulthread *p_ulthread=p_target;p_ulthread;p_ulthread=p_ulthread->p_joining)
        if (p_ulthread==s_p_current) return 1;
    return 0;
}
static ABI int32_t ulthread_join(void *p_object,int32_t *p_out_status) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    void *p_thread=NULL;
    runtime_exception_lock(&s_lock);
    Ulthread *p_ulthread=*find_ulthread(p_object);
    result=!p_ulthread ? ERR_STATE : is_join_cycle(p_ulthread) ? ERR_PERMISSION : p_ulthread->has_joiner ? ERR_BUSY : 0;
    if (!result) {
        p_ulthread->has_joiner=1;
        if (!p_ulthread->has_finished) {
            if (s_p_current) s_p_current->p_joining=p_ulthread;
            Block block;
            begin_block(&block,"sceUltUlthreadJoin",p_object);
            block.p_target=p_ulthread;
            while (!p_ulthread->has_finished) wait_on(&block,&p_ulthread->p_runtime->done);
            end_block();
            if (s_p_current) s_p_current->p_joining=NULL;
        }
        p_thread=release_joined(p_ulthread,p_out_status);
    }
    runtime_exception_unlock(&s_lock);
    if (p_thread) runtime_thread_join_spawned(p_thread,NULL);
    return counted(__func__,result);
}
static ABI int32_t ulthread_try_join(void *p_object,int32_t *p_out_status) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    void *p_thread=NULL;
    runtime_exception_lock(&s_lock);
    Ulthread *p_ulthread=*find_ulthread(p_object);
    result=!p_ulthread ? ERR_STATE : p_ulthread->has_joiner ? ERR_BUSY : !p_ulthread->has_finished ? ERR_AGAIN : 0;
    if (!result) p_thread=release_joined(p_ulthread,p_out_status);
    runtime_exception_unlock(&s_lock);
    if (p_thread) runtime_thread_join_spawned(p_thread,NULL);
    return counted(__func__,result);
}
static ABI int32_t ulthread_get_self(const void **pp_out_ulthread) {
    if (!pp_out_ulthread) return counted(__func__,ERR_NULL);
    *pp_out_ulthread=s_p_current ? s_p_current->p_object : NULL;
    return 0;
}
static ABI int32_t ulthread_yield(void) {
    if (!s_p_current) return counted(__func__,ERR_PERMISSION);
    Runtime *p_runtime=s_p_current->p_runtime;
    runtime_exception_lock(&s_lock);
    release_slot(p_runtime);
    runtime_exception_unlock(&s_lock);
    sched_yield();
    runtime_exception_lock(&s_lock);
    take_slot(p_runtime);
    runtime_exception_unlock(&s_lock);
    return 0;
}
/* From a guest exception handler the jump would cross the host's signal delivery frames, and from a
 * callback of a runtime function holding a lock it would leave the lock held. */
static ABI int32_t ulthread_exit(int32_t status) {
    if (!s_p_current) return counted(__func__,ERR_PERMISSION);
    if (runtime_exception_is_delivering() || runtime_exception_is_holding()) return counted(__func__,ERR_STATE);
    s_p_current->status=status;
    siglongjmp(s_exit_jump,1);
}

static ABI uint64_t waiting_pool_work_area_size(uint32_t c_threads,uint32_t c_sync_objects) {
    return ((uint64_t)c_threads*2+c_sync_objects+2)*32;
}
static ABI int32_t waiting_pool_create(void *p_object,const char *sz_name,uint32_t c_threads,uint32_t c_sync_objects,
                                       void *p_work_area,const OptParam *p_opt,uint32_t build) {
    (void)build;
    if (!sz_name || !p_work_area) return counted(__func__,ERR_NULL);
    int32_t result=check_pointer(p_object);
    if (!result && (uintptr_t)p_work_area&7) result=ERR_ALIGNMENT;
    if (!result && !__atomic_load_n(&s_is_initialized,__ATOMIC_RELAXED)) result=ERR_NOT_INITIALIZED;
    if (!result && !is_opt_valid(p_opt,0)) result=ERR_INVALID;
    if (result) return counted(__func__,result);
    WaitingPool *p_pool=calloc(1,sizeof(*p_pool));
    if (!p_pool) return counted(__func__,ERR_AGAIN);
    p_pool->c_threads=c_threads; p_pool->c_sync_objects=c_sync_objects;
    runtime_exception_lock(&s_lock);
    register_object(&p_pool->header,p_object,KIND_WAITING_POOL);
    runtime_exception_unlock(&s_lock);
    return 0;
}
static ABI int32_t waiting_pool_destroy(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    WaitingPool *p_pool=find_object(p_object,KIND_WAITING_POOL);
    result=!p_pool || p_pool->c_sync_used || p_pool->c_users ? ERR_STATE : 0;
    if (!result) unregister_object(&p_pool->header);
    runtime_exception_unlock(&s_lock);
    if (!result) free(p_pool);
    return counted(__func__,result);
}

static ABI int32_t mutex_create(void *p_object,const char *sz_name,void *p_pool_object,const OptParam *p_opt,uint32_t build) {
    (void)build;
    if (!sz_name || !p_pool_object) return counted(__func__,ERR_NULL);
    int32_t result=check_pointer(p_object);
    if (!result) result=check_pointer(p_pool_object);
    if (!result && !is_opt_valid(p_opt,1)) result=ERR_INVALID;
    if (result) return counted(__func__,result);
    Mutex *p_mutex=calloc(1,sizeof(*p_mutex));
    if (!p_mutex) return counted(__func__,ERR_AGAIN);
    pthread_cond_init(&p_mutex->granted,NULL);
    p_mutex->is_recursive=p_opt && p_opt->attribute&1;
    runtime_exception_lock(&s_lock);
    p_mutex->p_pool=find_object(p_pool_object,KIND_WAITING_POOL);
    result=p_mutex->p_pool ? take_sync_object(p_mutex->p_pool) : ERR_INVALID;
    if (!result) register_object(&p_mutex->header,p_object,KIND_MUTEX);
    runtime_exception_unlock(&s_lock);
    if (result) {
        pthread_cond_destroy(&p_mutex->granted);
        free(p_mutex);
    }
    return counted(__func__,result);
}
/* Queued behind the others; the unlock that reaches it hands it the mutex directly. */
static void wait_mutex(Mutex *p_mutex,uint64_t owner,Block *p_block) {
    Waiter waiter={.owner=owner};
    enqueue_waiter(&p_mutex->waiters,&waiter);
    ++p_mutex->c_waiters;
    while (!waiter.is_granted) wait_on(p_block,&p_mutex->granted);
    --p_mutex->c_waiters;
}
static void release_mutex(Mutex *p_mutex) {
    Waiter *p_waiter=dequeue_waiter(&p_mutex->waiters);
    p_mutex->owner=p_waiter ? p_waiter->owner : 0;
    p_mutex->c_recursion=p_waiter ? 1 : 0;
    if (!p_waiter) return;
    p_waiter->is_granted=1;
    pthread_cond_broadcast(&p_mutex->granted);
}
static int32_t lock_mutex(void *p_object,int should_block) {
    int32_t result=check_pointer(p_object);
    if (result) return result;
    uint64_t owner=caller_owner();
    runtime_exception_lock(&s_lock);
    Mutex *p_mutex=find_object(p_object,KIND_MUTEX);
    if (!p_mutex) result=ERR_STATE;
    else if (p_mutex->owner==owner) {
        if (p_mutex->is_recursive) ++p_mutex->c_recursion;
        else result=ERR_STATE;
    } else if (!p_mutex->owner) {
        p_mutex->owner=owner;
        p_mutex->c_recursion=1;
    } else if (!should_block) result=ERR_AGAIN;
    else if (!(result=reserve_waiter(p_mutex->p_pool,"sceUltMutexLock",p_object))) {
        Block block;
        begin_block(&block,"sceUltMutexLock",p_object);
        wait_mutex(p_mutex,owner,&block);
        --p_mutex->p_pool->c_waiters;
        end_block();
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t mutex_lock(void *p_object) { return counted(__func__,lock_mutex(p_object,1)); }
static ABI int32_t mutex_try_lock(void *p_object) { return counted(__func__,lock_mutex(p_object,0)); }
static ABI int32_t mutex_unlock(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    uint64_t owner=caller_owner();
    runtime_exception_lock(&s_lock);
    Mutex *p_mutex=find_object(p_object,KIND_MUTEX);
    if (!p_mutex) result=ERR_STATE;
    else if (p_mutex->owner!=owner) result=ERR_PERMISSION;
    else if (p_mutex->c_recursion>1) --p_mutex->c_recursion;
    else release_mutex(p_mutex);
    runtime_exception_unlock(&s_lock);
    return counted(__func__,result);
}
static ABI int32_t mutex_destroy(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    Mutex *p_mutex=find_object(p_object,KIND_MUTEX);
    result=!p_mutex || p_mutex->owner || p_mutex->c_waiters || p_mutex->c_conditions ? ERR_STATE : 0;
    if (!result) {
        unregister_object(&p_mutex->header);
        --p_mutex->p_pool->c_sync_used;
    }
    runtime_exception_unlock(&s_lock);
    if (!result) {
        pthread_cond_destroy(&p_mutex->granted);
        free(p_mutex);
    }
    return counted(__func__,result);
}

static ABI int32_t condition_create(void *p_object,const char *sz_name,void *p_mutex_object,const OptParam *p_opt,
                                    uint32_t build) {
    (void)build;
    if (!sz_name || !p_mutex_object) return counted(__func__,ERR_NULL);
    int32_t result=check_pointer(p_object);
    if (!result) result=check_pointer(p_mutex_object);
    if (!result && !is_opt_valid(p_opt,0)) result=ERR_INVALID;
    if (result) return counted(__func__,result);
    Condition *p_condition=calloc(1,sizeof(*p_condition));
    if (!p_condition) return counted(__func__,ERR_AGAIN);
    pthread_cond_init(&p_condition->signaled,NULL);
    runtime_exception_lock(&s_lock);
    Mutex *p_mutex=find_object(p_mutex_object,KIND_MUTEX);
    result=!p_mutex                ? ERR_STATE
           : p_mutex->is_recursive ? ERR_PERMISSION
           : !p_mutex->p_pool      ? ERR_INVALID
                                   : take_sync_object(p_mutex->p_pool);
    if (!result) {
        p_condition->p_mutex=p_mutex;
        ++p_mutex->c_conditions;
        register_object(&p_condition->header,p_object,KIND_CONDITION);
    }
    runtime_exception_unlock(&s_lock);
    if (result) {
        pthread_cond_destroy(&p_condition->signaled);
        free(p_condition);
    }
    return counted(__func__,result);
}
static ABI int32_t condition_wait(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    uint64_t owner=caller_owner();
    runtime_exception_lock(&s_lock);
    Condition *p_condition=find_object(p_object,KIND_CONDITION);
    Mutex *p_mutex=p_condition ? p_condition->p_mutex : NULL;
    result=!p_condition            ? ERR_STATE
           : p_mutex->owner!=owner ? ERR_PERMISSION
                                   : reserve_waiter(p_mutex->p_pool,"sceUltConditionVariableWait",p_object);
    if (!result) {
        uint64_t ticket=p_condition->c_waits++;
        ++p_condition->c_waiters;
        release_mutex(p_mutex);
        Block block;
        begin_block(&block,"sceUltConditionVariableWait",p_object);
        while (p_condition->c_signals<=ticket) wait_on(&block,&p_condition->signaled);
        if (p_mutex->owner) wait_mutex(p_mutex,owner,&block);
        else {
            p_mutex->owner=owner;
            p_mutex->c_recursion=1;
        }
        --p_condition->c_waiters;
        --p_mutex->p_pool->c_waiters;
        end_block();
    }
    runtime_exception_unlock(&s_lock);
    return counted(__func__,result);
}
static int32_t signal_condition(void *p_object,int should_wake_all) {
    int32_t result=check_pointer(p_object);
    if (result) return result;
    runtime_exception_lock(&s_lock);
    Condition *p_condition=find_object(p_object,KIND_CONDITION);
    if (!p_condition) result=ERR_STATE;
    else if (p_condition->c_signals<p_condition->c_waits) {
        p_condition->c_signals=should_wake_all ? p_condition->c_waits : p_condition->c_signals+1;
        pthread_cond_broadcast(&p_condition->signaled);
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t condition_signal(void *p_object) { return counted(__func__,signal_condition(p_object,0)); }
static ABI int32_t condition_signal_all(void *p_object) { return counted(__func__,signal_condition(p_object,1)); }
static ABI int32_t condition_destroy(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    Condition *p_condition=find_object(p_object,KIND_CONDITION);
    result=!p_condition || p_condition->c_waiters ? ERR_STATE : 0;
    if (!result) {
        unregister_object(&p_condition->header);
        --p_condition->p_mutex->c_conditions;
        --p_condition->p_mutex->p_pool->c_sync_used;
    }
    runtime_exception_unlock(&s_lock);
    if (!result) {
        pthread_cond_destroy(&p_condition->signaled);
        free(p_condition);
    }
    return counted(__func__,result);
}

static ABI int32_t rwlock_create(void *p_object,const char *sz_name,void *p_pool_object,const OptParam *p_opt,uint32_t build) {
    (void)build;
    if (!sz_name || !p_pool_object) return counted(__func__,ERR_NULL);
    int32_t result=check_pointer(p_object);
    if (!result) result=check_pointer(p_pool_object);
    if (!result && !is_opt_valid(p_opt,0)) result=ERR_INVALID;
    if (result) return counted(__func__,result);
    ReaderWriterLock *p_rwlock=calloc(1,sizeof(*p_rwlock));
    if (!p_rwlock) return counted(__func__,ERR_AGAIN);
    pthread_cond_init(&p_rwlock->released,NULL);
    runtime_exception_lock(&s_lock);
    p_rwlock->p_pool=find_object(p_pool_object,KIND_WAITING_POOL);
    result=p_rwlock->p_pool ? take_sync_object(p_rwlock->p_pool) : ERR_INVALID;
    if (!result) register_object(&p_rwlock->header,p_object,KIND_RWLOCK);
    runtime_exception_unlock(&s_lock);
    if (result) {
        pthread_cond_destroy(&p_rwlock->released);
        free(p_rwlock);
    }
    return counted(__func__,result);
}
static ReadHold **find_hold(ReaderWriterLock *p_rwlock,uint64_t owner) {
    ReadHold **pp_hold=&p_rwlock->p_holds;
    while (*pp_hold && (*pp_hold)->owner!=owner) pp_hold=&(*pp_hold)->p_next;
    return pp_hold;
}
/* Writers first: a reader arriving while a writer waits queues behind it, so readers cannot
 * starve writers; one that already holds a read lock takes it again, or it would wait on itself. */
static int is_rwlock_blocked(const ReaderWriterLock *p_rwlock,int is_writer,int has_read_hold) {
    if (p_rwlock->is_writing) return 1;
    if (is_writer) return p_rwlock->c_readers!=0;
    return (p_rwlock->c_writers_waiting && !has_read_hold) || p_rwlock->c_readers>=MAX_READERS;
}
static int32_t lock_rwlock(void *p_object,int is_writer,int should_block) {
    int32_t result=check_pointer(p_object);
    if (result) return result;
    uint64_t owner=caller_owner();
    runtime_exception_lock(&s_lock);
    ReaderWriterLock *p_rwlock=find_object(p_object,KIND_RWLOCK);
    ReadHold *p_hold=p_rwlock && !is_writer ? *find_hold(p_rwlock,owner) : NULL;
    int has_blocked=0;
    Block block;
    if (!p_rwlock) result=ERR_STATE;
    else if (is_rwlock_blocked(p_rwlock,is_writer,p_hold!=NULL)) {
        if (!should_block) result=ERR_AGAIN;
        else if (!(result=reserve_waiter(p_rwlock->p_pool,is_writer ? "sceUltReaderWriterLockLockWrite"
                                                                    : "sceUltReaderWriterLockLockRead",p_object))) {
            has_blocked=1;
            begin_block(&block,is_writer ? "sceUltReaderWriterLockLockWrite" : "sceUltReaderWriterLockLockRead",p_object);
            ++p_rwlock->c_waiters;
            p_rwlock->c_writers_waiting+=is_writer;
            while (is_rwlock_blocked(p_rwlock,is_writer,p_hold!=NULL)) wait_on(&block,&p_rwlock->released);
            p_rwlock->c_writers_waiting-=is_writer;
            --p_rwlock->c_waiters;
            --p_rwlock->p_pool->c_waiters;
        }
    }
    if (!result) {
        if (is_writer) p_rwlock->is_writing=1;
        else {
            ++p_rwlock->c_readers;
            if (p_hold) ++p_hold->c_holds;
            else if ((p_hold=malloc(sizeof(*p_hold)))) {
                *p_hold=(ReadHold){.owner=owner,.c_holds=1,.p_next=p_rwlock->p_holds};
                p_rwlock->p_holds=p_hold;
            }
        }
    }
    if (has_blocked) end_block();
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t rwlock_lock_read(void *p_object) { return counted(__func__,lock_rwlock(p_object,0,1)); }
static ABI int32_t rwlock_lock_write(void *p_object) { return counted(__func__,lock_rwlock(p_object,1,1)); }
static ABI int32_t rwlock_try_lock_read(void *p_object) { return counted(__func__,lock_rwlock(p_object,0,0)); }
static ABI int32_t rwlock_try_lock_write(void *p_object) { return counted(__func__,lock_rwlock(p_object,1,0)); }
static int32_t unlock_rwlock(void *p_object,int is_writer) {
    int32_t result=check_pointer(p_object);
    if (result) return result;
    uint64_t owner=caller_owner();
    runtime_exception_lock(&s_lock);
    ReaderWriterLock *p_rwlock=find_object(p_object,KIND_RWLOCK);
    if (!p_rwlock) result=ERR_STATE;
    else if (is_writer) {
        if (p_rwlock->is_writing) p_rwlock->is_writing=0;
        else result=ERR_STATE;
    } else if (!p_rwlock->c_readers || p_rwlock->is_writing) result=ERR_STATE;
    else {
        --p_rwlock->c_readers;
        /* The console tracks no owners, so another thread may unlock for the holder. */
        ReadHold **pp_hold=find_hold(p_rwlock,owner), *p_hold=*pp_hold;
        if (p_hold && !--p_hold->c_holds) {
            *pp_hold=p_hold->p_next;
            free(p_hold);
        }
    }
    if (!result) pthread_cond_broadcast(&p_rwlock->released);
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t rwlock_unlock_read(void *p_object) { return counted(__func__,unlock_rwlock(p_object,0)); }
static ABI int32_t rwlock_unlock_write(void *p_object) { return counted(__func__,unlock_rwlock(p_object,1)); }
static ABI int32_t rwlock_destroy(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    ReaderWriterLock *p_rwlock=find_object(p_object,KIND_RWLOCK);
    result=!p_rwlock || p_rwlock->c_waiters ? ERR_STATE : 0;
    if (!result) {
        unregister_object(&p_rwlock->header);
        --p_rwlock->p_pool->c_sync_used;
    }
    runtime_exception_unlock(&s_lock);
    if (!result) {
        while (p_rwlock->p_holds) {
            ReadHold *p_hold=p_rwlock->p_holds;
            p_rwlock->p_holds=p_hold->p_next;
            free(p_hold);
        }
        pthread_cond_destroy(&p_rwlock->released);
        free(p_rwlock);
    }
    return counted(__func__,result);
}

static ABI int32_t semaphore_create(void *p_object,const char *sz_name,int32_t c_resources,void *p_pool_object,
                                    const OptParam *p_opt,uint32_t build) {
    (void)build;
    if (!sz_name) return counted(__func__,ERR_NULL);
    int32_t result=check_pointer(p_object);
    if (!result && p_pool_object) result=check_pointer(p_pool_object);
    if (!result && !is_opt_valid(p_opt,0)) result=ERR_INVALID;
    if (!result && c_resources<0) result=ERR_RANGE;
    if (result) return counted(__func__,result);
    Semaphore *p_semaphore=calloc(1,sizeof(*p_semaphore));
    if (!p_semaphore) return counted(__func__,ERR_AGAIN);
    pthread_cond_init(&p_semaphore->granted,NULL);
    p_semaphore->c_resources=c_resources;
    runtime_exception_lock(&s_lock);
    if (p_pool_object) {
        p_semaphore->p_pool=find_object(p_pool_object,KIND_WAITING_POOL);
        result=p_semaphore->p_pool ? take_sync_object(p_semaphore->p_pool) : ERR_INVALID;
    }
    if (!result) register_object(&p_semaphore->header,p_object,KIND_SEMAPHORE);
    runtime_exception_unlock(&s_lock);
    if (result) {
        pthread_cond_destroy(&p_semaphore->granted);
        free(p_semaphore);
    }
    return counted(__func__,result);
}
/* In arrival order: a request waits for those queued before it even when the count would cover it. */
static int32_t acquire_semaphore(void *p_object,int32_t c_resources,int should_block) {
    int32_t result=check_pointer(p_object);
    if (result) return result;
    if (c_resources<=0) return ERR_RANGE;
    runtime_exception_lock(&s_lock);
    Semaphore *p_semaphore=find_object(p_object,KIND_SEMAPHORE);
    if (!p_semaphore) result=ERR_STATE;
    else if (!p_semaphore->waiters.p_head && p_semaphore->c_resources>=c_resources) p_semaphore->c_resources-=c_resources;
    else if (!should_block) result=ERR_AGAIN;
    else if (!(result=reserve_waiter(p_semaphore->p_pool,"sceUltSemaphoreAcquire",p_object))) {
        Block block;
        begin_block(&block,"sceUltSemaphoreAcquire",p_object);
        Waiter waiter={.c_resources=c_resources};
        enqueue_waiter(&p_semaphore->waiters,&waiter);
        ++p_semaphore->c_waiters;
        while (!waiter.is_granted) wait_on(&block,&p_semaphore->granted);
        --p_semaphore->c_waiters;
        --p_semaphore->p_pool->c_waiters;
        end_block();
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t semaphore_acquire(void *p_object,int32_t c_resources) {
    return counted(__func__,acquire_semaphore(p_object,c_resources,1));
}
static ABI int32_t semaphore_try_acquire(void *p_object,int32_t c_resources) {
    return counted(__func__,acquire_semaphore(p_object,c_resources,0));
}
static ABI int32_t semaphore_release(void *p_object,int32_t c_resources) {
    int32_t result=check_pointer(p_object);
    if (!result && c_resources<=0) result=ERR_RANGE;
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    Semaphore *p_semaphore=find_object(p_object,KIND_SEMAPHORE);
    if (!p_semaphore) result=ERR_STATE;
    else if (p_semaphore->c_resources>INT32_MAX-c_resources) result=ERR_RANGE;
    else {
        p_semaphore->c_resources+=c_resources;
        int has_granted=0;
        while (p_semaphore->waiters.p_head && p_semaphore->c_resources>=p_semaphore->waiters.p_head->c_resources) {
            Waiter *p_waiter=dequeue_waiter(&p_semaphore->waiters);
            p_semaphore->c_resources-=p_waiter->c_resources;
            p_waiter->is_granted=has_granted=1;
        }
        if (has_granted) pthread_cond_broadcast(&p_semaphore->granted);
    }
    runtime_exception_unlock(&s_lock);
    return counted(__func__,result);
}
static ABI int32_t semaphore_destroy(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    Semaphore *p_semaphore=find_object(p_object,KIND_SEMAPHORE);
    result=!p_semaphore || p_semaphore->c_waiters ? ERR_STATE : 0;
    if (!result) {
        unregister_object(&p_semaphore->header);
        if (p_semaphore->p_pool) --p_semaphore->p_pool->c_sync_used;
    }
    runtime_exception_unlock(&s_lock);
    if (!result) {
        pthread_cond_destroy(&p_semaphore->granted);
        free(p_semaphore);
    }
    return counted(__func__,result);
}

static ABI uint64_t data_pool_work_area_size(uint32_t c_data,uint64_t cb_data,uint32_t c_queues) {
    return c_data*cb_data+((uint64_t)c_data*2+c_queues+3)*32;
}
static ABI int32_t data_pool_create(void *p_object,const char *sz_name,uint32_t c_data,uint64_t cb_data,uint32_t c_queues,
                                    void *p_pool_object,void *p_work_area,const OptParam *p_opt,uint32_t build) {
    (void)build;
    if (!sz_name || !p_work_area) return counted(__func__,ERR_NULL);
    int32_t result=check_pointer(p_object);
    if (!result && p_pool_object) result=check_pointer(p_pool_object);
    if (!result && !is_opt_valid(p_opt,0)) result=ERR_INVALID;
    if (!result && (!c_data || !cb_data || cb_data>UINT32_MAX)) result=ERR_RANGE;
    if (result) return counted(__func__,result);
    DataPool *p_data_pool=calloc(1,sizeof(*p_data_pool));
    unsigned char *p_data=malloc(c_data*cb_data);
    uint32_t *p_i_next=malloc(c_data*sizeof(*p_i_next));
    if (!p_data_pool || !p_data || !p_i_next) {
        free(p_data_pool); free(p_data); free(p_i_next);
        return counted(__func__,ERR_AGAIN);
    }
    for (uint32_t i=0;i<c_data;++i) p_i_next[i]=i+1<c_data ? i+1 : END_OF_LIST;
    *p_data_pool=(DataPool){.cb_data=cb_data,.c_data=c_data,.c_queues=c_queues,.p_data=p_data,.p_i_next=p_i_next};
    pthread_cond_init(&p_data_pool->freed,NULL);
    runtime_exception_lock(&s_lock);
    if (p_pool_object) {
        p_data_pool->p_pool=find_object(p_pool_object,KIND_WAITING_POOL);
        if (p_data_pool->p_pool) ++p_data_pool->p_pool->c_users;
        else result=ERR_INVALID;
    }
    if (!result) register_object(&p_data_pool->header,p_object,KIND_DATA_POOL);
    runtime_exception_unlock(&s_lock);
    if (result) {
        pthread_cond_destroy(&p_data_pool->freed);
        free(p_data); free(p_i_next); free(p_data_pool);
    }
    return counted(__func__,result);
}
/* Also refused while a queue still draws on it, which the console leaves to the game: the queue's
 * shadow points into this one. */
static ABI int32_t data_pool_destroy(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    DataPool *p_data_pool=find_object(p_object,KIND_DATA_POOL);
    result=!p_data_pool || p_data_pool->c_waiters || p_data_pool->c_queues_used ? ERR_STATE : 0;
    if (!result) {
        unregister_object(&p_data_pool->header);
        if (p_data_pool->p_pool) --p_data_pool->p_pool->c_users;
    }
    runtime_exception_unlock(&s_lock);
    if (!result) {
        pthread_cond_destroy(&p_data_pool->freed);
        free(p_data_pool->p_data); free(p_data_pool->p_i_next); free(p_data_pool);
    }
    return counted(__func__,result);
}

static ABI int32_t queue_create(void *p_object,const char *sz_name,uint64_t cb_data,void *p_pool_object,
                                void *p_data_pool_object,const OptParam *p_opt,uint32_t build) {
    (void)build;
    if (!sz_name || !p_data_pool_object) return counted(__func__,ERR_NULL);
    int32_t result=check_pointer(p_object);
    if (!result) result=check_pointer(p_data_pool_object);
    if (!result && p_pool_object) result=check_pointer(p_pool_object);
    if (!result && !is_opt_valid(p_opt,0)) result=ERR_INVALID;
    if (result) return counted(__func__,result);
    Queue *p_queue=calloc(1,sizeof(*p_queue));
    if (!p_queue) return counted(__func__,ERR_AGAIN);
    pthread_cond_init(&p_queue->pushed,NULL);
    p_queue->cb_data=cb_data;
    p_queue->i_head=p_queue->i_tail=END_OF_LIST;
    runtime_exception_lock(&s_lock);
    DataPool *p_data_pool=find_object(p_data_pool_object,KIND_DATA_POOL);
    WaitingPool *p_pool=p_pool_object ? find_object(p_pool_object,KIND_WAITING_POOL) : NULL;
    result=!p_data_pool || !cb_data || cb_data>p_data_pool->cb_data || (p_pool_object && !p_pool) ? ERR_INVALID
           : p_data_pool->c_queues_used>=p_data_pool->c_queues                                   ? ERR_BUSY
                                                                                                  : 0;
    if (!result) {
        p_queue->p_pool=p_pool;
        p_queue->p_data_pool=p_data_pool;
        ++p_data_pool->c_queues_used;
        if (p_pool) ++p_pool->c_users;
        register_object(&p_queue->header,p_object,KIND_QUEUE);
    }
    runtime_exception_unlock(&s_lock);
    if (result) {
        pthread_cond_destroy(&p_queue->pushed);
        free(p_queue);
    }
    return counted(__func__,result);
}
static int32_t push_queue(void *p_object,const void *p_data,int should_block) {
    int32_t result=check_pointer(p_object);
    if (result) return result;
    if (!p_data) return ERR_NULL;
    runtime_exception_lock(&s_lock);
    Queue *p_queue=find_object(p_object,KIND_QUEUE);
    DataPool *p_data_pool=p_queue ? p_queue->p_data_pool : NULL;
    int has_blocked=0;
    Block block;
    if (!p_queue) result=ERR_STATE;
    else if (p_data_pool->i_free==END_OF_LIST) {
        if (!should_block) result=ERR_AGAIN;
        else if (!(result=reserve_waiter(p_queue->p_pool,"sceUltQueuePush",p_object))) {
            has_blocked=1;
            begin_block(&block,"sceUltQueuePush",p_object);
            ++p_queue->c_waiters; ++p_data_pool->c_waiters;
            while (p_data_pool->i_free==END_OF_LIST) wait_on(&block,&p_data_pool->freed);
            --p_queue->c_waiters; --p_data_pool->c_waiters;
            --p_queue->p_pool->c_waiters;
        }
    }
    if (!result) {
        uint32_t i_slot=p_data_pool->i_free;
        p_data_pool->i_free=p_data_pool->p_i_next[i_slot];
        p_data_pool->p_i_next[i_slot]=END_OF_LIST;
        memcpy(p_data_pool->p_data+i_slot*p_data_pool->cb_data,p_data,p_queue->cb_data);
        if (p_queue->i_tail==END_OF_LIST) p_queue->i_head=i_slot;
        else p_data_pool->p_i_next[p_queue->i_tail]=i_slot;
        p_queue->i_tail=i_slot;
        pthread_cond_signal(&p_queue->pushed);
    }
    if (has_blocked) end_block();
    runtime_exception_unlock(&s_lock);
    return result;
}
static void free_slot(DataPool *p_data_pool,uint32_t i_slot) {
    p_data_pool->p_i_next[i_slot]=p_data_pool->i_free;
    p_data_pool->i_free=i_slot;
    pthread_cond_signal(&p_data_pool->freed);
}
static int32_t pop_queue(void *p_object,void *p_out_data,int should_block) {
    int32_t result=check_pointer(p_object);
    if (result) return result;
    if (!p_out_data) return ERR_NULL;
    runtime_exception_lock(&s_lock);
    Queue *p_queue=find_object(p_object,KIND_QUEUE);
    int has_blocked=0;
    Block block;
    if (!p_queue) result=ERR_STATE;
    else if (p_queue->i_head==END_OF_LIST) {
        if (!should_block) result=ERR_AGAIN;
        else if (!(result=reserve_waiter(p_queue->p_pool,"sceUltQueuePop",p_object))) {
            has_blocked=1;
            begin_block(&block,"sceUltQueuePop",p_object);
            ++p_queue->c_waiters;
            while (p_queue->i_head==END_OF_LIST) wait_on(&block,&p_queue->pushed);
            --p_queue->c_waiters;
            --p_queue->p_pool->c_waiters;
        }
    }
    if (!result) {
        DataPool *p_data_pool=p_queue->p_data_pool;
        uint32_t i_slot=p_queue->i_head;
        p_queue->i_head=p_data_pool->p_i_next[i_slot];
        if (p_queue->i_head==END_OF_LIST) p_queue->i_tail=END_OF_LIST;
        memcpy(p_out_data,p_data_pool->p_data+i_slot*p_data_pool->cb_data,p_queue->cb_data);
        free_slot(p_data_pool,i_slot);
    }
    if (has_blocked) end_block();
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t queue_push(void *p_object,const void *p_data) { return counted(__func__,push_queue(p_object,p_data,1)); }
static ABI int32_t queue_try_push(void *p_object,const void *p_data) {
    return counted(__func__,push_queue(p_object,p_data,0));
}
static ABI int32_t queue_pop(void *p_object,void *p_out_data) { return counted(__func__,pop_queue(p_object,p_out_data,1)); }
static ABI int32_t queue_try_pop(void *p_object,void *p_out_data) {
    return counted(__func__,pop_queue(p_object,p_out_data,0));
}
static ABI int32_t queue_destroy(void *p_object) {
    int32_t result=check_pointer(p_object);
    if (result) return counted(__func__,result);
    runtime_exception_lock(&s_lock);
    Queue *p_queue=find_object(p_object,KIND_QUEUE);
    result=!p_queue || p_queue->c_waiters ? ERR_STATE : 0;
    if (!result) {
        DataPool *p_data_pool=p_queue->p_data_pool;
        while (p_queue->i_head!=END_OF_LIST) {
            uint32_t i_slot=p_queue->i_head;
            p_queue->i_head=p_data_pool->p_i_next[i_slot];
            free_slot(p_data_pool,i_slot);
        }
        unregister_object(&p_queue->header);
        --p_data_pool->c_queues_used;
        if (p_queue->p_pool) --p_queue->p_pool->c_users;
    }
    runtime_exception_unlock(&s_lock);
    if (!result) {
        pthread_cond_destroy(&p_queue->pushed);
        free(p_queue);
    }
    return counted(__func__,result);
}

static const RuntimeExport s_exports[]={
    {"sceUltInitialize",ult_initialize}, {"sceUltFinalize",ult_finalize},
    {"_sceUltUlthreadRuntimeOptParamInitialize",runtime_opt_initialize},
    {"sceUltUlthreadRuntimeGetWorkAreaSize",runtime_work_area_size}, {"_sceUltUlthreadRuntimeCreate",runtime_create},
    {"sceUltUlthreadRuntimeDestroy",runtime_destroy}, {"_sceUltUlthreadOptParamInitialize",opt_initialize},
    {"_sceUltUlthreadCreate",ulthread_create}, {"sceUltUlthreadJoin",ulthread_join},
    {"sceUltUlthreadTryJoin",ulthread_try_join}, {"sceUltUlthreadGetSelf",ulthread_get_self},
    {"sceUltUlthreadYield",ulthread_yield}, {"sceUltUlthreadExit",ulthread_exit},
    {"_sceUltWaitingQueueResourcePoolOptParamInitialize",opt_initialize},
    {"sceUltWaitingQueueResourcePoolGetWorkAreaSize",waiting_pool_work_area_size},
    {"_sceUltWaitingQueueResourcePoolCreate",waiting_pool_create},
    {"sceUltWaitingQueueResourcePoolDestroy",waiting_pool_destroy},
    {"_sceUltMutexOptParamInitialize",opt_initialize}, {"_sceUltMutexCreate",mutex_create},
    {"sceUltMutexLock",mutex_lock}, {"sceUltMutexTryLock",mutex_try_lock}, {"sceUltMutexUnlock",mutex_unlock},
    {"sceUltMutexDestroy",mutex_destroy},
    {"_sceUltConditionVariableOptParamInitialize",opt_initialize}, {"_sceUltConditionVariableCreate",condition_create},
    {"sceUltConditionVariableWait",condition_wait}, {"sceUltConditionVariableSignal",condition_signal},
    {"sceUltConditionVariableSignalAll",condition_signal_all}, {"sceUltConditionVariableDestroy",condition_destroy},
    {"_sceUltReaderWriterLockOptParamInitialize",opt_initialize}, {"_sceUltReaderWriterLockCreate",rwlock_create},
    {"sceUltReaderWriterLockLockRead",rwlock_lock_read}, {"sceUltReaderWriterLockLockWrite",rwlock_lock_write},
    {"sceUltReaderWriterLockTryLockRead",rwlock_try_lock_read},
    {"sceUltReaderWriterLockTryLockWrite",rwlock_try_lock_write},
    {"sceUltReaderWriterLockUnlockRead",rwlock_unlock_read}, {"sceUltReaderWriterLockUnlockWrite",rwlock_unlock_write},
    {"sceUltReaderWriterLockDestroy",rwlock_destroy},
    {"_sceUltSemaphoreOptParamInitialize",opt_initialize}, {"_sceUltSemaphoreCreate",semaphore_create},
    {"sceUltSemaphoreAcquire",semaphore_acquire}, {"sceUltSemaphoreTryAcquire",semaphore_try_acquire},
    {"sceUltSemaphoreRelease",semaphore_release}, {"sceUltSemaphoreDestroy",semaphore_destroy},
    {"_sceUltQueueDataResourcePoolOptParamInitialize",opt_initialize},
    {"sceUltQueueDataResourcePoolGetWorkAreaSize",data_pool_work_area_size},
    {"_sceUltQueueDataResourcePoolCreate",data_pool_create}, {"sceUltQueueDataResourcePoolDestroy",data_pool_destroy},
    {"_sceUltQueueOptParamInitialize",opt_initialize}, {"_sceUltQueueCreate",queue_create},
    {"sceUltQueuePush",queue_push}, {"sceUltQueueTryPush",queue_try_push}, {"sceUltQueuePop",queue_pop},
    {"sceUltQueueTryPop",queue_try_pop}, {"sceUltQueueDestroy",queue_destroy},
};
uintptr_t runtime_ult_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_ult_report(void) {
    runtime_exception_lock(&s_lock);
    printf("Runtime: ULT objects created=%zu, destroyed=%zu, recreated=%zu, blocking waits=%zu, long waits=%zu, "
           "waits without pool=%zu, try misses=%zu, failed calls=%zu\n",s_c_created,s_c_destroyed,s_c_recreated,
           s_c_waits,s_c_long_waits,s_c_no_pool,
           __atomic_load_n(&s_c_try_misses,__ATOMIC_RELAXED),__atomic_load_n(&s_c_failed,__ATOMIC_RELAXED));
    runtime_exception_unlock(&s_lock);
}
#else
uintptr_t runtime_ult_resolve(const char *name) { (void)name; return 0; }
void runtime_ult_report(void) { puts("Runtime: Windows ULT backend not implemented"); }
#endif
