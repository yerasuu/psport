/* libScePad on SDL3 gamepads and the keyboard. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard layout (also with a gamepad connected: both drive the game; not while the game has
 * libSceImeKeyboard open, runtime_keyboard.c, when the keys are the game's), unless the game's
 * title brings its own (Game::Title sz_default_controls, psport_gpu_title_controls):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right).
 *   Mouse (once the game window is clicked; F1 or leaving the window releases it): look (the
 *   right stick), Left R1, Right L2, Middle R3, side buttons R2 (X1) and L1 (X2), wheel the
 *   d-pad's up/down (quick items).
 *   A binding may be a chord, "Left Shift+Mouse Left": while its modifier is held, the second
 *   key's plain bindings do not count (Shift+Left is R2 alone, not R2 and R1).
 *
 * Stick neutral: SDL exposes no way to read a pad's calibration and some clones report a
 * biased neutral (a Switch-style pad was seen returning both sticks at a constant ~ +/-16380).
 * The neutral of each axis is taken from a quiet window after the pad opens (SDL returns zero
 * until the first report arrives, so those samples are skipped) and subtracted when all four axes
 * are biased; otherwise (a genuine pad, perhaps opened with a stick held) the neutral is 0.
 *
 * Travel: such a clone also uses only part of SDL's -32768..32767 span (its neutral sits in the
 * middle of one half), so reading the axis as -128..127 would reach only half deflection and a
 * full push would never run. Each axis is instead scaled by its own travel: a biased axis starts
 * from its neutral's magnitude and is refined by the largest push seen per direction, so both
 * directions reach full deflection even when the two travels differ by a few percent (that
 * difference is what makes one direction run and the other only walk). A genuine pad keeps the
 * plain -32768..32767 -> -128..127 read. The stick is then converted as shadPS4 does, with an
 * inner/outer dead zone (PS_PAD_DEADZONE, default 5; PS_PAD_DEADZONE_OUTER, default 127) mapping
 * the axis' travel up to full deflection. PS_PAD_CENTER=lx,ly,rx,ry overrides the neutral,
 * PS_PAD_CENTER_CAL=0 disables the measurement. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_pad.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define ERR_NO_HANDLE ((int32_t)0x80920008)
#define PAD_HANDLE 1

typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
typedef struct {
    int32_t device_class;
    uint8_t reserved[4], class_data[12];
} DeviceClassInfo;
/* ScePadDeviceClassData: the class, whether the data is valid, then the class's own fields
 * (wheel, guitar, drum, or others: length and raw bytes). */
typedef struct {
    int32_t device_class;
    uint8_t is_data_valid, reserved[3];
    uint8_t class_data[16];
} DeviceClassData;
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");
_Static_assert(sizeof(DeviceClassData)==24,"ScePadDeviceClassData layout");

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_initialized, s_is_opened;
int g_runtime_pad_sdl_ready;
static SDL_Gamepad *s_p_gamepad;
static size_t s_c_reads;
static uint8_t s_c_connecteds;

/* Per-controller stick neutral (see the file header): the value each axis holds while the
 * sticks are untouched, taken from a quiet window after the pad opens. Works for any pad; a
 * genuine one settles at 0 and the subtraction is a no-op. */
#define PAD_CAL_QUIET 512              /* an axis is quiet when it moved no more than this */
#define PAD_CAL_QUIET_SAMPLES 8
#define PAD_CAL_TIMEOUT_US 3000000u    /* after this, an all-zero reading is accepted as the neutral */
#define PAD_CAL_BIAS 8192              /* |neutral| past this: a biased neutral and a reduced travel */
#define PAD_CAL_REFINE 90              /* % of the base travel a push must reach to count as the full scale */
static int s_is_cal_enabled=-1, s_pad_deadzone=-1, s_pad_deadzone_outer=-1, s_cal_manual;
static int s_cal_have_center, s_cal_started, s_c_cal_quiet;
static int s_cal_center[4], s_cal_prev[4], s_cal_manual_center[4];
static int s_cal_base[4];                /* 0: normal pad; else |neutral| of a biased axis */
static int s_cal_max[4][2];              /* largest push seen per direction on a biased axis */
static uint64_t s_cal_since;

static uint64_t now_us(void) { return ps_host_time_monotonic_ns()/1000u; }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static void cal_load(void) {
    static int s_is_loaded;
    if (s_is_loaded) return;
    s_is_loaded=1;
    s_is_cal_enabled=runtime_env_flag("PS_PAD_CENTER_CAL",1);
    /* Inner/outer dead zone, as shadPS4's analog_deadzone: [inner, outer] maps linearly to the
     * full deflection, so an axis with a reduced travel (outer below 127) reaches full. */
    s_pad_deadzone=runtime_env_int("PS_PAD_DEADZONE",5);
    if (s_pad_deadzone<0) s_pad_deadzone=0; else if (s_pad_deadzone>126) s_pad_deadzone=126;
    s_pad_deadzone_outer=runtime_env_int("PS_PAD_DEADZONE_OUTER",127);
    if (s_pad_deadzone_outer<=s_pad_deadzone) s_pad_deadzone_outer=s_pad_deadzone<127 ? s_pad_deadzone+1 : 127;
    const char *sz_value=runtime_env("PS_PAD_CENTER","");
    if (*sz_value && sscanf(sz_value,"%d,%d,%d,%d",&s_cal_manual_center[0],&s_cal_manual_center[1],
                          &s_cal_manual_center[2],&s_cal_manual_center[3])==4) {
        s_cal_manual=1;
        printf("Runtime: pad center (PS_PAD_CENTER): lx=%d ly=%d rx=%d ry=%d\n",
               s_cal_manual_center[0],s_cal_manual_center[1],s_cal_manual_center[2],s_cal_manual_center[3]);
    }
}
static void cal_set_base(void) {
    int biased=0;
    for (int i=0;i<4;++i) {
        const int a=abs(s_cal_center[i]);
        s_cal_base[i]=a>=PAD_CAL_BIAS ? a : 0;   /* a biased axis spans one half of the int16 range */
        s_cal_max[i][0]=s_cal_max[i][1]=0;
        biased|=s_cal_base[i]!=0;
    }
    if (biased)
        printf("Runtime: pad travel (biased neutral, per-axis scaling): lx=%d ly=%d rx=%d ry=%d\n",
               s_cal_base[0]?s_cal_base[0]:32768,s_cal_base[1]?s_cal_base[1]:32768,
               s_cal_base[2]?s_cal_base[2]:32768,s_cal_base[3]?s_cal_base[3]:32768);
}
/* Full-scale travel of one direction. A biased axis starts from its neutral's magnitude and is
 * refined by the largest push seen (once that push is close to the base travel), so a direction
 * whose physical travel is a few percent shorter still reaches full deflection. */
static int cal_travel(int axis,int dir) {
    const int base=s_cal_base[axis] ? s_cal_base[axis] : 32768;
    return s_cal_base[axis] && s_cal_max[axis][dir]*100>=base*PAD_CAL_REFINE ? s_cal_max[axis][dir] : base;
}
static void cal_reset(void) {
    cal_load();
    s_cal_started=0; s_c_cal_quiet=0; s_cal_since=0; memset(s_cal_prev,0,sizeof s_cal_prev);
    if (s_cal_manual) { memcpy(s_cal_center,s_cal_manual_center,sizeof s_cal_center); s_cal_have_center=1; }
    else if (!s_is_cal_enabled) { memset(s_cal_center,0,sizeof s_cal_center); s_cal_have_center=1; }
    else { s_cal_have_center=0; memset(s_cal_center,0,sizeof s_cal_center); memset(s_cal_base,0,sizeof s_cal_base); }
    if (s_cal_have_center) cal_set_base();
}
/* The value each axis holds while the sticks are untouched (SDL's pre-report zeros are skipped);
 * once it is known, the largest push seen per direction on a biased axis (see cal_travel). */
