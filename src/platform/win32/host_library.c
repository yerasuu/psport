/* Windows: host libraries and code addresses (ps_host.h). */
#include "ps_host.h"
#include "host_win32.h"
#include <stdatomic.h>
#include <stdio.h>

PsHostLibrary *ps_host_dl_open(const char *sz_path, char *sz_out_error, size_t cb_out_error) {
    wchar_t sz_wide[PS_WIN32_PATH_MAX];
    DWORD error = ERROR_INVALID_NAME;
    HMODULE h_module = NULL;
    /* Its dependencies are looked up beside it first, as a Linux RUNPATH of $ORIGIN would. */
    if (!ps_win32_to_wide(sz_path, sz_wide, PS_WIN32_PATH_MAX)) {
        h_module = LoadLibraryExW(sz_wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h_module) error = GetLastError();
    }
    if (!h_module && sz_out_error && cb_out_error)
        snprintf(sz_out_error, cb_out_error, "%s: LoadLibraryExW failed with Windows error %lu", sz_path,
                 (unsigned long)error);
    return (PsHostLibrary *)h_module;
}

void *ps_host_dl_sym(PsHostLibrary *p_library, const char *sz_symbol) {
    return (void *)GetProcAddress((HMODULE)p_library, sz_symbol);
}

/* Module paths in UTF-8, kept for the life of the process since PsHostCodeAddress promises them
 * while the module stays loaded and nothing unloads one; filled without locks so that a fault
 * report may add one. A module past the table's end is named by its address only. */
#define MODULE_NAMES 64
enum { NAME_EMPTY, NAME_WRITING, NAME_READY };
typedef struct {
    atomic_int state;
    HMODULE h_module;
    char sz_path[MAX_PATH * 3 + 1];
} ModuleName;
static ModuleName s_module_names[MODULE_NAMES];

static const char *module_name(HMODULE h_module) {
    for (size_t i = 0; i < MODULE_NAMES; ++i) {
        ModuleName *p_name = &s_module_names[i];
        int state = atomic_load_explicit(&p_name->state, memory_order_acquire);
        if (state == NAME_READY && p_name->h_module == h_module) return p_name->sz_path;
        if (state != NAME_EMPTY ||
            !atomic_compare_exchange_strong_explicit(&p_name->state, &state, NAME_WRITING, memory_order_acquire,
                                                     memory_order_relaxed))
            continue;
        wchar_t sz_wide[MAX_PATH + 1];
        const DWORD c_path = GetModuleFileNameW(h_module, sz_wide, MAX_PATH + 1);
        if (!c_path || c_path > MAX_PATH || ps_win32_to_utf8(sz_wide, p_name->sz_path, sizeof(p_name->sz_path)))
            snprintf(p_name->sz_path, sizeof(p_name->sz_path), "module@%p", (void *)h_module);
        p_name->h_module = h_module;
        atomic_store_explicit(&p_name->state, NAME_READY, memory_order_release);
        return p_name->sz_path;
    }
    return "module beyond the name table";
}

/* The nearest named export at or below p_code, from the module's PE export directory, whose
 * strings live in the mapped image. */
static void nearest_export(HMODULE h_module, uintptr_t va_code, PsHostCodeAddress *p_out) {
    const unsigned char *p_base = (const unsigned char *)h_module;
    const IMAGE_DOS_HEADER *p_dos = (const IMAGE_DOS_HEADER *)p_base;
    const IMAGE_NT_HEADERS *p_nt = (const IMAGE_NT_HEADERS *)(p_base + p_dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY directory = p_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return;
    const IMAGE_EXPORT_DIRECTORY *p_exports = (const IMAGE_EXPORT_DIRECTORY *)(p_base + directory.VirtualAddress);
    const DWORD *p_names = (const DWORD *)(p_base + p_exports->AddressOfNames);
    const WORD *p_ordinals = (const WORD *)(p_base + p_exports->AddressOfNameOrdinals);
    const DWORD *p_functions = (const DWORD *)(p_base + p_exports->AddressOfFunctions);
    for (DWORD i = 0; i < p_exports->NumberOfNames; ++i) {
        if (p_ordinals[i] >= p_exports->NumberOfFunctions) continue;
        const DWORD off_function = p_functions[p_ordinals[i]];
        const int is_forwarder = off_function >= directory.VirtualAddress &&
                                 off_function < directory.VirtualAddress + directory.Size;
        const uintptr_t va_function = (uintptr_t)p_base + off_function;
        if (is_forwarder || va_function > va_code || va_function <= p_out->va_symbol) continue;
        p_out->va_symbol = va_function;
        p_out->sz_symbol = (const char *)(p_base + p_names[i]);
    }
}

int ps_host_dl_address(const void *p_code, PsHostCodeAddress *p_out) {
    *p_out = (PsHostCodeAddress){0};
    HMODULE h_module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)p_code, &h_module))
        return 0;
    p_out->sz_file = module_name(h_module);
    p_out->va_base = (uintptr_t)h_module;
    nearest_export(h_module, (uintptr_t)p_code, p_out);
    return 1;
}
