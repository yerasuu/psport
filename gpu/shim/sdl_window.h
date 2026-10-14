// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <bitset>
#include <deque>
#include <mutex>
#include <string>
#include "../bbgpu.h"
#include "common/types.h"

struct SDL_Window;
union SDL_Event;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title, const char* icon_path);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);
    /// Ends an entry the game took back (libSceIme closed) as cancelled, without waiting for a key.
    void EndTextInput();
    /// Mouse look (bbgpu_mouse_take): motion and wheel notches since the last call, reset.
    void TakeMouse(double& dx, double& dy, int& wheel_up, int& wheel_down);
    /// The window holds the mouse (relative mode) for looking around.
    bool MouseCaptured() const { return mouse_captured.load(std::memory_order_relaxed); }
    /// mouse_look=0: a click does not take the mouse.
    static void EnableMouseLook(bool enabled) { mouse_look.store(enabled, std::memory_order_relaxed); }
    /// libSceMouse (bbgpu_game_mouse_take): the buttons held (SDL_BUTTON_MASK bits), and the motion,
    /// wheel and tilt since the last call, reset. False while the host has no mouse.
    bool TakeGameMouse(s32& dx, s32& dy, s32& wheel, s32& tilt, u32& buttons);
    /// libSceImeKeyboard (bbgpu_game_keyboard_*): while open, the keys the port does not keep are
    /// queued for the game; take returns up to max of them. keyboard: the host has one.
    static void OpenGameKeyboard(bool open) { game_keyboard_open.store(open, std::memory_order_relaxed); }
    int TakeGameKeys(BbKeyEvent* events, int max, bool& keyboard, u32& dropped);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{}, m_is_text_end_requested{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    void UpdateCursor();
    u64 last_mouse_motion_ms{}; ///< SDL_GetTicks of the last mouse motion (UpdateCursor)
    bool cursor_hidden{};
    void CaptureMouse(bool capture);
    void GameMouseEvent(const SDL_Event& event);
    std::mutex mouse_mutex;
    double mouse_dx{}, mouse_dy{};
    int wheel_up{}, wheel_down{};
    std::atomic<bool> mouse_captured{false};
    static inline std::atomic<bool> mouse_look{true};
    double game_dx{}, game_dy{};
    float game_wheel{}, game_tilt{};
    u32 game_buttons{};
    std::atomic<bool> host_mouse{false};
    void GameKeyEvent(const SDL_Event& event);
    void SyncGameKeyboard();
    std::mutex key_mutex;
    std::deque<BbKeyEvent> game_keys;
    std::bitset<512> game_keys_held; ///< by scancode: presses the game saw, so it gets their release
    u32 game_keys_dropped{};
    bool game_keyboard_synced{};
    std::atomic<bool> host_keyboard{false};
    static inline std::atomic<bool> game_keyboard_open{false};
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
