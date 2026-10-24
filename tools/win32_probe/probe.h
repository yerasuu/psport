/* psport-windows-probe (tools/win32_probe): what a Windows machine offers psport's guest memory, without a game (kb #899).
 *
 *   psport-windows-probe.exe [--output PATH]
 *
 * Writes one report, psport-windows-report.txt next to the program (or PATH; %TEMP% when that folder
 * is not writable): the system and its display drivers, the Vulkan devices with the extensions,
 * features and memory types psport relies on, an active test of every way the GPU could share host
 * memory with the port (VK_EXT_external_memory_host on private memory, on views of a section, on a
 * view placed into a placeholder, on two views of one section at once, and the OPAQUE_WIN32 export
 * and import), the output of the bundled psport.exe --list-features, and a verdict on psport's
 * guest-memory mode (kb #870, #873). Vulkan is loaded at run time so that a machine without a loader
 * or a driver still gets a report, and every failure is a line in it: a crash inside a driver is
 * written down with the step it hit before the program ends. */
#ifndef PSPORT_WIN32_PROBE_H
#define PSPORT_WIN32_PROBE_H

#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CB_TEST ((VkDeviceSize)1 << 20)

#define PROBE_VULKAN_FUNCTIONS(X)                                                                                     \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceProperties2)                                                                                  \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkGetPhysicalDeviceMemoryProperties2)                                                                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceExternalBufferProperties)                                                                     \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindBufferMemory)                                                                                              \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkResetCommandBuffer)                                                                                            \
    X(vkCmdFillBuffer)                                                                                                 \
    X(vkCmdCopyBuffer)                                                                                                 \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkResetFences)                                                                                                   \
    X(vkWaitForFences)                                                                                                 \
    X(vkQueueSubmit)                                                                                                   \
    X(vkDeviceWaitIdle)

typedef struct {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;
#define PROBE_DECLARE(name) PFN_##name name;
    PROBE_VULKAN_FUNCTIONS(PROBE_DECLARE)
#undef PROBE_DECLARE
} ProbeVulkan;

typedef enum { OUTCOME_NOT_RUN, OUTCOME_PASS, OUTCOME_FAIL } OutcomeState;

typedef struct {
    OutcomeState state;
    VkResult result;
    char sz_detail[320];
} Outcome;

enum {
    TEST_PRIVATE,
    TEST_SECTION_VIEW,
    TEST_SECTION_RESERVE_VIEW,
    TEST_PLACED_VIEW,
    TEST_ALIAS_VIEWS,
    TEST_OPAQUE_WIN32,
    TEST_OPAQUE_WIN32_KMT,
    TEST_COUNT
};

typedef struct {
    VkPhysicalDevice h_physical;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkExtensionProperties *p_extensions;
    uint32_t c_extensions;
    VkDeviceSize cb_import_alignment;
    VkDeviceSize cb_local_heap;
    VkDeviceSize cb_rebar;
    Outcome outcomes[TEST_COUNT];
    char sz_tests_skipped[256];
} Device;

/* The step running now, which the crash handler writes down. */
extern const char *g_sz_step;
/* The probe's folder, with its trailing backslash: where the bundled programs and the report are. */
extern wchar_t g_wsz_folder[MAX_PATH * 2];
extern DWORD g_cb_granularity;
extern ProbeVulkan g_vk;
extern Outcome g_cpu_alias;
extern char g_sz_missing_dlls[1024];
extern const char *const g_sz_test_ids[TEST_COUNT];
extern const char *const g_sz_test_names[TEST_COUNT];

/* main.c: every line goes to the console and to the report. */
void say(const char *sz_format, ...) __attribute__((format(printf, 1, 2)));
void heading(const char *sz_name);
void to_utf8(const wchar_t *wsz, char *sz_out, size_t cb_out);

/* system.c */
int has_placeholders(void);
HANDLE create_section(VkDeviceSize cb, DWORD attributes);
void *map_placed_view(HANDLE h_section, VkDeviceSize cb, DWORD *p_out_error);
void report_system(void);
void report_display_drivers(void);
void report_environment(void);
void test_cpu_alias(void);
void run_bundled(const wchar_t *wsz_program, const wchar_t *wsz_arguments, DWORD *p_out_exit);
void list_missing_dlls(const wchar_t *wsz_program);

/* vulkan.c */
const char *vk_result_name(VkResult result);
void set_outcome(Outcome *p_outcome, OutcomeState state, VkResult result, const char *sz_format, ...)
    __attribute__((format(printf, 4, 5)));
int has_device_extension(const Device *p_device, const char *sz_name);
VkExternalMemoryFeatureFlags external_buffer_features(const Device *p_device,
                                                      VkExternalMemoryHandleTypeFlagBits handle_type);
HMODULE load_vulkan(void);
VkInstance create_instance(void);
void describe_device(uint32_t i_device, Device *p_device);

/* memory_tests.c */
void run_device_tests(Device *p_device);

#endif
