#include "probe.h"

#include <time.h>

#ifndef PS_VERSION
#define PS_VERSION "unknown"
#endif
#ifndef PS_COMMIT
#define PS_COMMIT "unknown"
#endif

#define REPORT_NAME L"psport-windows-report.txt"

const char *g_sz_step = "start";
wchar_t g_wsz_folder[MAX_PATH * 2];
static FILE *s_p_report;
static wchar_t s_wsz_report[MAX_PATH * 2];
static DWORD s_psport_exit = STILL_ACTIVE;
static int s_is_psport_run = 0;

void say(const char *sz_format, ...) {
    va_list args;
    va_start(args, sz_format);
    va_list copy;
    va_copy(copy, args);
    vfprintf(stdout, sz_format, args);
    fputc('\n', stdout);
    fflush(stdout);
    if (s_p_report) {
        vfprintf(s_p_report, sz_format, copy);
        fputc('\n', s_p_report);
        fflush(s_p_report);
    }
    va_end(copy);
    va_end(args);
}

void heading(const char *sz_name) {
    say("");
    say("== %s ==", sz_name);
}

void to_utf8(const wchar_t *wsz, char *sz_out, size_t cb_out) {
    if (!WideCharToMultiByte(CP_UTF8, 0, wsz, -1, sz_out, (int)cb_out, NULL, NULL)) snprintf(sz_out, cb_out, "?");
}

/* The process ends here either way: the line says which step a driver or the system crashed in, so
 * that the report sent back still tells what ran. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *p_info) {
    const EXCEPTION_RECORD *p_record = p_info ? p_info->ExceptionRecord : NULL;
    say("");
    say("CRASH: exception %#lx at %p during: %s", p_record ? (unsigned long)p_record->ExceptionCode : 0ul,
        p_record ? p_record->ExceptionAddress : NULL, g_sz_step);
    say("The report is incomplete: the step above crashed the probe (a driver or system defect, worth sending).");
    if (s_p_report) fclose(s_p_report);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- Verdict ---- */

static int is_better_device(const Device *p_candidate, const Device *p_current) {
    const int is_api = p_candidate->properties.apiVersion >= VK_API_VERSION_1_3;
    const int was_api = p_current->properties.apiVersion >= VK_API_VERSION_1_3;
    if (is_api != was_api) return is_api;
    const int is_discrete = p_candidate->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const int was_discrete = p_current->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if (is_discrete != was_discrete) return is_discrete;
    const int is_cpu = p_candidate->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const int was_cpu = p_current->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (is_cpu != was_cpu) return !is_cpu;
    return p_candidate->cb_local_heap > p_current->cb_local_heap;
}

static const char *state_text(const Outcome *p_outcome) {
    return p_outcome->state == OUTCOME_PASS ? "PASS" : p_outcome->state == OUTCOME_FAIL ? "FAIL" : "NOT RUN";
}

static void verdict_for(const Device *p_device, int is_full) {
    const Outcome *p_outcomes = p_device->outcomes;
    const int is_current_ok = g_cpu_alias.state == OUTCOME_PASS && p_outcomes[TEST_PLACED_VIEW].state == OUTCOME_PASS &&
                              p_outcomes[TEST_ALIAS_VIEWS].state == OUTCOME_PASS;
    const int is_private_ok = p_outcomes[TEST_PRIVATE].state == OUTCOME_PASS;
    const int is_opaque_ok =
        p_outcomes[TEST_OPAQUE_WIN32].state == OUTCOME_PASS || p_outcomes[TEST_OPAQUE_WIN32_KMT].state == OUTCOME_PASS;
    const char *sz_current = is_current_ok ? "WOULD WORK" : "WOULD NOT WORK";
    const char *sz_alternative =
        is_current_ok    ? "none needed"
        : is_private_ok  ? "private memory only: the GPU imports memory that is not a view of a section, so direct memory "
                           "would have to be private at guest addresses, with its aliases and write traps handled "
                           "another way (kb #873 option 2)"
        : is_opaque_ok   ? "none of the host imports: only Vulkan's own exported memory round-trips, so a copy path "
                           "(kb #873 option 3) unless its handle maps on the CPU (see e1)"
                         : "none: the GPU imports no host memory here; only a copy path (kb #873 option 3)";
    if (!is_full) {
        say("  %s: current mode %s; alternative: %s", p_device->properties.deviceName, sz_current, sz_alternative);
        return;
    }
    say("Current guest-memory mode (the GPU imports views of one direct memory section mapped at several addresses, "
        "VK_EXT_external_memory_host): %s",
        sz_current);
    say("Alternative that would work: %s", sz_alternative);
    if (p_device->properties.apiVersion < VK_API_VERSION_1_3)
        say("Also: the device is below Vulkan 1.3, which psport needs regardless of memory");
    say("Facts behind it:");
    say("  VK_EXT_external_memory_host: %s", p_device->cb_import_alignment ? "yes" : "no");
    if (p_device->cb_import_alignment)
        say("  minImportedHostPointerAlignment %llu, allocation granularity %lu",
            (unsigned long long)p_device->cb_import_alignment, g_cb_granularity);
    say("  one section at two addresses (CPU): %s", state_text(&g_cpu_alias));
    for (int i = 0; i < TEST_COUNT; ++i)
        say("  [%s] %s: %s (%s)%s%s", g_sz_test_ids[i], g_sz_test_names[i], state_text(&p_outcomes[i]),
            vk_result_name(p_outcomes[i].result), p_outcomes[i].sz_detail[0] ? ": " : "", p_outcomes[i].sz_detail);
}

