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
static const char signature[]="ubilamtl\0cig";

typedef struct Ulthread {
    const void *object;
    struct Runtime *runtime;
    UltEntry entry;
    uint64_t argument;
    int32_t status;
    int finished, joining;
    struct Ulthread *next, *queued;
} Ulthread;
typedef struct Runtime {
    const void *object;
    void **workers;
    uint32_t worker_count, max, live;
    int stopping;
    pthread_cond_t work, done;
    Ulthread *head, *tail;
    struct Runtime *next;
} Runtime;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Runtime *runtimes;
static Ulthread *ulthreads;
static int initialized;

static Runtime **find_runtime(const void *object) {
    Runtime **r=&runtimes;
    while (*r && (*r)->object!=object) r=&(*r)->next;
    return r;
}
static Ulthread **find_ulthread(const void *object) {
    Ulthread **u=&ulthreads;
    while (*u && (*u)->object!=object) u=&(*u)->next;
    return u;
}
static ABI void *worker(void *argument) {
    Runtime *r=argument;
    runtime_exception_lock(&lock);
    for (;;) {
        while (!r->head && !r->stopping) runtime_exception_wait(&r->work,&lock,NULL);
        Ulthread *u=r->head;
        if (!u) break;
        r->head=u->queued;
        if (!r->head) r->tail=NULL;
        runtime_exception_unlock(&lock);
        int32_t status=u->entry(u->argument);
        runtime_exception_lock(&lock);
        u->status=status; u->finished=1;
        pthread_cond_broadcast(&r->done);
    }
    runtime_exception_unlock(&lock);
    return NULL;
}

static ABI int32_t ult_initialize(void) {
    return __atomic_exchange_n(&initialized,1,__ATOMIC_RELAXED) ? ERR_STATE : 0;
}
static ABI int32_t runtime_opt_initialize(RuntimeOptParam *opt,uint32_t build) {
    if (!opt) return ERR_NULL;
    if ((uintptr_t)opt&7) return ERR_ALIGNMENT;
    *opt=(RuntimeOptParam){.build=build,.stack=0x4000,.affinity=0x3f,.priority=700,.inherit=4};
    memcpy(opt->signature,signature,sizeof(opt->signature));
    return 0;
}
static ABI uint64_t runtime_work_area_size(uint32_t max,uint32_t worker_count) {
    (void)max;
    return (uint64_t)worker_count*sizeof(void *);
}
static ABI int32_t runtime_create(void *object,const char *name,uint32_t max,uint32_t worker_count,void *work_area,
                                  const RuntimeOptParam *opt,uint32_t build) {
    (void)build;
    if (!object || !name || !work_area) return ERR_NULL;
    if ((uintptr_t)object&7 || (uintptr_t)work_area&7) return ERR_ALIGNMENT;
    if (opt && memcmp(opt->signature,signature,sizeof(opt->signature))) return ERR_INVALID;
    if (!max || !worker_count) return ERR_RANGE;
    Runtime *r=malloc(sizeof(*r));
    if (!r) return ERR_AGAIN;
    *r=(Runtime){.object=object,.workers=work_area,.worker_count=worker_count,.max=max};
    pthread_cond_init(&r->work,NULL);
    pthread_cond_init(&r->done,NULL);
    runtime_exception_lock(&lock);
    if (*find_runtime(object)) {
        runtime_exception_unlock(&lock);
        free(r);
        return ERR_STATE;
    }
    r->next=runtimes; runtimes=r;
    runtime_exception_unlock(&lock);
    for (uint32_t i=0;i<worker_count;++i)
        if (runtime_thread_spawn(&r->workers[i],worker,r,opt ? opt->stack : 0x4000,name)) {
            fprintf(stderr,"STOP: ULT runtime '%s' could not start its worker threads\n",name);
            runtime_stop(21);
        }
    return 0;
}
static ABI int32_t runtime_destroy(void *object) {
    if (!object) return ERR_NULL;
    runtime_exception_lock(&lock);
    Runtime **p=find_runtime(object), *r=*p;
    if (!r || r->live) {
        runtime_exception_unlock(&lock);
        return r ? ERR_BUSY : ERR_STATE;
    }
    *p=r->next;
    r->stopping=1;
    pthread_cond_broadcast(&r->work);
    runtime_exception_unlock(&lock);
    for (uint32_t i=0;i<r->worker_count;++i) runtime_thread_join_spawned(r->workers[i],NULL);
    pthread_cond_destroy(&r->work);
    pthread_cond_destroy(&r->done);
    free(r);
    return 0;
}
static ABI int32_t ulthread_create(void *object,const char *name,UltEntry entry,uint64_t argument,void *context,
                                   uint64_t context_size,void *runtime,const void *opt,uint32_t build) {
    (void)context; (void)context_size; (void)opt; (void)build;
    if (!object || !name || !entry || !runtime) return ERR_NULL;
    if ((uintptr_t)object&7 || (uintptr_t)runtime&7) return ERR_ALIGNMENT;
    Ulthread *u=malloc(sizeof(*u));
    if (!u) return ERR_AGAIN;
    runtime_exception_lock(&lock);
    Runtime *r=*find_runtime(runtime);
    int32_t result=!r || *find_ulthread(object) ? ERR_STATE : r->live>=r->max ? ERR_AGAIN : 0;
    if (!result) {
        *u=(Ulthread){.object=object,.runtime=r,.entry=entry,.argument=argument,.next=ulthreads};
        ulthreads=u;
        if (r->tail) r->tail->queued=u;
        else r->head=u;
        r->tail=u;
        ++r->live;
        pthread_cond_signal(&r->work);
    }
    runtime_exception_unlock(&lock);
    if (result) free(u);
    return result;
}
static ABI int32_t ulthread_join(void *object,int32_t *status) {
    if (!object) return ERR_NULL;
    runtime_exception_lock(&lock);
    Ulthread *u=*find_ulthread(object);
    if (!u || u->joining) {
        runtime_exception_unlock(&lock);
        return u ? ERR_BUSY : ERR_STATE;
    }
    u->joining=1;
    while (!u->finished) runtime_exception_wait(&u->runtime->done,&lock,NULL);
    if (status) *status=u->status;
    *find_ulthread(object)=u->next;
    --u->runtime->live;
    runtime_exception_unlock(&lock);
    free(u);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceUltInitialize",ult_initialize}, {"_sceUltUlthreadRuntimeOptParamInitialize",runtime_opt_initialize},
    {"sceUltUlthreadRuntimeGetWorkAreaSize",runtime_work_area_size}, {"_sceUltUlthreadRuntimeCreate",runtime_create},
    {"sceUltUlthreadRuntimeDestroy",runtime_destroy}, {"_sceUltUlthreadCreate",ulthread_create},
    {"sceUltUlthreadJoin",ulthread_join},
};
uintptr_t runtime_ult_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
uintptr_t runtime_ult_resolve(const char *name) { (void)name; return 0; }
#endif
