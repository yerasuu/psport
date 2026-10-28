// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux: host libraries and code addresses (ps_host.h). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <dlfcn.h>
#include <stdio.h>

PsHostLibrary *ps_host_dl_open(const char *sz_path, char *sz_out_error, size_t cb_out_error) {
    void *p_handle = dlopen(sz_path, RTLD_NOW | RTLD_LOCAL);
    if (!p_handle && sz_out_error && cb_out_error) {
        const char *sz_error = dlerror();
        snprintf(sz_out_error, cb_out_error, "%s", sz_error ? sz_error : "unknown dlopen error");
    }
    return p_handle;
}

void *ps_host_dl_sym(PsHostLibrary *p_library, const char *sz_symbol) { return dlsym(p_library, sz_symbol); }

PsHostLibrary *ps_host_dl_find_loaded(const char *sz_name) {
    char sz_file[256];
    if (snprintf(sz_file, sizeof(sz_file), "lib%s.so", sz_name) >= (int)sizeof(sz_file)) return NULL;
    return dlopen(sz_file, RTLD_NOW | RTLD_NOLOAD);
}

int ps_host_dl_address(const void *p_code, PsHostCodeAddress *p_out) {
    Dl_info info = {0};
    const int is_found = dladdr(p_code, &info) != 0;
    *p_out = (PsHostCodeAddress){info.dli_fname, info.dli_sname, (uintptr_t)info.dli_fbase, (uintptr_t)info.dli_saddr};
    return is_found;
}
