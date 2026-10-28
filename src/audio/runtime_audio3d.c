// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceAudio3d: ports that queue bed and object frames of `granularity`
 * samples. sceAudio3dPortAdvance mixes the head of each queue into one frame
 * and sceAudio3dPortPush sends the mixed frames, once the port's queue is full,
 * to an AudioOut port of its own, so AudioOut paces the caller. AudioOut ports
 * opened through sceAudio3dAudioOutOpen hold their buffers until the next push
 * of their Audio3d port, which sends them first. Beds and objects mix into 7.1:
 * objects pan in pairs at constant power over a horizontal ring of speakers,
 * their spread and elevation widening the arc they cover, and passthrough
 * objects go to one speaker only. Ports output 7.1 when the host device has
 * 5.1 or more, else stereo folded down with -3 dB centre and surrounds (no
 * LFE). An object's gain is unity until set, not shadPS4's zero: CRI's
 * passthrough objects (ALTDEUS) never set one. The attribute ids are the ones
 * FMOD, Koei Tecmo's engine and CRI pass, not shadPS4's enum. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>

#define ERR_INVALID_PORT ((int32_t)0x80EA0002)
#define ERR_INVALID_OBJECT ((int32_t)0x80EA0003)
#define ERR_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define ERR_OUT_OF_MEMORY ((int32_t)0x80EA0005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80EA0006)
#define ERR_NOT_READY ((int32_t)0x80EA0007)
#define ERR_NOT_SUPPORTED ((int32_t)0x80EA0008)
#define AUDIO_OUT_ALREADY_INIT ((int32_t)0x8026000E)
#define PORTS 4
#define MAX_DEPTH 64
#define MAX_OBJECTS 512
#define MAX_AUDIO_OUT 8
#define USER_SYSTEM 0xff
#define AUDIO_OUT_TYPE_AUDIO3D 126
#define AUDIO_OUT_FLOAT_STEREO 4
#define AUDIO_OUT_FLOAT_8CH 5
#define SPEAKERS 8
#define TAU 6.28318531f
enum { MODE_NO_ADVANCE=0, MODE_ADVANCE_NO_PUSH=1, MODE_ADVANCE_AND_PUSH=2 };
enum {
    ATTRIBUTE_PCM=1, ATTRIBUTE_PRIORITY=2, ATTRIBUTE_POSITION=3, ATTRIBUTE_SPREAD=4, ATTRIBUTE_GAIN=5,
    ATTRIBUTE_PASSTHROUGH=6, ATTRIBUTE_RESET_STATE=7, PORT_ATTRIBUTE_LATE_REVERB_LEVEL=0x10001
};
enum { PASSTHROUGH_NONE=0, PASSTHROUGH_LEFT=1, PASSTHROUGH_RIGHT=2 };
#define ROUTE_TV_ONLY 2
enum { FORMAT_S16=0, FORMAT_FLOAT=1 };
enum { BLOCKING_ASYNC=0, BLOCKING_SYNC=1 };

typedef struct {
    uint64_t size_this;
    uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, num_beds;
} OpenParameters;
typedef struct { uint32_t format; uint32_t pad; const void *samples; uint32_t num_samples; } Pcm;
typedef struct { uint32_t id; uint32_t pad; const void *value; uint64_t size; } Attribute;
typedef struct { int32_t handle; uint32_t pad; const void *data; } OutputParam;
_Static_assert(sizeof(OpenParameters)==0x28,"OrbisAudio3dOpenParameters layout");
_Static_assert(sizeof(Pcm)==24 && sizeof(Attribute)==24,"OrbisAudio3dPcm/Attribute layout");

typedef struct { void *pcm; uint32_t channels, is_float; } Frame;
typedef struct { Frame frames[MAX_DEPTH]; uint32_t head, c_frames; } Queue;
typedef struct {
    uint32_t id, passthrough;
    int unreserved;
    float gain, position[3], spread, rendered[SPEAKERS];
    Queue pcm;
} Object;
typedef struct { int32_t h_handle; uint32_t cb_frame, samples, is_float; Queue pending; } AudioOutPort;
typedef struct {
    int used;
    OpenParameters parameters;
    int32_t output;
    uint32_t serial, channels;
    Queue beds, mixed;
    Object *p_objects;
    AudioOutPort audio_out[MAX_AUDIO_OUT];
} Port;

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_initialized;
static Port s_ports[PORTS];
static size_t s_c_ports_opened, s_c_frames_advanced, s_c_frames_pushed, s_c_audio_out_buffers, s_c_objects_reserved, s_c_orphan_attributes;
static size_t s_c_object_frames_placed, s_c_object_frames_passed;
static unsigned s_ignored_attributes, s_ignored_port_attributes;

