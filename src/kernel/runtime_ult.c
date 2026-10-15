/* libSceUlt: user-level threads. A runtime's worker threads take its user-level threads in
 * creation order and run each one to its end on the worker's own stack, which is how the console
 * runs those created without a context of their own. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>

#define ERR_NULL ((int32_t)0x80810001)
#define ERR_ALIGNMENT ((int32_t)0x80810002)
#define ERR_RANGE ((int32_t)0x80810003)
#define ERR_INVALID ((int32_t)0x80810004)
#define ERR_STATE ((int32_t)0x80810006)
#define ERR_BUSY ((int32_t)0x80810007)
#define ERR_AGAIN ((int32_t)0x80810008)

typedef int32_t (ABI *UltEntry)(uint64_t);
typedef struct {
    char signature[12];
    uint32_t build;
    uint64_t stack, affinity;
    int32_t priority, inherit;
    unsigned char reserved[88];
} RuntimeOptParam;
_Static_assert(sizeof(RuntimeOptParam)==128,"SceUltUlthreadRuntimeOptParam layout");
static const char s_sz_signature[]="ubilamtl\0cig";

typedef struct Ulthread {
    const void *p_object;
    struct Runtime *p_runtime;
    UltEntry pfn_entry;
    uint64_t argument;
    int32_t status;
    int has_finished, joining;
    struct Ulthread *p_next, *p_queued;
} Ulthread;
typedef struct Runtime {
    const void *p_object;
    void **pp_workers;
    uint32_t c_workers, max, live;
    int stopping;
    pthread_cond_t work, done;
    Ulthread *p_head, *p_tail;
    struct Runtime *p_next;
} Runtime;

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static Runtime *s_p_runtimes;
static Ulthread *s_p_ulthreads;
static int s_is_initialized;

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
static ABI void *worker(void *p_argument) {
    Runtime *p_runtime=p_argument;
    runtime_exception_lock(&s_lock);
    for (;;) {
        while (!p_runtime->p_head && !p_runtime->stopping) runtime_exception_wait(&p_runtime->work,&s_lock,NULL);
        Ulthread *p_ulthread=p_runtime->p_head;
        if (!p_ulthread) break;
        p_runtime->p_head=p_ulthread->p_queued;
        if (!p_runtime->p_head) p_runtime->p_tail=NULL;
        runtime_exception_unlock(&s_lock);
        int32_t status=p_ulthread->pfn_entry(p_ulthread->argument);
        runtime_exception_lock(&s_lock);
        p_ulthread->status=status; p_ulthread->has_finished=1;
        pthread_cond_broadcast(&p_runtime->done);
    }
    runtime_exception_unlock(&s_lock);
    return NULL;
}

static ABI int32_t ult_initialize(void) {
    return __atomic_exchange_n(&s_is_initialized,1,__ATOMIC_RELAXED) ? ERR_STATE : 0;
}
static ABI int32_t runtime_opt_initialize(RuntimeOptParam *p_opt,uint32_t build) {
    if (!p_opt) return ERR_NULL;
    if ((uintptr_t)p_opt&7) return ERR_ALIGNMENT;
    *p_opt=(RuntimeOptParam){.build=build,.stack=0x4000,.affinity=0x3f,.priority=700,.inherit=4};
    memcpy(p_opt->signature,s_sz_signature,sizeof(p_opt->signature));
    return 0;
}
static ABI uint64_t runtime_work_area_size(uint32_t max,uint32_t c_workers) {
    (void)max;
    return (uint64_t)c_workers*sizeof(void *);
}
static ABI int32_t runtime_create(void *p_object,const char *sz_name,uint32_t max,uint32_t c_workers,void *p_work_area,
                                  const RuntimeOptParam *p_opt,uint32_t build) {
    (void)build;
    if (!p_object || !sz_name || !p_work_area) return ERR_NULL;
    if ((uintptr_t)p_object&7 || (uintptr_t)p_work_area&7) return ERR_ALIGNMENT;
    if (p_opt && memcmp(p_opt->signature,s_sz_signature,sizeof(p_opt->signature))) return ERR_INVALID;
    if (!max || !c_workers) return ERR_RANGE;
    Runtime *p_runtime=malloc(sizeof(*p_runtime));
    if (!p_runtime) return ERR_AGAIN;
    *p_runtime=(Runtime){.p_object=p_object,.pp_workers=p_work_area,.c_workers=c_workers,.max=max};
    pthread_cond_init(&p_runtime->work,NULL);
    pthread_cond_init(&p_runtime->done,NULL);
    runtime_exception_lock(&s_lock);
    if (*find_runtime(p_object)) {
        runtime_exception_unlock(&s_lock);
        free(p_runtime);
        return ERR_STATE;
    }
    p_runtime->p_next=s_p_runtimes; s_p_runtimes=p_runtime;
    runtime_exception_unlock(&s_lock);
    for (uint32_t i=0;i<c_workers;++i)
        if (runtime_thread_spawn(&p_runtime->pp_workers[i],worker,p_runtime,p_opt ? p_opt->stack : 0x4000,sz_name)) {
            fprintf(stderr,"STOP: ULT runtime '%s' could not start its worker threads\n",sz_name);
            runtime_stop(21);
        }
    return 0;
}
static ABI int32_t runtime_destroy(void *p_object) {
    if (!p_object) return ERR_NULL;
    runtime_exception_lock(&s_lock);
    Runtime **pp_runtime=find_runtime(p_object), *p_runtime=*pp_runtime;
    if (!p_runtime || p_runtime->live) {
        runtime_exception_unlock(&s_lock);
        return p_runtime ? ERR_BUSY : ERR_STATE;
    }
    *pp_runtime=p_runtime->p_next;
    p_runtime->stopping=1;
    pthread_cond_broadcast(&p_runtime->work);
    runtime_exception_unlock(&s_lock);
    for (uint32_t i=0;i<p_runtime->c_workers;++i) runtime_thread_join_spawned(p_runtime->pp_workers[i],NULL);
    pthread_cond_destroy(&p_runtime->work);
    pthread_cond_destroy(&p_runtime->done);
    free(p_runtime);
    return 0;
}
static ABI int32_t ulthread_create(void *p_object,const char *sz_name,UltEntry pfn_entry,uint64_t argument,void *p_context,
                                   uint64_t cb_context,void *p_runtime,const void *p_opt,uint32_t build) {
    (void)p_context; (void)cb_context; (void)p_opt; (void)build;
    if (!p_object || !sz_name || !pfn_entry || !p_runtime) return ERR_NULL;
    if ((uintptr_t)p_object&7 || (uintptr_t)p_runtime&7) return ERR_ALIGNMENT;
    Ulthread *p_ulthread=malloc(sizeof(*p_ulthread));
    if (!p_ulthread) return ERR_AGAIN;
    runtime_exception_lock(&s_lock);
    Runtime *p_owner=*find_runtime(p_runtime);
    int32_t result=!p_owner || *find_ulthread(p_object) ? ERR_STATE : p_owner->live>=p_owner->max ? ERR_AGAIN : 0;
    if (!result) {
        *p_ulthread=(Ulthread){.p_object=p_object,.p_runtime=p_owner,.pfn_entry=pfn_entry,.argument=argument,.p_next=s_p_ulthreads};
        s_p_ulthreads=p_ulthread;
        if (p_owner->p_tail) p_owner->p_tail->p_queued=p_ulthread;
        else p_owner->p_head=p_ulthread;
        p_owner->p_tail=p_ulthread;
        ++p_owner->live;
        pthread_cond_signal(&p_owner->work);
    }
    runtime_exception_unlock(&s_lock);
    if (result) free(p_ulthread);
    return result;
}
static ABI int32_t ulthread_join(void *p_object,int32_t *p_status) {
    if (!p_object) return ERR_NULL;
    runtime_exception_lock(&s_lock);
    Ulthread *p_ulthread=*find_ulthread(p_object);
    if (!p_ulthread || p_ulthread->joining) {
        runtime_exception_unlock(&s_lock);
        return p_ulthread ? ERR_BUSY : ERR_STATE;
    }
    p_ulthread->joining=1;
    while (!p_ulthread->has_finished) runtime_exception_wait(&p_ulthread->p_runtime->done,&s_lock,NULL);
    if (p_status) *p_status=p_ulthread->status;
    *find_ulthread(p_object)=p_ulthread->p_next;
    --p_ulthread->p_runtime->live;
    runtime_exception_unlock(&s_lock);
    free(p_ulthread);
    return 0;
}

static const RuntimeExport s_exports[]={
    {"sceUltInitialize",ult_initialize}, {"_sceUltUlthreadRuntimeOptParamInitialize",runtime_opt_initialize},
    {"sceUltUlthreadRuntimeGetWorkAreaSize",runtime_work_area_size}, {"_sceUltUlthreadRuntimeCreate",runtime_create},
    {"sceUltUlthreadRuntimeDestroy",runtime_destroy}, {"_sceUltUlthreadCreate",ulthread_create},
    {"sceUltUlthreadJoin",ulthread_join},
};
uintptr_t runtime_ult_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
#else
uintptr_t runtime_ult_resolve(const char *name) { (void)name; return 0; }
#endif
