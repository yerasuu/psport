// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: files and folders on Win32 handles, behind psport's int descriptors (ps_host.h). */
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The guest's table (runtime_file.c) holds 1024 files; the rest is for psport's own. */
#define DESCRIPTOR_MAX 4096
#define FILETIME_UNIX_EPOCH 116444736000000000LL
#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define MODE_FIFO 0010000u
#define MODE_CHARACTER 0020000u

typedef struct {
    HANDLE h_handle;
    /* A second handle on the same file for pread and pwrite, opened on first use, since an offset
     * given to ReadFile or WriteFile also moves a synchronous handle's file pointer, which read,
     * write and duplicated descriptors share. */
    HANDLE h_positional;
    DWORD access, type;
    unsigned flags;
    int is_directory, is_close_on_exec, is_random;
} Descriptor;

static Descriptor s_descriptors[DESCRIPTOR_MAX];
/* Held exclusively only to change a slot, never around I/O or anything that can fault, so that a
 * fault report writing to descriptor 2 cannot find it held by its own thread. */
static SRWLOCK s_lock = SRWLOCK_INIT;
static INIT_ONCE s_stdio_once = INIT_ONCE_STATIC_INIT;

static int64_t win32_failure(void) { return -(int64_t)ps_win32_errno(GetLastError()); }

static void describe_handle(Descriptor *p_descriptor) {
    p_descriptor->type = GetFileType(p_descriptor->h_handle);
    FILE_BASIC_INFO basic;
    p_descriptor->is_directory =
        p_descriptor->type == FILE_TYPE_DISK &&
        GetFileInformationByHandleEx(p_descriptor->h_handle, FileBasicInfo, &basic, sizeof(basic)) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* The process's standard handles as descriptors 0-2, copies owned by the table so that closing or
 * replacing one leaves the process's own. */
static BOOL CALLBACK adopt_stdio(PINIT_ONCE p_once, void *p_parameter, void **pp_context) {
    (void)p_once;
    (void)p_parameter;
    (void)pp_context;
    static const DWORD s_standard[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; ++i) {
        const HANDLE h_standard = GetStdHandle(s_standard[i]);
        HANDLE h_copy = NULL;
        if (!h_standard || h_standard == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(GetCurrentProcess(), h_standard, GetCurrentProcess(), &h_copy, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            continue;
        Descriptor *p_descriptor = &s_descriptors[i];
        *p_descriptor = (Descriptor){.h_handle = h_copy,
                                     .access = i ? GENERIC_WRITE : GENERIC_READ,
                                     .flags = i ? PS_HOST_OPEN_WRITE : PS_HOST_OPEN_READ};
        describe_handle(p_descriptor);
    }
    return TRUE;
}

static void ensure_stdio(void) { InitOnceExecuteOnce(&s_stdio_once, adopt_stdio, NULL, NULL); }

static int is_valid_descriptor(int h_file) { return h_file >= 0 && h_file < DESCRIPTOR_MAX; }

/* A copy of the slot: 0, or EBADF when it is not open. */
static int descriptor_get(int h_file, Descriptor *p_out) {
    if (!is_valid_descriptor(h_file)) return EBADF;
    ensure_stdio();
    AcquireSRWLockShared(&s_lock);
    *p_out = s_descriptors[h_file];
    ReleaseSRWLockShared(&s_lock);
    return p_out->h_handle ? 0 : EBADF;
}

/* Takes the handles of a filled descriptor into the lowest free slot from h_lowest: the descriptor,
 * or minus EMFILE with them closed. */
static int descriptor_add(const Descriptor *p_descriptor, int h_lowest) {
    ensure_stdio();
    int h_file = -EMFILE;
    AcquireSRWLockExclusive(&s_lock);
    for (int i = h_lowest; i < DESCRIPTOR_MAX; ++i)
        if (!s_descriptors[i].h_handle) {
            s_descriptors[i] = *p_descriptor;
            h_file = i;
            break;
        }
    ReleaseSRWLockExclusive(&s_lock);
    if (h_file < 0) {
        CloseHandle(p_descriptor->h_handle);
        if (p_descriptor->h_positional) CloseHandle(p_descriptor->h_positional);
    }
    return h_file;
}

static void descriptor_release(const Descriptor *p_descriptor) {
    if (p_descriptor->h_positional) CloseHandle(p_descriptor->h_positional);
    if (p_descriptor->h_handle) CloseHandle(p_descriptor->h_handle);
}

HANDLE ps_win32_fd_handle(int h_file, int is_for_exec) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor) || (is_for_exec && descriptor.is_close_on_exec)) return NULL;
    return descriptor.h_handle;
}

int ps_win32_fd_adopt(HANDLE h_handle, unsigned flags) {
    static const DWORD s_access[] = {GENERIC_READ, GENERIC_WRITE, GENERIC_READ | GENERIC_WRITE,
                                     GENERIC_READ | GENERIC_WRITE};
    Descriptor descriptor = {.h_handle = h_handle,
                             .access = s_access[flags & PS_HOST_OPEN_ACCESS_MASK],
                             .flags = flags,
                             .is_close_on_exec = 1};
    describe_handle(&descriptor);
    return descriptor_add(&descriptor, 3);
}

/* UTF-8 with either separator to the wide path the W calls take. */
static int wide_path(const char *sz_path, wchar_t *sz_out, size_t c_out) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = ps_win32_to_wide(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    for (wchar_t *p_char = sz_wide; *p_char; ++p_char)
        if (*p_char == L'/') *p_char = L'\\';
    const size_t c_path = wcslen(sz_wide);
    /* Past MAX_PATH (248 for folders) the W calls need the \\?\ prefix, which also turns off their
     * own resolution of "." and "..": GetFullPathNameW does that first. */
    if (c_path < MAX_PATH - 12 || !wcsncmp(sz_wide, L"\\\\?\\", 4)) {
        if (c_path >= c_out) return ENAMETOOLONG;
        wmemcpy(sz_out, sz_wide, c_path + 1);
        return 0;
    }
    wchar_t sz_full[PS_WIN32_PATH_MAX];
    const DWORD c_full = GetFullPathNameW(sz_wide, PS_WIN32_PATH_MAX, sz_full, NULL);
    if (!c_full) return ps_win32_errno(GetLastError());
    if (c_full >= PS_WIN32_PATH_MAX) return ENAMETOOLONG;
    const int is_unc = sz_full[0] == L'\\' && sz_full[1] == L'\\';
    const int c_written = is_unc ? swprintf(sz_out, c_out, L"\\\\?\\UNC%ls", sz_full + 1)
                                 : swprintf(sz_out, c_out, L"\\\\?\\%ls", sz_full);
    return c_written < 0 ? ENAMETOOLONG : 0;
}

