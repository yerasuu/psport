/* The host platform layer (decision #540): what psport asks of the operating system, behind one
 * small C ABI with an implementation per OS in src/platform/<os>/, so that the runtime and the
 * loader name what they need instead of a Linux call. Included from C and C++ (libpsport-gpu.so
 * reaches these through -rdynamic). Only Linux is implemented; host_features.inc says what each
 * OS has. */
#ifndef PS_HOST_H
#define PS_HOST_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- the capability table (host_features.inc) ---- */
typedef enum {
    PS_HOST_FEATURE_STATE_IMPLEMENTED,
    PS_HOST_FEATURE_STATE_EXPERIMENTAL,
    PS_HOST_FEATURE_STATE_UNAVAILABLE,
} PsHostFeatureState;

typedef uint16_t PsHostFeatureId;
enum {
#define FEATURE(id, area, sz_desc) PS_HOST_FEATURE_##id,
#define FEATURE_HOST(id, os, state, sz_reason, sz_ref)
#define FEATURE_PROBED(id)
#include "host_features.inc"
#undef FEATURE
#undef FEATURE_HOST
#undef FEATURE_PROBED
    PS_HOST_FEATURE_COUNT
};

/* psport --list-features: every entry as JSON on stdout, with this host's state. */
void ps_host_features_list(void);
/* Once at launch, before the game's code runs: the "Host features:" line and the exit summary. */
void ps_host_features_log(void);
/* A FEATURE_PROBED entry's run-time answer, from the code that probes it (the GPU library once the
 * Vulkan device is known): logged and kept for the exit summary. sz_result must outlive the
 * process (a string literal); later answers for the same feature are ignored and said. */
void ps_host_feature_probed(PsHostFeatureId id, int is_available, const char *sz_result);
/* The exit summary line, rebuilt at each publication so that the call report can print it from the
 * fault and watchdog handlers. */
const char *ps_host_features_summary(void);

/* ---- time ---- */
typedef enum {
    PS_HOST_CLOCK_MONOTONIC,
    PS_HOST_CLOCK_REALTIME,
    /* Realtime at the scheduler tick's resolution, cheaper to read. */
    PS_HOST_CLOCK_REALTIME_COARSE,
    PS_HOST_CLOCK_THREAD_CPU,
    PS_HOST_CLOCK_PROCESS_CPU,
} PsHostClock;

/* 0, or the host's errno. */
int ps_host_time_read(PsHostClock clock, struct timespec *p_out_time);
int ps_host_time_resolution(PsHostClock clock, struct timespec *p_out_time);
/* The monotonic and realtime clocks in nanoseconds, for callers that cannot fail: both clocks
 * always read on every host psport supports. */
uint64_t ps_host_time_monotonic_ns(void);
uint64_t ps_host_time_realtime_ns(void);
/* One sleep on the monotonic clock: 0 once it elapsed, EINTR when a signal cut it short (with what
 * was left in *p_out_remaining_ns when given), another errno on failure. Safe in signal handlers. */
int ps_host_time_sleep_ns(uint64_t ns, uint64_t *p_out_remaining_ns);
/* Sleeps until the monotonic clock reaches deadline_ns: 0, EINTR or another errno. */
int ps_host_time_sleep_until_ns(uint64_t deadline_ns);

/* ---- randomness ---- */
/* Fills the buffer from the host's cryptographic source: 0, or the host's errno. */
int ps_host_random_bytes(void *p_buffer, size_t cb_buffer);

/* ---- paths ---- */
/* The running executable's absolute path, NUL-terminated: its length, or -1 when the host cannot
 * say or it does not fit in cb_out. */
long ps_host_exe_path(char *sz_out, size_t cb_out);

#ifdef __cplusplus
}
#endif
#endif
