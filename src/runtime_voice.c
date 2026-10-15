/* libSceVoice on a console with no microphone and no remote talker: ports and the graph that
 * connects them behave as on the console, an input device captures silence and an output port
 * never has anything to read. Error codes follow the 0x804E08xx family (ARGUMENT_INVALID is
 * 0x804E0805), in the same order as the PS3's libvoice. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define VOICE_NOT_INIT ((int32_t)0x804E0801)
#define VOICE_INITIALIZED ((int32_t)0x804E0802)
#define VOICE_ARGUMENT_INVALID ((int32_t)0x804E0805)
#define VOICE_TOPOLOGY ((int32_t)0x804E0807)
#define VOICE_RESOURCE_INSUFFICIENT ((int32_t)0x804E0808)
#define MAX_PORTS 128
#define PORT_IN_DEVICE 0
#define PORT_IN_PCMAUDIO 1
#define PORT_IN_VOICE 2
#define PORT_OUT_PCMAUDIO 3
#define PORT_OUT_VOICE 4
#define PORT_OUT_DEVICE 5
#define STATE_IDLE 0
#define STATE_READY 1
#define PCM_FRAME_BYTES 640 /* 20 ms of 16 kHz 16-bit mono */
#define ATTR_SILENT_STATE 1001

typedef struct {
    int32_t type;
    uint16_t threshold, muted;
    float volume;
    union {
        struct { int32_t bitrate; } voice;
        struct { uint32_t buffer_size; int32_t data_type, sample_rate; } pcm;
    };
} PortParam;
typedef struct {
    int32_t type, state;
    uint32_t *edges;
    uint32_t bytes, frame_size;
    uint16_t edge_count, reserved;
} PortInfo;
typedef struct {
    int used;
    PortParam param;
    uint32_t edges[MAX_PORTS];
    uint16_t edge_count;
} Port;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, started;
static Port ports[MAX_PORTS];
static uint64_t unsupported_attributes;

static int is_input(int32_t type) { return type>=PORT_IN_DEVICE && type<=PORT_IN_VOICE; }
static int is_output(int32_t type) { return type>=PORT_OUT_PCMAUDIO && type<=PORT_OUT_DEVICE; }
static Port *find_port(uint32_t id) { return id<MAX_PORTS && ports[id].used ? &ports[id] : NULL; }
static int find_edge(const Port *port, uint32_t id) {
    for (int i=0;i<port->edge_count;++i)
        if (port->edges[i]==id) return i;
    return -1;
}
static void add_edge(Port *port, uint32_t id) { if (find_edge(port,id)<0) port->edges[port->edge_count++]=id; }
static void remove_edge(Port *port, uint32_t id) {
    const int i=find_edge(port,id);
    if (i>=0) port->edges[i]=port->edges[--port->edge_count];
}
static uint32_t frame_size(const PortParam *param) {
    if (param->type==PORT_IN_VOICE || param->type==PORT_OUT_VOICE)
        return param->voice.bitrate>0 ? ((uint32_t)param->voice.bitrate*20u+7999u)/8000u : 0;
    return PCM_FRAME_BYTES;
}
static uint32_t writable_bytes(const PortParam *param) {
    if (param->type==PORT_IN_PCMAUDIO) return param->pcm.buffer_size;
    return param->type==PORT_IN_VOICE ? frame_size(param) : 0;
}