static HANDLE open_existing(const wchar_t *sz_wide, DWORD access, int is_following_links) {
    return CreateFileW(sz_wide, access, SHARE_ALL, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | (is_following_links ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), NULL);
}

/* runtime_file.c mounts the guest's random devices on the host paths Linux has. Windows has no such
 * file: the descriptor holds the NUL device, which takes writes as /dev/random does, and its reads
 * come from BCryptGenRandom (ps_host_random_bytes). */
static int is_random_device(const char *sz_path) {
    return !strcmp(sz_path, "/dev/urandom") || !strcmp(sz_path, "/dev/random");
}

static int open_random_device(unsigned flags) {
    if (flags & PS_HOST_OPEN_DIRECTORY) return -ENOTDIR;
    const HANDLE h_handle =
        CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, SHARE_ALL, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h_handle == INVALID_HANDLE_VALUE) return (int)win32_failure();
    Descriptor descriptor = {.h_handle = h_handle,
                             .access = GENERIC_READ | GENERIC_WRITE,
                             .flags = flags & (PS_HOST_OPEN_ACCESS_MASK | PS_HOST_OPEN_NONBLOCK | PS_HOST_OPEN_APPEND),
                             .is_close_on_exec = 1,
                             .is_random = 1};
    describe_handle(&descriptor);
    return descriptor_add(&descriptor, 3);
}

static void stat_random_device(PsHostStat *p_out) {
    *p_out = (PsHostStat){.mode = MODE_CHARACTER | 0666u, .c_links = 1, .cb_block = 4096};
}

int ps_host_fs_open(const char *sz_path, unsigned flags, unsigned mode) {
    if (is_random_device(sz_path)) return open_random_device(flags);
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return -error;
    const unsigned access_mode = flags & PS_HOST_OPEN_ACCESS_MASK;
    const int is_truncating = (flags & PS_HOST_OPEN_TRUNCATE) != 0;
    DWORD access = FILE_READ_ATTRIBUTES;
    if (access_mode != PS_HOST_OPEN_WRITE) access |= GENERIC_READ;
    if (access_mode != PS_HOST_OPEN_READ || is_truncating) access |= GENERIC_WRITE;
    DWORD disposition = OPEN_EXISTING;
    if (flags & PS_HOST_OPEN_CREATE)
        disposition = flags & PS_HOST_OPEN_EXCLUSIVE ? CREATE_NEW : is_truncating ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (is_truncating)
        disposition = TRUNCATE_EXISTING;
    /* Windows has no permission bits: a created file without the owner's write bit is read-only. */
    DWORD attributes = flags & PS_HOST_OPEN_CREATE && !(mode & 0200) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
    attributes |= FILE_FLAG_BACKUP_SEMANTICS | (flags & PS_HOST_OPEN_SYNC ? FILE_FLAG_WRITE_THROUGH : 0);
    const HANDLE h_handle = CreateFileW(sz_wide, access, SHARE_ALL, NULL, disposition, attributes, NULL);
    if (h_handle == INVALID_HANDLE_VALUE) return (int)win32_failure();
    Descriptor descriptor = {.h_handle = h_handle,
                             .access = access,
                             .flags = flags & (PS_HOST_OPEN_ACCESS_MASK | PS_HOST_OPEN_NONBLOCK | PS_HOST_OPEN_APPEND),
                             .is_close_on_exec = 1};
    describe_handle(&descriptor);
    const int error_kind = descriptor.is_directory && access_mode != PS_HOST_OPEN_READ ? EISDIR
                           : !descriptor.is_directory && flags & PS_HOST_OPEN_DIRECTORY ? ENOTDIR
                                                                                         : 0;
    if (error_kind) {
        CloseHandle(h_handle);
        return -error_kind;
    }
    return descriptor_add(&descriptor, 3);
}

int ps_host_fs_close(int h_file) {
    if (h_file >= PS_WIN32_SECTION_BASE) return ps_win32_section_close(h_file);
    if (!is_valid_descriptor(h_file)) return EBADF;
    ensure_stdio();
    AcquireSRWLockExclusive(&s_lock);
    const Descriptor closed = s_descriptors[h_file];
    s_descriptors[h_file] = (Descriptor){0};
    ReleaseSRWLockExclusive(&s_lock);
    if (!closed.h_handle) return EBADF;
    descriptor_release(&closed);
    return 0;
}

static DWORD clamp_transfer(size_t cb_buffer) { return cb_buffer > INT32_MAX ? INT32_MAX : (DWORD)cb_buffer; }

static int64_t read_random(void *p_buffer, size_t cb_buffer) {
    const DWORD cb_read = clamp_transfer(cb_buffer);
    const int error = ps_host_random_bytes(p_buffer, cb_read);
    return error ? -(int64_t)error : (int64_t)cb_read;
}

/* A closed pipe's write end and the end of a file read as end of file, as read(2) has them. */
static int64_t read_failure(void) {
    const DWORD error = GetLastError();
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ? 0 : -(int64_t)ps_win32_errno(error);
}

int64_t ps_host_fs_read(int h_file, void *p_buffer, size_t cb_buffer) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return -EBADF;
    if (descriptor.is_directory) return -EISDIR;
    if (descriptor.is_random) return read_random(p_buffer, cb_buffer);
    DWORD cb_done = 0;
    return ReadFile(descriptor.h_handle, p_buffer, clamp_transfer(cb_buffer), &cb_done, NULL) ? cb_done : read_failure();
}

static OVERLAPPED at_offset(uint64_t off_file) {
    return (OVERLAPPED){.Offset = (DWORD)off_file, .OffsetHigh = (DWORD)(off_file >> 32)};
}

int64_t ps_host_fs_write(int h_file, const void *p_buffer, size_t cb_buffer) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return -EBADF;
    if (descriptor.is_directory) return -EISDIR;
    /* All bits set in the offset is Windows' own append: the end of file is found and written in
     * one step, as O_APPEND's. */
    OVERLAPPED append = at_offset(UINT64_MAX);
    const int is_appending = descriptor.flags & PS_HOST_OPEN_APPEND && descriptor.type == FILE_TYPE_DISK;
    DWORD cb_done = 0;
    return WriteFile(descriptor.h_handle, p_buffer, clamp_transfer(cb_buffer), &cb_done, is_appending ? &append : NULL)
               ? cb_done
               : win32_failure();
}

