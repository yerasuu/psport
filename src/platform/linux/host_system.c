/* Linux: randomness, the executable's path and the machine's name (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
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

int ps_host_host_name(char *sz_out, size_t cb_out) {
    if (!cb_out) return EINVAL;
    if (gethostname(sz_out, cb_out)) return errno;
    sz_out[cb_out - 1] = 0;
    return 0;
}