static ABI int32_t voice_init(const void *param) {
    if (!param) return VOICE_ARGUMENT_INVALID;
    pthread_mutex_lock(&lock);
    const int32_t result=initialized ? VOICE_INITIALIZED : 0;
    initialized=1;
    pthread_mutex_unlock(&lock);
    if (!result) puts("Runtime: Voice initialized; no microphone, no remote talkers");
    return result;
}
static ABI int32_t voice_end(void) {
    pthread_mutex_lock(&lock);
    const int32_t result=initialized ? 0 : VOICE_NOT_INIT;
    initialized=0; started=0;
    memset(ports,0,sizeof(ports));
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t voice_start(const void *param) {
    pthread_mutex_lock(&lock);
    int32_t result=!initialized ? VOICE_NOT_INIT : !param ? VOICE_ARGUMENT_INVALID : 0;
    if (!result) started=1;
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t voice_stop(void) {
    pthread_mutex_lock(&lock);
    const int32_t result=initialized ? 0 : VOICE_NOT_INIT;
    started=0;
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t voice_create_port(uint32_t *id, const PortParam *param) {
    if (!id || !param || !(is_input(param->type) || is_output(param->type))) return VOICE_ARGUMENT_INVALID;
    pthread_mutex_lock(&lock);
    int32_t result=initialized ? VOICE_RESOURCE_INSUFFICIENT : VOICE_NOT_INIT;
    uint32_t slot=0;
    while (initialized && slot<MAX_PORTS && ports[slot].used) ++slot;
    if (initialized && slot<MAX_PORTS) {
        memset(&ports[slot],0,sizeof(ports[slot]));
        ports[slot].used=1; ports[slot].param=*param;
        *id=slot; result=0;
    }
    pthread_mutex_unlock(&lock);
    if (!result) printf("Runtime: Voice port %u created (type %d)\n",slot,param->type);
    return result;
}
static ABI int32_t voice_delete_port(uint32_t id) {
    pthread_mutex_lock(&lock);
    Port *port=initialized ? find_port(id) : NULL;
    const int32_t result=!initialized ? VOICE_NOT_INIT : !port ? VOICE_TOPOLOGY : 0;
    if (port) {
        for (int i=0;i<port->edge_count;++i) remove_edge(&ports[port->edges[i]],id);
        port->used=0;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int32_t check_link(uint32_t in_id, uint32_t out_id, Port **in, Port **out) {
    if (!initialized) return VOICE_NOT_INIT;
    *in=find_port(in_id); *out=find_port(out_id);
    if (!*in || !is_input((*in)->param.type) || !*out || !is_output((*out)->param.type)) return VOICE_TOPOLOGY;
    return 0;
}
static ABI int32_t voice_connect(uint32_t in_id, uint32_t out_id) {
    Port *in, *out;
    pthread_mutex_lock(&lock);
    const int32_t result=check_link(in_id,out_id,&in,&out);
    if (!result) { add_edge(in,out_id); add_edge(out,in_id); }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t voice_disconnect(uint32_t in_id, uint32_t out_id) {
    Port *in, *out;
    pthread_mutex_lock(&lock);
    int32_t result=check_link(in_id,out_id,&in,&out);
    if (!result && find_edge(in,out_id)<0) result=VOICE_TOPOLOGY;
    if (!result) { remove_edge(in,out_id); remove_edge(out,in_id); }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t voice_port_info(uint32_t id, PortInfo *info) {
    pthread_mutex_lock(&lock);
    Port *port=initialized ? find_port(id) : NULL;
    const int32_t result=!initialized ? VOICE_NOT_INIT : !port ? VOICE_TOPOLOGY : !info ? VOICE_ARGUMENT_INVALID : 0;
    if (!result) {
        memset(info,0,sizeof(*info));
        info->type=port->param.type;
        info->state=started && port->edge_count ? STATE_READY : STATE_IDLE;
        info->edges=port->edges;
        info->bytes=writable_bytes(&port->param);
        info->frame_size=frame_size(&port->param);
        info->edge_count=port->edge_count;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t voice_port_attr(uint32_t id, uint32_t attr, void *value, size_t size) {
    pthread_mutex_lock(&lock);
    Port *port=initialized ? find_port(id) : NULL;
    int32_t result=!initialized ? VOICE_NOT_INIT : !port ? VOICE_TOPOLOGY : !value || !size ? VOICE_ARGUMENT_INVALID : 0;
    if (!result && attr==ATTR_SILENT_STATE) *(uint8_t *)value=1;
    else if (!result) result=VOICE_ARGUMENT_INVALID;
    const uint64_t bit=attr>=ATTR_SILENT_STATE-1 && attr<ATTR_SILENT_STATE+63 ? 1ull<<(attr-ATTR_SILENT_STATE+1) : 0;
    const int first=result==VOICE_ARGUMENT_INVALID && value && size && !(unsupported_attributes&bit);
    unsupported_attributes|=first ? bit : 0;
    pthread_mutex_unlock(&lock);
    if (first) printf("Runtime: Voice port %u: attribute %u is not supported\n",id,attr);
    return result;
}
static ABI int32_t voice_read(uint32_t id, void *data, uint32_t *size) {
    pthread_mutex_lock(&lock);
    Port *port=initialized ? find_port(id) : NULL;
    int32_t result=!initialized ? VOICE_NOT_INIT : !port || !is_output(port->param.type) ? VOICE_TOPOLOGY : 0;
    if (!result && (!data || !size)) result=VOICE_ARGUMENT_INVALID;
    if (!result) *size=0;
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t voice_write(uint32_t id, const void *data, uint32_t *size) {
    pthread_mutex_lock(&lock);
    Port *port=initialized ? find_port(id) : NULL;
    int32_t result=!initialized ? VOICE_NOT_INIT : !port ? VOICE_TOPOLOGY : 0;
    if (!result && port->param.type!=PORT_IN_PCMAUDIO && port->param.type!=PORT_IN_VOICE) result=VOICE_TOPOLOGY;
    if (!result && (!data || !size)) result=VOICE_ARGUMENT_INVALID;
    pthread_mutex_unlock(&lock);
    return result;
}

static const RuntimeExport exports[]={
    {"sceVoiceInit",voice_init}, {"sceVoiceEnd",voice_end},
    {"sceVoiceStart",voice_start}, {"sceVoiceStop",voice_stop},
    {"sceVoiceCreatePort",voice_create_port}, {"sceVoiceDeletePort",voice_delete_port},
    {"sceVoiceConnectIPortToOPort",voice_connect}, {"sceVoiceDisconnectIPortFromOPort",voice_disconnect},
    {"sceVoiceGetPortInfo",voice_port_info}, {"sceVoiceGetPortAttr",voice_port_attr},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
};
uintptr_t runtime_voice_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
