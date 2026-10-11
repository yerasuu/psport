/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
static Sema *find(uint32_t id) {
    for (Sema *s=registry;s;s=s->next) if (s->id==id) return s;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    runtime_exception_lock(&lock);
    if (next_id==UINT32_MAX) { runtime_exception_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    uint32_t id=s->id;
    s->next=registry; registry=s; *out=id; ++created;
    runtime_exception_unlock(&lock);
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",id,attr,initial,maximum);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    runtime_exception_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (getenv("BB_TRACE_SEMA")) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        const uint64_t wait_start=runtime_wait_clock();
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.done) {
            int e=runtime_exception_wait(&w.event,&lock,timeout ? &end : NULL);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        runtime_wait_note(2,runtime_wait_clock()-wait_start);
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--s->active && s->deleted) free(s);
    }
    runtime_exception_unlock(&lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) { return wait_count(id,need,timeout,1); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    runtime_exception_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_check(pthread_cond_signal(&w->event));
        }
    }
    runtime_exception_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    runtime_exception_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    runtime_exception_unlock(&lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    runtime_exception_lock(&lock);
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { runtime_exception_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    runtime_exception_unlock(&lock);
    return 0;
}
/* scePthreadSem*: POSIX-style semaphores whose handle the game keeps in its own 8 bytes; backed by a
 * kernel semaphore whose id lives in that handle. */
static ABI int32_t psem_init(uint64_t *sem,int flag,uint32_t value,const char *name) {
    (void)flag;
    if (!sem || value>INT32_MAX) return ERR(22);
    uint32_t id;
    int32_t result=sem_create(&id,name ? name : "",1,(int32_t)value,INT32_MAX,NULL);
    if (!result) *sem=id;
    return result;
}
static ABI int32_t psem_wait(uint64_t *sem) { return sem ? wait_count((uint32_t)*sem,1,NULL,1) : ERR(22); }
static ABI int32_t psem_trywait(uint64_t *sem) {
    if (!sem) return ERR(22);
    int32_t result=wait_count((uint32_t)*sem,1,NULL,0);
    return result==ERR(16) ? ERR(35) : result; /* POSIX trywait reports EAGAIN, the kernel call EBUSY */
}
static ABI int32_t psem_timedwait(uint64_t *sem,uint32_t usec) {
    return sem ? wait_count((uint32_t)*sem,1,&usec,1) : ERR(22);
}
static ABI int32_t psem_post(uint64_t *sem) { return sem ? sem_signal((uint32_t)*sem,1) : ERR(22); }
static ABI int32_t psem_destroy(uint64_t *sem) { return sem ? sem_delete((uint32_t)*sem) : ERR(22); }
static ABI int32_t psem_getvalue(uint64_t *sem,int32_t *value) {
    if (!sem || !value) return ERR(22);
    runtime_exception_lock(&lock);
    Sema *s=find((uint32_t)*sem);
    if (s) *value=s->count;
    runtime_exception_unlock(&lock);
    return s ? 0 : ERR(22);
}
static const RuntimeExport exports[]={
    {"scePthreadSemInit",psem_init}, {"scePthreadSemWait",psem_wait}, {"scePthreadSemTrywait",psem_trywait},
    {"scePthreadSemTimedwait",psem_timedwait}, {"scePthreadSemPost",psem_post},
    {"scePthreadSemDestroy",psem_destroy}, {"scePthreadSemGetvalue",psem_getvalue},
};
uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#p#J")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#p#J")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#p#J")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#p#J")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#p#J")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#p#J")) return (uintptr_t)sem_delete;
    return RUNTIME_LOOKUP(exports,name);
}
void runtime_sema_report(void) {
    runtime_exception_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    runtime_exception_unlock(&lock);
}
#else
uintptr_t runtime_sema_resolve(const char *name) { (void)name; return 0; }
void runtime_sema_report(void) { puts("Runtime: Windows semaphore backend not implemented"); }
#endif
