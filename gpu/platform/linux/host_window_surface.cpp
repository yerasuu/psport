// psport: the game window's surface on Linux: SDL3's X11 or Wayland window, presented to through
// VK_KHR_xlib_surface or VK_KHR_wayland_surface.
#include <cerrno>
#include <cstring>
#include "../../../src/platform/ps_host_window_vulkan.h"
#include "../common/host_window_sdl.h"
#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

namespace {
enum : int { KIND_XLIB = 1, KIND_WAYLAND };
}

bool host_window_read_surface(SDL_Window* p_sdl, PsHostWindowSurface* p_out) {
    const char* sz_driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(p_sdl);
    if (sz_driver && !std::strcmp(sz_driver, "x11")) {
        p_out->kind = KIND_XLIB;
        p_out->p_display = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        p_out->p_window = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        return true;
    }
    if (sz_driver && !std::strcmp(sz_driver, "wayland")) {
        p_out->kind = KIND_WAYLAND;
        p_out->p_display = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        p_out->p_window = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        return true;
    }
    return false;
}

extern "C" {

const char* ps_host_window_vulkan_extension(const PsHostWindowSurface* p_surface) {
    if (!p_surface) {
        return nullptr;
    }
    return p_surface->kind == KIND_XLIB      ? VK_KHR_XLIB_SURFACE_EXTENSION_NAME
           : p_surface->kind == KIND_WAYLAND ? VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
                                             : nullptr;
}

const char* ps_host_window_vulkan_system(const PsHostWindowSurface* p_surface) {
    return p_surface && p_surface->kind == KIND_XLIB ? "Xlib" : "Wayland";
}

// RenderDoc cannot capture a Wayland surface.
bool ps_host_window_vulkan_can_capture(const PsHostWindowSurface* p_surface) {
    return !p_surface || p_surface->kind != KIND_WAYLAND;
}

int ps_host_window_vulkan_surface(const PsHostWindowSurface* p_surface, VkInstance h_instance,
                                  PFN_vkGetInstanceProcAddr pfn_get_instance_proc_addr, VkSurfaceKHR* p_out_surface) {
    if (p_surface->kind == KIND_XLIB) {
        const auto pfn_create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
            pfn_get_instance_proc_addr(h_instance, "vkCreateXlibSurfaceKHR"));
        if (!pfn_create) {
            return ENOSYS;
        }
        const VkXlibSurfaceCreateInfoKHR xlib_ci = {
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy = static_cast<Display*>(p_surface->p_display),
            .window = reinterpret_cast<Window>(p_surface->p_window),
        };
        return pfn_create(h_instance, &xlib_ci, nullptr, p_out_surface) == VK_SUCCESS ? 0 : EIO;
    }
    const auto pfn_create = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        pfn_get_instance_proc_addr(h_instance, "vkCreateWaylandSurfaceKHR"));
    if (!pfn_create) {
        return ENOSYS;
    }
    const VkWaylandSurfaceCreateInfoKHR wayland_ci = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = static_cast<wl_display*>(p_surface->p_display),
        .surface = static_cast<wl_surface*>(p_surface->p_window),
    };
    return pfn_create(h_instance, &wayland_ci, nullptr, p_out_surface) == VK_SUCCESS ? 0 : EIO;
}

} // extern "C"