static void cal_sample(const int16_t raw[4]) {
    if (s_cal_have_center) {
        for (int i=0;i<4;++i) {
            if (!s_cal_base[i]) continue;
            const int d=raw[i]-s_cal_center[i], dir=d>=0, travel=dir ? d : -d;
            if (travel>s_cal_max[i][dir]) s_cal_max[i][dir]=travel;
        }
        return;
    }
    int allzero=1; for (int i=0;i<4;++i) if (raw[i]) allzero=0;
    int is_quiet=1; for (int i=0;i<4;++i) if (s_cal_started && abs(raw[i]-s_cal_prev[i])>PAD_CAL_QUIET) is_quiet=0;
    const uint64_t now=now_us();
    if (!s_cal_started) { s_cal_started=1; s_cal_since=now; }
    if (allzero && now-s_cal_since<PAD_CAL_TIMEOUT_US) is_quiet=0;
    if (is_quiet) {
        if (++s_c_cal_quiet>=PAD_CAL_QUIET_SAMPLES) {
            /* Only the clone pattern (all four axes biased) is taken as the neutral: a genuine pad
             * opened with a stick held steady would otherwise keep that push as its neutral. */
            int biased=0;
            for (int i=0;i<4;++i) biased+=abs(raw[i])>=PAD_CAL_BIAS;
            for (int i=0;i<4;++i) s_cal_center[i]=biased==4 ? raw[i] : 0;
            s_cal_have_center=1;
            if (biased==4)
                printf("Runtime: pad neutral: lx=%d ly=%d rx=%d ry=%d\n",s_cal_center[0],s_cal_center[1],s_cal_center[2],s_cal_center[3]);
            else if (biased)
                printf("Runtime: pad neutral 0 (%d of 4 axes off-center at open: a stick held, not a biased pad)\n",biased);
            cal_set_base();
        }
    } else s_c_cal_quiet=0;
    for (int i=0;i<4;++i) s_cal_prev[i]=raw[i];
}
/* One stick axis in the PS4 0..255 scale. The neutral is subtracted and the axis' own travel
 * (cal_travel) is read as -128..127, so a full push reaches full deflection in both directions
 * whatever part of SDL's range the pad uses; the inner/outer dead zone (PS_PAD_DEADZONE /
 * PS_PAD_DEADZONE_OUTER) then maps that travel to the full deflection, as shadPS4 does. */
static uint8_t stick_axis(int axis,int16_t raw) {
    const int d=raw-s_cal_center[axis];
    const int travel=cal_travel(axis,d>=0 ? 1 : 0);
    int v=travel>0 ? (int)((long long)d*128/travel) : 0;
    if (v>127) v=127; else if (v<-128) v=-128;
    const int mag=abs(v);
    if (mag<=s_pad_deadzone || s_pad_deadzone>=s_pad_deadzone_outer) v=0;
    else {
        int scaled=(int)(128.0*(mag-s_pad_deadzone)/(float)(s_pad_deadzone_outer-s_pad_deadzone));
        if (scaled>128) scaled=128;
        v = v>=0 ? scaled : -scaled;
    }
    int x=v+128;
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *p_pad_data, int right) {
    p_pad_data->buttons|=BTN_TOUCHPAD;
    p_pad_data->touch_count=1;
    p_pad_data->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* PS_GAMEPAD (the launcher's controller choice): its SDL GUID, or part of its name. Issue #15:
 * wheels and other controllers connected for good came first. */
static const char *preferred_gamepad(void) {
    static const char *s_sz_want; static int s_read;
    if (!s_read) { s_sz_want=runtime_env("PS_GAMEPAD",""); if (!*s_sz_want) s_sz_want=NULL; s_read=1; }
    return s_sz_want;
}
static int is_preferred(SDL_JoystickID id, const char *sz_want) {
    char sz_guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id),sz_guid,sizeof sz_guid);
    const char *sz_name=SDL_GetGamepadNameForID(id);
    return !strcasecmp(sz_guid,sz_want) || (sz_name && strcasestr(sz_name,sz_want));
}
/* The chosen gamepad, else the first while it is not connected (checked again every second, it
 * is taken as soon as it connects); called under lock. PS_PAD_HOST=0 (scripted runs) opens none. */
static SDL_Gamepad *current_gamepad(void) {
    static int s_is_on_preferred; static uint64_t s_last_scan;
    if (!g_runtime_pad_sdl_ready) {
        g_runtime_pad_sdl_ready = !runtime_env_flag("PS_PAD_HOST",1) ? -1 : SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    }
    if (g_runtime_pad_sdl_ready<0) return NULL;
    if (s_p_gamepad && !SDL_GamepadConnected(s_p_gamepad)) { SDL_CloseGamepad(s_p_gamepad); s_p_gamepad=NULL; }
    const char *sz_want=preferred_gamepad();
    const uint64_t now=now_us();
    if (!s_p_gamepad || (sz_want && !s_is_on_preferred && now-s_last_scan>1000000)) {
        s_last_scan=now;
        int c_gamepads=0, pick=-1;
        SDL_JoystickID *p_ids=SDL_GetGamepads(&c_gamepads);
        for (int i=0; sz_want && p_ids && i<c_gamepads && pick<0; ++i) if (is_preferred(p_ids[i],sz_want)) pick=i;
        if (pick<0 && !s_p_gamepad && p_ids && c_gamepads>0) pick=0;
        if (pick>=0 && (!s_p_gamepad || SDL_GetGamepadID(s_p_gamepad)!=p_ids[pick])) {
            if (s_p_gamepad) SDL_CloseGamepad(s_p_gamepad);
            s_p_gamepad=SDL_OpenGamepad(p_ids[pick]);
            cal_reset(); /* a new pad has its own neutral */
            s_is_on_preferred=sz_want && s_p_gamepad && is_preferred(p_ids[pick],sz_want);
            if (s_p_gamepad) {
                ++s_c_connecteds;
                printf("Runtime: gamepad connected: %s%s\n",SDL_GetGamepadName(s_p_gamepad),
                       !sz_want ? "" : s_is_on_preferred ? " (the chosen one)" : " (the chosen one is not connected)");
            }
        }
        SDL_free(p_ids);
    }
    return s_p_gamepad;
}
/* psport (frame stats): the game's libc heap, read every 5 s from a game thread (it reads the pad)
 * with libc.prx's malloc_stats (export stub at libc.prx+0x1f8c0; malloc_stats_fast returns 1 in this libc, the module at image +0x56e0000):
 * in use now and at most, and what it took from the system. A heap that keeps growing is a leak. */
