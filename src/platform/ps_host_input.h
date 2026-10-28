// SPDX-License-Identifier: GPL-2.0-or-later
/* Input from the host (decision #1009, phase 1a): gamepads, the keyboard and the mouse as libScePad,
 * libSceImeKeyboard, libSceMouse and the port's bindings read them. The window thread pumps the host's
 * events (gpu/shim/window.cpp); these calls sample the state that pumping leaves, and a watch sees each
 * event as it arrives. Errors are the host's errno values, with the backend's reason for the last one
 * in ps_host_input_error; a backend that lacks a capability returns its error and says so in
 * host_features.inc (input_pad, input_pad_motion, input_keyboard, input_mouse), never another
 * behaviour in its place. One shared backend serves every OS that has it (src/platform/common/). */
#ifndef PS_HOST_INPUT_H
#define PS_HOST_INPUT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- keyboard ---- */
/* Keys are USB HID keyboard usage IDs (usage page 0x07), below PS_HOST_INPUT_KEY_COUNT; the ones the
 * port binds by default are named. */
enum {
    PS_HOST_INPUT_KEY_A = 4,
    PS_HOST_INPUT_KEY_C = 6,
    PS_HOST_INPUT_KEY_D = 7,
    PS_HOST_INPUT_KEY_E = 8,
    PS_HOST_INPUT_KEY_F = 9,
    PS_HOST_INPUT_KEY_G = 10,
    PS_HOST_INPUT_KEY_H = 11,
    PS_HOST_INPUT_KEY_I = 12,
    PS_HOST_INPUT_KEY_J = 13,
    PS_HOST_INPUT_KEY_K = 14,
    PS_HOST_INPUT_KEY_L = 15,
    PS_HOST_INPUT_KEY_Q = 20,
    PS_HOST_INPUT_KEY_R = 21,
    PS_HOST_INPUT_KEY_S = 22,
    PS_HOST_INPUT_KEY_T = 23,
    PS_HOST_INPUT_KEY_W = 26,
    PS_HOST_INPUT_KEY_Z = 29,
    PS_HOST_INPUT_KEY_1 = 30,
    PS_HOST_INPUT_KEY_3 = 32,
    PS_HOST_INPUT_KEY_RETURN = 40,
    PS_HOST_INPUT_KEY_ESCAPE = 41,
    PS_HOST_INPUT_KEY_BACKSPACE = 42,
    PS_HOST_INPUT_KEY_TAB = 43,
    PS_HOST_INPUT_KEY_SPACE = 44,
    PS_HOST_INPUT_KEY_F9 = 66,
    PS_HOST_INPUT_KEY_RIGHT = 79,
    PS_HOST_INPUT_KEY_LEFT = 80,
    PS_HOST_INPUT_KEY_DOWN = 81,
    PS_HOST_INPUT_KEY_UP = 82,
    PS_HOST_INPUT_KEY_LSHIFT = 225,
    PS_HOST_INPUT_KEY_RSHIFT = 229,
    PS_HOST_INPUT_KEY_COUNT = 512,
};
/* The modifier bits of a key event (PsKeyEvent.mod, gpu/psport_gpu.h). */
enum {
    PS_HOST_INPUT_MOD_LSHIFT = 0x0001,
    PS_HOST_INPUT_MOD_RSHIFT = 0x0002,
    PS_HOST_INPUT_MOD_LCTRL = 0x0040,
    PS_HOST_INPUT_MOD_RCTRL = 0x0080,
    PS_HOST_INPUT_MOD_LALT = 0x0100,
    PS_HOST_INPUT_MOD_RALT = 0x0200,
    PS_HOST_INPUT_MOD_LGUI = 0x0400,
    PS_HOST_INPUT_MOD_RGUI = 0x0800,
    PS_HOST_INPUT_MOD_NUM = 0x1000,
    PS_HOST_INPUT_MOD_CAPS = 0x2000,
    PS_HOST_INPUT_MOD_SCROLL = 0x8000,
};
/* Nonzero once the window thread has started the host's event system, before which the keyboard and
 * mouse cannot be read and presses cannot be watched. */
int ps_host_input_is_window_ready(void);
/* Whether each key is down, indexed by key (PS_HOST_INPUT_KEY_COUNT entries), as of the last pump. The
 * array belongs to the backend and stays valid for the process's life. */
const bool *ps_host_input_keyboard_state(void);
/* A key from its name in psport.ini's key.<input> lines ("Left Shift", "Keypad +"; letters as either
 * case): the key, or -1 when the name is unknown. */
int ps_host_input_key_from_name(const char *sz_name);
/* The name ps_host_input_key_from_name reads back to that key: "" for a key without one. */
const char *ps_host_input_key_name(int key);

