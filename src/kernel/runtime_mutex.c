/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <time.h>
typedef struct { int type, protocol; } GuestAttr;
/* p_owner and c_holds mirror the host mutex's owner and recursion count as FreeBSD's libthr keeps
 * them: glibc neither refuses a normal mutex's unlock by another thread nor releases a recursive
 * one fully in a condition wait, and libkernel does both. c_holds is written only by the owner. */
typedef struct { pthread_mutex_t native; int type; const void *p_owner; unsigned c_holds; } GuestMutex;
static size_t s_c_created, s_c_locks, s_c_unlocks;
static size_t s_c_recursive_lock_failures, s_c_recursive_unlock_failures, s_c_foreign_unlocks, s_c_self_deadlocks;
enum { LOGGED_MUTEX_FAILURES=8 };
static __thread char s_owner_tag;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); runtime_stop(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **pp_out) {
    if (!pp_out) return orbis_error(EINVAL);
    GuestAttr *p_attr = malloc(sizeof(*p_attr));
    if (!p_attr) return orbis_error(ENOMEM);
    p_attr->type = 1; p_attr->protocol = 0; *pp_out = p_attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **pp_attr, int type) {
    if (!pp_attr || !*pp_attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*pp_attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **pp_attr, int protocol) {
    if (!pp_attr || !*pp_attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    (*pp_attr)->protocol = protocol; return 0;
}
static ABI int32_t attr_get_protocol(GuestAttr **pp_attr, int *p_protocol) {
    if (!pp_attr || !*pp_attr || !p_protocol) return orbis_error(EINVAL);
    *p_protocol = (*pp_attr)->protocol; return 0;
}
static ABI int32_t attr_destroy(GuestAttr **pp_attr) {
    if (!pp_attr || !*pp_attr) return orbis_error(EINVAL);
    free(*pp_attr); *pp_attr = NULL; return 0;
}
static int is_owner(const GuestMutex *p_mutex) { return __atomic_load_n(&p_mutex->p_owner,__ATOMIC_RELAXED)==&s_owner_tag; }
static void note_acquired(GuestMutex *p_mutex) {
    if (!p_mutex->c_holds++) __atomic_store_n(&p_mutex->p_owner,&s_owner_tag,__ATOMIC_RELAXED);
}
static void note_released(GuestMutex *p_mutex) {
    if (!--p_mutex->c_holds) __atomic_store_n(&p_mutex->p_owner,NULL,__ATOMIC_RELAXED);
}
static void log_mutex_failure(size_t *p_c_failures, const char *sz_what, const void *p_mutex, int e) {
    if (__atomic_fetch_add(p_c_failures,1,__ATOMIC_RELAXED)>=LOGGED_MUTEX_FAILURES) return;
    uint64_t sites[3];
    runtime_guest_call_sites(sites);
    printf("Runtime: %s %p failed with host error %d from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
           sz_what,p_mutex,e,sites[0],sites[1],sites[2]);
}
/* Busy try locks and expired timed locks are expected; anything else on a recursive mutex leaves
 * the caller unprotected, since games ignore what lock returns. */
static int finish_lock(GuestMutex *p_mutex, int e) {
    if (!e) { ++s_c_locks; note_acquired(p_mutex); return 0; }
    if (e==EDEADLK) log_mutex_failure(&s_c_self_deadlocks,"relock by its owner of non-recursive mutex",p_mutex,e);
    else if (p_mutex->type==2 && e!=EBUSY && e!=ETIMEDOUT) log_mutex_failure(&s_c_recursive_lock_failures,"lock of recursive mutex",p_mutex,e);
    return e;
}
static ABI int32_t mutex_init(GuestMutex **pp_out, GuestAttr **pp_attr, const char *sz_name) {
    (void)sz_name;
    if (!pp_out || (pp_attr && !*pp_attr)) return orbis_error(EINVAL);
    int type = pp_attr ? (*pp_attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *p_mutex = malloc(sizeof(*p_mutex));
    if (!p_mutex) return orbis_error(ENOMEM);
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (e) { free(p_mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&native_attr, native_type);
    if (!e) e = pthread_mutex_init(&p_mutex->native, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
    if (e) { free(p_mutex); return orbis_error(e); }
    p_mutex->type = type; p_mutex->p_owner = NULL; p_mutex->c_holds = 0;
    *pp_out = p_mutex; ++s_c_created; return 0;
}
static pthread_mutex_t s_static_init = PTHREAD_MUTEX_INITIALIZER;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **pp_mutex) {
    if (!pp_mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)pp_mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    pthread_mutex_lock(&s_static_init);
    int32_t e = 0;
    if ((uintptr_t)*pp_mutex < 2) {
        GuestMutex *p_created_mutex = NULL;
        e = mutex_init(&p_created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(pp_mutex, p_created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&s_static_init);
    return e;
}
static ABI int32_t mutex_lock(GuestMutex **pp_mutex) {
    int32_t e = ensure_mutex(pp_mutex);
    if (e) return e;
    GuestMutex *p_mutex = *pp_mutex;
    int r = pthread_mutex_trylock(&p_mutex->native);
    if (r == EBUSY) { /* contended: timed for the wait profile */
        const uint64_t start = runtime_wait_clock();
        RUNTIME_ZONE(zone, "mutex wait");
        r = pthread_mutex_lock(&p_mutex->native);
        RUNTIME_ZONE_END(zone);
        runtime_wait_note(1, runtime_wait_clock() - start);
    }
    return orbis_error(finish_lock(p_mutex, r));
}
static ABI int32_t mutex_trylock(GuestMutex **pp_mutex) {
    int32_t e = ensure_mutex(pp_mutex);
    if (e) return e;
    return orbis_error(finish_lock(*pp_mutex, pthread_mutex_trylock(&(*pp_mutex)->native)));
}
static ABI int32_t mutex_unlock(GuestMutex **pp_mutex) {
    if (!pp_mutex || (uintptr_t)*pp_mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*pp_mutex < 2) {
        log_mutex_failure(&s_c_foreign_unlocks, "unlock of never locked mutex", pp_mutex, EPERM);
        return orbis_error(EPERM);
    }
    GuestMutex *p_mutex = *pp_mutex;
    if (!is_owner(p_mutex)) {
        log_mutex_failure(&s_c_foreign_unlocks, "unlock by a thread not owning mutex", p_mutex, EPERM);
        if (p_mutex->type == 2) __atomic_fetch_add(&s_c_recursive_unlock_failures, 1, __ATOMIC_RELAXED);
        return orbis_error(EPERM);
    }
    note_released(p_mutex);
    const int e = pthread_mutex_unlock(&p_mutex->native);
    if (e) {
        if (p_mutex->type == 2) log_mutex_failure(&s_c_recursive_unlock_failures, "unlock by its owner of recursive mutex", p_mutex, e);
        return orbis_error(e);
    }
    ++s_c_unlocks;
    return 0;
}
static ABI int32_t mutex_destroy(GuestMutex **pp_mutex) {
    if (!pp_mutex || (uintptr_t)*pp_mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*pp_mutex < 2) return 0;
    int e = pthread_mutex_destroy(&(*pp_mutex)->native);
    if (!e) { free(*pp_mutex); *pp_mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
static int deadline_after(clockid_t clock, struct timespec *p_end, uint64_t usec) {
    const int e = ps_host_time_read(clock == CLOCK_MONOTONIC ? PS_HOST_CLOCK_MONOTONIC : PS_HOST_CLOCK_REALTIME, p_end);
    if (e) return e;
    uint64_t ns = (uint64_t)p_end->tv_nsec + (usec % 1000000) * 1000;
    p_end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    p_end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **pp_mutex, uint32_t usec) {
    int32_t e = ensure_mutex(pp_mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(CLOCK_REALTIME, &end, usec)) return orbis_error(EINVAL);
    return timed_error(finish_lock(*pp_mutex, pthread_mutex_timedlock(&(*pp_mutex)->native, &end)));
}
/* A relative timeout of 64-bit microseconds, unlike scePthreadMutexTimedlock's 32. */
static int32_t mutex_reltimedlock(GuestMutex **pp_mutex, uint64_t usec) {
    int32_t e = ensure_mutex(pp_mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(CLOCK_REALTIME, &end, usec)) return orbis_error(EINVAL);
    return timed_error(finish_lock(*pp_mutex, pthread_mutex_timedlock(&(*pp_mutex)->native, &end)));
}
/* Names are for the console's debugger only and the runtime keeps none (scePthreadMutexInit's
 * neither): libkernel puts "SceNoNameMutex" in place of a null one and never fails. */
static ABI int32_t posix_mutex_setname(GuestMutex **pp_mutex, const char *sz_name) {
    (void)pp_mutex; (void)sz_name;
    return 0;
}
/* Condition attributes: the clock timed waits measure deadlines on (FreeBSD ids: 0 realtime,
 * 1 virtual, 2 prof, 4 monotonic) and process sharing, of which only private exists. */
typedef struct { int32_t clock; } GuestCondAttr;
static ABI int32_t condattr_init(GuestCondAttr **pp_out) {
    if (!pp_out) return orbis_error(EINVAL);
    GuestCondAttr *p_attr = malloc(sizeof(*p_attr));
    if (!p_attr) return orbis_error(ENOMEM);
    p_attr->clock = 0; *pp_out = p_attr; return 0;
}
static ABI int32_t condattr_destroy(GuestCondAttr **pp_attr) {
    if (!pp_attr || !*pp_attr) return orbis_error(EINVAL);
    free(*pp_attr); *pp_attr = NULL; return 0;
}
static ABI int32_t condattr_getclock(GuestCondAttr **pp_attr, int32_t *p_clock) {
    if (!pp_attr || !*pp_attr || !p_clock) return orbis_error(EINVAL);
    *p_clock = (*pp_attr)->clock; return 0;
}
static ABI int32_t condattr_setclock(GuestCondAttr **pp_attr, int32_t clock) {
    if (!pp_attr || !*pp_attr || (clock != 0 && clock != 1 && clock != 2 && clock != 4)) return orbis_error(EINVAL);
    (*pp_attr)->clock = clock; return 0;
}
static ABI int32_t condattr_getpshared(GuestCondAttr **pp_attr, int32_t *p_shared) {
    if (!pp_attr || !*pp_attr || !p_shared) return orbis_error(EINVAL);
    *p_shared = 0; return 0;
}
static ABI int32_t condattr_setpshared(GuestCondAttr **pp_attr, int32_t shared) {
    if (!pp_attr || !*pp_attr || shared) return orbis_error(EINVAL);
    return 0;
}
typedef struct { pthread_cond_t native; clockid_t clock; } GuestCond;
static size_t s_c_conds, s_c_waits, s_c_wakeups;
static ABI int32_t cond_init(GuestCond **pp_out, GuestCondAttr **pp_attr, const char *sz_name) {
    (void)sz_name;
    if (!pp_out) return orbis_error(EINVAL);
    GuestCond *p_cond = malloc(sizeof(*p_cond));
    if (!p_cond) return orbis_error(ENOMEM);
    p_cond->clock = pp_attr && *pp_attr && (*pp_attr)->clock == 4 ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    pthread_condattr_t native_attr;
    int e = pthread_condattr_init(&native_attr);
    if (!e) e = pthread_condattr_setclock(&native_attr, p_cond->clock);
    if (!e) e = pthread_cond_init(&p_cond->native, &native_attr);
    pthread_condattr_destroy(&native_attr);
    if (e) { free(p_cond); return orbis_error(e); }
    *pp_out = p_cond; ++s_c_conds; return 0;
}
static int32_t ensure_cond(GuestCond **pp_cond) {
    if (!pp_cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)pp_cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    pthread_mutex_lock(&s_static_init);
    int32_t e = 0;
    if ((uintptr_t)*pp_cond < 2) {
        GuestCond *p_cond = NULL;
        e = cond_init(&p_cond, NULL, NULL);
        if (!e) __atomic_store_n(pp_cond, p_cond, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&s_static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **pp_cond) {
    if (!pp_cond) return orbis_error(EINVAL);
    if ((uintptr_t)*pp_cond < 2) return 0;
    int e = pthread_cond_destroy(&(*pp_cond)->native);
    if (!e) { free(*pp_cond); *pp_cond = NULL; }
    return orbis_error(e);
}
/* The wait releases every hold of a recursive mutex and takes them all back after it, as libthr's
 * _mutex_cv_unlock does: glibc drops one, so the mutex stayed locked for the whole wait. */
static int wait_released(GuestCond *p_cond, GuestMutex *p_mutex, const struct timespec *p_end) {
    if (!is_owner(p_mutex)) {
        log_mutex_failure(&s_c_foreign_unlocks, "condition wait by a thread not owning mutex", p_mutex, EPERM);
        return EPERM;
    }
    const unsigned c_holds = p_mutex->c_holds;
    /* An owner's unlock of a recursive mutex held more than once only decrements its count. */
    for (unsigned i = 1; i < c_holds; ++i) (void)pthread_mutex_unlock(&p_mutex->native);
    p_mutex->c_holds = 1;
    note_released(p_mutex);
    ++s_c_waits;
    const uint64_t start = runtime_wait_clock();
    RUNTIME_ZONE(zone, "cond wait");
    const int e = p_end ? pthread_cond_timedwait(&p_cond->native, &p_mutex->native, p_end)
                        : pthread_cond_wait(&p_cond->native, &p_mutex->native);
    RUNTIME_ZONE_END(zone);
    runtime_wait_note(0, runtime_wait_clock() - start);
    note_acquired(p_mutex);
    /* The owner's relock only increments the count, back to where it was before the wait. */
    for (unsigned i = 1; i < c_holds; ++i) (void)pthread_mutex_lock(&p_mutex->native);
    p_mutex->c_holds = c_holds;
    return e;
}
static ABI int32_t cond_wait(GuestCond **pp_cond, GuestMutex **pp_mutex) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    if (!pp_mutex || (uintptr_t)*pp_mutex < 3) return orbis_error(EINVAL);
    return orbis_error(wait_released(*pp_cond, *pp_mutex, NULL));
}
static int32_t cond_wait_until(GuestCond **pp_cond, GuestMutex **pp_mutex, const struct timespec *p_end) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    if (!pp_mutex || (uintptr_t)*pp_mutex < 3 || !p_end) return orbis_error(EINVAL);
    return timed_error(wait_released(*pp_cond, *pp_mutex, p_end));
}
static ABI int32_t cond_timedwait(GuestCond **pp_cond, GuestMutex **pp_mutex, uint32_t usec) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    struct timespec end;
    if (deadline_after((*pp_cond)->clock, &end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(pp_cond, pp_mutex, &end);
}
static ABI int32_t cond_signal(GuestCond **pp_cond) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    ++s_c_wakeups; return orbis_error(pthread_cond_signal(&(*pp_cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **pp_cond) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    ++s_c_wakeups; return orbis_error(pthread_cond_broadcast(&(*pp_cond)->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **pp_cond, GuestMutex **pp_mutex, const GuestTimespec *p_abs) {
    if (!p_abs || p_abs->nsec < 0 || p_abs->nsec >= 1000000000) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)p_abs->sec, .tv_nsec = (long)p_abs->nsec};
    return cond_wait_until(pp_cond, pp_mutex, &end);
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **pp_out) { return posix_result(attr_init(pp_out)); }
static ABI int32_t posix_attr_type(GuestAttr **pp_attr,int type) { return posix_result(attr_type(pp_attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **pp_attr) { return posix_result(attr_destroy(pp_attr)); }
static ABI int32_t posix_attr_protocol(GuestAttr **pp_attr,int protocol) { return posix_result(attr_protocol(pp_attr,protocol)); }
static ABI int32_t posix_attr_get_protocol(GuestAttr **pp_attr,int *p_protocol) { return posix_result(attr_get_protocol(pp_attr,p_protocol)); }
static ABI int32_t posix_mutex_init(GuestMutex **pp_out,GuestAttr **pp_attr) { return posix_result(mutex_init(pp_out,pp_attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **pp_mutex) { return posix_result(mutex_destroy(pp_mutex)); }
static ABI int32_t posix_mutex_lock(GuestMutex **pp_mutex) { return posix_result(mutex_lock(pp_mutex)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **pp_mutex) { return posix_result(mutex_trylock(pp_mutex)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **pp_mutex) { return posix_result(mutex_unlock(pp_mutex)); }
static ABI int32_t posix_cond_init(GuestCond **pp_cond, GuestCondAttr **pp_cond_attr) { return posix_result(cond_init(pp_cond, pp_cond_attr, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **pp_cond) { return posix_result(cond_destroy(pp_cond)); }
static ABI int32_t posix_cond_wait(GuestCond **pp_cond, GuestMutex **pp_mutex) { return posix_result(cond_wait(pp_cond, pp_mutex)); }
static ABI int32_t posix_cond_timedwait(GuestCond **pp_cond, GuestMutex **pp_mutex, const GuestTimespec *p_timespec) { return posix_result(cond_abs_timedwait(pp_cond, pp_mutex, p_timespec)); }
static ABI int32_t posix_cond_signal(GuestCond **pp_cond) { return posix_result(cond_signal(pp_cond)); }
static ABI int32_t posix_cond_broadcast(GuestCond **pp_cond) { return posix_result(cond_broadcast(pp_cond)); }
static ABI int32_t posix_cond_setname(GuestCond **pp_cond, const char *sz_name) { (void)pp_cond; return sz_name ? 0 : EINVAL; }
static ABI int32_t posix_mutex_reltimedlock(GuestMutex **pp_mutex, uint64_t usec) { return posix_result(mutex_reltimedlock(pp_mutex, usec)); }
static ABI int32_t posix_condattr_init(GuestCondAttr **pp_cond_attr) { return posix_result(condattr_init(pp_cond_attr)); }
static ABI int32_t posix_condattr_destroy(GuestCondAttr **pp_cond_attr) { return posix_result(condattr_destroy(pp_cond_attr)); }
static ABI int32_t posix_condattr_getclock(GuestCondAttr **pp_cond_attr, int32_t *p_clock) { return posix_result(condattr_getclock(pp_cond_attr, p_clock)); }
static ABI int32_t posix_condattr_setclock(GuestCondAttr **pp_cond_attr, int32_t c) { return posix_result(condattr_setclock(pp_cond_attr, c)); }
static ABI int32_t posix_condattr_getpshared(GuestCondAttr **pp_cond_attr, int32_t *p_shared) { return posix_result(condattr_getpshared(pp_cond_attr, p_shared)); }
static ABI int32_t posix_condattr_setpshared(GuestCondAttr **pp_cond_attr, int32_t p) { return posix_result(condattr_setpshared(pp_cond_attr, p)); }
static const RuntimeExport s_exports[] = {
    {"pthread_cond_init",posix_cond_init}, {"pthread_cond_destroy",posix_cond_destroy},
    {"pthread_cond_wait",posix_cond_wait}, {"pthread_cond_timedwait",posix_cond_timedwait},
    {"pthread_cond_signal",posix_cond_signal}, {"pthread_cond_broadcast",posix_cond_broadcast},
    {"scePthreadCondInit",cond_init}, {"scePthreadCondDestroy",cond_destroy}, {"scePthreadCondWait",cond_wait},
    {"scePthreadCondTimedwait",cond_timedwait}, {"scePthreadCondSignal",cond_signal},
    {"scePthreadCondBroadcast",cond_broadcast}, {"scePthreadMutexTimedlock",mutex_timedlock},
    {"pthread_mutexattr_init",posix_attr_init}, {"pthread_mutexattr_settype",posix_attr_type},
    {"pthread_mutexattr_destroy",posix_attr_destroy}, {"pthread_mutex_init",posix_mutex_init},
    {"pthread_mutex_destroy",posix_mutex_destroy}, {"pthread_mutex_lock",posix_mutex_lock},
    {"pthread_mutex_trylock",posix_mutex_trylock}, {"pthread_mutex_unlock",posix_mutex_unlock},
    {"scePthreadMutexattrInit",attr_init}, {"scePthreadMutexattrSettype",attr_type},
    {"scePthreadMutexattrDestroy",attr_destroy}, {"scePthreadMutexattrSetprotocol",attr_protocol},
    {"scePthreadMutexInit",mutex_init}, {"scePthreadMutexLock",mutex_lock}, {"scePthreadMutexTrylock",mutex_trylock},
    {"scePthreadMutexUnlock",mutex_unlock}, {"scePthreadMutexDestroy",mutex_destroy},
    {"scePthreadCondattrInit", condattr_init}, {"scePthreadCondattrDestroy", condattr_destroy},
    {"scePthreadCondattrGetclock", condattr_getclock}, {"scePthreadCondattrSetclock", condattr_setclock},
    {"scePthreadCondattrGetpshared", condattr_getpshared}, {"scePthreadCondattrSetpshared", condattr_setpshared},
    {"pthread_condattr_init", posix_condattr_init}, {"pthread_condattr_destroy", posix_condattr_destroy},
    {"pthread_condattr_getclock", posix_condattr_getclock}, {"pthread_condattr_setclock", posix_condattr_setclock},
    {"pthread_condattr_getpshared", posix_condattr_getpshared}, {"pthread_condattr_setpshared", posix_condattr_setpshared},
    {"pthread_mutexattr_setprotocol", posix_attr_protocol}, {"pthread_mutexattr_getprotocol", posix_attr_get_protocol},
    {"pthread_mutex_setname_np", posix_mutex_setname}, {"pthread_cond_setname_np", posix_cond_setname},
    {"pthread_mutex_reltimedlock_np", posix_mutex_reltimedlock},
};
uintptr_t runtime_mutex_resolve(const char *sz_name) {
    return RUNTIME_LOOKUP(s_exports, sz_name);
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", s_c_created, s_c_locks, s_c_unlocks);
    printf("Runtime: mutex failures: recursive lock=%zu, recursive unlock=%zu, unlock by non-owner=%zu, "
           "relock of non-recursive=%zu\n",
           s_c_recursive_lock_failures, s_c_recursive_unlock_failures, s_c_foreign_unlocks, s_c_self_deadlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", s_c_conds, s_c_waits, s_c_wakeups);
}
#else
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
static const RuntimeExport exports[] = {
    {"pthread_cond_init",posix_cond_init}, {"pthread_cond_destroy",posix_cond_destroy},
    {"pthread_cond_wait",posix_cond_wait}, {"pthread_cond_timedwait",posix_cond_timedwait},
    {"pthread_cond_signal",posix_cond_signal}, {"pthread_cond_broadcast",posix_cond_broadcast},
    {"scePthreadCondInit",cond_init}, {"scePthreadCondDestroy",cond_destroy}, {"scePthreadCondWait",cond_wait},
    {"scePthreadCondTimedwait",cond_timedwait}, {"scePthreadCondSignal",cond_signal},
    {"scePthreadCondBroadcast",cond_broadcast}, {"scePthreadMutexTimedlock",mutex_timedlock},
};
uintptr_t runtime_mutex_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
void runtime_mutex_report(void) { puts("Runtime: Windows mutex backend not implemented"); }
#endif
