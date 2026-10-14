/* libkernel barriers behind the PS4 pointer-to-handle ABI. Waiters sleep on one condition per
 * barrier; a generation count tells a released waiter from one woken early to take a guest
 * exception. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define BARRIER_SERIAL_THREAD (-1)
typedef struct Barrier {
    pthread_cond_t released;
    uint32_t c_threads, c_waiting;
    uint64_t generation;
    struct Barrier *p_next;
} Barrier;
typedef struct { int32_t is_process_shared; } BarrierAttributes;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static Barrier *s_p_registry;
static size_t s_c_created, s_c_waits, s_c_releases;
static Barrier *find_barrier_locked(Barrier *p_handle) {
    for (Barrier *p_barrier=s_p_registry; p_barrier; p_barrier=p_barrier->p_next) if (p_barrier==p_handle) return p_barrier;
    return NULL;
}
static ABI int32_t attributes_init(BarrierAttributes **pp_out_attributes) {
    if (!pp_out_attributes) return ERR(EINVAL);
    BarrierAttributes *p_attributes=calloc(1,sizeof(*p_attributes));
    if (!p_attributes) return ERR(ENOMEM);
    *pp_out_attributes=p_attributes; return 0;
}
static ABI int32_t attributes_destroy(BarrierAttributes **pp_attributes) {
    if (!pp_attributes || !*pp_attributes) return ERR(EINVAL);
    free(*pp_attributes); *pp_attributes=NULL; return 0;
}
static ABI int32_t attributes_set_pshared(BarrierAttributes **pp_attributes,int32_t is_process_shared) {
    if (!pp_attributes || !*pp_attributes || (is_process_shared!=0 && is_process_shared!=1)) return ERR(EINVAL);
    (*pp_attributes)->is_process_shared=is_process_shared; return 0;
}
static ABI int32_t attributes_get_pshared(BarrierAttributes **pp_attributes,int32_t *p_out_is_process_shared) {
    if (!pp_attributes || !*pp_attributes || !p_out_is_process_shared) return ERR(EINVAL);
    *p_out_is_process_shared=(*pp_attributes)->is_process_shared; return 0;
}
static ABI int32_t barrier_init(Barrier **pp_out_barrier,BarrierAttributes **pp_attributes,uint32_t c_threads,const char *sz_name) {
    (void)sz_name;
    if (!pp_out_barrier || (pp_attributes && !*pp_attributes) || !c_threads) return ERR(EINVAL);
    Barrier *p_barrier=calloc(1,sizeof(*p_barrier));
    if (!p_barrier) return ERR(ENOMEM);
    pthread_condattr_t cond_attributes;
    pthread_condattr_init(&cond_attributes);
    pthread_condattr_setclock(&cond_attributes,CLOCK_MONOTONIC);
    pthread_cond_init(&p_barrier->released,&cond_attributes);
    pthread_condattr_destroy(&cond_attributes);
    p_barrier->c_threads=c_threads;
    runtime_exception_lock(&s_lock);
    p_barrier->p_next=s_p_registry; s_p_registry=p_barrier; ++s_c_created;
    runtime_exception_unlock(&s_lock);
    *pp_out_barrier=p_barrier;
    return 0;
}
static ABI int32_t barrier_destroy(Barrier **pp_handle) {
    if (!pp_handle) return ERR(EINVAL);
    runtime_exception_lock(&s_lock);
    Barrier *p_barrier=find_barrier_locked(*pp_handle);
    int32_t result=0;
    if (!p_barrier) result=ERR(EINVAL);
    else if (p_barrier->c_waiting) result=ERR(EBUSY);
    else {
        Barrier **pp_link=&s_p_registry;
        while (*pp_link!=p_barrier) pp_link=&(*pp_link)->p_next;
        *pp_link=p_barrier->p_next;
        pthread_cond_destroy(&p_barrier->released);
        free(p_barrier);
        *pp_handle=NULL;
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t barrier_wait(Barrier **pp_handle) {
    if (!pp_handle) return ERR(EINVAL);
    runtime_exception_lock(&s_lock);
    Barrier *p_barrier=find_barrier_locked(*pp_handle);
    if (!p_barrier) { runtime_exception_unlock(&s_lock); return ERR(EINVAL); }
    ++s_c_waits;
    int32_t result=0;
    if (++p_barrier->c_waiting==p_barrier->c_threads) {
        p_barrier->c_waiting=0; ++p_barrier->generation; ++s_c_releases;
        pthread_cond_broadcast(&p_barrier->released);
        result=BARRIER_SERIAL_THREAD;
    } else {
        const uint64_t generation=p_barrier->generation, wait_start_ns=runtime_wait_clock();
        while (generation==p_barrier->generation) runtime_exception_wait(&p_barrier->released,&s_lock,NULL);
        runtime_wait_note(0,runtime_wait_clock()-wait_start_ns);
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static int32_t to_posix_result(int32_t result) {
    return result<0 && result!=BARRIER_SERIAL_THREAD ? (int32_t)((uint32_t)result&0xffff) : result;
}
static ABI int32_t posix_attributes_init(BarrierAttributes **pp_attributes) { return to_posix_result(attributes_init(pp_attributes)); }
static ABI int32_t posix_attributes_destroy(BarrierAttributes **pp_attributes) {
    return to_posix_result(attributes_destroy(pp_attributes));
}
static ABI int32_t posix_attributes_set_pshared(BarrierAttributes **pp_attributes,int32_t is_shared) {
    return to_posix_result(attributes_set_pshared(pp_attributes,is_shared));
}
static ABI int32_t posix_attributes_get_pshared(BarrierAttributes **pp_attributes,int32_t *p_out_is_shared) {
    return to_posix_result(attributes_get_pshared(pp_attributes,p_out_is_shared));
}
static ABI int32_t posix_barrier_init(Barrier **pp_out_barrier,BarrierAttributes **pp_attributes,uint32_t c_threads) {
    return to_posix_result(barrier_init(pp_out_barrier,pp_attributes,c_threads,NULL));
}
static ABI int32_t posix_barrier_destroy(Barrier **pp_handle) { return to_posix_result(barrier_destroy(pp_handle)); }
static ABI int32_t posix_barrier_wait(Barrier **pp_handle) { return to_posix_result(barrier_wait(pp_handle)); }
static ABI int32_t posix_barrier_set_name(Barrier **pp_handle,const char *sz_name) {
    (void)sz_name;
    runtime_exception_lock(&s_lock);
    const int is_found=pp_handle && find_barrier_locked(*pp_handle);
    runtime_exception_unlock(&s_lock);
    return is_found ? 0 : EINVAL;
}
static const RuntimeExport exports[]={
    {"scePthreadBarrierInit",barrier_init}, {"scePthreadBarrierDestroy",barrier_destroy},
    {"scePthreadBarrierWait",barrier_wait},
    {"scePthreadBarrierattrInit",attributes_init}, {"scePthreadBarrierattrDestroy",attributes_destroy},
    {"scePthreadBarrierattrSetpshared",attributes_set_pshared}, {"scePthreadBarrierattrGetpshared",attributes_get_pshared},
    {"pthread_barrier_init",posix_barrier_init}, {"pthread_barrier_destroy",posix_barrier_destroy},
    {"pthread_barrier_wait",posix_barrier_wait}, {"pthread_barrier_setname_np",posix_barrier_set_name},
    {"pthread_barrierattr_init",posix_attributes_init}, {"pthread_barrierattr_destroy",posix_attributes_destroy},
    {"pthread_barrierattr_setpshared",posix_attributes_set_pshared},
    {"pthread_barrierattr_getpshared",posix_attributes_get_pshared},
};
uintptr_t runtime_barrier_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
void runtime_barrier_report(void) {
    runtime_exception_lock(&s_lock);
    if (s_c_created) printf("Runtime: barriers created=%zu, waits=%zu, releases=%zu\n",s_c_created,s_c_waits,s_c_releases);
    runtime_exception_unlock(&s_lock);
}
#else
uintptr_t runtime_barrier_resolve(const char *sz_name) { (void)sz_name; return 0; }
void runtime_barrier_report(void) {}
#endif
