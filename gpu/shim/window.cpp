// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the game window for the Vulkan swapchain, on the platform layer's (ps_host_window.h).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/logging/log.h"
#include "game_profile.h"
#include "sdl_window.h"
#include "psport_keyboard.h"
#include "psport_overlay.h"
#include "psport_settings.h"

extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));

namespace {
std::array<std::atomic<PsGpuEventFilter>, 4> s_event_filters{};
std::atomic<u64> s_c_pad_mouse_dropped{0};
std::atomic<bool> s_is_pad_mouse_summary_added{false};

void SummarizePadMouse(char* sz_out, std::size_t cb_out) {
    std::snprintf(sz_out, cb_out,
                  "Mouse: %llu motion events dropped while a controller stick was pushed (mouse_pad_filter)\n",
                  static_cast<unsigned long long>(s_c_pad_mouse_dropped.load(std::memory_order_relaxed)));
}

bool IsTakenByModule(const PsHostWindowEvent* p_event, PsHostWindow* p_window) {
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

// Issue #69: the monitor the window (and fullscreen) goes to. PS_DISPLAY: its number in the host's
// order (1, 2, ...; psport-gpu-capabilities --displays lists them) or a part of its name; without it
// the host's primary display. The monitors are logged so a report says which one was taken.
PsHostDisplayId ChooseDisplay() {
    const PsHostDisplayId primary = ps_host_window_primary_display();
    const char* sz_wanted = std::getenv("PS_DISPLAY");
    int c_displays = 0;
    PsHostDisplayId* p_ids = ps_host_window_displays(&c_displays);
    PsHostDisplayId chosen = 0;
    if (sz_wanted && *sz_wanted) {
        char* sz_end = nullptr;
        const long number = std::strtol(sz_wanted, &sz_end, 10);
        if (sz_end && *sz_end == '\0') {
            if (number >= 1 && number <= c_displays) {
                chosen = p_ids[number - 1];
            }
        } else {
            for (int i = 0; i < c_displays && !chosen; ++i) {
                if (ps_host_window_display_is_named(p_ids[i], sz_wanted)) {
                    chosen = p_ids[i];
                }
            }
        }
    }
    const PsHostDisplayId display = chosen ? chosen : primary;
    for (int i = 0; i < c_displays; ++i) {
        const char* sz_name = ps_host_window_display_name(p_ids[i]);
        PsHostDisplayMode mode{};
        ps_host_window_display_mode(p_ids[i], &mode);
        std::printf("Display %d: %s %dx%d%s%s\n", i + 1, sz_name ? sz_name : "?", mode.width, mode.height,
                    p_ids[i] == primary ? " (primary)" : "", p_ids[i] == display ? " <- the game's (PS_DISPLAY)" : "");
    }
    if (sz_wanted && *sz_wanted && !chosen) {
        std::printf("Display: PS_DISPLAY=%s matches none, the primary one is used\n", sz_wanted);
    }
    ps_host_window_free(p_ids);
    return display;
}

// The monitor the window is on, in pixels: the output's aspect ratio "Auto" follows it, and frame
// generation's pacing its refresh rate.
void TellMonitor(PsHostWindow* p_window) {
    if (PsHostDisplayMode mode{}; ps_host_window_display_mode(ps_host_window_display(p_window), &mode) == 0) {
        const float density = mode.pixel_density > 0.0f ? mode.pixel_density : 1.0f;
        PsSettings::SetMonitor(int(mode.width * density + 0.5f), int(mode.height * density + 0.5f));
        PsSettings::Get().display_refresh_hz.store(mode.refresh_hz > 0.0f ? int(mode.refresh_hz + 0.5f) : 0,
                                                   std::memory_order_relaxed);
    }
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* sz_title, const char* sz_icon_path)
    : m_width{width_}, m_height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    const char* sz_host_pad = std::getenv("PS_PAD_HOST");
    if (ps_host_window_start(!(sz_host_pad && sz_host_pad[0] == '0')) != 0) {
        LOG_ERROR(Frontend, "Failed to initialize SDL video: {}", ps_host_window_error());
        return;
    }
    const PsHostDisplayId display = ChooseDisplay();
    const char* sz_fullscreen = std::getenv("PS_FULLSCREEN");
    m_base_title = sz_title;
    m_p_window = ps_host_window_create(sz_title, display, width_, height_, sz_fullscreen && sz_fullscreen[0] == '1');
    if (!m_p_window) {
        LOG_ERROR(Frontend, "Failed to create window: {}", ps_host_window_error());
        return;
    }
    if (int icon_width = 0, icon_height = 0;
        ps_host_window_set_icon(m_p_window, sz_icon_path, &icon_width, &icon_height) == 0) {
        std::printf("Window icon: %s (%dx%d)\n", sz_icon_path, icon_width, icon_height);
    } else {
        std::printf("Window icon: %s: %s\n", sz_icon_path, ps_host_window_error());
    }

    const char* sz_driver = ps_host_window_driver();
    m_p_surface = ps_host_window_surface(m_p_window);
    if (!m_p_surface) {
        LOG_ERROR(Frontend, "Unsupported SDL video driver {}", sz_driver ? sz_driver : "(none)");
        ps_host_window_destroy(m_p_window);
        m_p_window = nullptr;
        return;
    }
    int w = 0, h = 0;
    ps_host_window_size_in_pixels(m_p_window, &w, &h);
    m_width = w;
    m_height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, sz_driver);
    TellMonitor(m_p_window);
    std::printf("Window: %s (app id %s)\n", sz_title, ps_host_window_app_id());
    m_is_host_mouse = ps_host_window_has_mouse();
    m_is_host_keyboard = ps_host_window_has_keyboard();
}

