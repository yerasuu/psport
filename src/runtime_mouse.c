/* libSceMouse on the host mouse. SDL events are pumped by the window thread (gpu/shim/window.cpp);
 * here they are only collected. Every game that opens a mouse port gets the host mouse: port index
 * 0 (or a merged port) reports it whenever the host has one, the other index never has a mouse.
 * A read returns one entry, the buttons held and the motion, wheel and tilt since the previous read
 * of that handle. Without a window or a host mouse the port is open but reports no device, as a
 * console with no mouse plugged in does. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>

#define ERR_INVALID_ARG ((int32_t)0x80DF0001)
#define ERR_INVALID_HANDLE ((int32_t)0x80DF0003)
#define ERR_ALREADY_OPENED ((int32_t)0x80DF0004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80DF0005)
#define MOUSE_PORT_INDICES 2
#define MOUSE_HANDLES 8
#define MOUSE_MAX_DATA 64
#define MOUSE_MERGED 0x01

enum { BTN_PRIMARY=0x1, BTN_SECONDARY=0x2, BTN_OPTIONAL=0x4, BTN_OPTIONAL2=0x8, BTN_OPTIONAL3=0x10 };
typedef struct {
    uint64_t timestamp;
    uint8_t connected, pad[3];
    uint32_t buttons;
    int32_t x, y, wheel, tilt;
    uint8_t reserve[8];
} MouseData;
typedef struct { uint8_t behavior, reserve[7]; } MouseOpenParam;
_Static_assert(sizeof(MouseData)==40,"OrbisMouseData layout");

typedef struct { int open, host; int32_t user, index, x, y, wheel, tilt; } MousePort;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, host_connected=-1;
static MousePort ports[MOUSE_HANDLES];
static size_t reads;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static MousePort *port_of(int32_t handle) { return handle>=1 && handle<=MOUSE_HANDLES && ports[handle-1].open ? &ports[handle-1] : NULL; }
static uint32_t ps4_buttons(uint32_t sdl) {
    return (sdl & SDL_BUTTON_LMASK ? BTN_PRIMARY : 0) | (sdl & SDL_BUTTON_RMASK ? BTN_SECONDARY : 0) |
           (sdl & SDL_BUTTON_MMASK ? BTN_OPTIONAL : 0) | (sdl & SDL_BUTTON_X1MASK ? BTN_OPTIONAL2 : 0) |
           (sdl & SDL_BUTTON_X2MASK ? BTN_OPTIONAL3 : 0);
}
/* The host's motion since the last read of any handle goes to every handle on the host mouse, so
 * two of them (one per user) each see all of it. Called under lock. */
static int collect(uint32_t *buttons) {
    int32_t dx, dy, wheel, tilt;
    const int host=bbgpu_game_mouse_take(&dx,&dy,&wheel,&tilt,buttons);
    if (host!=host_connected) {
        printf("Runtime: mouse: %s\n",host ? "the host mouse reaches libSceMouse's port 0" : "no host mouse, port 0 reports none");
        host_connected=host;
    }
    /* The settings menu or the close prompt holds the game's input neutral. */
    if (!host || bbgpu_overlay_captures_input()) { *buttons=0; return host; }
    for (int i=0;i<MOUSE_HANDLES;++i)
        if (ports[i].open && ports[i].host) {
            ports[i].x+=dx; ports[i].y+=dy; ports[i].wheel+=wheel; ports[i].tilt+=tilt;
        }
    return host;
}

static ABI int32_t mouse_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t mouse_open(int32_t user, int32_t type, int32_t index, const MouseOpenParam *param) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (type!=0 || index<0 || index>=MOUSE_PORT_INDICES) return ERR_INVALID_ARG;
    const int merged=param && (param->behavior & MOUSE_MERGED);
    pthread_mutex_lock(&lock);
    int32_t handle=ERR_INVALID_ARG;
    for (int i=0;i<MOUSE_HANDLES;++i)
        if (ports[i].open && ports[i].user==user && ports[i].index==index) { handle=ERR_ALREADY_OPENED; break; }
    for (int i=0;i<MOUSE_HANDLES && handle==ERR_INVALID_ARG;++i)
        if (!ports[i].open) {
            ports[i]=(MousePort){.open=1,.host=index==0 || merged,.user=user,.index=index};
            handle=i+1;
        }
    pthread_mutex_unlock(&lock);
    if (handle>0) printf("Runtime: mouse port %d opened for user %d (handle %d%s)\n",index,user,handle,index==0 || merged ? ", the host mouse" : ", no mouse");
    return handle;
}
static ABI int32_t mouse_close(int32_t handle) {
    pthread_mutex_lock(&lock);
    MousePort *p=port_of(handle);
    if (p) p->open=0;
    pthread_mutex_unlock(&lock);
    return p ? 0 : ERR_INVALID_HANDLE;
}
static ABI int32_t mouse_read(int32_t handle, MouseData *data, int32_t count) {
    if (!data || count<1 || count>MOUSE_MAX_DATA) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    MousePort *p=port_of(handle);
    if (!p) { pthread_mutex_unlock(&lock); return ERR_INVALID_HANDLE; }
    uint32_t buttons=0;
    const int host=collect(&buttons);
    memset(data,0,sizeof(*data));
    data->timestamp=now_us();
    if (p->host && host) {
        data->connected=1;
        data->buttons=ps4_buttons(buttons);
        data->x=p->x; data->y=p->y; data->wheel=p->wheel; data->tilt=p->tilt;
    }
    p->x=p->y=p->wheel=p->tilt=0;
    ++reads;
    pthread_mutex_unlock(&lock);
    return 1;
}

static const RuntimeExport exports[]={
    {"sceMouseInit",mouse_init}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
};
uintptr_t runtime_mouse_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_mouse_report(void) {
    if (reads) printf("Runtime: mouse reads=%zu, host mouse=%s\n",reads,host_connected>0 ? "yes" : "none");
}