/* The descriptor's positional handle: 0, or the host's errno. */
static int positional_handle(int h_file, HANDLE *p_out) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    if (descriptor.is_directory) return EISDIR;
    if (descriptor.type != FILE_TYPE_DISK) return ESPIPE;
    if (descriptor.h_positional) {
        *p_out = descriptor.h_positional;
        return 0;
    }
    const HANDLE h_reopened = ReOpenFile(descriptor.h_handle, descriptor.access, SHARE_ALL, 0);
    if (h_reopened == INVALID_HANDLE_VALUE) return ps_win32_errno(GetLastError());
    AcquireSRWLockExclusive(&s_lock);
    Descriptor *p_slot = &s_descriptors[h_file];
    const int is_installed = p_slot->h_handle == descriptor.h_handle && !p_slot->h_positional;
    if (is_installed) p_slot->h_positional = h_reopened;
    *p_out = p_slot->h_handle == descriptor.h_handle ? p_slot->h_positional : NULL;
    ReleaseSRWLockExclusive(&s_lock);
    if (!is_installed) CloseHandle(h_reopened);
    return *p_out ? 0 : EBADF;
}

int64_t ps_host_fs_pread(int h_file, void *p_buffer, size_t cb_buffer, int64_t off_file) {
    if (off_file < 0) return -EINVAL;
    Descriptor descriptor;
    if (!descriptor_get(h_file, &descriptor) && descriptor.is_random) return read_random(p_buffer, cb_buffer);
    HANDLE h_positional;
    const int error = positional_handle(h_file, &h_positional);
    if (error) return -error;
    OVERLAPPED at = at_offset((uint64_t)off_file);
    DWORD cb_done = 0;
    return ReadFile(h_positional, p_buffer, clamp_transfer(cb_buffer), &cb_done, &at) ? cb_done : read_failure();
}

int64_t ps_host_fs_pwrite(int h_file, const void *p_buffer, size_t cb_buffer, int64_t off_file) {
    if (off_file < 0) return -EINVAL;
    HANDLE h_positional;
    const int error = positional_handle(h_file, &h_positional);
    if (error) return -error;
    OVERLAPPED at = at_offset((uint64_t)off_file);
    DWORD cb_done = 0;
    return WriteFile(h_positional, p_buffer, clamp_transfer(cb_buffer), &cb_done, &at) ? cb_done : win32_failure();
}

int64_t ps_host_fs_seek(int h_file, int64_t off_file, int whence) {
    static const DWORD s_methods[] = {[SEEK_SET] = FILE_BEGIN, [SEEK_CUR] = FILE_CURRENT, [SEEK_END] = FILE_END};
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -EINVAL;
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return -EBADF;
    if (descriptor.is_random) return 0;
    if (descriptor.type != FILE_TYPE_DISK) return -ESPIPE;
    LARGE_INTEGER off_new;
    return SetFilePointerEx(descriptor.h_handle, (LARGE_INTEGER){.QuadPart = off_file}, &off_new, s_methods[whence])
               ? off_new.QuadPart
               : win32_failure();
}

/* A folder's entries are journaled by NTFS, and FlushFileBuffers takes no folder handle opened for
 * reading: syncing one succeeds, as runtime_file.c's commit expects. */
int ps_host_fs_sync(int h_file) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    if (descriptor.is_directory) return 0;
    if (descriptor.type != FILE_TYPE_DISK) return EINVAL;
    return FlushFileBuffers(descriptor.h_handle) ? 0 : ps_win32_errno(GetLastError());
}

int ps_host_fs_sync_data(int h_file) { return ps_host_fs_sync(h_file); }

int ps_host_fs_pipe(int p_out_h_files[2]) {
    HANDLE h_read, h_write;
    if (!CreatePipe(&h_read, &h_write, NULL, 0)) return ps_win32_errno(GetLastError());
    const int h_read_file = ps_win32_fd_adopt(h_read, PS_HOST_OPEN_READ);
    if (h_read_file < 0) {
        CloseHandle(h_write);
        return -h_read_file;
    }
    const int h_write_file = ps_win32_fd_adopt(h_write, PS_HOST_OPEN_WRITE);
    if (h_write_file < 0) {
        (void)ps_host_fs_close(h_read_file);
        return -h_write_file;
    }
    p_out_h_files[0] = h_read_file;
    p_out_h_files[1] = h_write_file;
    return 0;
}

/* LockFileEx over the whole file stands in for flock. Unlike flock's advisory locks, Windows' are
 * mandatory: other handles' reads and writes of a locked range fail while it is held. A change
 * between shared and exclusive lets the old lock go first, as flock's conversion is not atomic
 * either. A lock held elsewhere reads as EAGAIN, which is Linux's EWOULDBLOCK. */
int ps_host_fs_lock(int h_file, unsigned operation) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    OVERLAPPED whole = {0};
    if (!UnlockFileEx(descriptor.h_handle, 0, MAXDWORD, MAXDWORD, &whole) && GetLastError() != ERROR_NOT_LOCKED)
        return ps_win32_errno(GetLastError());
    if (operation & PS_HOST_LOCK_UNLOCK) return 0;
    const DWORD flags = (operation & PS_HOST_LOCK_EXCLUSIVE ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
                        (operation & PS_HOST_LOCK_NONBLOCK ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    whole = (OVERLAPPED){0};
    if (LockFileEx(descriptor.h_handle, flags, 0, MAXDWORD, MAXDWORD, &whole)) return 0;
    const DWORD error = GetLastError();
    return error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING ? EAGAIN : ps_win32_errno(error);
}

/* Anonymous pipes say how much a read gets (PeekNamedPipe) but not a write's room, and a write end
 * cannot see its reader gone until it writes; disk files are always ready, a console's input when
 * its handle is signalled. */
int ps_host_fs_poll(int h_file, unsigned events, unsigned *p_out_events, int64_t *p_out_cb_ready) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    *p_out_events = 0;
    *p_out_cb_ready = 0;
    if (descriptor.is_random) {
        *p_out_events = events & (PS_HOST_POLL_IN | PS_HOST_POLL_OUT);
        return 0;
    }
    switch (descriptor.type) {
    case FILE_TYPE_PIPE:
        if (descriptor.access & GENERIC_READ) {
            DWORD cb_available = 0;
            if (!PeekNamedPipe(descriptor.h_handle, NULL, 0, NULL, &cb_available, NULL)) {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE) return ps_win32_errno(error);
                *p_out_events |= PS_HOST_POLL_HANGUP;
            } else if (cb_available && events & PS_HOST_POLL_IN) {
                *p_out_events |= PS_HOST_POLL_IN;
                *p_out_cb_ready = cb_available;
            }
        }
        if (descriptor.access & GENERIC_WRITE) *p_out_events |= events & PS_HOST_POLL_OUT;
        return 0;
    case FILE_TYPE_CHAR:
        if (events & PS_HOST_POLL_IN && descriptor.access & GENERIC_READ &&
            WaitForSingleObject(descriptor.h_handle, 0) == WAIT_OBJECT_0)
            *p_out_events |= PS_HOST_POLL_IN;
        if (descriptor.access & GENERIC_WRITE) *p_out_events |= events & PS_HOST_POLL_OUT;
        return 0;
    default: *p_out_events = events & (PS_HOST_POLL_IN | PS_HOST_POLL_OUT); return 0;
    }
}

