/* libkernel asynchronous file I/O: each submission (one id) keeps the commands copied in when it
 * was submitted and runs on the runtime's I/O workers, higher priority first, writing every
 * command's result as it completes. An id stays valid until the game deletes it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define EFAULT_CODE ERR(14)
#define EINVAL_CODE ERR(22)
#define ESRCH_CODE ERR(3)
#define MAX_SUBMISSIONS 4096
#define WORKERS 4
#define LOGGED_FAILURES 16
enum { SUBMITTED=1, PROCESSING=2, COMPLETED=3, ABORTED=4 };
enum { PRIORITY_LOW=1, PRIORITY_HIGH=3 };
enum { WAIT_AND=1, WAIT_OR=2 };
typedef struct { int64_t value; uint32_t state; } AioResult;
typedef struct { int64_t offset, nbyte; void *buf; AioResult *result; int32_t fd; } AioRequest;
_Static_assert(sizeof(AioRequest)==40,"SceKernelAioRWRequest layout");
typedef struct { uint32_t window, delayed, split, split_size, chunk_size; } SchedulingParam;
typedef struct { SchedulingParam low, mid, high; } AioParam;
_Static_assert(sizeof(AioParam)==60,"SceKernelAioParam layout");
typedef struct Submission {
    int32_t id, count;
    uint32_t state;
    int write, priority;
    struct Submission *next;
    AioRequest commands[];
} Submission;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work=PTHREAD_COND_INITIALIZER, finished=PTHREAD_COND_INITIALIZER;
static Submission *slots[MAX_SUBMISSIONS], *queues[PRIORITY_HIGH];
static int32_t next_id=1;
static size_t live, workers;
static size_t submissions, commands, failures, cancels, deletes, timeouts;
static uint64_t bytes_read, bytes_written;

static int trace(void) { static int v=-1; if (v<0) v=getenv("PS_TRACE_AIO")!=NULL; return v; }
static int settled(uint32_t state) { return state==COMPLETED || state==ABORTED; }
/* Lock held. */
static Submission *find(int32_t id) {
    Submission *s=id>0 ? slots[id%MAX_SUBMISSIONS] : NULL;
    return s && s->id==id ? s : NULL;
}
static Submission *dequeue(void) {
    for (int p=PRIORITY_HIGH-1; p>=0; --p) {
        Submission *s=queues[p];
        if (s) { queues[p]=s->next; return s; }
    }
    return NULL;
}
static void enqueue(Submission *s) {
    Submission **tail=&queues[s->priority-1];
    while (*tail) tail=&(*tail)->next;
    s->next=NULL;
    *tail=s;
}
static void unqueue(Submission *s) {
    Submission **at=&queues[s->priority-1];
    while (*at && *at!=s) at=&(*at)->next;
    if (*at) *at=s->next;
}

