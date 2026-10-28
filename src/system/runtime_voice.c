// SPDX-License-Identifier: GPL-2.0-or-later
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
#define VOICE_PORT_INVALID ((int32_t)0x804E0804)
#define VOICE_ARGUMENT_INVALID ((int32_t)0x804E0805)
#define VOICE_TOPOLOGY ((int32_t)0x804E0807)
#define VOICE_RESOURCE_INSUFFICIENT ((int32_t)0x804E0808)
#define VOICE_NO_PORT_TABLE ((int32_t)0x804E080A)
#define MAX_PORTS 64
#define PORT_IN_DEVICE 0
#define PORT_IN_PCMAUDIO 1
#define PORT_IN_VOICE 2
#define PORT_OUT_PCMAUDIO 3
#define PORT_OUT_VOICE 4
#define PORT_OUT_DEVICE 5
#define STATE_IDLE 0
#define STATE_READY 1
#define PCM_FRAME_BYTES 640 /* 20 ms of 16 kHz 16-bit mono */
#define CB_RING 0x800 /* device and voice ports; a high-quality instance's voice ports have 0x1800 */
#define ATTR_SILENT_STATE 1001

typedef struct {
    int32_t type;
    uint16_t threshold, muted;
    float volume;
    union {
        struct { int32_t bitrate; } voice;
        struct { uint32_t cb_buffer; int32_t data_type, sample_rate; } pcm;
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
    uint16_t c_edges;
} Port;

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_initialized, s_has_started;
static Port s_ports[MAX_PORTS];
static uint64_t s_unsupported_attributes;

static int is_input(int32_t type) { return type>=PORT_IN_DEVICE && type<=PORT_IN_VOICE; }
static int is_output(int32_t type) { return type>=PORT_OUT_PCMAUDIO && type<=PORT_OUT_DEVICE; }
static Port *find_port(uint32_t id) { return id<MAX_PORTS && s_ports[id].used ? &s_ports[id] : NULL; }
static int find_edge(const Port *p_port, uint32_t id) {
    for (int i=0;i<p_port->c_edges;++i)
        if (p_port->edges[i]==id) return i;
    return -1;
}
static void add_edge(Port *p_port, uint32_t id) { if (find_edge(p_port,id)<0) p_port->edges[p_port->c_edges++]=id; }
static void remove_edge(Port *p_port, uint32_t id) {
    const int i=find_edge(p_port,id);
    if (i>=0) p_port->edges[i]=p_port->edges[--p_port->c_edges];
}
static uint32_t frame_size(const PortParam *p_param) {
    if (p_param->type==PORT_IN_VOICE || p_param->type==PORT_OUT_VOICE)
        return p_param->voice.bitrate>0 ? ((uint32_t)p_param->voice.bitrate*20u+7999u)/8000u : 0;
    return PCM_FRAME_BYTES;
}
/* An input port's queued bytes and an output port's free space: nothing is ever queued here, so
   inputs hold none and an output's whole ring is free. */
static uint32_t info_bytes(const PortParam *p_param) {
    if (p_param->type==PORT_OUT_PCMAUDIO) return p_param->pcm.cb_buffer;
    return is_output(p_param->type) ? CB_RING : 0;
}

static ABI int32_t voice_init(const void *p_param) {
    if (!p_param) return VOICE_ARGUMENT_INVALID;
    pthread_mutex_lock(&s_lock);
    const int32_t result=s_is_initialized ? VOICE_INITIALIZED : 0;
    s_is_initialized=1;
    pthread_mutex_unlock(&s_lock);
    if (!result) puts("Runtime: Voice initialized; no microphone, no remote talkers");
    return result;
}
static ABI int32_t voice_end(void) {
    pthread_mutex_lock(&s_lock);
    const int32_t result=s_is_initialized ? 0 : VOICE_NOT_INIT;
    s_is_initialized=0; s_has_started=0;
    memset(s_ports,0,sizeof(s_ports));
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t voice_start(const void *p_param) {
    pthread_mutex_lock(&s_lock);
    int32_t result=!s_is_initialized ? VOICE_NOT_INIT : !p_param ? VOICE_ARGUMENT_INVALID : 0;
    if (!result) s_has_started=1;
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t voice_stop(void) {
    pthread_mutex_lock(&s_lock);
    const int32_t result=s_is_initialized ? 0 : VOICE_NOT_INIT;
    s_has_started=0;
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t voice_create_port(uint32_t *p_id, const PortParam *p_param) {
    if (!p_id || !p_param || !(is_input(p_param->type) || is_output(p_param->type))) return VOICE_ARGUMENT_INVALID;
    pthread_mutex_lock(&s_lock);
    int32_t result=s_is_initialized ? VOICE_RESOURCE_INSUFFICIENT : VOICE_NOT_INIT;
    uint32_t slot=0;
    while (s_is_initialized && slot<MAX_PORTS && s_ports[slot].used) ++slot;
    if (s_is_initialized && slot<MAX_PORTS) {
        memset(&s_ports[slot],0,sizeof(s_ports[slot]));
        s_ports[slot].used=1; s_ports[slot].param=*p_param;
        *p_id=slot; result=0;
    }
    pthread_mutex_unlock(&s_lock);
    if (!result) printf("Runtime: Voice port %u created (type %d)\n",slot,p_param->type);
    return result;
}
static ABI int32_t voice_delete_port(uint32_t id) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=s_is_initialized ? find_port(id) : NULL;
    const int32_t result=!s_is_initialized ? VOICE_NOT_INIT : !p_port ? VOICE_TOPOLOGY : 0;
    if (p_port) {
        for (int i=0;i<p_port->c_edges;++i) remove_edge(&s_ports[p_port->edges[i]],id);
        p_port->used=0;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static int32_t check_link(uint32_t in_id, uint32_t out_id, Port **pp_port, Port **pp_out) {
    if (!s_is_initialized) return VOICE_NOT_INIT;
    *pp_port=find_port(in_id); *pp_out=find_port(out_id);
    if (!*pp_port || !is_input((*pp_port)->param.type) || !*pp_out || !is_output((*pp_out)->param.type)) return VOICE_TOPOLOGY;
    return 0;
}
static ABI int32_t voice_connect(uint32_t in_id, uint32_t out_id) {
    Port *p_port, *p_out;
    pthread_mutex_lock(&s_lock);
    const int32_t result=check_link(in_id,out_id,&p_port,&p_out);
    if (!result) { add_edge(p_port,out_id); add_edge(p_out,in_id); }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t voice_disconnect(uint32_t in_id, uint32_t out_id) {
    Port *p_port, *p_out;
    pthread_mutex_lock(&s_lock);
    int32_t result=check_link(in_id,out_id,&p_port,&p_out);
    if (!result && find_edge(p_port,out_id)<0) result=VOICE_TOPOLOGY;
    if (!result) { remove_edge(p_port,out_id); remove_edge(p_out,in_id); }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t voice_port_info(uint32_t id, PortInfo *p_info) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=s_is_initialized ? find_port(id) : NULL;
    /* libSceVoice builds its port table in sceVoiceStart, which is also what makes a port READY. */
    const int32_t result=!s_is_initialized ? VOICE_NOT_INIT : id>=MAX_PORTS ? VOICE_PORT_INVALID : !p_info ? VOICE_ARGUMENT_INVALID
        : !s_has_started ? VOICE_NO_PORT_TABLE : !p_port ? VOICE_TOPOLOGY : 0;
    /* The edge pointer and count stay as the caller left them, since libSceVoice never writes them. */
    if (!result) {
        p_info->type=p_port->param.type;
        p_info->state=p_port->c_edges && !p_port->param.muted ? STATE_READY : STATE_IDLE;
        p_info->bytes=info_bytes(&p_port->param);
        p_info->frame_size=frame_size(&p_port->param);
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t voice_port_attr(uint32_t id, uint32_t attr, void *p_value, size_t cb_value) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=s_is_initialized ? find_port(id) : NULL;
    int32_t result=!s_is_initialized ? VOICE_NOT_INIT : !p_port ? VOICE_TOPOLOGY : !p_value || !cb_value ? VOICE_ARGUMENT_INVALID : 0;
    if (!result && attr==ATTR_SILENT_STATE) *(uint8_t *)p_value=1;
    else if (!result) result=VOICE_ARGUMENT_INVALID;
    const uint64_t bit=attr>=ATTR_SILENT_STATE-1 && attr<ATTR_SILENT_STATE+63 ? 1ull<<(attr-ATTR_SILENT_STATE+1) : 0;
    const int first=result==VOICE_ARGUMENT_INVALID && p_value && cb_value && !(s_unsupported_attributes&bit);
    s_unsupported_attributes|=first ? bit : 0;
    pthread_mutex_unlock(&s_lock);
    if (first) printf("Runtime: Voice port %u: attribute %u is not supported\n",id,attr);
    return result;
}
static ABI int32_t voice_read(uint32_t id, void *p_data, uint32_t *p_size) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=s_is_initialized ? find_port(id) : NULL;
    int32_t result=!s_is_initialized ? VOICE_NOT_INIT : !p_port || !is_output(p_port->param.type) ? VOICE_TOPOLOGY : 0;
    if (!result && (!p_data || !p_size)) result=VOICE_ARGUMENT_INVALID;
    if (!result) *p_size=0;
    pthread_mutex_unlock(&s_lock);
    return result;
}
static ABI int32_t voice_write(uint32_t id, const void *p_data, uint32_t *p_size) {
    pthread_mutex_lock(&s_lock);
    Port *p_port=s_is_initialized ? find_port(id) : NULL;
    int32_t result=!s_is_initialized ? VOICE_NOT_INIT : !p_port ? VOICE_TOPOLOGY : 0;
    if (!result && p_port->param.type!=PORT_IN_PCMAUDIO && p_port->param.type!=PORT_IN_VOICE) result=VOICE_TOPOLOGY;
    if (!result && (!p_data || !p_size)) result=VOICE_ARGUMENT_INVALID;
    pthread_mutex_unlock(&s_lock);
    return result;
}

static const RuntimeExport s_exports[]={
    {"sceVoiceInit",voice_init}, {"sceVoiceEnd",voice_end},
    {"sceVoiceStart",voice_start}, {"sceVoiceStop",voice_stop},
    {"sceVoiceCreatePort",voice_create_port}, {"sceVoiceDeletePort",voice_delete_port},
    {"sceVoiceConnectIPortToOPort",voice_connect}, {"sceVoiceDisconnectIPortFromOPort",voice_disconnect},
    {"sceVoiceGetPortInfo",voice_port_info}, {"sceVoiceGetPortAttr",voice_port_attr},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
};
uintptr_t runtime_voice_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
