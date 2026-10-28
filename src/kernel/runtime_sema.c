// SPDX-License-Identifier: GPL-2.0-or-later
/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. POSIX semaphores and _umtx_op below. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int is_done;
    pthread_cond_t event;
    struct Waiter *p_next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t c_resources, c_initial, c_maximum;
    unsigned active;
    int deleted;
    Waiter *p_first;
    struct Sema *p_next;
} Sema;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *s_p_registry;
static uint32_t s_next_id=1;
static size_t s_c_created, s_c_deleted, s_c_acquired, s_c_signaled, s_c_timed_out;
static Sema *find(uint32_t id) {
    for (Sema *p_sema=s_p_registry;p_sema;p_sema=p_sema->p_next) if (p_sema->id==id) return p_sema;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); runtime_stop(21); }
}
static uint64_t now_ns(void) { return ps_host_time_monotonic_ns(); }
static ABI int32_t sem_create(uint32_t *p_out,const char *sz_name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *p_options) {
    if (!p_out || !sz_name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (p_options) { fputs("STOP: semaphore options are not implemented\n",stderr); runtime_stop(21); }
    Sema *p_sema=calloc(1,sizeof(*p_sema));
    if (!p_sema) return ERR(12);
    runtime_exception_lock(&s_lock);
    if (s_next_id==UINT32_MAX) { runtime_exception_unlock(&s_lock); free(p_sema); return ERR(28); }
    p_sema->id=s_next_id++; p_sema->attr=attr; p_sema->c_resources=p_sema->c_initial=initial; p_sema->c_maximum=maximum;
    uint32_t id=p_sema->id;
    p_sema->p_next=s_p_registry; s_p_registry=p_sema; *p_out=id; ++s_c_created;
    runtime_exception_unlock(&s_lock);
    if (runtime_env("PS_TRACE_SEMA",NULL)) printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",id,attr,initial,maximum);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *p_timeout,int block) {
    runtime_exception_lock(&s_lock);
    Sema *p_sema=find(id);
    int32_t result=0;
    if (!p_sema) result=ERR(3);
    else if (need<=0 || need>p_sema->c_maximum) result=ERR(22);
    else if (p_sema->c_resources>=need) { p_sema->c_resources-=need; ++s_c_acquired; }
    else if (!block) result=ERR(16);
    else if (p_timeout && !*p_timeout) { result=ERR(60); ++s_c_timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (runtime_env("PS_TRACE_SEMA",NULL)) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,p_sema->c_resources); /* deadlock diagnosis */
        Waiter w={.need=need};
        host_check(pthread_cond_init(&w.event,NULL));
        Waiter **pp_tail=&p_sema->p_first;
        while (*pp_tail) pp_tail=&(*pp_tail)->p_next;
        *pp_tail=&w; ++p_sema->active;
        const uint64_t wait_start=runtime_wait_clock();
        uint64_t deadline=p_timeout ? now_ns()+(uint64_t)*p_timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        RUNTIME_ZONE(zone,"sema wait");
        while (!w.is_done) {
            int e=runtime_exception_wait(&w.event,&s_lock,p_timeout ? &end : NULL);
            if (e==ETIMEDOUT && !w.is_done) {
                Waiter **pp_waiter=&p_sema->p_first;
                while (*pp_waiter!=&w) pp_waiter=&(*pp_waiter)->p_next;
                *pp_waiter=w.p_next; w.result=ERR(60); w.is_done=1; ++s_c_timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        RUNTIME_ZONE_END(zone);
        runtime_wait_note(2,runtime_wait_clock()-wait_start);
        result=w.result;
        if (!result) ++s_c_acquired;
        if (p_timeout) {
            uint64_t now=now_ns();
            *p_timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--p_sema->active && p_sema->deleted) free(p_sema);
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *p_timeout) { return wait_count(id,need,p_timeout,1); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t c_resources) {
    runtime_exception_lock(&s_lock);
    Sema *p_sema=find(id);
    int32_t result=0;
    if (!p_sema) result=ERR(3);
    else if (c_resources<=0 || c_resources>p_sema->c_maximum-p_sema->c_resources) result=ERR(22);
    else {
        p_sema->c_resources+=c_resources; ++s_c_signaled;
        Waiter **pp_waiter=&p_sema->p_first;
        while (*pp_waiter) {
            Waiter *p_waiter=*pp_waiter;
            if (p_waiter->need>p_sema->c_resources) { pp_waiter=&p_waiter->p_next; continue; }
            p_sema->c_resources-=p_waiter->need; *pp_waiter=p_waiter->p_next; p_waiter->is_done=1;
            host_check(pthread_cond_signal(&p_waiter->event));
        }
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static int32_t wake_all(Sema *p_sema,int32_t result) {
    int32_t c_woken=0;
    while (p_sema->p_first) {
        Waiter *p_waiter=p_sema->p_first; p_sema->p_first=p_waiter->p_next;
        p_waiter->result=result; p_waiter->is_done=1; ++c_woken;
        host_check(pthread_cond_signal(&p_waiter->event));
    }
    return c_woken;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t c_resources,int32_t *p_waiters) {
    runtime_exception_lock(&s_lock);
    Sema *p_sema=find(id);
    int32_t result=0;
    if (!p_sema) result=ERR(3);
    else if (c_resources>p_sema->c_maximum) result=ERR(22);
    else {
        int32_t c_woken=wake_all(p_sema,ERR(85));
        if (p_waiters) *p_waiters=c_woken;
        p_sema->c_resources=c_resources<0 ? p_sema->c_initial : c_resources;
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    runtime_exception_lock(&s_lock);
    Sema **pp_sema=&s_p_registry;
    while (*pp_sema && (*pp_sema)->id!=id) pp_sema=&(*pp_sema)->p_next;
    if (!*pp_sema) { runtime_exception_unlock(&s_lock); return ERR(3); }
    Sema *p_sema=*pp_sema; *pp_sema=p_sema->p_next; p_sema->deleted=1; ++s_c_deleted;
    wake_all(p_sema,ERR(13));
    if (!p_sema->active) free(p_sema);
    runtime_exception_unlock(&s_lock);
    return 0;
}
/* scePthreadSem*: POSIX-style semaphores whose handle the game keeps in its own 8 bytes; backed by a
 * kernel semaphore whose id lives in that handle. */
static ABI int32_t psem_init(uint64_t *p_sem,int flag,uint32_t value,const char *sz_name) {
    (void)flag;
    if (!p_sem || value>INT32_MAX) return ERR(22);
    uint32_t id;
    int32_t result=sem_create(&id,sz_name ? sz_name : "",1,(int32_t)value,INT32_MAX,NULL);
    if (!result) *p_sem=id;
    return result;
}
static ABI int32_t psem_wait(uint64_t *p_sem) { return p_sem ? wait_count((uint32_t)*p_sem,1,NULL,1) : ERR(22); }
static ABI int32_t psem_trywait(uint64_t *p_sem) {
    if (!p_sem) return ERR(22);
    int32_t result=wait_count((uint32_t)*p_sem,1,NULL,0);
    return result==ERR(16) ? ERR(35) : result; /* POSIX trywait reports EAGAIN, the kernel call EBUSY */
}
static ABI int32_t psem_timedwait(uint64_t *p_sem,uint32_t usec) {
    return p_sem ? wait_count((uint32_t)*p_sem,1,&usec,1) : ERR(22);
}
static ABI int32_t psem_post(uint64_t *p_sem) { return p_sem ? sem_signal((uint32_t)*p_sem,1) : ERR(22); }
static ABI int32_t psem_destroy(uint64_t *p_sem) { return p_sem ? sem_delete((uint32_t)*p_sem) : ERR(22); }
static ABI int32_t psem_getvalue(uint64_t *p_sem,int32_t *p_value) {
    if (!p_sem || !p_value) return ERR(22);
    runtime_exception_lock(&s_lock);
    Sema *p_sema=find((uint32_t)*p_sem);
    if (p_sema) *p_value=p_sema->c_resources;
    runtime_exception_unlock(&s_lock);
    return p_sema ? 0 : ERR(22);
}
/* POSIX sem_t as the console's libkernel lays it out inline (FreeBSD's struct _usem behind a 16-bit magic
 * and the id of its named object), so a semaphore the game initializes itself works with these calls.
 * Waiters sleep on the count word. */
typedef struct {
    uint16_t magic, object;
    _Atomic uint32_t has_waiters, count;
    uint32_t flags;
} GuestSem;
enum { SEM_MAGIC=0x736d, SEM_NO_OBJECT=0xffff, SEM_SHARED=1, SEM_NAMED=2 };
static int usem_init(GuestSem *p_sem,int shared,uint32_t value) {
    if (!p_sem || value>INT32_MAX) return 22;
    p_sem->magic=SEM_MAGIC; p_sem->object=SEM_NO_OBJECT; p_sem->flags=shared ? SEM_SHARED : 0;
    atomic_store(&p_sem->has_waiters,0); atomic_store(&p_sem->count,value);
    return 0;
}
static int usem_valid(const GuestSem *p_sem) { return p_sem && p_sem->magic==SEM_MAGIC; }
static int usem_is_private(const GuestSem *p_sem) { return !(p_sem->flags&SEM_SHARED); }
static int usem_take(GuestSem *p_sem) {
    uint32_t c_resources=atomic_load(&p_sem->count);
    while (c_resources) if (atomic_compare_exchange_weak(&p_sem->count,&c_resources,c_resources-1)) return 1;
    return 0;
}
static int usem_trywait(GuestSem *p_sem) { return !usem_valid(p_sem) ? 22 : usem_take(p_sem) ? 0 : 35; }
/* end: absolute time on `clock`, NULL to wait without a limit. */
static int usem_wait(GuestSem *p_sem,PsHostClock clock,const struct timespec *p_end) {
    if (!usem_valid(p_sem)) return 22;
    if (usem_take(p_sem)) return 0;
    if (p_end && (p_end->tv_nsec<0 || p_end->tv_nsec>=1000000000)) return 22;
    const uint64_t wait_start=runtime_wait_clock();
    int result=0;
    while (!usem_take(p_sem)) {
        atomic_store(&p_sem->has_waiters,1);
        if (ps_host_wait_on_address((const void *)&p_sem->count,0,usem_is_private(p_sem),clock,p_end)==ETIMEDOUT) {
            result=usem_take(p_sem) ? 0 : 60;
            break;
        }
    }
    runtime_wait_note(2,runtime_wait_clock()-wait_start);
    return result;
}
static int usem_post(GuestSem *p_sem) {
    if (!usem_valid(p_sem)) return 22;
    atomic_fetch_add(&p_sem->count,1);
    if (atomic_load(&p_sem->has_waiters)) ps_host_wake_address((const void *)&p_sem->count,1,usem_is_private(p_sem));
    return 0;
}
static int usem_destroy(GuestSem *p_sem) {
    if (!usem_valid(p_sem) || p_sem->flags&SEM_NAMED) return 22;
    p_sem->magic=0;
    return 0;
}
static int usem_getvalue(GuestSem *p_sem,int32_t *p_value) {
    if (!usem_valid(p_sem) || !p_value) return 22;
    *p_value=(int32_t)atomic_load(&p_sem->count);
    return 0;
}
static int32_t posix_result(int e) {
    if (!e) return 0;
    *runtime_errno()=e;
    return -1;
}
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
static ABI int32_t posix_sem_init(GuestSem *p_sem,int shared,uint32_t value) { return posix_result(usem_init(p_sem,shared,value)); }
static ABI int32_t posix_sem_destroy(GuestSem *p_sem) { return posix_result(usem_destroy(p_sem)); }
static ABI int32_t posix_sem_wait(GuestSem *p_sem) { return posix_result(usem_wait(p_sem,PS_HOST_CLOCK_MONOTONIC,NULL)); }
static ABI int32_t posix_sem_trywait(GuestSem *p_sem) { return posix_result(usem_trywait(p_sem)); }
static ABI int32_t posix_sem_post(GuestSem *p_sem) { return posix_result(usem_post(p_sem)); }
static ABI int32_t posix_sem_getvalue(GuestSem *p_sem,int32_t *p_value) { return posix_result(usem_getvalue(p_sem,p_value)); }
/* A name is for the console's debugger only; the runtime keeps none. */
static ABI int32_t posix_sem_setname(GuestSem *p_sem,const char *sz_name) { (void)p_sem; return posix_result(sz_name ? 0 : 22); }
static ABI int32_t posix_sem_timedwait(GuestSem *p_sem,const GuestTime *p_end) {
    if (!p_end) return posix_result(usem_wait(p_sem,PS_HOST_CLOCK_REALTIME,NULL));
    struct timespec t={.tv_sec=(time_t)p_end->seconds,.tv_nsec=(long)p_end->nanoseconds};
    return posix_result(usem_wait(p_sem,PS_HOST_CLOCK_REALTIME,&t));
}
/* FreeBSD's _umtx_op for the ops libc's own primitives build on: plain waits and wakes on a word
 * (over futexes, private ones for the _PRIVATE ops as FreeBSD keys them apart) and the _usem
 * semaphore inside sem_t. A spurious return from a wait is allowed, so a signal or a word that
 * changed before the sleep returns 0. */
enum {
    UMTX_OP_WAIT=2, UMTX_OP_WAKE=3, UMTX_OP_WAIT_UINT=11, UMTX_OP_WAIT_UINT_PRIVATE=15, UMTX_OP_WAKE_PRIVATE=16,
    UMTX_OP_SEM_WAIT=19, UMTX_OP_SEM_WAKE=20, UMTX_OP_NWAKE_PRIVATE=21, UMTX_OP_COUNT=32,
};
typedef struct { _Atomic uint32_t has_waiters, count; uint32_t flags; } GuestUsem;
typedef struct { GuestTime timeout; uint32_t flags, clock; } GuestUmtxTime;
/* The timeout as an absolute CLOCK_MONOTONIC or CLOCK_REALTIME time: FreeBSD 9 passes a relative
 * timespec in uaddr2; later ABIs put its size in uaddr and, past a timespec, a _umtx_time with
 * UMTX_ABSTIME (1) and a clock (0 realtime). */
static int umtx_deadline(const void *p_size, const void *p_timeout, PsHostClock *p_out_clock, struct timespec *p_out_end) {
    GuestUmtxTime timeout={.clock=4};
    memcpy(&timeout,p_timeout,(uintptr_t)p_size>sizeof(GuestTime) ? sizeof(timeout) : sizeof(GuestTime));
    if (timeout.timeout.seconds<0 || timeout.timeout.nanoseconds<0 || timeout.timeout.nanoseconds>=1000000000) return 22;
    *p_out_clock=timeout.clock==0 ? PS_HOST_CLOCK_REALTIME : PS_HOST_CLOCK_MONOTONIC;
    *p_out_end=(struct timespec){.tv_sec=(time_t)timeout.timeout.seconds,.tv_nsec=(long)timeout.timeout.nanoseconds};
    if (timeout.flags&1) return 0;
    struct timespec now;
    ps_host_time_read(*p_out_clock,&now);
    p_out_end->tv_sec+=now.tv_sec;
    p_out_end->tv_nsec+=now.tv_nsec;
    if (p_out_end->tv_nsec>=1000000000) { p_out_end->tv_nsec-=1000000000; ++p_out_end->tv_sec; }
    return 0;
}
static int umtx_sleep(void *p_word, uint32_t expected, int is_private, const void *p_size, const void *p_timeout) {
    PsHostClock clock=PS_HOST_CLOCK_MONOTONIC;
    struct timespec end;
    if (p_timeout) {
        const int error=umtx_deadline(p_size,p_timeout,&clock,&end);
        if (error) return error;
    }
    const uint64_t wait_start=runtime_wait_clock();
    const int result=ps_host_wait_on_address(p_word,expected,is_private,clock,p_timeout ? &end : NULL);
    runtime_wait_note(2,runtime_wait_clock()-wait_start);
    return result==ETIMEDOUT ? 60 : 0;
}
static void umtx_wake(void *p_word, uint64_t c_waiters, int is_private) {
    ps_host_wake_address(p_word,c_waiters>INT32_MAX ? INT32_MAX : (int)c_waiters,is_private);
}
static ABI int32_t posix_umtx_op(void *p_object, int op, uint64_t value, void *p_address, void *p_address2) {
    static _Atomic unsigned char s_is_op_reported[UMTX_OP_COUNT];
    if (!p_object) return posix_result(14);
    switch (op) {
    case UMTX_OP_WAIT:
        if (*(volatile uint64_t *)p_object!=value) return 0;
        return posix_result(umtx_sleep(p_object,(uint32_t)value,0,p_address,p_address2));
    case UMTX_OP_WAIT_UINT:
    case UMTX_OP_WAIT_UINT_PRIVATE:
        return posix_result(umtx_sleep(p_object,(uint32_t)value,op==UMTX_OP_WAIT_UINT_PRIVATE,p_address,p_address2));
    case UMTX_OP_WAKE:
    case UMTX_OP_WAKE_PRIVATE:
        umtx_wake(p_object,value,op==UMTX_OP_WAKE_PRIVATE);
        return 0;
    case UMTX_OP_NWAKE_PRIVATE:
        for (uint64_t i=0;i<value;++i) umtx_wake(((void **)p_object)[i],INT32_MAX,1);
        return 0;
    case UMTX_OP_SEM_WAIT: {
        GuestUsem *p_sem=p_object;
        atomic_store(&p_sem->has_waiters,1);
        if (atomic_load(&p_sem->count)) return 0;
        return posix_result(umtx_sleep(&p_sem->count,0,!(p_sem->flags&SEM_SHARED),p_address,p_address2));
    }
    case UMTX_OP_SEM_WAKE: {
        GuestUsem *p_sem=p_object;
        umtx_wake(&p_sem->count,1,!(p_sem->flags&SEM_SHARED));
        return 0;
    }
    default:
        if (op>=0 && op<UMTX_OP_COUNT && !atomic_exchange(&s_is_op_reported[op],1))
            fprintf(stderr,"Runtime: _umtx_op op %d on %p not supported\n",op,p_object);
        return posix_result(22);
    }
}
static const RuntimeExport s_exports[]={
    {"sceKernelCreateSema",sem_create}, {"sceKernelWaitSema",sem_wait}, {"sceKernelSignalSema",sem_signal},
    {"sceKernelPollSema",sem_poll}, {"sceKernelCancelSema",sem_cancel}, {"sceKernelDeleteSema",sem_delete},
    {"sem_init",posix_sem_init}, {"sem_destroy",posix_sem_destroy}, {"sem_wait",posix_sem_wait},
    {"sem_trywait",posix_sem_trywait}, {"sem_timedwait",posix_sem_timedwait}, {"sem_post",posix_sem_post},
    {"sem_getvalue",posix_sem_getvalue}, {"sem_setname",posix_sem_setname}, {"_umtx_op",posix_umtx_op},
    {"scePthreadSemInit",psem_init}, {"scePthreadSemWait",psem_wait}, {"scePthreadSemTrywait",psem_trywait},
    {"scePthreadSemTimedwait",psem_timedwait}, {"scePthreadSemPost",psem_post},
    {"scePthreadSemDestroy",psem_destroy}, {"scePthreadSemGetvalue",psem_getvalue},
};
uintptr_t runtime_sema_resolve(const char *sz_name) {
    return RUNTIME_LOOKUP(s_exports,sz_name);
}
void runtime_sema_report(void) {
    if (!runtime_exception_report_lock(&s_lock,"semaphore")) return;
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           s_c_created,s_c_deleted,s_c_acquired,s_c_signaled,s_c_timed_out);
    runtime_exception_unlock(&s_lock);
}