/* ---- mouse ---- */
enum {
    PS_HOST_INPUT_MOUSE_LEFT = 1,
    PS_HOST_INPUT_MOUSE_MIDDLE = 2,
    PS_HOST_INPUT_MOUSE_RIGHT = 3,
    PS_HOST_INPUT_MOUSE_X1 = 4,
    PS_HOST_INPUT_MOUSE_X2 = 5,
};
#define PS_HOST_INPUT_MOUSE_MASK(button) (1u << ((button) - 1))
/* The mouse buttons held, as of the last pump: PS_HOST_INPUT_MOUSE_MASK bits. */
uint32_t ps_host_input_mouse_buttons(void);

/* ---- gamepads ---- */
typedef struct PsHostPad PsHostPad;
/* Names one connected gamepad for as long as it stays connected; 0 is none. */
typedef uint32_t PsHostPadId;
typedef enum {
    PS_HOST_PAD_BUTTON_INVALID = -1,
    PS_HOST_PAD_BUTTON_SOUTH,
    PS_HOST_PAD_BUTTON_EAST,
    PS_HOST_PAD_BUTTON_WEST,
    PS_HOST_PAD_BUTTON_NORTH,
    PS_HOST_PAD_BUTTON_BACK,
    PS_HOST_PAD_BUTTON_GUIDE,
    PS_HOST_PAD_BUTTON_START,
    PS_HOST_PAD_BUTTON_LEFT_STICK,
    PS_HOST_PAD_BUTTON_RIGHT_STICK,
    PS_HOST_PAD_BUTTON_LEFT_SHOULDER,
    PS_HOST_PAD_BUTTON_RIGHT_SHOULDER,
    PS_HOST_PAD_BUTTON_DPAD_UP,
    PS_HOST_PAD_BUTTON_DPAD_DOWN,
    PS_HOST_PAD_BUTTON_DPAD_LEFT,
    PS_HOST_PAD_BUTTON_DPAD_RIGHT,
    PS_HOST_PAD_BUTTON_MISC1,
    PS_HOST_PAD_BUTTON_RIGHT_PADDLE1,
    PS_HOST_PAD_BUTTON_LEFT_PADDLE1,
    PS_HOST_PAD_BUTTON_RIGHT_PADDLE2,
    PS_HOST_PAD_BUTTON_LEFT_PADDLE2,
    PS_HOST_PAD_BUTTON_TOUCHPAD,
    PS_HOST_PAD_BUTTON_MISC2,
    PS_HOST_PAD_BUTTON_MISC3,
    PS_HOST_PAD_BUTTON_MISC4,
    PS_HOST_PAD_BUTTON_MISC5,
    PS_HOST_PAD_BUTTON_MISC6,
    PS_HOST_PAD_BUTTON_COUNT
} PsHostPadButton;
/* Sticks from -32768 to 32767 (left and up negative), triggers from 0 to 32767. */
typedef enum {
    PS_HOST_PAD_AXIS_LEFTX,
    PS_HOST_PAD_AXIS_LEFTY,
    PS_HOST_PAD_AXIS_RIGHTX,
    PS_HOST_PAD_AXIS_RIGHTY,
    PS_HOST_PAD_AXIS_LEFT_TRIGGER,
    PS_HOST_PAD_AXIS_RIGHT_TRIGGER,
} PsHostPadAxis;
/* A gamepad's family, for button prompts; other values name families the port draws no prompt of
 * its own for, and are only handed back to ps_host_input_pad_button_label. */
enum {
    PS_HOST_PAD_TYPE_UNKNOWN = 0,
    PS_HOST_PAD_TYPE_PS3 = 4,
    PS_HOST_PAD_TYPE_PS4 = 5,
    PS_HOST_PAD_TYPE_PS5 = 6,
    PS_HOST_PAD_TYPE_SWITCH_PRO = 7,
    PS_HOST_PAD_TYPE_JOYCON_LEFT = 8,
    PS_HOST_PAD_TYPE_JOYCON_RIGHT = 9,
    PS_HOST_PAD_TYPE_JOYCON_PAIR = 10,
};
typedef enum {
    PS_HOST_PAD_LABEL_UNKNOWN,
    PS_HOST_PAD_LABEL_A,
    PS_HOST_PAD_LABEL_B,
    PS_HOST_PAD_LABEL_X,
    PS_HOST_PAD_LABEL_Y,
    PS_HOST_PAD_LABEL_CROSS,
    PS_HOST_PAD_LABEL_CIRCLE,
    PS_HOST_PAD_LABEL_SQUARE,
    PS_HOST_PAD_LABEL_TRIANGLE,
} PsHostPadLabel;
/* Both in one frame for every gamepad, the pad held in front of the player: +X right, +Y up out of
 * its face, +Z towards the player. The accelerometer in m/s^2 with gravity reading +Y at rest, the
 * gyroscope in rad/s counter-clockwise seen from the positive end of each axis. */
typedef enum {
    PS_HOST_PAD_SENSOR_ACCEL,
    PS_HOST_PAD_SENSOR_GYRO,
} PsHostPadSensor;

/* Starts the host's gamepad support, unless it already runs: 0, or EIO. */
int ps_host_input_pad_start(void);
/* The connected gamepads, *p_out_count of them: an array the caller frees with ps_host_input_free,
 * or NULL with the host's reason in ps_host_input_error. */
