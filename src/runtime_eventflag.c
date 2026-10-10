/* Kernel event flags: a 64-bit pattern that threads wait on for all (AND) or any (OR) of a set
 * of bits, optionally clearing them when the wait is satisfied. The handle is a 64-bit value the
 * guest only passes back. Waiters are woken together and each re-checks its condition, so the
 * order among several satisfied waiters is the host's, not FIFO or priority order. */
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
enum { ATTR_FIFO=0x01, ATTR_PRIO=0x02, ATTR_SINGLE=0x10, ATTR_MULTI=0x20 };
enum { WAIT_AND=0x01, WAIT_OR=0x02, CLEAR_ALL=0x10, CLEAR_PAT=0x20 };
typedef struct Flag {
    uint64_t id, pattern;
    uint32_t attr;
    unsigned waiters;
    /* Bumped by a cancel; a waiter that saw it change returns ECANCELED. */
    uint64_t cancels;
    int deleted;
    pthread_cond_t changed;
    struct Flag *next;
} Flag;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Flag *registry;
static uint64_t next_id=1;
static size_t created, deleted, waits, timed_out;
static Flag *find(uint64_t id) {
    for (Flag *f=registry;f;f=f->next) if (f->id==id) return f;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: event flag host error %d\n",e); exit(21); }
}
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static ABI int32_t flag_create(uint64_t *out,const char *name,uint32_t attr,uint64_t pattern,const void *options) {
    (void)options; /* only a size field on the console; nothing in it changes behaviour here */
    if (!out || !name || (attr & ~(uint32_t)(ATTR_FIFO|ATTR_PRIO|ATTR_SINGLE|ATTR_MULTI)) ||
        (attr&(ATTR_FIFO|ATTR_PRIO))==(ATTR_FIFO|ATTR_PRIO) || (attr&(ATTR_SINGLE|ATTR_MULTI))==(ATTR_SINGLE|ATTR_MULTI))
        return ERR(22);
    if (strlen(name)>31) return ERR(63);
    Flag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    pthread_condattr_t cattr;
    host_check(pthread_condattr_init(&cattr));
    host_check(pthread_condattr_setclock(&cattr,CLOCK_MONOTONIC));
    host_check(pthread_cond_init(&f->changed,&cattr));
    host_check(pthread_condattr_destroy(&cattr));
    pthread_mutex_lock(&lock);
    f->id=next_id++; f->attr=attr; f->pattern=pattern;
    f->next=registry; registry=f; *out=f->id; ++created;
    printf("Runtime: event flag id=%llu '%s', attr=%#x, pattern=%#llx\n",
           (unsigned long long)f->id,name,attr,(unsigned long long)pattern);
    pthread_mutex_unlock(&lock);
    return 0;
}
static int valid_mode(uint32_t mode) {
    uint32_t wait=mode&(WAIT_AND|WAIT_OR), clear=mode&(CLEAR_ALL|CLEAR_PAT);
    return !(mode & ~(uint32_t)(WAIT_AND|WAIT_OR|CLEAR_ALL|CLEAR_PAT)) &&
           (wait==WAIT_AND || wait==WAIT_OR) && clear!=(CLEAR_ALL|CLEAR_PAT);
}
/* Called with the lock held; clears what the mode asks once satisfied. */
static int take(Flag *f,uint64_t bits,uint32_t mode,uint64_t *result) {
    int met=(mode&WAIT_AND) ? (f->pattern&bits)==bits : (f->pattern&bits)!=0;
    if (!met) return 0;
    if (result) *result=f->pattern;
    if (mode&CLEAR_ALL) f->pattern=0;
    else if (mode&CLEAR_PAT) f->pattern&=~bits;
    return 1;
}
static void release(Flag *f) {
    if (!--f->waiters && f->deleted) {
        host_check(pthread_cond_destroy(&f->changed));
        free(f);
    }
}
static int32_t wait_flag(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout,int block) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    int32_t status=0;
    if (!f) status=ERR(3);
    else if (!bits || !valid_mode(mode)) status=ERR(22);
    else if (!(f->attr&ATTR_MULTI) && f->waiters) status=ERR(1);
    else if (take(f,bits,mode,result)) ++waits;
    else if (!block) { status=ERR(16); if (result) *result=f->pattern; }
    else if (timeout && !*timeout) { status=ERR(60); if (result) *result=f->pattern; ++timed_out; }
    else {
        ++f->waiters;
        const uint64_t cancels=f->cancels, wait_start=runtime_wait_clock();
        const uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        const struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        for (;;) {
            if (f->deleted) { status=ERR(13); break; }
            if (f->cancels!=cancels) { status=ERR(85); if (result) *result=f->pattern; break; }
            if (take(f,bits,mode,result)) { ++waits; break; }
            int e=timeout ? pthread_cond_timedwait(&f->changed,&lock,&end) : pthread_cond_wait(&f->changed,&lock);
            if (e==ETIMEDOUT) {
                if (take(f,bits,mode,result)) { ++waits; break; }
                status=ERR(60); if (result) *result=f->pattern; ++timed_out;
                break;
            }
            host_check(e);
        }
        runtime_wait_note(0,runtime_wait_clock()-wait_start);
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=status ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        release(f);
    }
    pthread_mutex_unlock(&lock);
    return status;
}
static ABI int32_t flag_wait(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    return wait_flag(id,bits,mode,result,timeout,1);
}
static ABI int32_t flag_poll(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result) {
    return wait_flag(id,bits,mode,result,NULL,0);
}
static ABI int32_t flag_set(uint64_t id,uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (f) { f->pattern|=bits; host_check(pthread_cond_broadcast(&f->changed)); }
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
/* The console keeps the bits that are set in the argument: pattern &= bits. */
static ABI int32_t flag_clear(uint64_t id,uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (f) f->pattern&=bits;
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static ABI int32_t flag_cancel(uint64_t id,uint64_t pattern,int32_t *waiting) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (f) {
        if (waiting) *waiting=(int32_t)f->waiters;
        f->pattern=pattern; ++f->cancels;
        host_check(pthread_cond_broadcast(&f->changed));
    }
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static ABI int32_t flag_delete(uint64_t id) {
    pthread_mutex_lock(&lock);
    Flag **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Flag *f=*p; *p=f->next; f->deleted=1; ++deleted;
    if (f->waiters) host_check(pthread_cond_broadcast(&f->changed));
    else { host_check(pthread_cond_destroy(&f->changed)); free(f); }
    pthread_mutex_unlock(&lock);
    return 0;
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",flag_create}, {"sceKernelDeleteEventFlag",flag_delete},
    {"sceKernelWaitEventFlag",flag_wait}, {"sceKernelPollEventFlag",flag_poll},
    {"sceKernelSetEventFlag",flag_set}, {"sceKernelClearEventFlag",flag_clear},
    {"sceKernelCancelEventFlag",flag_cancel},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_eventflag_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, deleted=%zu, waits=%zu, timeouts=%zu\n",
           created,deleted,waits,timed_out);
    pthread_mutex_unlock(&lock);
}
#else
uintptr_t runtime_eventflag_resolve(const char *name) { (void)name; return 0; }
void runtime_eventflag_report(void) { puts("Runtime: Windows event flag backend not implemented"); }
#endif
