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
typedef struct { Frame frames[MAX_DEPTH]; uint32_t head, count; } Queue;
typedef struct {
    uint32_t id, passthrough;
    int unreserved;
    float gain, position[3], spread, rendered[SPEAKERS];
    Queue pcm;
} Object;
typedef struct { int32_t handle; uint32_t bytes, samples, is_float; Queue pending; } AudioOutPort;
typedef struct {
    int used;
    OpenParameters parameters;
    int32_t output;
    uint32_t serial, channels;
    Queue beds, mixed;
    Object *objects;
    AudioOutPort audio_out[MAX_AUDIO_OUT];
} Port;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static Port ports[PORTS];
static size_t ports_opened, frames_advanced, frames_pushed, audio_out_buffers, objects_reserved, orphan_attributes;
static size_t object_frames_placed, object_frames_passed;
static unsigned ignored_attributes, ignored_port_attributes;

static void queue_push(Queue *q, Frame f) { q->frames[(q->head+q->count++)%MAX_DEPTH]=f; }
static Frame queue_pop(Queue *q) {
    Frame f=q->frames[q->head];
    q->head=(q->head+1)%MAX_DEPTH; --q->count;
    return f;
}
static void queue_clear(Queue *q) { while (q->count) free(queue_pop(q).pcm); }

static Port *port_of(uint32_t id) { return initialized && id<PORTS && ports[id].used ? &ports[id] : NULL; }
static Object *object_of(Port *p, uint32_t id) {
    uint32_t slot=(id & 0x3ff)-1;
    return slot<p->parameters.max_objects && p->objects[slot].id==id ? &p->objects[slot] : NULL;
}
static int misaligned(const void *data, int is_float) { return ((uintptr_t)data & (is_float ? 3u : 1u))!=0; }

/* A frame of exactly `granularity` samples: a shorter one is padded with silence. */
static int32_t enqueue(Queue *q, const Port *p, uint32_t format, const void *samples, uint32_t count, uint32_t channels) {
    if (!samples || !count || format>FORMAT_FLOAT || misaligned(samples,format==FORMAT_FLOAT)) return ERR_INVALID_PARAMETER;
    if (q->count>=p->parameters.queue_depth) return ERR_NOT_READY;
    size_t sample_bytes=format==FORMAT_FLOAT ? 4 : 2, frame_bytes=channels*sample_bytes;
    void *pcm=calloc(p->parameters.granularity,frame_bytes);
    if (!pcm) return ERR_OUT_OF_MEMORY;
    memcpy(pcm,samples,(count<p->parameters.granularity ? count : p->parameters.granularity)*frame_bytes);
    queue_push(q,(Frame){pcm,channels,format==FORMAT_FLOAT});
    return 0;
}

static float sample(const Frame *f, uint32_t i, uint32_t c) {
    return f->is_float ? ((const float *)f->pcm)[i*f->channels+c] : ((const int16_t *)f->pcm)[i*f->channels+c]/32768.0f;
}
/* A stereo bed feeds L and R; 5.1 and 7.1 beds share AudioOut's 8-channel order (L R C LFE SL SR BL BR). */
static void mix_bed(float *out, uint32_t frames, Queue *q) {
    if (!q->count) return;
    Frame f=queue_pop(q);
    for (uint32_t i=0;i<frames;++i)
        for (uint32_t c=0;c<f.channels;++c) out[i*SPEAKERS+c]+=sample(&f,i,c);
    free(f.pcm);
}

