/* libSceImeKeyboard on the host keyboard. The host's events are pumped by the window thread
 * (gpu/shim/window.cpp), which queues the keys for the game while it has the keyboard open; here
 * they are handed to the game's handler from sceImeUpdate, on the game's thread. The host keyboard
 * is resource 1: GetResourceId and the open event list it while the host has a keyboard, and its
 * arrival or removal is reported as a connection or disconnection. While the keyboard is open its
 * keys reach the game and not the keyboard-as-pad (runtime_pad.c); the port keeps its own keys.
 * Without a window or a host keyboard the keyboard opens and lists none, as a console with no
 * keyboard plugged in does. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define IME_BUSY ((int32_t)0x80BC0001)
#define IME_NOT_OPENED ((int32_t)0x80BC0002)
#define IME_CONNECTION_FAILED ((int32_t)0x80BC0004)
#define IME_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_INVALID_OPTION ((int32_t)0x80BC0015)
#define IME_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_INVALID_RESERVED ((int32_t)0x80BC0032)
#define UNIX_EPOCH_TICKS UINT64_C(0xdcbffeff2bc000)
#define HOST_RESOURCE 1u
#define HID_LAST_KEY 0xE7
#define EVENTS_PER_UPDATE 64

enum { OPTION_REPEAT=0x1, OPTION_REPEAT_EACH_KEY=0x2, OPTION_MASK=0x3f };
enum { EVENT_OPEN=256, EVENT_KEY_DOWN=257, EVENT_KEY_UP=258, EVENT_KEY_REPEAT=259, EVENT_CONNECTION=260,
       EVENT_DISCONNECTION=261 };
enum {
    STATE_KEYCODE_VALID=0x1, STATE_CHARACTER_VALID=0x2, STATE_L_CTRL=0x100, STATE_L_SHIFT=0x200, STATE_L_ALT=0x400,
    STATE_L_GUI=0x800, STATE_R_CTRL=0x1000, STATE_R_SHIFT=0x2000, STATE_R_ALT=0x4000, STATE_R_GUI=0x8000,
    STATE_NUM_LOCK=0x10000, STATE_CAPS_LOCK=0x20000, STATE_SCROLL_LOCK=0x40000,
};
#define KEYBOARD_TYPE_ENGLISH_US 4u
#define KEYBOARD_CONNECTED 1u

typedef ABI void (*ImeHandler)(void *p_arg, const void *p_event);
typedef struct { uint32_t option; int8_t reserved1[4]; void *arg; ImeHandler handler; int8_t reserved2[8]; } KeyboardParam;
typedef struct { int32_t user; uint32_t resource[5]; } ResourceIds;
typedef struct {
    uint16_t keycode, character;
    uint32_t status, type;
    int32_t user;
    uint32_t resource;
    uint64_t timestamp; /* OrbisRtcTick */
} Keycode;
typedef struct {
    uint32_t id;
    union { Keycode keycode; ResourceIds ids; int8_t reserved[64]; uint64_t align; } param;
} ImeEvent;
typedef struct {
    int32_t user;
    uint32_t device, type, repeat_delay, repeat_rate, status;
    int8_t reserved[12];
} KeyboardInfo;
_Static_assert(sizeof(Keycode)==32,"OrbisImeKeycode layout");
_Static_assert(sizeof(ImeEvent)==72,"OrbisImeEvent layout");
_Static_assert(sizeof(KeyboardInfo)==36,"OrbisImeKeyboardInfo layout");

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { int is_open, is_opened_sent, is_connected; int32_t user; uint32_t option; void *p_arg; ImeHandler pfn_handler; } s_kbd;
static int s_taken;
static int s_host_logged=-1;
static uint64_t s_c_delivered, s_c_repeats_dropped, s_c_unmapped_keys;
static uint32_t s_c_dropped;