static int set_size(HANDLE h_handle, int64_t cb_file) {
    FILE_END_OF_FILE_INFO end = {.EndOfFile.QuadPart = cb_file};
    return SetFileInformationByHandle(h_handle, FileEndOfFileInfo, &end, sizeof(end)) ? 0
                                                                                      : ps_win32_errno(GetLastError());
}

int ps_host_fs_truncate(int h_file, int64_t cb_file) {
    if (cb_file < 0) return EINVAL;
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    if (descriptor.is_directory) return EISDIR;
    if (descriptor.type != FILE_TYPE_DISK || !(descriptor.access & GENERIC_WRITE)) return EINVAL;
    return set_size(descriptor.h_handle, cb_file);
}

int ps_host_fs_truncate_path(const char *sz_path, int64_t cb_file) {
    if (cb_file < 0) return EINVAL;
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    const HANDLE h_handle = CreateFileW(sz_wide, GENERIC_WRITE, SHARE_ALL, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h_handle == INVALID_HANDLE_VALUE) return ps_win32_errno(GetLastError());
    const int error_size = set_size(h_handle, cb_file);
    CloseHandle(h_handle);
    return error_size;
}

static struct timespec to_timespec(int64_t filetime) {
    const int64_t unix_ticks = filetime - FILETIME_UNIX_EPOCH;
    int64_t sec = unix_ticks / 10000000, ticks = unix_ticks % 10000000;
    if (ticks < 0) {
        --sec;
        ticks += 10000000;
    }
    return (struct timespec){.tv_sec = (time_t)sec, .tv_nsec = (long)(ticks * 100)};
}

static FILETIME to_filetime(const struct timespec *p_time) {
    const uint64_t filetime =
        (uint64_t)((int64_t)p_time->tv_sec * 10000000 + p_time->tv_nsec / 100 + FILETIME_UNIX_EPOCH);
    return (FILETIME){.dwLowDateTime = (DWORD)filetime, .dwHighDateTime = (DWORD)(filetime >> 32)};
}

/* Windows keeps no Unix mode: folders read 0755, files 0644, or 0444 when read-only; a symbolic
 * link or junction reads as a link. */
static int stat_handle(HANDLE h_handle, PsHostStat *p_out) {
    const DWORD type = GetFileType(h_handle);
    if (type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR) {
        *p_out = (PsHostStat){.mode = (type == FILE_TYPE_PIPE ? MODE_FIFO : MODE_CHARACTER) | 0620u,
                              .c_links = 1,
                              .cb_block = 4096};
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION info;
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    if (!GetFileInformationByHandle(h_handle, &info) ||
        !GetFileInformationByHandleEx(h_handle, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(h_handle, FileStandardInfo, &standard, sizeof(standard)))
        return ps_win32_errno(GetLastError());
    uint32_t mode = PS_HOST_MODE_REGULAR | (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY ? 0444u : 0644u);
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) mode = PS_HOST_MODE_DIRECTORY | 0755u;
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT &&
        GetFileInformationByHandleEx(h_handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT))
        mode = PS_HOST_MODE_LINK | 0777u;
    *p_out = (PsHostStat){
        .dev = info.dwVolumeSerialNumber,
        .ino = (uint64_t)info.nFileIndexHigh << 32 | info.nFileIndexLow,
        .mode = mode,
        .c_links = (uint32_t)info.nNumberOfLinks,
        .cb_block = 4096,
        .cb_size = PS_HOST_MODE_IS_DIRECTORY(mode) ? 0 : standard.EndOfFile.QuadPart,
        .c_blocks = standard.AllocationSize.QuadPart / 512,
        .atime = to_timespec(basic.LastAccessTime.QuadPart),
        .mtime = to_timespec(basic.LastWriteTime.QuadPart),
        .ctime = to_timespec(basic.ChangeTime.QuadPart),
    };
    return 0;
}

static int stat_wide(const wchar_t *sz_wide, int is_following_links, PsHostStat *p_out) {
    const HANDLE h_handle = open_existing(sz_wide, FILE_READ_ATTRIBUTES, is_following_links);
    if (h_handle == INVALID_HANDLE_VALUE) return ps_win32_errno(GetLastError());
    const int error = stat_handle(h_handle, p_out);
    CloseHandle(h_handle);
    return error;
}

static int stat_path(const char *sz_path, int is_following_links, PsHostStat *p_out) {
    if (is_random_device(sz_path)) {
        stat_random_device(p_out);
        return 0;
    }
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    return error ? error : stat_wide(sz_wide, is_following_links, p_out);
}

int ps_host_fs_stat(const char *sz_path, PsHostStat *p_out) { return stat_path(sz_path, 1, p_out); }

int ps_host_fs_stat_link(const char *sz_path, PsHostStat *p_out) { return stat_path(sz_path, 0, p_out); }

int ps_host_fs_stat_file(int h_file, PsHostStat *p_out) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    if (descriptor.is_random) {
        stat_random_device(p_out);
        return 0;
    }
    return stat_handle(descriptor.h_handle, p_out);
}

/* Only the read-only attribute denies anything Windows' permission bits would: write access to a
 * read-only file. */
static HANDLE stream_handle(FILE *p_stream) {
    const intptr_t h_handle = _get_osfhandle(_fileno(p_stream));
    return h_handle == -1 ? NULL : (HANDLE)h_handle;
}

int ps_host_fs_stat_stream(FILE *p_stream, PsHostStat *p_out) {
    const HANDLE h_handle = stream_handle(p_stream);
    return h_handle ? stat_handle(h_handle, p_out) : EBADF;
}

int ps_host_fs_sync_stream(FILE *p_stream) {
    const HANDLE h_handle = stream_handle(p_stream);
    if (!h_handle) return EBADF;
    return FlushFileBuffers(h_handle) ? 0 : ps_win32_errno(GetLastError());
}

