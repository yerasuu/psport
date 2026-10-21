/* Kernel event flags: a 64-bit pattern that threads wait on for all (AND) or any (OR) of a set
 * of bits, optionally clearing them when the wait is satisfied. The handle is a 64-bit value the
 * guest only passes back. Waiters are woken together and each re-checks its condition, so the
 * order among several satisfied waiters is the host's, not FIFO or priority order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
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
    char sz_name[32];
    pthread_cond_t changed;
    struct Flag *p_next;
} Flag;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static Flag *s_p_registry;
static uint64_t s_next_id=1;
static size_t s_c_created, s_c_deleted, s_c_waits, s_c_timed_out;
/* Some titles create event flags dozens of times a second: past these, creations are counted. */
enum { LISTED_CREATIONS=8 };
static int trace_eventflag(void) { static int s_is_enabled=-1; if (s_is_enabled<0) s_is_enabled=runtime_env("PS_TRACE_EVENTFLAG",NULL)!=NULL; return s_is_enabled; }
static Flag *find(uint64_t id) {
    for (Flag *p_flag=s_p_registry;p_flag;p_flag=p_flag->p_next) if (p_flag->id==id) return p_flag;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: event flag host error %d\n",e); runtime_stop(21); }
}
static uint64_t now_ns(void) { return ps_host_time_monotonic_ns(); }
static ABI int32_t flag_create(uint64_t *p_out,const char *sz_name,uint32_t attr,uint64_t pattern,const void *p_options) {
    (void)p_options; /* only a size field on the console; nothing in it changes behaviour here */
    if (!p_out || !sz_name || (attr & ~(uint32_t)(ATTR_FIFO|ATTR_PRIO|ATTR_SINGLE|ATTR_MULTI)) ||
        (attr&(ATTR_FIFO|ATTR_PRIO))==(ATTR_FIFO|ATTR_PRIO) || (attr&(ATTR_SINGLE|ATTR_MULTI))==(ATTR_SINGLE|ATTR_MULTI))
        return ERR(22);
    if (strlen(sz_name)>31) return ERR(63);
    Flag *p_flag=calloc(1,sizeof(*p_flag));
    if (!p_flag) return ERR(12);
    host_check(pthread_cond_init(&p_flag->changed,NULL));
    runtime_exception_lock(&s_lock);
    p_flag->id=s_next_id++; p_flag->attr=attr; p_flag->pattern=pattern;
    snprintf(p_flag->sz_name,sizeof(p_flag->sz_name),"%s",sz_name);
    uint64_t id=p_flag->id;
    p_flag->p_next=s_p_registry; s_p_registry=p_flag; *p_out=id;
    const size_t c_created=++s_c_created, c_deleted=s_c_deleted;
    runtime_exception_unlock(&s_lock);
    if (c_created<=LISTED_CREATIONS || trace_eventflag())
        printf("Runtime: event flag id=%llu '%s', attr=%#x, pattern=%#llx\n",
               (unsigned long long)id,sz_name,attr,(unsigned long long)pattern);
    else if (!(c_created&(c_created-1)))
        printf("Runtime: event flags created=%zu so far, deleted=%zu; only the first %d are listed (PS_TRACE_EVENTFLAG=1: each)\n",
               c_created,c_deleted,LISTED_CREATIONS);
    return 0;
}
static int valid_mode(uint32_t mode) {
    uint32_t wait=mode&(WAIT_AND|WAIT_OR), clear=mode&(CLEAR_ALL|CLEAR_PAT);
    return !(mode & ~(uint32_t)(WAIT_AND|WAIT_OR|CLEAR_ALL|CLEAR_PAT)) &&
           (wait==WAIT_AND || wait==WAIT_OR) && clear!=(CLEAR_ALL|CLEAR_PAT);
}
/* Called with the lock held; clears what the mode asks once satisfied. */
static int take(Flag *p_flag,uint64_t bits,uint32_t mode,uint64_t *p_result) {
    int met=(mode&WAIT_AND) ? (p_flag->pattern&bits)==bits : (p_flag->pattern&bits)!=0;
    if (!met) return 0;
    if (p_result) *p_result=p_flag->pattern;
    if (mode&CLEAR_ALL) p_flag->pattern=0;
    else if (mode&CLEAR_PAT) p_flag->pattern&=~bits;
    return 1;
}
static void release(Flag *p_flag) {
    if (!--p_flag->waiters && p_flag->deleted) {
        host_check(pthread_cond_destroy(&p_flag->changed));
        free(p_flag);
    }
}
static int32_t wait_flag(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *p_result,uint32_t *p_timeout,int block) {
    runtime_exception_lock(&s_lock);
    Flag *p_flag=find(id);
    int32_t status=0;
    if (!p_flag) status=ERR(3);
    else if (!bits || !valid_mode(mode)) status=ERR(22);
    else if (!(p_flag->attr&ATTR_MULTI) && p_flag->waiters) status=ERR(1);
    else if (take(p_flag,bits,mode,p_result)) ++s_c_waits;
    else if (!block) { status=ERR(16); if (p_result) *p_result=p_flag->pattern; }
    else if (p_timeout && !*p_timeout) { status=ERR(60); if (p_result) *p_result=p_flag->pattern; ++s_c_timed_out; }
    else {
        ++p_flag->waiters;
        const uint64_t cancels=p_flag->cancels, wait_start=runtime_wait_clock();
        const uint64_t deadline=p_timeout ? now_ns()+(uint64_t)*p_timeout*1000 : 0;
        const struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        RUNTIME_ZONE(zone,"event flag wait");
        for (;;) {
            if (p_flag->deleted) { status=ERR(13); break; }
            if (p_flag->cancels!=cancels) { status=ERR(85); if (p_result) *p_result=p_flag->pattern; break; }
            if (take(p_flag,bits,mode,p_result)) { ++s_c_waits; break; }
            int e=runtime_exception_wait(&p_flag->changed,&s_lock,p_timeout ? &end : NULL);
            if (e==ETIMEDOUT) {
                if (take(p_flag,bits,mode,p_result)) { ++s_c_waits; break; }
                status=ERR(60); if (p_result) *p_result=p_flag->pattern; ++s_c_timed_out;
                break;
            }
            host_check(e);
        }
        RUNTIME_ZONE_END(zone);
        runtime_wait_note(0,runtime_wait_clock()-wait_start);
        if (p_timeout) {
            uint64_t now=now_ns();
            *p_timeout=status ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        release(p_flag);
    }
    runtime_exception_unlock(&s_lock);
    return status;
}
static ABI int32_t flag_wait(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *p_result,uint32_t *p_timeout) {
    return wait_flag(id,bits,mode,p_result,p_timeout,1);
}
static ABI int32_t flag_poll(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *p_result) {
    return wait_flag(id,bits,mode,p_result,NULL,0);
}
static ABI int32_t flag_set(uint64_t id,uint64_t bits) {
    runtime_exception_lock(&s_lock);
    Flag *p_flag=find(id);
    if (p_flag) { p_flag->pattern|=bits; host_check(pthread_cond_broadcast(&p_flag->changed)); }
    runtime_exception_unlock(&s_lock);
    return p_flag ? 0 : ERR(3);
}
/* The console keeps the bits that are set in the argument: pattern &= bits. */
static ABI int32_t flag_clear(uint64_t id,uint64_t bits) {
    runtime_exception_lock(&s_lock);
    Flag *p_flag=find(id);
    if (p_flag) p_flag->pattern&=bits;
    runtime_exception_unlock(&s_lock);
    return p_flag ? 0 : ERR(3);
}
static ABI int32_t flag_cancel(uint64_t id,uint64_t pattern,int32_t *p_waiting) {
    runtime_exception_lock(&s_lock);
    Flag *p_flag=find(id);
    if (p_flag) {
        if (p_waiting) *p_waiting=(int32_t)p_flag->waiters;
        p_flag->pattern=pattern; ++p_flag->cancels;
        host_check(pthread_cond_broadcast(&p_flag->changed));
    }
    runtime_exception_unlock(&s_lock);
    return p_flag ? 0 : ERR(3);
}
static ABI int32_t flag_delete(uint64_t id) {
    runtime_exception_lock(&s_lock);
    Flag **pp_flag=&s_p_registry;
    while (*pp_flag && (*pp_flag)->id!=id) pp_flag=&(*pp_flag)->p_next;
    if (!*pp_flag) { runtime_exception_unlock(&s_lock); return ERR(3); }
    Flag *p_flag=*pp_flag; *pp_flag=p_flag->p_next; p_flag->deleted=1; ++s_c_deleted;
    if (p_flag->waiters) host_check(pthread_cond_broadcast(&p_flag->changed));
    else { host_check(pthread_cond_destroy(&p_flag->changed)); free(p_flag); }
    runtime_exception_unlock(&s_lock);
    return 0;
}
/* One process: the flags it can open by name are the ones it created. What the kernel answers for a
 * name nobody created is not confirmed (kb #691); ENOENT, counted. */
static uint64_t s_c_open_misses;
static ABI int32_t flag_open(uint64_t *p_out,const char *sz_name) {
    if (!p_out || !sz_name) return ERR(14);
    if (strnlen(sz_name,32)>31) return ERR(63);
    runtime_exception_lock(&s_lock);
    const Flag *p_flag=s_p_registry;
    while (p_flag && strcmp(p_flag->sz_name,sz_name)) p_flag=p_flag->p_next;
    if (p_flag) *p_out=p_flag->id;
    runtime_exception_unlock(&s_lock);
    if (p_flag) return 0;
    if (!__atomic_fetch_add(&s_c_open_misses,1,__ATOMIC_RELAXED)) {
        printf("Runtime: sceKernelOpenEventFlag('%s'): no event flag of that name, ENOENT\n",sz_name);
        runtime_calls_hle_stub("sceKernelOpenEventFlag of a name nobody created (ENOENT)",&s_c_open_misses);
    }
    return ERR(2);
}
/* Closing what was opened leaves the flag to its creator, who deletes it. */
static ABI int32_t flag_close(uint64_t id) {
    runtime_exception_lock(&s_lock);
    const Flag *p_flag=find(id);
    runtime_exception_unlock(&s_lock);
    return p_flag ? 0 : ERR(3);
}
static const RuntimeExport s_exports[]={
    {"sceKernelCreateEventFlag",flag_create}, {"sceKernelDeleteEventFlag",flag_delete},
    {"sceKernelWaitEventFlag",flag_wait}, {"sceKernelPollEventFlag",flag_poll},
    {"sceKernelSetEventFlag",flag_set}, {"sceKernelClearEventFlag",flag_clear},
    {"sceKernelCancelEventFlag",flag_cancel},
    {"sceKernelOpenEventFlag",flag_open}, {"sceKernelCloseEventFlag",flag_close},
};
uintptr_t runtime_eventflag_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_eventflag_report(void) {
    if (!runtime_exception_report_lock(&s_lock,"event flag")) return;
    printf("Runtime: event flag waits=%zu, timeouts=%zu\n",s_c_waits,s_c_timed_out);
    runtime_exception_unlock(&s_lock);
}
void runtime_eventflag_created_report(void) {
    char sz_line[96];
    const int used=snprintf(sz_line,sizeof(sz_line),"Runtime: event flags created=%zu, deleted=%zu\n",
                            __atomic_load_n(&s_c_created,__ATOMIC_RELAXED),__atomic_load_n(&s_c_deleted,__ATOMIC_RELAXED));
    if (used>0 && ps_host_fs_write(1,sz_line,(size_t)used)<0) return;
}
