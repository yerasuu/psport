// psport: what the shared SDL3 window backend (host_window.cpp) and each OS's surface part
// (gpu/platform/<os>/host_window_surface.cpp) share, inside the platform layer.
#pragma once
#include <SDL3/SDL.h>
#include "../../../src/platform/ps_host_window.h"

/// kind 0 is none; each OS numbers the window systems it presents to.
struct PsHostWindowSurface {
    int kind;
    void* p_display;
    void* p_window;
};

struct PsHostWindow {
    SDL_Window* p_sdl;
    PsHostWindowSurface surface;
};

/// gpu/platform/<os>/: reads the window system's handles of p_sdl into *p_out; false when the window
/// system is not one this OS presents to.
bool host_window_read_surface(SDL_Window* p_sdl, PsHostWindowSurface* p_out);
