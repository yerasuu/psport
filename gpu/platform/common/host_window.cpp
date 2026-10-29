// SPDX-License-Identifier: GPL-2.0-or-later
// psport: ps_host_window.h on SDL3, the same on Linux and Windows. The contract's keycodes are SDL's
// own values (checked below), as ps_host_input.h's keys, buttons and modifiers are, so they pass
// through unchanged.
#include <cerrno>
#include "host_window_sdl.h"

static_assert(PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK == SDLK_SCANCODE_MASK &&
              PS_HOST_WINDOW_KEYCODE_UNKNOWN == SDLK_UNKNOWN);
static_assert(PS_HOST_WINDOW_KEYCODE_BACKSPACE == SDLK_BACKSPACE && PS_HOST_WINDOW_KEYCODE_TAB == SDLK_TAB &&
              PS_HOST_WINDOW_KEYCODE_RETURN == SDLK_RETURN && PS_HOST_WINDOW_KEYCODE_ESCAPE == SDLK_ESCAPE &&
              PS_HOST_WINDOW_KEYCODE_SPACE == SDLK_SPACE && PS_HOST_WINDOW_KEYCODE_COMMA == SDLK_COMMA &&
              PS_HOST_WINDOW_KEYCODE_PERIOD == SDLK_PERIOD && PS_HOST_WINDOW_KEYCODE_SLASH == SDLK_SLASH &&
              PS_HOST_WINDOW_KEYCODE_LEFTBRACKET == SDLK_LEFTBRACKET &&
              PS_HOST_WINDOW_KEYCODE_RIGHTBRACKET == SDLK_RIGHTBRACKET && PS_HOST_WINDOW_KEYCODE_DELETE == SDLK_DELETE);
static_assert(PS_HOST_WINDOW_KEYCODE_A == SDLK_A && PS_HOST_WINDOW_KEYCODE_C == SDLK_C &&
              PS_HOST_WINDOW_KEYCODE_D == SDLK_D && PS_HOST_WINDOW_KEYCODE_E == SDLK_E &&
              PS_HOST_WINDOW_KEYCODE_Q == SDLK_Q && PS_HOST_WINDOW_KEYCODE_S == SDLK_S &&
              PS_HOST_WINDOW_KEYCODE_V == SDLK_V && PS_HOST_WINDOW_KEYCODE_W == SDLK_W &&
              PS_HOST_WINDOW_KEYCODE_X == SDLK_X && PS_HOST_WINDOW_KEYCODE_Z == SDLK_Z);
static_assert(PS_HOST_WINDOW_KEYCODE_F1 == SDLK_F1 && PS_HOST_WINDOW_KEYCODE_F2 == SDLK_F2 &&
              PS_HOST_WINDOW_KEYCODE_F12 == SDLK_F12 && PS_HOST_WINDOW_KEYCODE_F12 - PS_HOST_WINDOW_KEYCODE_F2 == 10 &&
              PS_HOST_WINDOW_KEYCODE_INSERT == SDLK_INSERT && PS_HOST_WINDOW_KEYCODE_HOME == SDLK_HOME &&
              PS_HOST_WINDOW_KEYCODE_PAGEUP == SDLK_PAGEUP && PS_HOST_WINDOW_KEYCODE_END == SDLK_END &&
              PS_HOST_WINDOW_KEYCODE_PAGEDOWN == SDLK_PAGEDOWN && PS_HOST_WINDOW_KEYCODE_RIGHT == SDLK_RIGHT &&
              PS_HOST_WINDOW_KEYCODE_LEFT == SDLK_LEFT && PS_HOST_WINDOW_KEYCODE_DOWN == SDLK_DOWN &&
              PS_HOST_WINDOW_KEYCODE_UP == SDLK_UP && PS_HOST_WINDOW_KEYCODE_KP_ENTER == SDLK_KP_ENTER);
static_assert(PS_HOST_WINDOW_KEYCODE_LCTRL == SDLK_LCTRL && PS_HOST_WINDOW_KEYCODE_LSHIFT == SDLK_LSHIFT &&
              PS_HOST_WINDOW_KEYCODE_LALT == SDLK_LALT && PS_HOST_WINDOW_KEYCODE_RCTRL == SDLK_RCTRL &&
              PS_HOST_WINDOW_KEYCODE_RSHIFT == SDLK_RSHIFT && PS_HOST_WINDOW_KEYCODE_RALT == SDLK_RALT);
static_assert(PS_HOST_WINDOW_MOD_CTRL == SDL_KMOD_CTRL && PS_HOST_WINDOW_MOD_SHIFT == SDL_KMOD_SHIFT &&
              PS_HOST_WINDOW_MOD_ALT == SDL_KMOD_ALT);
static_assert(PS_HOST_INPUT_MOUSE_LEFT == SDL_BUTTON_LEFT && PS_HOST_INPUT_MOUSE_RIGHT == SDL_BUTTON_RIGHT &&
              PS_HOST_INPUT_MOUSE_MIDDLE == SDL_BUTTON_MIDDLE);
