// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Include the vulkan platform specific header
#if defined(ANDROID)
#define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(_WIN64)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fmt/ranges.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "../../src/platform/ps_host.h"
#include "../../src/platform/ps_host_window_vulkan.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <unistd.h>

extern "C" volatile int runtime_restarting;
extern "C" void runtime_calls_report(void);
extern "C" int runtime_debug_open(const char* sz_name);
extern "C" void psport_gpu_flush_caches(int can_wait);

namespace Vulkan {

static const char* const VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";
static const char* const CRASH_DIAGNOSTIC_LAYER_NAME = "VK_LAYER_LUNARG_crash_diagnostic";

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT severity, vk::DebugUtilsMessageTypeFlagsEXT type,
    const vk::DebugUtilsMessengerCallbackDataEXT* callback_data, void* user_data) {

    spdlog::level level{};
    switch (severity) {
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
        level = spdlog::level::err;
        break;
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
        level = spdlog::level::info;
        break;
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
        level = spdlog::level::debug;
        break;
    default:
        level = spdlog::level::info;
    }

    LOG_GENERIC(Common::Log::Class::Render_Vulkan, level, "{}: {}",
                callback_data->pMessageIdName ? callback_data->pMessageIdName : "<null>",
                callback_data->pMessage ? callback_data->pMessage : "<null>");

    // psport: debug mode's validation.txt (src/runtime/runtime_debug.c) gets every message,
    // whatever PS_GPU_LOG lets through to the log.
    static const int s_h_validation = runtime_debug_open("validation.txt");
    if (s_h_validation >= 0) {
        const std::string text =
            fmt::format("{} {}: {}\n", vk::to_string(severity),
                        callback_data->pMessageIdName ? callback_data->pMessageIdName : "<null>",
                        callback_data->pMessage ? callback_data->pMessage : "<null>");
        (void)!write(s_h_validation, text.data(), text.size());
    }

    return VK_FALSE;
}

// psport: the window system's surface is made in gpu/platform/<os>/ (ps_host_window_vulkan.h).
vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window) {
    const PsHostWindowSurface* p_surface = emu_window.GetSurface();
    VkSurfaceKHR h_surface = VK_NULL_HANDLE;
    if (!p_surface) {
        LOG_CRITICAL(Render_Vulkan, "Presentation not supported on this platform");
        return {};
    }
    if (EmulatorSettings.IsRenderdocEnabled() && !ps_host_window_vulkan_can_capture(p_surface)) {
        LOG_ERROR(Render_Vulkan, "RenderDoc is not compatible with Wayland, use an X11 window instead.");
    }
    if (ps_host_window_vulkan_surface(p_surface, static_cast<VkInstance>(instance),
                                      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                                      &h_surface) != 0) {
        LOG_CRITICAL(Render_Vulkan, "Failed to initialize {} surface", ps_host_window_vulkan_system(p_surface));
        return {};
    }
    return vk::SurfaceKHR{h_surface};
}

static auto GetLayerExtensions(std::vector<const char*>&& extensions,
                               const std::vector<const char*>& layers) {
    auto all_missing_vk_settings = true;

    for (const auto& layer_name : layers) {
        const auto [layer_properties_result, layer_extensions] =
            vk::enumerateInstanceExtensionProperties(std::string(layer_name));
        if (layer_properties_result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "Failed to query extension properties of {}: {}", layer_name,
                      vk::to_string(layer_properties_result));
        }
        auto found = false;
        for (const auto& extension : layer_extensions) {
            if (extension.extensionName == std::string_view(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
                found = true;
                all_missing_vk_settings = false;
                break;
            }
        }
        if (!found) {
            LOG_ERROR(Render_Vulkan, "Settings for layer {} not available.", layer_name);
        }
    }

    if (!all_missing_vk_settings) {
        extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    }

    return extensions;
}

