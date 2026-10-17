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

unsigned ps_host_thread_cpu_count(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    return sched_getaffinity(0, sizeof(set), &set) ? 0u : (unsigned)CPU_COUNT(&set);
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