WindowSDL::~WindowSDL() {
    ps_host_window_destroy(m_p_window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt, u32 max_length, int charset) {
    PsOverlay::OpenKeyboard(charset, max_length);
    std::scoped_lock lock{m_text_mutex};
    m_text = initial;
    m_text_prompt = prompt;
    m_text_state = 0;
    m_is_text_requested = true;
}

void WindowSDL::ReplaceTextInput(const std::string& text) {
    std::scoped_lock lock{m_text_mutex};
    m_text = text;
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

void WindowSDL::GameMouseEvent(const PsHostWindowEvent& event) {
    std::scoped_lock lock{m_mouse_mutex};
    switch (event.kind) {
    case PS_HOST_WINDOW_EVENT_MOUSE_MOTION:
        m_game_dx += event.motion.dx;
        m_game_dy += event.motion.dy;
        break;
    case PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN:
        m_game_buttons |= PS_HOST_INPUT_MOUSE_MASK(event.button.button);
        break;
    case PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP:
        m_game_buttons &= ~PS_HOST_INPUT_MOUSE_MASK(event.button.button);
        break;
    case PS_HOST_WINDOW_EVENT_MOUSE_WHEEL: {
        const float flip = event.wheel.is_flipped ? -1.0f : 1.0f;
        m_game_wheel += event.wheel.y * flip;
        m_game_tilt += event.wheel.x * flip;
        break;
    }
    case PS_HOST_WINDOW_EVENT_FOCUS_LOST:
        m_game_buttons = 0;
        break;
    case PS_HOST_WINDOW_EVENT_MOUSE_ADDED:
    case PS_HOST_WINDOW_EVENT_MOUSE_REMOVED:
        m_is_host_mouse = ps_host_window_has_mouse();
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

void WindowSDL::GameKeyEvent(const PsHostWindowEvent& event) {
    constexpr size_t queue_limit = 256;
    std::scoped_lock lock{m_key_mutex};
    SyncGameKeyboard();
    const int scancode = event.key.scancode;
    if (!m_is_game_keyboard_synced || size_t(scancode) >= m_game_keys_held.size()) {
        return;
    }
    const bool is_down = event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN;
    if (!is_down && !m_game_keys_held.test(scancode)) {
        return;
    }
    m_game_keys_held.set(scancode, is_down);
    const u32 key = ps_host_window_keycode(scancode, event.key.mod);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    if (m_game_keys.size() == queue_limit) {
        m_game_keys.pop_front();
        ++m_game_keys_dropped;
    }
    m_game_keys.push_back(PsKeyEvent{
        .time_us = u64(std::chrono::duration_cast<std::chrono::microseconds>(now).count()),
        .character = key & PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK ? 0u : key,
        .scancode = u16(scancode),
        .mod = u16(event.key.mod),
        .down = u8(is_down),
        .repeat = u8(event.key.is_repeat),
    });
}

// Mouse look: relative mode while the window holds the mouse (runtime_pad.c turns its motion into
// the right stick). Motion from before is dropped, so taking the mouse does not turn the camera.
void WindowSDL::CaptureMouse(bool should_capture) {
    if (should_capture == m_is_mouse_captured.load(std::memory_order_relaxed)) {
        return;
    }
    if (ps_host_window_set_relative_mouse(m_p_window, should_capture) != 0 && should_capture) {
        std::printf("Mouse look: relative mouse mode failed: %s\n", ps_host_window_error());
        return;
    }
    {
        std::scoped_lock lock{m_mouse_mutex};
        m_mouse_dx = m_mouse_dy = 0;
        m_c_wheel_up = m_c_wheel_down = 0;
    }
    m_is_mouse_captured.store(should_capture, std::memory_order_relaxed);
    if (should_capture) {
        m_capture_settle_ms = ps_host_window_ticks_ms() + 100;
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
    ps_host_window_set_title(m_p_window, title.c_str());
    PsOverlay::SetTextPrompt(m_is_text_active, m_text_prompt, m_text);
}

// Circle erases, and once nothing is left cancels, as the dialog's back button.
void WindowSDL::PressKeyboardKey(const PsOverlay::KeyPress& press) {
    std::scoped_lock lock{m_text_mutex};
    if (press.action == PsOverlay::KeyAction::None) {
        return;
    }
    if (press.action == PsOverlay::KeyAction::Accept ||
        (press.action == PsOverlay::KeyAction::Back && m_text.empty())) {
        m_text_state = press.action == PsOverlay::KeyAction::Accept ? 1 : 2;
        m_is_text_active = false;
        ps_host_window_text_input(m_p_window, false);
    } else if (press.action == PsOverlay::KeyAction::Type) {
        PsOverlay::TypeInto(m_text, press.text);
    } else {
        PsOverlay::EraseLast(m_text);
    }
    UpdateTextTitle();
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{m_text_mutex};
        if (m_is_text_end_requested) {
            m_is_text_end_requested = false;
            if (m_is_text_active) {
                m_is_text_active = false;
                m_text_state = 2;
                ps_host_window_text_input(m_p_window, false);
                UpdateTextTitle();
            }
        }
        if (m_is_text_requested) { // text input must be toggled from the window thread
            m_is_text_requested = false;
            m_is_text_active = true;
            ps_host_window_text_input(m_p_window, true);
            UpdateTextTitle();
        }
    }
    if (!m_is_text_active) {
        PsOverlay::UpdateTextInput(m_p_window);
    }
    const Game::MouseHooks game_mouse = Game::Mouse();
    // A menu of the game's that the mouse points into (Game::MouseHooks pfn_wants_pointer).
    const bool is_pointer_wanted = game_mouse.pfn_wants_pointer && game_mouse.pfn_wants_pointer();
    PsHostWindowEvent event;
    while (ps_host_window_poll_event(&event)) {
        TrackPadSticks(event);
        if (IsPadMouseMotion(event)) {
            if (!s_is_pad_mouse_summary_added.exchange(true, std::memory_order_relaxed)) {
                runtime_calls_add_summary(&SummarizePadMouse);
                const char* sz_mouse = ps_host_window_mouse_name(event.motion.h_mouse);
                std::printf("Mouse: motion from mouse %u (%s) while a controller stick is pushed is dropped: a mapping "
                            "program (Steam's Desktop Layout, reWASD) makes it from the stick; mouse_pad_filter=0 "
                            "keeps it\n",
                            event.motion.h_mouse, sz_mouse ? sz_mouse : "no name");
            }
            s_c_pad_mouse_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (IsTakenByModule(&event, m_p_window) ||
            (game_mouse.pfn_window_event && game_mouse.pfn_window_event(&event, m_p_window))) {
            continue;
        }
        if (event.kind == PS_HOST_WINDOW_EVENT_MOUSE_MOTION) {
            m_last_mouse_motion_ms = ps_host_window_ticks_ms();
            // Just taken: the pointer's jump into relative mode is not a turn of the camera.
            if (m_is_mouse_captured.load(std::memory_order_relaxed) &&
                ps_host_window_ticks_ms() >= m_capture_settle_ms) {
                std::scoped_lock lock{m_mouse_mutex};
                m_mouse_dx += event.motion.dx;
                m_mouse_dy += event.motion.dy;
            }
        }
        // A release always reaches the game when it saw the press, so a key held while the menu or
        // the name dialog opened is not left held.
        if (event.kind == PS_HOST_WINDOW_EVENT_KEY_UP) {
            GameKeyEvent(event);
        }
        if (m_is_text_active &&
            (event.kind == PS_HOST_WINDOW_EVENT_TEXT_INPUT || event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{m_text_mutex};
            if (event.kind == PS_HOST_WINDOW_EVENT_TEXT_INPUT) {
                PsOverlay::TypeInto(m_text, event.text.sz_utf8);
            } else if (event.key.keycode == PS_HOST_WINDOW_KEYCODE_BACKSPACE) {
                PsOverlay::EraseLast(m_text);
            } else if (event.key.keycode == PS_HOST_WINDOW_KEYCODE_RETURN ||
                       event.key.keycode == PS_HOST_WINDOW_KEYCODE_KP_ENTER ||
                       event.key.keycode == PS_HOST_WINDOW_KEYCODE_ESCAPE) {
                m_text_state = event.key.keycode == PS_HOST_WINDOW_KEYCODE_ESCAPE ? 2 : 1;
                m_is_text_active = false;
                ps_host_window_text_input(m_p_window, false);
            }
            UpdateTextTitle();
            continue;
        }
        // A player with only a controller types on the overlay's keyboard; the buttons it leaves
        // (L3+R3, Guide) and the menu or close prompt they open stay the overlay's.
        if (m_is_text_active && !PsOverlay::MenuOpen() && !PsOverlay::ClosePromptOpen()) {
            if (event.kind == PS_HOST_WINDOW_EVENT_PAD_AXIS) {
                PsOverlay::KeyboardAxis(u8(event.pad_axis.axis), event.pad_axis.value);
                continue;
            }
            PsOverlay::KeyPress press;
            const bool is_pad_down = event.kind == PS_HOST_WINDOW_EVENT_PAD_BUTTON_DOWN;
            if ((is_pad_down || event.kind == PS_HOST_WINDOW_EVENT_PAD_BUTTON_UP) &&
                PsOverlay::KeyboardButton(u8(event.pad_button.button), is_pad_down, press)) {
                PressKeyboardKey(press);
                continue;
            }
        }
        // A release always counts, so a button held while the menu opened is not left held.
        if (!PsOverlay::CapturesInput() || event.kind == PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP) {
            GameMouseEvent(event);
        }
        if (PsOverlay::HandleEvent(event)) {
            continue;
        }
        // Mouse look: a click in the game takes the mouse (that click is not passed on as a
        // button: runtime_pad.c reads the buttons only while it is held); F1 and leaving the
        // window let it go.
        if (event.kind == PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN &&
            !m_is_mouse_captured.load(std::memory_order_relaxed) && m_is_mouse_look.load(std::memory_order_relaxed) &&
            !PsOverlay::MenuOpen() && !PsOverlay::GameCursorShown() && !is_pointer_wanted) {
            m_focus_recapture_ms = 0;
            CaptureMouse(true);
            continue;
        }
        if (event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN && !event.key.is_repeat &&
            event.key.keycode == PS_HOST_WINDOW_KEYCODE_F1 &&
            m_is_mouse_captured.load(std::memory_order_relaxed)) {
            CaptureMouse(false);
            continue;
        }
        if (event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN) {
            GameKeyEvent(event);
        }
        if (event.kind == PS_HOST_WINDOW_EVENT_MOUSE_WHEEL && m_is_mouse_captured.load(std::memory_order_relaxed)) {
            const float notches = event.wheel.is_flipped ? -event.wheel.y : event.wheel.y;
            std::scoped_lock lock{m_mouse_mutex};
            (notches > 0 ? m_c_wheel_up : m_c_wheel_down) += notches != 0 ? 1 : 0;
        }
        if (event.kind == PS_HOST_WINDOW_EVENT_FOCUS_LOST) {
            m_is_released_for_focus = m_is_mouse_captured.load(std::memory_order_relaxed);
            m_focus_recapture_ms = 0;
            CaptureMouse(false);
        }
        // Back into the game: the mouse it held comes back, one let go with F1 stays free. Taken a
        // moment later, since a click that gives the focus arrives just after it: that click then
        // takes the mouse above, as any click does, instead of reaching the game as a button.
        if (event.kind == PS_HOST_WINDOW_EVENT_FOCUS_GAINED && m_is_released_for_focus) {
            m_is_released_for_focus = false;
            m_focus_recapture_ms = ps_host_window_ticks_ms() + 250;
        }
        switch (event.kind) {
        case PS_HOST_WINDOW_EVENT_PIXEL_SIZE_CHANGED:
        case PS_HOST_WINDOW_EVENT_RESIZED: {
            int w = 0, h = 0;
            ps_host_window_size_in_pixels(m_p_window, &w, &h);
            m_width = w;
            m_height = h;
            break;
        }
        case PS_HOST_WINDOW_EVENT_DISPLAY_CHANGED:
            TellMonitor(m_p_window);
            break;
        case PS_HOST_WINDOW_EVENT_KEYBOARD_ADDED:
        case PS_HOST_WINDOW_EVENT_KEYBOARD_REMOVED:
            m_is_host_keyboard = ps_host_window_has_keyboard();
            break;
        case PS_HOST_WINDOW_EVENT_QUIT:
        case PS_HOST_WINDOW_EVENT_CLOSE_REQUESTED:
            // SIGTERM and SIGINT become a quit event too, so a quit is not always the user's.
            if (m_is_open) {
                std::printf(event.kind == PS_HOST_WINDOW_EVENT_QUIT
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
    if (m_focus_recapture_ms && ps_host_window_ticks_ms() >= m_focus_recapture_ms) {
        m_focus_recapture_ms = 0;
        if (m_is_mouse_look.load(std::memory_order_relaxed) && !PsOverlay::MenuOpen() && !m_is_text_active &&
            !PsOverlay::GameCursorShown() && !is_pointer_wanted &&
            ps_host_window_has_input_focus(m_p_window)) {
            CaptureMouse(true);
        }
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
            ps_host_window_has_input_focus(m_p_window)) {
            CaptureMouse(true);
        }
    }
    m_is_pointer_shown = is_pointer_wanted;
    UpdateCursor();
    return m_is_open;
}

void WindowSDL::TrackPadSticks(const PsHostWindowEvent& event) {
    if (event.kind == PS_HOST_WINDOW_EVENT_PAD_REMOVED) {
        for (PadSticks& pad : m_pad_sticks) {
            if (pad.h_pad == event.pad_button.h_pad) {
                pad = {};
            }
        }
        return;
    }
    if (event.kind != PS_HOST_WINDOW_EVENT_PAD_AXIS || event.pad_axis.axis < 0 ||
        event.pad_axis.axis >= PS_HOST_PAD_AXIS_LEFT_TRIGGER) {
        return;
    }
    PadSticks* p_free = nullptr;
    for (PadSticks& pad : m_pad_sticks) {
        if (pad.h_pad == event.pad_axis.h_pad) {
            pad.axes[event.pad_axis.axis] = event.pad_axis.value;
            return;
        }
        if (!p_free && pad.h_pad == 0) {
            p_free = &pad;
        }
    }
    if (p_free) {
        p_free->h_pad = event.pad_axis.h_pad;
        p_free->axes[event.pad_axis.axis] = event.pad_axis.value;
    }
}

// Issue #1185: Steam's Desktop Layout (what Steam applies to a controller it manages for a program
// it did not start) and reWASD turn a controller's stick into mouse motion besides the controller's
// input, so the stick turned the camera and the motion made from it turned it again. That motion is
// told by the stick, not by the device, since under XWayland every pointer reaches the game as one
// mouse. About a third of the stick's travel: far above a resting stick's drift, so a mouse player
// with a controller lying beside them keeps the mouse.
bool WindowSDL::IsPadMouseMotion(const PsHostWindowEvent& event) const {
    constexpr int stick_turning = 12000;
    if (event.kind != PS_HOST_WINDOW_EVENT_MOUSE_MOTION || !m_is_pad_mouse_filtered.load(std::memory_order_relaxed)) {
        return false;
    }
    for (const PadSticks& pad : m_pad_sticks) {
        for (const s16 value : pad.axes) {
            if (pad.h_pad != 0 && std::abs(int(value)) > stick_turning) {
                return true;
            }
        }
    }
    return false;
}

// Issue #3: the OS cursor over the game. Hidden in fullscreen, while the game draws its own
// (VideoOut's hardware cursor), and in a window after 3 s without moving the mouse; always shown
// while the settings menu or a game's pointer menu is open.
void WindowSDL::UpdateCursor() {
    const bool is_fullscreen = ps_host_window_is_fullscreen(m_p_window);
    const bool should_hide = !PsOverlay::MenuOpen() && !m_is_pointer_shown && (is_fullscreen || PsOverlay::GameCursorShown() ||
                                                 ps_host_window_ticks_ms() - m_last_mouse_motion_ms > 3000);
    if (should_hide != m_is_cursor_hidden) {
        m_is_cursor_hidden = should_hide;
        ps_host_window_show_cursor(!should_hide);
    }
}

} // namespace Frontend