static void queue_push(Queue *p_queue, Frame f) { p_queue->frames[(p_queue->head+p_queue->c_frames++)%MAX_DEPTH]=f; }
static Frame queue_pop(Queue *p_queue) {
    Frame f=p_queue->frames[p_queue->head];
    p_queue->head=(p_queue->head+1)%MAX_DEPTH; --p_queue->c_frames;
    return f;
}
static void queue_clear(Queue *p_queue) { while (p_queue->c_frames) free(queue_pop(p_queue).pcm); }

static Port *port_of(uint32_t id) { return s_is_initialized && id<PORTS && s_ports[id].used ? &s_ports[id] : NULL; }
static Object *object_of(Port *p_port, uint32_t id) {
    uint32_t slot=(id & 0x3ff)-1;
    return slot<p_port->parameters.max_objects && p_port->p_objects[slot].id==id ? &p_port->p_objects[slot] : NULL;
}
static int misaligned(const void *p_data, int is_float) { return ((uintptr_t)p_data & (is_float ? 3u : 1u))!=0; }

/* A frame of exactly `granularity` samples: a shorter one is padded with silence. */
static int32_t enqueue(Queue *p_queue, const Port *p_port, uint32_t format, const void *p_samples, uint32_t c_samples, uint32_t channels) {
    if (!p_samples || !c_samples || format>FORMAT_FLOAT || misaligned(p_samples,format==FORMAT_FLOAT)) return ERR_INVALID_PARAMETER;
    if (p_queue->c_frames>=p_port->parameters.queue_depth) return ERR_NOT_READY;
    size_t cb_sample=format==FORMAT_FLOAT ? 4 : 2, cb_frame=channels*cb_sample;
    void *p_pcm=calloc(p_port->parameters.granularity,cb_frame);
    if (!p_pcm) return ERR_OUT_OF_MEMORY;
    memcpy(p_pcm,p_samples,(c_samples<p_port->parameters.granularity ? c_samples : p_port->parameters.granularity)*cb_frame);
    queue_push(p_queue,(Frame){p_pcm,channels,format==FORMAT_FLOAT});
    return 0;
}

static float sample(const Frame *p_frame, uint32_t i, uint32_t c) {
    return p_frame->is_float ? ((const float *)p_frame->pcm)[i*p_frame->channels+c] : ((const int16_t *)p_frame->pcm)[i*p_frame->channels+c]/32768.0f;
}
/* A stereo bed feeds L and R; 5.1 and 7.1 beds share AudioOut's 8-channel order (L R C LFE SL SR BL BR). */
static void mix_bed(float *p_out, uint32_t frames, Queue *p_queue) {
    if (!p_queue->c_frames) return;
    Frame f=queue_pop(p_queue);
    for (uint32_t i=0;i<frames;++i)
        for (uint32_t c=0;c<f.channels;++c) p_out[i*SPEAKERS+c]+=sample(&f,i,c);
    free(f.pcm);
}

