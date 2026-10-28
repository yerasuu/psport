// SPDX-License-Identifier: GPL-2.0-or-later
/* The game's window (decision #1009, phase 2b): one window on a display of the host, the events that
 * reach it, the pointer over it and text entry into it. The window thread (gpu/port/psport_gpu.cpp)
 * starts the host's windowing, creates the window and pumps its events; the calls are made on that
 * thread unless one says otherwise, as the host's windowing requires. Keys come twice: by position
 * (scancode, a USB HID usage as in ps_host_input.h) and by meaning (keycode: the character the key types
 * in the host's layout, or PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | scancode for a key that types none).
 * Mouse buttons, modifiers, gamepad buttons and axes are ps_host_input.h's. The surface the GPU API
 * presents to is ps_host_window_vulkan.h's. Errors are the host's errno values, with the backend's
 * reason for the last one in ps_host_window_error; host_features.inc says what each OS has (window).
 * One shared backend serves every OS that has it (gpu/platform/common/, in libpsport-gpu, which alone
 * calls these); what differs by window system is in gpu/platform/<os>/. */
#ifndef PS_HOST_WINDOW_H
#define PS_HOST_WINDOW_H
#include <stdbool.h>
#include <stdint.h>
#include "ps_host_input.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsHostWindow PsHostWindow;
/* Names a window in its events for as long as it exists; 0 is none. */
typedef uint32_t PsHostWindowId;
/* Names a connected display; 0 is none. */
typedef uint32_t PsHostDisplayId;
/* What a window system's surface is made of, for the GPU API (ps_host_window_vulkan.h). */
typedef struct PsHostWindowSurface PsHostWindowSurface;

/* ---- keycodes ---- */
enum {
    PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK = 0x40000000,
    PS_HOST_WINDOW_KEYCODE_UNKNOWN = 0,
    PS_HOST_WINDOW_KEYCODE_BACKSPACE = 0x08,
    PS_HOST_WINDOW_KEYCODE_TAB = 0x09,
    PS_HOST_WINDOW_KEYCODE_RETURN = 0x0d,
    PS_HOST_WINDOW_KEYCODE_ESCAPE = 0x1b,
    PS_HOST_WINDOW_KEYCODE_SPACE = ' ',
    PS_HOST_WINDOW_KEYCODE_COMMA = ',',
    PS_HOST_WINDOW_KEYCODE_PERIOD = '.',
    PS_HOST_WINDOW_KEYCODE_SLASH = '/',
    PS_HOST_WINDOW_KEYCODE_LEFTBRACKET = '[',
    PS_HOST_WINDOW_KEYCODE_RIGHTBRACKET = ']',
    PS_HOST_WINDOW_KEYCODE_A = 'a',
    PS_HOST_WINDOW_KEYCODE_C = 'c',
    PS_HOST_WINDOW_KEYCODE_D = 'd',
    PS_HOST_WINDOW_KEYCODE_E = 'e',
    PS_HOST_WINDOW_KEYCODE_Q = 'q',
    PS_HOST_WINDOW_KEYCODE_S = 's',
    PS_HOST_WINDOW_KEYCODE_V = 'v',
    PS_HOST_WINDOW_KEYCODE_W = 'w',
    PS_HOST_WINDOW_KEYCODE_X = 'x',
    PS_HOST_WINDOW_KEYCODE_Z = 'z',
    PS_HOST_WINDOW_KEYCODE_DELETE = 0x7f,
    /* The keys that type nothing: the mask with their scancode. */
    PS_HOST_WINDOW_KEYCODE_F1 = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 58,
    /* F2 to F12 are consecutive. */
    PS_HOST_WINDOW_KEYCODE_F2 =PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 59,
    PS_HOST_WINDOW_KEYCODE_F12 = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 69,
    PS_HOST_WINDOW_KEYCODE_INSERT = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 73,
    PS_HOST_WINDOW_KEYCODE_HOME = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 74,
    PS_HOST_WINDOW_KEYCODE_PAGEUP = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 75,
    PS_HOST_WINDOW_KEYCODE_END = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 77,
    PS_HOST_WINDOW_KEYCODE_PAGEDOWN = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 78,
    PS_HOST_WINDOW_KEYCODE_RIGHT = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 79,
    PS_HOST_WINDOW_KEYCODE_LEFT = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 80,
    PS_HOST_WINDOW_KEYCODE_DOWN = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 81,
    PS_HOST_WINDOW_KEYCODE_UP = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 82,
    PS_HOST_WINDOW_KEYCODE_KP_ENTER = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 88,
    PS_HOST_WINDOW_KEYCODE_LCTRL = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 224,
    PS_HOST_WINDOW_KEYCODE_LSHIFT = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 225,
    PS_HOST_WINDOW_KEYCODE_LALT = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 226,
    PS_HOST_WINDOW_KEYCODE_RCTRL = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 228,
    PS_HOST_WINDOW_KEYCODE_RSHIFT = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 229,
    PS_HOST_WINDOW_KEYCODE_RALT = PS_HOST_WINDOW_KEYCODE_SCANCODE_MASK | 230,
};
#define PS_HOST_WINDOW_MOD_CTRL (PS_HOST_INPUT_MOD_LCTRL | PS_HOST_INPUT_MOD_RCTRL)
#define PS_HOST_WINDOW_MOD_SHIFT (PS_HOST_INPUT_MOD_LSHIFT | PS_HOST_INPUT_MOD_RSHIFT)
#define PS_HOST_WINDOW_MOD_ALT (PS_HOST_INPUT_MOD_LALT | PS_HOST_INPUT_MOD_RALT)

