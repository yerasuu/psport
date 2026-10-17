/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <dirent.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/uio.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <x86intrin.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    case EINPROGRESS: return 36; case EALREADY: return 37; case ENOTSOCK: return 38; case EMSGSIZE: return 40;
    case ENOPROTOOPT: return 42; case EOPNOTSUPP: return 45; case EADDRINUSE: return 48; case EADDRNOTAVAIL: return 49;
    case ENETUNREACH: return 51; case ECONNABORTED: return 53; case ECONNRESET: return 54; case ENOBUFS: return 55;
    case EISCONN: return 56; case ENOTCONN: return 57; case ECONNREFUSED: return 61; case EHOSTUNREACH: return 65;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static struct timespec s_process_start;
static uint64_t s_process_start_tsc;
__attribute__((constructor)) static void remember_start(void) { clock_gettime(CLOCK_MONOTONIC,&s_process_start); s_process_start_tsc=__rdtsc(); }
static int host_clock(uint32_t id,clockid_t *p_out) {
    switch (id) {
    case 0: case 9: case 10: *p_out=CLOCK_REALTIME; return 1;            /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *p_out=CLOCK_MONOTONIC; return 1; /* MONOTONIC/UPTIME */
    case 13: *p_out=CLOCK_REALTIME_COARSE; return 1;                    /* SECOND */
    case 14: *p_out=CLOCK_THREAD_CPUTIME_ID; return 1;
    case 2: case 15: *p_out=CLOCK_PROCESS_CPUTIME_ID; return 1;         /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: *p_out=CLOCK_MONOTONIC; return 1; /* PS4 network clocks */
    default: return 0;
    }
}
static int clock_read(uint32_t id,GuestTimespec *p_timespec) {
    clockid_t host;
    if (!p_timespec || !host_clock(id,&host)) return EINVAL;
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    p_timespec->sec=t.tv_sec; p_timespec->nsec=t.tv_nsec; return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *p_timespec) {
    int e=clock_read(id,p_timespec); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *p_timespec) {
    int e=clock_read(id,p_timespec); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *p_timespec) {
    clockid_t host; struct timespec t;
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (p_timespec) { p_timespec->sec=t.tv_sec; p_timespec->nsec=t.tv_nsec; }
    return 0;
}
static ABI uint64_t process_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-s_process_start.tv_sec)*1000000+(uint64_t)((t.tv_nsec-s_process_start.tv_nsec)/1000);
}
/* The console's process time counter is the TSC since the process started, and games mix it with
 * sceKernelGetTscFrequency to convert it, so both tick at the TSC rate. */
