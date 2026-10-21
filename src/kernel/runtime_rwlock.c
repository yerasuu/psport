/* Linux rwlocks behind PS4 pointer-to-handle ABI. The registry protects
   lifetime, static initialization and ownership; waiting happens outside it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <limits.h>
typedef struct Holder {
    pthread_t thread;
    unsigned readers, writer, pending;
    struct Holder *p_next;
} Holder;
typedef struct Rwlock {
    pthread_rwlock_t native;
    Holder *p_holders;
    unsigned inflight;
    struct Rwlock *p_next;
} Rwlock;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
enum { RWLOCK_NORMAL=1, RWLOCK_PREFER_READER=2 };
typedef struct { int32_t type; } RwlockAttr;
static pthread_mutex_t s_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static Rwlock *s_p_registry;
static size_t s_c_created, s_c_reads, s_c_writes, s_c_unlocks;
static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); runtime_stop(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}
/* All helpers ending in _locked require s_registry_lock. */
static Rwlock *find_locked(Rwlock *p_handle) {
    for (Rwlock *p_rwlock=s_p_registry; p_rwlock; p_rwlock=p_rwlock->p_next) if (p_rwlock==p_handle) return p_rwlock;
    return NULL;
}
/* libkernel's rwlocks block new readers while a writer waits unless the type prefers readers. */
static int create_locked(Rwlock **pp_out, int32_t type) {
    Rwlock *p_rwlock=calloc(1,sizeof(*p_rwlock));
    if (!p_rwlock) return ENOMEM;
    pthread_rwlockattr_t native_attr;
    int e=pthread_rwlockattr_init(&native_attr);
    if (!e) e=pthread_rwlockattr_setkind_np(&native_attr,type==RWLOCK_PREFER_READER ?
        PTHREAD_RWLOCK_PREFER_READER_NP : PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    if (!e) e=pthread_rwlock_init(&p_rwlock->native,&native_attr);
    pthread_rwlockattr_destroy(&native_attr);
    if (e) { free(p_rwlock); return e; }
    p_rwlock->p_next=s_p_registry; s_p_registry=p_rwlock; *pp_out=p_rwlock; ++s_c_created; return 0;
}
static ABI int32_t attr_init(RwlockAttr **pp_out) {
    if (!pp_out) return error(EINVAL);
    RwlockAttr *p_attr=malloc(sizeof(*p_attr));
    if (!p_attr) return error(ENOMEM);
    p_attr->type=RWLOCK_NORMAL; *pp_out=p_attr; return 0;
}
static ABI int32_t attr_destroy(RwlockAttr **pp_attr) {
    if (!pp_attr || !*pp_attr) return error(EINVAL);
    free(*pp_attr); *pp_attr=NULL; return 0;
}
static ABI int32_t attr_settype(RwlockAttr **pp_attr, int32_t type) {
    if (!pp_attr || !*pp_attr || (type!=RWLOCK_NORMAL && type!=RWLOCK_PREFER_READER)) return error(EINVAL);
    (*pp_attr)->type=type; return 0;
}
static ABI int32_t attr_gettype(RwlockAttr **pp_attr, int32_t *p_type) {
    if (!pp_attr || !*pp_attr || !p_type) return error(EINVAL);
    *p_type=(*pp_attr)->type; return 0;
}
static ABI int32_t attr_setpshared(RwlockAttr **pp_attr, int32_t shared) {
    if (!pp_attr || !*pp_attr || shared) return error(EINVAL);
    return 0;
}
static ABI int32_t attr_getpshared(RwlockAttr **pp_attr, int32_t *p_shared) {
    if (!pp_attr || !*pp_attr || !p_shared) return error(EINVAL);
    *p_shared=0; return 0;
}
static ABI int32_t rw_init(Rwlock **pp_out, RwlockAttr *const *pp_attr, const char *sz_name) {
    (void)sz_name;
    if (!pp_out || (pp_attr && !*pp_attr)) return error(EINVAL);
    pthread_mutex_lock(&s_registry_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    int e=create_locked(pp_out,pp_attr ? (*pp_attr)->type : RWLOCK_NORMAL);
    pthread_mutex_unlock(&s_registry_lock); return error(e);
}
static void prune_locked(Rwlock *p_rwlock, Holder *p_holder) {
    if (p_holder->readers || p_holder->writer || p_holder->pending) return;
    Holder **pp_link=&p_rwlock->p_holders;
    while (*pp_link!=p_holder) pp_link=&(*pp_link)->p_next;
    *pp_link=p_holder->p_next; free(p_holder);
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **pp_handle, enum Operation op, const GuestTime *p_time, clockid_t clock) {
    if (!pp_handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    pthread_mutex_lock(&s_registry_lock);
    int e=0;
    if (!*pp_handle) e=create_locked(pp_handle,RWLOCK_NORMAL);
    Rwlock *p_rwlock=e ? NULL : find_locked(*pp_handle);
    if (!p_rwlock) { pthread_mutex_unlock(&s_registry_lock); return error(e ? e : EINVAL); }
    Holder *p_holder=p_rwlock->p_holders;
    while (p_holder && !pthread_equal(p_holder->thread,pthread_self())) p_holder=p_holder->p_next;
    if (p_holder && (p_holder->writer || (writer && p_holder->readers))) {
        pthread_mutex_unlock(&s_registry_lock); return error(attempt ? EBUSY : EDEADLK);
    }
    if (p_holder && p_holder->readers==UINT_MAX) { pthread_mutex_unlock(&s_registry_lock); return error(EAGAIN); }
    /* libthr lets a thread already reading take the lock again past waiting writers, so only its
       first read holds the native lock; otherwise writer preference would deadlock it. */
    if (p_holder && p_holder->readers) { ++p_holder->readers; ++s_c_reads; pthread_mutex_unlock(&s_registry_lock); return 0; }
    if (!p_holder) {
        p_holder=calloc(1,sizeof(*p_holder));
        if (!p_holder) { pthread_mutex_unlock(&s_registry_lock); return error(ENOMEM); }
        p_holder->thread=pthread_self(); p_holder->p_next=p_rwlock->p_holders; p_rwlock->p_holders=p_holder;
    }
    ++p_rwlock->inflight; ++p_holder->pending;
    pthread_mutex_unlock(&s_registry_lock);
    /* Timed operations first try the lock, matching POSIX timeout validation. */
    if (attempt || timed) e=writer ? pthread_rwlock_trywrlock(&p_rwlock->native) : pthread_rwlock_tryrdlock(&p_rwlock->native);
    else e=writer ? pthread_rwlock_wrlock(&p_rwlock->native) : pthread_rwlock_rdlock(&p_rwlock->native);
    if (timed && e==EBUSY) {
        if (!p_time || p_time->nanoseconds<0 || p_time->nanoseconds>=1000000000) e=EINVAL;
        else {
            struct timespec deadline={.tv_sec=(time_t)p_time->seconds,.tv_nsec=(long)p_time->nanoseconds};
            e=writer ? pthread_rwlock_clockwrlock(&p_rwlock->native,clock,&deadline) : pthread_rwlock_clockrdlock(&p_rwlock->native,clock,&deadline);
        }
    }
    pthread_mutex_lock(&s_registry_lock);
    --p_rwlock->inflight; --p_holder->pending;
    if (!e) {
        if (writer) { p_holder->writer=1; ++s_c_writes; }
        else { ++p_holder->readers; ++s_c_reads; }
    }
    prune_locked(p_rwlock,p_holder);
    pthread_mutex_unlock(&s_registry_lock); return error(e);
}
static ABI int32_t rw_read(Rwlock **pp_rwlock) { return acquire(pp_rwlock,READ,NULL,CLOCK_REALTIME); }
static ABI int32_t rw_write(Rwlock **pp_rwlock) { return acquire(pp_rwlock,WRITE,NULL,CLOCK_REALTIME); }
static ABI int32_t rw_tryread(Rwlock **pp_rwlock) { return acquire(pp_rwlock,TRY_READ,NULL,CLOCK_REALTIME); }
static ABI int32_t rw_trywrite(Rwlock **pp_rwlock) { return acquire(pp_rwlock,TRY_WRITE,NULL,CLOCK_REALTIME); }
/* scePthreadRwlockTimed*lock take a timeout in microseconds from the call, unlike the POSIX calls'
   absolute realtime deadline; libkernel measures it on a monotonic clock. */
static int32_t sce_timed(Rwlock **pp_rwlock, enum Operation op, uint32_t timeout_us) {
    struct timespec now;
    if (ps_host_time_read(PS_HOST_CLOCK_MONOTONIC,&now)) return error(EINVAL);
    int64_t ns=now.tv_nsec+(int64_t)(timeout_us%1000000)*1000;
    GuestTime end={.seconds=now.tv_sec+timeout_us/1000000+ns/1000000000,.nanoseconds=ns%1000000000};
    return acquire(pp_rwlock,op,&end,CLOCK_MONOTONIC);
}
static ABI int32_t rw_timedread(Rwlock **pp_rwlock,uint32_t timeout_us) { return sce_timed(pp_rwlock,TIMED_READ,timeout_us); }
static ABI int32_t rw_timedwrite(Rwlock **pp_rwlock,uint32_t timeout_us) { return sce_timed(pp_rwlock,TIMED_WRITE,timeout_us); }
static ABI int32_t rw_unlock(Rwlock **pp_handle) {
    if (!pp_handle) return error(EINVAL);
    pthread_mutex_lock(&s_registry_lock);
    Rwlock *p_rwlock=find_locked(*pp_handle);
    if (!p_rwlock) { pthread_mutex_unlock(&s_registry_lock); return error(EINVAL); }
    Holder *p_holder=p_rwlock->p_holders;
    while (p_holder && !pthread_equal(p_holder->thread,pthread_self())) p_holder=p_holder->p_next;
    if (!p_holder || (!p_holder->readers && !p_holder->writer)) { pthread_mutex_unlock(&s_registry_lock); return error(EPERM); }
    int e=p_holder->writer || p_holder->readers==1 ? pthread_rwlock_unlock(&p_rwlock->native) : 0;
    if (!e) { if (p_holder->writer) p_holder->writer=0; else --p_holder->readers; ++s_c_unlocks; }
    prune_locked(p_rwlock,p_holder);
    pthread_mutex_unlock(&s_registry_lock); return error(e);
}
static ABI int32_t rw_destroy(Rwlock **pp_handle) {
    if (!pp_handle) return error(EINVAL);
    pthread_mutex_lock(&s_registry_lock);
    if (!*pp_handle) { pthread_mutex_unlock(&s_registry_lock); return 0; }
    Rwlock *p_rwlock=find_locked(*pp_handle);
    if (!p_rwlock) { pthread_mutex_unlock(&s_registry_lock); return error(EINVAL); }
    if (p_rwlock->inflight || p_rwlock->p_holders) { pthread_mutex_unlock(&s_registry_lock); return error(EBUSY); }
    int e=pthread_rwlock_destroy(&p_rwlock->native);
    if (!e) {
        Rwlock **pp_link=&s_p_registry;
        while (*pp_link!=p_rwlock) pp_link=&(*pp_link)->p_next;
        *pp_link=p_rwlock->p_next; free(p_rwlock); *pp_handle=(Rwlock *)(uintptr_t)1;
    }
    pthread_mutex_unlock(&s_registry_lock); return error(e);
}
static int32_t posix_result(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(RwlockAttr **pp_rwlock_attr) { return posix_result(attr_init(pp_rwlock_attr)); }
static ABI int32_t posix_attr_destroy(RwlockAttr **pp_rwlock_attr) { return posix_result(attr_destroy(pp_rwlock_attr)); }
static ABI int32_t posix_attr_settype(RwlockAttr **pp_rwlock_attr,int32_t t) { return posix_result(attr_settype(pp_rwlock_attr,t)); }
static ABI int32_t posix_attr_gettype(RwlockAttr **pp_rwlock_attr,int32_t *p_type) { return posix_result(attr_gettype(pp_rwlock_attr,p_type)); }
static ABI int32_t posix_attr_setpshared(RwlockAttr **pp_rwlock_attr,int32_t s) { return posix_result(attr_setpshared(pp_rwlock_attr,s)); }
static ABI int32_t posix_attr_getpshared(RwlockAttr **pp_rwlock_attr,int32_t *p_shared) { return posix_result(attr_getpshared(pp_rwlock_attr,p_shared)); }
static ABI int32_t posix_init(Rwlock **pp_out,RwlockAttr *const *pp_attr) { return posix_result(rw_init(pp_out,pp_attr,NULL)); }
static ABI int32_t posix_destroy(Rwlock **pp_rwlock) { return posix_result(rw_destroy(pp_rwlock)); }
static ABI int32_t posix_read(Rwlock **pp_rwlock) { return posix_result(rw_read(pp_rwlock)); }
static ABI int32_t posix_write(Rwlock **pp_rwlock) { return posix_result(rw_write(pp_rwlock)); }
static ABI int32_t posix_tryread(Rwlock **pp_rwlock) { return posix_result(rw_tryread(pp_rwlock)); }
static ABI int32_t posix_trywrite(Rwlock **pp_rwlock) { return posix_result(rw_trywrite(pp_rwlock)); }
static ABI int32_t posix_timedread(Rwlock **pp_rwlock,const GuestTime *p_time) { return posix_result(acquire(pp_rwlock,TIMED_READ,p_time,CLOCK_REALTIME)); }
static ABI int32_t posix_timedwrite(Rwlock **pp_rwlock,const GuestTime *p_time) { return posix_result(acquire(pp_rwlock,TIMED_WRITE,p_time,CLOCK_REALTIME)); }
static ABI int32_t posix_unlock(Rwlock **pp_rwlock) { return posix_result(rw_unlock(pp_rwlock)); }
/* Names are for the console's debugger only; the runtime keeps none (scePthreadRwlockInit's neither). */
static ABI int32_t posix_setname(Rwlock **pp_rwlock,const char *sz_name) { (void)pp_rwlock; return sz_name ? 0 : EINVAL; }
static const RuntimeExport s_exports[]={
    {"scePthreadRwlockInit",rw_init}, {"scePthreadRwlockDestroy",rw_destroy}, {"scePthreadRwlockRdlock",rw_read},
    {"scePthreadRwlockWrlock",rw_write}, {"scePthreadRwlockTryrdlock",rw_tryread},
    {"scePthreadRwlockTrywrlock",rw_trywrite}, {"scePthreadRwlockUnlock",rw_unlock},
    {"scePthreadRwlockTimedrdlock",rw_timedread}, {"scePthreadRwlockTimedwrlock",rw_timedwrite},
    {"scePthreadRwlockattrInit",attr_init}, {"scePthreadRwlockattrDestroy",attr_destroy},
    {"scePthreadRwlockattrSettype",attr_settype}, {"scePthreadRwlockattrGettype",attr_gettype},
    {"scePthreadRwlockattrSetpshared",attr_setpshared}, {"scePthreadRwlockattrGetpshared",attr_getpshared},
    {"pthread_rwlockattr_init",posix_attr_init}, {"pthread_rwlockattr_destroy",posix_attr_destroy},
    {"pthread_rwlockattr_settype_np",posix_attr_settype}, {"pthread_rwlockattr_gettype_np",posix_attr_gettype},
    {"pthread_rwlockattr_setpshared",posix_attr_setpshared}, {"pthread_rwlockattr_getpshared",posix_attr_getpshared},
    {"pthread_rwlock_init",posix_init}, {"pthread_rwlock_destroy",posix_destroy},
    {"pthread_rwlock_rdlock",posix_read}, {"pthread_rwlock_wrlock",posix_write},
    {"pthread_rwlock_tryrdlock",posix_tryread}, {"pthread_rwlock_trywrlock",posix_trywrite},
    {"pthread_rwlock_timedrdlock",posix_timedread}, {"pthread_rwlock_timedwrlock",posix_timedwrite},
    {"pthread_rwlock_unlock",posix_unlock}, {"pthread_rwlock_setname_np",posix_setname},
};
uintptr_t runtime_rwlock_resolve(const char *sz_name) {
    return RUNTIME_LOOKUP(s_exports,sz_name);
}
void runtime_rwlock_report(void) {
    if (!runtime_exception_report_lock(&s_registry_lock,"rwlock")) return;
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",s_c_created,s_c_reads,s_c_writes,s_c_unlocks);
    runtime_exception_unlock(&s_registry_lock);
}
#else
uintptr_t runtime_rwlock_resolve(const char *name) { (void)name; return 0; }
void runtime_rwlock_report(void) { puts("Runtime: Windows rwlock backend not implemented"); }
#endif
