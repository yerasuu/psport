/* Check whether native-size depth/stencil images can be blitted to reduced
 * renderer targets. The renderer needs both directions for live presets.
 * --gamepads: the connected gamepads, "GUID<tab>name" per line (the launcher's controller list,
 * PS_GAMEPAD). --displays: the monitors, "name<tab>WxH<tab>primary (1 or 0)" per line in SDL's
 * order (the launcher's monitor list, PS_DISPLAY; issue #69). --read-input: one key or button for the
 * launcher's controls (below). --device: "vendorID<tab>name" of the GPU the game takes.
 * --host-env: run.sh's settings from the quirk registry's HOST quirks for that GPU (below). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include "runtime_quirks.h"

static VkDeviceSize largest_local_heap(VkPhysicalDevice h_device) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(h_device, &memory);
    VkDeviceSize largest = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            memory.memoryHeaps[i].size > largest) {
            largest = memory.memoryHeaps[i].size;
        }
    }
    return largest;
}

static int better_device(VkPhysicalDevice h_candidate, VkPhysicalDevice h_current) {
    VkPhysicalDeviceProperties next, old;
    vkGetPhysicalDeviceProperties(h_candidate, &next);
    vkGetPhysicalDeviceProperties(h_current, &old);
    const int next_api = next.apiVersion >= VK_API_VERSION_1_3;
    const int old_api = old.apiVersion >= VK_API_VERSION_1_3;
    if (next_api != old_api) return next_api;
    const int next_discrete = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const int old_discrete = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if (next_discrete != old_discrete) return next_discrete;
    const int next_cpu = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const int old_cpu = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (next_cpu != old_cpu) return !next_cpu;
    return largest_local_heap(h_candidate) > largest_local_heap(h_current);
}

/* --live-resolution: prints 1 when live resolution changes suit the GPU (run.sh, setting
 * live_resolution=auto), else 0. Live scaling keeps the game's post-processing at 1080p and
 * copies scene targets every frame: fine on a strong discrete GPU, 7-8 FPS on the Steam Deck
 * and a GTX 1060. Rule: discrete, at least 8 GB of device memory, and not an NVIDIA GPU older
 * than Turing (no fragment shader barycentrics; its depth/stencil copies take nine draws). */
static int has_extension(VkPhysicalDevice h_device, const char *sz_name) {
    uint32_t c_extensions = 0;
    if (vkEnumerateDeviceExtensionProperties(h_device, NULL, &c_extensions, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *p_list = calloc(c_extensions ? c_extensions : 1, sizeof(*p_list));
    int is_found = 0;
    if (p_list && vkEnumerateDeviceExtensionProperties(h_device, NULL, &c_extensions, p_list) == VK_SUCCESS)
        for (uint32_t i = 0; i < c_extensions && !is_found; ++i) is_found = !strcmp(p_list[i].extensionName, sz_name);
    free(p_list);
    return is_found;
}

/* The facts vk_instance.cpp hands to runtime_quirks_note_gpu, so that a quirk applies here as it
 * does in psport. */
static int quirk_applies(VkPhysicalDevice h_device, RuntimeQuirkId id) {
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(h_device, &props);
    return runtime_quirk_applies_to_gpu(id, props.properties.vendorID, (uint32_t)driver.driverID,
                                        props.properties.deviceName, props.properties.driverVersion);
}

static int live_resolution_suits(VkPhysicalDevice h_device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(h_device, &props);
    const int discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const VkDeviceSize memory = largest_local_heap(h_device);
    const int old_nvidia = quirk_applies(h_device, RUNTIME_QUIRK_nvidia_slow_depth_copies) &&
                           !has_extension(h_device, "VK_KHR_fragment_shader_barycentric");
    const int suits = discrete && memory >= (VkDeviceSize)7680 << 20 && !old_nvidia;
    fprintf(stderr, "GPU: %s, %s, %llu MiB: live resolution changes %s\n", props.deviceName,
            discrete ? "discrete" : "integrated or other", (unsigned long long)(memory >> 20),
            suits ? "on" : "off (startup resolution patch)");
    return suits;
}

/* --host-env: "KEY=VALUE<tab>message" per line, the settings run.sh takes before psport runs (a
 * driver reads its variables when it loads), with the line run.sh prints for each. run.sh keeps the
 * user's own values and its PS_AUTO_NOHIZ switch; the memory model line is always there. */
static void print_host_env(VkPhysicalDevice h_device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(h_device, &props);
    if (quirk_applies(h_device, RUNTIME_QUIRK_amd_pc_memory_model))
        printf("PS_PC_MODEL=1\tMemory model: new (AMD GPU, %s; PS_PC_MODEL=0: the 0.3 model)\n", props.deviceName);
    else
        printf("PS_PC_MODEL=0\tMemory model: the 0.3 one (%s; PS_PC_MODEL=1: the new one)\n", props.deviceName);
    if (quirk_applies(h_device, RUNTIME_QUIRK_radv_gcn_nohiz))
        printf("RADV_DEBUG=nohiz\tRADV: %s: RADV_DEBUG=nohiz (depth compression artifacts on GCN 3-5; "
               "PS_AUTO_NOHIZ=0: off)\n",
               props.deviceName);
}

static int list_gamepads(void) {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gamepads: %s\n", SDL_GetError());
        return 1;
    }
    // Some devices (HIDAPI) show up only after events are pumped.
    for (int i = 0; i < 5; ++i) {
        SDL_PumpEvents();
        SDL_Delay(40);
    }
    int c_gamepads = 0;
    SDL_JoystickID *p_ids = SDL_GetGamepads(&c_gamepads);
    for (int i = 0; p_ids && i < c_gamepads; ++i) {
        char sz_guid[33];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(p_ids[i]), sz_guid, sizeof sz_guid);
        const char *sz_name = SDL_GetGamepadNameForID(p_ids[i]);
        printf("%s\t%s\n", sz_guid, sz_name ? sz_name : "?");
    }
    SDL_free(p_ids);
    SDL_Quit();
    return 0;
}