static const struct { float azimuth; uint32_t speaker; } s_ring[]={
    {0,2}, {TAU/12,1}, {TAU/4,5}, {TAU*5/12,7}, {TAU*7/12,6}, {TAU*3/4,4}, {TAU*11/12,0},
};
#define RING (sizeof(s_ring)/sizeof(s_ring[0]))
static void pan(float azimuth, float weight, float *p_power) {
    float a=fmodf(azimuth,TAU);
    if (a<0) a+=TAU;
    uint32_t i=0;
    while (i+1<RING && a>=s_ring[i+1].azimuth) ++i;
    uint32_t next=(i+1)%RING;
    float end=next ? s_ring[next].azimuth : TAU, t=(a-s_ring[i].azimuth)/(end-s_ring[i].azimuth)*(TAU/4);
    p_power[s_ring[i].speaker]+=weight*cosf(t)*cosf(t);
    p_power[s_ring[next].speaker]+=weight*sinf(t)*sinf(t);
}
/* The ring is horizontal, so elevation widens the arc: an object overhead or at the listener covers all of it. */
static void object_gains(const Object *p_object, float *p_gains) {
    memset(p_gains,0,SPEAKERS*sizeof(float));
    if (p_object->passthrough!=PASSTHROUGH_NONE) { p_gains[p_object->passthrough==PASSTHROUGH_LEFT ? 0 : 1]=p_object->gain; return; }
    float x=p_object->position[0], y=p_object->position[1], z=p_object->position[2];
    float horizontal=sqrtf(x*x+z*z), distance=sqrtf(x*x+y*y+z*z);
    float spread=distance>1e-6f ? TAU*(1-horizontal/distance) : TAU;
    if (p_object->spread>spread) spread=p_object->spread;
    float azimuth=horizontal>1e-6f ? atan2f(x,z) : 0;
    uint32_t steps=1+(uint32_t)(spread/(TAU/72));
    for (uint32_t k=0;k<steps;++k)
        pan(steps==1 ? azimuth : azimuth-spread/2+spread*((float)k+0.5f)/(float)steps,1.0f/(float)steps,p_gains);
    for (uint32_t c=0;c<SPEAKERS;++c) p_gains[c]=sqrtf(p_gains[c])*p_object->gain;
}
/* Gains ramp across the frame from where the last one ended, so moving objects don't click. */
static void mix_object(float *p_out, uint32_t frames, Object *p_object) {
    if (!p_object->pcm.c_frames) return;
    Frame f=queue_pop(&p_object->pcm);
    float target[SPEAKERS];
    object_gains(p_object,target);
    for (uint32_t i=0;i<frames;++i) {
        float s=sample(&f,i,0), t=(float)(i+1)/(float)frames;
        for (uint32_t c=0;c<SPEAKERS;++c) p_out[i*SPEAKERS+c]+=s*(p_object->rendered[c]+(target[c]-p_object->rendered[c])*t);
    }
    memcpy(p_object->rendered,target,sizeof(target));
    ++*(p_object->passthrough!=PASSTHROUGH_NONE ? &s_c_object_frames_passed : &s_c_object_frames_placed);
    free(f.pcm);
}
static void fold_to_stereo(float *p_pcm, uint32_t frames) {
    for (uint32_t i=0;i<frames;++i) {
        const float *p_float=&p_pcm[i*SPEAKERS];
        float left=p_float[0]+0.7071f*(p_float[2]+p_float[4]+p_float[6]), right=p_float[1]+0.7071f*(p_float[2]+p_float[5]+p_float[7]);
        p_pcm[2*i]=left; p_pcm[2*i+1]=right;
    }
}
static int32_t mix(Port *p_port) {
    uint32_t frames=p_port->parameters.granularity;
    float *p_out=calloc(frames,SPEAKERS*sizeof(float));
    if (!p_out) return ERR_OUT_OF_MEMORY;
    mix_bed(p_out,frames,&p_port->beds);
    for (uint32_t i=0;i<p_port->parameters.max_objects;++i) {
        Object *p_object=&p_port->p_objects[i];
        if (!p_object->id) continue;
        mix_object(p_out,frames,p_object);
        if (p_object->unreserved && !p_object->pcm.c_frames) memset(p_object,0,sizeof(*p_object));
    }
    if (p_port->channels==2) fold_to_stereo(p_out,frames);
    queue_push(&p_port->mixed,(Frame){p_out,p_port->channels,1});
    ++s_c_frames_advanced;
    return 0;
}

/* The lock is released around each AudioOut call, which waits for the port's period. */
static int32_t drain_audio_out(uint32_t id) {
    for (;;) {
        Port *p_port=port_of(id);
        AudioOutPort *p_audio_out_port=NULL;
        for (int i=0;p_port && !p_audio_out_port && i<MAX_AUDIO_OUT;++i) if (p_port->audio_out[i].pending.c_frames) p_audio_out_port=&p_port->audio_out[i];
        if (!p_audio_out_port) return 0;
        int32_t h_handle=p_audio_out_port->h_handle;
        Frame f=queue_pop(&p_audio_out_port->pending);
        pthread_mutex_unlock(&s_lock);
        int32_t r=runtime_audio_output(h_handle,f.pcm);
        free(f.pcm);
        pthread_mutex_lock(&s_lock);
        if (r<0) return r;
    }
}
static int32_t open_output(Port *p_port) {
    if (p_port->output<0) {
        p_port->output=runtime_audio_open(USER_SYSTEM,AUDIO_OUT_TYPE_AUDIO3D,0,p_port->parameters.granularity,48000,
                                     p_port->channels==2 ? AUDIO_OUT_FLOAT_STEREO : AUDIO_OUT_FLOAT_8CH);
        if (p_port->output<0) { int32_t r=p_port->output; p_port->output=-1; return r; }
    }
    return 0;
}
static int32_t push_mixed(uint32_t id) {
    Port *p_port=port_of(id);
    if (!p_port || !p_port->mixed.c_frames) return 0;
    int32_t h_handle=p_port->output;
    Frame f=queue_pop(&p_port->mixed);
    pthread_mutex_unlock(&s_lock);
    int32_t r=runtime_audio_output(h_handle,f.pcm);
    free(f.pcm);
    pthread_mutex_lock(&s_lock);
    if (r>=0) ++s_c_frames_pushed;
    return r<0 ? r : 0;
}

