/* Windows: what the win32 files share, Win32 errors as errno values and UTF-8 paths as the wide
 * strings the W APIs take. Not part of ps_host.h. */
#ifndef PS_HOST_WIN32_H
#define PS_HOST_WIN32_H
#include <windows.h>
#include <stddef.h>

/* Long enough for any path psport builds; the W APIs take longer ones only with the \\?\ prefix. */
#define PS_WIN32_PATH_MAX 4096

/* The errno value whose meaning matches a GetLastError code; EIO for codes without one. */
int ps_win32_errno(DWORD error);
/* 0, ENAMETOOLONG when the result does not fit with its NUL, or EILSEQ for invalid input. */
int ps_win32_to_wide(const char *sz_utf8, wchar_t *sz_out, size_t c_out);
int ps_win32_to_utf8(const wchar_t *sz_wide, char *sz_out, size_t cb_out);

#endif
