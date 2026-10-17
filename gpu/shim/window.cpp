// psport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/logging/log.h"
#include "game_profile.h"
#include "sdl_window.h"
#include "psport_overlay.h"
#include "psport_settings.h"

namespace {
std::array<std::atomic<PsGpuEventFilter>, 4> s_event_filters{};

bool IsTakenByModule(const SDL_Event* p_event, SDL_Window* p_window) {
    for (const auto& slot : s_event_filters) {
        const PsGpuEventFilter pfn_filter = slot.load(std::memory_order_acquire);
        if (pfn_filter && pfn_filter(p_event, p_window)) {
            return true;
        }
    }
    return false;
}
} // namespace

extern "C" int psport_gpu_add_event_filter(PsGpuEventFilter pfn_filter) {
    for (auto& slot : s_event_filters) {
        PsGpuEventFilter pfn_empty = nullptr;
        if (slot.compare_exchange_strong(pfn_empty, pfn_filter)) {
            return 1;
        }
    }
    LOG_ERROR(Frontend, "Window: no free event filter slot: a game module does not see the window's events");
    return 0;
}

namespace Frontend {

namespace {

// Issue #69: the monitor the window (and fullscreen) goes to. PS_DISPLAY: its number in SDL's
// order (1, 2, ...; psport-gpu-capabilities --displays lists them) or a part of its name; without it
// SDL's primary display. The monitors are logged so a report says which one was taken.
SDL_DisplayID ChooseDisplay() {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    const char* sz_wanted = std::getenv("PS_DISPLAY");
    int c_displays = 0;
    SDL_DisplayID* p_ids = SDL_GetDisplays(&c_displays);
    SDL_DisplayID chosen = 0;
    if (sz_wanted && *sz_wanted) {
        char* sz_end = nullptr;
        const long number = std::strtol(sz_wanted, &sz_end, 10);
        if (sz_end && *sz_end == '\0') {
            if (number >= 1 && number <= c_displays) {
                chosen = p_ids[number - 1];
            }
        } else {
            for (int i = 0; i < c_displays && !chosen; ++i) {
                const char* sz_name = SDL_GetDisplayName(p_ids[i]);
                if (sz_name && strcasestr(sz_name, sz_wanted)) {
                    chosen = p_ids[i];
                }
            }
        }
    }
    const SDL_DisplayID display = chosen ? chosen : primary;
    for (int i = 0; i < c_displays; ++i) {
        const char* sz_name = SDL_GetDisplayName(p_ids[i]);
        const SDL_DisplayMode* p_mode = SDL_GetDesktopDisplayMode(p_ids[i]);
        std::printf("Display %d: %s %dx%d%s%s\n", i + 1, sz_name ? sz_name : "?", p_mode ? p_mode->w : 0,
                    p_mode ? p_mode->h : 0, p_ids[i] == primary ? " (primary)" : "",
                    p_ids[i] == display ? " <- the game's (PS_DISPLAY)" : "");
    }
    if (sz_wanted && *sz_wanted && !chosen) {
        std::printf("Display: PS_DISPLAY=%s matches none, the primary one is used\n", sz_wanted);
    }
    SDL_free(p_ids);
    return display;
}

// The monitor the window is on, in pixels: the output's aspect ratio "Auto" follows it.
void TellMonitor(SDL_Window* p_window) {
    if (const SDL_DisplayMode* p_mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(p_window))) {
        const float density = p_mode->pixel_density > 0.0f ? p_mode->pixel_density : 1.0f;
        PsSettings::SetMonitor(int(p_mode->w * density + 0.5f), int(p_mode->h * density + 0.5f));
    }
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* sz_title, const char* sz_icon_path)
    : m_width{width_}, m_height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    const char* sz_host_pad = std::getenv("PS_PAD_HOST");
    const SDL_InitFlags gamepad = sz_host_pad && sz_host_pad[0] == '0' ? 0 : SDL_INIT_GAMEPAD;
    // SDL_APP_ID from the environment still takes precedence over this identifier.
    SDL_SetAppMetadata("psport", nullptr, "psport");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | gamepad)) {
        LOG_ERROR(Frontend, "Failed to initialize SDL video: {}", SDL_GetError());
        return;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, sz_title);
    const SDL_DisplayID display = ChooseDisplay();
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* sz_fullscreen = std::getenv("PS_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, sz_fullscreen && sz_fullscreen[0] == '1');
    m_base_title = sz_title;
    m_p_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!m_p_window) {
        LOG_ERROR(Frontend, "Failed to create window: {}", SDL_GetError());
        return;
    }
    if (SDL_Surface* p_icon = SDL_LoadPNG(sz_icon_path)) {
        SDL_SetWindowIcon(m_p_window, p_icon);
        std::printf("Window icon: %s (%dx%d)\n", sz_icon_path, p_icon->w, p_icon->h);
        SDL_DestroySurface(p_icon);
    } else {
        std::printf("Window icon: %s: %s\n", sz_icon_path, SDL_GetError());
    }

    const char* sz_driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(m_p_window);
    if (sz_driver && !std::strcmp(sz_driver, "x11")) {
        m_window_info.type = WindowSystemType::X11;
        m_window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        m_window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (sz_driver && !std::strcmp(sz_driver, "wayland")) {
        m_window_info.type = WindowSystemType::Wayland;
        m_window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        m_window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        LOG_ERROR(Frontend, "Unsupported SDL video driver {}", sz_driver ? sz_driver : "(none)");
        SDL_DestroyWindow(m_p_window);
        m_p_window = nullptr;
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_p_window, &w, &h);
    m_width = w;
    m_height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, sz_driver);
    TellMonitor(m_p_window);
    std::printf("Window: %s (app id %s)\n", sz_title, SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING));
    m_is_host_mouse = SDL_HasMouse();
    m_is_host_keyboard = SDL_HasKeyboard();
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(m_p_window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{m_text_mutex};
    m_text = initial;
    m_text_prompt = prompt;
    m_text_state = 0;
    m_is_text_requested = true;
}

