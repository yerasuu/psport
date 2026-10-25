/* libSceMouse on the host mouse. The host's events are pumped by the window thread (gpu/shim/window.cpp);
 * here they are only collected. Every game that opens a mouse port gets the host mouse: port index
 * 0 (or a merged port) reports it whenever the host has one, the other index never has a mouse.
 * A read returns one entry, the buttons held and the motion, wheel and tilt since the previous read
 * of that handle. Without a window or a host mouse the port is open but reports no device, as a
 * console with no mouse plugged in does. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

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

typedef struct { int is_open, host; int32_t user, i_device, x, y, wheel, tilt; } MousePort;
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_initialized, s_is_host_connected=-1;
static MousePort s_ports[MOUSE_HANDLES];
static size_t s_c_reads;
static float s_sensitivity=-1.0f, s_carry_x, s_carry_y;

/* game_mouse_sensitivity in psport.ini (PS_CONFIG), beside mouse look's keys (runtime_pad.c):
 * scales the motion the game receives; 1 passes the host's pixels as they are. */
static void load_sensitivity(void) {
    s_sensitivity=1.0f;
    const char *sz_key, *sz_value;
    for (size_t i_entry=0; runtime_config_entry(i_entry,&sz_key,&sz_value); ++i_entry)
        if (!strcmp(sz_key,"game_mouse_sensitivity")) {
            const float v=strtof(sz_value,NULL);
            if (v>=0.05f && v<=20.0f) s_sensitivity=v;
            else printf("Runtime: mouse: game_mouse_sensitivity=%g is outside 0.05-20, 1 is used\n",v);
        }
    if (s_sensitivity!=1.0f) printf("Runtime: mouse: game_mouse_sensitivity=%.2f scales the motion games receive\n",s_sensitivity);
}
/* Fractions stay for the next read, so slow motion still adds up at a low sensitivity. */
static int32_t scaled(int32_t delta, float *p_carry) {
    *p_carry+=(float)delta*s_sensitivity;
    const int32_t whole=(int32_t)*p_carry;
    *p_carry-=(float)whole;
    return whole;
}

static uint64_t now_us(void) { return ps_host_time_monotonic_ns()/1000u; }
static MousePort *port_of(int32_t h_handle) { return h_handle>=1 && h_handle<=MOUSE_HANDLES && s_ports[h_handle-1].is_open ? &s_ports[h_handle-1] : NULL; }
static uint32_t ps4_buttons(uint32_t host_mask) {
    return (host_mask & PS_HOST_INPUT_MOUSE_MASK(PS_HOST_INPUT_MOUSE_LEFT) ? BTN_PRIMARY : 0) |
           (host_mask & PS_HOST_INPUT_MOUSE_MASK(PS_HOST_INPUT_MOUSE_RIGHT) ? BTN_SECONDARY : 0) |
           (host_mask & PS_HOST_INPUT_MOUSE_MASK(PS_HOST_INPUT_MOUSE_MIDDLE) ? BTN_OPTIONAL : 0) |
           (host_mask & PS_HOST_INPUT_MOUSE_MASK(PS_HOST_INPUT_MOUSE_X1) ? BTN_OPTIONAL2 : 0) |
           (host_mask & PS_HOST_INPUT_MOUSE_MASK(PS_HOST_INPUT_MOUSE_X2) ? BTN_OPTIONAL3 : 0);
}
/* The host's motion since the last read of any handle goes to every handle on the host mouse, so
 * two of them (one per user) each see all of it. Called under lock. */