typedef struct { uint16_t size, version; uint32_t reserved; uint64_t max_system, system, max_in_use, in_use; } MallocManagedSize;
static void report_guest_heap(void) {
    static int s_is_enabled=-1; static uint64_t s_last;
    if (s_is_enabled<0) s_is_enabled=runtime_env("PS_FRAME_STATS",NULL)!=NULL;
    const uint64_t now=now_us();
    if (!s_is_enabled || now-s_last<5000000) return;
    s_last=now;
    const uint8_t *p_stub=(const uint8_t *)(0x800000000ull+0x56e0000+0x1f8c0);
    /* Read without touching it first: in another title nothing need be mapped there. */
    uint8_t head[2];
    if (ps_host_read_memory(head,p_stub,sizeof(head))!=(int64_t)sizeof(head)) return;
    if (head[0]!=0xff || head[1]!=0x25) return; /* another libc */
    ABI int (*pfn_stats)(MallocManagedSize *)=(ABI int (*)(MallocManagedSize *))(uintptr_t)p_stub;
    MallocManagedSize m={.size=sizeof(m),.version=1};
    const int result=pfn_stats(&m);
    if (result!=0) { static int s_told; if (!s_told++) printf("Guest heap: malloc_stats returned %#x\n",(unsigned)result); return; }
    {
        printf("Guest heap: %.1f MB in use (most %.1f), %.1f MB from the system (most %.1f)\n",
               m.in_use/1048576.0,m.max_in_use/1048576.0,m.system/1048576.0,m.max_system/1048576.0);
    }
}
/* Controls: what each PS4 input is bound to. Defaults below, then the title's own lines; psport.ini
 * (PS_CONFIG) lines key.<input>=<SDL key names> and pad.<input>=<SDL gamepad button names>,
 * comma-separated, replace an input's binding (empty: unbound); a key may be "<modifier>+<key>".
 * Inputs: the buttons (cross ... right, touchpad = a left-side click, touchpad_right), and on the
 * keyboard the sticks: move_* (left), look_* (right). Gamepad names as SDL's: a b x y back start leftstick rightstick leftshoulder
 * rightshoulder dpup dpdown dpleft dpright touchpad misc1 paddle1-4, plus lefttrigger and
 * righttrigger. The keyboard works next to a gamepad (the Steam Deck always has one): its buttons
 * add to the gamepad's, a held move/look key moves the stick all the way. */
enum {
    IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE, IN_L1, IN_R1, IN_L2, IN_R2, IN_L3, IN_R3,
    IN_OPTIONS, IN_TOUCHPAD, IN_TOUCHPAD_RIGHT, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_MOVE_UP, IN_MOVE_DOWN, IN_MOVE_LEFT, IN_MOVE_RIGHT, IN_LOOK_UP, IN_LOOK_DOWN, IN_LOOK_LEFT,
    IN_LOOK_RIGHT, IN_COUNT
};
static const char *const s_input_names[IN_COUNT]={
    "cross","circle","square","triangle","l1","r1","l2","r2","l3","r3","options","touchpad",
    "touchpad_right","up","down","left","right","move_up","move_down","move_left","move_right",
    "look_up","look_down","look_left","look_right",
};
static const uint32_t s_input_buttons[IN_COUNT]={
    BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE,BTN_L1,BTN_R1,BTN_L2,BTN_R2,BTN_L3,BTN_R3,
    BTN_OPTIONS,BTN_TOUCHPAD,0,BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT,
};
#define MAX_BIND 4
/* Mouse buttons and the wheel in key.<input> lines ("Mouse Left", "Wheel Down", ...): codes past
 * SDL's scancodes. They count only while the window holds the mouse for looking (gpu/shim
 * window.cpp): a click that takes the mouse is not an attack. */
enum { MOUSE_KEY=SDL_SCANCODE_COUNT, MOUSE_WHEEL_UP=MOUSE_KEY+SDL_BUTTON_X2+1, MOUSE_WHEEL_DOWN };
static const struct { const char *sz_name; int code; } s_mouse_keys[]={
    {"Mouse Left",MOUSE_KEY+SDL_BUTTON_LEFT}, {"Mouse Right",MOUSE_KEY+SDL_BUTTON_RIGHT},
    {"Mouse Middle",MOUSE_KEY+SDL_BUTTON_MIDDLE}, {"Mouse X1",MOUSE_KEY+SDL_BUTTON_X1},
    {"Mouse X2",MOUSE_KEY+SDL_BUTTON_X2}, {"Wheel Up",MOUSE_WHEEL_UP}, {"Wheel Down",MOUSE_WHEEL_DOWN},
};
enum { PAD_LEFT_TRIGGER=SDL_GAMEPAD_BUTTON_COUNT, PAD_RIGHT_TRIGGER }; /* triggers as buttons */
/* keys[i] needs mods[i] held too when mods[i] is not -1 (a chord: "Left Shift+Mouse Left"). */
typedef struct { int c_keys, c_pads; int keys[MAX_BIND]; int mods[MAX_BIND]; int pad[MAX_BIND]; } Binding;
static Binding s_bindings[IN_COUNT];
static int s_is_bindings_ready;

