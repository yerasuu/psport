/* libSceImeKeyboard on the host keyboard. SDL events are pumped by the window thread
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
#include <SDL3/SDL.h>

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

typedef ABI void (*ImeHandler)(void *arg, const void *event);
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

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct { int open, opened_sent, connected; int32_t user; uint32_t option; void *arg; ImeHandler handler; } kbd;
static int taken;
static int host_logged=-1;
static uint64_t delivered, repeats_dropped, unmapped_keys;
static uint32_t dropped;

static int host_keyboard(void) {
    int keyboard=0;
    psport_gpu_game_keyboard_take(NULL,0,&keyboard,&dropped);
    return keyboard;
}
static void log_host(int keyboard) {
    if (keyboard==host_logged) return;
    host_logged=keyboard;
    printf("Runtime: keyboard: %s\n",keyboard ? "the host keyboard reaches libSceImeKeyboard (its keys go to the game, not to the pad)"
                                              : "no host keyboard, libSceImeKeyboard lists none");
}
static void resource_ids(ResourceIds *ids, int32_t user, int connected) {
    memset(ids,0,sizeof(*ids));
    ids->user=user;
    if (connected) ids->resource[0]=HOST_RESOURCE;
}
static uint32_t key_state(const PsKeyEvent *key) {
    static const struct { uint16_t mod; uint32_t state; } mods[]={
        {SDL_KMOD_LCTRL,STATE_L_CTRL}, {SDL_KMOD_LSHIFT,STATE_L_SHIFT}, {SDL_KMOD_LALT,STATE_L_ALT},
        {SDL_KMOD_LGUI,STATE_L_GUI}, {SDL_KMOD_RCTRL,STATE_R_CTRL}, {SDL_KMOD_RSHIFT,STATE_R_SHIFT},
        {SDL_KMOD_RALT,STATE_R_ALT}, {SDL_KMOD_RGUI,STATE_R_GUI}, {SDL_KMOD_NUM,STATE_NUM_LOCK},
        {SDL_KMOD_CAPS,STATE_CAPS_LOCK}, {SDL_KMOD_SCROLL,STATE_SCROLL_LOCK},
    };
    uint32_t state=STATE_KEYCODE_VALID;
    if (key->character && key->character<0x10000) state|=STATE_CHARACTER_VALID;
    for (size_t i=0;i<sizeof(mods)/sizeof(*mods);++i) if (key->mod & mods[i].mod) state|=mods[i].state;
    return state;
}
/* The events due since the last update, in order; called under lock. */
static int collect(ImeEvent *events) {
    PsKeyEvent keys[EVENTS_PER_UPDATE];
    int keyboard=0, count=0;
    const int taken_keys=psport_gpu_game_keyboard_take(keys,EVENTS_PER_UPDATE-1,&keyboard,&dropped);
    log_host(keyboard);
    if (!kbd.opened_sent) {
        kbd.opened_sent=1; kbd.connected=keyboard;
        events[count].id=EVENT_OPEN;
        resource_ids(&events[count++].param.ids,kbd.user,keyboard);
    } else if (keyboard!=kbd.connected) {
        kbd.connected=keyboard;
        events[count].id=keyboard ? EVENT_CONNECTION : EVENT_DISCONNECTION;
        resource_ids(&events[count++].param.ids,kbd.user,1);
    }
    for (int i=0;i<taken_keys;++i) {
        const PsKeyEvent *key=&keys[i];
        if (key->scancode==0 || key->scancode>HID_LAST_KEY) { ++unmapped_keys; continue; }
        if (key->repeat && !(kbd.option & (OPTION_REPEAT|OPTION_REPEAT_EACH_KEY))) { ++repeats_dropped; continue; }
        ImeEvent *e=&events[count++];
        memset(e,0,sizeof(*e));
        e->id=!key->down ? EVENT_KEY_UP : key->repeat ? EVENT_KEY_REPEAT : EVENT_KEY_DOWN;
        e->param.keycode=(Keycode){
            .keycode=key->scancode, .character=key->character<0x10000 ? (uint16_t)key->character : 0,
            .status=key_state(key), .type=KEYBOARD_TYPE_ENGLISH_US, .user=kbd.user, .resource=HOST_RESOURCE,
            .timestamp=UNIX_EPOCH_TICKS+key->time_us,
        };
    }
    return count;
}

