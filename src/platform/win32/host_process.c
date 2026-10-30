// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: folder listings, exec as a new process and the output helper (ps_host.h). */
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* FindFirstFileExW allocates, which the contract's "without allocating" is there to avoid in a
 * signal handler: no Windows handler interrupts a thread inside the heap, since the watchdog and
 * the thread dumps run on threads of their own. */
int ps_host_list_dir(const char *sz_path, void (*pfn_entry)(const char *sz_name, void *p_context), void *p_context) {
    wchar_t sz_pattern[PS_WIN32_PATH_MAX];
    int error = ps_win32_to_wide(sz_path, sz_pattern, PS_WIN32_PATH_MAX - 2);
    if (error) return error;
    size_t c_pattern = wcslen(sz_pattern);
    for (size_t i = 0; i < c_pattern; ++i)
        if (sz_pattern[i] == L'/') sz_pattern[i] = L'\\';
    if (c_pattern && sz_pattern[c_pattern - 1] == L'\\') --c_pattern;
    wmemcpy(sz_pattern + c_pattern, L"\\*", 3);
    WIN32_FIND_DATAW data;
    const HANDLE h_find =
        FindFirstFileExW(sz_pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h_find == INVALID_HANDLE_VALUE) {
        const DWORD error_find = GetLastError();
        return error_find == ERROR_FILE_NOT_FOUND ? 0 : ps_win32_errno(error_find);
    }
    char sz_name[MAX_PATH * 3 + 1];
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        /* As ps_host_dir_next: a name that is not valid UTF-16 comes with U+FFFD rather than left out. */
        if (WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, sz_name, sizeof(sz_name), NULL, NULL))
            pfn_entry(sz_name, p_context);
    } while (FindNextFileW(h_find, &data));
    const DWORD error_next = GetLastError();
    error = error_next == ERROR_NO_MORE_FILES ? 0 : ps_win32_errno(error_next);
    FindClose(h_find);
    return error;
}

/* Windows hands a new process one command line, which its C runtime splits again
 * (CommandLineToArgvW's rules): an argument with blanks or quotes is quoted, the backslashes before
 * a quote doubled. 0, or E2BIG when the line would pass Windows' 32767 characters. */
#define COMMAND_LINE_MAX 32767
static int put(wchar_t *sz_line, size_t *p_c_used, wchar_t c) {
    if (*p_c_used >= COMMAND_LINE_MAX - 1) return E2BIG;
    sz_line[(*p_c_used)++] = c;
    sz_line[*p_c_used] = 0;
    return 0;
}

static int append_argument(wchar_t *sz_line, size_t *p_c_used, const wchar_t *sz_argument) {
    const int is_quoted = !*sz_argument || wcspbrk(sz_argument, L" \t\n\v\"") != NULL;
    int error = *p_c_used ? put(sz_line, p_c_used, L' ') : 0;
    if (!error && is_quoted) error = put(sz_line, p_c_used, L'"');
    for (const wchar_t *p_char = sz_argument; !error; ++p_char) {
        size_t c_backslashes = 0;
        while (*p_char == L'\\') {
            ++c_backslashes;
            ++p_char;
        }
        const int is_end = !*p_char;
        if (is_quoted && (is_end || *p_char == L'"')) c_backslashes *= 2;
        for (size_t i = 0; !error && i < c_backslashes; ++i) error = put(sz_line, p_c_used, L'\\');
        if (error || is_end) break;
        if (*p_char == L'"') error = put(sz_line, p_c_used, L'\\');
        if (!error) error = put(sz_line, p_c_used, *p_char);
    }
    if (!error && is_quoted) error = put(sz_line, p_c_used, L'"');
    return error;
}

static int build_command_line(char *const *pp_argv, wchar_t *sz_line, wchar_t *sz_argument) {
    size_t c_used = 0;
    sz_line[0] = 0;
    for (char *const *pp_next = pp_argv; *pp_next; ++pp_next) {
        int error = ps_win32_to_wide(*pp_next, sz_argument, COMMAND_LINE_MAX);
        if (error == ENAMETOOLONG) error = E2BIG;
        if (!error) error = append_argument(sz_line, &c_used, sz_argument);
        if (error) return error;
    }
    return 0;
}

/* Descriptors 0-2 that are not close-on-exec become the new process's standard handles, through
 * inheritable copies listed for it alone, so that no other handle of this process leaks into it. */
static int start_process(const wchar_t *sz_application, wchar_t *sz_line, PROCESS_INFORMATION *p_out) {
    HANDLE h_inherited[3] = {NULL, NULL, NULL}, h_listed[3];
    DWORD c_listed = 0;
    for (int i = 0; i < 3; ++i) {
        const HANDLE h_standard = ps_win32_fd_handle(i, 1);
        if (h_standard && DuplicateHandle(GetCurrentProcess(), h_standard, GetCurrentProcess(), &h_inherited[i], 0, TRUE,
                                          DUPLICATE_SAME_ACCESS))
            h_listed[c_listed++] = h_inherited[i];
    }
    STARTUPINFOEXW startup = {.StartupInfo = {.cb = sizeof(startup),
                                              .dwFlags = STARTF_USESTDHANDLES,
                                              .hStdInput = h_inherited[0],
                                              .hStdOutput = h_inherited[1],
                                              .hStdError = h_inherited[2]}};
    SIZE_T cb_attributes = 0;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &cb_attributes); /* only asks for the size */
    int error = 0;
    if (c_listed) {
        startup.lpAttributeList = malloc(cb_attributes);
        if (!startup.lpAttributeList)
            error = ENOMEM;
        else if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &cb_attributes)) {
            error = ps_win32_errno(GetLastError());
            free(startup.lpAttributeList);
            startup.lpAttributeList = NULL;
        } else if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, h_listed,
                                              c_listed * sizeof(*h_listed), NULL, NULL))
            error = ps_win32_errno(GetLastError());
    }
    if (!error && !CreateProcessW(sz_application, sz_line, NULL, NULL, c_listed != 0,
                                  EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                  &startup.StartupInfo, p_out))
        error = ps_win32_errno(GetLastError());
    if (startup.lpAttributeList) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
    }
    for (int i = 0; i < 3; ++i)
        if (h_inherited[i]) CloseHandle(h_inherited[i]);
    return error;
}