void WindowSDL::EndTextInput() {
    std::scoped_lock lock{m_text_mutex};
    m_is_text_requested = false;
    m_is_text_end_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{m_text_mutex};
    out = m_text;
    return m_text_state;
}

void WindowSDL::TakeMouse(double& dx, double& dy, int& up, int& down) {
    std::scoped_lock lock{m_mouse_mutex};
    dx = m_mouse_dx;
    dy = m_mouse_dy;
    up = m_c_wheel_up;
    down = m_c_wheel_down;
    m_mouse_dx = m_mouse_dy = 0;
    m_c_wheel_up = m_c_wheel_down = 0;
}

bool WindowSDL::TakeGameMouse(s32& dx, s32& dy, s32& wheel, s32& tilt, u32& buttons) {
    std::scoped_lock lock{m_mouse_mutex};
    dx = s32(m_game_dx);
    dy = s32(m_game_dy);
    wheel = s32(m_game_wheel);
    tilt = s32(m_game_tilt);
    // Fractions stay for the next call, so slow motion and smooth scrolling still add up.
    m_game_dx -= dx;
    m_game_dy -= dy;
    m_game_wheel -= float(wheel);
    m_game_tilt -= float(tilt);
    buttons = m_game_buttons;
    return m_is_host_mouse.load(std::memory_order_relaxed);
}

void WindowSDL::GameMouseEvent(const SDL_Event& event) {
    std::scoped_lock lock{m_mouse_mutex};
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        m_game_dx += event.motion.xrel;
        m_game_dy += event.motion.yrel;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        m_game_buttons |= SDL_BUTTON_MASK(event.button.button);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        m_game_buttons &= ~SDL_BUTTON_MASK(event.button.button);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        const float flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        m_game_wheel += event.wheel.y * flip;
        m_game_tilt += event.wheel.x * flip;
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        m_game_buttons = 0;
        break;
    case SDL_EVENT_MOUSE_ADDED:
    case SDL_EVENT_MOUSE_REMOVED:
        m_is_host_mouse = SDL_HasMouse();
        break;
    default:
        break;
    }
}

int WindowSDL::TakeGameKeys(PsKeyEvent* p_events, int max, bool& keyboard, u32& dropped) {
    std::scoped_lock lock{m_key_mutex};
    SyncGameKeyboard();
    int c_keys = 0;
    for (; c_keys < max && !m_game_keys.empty(); ++c_keys) {
        p_events[c_keys] = m_game_keys.front();
        m_game_keys.pop_front();
    }
    keyboard = m_is_host_keyboard.load(std::memory_order_relaxed);
    dropped = m_game_keys_dropped;
    return c_keys;
}

// Opening or closing the game's keyboard starts it afresh: nothing typed before reaches it, and
// keys still held when it opened are not reported released. Called under m_key_mutex.
void WindowSDL::SyncGameKeyboard() {
    const bool is_open = m_is_game_keyboard_open.load(std::memory_order_relaxed);
    if (is_open != m_is_game_keyboard_synced) {
        m_is_game_keyboard_synced = is_open;
        m_game_keys.clear();
        m_game_keys_held.reset();
    }
}

