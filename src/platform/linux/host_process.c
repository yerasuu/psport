/* Linux: folder listings, descriptors, exec and the output helper (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/close_range.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
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
    /* By its own path while that still names the running file: an exec of /proc/self/exe names the
     * process "exe", and launchers look for the game by its name (debug mode starts again on every
     * run). */
    char sz_path[4096];
    struct stat self, named;
    const ssize_t cb_path = readlink("/proc/self/exe", sz_path, sizeof(sz_path) - 1);
    if (cb_path > 0) {
        sz_path[cb_path] = 0;
        if (!stat("/proc/self/exe", &self) && !stat(sz_path, &named) && self.st_dev == named.st_dev && self.st_ino == named.st_ino)
            execv(sz_path, pp_argv);
    }
    execv("/proc/self/exe", pp_argv);
    return errno;
}

static void (*s_pfn_exit)(int status);
static void on_exit_trampoline(int status, void *p_context) {
    (void)p_context;
    s_pfn_exit(status);
}
int ps_host_on_exit(void (*pfn_exit)(int status)) {
    s_pfn_exit = pfn_exit;
    return on_exit(on_exit_trampoline, NULL) ? ENOMEM : 0;
}

int ps_host_fs_duplicate(int h_file, int h_target) {
    const int h_new = h_target < 0 ? fcntl(h_file, F_DUPFD_CLOEXEC, 3) : dup2(h_file, h_target);
    return h_new < 0 ? -errno : h_new;
}

static int s_h_saved_output[2] = {-1, -1};
static void pump_output(int h_out, int h_err, void (*pfn_chunk)(int is_error, const char *p_data, size_t cb_data)) {
    struct pollfd pipes[2] = {{h_out, POLLIN, 0}, {h_err, POLLIN, 0}};
    int is_forwarding[2] = {1, 1};
    static char s_buffer[65536];
    while (pipes[0].fd >= 0 || pipes[1].fd >= 0) {
        if (poll(pipes, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        for (int i = 0; i < 2; ++i) {
            if (pipes[i].fd < 0 || !pipes[i].revents) continue;
            const ssize_t cb_read = read(pipes[i].fd, s_buffer, sizeof(s_buffer));
            if (cb_read < 0 && errno == EINTR) continue;
            if (cb_read <= 0) {
                close(pipes[i].fd);
                pipes[i].fd = -1;
                continue;
            }
            /* A closed terminal or launcher pipe ends the forwarding, not the copy. */
            for (ssize_t off = 0; is_forwarding[i] && off < cb_read;) {
                const ssize_t cb_written = write(i + 1, s_buffer + off, (size_t)(cb_read - off));
                if (cb_written < 0 && errno == EINTR) continue;
                if (cb_written <= 0) is_forwarding[i] = 0;
                else off += cb_written;
            }
            pfn_chunk(i, s_buffer, (size_t)cb_read);
        }
    }
}
int ps_host_output_tee(void (*pfn_chunk)(int is_error, const char *p_data, size_t cb_data), void (*pfn_end)(void)) {
    if (s_h_saved_output[0] >= 0) return EBUSY;
    int h_out[2], h_err[2];
    if (pipe2(h_out, O_CLOEXEC)) return errno;
    if (pipe2(h_err, O_CLOEXEC)) {
        const int error = errno;
        close(h_out[0]);
        close(h_out[1]);
        return error;
    }
    const int h_saved_out = fcntl(1, F_DUPFD_CLOEXEC, 3), h_saved_err = h_saved_out < 0 ? -1 : fcntl(2, F_DUPFD_CLOEXEC, 3);
    const pid_t pid = h_saved_err < 0 ? -1 : fork();
    if (pid < 0) {
        const int error = errno;
        if (h_saved_out >= 0) close(h_saved_out);
        if (h_saved_err >= 0) close(h_saved_err);
        close(h_out[0]); close(h_out[1]); close(h_err[0]); close(h_err[1]);
        return error;
    }
    if (!pid) {
        close(h_out[1]);
        close(h_err[1]);
        close(h_saved_out);
        close(h_saved_err);
        static const int s_ignored[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGTSTP, SIGTTOU};
        for (size_t i = 0; i < sizeof(s_ignored) / sizeof(*s_ignored); ++i) signal(s_ignored[i], SIG_IGN);
        /* Not psport's name, so that whoever looks for the game process by name finds it alone. */
        prctl(PR_SET_NAME, "psport-output");
        pump_output(h_out[0], h_err[0], pfn_chunk);
        pfn_end();
        _exit(0);
    }
    dup2(h_out[1], 1);
    dup2(h_err[1], 2);
    close(h_out[0]); close(h_out[1]); close(h_err[0]); close(h_err[1]);
    s_h_saved_output[0] = h_saved_out;
    s_h_saved_output[1] = h_saved_err;
    return 0;
}
void ps_host_output_untee(void) {
    if (s_h_saved_output[0] < 0) return;
    dup2(s_h_saved_output[0], 1);
    dup2(s_h_saved_output[1], 2);
    close(s_h_saved_output[0]);
    close(s_h_saved_output[1]);
    s_h_saved_output[0] = s_h_saved_output[1] = -1;
}