static FILE *open_report(const wchar_t *wsz_requested) {
    if (wsz_requested) {
        wcsncpy(s_wsz_report, wsz_requested, sizeof(s_wsz_report) / sizeof(s_wsz_report[0]) - 1);
    } else {
        swprintf(s_wsz_report, sizeof(s_wsz_report) / sizeof(s_wsz_report[0]), L"%ls%ls", g_wsz_folder, REPORT_NAME);
    }
    FILE *p_file = _wfopen(s_wsz_report, L"w");
    if (p_file || wsz_requested) return p_file;
    wchar_t wsz_temp[MAX_PATH + 1];
    const DWORD c_temp = GetTempPathW(MAX_PATH + 1, wsz_temp);
    if (!c_temp || c_temp > MAX_PATH) return NULL;
    swprintf(s_wsz_report, sizeof(s_wsz_report) / sizeof(s_wsz_report[0]), L"%ls%ls", wsz_temp, REPORT_NAME);
    return _wfopen(s_wsz_report, L"w");
}

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(on_crash);
    SetConsoleOutputCP(CP_UTF8);
    const wchar_t *wsz_output = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--output") && i + 1 < argc) {
            wsz_output = argv[++i];
        } else {
            fprintf(stderr, "Usage: psport-windows-probe.exe [--output PATH]\n");
            return 2;
        }
    }
    const DWORD c_folder = GetModuleFileNameW(NULL, g_wsz_folder, sizeof(g_wsz_folder) / sizeof(g_wsz_folder[0]));
    if (!c_folder || c_folder >= sizeof(g_wsz_folder) / sizeof(g_wsz_folder[0])) {
        g_wsz_folder[0] = 0;
    } else {
        wchar_t *p_slash = wcsrchr(g_wsz_folder, L'\\');
        if (p_slash) p_slash[1] = 0;
    }
    s_p_report = open_report(wsz_output);
    char sz_report[MAX_PATH * 6];
    to_utf8(s_wsz_report, sz_report, sizeof(sz_report));
    if (!s_p_report) fprintf(stderr, "psport-windows-probe: cannot write %s; the report goes to this window only\n", sz_report);

    char sz_time[64];
    const time_t now = time(NULL);
    struct tm utc;
    if (gmtime_s(&utc, &now) || !strftime(sz_time, sizeof(sz_time), "%Y-%m-%dT%H:%M:%SZ", &utc))
        snprintf(sz_time, sizeof(sz_time), "unknown");
    say("psport Windows report (format 1)");
    say("Probe: psport-windows-probe %s (commit %s), run %s", PS_VERSION, PS_COMMIT, sz_time);
    say("Send this whole file back. It holds no personal data beyond the machine's hardware and driver names.");

    report_system();
    report_display_drivers();
    report_environment();
    test_cpu_alias();

    Device *p_devices = NULL;
    uint32_t c_devices = 0;
    VkInstance h_instance = VK_NULL_HANDLE;
    const char *sz_no_vulkan = NULL;
    if (!load_vulkan()) {
        sz_no_vulkan = "the Vulkan loader does not load";
    } else if (!(h_instance = create_instance())) {
        sz_no_vulkan = "no Vulkan instance";
    } else {
        g_sz_step = "enumerating Vulkan devices";
        VkResult result = g_vk.vkEnumeratePhysicalDevices(h_instance, &c_devices, NULL);
        if (result == VK_SUCCESS && c_devices) {
            VkPhysicalDevice *p_handles = calloc(c_devices, sizeof(*p_handles));
            p_devices = calloc(c_devices, sizeof(*p_devices));
            if (!p_handles || !p_devices) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                result = g_vk.vkEnumeratePhysicalDevices(h_instance, &c_devices, p_handles);
                if (result == VK_INCOMPLETE) result = VK_SUCCESS;
            }
            for (uint32_t i = 0; result == VK_SUCCESS && i < c_devices; ++i) {
                Device *p_device = &p_devices[i];
                p_device->h_physical = p_handles[i];
                g_vk.vkGetPhysicalDeviceProperties(p_device->h_physical, &p_device->properties);
                uint32_t c_extensions = 0;
                if (g_vk.vkEnumerateDeviceExtensionProperties(p_device->h_physical, NULL, &c_extensions, NULL) ==
                        VK_SUCCESS &&
                    c_extensions) {
                    p_device->p_extensions = calloc(c_extensions, sizeof(*p_device->p_extensions));
                    if (p_device->p_extensions &&
                        g_vk.vkEnumerateDeviceExtensionProperties(p_device->h_physical, NULL, &c_extensions,
                                                                  p_device->p_extensions) == VK_SUCCESS)
                        p_device->c_extensions = c_extensions;
                }
            }
            free(p_handles);
        }
        if (result != VK_SUCCESS || !c_devices) {
            say("Devices: vkEnumeratePhysicalDevices -> %s (%d), %u devices", vk_result_name(result), result, c_devices);
            sz_no_vulkan = "no Vulkan device";
            c_devices = 0;
        } else {
            say("Devices: %u", c_devices);
            for (uint32_t i = 0; i < c_devices; ++i) {
                describe_device(i, &p_devices[i]);
                run_device_tests(&p_devices[i]);
            }
        }
    }

    run_bundled(L"psport-gpu-capabilities.exe", L"--gpus", NULL);
    run_bundled(L"psport.exe", L"--list-features", &s_psport_exit);
    s_is_psport_run = s_psport_exit != STILL_ACTIVE;
    if (s_is_psport_run) list_missing_dlls(L"psport.exe");

    g_sz_step = "verdict";
    heading("Verdict");
    if (sz_no_vulkan) {
        say("psport cannot run here: %s. Install or repair the GPU vendor's driver, then run the probe again.",
            sz_no_vulkan);
    } else {
        uint32_t i_taken = 0;
        const char *sz_gpu_id = getenv("PS_GPU_ID");
        const char *sz_why = "psport's default: Vulkan 1.3 first, then discrete, then the largest device-local heap";
        if (sz_gpu_id && *sz_gpu_id) {
            char *p_end = NULL;
            const unsigned long i_wanted = strtoul(sz_gpu_id, &p_end, 10);
            if (p_end && !*p_end && i_wanted < c_devices) {
                i_taken = (uint32_t)i_wanted;
                sz_why = "PS_GPU_ID";
            } else {
                for (uint32_t i = 1; i < c_devices; ++i)
                    if (is_better_device(&p_devices[i], &p_devices[i_taken])) i_taken = i;
                sz_why = "PS_GPU_ID is not a device index here, so psport's default";
            }
        } else {
            for (uint32_t i = 1; i < c_devices; ++i)
                if (is_better_device(&p_devices[i], &p_devices[i_taken])) i_taken = i;
        }
        say("Device psport takes: %u, %s, vendor id %#06x (%s)", i_taken, p_devices[i_taken].properties.deviceName,
            p_devices[i_taken].properties.vendorID, sz_why);
        verdict_for(&p_devices[i_taken], 1);
        if (c_devices > 1) {
            say("Other devices:");
            for (uint32_t i = 0; i < c_devices; ++i)
                if (i != i_taken) verdict_for(&p_devices[i], 0);
        }
    }
    if (!s_is_psport_run) {
        say("psport.exe --list-features: not run");
    } else {
        say("psport.exe --list-features: exit code %#lx%s", s_psport_exit,
            s_psport_exit == 0 ? " (psport.exe and its DLLs load)" : " (see its section above)");
        if (g_sz_missing_dlls[0]) say("DLLs psport.exe lacks: %s", g_sz_missing_dlls);
    }
    say("");
    say("End of report.");
    for (uint32_t i = 0; i < c_devices; ++i) free(p_devices[i].p_extensions);
    free(p_devices);
    if (h_instance) g_vk.vkDestroyInstance(h_instance, NULL);
    if (s_p_report) {
        fclose(s_p_report);
        s_p_report = NULL;
        printf("\nReport written to %s\n", sz_report);
    }
    return 0;
}