int ps_host_fs_truncate_stream(FILE *p_stream, int64_t cb_file) {
    const HANDLE h_handle = stream_handle(p_stream);
    return h_handle ? set_size(h_handle, cb_file) : EBADF;
}

int ps_host_fs_access(const char *sz_path, int mode) {
    if (is_random_device(sz_path)) return 0;
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    const DWORD attributes = GetFileAttributesW(sz_wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) return ps_win32_errno(GetLastError());
    const int is_read_only = (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) == FILE_ATTRIBUTE_READONLY;
    return mode & PS_HOST_ACCESS_WRITE && is_read_only ? EACCES : 0;
}

int ps_host_fs_make_dir(const char *sz_path, unsigned mode) {
    (void)mode;
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    return CreateDirectoryW(sz_wide, NULL) ? 0 : ps_win32_errno(GetLastError());
}

int ps_host_fs_make_unique_dir(char *sz_template) {
    const size_t cb_template = strlen(sz_template);
    if (cb_template < 6 || strcmp(sz_template + cb_template - 6, "XXXXXX")) return EINVAL;
    static const char s_alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char *p_unique = sz_template + cb_template - 6;
    for (int i_attempt = 0; i_attempt < 100; ++i_attempt) {
        unsigned char random[6];
        const int error_random = ps_host_random_bytes(random, sizeof(random));
        if (error_random) return error_random;
        for (size_t i = 0; i < sizeof(random); ++i) p_unique[i] = s_alphabet[random[i] % (sizeof(s_alphabet) - 1)];
        const int error = ps_host_fs_make_dir(sz_template, 0700);
        if (error != EEXIST) return error;
    }
    return EEXIST;
}

int ps_host_fs_remove_dir(const char *sz_path) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    return RemoveDirectoryW(sz_wide) ? 0 : ps_win32_errno(GetLastError());
}

/* unlink(2) ignores the file's own permissions, which DeleteFileW does not: a read-only file loses
 * that attribute first. */
int ps_host_fs_remove(const char *sz_path) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    if (DeleteFileW(sz_wide)) return 0;
    const DWORD error_delete = GetLastError();
    const DWORD attributes = GetFileAttributesW(sz_wide);
    if (error_delete != ERROR_ACCESS_DENIED || attributes == INVALID_FILE_ATTRIBUTES)
        return ps_win32_errno(error_delete);
    /* A link to a folder is a folder to DeleteFileW; RemoveDirectoryW takes it away without
     * touching what it points to, as unlink(2) does a link. */
    if (attributes & FILE_ATTRIBUTE_DIRECTORY && attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return RemoveDirectoryW(sz_wide) ? 0 : ps_win32_errno(GetLastError());
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return EISDIR;
    if (!(attributes & FILE_ATTRIBUTE_READONLY) || !SetFileAttributesW(sz_wide, attributes & ~FILE_ATTRIBUTE_READONLY))
        return ps_win32_errno(error_delete);
    if (DeleteFileW(sz_wide)) return 0;
    const DWORD error_retry = GetLastError();
    (void)SetFileAttributesW(sz_wide, attributes); /* best effort: the error below is what the caller needs */
    return ps_win32_errno(error_retry);
}

