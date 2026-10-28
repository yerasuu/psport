// SPDX-License-Identifier: GPL-2.0-or-later
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

/* ps_host_vm_section_create's descriptors start here, above any host_file.c hands out, so that
 * ps_host_fs_close passes them to ps_win32_section_close (host_memory.c). */
#define PS_WIN32_SECTION_BASE 0x50000000
/* 0, or EBADF when h_section is not an open section. Views of it stay valid until unmapped. */
int ps_win32_section_close(int h_section);

/* Around a wait on an address (host_thread.c), so that an interrupt wakes the thread to run
 * (host_fault.c). */
void ps_win32_wait_begin(const void *p_word);
void ps_win32_wait_end(void);

/* psport's int descriptors are slots of host_file.c's table over Win32 handles, 0-2 copies of the
 * standard handles. The handle stays the table's: NULL when the descriptor is not open, or, with
 * is_for_exec, when it is marked close-on-exec. */
HANDLE ps_win32_fd_handle(int h_file, int is_for_exec);
/* Hands a handle the caller owns (a section, a pipe) to the table, close-on-exec, with flags as
 * ps_host_fs_open's access mode: the descriptor, or minus EMFILE with the handle closed. */
int ps_win32_fd_adopt(HANDLE h_handle, unsigned flags);

#endif
