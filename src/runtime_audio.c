/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to two buffers (silence prefill on underrun, ±3%
 * cadence corrections), which also absorbs device clock drift. Without an audio device (headless runs, PS_AUDIO=none)
 * ports only follow the clock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>

#define PORTS 25
#define ERR_NOT_OPENED ((int32_t)0x80260001)
#define ERR_INVALID_PORT ((int32_t)0x80260003)
#define ERR_INVALID_POINTER ((int32_t)0x80260004)
#define ERR_PORT_FULL ((int32_t)0x80260005)
#define ERR_INVALID_SIZE ((int32_t)0x80260006)
#define ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define ERR_INVALID_FREQ ((int32_t)0x80260008)
#define ERR_INVALID_VOLUME ((int32_t)0x80260009)
#define ERR_INVALID_TYPE ((int32_t)0x8026000A)
#define ERR_ALREADY_INIT ((int32_t)0x8026000E)
#define ERR_NOT_INIT ((int32_t)0x8026000F)
#define VOLUME_0DB 32768

typedef struct {
    int used, type, channels, is_float, frames, cb_sample, std_layout;
    int32_t volume[8];
    SDL_AudioStream *p_stream;
    uint64_t next_deadline_ns; /* next return of sceAudioOutOutput */
    int64_t adjust_ns; int window_min, c_windows; /* queue level control */
    uint64_t last_output_us;
    pthread_mutex_t lock;
    FILE *p_dump;                     /* PS_AUDIO_DUMP: raw converted PCM per port */
    int stats;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* PS_AUDIO_STATS */
    unsigned stat_starved, stat_buffers; int stat_min_queued;
    /* Level of what the game sent (after volume), so a test can tell sound from silence. */
    float stat_peak; double stat_sum_sq; uint64_t stat_samples;
} Port;
typedef struct { uint16_t output; uint8_t channel, reserved; int16_t volume; uint16_t reroute; uint64_t flag, reserved64[2]; } PortState;
_Static_assert(sizeof(PortState)==32,"AudioOut port state layout");