/* ---- the host's windowing ---- */
/* Starts the host's windowing, with its gamepad support too when has_gamepads (their events are pumped
 * with the window's): 0, or EIO. */
int ps_host_window_start(bool has_gamepads);
/* The host's name for its window system ("x11", "wayland", "windows"); NULL before ps_host_window_start. */
const char *ps_host_window_driver(void);
/* The identifier the window system groups the process's windows by (an app id). */
const char *ps_host_window_app_id(void);
/* Milliseconds since ps_host_window_start, for timing what the user does with the window. */
uint64_t ps_host_window_ticks_ms(void);
/* Pauses the window thread between pumps. */
void ps_host_window_delay_ms(uint32_t delay_ms);
/* Whether a mouse, or a keyboard, is connected. */
bool ps_host_window_has_mouse(void);
bool ps_host_window_has_keyboard(void);
/* The keycode a key gives in the host's layout with the modifiers held, for libSceImeKeyboard's
 * characters. */
uint32_t ps_host_window_keycode(int scancode, uint16_t mod);
void ps_host_window_free(void *p_memory);
/* The backend's reason for the last failure on the calling thread. */
const char *ps_host_window_error(void);

/* ---- displays ---- */
typedef struct {
    int width, height;   /* in the host's coordinates; pixels are these times pixel_density */
    float pixel_density; /* 0 when the host does not say */
    float refresh_hz;    /* 0 when the host does not say */
} PsHostDisplayMode;
/* The display the host calls its primary one, 0 when it has none. */
PsHostDisplayId ps_host_window_primary_display(void);
/* The connected displays in the host's order, *p_out_count of them: an array the caller frees with
 * ps_host_window_free, or NULL. */
PsHostDisplayId *ps_host_window_displays(int *p_out_count);
/* NULL when the host has no name for it. */
const char *ps_host_window_display_name(PsHostDisplayId h_display);
/* Whether the display's name contains sz_part, ignoring case as the host compares names. */
bool ps_host_window_display_is_named(PsHostDisplayId h_display, const char *sz_part);
/* The display's desktop mode in *p_out: 0, or EIO. */
int ps_host_window_display_mode(PsHostDisplayId h_display, PsHostDisplayMode *p_out);

/* ---- the window ---- */
/* A resizable window the GPU API can present to, width x height, centred on h_display, fullscreen there
 * when is_fullscreen: NULL with the host's reason in ps_host_window_error. */
PsHostWindow *ps_host_window_create(const char *sz_title, PsHostDisplayId h_display, int width, int height,
                                    bool is_fullscreen);
void ps_host_window_destroy(PsHostWindow *p_window);
/* The window's icon from a PNG file, its size in *p_out_width and *p_out_height: 0, or EIO. */
int ps_host_window_set_icon(PsHostWindow *p_window, const char *sz_png_path, int *p_out_width, int *p_out_height);
void ps_host_window_set_title(PsHostWindow *p_window, const char *sz_title);
/* The window's size in the host's coordinates (the pointer's), and in pixels (the surface's). */
void ps_host_window_size(PsHostWindow *p_window, int *p_out_width, int *p_out_height);
void ps_host_window_size_in_pixels(PsHostWindow *p_window, int *p_out_width, int *p_out_height);
/* The display most of the window is on, 0 when the host cannot tell. */
PsHostDisplayId ps_host_window_display(PsHostWindow *p_window);
bool ps_host_window_is_fullscreen(PsHostWindow *p_window);
bool ps_host_window_has_input_focus(PsHostWindow *p_window);
/* The window system's surface of the window (gpu/platform/<os>/): NULL when the window system is not
 * one this OS presents to. Valid while the window exists. */