/* Windows cannot replace a process's image: the new one starts and this one waits for it and ends
 * with its exit code, so that a launcher waiting on this process sees the run end when the new one
 * does. This process keeps what it holds (the Vulkan device among it) until then. */
static int replace_process(const wchar_t *sz_application, char *const *pp_argv) {
    wchar_t *p_buffers = malloc(2 * COMMAND_LINE_MAX * sizeof(*p_buffers));
    if (!p_buffers) return errno = ENOMEM;
    PROCESS_INFORMATION process;
    int error = build_command_line(pp_argv, p_buffers, p_buffers + COMMAND_LINE_MAX);
    if (!error) error = start_process(sz_application, p_buffers, &process);
    free(p_buffers);
    if (error) return errno = error;
    CloseHandle(process.hThread);
    /* Set only now, since the new process would inherit it: a console's ^C reaches the new process
     * and must not end this one before it. */
    (void)SetConsoleCtrlHandler(NULL, TRUE); /* without a console there is no ^C to ignore */
    DWORD exit_code = 1;
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &exit_code))
        exit_code = 1;
    ps_host_exit_at_once((int)exit_code);
}

/* A name without a separator is looked up as execvp does in PATH, by SearchPathW (the executable's
 * folder, the system folders, then PATH), with .exe added when it has no extension. */
int ps_host_exec(const char *sz_file, char *const *pp_argv) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX], sz_found[PS_WIN32_PATH_MAX];
    int error = ps_win32_to_wide(sz_file, sz_wide, PS_WIN32_PATH_MAX);
    if (error) return errno = error;
    if (wcspbrk(sz_wide, L"/\\")) return replace_process(sz_wide, pp_argv);
    const DWORD c_found = SearchPathW(NULL, sz_wide, L".exe", PS_WIN32_PATH_MAX, sz_found, NULL);
    if (!c_found) return errno = ps_win32_errno(GetLastError());
    if (c_found >= PS_WIN32_PATH_MAX) return errno = ENAMETOOLONG;
    return replace_process(sz_found, pp_argv);
}

int ps_host_exec_self(char *const *pp_argv) {
    wchar_t sz_self[PS_WIN32_PATH_MAX];
    const DWORD c_self = GetModuleFileNameW(NULL, sz_self, PS_WIN32_PATH_MAX);
    if (!c_self) return errno = ps_win32_errno(GetLastError());
    if (c_self >= PS_WIN32_PATH_MAX) return errno = ENAMETOOLONG;
    return replace_process(sz_self, pp_argv);
}

/* atexit handlers are not told the status, and the C runtime keeps no readable copy of it. */
int ps_host_on_exit(void (*pfn_exit)(int status)) {
    (void)pfn_exit;
    return ENOSYS;
}

/* Not _exit: it ends in ExitProcess, which kills the other threads and then runs every library's
 * DLL_PROCESS_DETACH, and a detach that takes a lock a killed thread held never returns (the run
 * then lives on after its fault report until something kills it). TerminateProcess runs no detach. */
void ps_host_exit_at_once(int status) {
    (void)TerminateProcess(GetCurrentProcess(), (UINT)status);
    const DWORD error = GetLastError();
    char sz_line[96];
    const int cb_line = snprintf(sz_line, sizeof(sz_line), "Host: TerminateProcess failed (Windows error %lu)\n",
                                 (unsigned long)error);
    DWORD cb_written;
    if (cb_line > 0) (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), sz_line, (DWORD)cb_line, &cb_written, NULL);
    _exit(status);
}

void ps_host_exit(int status) {
    printf("Host: exit(%d) ends through TerminateProcess: no atexit handler or DLL detach runs\n", status);
    fflush(stdout);
    ps_host_exit_at_once(status);
}

int32_t ps_host_process_id(void) { return (int32_t)GetCurrentProcessId(); }

int ps_host_dumpable(void) { return -ENOSYS; }

int ps_host_set_dumpable(int is_dumpable) {
    (void)is_dumpable;
    return ENOSYS;
}

/* The cleaner would have to be psport.exe started again with an option of its own, since Windows
 * has no fork to give it the caller's code. */
int ps_host_remove_tree_at_exit(const char *sz_path) {
    (void)sz_path;
    return ENOSYS;
}

/* The helper's callbacks are this process's code, which only fork gives to another process: a
 * thread instead would end with the process instead of draining its output after a crash. */
int ps_host_output_tee(void (*pfn_chunk)(int is_error, const char *p_data, size_t cb_data), void (*pfn_end)(void)) {
    (void)pfn_chunk;
    (void)pfn_end;
    return ENOSYS;
}

void ps_host_output_untee(void) {}
