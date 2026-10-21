/* Windows: clocks and sleeping (ps_host.h). */
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>
#include <stdio.h>

#define NS_PER_SECOND 1000000000u
#define NS_PER_FILETIME_TICK 100u
/* 1601-01-01, FILETIME's epoch, to 1970-01-01 in 100 ns ticks. */
#define FILETIME_UNIX_EPOCH 116444736000000000ull
#define FILETIME_TICKS_PER_SECOND 10000000

static uint64_t filetime_ticks(FILETIME time) {
    return ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

static uint64_t counter_frequency(void) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)frequency.QuadPart;
}

uint64_t ps_host_time_monotonic_ns(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = (uint64_t)counter.QuadPart, frequency = counter_frequency();
    return ticks / frequency * NS_PER_SECOND + ticks % frequency * NS_PER_SECOND / frequency;
}

uint64_t ps_host_time_realtime_ns(void) {
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    return (filetime_ticks(now) - FILETIME_UNIX_EPOCH) * NS_PER_FILETIME_TICK;
}

static void split_ns(uint64_t ns, struct timespec *p_out_time) {
    p_out_time->tv_sec = (time_t)(ns / NS_PER_SECOND);
    p_out_time->tv_nsec = (long)(ns % NS_PER_SECOND);
}

static int cpu_time_ns(PsHostClock clock, uint64_t *p_out_ns) {
    FILETIME created, exited, kernel, user;
    const BOOL is_read = clock == PS_HOST_CLOCK_THREAD_CPU
                             ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
                             : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    if (!is_read) return ps_win32_errno(GetLastError());
    *p_out_ns = (filetime_ticks(kernel) + filetime_ticks(user)) * NS_PER_FILETIME_TICK;
    return 0;
}

int ps_host_time_read(PsHostClock clock, struct timespec *p_out_time) {
    uint64_t ns = 0;
    switch (clock) {
    case PS_HOST_CLOCK_MONOTONIC: ns = ps_host_time_monotonic_ns(); break;
    case PS_HOST_CLOCK_REALTIME: ns = ps_host_time_realtime_ns(); break;
    case PS_HOST_CLOCK_REALTIME_COARSE: {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ns = (filetime_ticks(now) - FILETIME_UNIX_EPOCH) * NS_PER_FILETIME_TICK;
        break;
    }
    case PS_HOST_CLOCK_THREAD_CPU:
    case PS_HOST_CLOCK_PROCESS_CPU: {
        const int error = cpu_time_ns(clock, &ns);
        if (error) return error;
        break;
    }
    default: return EINVAL;
    }
    split_ns(ns, p_out_time);
    return 0;
}

/* The coarse clock and the CPU times advance once per scheduler tick. */
static uint64_t tick_ns(void) {
    DWORD adjustment = 0, increment = 0;
    BOOL is_adjustment_disabled = TRUE;
    if (!GetSystemTimeAdjustment(&adjustment, &increment, &is_adjustment_disabled) || !increment) return 15625000u;
    return (uint64_t)increment * NS_PER_FILETIME_TICK;
}

int ps_host_time_resolution(PsHostClock clock, struct timespec *p_out_time) {
    uint64_t ns = 0;
    switch (clock) {
    case PS_HOST_CLOCK_MONOTONIC: {
        const uint64_t frequency = counter_frequency();
        ns = frequency >= NS_PER_SECOND ? 1 : (NS_PER_SECOND + frequency - 1) / frequency;
        break;
    }
    case PS_HOST_CLOCK_REALTIME: ns = NS_PER_FILETIME_TICK; break;
    case PS_HOST_CLOCK_REALTIME_COARSE:
    case PS_HOST_CLOCK_THREAD_CPU:
    case PS_HOST_CLOCK_PROCESS_CPU: ns = tick_ns(); break;
    default: return EINVAL;
    }
    split_ns(ns, p_out_time);
    return 0;
}

/* One high-resolution waitable timer per thread, closed when the thread ends: Sleep rounds to the
 * scheduler tick (15.6 ms unless someone raised the timer resolution), too coarse for frame pacing. */
static DWORD s_i_timer_slot = FLS_OUT_OF_INDEXES;
static INIT_ONCE s_timer_slot_once = INIT_ONCE_STATIC_INIT;

static void WINAPI close_timer(void *p_timer) {
    if (p_timer) CloseHandle(p_timer);
}

static BOOL CALLBACK allocate_timer_slot(INIT_ONCE *p_once, void *p_parameter, void **pp_context) {
    (void)p_once, (void)p_parameter, (void)pp_context;
    s_i_timer_slot = FlsAlloc(close_timer);
    return TRUE;
}

