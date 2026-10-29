// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux: thread ids, names, stacks and scheduling, address waits and the guest's thread pointer
 * (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <asm/prctl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

int32_t ps_host_thread_id(void) { return (int32_t)syscall(SYS_gettid); }

void ps_host_thread_yield(void) { sched_yield(); }

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

/* pthread_setname_np refuses names past 15 characters instead of shortening them. */
void ps_host_thread_set_name(const char *sz_name) {
    char sz_host[16] = {0};
    memcpy(sz_host, sz_name, strnlen(sz_name, sizeof(sz_host) - 1));
    pthread_setname_np(pthread_self(), sz_host);
}

int ps_host_thread_name(char *sz_out, size_t cb_out) { return pthread_getname_np(pthread_self(), sz_out, cb_out); }

int32_t ps_host_thread_find(const char *sz_prefix) {
    DIR *p_tasks = opendir("/proc/self/task");
    int32_t tid = 0;
    for (struct dirent *p_entry; !tid && p_tasks && (p_entry = readdir(p_tasks));) {
        char sz_path[300], sz_comm[32] = {0};
        snprintf(sz_path, sizeof(sz_path), "/proc/self/task/%s/comm", p_entry->d_name);
        FILE *p_file = fopen(sz_path, "r");
        if (!p_file) continue;
        if (fgets(sz_comm, sizeof(sz_comm), p_file) && !strncmp(sz_comm, sz_prefix, strlen(sz_prefix)))
            tid = (int32_t)atoi(p_entry->d_name);
        fclose(p_file);
    }
    if (p_tasks) closedir(p_tasks);
    return tid;
}

int ps_host_thread_stack(uintptr_t *p_out_va_low, size_t *p_out_cb_stack) {
    pthread_attr_t self;
    int error = pthread_getattr_np(pthread_self(), &self);
    if (error) return error;
    void *p_low = NULL;
    size_t cb_stack = 0;
    error = pthread_attr_getstack(&self, &p_low, &cb_stack);
    pthread_attr_destroy(&self);
    if (error) return error;
    *p_out_va_low = (uintptr_t)p_low;
    *p_out_cb_stack = cb_stack;
    return 0;
}

int ps_host_thread_cpu_times(int is_whole_process, struct timespec *p_out_user, struct timespec *p_out_system) {
    struct rusage usage;
    if (getrusage(is_whole_process ? RUSAGE_SELF : RUSAGE_THREAD, &usage)) return errno;
    *p_out_user = (struct timespec){usage.ru_utime.tv_sec, usage.ru_utime.tv_usec * 1000};
    *p_out_system = (struct timespec){usage.ru_stime.tv_sec, usage.ru_stime.tv_usec * 1000};
    return 0;
}

/* cgroup v2 CPU quota of the process's own cgroup: ceil(quota / period), 0 when unlimited or unreadable. */
static unsigned cgroup_cpu_quota(void) {
    char sz_path[PATH_MAX], sz_line[256];
    char sz_group[PATH_MAX - 64] = "";
    FILE *p_file = fopen("/proc/self/cgroup", "r");
    if (!p_file) return 0;
    while (fgets(sz_line, sizeof sz_line, p_file)) {
        if (strncmp(sz_line, "0::", 3) == 0) {
            sz_line[strcspn(sz_line, "\n")] = '\0';
            snprintf(sz_group, sizeof sz_group, "%s", sz_line + 3);
            break;
        }
    }
    fclose(p_file);
    snprintf(sz_path, sizeof sz_path, "/sys/fs/cgroup%s/cpu.max", sz_group);
    p_file = fopen(sz_path, "r");
    if (!p_file) return 0;
    unsigned long long cb_quota = 0, cb_period = 0;
    const int c_fields = fscanf(p_file, "%llu %llu", &cb_quota, &cb_period);
    fclose(p_file);
    if (c_fields != 2 || !cb_quota || !cb_period) return 0;
    const unsigned long long c_cpus = (cb_quota + cb_period - 1) / cb_period;
    return c_cpus > UINT_MAX ? UINT_MAX : (unsigned)c_cpus;
}

unsigned ps_host_thread_cpu_count(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set)) return 0u;
    unsigned c_cpus = (unsigned)CPU_COUNT(&set);
    const unsigned c_quota = cgroup_cpu_quota();
    if (c_quota && c_quota < c_cpus) c_cpus = c_quota;
    return c_cpus;
}

int ps_host_thread_make_background(void) {
    const struct sched_param param = {0};
    if (!sched_setscheduler(0, SCHED_IDLE, &param)) return 0;
    return setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19) ? errno : 0;
}

typedef struct {
    void (*pfn_thread)(int32_t tid, void *p_context);
    void *p_context;
} ThreadWalk;

static void visit_task(const char *sz_tid, void *p_walk) {
    const ThreadWalk *p = p_walk;
    const int32_t tid = (int32_t)strtol(sz_tid, NULL, 10);
    if (tid > 0) p->pfn_thread(tid, p->p_context);
}

int ps_host_thread_each(void (*pfn_thread)(int32_t tid, void *p_context), void *p_context) {
    ThreadWalk walk = {pfn_thread, p_context};
    return ps_host_list_dir("/proc/self/task", visit_task, &walk);
}

int ps_host_tls_set_base(void *p_base) {
    return syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)p_base) ? errno : 0;
}

int ps_host_tls_base(uintptr_t *p_out_va_base) {
    unsigned long va_base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &va_base)) return errno;
    *p_out_va_base = (uintptr_t)va_base;
    return 0;
}

int ps_host_tls_init(void) { return 0; }

int32_t ps_host_tls_tcb_offset(void) { return 0; }