static const struct { float azimuth; uint32_t speaker; } ring[]={
    {0,2}, {TAU/12,1}, {TAU/4,5}, {TAU*5/12,7}, {TAU*7/12,6}, {TAU*3/4,4}, {TAU*11/12,0},
};
#define RING (sizeof(ring)/sizeof(ring[0]))
static void pan(float azimuth, float weight, float *power) {
    float a=fmodf(azimuth,TAU);
    if (a<0) a+=TAU;
    uint32_t i=0;
    while (i+1<RING && a>=ring[i+1].azimuth) ++i;
    uint32_t next=(i+1)%RING;
    float end=next ? ring[next].azimuth : TAU, t=(a-ring[i].azimuth)/(end-ring[i].azimuth)*(TAU/4);
    power[ring[i].speaker]+=weight*cosf(t)*cosf(t);
    power[ring[next].speaker]+=weight*sinf(t)*sinf(t);
}
/* The ring is horizontal, so elevation widens the arc: an object overhead or at the listener covers all of it. */
static void object_gains(const Object *o, float *gains) {
    memset(gains,0,SPEAKERS*sizeof(float));
    if (o->passthrough!=PASSTHROUGH_NONE) { gains[o->passthrough==PASSTHROUGH_LEFT ? 0 : 1]=o->gain; return; }
    float x=o->position[0], y=o->position[1], z=o->position[2];
    float horizontal=sqrtf(x*x+z*z), distance=sqrtf(x*x+y*y+z*z);
    float spread=distance>1e-6f ? TAU*(1-horizontal/distance) : TAU;
    if (o->spread>spread) spread=o->spread;
    float azimuth=horizontal>1e-6f ? atan2f(x,z) : 0;
    uint32_t steps=1+(uint32_t)(spread/(TAU/72));
    for (uint32_t k=0;k<steps;++k)
        pan(steps==1 ? azimuth : azimuth-spread/2+spread*((float)k+0.5f)/(float)steps,1.0f/(float)steps,gains);
    for (uint32_t c=0;c<SPEAKERS;++c) gains[c]=sqrtf(gains[c])*o->gain;
}
/* Gains ramp across the frame from where the last one ended, so moving objects don't click. */
static void mix_object(float *out, uint32_t frames, Object *o) {
    if (!o->pcm.count) return;
    Frame f=queue_pop(&o->pcm);
    float target[SPEAKERS];
    object_gains(o,target);
    for (uint32_t i=0;i<frames;++i) {
        float s=sample(&f,i,0), t=(float)(i+1)/(float)frames;
        for (uint32_t c=0;c<SPEAKERS;++c) out[i*SPEAKERS+c]+=s*(o->rendered[c]+(target[c]-o->rendered[c])*t);
    }
    memcpy(o->rendered,target,sizeof(target));
    ++*(o->passthrough!=PASSTHROUGH_NONE ? &object_frames_passed : &object_frames_placed);
    free(f.pcm);
}
static void fold_to_stereo(float *pcm, uint32_t frames) {
    for (uint32_t i=0;i<frames;++i) {
        const float *s=&pcm[i*SPEAKERS];
        float left=s[0]+0.7071f*(s[2]+s[4]+s[6]), right=s[1]+0.7071f*(s[2]+s[5]+s[7]);
        pcm[2*i]=left; pcm[2*i+1]=right;
    }
}
static int32_t mix(Port *p) {
    uint32_t frames=p->parameters.granularity;
    float *out=calloc(frames,SPEAKERS*sizeof(float));
    if (!out) return ERR_OUT_OF_MEMORY;
    mix_bed(out,frames,&p->beds);
    for (uint32_t i=0;i<p->parameters.max_objects;++i) {
        Object *o=&p->objects[i];
        if (!o->id) continue;
        mix_object(out,frames,o);
        if (o->unreserved && !o->pcm.count) memset(o,0,sizeof(*o));
    }
    if (p->channels==2) fold_to_stereo(out,frames);
    queue_push(&p->mixed,(Frame){out,p->channels,1});
    ++frames_advanced;
    return 0;
}

