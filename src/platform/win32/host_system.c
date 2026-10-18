/* Windows: randomness and the executable's path (ps_host.h). */
#include "ps_host.h"
#include "host_win32.h"
#include <bcrypt.h>
#include <errno.h>
#include <limits.h>
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
