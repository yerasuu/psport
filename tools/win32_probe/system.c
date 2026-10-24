#include "probe.h"

#define CHILD_TIMEOUT_MS 60000
#define CB_CHILD_OUTPUT_MAX (256u << 10)

/* ---- Windows memory API: VirtualAlloc2 and MapViewOfFile3 arrived in Windows 10 1803, so they are
 * looked up rather than imported, and their absence is a line in the report. */

typedef PVOID(WINAPI *PfnVirtualAlloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *PfnMapViewOfFile3)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
static PfnVirtualAlloc2 s_pfn_virtual_alloc2;
static PfnMapViewOfFile3 s_pfn_map_view_of_file3;
DWORD g_cb_granularity = 65536;

static void load_memory_api(void) {
    HMODULE h_kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!h_kernelbase) h_kernelbase = LoadLibraryW(L"kernelbase.dll");
    if (!h_kernelbase) return;
    s_pfn_virtual_alloc2 = (PfnVirtualAlloc2)(void (*)(void))GetProcAddress(h_kernelbase, "VirtualAlloc2");
    s_pfn_map_view_of_file3 = (PfnMapViewOfFile3)(void (*)(void))GetProcAddress(h_kernelbase, "MapViewOfFile3");
}

int has_placeholders(void) { return s_pfn_virtual_alloc2 && s_pfn_map_view_of_file3; }

HANDLE create_section(VkDeviceSize cb, DWORD attributes) {
    return CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | attributes, (DWORD)(cb >> 32),
                              (DWORD)cb, NULL);
}

/* A view of the section placed into a placeholder reserved for it, as psport maps the guest's direct
 * memory (src/platform/win32/host_memory.c). */
void *map_placed_view(HANDLE h_section, VkDeviceSize cb, DWORD *p_out_error) {
    void *p_placeholder =
        s_pfn_virtual_alloc2(NULL, NULL, cb, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!p_placeholder) {
        *p_out_error = GetLastError();
        return NULL;
    }
    void *p_view = s_pfn_map_view_of_file3(h_section, GetCurrentProcess(), p_placeholder, 0, cb,
                                           MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    if (!p_view) {
        *p_out_error = GetLastError();
        VirtualFree(p_placeholder, 0, MEM_RELEASE);
    }
    return p_view;
}

/* ---- System ---- */

typedef LONG(WINAPI *PfnRtlGetVersion)(OSVERSIONINFOW *);
typedef const char *(CDECL *PfnWineGetVersion)(void);
typedef void(CDECL *PfnWineGetHostVersion)(const char **, const char **);

static void registry_string(HKEY h_root, const char *sz_key, const char *sz_value, char *sz_out, DWORD cb_out) {
    sz_out[0] = 0;
    DWORD cb = cb_out;
    if (RegGetValueA(h_root, sz_key, sz_value, RRF_RT_REG_SZ, NULL, sz_out, &cb) != ERROR_SUCCESS) sz_out[0] = 0;
}

static DWORD registry_dword(HKEY h_root, const char *sz_key, const char *sz_value) {
    DWORD value = 0, cb = sizeof(value);
    if (RegGetValueA(h_root, sz_key, sz_value, RRF_RT_REG_DWORD, NULL, &value, &cb) != ERROR_SUCCESS) return 0;
    return value;
}

void report_system(void) {
    g_sz_step = "system information";
    heading("System");
    const char *sz_current = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    char sz_product[128], sz_display[64], sz_cpu[128];
    registry_string(HKEY_LOCAL_MACHINE, sz_current, "ProductName", sz_product, sizeof(sz_product));
    registry_string(HKEY_LOCAL_MACHINE, sz_current, "DisplayVersion", sz_display, sizeof(sz_display));
    registry_string(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                    sz_cpu, sizeof(sz_cpu));
    OSVERSIONINFOW version = {.dwOSVersionInfoSize = sizeof(version)};
    HMODULE h_ntdll = GetModuleHandleW(L"ntdll.dll");
    PfnRtlGetVersion pfn_get_version =
        h_ntdll ? (PfnRtlGetVersion)(void (*)(void))GetProcAddress(h_ntdll, "RtlGetVersion") : NULL;
    if (pfn_get_version && pfn_get_version(&version) == 0) {
        say("Windows: %lu.%lu build %lu.%lu (%s%s%s)", version.dwMajorVersion, version.dwMinorVersion,
            version.dwBuildNumber, registry_dword(HKEY_LOCAL_MACHINE, sz_current, "UBR"),
            sz_product[0] ? sz_product : "product name unknown", sz_display[0] ? ", " : "", sz_display);
    } else {
        say("Windows: version unknown (RtlGetVersion failed)");
    }
    PfnWineGetVersion pfn_wine =
        h_ntdll ? (PfnWineGetVersion)(void (*)(void))GetProcAddress(h_ntdll, "wine_get_version") : NULL;
    PfnWineGetHostVersion pfn_wine_host =
        h_ntdll ? (PfnWineGetHostVersion)(void (*)(void))GetProcAddress(h_ntdll, "wine_get_host_version") : NULL;
    if (pfn_wine) {
        const char *sz_sysname = "?", *sz_release = "?";
        if (pfn_wine_host) pfn_wine_host(&sz_sysname, &sz_release);
        say("Wine: %s on %s %s (not native Windows)", pfn_wine(), sz_sysname, sz_release);
    } else {
        say("Wine: no (native Windows)");
    }
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    g_cb_granularity = info.dwAllocationGranularity;
    say("CPU: %s, %lu logical processors, architecture %u", sz_cpu[0] ? sz_cpu : "name unknown",
        info.dwNumberOfProcessors, info.wProcessorArchitecture);
    say("Page size: %lu, allocation granularity: %lu, user addresses %p-%p", info.dwPageSize,
        info.dwAllocationGranularity, info.lpMinimumApplicationAddress, info.lpMaximumApplicationAddress);
    MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) {
        say("RAM: %llu MiB, %llu MiB free; commit limit %llu MiB, %llu MiB free",
            (unsigned long long)(memory.ullTotalPhys >> 20), (unsigned long long)(memory.ullAvailPhys >> 20),
            (unsigned long long)(memory.ullTotalPageFile >> 20), (unsigned long long)(memory.ullAvailPageFile >> 20));
    } else {
        say("RAM: GlobalMemoryStatusEx failed (error %lu)", GetLastError());
    }
    load_memory_api();
    say("VirtualAlloc2: %s, MapViewOfFile3: %s (psport maps the guest's memory with both; Windows 10 1803 and later)",
        s_pfn_virtual_alloc2 ? "present" : "MISSING", s_pfn_map_view_of_file3 ? "present" : "MISSING");
}

