/* libkernel asynchronous file I/O: each submission (one id) keeps the commands copied in when it
 * was submitted and runs on the runtime's I/O workers, higher priority first, writing every
 * command's result as it completes. An id stays valid until the game deletes it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
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
    int32_t id, c_requests;
    uint32_t state;
    int write, priority;
    struct Submission *p_next;
    AioRequest commands[];
} Submission;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_work=PTHREAD_COND_INITIALIZER, s_finished=PTHREAD_COND_INITIALIZER;
static Submission *s_slots[MAX_SUBMISSIONS], *s_queues[PRIORITY_HIGH];
static int32_t s_next_id=1;
static size_t s_c_live, s_c_workers;
static size_t s_c_submissions, s_c_commands, s_c_failures, s_c_cancels, s_c_deletes, s_c_timeouts;
static uint64_t s_cb_read, s_cb_written;

static int trace(void) { static int s_is_enabled=-1; if (s_is_enabled<0) s_is_enabled=runtime_env("PS_TRACE_AIO",NULL)!=NULL; return s_is_enabled; }
static int settled(uint32_t state) { return state==COMPLETED || state==ABORTED; }
/* Lock held. */
static Submission *find(int32_t id) {
    Submission *p_submission=id>0 ? s_slots[id%MAX_SUBMISSIONS] : NULL;
    return p_submission && p_submission->id==id ? p_submission : NULL;
}
static Submission *dequeue(void) {
    for (int p=PRIORITY_HIGH-1; p>=0; --p) {
        Submission *p_submission=s_queues[p];
        if (p_submission) { s_queues[p]=p_submission->p_next; return p_submission; }
    }
    return NULL;
}
static void enqueue(Submission *p_submission) {
    Submission **pp_tail=&s_queues[p_submission->priority-1];
    while (*pp_tail) pp_tail=&(*pp_tail)->p_next;
    p_submission->p_next=NULL;
    *pp_tail=p_submission;
}
static void unqueue(Submission *p_submission) {
    Submission **pp_submission=&s_queues[p_submission->priority-1];
    while (*pp_submission && *pp_submission!=p_submission) pp_submission=&(*pp_submission)->p_next;
    if (*pp_submission) *pp_submission=p_submission->p_next;
}

static int run_command(const AioRequest *p_aio_request, int write) {
    const int64_t cb_transferred=write ? runtime_file_pwrite(p_aio_request->fd,p_aio_request->buf,(uint64_t)p_aio_request->nbyte,p_aio_request->offset)
                          : runtime_file_pread(p_aio_request->fd,p_aio_request->buf,(uint64_t)p_aio_request->nbyte,p_aio_request->offset);
    p_aio_request->result->value=cb_transferred<0 ? ERR(runtime_guest_errno((int)-cb_transferred)) : cb_transferred;
    __atomic_store_n(&p_aio_request->result->state,cb_transferred<0 ? ABORTED : COMPLETED,__ATOMIC_RELEASE);
    if (cb_transferred<0) {
        if (__atomic_fetch_add(&s_c_failures,1,__ATOMIC_RELAXED)<LOGGED_FAILURES)
            printf("Runtime: aio %s of %" PRId64 " bytes at %" PRId64 " on fd %d failed (error %#x)\n",
                   write ? "write" : "read",p_aio_request->nbyte,p_aio_request->offset,p_aio_request->fd,(unsigned)p_aio_request->result->value);
        return 1;
    }
    __atomic_add_fetch(write ? &s_cb_written : &s_cb_read,(uint64_t)cb_transferred,__ATOMIC_RELAXED);
    return 0;
}
static void *worker(void *p_unused) {
    (void)p_unused;
    pthread_mutex_lock(&s_lock);
    for (;;) {
        Submission *p_submission=dequeue();
        if (!p_submission) { pthread_cond_wait(&s_work,&s_lock); continue; }
        p_submission->state=PROCESSING;
        pthread_mutex_unlock(&s_lock);
        int has_failed=0;
        for (int32_t i=0; i<p_submission->c_requests; ++i) has_failed|=run_command(&p_submission->commands[i],p_submission->write);
        pthread_mutex_lock(&s_lock);
        p_submission->state=has_failed ? ABORTED : COMPLETED;
        pthread_cond_broadcast(&s_finished);
    }
    return NULL;
}
/* Lock held. The workers take only fault signals: the guest's own signals go to its threads. */
static void start_workers(void) {
    PsHostInterruptMask previous;
    ps_host_interrupts_hold(1,&previous);
    for (; s_c_workers<WORKERS; ++s_c_workers) {
        pthread_t thread;
        int e=pthread_create(&thread,NULL,worker,NULL);
        if (e) { fprintf(stderr,"STOP: aio worker pthread_create failed: %d\n",e); runtime_stop(21); }
        pthread_detach(thread);
    }
    ps_host_interrupts_restore(&previous);
}

