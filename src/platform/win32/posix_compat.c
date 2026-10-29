// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: the C library gaps posix_compat.h declares. */
#include "posix_compat.h"
#include "ps_host.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *p_attr, int kind) {
    static atomic_int s_has_said;
    if (!p_attr) return EINVAL;
    if (!atomic_exchange(&s_has_said, 1))
        printf("Host: rwlock preference %d kept without effect: winpthreads has a single policy\n", kind);
    return 0;
}

/* A deadline on the given clock as the same instant on CLOCK_REALTIME: 0, or EINVAL. */
static int realtime_deadline(clockid_t clock, const struct timespec *p_deadline, struct timespec *p_out) {
    if (clock == CLOCK_REALTIME) {
        *p_out = *p_deadline;
        return 0;
    }
    if (clock != CLOCK_MONOTONIC) return EINVAL;
    struct timespec now_monotonic;
    const int error = ps_host_time_read(PS_HOST_CLOCK_MONOTONIC, &now_monotonic);
    if (error) return error;
    const int64_t ns_left = ((int64_t)p_deadline->tv_sec - (int64_t)now_monotonic.tv_sec) * 1000000000 +
                            (p_deadline->tv_nsec - now_monotonic.tv_nsec);
    const uint64_t ns_at = ps_host_time_realtime_ns() + (uint64_t)(ns_left > 0 ? ns_left : 0);
    *p_out = (struct timespec){(time_t)(ns_at / 1000000000u), (long)(ns_at % 1000000000u)};
    return 0;
}

int pthread_mutex_clocklock(pthread_mutex_t *p_mutex, clockid_t clock, const struct timespec *p_deadline) {
    struct timespec deadline;
    const int error = realtime_deadline(clock, p_deadline, &deadline);
    return error ? error : pthread_mutex_timedlock(p_mutex, &deadline);
}

int pthread_rwlock_clockrdlock(pthread_rwlock_t *p_rwlock, clockid_t clock, const struct timespec *p_deadline) {
    struct timespec deadline;
    const int error = realtime_deadline(clock, p_deadline, &deadline);
    return error ? error : pthread_rwlock_timedrdlock(p_rwlock, &deadline);
}

int pthread_rwlock_clockwrlock(pthread_rwlock_t *p_rwlock, clockid_t clock, const struct timespec *p_deadline) {
    struct timespec deadline;
    const int error = realtime_deadline(clock, p_deadline, &deadline);
    return error ? error : pthread_rwlock_timedwrlock(p_rwlock, &deadline);
}

int pthread_tryjoin_np(pthread_t thread, void **pp_result) { return _pthread_tryjoin(thread, pp_result); }

int pthread_cond_clockwait(pthread_cond_t *p_cond, pthread_mutex_t *p_mutex, clockid_t clock,
                           const struct timespec *p_deadline) {
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return EINVAL;
    struct timespec now;
    const int error = ps_host_time_read(clock == CLOCK_MONOTONIC ? PS_HOST_CLOCK_MONOTONIC : PS_HOST_CLOCK_REALTIME, &now);
    if (error) return error;
    int64_t ns_left = ((int64_t)p_deadline->tv_sec - (int64_t)now.tv_sec) * 1000000000 + (p_deadline->tv_nsec - now.tv_nsec);
    if (ns_left < 0) ns_left = 0;
    const struct timespec relative = {(time_t)(ns_left / 1000000000), (long)(ns_left % 1000000000)};
    return pthread_cond_timedwait_relative_np(p_cond, p_mutex, &relative);
}

void *memmem(const void *p_haystack, size_t cb_haystack, const void *p_needle, size_t cb_needle) {
    if (!cb_needle) return (void *)p_haystack;
    const unsigned char *p_cursor = p_haystack, *p_end = p_cursor + cb_haystack;
    while (cb_needle <= (size_t)(p_end - p_cursor)) {
        const unsigned char *p_first = memchr(p_cursor, *(const unsigned char *)p_needle, (size_t)(p_end - p_cursor) - cb_needle + 1);
        if (!p_first) return NULL;
        if (!memcmp(p_first, p_needle, cb_needle)) return (void *)p_first;
        p_cursor = p_first + 1;
    }
    return NULL;
}

char *strcasestr(const char *sz_haystack, const char *sz_needle) {
    const size_t cb_needle = strlen(sz_needle);
    for (; *sz_haystack; ++sz_haystack)
        if (!_strnicmp(sz_haystack, sz_needle, cb_needle)) return (char *)sz_haystack;
    return cb_needle ? NULL : (char *)sz_haystack;
}

ssize_t getline(char **psz_line, size_t *p_cb_line, FILE *p_stream) {
    if (!psz_line || !p_cb_line || !p_stream) {
        errno = EINVAL;
        return -1;
    }
    size_t cb_used = 0;
    for (int c; (c = fgetc(p_stream)) != EOF;) {
        if (cb_used + 2 > *p_cb_line) {
            const size_t cb_grown = *p_cb_line ? *p_cb_line * 2 : 128;
            char *sz_grown = realloc(*psz_line, cb_grown);
            if (!sz_grown) {
                errno = ENOMEM;
                return -1;
            }
            *psz_line = sz_grown;
            *p_cb_line = cb_grown;
        }
        (*psz_line)[cb_used++] = (char)c;
        if (c == '\n') break;
    }
    if (!cb_used) return -1;
    (*psz_line)[cb_used] = 0;
    return (ssize_t)cb_used;
}