static ABI int32_t audio3d_initialize(int64_t reserved) {
    if (reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&s_lock);
    int32_t r=s_is_initialized ? ERR_NOT_READY : 0;
    if (!r) {
        r=runtime_audio_init();
        if (r==AUDIO_OUT_ALREADY_INIT) r=0;
        s_is_initialized=r>=0;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static void close_port(Port *p_port) {
    if (p_port->output>=0) runtime_audio_close(p_port->output);
    for (int i=0;i<MAX_AUDIO_OUT;++i) if (p_port->audio_out[i].h_handle) {
        runtime_audio_close(p_port->audio_out[i].h_handle);
        queue_clear(&p_port->audio_out[i].pending);
    }
    queue_clear(&p_port->beds); queue_clear(&p_port->mixed);
    for (uint32_t i=0;i<p_port->parameters.max_objects;++i) queue_clear(&p_port->p_objects[i].pcm);
    free(p_port->p_objects);
    memset(p_port,0,sizeof(*p_port));
}
static ABI int32_t audio3d_terminate(void) {
    pthread_mutex_lock(&s_lock);
    int32_t r=s_is_initialized ? 0 : ERR_NOT_READY;
    for (int i=0;i<PORTS;++i) if (s_ports[i].used) close_port(&s_ports[i]);
    s_is_initialized=0;
    pthread_mutex_unlock(&s_lock);
    return r;
}
/* Only the 0x20 bytes that size_this announces: callers built before num_beds pass a 0x20-byte structure. */
static ABI int32_t audio3d_default_parameters(OpenParameters *p_parameters) {
    const OpenParameters defaults={0x20,0x100,0,MAX_OBJECTS,2,MODE_ADVANCE_AND_PUSH,0,0};
    if (p_parameters) memcpy(p_parameters,&defaults,0x20);
    return 0;
}
/* Fields past size_this keep their defaults; a structure without buffer_mode advances without pushing. */
static int32_t effective_parameters(const OpenParameters *p_open_parameters, OpenParameters *p_out) {
    *p_out=(OpenParameters){0x28,p_open_parameters->granularity,p_open_parameters->rate,MAX_OBJECTS,2,MODE_NO_ADVANCE,0,2};
    switch (p_open_parameters->size_this & ~UINT64_C(7)) {
    case 0x10: break;
    case 0x18: p_out->max_objects=p_open_parameters->max_objects; p_out->queue_depth=p_open_parameters->queue_depth; p_out->buffer_mode=MODE_ADVANCE_NO_PUSH; break;
    case 0x28: p_out->num_beds=p_open_parameters->num_beds; /* fall through */
    case 0x20: p_out->max_objects=p_open_parameters->max_objects; p_out->queue_depth=p_open_parameters->queue_depth; p_out->buffer_mode=p_open_parameters->buffer_mode; break;
    default: return ERR_INVALID_PARAMETER;
    }
    uint32_t g=p_out->granularity, depth=p_out->queue_depth;
    if (p_out->rate || g<0x100 || (g & 0xff) || !p_out->max_objects || !depth || p_out->buffer_mode>MODE_ADVANCE_AND_PUSH ||
        (p_out->num_beds & ~1u)!=2 || (g==0x100 && depth>0x40) || (g==0x200 && depth>0x1f) || (g==0x300 && depth>0x14) ||
        (g>0x3ff && depth>0xf))
        return ERR_INVALID_PARAMETER;
    if (p_out->max_objects>MAX_OBJECTS) p_out->max_objects=MAX_OBJECTS;
    return 0;
}
static ABI int32_t audio3d_port_open(int32_t user, const OpenParameters *p_parameters, uint32_t *p_id) {
    if (p_id) *p_id=0xffffffffu;
    if (user!=USER_SYSTEM || !p_parameters || !p_id) return ERR_INVALID_PARAMETER;
    OpenParameters effective;
    int32_t r=effective_parameters(p_parameters,&effective);
    if (r) return r;
    pthread_mutex_lock(&s_lock);
    int slot=-1;
    for (int i=0;s_is_initialized && slot<0 && i<PORTS;++i) if (!s_ports[i].used) slot=i;
    r=!s_is_initialized ? ERR_NOT_READY : slot<0 ? ERR_OUT_OF_RESOURCES : 0;
    Object *p_objects=r ? NULL : calloc(effective.max_objects,sizeof(Object));
    if (!r && !p_objects) r=ERR_OUT_OF_MEMORY;
    if (!r) {
        uint32_t channels=runtime_audio_device_channels()>=6 ? SPEAKERS : 2;
        s_ports[slot]=(Port){.used=1,.parameters=effective,.output=-1,.channels=channels,.p_objects=p_objects};
        *p_id=(uint32_t)slot;
        ++s_c_ports_opened;
        printf("Runtime: Audio3d port %d opened (granularity %u, queue depth %u, buffer mode %u, %u objects, %u ch out)\n",
               slot,effective.granularity,effective.queue_depth,effective.buffer_mode,effective.max_objects,channels);
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t audio3d_port_close(uint32_t id) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    if (p_port) close_port(p_port);
    pthread_mutex_unlock(&s_lock);
    return p_port ? 0 : ERR_INVALID_PORT;
}
static ABI int32_t audio3d_port_advance(uint32_t id) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    int32_t r=!p_port ? ERR_INVALID_PORT : p_port->parameters.buffer_mode==MODE_NO_ADVANCE ? ERR_NOT_SUPPORTED
              : p_port->mixed.c_frames>=p_port->parameters.queue_depth ? ERR_NOT_READY : mix(p_port);
    pthread_mutex_unlock(&s_lock);
    return r;
}
/* Sends a mixed frame only once the queue is full; a synchronous push sends until it has room again. */
static ABI int32_t audio3d_port_push(uint32_t id, uint32_t blocking) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    int32_t r=!p_port ? ERR_INVALID_PORT : p_port->parameters.buffer_mode!=MODE_ADVANCE_AND_PUSH ? ERR_NOT_SUPPORTED : 0;
    if (!r) r=drain_audio_out(id);
    if (!r && (p_port=port_of(id))) r=open_output(p_port);
    for (int first=1;!r && (p_port=port_of(id)) && p_port->mixed.c_frames>=p_port->parameters.queue_depth && (first || blocking==BLOCKING_SYNC);first=0)
        r=push_mixed(id);
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t audio3d_port_flush(uint32_t id) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    int32_t r=p_port ? 0 : ERR_INVALID_PORT;
    int associated=0;
    for (int i=0;p_port && i<MAX_AUDIO_OUT;++i) associated|=p_port->audio_out[i].h_handle!=0;
    if (!r && associated) {
        r=drain_audio_out(id);
        for (int i=0;!r && (p_port=port_of(id)) && i<MAX_AUDIO_OUT;++i)
            if (p_port->audio_out[i].h_handle) r=runtime_audio_output(p_port->audio_out[i].h_handle,NULL);
    } else if (!r) {
        int queued=p_port->beds.c_frames>0;
        for (uint32_t i=0;!queued && i<p_port->parameters.max_objects;++i) queued=p_port->p_objects[i].pcm.c_frames>0;
        if (!p_port->mixed.c_frames && queued) r=mix(p_port);
        if (!r && p_port->mixed.c_frames) r=open_output(p_port);
        while (!r && (p_port=port_of(id)) && p_port->mixed.c_frames) r=push_mixed(id);
    }
    pthread_mutex_unlock(&s_lock);
    return r<0 ? r : 0;
}
static ABI int32_t audio3d_port_queue_level(uint32_t id, uint32_t *p_level, uint32_t *p_available) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    int32_t r=!p_port ? ERR_INVALID_PORT : !p_level && !p_available ? ERR_INVALID_PARAMETER : 0;
    if (!r) {
        uint32_t c_mixed=p_port->mixed.c_frames, depth=p_port->parameters.queue_depth;
        if (p_level) *p_level=c_mixed;
        if (p_available) *p_available=c_mixed<depth ? depth-c_mixed : 0;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}

static ABI int32_t audio3d_object_reserve(uint32_t id, uint32_t *p_object) {
    if (!p_object) return ERR_INVALID_PARAMETER;
    *p_object=0xffffffffu;
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    int32_t r=p_port ? ERR_OUT_OF_RESOURCES : ERR_INVALID_PORT;
    for (uint32_t i=0;p_port && r && i<p_port->parameters.max_objects;++i) if (!p_port->p_objects[i].id) {
        p_port->serial=(p_port->serial+1) & 0x3fffff;
        p_port->p_objects[i]=(Object){.id=(p_port->serial<<10) | (i+1),.gain=1.0f};
        *p_object=p_port->p_objects[i].id;
        ++s_c_objects_reserved;
        r=0;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t audio3d_object_unreserve(uint32_t id, uint32_t object) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    Object *p_object=p_port ? object_of(p_port,object) : NULL;
    int32_t r=!p_port ? ERR_INVALID_PORT : !p_object || p_object->unreserved ? ERR_INVALID_OBJECT : 0;
    if (!r && p_object->pcm.c_frames) p_object->unreserved=1;
    else if (!r) memset(p_object,0,sizeof(*p_object));
    pthread_mutex_unlock(&s_lock);
    return r;
}
static int32_t set_attribute(Port *p_port, Object *p_object, uint32_t attribute, const void *p_value, uint64_t cb_value) {
    switch (attribute) {
    case ATTRIBUTE_RESET_STATE: {
        uint32_t id=p_object->id;
        queue_clear(&p_object->pcm);
        *p_object=(Object){.id=id,.gain=1.0f};
        return 0;
    }
    case ATTRIBUTE_PCM: {
        if (!p_value || cb_value<sizeof(Pcm)) return ERR_INVALID_PARAMETER;
        const Pcm *p_pcm=p_value;
        return enqueue(&p_object->pcm,p_port,p_pcm->format,p_pcm->samples,p_pcm->num_samples,1);
    }
    case ATTRIBUTE_GAIN:
        if (!p_value || cb_value<sizeof(float)) return ERR_INVALID_PARAMETER;
        memcpy(&p_object->gain,p_value,sizeof(float));
        return 0;
    case ATTRIBUTE_POSITION:
        if (!p_value || cb_value<sizeof(p_object->position)) return ERR_INVALID_PARAMETER;
        memcpy(p_object->position,p_value,sizeof(p_object->position));
        for (int i=0;i<3;++i) if (!isfinite(p_object->position[i])) p_object->position[i]=0;
        return 0;
    case ATTRIBUTE_SPREAD:
        if (!p_value || cb_value<sizeof(float)) return ERR_INVALID_PARAMETER;
        memcpy(&p_object->spread,p_value,sizeof(float));
        p_object->spread=p_object->spread>0 ? fminf(p_object->spread,TAU) : 0;
        return 0;
    /* Priority picks the objects the console drops when it runs out of voices; every object is rendered here. */
    case ATTRIBUTE_PRIORITY: return p_value && cb_value>=sizeof(uint32_t) ? 0 : ERR_INVALID_PARAMETER;
    case ATTRIBUTE_PASSTHROUGH:
        if (!p_value || cb_value<sizeof(uint32_t)) return ERR_INVALID_PARAMETER;
        memcpy(&p_object->passthrough,p_value,sizeof(uint32_t));
        if (p_object->passthrough>PASSTHROUGH_RIGHT) p_object->passthrough=PASSTHROUGH_NONE;
        return 0;
    default:
        if (attribute<32 && !(__atomic_fetch_or(&s_ignored_attributes,1u<<attribute,__ATOMIC_RELAXED) & (1u<<attribute)))
            printf("Runtime: Audio3d object attribute %u is accepted but not rendered\n",attribute);
        return 0;
    }
}
/* An object unreserved meanwhile (another thread) takes attributes as a no-op. */
static ABI int32_t audio3d_object_attribute(uint32_t id, uint32_t object, uint32_t attribute, const void *p_value, uint64_t cb_value) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    Object *p_object=p_port ? object_of(p_port,object) : NULL;
    s_c_orphan_attributes+=p_port && !p_object;
    int32_t r=!p_port ? ERR_INVALID_PORT : !p_object ? 0 : p_object->unreserved ? ERR_INVALID_OBJECT
              : !cb_value && attribute!=ATTRIBUTE_RESET_STATE ? ERR_INVALID_PARAMETER : set_attribute(p_port,p_object,attribute,p_value,cb_value);
    pthread_mutex_unlock(&s_lock);
    return r;
}
/* A reset in the list applies before the other attributes, wherever it stands. */
static ABI int32_t audio3d_object_attributes(uint32_t id, uint32_t object, uint64_t c_attributes, const Attribute *p_attributes) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    Object *p_object=p_port ? object_of(p_port,object) : NULL;
    int32_t r=!p_port ? ERR_INVALID_PORT : !c_attributes || !p_attributes ? ERR_INVALID_PARAMETER : !p_object ? 0 : p_object->unreserved ? ERR_INVALID_OBJECT : 0;
    s_c_orphan_attributes+=!r && !p_object;
    for (uint64_t i=0;!r && p_object && i<c_attributes;++i)
        if (p_attributes[i].id==ATTRIBUTE_RESET_STATE) { set_attribute(p_port,p_object,ATTRIBUTE_RESET_STATE,NULL,0); break; }
    for (uint64_t i=0;!r && p_object && i<c_attributes;++i) {
        if (p_attributes[i].id==ATTRIBUTE_RESET_STATE) continue;
        if (p_attributes[i].id==ATTRIBUTE_PCM && p_attributes[i].size<sizeof(Pcm))
            printf("Runtime: sceAudio3dObjectSetAttributes skipped a PCM attribute of %llu bytes\n",(unsigned long long)p_attributes[i].size);
        else r=set_attribute(p_port,p_object,p_attributes[i].id,p_attributes[i].value,p_attributes[i].size);
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}

/* A late reverb level of 0, FMOD's, is the dry mix rendered here. */
static ABI int32_t audio3d_port_attribute(uint32_t id, uint32_t attribute, const void *p_value, uint64_t cb_value) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    int32_t r=!p_port ? ERR_INVALID_PORT : !p_value ? ERR_INVALID_PARAMETER : 0;
    int rendered=1;
    if (!r && attribute==PORT_ATTRIBUTE_LATE_REVERB_LEVEL) {
        float level=-1;
        if (cb_value>=sizeof(float)) memcpy(&level,p_value,sizeof(float));
        if (!(level>=0 && level<=1)) r=ERR_INVALID_PARAMETER;
        else rendered=level==0;
    } else if (!r) rendered=0;
    pthread_mutex_unlock(&s_lock);
    unsigned bit=1u<<(attribute & 31);
    if (!rendered && !(__atomic_fetch_or(&s_ignored_port_attributes,bit,__ATOMIC_RELAXED) & bit))
        printf("Runtime: Audio3d port attribute 0x%x is accepted but not rendered\n",attribute);
    return r;
}