int ps_host_fs_rename(const char *sz_from, const char *sz_to) {
    wchar_t sz_wide_from[PS_WIN32_PATH_MAX], sz_wide_to[PS_WIN32_PATH_MAX];
    int error = wide_path(sz_from, sz_wide_from, PS_WIN32_PATH_MAX);
    if (!error) error = wide_path(sz_to, sz_wide_to, PS_WIN32_PATH_MAX);
    if (error) return error;
    return MoveFileExW(sz_wide_from, sz_wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0
               : ps_win32_errno(GetLastError());
}

/* REPARSE_DATA_BUFFER's mount point form, which only the driver kit's headers declare. */
typedef struct {
    DWORD tag;
    WORD cb_data, reserved;
    WORD off_substitute, cb_substitute, off_print, cb_print;
    wchar_t sz_paths[(MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 16) / sizeof(wchar_t)];
} MountPointReparse;

/* A junction to sz_existing, which must be a folder on a local volume: 0 or the Win32 error. */
static DWORD make_junction(const wchar_t *sz_link, const wchar_t *sz_existing) {
    wchar_t sz_full[PS_WIN32_PATH_MAX];
    const DWORD c_full = GetFullPathNameW(sz_existing, PS_WIN32_PATH_MAX, sz_full, NULL);
    if (!c_full) return GetLastError();
    if (c_full >= PS_WIN32_PATH_MAX) return ERROR_FILENAME_EXCED_RANGE;
    const wchar_t *sz_print = wcsncmp(sz_full, L"\\\\?\\", 4) ? sz_full : sz_full + 4;
    /* A junction holds a drive's NT name; a share has none it can hold. */
    if (!wcsncmp(sz_print, L"UNC\\", 4) || sz_print[0] == L'\\') return ERROR_NOT_SUPPORTED;
    static const wchar_t s_nt_prefix[] = L"\\??\\";
    const size_t c_print = wcslen(sz_print), c_prefix = sizeof(s_nt_prefix) / sizeof(*s_nt_prefix) - 1;
    MountPointReparse reparse = {.tag = IO_REPARSE_TAG_MOUNT_POINT};
    if (c_prefix + c_print + 1 + c_print + 1 > sizeof(reparse.sz_paths) / sizeof(*reparse.sz_paths))
        return ERROR_FILENAME_EXCED_RANGE;
    wmemcpy(reparse.sz_paths, s_nt_prefix, c_prefix);
    wmemcpy(reparse.sz_paths + c_prefix, sz_print, c_print + 1);
    wmemcpy(reparse.sz_paths + c_prefix + c_print + 1, sz_print, c_print + 1);
    reparse.cb_substitute = (WORD)((c_prefix + c_print) * sizeof(wchar_t));
    reparse.off_print = (WORD)(reparse.cb_substitute + sizeof(wchar_t));
    reparse.cb_print = (WORD)(c_print * sizeof(wchar_t));
    const DWORD cb_paths = reparse.off_print + reparse.cb_print + sizeof(wchar_t);
    reparse.cb_data = (WORD)(offsetof(MountPointReparse, sz_paths) - offsetof(MountPointReparse, off_substitute) + cb_paths);
    if (!CreateDirectoryW(sz_link, NULL)) return GetLastError();
    const HANDLE h_link = CreateFileW(sz_link, GENERIC_WRITE, SHARE_ALL, NULL, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD error = h_link == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    DWORD cb_returned;
    if (!error && !DeviceIoControl(h_link, FSCTL_SET_REPARSE_POINT, &reparse,
                                   (DWORD)offsetof(MountPointReparse, sz_paths) + cb_paths, NULL, 0, &cb_returned, NULL))
        error = GetLastError();
    if (h_link != INVALID_HANDLE_VALUE) CloseHandle(h_link);
    if (error) (void)RemoveDirectoryW(sz_link); /* the empty folder made above; the error is what counts */
    return error;
}

/* Without the privilege for symbolic links what a link would show is made another way: a folder as
 * a junction, a file as a hard link (both read the target live), or, across volumes, as a copy. */
static int link_unprivileged(const char *sz_target, const char *sz_link, const wchar_t *sz_wide_existing,
                             const wchar_t *sz_wide_link, int is_directory) {
    static LONG s_is_reported, s_c_copies;
    if (!InterlockedExchange(&s_is_reported, 1))
        fprintf(stderr,
                "Host: CreateSymbolicLinkW: Win32 error %lu (no Developer Mode or SeCreateSymbolicLinkPrivilege): folder "
                "links become junctions, file links hard links, or copies across volumes\n",
                (unsigned long)ERROR_PRIVILEGE_NOT_HELD);
    if (is_directory) {
        const DWORD error = make_junction(sz_wide_link, sz_wide_existing);
        if (error) fprintf(stderr, "Host: junction %s -> %s: Win32 error %lu\n", sz_link, sz_target, (unsigned long)error);
        return ps_win32_errno(error);
    }
    if (CreateHardLinkW(sz_wide_link, sz_wide_existing, NULL)) return 0;
    const DWORD error_hard = GetLastError();
    if (!CopyFileW(sz_wide_existing, sz_wide_link, TRUE)) {
        const DWORD error_copy = GetLastError();
        fprintf(stderr, "Host: link %s -> %s: hard link Win32 error %lu, copy Win32 error %lu\n", sz_link, sz_target,
                (unsigned long)error_hard, (unsigned long)error_copy);
        return ps_win32_errno(error_copy);
    }
    fprintf(stderr, "Host: %s copied to %s (copy %ld): no hard link, Win32 error %lu\n", sz_target, sz_link,
            InterlockedIncrement(&s_c_copies), (unsigned long)error_hard);
    return 0;
}

/* Windows links to folders and to files are different kinds: the target, relative to the link's
 * folder, says which. Without the developer mode or the privilege the link is made unprivileged
 * (link_unprivileged), and a file's then reads as a regular file. */
int ps_host_fs_symlink(const char *sz_target, const char *sz_link) {
    wchar_t sz_wide_target[PS_WIN32_PATH_MAX], sz_wide_link[PS_WIN32_PATH_MAX], sz_resolved[PS_WIN32_PATH_MAX];
    int error = ps_win32_to_wide(sz_target, sz_wide_target, PS_WIN32_PATH_MAX);
    if (!error) error = wide_path(sz_link, sz_wide_link, PS_WIN32_PATH_MAX);
    if (error) return error;
    for (wchar_t *p_char = sz_wide_target; *p_char; ++p_char)
        if (*p_char == L'/') *p_char = L'\\';
    const int is_absolute = sz_wide_target[0] == L'\\' || (sz_wide_target[0] && sz_wide_target[1] == L':');
    const wchar_t *p_folder_end = wcsrchr(sz_wide_link, L'\\');
    const int c_folder = is_absolute || !p_folder_end ? 0 : (int)(p_folder_end - sz_wide_link + 1);
    /* An absolute target through wide_path too, so that the hard link or copy reads it past MAX_PATH. */
    if (is_absolute) error = wide_path(sz_target, sz_resolved, PS_WIN32_PATH_MAX);
    else if (swprintf(sz_resolved, PS_WIN32_PATH_MAX, L"%.*ls%ls", c_folder, sz_wide_link, sz_wide_target) < 0)
        error = ENAMETOOLONG;
    if (error) return error;
    const DWORD attributes = GetFileAttributesW(sz_resolved);
    const int is_directory = attributes != INVALID_FILE_ATTRIBUTES && attributes & FILE_ATTRIBUTE_DIRECTORY;
    const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (is_directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (CreateSymbolicLinkW(sz_wide_link, sz_wide_target, flags)) return 0;
    const DWORD error_link = GetLastError();
    if (error_link != ERROR_PRIVILEGE_NOT_HELD) return ps_win32_errno(error_link);
    return link_unprivileged(sz_target, sz_link, sz_resolved, sz_wide_link, is_directory);
}

/* GetDiskFreeSpaceExW takes folders only: a file's answer is its folder's. */
int ps_host_fs_free_space(const char *sz_path, uint64_t *p_out_cb_free) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    ULARGE_INTEGER cb_available;
    BOOL is_read = GetDiskFreeSpaceExW(sz_wide, &cb_available, NULL, NULL);
    wchar_t *p_separator = wcsrchr(sz_wide, L'\\');
    if (!is_read && GetLastError() == ERROR_DIRECTORY && p_separator) {
        p_separator[1] = 0;
        is_read = GetDiskFreeSpaceExW(sz_wide, &cb_available, NULL, NULL);
    }
    if (!is_read) return ps_win32_errno(GetLastError());
    *p_out_cb_free = cb_available.QuadPart;
    return 0;
}

int ps_host_fs_real_path(const char *sz_path, char *sz_out, size_t cb_out) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    const HANDLE h_handle = open_existing(sz_wide, FILE_READ_ATTRIBUTES, 1);
    if (h_handle == INVALID_HANDLE_VALUE) return ps_win32_errno(GetLastError());
    const DWORD c_final = GetFinalPathNameByHandleW(h_handle, sz_wide, PS_WIN32_PATH_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const DWORD error_final = GetLastError();
    CloseHandle(h_handle);
    if (!c_final) return ps_win32_errno(error_final);
    if (c_final >= PS_WIN32_PATH_MAX) return ENAMETOOLONG;
    /* The answer always has the \\?\ prefix (\\?\UNC\ for a share), which psport's paths do not. */
    wchar_t *sz_plain = sz_wide;
    if (!wcsncmp(sz_wide, L"\\\\?\\UNC\\", 8)) {
        sz_plain = sz_wide + 6;
        sz_plain[0] = L'\\';
    } else if (!wcsncmp(sz_wide, L"\\\\?\\", 4)) {
        sz_plain = sz_wide + 4;
    }
    const int error_utf8 = ps_win32_to_utf8(sz_plain, sz_out, cb_out);
    if (error_utf8) return error_utf8;
    for (char *p_char = sz_out; *p_char; ++p_char)
        if (*p_char == '\\') *p_char = '/';
    return 0;
}

static int set_times_handle(HANDLE h_handle, const struct timespec *p_times) {
    FILETIME times[2];
    if (p_times) {
        times[0] = to_filetime(&p_times[0]);
        times[1] = to_filetime(&p_times[1]);
    } else {
        GetSystemTimePreciseAsFileTime(&times[0]);
        times[1] = times[0];
    }
    return SetFileTime(h_handle, NULL, &times[0], &times[1]) ? 0 : ps_win32_errno(GetLastError());
}

int ps_host_fs_set_times(const char *sz_path, const struct timespec *p_times) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    const HANDLE h_handle = open_existing(sz_wide, FILE_WRITE_ATTRIBUTES, 1);
    if (h_handle == INVALID_HANDLE_VALUE) return ps_win32_errno(GetLastError());
    const int error_times = set_times_handle(h_handle, p_times);
    CloseHandle(h_handle);
    return error_times;
}

/* A handle of its own with the attribute access that the descriptor's may lack, as futimes and
 * fchmod need none on Linux: 0, or the host's errno. */
static int attribute_handle(int h_file, HANDLE *p_out) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    if (descriptor.type != FILE_TYPE_DISK) return EINVAL;
    *p_out = ReOpenFile(descriptor.h_handle, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, SHARE_ALL,
                        descriptor.is_directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    return *p_out == INVALID_HANDLE_VALUE ? ps_win32_errno(GetLastError()) : 0;
}

int ps_host_fs_set_times_file(int h_file, const struct timespec *p_times) {
    HANDLE h_attributes;
    const int error = attribute_handle(h_file, &h_attributes);
    if (error) return error;
    const int error_times = set_times_handle(h_attributes, p_times);
    CloseHandle(h_attributes);
    return error_times;
}

static DWORD mode_attributes(DWORD attributes, unsigned mode) {
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return attributes;
    return mode & 0200 ? attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY : attributes | FILE_ATTRIBUTE_READONLY;
}

/* Only the owner's write bit has a Windows counterpart, the read-only attribute of files. */
int ps_host_fs_set_mode(const char *sz_path, unsigned mode) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return error;
    const DWORD attributes = GetFileAttributesW(sz_wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) return ps_win32_errno(GetLastError());
    const DWORD wanted = mode_attributes(attributes, mode);
    return wanted == attributes || SetFileAttributesW(sz_wide, wanted) ? 0 : ps_win32_errno(GetLastError());
}