/* What Windows itself says about each display adapter's driver, which still answers when Vulkan does
 * not: the display class's instances in the registry. */
void report_display_drivers(void) {
    g_sz_step = "display drivers";
    heading("Display drivers (Windows)");
    const char *sz_class = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    int c_found = 0;
    for (int i = 0; i < 32; ++i) {
        char sz_key[160], sz_name[256], sz_version[64], sz_provider[128], sz_date[64];
        snprintf(sz_key, sizeof(sz_key), "%s\\%04d", sz_class, i);
        registry_string(HKEY_LOCAL_MACHINE, sz_key, "DriverDesc", sz_name, sizeof(sz_name));
        if (!sz_name[0]) continue;
        registry_string(HKEY_LOCAL_MACHINE, sz_key, "DriverVersion", sz_version, sizeof(sz_version));
        registry_string(HKEY_LOCAL_MACHINE, sz_key, "ProviderName", sz_provider, sizeof(sz_provider));
        registry_string(HKEY_LOCAL_MACHINE, sz_key, "DriverDate", sz_date, sizeof(sz_date));
        say("%04d: %s, driver %s by %s, dated %s", i, sz_name, sz_version[0] ? sz_version : "?",
            sz_provider[0] ? sz_provider : "?", sz_date[0] ? sz_date : "?");
        ++c_found;
    }
    if (!c_found) say("none listed in the registry");
}

void report_environment(void) {
    g_sz_step = "environment";
    heading("Environment (PS_* and VK_* variables)");
    wchar_t *p_block = GetEnvironmentStringsW();
    int c_found = 0;
    for (const wchar_t *p = p_block; p && *p; p += wcslen(p) + 1) {
        if (!wcsncmp(p, L"PS_", 3) || !wcsncmp(p, L"VK_", 3)) {
            char sz_variable[1024];
            to_utf8(p, sz_variable, sizeof(sz_variable));
            say("%s", sz_variable);
            ++c_found;
        }
    }
    if (p_block) FreeEnvironmentStringsW(p_block);
    if (!c_found) say("none set");
}

