/* libSceAudioOut on the host's audio streams (ps_host_audio.h). sceAudioOutOutput returns once
 * per buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest host queue level of
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
    int used, is_opening, type, channels, is_float, frames, cb_sample, std_layout;
    int c_device_channels; /* what the port hands the host: fewer than channels when downmix() folds them */
    int32_t volume[8];
    PsHostAudioOut *p_stream;
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
static int s_is_initialized, s_is_host_ready=-1;
static size_t s_c_buffers_out, s_c_ports_opened;

/* SDL3, the host's backend, normalises its downmix matrices so that every channel at full scale
 * cannot clip: from 7.1 the fronts reach a stereo device at 0.2119 (-13.5 dB), a quad one at 0.4663
 * and a 5.1 one at 0.541, while a game mixes mostly into its fronts, which then play ~13 dB below
 * other programs. The fold here keeps the fronts at unity, centre and surrounds at -3 dB (ITU-R
 * BS.775) and LFE at -6 dB, unnormalised like PipeWire's channel mixer by default
 * (channelmix.normalize = false) when it gets the 8 channels itself, and clamps. In and out in
 * ps_host_audio.h's channel orders. */
static void downmix(const float in[8], float *p_out, int c_out) {
    const float k=0.70710678f;
    const float fl=in[0], fr=in[1], fc=in[2], lfe=in[3], bl=in[4], br=in[5], sl=in[6], sr=in[7];
    switch (c_out) {
    case 1: p_out[0]=0.5f*(fl+fr+2.f*k*fc+lfe+k*(sl+sr+bl+br)); break;
    case 2: p_out[0]=fl+k*fc+0.5f*lfe+k*sl+k*bl; p_out[1]=fr+k*fc+0.5f*lfe+k*sr+k*br; break;
    case 3: p_out[0]=fl+k*fc+k*sl+k*bl; p_out[1]=fr+k*fc+k*sr+k*br; p_out[2]=lfe; break;
    case 4: p_out[0]=fl+k*fc+0.5f*lfe+k*sl; p_out[1]=fr+k*fc+0.5f*lfe+k*sr; p_out[2]=bl; p_out[3]=br; break;
    case 5: p_out[0]=fl+k*fc+k*sl; p_out[1]=fr+k*fc+k*sr; p_out[2]=lfe; p_out[3]=bl; p_out[4]=br; break;
    case 6: p_out[0]=fl; p_out[1]=fr; p_out[2]=fc; p_out[3]=lfe; p_out[4]=bl+k*sl; p_out[5]=br+k*sr; break;
    default: p_out[0]=fl; p_out[1]=fr; p_out[2]=fc; p_out[3]=lfe; p_out[4]=k*(bl+br); p_out[5]=sl; p_out[6]=sr; break;
    }
    for (int c=0;c<c_out;++c) p_out[c]=p_out[c]>1.f ? 1.f : p_out[c]<-1.f ? -1.f : p_out[c];
}
/* In place: frame f's c_out samples land at or before where its 8 were read. */
static void downmix_buffer(unsigned char *p_samples, int c_frames, int is_float, int c_out) {
    for (size_t f=0;f<(size_t)c_frames;++f) {
        float in[8], out[7];
        for (size_t c=0;c<8;++c) {
            if (is_float) memcpy(&in[c],p_samples+(f*8+c)*4,4);
            else { int16_t v; memcpy(&v,p_samples+(f*8+c)*2,2); in[c]=v/32768.f; }
        }
        downmix(in,out,c_out);
        for (size_t c=0;c<(size_t)c_out;++c) {
            if (is_float) memcpy(p_samples+(f*(size_t)c_out+c)*4,&out[c],4);
            else { int16_t v=(int16_t)lrintf(out[c]*32767.f); memcpy(p_samples+(f*(size_t)c_out+c)*2,&v,2); }
        }
    }
}