/* The lock is released around each AudioOut call, which waits for the port's period. */
static int32_t drain_audio_out(uint32_t id) {
    for (;;) {
        Port *p=port_of(id);
        AudioOutPort *a=NULL;
        for (int i=0;p && !a && i<MAX_AUDIO_OUT;++i) if (p->audio_out[i].pending.count) a=&p->audio_out[i];
        if (!a) return 0;
        int32_t handle=a->handle;
        Frame f=queue_pop(&a->pending);
        pthread_mutex_unlock(&lock);
        int32_t r=runtime_audio_output(handle,f.pcm);
        free(f.pcm);
        pthread_mutex_lock(&lock);
        if (r<0) return r;
    }
}
static int32_t open_output(Port *p) {
    if (p->output<0) {
        p->output=runtime_audio_open(USER_SYSTEM,AUDIO_OUT_TYPE_AUDIO3D,0,p->parameters.granularity,48000,
                                     p->channels==2 ? AUDIO_OUT_FLOAT_STEREO : AUDIO_OUT_FLOAT_8CH);
        if (p->output<0) { int32_t r=p->output; p->output=-1; return r; }
    }
    return 0;
}
static int32_t push_mixed(uint32_t id) {
    Port *p=port_of(id);
    if (!p || !p->mixed.count) return 0;
    int32_t handle=p->output;
    Frame f=queue_pop(&p->mixed);
    pthread_mutex_unlock(&lock);
    int32_t r=runtime_audio_output(handle,f.pcm);
    free(f.pcm);
    pthread_mutex_lock(&lock);
    if (r>=0) ++frames_pushed;
    return r<0 ? r : 0;
}

