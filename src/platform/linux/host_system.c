// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux: randomness, the executable's path, the machine's name and the environment (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
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
