// SPDX-License-Identifier: GPL-2.0-or-later
/* ps_host_input.h on SDL3, the same on Linux and Windows. The contract's codes are SDL's own values
 * (checked below), so keys, buttons and types pass through unchanged. */
#include "ps_host_input.h"
#include <errno.h>
#include <SDL3/SDL.h>

#define SAME(ours, sdl) ((long)(ours) == (long)(sdl))
_Static_assert(SAME(PS_HOST_INPUT_KEY_COUNT, SDL_SCANCODE_COUNT), "key count");
_Static_assert(SAME(PS_HOST_INPUT_KEY_A, SDL_SCANCODE_A) && SAME(PS_HOST_INPUT_KEY_C, SDL_SCANCODE_C) &&
                   SAME(PS_HOST_INPUT_KEY_D, SDL_SCANCODE_D) && SAME(PS_HOST_INPUT_KEY_E, SDL_SCANCODE_E) &&
                   SAME(PS_HOST_INPUT_KEY_F, SDL_SCANCODE_F) && SAME(PS_HOST_INPUT_KEY_G, SDL_SCANCODE_G) &&
                   SAME(PS_HOST_INPUT_KEY_H, SDL_SCANCODE_H) && SAME(PS_HOST_INPUT_KEY_I, SDL_SCANCODE_I) &&
                   SAME(PS_HOST_INPUT_KEY_J, SDL_SCANCODE_J) && SAME(PS_HOST_INPUT_KEY_K, SDL_SCANCODE_K) &&
                   SAME(PS_HOST_INPUT_KEY_L, SDL_SCANCODE_L) && SAME(PS_HOST_INPUT_KEY_Q, SDL_SCANCODE_Q) &&
                   SAME(PS_HOST_INPUT_KEY_R, SDL_SCANCODE_R) && SAME(PS_HOST_INPUT_KEY_S, SDL_SCANCODE_S) &&
                   SAME(PS_HOST_INPUT_KEY_T, SDL_SCANCODE_T) && SAME(PS_HOST_INPUT_KEY_W, SDL_SCANCODE_W) &&
                   SAME(PS_HOST_INPUT_KEY_Z, SDL_SCANCODE_Z) && SAME(PS_HOST_INPUT_KEY_1, SDL_SCANCODE_1) &&
                   SAME(PS_HOST_INPUT_KEY_3, SDL_SCANCODE_3),
               "letter and digit keys");
_Static_assert(SAME(PS_HOST_INPUT_KEY_RETURN, SDL_SCANCODE_RETURN) &&
                   SAME(PS_HOST_INPUT_KEY_ESCAPE, SDL_SCANCODE_ESCAPE) &&
                   SAME(PS_HOST_INPUT_KEY_BACKSPACE, SDL_SCANCODE_BACKSPACE) &&
                   SAME(PS_HOST_INPUT_KEY_TAB, SDL_SCANCODE_TAB) && SAME(PS_HOST_INPUT_KEY_SPACE, SDL_SCANCODE_SPACE) &&
                   SAME(PS_HOST_INPUT_KEY_F9, SDL_SCANCODE_F9) && SAME(PS_HOST_INPUT_KEY_RIGHT, SDL_SCANCODE_RIGHT) &&
                   SAME(PS_HOST_INPUT_KEY_LEFT, SDL_SCANCODE_LEFT) && SAME(PS_HOST_INPUT_KEY_DOWN, SDL_SCANCODE_DOWN) &&
                   SAME(PS_HOST_INPUT_KEY_UP, SDL_SCANCODE_UP) && SAME(PS_HOST_INPUT_KEY_LSHIFT, SDL_SCANCODE_LSHIFT) &&
                   SAME(PS_HOST_INPUT_KEY_RSHIFT, SDL_SCANCODE_RSHIFT),
               "other keys");