/* ---- Without the GPU: one section at two addresses ---- */

Outcome g_cpu_alias;

void test_cpu_alias(void) {
    g_sz_step = "one section at two addresses, CPU only";
    heading("Aliasing (CPU only)");
    const VkDeviceSize cb = CB_TEST;
    HANDLE h_section = create_section(cb, SEC_COMMIT);
    if (!h_section) {
        set_outcome(&g_cpu_alias, OUTCOME_FAIL, VK_SUCCESS, "CreateFileMappingW failed (error %lu)", GetLastError());
    } else {
        DWORD error = 0;
        uint8_t *p_first = has_placeholders() ? map_placed_view(h_section, cb, &error)
                                              : MapViewOfFile(h_section, FILE_MAP_ALL_ACCESS, 0, 0, cb);
        if (!p_first && !error) error = GetLastError();
        uint8_t *p_second = p_first ? MapViewOfFile(h_section, FILE_MAP_ALL_ACCESS, 0, 0, cb) : NULL;
        if (p_first && !p_second) error = GetLastError();
        if (!p_first || !p_second) {
            set_outcome(&g_cpu_alias, OUTCOME_FAIL, VK_SUCCESS, "mapping two views failed (error %lu)", error);
        } else {
            volatile uint32_t *p_a = (volatile uint32_t *)p_first, *p_b = (volatile uint32_t *)p_second;
            p_a[0] = 0x12345678u;
            p_b[1024] = 0x9abcdef0u;
            const int is_alias = p_a != p_b && p_b[0] == 0x12345678u && p_a[1024] == 0x9abcdef0u;
            set_outcome(&g_cpu_alias, is_alias ? OUTCOME_PASS : OUTCOME_FAIL, VK_SUCCESS,
                        "views at %p (%s) and %p: a write through either %s", (void *)p_first,
                        has_placeholders() ? "placed" : "MapViewOfFile", (void *)p_second,
                        is_alias ? "shows through the other" : "does NOT show through the other");
        }
        if (p_second) UnmapViewOfFile(p_second);
        if (p_first) UnmapViewOfFile(p_first);
        CloseHandle(h_section);
    }
    say("One section mapped at two addresses: %s: %s", g_cpu_alias.state == OUTCOME_PASS ? "PASS" : "FAIL",
        g_cpu_alias.sz_detail);
}

/* ---- The bundled programs ---- */

