/* Linux: folder listings, descriptors and exec (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/close_range.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int ps_host_list_dir(const char *sz_path, void (*pfn_entry)(const char *sz_name, void *p_context), void *p_context) {
    const int h_dir = open(sz_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (h_dir < 0) return errno;
    char buffer[4096] __attribute__((aligned(8)));
    long cb_read;
    while ((cb_read = syscall(SYS_getdents64, h_dir, buffer, sizeof(buffer))) > 0)
        for (long off_entry = 0; off_entry < cb_read;) {
            const struct {
                uint64_t ino;
                int64_t off;
                unsigned short cb_record;
                unsigned char type;
                char sz_name[];
            } *p_entry = (const void *)(buffer + off_entry);
            if (strcmp(p_entry->sz_name, ".") && strcmp(p_entry->sz_name, "..")) pfn_entry(p_entry->sz_name, p_context);
            off_entry += p_entry->cb_record;
        }
    const int error = cb_read < 0 ? errno : 0;
    close(h_dir);
    return error;
}

int ps_host_close_from(int h_first, int h_last, int is_on_exec) {
    return syscall(SYS_close_range, (unsigned)h_first, h_last < 0 ? ~0u : (unsigned)h_last,
                   is_on_exec ? CLOSE_RANGE_CLOEXEC : 0u)
               ? errno
               : 0;
}

int ps_host_exec(const char *sz_file, char *const *pp_argv) {
    execvp(sz_file, pp_argv);
    return errno;
}

int ps_host_exec_self(char *const *pp_argv) {
    execv("/proc/self/exe", pp_argv);
    return errno;
}
