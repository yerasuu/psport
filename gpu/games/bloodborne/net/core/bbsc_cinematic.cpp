// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// Input side of the BBSC cinematic camera. While it is on, the keyboard and mouse fly the camera
// and never reach the game (shadPS4 would otherwise map them to the pad and move the hunter too);
// the controller is untouched. Keys:
//   F1 on/off | hold right mouse: look | WASD move | E / Space up, Q / C down
//   Shift fast, Alt slow | wheel: speed | Z / X zoom, V reset zoom | , . roll, / reset roll
//   arrow keys: look | [ ] smoothing (glide) less / more

#include <mutex>

#include "common/logging/log.h"
#include "core/bbsc_cinematic.h"
#include "core/bbsc_persistence.h"

namespace Core::Bbsc {
namespace {

std::mutex cine_mutex;
CinematicInput cine_state;
bool looking = false;

bool CinematicEnabledBySwitch() {
    static const bool enabled = SwitchEnabled("SHADPS4_BBSC_CINEMATIC");
    return enabled;
}

void SetKey(u32 key, bool down) {
    switch (key) {
    case PS_HOST_WINDOW_KEYCODE_W: cine_state.forward = down; break;
    case PS_HOST_WINDOW_KEYCODE_S: cine_state.back = down; break;
    case PS_HOST_WINDOW_KEYCODE_A: cine_state.left = down; break;
    case PS_HOST_WINDOW_KEYCODE_D: cine_state.right = down; break;
    case PS_HOST_WINDOW_KEYCODE_E:
    case PS_HOST_WINDOW_KEYCODE_SPACE: cine_state.up = down; break;
    case PS_HOST_WINDOW_KEYCODE_Q:
    case PS_HOST_WINDOW_KEYCODE_C: cine_state.down = down; break;
    case PS_HOST_WINDOW_KEYCODE_LSHIFT:
    case PS_HOST_WINDOW_KEYCODE_RSHIFT: cine_state.fast = down; break;
    case PS_HOST_WINDOW_KEYCODE_LALT:
    case PS_HOST_WINDOW_KEYCODE_RALT: cine_state.slow = down; break;
    case PS_HOST_WINDOW_KEYCODE_Z: cine_state.zoom_in = down; break;
    case PS_HOST_WINDOW_KEYCODE_X: cine_state.zoom_out = down; break;
    case PS_HOST_WINDOW_KEYCODE_V: cine_state.zoom_reset = cine_state.zoom_reset || down; break;
    case PS_HOST_WINDOW_KEYCODE_COMMA: cine_state.roll_left = down; break;
    case PS_HOST_WINDOW_KEYCODE_PERIOD: cine_state.roll_right = down; break;
    case PS_HOST_WINDOW_KEYCODE_SLASH: cine_state.roll_reset = cine_state.roll_reset || down; break;
    case PS_HOST_WINDOW_KEYCODE_UP: cine_state.look_up = down; break;
    case PS_HOST_WINDOW_KEYCODE_DOWN: cine_state.look_down = down; break;
    case PS_HOST_WINDOW_KEYCODE_LEFT: cine_state.look_left = down; break;
    case PS_HOST_WINDOW_KEYCODE_RIGHT: cine_state.look_right = down; break;
    case PS_HOST_WINDOW_KEYCODE_LEFTBRACKET:
        if (down) --cine_state.smooth_steps;
        break;
    case PS_HOST_WINDOW_KEYCODE_RIGHTBRACKET:
        if (down) ++cine_state.smooth_steps;
        break;
    default: break;
    }
}

void ReleaseAll(PsHostWindow* window) {
    const bool enabled = cine_state.enabled;
    cine_state = CinematicInput{};
    cine_state.enabled = enabled;
    if (looking && window != nullptr) {
        ps_host_window_set_relative_mouse(window, false);
    }
    looking = false;
}

} // namespace

bool CinematicHandleEvent(const PsHostWindowEvent& event, PsHostWindow* window) {
    if (!CinematicEnabledBySwitch()) {
        return false;
    }
    std::scoped_lock lock{cine_mutex};
    if (event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN && event.key.keycode == PS_HOST_WINDOW_KEYCODE_F1) {
        if (!event.key.is_repeat) {
            cine_state.enabled = !cine_state.enabled;
            ReleaseAll(window);
            LOG_INFO(Debug, "BBSC cinematic camera: {} (F1)", cine_state.enabled ? "on" : "off");
        }
        return true;
    }
    if (event.kind == PS_HOST_WINDOW_EVENT_KEY_UP && event.key.keycode == PS_HOST_WINDOW_KEYCODE_F1) {
        return true;
    }
    if (!cine_state.enabled) {
        return false;
    }
    switch (event.kind) {
    case PS_HOST_WINDOW_EVENT_KEY_DOWN:
    case PS_HOST_WINDOW_EVENT_KEY_UP: {
        const u32 key = event.key.keycode;
        if (key >= PS_HOST_WINDOW_KEYCODE_F2 && key <= PS_HOST_WINDOW_KEYCODE_F12) {
            return false; // the emulator's own hotkeys (fullscreen, fps, pause...) keep working
        }
        SetKey(key, event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN);
        return true;
    }
    case PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN:
    case PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == PS_HOST_INPUT_MOUSE_RIGHT) {
            looking = event.kind == PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN;
            if (window != nullptr) {
                ps_host_window_set_relative_mouse(window, looking);
            }
        }
        return true;
    case PS_HOST_WINDOW_EVENT_MOUSE_MOTION:
        if (looking) {
            cine_state.mouse_dx += event.motion.dx;
            cine_state.mouse_dy += event.motion.dy;
        }
        return true;
    case PS_HOST_WINDOW_EVENT_MOUSE_WHEEL:
        cine_state.wheel += event.wheel.y > 0.0f ? 1 : (event.wheel.y < 0.0f ? -1 : 0);
        return true;
    case PS_HOST_WINDOW_EVENT_FOCUS_LOST:
        ReleaseAll(window);
        return false;
    default:
        return false;
    }
}

CinematicInput TakeCinematicInput() {
    std::scoped_lock lock{cine_mutex};
    CinematicInput out = cine_state;
    cine_state.mouse_dx = 0.0f;
    cine_state.mouse_dy = 0.0f;
    cine_state.wheel = 0;
    cine_state.smooth_steps = 0;
    cine_state.zoom_reset = false;
    cine_state.roll_reset = false;
    return out;
}

void CinematicForceOff() {
    std::scoped_lock lock{cine_mutex};
    cine_state = CinematicInput{};
}

void CinematicForceOn() {
    std::scoped_lock lock{cine_mutex};
    if (!cine_state.enabled) {
        cine_state = CinematicInput{};
        cine_state.enabled = true;
    }
}

} // namespace Core::Bbsc