PsHostPadId *ps_host_input_pad_list(int *p_out_count);
void ps_host_input_free(void *p_memory);
/* A connected gamepad's GUID as 32 hex digits (cb_out at least 33), which a launcher's choice may name. */
void ps_host_input_pad_guid(PsHostPadId h_pad, char *sz_out, size_t cb_out);
/* NULL when the host has no name for it. */
const char *ps_host_input_pad_name_for_id(PsHostPadId h_pad);
/* NULL with the host's reason in ps_host_input_error. Every call other than close accepts a pad that
 * has disconnected meanwhile and fails or reads it at rest. */
PsHostPad *ps_host_input_pad_open(PsHostPadId h_pad);
void ps_host_input_pad_close(PsHostPad *p_pad);
int ps_host_input_pad_is_connected(PsHostPad *p_pad);
/* 0 when the pad is no longer valid. */
PsHostPadId ps_host_input_pad_id(PsHostPad *p_pad);
const char *ps_host_input_pad_name(PsHostPad *p_pad);
int ps_host_input_pad_type(PsHostPad *p_pad);
int ps_host_input_pad_button(PsHostPad *p_pad, PsHostPadButton button);
int16_t ps_host_input_pad_axis(PsHostPad *p_pad, PsHostPadAxis axis);
/* The pad's touch surfaces, how many fingers one can track, and one finger: 0 with whether it is down
 * and where (0 to 1 from the top left), or EIO. */
int ps_host_input_pad_touchpads(PsHostPad *p_pad);
int ps_host_input_pad_touchpad_fingers(PsHostPad *p_pad, int i_touchpad);
int ps_host_input_pad_touchpad_finger(PsHostPad *p_pad, int i_touchpad, int i_finger, bool *p_out_is_down,
                                      float *p_out_x, float *p_out_y);
/* Both motors for duration_ms, replacing the last request: 0, or EIO when the host refused it (a pad
 * without motors too; ps_host_input_error says why). The report may be a synchronous device write. */
int ps_host_input_pad_rumble(PsHostPad *p_pad, uint16_t low, uint16_t high, uint32_t duration_ms);
/* 0, or EIO when the host refused it (a pad without a light it can set too). */
int ps_host_input_pad_set_light_bar(PsHostPad *p_pad, uint8_t red, uint8_t green, uint8_t blue);
int ps_host_input_pad_has_sensor(PsHostPad *p_pad, PsHostPadSensor sensor);
/* Turns a sensor's reports on: 0, or EIO. */
int ps_host_input_pad_enable_sensor(PsHostPad *p_pad, PsHostPadSensor sensor);
/* The sensor's latest reading in p_out (three floats): 0, or EIO. */
int ps_host_input_pad_read_sensor(PsHostPad *p_pad, PsHostPadSensor sensor, float *p_out);
/* A button from its name in psport.ini's pad.<input> lines ("a", "leftshoulder", "paddle1"): the
 * button, or PS_HOST_PAD_BUTTON_INVALID. */
PsHostPadButton ps_host_input_pad_button_from_name(const char *sz_name);
/* The name ps_host_input_pad_button_from_name reads back to that button; NULL for no button. */
const char *ps_host_input_pad_button_name(PsHostPadButton button);
/* What a face button is labelled on a pad of that type (ps_host_input_pad_type). */
PsHostPadLabel ps_host_input_pad_button_label(int type, PsHostPadButton button);

/* ---- events ---- */
typedef enum {
    PS_HOST_INPUT_EVENT_OTHER,
    /* code: the key (possibly past PS_HOST_INPUT_KEY_COUNT); is_repeat: the keyboard's auto-repeat. */
    PS_HOST_INPUT_EVENT_KEY_DOWN,
    /* code: the PS_HOST_INPUT_MOUSE_* button. */
    PS_HOST_INPUT_EVENT_MOUSE_BUTTON_DOWN,
    /* code: the PsHostPadButton, of the pad h_pad. */
    PS_HOST_INPUT_EVENT_PAD_BUTTON_DOWN,
    /* code: the PsHostPadAxis, of the pad h_pad, now at value. */
    PS_HOST_INPUT_EVENT_PAD_AXIS,
} PsHostInputEventKind;
typedef struct {
    PsHostInputEventKind kind;
    int code;
    PsHostPadId h_pad;
    int16_t value;
    bool is_repeat;
} PsHostInputEvent;
typedef void (*PsHostInputEventFn)(const PsHostInputEvent *p_event);
/* Calls pfn_on_event with every host event, on the thread that pumps them and before the window's own
 * handling; it must not block. One watcher: a later call replaces the function. Once the window is
 * ready (ps_host_input_is_window_ready): 0, or EIO. */
int ps_host_input_watch(PsHostInputEventFn pfn_on_event);
/* The backend's reason for the last failure on the calling thread. */
const char *ps_host_input_error(void);

#ifdef __cplusplus
}
#endif
#endif
