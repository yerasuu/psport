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

int ps_host_time_zone_at(int64_t utc_sec, int32_t *p_out_standard_sec, int32_t *p_out_dst_sec) {
    tzset();
    const time_t at = (time_t)utc_sec;
    struct tm local;
    if (!localtime_r(&at, &local)) return errno;
    /* As the firmware's tzdata table: daylight saving is what the offset carries beyond the last
       standard period's, so that it may be negative (Europe/Dublin's winter) and the sum stays the
       offset in force. Week steps since only that period's offset matters, not where it ends. */
    long standard_sec = local.tm_gmtoff;
    if (local.tm_isdst > 0) {
        standard_sec = -timezone;
        for (int week = 1; week <= 60; ++week) {
            const time_t probe = at - (time_t)week * 7 * 86400;
            struct tm prior;
            if (localtime_r(&probe, &prior) && prior.tm_isdst <= 0) {
                standard_sec = prior.tm_gmtoff;
                break;
            }
        }
    }
    *p_out_standard_sec = (int32_t)standard_sec;
    *p_out_dst_sec = (int32_t)(local.tm_gmtoff - standard_sec);
    return 0;
}

int ps_host_time_sleep_until_ns(uint64_t deadline_ns) {
    const struct timespec time = {.tv_sec = (time_t)(deadline_ns / 1000000000u), .tv_nsec = (long)(deadline_ns % 1000000000u)};
    return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL);
}

int ps_host_time_local_fields(int64_t utc_sec, struct tm *p_out) {
    const time_t at = (time_t)utc_sec;
    tzset();
    return localtime_r(&at, p_out) ? 0 : errno;
}

int ps_host_time_utc_fields(int64_t utc_sec, struct tm *p_out) {
    const time_t at = (time_t)utc_sec;
    return gmtime_r(&at, p_out) ? 0 : errno;
}

int ps_host_time_utc_seconds(const struct tm *p_fields, int64_t *p_out_utc_sec) {
    struct tm fields = *p_fields;
    errno = 0;
    const time_t at = timegm(&fields);
    if (at == (time_t)-1 && errno) return errno;
    *p_out_utc_sec = (int64_t)at;
    return 0;
}
