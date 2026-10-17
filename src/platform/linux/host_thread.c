/* Linux: thread ids, thread signals and address waits (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

int32_t ps_host_thread_id(void) { return (int32_t)syscall(SYS_gettid); }

int ps_host_thread_signal(int32_t tid, int signal) {
    return syscall(SYS_tgkill, getpid(), (pid_t)tid, signal) ? errno : 0;
}

int ps_host_wait_on_address(const void *p_word, uint32_t expected, int is_private, PsHostClock clock,
                            const struct timespec *p_deadline) {
    if (clock != PS_HOST_CLOCK_MONOTONIC && clock != PS_HOST_CLOCK_REALTIME) return EINVAL;
    const int op = FUTEX_WAIT_BITSET | (is_private ? FUTEX_PRIVATE_FLAG : 0) |
                   (clock == PS_HOST_CLOCK_REALTIME ? FUTEX_CLOCK_REALTIME : 0);
    return syscall(SYS_futex, p_word, op, expected, p_deadline, NULL, FUTEX_BITSET_MATCH_ANY) ? errno : 0;
}

int ps_host_wake_address(const void *p_word, int c_waiters, int is_private) {
    const long c_woken = syscall(SYS_futex, p_word, FUTEX_WAKE | (is_private ? FUTEX_PRIVATE_FLAG : 0), c_waiters, NULL,
                                 NULL, 0);
    return c_woken < 0 ? -errno : (int)c_woken;
}
