/* Linux: files and folders (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

static int open_flags(unsigned flags) {
    static const int s_access[] = {O_RDONLY, O_WRONLY, O_RDWR, O_RDWR};
    int host = s_access[flags & PS_HOST_OPEN_ACCESS_MASK] | O_CLOEXEC;
    if (flags & PS_HOST_OPEN_NONBLOCK) host |= O_NONBLOCK;
    if (flags & PS_HOST_OPEN_APPEND) host |= O_APPEND;
    if (flags & PS_HOST_OPEN_SYNC) host |= O_SYNC;
    if (flags & PS_HOST_OPEN_CREATE) host |= O_CREAT;
    if (flags & PS_HOST_OPEN_TRUNCATE) host |= O_TRUNC;
    if (flags & PS_HOST_OPEN_EXCLUSIVE) host |= O_EXCL;
    if (flags & PS_HOST_OPEN_DIRECTORY) host |= O_DIRECTORY;
    return host;
}

static void convert_stat(const struct stat *p_stat, PsHostStat *p_out) {
    *p_out = (PsHostStat){
        .dev = (uint64_t)p_stat->st_dev,
        .ino = (uint64_t)p_stat->st_ino,
        .mode = (uint32_t)p_stat->st_mode,
        .c_links = (uint32_t)p_stat->st_nlink,
        .cb_block = (uint32_t)p_stat->st_blksize,
        .cb_size = p_stat->st_size,
        .c_blocks = p_stat->st_blocks,
        .atime = p_stat->st_atim,
        .mtime = p_stat->st_mtim,
        .ctime = p_stat->st_ctim,
    };
}

static int64_t transferred(ssize_t cb_transferred) { return cb_transferred < 0 ? -errno : cb_transferred; }

static int to_timevals(const struct timespec *p_times, struct timeval *p_out) {
    if (!p_times) return 0;
    for (int i = 0; i < 2; ++i)
        p_out[i] = (struct timeval){.tv_sec = p_times[i].tv_sec, .tv_usec = p_times[i].tv_nsec / 1000};
    return 1;
}

int ps_host_fs_open(const char *sz_path, unsigned flags, unsigned mode) {
    const int h_file = open(sz_path, open_flags(flags), (mode_t)mode);
    return h_file < 0 ? -errno : h_file;
}

int ps_host_fs_close(int h_file) { return close(h_file) ? errno : 0; }

int64_t ps_host_fs_read(int h_file, void *p_buffer, size_t cb_buffer) {
    return transferred(read(h_file, p_buffer, cb_buffer));
}

int64_t ps_host_fs_pread(int h_file, void *p_buffer, size_t cb_buffer, int64_t off_file) {
    return transferred(pread(h_file, p_buffer, cb_buffer, (off_t)off_file));
}

int64_t ps_host_fs_write(int h_file, const void *p_buffer, size_t cb_buffer) {
    return transferred(write(h_file, p_buffer, cb_buffer));
}

int64_t ps_host_fs_pwrite(int h_file, const void *p_buffer, size_t cb_buffer, int64_t off_file) {
    return transferred(pwrite(h_file, p_buffer, cb_buffer, (off_t)off_file));
}

int64_t ps_host_fs_seek(int h_file, int64_t off_file, int whence) {
    const off_t off_result = lseek(h_file, (off_t)off_file, whence);
    return off_result < 0 ? -errno : off_result;
}

int ps_host_fs_sync(int h_file) { return fsync(h_file) ? errno : 0; }

int ps_host_fs_truncate(int h_file, int64_t cb_file) { return ftruncate(h_file, (off_t)cb_file) ? errno : 0; }

int ps_host_fs_truncate_path(const char *sz_path, int64_t cb_file) {
    return truncate(sz_path, (off_t)cb_file) ? errno : 0;
}

int ps_host_fs_stat(const char *sz_path, PsHostStat *p_out) {
    struct stat info;
    if (stat(sz_path, &info)) return errno;
    convert_stat(&info, p_out);
    return 0;
}

int ps_host_fs_stat_link(const char *sz_path, PsHostStat *p_out) {
    struct stat info;
    if (lstat(sz_path, &info)) return errno;
    convert_stat(&info, p_out);
    return 0;
}

int ps_host_fs_stat_file(int h_file, PsHostStat *p_out) {
    struct stat info;
    if (fstat(h_file, &info)) return errno;
    convert_stat(&info, p_out);
    return 0;
}

int ps_host_fs_stat_stream(FILE *p_stream, PsHostStat *p_out) { return ps_host_fs_stat_file(fileno(p_stream), p_out); }

int ps_host_fs_sync_stream(FILE *p_stream) { return ps_host_fs_sync(fileno(p_stream)); }

int ps_host_fs_truncate_stream(FILE *p_stream, int64_t cb_file) { return ps_host_fs_truncate(fileno(p_stream), cb_file); }

int ps_host_fs_access(const char *sz_path, int mode) { return access(sz_path, mode) ? errno : 0; }

int ps_host_fs_make_dir(const char *sz_path, unsigned mode) { return mkdir(sz_path, (mode_t)mode) ? errno : 0; }

int ps_host_fs_make_unique_dir(char *sz_template) { return mkdtemp(sz_template) ? 0 : errno; }

int ps_host_fs_remove_dir(const char *sz_path) { return rmdir(sz_path) ? errno : 0; }

int ps_host_fs_remove(const char *sz_path) { return unlink(sz_path) ? errno : 0; }

int ps_host_fs_rename(const char *sz_from, const char *sz_to) { return rename(sz_from, sz_to) ? errno : 0; }

int ps_host_fs_symlink(const char *sz_target, const char *sz_link) { return symlink(sz_target, sz_link) ? errno : 0; }

int ps_host_fs_free_space(const char *sz_path, uint64_t *p_out_cb_free) {
    struct statvfs info;
    if (statvfs(sz_path, &info)) return errno;
    *p_out_cb_free = (uint64_t)info.f_bavail * info.f_frsize;
    return 0;
}

int ps_host_fs_real_path(const char *sz_path, char *sz_out, size_t cb_out) {
    char sz_real[PATH_MAX];
    if (!realpath(sz_path, sz_real)) return errno;
    return (size_t)snprintf(sz_out, cb_out, "%s", sz_real) < cb_out ? 0 : ENAMETOOLONG;
}

int ps_host_fs_set_times(const char *sz_path, const struct timespec *p_times) {
    struct timeval times[2];
    return utimes(sz_path, to_timevals(p_times, times) ? times : NULL) ? errno : 0;
}

int ps_host_fs_set_times_file(int h_file, const struct timespec *p_times) {
    struct timeval times[2];
    return futimes(h_file, to_timevals(p_times, times) ? times : NULL) ? errno : 0;
}

int ps_host_fs_set_mode(const char *sz_path, unsigned mode) { return chmod(sz_path, (mode_t)(mode & 07777)) ? errno : 0; }

int ps_host_fs_set_mode_file(int h_file, unsigned mode) { return fchmod(h_file, (mode_t)(mode & 07777)) ? errno : 0; }

int ps_host_fs_status(int h_file, unsigned *p_out_flags) {
    const int flags = fcntl(h_file, F_GETFL);
    if (flags < 0) return errno;
    static const unsigned s_access[] = {PS_HOST_OPEN_READ, PS_HOST_OPEN_WRITE, PS_HOST_OPEN_READ_WRITE,
                                        PS_HOST_OPEN_READ_WRITE};
    *p_out_flags = s_access[flags & O_ACCMODE] | (flags & O_NONBLOCK ? PS_HOST_OPEN_NONBLOCK : 0u) |
                   (flags & O_APPEND ? PS_HOST_OPEN_APPEND : 0u);
    return 0;
}

int ps_host_fs_set_status(int h_file, unsigned flags) {
    const int current = fcntl(h_file, F_GETFL);
    if (current < 0) return errno;
    const int wanted = (current & ~(O_NONBLOCK | O_APPEND)) | (flags & PS_HOST_OPEN_NONBLOCK ? O_NONBLOCK : 0) |
                       (flags & PS_HOST_OPEN_APPEND ? O_APPEND : 0);
    return fcntl(h_file, F_SETFL, wanted) ? errno : 0;
}

int ps_host_fs_close_on_exec(int h_file) {
    const int flags = fcntl(h_file, F_GETFD);
    return flags < 0 ? -errno : (flags & FD_CLOEXEC) != 0;
}

int ps_host_fs_set_close_on_exec(int h_file, int is_set) {
    return fcntl(h_file, F_SETFD, is_set ? FD_CLOEXEC : 0) ? errno : 0;
}

int ps_host_fs_sync_data(int h_file) { return fdatasync(h_file) ? errno : 0; }

int ps_host_fs_pipe(int p_out_h_files[2]) { return pipe2(p_out_h_files, O_CLOEXEC) ? errno : 0; }

int ps_host_fs_lock(int h_file, unsigned operation) {
    int host = operation & PS_HOST_LOCK_UNLOCK ? LOCK_UN : operation & PS_HOST_LOCK_EXCLUSIVE ? LOCK_EX : LOCK_SH;
    if (operation & PS_HOST_LOCK_NONBLOCK) host |= LOCK_NB;
    return flock(h_file, host) ? errno : 0;
}

static int64_t room_to_write(int h_file) {
    int cb_queued = 0;
    const int cb_pipe = fcntl(h_file, F_GETPIPE_SZ);
    if (cb_pipe >= 0) return ioctl(h_file, FIONREAD, &cb_queued) || cb_queued > cb_pipe ? 0 : cb_pipe - cb_queued;
    int cb_buffer = 0;
    socklen_t cb_option = sizeof(cb_buffer);
    if (getsockopt(h_file, SOL_SOCKET, SO_SNDBUF, &cb_buffer, &cb_option) || ioctl(h_file, TIOCOUTQ, &cb_queued))
        return 0;
    return cb_queued > cb_buffer ? 0 : cb_buffer - cb_queued;
}

int ps_host_fs_poll(int h_file, unsigned events, unsigned *p_out_events, int64_t *p_out_cb_ready) {
    const short host_events = (short)((events & PS_HOST_POLL_IN ? POLLIN : 0) | (events & PS_HOST_POLL_OUT ? POLLOUT : 0));
    struct pollfd polled = {.fd = h_file, .events = host_events};
    int c_ready;
    while ((c_ready = poll(&polled, 1, 0)) < 0 && errno == EINTR) {}
    if (c_ready < 0) return errno;
    if (polled.revents & POLLNVAL) return EBADF;
    *p_out_events = (polled.revents & POLLIN ? PS_HOST_POLL_IN : 0u) |
                    (polled.revents & POLLOUT ? PS_HOST_POLL_OUT : 0u) |
                    (polled.revents & POLLHUP ? PS_HOST_POLL_HANGUP : 0u) |
                    (polled.revents & POLLERR ? PS_HOST_POLL_ERROR : 0u);
    *p_out_cb_ready = 0;
    int cb_queued = 0;
    if (events & PS_HOST_POLL_IN) {
        if (!ioctl(h_file, FIONREAD, &cb_queued)) *p_out_cb_ready = cb_queued;
    } else if (events & PS_HOST_POLL_OUT) {
        *p_out_cb_ready = room_to_write(h_file);
    }
    return 0;
}

PsHostDir *ps_host_dir_open(const char *sz_path) { return (PsHostDir *)opendir(sz_path); }

int ps_host_dir_next(PsHostDir *p_dir, PsHostDirEntry *p_out) {
    const struct dirent *p_entry = readdir((DIR *)p_dir);
    if (!p_entry) return 0;
    p_out->sz_name = p_entry->d_name;
    switch (p_entry->d_type) {
    case DT_UNKNOWN: p_out->kind = PS_HOST_ENTRY_UNKNOWN; break;
    case DT_REG: p_out->kind = PS_HOST_ENTRY_REGULAR; break;
    case DT_DIR: p_out->kind = PS_HOST_ENTRY_DIRECTORY; break;
    case DT_LNK: p_out->kind = PS_HOST_ENTRY_LINK; break;
    default: p_out->kind = PS_HOST_ENTRY_OTHER; break;
    }
    return 1;
}

int ps_host_dir_stat(PsHostDir *p_dir, const char *sz_name, PsHostStat *p_out) {
    struct stat info;
    if (fstatat(dirfd((DIR *)p_dir), sz_name, &info, 0)) return errno;
    convert_stat(&info, p_out);
    return 0;
}

void ps_host_dir_close(PsHostDir *p_dir) { closedir((DIR *)p_dir); }

FILE *ps_host_fs_open_stream(const char *sz_path, const char *sz_mode) { return fopen(sz_path, sz_mode); }