static ABI uint64_t process_time_counter(void) { return __rdtsc()-s_process_start_tsc; }
static ABI uint64_t read_tsc(void) { return __rdtsc(); }
static uint64_t s_tsc_hz;
static ABI uint64_t tsc_frequency(void) {
    if (!s_tsc_hz) {
        struct timespec a,b,nap={0,20000000};
        clock_gettime(CLOCK_MONOTONIC,&a); uint64_t t0=__rdtsc();
        nanosleep(&nap,NULL);
        clock_gettime(CLOCK_MONOTONIC,&b); uint64_t t1=__rdtsc();
        uint64_t ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000+(uint64_t)(b.tv_nsec-a.tv_nsec);
        s_tsc_hz=(t1-t0)*1000000000/(ns ? ns : 1);
    }
    return s_tsc_hz;
}
static ABI uint64_t process_time_frequency(void) { return tsc_frequency(); }
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
/* psport (frame stats): how often and how long the game sleeps (it polls GPU labels that way). */
static _Atomic uint64_t s_c_sleep_calls, s_sleep_total_ns;
void runtime_sleep_stats(uint64_t *p_calls, uint64_t *p_ns) {
    *p_calls=atomic_exchange(&s_c_sleep_calls,0); *p_ns=atomic_exchange(&s_sleep_total_ns,0);
}
static int sleep_ns(uint64_t ns) {
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)}, a, b;
    clock_gettime(CLOCK_MONOTONIC,&a);
    int result=0;
    while (nanosleep(&t,&t)) if (errno!=EINTR) { result=errno; break; }
    clock_gettime(CLOCK_MONOTONIC,&b);
    atomic_fetch_add(&s_c_sleep_calls,1);
    atomic_fetch_add(&s_sleep_total_ns,(uint64_t)((b.tv_sec-a.tv_sec)*1000000000+(b.tv_nsec-a.tv_nsec)));
    runtime_wait_note(3,(uint64_t)((b.tv_sec-a.tv_sec)*1000000000+(b.tv_nsec-a.tv_nsec)));
    return result;
}
static ABI int32_t kernel_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t kernel_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *p_requested,GuestTimespec *p_remaining) {
    if (!p_requested || p_requested->nsec<0 || p_requested->nsec>=1000000000 || p_requested->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)p_requested->sec*1000000000+(uint64_t)p_requested->nsec);
    if (p_remaining) { p_remaining->sec=0; p_remaining->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *p_requested,GuestTimespec *p_remaining) {
    return posix_nanosleep(p_requested,p_remaining) ? ERR(*runtime_errno()) : 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *p_timezone) {
    if (!p_timezone) return ERR(22);
    time_t now=time(NULL); struct tm local; localtime_r(&now,&local);
    p_timezone->minuteswest=(int32_t)(-local.tm_gmtoff/60); p_timezone->dsttime=0;
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *p_timeval,GuestTimezone *p_timezone) {
    struct timeval t;
    gettimeofday(&t,NULL);
    if (p_timeval) { p_timeval->sec=t.tv_sec; p_timeval->usec=t.tv_usec; }
    if (p_timezone) kernel_gettimezone(p_timezone);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *p_timeval) {
    if (!p_timeval) return ERR(22);
    return posix_gettimeofday(p_timeval,NULL);
}
static ABI int64_t posix_time(int64_t *p_out) { int64_t t=(int64_t)time(NULL); if (p_out) *p_out=t; return t; }
/* SceKernelTimesec: west_sec is the standard offset east of UTC (whatever its name says), dst_sec
 * the daylight saving on top of it; local time is UTC plus both. */
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } GuestTimesec;
/* Daylight saving in effect at a broken-down time: its offset beyond the zone's standard one. */
static int64_t daylight_saving(const struct tm *p_time) { return p_time->tm_isdst>0 ? p_time->tm_gmtoff+timezone : 0; }
static ABI int32_t utc_to_local(int64_t utc,int64_t *p_local,GuestTimesec *p_zone,uint64_t *p_dst_sec) {
    if (!p_local) return ERR(22);
    tzset();
    time_t at=(time_t)utc; struct tm t;
    if (!localtime_r(&at,&t)) return ERR(22);
    const int64_t dst=daylight_saving(&t);
    *p_local=utc+t.tm_gmtoff;
    if (p_zone) { p_zone->t=utc; p_zone->west_sec=(uint32_t)(t.tm_gmtoff-dst); p_zone->dst_sec=(uint32_t)dst; }
    if (p_dst_sec) *p_dst_sec=(uint64_t)dst;
    return 0;
}
/* The zone's daylight saving whether or not it is in effect at that time: libc's mktime moves a
 * time the caller marks as daylight saving by it when the zone is on standard time then. */
static int64_t zone_daylight_saving(time_t at) {
    for (int half=0; half<2; ++half) {
        const time_t probe=at+half*183*86400; struct tm t;
        if (localtime_r(&probe,&t) && t.tm_isdst>0) return daylight_saving(&t);
    }
    return 0;
}
/* libc's mktime passes the tm_isdst it was given as the second argument and settles a conflict
 * with the zone itself, from the zone in effect at that local time written to `zone`. */
static ABI int32_t local_to_utc(int64_t local,int32_t is_dst,int64_t *p_utc,GuestTimesec *p_zone,int32_t *p_dst_sec) {
    (void)is_dst;
    if (!p_zone) return ERR(22);
    tzset();
    time_t at=(time_t)local; struct tm t;
    if (!gmtime_r(&at,&t)) return ERR(22);
    t.tm_isdst=-1;
    const time_t when=mktime(&t);
    const int64_t dst=daylight_saving(&t);
    if (p_utc) *p_utc=(int64_t)when;
    p_zone->t=(int64_t)when; p_zone->west_sec=(uint32_t)(t.tm_gmtoff-dst); p_zone->dst_sec=(uint32_t)dst;
    if (p_dst_sec) *p_dst_sec=(int32_t)zone_daylight_saving(when);
    return 0;
}

/* ---- process ---- */
static uint32_t s_sdk_version, s_psf_attributes;
void runtime_process_configure(uint32_t sdk, uint32_t attributes) { s_sdk_version=sdk; s_psf_attributes=attributes; }
/* Always the base PS4, never a Pro or a PS5 (sceKernelIsProspero shares the answer): the GPU library
 * builds its command buffers for it (its own sceKernelIsNeoMode answers 0), so a Pro answer here
 * would contradict what it draws. */