void run_bundled(const wchar_t *wsz_program, const wchar_t *wsz_arguments, DWORD *p_out_exit) {
    char sz_program[MAX_PATH * 3], sz_arguments[256];
    to_utf8(wsz_program, sz_program, sizeof(sz_program));
    to_utf8(wsz_arguments, sz_arguments, sizeof(sz_arguments));
    char sz_heading[MAX_PATH * 3 + 300];
    snprintf(sz_heading, sizeof(sz_heading), "%s %s", sz_program, sz_arguments);
    g_sz_step = "running a bundled program";
    heading(sz_heading);
    wchar_t wsz_path[MAX_PATH * 3];
    swprintf(wsz_path, sizeof(wsz_path) / sizeof(wsz_path[0]), L"%ls%ls", g_wsz_folder, wsz_program);
    if (GetFileAttributesW(wsz_path) == INVALID_FILE_ATTRIBUTES) {
        say("not run: %s is not next to the probe", sz_program);
        return;
    }
    wchar_t wsz_command[MAX_PATH * 3 + 300];
    swprintf(wsz_command, sizeof(wsz_command) / sizeof(wsz_command[0]), L"\"%ls\" %ls", wsz_path, wsz_arguments);
    SECURITY_ATTRIBUTES inherit = {.nLength = sizeof(inherit), .bInheritHandle = TRUE};
    HANDLE h_read = NULL, h_write = NULL;
    if (!CreatePipe(&h_read, &h_write, &inherit, 0)) {
        say("not run: CreatePipe failed (error %lu)", GetLastError());
        return;
    }
    SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup = {.cb = sizeof(startup),
                            .dwFlags = STARTF_USESTDHANDLES,
                            .hStdOutput = h_write,
                            .hStdError = h_write};
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(wsz_path, wsz_command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_wsz_folder, &startup,
                        &process)) {
        say("not run: CreateProcessW failed (error %lu)", GetLastError());
        CloseHandle(h_read);
        CloseHandle(h_write);
        return;
    }
    CloseHandle(h_write);
    const ULONGLONG start_ms = GetTickCount64();
    static char s_sz_line[16384];
    char buffer[4096];
    size_t cb_line = 0, cb_total = 0;
    int is_done = 0, is_timed_out = 0;
    while (!is_done) {
        DWORD cb_available = 0;
        if (!PeekNamedPipe(h_read, NULL, 0, NULL, &cb_available, NULL)) {
            is_done = 1;
        } else if (cb_available) {
            DWORD cb_read = 0;
            if (!ReadFile(h_read, buffer, sizeof(buffer), &cb_read, NULL) || !cb_read) {
                is_done = 1;
            }
            for (DWORD i = 0; i < cb_read; ++i) {
                if (buffer[i] == '\n' || cb_line == sizeof(s_sz_line) - 1) {
                    s_sz_line[cb_line] = 0;
                    if (cb_line && s_sz_line[cb_line - 1] == '\r') s_sz_line[cb_line - 1] = 0;
                    if (cb_total < CB_CHILD_OUTPUT_MAX) say("  | %s", s_sz_line);
                    cb_total += cb_line;
                    cb_line = 0;
                }
                if (buffer[i] != '\n') s_sz_line[cb_line++] = buffer[i];
            }
        } else if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD cb_left = 0;
            if (!PeekNamedPipe(h_read, NULL, 0, NULL, &cb_left, NULL) || !cb_left) is_done = 1;
        }
        if (!is_done && GetTickCount64() - start_ms > CHILD_TIMEOUT_MS) {
            is_timed_out = 1;
            TerminateProcess(process.hProcess, 1);
            is_done = 1;
        }
    }
    if (cb_line) {
        s_sz_line[cb_line] = 0;
        say("  | %s", s_sz_line);
    }
    if (cb_total >= CB_CHILD_OUTPUT_MAX) say("  (output cut at %u KiB)", CB_CHILD_OUTPUT_MAX >> 10);
    WaitForSingleObject(process.hProcess, 5000);
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    const char *sz_meaning = exit_code == 0                 ? "success"
                             : exit_code == 0xc0000135      ? "STATUS_DLL_NOT_FOUND: a DLL it needs is missing"
                             : exit_code == 0xc0000139      ? "STATUS_ENTRYPOINT_NOT_FOUND: a DLL is too old"
                             : exit_code == 0xc000007b      ? "STATUS_INVALID_IMAGE_FORMAT: a 32-bit or broken DLL"
                             : exit_code == 0xc0000005      ? "STATUS_ACCESS_VIOLATION: it crashed"
                             : exit_code == 0xc0000142      ? "STATUS_DLL_INIT_FAILED"
                             : exit_code == STILL_ACTIVE    ? "still running"
                                                            : "its own error code";
    say("exit code %#lx (%s)%s, %llu ms", exit_code, sz_meaning,
        is_timed_out ? ", ended by the probe after 60 s" : "", (unsigned long long)(GetTickCount64() - start_ms));
    if (p_out_exit) *p_out_exit = exit_code;
    CloseHandle(h_read);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

#define C_DLLS_MAX 256
char g_sz_missing_dlls[1024];

static int is_imported_name_seen(wchar_t (*p_names)[64], int c_names, const wchar_t *wsz_name) {
    for (int i = 0; i < c_names; ++i)
        if (!_wcsicmp(p_names[i], wsz_name)) return 1;
    return 0;
}

/* Every DLL a program in the probe's folder loads at start, through the import tables of the DLLs found
 * next to it, and which of them neither that folder nor Windows has: STATUS_DLL_NOT_FOUND names none. The
 * files are mapped as image resources, so that nothing in them runs. */