static ABI int32_t audio3d_bed_write2(uint32_t id, uint32_t channels, uint32_t format, const void *p_buffer, uint32_t samples,
                                      uint32_t route, int restricted) {
    (void)restricted;
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    int32_t r=!p_port ? ERR_INVALID_PORT : route>ROUTE_TV_ONLY || (channels!=2 && channels!=6 && channels!=8) ? ERR_INVALID_PARAMETER
              : enqueue(&p_port->beds,p_port,format,p_buffer,samples,channels);
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t audio3d_bed_write(uint32_t id, uint32_t channels, uint32_t format, const void *p_buffer, uint32_t samples) {
    return audio3d_bed_write2(id,channels,format,p_buffer,samples,0,0);
}

static ABI int32_t audio3d_audio_out_open(uint32_t id, int32_t user, int32_t type, int32_t i_device, uint32_t length,
                                          uint32_t freq, uint32_t param) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=port_of(id);
    AudioOutPort *p_audio_out_port=NULL;
    for (int i=0;p_port && !p_audio_out_port && i<MAX_AUDIO_OUT;++i) if (!p_port->audio_out[i].h_handle) p_audio_out_port=&p_port->audio_out[i];
    int32_t r=!p_port ? ERR_INVALID_PORT : length!=p_port->parameters.granularity ? ERR_INVALID_PARAMETER : !p_audio_out_port ? ERR_OUT_OF_RESOURCES
              : runtime_audio_open(user,type,i_device,length,freq,param);
    if (r>=0) {
        static const uint32_t s_channels[8]={1,2,8,1,2,8,8,8};
        uint32_t format=param & 0xff, is_float=format>=3 && format!=6;
        *p_audio_out_port=(AudioOutPort){.h_handle=r,.cb_frame=length*s_channels[format]*(is_float ? 4 : 2),.samples=length*s_channels[format],.is_float=is_float};
    }
    pthread_mutex_unlock(&s_lock);
    /* Its seventh argument travels on the stack, so the import thunks don't report its failures. */
    if (r<0) printf("Runtime: sceAudio3dAudioOutOpen(port %u, type %d, length %u, param 0x%x) failed 0x%08x\n",id,type,length,param,(unsigned)r);
    return r;
}
static AudioOutPort *audio_out_of(int32_t h_handle, Port **pp_port) {
    for (int i=0;h_handle && i<PORTS;++i)
        for (int j=0;s_ports[i].used && j<MAX_AUDIO_OUT;++j)
            if (s_ports[i].audio_out[j].h_handle==h_handle) { *pp_port=&s_ports[i]; return &s_ports[i].audio_out[j]; }
    return NULL;
}
static ABI int32_t audio3d_audio_out_close(int32_t h_handle) {
    pthread_mutex_lock(&s_lock);
    Port *p_port;
    AudioOutPort *p_audio_out_port=audio_out_of(h_handle,&p_port);
    if (p_audio_out_port) { queue_clear(&p_audio_out_port->pending); memset(p_audio_out_port,0,sizeof(*p_audio_out_port)); }
    pthread_mutex_unlock(&s_lock);
    return p_audio_out_port ? runtime_audio_close(h_handle) : ERR_INVALID_PORT;
}
static ABI int32_t audio3d_audio_out_output(int32_t h_handle, const void *p_data) {
    if (!p_data) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&s_lock);
    Port *p_port;
    AudioOutPort *p_audio_out_port=audio_out_of(h_handle,&p_port);
    int32_t r=!p_audio_out_port ? ERR_INVALID_PORT : misaligned(p_data,(int)p_audio_out_port->is_float) ? ERR_INVALID_PARAMETER
              : p_audio_out_port->pending.c_frames>=p_port->parameters.queue_depth ? ERR_NOT_READY : 0;
    void *p_copy=r ? NULL : malloc(p_audio_out_port->cb_frame);
    if (!r && !p_copy) r=ERR_OUT_OF_MEMORY;
    if (!r) {
        memcpy(p_copy,p_data,p_audio_out_port->cb_frame);
        queue_push(&p_audio_out_port->pending,(Frame){p_copy,0,p_audio_out_port->is_float});
        ++s_c_audio_out_buffers;
        r=(int32_t)p_audio_out_port->samples;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t audio3d_audio_out_outputs(const OutputParam *p_params, uint32_t c_params) {
    if (!p_params || !c_params) return ERR_INVALID_PARAMETER;
    for (uint32_t i=0;i<c_params;++i) { int32_t r=audio3d_audio_out_output(p_params[i].handle,p_params[i].data); if (r<0) return r; }
    return 0;
}

static const RuntimeExport s_exports[]={
    {"sceAudio3dInitialize",audio3d_initialize}, {"sceAudio3dTerminate",audio3d_terminate},
    {"sceAudio3dGetDefaultOpenParameters",audio3d_default_parameters},
    {"sceAudio3dPortOpen",audio3d_port_open}, {"sceAudio3dPortClose",audio3d_port_close},
    {"sceAudio3dPortAdvance",audio3d_port_advance}, {"sceAudio3dPortPush",audio3d_port_push},
    {"sceAudio3dPortFlush",audio3d_port_flush}, {"sceAudio3dPortGetQueueLevel",audio3d_port_queue_level},
    {"sceAudio3dPortSetAttribute",audio3d_port_attribute},
    {"sceAudio3dObjectReserve",audio3d_object_reserve}, {"sceAudio3dObjectUnreserve",audio3d_object_unreserve},
    {"sceAudio3dObjectSetAttribute",audio3d_object_attribute}, {"sceAudio3dObjectSetAttributes",audio3d_object_attributes},
    {"sceAudio3dBedWrite",audio3d_bed_write}, {"sceAudio3dBedWrite2",audio3d_bed_write2},
    {"sceAudio3dAudioOutOpen",audio3d_audio_out_open}, {"sceAudio3dAudioOutClose",audio3d_audio_out_close},
    {"sceAudio3dAudioOutOutput",audio3d_audio_out_output}, {"sceAudio3dAudioOutOutputs",audio3d_audio_out_outputs},
};
uintptr_t runtime_audio3d_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_audio3d_report(void) {
    printf("Runtime: Audio3d ports opened=%zu, frames advanced=%zu, pushed=%zu, objects reserved=%zu, object frames placed=%zu, "
           "passed through=%zu, AudioOut buffers=%zu, attributes on unreserved objects=%zu\n",
           s_c_ports_opened,s_c_frames_advanced,s_c_frames_pushed,s_c_objects_reserved,s_c_object_frames_placed,s_c_object_frames_passed,
           s_c_audio_out_buffers,s_c_orphan_attributes);
}