static int collect(uint32_t *p_buttons) {
    int32_t dx, dy, wheel, tilt;
    const int host=psport_gpu_game_mouse_take(&dx,&dy,&wheel,&tilt,p_buttons);
    if (s_sensitivity<0) load_sensitivity();
    dx=scaled(dx,&s_carry_x); dy=scaled(dy,&s_carry_y);
    if (host!=s_is_host_connected) {
        printf("Runtime: mouse: %s\n",host ? "the host mouse reaches libSceMouse's port 0" : "no host mouse, port 0 reports none");
        s_is_host_connected=host;
    }
    /* The settings menu or the close prompt holds the game's input neutral. */
    if (!host || psport_gpu_overlay_captures_input()) { *p_buttons=0; return host; }
    for (int i=0;i<MOUSE_HANDLES;++i)
        if (s_ports[i].is_open && s_ports[i].host) {
            s_ports[i].x+=dx; s_ports[i].y+=dy; s_ports[i].wheel+=wheel; s_ports[i].tilt+=tilt;
        }
    return host;
}

static ABI int32_t mouse_init(void) { pthread_mutex_lock(&s_lock); s_is_initialized=1; pthread_mutex_unlock(&s_lock); return 0; }
static ABI int32_t mouse_open(int32_t user, int32_t type, int32_t i_device, const MouseOpenParam *p_param) {
    if (!s_is_initialized) return ERR_NOT_INITIALIZED;
    if (type!=0 || i_device<0 || i_device>=MOUSE_PORT_INDICES) return ERR_INVALID_ARG;
    const int merged=p_param && (p_param->behavior & MOUSE_MERGED);
    pthread_mutex_lock(&s_lock);
    int32_t h_handle=ERR_INVALID_ARG;
    for (int i=0;i<MOUSE_HANDLES;++i)
        if (s_ports[i].is_open && s_ports[i].user==user && s_ports[i].i_device==i_device) { h_handle=ERR_ALREADY_OPENED; break; }
    for (int i=0;i<MOUSE_HANDLES && h_handle==ERR_INVALID_ARG;++i)
        if (!s_ports[i].is_open) {
            s_ports[i]=(MousePort){.is_open=1,.host=i_device==0 || merged,.user=user,.i_device=i_device};
            h_handle=i+1;
        }
    pthread_mutex_unlock(&s_lock);
    if (h_handle>0) printf("Runtime: mouse port %d opened for user %d (handle %d%s)\n",i_device,user,h_handle,i_device==0 || merged ? ", the host mouse" : ", no mouse");
    return h_handle;
}
static ABI int32_t mouse_close(int32_t h_handle) {
    pthread_mutex_lock(&s_lock);
    MousePort *p_mouse_port=port_of(h_handle);
    if (p_mouse_port) p_mouse_port->is_open=0;
    pthread_mutex_unlock(&s_lock);
    return p_mouse_port ? 0 : ERR_INVALID_HANDLE;
}
static ABI int32_t mouse_read(int32_t h_handle, MouseData *p_data, int32_t c_data) {
    if (!p_data || c_data<1 || c_data>MOUSE_MAX_DATA) return ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    MousePort *p_mouse_port=port_of(h_handle);
    if (!p_mouse_port) { pthread_mutex_unlock(&s_lock); return ERR_INVALID_HANDLE; }
    uint32_t buttons=0;
    const int host=collect(&buttons);
    memset(p_data,0,sizeof(*p_data));
    p_data->timestamp=now_us();
    if (p_mouse_port->host && host) {
        p_data->connected=1;
        p_data->buttons=ps4_buttons(buttons);
        p_data->x=p_mouse_port->x; p_data->y=p_mouse_port->y; p_data->wheel=p_mouse_port->wheel; p_data->tilt=p_mouse_port->tilt;
    }
    p_mouse_port->x=p_mouse_port->y=p_mouse_port->wheel=p_mouse_port->tilt=0;
    ++s_c_reads;
    pthread_mutex_unlock(&s_lock);
    return 1;
}

static const RuntimeExport s_exports[]={
    {"sceMouseInit",mouse_init}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
};
uintptr_t runtime_mouse_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_mouse_report(void) {
    if (s_c_reads) printf("Runtime: mouse reads=%zu, host mouse=%s\n",s_c_reads,s_is_host_connected>0 ? "yes" : "none");
}
