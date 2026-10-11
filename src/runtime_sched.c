/* Scheduling parameters and names of guest threads, over the entry points of runtime_thread.c:
 * their checks decide whether a handle is a live thread. Nothing enforces PS4 scheduling, so a
 * policy is only kept to be reported back. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define SCHED_FIFO_GUEST 1 /* FreeBSD numbering: 1 FIFO, 2 OTHER, 3 RR; scePthreadAttrInit's default */

typedef struct { int32_t priority; } GuestSchedParam;
typedef int32_t (ABI *GetPrio)(void *thread, int32_t *prio);
typedef int32_t (ABI *SetPrio)(void *thread, int32_t prio);
typedef void *(ABI *Self)(void);

/* Policies set through scePthreadSetschedparam; a thread not listed has the default. Entries of
 * exited threads stay until their slot is reused for the same handle. */
#define POLICIES 64
static struct { void *thread; int32_t policy; } policies[POLICIES];
static pthread_rwlock_t policy_lock=PTHREAD_RWLOCK_INITIALIZER;
static int32_t policy_of(void *thread) {
    int32_t policy=SCHED_FIFO_GUEST;
    pthread_rwlock_rdlock(&policy_lock);
    for (int i=0;i<POLICIES;++i) if (policies[i].thread==thread) { policy=policies[i].policy; break; }
    pthread_rwlock_unlock(&policy_lock);
    return policy;
}
static void remember_policy(void *thread, int32_t policy) {
    pthread_rwlock_wrlock(&policy_lock);
    int slot=-1;
    for (int i=0;i<POLICIES;++i) {
        if (policies[i].thread==thread) { slot=i; break; }
        if (slot<0 && !policies[i].thread) slot=i;
    }
    if (slot>=0) { policies[slot].thread=thread; policies[slot].policy=policy; }
    else fputs("Runtime: thread policy table full; scePthreadGetschedparam reports FIFO\n",stderr);
    pthread_rwlock_unlock(&policy_lock);
}

static ABI int32_t get_sched_param(void *thread, int32_t *policy, GuestSchedParam *param) {
    if (!policy || !param) return ERR(22);
    int32_t priority;
    const int32_t r=((GetPrio)runtime_thread_resolve("1tKyG7RlMJo#p#J"))(thread,&priority);
    if (r) return r;
    *policy=policy_of(thread); param->priority=priority;
    return 0;
}
static ABI int32_t set_sched_param(void *thread, int32_t policy, const GuestSchedParam *param) {
    if (!param || policy<1 || policy>3) return ERR(22);
    const int32_t r=((SetPrio)runtime_thread_resolve("W0Hpm2X0uPE#p#J"))(thread,param->priority);
    if (r) return r;
    remember_policy(thread,policy);
    return 0;
}
/* Only the calling thread's name is reachable from here, through its host thread, which keeps
 * the first 15 characters. */
static ABI int32_t get_name(void *thread, char *name) {
    if (!name) return ERR(22);
    int32_t priority;
    const int32_t r=((GetPrio)runtime_thread_resolve("1tKyG7RlMJo#p#J"))(thread,&priority);
    if (r) return r;
    if (thread!=((Self)runtime_thread_resolve("aI+OeCz8xrQ#p#J"))()) {
        static int warned;
        if (!warned) { warned=1; fputs("Runtime: scePthreadGetname of another thread is not supported\n",stderr); }
        return ERR(78); /* ENOSYS */
    }
    char host[16]={0};
    pthread_getname_np(pthread_self(),host,sizeof(host));
    memcpy(name,host,sizeof(host));
    return 0;
}
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_get_sched_param(void *t, int32_t *p, GuestSchedParam *s) { return posix(get_sched_param(t,p,s)); }
static ABI int32_t posix_set_sched_param(void *t, int32_t p, const GuestSchedParam *s) { return posix(set_sched_param(t,p,s)); }

static const RuntimeExport exports[]={
    {"scePthreadGetschedparam",get_sched_param}, {"scePthreadSetschedparam",set_sched_param},
    {"pthread_getschedparam",posix_get_sched_param}, {"pthread_setschedparam",posix_set_sched_param},
    {"scePthreadGetname",get_name},
};
uintptr_t runtime_sched_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
uintptr_t runtime_sched_resolve(const char *name) { (void)name; return 0; }
#endif