static ABI int32_t audio3d_initialize(int64_t reserved) {
    if (reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    int32_t r=initialized ? ERR_NOT_READY : 0;
    if (!r) {
        r=runtime_audio_init();
        if (r==AUDIO_OUT_ALREADY_INIT) r=0;
        initialized=r>=0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static void close_port(Port *p) {
    if (p->output>=0) runtime_audio_close(p->output);
    for (int i=0;i<MAX_AUDIO_OUT;++i) if (p->audio_out[i].handle) {
        runtime_audio_close(p->audio_out[i].handle);
        queue_clear(&p->audio_out[i].pending);
    }
    queue_clear(&p->beds); queue_clear(&p->mixed);
    for (uint32_t i=0;i<p->parameters.max_objects;++i) queue_clear(&p->objects[i].pcm);
    free(p->objects);
    memset(p,0,sizeof(*p));
}
static ABI int32_t audio3d_terminate(void) {
    pthread_mutex_lock(&lock);
    int32_t r=initialized ? 0 : ERR_NOT_READY;
    for (int i=0;i<PORTS;++i) if (ports[i].used) close_port(&ports[i]);
    initialized=0;
    pthread_mutex_unlock(&lock);
    return r;
}
/* Only the 0x20 bytes that size_this announces: callers built before num_beds pass a 0x20-byte structure. */
static ABI int32_t audio3d_default_parameters(OpenParameters *parameters) {
    const OpenParameters defaults={0x20,0x100,0,MAX_OBJECTS,2,MODE_ADVANCE_AND_PUSH,0,0};
    if (parameters) memcpy(parameters,&defaults,0x20);
    return 0;
}
/* Fields past size_this keep their defaults; a structure without buffer_mode advances without pushing. */
static int32_t effective_parameters(const OpenParameters *in, OpenParameters *out) {
    *out=(OpenParameters){0x28,in->granularity,in->rate,MAX_OBJECTS,2,MODE_NO_ADVANCE,0,2};
    switch (in->size_this & ~UINT64_C(7)) {
    case 0x10: break;
    case 0x18: out->max_objects=in->max_objects; out->queue_depth=in->queue_depth; out->buffer_mode=MODE_ADVANCE_NO_PUSH; break;
    case 0x28: out->num_beds=in->num_beds; /* fall through */
    case 0x20: out->max_objects=in->max_objects; out->queue_depth=in->queue_depth; out->buffer_mode=in->buffer_mode; break;
    default: return ERR_INVALID_PARAMETER;
    }
    uint32_t g=out->granularity, depth=out->queue_depth;
    if (out->rate || g<0x100 || (g & 0xff) || !out->max_objects || !depth || out->buffer_mode>MODE_ADVANCE_AND_PUSH ||
        (out->num_beds & ~1u)!=2 || (g==0x100 && depth>0x40) || (g==0x200 && depth>0x1f) || (g==0x300 && depth>0x14) ||
        (g>0x3ff && depth>0xf))
        return ERR_INVALID_PARAMETER;
    if (out->max_objects>MAX_OBJECTS) out->max_objects=MAX_OBJECTS;
    return 0;
}
static ABI int32_t audio3d_port_open(int32_t user, const OpenParameters *parameters, uint32_t *id) {
    if (id) *id=0xffffffffu;
    if (user!=USER_SYSTEM || !parameters || !id) return ERR_INVALID_PARAMETER;
    OpenParameters effective;
    int32_t r=effective_parameters(parameters,&effective);
    if (r) return r;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;initialized && slot<0 && i<PORTS;++i) if (!ports[i].used) slot=i;
    r=!initialized ? ERR_NOT_READY : slot<0 ? ERR_OUT_OF_RESOURCES : 0;
    Object *objects=r ? NULL : calloc(effective.max_objects,sizeof(Object));
    if (!r && !objects) r=ERR_OUT_OF_MEMORY;
    if (!r) {
        uint32_t channels=runtime_audio_device_channels()>=6 ? SPEAKERS : 2;
        ports[slot]=(Port){.used=1,.parameters=effective,.output=-1,.channels=channels,.objects=objects};
        *id=(uint32_t)slot;
        ++ports_opened;
        printf("Runtime: Audio3d port %d opened (granularity %u, queue depth %u, buffer mode %u, %u objects, %u ch out)\n",
               slot,effective.granularity,effective.queue_depth,effective.buffer_mode,effective.max_objects,channels);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t audio3d_port_close(uint32_t id) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    if (p) close_port(p);
    pthread_mutex_unlock(&lock);
    return p ? 0 : ERR_INVALID_PORT;
}
static ABI int32_t audio3d_port_advance(uint32_t id) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    int32_t r=!p ? ERR_INVALID_PORT : p->parameters.buffer_mode==MODE_NO_ADVANCE ? ERR_NOT_SUPPORTED
              : p->mixed.count>=p->parameters.queue_depth ? ERR_NOT_READY : mix(p);
    pthread_mutex_unlock(&lock);
    return r;
}
/* Sends a mixed frame only once the queue is full; a synchronous push sends until it has room again. */
static ABI int32_t audio3d_port_push(uint32_t id, uint32_t blocking) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    int32_t r=!p ? ERR_INVALID_PORT : p->parameters.buffer_mode!=MODE_ADVANCE_AND_PUSH ? ERR_NOT_SUPPORTED : 0;
    if (!r) r=drain_audio_out(id);
    if (!r && (p=port_of(id))) r=open_output(p);
    for (int first=1;!r && (p=port_of(id)) && p->mixed.count>=p->parameters.queue_depth && (first || blocking==BLOCKING_SYNC);first=0)
        r=push_mixed(id);
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t audio3d_port_flush(uint32_t id) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    int32_t r=p ? 0 : ERR_INVALID_PORT;
    int associated=0;
    for (int i=0;p && i<MAX_AUDIO_OUT;++i) associated|=p->audio_out[i].handle!=0;
    if (!r && associated) {
        r=drain_audio_out(id);
        for (int i=0;!r && (p=port_of(id)) && i<MAX_AUDIO_OUT;++i)
            if (p->audio_out[i].handle) r=runtime_audio_output(p->audio_out[i].handle,NULL);
    } else if (!r) {
        int queued=p->beds.count>0;
        for (uint32_t i=0;!queued && i<p->parameters.max_objects;++i) queued=p->objects[i].pcm.count>0;
        if (!p->mixed.count && queued) r=mix(p);
        if (!r && p->mixed.count) r=open_output(p);
        while (!r && (p=port_of(id)) && p->mixed.count) r=push_mixed(id);
    }
    pthread_mutex_unlock(&lock);
    return r<0 ? r : 0;
}
static ABI int32_t audio3d_port_queue_level(uint32_t id, uint32_t *level, uint32_t *available) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    int32_t r=!p ? ERR_INVALID_PORT : !level && !available ? ERR_INVALID_PARAMETER : 0;
    if (!r) {
        uint32_t count=p->mixed.count, depth=p->parameters.queue_depth;
        if (level) *level=count;
        if (available) *available=count<depth ? depth-count : 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

static ABI int32_t audio3d_object_reserve(uint32_t id, uint32_t *object) {
    if (!object) return ERR_INVALID_PARAMETER;
    *object=0xffffffffu;
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    int32_t r=p ? ERR_OUT_OF_RESOURCES : ERR_INVALID_PORT;
    for (uint32_t i=0;p && r && i<p->parameters.max_objects;++i) if (!p->objects[i].id) {
        p->serial=(p->serial+1) & 0x3fffff;
        p->objects[i]=(Object){.id=(p->serial<<10) | (i+1),.gain=1.0f};
        *object=p->objects[i].id;
        ++objects_reserved;
        r=0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t audio3d_object_unreserve(uint32_t id, uint32_t object) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    Object *o=p ? object_of(p,object) : NULL;
    int32_t r=!p ? ERR_INVALID_PORT : !o || o->unreserved ? ERR_INVALID_OBJECT : 0;
    if (!r && o->pcm.count) o->unreserved=1;
    else if (!r) memset(o,0,sizeof(*o));
    pthread_mutex_unlock(&lock);
    return r;
}
static int32_t set_attribute(Port *p, Object *o, uint32_t attribute, const void *value, uint64_t size) {
    switch (attribute) {
    case ATTRIBUTE_RESET_STATE: {
        uint32_t id=o->id;
        queue_clear(&o->pcm);
        *o=(Object){.id=id,.gain=1.0f};
        return 0;
    }
    case ATTRIBUTE_PCM: {
        if (!value || size<sizeof(Pcm)) return ERR_INVALID_PARAMETER;
        const Pcm *pcm=value;
        return enqueue(&o->pcm,p,pcm->format,pcm->samples,pcm->num_samples,1);
    }
    case ATTRIBUTE_GAIN:
        if (!value || size<sizeof(float)) return ERR_INVALID_PARAMETER;
        memcpy(&o->gain,value,sizeof(float));
        return 0;
    case ATTRIBUTE_POSITION:
        if (!value || size<sizeof(o->position)) return ERR_INVALID_PARAMETER;
        memcpy(o->position,value,sizeof(o->position));
        for (int i=0;i<3;++i) if (!isfinite(o->position[i])) o->position[i]=0;
        return 0;
    case ATTRIBUTE_SPREAD:
        if (!value || size<sizeof(float)) return ERR_INVALID_PARAMETER;
        memcpy(&o->spread,value,sizeof(float));
        o->spread=o->spread>0 ? fminf(o->spread,TAU) : 0;
        return 0;
    /* Priority picks the objects the console drops when it runs out of voices; every object is rendered here. */
    case ATTRIBUTE_PRIORITY: return value && size>=sizeof(uint32_t) ? 0 : ERR_INVALID_PARAMETER;
    case ATTRIBUTE_PASSTHROUGH:
        if (!value || size<sizeof(uint32_t)) return ERR_INVALID_PARAMETER;
        memcpy(&o->passthrough,value,sizeof(uint32_t));
        if (o->passthrough>PASSTHROUGH_RIGHT) o->passthrough=PASSTHROUGH_NONE;
        return 0;
    default:
        if (attribute<32 && !(__atomic_fetch_or(&ignored_attributes,1u<<attribute,__ATOMIC_RELAXED) & (1u<<attribute)))
            printf("Runtime: Audio3d object attribute %u is accepted but not rendered\n",attribute);
        return 0;
    }
}
/* An object unreserved meanwhile (another thread) takes attributes as a no-op. */
static ABI int32_t audio3d_object_attribute(uint32_t id, uint32_t object, uint32_t attribute, const void *value, uint64_t size) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    Object *o=p ? object_of(p,object) : NULL;
    orphan_attributes+=p && !o;
    int32_t r=!p ? ERR_INVALID_PORT : !o ? 0 : o->unreserved ? ERR_INVALID_OBJECT
              : !size && attribute!=ATTRIBUTE_RESET_STATE ? ERR_INVALID_PARAMETER : set_attribute(p,o,attribute,value,size);
    pthread_mutex_unlock(&lock);
    return r;
}
/* A reset in the list applies before the other attributes, wherever it stands. */
static ABI int32_t audio3d_object_attributes(uint32_t id, uint32_t object, uint64_t count, const Attribute *attributes) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    Object *o=p ? object_of(p,object) : NULL;
    int32_t r=!p ? ERR_INVALID_PORT : !count || !attributes ? ERR_INVALID_PARAMETER : !o ? 0 : o->unreserved ? ERR_INVALID_OBJECT : 0;
    orphan_attributes+=!r && !o;
    for (uint64_t i=0;!r && o && i<count;++i)
        if (attributes[i].id==ATTRIBUTE_RESET_STATE) { set_attribute(p,o,ATTRIBUTE_RESET_STATE,NULL,0); break; }
    for (uint64_t i=0;!r && o && i<count;++i) {
        if (attributes[i].id==ATTRIBUTE_RESET_STATE) continue;
        if (attributes[i].id==ATTRIBUTE_PCM && attributes[i].size<sizeof(Pcm))
            printf("Runtime: sceAudio3dObjectSetAttributes skipped a PCM attribute of %llu bytes\n",(unsigned long long)attributes[i].size);
        else r=set_attribute(p,o,attributes[i].id,attributes[i].value,attributes[i].size);
    }
    pthread_mutex_unlock(&lock);
    return r;
}

/* A late reverb level of 0, FMOD's, is the dry mix rendered here. */
static ABI int32_t audio3d_port_attribute(uint32_t id, uint32_t attribute, const void *value, uint64_t size) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    int32_t r=!p ? ERR_INVALID_PORT : !value ? ERR_INVALID_PARAMETER : 0;
    int rendered=1;
    if (!r && attribute==PORT_ATTRIBUTE_LATE_REVERB_LEVEL) {
        float level=-1;
        if (size>=sizeof(float)) memcpy(&level,value,sizeof(float));
        if (!(level>=0 && level<=1)) r=ERR_INVALID_PARAMETER;
        else rendered=level==0;
    } else if (!r) rendered=0;
    pthread_mutex_unlock(&lock);
    unsigned bit=1u<<(attribute & 31);
    if (!rendered && !(__atomic_fetch_or(&ignored_port_attributes,bit,__ATOMIC_RELAXED) & bit))
        printf("Runtime: Audio3d port attribute 0x%x is accepted but not rendered\n",attribute);
    return r;
}

