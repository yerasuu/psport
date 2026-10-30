// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux: randomness, the executable's path, the machine's name and the environment (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

int ps_host_random_bytes(void *p_buffer, size_t cb_buffer) {
    unsigned char *p_next = p_buffer;
    while (cb_buffer) {
        const ssize_t cb_read = getrandom(p_next, cb_buffer, 0);
        if (cb_read < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        p_next += cb_read;
        cb_buffer -= (size_t)cb_read;
    }
    return 0;
}

long ps_host_exe_path(char *sz_out, size_t cb_out) {
    if (!cb_out) return -1;
    const ssize_t cb_path = readlink("/proc/self/exe", sz_out, cb_out - 1);
    if (cb_path <= 0 || (size_t)cb_path >= cb_out - 1) {
        sz_out[0] = 0;
        return -1;
    }
    sz_out[cb_path] = 0;
    return (long)cb_path;
}

long ps_host_command_line(char *sz_out, size_t cb_out) {
    if (!cb_out) return -1;
    sz_out[0] = 0;
    const int h_cmdline = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (h_cmdline < 0) return -1;
    size_t cb_read = 0;
    for (;;) {
        const ssize_t cb_chunk = read(h_cmdline, sz_out + cb_read, cb_out - cb_read);
        if (cb_chunk < 0 && errno == EINTR) continue;
        if (cb_chunk <= 0) break;
        cb_read += (size_t)cb_chunk;
        if (cb_read == cb_out) break;
    }
    const int is_complete = cb_read < cb_out;
    close(h_cmdline);
    if (!is_complete) {
        sz_out[0] = 0;
        return -1;
    }
    /* Arguments end in NUL, so the last one is dropped: the string's own terminator takes its place. */
    if (cb_read && sz_out[cb_read - 1] == 0) --cb_read;
    for (size_t i = 0; i < cb_read; ++i)
        if (sz_out[i] == 0) sz_out[i] = ' ';
    sz_out[cb_read] = 0;
    return (long)cb_read;
}

long ps_host_shadps4_user_dir(char *sz_out, size_t cb_out) {
    if (cb_out) sz_out[0] = 0;
    return -1;
}

int ps_host_host_name(char *sz_out, size_t cb_out) {
    if (!cb_out) return EINVAL;
    if (gethostname(sz_out, cb_out)) return errno;
    sz_out[cb_out - 1] = 0;
    return 0;
}

int ps_host_env_set(const char *sz_name, const char *sz_value, int is_overwrite) {
    return setenv(sz_name, sz_value, is_overwrite) ? errno : 0;
}

/* The block the kernel copied the environment into at exec (proc(5): fields 50 and 51 of
 * /proc/self/stat, since Linux 3.5). /proc/<pid>/environ and a core read these bytes as they are
 * now, whatever environ points to: setenv and unsetenv only change environ's array. */
static int start_environment(char **pp_out_start, char **pp_out_end) {
    char sz_stat[2048];
    const int h_stat = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (h_stat < 0) return errno;
    const ssize_t cb_stat = read(h_stat, sz_stat, sizeof(sz_stat) - 1);
    const int error = cb_stat < 0 ? errno : EIO;
    close(h_stat);
    if (cb_stat <= 0) return error;
    sz_stat[cb_stat] = 0;
    /* The command name (field 2) may hold spaces and parentheses: the fields after it count from
     * its last ')'. */
    char *sz_fields = strrchr(sz_stat, ')');
    if (!sz_fields) return EIO;
    char *p_save, *sz_field = strtok_r(sz_fields + 1, " ", &p_save);
    for (int i_field = 3; sz_field && i_field < 50; ++i_field) sz_field = strtok_r(NULL, " ", &p_save);
    if (!sz_field) return ENOSYS;
    const uintptr_t va_start = (uintptr_t)strtoull(sz_field, NULL, 10);
    sz_field = strtok_r(NULL, " ", &p_save);
    const uintptr_t va_end = sz_field ? (uintptr_t)strtoull(sz_field, NULL, 10) : 0;
    if (!va_start || va_end <= va_start) return ENOSYS;
    *pp_out_start = (char *)va_start;
    *pp_out_end = (char *)va_end;
    return 0;
}

static int is_entry_of(const char *sz_entry, const char *sz_name, size_t cb_name) {
    return !strncmp(sz_entry, sz_name, cb_name) && sz_entry[cb_name] == '=';
}

int ps_host_env_scrub(const char *sz_name, int *p_out_is_set) {
    *p_out_is_set = 0;
    const size_t cb_name = strlen(sz_name);
    if (!cb_name || strchr(sz_name, '=')) return EINVAL;
    for (char **pp_entry = environ; pp_entry && *pp_entry; ++pp_entry) {
        if (!is_entry_of(*pp_entry, sz_name, cb_name)) continue;
        ps_host_wipe(*pp_entry + cb_name + 1, strlen(*pp_entry + cb_name + 1));
        *p_out_is_set = 1;
    }
    if (unsetenv(sz_name)) return errno;
    /* Also the entries environ no longer points to (a setenv since exec left the original behind);
     * their names go too, once nothing refers to them, so that the block does not even say which
     * secrets the process was given. */
    char *p_start, *p_end;
    const int error = start_environment(&p_start, &p_end);
    if (error) return error;
    for (char *p_entry = p_start; p_entry < p_end;) {
        const size_t cb_entry = strnlen(p_entry, (size_t)(p_end - p_entry));
        if (cb_entry > cb_name && is_entry_of(p_entry, sz_name, cb_name)) {
            ps_host_wipe(p_entry, cb_entry);
            *p_out_is_set = 1;
        }
        p_entry += cb_entry + 1;
    }
    return 0;
}
