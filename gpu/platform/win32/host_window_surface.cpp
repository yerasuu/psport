// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the game window's surface on Windows: SDL3's Win32 window, presented to through
// VK_KHR_win32_surface.
#include <cerrno>
#include <cstring>
#include "../../../src/platform/ps_host_window_vulkan.h"
#include "../common/host_window_sdl.h"
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace {
enum : int { KIND_WIN32 = 1 };
}

bool host_window_read_surface(SDL_Window* p_sdl, PsHostWindowSurface* p_out) {
    const char* sz_driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(p_sdl);
    if (sz_driver && !std::strcmp(sz_driver, "windows")) {
        p_out->kind = KIND_WIN32;
        p_out->p_display = nullptr;
        p_out->p_window = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        return true;
    }
    return false;
}

extern "C" {

const char* ps_host_window_vulkan_extension(const PsHostWindowSurface* p_surface) {
    return p_surface && p_surface->kind == KIND_WIN32 ? VK_KHR_WIN32_SURFACE_EXTENSION_NAME : nullptr;
}

const char* ps_host_window_vulkan_system(const PsHostWindowSurface*) {
    return "Win32";
}

bool ps_host_window_vulkan_can_capture(const PsHostWindowSurface*) {
    return true;
}

int ps_host_window_vulkan_surface(const PsHostWindowSurface* p_surface, VkInstance h_instance,
                                  PFN_vkGetInstanceProcAddr pfn_get_instance_proc_addr, VkSurfaceKHR* p_out_surface) {
    const auto pfn_create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        pfn_get_instance_proc_addr(h_instance, "vkCreateWin32SurfaceKHR"));
    if (!pfn_create) {
        return ENOSYS;
    }
    const VkWin32SurfaceCreateInfoKHR win32_ci = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = nullptr,
        .hwnd = static_cast<HWND>(p_surface->p_window),
    };
    return pfn_create(h_instance, &win32_ci, nullptr, p_out_surface) == VK_SUCCESS ? 0 : EIO;
}

} // extern "C"