static_assert(sizeof(PsHostWindowId) == sizeof(SDL_WindowID) && sizeof(PsHostDisplayId) == sizeof(SDL_DisplayID) &&
              sizeof(PsHostPadId) == sizeof(SDL_JoystickID));

extern "C" {

int ps_host_window_start(bool has_gamepads) {
    // SDL_APP_ID from the environment still takes precedence over this identifier.
    SDL_SetAppMetadata("psport", nullptr, "psport");
    return SDL_InitSubSystem(SDL_INIT_VIDEO | (has_gamepads ? SDL_INIT_GAMEPAD : 0)) ? 0 : EIO;
}
const char* ps_host_window_driver(void) { return SDL_GetCurrentVideoDriver(); }
const char* ps_host_window_app_id(void) { return SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING); }
uint64_t ps_host_window_ticks_ms(void) { return SDL_GetTicks(); }
void ps_host_window_delay_ms(uint32_t delay_ms) { SDL_Delay(delay_ms); }
bool ps_host_window_has_mouse(void) { return SDL_HasMouse(); }
bool ps_host_window_has_keyboard(void) { return SDL_HasKeyboard(); }
const char* ps_host_window_mouse_name(PsHostMouseId h_mouse) { return SDL_GetMouseNameForID(h_mouse); }
uint32_t ps_host_window_keycode(int scancode, uint16_t mod) {
    return SDL_GetKeyFromScancode(SDL_Scancode(scancode), mod, false);
}
void ps_host_window_free(void* p_memory) { SDL_free(p_memory); }
const char* ps_host_window_error(void) { return SDL_GetError(); }

PsHostDisplayId ps_host_window_primary_display(void) { return SDL_GetPrimaryDisplay(); }
PsHostDisplayId* ps_host_window_displays(int* p_out_count) { return SDL_GetDisplays(p_out_count); }
const char* ps_host_window_display_name(PsHostDisplayId h_display) { return SDL_GetDisplayName(h_display); }
bool ps_host_window_display_is_named(PsHostDisplayId h_display, const char* sz_part) {
    const char* sz_name = SDL_GetDisplayName(h_display);
    return sz_name && SDL_strcasestr(sz_name, sz_part);
}
int ps_host_window_display_mode(PsHostDisplayId h_display, PsHostDisplayMode* p_out) {
    const SDL_DisplayMode* p_mode = SDL_GetDesktopDisplayMode(h_display);
    if (!p_mode) {
        return EIO;
    }
    *p_out = {p_mode->w, p_mode->h, p_mode->pixel_density, p_mode->refresh_rate};
    return 0;
}

PsHostWindow* ps_host_window_create(const char* sz_title, PsHostDisplayId h_display, int width, int height,
                                    bool is_fullscreen) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, sz_title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(h_display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(h_display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, is_fullscreen);
    SDL_Window* p_sdl = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return p_sdl ? new PsHostWindow{p_sdl, {}} : nullptr;
}
void ps_host_window_destroy(PsHostWindow* p_window) {
    if (p_window) {
        SDL_DestroyWindow(p_window->p_sdl);
        delete p_window;
    }
}
int ps_host_window_set_icon(PsHostWindow* p_window, const char* sz_png_path, int* p_out_width, int* p_out_height) {
    SDL_Surface* p_icon = SDL_LoadPNG(sz_png_path);
    if (!p_icon) {
        return EIO;
    }
    SDL_SetWindowIcon(p_window->p_sdl, p_icon);
    *p_out_width = p_icon->w;
    *p_out_height = p_icon->h;
    SDL_DestroySurface(p_icon);
    return 0;
}
void ps_host_window_set_title(PsHostWindow* p_window, const char* sz_title) {
    SDL_SetWindowTitle(p_window->p_sdl, sz_title);
}
void ps_host_window_size(PsHostWindow* p_window, int* p_out_width, int* p_out_height) {
    SDL_GetWindowSize(p_window->p_sdl, p_out_width, p_out_height);
}
void ps_host_window_size_in_pixels(PsHostWindow* p_window, int* p_out_width, int* p_out_height) {
    SDL_GetWindowSizeInPixels(p_window->p_sdl, p_out_width, p_out_height);
}
PsHostDisplayId ps_host_window_display(PsHostWindow* p_window) { return SDL_GetDisplayForWindow(p_window->p_sdl); }
bool ps_host_window_is_fullscreen(PsHostWindow* p_window) {
    return (SDL_GetWindowFlags(p_window->p_sdl) & SDL_WINDOW_FULLSCREEN) != 0;
}
bool ps_host_window_has_input_focus(PsHostWindow* p_window) {
    return (SDL_GetWindowFlags(p_window->p_sdl) & SDL_WINDOW_INPUT_FOCUS) != 0;
}
const PsHostWindowSurface* ps_host_window_surface(PsHostWindow* p_window) {
    if (!p_window->surface.kind && !host_window_read_surface(p_window->p_sdl, &p_window->surface)) {
        return nullptr;
    }
    return &p_window->surface;
}
int ps_host_window_set_relative_mouse(PsHostWindow* p_window, bool is_relative) {
    return SDL_SetWindowRelativeMouseMode(p_window->p_sdl, is_relative) ? 0 : EIO;
}
void ps_host_window_show_cursor(bool is_shown) {
    is_shown ? SDL_ShowCursor() : SDL_HideCursor();
}
bool ps_host_window_mouse_focus(PsHostWindowId* p_out_window, float* p_out_x, float* p_out_y) {
    SDL_Window* p_sdl = SDL_GetMouseFocus();
    if (!p_sdl) {
        return false;
    }
    SDL_GetMouseState(p_out_x, p_out_y);
    *p_out_window = SDL_GetWindowID(p_sdl);
    return true;
}
float ps_host_window_pixel_density(PsHostWindowId h_window) {
    SDL_Window* p_sdl = SDL_GetWindowFromID(h_window);
    return p_sdl ? SDL_GetWindowPixelDensity(p_sdl) : 0.0f;
}
void ps_host_window_text_input(PsHostWindow* p_window, bool is_on) {
    is_on ? SDL_StartTextInput(p_window->p_sdl) : SDL_StopTextInput(p_window->p_sdl);
}
bool ps_host_window_is_text_input(PsHostWindow* p_window) { return SDL_TextInputActive(p_window->p_sdl); }