static ABI int32_t keyboard_open(int32_t user, const KeyboardParam *param) {
    if (!param) return IME_INVALID_ADDRESS;
    if (!param->handler) return IME_INVALID_HANDLER;
    if (param->option & ~(uint32_t)OPTION_MASK) return IME_INVALID_OPTION;
    if (user==-1) return IME_INVALID_USER_ID;
    for (size_t i=0;i<sizeof(param->reserved1);++i) if (param->reserved1[i]) return IME_INVALID_RESERVED;
    for (size_t i=0;i<sizeof(param->reserved2);++i) if (param->reserved2[i]) return IME_INVALID_RESERVED;
    pthread_mutex_lock(&lock);
    if (kbd.open) { pthread_mutex_unlock(&lock); return IME_BUSY; }
    kbd.open=1; kbd.opened_sent=0; kbd.user=user; kbd.option=param->option; kbd.arg=param->arg; kbd.handler=param->handler;
    __atomic_store_n(&taken,1,__ATOMIC_RELAXED);
    psport_gpu_game_keyboard_open(1);
    log_host(host_keyboard());
    pthread_mutex_unlock(&lock);
    printf("Runtime: keyboard opened for user %d (option 0x%x)\n",user,param->option);
    return 0;
}
static ABI int32_t keyboard_close(int32_t user) {
    if (user==-1) return IME_INVALID_USER_ID;
    pthread_mutex_lock(&lock);
    const int was_open=kbd.open;
    kbd.open=0;
    __atomic_store_n(&taken,0,__ATOMIC_RELAXED);
    psport_gpu_game_keyboard_open(0);
    pthread_mutex_unlock(&lock);
    return was_open ? 0 : IME_NOT_OPENED;
}
static ABI int32_t keyboard_resource_id(int32_t user, ResourceIds *ids) {
    if (!ids) return IME_INVALID_ADDRESS;
    if (user==-1) return IME_INVALID_USER_ID;
    pthread_mutex_lock(&lock);
    const int open=kbd.open, keyboard=open && host_keyboard();
    pthread_mutex_unlock(&lock);
    resource_ids(ids,user,keyboard);
    return !open ? IME_NOT_OPENED : keyboard ? 0 : IME_CONNECTION_FAILED;
}
/* The host's key repeat timing applies (SDL does not tell it), so delay and rate stay 0. */
static ABI int32_t keyboard_info(uint32_t resource, KeyboardInfo *info) {
    if (!info) return IME_INVALID_ADDRESS;
    pthread_mutex_lock(&lock);
    const int connected=kbd.open && resource==HOST_RESOURCE && host_keyboard();
    const int32_t user=kbd.user;
    pthread_mutex_unlock(&lock);
    memset(info,0,sizeof(*info));
    if (connected) { info->user=user; info->type=KEYBOARD_TYPE_ENGLISH_US; info->status=KEYBOARD_CONNECTED; }
    return 0;
}
/* The handler runs outside the lock: it may call back into the library (GetInfo, Close). The
 * on-screen keyboard's events (runtime_ime.c) come first. */
static ABI int32_t ime_update(ImeHandler handler) {
    ImeEvent events[EVENTS_PER_UPDATE];
    const int is_panel_open=runtime_ime_update((void *)handler);
    pthread_mutex_lock(&lock);
    if (!kbd.open) { pthread_mutex_unlock(&lock); return is_panel_open ? 0 : IME_NOT_OPENED; }
    const int count=collect(events);
    ImeHandler call=handler ? handler : kbd.handler;
    void *arg=kbd.arg;
    delivered+=(uint64_t)count;
    pthread_mutex_unlock(&lock);
    for (int i=0;i<count;++i) call(arg,&events[i]);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceImeKeyboardOpen",keyboard_open}, {"sceImeKeyboardGetResourceId",keyboard_resource_id},
    {"sceImeKeyboardGetInfo",keyboard_info}, {"sceImeKeyboardClose",keyboard_close}, {"sceImeUpdate",ime_update},
};
uintptr_t runtime_keyboard_resolve(const char *name) {
    const uintptr_t va_found=RUNTIME_LOOKUP(exports,name);
    return va_found ? va_found : runtime_ime_resolve(name);
}
int runtime_keyboard_taken(void) { return __atomic_load_n(&taken,__ATOMIC_RELAXED); }
void runtime_keyboard_report(void) {
    if (host_logged<0) return;
    printf("Runtime: keyboard events=%llu, host keyboard=%s, queue overflow=%u, repeats not asked for=%llu, keys with no USB keycode=%llu\n",
           (unsigned long long)delivered,host_logged>0 ? "yes" : "none",dropped,(unsigned long long)repeats_dropped,
           (unsigned long long)unmapped_keys);
}
