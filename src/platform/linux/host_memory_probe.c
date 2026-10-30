// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux: the host's memory state, /proc/meminfo, the process's cgroup v2 limits and PSI (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* A cgroup file holding one number or "max": 0 when read, with UINT64_MAX for "max". */
static int read_cgroup_value(const char *sz_group_dir, const char *sz_file, uint64_t *p_out_value) {
    char sz_path[PATH_MAX], sz_value[32] = "";
    if (snprintf(sz_path, sizeof sz_path, "%s/%s", sz_group_dir, sz_file) >= (int)sizeof sz_path) return ENAMETOOLONG;
    FILE *p_file = fopen(sz_path, "r");
    if (!p_file) return errno;
    const int is_read = fgets(sz_value, sizeof sz_value, p_file) != NULL;
    fclose(p_file);
    if (!is_read) return EIO;
    if (!strncmp(sz_value, "max", 3)) {
        *p_out_value = UINT64_MAX;
        return 0;
    }
    unsigned long long value = 0;
    if (sscanf(sz_value, "%llu", &value) != 1) return EIO;
    *p_out_value = value;
    return 0;
}

/* Page cache the cgroup can drop before it reaches its limit. Active file pages are only half
 * counted, as MemAvailable does, since part of them is the running programs' own code. */
static uint64_t cgroup_reclaimable(const char *sz_group_dir) {
    char sz_path[PATH_MAX], sz_line[128];
    if (snprintf(sz_path, sizeof sz_path, "%s/memory.stat", sz_group_dir) >= (int)sizeof sz_path) return 0;
    FILE *p_file = fopen(sz_path, "r");
    if (!p_file) return 0;
    unsigned long long cb_inactive = 0, cb_active = 0;
    while (fgets(sz_line, sizeof sz_line, p_file)) {
        sscanf(sz_line, "inactive_file %llu", &cb_inactive);
        sscanf(sz_line, "active_file %llu", &cb_active);
    }
    fclose(p_file);
    return cb_inactive + cb_active / 2;
}

/* Walks the process's own cgroup and its ancestors (the same own-group lookup as host_thread.c's
 * cgroup_cpu_quota), since a limit set on a parent slice binds as much as one on the group. */
static void read_cgroup_limit(PsHostMemory *p_memory, char *sz_group_dir, size_t cb_group_dir) {
    char sz_line[PATH_MAX];
    sz_group_dir[0] = '\0';
    FILE *p_file = fopen("/proc/self/cgroup", "r");
    if (!p_file) return;
    while (fgets(sz_line, sizeof sz_line, p_file)) {
        if (!strncmp(sz_line, "0::", 3)) {
            sz_line[strcspn(sz_line, "\n")] = '\0';
            if (snprintf(sz_group_dir, cb_group_dir, "/sys/fs/cgroup%s", sz_line + 3) >= (int)cb_group_dir)
                sz_group_dir[0] = '\0';
            break;
        }
    }
    fclose(p_file);
    if (!sz_group_dir[0]) return;
    char sz_dir[PATH_MAX];
    snprintf(sz_dir, sizeof sz_dir, "%s", sz_group_dir);
    uint64_t cb_best_headroom = UINT64_MAX;
    for (;;) {
        uint64_t cb_max = UINT64_MAX, cb_high = UINT64_MAX, cb_current = 0;
        const int has_max = !read_cgroup_value(sz_dir, "memory.max", &cb_max);
        const int has_high = !read_cgroup_value(sz_dir, "memory.high", &cb_high);
        const uint64_t cb_limit = cb_high < cb_max ? cb_high : cb_max;
        if ((has_max || has_high) && cb_limit != UINT64_MAX && !read_cgroup_value(sz_dir, "memory.current", &cb_current)) {
            const uint64_t cb_reclaimable = cgroup_reclaimable(sz_dir);
            const uint64_t cb_used = cb_current > cb_reclaimable ? cb_current - cb_reclaimable : 0;
            const uint64_t cb_headroom = cb_limit > cb_used ? cb_limit - cb_used : 0;
            if (cb_headroom < cb_best_headroom) {
                cb_best_headroom = cb_headroom;
                p_memory->cb_limit = cb_limit;
                p_memory->cb_limit_used = cb_used;
                char *sz_source = p_memory->sz_limit_source;
                if (snprintf(sz_source, sizeof p_memory->sz_limit_source, "cgroup %s of %s",
                             cb_high < cb_max ? "memory.high" : "memory.max",
                             sz_dir + strlen("/sys/fs/cgroup")) >= (int)sizeof p_memory->sz_limit_source)
                    strcpy(sz_source + sizeof p_memory->sz_limit_source - 4, "...");
            }
        }
        char *p_slash = strrchr(sz_dir, '/');
        if (!p_slash || (size_t)(p_slash - sz_dir) <= strlen("/sys/fs/cgroup")) break;
        *p_slash = '\0';
    }
}

/* "some avg10=1.23 ..." as 123: 0 when read. */
static int read_pressure(const char *sz_path, int32_t *p_out_permyriad) {
    FILE *p_file = fopen(sz_path, "r");
    if (!p_file) return errno;
    unsigned whole = 0, hundredths = 0;
    const int c_fields = fscanf(p_file, "some avg10=%u.%2u", &whole, &hundredths);
    fclose(p_file);
    if (c_fields != 2 || whole > 100) return EIO;
    *p_out_permyriad = (int32_t)(whole * 100 + hundredths);
    return 0;
}

int ps_host_memory_read(PsHostMemory *p_out_memory) {
    *p_out_memory = (PsHostMemory){.cb_limit = UINT64_MAX, .pressure_permyriad = -1};
    FILE *p_file = fopen("/proc/meminfo", "r");
    if (!p_file) return errno;
    char sz_line[128];
    unsigned long long kb_total = 0, kb_available = 0;
    int c_found = 0;
    while (c_found < 2 && fgets(sz_line, sizeof sz_line, p_file)) {
        c_found += sscanf(sz_line, "MemTotal: %llu kB", &kb_total);
        c_found += sscanf(sz_line, "MemAvailable: %llu kB", &kb_available);
    }
    fclose(p_file);
    if (c_found < 2) return EIO;
    p_out_memory->cb_total = (uint64_t)kb_total * 1024;
    p_out_memory->cb_available = (uint64_t)kb_available * 1024;
    char sz_group_dir[PATH_MAX - 32];
    read_cgroup_limit(p_out_memory, sz_group_dir, sizeof sz_group_dir);
    /* The group's own PSI first: under a cgroup limit the group stalls while the machine as a whole
     * may not. */
    char sz_path[PATH_MAX];
    if (!sz_group_dir[0] ||
        snprintf(sz_path, sizeof sz_path, "%s/memory.pressure", sz_group_dir) >= (int)sizeof sz_path || read_pressure(sz_path, &p_out_memory->pressure_permyriad))
        read_pressure("/proc/pressure/memory", &p_out_memory->pressure_permyriad);
    return 0;
}
