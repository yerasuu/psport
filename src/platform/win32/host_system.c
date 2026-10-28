/* Windows: randomness, the executable's path, the machine's name and the environment (ps_host.h). */
#include "ps_host.h"
#include "host_win32.h"
#include <bcrypt.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int ps_host_random_bytes(void *p_buffer, size_t cb_buffer) {
    unsigned char *p_next = p_buffer;
    while (cb_buffer) {
        const ULONG cb_chunk = cb_buffer > ULONG_MAX ? ULONG_MAX : (ULONG)cb_buffer;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, p_next, cb_chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return EIO;
        p_next += cb_chunk;
        cb_buffer -= cb_chunk;
    }
    return 0;
}

long ps_host_exe_path(char *sz_out, size_t cb_out) {
    if (!cb_out) return -1;
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    const DWORD c_path = GetModuleFileNameW(NULL, sz_wide, PS_WIN32_PATH_MAX);
    if (!c_path || c_path >= PS_WIN32_PATH_MAX || ps_win32_to_utf8(sz_wide, sz_out, cb_out)) {
        sz_out[0] = 0;
        return -1;
    }
    /* psport's own path code splits on '/', which every Win32 call accepts as a separator. */
    for (char *p_slash = sz_out; (p_slash = strchr(p_slash, '\\'));) *p_slash = '/';
    return (long)strlen(sz_out);
}

long ps_host_command_line(char *sz_out, size_t cb_out) {
    if (!cb_out) return -1;
    if (ps_win32_to_utf8(GetCommandLineW(), sz_out, cb_out)) {
        sz_out[0] = 0;
        return -1;
    }
    return (long)strlen(sz_out);
}

long ps_host_shadps4_user_dir(char *sz_out, size_t cb_out) {
    static const char sz_folder[] = "\\shadPS4";
    const char *sz_appdata = getenv("APPDATA");
    const size_t c_appdata = sz_appdata ? strlen(sz_appdata) : 0;
    if (!c_appdata || c_appdata + sizeof(sz_folder) > cb_out) {
        if (cb_out) sz_out[0] = 0;
        return -1;
    }
    memcpy(sz_out, sz_appdata, c_appdata);
    memcpy(sz_out + c_appdata, sz_folder, sizeof(sz_folder));
    return (long)(c_appdata + sizeof(sz_folder) - 1);
}

int ps_host_host_name(char *sz_out, size_t cb_out) {
    wchar_t sz_wide[256];
    DWORD c_wide = sizeof(sz_wide) / sizeof(*sz_wide);
    if (!GetComputerNameExW(ComputerNameDnsHostname, sz_wide, &c_wide)) return ps_win32_errno(GetLastError());
    return ps_win32_to_utf8(sz_wide, sz_out, cb_out);
}

/* Through the wide environment, so that UTF-8 values (paths) survive whatever the ANSI code page is;
 * the CRT keeps its narrow copy in step. */
int ps_host_env_set(const char *sz_name, const char *sz_value, int is_overwrite) {
    wchar_t sz_wide_name[256], sz_wide_value[PS_WIN32_PATH_MAX];
    int error = ps_win32_to_wide(sz_name, sz_wide_name, sizeof(sz_wide_name) / sizeof(*sz_wide_name));
    if (!error) error = ps_win32_to_wide(sz_value, sz_wide_value, PS_WIN32_PATH_MAX);
    if (error) return error;
    if (!is_overwrite && _wgetenv(sz_wide_name)) return 0;
    return _wputenv_s(sz_wide_name, sz_wide_value);
}