static pthread_mutex_t s_table_lock=PTHREAD_MUTEX_INITIALIZER;
static Port s_ports[PORTS];
static int s_is_initialized, s_is_sdl_ready=-1;
static size_t s_buffers_out, s_ports_opened;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void sleep_until(uint64_t deadline) {
    struct timespec t={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)) {}
}
static int sdl_audio(void) {
    if (s_is_sdl_ready<0) {
        const char *sz_mode=getenv("PS_AUDIO");
        s_is_sdl_ready = (!sz_mode || strcmp(sz_mode,"none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", s_is_sdl_ready ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return s_is_sdl_ready;
}
static int port_range(int type, int *p_first, int *p_last) {
    switch (type) {
    case 0: *p_first=0; *p_last=7; return 1;      /* main */
    case 1: *p_first=8; *p_last=8; return 1;      /* bgm */
    case 2: *p_first=9; *p_last=12; return 1;     /* voice */
    case 3: *p_first=13; *p_last=16; return 1;    /* personal */
    case 4: *p_first=17; *p_last=20; return 1;    /* pad speaker */
    case 126: *p_first=21; *p_last=22; return 1;  /* audio3d */
    case 127: *p_first=23; *p_last=24; return 1;  /* aux */
    default: return 0;
    }
}
static Port *port_of(int32_t h_handle, int32_t *p_error) {
    int id=h_handle & 0xff;
    if ((h_handle & 0x3f000000)!=0x20000000 || id>=PORTS) { *p_error=ERR_INVALID_PORT; return NULL; }
    if (!s_ports[id].used) { *p_error=ERR_NOT_OPENED; return NULL; }
    return &s_ports[id];
}

static ABI int32_t audio_init(void) {
    pthread_mutex_lock(&s_table_lock);
    int32_t r=s_is_initialized ? ERR_ALREADY_INIT : 0;
    s_is_initialized=1;
    pthread_mutex_unlock(&s_table_lock);
    return r;
}
static ABI int32_t audio_open(int32_t user, int32_t type, int32_t i_device, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)i_device;
    if (!s_is_initialized) return ERR_NOT_INIT;
    if (!length || length>2048 || (length & 0xff)) return ERR_INVALID_SIZE;
    if (freq!=48000) return ERR_INVALID_FREQ;
    uint32_t format=param & 0xff;
    if (format>7) return ERR_INVALID_FORMAT;
    int first, last;
    if (!port_range(type,&first,&last)) return ERR_INVALID_TYPE;
    static const int s_channels[8]={1,2,8,1,2,8,8,8};
    pthread_mutex_lock(&s_table_lock);
    int id=-1;
    for (int i=first;i<=last;++i) if (!s_ports[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&s_table_lock); return ERR_PORT_FULL; }
    Port *p_port=&s_ports[id];
    memset(p_port,0,sizeof(*p_port));
    pthread_mutex_init(&p_port->lock,NULL);
    p_port->used=1; p_port->type=type; p_port->channels=s_channels[format]; p_port->is_float=format>=3 && format!=6;
    p_port->cb_sample=p_port->is_float ? 4 : 2; p_port->frames=(int)length; p_port->std_layout=format>=6;
    for (int c=0;c<8;++c) p_port->volume[c]=VOLUME_0DB;
    if (sdl_audio()) {
        SDL_AudioSpec spec={p_port->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, p_port->channels, 48000};
        p_port->p_stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (p_port->p_stream) SDL_ResumeAudioStreamDevice(p_port->p_stream);
        else fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
    }
    p_port->stats=getenv("PS_AUDIO_STATS")!=NULL;
    const char *sz_dump=getenv("PS_AUDIO_DUMP");
    if (sz_dump) {
        char sz_path[4096];
        snprintf(sz_path,sizeof(sz_path),"%s.port%d.%dch.%s",sz_dump,id,p_port->channels,p_port->is_float ? "f32" : "s16");
        p_port->p_dump=fopen(sz_path,"wb");
    }
    ++s_ports_opened;
    pthread_mutex_unlock(&s_table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames)\n",id,type,p_port->channels,p_port->is_float ? "float" : "s16",length);
    return (type<<16) | id | 0x20000000;
}
static ABI int32_t audio_close(int32_t h_handle) {
    pthread_mutex_lock(&s_table_lock);
    int32_t error=0;
    Port *p_port=port_of(h_handle,&error);
    if (p_port) {
        pthread_mutex_lock(&p_port->lock);
        if (p_port->p_stream) SDL_DestroyAudioStream(p_port->p_stream);
        if (p_port->p_dump) fclose(p_port->p_dump);
        p_port->p_dump=NULL;
        p_port->p_stream=NULL; p_port->used=0;
        pthread_mutex_unlock(&p_port->lock);
    }
    pthread_mutex_unlock(&s_table_lock);
    return p_port ? 0 : error;
}
/* `pace`: wait for this port's next period. sceAudioOutOutputs waits once for all its ports. */
static int32_t output_port(int32_t h_handle, const void *p_data, int pace) {
    int32_t error=0;
    pthread_mutex_lock(&s_table_lock);
    Port *p_port=port_of(h_handle,&error);
    pthread_mutex_unlock(&s_table_lock);
    if (!p_port) return error;
    pthread_mutex_lock(&p_port->lock);
    size_t samples=(size_t)p_port->frames*(size_t)p_port->channels, cb_data=samples*(size_t)p_port->cb_sample;
    uint64_t period=(uint64_t)p_port->frames*1000000000u/48000u;
    if (p_data) {
        /* Apply per-channel volume and map PS4 8ch (L R C LFE SL SR BL BR) to SDL 7.1 order. */
        unsigned char converted[2048*8*4];
        static const int s_remap[8]={0,1,2,3,6,7,4,5};
        for (size_t f=0;f<(size_t)p_port->frames;++f) for (int c=0;c<p_port->channels;++c) {
            int target=p_port->channels==8 && !p_port->std_layout ? s_remap[c] : c;
            size_t from=f*(size_t)p_port->channels+(size_t)c, to=f*(size_t)p_port->channels+(size_t)target;
            float gain=(float)p_port->volume[c]/VOLUME_0DB;
            float level;
            if (p_port->is_float) { float v; memcpy(&v,(const char *)p_data+from*4,4); v*=gain; memcpy(converted+to*4,&v,4); level=v; }
            else { int16_t v; memcpy(&v,(const char *)p_data+from*2,2); v=(int16_t)((float)v*gain); memcpy(converted+to*2,&v,2); level=v/32768.0f; }
            if (p_port->stats) {
                if (fabsf(level)>p_port->stat_peak) p_port->stat_peak=fabsf(level);
                p_port->stat_sum_sq+=(double)level*level;
            }
        }
        if (p_port->stats) p_port->stat_samples+=samples;
        if (p_port->p_dump) fwrite(converted,1,cb_data,p_port->p_dump);
        if (p_port->stats) {
            uint64_t now=now_ns();
            if (!p_port->stat_start_ns) { p_port->stat_start_ns=now; p_port->stat_min_queued=1<<30; }
            else if (now-p_port->stat_last_ns>p_port->stat_max_gap_ns) p_port->stat_max_gap_ns=now-p_port->stat_last_ns;
            p_port->stat_last_ns=now; ++p_port->stat_buffers;
            if (p_port->p_stream) {
                int queued=SDL_GetAudioStreamQueued(p_port->p_stream);
                if (queued<p_port->stat_min_queued) p_port->stat_min_queued=queued;
                if (queued==0) ++p_port->stat_starved;
            }
            if (now-p_port->stat_start_ns>=5000000000u) {
                const double rms=p_port->stat_samples ? sqrt(p_port->stat_sum_sq/(double)p_port->stat_samples) : 0;
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u, ",
                       (int)(p_port-s_ports),p_port->stat_buffers,(unsigned)(5u*48000u/(unsigned)p_port->frames),p_port->stat_max_gap_ns/1e6,
                       p_port->stat_min_queued,p_port->stat_starved);
                if (p_port->stat_peak>0) printf("peak %.1f dBFS, rms %.1f dBFS\n",20*log10(p_port->stat_peak),20*log10(rms));
                else printf("silent\n");
                p_port->stat_start_ns=now; p_port->stat_max_gap_ns=0; p_port->stat_starved=0; p_port->stat_buffers=0; p_port->stat_min_queued=1<<30;
                p_port->stat_peak=0; p_port->stat_sum_sq=0; p_port->stat_samples=0;
            }
        }
        if (pace) {
            uint64_t now=now_ns();
            if (!p_port->next_deadline_ns || now>p_port->next_deadline_ns+8*period) p_port->next_deadline_ns=now; /* start or stall: restart cadence */
            else sleep_until(p_port->next_deadline_ns);
            p_port->next_deadline_ns+=period;
        }
        if (p_port->p_stream) {
            /* The device drains the queue in quanta (21 ms on PipeWire), so the level
               is a sawtooth: its minimum over ~32 buffers is what gets controlled. */
            int low=2*(int)cb_data, queued=SDL_GetAudioStreamQueued(p_port->p_stream);
            if (queued<(int)cb_data) {
                static const unsigned char s_silence[2048*8*4];
                SDL_PutAudioStreamData(p_port->p_stream,s_silence,low-queued<(int)cb_data ? low-queued : (int)cb_data);
                queued=low;
            }
            if (!p_port->c_windows || queued<p_port->window_min) p_port->window_min=queued;
            if (++p_port->c_windows==32) {
                p_port->adjust_ns=p_port->window_min>low+2*(int)cb_data ? (int64_t)(period/32) : p_port->window_min<low ? -(int64_t)(period/32) : 0;
                p_port->c_windows=0;
            }
            p_port->next_deadline_ns+=(uint64_t)p_port->adjust_ns;
            SDL_PutAudioStreamData(p_port->p_stream,converted,(int)cb_data);
        }
        ++s_buffers_out;
    }
    p_port->last_output_us=now_ns()/1000;
    pthread_mutex_unlock(&p_port->lock);
    return p_data ? (int32_t)samples : 0;
}
static ABI int32_t audio_output(int32_t h_handle, const void *p_data) { return output_port(h_handle,p_data,1); }
typedef struct { int32_t handle; const void *data; } OutputParam;
static ABI int32_t audio_outputs(const OutputParam *p_params, uint32_t c_params) {
    if (!p_params) return ERR_INVALID_POINTER;
    if (!c_params || c_params>PORTS) return ERR_PORT_FULL;
    int32_t result=0;
    /* One period per call for all ports (FMOD feeds main and BGM together): pacing every
       port would halve the rate and starve both. */
    for (uint32_t i=0;i<c_params;++i) { int32_t r=output_port(p_params[i].handle,p_params[i].data,i==0); if (r<0) return r; result=r; }
    return result;
}
static ABI int32_t audio_volume(int32_t h_handle, int32_t flags, const int32_t *p_volume) {
    int32_t error=0;
    pthread_mutex_lock(&s_table_lock);
    Port *p_port=port_of(h_handle,&error);
    pthread_mutex_unlock(&s_table_lock);
    if (!p_port) return error;
    if (!p_volume) return ERR_INVALID_POINTER;
    pthread_mutex_lock(&p_port->lock);
    for (int c=0;c<8;++c) if (flags & (1<<c)) {
        if (p_volume[c]<0 || p_volume[c]>VOLUME_0DB) { pthread_mutex_unlock(&p_port->lock); return ERR_INVALID_VOLUME; }
        p_port->volume[c]=p_volume[c];
    }
    pthread_mutex_unlock(&p_port->lock);
    return 0;
}
/* How much of a port also plays on the pad's speaker. No pad speaker is driven from the host, so
 * the level is checked and has nothing to change. */
static ABI int32_t audio_pad_speaker_mix(int32_t h_port, int32_t mix_level) {
    int32_t error=0;
    pthread_mutex_lock(&s_table_lock);
    const Port *p_port=port_of(h_port,&error);
    pthread_mutex_unlock(&s_table_lock);
    if (!p_port) return error;
    return mix_level<0 || mix_level>VOLUME_0DB ? ERR_INVALID_VOLUME : 0;
}
static ABI int32_t audio_state(int32_t h_handle, PortState *p_state) {
    int32_t error=0;
    pthread_mutex_lock(&s_table_lock);
    Port *p_port=port_of(h_handle,&error);
    pthread_mutex_unlock(&s_table_lock);
    if (!p_port) return error;
    if (!p_state) return ERR_INVALID_POINTER;
    memset(p_state,0,sizeof(*p_state));
    switch (p_port->type) {
    case 2: case 3: p_state->output=0x40; p_state->channel=1; break;          /* headphone */
    case 4: p_state->output=0x04; p_state->channel=1; p_state->volume=127; break; /* pad speaker */
    default: p_state->output=0x01; p_state->channel=(uint8_t)(p_port->channels>2 ? 2 : p_port->channels); break;
    }
    return 0;
}
static ABI int32_t audio_last_time(int32_t h_handle, uint64_t *p_time) {
    int32_t error=0;
    pthread_mutex_lock(&s_table_lock);
    Port *p_port=port_of(h_handle,&error);
    pthread_mutex_unlock(&s_table_lock);
    if (!p_port) return error;
    if (!p_time) return ERR_INVALID_POINTER;
    *p_time=p_port->last_output_us; return 0;
}

static const RuntimeExport s_exports[]={
    {"sceAudioOutInit",audio_init}, {"sceAudioOutOpen",audio_open}, {"sceAudioOutClose",audio_close},
    {"sceAudioOutOutput",audio_output}, {"sceAudioOutOutputs",audio_outputs},
    {"sceAudioOutSetVolume",audio_volume}, {"sceAudioOutSetMixLevelPadSpk",audio_pad_speaker_mix}, {"sceAudioOutGetPortState",audio_state},
    {"sceAudioOutGetLastOutputTime",audio_last_time},
};
int32_t runtime_audio_init(void) { return audio_init(); }
int32_t runtime_audio_open(int32_t user, int32_t type, int32_t i_port, uint32_t length, uint32_t freq, uint32_t param) {
    return audio_open(user,type,i_port,length,freq,param);
}
int32_t runtime_audio_output(int32_t h_handle, const void *p_data) { return audio_output(h_handle,p_data); }
int32_t runtime_audio_close(int32_t h_handle) { return audio_close(h_handle); }
int runtime_audio_device_channels(void) {
    pthread_mutex_lock(&s_table_lock);
    SDL_AudioSpec spec;
    int channels=sdl_audio() && SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL) ? spec.channels : 2;
    pthread_mutex_unlock(&s_table_lock);
    return channels;
}
uintptr_t runtime_audio_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_audio_report(void) { printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",s_ports_opened,s_buffers_out); }
