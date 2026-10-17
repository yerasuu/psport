#ifndef BB_RUNTIME_PAD_H
#define BB_RUNTIME_PAD_H
/* libScePad's data and the scripted input that runtime_pad.c mixes into it (runtime_pad_script.c). */
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
/* PS_PAD_FILE's held buttons, sticks (-1: not overridden) and touchpad side (-1: none). */
typedef struct { uint32_t buttons; int stick[4]; int touch_side; } PadInjected;
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
void runtime_pad_record_sample(const PadData *p_pad_data);
void runtime_pad_replay_sample(PadData *p_pad_data);
#endif