std::vector<const char*> GetInstanceExtensions(const PsHostWindowSurface* p_surface, bool enable_debug_utils) {
    const auto [properties_result, properties] = vk::enumerateInstanceExtensionProperties();
    if (properties_result != vk::Result::eSuccess || properties.empty()) {
        LOG_ERROR(Render_Vulkan, "Failed to query extension properties: {}",
                  vk::to_string(properties_result));
        return {};
    }

    // Add the windowing system specific extension
    std::vector<const char*> extensions;
    extensions.reserve(7);

    // psport: the window system's extension comes from gpu/platform/<os>/; none headless.
    if (p_surface) {
        if (const char* sz_extension = ps_host_window_vulkan_extension(p_surface)) {
            extensions.push_back(sz_extension);
        } else {
            LOG_ERROR(Render_Vulkan, "Presentation not supported on this platform");
        }
    }

    // The headless renderer creates the same logical device with VK_KHR_swapchain enabled;
    // its instance dependency is required even when no surface is created by a GPU test.
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

    if (EmulatorSettings.IsHdrAllowed()) {
        extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    if (enable_debug_utils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // Sanitize extension list
    std::erase_if(extensions, [&](const char* extension) -> bool {
        const auto it =
            std::find_if(properties.begin(), properties.end(), [extension](const auto& prop) {
                return std::strcmp(extension, prop.extensionName) == 0;
            });

        if (it == properties.end()) {
            LOG_INFO(Render_Vulkan, "Candidate instance extension {} is not available", extension);
            return true;
        }
        return false;
    });

    return extensions;
}

std::vector<const char*> GetInstanceLayers(bool enable_validation, bool enable_crash_diagnostic) {
    const auto [properties_result, properties] = vk::enumerateInstanceLayerProperties();
    if (properties_result != vk::Result::eSuccess || properties.empty()) {
        LOG_ERROR(Render_Vulkan, "Failed to query layer properties: {}",
                  vk::to_string(properties_result));
        return {};
    }

    std::vector<const char*> layers;
    layers.reserve(2);

    if (enable_validation) {
        layers.push_back(VALIDATION_LAYER_NAME);
    }
    if (enable_crash_diagnostic) {
        layers.push_back(CRASH_DIAGNOSTIC_LAYER_NAME);
    }

    // Sanitize layer list
    std::erase_if(layers, [&](const char* layer) -> bool {
        const auto it = std::ranges::find_if(properties, [layer](const auto& prop) {
            return std::strcmp(layer, prop.layerName) == 0;
        });
        if (it == properties.end()) {
            LOG_ERROR(Render_Vulkan, "Requested layer {} is not available", layer);
            return true;
        }
        return false;
    });

    return layers;
}

vk::UniqueInstance CreateInstance(const PsHostWindowSurface* p_surface, bool enable_validation,
                                  bool enable_crash_diagnostic, std::string* p_out_failure) {
    const auto fail = [p_out_failure](std::string failure) -> vk::UniqueInstance {
        if (p_out_failure) {
            *p_out_failure = std::move(failure);
        }
        return {};
    };
    LOG_INFO(Render_Vulkan, "Creating vulkan instance");

#if defined(__APPLE__)
    // Initialize the environment with the path to the included ICD, so that the loader will
    // find it.
    static const auto icd_path = [] {
        char path[PATH_MAX];
        u32 length = PATH_MAX;
        _NSGetExecutablePath(path, &length);
        return std::filesystem::path(path).parent_path();
    }();
    setenv("VK_DRIVER_FILES", icd_path.c_str(), true);
#endif

    static vk::detail::DynamicLoader dl;
    if (!dl.success()) {
        LOG_CRITICAL(Render_Vulkan, "The Vulkan loader library could not be loaded");
        return fail("the Vulkan loader library could not be loaded");
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

    const auto [available_version_result, available_version] =
        VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion
            ? vk::enumerateInstanceVersion()
            : vk::ResultValue(vk::Result::eSuccess, VK_API_VERSION_1_0);
    if (available_version_result != vk::Result::eSuccess) {
        LOG_CRITICAL(Render_Vulkan, "Failed to query Vulkan API version: {}",
                     vk::to_string(available_version_result));
        return fail("vkEnumerateInstanceVersion returned " + vk::to_string(available_version_result));
    }
    if (available_version < TargetVulkanApiVersion) {
        LOG_CRITICAL(Render_Vulkan, "Vulkan {}.{} is required, but only {}.{} is supported by instance!",
                     VK_VERSION_MAJOR(TargetVulkanApiVersion), VK_VERSION_MINOR(TargetVulkanApiVersion),
                     VK_VERSION_MAJOR(available_version), VK_VERSION_MINOR(available_version));
        return fail(fmt::format("the Vulkan loader offers {}.{}, 1.3 is needed", VK_VERSION_MAJOR(available_version),
                                VK_VERSION_MINOR(available_version)));
    }

    const auto layers = GetInstanceLayers(enable_validation, enable_crash_diagnostic);
    auto extensions = GetLayerExtensions(GetInstanceExtensions(p_surface, true), layers);
    // psport: DLSS (optional bridge library) needs its own instance extensions.
    if (Dlss* dlss = Dlss::Get()) {
        dlss->AppendInstanceExtensions(extensions);
    }

    const vk::ApplicationInfo application_info = {
        .pApplicationName = "shadPS4",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "shadPS4 Vulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = available_version,
    };

    const std::string extensions_string = fmt::format("{}", fmt::join(extensions, ", "));
    const std::string layers_string = fmt::format("{}", fmt::join(layers, ", "));
    LOG_INFO(Render_Vulkan, "Enabled instance extensions: {}", extensions_string);
    LOG_INFO(Render_Vulkan, "Enabled instance layers: {}", layers_string);

    // Validation settings
    vk::Bool32 enable_core = EmulatorSettings.IsVkValidationCoreEnabled() ? vk::True : vk::False;
    vk::Bool32 enable_sync = EmulatorSettings.IsVkValidationSyncEnabled() ? vk::True : vk::False;
    vk::Bool32 enable_gpuav = EmulatorSettings.IsVkValidationGpuEnabled() ? vk::True : vk::False;

    // Crash diagnostics settings
    static const auto crash_diagnostic_path =
        Common::FS::GetUserPathString(Common::FS::PathType::LogDir);
    const char* log_path = crash_diagnostic_path.c_str();
    vk::Bool32 enable_force_barriers = vk::True;

    const std::array layer_setings = {
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "validate_core",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_core,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "validate_sync",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_sync,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "syncval_submit_time_validation",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_sync,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_enable",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_descriptor_checks",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_buffers_validation",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_indirect_draws_buffers",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_indirect_dispatches_buffers",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_indirect_trace_rays_buffers",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_buffer_copies",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = "lunarg_crash_diagnostic",
            .pSettingName = "output_path",
            .type = vk::LayerSettingTypeEXT::eString,
            .valueCount = 1,
            .pValues = &log_path,
        },
        vk::LayerSettingEXT{
            .pLayerName = "lunarg_crash_diagnostic",
            .pSettingName = "sync_after_commands",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_force_barriers,
        },
    };

    vk::StructureChain<vk::InstanceCreateInfo, vk::LayerSettingsCreateInfoEXT> instance_ci_chain = {
        vk::InstanceCreateInfo{
            .pApplicationInfo = &application_info,
            .enabledLayerCount = static_cast<u32>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        },
        vk::LayerSettingsCreateInfoEXT{
            .settingCount = layer_setings.size(),
            .pSettings = layer_setings.data(),
        },
    };

    auto [instance_result, instance] = vk::createInstanceUnique(instance_ci_chain.get());
    if (instance_result != vk::Result::eSuccess) {
        LOG_CRITICAL(Render_Vulkan, "Failed to create instance: {}", vk::to_string(instance_result));
        return fail("vkCreateInstance returned " + vk::to_string(instance_result));
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);

    return std::move(instance);
}

vk::UniqueDebugUtilsMessengerEXT CreateDebugCallback(vk::Instance instance) {
    const vk::DebugUtilsMessengerCreateInfoEXT msg_ci = {
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = DebugUtilsCallback,
    };
    auto [messenger_result, messenger] = instance.createDebugUtilsMessengerEXTUnique(msg_ci);
    if (messenger_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Failed to create debug callback, validation messages not shown: {}",
                  vk::to_string(messenger_result));
    }
    return std::move(messenger);
}

namespace {
std::atomic<bool> s_has_init_failed;
} // Anonymous namespace

void ReportInitFailure() {
    s_has_init_failed.store(true, std::memory_order_relaxed);
}

bool HasInitFailed() {
    return s_has_init_failed.load(std::memory_order_relaxed);
}

void StopOnDeviceLost(const char* sz_where) {
    // The port closes the device before it restarts itself (probe.c): what fails then waits for
    // the exec instead, as assert_fail_impl does.
    if (runtime_restarting) {
        for (;;) {
            ps_host_time_sleep_ns(1000000000, nullptr);
        }
    }
    Breadcrumbs::ReportDeviceLost(sz_where);
    std::fflush(stdout);
    std::fprintf(stderr, "STOP: GPU device lost (%s)\n", sz_where);
    psport_gpu_flush_caches(1);
    runtime_calls_report();
    std::_Exit(24);
}

} // namespace Vulkan
