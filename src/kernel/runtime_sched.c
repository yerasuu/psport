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
typedef int32_t (ABI *GetPrio)(void *p_thread, int32_t *p_prio);
typedef int32_t (ABI *SetPrio)(void *p_thread, int32_t p_prio);
typedef void *(ABI *Self)(void);

#define POLICIES 64
static struct { void *p_thread; int32_t policy; } s_policies[POLICIES];
static pthread_rwlock_t s_policy_lock=PTHREAD_RWLOCK_INITIALIZER;
static int32_t policy_of(void *p_thread) {
    int32_t policy=SCHED_FIFO_GUEST;
    pthread_rwlock_rdlock(&s_policy_lock);
    for (int i=0;i<POLICIES;++i) if (s_policies[i].p_thread==p_thread) { policy=s_policies[i].policy; break; }
    pthread_rwlock_unlock(&s_policy_lock);
    return policy;
}
static void remember_policy(void *p_thread, int32_t policy) {
    pthread_rwlock_wrlock(&s_policy_lock);
    int slot=-1;
    for (int i=0;i<POLICIES;++i) {
        if (s_policies[i].p_thread==p_thread) { slot=i; break; }
        if (slot<0 && !s_policies[i].p_thread) slot=i;
    }
    if (slot>=0) { s_policies[slot].p_thread=p_thread; s_policies[slot].policy=policy; }
    else fputs("Runtime: thread policy table full; scePthreadGetschedparam reports FIFO\n",stderr);
    pthread_rwlock_unlock(&s_policy_lock);
}

static ABI int32_t get_sched_param(void *p_thread, int32_t *p_policy, GuestSchedParam *p_param) {
    if (!p_policy || !p_param) return ERR(22);
    int32_t priority;
    const int32_t r=((GetPrio)runtime_thread_resolve("1tKyG7RlMJo"))(p_thread,&priority);
    if (r) return r;
    *p_policy=policy_of(p_thread); p_param->priority=priority;
    return 0;
}
static ABI int32_t set_sched_param(void *p_thread, int32_t policy, const GuestSchedParam *p_param) {
    if (!p_param || policy<1 || policy>3) return ERR(22);
    const int32_t r=((SetPrio)runtime_thread_resolve("W0Hpm2X0uPE"))(p_thread,p_param->priority);
    if (r) return r;
    remember_policy(p_thread,policy);
    return 0;
}
/* Only the calling thread's name is reachable from here, through its host thread, which keeps
 * the first 15 characters. */
static ABI int32_t get_name(void *p_thread, char *sz_name) {
    if (!sz_name) return ERR(22);
    int32_t priority;
    const int32_t r=((GetPrio)runtime_thread_resolve("1tKyG7RlMJo"))(p_thread,&priority);
    if (r) return r;
    if (p_thread!=((Self)runtime_thread_resolve("aI+OeCz8xrQ"))()) {
        static int s_has_warned;
        if (!s_has_warned) { s_has_warned=1; fputs("Runtime: scePthreadGetname of another thread is not supported\n",stderr); }
        return ERR(78); /* ENOSYS */
    }
    char sz_host[16]={0};
    ps_host_thread_name(sz_host,sizeof(sz_host));
    memcpy(sz_name,sz_host,sizeof(sz_host));
    return 0;
}
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_get_sched_param(void *p_thread, int32_t *p_policy, GuestSchedParam *p_sched_param) { return posix(get_sched_param(p_thread,p_policy,p_sched_param)); }
static ABI int32_t posix_set_sched_param(void *p_thread, int32_t p, const GuestSchedParam *p_sched_param) { return posix(set_sched_param(p_thread,p,p_sched_param)); }

/* Every policy schedules over the same range, highest first: SCE_KERNEL_PRIO_FIFO_HIGHEST (256) is
 * the maximum, SCE_KERNEL_PRIO_FIFO_LOWEST (767) the minimum. */
static ABI int32_t priority_max(int32_t policy) { (void)policy; return 256; }
static ABI int32_t priority_min(int32_t policy) { (void)policy; return 767; }

static const RuntimeExport s_exports[]={
    {"sched_get_priority_max",priority_max}, {"sched_get_priority_min",priority_min},
    {"scePthreadGetschedparam",get_sched_param}, {"scePthreadSetschedparam",set_sched_param},
    {"pthread_getschedparam",posix_get_sched_param}, {"pthread_setschedparam",posix_set_sched_param},
    {"scePthreadGetname",get_name},
};
uintptr_t runtime_sched_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
#else
uintptr_t runtime_sched_resolve(const char *name) { (void)name; return 0; }
#endif
