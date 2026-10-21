#ifndef BB_RUNTIME_PAD_H
#define BB_RUNTIME_PAD_H
/* libScePad's data and the scripted input that runtime_pad.c mixes into it (runtime_pad_script.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
/* 1 once SDL's gamepad subsystem is up, -1 when it is not used. */
extern int g_runtime_pad_sdl_ready;
/* PS_PAD_FILE's held buttons, sticks (-1: not overridden), touchpad side (-1: none) and the mouse
 * motion in pixels added at every pad read. */
typedef struct { uint32_t buttons; int stick[4]; int touch_side; float mouse_dx, mouse_dy; } PadInjected;
extern PadInjected g_runtime_pad_injected;
void runtime_pad_read_inject(void);
/* Button prompts for the device in use (runtime_pad.c): the device whose input came last, and a
 * generation that changes with it, the pad's type and the bindings. */
enum { RUNTIME_PAD_PROMPT_PLAYSTATION=0, RUNTIME_PAD_PROMPT_GAMEPAD=1, RUNTIME_PAD_PROMPT_KEYBOARD=2 };
int runtime_pad_prompt_device(void); /* -1 until the pad is first read */
unsigned runtime_pad_prompt_generation(void);
/* mouse_look in psport.ini: the right stick's prompt is the mouse. */
int runtime_pad_mouse_look(void);
/* The prompt of input sz_input (the key.<input> names: cross ... look_right) on the device in use,
 * its label (UTF-8) in sz_out_label; with should_prefer_mouse, a bound mouse button before a key
 * while mouse look is on. Returns its kind: 0 none (the game's own icon stays), 'K' a key, 'M' a
 * mouse button (Left Right Middle X1 X2, Wheel Up, Wheel Down), 'F' a face button (A B X Y), 'S' a
 * shoulder or trigger (LB RB LT RT; L R ZL ZR), 'C' another button or a stick (LS RS Menu View ...). */
int runtime_pad_prompt(const char *sz_input, int should_prefer_mouse, char *sz_out_label, size_t cb_label);
/* 1 once per press of psport.ini's key.menu or pad.menu binding (unbound: never), for the overlay's
 * window-thread event handler, which toggles the port's menu on it. */
int runtime_pad_menu_pressed(void);
/* The default bindings a launcher shows, as psport.ini key.<input>= and pad.<input>= lines on
 * stdout: the neutral layout with sz_title_controls (Game::Title sz_default_controls, or NULL) over
 * it. Only for a query: it leaves the bindings at those defaults. */
void runtime_pad_print_default_controls(const char *sz_title_controls);
void runtime_pad_record_sample(const PadData *p_pad_data);
/* Motion emulation (runtime_pad_motion.c). configure: psport.ini's pad_motion (-1 without the line),
 * PS_PAD_MOTION wins; the rest only while it is on. begin: once per sample, the seconds since the
 * previous one. from_pad after the gamepad's buttons and sticks; from_desktop after the keyboard's
 * and the mouse buttons', before mouse look, which gets what is left of the motion in *p_dx, *p_dy;
 * report last. */
typedef struct SDL_Gamepad SDL_Gamepad;
void runtime_pad_motion_configure(int configured);
int runtime_pad_motion_is_on(void);
float runtime_pad_motion_begin(uint64_t timestamp_us);
void runtime_pad_motion_from_pad(PadData *p_pad_data, SDL_Gamepad *p_sdl_gamepad, float dt);
void runtime_pad_motion_from_desktop(PadData *p_pad_data, const bool *p_is_key_down, int is_touch_key_down,
                                     int is_mouse_left_down, float mouse_sensitivity, double *p_dx, double *p_dy,
                                     int is_captured);
void runtime_pad_motion_report(PadData *p_pad_data, float dt);
/* PS_PAD_REPLAY (runtime_pad_script.c). Callers must hold runtime_pad.c's pad lock (sample() does),
 * since a re-arm frees and reloads the recording another pad reader would otherwise be indexing. */
void runtime_pad_replay_sample(PadData *p_pad_data);
/* The pad's own motion sensors (runtime_pad_sensors.c). open: each time another pad (or none) is
 * opened, which enables its sensors; read: once per sample with the seconds since the previous one,
 * 1 when the sensors filled the motion fields (the emulation then stays out); finish last, at rest
 * while the game has turned the reports off. Called under the pad's lock. */
void runtime_pad_sensors_open(SDL_Gamepad *p_sdl_gamepad);
int runtime_pad_sensors_read(PadData *p_pad_data, float dt);
void runtime_pad_sensors_finish(PadData *p_pad_data);
void runtime_pad_sensors_set_reporting(int is_enabled);
void runtime_pad_sensors_set_tilt_correction(int is_enabled);
void runtime_pad_sensors_set_deadband(int is_enabled);
void runtime_pad_sensors_reset_orientation(void);
void runtime_pad_sensors_report(void);
#endif