/* Lock held; live leaves room for it. */
static Submission *create(const AioRequest *p_requests, int32_t c_requests, int priority, int write) {
    Submission *p_submission=malloc(sizeof(*p_submission)+(size_t)c_requests*sizeof(*p_requests));
    if (!p_submission) return NULL;
    while (s_slots[s_next_id%MAX_SUBMISSIONS]) s_next_id=s_next_id==INT32_MAX ? 1 : s_next_id+1;
    *p_submission=(Submission){.id=s_next_id,.c_requests=c_requests,.state=SUBMITTED,.write=write,.priority=priority};
    for (int32_t i=0; i<c_requests; ++i) {
        p_submission->commands[i]=p_requests[i];
        __atomic_store_n(&p_requests[i].result->state,SUBMITTED,__ATOMIC_RELEASE);
    }
    s_next_id=s_next_id==INT32_MAX ? 1 : s_next_id+1;
    s_slots[p_submission->id%MAX_SUBMISSIONS]=p_submission;
    ++s_c_live;
    enqueue(p_submission);
    return p_submission;
}
static int32_t submit(const AioRequest *p_requests, int32_t c_requests, int priority, int32_t *p_ids, int multiple, int write) {
    if (!p_requests || !p_ids) return EFAULT_CODE;
    if (c_requests<=0 || priority<PRIORITY_LOW || priority>PRIORITY_HIGH) return EINVAL_CODE;
    for (int32_t i=0; i<c_requests; ++i) if (!p_requests[i].result) return EFAULT_CODE;
    const size_t needed=multiple ? (size_t)c_requests : 1;
    runtime_exception_lock(&s_lock);
    const size_t in_use=s_c_live;
    int32_t result=in_use+needed>MAX_SUBMISSIONS ? ERR(35) : 0;
    for (size_t i=0; !result && i<needed; ++i) {
        Submission *p_submission=multiple ? create(&p_requests[i],1,priority,write) : create(p_requests,c_requests,priority,write);
        if (!p_submission) result=ERR(12);
        else p_ids[i]=p_submission->id;
    }
    if (!result) {
        if (!s_c_workers) start_workers();
        s_c_submissions+=needed; s_c_commands+=(size_t)c_requests;
        pthread_cond_broadcast(&s_work);
    }
    runtime_exception_unlock(&s_lock);
    if (trace() && !result)
        printf("Runtime: aio %s id=%d, commands=%d, priority=%d, first fd=%d, %" PRId64 " bytes at %" PRId64 "\n",
               write ? "write" : "read",p_ids[0],c_requests,priority,p_requests[0].fd,p_requests[0].nbyte,p_requests[0].offset);
    if (result) printf("Runtime: aio submit of %d commands failed (error %#x), %zu ids in use\n",c_requests,(unsigned)result,in_use);
    return result;
}
static ABI int32_t submit_reads(const AioRequest *p_requests, int32_t c_requests, int32_t prio, int32_t *p_out_id) { return submit(p_requests,c_requests,prio,p_out_id,0,0); }
static ABI int32_t submit_reads_multiple(const AioRequest *p_requests, int32_t c_requests, int32_t prio, int32_t *p_ids) { return submit(p_requests,c_requests,prio,p_ids,1,0); }
static ABI int32_t submit_writes(const AioRequest *p_requests, int32_t c_requests, int32_t prio, int32_t *p_out_id) { return submit(p_requests,c_requests,prio,p_out_id,0,1); }
static ABI int32_t submit_writes_multiple(const AioRequest *p_requests, int32_t c_requests, int32_t prio, int32_t *p_ids) { return submit(p_requests,c_requests,prio,p_ids,1,1); }

/* Lock held: ESRCH when an id is not live, otherwise whether the AND (all) or OR (any) of the
 * ids have settled, their states written. */