static int host_keyboard(void) {
    int keyboard=0;
    psport_gpu_game_keyboard_take(NULL,0,&keyboard,&s_c_dropped);
    return keyboard;
}
static void log_host(int keyboard) {
    if (keyboard==s_host_logged) return;
    s_host_logged=keyboard;
    printf("Runtime: keyboard: %s\n",keyboard ? "the host keyboard reaches libSceImeKeyboard (its keys go to the game, not to the pad)"
                                              : "no host keyboard, libSceImeKeyboard lists none");
}
static void resource_ids(ResourceIds *p_ids, int32_t user, int is_connected) {
    memset(p_ids,0,sizeof(*p_ids));
    p_ids->user=user;
    if (is_connected) p_ids->resource[0]=HOST_RESOURCE;
}
static uint32_t key_state(const PsKeyEvent *p_key) {
    static const struct { uint16_t mod; uint32_t state; } s_mods[]={
        {PS_HOST_INPUT_MOD_LCTRL,STATE_L_CTRL}, {PS_HOST_INPUT_MOD_LSHIFT,STATE_L_SHIFT},
        {PS_HOST_INPUT_MOD_LALT,STATE_L_ALT}, {PS_HOST_INPUT_MOD_LGUI,STATE_L_GUI},
        {PS_HOST_INPUT_MOD_RCTRL,STATE_R_CTRL}, {PS_HOST_INPUT_MOD_RSHIFT,STATE_R_SHIFT},
        {PS_HOST_INPUT_MOD_RALT,STATE_R_ALT}, {PS_HOST_INPUT_MOD_RGUI,STATE_R_GUI},
        {PS_HOST_INPUT_MOD_NUM,STATE_NUM_LOCK}, {PS_HOST_INPUT_MOD_CAPS,STATE_CAPS_LOCK},
        {PS_HOST_INPUT_MOD_SCROLL,STATE_SCROLL_LOCK},
    };
    uint32_t state=STATE_KEYCODE_VALID;
    if (p_key->character && p_key->character<0x10000) state|=STATE_CHARACTER_VALID;
    for (size_t i=0;i<sizeof(s_mods)/sizeof(*s_mods);++i) if (p_key->mod & s_mods[i].mod) state|=s_mods[i].state;
    return state;
}
/* The events due since the last update, in order; called under lock. */
static int collect(ImeEvent *p_events) {
    PsKeyEvent keys[EVENTS_PER_UPDATE];
    int keyboard=0, c_events=0;
    const int taken_keys=psport_gpu_game_keyboard_take(keys,EVENTS_PER_UPDATE-1,&keyboard,&s_c_dropped);
    log_host(keyboard);
    if (!s_kbd.is_opened_sent) {
        s_kbd.is_opened_sent=1; s_kbd.is_connected=keyboard;
        p_events[c_events].id=EVENT_OPEN;
        resource_ids(&p_events[c_events++].param.ids,s_kbd.user,keyboard);
    } else if (keyboard!=s_kbd.is_connected) {
        s_kbd.is_connected=keyboard;
        p_events[c_events].id=keyboard ? EVENT_CONNECTION : EVENT_DISCONNECTION;
        resource_ids(&p_events[c_events++].param.ids,s_kbd.user,1);
    }
    for (int i=0;i<taken_keys;++i) {
        const PsKeyEvent *p_key=&keys[i];
        if (p_key->scancode==0 || p_key->scancode>HID_LAST_KEY) { ++s_c_unmapped_keys; continue; }
        if (p_key->repeat && !(s_kbd.option & (OPTION_REPEAT|OPTION_REPEAT_EACH_KEY))) { ++s_c_repeats_dropped; continue; }
        ImeEvent *p_ime_event=&p_events[c_events++];
        memset(p_ime_event,0,sizeof(*p_ime_event));
        p_ime_event->id=!p_key->down ? EVENT_KEY_UP : p_key->repeat ? EVENT_KEY_REPEAT : EVENT_KEY_DOWN;
        p_ime_event->param.keycode=(Keycode){
            .keycode=p_key->scancode, .character=p_key->character<0x10000 ? (uint16_t)p_key->character : 0,
            .status=key_state(p_key), .type=KEYBOARD_TYPE_ENGLISH_US, .user=s_kbd.user, .resource=HOST_RESOURCE,
            .timestamp=UNIX_EPOCH_TICKS+p_key->time_us,
        };
    }
    return c_events;
}

