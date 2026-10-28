// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: Win32 errors as errno values and UTF-8 conversions (host_win32.h). */
#include "host_win32.h"
#include <errno.h>
#include <limits.h>

int ps_win32_errno(DWORD error) {
    switch (error) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_MOD_NOT_FOUND:
    case ERROR_PROC_NOT_FOUND:
    case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_FLAGS: return EINVAL;
    case ERROR_INSUFFICIENT_BUFFER:
    case ERROR_MORE_DATA: return ERANGE;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT: return ETIMEDOUT;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED: return ENOSYS;
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH: return ENOEXEC;
    case ERROR_NO_UNICODE_TRANSLATION: return EILSEQ;
    case ERROR_NOT_READY:
    case ERROR_BUSY: return EBUSY;
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_PATHNAME: return ENOENT;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA: return EPIPE;
    case ERROR_NEGATIVE_SEEK: return EINVAL;
    case ERROR_PRIVILEGE_NOT_HELD: return EPERM;
    case ERROR_WRITE_PROTECT: return EROFS;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_CANT_RESOLVE_FILENAME: return ELOOP;
    }
    return EIO;
}

static int conversion_error(void) {
    return GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : EILSEQ;
}

int ps_win32_to_wide(const char *sz_utf8, wchar_t *sz_out, size_t c_out) {
    const int c_room = c_out > INT_MAX ? INT_MAX : (int)c_out;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, sz_utf8, -1, sz_out, c_room) ? 0 : conversion_error();
}

int ps_win32_to_utf8(const wchar_t *sz_wide, char *sz_out, size_t cb_out) {
    const int cb_room = cb_out > INT_MAX ? INT_MAX : (int)cb_out;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, sz_wide, -1, sz_out, cb_room, NULL, NULL)
               ? 0
               : conversion_error();
}