static int list_displays(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "displays: %s\n", SDL_GetError());
        return 1;
    }
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    int c_displays = 0;
    SDL_DisplayID *p_ids = SDL_GetDisplays(&c_displays);
    for (int i = 0; p_ids && i < c_displays; ++i) {
        const char *sz_name = SDL_GetDisplayName(p_ids[i]);
        const SDL_DisplayMode *p_mode = SDL_GetDesktopDisplayMode(p_ids[i]);
        printf("%s\t%dx%d\t%d\n", sz_name && *sz_name ? sz_name : "?", p_mode ? p_mode->w : 0,
               p_mode ? p_mode->h : 0, p_ids[i] == primary);
    }
    SDL_free(p_ids);
    SDL_Quit();
    return 0;
}

/* --read-input key|pad: a small window; prints "key <SDL key name>" ("Mouse Left", "Wheel Up", ...
 * for the mouse) or "pad <SDL button name>"|pad: a small window; prints "key <SDL key name>" or "pad <SDL button name>"
 * (lefttrigger/righttrigger for the triggers) for the first key or gamepad button pressed, the
 * names psport.ini's key.* and pad.* lines take. Escape, closing it or 15 s: nothing. */
static int read_input(const char *sz_kind) {
    const int should_read_key = strcmp(sz_kind, "pad") != 0, should_read_pad = strcmp(sz_kind, "key") != 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "read-input: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *p_window = NULL;
    SDL_Renderer *p_renderer = NULL;
    const char *sz_prompt = should_read_key && should_read_pad ? "Press a key, a mouse button or a gamepad button"
                         : should_read_key           ? "Press a key or a mouse button"
                                              : "Press a gamepad button";
    if (!SDL_CreateWindowAndRenderer("psport", 520, 90, 0, &p_window, &p_renderer)) {
        fprintf(stderr, "read-input: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    int c_gamepads = 0;
    SDL_JoystickID *p_ids = SDL_GetGamepads(&c_gamepads);
    for (int i = 0; p_ids && i < c_gamepads; ++i) SDL_OpenGamepad(p_ids[i]);
    SDL_free(p_ids);
    const Uint64 end = SDL_GetTicks() + 15000;
    int is_done = 0;
    while (!is_done && SDL_GetTicks() < end) {
        SDL_SetRenderDrawColor(p_renderer, 24, 24, 28, 255);
        SDL_RenderClear(p_renderer);
        SDL_SetRenderDrawColor(p_renderer, 230, 230, 230, 255);
        SDL_RenderDebugText(p_renderer, 16, 30, sz_prompt);
        SDL_RenderDebugText(p_renderer, 16, 50, "Escape: cancel");
        SDL_RenderPresent(p_renderer);
        SDL_Event e;
        while (!is_done && SDL_WaitEventTimeout(&e, 50)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                is_done = 1;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    is_done = 1;
                } else if (should_read_key) {
                    printf("key %s\n", SDL_GetScancodeName(e.key.scancode));
                    is_done = 1;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: /* the names runtime_pad.c's key.* lines take */
                if (should_read_key && e.button.button >= SDL_BUTTON_LEFT && e.button.button <= SDL_BUTTON_X2) {
                    static const char *const s_names[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right",
                                                        "Mouse X1", "Mouse X2"};
                    printf("key %s\n", s_names[e.button.button]);
                    is_done = 1;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (should_read_key && e.wheel.y != 0) {
                    const float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                    printf("key %s\n", y > 0 ? "Wheel Up" : "Wheel Down");
                    is_done = 1;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (should_read_pad) {
                    printf("pad %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e.gbutton.button));
                    is_done = 1;
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (should_read_pad && e.gaxis.value > 16000 &&
                    (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
                    printf("pad %s\n", e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "lefttrigger" : "righttrigger");
                    is_done = 1;
                }
                break;
            default:
                break;
            }
        }
    }
    fflush(stdout);
    SDL_DestroyRenderer(p_renderer);
    SDL_DestroyWindow(p_window);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--read-input")) {
        return read_input(argc > 2 ? argv[2] : "any");
    }
    if (argc > 1 && !strcmp(argv[1], "--gamepads")) {
        return list_gamepads();
    }
    if (argc > 1 && !strcmp(argv[1], "--displays")) {
        return list_displays();
    }
    const int live_mode = argc > 1 && !strcmp(argv[1], "--live-resolution");
    const int device_mode = argc > 1 && !strcmp(argv[1], "--device");
    const int host_env_mode = argc > 1 && !strcmp(argv[1], "--host-env");
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "psport scene scaling probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance h_instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, NULL, &h_instance) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot create Vulkan instance\n", stderr);
        return 1;
    }
    uint32_t c_devices = 0;
    if (vkEnumeratePhysicalDevices(h_instance, &c_devices, NULL) != VK_SUCCESS || !c_devices) {
        fputs("GPU scene scaling: no Vulkan device\n", stderr);
        vkDestroyInstance(h_instance, NULL);
        return 1;
    }
    VkPhysicalDevice *p_devices = calloc(c_devices, sizeof(*p_devices));
    if (!p_devices || vkEnumeratePhysicalDevices(h_instance, &c_devices, p_devices) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot enumerate Vulkan devices\n", stderr);
        free(p_devices);
        vkDestroyInstance(h_instance, NULL);
        return 1;
    }
    /* Match vk_instance.cpp's default ranking or its explicit PS_GPU_ID index. */
    VkPhysicalDevice h_selected = p_devices[0];
    const char *sz_gpu_id = getenv("PS_GPU_ID");
    if (sz_gpu_id && atoi(sz_gpu_id) >= 0) {
        const unsigned long i_gpu = strtoul(sz_gpu_id, NULL, 10);
        if (i_gpu >= c_devices) {
            fputs("GPU scene scaling: PS_GPU_ID is outside the device list\n", stderr);
            free(p_devices);
            vkDestroyInstance(h_instance, NULL);
            return 1;
        }
        h_selected = p_devices[i_gpu];
    } else {
        for (uint32_t i = 1; i < c_devices; ++i)
            if (better_device(p_devices[i], h_selected)) h_selected = p_devices[i];
    }
    if (device_mode) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(h_selected, &props);
        printf("%#06x\t%s\n", props.vendorID, props.deviceName);
        free(p_devices);
        vkDestroyInstance(h_instance, NULL);
        return 0;
    }
    if (host_env_mode) {
        print_host_env(h_selected);
        free(p_devices);
        vkDestroyInstance(h_instance, NULL);
        return 0;
    }
    if (live_mode) {
        printf("%d\n", live_resolution_suits(h_selected));
        free(p_devices);
        vkDestroyInstance(h_instance, NULL);
        return 0;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(h_selected, &props);
    const struct { VkFormat format; const char *sz_name; } formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
        {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
        {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"},
    };
    int is_supported = 1;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        VkFormatProperties features;
        vkGetPhysicalDeviceFormatProperties(h_selected, formats[i].format, &features);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                              VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((features.optimalTilingFeatures & required) != required) {
            fprintf(stderr, "GPU scene scaling: %s lacks blit support for %s\n",
                    props.deviceName, formats[i].sz_name);
            is_supported = 0;
        }
    }
    if (is_supported) fprintf(stderr, "GPU scene scaling: %s supports live presets\n", props.deviceName);
    free(p_devices);
    vkDestroyInstance(h_instance, NULL);
    return is_supported ? 0 : 1;
}