void list_missing_dlls(const wchar_t *wsz_program) {
    g_sz_step = "reading import tables";
    static wchar_t s_names[C_DLLS_MAX][64], s_importers[C_DLLS_MAX][64];
    int c_names = 1, i_next = 0, c_local = 0, c_system = 0, c_missing = 0;
    size_t off_missing = 0;
    wcsncpy(s_names[0], wsz_program, 63);
    s_importers[0][0] = 0;
    g_sz_missing_dlls[0] = 0;
    for (; i_next < c_names; ++i_next) {
        wchar_t wsz_path[MAX_PATH * 3];
        swprintf(wsz_path, sizeof(wsz_path) / sizeof(wsz_path[0]), L"%ls%ls", g_wsz_folder, s_names[i_next]);
        if (i_next) {
            if (!_wcsnicmp(s_names[i_next], L"api-ms-", 7) || !_wcsnicmp(s_names[i_next], L"ext-ms-", 7)) {
                ++c_system;
                continue;
            }
            if (GetFileAttributesW(wsz_path) == INVALID_FILE_ATTRIBUTES) {
                HMODULE h_system = LoadLibraryExW(s_names[i_next], NULL,
                                                  LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (h_system) {
                    FreeLibrary(h_system);
                    ++c_system;
                } else {
                    char sz_name[200], sz_importer[200];
                    to_utf8(s_names[i_next], sz_name, sizeof(sz_name));
                    to_utf8(s_importers[i_next], sz_importer, sizeof(sz_importer));
                    const int cb_written =
                        snprintf(g_sz_missing_dlls + off_missing, sizeof(g_sz_missing_dlls) - off_missing,
                                 "%s%s (imported by %s)", c_missing ? ", " : "", sz_name, sz_importer);
                    if (cb_written > 0 && (size_t)cb_written < sizeof(g_sz_missing_dlls) - off_missing)
                        off_missing += (size_t)cb_written;
                    ++c_missing;
                }
                continue;
            }
            ++c_local;
        }
        HMODULE h_image = LoadLibraryExW(wsz_path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        if (!h_image) {
            char sz_name[200];
            to_utf8(s_names[i_next], sz_name, sizeof(sz_name));
            say("  %s: its import table cannot be read (error %lu)", sz_name, GetLastError());
            continue;
        }
        const uint8_t *p_base = (const uint8_t *)((ULONG_PTR)h_image & ~(ULONG_PTR)3);
        const IMAGE_DOS_HEADER *p_dos = (const IMAGE_DOS_HEADER *)p_base;
        const IMAGE_NT_HEADERS64 *p_nt =
            p_dos->e_magic == IMAGE_DOS_SIGNATURE && p_dos->e_lfanew > 0 && p_dos->e_lfanew < 4096
                ? (const IMAGE_NT_HEADERS64 *)(p_base + p_dos->e_lfanew)
                : NULL;
        if (p_nt && p_nt->Signature == IMAGE_NT_SIGNATURE &&
            p_nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            const DWORD cb_image = p_nt->OptionalHeader.SizeOfImage;
            const IMAGE_DATA_DIRECTORY *p_imports = &p_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            for (DWORD off = p_imports->VirtualAddress;
                 p_imports->Size && off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= cb_image;
                 off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
                const IMAGE_IMPORT_DESCRIPTOR *p_descriptor = (const IMAGE_IMPORT_DESCRIPTOR *)(p_base + off);
                if (!p_descriptor->Name) break;
                if (p_descriptor->Name >= cb_image) continue;
                const char *sz_import = (const char *)(p_base + p_descriptor->Name);
                const size_t cb_import = strnlen(sz_import, cb_image - p_descriptor->Name);
                wchar_t wsz_import[64];
                if (cb_import >= 64 || p_descriptor->Name + cb_import >= cb_image ||
                    !MultiByteToWideChar(CP_ACP, 0, sz_import, (int)cb_import + 1, wsz_import, 64))
                    continue;
                if (c_names < C_DLLS_MAX && !is_imported_name_seen(s_names, c_names, wsz_import)) {
                    wcscpy(s_names[c_names], wsz_import);
                    wcsncpy(s_importers[c_names], s_names[i_next], 63);
                    ++c_names;
                }
            }
        } else {
            char sz_name[200];
            to_utf8(s_names[i_next], sz_name, sizeof(sz_name));
            say("  %s: not a 64-bit PE image", sz_name);
        }
        FreeLibrary(h_image);
    }
    say("DLLs it loads at start: %d from this folder, %d from Windows, %d missing%s%s", c_local, c_system, c_missing,
        c_missing ? ": " : "", g_sz_missing_dlls);
}