const PsHostWindowSurface *ps_host_window_surface(PsHostWindow *p_window);
/* Relative mode holds the pointer in the window, hidden, and reports only its motion: 0, or EIO. */
int ps_host_window_set_relative_mouse(PsHostWindow *p_window, bool is_relative);
/* The system pointer over the window, shown or hidden. */
void ps_host_window_show_cursor(bool is_shown);
/* The window under the pointer and where the pointer is in it (host coordinates); false when the
 * pointer is over none of the process's windows. */
bool ps_host_window_mouse_focus(PsHostWindowId *p_out_window, float *p_out_x, float *p_out_y);
/* Pixels per host coordinate of a window, 0 when it no longer exists. */
float ps_host_window_pixel_density(PsHostWindowId h_window);
/* Text entry: while on, typing comes as PS_HOST_WINDOW_EVENT_TEXT_INPUT (the host's input method
 * composes it). */
void ps_host_window_text_input(PsHostWindow *p_window, bool is_on);
bool ps_host_window_is_text_input(PsHostWindow *p_window);

/* ---- events ---- */
typedef enum {
    PS_HOST_WINDOW_EVENT_OTHER,
    /* The process is asked to end: ps_host_window_request_quit, or SIGTERM and SIGINT, which the host's
     * windowing turns into this. */
    PS_HOST_WINDOW_EVENT_QUIT,
    PS_HOST_WINDOW_EVENT_CLOSE_REQUESTED,
    PS_HOST_WINDOW_EVENT_RESIZED,
    PS_HOST_WINDOW_EVENT_PIXEL_SIZE_CHANGED,
    PS_HOST_WINDOW_EVENT_DISPLAY_CHANGED,
    PS_HOST_WINDOW_EVENT_FOCUS_GAINED,
    PS_HOST_WINDOW_EVENT_FOCUS_LOST,
    PS_HOST_WINDOW_EVENT_MOUSE_LEAVE,
    PS_HOST_WINDOW_EVENT_KEY_DOWN, /* key */
    PS_HOST_WINDOW_EVENT_KEY_UP,   /* key */
    PS_HOST_WINDOW_EVENT_TEXT_INPUT, /* text */
    PS_HOST_WINDOW_EVENT_MOUSE_MOTION, /* motion */
    PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN, /* button */
    PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP,   /* button */
    PS_HOST_WINDOW_EVENT_MOUSE_WHEEL,       /* wheel */
    PS_HOST_WINDOW_EVENT_MOUSE_ADDED,
    PS_HOST_WINDOW_EVENT_MOUSE_REMOVED,
    PS_HOST_WINDOW_EVENT_KEYBOARD_ADDED,
    PS_HOST_WINDOW_EVENT_KEYBOARD_REMOVED,
    PS_HOST_WINDOW_EVENT_PAD_BUTTON_DOWN, /* pad_button */
    PS_HOST_WINDOW_EVENT_PAD_BUTTON_UP,   /* pad_button */
    PS_HOST_WINDOW_EVENT_PAD_AXIS,        /* pad_axis */
} PsHostWindowEventKind;
typedef struct PsHostWindowEvent {
    PsHostWindowEventKind kind;
    /* The window it happened in, 0 for none. */
    PsHostWindowId h_window;
    union {
        struct {
            int scancode;
            uint32_t keycode;
            uint16_t mod; /* PS_HOST_INPUT_MOD_* bits */
            bool is_repeat;
        } key;
        struct {
            const char *sz_utf8; /* valid until the next ps_host_window_poll_event */
        } text;
        struct {
            float x, y;   /* where the pointer is, in host coordinates */
            float dx, dy; /* how far it moved, also in relative mode */
        } motion;
        struct {
            int button; /* PS_HOST_INPUT_MOUSE_* */
            float x, y;
        } button;
        struct {
            float x, y; /* notches, fractional on smooth wheels; +y away from the user */
            bool is_flipped; /* the host reports the wheel's direction reversed (natural scrolling) */
        } wheel;
        struct {
            PsHostPadId h_pad;
            int button; /* PsHostPadButton */
        } pad_button;
        struct {
            PsHostPadId h_pad;
            int axis; /* PsHostPadAxis */
            int16_t value;
        } pad_axis;
    };
} PsHostWindowEvent;
/* The next pending event in *p_out, false when there is none. Pumping runs ps_host_input_watch's watcher
 * on each event as it arrives, before this returns it. */
bool ps_host_window_poll_event(PsHostWindowEvent *p_out);
/* From any thread: a PS_HOST_WINDOW_EVENT_QUIT for the window thread: 0, or EIO. */
int ps_host_window_request_quit(void);

#ifdef __cplusplus
}
#endif
#endif