static int32_t check(const int32_t *p_ids, int32_t c_ids, int32_t *p_states, int all, int *p_done) {
    *p_done=all;
    for (int32_t i=0; i<c_ids; ++i) {
        const Submission *p_submission=find(p_ids[i]);
        if (!p_submission) return ESRCH_CODE;
        p_states[i]=(int32_t)p_submission->state;
        if (all) *p_done&=settled(p_submission->state);
        else *p_done|=settled(p_submission->state);
    }
    return 0;
}
static int32_t wait_ids(const int32_t *p_ids, int32_t c_ids, int32_t *p_states, int all, uint32_t *p_usec) {
    if (!p_ids || !p_states) return EFAULT_CODE;
    if (c_ids<=0) return EINVAL_CODE;
    const uint64_t deadline=p_usec ? runtime_wait_clock()+(uint64_t)*p_usec*1000 : 0;
    const struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
    const uint64_t wait_start=runtime_wait_clock();
    runtime_exception_lock(&s_lock);
    int is_done=0;
    int32_t result=check(p_ids,c_ids,p_states,all,&is_done);
    while (!result && !is_done) {
        if (p_usec && runtime_wait_clock()>=deadline) { result=ERR(60); ++s_c_timeouts; break; }
        int e=runtime_exception_wait(&s_finished,&s_lock,p_usec ? &end : NULL);
        if (e && e!=ETIMEDOUT) { fprintf(stderr,"STOP: aio wait host error %d\n",e); runtime_stop(21); }
        result=check(p_ids,c_ids,p_states,all,&is_done);
    }
    runtime_exception_unlock(&s_lock);
    runtime_wait_note(0,runtime_wait_clock()-wait_start);
    if (trace()) printf("Runtime: aio wait id=%d (of %d), timeout=%lld us, result=%#x, state=%d\n",p_ids[0],c_ids,
                        p_usec ? (long long)*p_usec : -1LL,(unsigned)result,p_states[0]);
    if (p_usec) {
        const uint64_t now=runtime_wait_clock();
        *p_usec=result || now>=deadline ? 0 : (uint32_t)((deadline-now)/1000);
    }
    return result;
}
static ABI int32_t wait_request(int32_t id, int32_t *p_state, uint32_t *p_usec) { return wait_ids(&id,1,p_state,1,p_usec); }
static ABI int32_t wait_requests(const int32_t *p_ids, int32_t c_ids, int32_t *p_states, uint32_t mode, uint32_t *p_usec) {
    if (mode!=WAIT_AND && mode!=WAIT_OR) return EINVAL_CODE;
    return wait_ids(p_ids,c_ids,p_states,mode==WAIT_AND,p_usec);
}
static ABI int32_t poll_requests(const int32_t *p_ids, int32_t c_ids, int32_t *p_states) {
    if (!p_ids || !p_states) return EFAULT_CODE;
    if (c_ids<=0) return EINVAL_CODE;
    int is_done;
    runtime_exception_lock(&s_lock);
    const int32_t result=check(p_ids,c_ids,p_states,1,&is_done);
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t poll_request(int32_t id, int32_t *p_state) { return poll_requests(&id,1,p_state); }

/* Lock held: a submission no worker has taken is aborted, its commands with ECANCELED. */
static int abort_queued(Submission *p_submission) {
    if (p_submission->state!=SUBMITTED) return 0;
    unqueue(p_submission);
    p_submission->state=ABORTED;
    for (int32_t i=0; i<p_submission->c_requests; ++i) {
        p_submission->commands[i].result->value=ERR(85);
        __atomic_store_n(&p_submission->commands[i].result->state,ABORTED,__ATOMIC_RELEASE);
    }
    ++s_c_cancels;
    pthread_cond_broadcast(&s_finished);
    return 1;
}
static ABI int32_t cancel_requests(const int32_t *p_ids, int32_t c_ids, int32_t *p_states) {
    if (!p_ids || !p_states) return EFAULT_CODE;
    if (c_ids<=0) return EINVAL_CODE;
    int32_t result=0;
    runtime_exception_lock(&s_lock);
    for (int32_t i=0; i<c_ids; ++i) {
        Submission *p_submission=find(p_ids[i]);
        if (!p_submission) { result=ESRCH_CODE; continue; }
        abort_queued(p_submission);
        p_states[i]=(int32_t)p_submission->state;
    }
    runtime_exception_unlock(&s_lock);
    return result;
}
static ABI int32_t cancel_request(int32_t id, int32_t *p_state) { return cancel_requests(&id,1,p_state); }
/* One a worker is running is let finish first: its results land in the game's memory. */
static ABI int32_t delete_requests(const int32_t *p_ids, int32_t c_ids, int32_t *p_results) {
    if (!p_ids || !p_results) return EFAULT_CODE;
    if (c_ids<=0) return EINVAL_CODE;
    runtime_exception_lock(&s_lock);
    for (int32_t i=0; i<c_ids; ++i) {
        Submission *p_submission=find(p_ids[i]);
        while (p_submission && p_submission->state==PROCESSING) {
            int e=runtime_exception_wait(&s_finished,&s_lock,NULL);
            if (e && e!=ETIMEDOUT) { fprintf(stderr,"STOP: aio delete host error %d\n",e); runtime_stop(21); }
            p_submission=find(p_ids[i]);
        }
        if (!p_submission) { p_results[i]=ESRCH_CODE; continue; }
        abort_queued(p_submission);
        s_slots[p_submission->id%MAX_SUBMISSIONS]=NULL;
        --s_c_live; ++s_c_deletes;
        free(p_submission);
        p_results[i]=0;
    }
    runtime_exception_unlock(&s_lock);
    return 0;
}
static ABI int32_t delete_request(int32_t id, int32_t *p_result) { return delete_requests(&id,1,p_result); }

static int32_t check_scheduling(const SchedulingParam *p_scheduling_param) {
    if (p_scheduling_param->window>0x80 || p_scheduling_param->delayed>0x80) return EINVAL_CODE;
    if (p_scheduling_param->split && (p_scheduling_param->split!=1 || p_scheduling_param->split_size-1>=0x1000000 || p_scheduling_param->chunk_size-1>=0x1000000)) return EINVAL_CODE;
    return 0;
}
/* The scheduling parameters shape how the console's kernel interleaves and splits requests; the
 * workers run each submission whole, so they are only checked. */
static ABI int32_t initialize(const AioParam *p_param, int32_t cb_param) {
    if (!p_param) return 0;
    if (cb_param!=(int32_t)sizeof(*p_param)) return EINVAL_CODE;
    int32_t result=check_scheduling(&p_param->low);
    if (!result) result=check_scheduling(&p_param->mid);
    if (!result) result=check_scheduling(&p_param->high);
    return result;
}
static ABI void initialize_param(AioParam *p_param) {
    const SchedulingParam split={0x20,0x20,1,0x100000,0x100000};
    *p_param=(AioParam){.low=split,.mid=split,.high={0x20,0x20,0,0,0}};
}

static const RuntimeExport s_exports[]={
    {"sceKernelAioInitializeImpl",initialize}, {"sceKernelAioInitializeParam",initialize_param},
    {"sceKernelAioSubmitReadCommands",submit_reads}, {"sceKernelAioSubmitReadCommandsMultiple",submit_reads_multiple},
    {"sceKernelAioSubmitWriteCommands",submit_writes}, {"sceKernelAioSubmitWriteCommandsMultiple",submit_writes_multiple},
    {"sceKernelAioWaitRequest",wait_request}, {"sceKernelAioWaitRequests",wait_requests},
    {"sceKernelAioPollRequest",poll_request}, {"sceKernelAioPollRequests",poll_requests},
    {"sceKernelAioCancelRequest",cancel_request}, {"sceKernelAioCancelRequests",cancel_requests},
    {"sceKernelAioDeleteRequest",delete_request}, {"sceKernelAioDeleteRequests",delete_requests},
};
uintptr_t runtime_aio_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_aio_report(void) {
    if (!runtime_exception_report_lock(&s_lock,"aio")) return;
    printf("Runtime: aio submissions=%zu, commands=%zu (failed %zu), read=%" PRIu64 " bytes, written=%" PRIu64
           " bytes, cancels=%zu, deletes=%zu, wait timeouts=%zu, ids in use=%zu\n",
           s_c_submissions,s_c_commands,s_c_failures,s_cb_read,s_cb_written,s_c_cancels,s_c_deletes,s_c_timeouts,s_c_live);
    runtime_exception_unlock(&s_lock);
}