int ps_host_fs_set_mode_file(int h_file, unsigned mode) {
    HANDLE h_attributes;
    const int error = attribute_handle(h_file, &h_attributes);
    if (error) return error;
    FILE_BASIC_INFO basic;
    int error_mode = 0;
    if (!GetFileInformationByHandleEx(h_attributes, FileBasicInfo, &basic, sizeof(basic))) {
        error_mode = ps_win32_errno(GetLastError());
    } else {
        /* Zero times leave them as they are. */
        const FILE_BASIC_INFO wanted = {.FileAttributes = mode_attributes(basic.FileAttributes, mode)};
        if (wanted.FileAttributes != basic.FileAttributes &&
            !SetFileInformationByHandle(h_attributes, FileBasicInfo, (void *)&wanted, sizeof(wanted)))
            error_mode = ps_win32_errno(GetLastError());
    }
    CloseHandle(h_attributes);
    return error_mode;
}

int ps_host_fs_status(int h_file, unsigned *p_out_flags) {
    Descriptor descriptor;
    if (descriptor_get(h_file, &descriptor)) return EBADF;
    *p_out_flags = descriptor.flags;
    return 0;
}

/* Windows has no non-blocking mode for files or anonymous pipes: the flag is kept for what reads it
 * back, as Linux keeps O_NONBLOCK on regular files without effect. */
int ps_host_fs_set_status(int h_file, unsigned flags) {
    if (!is_valid_descriptor(h_file)) return EBADF;
    ensure_stdio();
    const unsigned changeable = PS_HOST_OPEN_NONBLOCK | PS_HOST_OPEN_APPEND;
    AcquireSRWLockExclusive(&s_lock);
    Descriptor *p_slot = &s_descriptors[h_file];
    const int is_open = p_slot->h_handle != NULL;
    if (is_open) p_slot->flags = (p_slot->flags & ~changeable) | (flags & changeable);
    ReleaseSRWLockExclusive(&s_lock);
    return is_open ? 0 : EBADF;
}

int ps_host_fs_close_on_exec(int h_file) {
    Descriptor descriptor;
    return descriptor_get(h_file, &descriptor) ? -EBADF : descriptor.is_close_on_exec;
}

int ps_host_fs_set_close_on_exec(int h_file, int is_set) {
    if (!is_valid_descriptor(h_file)) return EBADF;
    ensure_stdio();
    AcquireSRWLockExclusive(&s_lock);
    Descriptor *p_slot = &s_descriptors[h_file];
    const int is_open = p_slot->h_handle != NULL;
    if (is_open) p_slot->is_close_on_exec = is_set != 0;
    ReleaseSRWLockExclusive(&s_lock);
    return is_open ? 0 : EBADF;
}

/* The C runtime's stdio (printf, perror) writes through its own descriptors 0-2 and GetStdHandle:
 * both follow a duplicate onto 0-2, as they do after dup2 on Linux. */
