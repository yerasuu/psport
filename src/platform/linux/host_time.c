/* Linux: clocks and sleeping (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>

static clockid_t host_clock(PsHostClock clock) {
    switch (clock) {
    case PS_HOST_CLOCK_MONOTONIC: return CLOCK_MONOTONIC;
    case PS_HOST_CLOCK_REALTIME: return CLOCK_REALTIME;
    case PS_HOST_CLOCK_REALTIME_COARSE: return CLOCK_REALTIME_COARSE;
    case PS_HOST_CLOCK_THREAD_CPU: return CLOCK_THREAD_CPUTIME_ID;
    case PS_HOST_CLOCK_PROCESS_CPU: return CLOCK_PROCESS_CPUTIME_ID;
    }
    return -1;
}

int ps_host_time_read(PsHostClock clock, struct timespec *p_out_time) {
    return clock_gettime(host_clock(clock), p_out_time) ? errno : 0;
}

int ps_host_time_resolution(PsHostClock clock, struct timespec *p_out_time) {
    return clock_getres(host_clock(clock), p_out_time) ? errno : 0;
}

static uint64_t nanoseconds(clockid_t clock) {
    struct timespec time;
    clock_gettime(clock, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

uint64_t ps_host_time_monotonic_ns(void) { return nanoseconds(CLOCK_MONOTONIC); }
uint64_t ps_host_time_realtime_ns(void) { return nanoseconds(CLOCK_REALTIME); }

int ps_host_time_sleep_ns(uint64_t ns, uint64_t *p_out_remaining_ns) {
    struct timespec time = {.tv_sec = (time_t)(ns / 1000000000u), .tv_nsec = (long)(ns % 1000000000u)};
    if (!nanosleep(&time, &time)) return 0;
    const int error = errno;
    if (error == EINTR && p_out_remaining_ns)
        *p_out_remaining_ns = (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
    return error;
}

int ps_host_time_sleep_until_ns(uint64_t deadline_ns) {
    const struct timespec time = {.tv_sec = (time_t)(deadline_ns / 1000000000u), .tv_nsec = (long)(deadline_ns % 1000000000u)};
    return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL);
}
