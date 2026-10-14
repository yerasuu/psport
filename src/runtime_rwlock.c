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
    struct Holder *next;
} Holder;
typedef struct Rwlock {
    pthread_rwlock_t native;
    Holder *holders;
    unsigned inflight;
    struct Rwlock *next;
} Rwlock;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
enum { RWLOCK_NORMAL=1, RWLOCK_PREFER_READER=2 };
typedef struct { int32_t type; } RwlockAttr;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static Rwlock *registry;
static size_t created, reads, writes, unlocks;
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
/* All helpers ending in _locked require registry_lock. */
static Rwlock *find_locked(Rwlock *handle) {
    for (Rwlock *r=registry; r; r=r->next) if (r==handle) return r;
    return NULL;
}
/* libkernel's rwlocks block new readers while a writer waits unless the type prefers readers. */
static int create_locked(Rwlock **out, int32_t type) {
    Rwlock *r=calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    pthread_rwlockattr_t native_attr;
    int e=pthread_rwlockattr_init(&native_attr);
    if (!e) e=pthread_rwlockattr_setkind_np(&native_attr,type==RWLOCK_PREFER_READER ?
        PTHREAD_RWLOCK_PREFER_READER_NP : PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    if (!e) e=pthread_rwlock_init(&r->native,&native_attr);
    pthread_rwlockattr_destroy(&native_attr);
    if (e) { free(r); return e; }
    r->next=registry; registry=r; *out=r; ++created; return 0;
}
static ABI int32_t attr_init(RwlockAttr **out) {
    if (!out) return error(EINVAL);
    RwlockAttr *attr=malloc(sizeof(*attr));
    if (!attr) return error(ENOMEM);
    attr->type=RWLOCK_NORMAL; *out=attr; return 0;
}
static ABI int32_t attr_destroy(RwlockAttr **attr) {
    if (!attr || !*attr) return error(EINVAL);
    free(*attr); *attr=NULL; return 0;
}
static ABI int32_t attr_settype(RwlockAttr **attr, int32_t type) {
    if (!attr || !*attr || (type!=RWLOCK_NORMAL && type!=RWLOCK_PREFER_READER)) return error(EINVAL);
    (*attr)->type=type; return 0;
}
static ABI int32_t attr_gettype(RwlockAttr **attr, int32_t *type) {
    if (!attr || !*attr || !type) return error(EINVAL);
    *type=(*attr)->type; return 0;
}
static ABI int32_t attr_setpshared(RwlockAttr **attr, int32_t shared) {
    if (!attr || !*attr || shared) return error(EINVAL);
    return 0;
}
static ABI int32_t attr_getpshared(RwlockAttr **attr, int32_t *shared) {
    if (!attr || !*attr || !shared) return error(EINVAL);
    *shared=0; return 0;
}
static ABI int32_t rw_init(Rwlock **out, RwlockAttr *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    int e=create_locked(out,attr ? (*attr)->type : RWLOCK_NORMAL);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static void prune_locked(Rwlock *r, Holder *h) {
    if (h->readers || h->writer || h->pending) return;
    Holder **link=&r->holders;
    while (*link!=h) link=&(*link)->next;
    *link=h->next; free(h);
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    pthread_mutex_lock(&registry_lock);
    int e=0;
    if (!*handle) e=create_locked(handle,RWLOCK_NORMAL);
    Rwlock *r=e ? NULL : find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(e ? e : EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (h && (h->writer || (writer && h->readers))) {
        pthread_mutex_unlock(&registry_lock); return error(attempt ? EBUSY : EDEADLK);
    }
    if (h && h->readers==UINT_MAX) { pthread_mutex_unlock(&registry_lock); return error(EAGAIN); }
    /* libthr lets a thread already reading take the lock again past waiting writers, so only its
       first read holds the native lock; otherwise writer preference would deadlock it. */
    if (h && h->readers) { ++h->readers; ++reads; pthread_mutex_unlock(&registry_lock); return 0; }
    if (!h) {
        h=calloc(1,sizeof(*h));
        if (!h) { pthread_mutex_unlock(&registry_lock); return error(ENOMEM); }
        h->thread=pthread_self(); h->next=r->holders; r->holders=h;
    }
    ++r->inflight; ++h->pending;
    pthread_mutex_unlock(&registry_lock);
    /* Timed operations first try the lock, matching POSIX timeout validation. */
    if (attempt || timed) e=writer ? pthread_rwlock_trywrlock(&r->native) : pthread_rwlock_tryrdlock(&r->native);
    else e=writer ? pthread_rwlock_wrlock(&r->native) : pthread_rwlock_rdlock(&r->native);
    if (timed && e==EBUSY) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000) e=EINVAL;
        else {
            struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds};
            e=writer ? pthread_rwlock_timedwrlock(&r->native,&deadline) : pthread_rwlock_timedrdlock(&r->native,&deadline);
        }
    }
    pthread_mutex_lock(&registry_lock);
    --r->inflight; --h->pending;
    if (!e) {
        if (writer) { h->writer=1; ++writes; }
        else { ++h->readers; ++reads; }
    }
    prune_locked(r,h);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    Rwlock *r=find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(EINVAL); }
    Holder *h=r->holders;
    while (h && !pthread_equal(h->thread,pthread_self())) h=h->next;
    if (!h || (!h->readers && !h->writer)) { pthread_mutex_unlock(&registry_lock); return error(EPERM); }
    int e=h->writer || h->readers==1 ? pthread_rwlock_unlock(&r->native) : 0;
    if (!e) { if (h->writer) h->writer=0; else --h->readers; ++unlocks; }
    prune_locked(r,h);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    if (!*handle) { pthread_mutex_unlock(&registry_lock); return 0; }
    Rwlock *r=find_locked(*handle);
    if (!r) { pthread_mutex_unlock(&registry_lock); return error(EINVAL); }
    if (r->inflight || r->holders) { pthread_mutex_unlock(&registry_lock); return error(EBUSY); }
    int e=pthread_rwlock_destroy(&r->native);
    if (!e) {
        Rwlock **link=&registry;
        while (*link!=r) link=&(*link)->next;
        *link=r->next; free(r); *handle=(Rwlock *)(uintptr_t)1;
    }
    pthread_mutex_unlock(&registry_lock); return error(e);
}
static int32_t posix_result(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(RwlockAttr **a) { return posix_result(attr_init(a)); }
static ABI int32_t posix_attr_destroy(RwlockAttr **a) { return posix_result(attr_destroy(a)); }
static ABI int32_t posix_attr_settype(RwlockAttr **a,int32_t t) { return posix_result(attr_settype(a,t)); }
static ABI int32_t posix_attr_gettype(RwlockAttr **a,int32_t *t) { return posix_result(attr_gettype(a,t)); }
static ABI int32_t posix_attr_setpshared(RwlockAttr **a,int32_t s) { return posix_result(attr_setpshared(a,s)); }
static ABI int32_t posix_attr_getpshared(RwlockAttr **a,int32_t *s) { return posix_result(attr_getpshared(a,s)); }
static ABI int32_t posix_init(Rwlock **out,RwlockAttr *const *attr) { return posix_result(rw_init(out,attr,NULL)); }
static ABI int32_t posix_destroy(Rwlock **r) { return posix_result(rw_destroy(r)); }
static ABI int32_t posix_read(Rwlock **r) { return posix_result(rw_read(r)); }
static ABI int32_t posix_write(Rwlock **r) { return posix_result(rw_write(r)); }
static ABI int32_t posix_tryread(Rwlock **r) { return posix_result(rw_tryread(r)); }
static ABI int32_t posix_trywrite(Rwlock **r) { return posix_result(rw_trywrite(r)); }
static ABI int32_t posix_timedread(Rwlock **r,const GuestTime *t) { return posix_result(rw_timedread(r,t)); }
static ABI int32_t posix_timedwrite(Rwlock **r,const GuestTime *t) { return posix_result(rw_timedwrite(r,t)); }
static ABI int32_t posix_unlock(Rwlock **r) { return posix_result(rw_unlock(r)); }
static const RuntimeExport exports[]={
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
    {"pthread_rwlock_unlock",posix_unlock},
};
uintptr_t runtime_rwlock_resolve(const char *name) {
    return RUNTIME_LOOKUP(exports,name);
}
void runtime_rwlock_report(void) {
    pthread_mutex_lock(&registry_lock);
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",created,reads,writes,unlocks);
    pthread_mutex_unlock(&registry_lock);
}
#else
uintptr_t runtime_rwlock_resolve(const char *name) { (void)name; return 0; }
void runtime_rwlock_report(void) { puts("Runtime: Windows rwlock backend not implemented"); }
#endif