static uint64_t now_ns(void) { return ps_host_time_monotonic_ns(); }
static void sleep_until(uint64_t deadline) { while (ps_host_time_sleep_until_ns(deadline)) {} }
static int host_audio(void) {
    if (s_is_host_ready<0) {
        const char *sz_mode=runtime_env("PS_AUDIO",NULL);
        s_is_host_ready = (!sz_mode || strcmp(sz_mode,"none")) && !ps_host_audio_start();
        printf("Runtime: audio backend %s\n", s_is_host_ready ? ps_host_audio_driver() : "timer (silent)");
    }
    return s_is_host_ready;
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
    for (int i=first;i<=last;++i) if (!s_ports[i].used && !s_ports[i].is_opening) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&s_table_lock); return ERR_PORT_FULL; }
    Port *p_port=&s_ports[id];
    memset(p_port,0,sizeof(*p_port));
    pthread_mutex_init(&p_port->lock,NULL);
    p_port->is_opening=1; p_port->type=type; p_port->channels=s_channels[format]; p_port->is_float=format>=3 && format!=6;
    p_port->cb_sample=p_port->is_float ? 4 : 2; p_port->frames=(int)length; p_port->std_layout=format>=6;
    p_port->c_device_channels=p_port->channels;
    for (int c=0;c<8;++c) p_port->volume[c]=VOLUME_0DB;
    const int is_host_ready=host_audio();
    /* Opening the device takes tens of milliseconds, and every other port's output looks its
       handle up under the table's lock. The slot stays reserved and unopened meanwhile. */
    pthread_mutex_unlock(&s_table_lock);
    if (is_host_ready) {
        const PsHostAudioFormat format=p_port->is_float ? PS_HOST_AUDIO_F32 : PS_HOST_AUDIO_S16;
        p_port->p_stream=ps_host_audio_out_open(format,p_port->channels,48000);
        /* Only after the open is the device's channel count known: PipeWire and PulseAudio take the
           8 channels and mix them themselves, while ALSA, WASAPI and a device an earlier stereo
           port already opened in stereo leave the fold to SDL (see downmix). */
        int c_device=0;
        if (p_port->p_stream && p_port->channels==8 &&
            !ps_host_audio_device_channels(p_port->p_stream,&c_device) &&
            c_device>0 && c_device<8) {
            if (!ps_host_audio_out_set_format(p_port->p_stream,format,c_device,48000)) p_port->c_device_channels=c_device;
            else fprintf(stderr,"Runtime: audio port %d: own downmix to %d ch refused (%s); SDL folds the 8 channels\n",id,c_device,ps_host_audio_error());
        }
        if (p_port->p_stream) ps_host_audio_out_resume(p_port->p_stream);
        else fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",ps_host_audio_error(),id);
    }
    p_port->stats=runtime_env("PS_AUDIO_STATS",NULL)!=NULL;
    const char *sz_dump=runtime_env("PS_AUDIO_DUMP",NULL);
    if (sz_dump) {
        char sz_path[4096];
        snprintf(sz_path,sizeof(sz_path),"%s.port%d.%dch.%s",sz_dump,id,p_port->c_device_channels,p_port->is_float ? "f32" : "s16");
        p_port->p_dump=ps_host_fs_open_stream(sz_path,"wb");
    }
    pthread_mutex_lock(&s_table_lock);
    p_port->is_opening=0; p_port->used=1;
    ++s_c_ports_opened;
    pthread_mutex_unlock(&s_table_lock);
    if (p_port->c_device_channels!=p_port->channels)
        printf("Runtime: audio port %d opened (type %d, %d ch downmixed to the device's %d, %s, %u frames)\n",id,type,p_port->channels,
               p_port->c_device_channels,p_port->is_float ? "float" : "s16",length);
    else printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames)\n",id,type,p_port->channels,p_port->is_float ? "float" : "s16",length);
    return (type<<16) | id | 0x20000000;
}
static ABI int32_t audio_close(int32_t h_handle) {
    pthread_mutex_lock(&s_table_lock);
    int32_t error=0;
    Port *p_port=port_of(h_handle,&error);
    PsHostAudioOut *p_stream=NULL;
    FILE *p_dump=NULL;
    if (p_port) {
        pthread_mutex_lock(&p_port->lock);
        p_stream=p_port->p_stream; p_dump=p_port->p_dump;
        p_port->p_dump=NULL;
        p_port->p_stream=NULL; p_port->used=0;
        pthread_mutex_unlock(&p_port->lock);
    }
    pthread_mutex_unlock(&s_table_lock);
    /* Closing the device waits for its audio thread: not under the table's lock either. */
    if (p_stream) ps_host_audio_out_close(p_stream);
    if (p_dump) fclose(p_dump);
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
    size_t samples=(size_t)p_port->frames*(size_t)p_port->channels;
    size_t cb_data=(size_t)p_port->frames*(size_t)p_port->c_device_channels*(size_t)p_port->cb_sample;
    uint64_t period=(uint64_t)p_port->frames*1000000000u/48000u;
    if (p_data) {
        /* Apply per-channel volume and map PS4 8ch (L R C LFE SL SR BL BR) to the host's 7.1 order. */
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
        if (p_port->c_device_channels!=p_port->channels) downmix_buffer(converted,p_port->frames,p_port->is_float,p_port->c_device_channels);
        if (p_port->p_dump) fwrite(converted,1,cb_data,p_port->p_dump);
        if (p_port->stats) {
            uint64_t now=now_ns();
            if (!p_port->stat_start_ns) { p_port->stat_start_ns=now; p_port->stat_min_queued=1<<30; }
            else if (now-p_port->stat_last_ns>p_port->stat_max_gap_ns) p_port->stat_max_gap_ns=now-p_port->stat_last_ns;
            p_port->stat_last_ns=now; ++p_port->stat_buffers;
            if (p_port->p_stream) {
                int queued=ps_host_audio_out_queued(p_port->p_stream);
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
            uint64_t now=now_ns(), deadline_ns=p_port->next_deadline_ns;
            if (!deadline_ns || now>deadline_ns+8*period) { p_port->next_deadline_ns=now; deadline_ns=0; } /* start or stall: restart cadence */
            p_port->next_deadline_ns+=period;
            /* Not under the port's lock: the thread that feeds the port takes it again right after
               it returns, so a sceAudioOutSetVolume from another thread could wait for seconds. */
            if (deadline_ns) { pthread_mutex_unlock(&p_port->lock); sleep_until(deadline_ns); pthread_mutex_lock(&p_port->lock); }
        }
        if (p_port->p_stream) {
            /* The device drains the queue in quanta (21 ms on PipeWire), so the level
               is a sawtooth: its minimum over ~32 buffers is what gets controlled. */
            int low=2*(int)cb_data, queued=ps_host_audio_out_queued(p_port->p_stream);
            if (queued<(int)cb_data) {
                static const unsigned char s_silence[2048*8*4];
                ps_host_audio_out_submit(p_port->p_stream,s_silence,low-queued<(int)cb_data ? low-queued : (int)cb_data);
                queued=low;
            }
            if (!p_port->c_windows || queued<p_port->window_min) p_port->window_min=queued;
            if (++p_port->c_windows==32) {
                p_port->adjust_ns=p_port->window_min>low+2*(int)cb_data ? (int64_t)(period/32) : p_port->window_min<low ? -(int64_t)(period/32) : 0;
                p_port->c_windows=0;
            }
            p_port->next_deadline_ns+=(uint64_t)p_port->adjust_ns;
            ps_host_audio_out_submit(p_port->p_stream,converted,(int)cb_data);
        }
        ++s_c_buffers_out;
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
    int c_device=0;
    int channels=host_audio() && !ps_host_audio_device_channels(NULL,&c_device) ? c_device : 2;
    pthread_mutex_unlock(&s_table_lock);
    return channels;
}
uintptr_t runtime_audio_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_audio_report(void) { printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",s_c_ports_opened,s_c_buffers_out); }