_Static_assert(SAME(PS_HOST_INPUT_MOD_LSHIFT, SDL_KMOD_LSHIFT) && SAME(PS_HOST_INPUT_MOD_RSHIFT, SDL_KMOD_RSHIFT) &&
                   SAME(PS_HOST_INPUT_MOD_LCTRL, SDL_KMOD_LCTRL) && SAME(PS_HOST_INPUT_MOD_RCTRL, SDL_KMOD_RCTRL) &&
                   SAME(PS_HOST_INPUT_MOD_LALT, SDL_KMOD_LALT) && SAME(PS_HOST_INPUT_MOD_RALT, SDL_KMOD_RALT) &&
                   SAME(PS_HOST_INPUT_MOD_LGUI, SDL_KMOD_LGUI) && SAME(PS_HOST_INPUT_MOD_RGUI, SDL_KMOD_RGUI) &&
                   SAME(PS_HOST_INPUT_MOD_NUM, SDL_KMOD_NUM) && SAME(PS_HOST_INPUT_MOD_CAPS, SDL_KMOD_CAPS) &&
                   SAME(PS_HOST_INPUT_MOD_SCROLL, SDL_KMOD_SCROLL),
               "modifiers");
_Static_assert(SAME(PS_HOST_INPUT_MOUSE_LEFT, SDL_BUTTON_LEFT) && SAME(PS_HOST_INPUT_MOUSE_MIDDLE, SDL_BUTTON_MIDDLE) &&
                   SAME(PS_HOST_INPUT_MOUSE_RIGHT, SDL_BUTTON_RIGHT) && SAME(PS_HOST_INPUT_MOUSE_X1, SDL_BUTTON_X1) &&
                   SAME(PS_HOST_INPUT_MOUSE_X2, SDL_BUTTON_X2) &&
                   SAME(PS_HOST_INPUT_MOUSE_MASK(PS_HOST_INPUT_MOUSE_X2), SDL_BUTTON_X2MASK),
               "mouse buttons");
