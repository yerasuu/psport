// psport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp). The window itself is the platform
// layer's (src/platform/ps_host_window.h); WindowSDL keeps the name of the shadPS4 class it stands
// in for, so that the renderer takes it unchanged.
#pragma once
#include <atomic>
#include <bitset>
#include <deque>
#include <mutex>
#include <string>
#include "../psport_gpu.h"
#include "../../src/platform/ps_host_window.h"
#include "common/types.h"

namespace PsOverlay {
struct KeyPress;
}

namespace Frontend {

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* sz_title, const char* sz_icon_path);
    ~WindowSDL();
    s32 GetWidth() const { return m_width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return m_height.load(std::memory_order_relaxed); }
    /// False when the host's windowing, the window or its window system failed; the reason is in the log.
    bool IsCreated() const { return m_p_window != nullptr; }
    /// What the renderer's surface is made from (ps_host_window_vulkan.h).
    const PsHostWindowSurface* GetSurface() const { return m_p_surface; }
    bool IsOpen() const { return m_is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Text entry for the system IME dialog, typed on the keyboard or on the overlay's keyboard with
    /// a controller (psport_keyboard.h); typed text shows in the title bar. The game's `initial` text
    /// is kept as given; only what the player types is held to `charset` and `max_length`.
    void BeginTextInput(const std::string& initial, const std::string& prompt, u32 max_length, int charset);
    /// The game rewrote the text of the open entry: the keyboard keeps its cursor, shift and page.
    void ReplaceTextInput(const std::string& text);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);
    /// Ends an entry the game took back (libSceIme closed) as cancelled, without waiting for a key.
    void EndTextInput();
    /// Mouse look (psport_gpu_mouse_take): motion and wheel notches since the last call, reset.
    void TakeMouse(double& dx, double& dy, int& wheel_up, int& wheel_down);
    /// The window holds the mouse (relative mode) for looking around.
    bool MouseCaptured() const { return m_is_mouse_captured.load(std::memory_order_relaxed); }
    /// mouse_look=0: a click does not take the mouse.
    static void EnableMouseLook(bool is_enabled) { m_is_mouse_look.store(is_enabled, std::memory_order_relaxed); }
    /// From any thread: the window takes the mouse at its next poll (psport_gpu_mouse_capture).
    void RequestMouseCapture() { m_is_capture_requested.store(true, std::memory_order_relaxed); }
    /// libSceMouse (psport_gpu_game_mouse_take): the buttons held (PS_HOST_INPUT_MOUSE_MASK bits), and the motion,
    /// wheel and tilt since the last call, reset. False while the host has no mouse.
    bool TakeGameMouse(s32& dx, s32& dy, s32& wheel, s32& tilt, u32& buttons);
    /// libSceImeKeyboard (psport_gpu_game_keyboard_*): while open, the keys the port does not keep are
    /// queued for the game; take returns up to max of them. keyboard: the host has one.
    static void OpenGameKeyboard(bool is_open) { m_is_game_keyboard_open.store(is_open, std::memory_order_relaxed); }
    int TakeGameKeys(PsKeyEvent* p_events, int max, bool& keyboard, u32& dropped);

private:
    std::atomic<s32> m_width, m_height;
    std::atomic<bool> m_is_open{true};
    std::mutex m_text_mutex;
    bool m_is_text_requested{}, m_is_text_active{}, m_is_text_end_requested{};
    int m_text_state{};
    std::string m_text, m_text_prompt, m_base_title;
    void UpdateTextTitle();
    void PressKeyboardKey(const PsOverlay::KeyPress& press);
    void UpdateCursor();
    u64 m_last_mouse_motion_ms{}; ///< ps_host_window_ticks_ms of the last mouse motion (UpdateCursor)
    bool m_is_cursor_hidden{};
    u64 m_capture_settle_ms{};       ///< mouse motion before this (ps_host_window_ticks_ms) is not looking around
    bool m_is_pointer_shown{};       ///< a game's pointer menu is open (Game::MouseHooks pfn_wants_pointer)
    bool m_is_released_for_pointer{}; ///< the mouse was let go for it: taken back after
    bool m_is_released_for_focus{};   ///< held when the window lost focus: taken back when it returns
    u64 m_focus_recapture_ms{};       ///< ps_host_window_ticks_ms when it is taken back, unless a click takes it first
    void CaptureMouse(bool should_capture);
    void GameMouseEvent(const PsHostWindowEvent& event);
    std::mutex m_mouse_mutex;
    double m_mouse_dx{}, m_mouse_dy{};
    int m_c_wheel_up{}, m_c_wheel_down{};
    std::atomic<bool> m_is_mouse_captured{false};
    std::atomic<bool> m_is_capture_requested{false};
    static inline std::atomic<bool> m_is_mouse_look{true};
    double m_game_dx{}, m_game_dy{};
    float m_game_wheel{}, m_game_tilt{};
    u32 m_game_buttons{};
    std::atomic<bool> m_is_host_mouse{false};
    void GameKeyEvent(const PsHostWindowEvent& event);
    void SyncGameKeyboard();
    std::mutex m_key_mutex;
    std::deque<PsKeyEvent> m_game_keys;
    std::bitset<512> m_game_keys_held; ///< by scancode: presses the game saw, so it gets their release
    u32 m_game_keys_dropped{};
    bool m_is_game_keyboard_synced{};
    std::atomic<bool> m_is_host_keyboard{false};
    static inline std::atomic<bool> m_is_game_keyboard_open{false};
    PsHostWindow* m_p_window{};
    const PsHostWindowSurface* m_p_surface{};
};

} // namespace Frontend