static HANDLE thread_timer(void) {
    InitOnceExecuteOnce(&s_timer_slot_once, allocate_timer_slot, NULL, NULL);
    if (s_i_timer_slot == FLS_OUT_OF_INDEXES) return NULL;
    HANDLE h_timer = FlsGetValue(s_i_timer_slot);
    if (h_timer) return h_timer;
    h_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (h_timer && !FlsSetValue(s_i_timer_slot, h_timer)) {
        CloseHandle(h_timer);
        return NULL;
    }
    return h_timer;
}

/* Windows has no signals to cut a sleep short: it always runs to its end. */
int ps_host_time_sleep_ns(uint64_t ns, uint64_t *p_out_remaining_ns) {
    (void)p_out_remaining_ns;
    if (!ns) {
        Sleep(0);
        return 0;
    }
    HANDLE h_timer = thread_timer();
    if (!h_timer) {
        /* Windows 10 before 1803 has no high-resolution timers: millisecond sleeps, rounded up. */
        const uint64_t ms = (ns + 999999u) / 1000000u;
        Sleep(ms >= INFINITE ? INFINITE - 1 : (DWORD)ms);
        return 0;
    }
    const uint64_t ticks = (ns + NS_PER_FILETIME_TICK - 1) / NS_PER_FILETIME_TICK;
    LARGE_INTEGER due = {.QuadPart = ticks > (uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)ticks};
    if (!SetWaitableTimer(h_timer, &due, 0, NULL, NULL, FALSE)) return ps_win32_errno(GetLastError());
    return WaitForSingleObject(h_timer, INFINITE) == WAIT_OBJECT_0 ? 0 : ps_win32_errno(GetLastError());
}

int ps_host_time_sleep_until_ns(uint64_t deadline_ns) {
    const uint64_t now_ns = ps_host_time_monotonic_ns();
    return deadline_ns > now_ns ? ps_host_time_sleep_ns(deadline_ns - now_ns, NULL) : 0;
}

/* Biases are minutes west of UTC, the daylight one counted from the standard time's; the year's
 * rule converts the instant, so daylight saving is whatever its offset carries beyond the standard
 * bias. */
int ps_host_time_zone_at(int64_t utc_sec, int32_t *p_out_standard_sec, int32_t *p_out_dst_sec) {
    const int64_t off_epoch_sec = (int64_t)(FILETIME_UNIX_EPOCH / FILETIME_TICKS_PER_SECOND);
    /* FILETIME starts in 1601, but guest ticks start in year 1 and a zeroed one is common (an empty
       save slot): earlier instants take the rule of 1601-01-02, a day in so that local time stays
       within 1601 in every zone. */
    const int64_t first_utc_sec = -off_epoch_sec + 86400;
    if (utc_sec < first_utc_sec) {
        static LONG s_is_logged;
        if (!InterlockedExchange(&s_is_logged, 1))
            fprintf(stderr, "Runtime: time zone asked for a time before 1601 (%lld s from 1970), using 1601's rule\n",
                    (long long)utc_sec);
        utc_sec = first_utc_sec;
    }
    if (utc_sec > INT64_MAX / FILETIME_TICKS_PER_SECOND - off_epoch_sec) return EOVERFLOW;
    const uint64_t ticks_utc = (uint64_t)(utc_sec + off_epoch_sec) * FILETIME_TICKS_PER_SECOND;
    const FILETIME file_utc = {.dwLowDateTime = (DWORD)ticks_utc, .dwHighDateTime = (DWORD)(ticks_utc >> 32)};
    DYNAMIC_TIME_ZONE_INFORMATION dynamic;
    SYSTEMTIME system_utc, system_local;
    TIME_ZONE_INFORMATION zone;
    FILETIME file_local;
    if (GetDynamicTimeZoneInformation(&dynamic) == TIME_ZONE_ID_INVALID || !FileTimeToSystemTime(&file_utc, &system_utc) ||
        !GetTimeZoneInformationForYear(system_utc.wYear, &dynamic, &zone) ||
        !SystemTimeToTzSpecificLocalTimeEx(&dynamic, &system_utc, &system_local) ||
        !SystemTimeToFileTime(&system_local, &file_local))
        return ps_win32_errno(GetLastError());
    const int64_t offset_sec = ((int64_t)filetime_ticks(file_local) - (int64_t)ticks_utc) / FILETIME_TICKS_PER_SECOND;
    *p_out_standard_sec = -(int32_t)(zone.Bias + zone.StandardBias) * 60;
    *p_out_dst_sec = (int32_t)(offset_sec - *p_out_standard_sec);
    return 0;
}