static int run_command(const AioRequest *c, int write) {
    const int64_t n=write ? runtime_file_pwrite(c->fd,c->buf,(uint64_t)c->nbyte,c->offset)
                          : runtime_file_pread(c->fd,c->buf,(uint64_t)c->nbyte,c->offset);
    c->result->value=n<0 ? ERR(runtime_guest_errno((int)-n)) : n;
    __atomic_store_n(&c->result->state,n<0 ? ABORTED : COMPLETED,__ATOMIC_RELEASE);
    if (n<0) {
        if (__atomic_fetch_add(&failures,1,__ATOMIC_RELAXED)<LOGGED_FAILURES)
            printf("Runtime: aio %s of %" PRId64 " bytes at %" PRId64 " on fd %d failed (error %#x)\n",
                   write ? "write" : "read",c->nbyte,c->offset,c->fd,(unsigned)c->result->value);
        return 1;
    }
    __atomic_add_fetch(write ? &bytes_written : &bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return 0;
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    for (;;) {
        Submission *s=dequeue();
        if (!s) { pthread_cond_wait(&work,&lock); continue; }
        s->state=PROCESSING;
        pthread_mutex_unlock(&lock);
        int failed=0;
        for (int32_t i=0; i<s->count; ++i) failed|=run_command(&s->commands[i],s->write);
        pthread_mutex_lock(&lock);
        s->state=failed ? ABORTED : COMPLETED;
        pthread_cond_broadcast(&finished);
    }
    return NULL;
}
/* Lock held. The workers take only fault signals: the guest's own signals go to its threads. */
static void start_workers(void) {
    sigset_t all, previous;
    sigfillset(&all);
    sigdelset(&all,SIGSEGV); sigdelset(&all,SIGBUS); sigdelset(&all,SIGILL); sigdelset(&all,SIGFPE);
    pthread_sigmask(SIG_SETMASK,&all,&previous);
    for (; workers<WORKERS; ++workers) {
        pthread_t thread;
        int e=pthread_create(&thread,NULL,worker,NULL);
        if (e) { fprintf(stderr,"STOP: aio worker pthread_create failed: %d\n",e); runtime_stop(21); }
        pthread_detach(thread);
    }
    pthread_sigmask(SIG_SETMASK,&previous,NULL);
}

/* Lock held; live leaves room for it. */
static Submission *create(const AioRequest *requests, int32_t count, int priority, int write) {
    Submission *s=malloc(sizeof(*s)+(size_t)count*sizeof(*requests));
    if (!s) return NULL;
    while (slots[next_id%MAX_SUBMISSIONS]) next_id=next_id==INT32_MAX ? 1 : next_id+1;
    *s=(Submission){.id=next_id,.count=count,.state=SUBMITTED,.write=write,.priority=priority};
    for (int32_t i=0; i<count; ++i) {
        s->commands[i]=requests[i];
        __atomic_store_n(&requests[i].result->state,SUBMITTED,__ATOMIC_RELEASE);
    }
    next_id=next_id==INT32_MAX ? 1 : next_id+1;
    slots[s->id%MAX_SUBMISSIONS]=s;
    ++live;
    enqueue(s);
    return s;
}
static int32_t submit(const AioRequest *requests, int32_t count, int priority, int32_t *ids, int multiple, int write) {
    if (!requests || !ids) return EFAULT_CODE;
    if (count<=0 || priority<PRIORITY_LOW || priority>PRIORITY_HIGH) return EINVAL_CODE;
    for (int32_t i=0; i<count; ++i) if (!requests[i].result) return EFAULT_CODE;
    const size_t needed=multiple ? (size_t)count : 1;
    runtime_exception_lock(&lock);
    const size_t in_use=live;
    int32_t result=in_use+needed>MAX_SUBMISSIONS ? ERR(35) : 0;
    for (size_t i=0; !result && i<needed; ++i) {
        Submission *s=multiple ? create(&requests[i],1,priority,write) : create(requests,count,priority,write);
        if (!s) result=ERR(12);
        else ids[i]=s->id;
    }
    if (!result) {
        if (!workers) start_workers();
        submissions+=needed; commands+=(size_t)count;
        pthread_cond_broadcast(&work);
    }
    runtime_exception_unlock(&lock);
    if (trace() && !result)
        printf("Runtime: aio %s id=%d, commands=%d, priority=%d, first fd=%d, %" PRId64 " bytes at %" PRId64 "\n",
               write ? "write" : "read",ids[0],count,priority,requests[0].fd,requests[0].nbyte,requests[0].offset);
    if (result) printf("Runtime: aio submit of %d commands failed (error %#x), %zu ids in use\n",count,(unsigned)result,in_use);
    return result;
}
static ABI int32_t submit_reads(const AioRequest *r, int32_t n, int32_t prio, int32_t *id) { return submit(r,n,prio,id,0,0); }
static ABI int32_t submit_reads_multiple(const AioRequest *r, int32_t n, int32_t prio, int32_t *ids) { return submit(r,n,prio,ids,1,0); }
static ABI int32_t submit_writes(const AioRequest *r, int32_t n, int32_t prio, int32_t *id) { return submit(r,n,prio,id,0,1); }
static ABI int32_t submit_writes_multiple(const AioRequest *r, int32_t n, int32_t prio, int32_t *ids) { return submit(r,n,prio,ids,1,1); }

/* Lock held: ESRCH when an id is not live, otherwise whether the AND (all) or OR (any) of the
 * ids have settled, their states written. */
static int32_t check(const int32_t *ids, int32_t count, int32_t *states, int all, int *done) {
    *done=all;
    for (int32_t i=0; i<count; ++i) {
        const Submission *s=find(ids[i]);
        if (!s) return ESRCH_CODE;
        states[i]=(int32_t)s->state;
        if (all) *done&=settled(s->state);
        else *done|=settled(s->state);
    }
    return 0;
}
static int32_t wait_ids(const int32_t *ids, int32_t count, int32_t *states, int all, uint32_t *usec) {
    if (!ids || !states) return EFAULT_CODE;
    if (count<=0) return EINVAL_CODE;
    const uint64_t deadline=usec ? runtime_wait_clock()+(uint64_t)*usec*1000 : 0;
    const struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
    const uint64_t wait_start=runtime_wait_clock();
    runtime_exception_lock(&lock);
    int done=0;
    int32_t result=check(ids,count,states,all,&done);
    while (!result && !done) {
        if (usec && runtime_wait_clock()>=deadline) { result=ERR(60); ++timeouts; break; }
        int e=runtime_exception_wait(&finished,&lock,usec ? &end : NULL);
        if (e && e!=ETIMEDOUT) { fprintf(stderr,"STOP: aio wait host error %d\n",e); runtime_stop(21); }
        result=check(ids,count,states,all,&done);
    }
    runtime_exception_unlock(&lock);
    runtime_wait_note(0,runtime_wait_clock()-wait_start);
    if (trace()) printf("Runtime: aio wait id=%d (of %d), timeout=%lld us, result=%#x, state=%d\n",ids[0],count,
                        usec ? (long long)*usec : -1LL,(unsigned)result,states[0]);
    if (usec) {
        const uint64_t now=runtime_wait_clock();
        *usec=result || now>=deadline ? 0 : (uint32_t)((deadline-now)/1000);
    }
    return result;
}
static ABI int32_t wait_request(int32_t id, int32_t *state, uint32_t *usec) { return wait_ids(&id,1,state,1,usec); }
static ABI int32_t wait_requests(const int32_t *ids, int32_t count, int32_t *states, uint32_t mode, uint32_t *usec) {
    if (mode!=WAIT_AND && mode!=WAIT_OR) return EINVAL_CODE;
    return wait_ids(ids,count,states,mode==WAIT_AND,usec);
}
static ABI int32_t poll_requests(const int32_t *ids, int32_t count, int32_t *states) {
    if (!ids || !states) return EFAULT_CODE;
    if (count<=0) return EINVAL_CODE;
    int done;
    runtime_exception_lock(&lock);
    const int32_t result=check(ids,count,states,1,&done);
    runtime_exception_unlock(&lock);
    return result;
}
static ABI int32_t poll_request(int32_t id, int32_t *state) { return poll_requests(&id,1,state); }

/* Lock held: a submission no worker has taken is aborted, its commands with ECANCELED. */
static int abort_queued(Submission *s) {
    if (s->state!=SUBMITTED) return 0;
    unqueue(s);
    s->state=ABORTED;
    for (int32_t i=0; i<s->count; ++i) {
        s->commands[i].result->value=ERR(85);
        __atomic_store_n(&s->commands[i].result->state,ABORTED,__ATOMIC_RELEASE);
    }
    ++cancels;
    pthread_cond_broadcast(&finished);
    return 1;
}
static ABI int32_t cancel_requests(const int32_t *ids, int32_t count, int32_t *states) {
    if (!ids || !states) return EFAULT_CODE;
    if (count<=0) return EINVAL_CODE;
    int32_t result=0;
    runtime_exception_lock(&lock);
    for (int32_t i=0; i<count; ++i) {
        Submission *s=find(ids[i]);
        if (!s) { result=ESRCH_CODE; continue; }
        abort_queued(s);
        states[i]=(int32_t)s->state;
    }
    runtime_exception_unlock(&lock);
    return result;
}
static ABI int32_t cancel_request(int32_t id, int32_t *state) { return cancel_requests(&id,1,state); }
/* One a worker is running is let finish first: its results land in the game's memory. */
static ABI int32_t delete_requests(const int32_t *ids, int32_t count, int32_t *results) {
    if (!ids || !results) return EFAULT_CODE;
    if (count<=0) return EINVAL_CODE;
    runtime_exception_lock(&lock);
    for (int32_t i=0; i<count; ++i) {
        Submission *s=find(ids[i]);
        while (s && s->state==PROCESSING) {
            int e=runtime_exception_wait(&finished,&lock,NULL);
            if (e && e!=ETIMEDOUT) { fprintf(stderr,"STOP: aio delete host error %d\n",e); runtime_stop(21); }
            s=find(ids[i]);
        }
        if (!s) { results[i]=ESRCH_CODE; continue; }
        abort_queued(s);
        slots[s->id%MAX_SUBMISSIONS]=NULL;
        --live; ++deletes;
        free(s);
        results[i]=0;
    }
    runtime_exception_unlock(&lock);
    return 0;
}
static ABI int32_t delete_request(int32_t id, int32_t *result) { return delete_requests(&id,1,result); }

static int32_t check_scheduling(const SchedulingParam *p) {
    if (p->window>0x80 || p->delayed>0x80) return EINVAL_CODE;
    if (p->split && (p->split!=1 || p->split_size-1>=0x1000000 || p->chunk_size-1>=0x1000000)) return EINVAL_CODE;
    return 0;
}
/* The scheduling parameters shape how the console's kernel interleaves and splits requests; the
 * workers run each submission whole, so they are only checked. */
static ABI int32_t initialize(const AioParam *param, int32_t size) {
    if (!param) return 0;
    if (size!=(int32_t)sizeof(*param)) return EINVAL_CODE;
    int32_t result=check_scheduling(&param->low);
    if (!result) result=check_scheduling(&param->mid);
    if (!result) result=check_scheduling(&param->high);
    return result;
}
static ABI void initialize_param(AioParam *param) {
    const SchedulingParam split={0x20,0x20,1,0x100000,0x100000};
    *param=(AioParam){.low=split,.mid=split,.high={0x20,0x20,0,0,0}};
}

static const RuntimeExport exports[]={
    {"sceKernelAioInitializeImpl",initialize}, {"sceKernelAioInitializeParam",initialize_param},
    {"sceKernelAioSubmitReadCommands",submit_reads}, {"sceKernelAioSubmitReadCommandsMultiple",submit_reads_multiple},
    {"sceKernelAioSubmitWriteCommands",submit_writes}, {"sceKernelAioSubmitWriteCommandsMultiple",submit_writes_multiple},
    {"sceKernelAioWaitRequest",wait_request}, {"sceKernelAioWaitRequests",wait_requests},
    {"sceKernelAioPollRequest",poll_request}, {"sceKernelAioPollRequests",poll_requests},
    {"sceKernelAioCancelRequest",cancel_request}, {"sceKernelAioCancelRequests",cancel_requests},
    {"sceKernelAioDeleteRequest",delete_request}, {"sceKernelAioDeleteRequests",delete_requests},
};
uintptr_t runtime_aio_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_aio_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: aio submissions=%zu, commands=%zu (failed %zu), read=%" PRIu64 " bytes, written=%" PRIu64
           " bytes, cancels=%zu, deletes=%zu, wait timeouts=%zu, ids in use=%zu\n",
           submissions,commands,failures,bytes_read,bytes_written,cancels,deletes,timeouts,live);
    pthread_mutex_unlock(&lock);
}
#else
uintptr_t runtime_aio_resolve(const char *name) { (void)name; return 0; }
void runtime_aio_report(void) {}
#endif