static ABI int32_t keyboard_open(int32_t user, const KeyboardParam *p_param) {
    if (!p_param) return IME_INVALID_ADDRESS;
    if (!p_param->handler) return IME_INVALID_HANDLER;
    if (p_param->option & ~(uint32_t)OPTION_MASK) return IME_INVALID_OPTION;
    if (user==-1) return IME_INVALID_USER_ID;
    for (size_t i=0;i<sizeof(p_param->reserved1);++i) if (p_param->reserved1[i]) return IME_INVALID_RESERVED;
    for (size_t i=0;i<sizeof(p_param->reserved2);++i) if (p_param->reserved2[i]) return IME_INVALID_RESERVED;
    pthread_mutex_lock(&s_lock);
    if (s_kbd.is_open) { pthread_mutex_unlock(&s_lock); return IME_BUSY; }
    s_kbd.is_open=1; s_kbd.is_opened_sent=0; s_kbd.user=user; s_kbd.option=p_param->option; s_kbd.p_arg=p_param->arg; s_kbd.pfn_handler=p_param->handler;
    __atomic_store_n(&s_taken,1,__ATOMIC_RELAXED);
    psport_gpu_game_keyboard_open(1);
    log_host(host_keyboard());
    pthread_mutex_unlock(&s_lock);
    printf("Runtime: keyboard opened for user %d (option 0x%x)\n",user,p_param->option);
    return 0;
}
static ABI int32_t keyboard_close(int32_t user) {
    if (user==-1) return IME_INVALID_USER_ID;
    pthread_mutex_lock(&s_lock);
    const int was_open=s_kbd.is_open;
    s_kbd.is_open=0;
    __atomic_store_n(&s_taken,0,__ATOMIC_RELAXED);
    psport_gpu_game_keyboard_open(0);
    pthread_mutex_unlock(&s_lock);
    return was_open ? 0 : IME_NOT_OPENED;
}
static ABI int32_t keyboard_resource_id(int32_t user, ResourceIds *p_ids) {
    if (!p_ids) return IME_INVALID_ADDRESS;
    if (user==-1) return IME_INVALID_USER_ID;
    pthread_mutex_lock(&s_lock);
    const int is_open=s_kbd.is_open, keyboard=is_open && host_keyboard();
    pthread_mutex_unlock(&s_lock);
    resource_ids(p_ids,user,keyboard);
    return !is_open ? IME_NOT_OPENED : keyboard ? 0 : IME_CONNECTION_FAILED;
}
/* The host's key repeat timing applies (the host does not tell it), so delay and rate stay 0. */
static ABI int32_t keyboard_info(uint32_t resource, KeyboardInfo *p_info) {
    if (!p_info) return IME_INVALID_ADDRESS;
    pthread_mutex_lock(&s_lock);
    const int is_connected=s_kbd.is_open && resource==HOST_RESOURCE && host_keyboard();
    const int32_t user=s_kbd.user;
    pthread_mutex_unlock(&s_lock);
    memset(p_info,0,sizeof(*p_info));
    if (is_connected) { p_info->user=user; p_info->type=KEYBOARD_TYPE_ENGLISH_US; p_info->status=KEYBOARD_CONNECTED; }
    return 0;
}
/* The handler runs outside the lock: it may call back into the library (GetInfo, Close). The
 * on-screen keyboard's events (runtime_ime.c) come first. */
static ABI int32_t ime_update(ImeHandler pfn_handler) {
    ImeEvent events[EVENTS_PER_UPDATE];
    const int is_panel_open=runtime_ime_update((void *)pfn_handler);
    pthread_mutex_lock(&s_lock);
    if (!s_kbd.is_open) { pthread_mutex_unlock(&s_lock); return is_panel_open ? 0 : IME_NOT_OPENED; }
    const int c_events=collect(events);
    ImeHandler pfn_call=pfn_handler ? pfn_handler : s_kbd.pfn_handler;
    void *p_arg=s_kbd.p_arg;
    s_c_delivered+=(uint64_t)c_events;
    pthread_mutex_unlock(&s_lock);
    for (int i=0;i<c_events;++i) pfn_call(p_arg,&events[i]);
    return 0;
}

static const RuntimeExport s_exports[]={
    {"sceImeKeyboardOpen",keyboard_open}, {"sceImeKeyboardGetResourceId",keyboard_resource_id},
    {"sceImeKeyboardGetInfo",keyboard_info}, {"sceImeKeyboardClose",keyboard_close}, {"sceImeUpdate",ime_update},
};
uintptr_t runtime_keyboard_resolve(const char *sz_name) {
    const uintptr_t va_found=RUNTIME_LOOKUP(s_exports,sz_name);
    return va_found ? va_found : runtime_ime_resolve(sz_name);
}
int runtime_keyboard_taken(void) { return __atomic_load_n(&s_taken,__ATOMIC_RELAXED); }
void runtime_keyboard_report(void) {
    if (s_host_logged<0) return;
    printf("Runtime: keyboard events=%llu, host keyboard=%s, queue overflow=%u, repeats not asked for=%llu, keys with no USB keycode=%llu\n",
           (unsigned long long)s_c_delivered,s_host_logged>0 ? "yes" : "none",s_c_dropped,(unsigned long long)s_c_repeats_dropped,
           (unsigned long long)s_c_unmapped_keys);
}
