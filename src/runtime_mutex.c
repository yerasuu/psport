/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <time.h>
typedef struct { int type, protocol; } GuestAttr;
typedef struct { pthread_mutex_t native; } GuestMutex;
static size_t s_created, s_locks, s_unlocks;
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
    *pp_out = p_mutex; ++s_created; return 0;
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
    int r = pthread_mutex_trylock(&(*pp_mutex)->native);
    if (r == EBUSY) { /* contended: timed for the wait profile */
        const uint64_t start = runtime_wait_clock();
        r = pthread_mutex_lock(&(*pp_mutex)->native);
        runtime_wait_note(1, runtime_wait_clock() - start);
    }
    e = orbis_error(r);
    if (!e) ++s_locks;
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **pp_mutex) {
    int32_t e = ensure_mutex(pp_mutex);
    if (e) return e;
    e = orbis_error(pthread_mutex_trylock(&(*pp_mutex)->native));
    if (!e) ++s_locks;
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **pp_mutex) {
    if (!pp_mutex || (uintptr_t)*pp_mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*pp_mutex < 2) return orbis_error(EPERM);
    int32_t e = orbis_error(pthread_mutex_unlock(&(*pp_mutex)->native));
    if (!e) ++s_unlocks;
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **pp_mutex) {
    if (!pp_mutex || (uintptr_t)*pp_mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*pp_mutex < 2) return 0;
    int e = pthread_mutex_destroy(&(*pp_mutex)->native);
    if (!e) { free(*pp_mutex); *pp_mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
static int deadline_after(clockid_t clock, struct timespec *p_end, uint64_t usec) {
    if (clock_gettime(clock, p_end)) return errno;
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
    e = timed_error(pthread_mutex_timedlock(&(*pp_mutex)->native, &end));
    if (!e) ++s_locks;
    return e;
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
static size_t s_conds, s_waits, s_wakeups;
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
    *pp_out = p_cond; ++s_conds; return 0;
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
static ABI int32_t cond_wait(GuestCond **pp_cond, GuestMutex **pp_mutex) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    if (!pp_mutex || (uintptr_t)*pp_mutex < 3) return orbis_error(EINVAL);
    ++s_waits;
    const uint64_t start = runtime_wait_clock();
    const int e2 = pthread_cond_wait(&(*pp_cond)->native, &(*pp_mutex)->native);
    runtime_wait_note(0, runtime_wait_clock() - start);
    return orbis_error(e2);
}
static int32_t cond_wait_until(GuestCond **pp_cond, GuestMutex **pp_mutex, const struct timespec *p_end) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    if (!pp_mutex || (uintptr_t)*pp_mutex < 3 || !p_end) return orbis_error(EINVAL);
    ++s_waits;
    const uint64_t start = runtime_wait_clock();
    const int e2 = pthread_cond_timedwait(&(*pp_cond)->native, &(*pp_mutex)->native, p_end);
    runtime_wait_note(0, runtime_wait_clock() - start);
    return timed_error(e2);
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
    ++s_wakeups; return orbis_error(pthread_cond_signal(&(*pp_cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **pp_cond) {
    int32_t e = ensure_cond(pp_cond);
    if (e) return e;
    ++s_wakeups; return orbis_error(pthread_cond_broadcast(&(*pp_cond)->native));
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
};
uintptr_t runtime_mutex_resolve(const char *sz_name) {
    return RUNTIME_LOOKUP(s_exports, sz_name);
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", s_created, s_locks, s_unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", s_conds, s_waits, s_wakeups);
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