static void redirect_standard(int h_target, HANDLE h_handle) {
    static const DWORD s_standard[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE h_crt;
    if (!DuplicateHandle(GetCurrentProcess(), h_handle, GetCurrentProcess(), &h_crt, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    const int h_crt_file = _open_osfhandle((intptr_t)h_crt, h_target ? _O_WRONLY | _O_BINARY : _O_RDONLY | _O_BINARY);
    if (h_crt_file < 0) {
        CloseHandle(h_crt);
        return;
    }
    if (h_crt_file != h_target) {
        (void)_dup2(h_crt_file, h_target); /* on failure the CRT keeps writing where it did */
        _close(h_crt_file);
    }
    SetStdHandle(s_standard[h_target], (HANDLE)_get_osfhandle(h_target));
}

int ps_host_fs_duplicate(int h_file, int h_target) {
    if (h_target >= DESCRIPTOR_MAX) return -EBADF;
    Descriptor source;
    if (descriptor_get(h_file, &source)) return -EBADF;
    if (h_file == h_target) return h_target;
    HANDLE h_copy;
    if (!DuplicateHandle(GetCurrentProcess(), source.h_handle, GetCurrentProcess(), &h_copy, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return (int)win32_failure();
    Descriptor descriptor = source;
    descriptor.h_handle = h_copy;
    descriptor.h_positional = NULL;
    descriptor.is_close_on_exec = h_target < 0;
    if (h_target < 0) return descriptor_add(&descriptor, 3);
    AcquireSRWLockExclusive(&s_lock);
    const Descriptor replaced = s_descriptors[h_target];
    s_descriptors[h_target] = descriptor;
    ReleaseSRWLockExclusive(&s_lock);
    descriptor_release(&replaced);
    if (h_target < 3) redirect_standard(h_target, h_copy);
    return h_target;
}

/* Nothing but descriptors 0-2 reaches a new process on Windows (ps_host_exec): marking the rest
 * close-on-exec is bookkeeping for ps_host_fs_close_on_exec. */
int ps_host_close_from(int h_first, int h_last, int is_on_exec) {
    if (h_first < 0) return EBADF;
    ensure_stdio();
    const int h_end = h_last < 0 || h_last >= DESCRIPTOR_MAX ? DESCRIPTOR_MAX - 1 : h_last;
    for (int h_file = h_first; h_file <= h_end; ++h_file) {
        AcquireSRWLockExclusive(&s_lock);
        Descriptor *p_slot = &s_descriptors[h_file];
        const Descriptor closed = is_on_exec ? (Descriptor){0} : *p_slot;
        if (is_on_exec)
            p_slot->is_close_on_exec = p_slot->h_handle != NULL;
        else
            *p_slot = (Descriptor){0};
        ReleaseSRWLockExclusive(&s_lock);
        descriptor_release(&closed);
    }
    return 0;
}

struct PsHostDir {
    HANDLE h_find;
    int is_started, is_done;
    WIN32_FIND_DATAW data;
    /* UTF-8 takes up to 3 bytes for each UTF-16 unit of cFileName. */
    char sz_name[MAX_PATH * 3 + 1];
    size_t c_folder;
    wchar_t sz_folder[];
};

PsHostDir *ps_host_dir_open(const char *sz_path) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX - 2);
    if (error) {
        errno = error;
        return NULL;
    }
    size_t c_folder = wcslen(sz_wide);
    if (c_folder && sz_wide[c_folder - 1] == L'\\') --c_folder;
    PsHostDir *p_dir = malloc(sizeof(*p_dir) + (c_folder + 3) * sizeof(wchar_t));
    if (!p_dir) {
        errno = ENOMEM;
        return NULL;
    }
    p_dir->c_folder = c_folder;
    p_dir->is_started = p_dir->is_done = 0;
    wmemcpy(p_dir->sz_folder, sz_wide, c_folder);
    wmemcpy(p_dir->sz_folder + c_folder, L"\\*", 3);
    p_dir->h_find = FindFirstFileExW(p_dir->sz_folder, FindExInfoBasic, &p_dir->data, FindExSearchNameMatch, NULL,
                                     FIND_FIRST_EX_LARGE_FETCH);
    p_dir->sz_folder[c_folder] = 0;
    if (p_dir->h_find != INVALID_HANDLE_VALUE) return p_dir;
    const DWORD error_find = GetLastError();
    const DWORD attributes = GetFileAttributesW(p_dir->sz_folder);
    /* A drive's root has no "." or "..": empty, it finds nothing. */
    if (error_find == ERROR_FILE_NOT_FOUND && attributes != INVALID_FILE_ATTRIBUTES &&
        attributes & FILE_ATTRIBUTE_DIRECTORY) {
        p_dir->is_done = 1;
        return p_dir;
    }
    error = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? ENOTDIR
                                                                                              : ps_win32_errno(error_find);
    free(p_dir);
    errno = error;
    return NULL;
}

int ps_host_dir_next(PsHostDir *p_dir, PsHostDirEntry *p_out) {
    if (p_dir->is_done) return 0;
    if (p_dir->is_started && !FindNextFileW(p_dir->h_find, &p_dir->data)) {
        p_dir->is_done = 1;
        return 0;
    }
    p_dir->is_started = 1;
    const DWORD attributes = p_dir->data.dwFileAttributes;
    /* A name that is not valid UTF-16 comes out with U+FFFD in it, visible in listings, rather than
     * left out: opening it fails with ENOENT. */
    if (!WideCharToMultiByte(CP_UTF8, 0, p_dir->data.cFileName, -1, p_dir->sz_name, sizeof(p_dir->sz_name), NULL, NULL))
        snprintf(p_dir->sz_name, sizeof(p_dir->sz_name), "?");
    p_out->sz_name = p_dir->sz_name;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT &&
        (p_dir->data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || p_dir->data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
        p_out->kind = PS_HOST_ENTRY_LINK;
    else if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        p_out->kind = PS_HOST_ENTRY_DIRECTORY;
    else if (attributes & FILE_ATTRIBUTE_DEVICE)
        p_out->kind = PS_HOST_ENTRY_OTHER;
    else
        p_out->kind = PS_HOST_ENTRY_REGULAR;
    return 1;
}

int ps_host_dir_stat(PsHostDir *p_dir, const char *sz_name, PsHostStat *p_out) {
    wchar_t sz_wide_name[MAX_PATH * 2], sz_wide[PS_WIN32_PATH_MAX];
    const int error = ps_win32_to_wide(sz_name, sz_wide_name, sizeof(sz_wide_name) / sizeof(*sz_wide_name));
    if (error) return error;
    if (swprintf(sz_wide, PS_WIN32_PATH_MAX, L"%ls\\%ls", p_dir->sz_folder, sz_wide_name) < 0) return ENAMETOOLONG;
    return stat_wide(sz_wide, 1, p_out);
}

void ps_host_dir_close(PsHostDir *p_dir) {
    if (p_dir->h_find != INVALID_HANDLE_VALUE) FindClose(p_dir->h_find);
    free(p_dir);
}

/* fopen takes the ANSI code page's path, which cannot name every UTF-8 one. */
FILE *ps_host_fs_open_stream(const char *sz_path, const char *sz_mode) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX], sz_wide_mode[16];
    int error = wide_path(sz_path, sz_wide, PS_WIN32_PATH_MAX);
    if (!error) error = ps_win32_to_wide(sz_mode, sz_wide_mode, sizeof(sz_wide_mode) / sizeof(*sz_wide_mode));
    if (error) {
        errno = error;
        return NULL;
    }
    return _wfopen(sz_wide, sz_wide_mode);
}