void WindowSDL::GameKeyEvent(const SDL_Event& event) {
    constexpr size_t queue_limit = 256;
    std::scoped_lock lock{m_key_mutex};
    SyncGameKeyboard();
    const SDL_Scancode scancode = event.key.scancode;
    if (!m_is_game_keyboard_synced || size_t(scancode) >= m_game_keys_held.size()) {
        return;
    }
    const bool is_down = event.type == SDL_EVENT_KEY_DOWN;
    if (!is_down && !m_game_keys_held.test(scancode)) {
        return;
    }
    m_game_keys_held.set(scancode, is_down);
    const SDL_Keycode key = SDL_GetKeyFromScancode(scancode, event.key.mod, false);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    if (m_game_keys.size() == queue_limit) {
        m_game_keys.pop_front();
        ++m_game_keys_dropped;
    }
    m_game_keys.push_back(PsKeyEvent{
        .time_us = u64(std::chrono::duration_cast<std::chrono::microseconds>(now).count()),
        .character = key & SDLK_SCANCODE_MASK ? 0u : u32(key),
        .scancode = u16(scancode),
        .mod = u16(event.key.mod),
        .down = u8(is_down),
        .repeat = u8(event.key.repeat),
    });
}

// Mouse look: relative mode while the window holds the mouse (runtime_pad.c turns its motion into
// the right stick). Motion from before is dropped, so taking the mouse does not turn the camera.
void WindowSDL::CaptureMouse(bool should_capture) {
    if (should_capture == m_is_mouse_captured.load(std::memory_order_relaxed)) {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(m_p_window, should_capture) && should_capture) {
        std::printf("Mouse look: relative mouse mode failed: %s\n", SDL_GetError());
        return;
    }
    {
        std::scoped_lock lock{m_mouse_mutex};
        m_mouse_dx = m_mouse_dy = 0;
        m_c_wheel_up = m_c_wheel_down = 0;
    }
    m_is_mouse_captured.store(should_capture, std::memory_order_relaxed);
    if (should_capture) {
        m_capture_settle_ms = SDL_GetTicks() + 100;
    }
    static bool s_has_told;
    if (should_capture && !s_has_told) {
        s_has_told = true;
        std::printf("Mouse look: on (F1 releases the mouse, a click takes it again)\n");
    }
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = m_is_text_active ? m_base_title + " \u2014 " + m_text_prompt + ": " + m_text + "_  (Enter = OK, Esc = cancel)"
                                          : m_base_title;
    SDL_SetWindowTitle(m_p_window, title.c_str());
    PsOverlay::SetTextPrompt(m_is_text_active, m_text_prompt, m_text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{m_text_mutex};
        if (m_is_text_end_requested) {
            m_is_text_end_requested = false;
            if (m_is_text_active) {
                m_is_text_active = false;
                m_text_state = 2;
                SDL_StopTextInput(m_p_window);
                UpdateTextTitle();
            }
        }
        if (m_is_text_requested) { // SDL text input must be toggled from the window thread
            m_is_text_requested = false;
            m_is_text_active = true;
            SDL_StartTextInput(m_p_window);
            UpdateTextTitle();
        }
    }
    if (!m_is_text_active) {
        PsOverlay::UpdateTextInput(m_p_window);
    }
    const Game::MouseHooks game_mouse = Game::Mouse();
    // A menu of the game's that the mouse points into (Game::MouseHooks pfn_wants_pointer).
    const bool is_pointer_wanted = game_mouse.pfn_wants_pointer && game_mouse.pfn_wants_pointer();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (IsTakenByModule(&event, m_p_window) ||
            (game_mouse.pfn_window_event && game_mouse.pfn_window_event(&event, m_p_window))) {
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            m_last_mouse_motion_ms = SDL_GetTicks();
            // Just taken: the pointer's jump into relative mode is not a turn of the camera.
            if (m_is_mouse_captured.load(std::memory_order_relaxed) && SDL_GetTicks() >= m_capture_settle_ms) {
                std::scoped_lock lock{m_mouse_mutex};
                m_mouse_dx += event.motion.xrel;
                m_mouse_dy += event.motion.yrel;
            }
        }
        // A release always reaches the game when it saw the press, so a key held while the menu or
        // the name dialog opened is not left held.
        if (event.type == SDL_EVENT_KEY_UP) {
            GameKeyEvent(event);
        }
        if (m_is_text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{m_text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                m_text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !m_text.empty()) {
                size_t cut = m_text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(m_text[cut]) & 0xC0) == 0x80) --cut;
                m_text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                m_text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                m_is_text_active = false;
                SDL_StopTextInput(m_p_window);
            }
            UpdateTextTitle();
            continue;
        }
        // A release always counts, so a button held while the menu opened is not left held.
        if (!PsOverlay::CapturesInput() || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            GameMouseEvent(event);
        }
        if (PsOverlay::HandleEvent(event)) {
            continue;
        }
        // Mouse look: a click in the game takes the mouse (that click is not passed on as a
        // button: runtime_pad.c reads the buttons only while it is held); F1 and leaving the
        // window let it go.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !m_is_mouse_captured.load(std::memory_order_relaxed) &&
            m_is_mouse_look.load(std::memory_order_relaxed) && !PsOverlay::MenuOpen() && !PsOverlay::GameCursorShown() &&
            !is_pointer_wanted) {
            CaptureMouse(true);
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F1 &&
            m_is_mouse_captured.load(std::memory_order_relaxed)) {
            CaptureMouse(false);
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN) {
            GameKeyEvent(event);
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && m_is_mouse_captured.load(std::memory_order_relaxed)) {
            const float notches = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            std::scoped_lock lock{m_mouse_mutex};
            (notches > 0 ? m_c_wheel_up : m_c_wheel_down) += notches != 0 ? 1 : 0;
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            CaptureMouse(false);
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(m_p_window, &w, &h);
            m_width = w;
            m_height = h;
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            TellMonitor(m_p_window);
            break;
        case SDL_EVENT_KEYBOARD_ADDED:
        case SDL_EVENT_KEYBOARD_REMOVED:
            m_is_host_keyboard = SDL_HasKeyboard();
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            // SDL turns SIGTERM and SIGINT into a quit event too, so a quit is not always the user's.
            if (m_is_open) {
                std::printf(event.type == SDL_EVENT_QUIT
                                ? "Window: quit requested (in-game menu, SIGTERM or SIGINT), the game ends\n"
                                : "Window: closed by the user, the game ends\n");
            }
            m_is_open = false;
            break;
        default:
            break;
        }
    }
    if (m_is_capture_requested.exchange(false, std::memory_order_relaxed) && !PsOverlay::MenuOpen() &&
        !m_is_text_active) {
        CaptureMouse(true);
    }
    // The menu, the name dialog, the game's own cursor and mouse_look=0 want the mouse as a pointer.
    if (m_is_mouse_captured.load(std::memory_order_relaxed) &&
        (PsOverlay::MenuOpen() || m_is_text_active || PsOverlay::GameCursorShown() ||
         !m_is_mouse_look.load(std::memory_order_relaxed))) {
        CaptureMouse(false);
    }
    // The game's pointer menus: the mouse goes, and comes back when they close.
    if (is_pointer_wanted && m_is_mouse_captured.load(std::memory_order_relaxed)) {
        CaptureMouse(false);
        m_is_released_for_pointer = true;
    } else if (!is_pointer_wanted && m_is_released_for_pointer) {
        m_is_released_for_pointer = false;
        if (m_is_mouse_look.load(std::memory_order_relaxed) && !PsOverlay::MenuOpen() && !m_is_text_active &&
            (SDL_GetWindowFlags(m_p_window) & SDL_WINDOW_INPUT_FOCUS)) {
            CaptureMouse(true);
        }
    }
    m_is_pointer_shown = is_pointer_wanted;
    UpdateCursor();
    return m_is_open;
}

// Issue #3: the OS cursor over the game. Hidden in fullscreen, while the game draws its own
// (VideoOut's hardware cursor), and in a window after 3 s without moving the mouse; always shown
// while the settings menu or a game's pointer menu is open.
void WindowSDL::UpdateCursor() {
    const bool is_fullscreen = (SDL_GetWindowFlags(m_p_window) & SDL_WINDOW_FULLSCREEN) != 0;
    const bool should_hide = !PsOverlay::MenuOpen() && !m_is_pointer_shown && (is_fullscreen || PsOverlay::GameCursorShown() ||
                                                 SDL_GetTicks() - m_last_mouse_motion_ms > 3000);
    if (should_hide != m_is_cursor_hidden) {
        m_is_cursor_hidden = should_hide;
        should_hide ? SDL_HideCursor() : SDL_ShowCursor();
    }
}

} // namespace Frontend
