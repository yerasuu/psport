// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/string_literal.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"

namespace Frontend {
enum class WindowSystemType : u8;
class WindowSDL;
} // namespace Frontend

namespace Vulkan {

constexpr u32 TargetVulkanApiVersion = VK_API_VERSION_1_3;

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window);

/// psport: on failure (a null instance) p_out_failure, when given, says why.
vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool enable_crash_diagnostic, std::string* p_out_failure = nullptr);

vk::UniqueDebugUtilsMessengerEXT CreateDebugCallback(vk::Instance instance);

/// psport: an object the GPU library needs from the start (device, swapchain, built-in layouts
/// and pipelines) could not be created. Its creator logs why, leaves it null and goes on; the
/// presenter's creation then fails, so that psport_gpu_init returns -1 instead of the game
/// running without it.
void ReportInitFailure();
[[nodiscard]] bool HasInitFailed();

/// psport: a lost device cannot be recovered from: reports the breadcrumbs, prints a STOP line and
/// the call summary, and ends the process with exit code 24.
[[noreturn]] void StopOnDeviceLost(const char* sz_where);

template <typename T>
concept VulkanHandleType = vk::isVulkanHandleType<T>::value;

template <VulkanHandleType HandleType>
void SetObjectName(vk::Device device, const HandleType& handle, std::string_view debug_name) {
    const vk::DebugUtilsObjectNameInfoEXT name_info = {
        .objectType = HandleType::objectType,
        .objectHandle = reinterpret_cast<u64>(static_cast<typename HandleType::NativeType>(handle)),
        .pObjectName = debug_name.data(),
    };
    auto result = device.setDebugUtilsObjectNameEXT(name_info);
    if (result != vk::Result::eSuccess) {
        LOG_DEBUG(Render_Vulkan, "Could not set object debug name: {}", vk::to_string(result));
    }
}

template <VulkanHandleType HandleType, typename... Args>
void SetObjectName(vk::Device device, const HandleType& handle, const char* format,
                   const Args&... args) {
    const std::string debug_name = fmt::vformat(format, fmt::make_format_args(args...));
    SetObjectName(device, handle, debug_name);
}

// psport: a failed call is an error value: logged with a count, its result left null for the
// caller to check (true on success). A lost device stops the port.
template <StringLiteral msg = "">
static bool Check(vk::Result r) {
    if (r == vk::Result::eSuccess) {
        return true;
    }
    if (r == vk::Result::eErrorDeviceLost) {
        StopOnDeviceLost("a Vulkan call");
    }
    if constexpr (msg.len <= 1) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Vulkan call failed: {}", vk::to_string(r));
    } else {
        LOG_ERROR_COUNTED(Render_Vulkan, "Failed to {}: {}", msg.value, vk::to_string(r));
    }
    return false;
}

template <StringLiteral msg = "", typename T>
static T Check(vk::ResultValue<T> r) {
    Check<msg>(r.result);
    return std::move(r.value);
}

} // namespace Vulkan