static ABI int32_t audio3d_bed_write2(uint32_t id, uint32_t channels, uint32_t format, const void *buffer, uint32_t samples,
                                      uint32_t route, int restricted) {
    (void)restricted;
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    int32_t r=!p ? ERR_INVALID_PORT : route>ROUTE_TV_ONLY || (channels!=2 && channels!=6 && channels!=8) ? ERR_INVALID_PARAMETER
              : enqueue(&p->beds,p,format,buffer,samples,channels);
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t audio3d_bed_write(uint32_t id, uint32_t channels, uint32_t format, const void *buffer, uint32_t samples) {
    return audio3d_bed_write2(id,channels,format,buffer,samples,0,0);
}

static ABI int32_t audio3d_audio_out_open(uint32_t id, int32_t user, int32_t type, int32_t index, uint32_t length,
                                          uint32_t freq, uint32_t param) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    AudioOutPort *a=NULL;
    for (int i=0;p && !a && i<MAX_AUDIO_OUT;++i) if (!p->audio_out[i].handle) a=&p->audio_out[i];
    int32_t r=!p ? ERR_INVALID_PORT : length!=p->parameters.granularity ? ERR_INVALID_PARAMETER : !a ? ERR_OUT_OF_RESOURCES
              : runtime_audio_open(user,type,index,length,freq,param);
    if (r>=0) {
        static const uint32_t channels[8]={1,2,8,1,2,8,8,8};
        uint32_t format=param & 0xff, is_float=format>=3 && format!=6;
        *a=(AudioOutPort){.handle=r,.bytes=length*channels[format]*(is_float ? 4 : 2),.samples=length*channels[format],.is_float=is_float};
    }
    pthread_mutex_unlock(&lock);
    /* Its seventh argument travels on the stack, so the import thunks don't report its failures. */
    if (r<0) printf("Runtime: sceAudio3dAudioOutOpen(port %u, type %d, length %u, param 0x%x) failed 0x%08x\n",id,type,length,param,(unsigned)r);
    return r;
}
static AudioOutPort *audio_out_of(int32_t handle, Port **port) {
    for (int i=0;handle && i<PORTS;++i)
        for (int j=0;ports[i].used && j<MAX_AUDIO_OUT;++j)
            if (ports[i].audio_out[j].handle==handle) { *port=&ports[i]; return &ports[i].audio_out[j]; }
    return NULL;
}
static ABI int32_t audio3d_audio_out_close(int32_t handle) {
    pthread_mutex_lock(&lock);
    Port *p;
    AudioOutPort *a=audio_out_of(handle,&p);
    if (a) { queue_clear(&a->pending); memset(a,0,sizeof(*a)); }
    pthread_mutex_unlock(&lock);
    return a ? runtime_audio_close(handle) : ERR_INVALID_PORT;
}
static ABI int32_t audio3d_audio_out_output(int32_t handle, const void *data) {
    if (!data) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Port *p;
    AudioOutPort *a=audio_out_of(handle,&p);
    int32_t r=!a ? ERR_INVALID_PORT : misaligned(data,(int)a->is_float) ? ERR_INVALID_PARAMETER
              : a->pending.count>=p->parameters.queue_depth ? ERR_NOT_READY : 0;
    void *copy=r ? NULL : malloc(a->bytes);
    if (!r && !copy) r=ERR_OUT_OF_MEMORY;
    if (!r) {
        memcpy(copy,data,a->bytes);
        queue_push(&a->pending,(Frame){copy,0,a->is_float});
        ++audio_out_buffers;
        r=(int32_t)a->samples;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t audio3d_audio_out_outputs(const OutputParam *params, uint32_t count) {
    if (!params || !count) return ERR_INVALID_PARAMETER;
    for (uint32_t i=0;i<count;++i) { int32_t r=audio3d_audio_out_output(params[i].handle,params[i].data); if (r<0) return r; }
    return 0;
}

static const RuntimeExport exports[]={
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
uintptr_t runtime_audio3d_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
void runtime_audio3d_report(void) {
    printf("Runtime: Audio3d ports opened=%zu, frames advanced=%zu, pushed=%zu, objects reserved=%zu, object frames placed=%zu, "
           "passed through=%zu, AudioOut buffers=%zu, attributes on unreserved objects=%zu\n",
           ports_opened,frames_advanced,frames_pushed,objects_reserved,object_frames_placed,object_frames_passed,
           audio_out_buffers,orphan_attributes);
}