_Static_assert(SAME(PS_HOST_PAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID) &&
                   SAME(PS_HOST_PAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_SOUTH) &&
                   SAME(PS_HOST_PAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_BACK) &&
                   SAME(PS_HOST_PAD_BUTTON_START, SDL_GAMEPAD_BUTTON_START) &&
                   SAME(PS_HOST_PAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
                   SAME(PS_HOST_PAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) &&
                   SAME(PS_HOST_PAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_MISC1) &&
                   SAME(PS_HOST_PAD_BUTTON_LEFT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2) &&
                   SAME(PS_HOST_PAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_TOUCHPAD) &&
                   SAME(PS_HOST_PAD_BUTTON_MISC6, SDL_GAMEPAD_BUTTON_MISC6) &&
                   SAME(PS_HOST_PAD_BUTTON_COUNT, SDL_GAMEPAD_BUTTON_COUNT),
               "gamepad buttons");
_Static_assert(SAME(PS_HOST_PAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTX) &&
                   SAME(PS_HOST_PAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_RIGHTY) &&
                   SAME(PS_HOST_PAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) &&
                   SAME(PS_HOST_PAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
               "gamepad axes");
_Static_assert(SAME(PS_HOST_PAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN) &&
                   SAME(PS_HOST_PAD_TYPE_PS3, SDL_GAMEPAD_TYPE_PS3) &&
                   SAME(PS_HOST_PAD_TYPE_PS4, SDL_GAMEPAD_TYPE_PS4) &&
                   SAME(PS_HOST_PAD_TYPE_PS5, SDL_GAMEPAD_TYPE_PS5) &&
                   SAME(PS_HOST_PAD_TYPE_SWITCH_PRO, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) &&
                   SAME(PS_HOST_PAD_TYPE_JOYCON_LEFT, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT) &&
                   SAME(PS_HOST_PAD_TYPE_JOYCON_RIGHT, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT) &&
                   SAME(PS_HOST_PAD_TYPE_JOYCON_PAIR, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR),
               "gamepad types");
_Static_assert(SAME(PS_HOST_PAD_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN) &&
                   SAME(PS_HOST_PAD_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_A) &&
                   SAME(PS_HOST_PAD_LABEL_Y, SDL_GAMEPAD_BUTTON_LABEL_Y) &&
                   SAME(PS_HOST_PAD_LABEL_TRIANGLE, SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE),
               "gamepad button labels");
_Static_assert(sizeof(PsHostPadId) == sizeof(SDL_JoystickID), "gamepad id");
#undef SAME

static SDL_Gamepad *sdl_pad(PsHostPad *p_pad) { return (SDL_Gamepad *)p_pad; }
static SDL_SensorType sdl_sensor(PsHostPadSensor sensor) {
    return sensor == PS_HOST_PAD_SENSOR_GYRO ? SDL_SENSOR_GYRO : SDL_SENSOR_ACCEL;
}

int ps_host_input_is_window_ready(void) { return SDL_WasInit(SDL_INIT_VIDEO) != 0; }
const bool *ps_host_input_keyboard_state(void) { return SDL_GetKeyboardState(NULL); }
int ps_host_input_key_from_name(const char *sz_name) {
    const SDL_Scancode key = SDL_GetScancodeFromName(sz_name);
    return key == SDL_SCANCODE_UNKNOWN ? -1 : (int)key;
}
const char *ps_host_input_key_name(int key) { return SDL_GetScancodeName((SDL_Scancode)key); }
uint32_t ps_host_input_mouse_buttons(void) { return SDL_GetMouseState(NULL, NULL); }

int ps_host_input_pad_start(void) {
    return SDL_WasInit(SDL_INIT_GAMEPAD) || SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 0 : EIO;
}
PsHostPadId *ps_host_input_pad_list(int *p_out_count) { return SDL_GetGamepads(p_out_count); }
void ps_host_input_free(void *p_memory) { SDL_free(p_memory); }
void ps_host_input_pad_guid(PsHostPadId h_pad, char *sz_out, size_t cb_out) {
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(h_pad), sz_out, (int)cb_out);
}
const char *ps_host_input_pad_name_for_id(PsHostPadId h_pad) { return SDL_GetGamepadNameForID(h_pad); }
PsHostPad *ps_host_input_pad_open(PsHostPadId h_pad) { return (PsHostPad *)SDL_OpenGamepad(h_pad); }
void ps_host_input_pad_close(PsHostPad *p_pad) { SDL_CloseGamepad(sdl_pad(p_pad)); }
int ps_host_input_pad_is_connected(PsHostPad *p_pad) { return SDL_GamepadConnected(sdl_pad(p_pad)); }
PsHostPadId ps_host_input_pad_id(PsHostPad *p_pad) { return SDL_GetGamepadID(sdl_pad(p_pad)); }
const char *ps_host_input_pad_name(PsHostPad *p_pad) { return SDL_GetGamepadName(sdl_pad(p_pad)); }
int ps_host_input_pad_type(PsHostPad *p_pad) { return (int)SDL_GetGamepadType(sdl_pad(p_pad)); }
int ps_host_input_pad_button(PsHostPad *p_pad, PsHostPadButton button) {
    return SDL_GetGamepadButton(sdl_pad(p_pad), (SDL_GamepadButton)button);
}
int16_t ps_host_input_pad_axis(PsHostPad *p_pad, PsHostPadAxis axis) {
    return SDL_GetGamepadAxis(sdl_pad(p_pad), (SDL_GamepadAxis)axis);
}
int ps_host_input_pad_touchpads(PsHostPad *p_pad) { return SDL_GetNumGamepadTouchpads(sdl_pad(p_pad)); }
int ps_host_input_pad_touchpad_fingers(PsHostPad *p_pad, int i_touchpad) {
    return SDL_GetNumGamepadTouchpadFingers(sdl_pad(p_pad), i_touchpad);
}
int ps_host_input_pad_touchpad_finger(PsHostPad *p_pad, int i_touchpad, int i_finger, bool *p_out_is_down,
                                      float *p_out_x, float *p_out_y) {
    return SDL_GetGamepadTouchpadFinger(sdl_pad(p_pad), i_touchpad, i_finger, p_out_is_down, p_out_x, p_out_y, NULL)
               ? 0
               : EIO;
}
int ps_host_input_pad_rumble(PsHostPad *p_pad, uint16_t low, uint16_t high, uint32_t duration_ms) {
    return SDL_RumbleGamepad(sdl_pad(p_pad), low, high, duration_ms) ? 0 : EIO;
}
int ps_host_input_pad_set_light_bar(PsHostPad *p_pad, uint8_t red, uint8_t green, uint8_t blue) {
    return SDL_SetGamepadLED(sdl_pad(p_pad), red, green, blue) ? 0 : EIO;
}
int ps_host_input_pad_has_sensor(PsHostPad *p_pad, PsHostPadSensor sensor) {
    return SDL_GamepadHasSensor(sdl_pad(p_pad), sdl_sensor(sensor));
}
int ps_host_input_pad_enable_sensor(PsHostPad *p_pad, PsHostPadSensor sensor) {
    return SDL_SetGamepadSensorEnabled(sdl_pad(p_pad), sdl_sensor(sensor), true) ? 0 : EIO;
}
int ps_host_input_pad_read_sensor(PsHostPad *p_pad, PsHostPadSensor sensor, float *p_out) {
    return SDL_GetGamepadSensorData(sdl_pad(p_pad), sdl_sensor(sensor), p_out, 3) ? 0 : EIO;
}
PsHostPadButton ps_host_input_pad_button_from_name(const char *sz_name) {
    return (PsHostPadButton)SDL_GetGamepadButtonFromString(sz_name);
}
const char *ps_host_input_pad_button_name(PsHostPadButton button) {
    return SDL_GetGamepadStringForButton((SDL_GamepadButton)button);
}
PsHostPadLabel ps_host_input_pad_button_label(int type, PsHostPadButton button) {
    return (PsHostPadLabel)SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, (SDL_GamepadButton)button);
}

