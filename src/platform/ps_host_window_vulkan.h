/* The game window's Vulkan surface (decision #1009, phase 2b): the renderer creates its instance and
 * surface from the opaque PsHostWindowSurface that ps_host_window_surface hands out, and never names a
 * window system. Which ones an OS presents to, and how, is gpu/platform/<os>/'s (Xlib and Wayland on
 * Linux, Win32 on Windows). A NULL surface is no window: the renderer runs headless. */
#ifndef PS_HOST_WINDOW_VULKAN_H
#define PS_HOST_WINDOW_VULKAN_H
#include <stdbool.h>
#include <vulkan/vulkan_core.h>
#include "ps_host_window.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The instance extension the surface needs besides VK_KHR_surface; NULL for no surface. */
const char *ps_host_window_vulkan_extension(const PsHostWindowSurface *p_surface);
/* The window system's name in the renderer's log ("Xlib", "Wayland", "Win32"). */
const char *ps_host_window_vulkan_system(const PsHostWindowSurface *p_surface);
/* Whether a frame debugger (RenderDoc) can capture what is presented to the surface. */
bool ps_host_window_vulkan_can_capture(const PsHostWindowSurface *p_surface);
/* The surface of the window in *p_out_surface, created on an instance with the extension enabled, its
 * functions looked up through pfn_get_instance_proc_addr: 0, ENOSYS when the instance lacks the
 * function, or EIO when the driver refused. */
int ps_host_window_vulkan_surface(const PsHostWindowSurface *p_surface, VkInstance h_instance,
                                  PFN_vkGetInstanceProcAddr pfn_get_instance_proc_addr, VkSurfaceKHR *p_out_surface);

#ifdef __cplusplus
}
#endif
#endif