static void bind_defaults(void) {
    static const struct { int input; int key; } s_keys[]={
        {IN_CROSS,SDL_SCANCODE_SPACE}, {IN_CIRCLE,SDL_SCANCODE_LSHIFT}, {IN_SQUARE,SDL_SCANCODE_E},
        {IN_TRIANGLE,SDL_SCANCODE_Q}, {IN_L1,SDL_SCANCODE_1}, {IN_R1,SDL_SCANCODE_3},
        {IN_L2,SDL_SCANCODE_R}, {IN_R2,SDL_SCANCODE_F}, {IN_L3,SDL_SCANCODE_Z}, {IN_R3,SDL_SCANCODE_C},
        {IN_OPTIONS,SDL_SCANCODE_RETURN}, {IN_TOUCHPAD,SDL_SCANCODE_TAB},
        {IN_TOUCHPAD_RIGHT,SDL_SCANCODE_BACKSPACE},
        {IN_UP,SDL_SCANCODE_I}, {IN_DOWN,SDL_SCANCODE_K}, {IN_LEFT,SDL_SCANCODE_J}, {IN_RIGHT,SDL_SCANCODE_L},
        {IN_MOVE_UP,SDL_SCANCODE_W}, {IN_MOVE_DOWN,SDL_SCANCODE_S}, {IN_MOVE_LEFT,SDL_SCANCODE_A},
        {IN_MOVE_RIGHT,SDL_SCANCODE_D}, {IN_LOOK_UP,SDL_SCANCODE_UP}, {IN_LOOK_DOWN,SDL_SCANCODE_DOWN},
        {IN_LOOK_LEFT,SDL_SCANCODE_LEFT}, {IN_LOOK_RIGHT,SDL_SCANCODE_RIGHT},
        {IN_R1,MOUSE_KEY+SDL_BUTTON_LEFT}, {IN_L2,MOUSE_KEY+SDL_BUTTON_RIGHT},
        {IN_R3,MOUSE_KEY+SDL_BUTTON_MIDDLE}, {IN_R2,MOUSE_KEY+SDL_BUTTON_X1}, {IN_L1,MOUSE_KEY+SDL_BUTTON_X2},
        {IN_UP,MOUSE_WHEEL_UP}, {IN_DOWN,MOUSE_WHEEL_DOWN},
    };
    static const struct { int input, button; } s_pads[]={
        {IN_CROSS,SDL_GAMEPAD_BUTTON_SOUTH}, {IN_CIRCLE,SDL_GAMEPAD_BUTTON_EAST},
        {IN_SQUARE,SDL_GAMEPAD_BUTTON_WEST}, {IN_TRIANGLE,SDL_GAMEPAD_BUTTON_NORTH},
        {IN_L1,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {IN_R1,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {IN_L2,PAD_LEFT_TRIGGER}, {IN_R2,PAD_RIGHT_TRIGGER},
        {IN_L3,SDL_GAMEPAD_BUTTON_LEFT_STICK}, {IN_R3,SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {IN_OPTIONS,SDL_GAMEPAD_BUTTON_START},
        {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_BACK}, {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_TOUCHPAD},
        {IN_UP,SDL_GAMEPAD_BUTTON_DPAD_UP}, {IN_DOWN,SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {IN_LEFT,SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {IN_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    memset(s_bindings,0,sizeof s_bindings);
    for (size_t i=0;i<sizeof(s_keys)/sizeof(*s_keys);++i) {
        Binding *p_binding=&s_bindings[s_keys[i].input];
        p_binding->mods[p_binding->c_keys]=-1; p_binding->keys[p_binding->c_keys++]=s_keys[i].key;
    }
    for (size_t i=0;i<sizeof(s_pads)/sizeof(*s_pads);++i) {
        Binding *p_binding=&s_bindings[s_pads[i].input]; p_binding->pad[p_binding->c_pads++]=s_pads[i].button;
    }
}
static int pad_button_from_name(const char *sz_name) {
    if (!SDL_strcasecmp(sz_name,"lefttrigger")) return PAD_LEFT_TRIGGER;
    if (!SDL_strcasecmp(sz_name,"righttrigger")) return PAD_RIGHT_TRIGGER;
    const SDL_GamepadButton b=SDL_GetGamepadButtonFromString(sz_name);
    return b==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
}
static int key_from_name(const char *sz_name) {
    for (size_t i=0;i<sizeof(s_mouse_keys)/sizeof(*s_mouse_keys);++i)
        if (!SDL_strcasecmp(sz_name,s_mouse_keys[i].sz_name)) return s_mouse_keys[i].code;
    const SDL_Scancode s=SDL_GetScancodeFromName(sz_name);
    return s==SDL_SCANCODE_UNKNOWN ? -1 : (int)s;
}
/* Mouse look (psport.ini): mouse_look=0 leaves the mouse alone; mouse_sensitivity (default 1:
 * ~1000 pixels/s turn the camera at full speed), mouse_invert_y=1. */
static int s_mouse_look=1, s_mouse_invert_y;
static float s_mouse_sensitivity=1.0f;
/* A key.<input>= or pad.<input>= line (sz_origin: where it comes from, for the log); 0 when it is
 * not one. */
static int apply_binding_line(const char *sz_key, const char *sz_value, const char *sz_origin) {
    const int keyboard=!strncmp(sz_key,"key.",4), pad=!strncmp(sz_key,"pad.",4);
    if (!keyboard && !pad) return 0;
    int input=-1;
    for (int i=0;i<IN_COUNT;++i) if (!strcmp(sz_key+4,s_input_names[i])) input=i;
    if (input<0 || (pad && input>=IN_MOVE_UP)) { printf("Runtime: controls: unknown input %s (%s)\n",sz_key,sz_origin); return 1; }
    Binding *p_binding=&s_bindings[input];
    if (keyboard) p_binding->c_keys=0; else p_binding->c_pads=0;
    char sz_names[512];
    snprintf(sz_names,sizeof sz_names,"%s",sz_value);
    char *p_save=NULL;
    for (char *sz_name=strtok_r(sz_names,",",&p_save); sz_name; sz_name=strtok_r(NULL,",",&p_save)) {
        while (*sz_name==' ') ++sz_name;
        for (char *sz_end=sz_name+strlen(sz_name); sz_end>sz_name && sz_end[-1]==' ';) *--sz_end=0;
        if (!*sz_name) continue;
        if (keyboard) {
            /* A chord: "<modifier>+<key>" ("Keypad +" is one key: nothing after its '+'). */
            int mod=-1;
            char *p_plus=strrchr(sz_name,'+');
            if (p_plus && p_plus>sz_name && p_plus[1]) {
                *p_plus=0;
                for (char *sz_end=p_plus; sz_end>sz_name && sz_end[-1]==' ';) *--sz_end=0;
                mod=key_from_name(sz_name);
                if (mod<0) *p_plus='+'; else sz_name=p_plus+1;
                while (*sz_name==' ') ++sz_name;
            }
            const int s=key_from_name(sz_name);
            if (s<0) printf("Runtime: controls: unknown key \"%s\" for %s (%s)\n",sz_name,sz_key+4,sz_origin);
            else if (p_binding->c_keys<MAX_BIND) { p_binding->mods[p_binding->c_keys]=mod; p_binding->keys[p_binding->c_keys++]=s; }
        } else {
            const int button=pad_button_from_name(sz_name);
            if (button<0) printf("Runtime: controls: unknown gamepad button \"%s\" for %s (%s)\n",sz_name,sz_key+4,sz_origin);
            else if (p_binding->c_pads<MAX_BIND) p_binding->pad[p_binding->c_pads++]=button;
        }
    }
    return 1;
}
/* The running title's default controls (psport.ini lines, one per line), over the neutral ones. */
static void apply_title_controls(void) {
    const char *sz_controls=psport_gpu_title_controls();
    if (!sz_controls) return;
    int c_lines=0;
    for (const char *sz_line=sz_controls; *sz_line;) {
        const char *sz_end=strchr(sz_line,'\n');
        const size_t cb_line=sz_end ? (size_t)(sz_end-sz_line) : strlen(sz_line);
        char sz_entry[512];
        snprintf(sz_entry,sizeof sz_entry,"%.*s",(int)cb_line,sz_line);
        char *p_equals=strchr(sz_entry,'=');
        if (p_equals) {
            *p_equals=0;
            c_lines+=apply_binding_line(sz_entry,p_equals+1,"the title's defaults");
        }
        sz_line+=cb_line+(sz_end ? 1 : 0);
    }
    printf("Runtime: controls: the title's default layout (%d bindings), psport.ini's key. and pad. lines over it\n",c_lines);
}
/* key.<input>= / pad.<input>= lines of the settings file. */
static void load_bindings(void) {
    bind_defaults();
    apply_title_controls();
    s_mouse_look=1; s_mouse_invert_y=0; s_mouse_sensitivity=1.0f;
    const char *sz_key, *sz_value;
    for (size_t i_entry=0; runtime_config_entry(i_entry,&sz_key,&sz_value); ++i_entry) {
        if (apply_binding_line(sz_key,sz_value,"psport.ini")) continue;
        if (!strcmp(sz_key,"mouse_look")) s_mouse_look=atoi(sz_value)!=0;
        else if (!strcmp(sz_key,"mouse_invert_y")) s_mouse_invert_y=atoi(sz_value)!=0;
        else if (!strcmp(sz_key,"mouse_sensitivity")) {
            const float v=strtof(sz_value,NULL);
            if (v>=0.05f && v<=20.0f) s_mouse_sensitivity=v;
        }
    }
}
/* The mouse while the window holds it: buttons, and the wheel as short presses (one per notch,
 * each held and released for a few frames so the game sees it). */
static SDL_MouseButtonFlags s_mouse_buttons;
static int s_wheel_pending[2], s_wheel_phase[2]; /* [up, down]; phase: 0 idle, else press/release */
static uint64_t s_wheel_until[2];
static void mouse_wheel_step(uint64_t now, int up, int is_down) {
    s_wheel_pending[0]+=up; s_wheel_pending[1]+=is_down;
    for (int i=0;i<2;++i) {
        if (s_wheel_pending[i]>8) s_wheel_pending[i]=8;
        if (s_wheel_phase[i] && now<s_wheel_until[i]) continue;
        if (s_wheel_phase[i]==1) { s_wheel_phase[i]=2; s_wheel_until[i]=now+60000; } /* released */
        else if (s_wheel_pending[i]>0) { --s_wheel_pending[i]; s_wheel_phase[i]=1; s_wheel_until[i]=now+60000; }
        else s_wheel_phase[i]=0;
    }
}
static int mouse_key_down(int code) {
    if (code==MOUSE_WHEEL_UP) return s_wheel_phase[0]==1;
    if (code==MOUSE_WHEEL_DOWN) return s_wheel_phase[1]==1;
    return (s_mouse_buttons & SDL_BUTTON_MASK(code-MOUSE_KEY))!=0;
}
static int code_down(const bool *p_is_key_down, int code) {
    return code>=MOUSE_KEY ? mouse_key_down(code) : p_is_key_down && p_is_key_down[code];
}
/* Whether a chord on code (any input's) has its modifier held: the key's plain bindings then wait
 * (Shift+Left is R2 alone). */
static int is_chord_held(const bool *p_is_key_down, int code) {
    for (int input=0;input<IN_COUNT;++input)
        for (int i=0;i<s_bindings[input].c_keys;++i)
            if (s_bindings[input].keys[i]==code && s_bindings[input].mods[i]>=0 &&
                code_down(p_is_key_down,s_bindings[input].mods[i])) return 1;
    return 0;
}
static int key_down(const bool *p__bool, int input) {
    for (int i=0;i<s_bindings[input].c_keys;++i) {
        const int code=s_bindings[input].keys[i], mod=s_bindings[input].mods[i];
        if (!code_down(p__bool,code)) continue;
        if (mod>=0 ? code_down(p__bool,mod) : !is_chord_held(p__bool,code)) return 1;
    }
    return 0;
}
/* The bound gamepad buttons' state; triggers as their analog value. */
static int pad_value(SDL_Gamepad *p_sdl_gamepad, int input) {
    int value=0;
    for (int i=0;i<s_bindings[input].c_pads;++i) {
        const int b=s_bindings[input].pad[i];
        const int v=b==PAD_LEFT_TRIGGER ? trigger(SDL_GetGamepadAxis(p_sdl_gamepad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER))
                  : b==PAD_RIGHT_TRIGGER ? trigger(SDL_GetGamepadAxis(p_sdl_gamepad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                  : SDL_GetGamepadButton(p_sdl_gamepad,(SDL_GamepadButton)b) ? 255 : 0;
        if (v>value) value=v;
    }
    return value;
}

/* key held for the negative / positive direction: the stick all the way, else the gamepad's. */
static uint8_t key_axis(uint8_t value, int negative, int positive) {
    return negative || positive ? (uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0)) : value;
}
static void apply_keyboard(PadData *p_pad_data, const bool *p__bool) {
    for (int i=IN_CROSS;i<=IN_RIGHT;++i)
        if (i!=IN_TOUCHPAD && i!=IN_TOUCHPAD_RIGHT && key_down(p__bool,i)) p_pad_data->buttons|=s_input_buttons[i];
    if (key_down(p__bool,IN_TOUCHPAD)) touch_click(p_pad_data,0);
    if (key_down(p__bool,IN_TOUCHPAD_RIGHT)) touch_click(p_pad_data,1);
    if (key_down(p__bool,IN_L2)) p_pad_data->l2=255;
    if (key_down(p__bool,IN_R2)) p_pad_data->r2=255;
    p_pad_data->left_x=key_axis(p_pad_data->left_x,key_down(p__bool,IN_MOVE_LEFT),key_down(p__bool,IN_MOVE_RIGHT));
    p_pad_data->left_y=key_axis(p_pad_data->left_y,key_down(p__bool,IN_MOVE_UP),key_down(p__bool,IN_MOVE_DOWN));
    p_pad_data->right_x=key_axis(p_pad_data->right_x,key_down(p__bool,IN_LOOK_LEFT),key_down(p__bool,IN_LOOK_RIGHT));
    p_pad_data->right_y=key_axis(p_pad_data->right_y,key_down(p__bool,IN_LOOK_UP),key_down(p__bool,IN_LOOK_DOWN));
}

/* Mouse look: the game turns its camera at a rate set by the right stick, so the stick follows
 * the mouse's speed. Each sample adds the motion since the last one and the sum decays over
 * LOOK_TAU (linearly per sample: the level a steady speed holds, speed * gain * LOOK_TAU, is the
 * same at any frame rate). ~1000 pixels/s reach full deflection at sensitivity 1. The game ignores
 * small deflections, so a moving mouse starts past them (PS_MOUSE_DEADZONE, default 16 of 127):
 * slow aiming still turns the camera. While the mouse moves it replaces the right stick. */
#define LOOK_TAU_US 60000.0f
static float s_look_x, s_look_y;
static uint64_t s_look_last_us;
static uint8_t look_axis(float value, uint8_t stick, int deadzone) {
    if (value>-0.5f && value<0.5f) return stick;
    const float magnitude=value<0 ? -value : value;
    const float out=deadzone+magnitude*(127-deadzone)/127.0f;
    const int v=128+(int)(value<0 ? -out : out);
    return (uint8_t)(v<0 ? 0 : v>255 ? 255 : v);
}
/* Native mouse look (the game profile's camera hook, psport_gpu_camera_look): the camera's own update
 * takes the turn, as far as the mouse moved: MOUSE_RAD_PER_PIXEL at sensitivity 1 (1000 pixels: 1.5
 * rad, 86 degrees). The stick below only without that hook or while the camera does not take it. */
#define MOUSE_RAD_PER_PIXEL 0.0015f
static void apply_mouse_look(PadData *p_pad_data, double dx, double dy, uint64_t now, int is_captured) {
    if (is_captured && (dx!=0 || dy!=0)) {
        const float k=MOUSE_RAD_PER_PIXEL*s_mouse_sensitivity;
        /* The camera's pitch grows as it rises (looking down): the mouse moving down (dy > 0) looks
           down, as the stick pushed down does. */
        if (psport_gpu_camera_look((float)(s_mouse_invert_y ? -dy : dy)*k,(float)dx*k)) dx=dy=0;
    }
    float dt=s_look_last_us && now>s_look_last_us ? (float)(now-s_look_last_us) : 0.0f;
    s_look_last_us=now;
    if (!is_captured) { s_look_x=s_look_y=0; return; }
    if (dt>LOOK_TAU_US) dt=LOOK_TAU_US;
    static int s_deadzone=-1;
    if (s_deadzone<0) {
        s_deadzone=runtime_env_int("PS_MOUSE_DEADZONE",16);
        if (s_deadzone<0 || s_deadzone>100) s_deadzone=16;
    }
    const float gain=127.0f/(1000.0f*LOOK_TAU_US/1e6f)*s_mouse_sensitivity, decay=1.0f-dt/LOOK_TAU_US;
    s_look_x=s_look_x*decay+(float)dx*gain;
    s_look_y=s_look_y*decay+(float)(s_mouse_invert_y ? -dy : dy)*gain;
    s_look_x=s_look_x<-127 ? -127 : s_look_x>127 ? 127 : s_look_x;
    s_look_y=s_look_y<-127 ? -127 : s_look_y>127 ? 127 : s_look_y;
    p_pad_data->right_x=look_axis(s_look_x,p_pad_data->right_x,s_deadzone);
    p_pad_data->right_y=look_axis(s_look_y,p_pad_data->right_y,s_deadzone);
}

/* Button prompts: the device whose input came last. RUNTIME_PAD_PROMPT_PLAYSTATION a PlayStation pad
 * (the game's own icons), _GAMEPAD another gamepad (its labels), _KEYBOARD the keyboard and mouse.
 * The generation changes with the device, the pad's type and the bindings. */
static int s_prompt_device=-1;
static SDL_GamepadType s_prompt_pad_type=SDL_GAMEPAD_TYPE_UNKNOWN;
static unsigned s_prompt_generation=1;
static void prompt_note(SDL_Gamepad *p_sdl_gamepad, int is_pad_active, int is_keyboard_active) {
    int device=s_prompt_device;
    const SDL_GamepadType type=p_sdl_gamepad ? SDL_GetGamepadType(p_sdl_gamepad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    const int is_playstation=type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 || type==SDL_GAMEPAD_TYPE_PS5;
    if (is_keyboard_active) device=RUNTIME_PAD_PROMPT_KEYBOARD;
    else if (p_sdl_gamepad && (is_pad_active || device<0))
        device=is_playstation ? RUNTIME_PAD_PROMPT_PLAYSTATION : RUNTIME_PAD_PROMPT_GAMEPAD;
    else if (device<0) device=RUNTIME_PAD_PROMPT_KEYBOARD;
    if (device!=s_prompt_device || (device!=RUNTIME_PAD_PROMPT_KEYBOARD && type!=s_prompt_pad_type)) {
        s_prompt_device=device;
        if (device!=RUNTIME_PAD_PROMPT_KEYBOARD) s_prompt_pad_type=type;
        __atomic_add_fetch(&s_prompt_generation,1,__ATOMIC_RELAXED);
    }
}
static int is_stick_moved(uint8_t value) { return value<128-48 || value>128+48; }
int runtime_pad_prompt_device(void) {
    return __atomic_load_n(&s_prompt_device,__ATOMIC_RELAXED);
}
unsigned runtime_pad_prompt_generation(void) {
    return __atomic_load_n(&s_prompt_generation,__ATOMIC_RELAXED);
}
int runtime_pad_mouse_look(void) {
    return __atomic_load_n(&s_mouse_look,__ATOMIC_RELAXED);
}
/* A key's name for a prompt: short (a prompt is a key cap), arrows as arrows. */
static void key_label(int code, char *sz_out, size_t cb_out) {
    static const struct { const char *sz_sdl, *sz_label; } s_names[]={
        {"Left Shift","Shift"}, {"Right Shift","RShift"}, {"Left Ctrl","Ctrl"}, {"Right Ctrl","RCtrl"},
        {"Left Alt","Alt"}, {"Right Alt","AltGr"}, {"Return","Enter"}, {"Keypad Enter","Enter"},
        {"Backspace","Bksp"}, {"Escape","Esc"}, {"CapsLock","Caps"}, {"Delete","Del"},
        {"Insert","Ins"}, {"PageUp","PgUp"}, {"PageDown","PgDn"}, {"Up","\xe2\x86\x91"},
        {"Down","\xe2\x86\x93"}, {"Left","\xe2\x86\x90"}, {"Right","\xe2\x86\x92"},
    };
    const char *sz_name=SDL_GetScancodeName((SDL_Scancode)code);
    if (!sz_name || !*sz_name) sz_name="?";
    for (size_t i=0;i<sizeof(s_names)/sizeof(*s_names);++i)
        if (!strcmp(sz_name,s_names[i].sz_sdl)) { sz_name=s_names[i].sz_label; break; }
    if (!strncmp(sz_name,"Keypad ",7)) { snprintf(sz_out,cb_out,"Num%s",sz_name+7); return; }
    snprintf(sz_out,cb_out,"%s",sz_name);
}
int runtime_pad_prompt(const char *sz_input, int should_prefer_mouse, char *sz_out_label, size_t cb_label) {
    if (!sz_input || !sz_out_label || cb_label<2) return 0;
    sz_out_label[0]=0;
    int input=-1;
    for (int i=0;i<IN_COUNT;++i) if (!strcmp(sz_input,s_input_names[i])) input=i;
    if (input<0) return 0;
    pthread_mutex_lock(&s_lock);
    if (!s_is_bindings_ready) { load_bindings(); s_is_bindings_ready=1; }
    const int device=s_prompt_device;
    const Binding binding=s_bindings[input];
    const int is_look=s_mouse_look;
    const SDL_GamepadType type=s_prompt_pad_type;
    pthread_mutex_unlock(&s_lock);
    if (device==RUNTIME_PAD_PROMPT_KEYBOARD) {
        int key=-1, mouse=-1, key_mod=-1, mouse_mod=-1;
        for (int i=0;i<binding.c_keys;++i) {
            if (binding.keys[i]>=MOUSE_KEY) { if (mouse<0) { mouse=binding.keys[i]; mouse_mod=binding.mods[i]; } }
            else if (key<0) { key=binding.keys[i]; key_mod=binding.mods[i]; }
        }
        /* With mouse look the hand is on the mouse: its button first (R1: Left), unless a key cluster
           is drawn (should_prefer_mouse 0: the d-pad's keys, not the wheel). */
        const int is_mouse=mouse>=0 && ((is_look && should_prefer_mouse) || key<0);
        /* A chord's modifier as a prefix: Shift as its arrow (R2: "\xe2\x87\xa7Left"). */
        char sz_prefix[24]="";
        const int mod=is_mouse ? mouse_mod : key_mod;
        if (mod==SDL_SCANCODE_LSHIFT || mod==SDL_SCANCODE_RSHIFT) snprintf(sz_prefix,sizeof sz_prefix,"\xe2\x87\xa7");
        else if (mod>=0) {
            char sz_mod[16];
            key_label(mod,sz_mod,sizeof sz_mod);
            snprintf(sz_prefix,sizeof sz_prefix,"%s+",sz_mod);
        }
        if (is_mouse) {
            for (size_t i=0;i<sizeof(s_mouse_keys)/sizeof(*s_mouse_keys);++i)
                if (s_mouse_keys[i].code==mouse) {
                    const char *sz_name=s_mouse_keys[i].sz_name;
                    snprintf(sz_out_label,cb_label,"%s%s",sz_prefix,strncmp(sz_name,"Mouse ",6) ? sz_name : sz_name+6);
                }
            return 'M';
        }
        if (key<0) return 0;
        char sz_name[32];
        key_label(key,sz_name,sizeof sz_name);
        snprintf(sz_out_label,cb_label,"%s%s",sz_prefix,sz_name);
        return 'K';
    }
    if (device!=RUNTIME_PAD_PROMPT_GAMEPAD) return 0; /* a PlayStation pad: the game's own icons */
    const int is_nintendo=type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
                          type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                          type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
                          type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    if (input>=IN_MOVE_UP) { snprintf(sz_out_label,cb_label,"%s",input<IN_LOOK_UP ? "LS" : "RS"); return 'C'; }
    if (binding.c_pads==0) return 0;
    const int button=binding.pad[0];
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH: {
        static const char *const s_by_position[]={"A","B","X","Y"};
        const SDL_GamepadButtonLabel label=SDL_GetGamepadButtonLabelForType(type,(SDL_GamepadButton)button);
        const char *sz_text=label==SDL_GAMEPAD_BUTTON_LABEL_A ? "A" : label==SDL_GAMEPAD_BUTTON_LABEL_B ? "B"
                          : label==SDL_GAMEPAD_BUTTON_LABEL_X ? "X" : label==SDL_GAMEPAD_BUTTON_LABEL_Y ? "Y"
                          : s_by_position[button-SDL_GAMEPAD_BUTTON_SOUTH];
        snprintf(sz_out_label,cb_label,"%s",sz_text);
        return 'F';
    }
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: snprintf(sz_out_label,cb_label,"%s",is_nintendo ? "L" : "LB"); return 'S';
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: snprintf(sz_out_label,cb_label,"%s",is_nintendo ? "R" : "RB"); return 'S';
    case PAD_LEFT_TRIGGER: snprintf(sz_out_label,cb_label,"%s",is_nintendo ? "ZL" : "LT"); return 'S';
    case PAD_RIGHT_TRIGGER: snprintf(sz_out_label,cb_label,"%s",is_nintendo ? "ZR" : "RT"); return 'S';
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: snprintf(sz_out_label,cb_label,"LS"); return 'C';
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: snprintf(sz_out_label,cb_label,"RS"); return 'C';
    case SDL_GAMEPAD_BUTTON_START: snprintf(sz_out_label,cb_label,"%s",is_nintendo ? "+" : "Menu"); return 'C';
    case SDL_GAMEPAD_BUTTON_BACK: snprintf(sz_out_label,cb_label,"%s",is_nintendo ? "-" : "View"); return 'C';
    case SDL_GAMEPAD_BUTTON_MISC1: snprintf(sz_out_label,cb_label,"Share"); return 'C';
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: snprintf(sz_out_label,cb_label,"Pad"); return 'C';
    default: return 0; /* the d-pad: the game's icons fit any pad */
    }
}

static void sample_host(PadData *p_pad_data) {
    report_guest_heap();
    memset(p_pad_data,0,sizeof(*p_pad_data));
    p_pad_data->left_x=p_pad_data->left_y=p_pad_data->right_x=p_pad_data->right_y=128;
    p_pad_data->orientation[3]=1.0f;
    p_pad_data->acceleration[1]=1.0f; /* 1 g up, the pad lying flat: zeros read as free fall */
    p_pad_data->connected=1; p_pad_data->connected_count=s_c_connecteds ? s_c_connecteds : 1;
    p_pad_data->timestamp=now_us();
    SDL_Gamepad *p_sdl_gamepad=current_gamepad();
    if (!s_is_bindings_ready) { load_bindings(); s_is_bindings_ready=1; psport_gpu_mouse_look_enable(s_mouse_look); }
    /* The mouse's motion and wheel since the last sample (always taken: none of it piles up while
     * the menu is open); it counts while the window holds the mouse. */
    double mouse_dx=0, mouse_dy=0;
    int wheel_up=0, is_wheel_down=0;
    psport_gpu_mouse_take(&mouse_dx,&mouse_dy,&wheel_up,&is_wheel_down);
    const int is_captured=psport_gpu_mouse_captured();
    if (psport_gpu_overlay_captures_input()) { apply_mouse_look(p_pad_data,0,0,p_pad_data->timestamp,0); return; } /* menu open: neutral input */
    const bool *p__bool=SDL_WasInit(SDL_INIT_VIDEO) && !runtime_keyboard_taken() ? SDL_GetKeyboardState(NULL) : NULL;
    s_mouse_buttons=is_captured && SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetMouseState(NULL,NULL) : 0;
    mouse_wheel_step(p_pad_data->timestamp,is_captured ? wheel_up : 0,is_captured ? is_wheel_down : 0);
    if (p_sdl_gamepad) {
        int touch_right=0;
        for (int i=IN_CROSS;i<=IN_RIGHT;++i) {
            const int v=pad_value(p_sdl_gamepad,i);
            if (i==IN_L2) p_pad_data->l2=(uint8_t)v;
            if (i==IN_R2) p_pad_data->r2=(uint8_t)v;
            if (i==IN_TOUCHPAD_RIGHT) touch_right=v>30;
            else if (v>30) p_pad_data->buttons|=s_input_buttons[i];
        }
        static const SDL_GamepadAxis s_stick_axes[4]={SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,
                                                    SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY};
        int16_t raw[4];
        for (int i=0;i<4;++i) raw[i]=(int16_t)SDL_GetGamepadAxis(p_sdl_gamepad,s_stick_axes[i]);
        cal_load();
        cal_sample(raw);
        uint8_t *axis_out[4]={&p_pad_data->left_x,&p_pad_data->left_y,&p_pad_data->right_x,&p_pad_data->right_y};
        for (int i=0;i<4;++i)
            *axis_out[i]=stick_axis(i,raw[i]); /* center 0 until the neutral is known */
        if (SDL_GetNumGamepadTouchpads(p_sdl_gamepad)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(p_sdl_gamepad,0);
            for (int finger=0;finger<fingers && p_pad_data->touch_count<2;++finger) {
                bool is_down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(p_sdl_gamepad,0,finger,&is_down,&x,&y,NULL) && is_down) {
                    p_pad_data->touches[p_pad_data->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((p_pad_data->buttons & BTN_TOUCHPAD) && !p_pad_data->touch_count) touch_click(p_pad_data,0);
        if (touch_right) touch_click(p_pad_data,1);
    }
    const int is_pad_active=p_sdl_gamepad && (p_pad_data->buttons || p_pad_data->l2>30 || p_pad_data->r2>30 ||
                                              p_pad_data->touch_count || is_stick_moved(p_pad_data->left_x) ||
                                              is_stick_moved(p_pad_data->left_y) || is_stick_moved(p_pad_data->right_x) ||
                                              is_stick_moved(p_pad_data->right_y));
    int is_keyboard_active=(is_captured && (mouse_dx*mouse_dx+mouse_dy*mouse_dy>9.0 || wheel_up || is_wheel_down)) ||
                           s_mouse_buttons!=0;
    for (int i=0;i<IN_COUNT && !is_keyboard_active;++i) is_keyboard_active=key_down(p__bool,i);
    prompt_note(p_sdl_gamepad,is_pad_active,is_keyboard_active);
    apply_keyboard(p_pad_data,p__bool); /* the bound mouse buttons too */
    apply_mouse_look(p_pad_data,mouse_dx,mouse_dy,p_pad_data->timestamp,is_captured);
}

/* Touches as the DualShock 4 reports them: every finger that goes down gets a new id (1..127, kept
 * while it stays down) and the time since the first one went down. With id 0 and no hold time
 * the game ignored touchpad presses: no gesture menu from the touchpad, Back or Tab. */
static void touch_ids(PadData *p_pad_data) {
    static uint8_t s_next_id=1, s_ids[2]; static int s_down[2]; static uint64_t s_since;
    for (int i=0;i<2;++i) {
        const int now=i<p_pad_data->touch_count;
        if (now && !s_down[i]) { s_ids[i]=s_next_id; s_next_id=s_next_id==127 ? 1 : s_next_id+1; }
        s_down[i]=now;
        if (now) p_pad_data->touches[i].id=s_ids[i];
    }
    if (!p_pad_data->touch_count) s_since=0;
    else if (!s_since) s_since=p_pad_data->timestamp;
    p_pad_data->touch_held_time=p_pad_data->touch_count ? (uint32_t)(p_pad_data->timestamp-s_since) : 0;
}
static void sample(PadData *p_pad_data) {
    sample_host(p_pad_data);
    runtime_pad_read_inject();
    if (psport_gpu_overlay_captures_input()) return;
    runtime_pad_record_sample(p_pad_data);
    runtime_pad_replay_sample(p_pad_data);
    p_pad_data->buttons|=g_runtime_pad_injected.buttons;
    if (g_runtime_pad_injected.touch_side>=0) touch_click(p_pad_data,g_runtime_pad_injected.touch_side);
    else if ((p_pad_data->buttons & BTN_TOUCHPAD) && !p_pad_data->touch_count) touch_click(p_pad_data,0);
    if (g_runtime_pad_injected.buttons & BTN_L2) p_pad_data->l2=255;
    if (g_runtime_pad_injected.buttons & BTN_R2) p_pad_data->r2=255;
    uint8_t *axes[4]={&p_pad_data->left_x,&p_pad_data->left_y,&p_pad_data->right_x,&p_pad_data->right_y};
    for (int i=0;i<4;++i) if (g_runtime_pad_injected.stick[i]>=0) *axes[i]=(uint8_t)g_runtime_pad_injected.stick[i];
    const uint8_t sticks[4]={p_pad_data->left_x,p_pad_data->left_y,p_pad_data->right_x,p_pad_data->right_y};
    psport_gpu_filter_pad(&p_pad_data->buttons,sticks); /* the game profile's (a menu pointer's presses) */
    touch_ids(p_pad_data);
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&s_lock); s_is_initialized=1; pthread_mutex_unlock(&s_lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t i_device, const void *p_param) {
    (void)p_param;
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (i_device) return ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    int already=s_is_opened; s_is_opened=1;
    pthread_mutex_unlock(&s_lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_get_handle(int32_t user, int32_t type, int32_t i_device) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    return user==1 && (type==0 || type==2) && !i_device && s_is_opened ? PAD_HANDLE : ERR_NO_HANDLE;
}
static ABI int32_t pad_close(int32_t h_handle) {
    if (h_handle!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    s_is_opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t h_handle, PadData *p_data) {
    if (h_handle!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    sample(p_data); ++s_c_reads;
    pthread_mutex_unlock(&s_lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t h_handle, PadData *p_data, int32_t c_data) {
    if (h_handle!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_data || c_data<1 || c_data>64) return ERR_INVALID_ARG;
    pad_read_state(h_handle,p_data);
    return 1;
}
static ABI int32_t pad_info(int32_t h_handle, ControllerInfo *p_info) {
    if (h_handle!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_info) return ERR_INVALID_ARG;
    memset(p_info,0,sizeof(*p_info));
    p_info->pixel_density=44.86f; p_info->resolution_x=1920; p_info->resolution_y=943;
    p_info->dead_zone_left=p_info->dead_zone_right=2;
    p_info->connection_type=0; p_info->connected=1; p_info->device_class=0;
    pthread_mutex_lock(&s_lock);
    current_gamepad();
    p_info->connected_count=s_c_connecteds ? s_c_connecteds : 1;
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t pad_class_info(int32_t h_handle, DeviceClassInfo *p_info) {
    if (h_handle!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_info) return ERR_INVALID_ARG;
    memset(p_info,0,sizeof(*p_info));
    return 0;
}
/* The pad is a standard controller (class 0), which has no class fields to parse out of a state's
 * device unique data: only whether that state came from a connected pad. */
static ABI int32_t pad_class_parse(int32_t h_pad, const PadData *p_data, DeviceClassData *p_out_class_data) {
    if (h_pad!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_data || !p_out_class_data) return ERR_INVALID_ARG;
    memset(p_out_class_data,0,sizeof(*p_out_class_data));
    p_out_class_data->is_data_valid=p_data->connected!=0;
    return 0;
}
static ABI int32_t pad_vibration(int32_t h_handle, const uint8_t *p_param) {
    if (h_handle!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    SDL_Gamepad *p_sdl_gamepad=current_gamepad();
    pthread_mutex_unlock(&s_lock);
    /* Not under the pad's lock: the report can be a synchronous HID write, and reads wait for the
       lock. SDL checks the handle, so one closed meanwhile only fails. */
    if (p_sdl_gamepad) SDL_RumbleGamepad(p_sdl_gamepad,(uint16_t)(p_param[0]*257),(uint16_t)(p_param[1]*257),1000);
    return 0;
}
/* ScePadLightBarParam: red, green, blue, reserved. Shown on the host pad when it has an LED. */
static int set_host_light_bar(uint8_t red, uint8_t green, uint8_t blue) {
    pthread_mutex_lock(&s_lock);
    SDL_Gamepad *p_gamepad=current_gamepad();
    pthread_mutex_unlock(&s_lock);
    return p_gamepad && SDL_SetGamepadLED(p_gamepad,red,green,blue);
}
static ABI int32_t pad_set_light_bar(int32_t h_pad, const uint8_t *p_param) {
    if (h_pad!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_param) return ERR_INVALID_ARG;
    static int s_is_logged;
    const int is_shown=set_host_light_bar(p_param[0],p_param[1],p_param[2]);
    if (!s_is_logged) {
        s_is_logged=1;
        printf("Runtime: pad light bar set to %u,%u,%u (%s)\n",p_param[0],p_param[1],p_param[2],
               is_shown ? "shown on the host pad" : "the host pad has no LED");
    }
    return 0;
}
/* The colour the system gives user 1's pad: blue (sceUserServiceGetUserColor answers 0, blue). */
static ABI int32_t pad_reset_light_bar(int32_t h_pad) {
    if (h_pad!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    set_host_light_bar(0,0,255);
    return 0;
}
/* Remote Play's pad is the only remote one, and this port has no Remote Play. */
static ABI int32_t pad_is_remote(int32_t h_handle, uint8_t *p_remote) {
    if (h_handle!=PAD_HANDLE || !s_is_opened) return ERR_INVALID_HANDLE;
    if (!p_remote) return ERR_INVALID_ARG;
    *p_remote=0;
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t h_handle) { return h_handle==PAD_HANDLE && s_is_opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t h_handle, uint8_t flag) { (void)flag; return pad_ok_handle(h_handle); }

static const RuntimeExport s_exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadGetHandle",pad_get_handle}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadIsRemoteController",pad_is_remote},
    {"scePadDeviceClassGetExtendedInformation",pad_class_info}, {"scePadDeviceClassParseData",pad_class_parse}, {"scePadSetVibration",pad_vibration},
    {"scePadSetLightBar",pad_set_light_bar}, {"scePadResetLightBar",pad_reset_light_bar},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",s_c_reads,s_p_gamepad ? SDL_GetGamepadName(s_p_gamepad) : "none"); }