static ABI int32_t is_neo_mode(void) { return 0; }
static ABI int32_t compiled_sdk_version(int32_t *p_version) {
    if (!p_version) return ERR(22);
    *p_version=(int32_t)s_sdk_version; return 0;
}
/* param.sfo ATTRIBUTE bits 15 and 16 ask for the sixth and seventh core; the answers are
 * shadPS4's. */
static ABI int32_t cpu_mode(void) {
    const int six=(s_psf_attributes>>15)&1, seven=(s_psf_attributes>>16)&1;
    return six && seven ? 2 : seven ? 5 : 0;
}
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { sched_yield(); return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    psport_gpu_flush_caches(1);
    fflush(stdout);
    _exit(status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    fprintf(stderr,"STOP: guest raised debug exception code=0x%x argument=0x%llx\n",code,(unsigned long long)argument);
    /* Who raised it: the caller and the game's return addresses further up this thread's stack. */
    const uintptr_t va_caller=(uintptr_t)__builtin_return_address(0);
    uint64_t off_code=0;
    const char *sz_module=runtime_module_locate(va_caller,&off_code);
    if (sz_module) fprintf(stderr,"STOP: called from %s+0x%llx; game code up the stack:",sz_module,(unsigned long long)off_code);
    else fprintf(stderr,"STOP: called from host 0x%llx; game code up the stack:",(unsigned long long)va_caller);
    /* Read through the kernel because a guest stack may end before 256 words do. */
    uint64_t stack[256]={0};
    struct iovec local={stack,sizeof(stack)}, remote={__builtin_frame_address(0),sizeof(stack)};
    const ssize_t cb_read=process_vm_readv(getpid(),&local,1,&remote,1,0);
    for (ssize_t i=0,c_found=0;i<cb_read/8 && c_found<12;++i) {
        if ((sz_module=runtime_module_locate(stack[i],&off_code))) {
            fprintf(stderr," %s+0x%llx",sz_module,(unsigned long long)off_code);
            ++c_found;
        }
    }
    fputc('\n',stderr);
    runtime_report();
    runtime_stop(23);
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n",stderr);
    return 0;
}
typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset s_signal_mask;
static uintptr_t s_handlers[128];
static ABI uintptr_t guest_signal(int sig,uintptr_t handler) {
    if (sig<=0 || sig>=128) { *runtime_errno()=22; return (uintptr_t)-1; } /* SIG_ERR */
    uintptr_t old=s_handlers[sig]; s_handlers[sig]=handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n",sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *p_set,GuestSigset *p_old) {
    if (p_old) *p_old=s_signal_mask;
    if (!p_set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) s_signal_mask.bits[i]|=p_set->bits[i];        /* SIG_BLOCK */
        else if (how==2) s_signal_mask.bits[i]&=~p_set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) s_signal_mask.bits[i]=p_set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
void runtime_kernel_set_signal_mask(const uint32_t bits[4]) { memcpy(s_signal_mask.bits,bits,sizeof(s_signal_mask.bits)); }
static ABI int32_t guest_sigfillset(GuestSigset *p_set) { if (!p_set) return fail_posix(EINVAL); memset(p_set,0xff,sizeof(*p_set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *p_set) { if (!p_set) return fail_posix(EINVAL); memset(p_set,0,sizeof(*p_set)); return 0; }
/* Guest exception handlers are called from the runtime's host signal handler and return into it, so
 * no guest frame is a signal trampoline. */
static ABI int32_t is_signal_return(const void *p_address) { (void)p_address; return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *p_out) {
    struct rusage r;
    if (!p_out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    memset(p_out,0,sizeof(*p_out));
    p_out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    p_out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *p_name,uint32_t namelen,void *p_old,uint64_t *p_oldlen,const void *p_new_value,uint64_t newlen) {
    (void)newlen;
    if (!p_name || namelen<2 || p_new_value) return fail_posix(EINVAL);
    if (p_name[0]==1 && p_name[1]==37) { /* kern.arandom */
        if (!p_old || !p_oldlen) return fail_posix(EINVAL);
        if (getrandom(p_old,(size_t)*p_oldlen,0)<0) return fail_posix(errno);
        return 0;
    }
    if (p_name[0]==6 && (p_name[1]==7 || p_name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!p_oldlen) return fail_posix(EINVAL);
        int32_t value=p_name[1]==7 ? PAGE : 7;
        if (p_old) { if (*p_oldlen<4) return fail_posix(ENOMEM); memcpy(p_old,&value,4); }
        *p_oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",p_name[i]);
    fputc('\n',stderr);
    runtime_stop(21);
}
static ABI int32_t uuid_create(uint8_t *p_uuid) {
    if (!p_uuid) return ERR(22);
    if (getrandom(p_uuid,16,0)!=16) return ERR(14);
    p_uuid[7]=(uint8_t)((p_uuid[7]&0x0f)|0x40); /* version 4 in the high bits of the little-endian time_hi */
    p_uuid[8]=(uint8_t)((p_uuid[8]&0x3f)|0x80);
    return 0;
}

#define RANDOM_INVALID ((int32_t)0x817C0016)
#define RANDOM_MAX_SIZE 64
static ABI int32_t random_number(void *p_buffer,uint64_t cb_buffer) {
    if (!p_buffer || cb_buffer>RANDOM_MAX_SIZE) return RANDOM_INVALID;
    return getrandom(p_buffer,cb_buffer,0)==(ssize_t)cb_buffer ? 0 : RANDOM_INVALID;
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *p_once,void (ABI *pfn_routine)(void)) {
    if (!p_once || !pfn_routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(p_once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(p_once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            pfn_routine();
            __atomic_store_n(p_once,1,__ATOMIC_RELEASE);
            return 0;
        }
        sched_yield();
    }
}
static ABI int32_t posix_once(int32_t *p_once,void (ABI *pfn_routine)(void)) { return thread_once(p_once,pfn_routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; KeyDestructor pfn_destructor; } s_keys[KEYS];
static pthread_mutex_t s_key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *s_key_values[KEYS];
static ABI int32_t key_create(uint32_t *p_key,KeyDestructor pfn_destructor) {
    if (!p_key) return ERR(22);
    pthread_mutex_lock(&s_key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!s_keys[i].used) {
        s_keys[i].used=1; s_keys[i].pfn_destructor=pfn_destructor;
        pthread_mutex_unlock(&s_key_lock);
        *p_key=i; return 0;
    }
    pthread_mutex_unlock(&s_key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    pthread_mutex_lock(&s_key_lock);
    int32_t r=s_keys[key].used ? 0 : ERR(22);
    s_keys[key].used=0; s_keys[key].pfn_destructor=NULL;
    pthread_mutex_unlock(&s_key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key,void *p_value) {
    if (!key || key>=KEYS || !s_keys[key].used) return ERR(22);
    s_key_values[key]=p_value; return 0;
}
static ABI void *key_get(uint32_t key) { return key && key<KEYS ? s_key_values[key] : NULL; }
void runtime_thread_keys_cleanup(void) {
    for (int round=0;round<4;++round) {
        int any=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *p_value=s_key_values[i];
            if (!p_value || !s_keys[i].used || !s_keys[i].pfn_destructor) continue;
            s_key_values[i]=NULL; any=1;
            s_keys[i].pfn_destructor(p_value);
        }
        if (!any) break;
    }
}
static ABI int32_t posix_key_create(uint32_t *p_key,KeyDestructor pfn_destructor) { int32_t r=key_create(p_key,pfn_destructor); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_delete(uint32_t k) { int32_t r=key_delete(k); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_set(uint32_t k,void *p_value) { int32_t r=key_set(k,p_value); return r ? r&0xffff : 0; }

/* What newer SDKs' libc.prx registers or asks for while it starts (Atelier Ryza, CUSA16458).
 * Thread destructors and atexit reports are the kernel's bookkeeping for that libc: nothing here
 * runs them. No sanitizer replaces malloc or new. */
static ABI int32_t set_thread_dtors(void *p_dtors) { (void)p_dtors; return 0; }
static ABI int32_t set_thread_atexit_count(void *p_count) { (void)p_count; return 0; }
static ABI int32_t set_thread_atexit_report(void *p_report) { (void)p_report; return 0; }
/* libc pins the module of each thread_local destructor it registers so that the module is not
 * unloaded before the destructor runs; modules here are never unmapped, so there is nothing to pin. */
static ABI int32_t rtld_thread_atexit_increment(void *p_module) { (void)p_module; return 0; }
static ABI int32_t rtld_thread_atexit_decrement(void *p_module) { (void)p_module; return 0; }
static ABI void *sanitizer_replace(void *p_table) { (void)p_table; return NULL; }
static ABI int32_t sanitizer_enabled(void) { return 0; }
/* libc follows the info's two pointers when it starts tracing its heap, a null one faults
 * (guest offset 0x19fe22c in libc.prx); tracing stays off with an empty mask and table. The
 * layout is upstream universal_ps4_native's. */
static uint64_t s_heap_trace_mask, s_heap_trace_table[512];
static ABI int32_t libc_heap_trace_info(void *p_info) {
    if (p_info) {
        ((uint32_t *)p_info)[3]=0;
        ((uint64_t **)p_info)[2]=&s_heap_trace_mask;
        ((uint64_t **)p_info)[3]=s_heap_trace_table;
    }
    return 0;
}
/* The kernel's named objects only give a synchronization object a name for debuggers, so an id and
 * its type are all there is to keep. Ids stay small: sem_t keeps one in 16 bits. */
#define NAMED_OBJECTS 1024
static _Atomic uint32_t s_named_object_types[NAMED_OBJECTS];
static ABI int32_t namedobj_create(const char *sz_name, void *p_object, uint32_t type) {
    (void)p_object;
    if (!sz_name) return fail_posix(EINVAL);
    if (strnlen(sz_name,33)>32) return fail_posix(ENAMETOOLONG);
    for (int32_t h_object=1;h_object<NAMED_OBJECTS;++h_object) {
        uint32_t free_slot=0;
        if (atomic_compare_exchange_strong(&s_named_object_types[h_object],&free_slot,(type&0xffff)+1)) return h_object;
    }
    return fail_posix(ENOMEM);
}
static ABI int32_t namedobj_delete(int32_t h_object, uint32_t type) {
    uint32_t slot=(type&0xffff)+1;
    if (h_object<=0 || h_object>=NAMED_OBJECTS || !atomic_compare_exchange_strong(&s_named_object_types[h_object],&slot,0))
        return fail_posix(ESRCH);
    return 0;
}

static const RuntimeExport s_exports[]={
    {"sceKernelClockGettime",kernel_clock_gettime}, {"clock_gettime",posix_clock_gettime},
    {"clock_getres",posix_clock_getres},
    {"sceKernelGetProcessTime",process_time}, {"sceKernelGetProcessTimeCounter",process_time_counter},
    {"sceKernelGetProcessTimeCounterFrequency",process_time_frequency},
    {"sceKernelReadTsc",read_tsc}, {"sceKernelGetTscFrequency",tsc_frequency},
    {"sceKernelUsleep",kernel_usleep}, {"usleep",posix_usleep}, {"sleep",posix_sleep},
    {"sceKernelSleep",kernel_sleep},
    {"nanosleep",posix_nanosleep}, {"_nanosleep",posix_nanosleep}, {"sceKernelNanosleep",kernel_nanosleep},
    {"sceKernelGettimezone",kernel_gettimezone}, {"gettimeofday",posix_gettimeofday},
    {"sceKernelGettimeofday",kernel_gettimeofday},
    {"time",posix_time},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sceKernelIsNeoMode",is_neo_mode}, {"sceKernelIsProspero",is_neo_mode}, {"sceKernelSetGPO",runtime_stub}, {"sceKernelGetCompiledSdkVersion",compiled_sdk_version},
    {"sceKernelGetCpumode",cpu_mode},
    {"getpagesize",get_pagesize}, {"getpid",get_pid}, {"sched_yield",yield},
    {"_exit",hard_exit},
    {"sceKernelDebugRaiseException",raise_exception},
    {"sceKernelDebugRaiseExceptionOnReleaseMode",raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo",print_backtrace},
    {"signal",guest_signal}, {"sigprocmask",guest_sigprocmask}, {"_sigprocmask",guest_sigprocmask},
    {"sigfillset",guest_sigfillset}, {"sigemptyset",guest_sigemptyset}, {"_is_signal_return",is_signal_return},
    {"getrusage",guest_getrusage}, {"sysctl",guest_sysctl}, {"sceKernelUuidCreate",uuid_create},
    {"sceRandomGetRandomNumber",random_number},
    {"scePthreadOnce",thread_once}, {"pthread_once",posix_once},
    {"scePthreadKeyCreate",key_create}, {"scePthreadKeyDelete",key_delete},
    {"scePthreadSetspecific",key_set}, {"scePthreadGetspecific",key_get},
    {"pthread_key_create",posix_key_create}, {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set}, {"pthread_getspecific",key_get},
    {"_sceKernelSetThreadDtors",set_thread_dtors}, {"_sceKernelSetThreadAtexitCount",set_thread_atexit_count},
    {"_sceKernelSetThreadAtexitReport",set_thread_atexit_report},
    {"_sceKernelRtldThreadAtexitIncrement",rtld_thread_atexit_increment},
    {"_sceKernelRtldThreadAtexitDecrement",rtld_thread_atexit_decrement},
    {"sceKernelGetSanitizerMallocReplaceExternal",sanitizer_replace},
    {"sceKernelGetSanitizerNewReplaceExternal",sanitizer_replace},
    {"sceKernelIsAddressSanitizerEnabled",sanitizer_enabled},
    {"sceLibcHeapGetTraceInfo",libc_heap_trace_info},
    {"__sys_namedobj_create",namedobj_create}, {"__sys_namedobj_delete",namedobj_delete},
};
uintptr_t runtime_kernel_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
#else
uintptr_t runtime_kernel_resolve(const char *name) { (void)name; return 0; }
void runtime_process_configure(uint32_t sdk, uint32_t attributes) { (void)sdk; (void)attributes; }
int32_t runtime_guest_errno(int e) { return e ? 5 : 0; }
void runtime_thread_keys_cleanup(void) {}
#endif

static void sample_start(void);
static void sample_report(void);

/* psport (frame stats): time guest threads spend blocked in the runtime (condition variables,
 * mutexes, semaphores, sleeps), by thread and guest call site: the first return address into the
 * game's code on the stack. Shows where the game waits for the GPU (labels, frame pacing). */
typedef struct { _Atomic uint64_t key; uint64_t site, site2, site3; int tid, kind; char sz_name[16]; _Atomic uint64_t count, ns; } WaitSite;
static WaitSite s_wait_sites[512];
static _Thread_local int s_wait_tid;
void runtime_guest_call_sites(uint64_t out[3]);
/* The first three return addresses into the game's code on this thread's stack (guest offsets). */
static void guest_call_sites(uint64_t out[3]) {
    const uintptr_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    static _Thread_local uintptr_t s_stack_hi;
    if (!s_stack_hi) {
        pthread_attr_t attr; void *p_base=NULL; size_t cb_stack=0;
        if (!pthread_getattr_np(pthread_self(),&attr)) { pthread_attr_getstack(&attr,&p_base,&cb_stack); pthread_attr_destroy(&attr); }
        s_stack_hi=(uintptr_t)p_base+cb_stack;
    }
    const uintptr_t *p_stack=(const uintptr_t *)__builtin_frame_address(0);
    int is_found=0;
    out[0]=out[1]=out[2]=0;
    for (int i=0;i<400 && is_found<3 && (uintptr_t)(p_stack+i+1)<=s_stack_hi;++i) if (p_stack[i]>=text_lo && p_stack[i]<text_hi) out[is_found++]=p_stack[i]-text_lo;
}
void runtime_wait_note(int kind, uint64_t ns) {
    static int s_is_enabled=-1;
    if (s_is_enabled<0) s_is_enabled=getenv("PS_FRAME_STATS")!=NULL;
    if (!s_is_enabled) return;
    if (!s_wait_tid) s_wait_tid=(int)syscall(SYS_gettid);
    uint64_t sites[3];
    guest_call_sites(sites);
    const uint64_t key=((sites[0]<<20)^(sites[1]*0x9E3779B1ull)^(sites[2]<<7)^((uint64_t)s_wait_tid<<4)^(uint64_t)kind)|1;
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>55;i<512;++i) {
        WaitSite *p_wait_site=&s_wait_sites[(slot+i)%512];
        uint64_t expected=0;
        if (atomic_load(&p_wait_site->key)==key || atomic_compare_exchange_strong(&p_wait_site->key,&expected,key)) {
            if (!p_wait_site->tid) { p_wait_site->site=sites[0]; p_wait_site->site2=sites[1]; p_wait_site->site3=sites[2]; p_wait_site->kind=kind; pthread_getname_np(pthread_self(),p_wait_site->sz_name,sizeof(p_wait_site->sz_name)); p_wait_site->tid=s_wait_tid; }
            atomic_fetch_add(&p_wait_site->count,1); atomic_fetch_add(&p_wait_site->ns,ns);
            return;
        }
    }
}
void runtime_wait_report(double frames) {
    sample_start();
    sample_report();
    static const char *s_kinds[]={"cond","mutex","sema","sleep"};
    struct { WaitSite *p_wait_site; uint64_t ns, c_waits; } rows[512]; int c_rows=0;
    for (int i=0;i<512;++i) {
        uint64_t ns=atomic_exchange(&s_wait_sites[i].ns,0), c_waits=atomic_exchange(&s_wait_sites[i].count,0);
        if (c_waits) { rows[c_rows].p_wait_site=&s_wait_sites[i]; rows[c_rows].ns=ns; rows[c_rows].c_waits=c_waits; ++c_rows; }
    }
    if (!c_rows || frames<=0) return;
    static FILE *s_p_dump; static int s_is_dump_checked;
    if (!s_is_dump_checked) { const char *sz_path=getenv("PS_WAIT_LOG"); if (sz_path && *sz_path) s_p_dump=fopen(sz_path,"w"); s_is_dump_checked=1; }
    if (s_p_dump) {
        for (int i=0;i<c_rows;++i)
            fprintf(s_p_dump,"%s %d %s %#lx %#lx %#lx %.2f %.3f\n", rows[i].p_wait_site->sz_name, rows[i].p_wait_site->tid, s_kinds[rows[i].p_wait_site->kind&3],
                    (unsigned long)rows[i].p_wait_site->site, (unsigned long)rows[i].p_wait_site->site2, (unsigned long)rows[i].p_wait_site->site3,
                    rows[i].c_waits/frames, rows[i].ns/(frames*1e6));
        fprintf(s_p_dump,"--\n"); fflush(s_p_dump);
    }
    for (int i=1;i<c_rows;++i) for (int j=i;j>0 && rows[j].ns>rows[j-1].ns;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Guest waits per frame (thread kind +site):");
    for (int i=0;i<c_rows && i<12;++i) if (rows[i].ns/rows[i].c_waits<5000000)
        printf("%s %s %s +%#lx<+%#lx<+%#lx %.1fx %.2f ms", i ? ";" : "", rows[i].p_wait_site->sz_name, s_kinds[rows[i].p_wait_site->kind&3],
               (unsigned long)rows[i].p_wait_site->site, (unsigned long)rows[i].p_wait_site->site2, (unsigned long)rows[i].p_wait_site->site3,
               rows[i].c_waits/frames, rows[i].ns/(frames*1e6));
    printf("\n");
}

/* psport PS_SAMPLE_THREAD=<name> (with frame stats): where that thread runs, sampled with SIGPROF
 * every 0.5 ms; reported with the wait profile. A thread that never blocks but waits for the GPU
 * shows up spinning in its polling loop. */
static _Atomic uint64_t s_sample_keys[1024], s_c_slot_samples[1024];
static uint64_t s_sample_rips[1024], s_sample_callers[1024];
static _Atomic uint64_t s_c_samples;
static void sample_handler(int sig, siginfo_t *p_info, void *p_context) {
    (void)sig; (void)p_info;
    const ucontext_t *p_ucontext=(const ucontext_t *)p_context;
    uint64_t rip=(uint64_t)p_ucontext->uc_mcontext.gregs[REG_RIP];
    const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
    /* Host code: keyed with its guest caller, the first return address into the game on the stack. */
    uint64_t caller=0;
    if (rip<text_lo || rip>=text_hi) {
        uint64_t stack[256]={0};
        struct iovec local={stack,sizeof(stack)}, remote={(void *)p_ucontext->uc_mcontext.gregs[REG_RSP],sizeof(stack)};
        const ssize_t got=process_vm_readv(getpid(),&local,1,&remote,1,0);
        for (ssize_t i=0;i<got/8;++i) if (stack[i]>=text_lo && stack[i]<text_hi) { caller=stack[i]-text_lo; break; }
    }
    const uint64_t key=(rip*0x9E3779B97F4A7C15ull)^caller^1;
    atomic_fetch_add(&s_c_samples,1);
    for (uint64_t i=0,slot=(key*0x9E3779B97F4A7C15ull)>>54;i<1024;++i) {
        _Atomic uint64_t *p_key=&s_sample_keys[(slot+i)%1024];
        uint64_t expected=0;
        if (atomic_load(p_key)==key || atomic_compare_exchange_strong(p_key,&expected,key)) {
            s_sample_rips[(slot+i)%1024]=rip; s_sample_callers[(slot+i)%1024]=caller;
            atomic_fetch_add(&s_c_slot_samples[(slot+i)%1024],1);
            return;
        }
    }
}
static void *sampler_main(void *p_arg) {
    const char *sz_name=(const char *)p_arg;
    pid_t tid=0;
    while (!tid) {
        DIR *p_dir=opendir("/proc/self/task");
        struct dirent *p_entry;
        while (p_dir && (p_entry=readdir(p_dir))) {
            char sz_path[300], sz_comm[32]={0};
            snprintf(sz_path,sizeof(sz_path),"/proc/self/task/%s/comm",p_entry->d_name);
            FILE *p_file=fopen(sz_path,"r");
            if (!p_file) continue;
            if (fgets(sz_comm,sizeof(sz_comm),p_file) && !strncmp(sz_comm,sz_name,strlen(sz_name))) tid=(pid_t)atoi(p_entry->d_name);
            fclose(p_file);
        }
        if (p_dir) closedir(p_dir);
        if (!tid) sleep(1);
    }
    printf("Runtime: sampling thread %s (%d)\n",sz_name,(int)tid);
    for (;;) {
        if (syscall(SYS_tgkill,getpid(),tid,SIGPROF)) break;
        struct timespec t={0,500000};
        nanosleep(&t,NULL);
    }
    return NULL;
}
static void sample_start(void) {
    static int s_has_started;
    const char *sz_name=getenv("PS_SAMPLE_THREAD");
    if (s_has_started || !sz_name || !*sz_name) return;
    s_has_started=1;
    struct sigaction action={0};
    action.sa_sigaction=sample_handler;
    action.sa_flags=SA_SIGINFO|SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF,&action,NULL);
    pthread_t thread;
    pthread_create(&thread,NULL,sampler_main,(void *)sz_name);
    pthread_detach(thread);
}
static void sample_report(void) {
    static int s_got_dumped;
    if (!s_got_dumped) {
        s_got_dumped=1;
        const char *sz_got=getenv("PS_DUMP_GOT"); /* a guest GOT slot (offset): which host function it calls */
        if (sz_got && *sz_got) {
            const uint64_t target=*(const uint64_t *)(0x800000000ull+strtoull(sz_got,NULL,0));
            Dl_info dl={0};
            dladdr((void *)target,&dl);
            printf("Runtime: GOT %s -> %#lx %s:%s+%#lx\n",sz_got,(unsigned long)target,dl.dli_fname ? dl.dli_fname : "?",
                   dl.dli_sname ? dl.dli_sname : "?",(unsigned long)(target-(uint64_t)(dl.dli_sname ? dl.dli_saddr : dl.dli_fbase)));
        }
    }
    const uint64_t c_samples=atomic_exchange(&s_c_samples,0);
    if (!c_samples) return;
    struct { uint64_t rip, caller, c_samples; } rows[1024]; int c_rows=0;
    for (int i=0;i<1024;++i) { uint64_t c=atomic_exchange(&s_c_slot_samples[i],0); if (c) { rows[c_rows].rip=s_sample_rips[i]; rows[c_rows].caller=s_sample_callers[i]; rows[c_rows].c_samples=c; ++c_rows; } }
    for (int i=1;i<c_rows;++i) for (int j=i;j>0 && rows[j].c_samples>rows[j-1].c_samples;--j) { __typeof__(rows[0]) t=rows[j]; rows[j]=rows[j-1]; rows[j-1]=t; }
    printf("Thread samples (%llu):",(unsigned long long)c_samples);
    for (int i=0;i<c_rows && i<16;++i) {
        const uint64_t text_lo=0x800000000ull, text_hi=0x800000000ull+0x50d96dcull;
        if (rows[i].rip>=text_lo && rows[i].rip<text_hi) {
            printf("%s +%#lx %.1f%%",i?";":"",(unsigned long)(rows[i].rip-text_lo),100.0*rows[i].c_samples/c_samples);
            continue;
        }
        Dl_info dl={0};
        dladdr((void *)rows[i].rip,&dl);
        const char *sz_module=dl.dli_fname ? strrchr(dl.dli_fname,'/') : NULL;
        printf("%s host:%s%s%s+%#lx (from +%#lx) %.1f%%",i?";":"",sz_module ? sz_module+1 : "?",dl.dli_sname ? ":" : "",
               dl.dli_sname ? dl.dli_sname : "",(unsigned long)(rows[i].rip-(dl.dli_sname ? (uint64_t)dl.dli_saddr : (uint64_t)dl.dli_fbase)),
               (unsigned long)rows[i].caller,100.0*rows[i].c_samples/c_samples);
    }
    printf("\n");
}
/* The first three return addresses into the game's code on the calling thread's stack. */
void runtime_guest_call_sites(uint64_t out[3]) { guest_call_sites(out); }