bool ps_host_window_poll_event(PsHostWindowEvent* p_out) {
    SDL_Event event;
    if (!SDL_PollEvent(&event)) {
        return false;
    }
    *p_out = {};
    switch (event.type) {
    case SDL_EVENT_QUIT:
        p_out->kind = PS_HOST_WINDOW_EVENT_QUIT;
        return true;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        p_out->kind = PS_HOST_WINDOW_EVENT_CLOSE_REQUESTED;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
        p_out->kind = PS_HOST_WINDOW_EVENT_RESIZED;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        p_out->kind = PS_HOST_WINDOW_EVENT_PIXEL_SIZE_CHANGED;
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        p_out->kind = PS_HOST_WINDOW_EVENT_DISPLAY_CHANGED;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        p_out->kind = PS_HOST_WINDOW_EVENT_FOCUS_GAINED;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        p_out->kind = PS_HOST_WINDOW_EVENT_FOCUS_LOST;
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        p_out->kind = PS_HOST_WINDOW_EVENT_MOUSE_LEAVE;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        p_out->kind = event.type == SDL_EVENT_KEY_DOWN ? PS_HOST_WINDOW_EVENT_KEY_DOWN : PS_HOST_WINDOW_EVENT_KEY_UP;
        p_out->h_window = event.key.windowID;
        p_out->key = {int(event.key.scancode), event.key.key, event.key.mod, event.key.repeat};
        return true;
    case SDL_EVENT_TEXT_INPUT:
        p_out->kind = PS_HOST_WINDOW_EVENT_TEXT_INPUT;
        p_out->h_window = event.text.windowID;
        p_out->text.sz_utf8 = event.text.text;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        p_out->kind = PS_HOST_WINDOW_EVENT_MOUSE_MOTION;
        p_out->h_window = event.motion.windowID;
        p_out->motion = {event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel, event.motion.which};
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        p_out->kind = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN
                                                                 : PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP;
        p_out->h_window = event.button.windowID;
        p_out->button = {event.button.button, event.button.x, event.button.y};
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        p_out->kind = PS_HOST_WINDOW_EVENT_MOUSE_WHEEL;
        p_out->h_window = event.wheel.windowID;
        p_out->wheel = {event.wheel.x, event.wheel.y, event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED};
        return true;
    case SDL_EVENT_MOUSE_ADDED:
        p_out->kind = PS_HOST_WINDOW_EVENT_MOUSE_ADDED;
        return true;
    case SDL_EVENT_MOUSE_REMOVED:
        p_out->kind = PS_HOST_WINDOW_EVENT_MOUSE_REMOVED;
        return true;
    case SDL_EVENT_KEYBOARD_ADDED:
        p_out->kind = PS_HOST_WINDOW_EVENT_KEYBOARD_ADDED;
        return true;
    case SDL_EVENT_KEYBOARD_REMOVED:
        p_out->kind = PS_HOST_WINDOW_EVENT_KEYBOARD_REMOVED;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        p_out->kind = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? PS_HOST_WINDOW_EVENT_PAD_BUTTON_DOWN
                                                                   : PS_HOST_WINDOW_EVENT_PAD_BUTTON_UP;
        p_out->pad_button = {event.gbutton.which, event.gbutton.button};
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        p_out->kind = PS_HOST_WINDOW_EVENT_PAD_AXIS;
        p_out->pad_axis = {event.gaxis.which, event.gaxis.axis, event.gaxis.value};
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        p_out->kind = PS_HOST_WINDOW_EVENT_PAD_REMOVED;
        p_out->pad_button = {event.gdevice.which, PS_HOST_PAD_BUTTON_INVALID};
        return true;
    default:
        return true;
    }
    p_out->h_window = event.window.windowID;
    return true;
}

int ps_host_window_request_quit(void) {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    return SDL_PushEvent(&event) ? 0 : EIO;
}

} // extern "C"
