// psport: the game window's input for the C runtime: overlay, mouse, keyboard and text input.
#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include "../psport_gpu.h"
#include "core/libraries/avplayer/avplayer_state.h"
#include "game_profile.h"
#include "psport_overlay.h"
#include "sdl_window.h"

extern Frontend::WindowSDL* g_window;

extern "C" int psport_gpu_overlay_captures_input(void) {
    return PsOverlay::CapturesInput() ? 1 : 0;
}

extern "C" void psport_gpu_overlay_banner(const char* sz_utf8) {
    PsOverlay::SetBanner(sz_utf8);
}

extern "C" int psport_gpu_overlay_pad_file_press(int guide, int cross, int circle) {
    return PsOverlay::PadFilePress(guide != 0, cross != 0, circle != 0) ? 1 : 0;
}

extern "C" void psport_gpu_mouse_take(double* p_dx, double* p_dy, int* p_wheel_up, int* p_wheel_down) {
    double x = 0, y = 0;
    int is_up = 0, is_down = 0;
    if (g_window) {
        g_window->TakeMouse(x, y, is_up, is_down);
    }
    *p_dx = x;
    *p_dy = y;
    *p_wheel_up = is_up;
    *p_wheel_down = is_down;
}

extern "C" int psport_gpu_mouse_captured(void) {
    return g_window && g_window->MouseCaptured() ? 1 : 0;
}

extern "C" int psport_gpu_game_mouse_take(int32_t* p_dx, int32_t* p_dy, int32_t* p_wheel, int32_t* p_tilt, uint32_t* p_buttons) {
    *p_dx = *p_dy = *p_wheel = *p_tilt = 0;
    *p_buttons = 0;
    return g_window && g_window->TakeGameMouse(*p_dx, *p_dy, *p_wheel, *p_tilt, *p_buttons) ? 1 : 0;
}

extern "C" void psport_gpu_game_keyboard_open(int is_open) {
    Frontend::WindowSDL::OpenGameKeyboard(is_open != 0);
}

extern "C" int psport_gpu_game_keyboard_take(PsKeyEvent* p_events, int max, int* p_keyboard, uint32_t* p_dropped) {
    bool is_host = false;
    u32 lost = 0;
    const int c_keys = g_window ? g_window->TakeGameKeys(p_events, max, is_host, lost) : 0;
    *p_keyboard = is_host ? 1 : 0;
    *p_dropped = lost;
    return c_keys;
}

extern "C" void psport_gpu_mouse_capture(void) {
    if (g_window) {
        g_window->RequestMouseCapture();
    }
}

extern "C" void psport_gpu_mouse_look_enable(int is_enabled) {
    Frontend::WindowSDL::EnableMouseLook(is_enabled != 0);
}

extern "C" void psport_gpu_mouse_pad_filter(int is_on) {
    Frontend::WindowSDL::FilterPadMouse(is_on != 0);
}

extern "C" int psport_gpu_camera_look(float pitch, float yaw) {
    const Game::MouseHooks mouse = Game::Mouse();
    return mouse.pfn_camera_look && mouse.pfn_camera_look(pitch, yaw) ? 1 : 0;
}

extern "C" void psport_gpu_filter_pad(uint32_t* p_buttons, const uint8_t* p_sticks) {
    if (const Game::MouseHooks mouse = Game::Mouse(); mouse.pfn_filter_pad) {
        mouse.pfn_filter_pad(p_buttons, p_sticks);
    }
}

extern "C" int psport_gpu_skip_movies(void) {
    return static_cast<int>(Libraries::AvPlayer::RequestSkipAll());
}

extern "C" int psport_gpu_skip_scene(int has_stopped_movie) {
    const Game::Profile* p_profile = Game::Active();
    return p_profile && p_profile->pfn_skip_scene && p_profile->pfn_skip_scene(has_stopped_movie != 0) ? 1 : 0;
}

extern "C" int psport_gpu_text_input_begin(const char* sz_initial, const char* sz_prompt, uint32_t max_length,
                                           int charset) {
    if (!g_window) return 0;
    g_window->BeginTextInput(sz_initial ? sz_initial : "", sz_prompt ? sz_prompt : "Text", max_length, charset);
    return 1;
}

extern "C" void psport_gpu_text_input_replace(const char* sz_text) {
    if (g_window) g_window->ReplaceTextInput(sz_text ? sz_text : "");
}

extern "C" void psport_gpu_text_input_end(void) {
    if (g_window) g_window->EndTextInput();
}

extern "C" int psport_gpu_message_begin(const char* sz_text, int system_id, const PsMessageButton* p_buttons,
                                        int c_buttons, int i_focus, int can_cancel) {
    if (!g_window) return 0;
    return PsOverlay::OpenMessage(sz_text ? sz_text : "", sz_text ? -1 : system_id,
                                  std::span<const PsMessageButton>(p_buttons, std::size_t(std::max(c_buttons, 0))),
                                  i_focus, can_cancel != 0)
               ? 1
               : 0;
}

extern "C" int psport_gpu_message_poll(void) {
    return PsOverlay::PollMessage();
}

extern "C" void psport_gpu_message_end(void) {
    PsOverlay::CloseMessage();
}

extern "C" int psport_gpu_text_input_poll(char* sz_out, uint64_t cb_out) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (cb_out) {
        const size_t cb_copy = std::min<size_t>(text.size(), cb_out - 1);
        std::memcpy(sz_out, text.data(), cb_copy);
        sz_out[cb_copy] = 0;
    }
    return state;
}
