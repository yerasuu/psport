// SPDX-License-Identifier: GPL-2.0-or-later
/* The glibc and POSIX calls psport's generic code uses that MinGW's C runtime (UCRT with winpthreads)
 * lacks, force-included into every runtime source by the Windows build (-include), so that the
 * generic code keeps one spelling for both. Only C library gaps belong here; what is the operating
 * system's own goes through ps_host.h. */
#ifndef PS_POSIX_COMPAT_H
#define PS_POSIX_COMPAT_H
/* localtime_r and gmtime_r, which time.h defines inline only then. */
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
enum {
    PTHREAD_RWLOCK_PREFER_READER_NP,
    PTHREAD_RWLOCK_PREFER_WRITER_NP,
    PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP,
};
/* winpthreads' rwlocks have one policy: the kind is accepted and said once. */
int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *p_attr, int kind);
/* CLOCK_MONOTONIC and CLOCK_REALTIME deadlines, as a relative wait from the clock's present. */
int pthread_cond_clockwait(pthread_cond_t *p_cond, pthread_mutex_t *p_mutex, clockid_t clock,
                           const struct timespec *p_deadline);
/* The same deadlines for the timed locks, which winpthreads takes on CLOCK_REALTIME only. */
int pthread_mutex_clocklock(pthread_mutex_t *p_mutex, clockid_t clock, const struct timespec *p_deadline);
int pthread_rwlock_clockrdlock(pthread_rwlock_t *p_rwlock, clockid_t clock, const struct timespec *p_deadline);
int pthread_rwlock_clockwrlock(pthread_rwlock_t *p_rwlock, clockid_t clock, const struct timespec *p_deadline);
/* glibc's name for winpthreads' _pthread_tryjoin: EBUSY while the thread runs. */
int pthread_tryjoin_np(pthread_t thread, void **pp_result);

void *memmem(const void *p_haystack, size_t cb_haystack, const void *p_needle, size_t cb_needle);
char *strcasestr(const char *sz_haystack, const char *sz_needle);
ssize_t getline(char **psz_line, size_t *p_cb_line, FILE *p_stream);
#define timegm _mkgmtime

#endif