static PsHostInputEventFn s_pfn_on_event;
static bool SDLCALL watch_event(void *p_user, SDL_Event *p_event) {
    (void)p_user;
    PsHostInputEvent event = {.kind = PS_HOST_INPUT_EVENT_OTHER};
    if (p_event->type == SDL_EVENT_KEY_DOWN) {
        event.kind = PS_HOST_INPUT_EVENT_KEY_DOWN;
        event.code = (int)p_event->key.scancode;
        event.is_repeat = p_event->key.repeat;
    } else if (p_event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        event.kind = PS_HOST_INPUT_EVENT_MOUSE_BUTTON_DOWN;
        event.code = p_event->button.button;
    } else if (p_event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        event.kind = PS_HOST_INPUT_EVENT_PAD_BUTTON_DOWN;
        event.h_pad = p_event->gbutton.which;
        event.code = p_event->gbutton.button;
    } else if (p_event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        event.kind = PS_HOST_INPUT_EVENT_PAD_AXIS;
        event.h_pad = p_event->gaxis.which;
        event.code = p_event->gaxis.axis;
        event.value = p_event->gaxis.value;
    }
    __atomic_load_n(&s_pfn_on_event, __ATOMIC_ACQUIRE)(&event);
    return true;
}
int ps_host_input_watch(PsHostInputEventFn pfn_on_event) {
    if (__atomic_exchange_n(&s_pfn_on_event, pfn_on_event, __ATOMIC_ACQ_REL)) return 0;
    if (SDL_AddEventWatch(watch_event, NULL)) return 0;
    __atomic_store_n(&s_pfn_on_event, NULL, __ATOMIC_RELEASE);
    return EIO;
}
const char *ps_host_input_error(void) { return SDL_GetError(); }
