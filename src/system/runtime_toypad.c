/* The LEGO Dimensions toy pad, a USB peripheral emulated behind libSceUsbd for any game that looks
 * for it, offered only with PS_TOYPAD=1 (else toypad=1 in psport.ini): otherwise these exports stay
 * out and runtime_devices.c answers that no USB device is plugged in.
 *
 * The pad is a USB HID device (vendor 0x0E6F, product 0x0241) with one interrupt endpoint each way
 * and 32-byte reports. The game writes requests [0x55, length, command, sequence, payload...,
 * checksum] and reads replies [0x55, length, sequence, payload..., checksum]; figures placed on or
 * lifted from the pad come as unsolicited [0x56, ...] reports. The pad proves itself with the
 * seed/challenge commands (TEA with the pad's command key, a small 32-bit generator).
 *
 * Figures are virtual: a text file lists what stands on the pad, one "<zone> <figure id>" per line
 * (zone 1: the centre, 2: left, 3: right; '#' starts a comment), read again whenever it changes, so
 * figures can be put on and lifted off while the game runs (the overlay's Toy pad section edits
 * it). Each one is a 45-page NTAG213 image kept in <user>/toypad/<id>-<copy>.bin: made on first use
 * (a character's id encrypted with the key of its tag UID, the tag's password), then written as the
 * game writes the tag (vehicles, upgrades). The file: PS_TOYPAD_FILE, else <user>/toypad/figures.txt. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_usbd.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

enum { VENDOR=0x0E6F, PRODUCT=0x0241, ENDPOINT_IN=0x81, ENDPOINT_OUT=0x01, REPORT=32 };
enum { TRANSFER_COMPLETED=0, TRANSFER_TIMED_OUT=2, TRANSFER_CANCELLED=3 };
typedef void (ABI *UsbdTransferCallback)(UsbdTransfer *);

/* libusb 1.0's descriptors, as the PS4's library lays them out. */
typedef struct {
    uint8_t length, type, endpoint, attributes;
    uint16_t max_packet;
    uint8_t interval, refresh, synch_address;
    const uint8_t *p_extra;
    int32_t cb_extra;
} EndpointDescriptor;
typedef struct {
    uint8_t length, type, number, alternate, c_endpoints, class_, subclass, protocol, name;
    const EndpointDescriptor *p_endpoints;
    const uint8_t *p_extra;
    int32_t cb_extra;
} InterfaceDescriptor;
typedef struct { const InterfaceDescriptor *p_altsettings; int32_t c_altsettings; } Interface;
typedef struct {
    uint8_t length, type;
    uint16_t total_length;
    uint8_t c_interfaces, value, name, attributes, max_power;
    const Interface *p_interfaces;
    const uint8_t *p_extra;
    int32_t cb_extra;
} ConfigDescriptor;
typedef struct {
    uint8_t length, type;
    uint16_t usb;
    uint8_t class_, subclass, protocol, max_packet0;
    uint16_t vendor, product, device;
    uint8_t manufacturer, product_name, serial, c_configurations;
} DeviceDescriptor;
_Static_assert(sizeof(DeviceDescriptor)==18,"libusb_device_descriptor layout");
_Static_assert(__builtin_offsetof(ConfigDescriptor,p_interfaces)==16,"libusb_config_descriptor layout");
_Static_assert(__builtin_offsetof(InterfaceDescriptor,p_endpoints)==16,"libusb_interface_descriptor layout");

static const EndpointDescriptor s_endpoints[2]={
    {7,5,ENDPOINT_IN,3,REPORT,1,0,0,NULL,0},
    {7,5,ENDPOINT_OUT,3,REPORT,1,0,0,NULL,0},
};
static const InterfaceDescriptor s_interface_descriptor={9,4,0,0,2,3,0,0,0,s_endpoints,NULL,0};
static const Interface s_interfaces[1]={{&s_interface_descriptor,1}};
static const ConfigDescriptor s_config={9,2,0x29,1,1,0,0x80,0xFA,s_interfaces,NULL,0};

/* The device and its handle are objects of our own: only their addresses matter to the game. */
typedef struct { uint32_t magic; } ToypadObject;
static ToypadObject s_device={0x544f5950}, s_handle={0x48414e44};

/* ---- The pad's protocol ---- */

static const uint32_t s_command_key[4]={0xB0F6FE55,0x410BBF62,0xB47CB3C9,0x7B293E97};

static void tea_encrypt_with(uint8_t data[8], const uint32_t key[4]) {
    uint32_t a, b, sum=0;
    memcpy(&a,data,4); memcpy(&b,data+4,4);
    for (int i=0;i<32;++i) {
        sum+=0x9E3779B9;
        a+=((b<<4)+key[0])^(b+sum)^((b>>5)+key[1]);
        b+=((a<<4)+key[2])^(a+sum)^((a>>5)+key[3]);
    }
    memcpy(data,&a,4); memcpy(data+4,&b,4);
}
static void tea_decrypt(uint8_t data[8]) {
    uint32_t a, b, sum=0xC6EF3720;
    memcpy(&a,data,4); memcpy(&b,data+4,4);
    for (int i=0;i<32;++i) {
        b-=((a<<4)+s_command_key[2])^(a+sum)^((a>>5)+s_command_key[3]);
        a-=((b<<4)+s_command_key[0])^(b+sum)^((b>>5)+s_command_key[1]);
        sum-=0x9E3779B9;
    }
    memcpy(data,&a,4); memcpy(data+4,&b,4);
}
static uint32_t rotate_left(uint32_t v, int n) { return (v<<n)|(v>>(32-n)); }
static uint32_t rotate_right(uint32_t v, int n) { return (v>>n)|(v<<(32-n)); }

/* The pad's random generator, seeded by the game through command 0xB1. */
static uint32_t s_rng[4];
static uint32_t next_random(void) {
    const uint32_t e=s_rng[0]-rotate_left(s_rng[1],21);
    s_rng[0]=s_rng[1]^rotate_left(s_rng[2],19);
    s_rng[1]=s_rng[2]+rotate_left(s_rng[3],6);
    s_rng[2]=s_rng[3]+e;
    s_rng[3]=e+s_rng[0];
    return s_rng[3];
}
static void seed_random(uint32_t seed) {
    s_rng[0]=0xF1EA5EED;
    s_rng[1]=s_rng[2]=s_rng[3]=seed;
    for (int i=0;i<42;++i) next_random();
}
static uint8_t checksum(const uint8_t *p_data, int c_bytes) {
    unsigned sum=0;
    for (int i=0;i<c_bytes;++i) sum+=p_data[i];
    return (uint8_t)sum;
}

/* ---- Figures ---- */

enum { PAGES=45, SLOTS=7, VEHICLE_IDS=1000 };
typedef struct {
    int is_present;
    uint8_t zone, i_reader; /* zone 1..3, the pad's reader index 1..7 */
    uint32_t id, copy;
    uint8_t data[PAGES*4];
} Figure;
static Figure s_figures[SLOTS];

/* The tags' hash over little-endian words of a key. */
static uint32_t tag_hash(const uint8_t *p_key, int c_words) {
    uint32_t h=0;
    for (int i=0;i<c_words;++i) {
        uint32_t w;
        memcpy(&w,p_key+i*4,4);
        h=w+rotate_right(h,25)+rotate_right(h,10)-h;
    }
    return h;
}
static void copy_tag_uid(const uint8_t *p_data, uint8_t uid[7]) {
    uid[0]=p_data[0]; uid[1]=p_data[1]; uid[2]=p_data[2];
    memcpy(uid+3,p_data+4,4);
}
/* A character's key: four hashes of the UID followed by the tags' constant. */
static void derive_figure_key(const uint8_t *p_data, uint32_t key[4]) {
    static const uint8_t s_constant[17]={0xB7,0xD5,0xD7,0xE6,0xE7,0xBA,0x3C,0xA8,0xD8,
                                         0x75,0x47,0x68,0xCF,0x23,0xE9,0xFE,0xAA};
    for (int n=3;n<=6;++n) {
        uint8_t buffer[24];
        copy_tag_uid(p_data,buffer);
        memcpy(buffer+7,s_constant,sizeof(s_constant));
        buffer[n*4-1]=0xAA;
        key[n-3]=tag_hash(buffer,n);
    }
}
/* A tag: a UID made from the figure id and copy, the id encrypted into pages 36 and 37 for
 * characters (in plain for vehicles and gadgets, ids from 1000), the password. */
static void create_figure(Figure *p_figure) {
    uint8_t *p_data=p_figure->data;
    memset(p_data,0,sizeof(p_figure->data));
    const uint32_t h=p_figure->id*2654435761u^(p_figure->copy+1)*40503u;
    const uint8_t uid[7]={0x04,(uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24),
                          (uint8_t)(p_figure->copy*37+11),0x80};
    p_data[0]=uid[0]; p_data[1]=uid[1]; p_data[2]=uid[2];
    p_data[3]=0x88^uid[0]^uid[1]^uid[2];
    memcpy(p_data+4,uid+3,4);
    p_data[8]=uid[3]^uid[4]^uid[5]^uid[6];
    if (p_figure->id<VEHICLE_IDS) {
        uint32_t key[4];
        derive_figure_key(p_data,key);
        uint8_t value[8];
        memcpy(value,&p_figure->id,4);
        memcpy(value+4,&p_figure->id,4);
        tea_encrypt_with(value,key);
        memcpy(p_data+36*4,value,8);
    } else {
        memcpy(p_data+36*4,&p_figure->id,4);
        p_data[38*4+1]=1;
    }
    static const char s_copyright[]="(c) Copyright LEGO 2014";
    uint8_t buffer[32];
    memcpy(buffer,uid,7);
    memcpy(buffer+7,s_copyright,23);
    buffer[30]=buffer[31]=0xAA;
    const uint32_t password=tag_hash(buffer,8);
    memcpy(p_data+43*4,&password,4);
}
static void make_toypad_directory(void) {
    char sz_path[512];
    snprintf(sz_path,sizeof(sz_path),"%s/toypad",runtime_file_user_dir());
    const int error=ps_host_fs_make_dir(sz_path,0755);
    if (error && error!=EEXIST) printf("Runtime: toypad: cannot create %s: %s\n",sz_path,strerror(error));
}
static void format_figure_path(const Figure *p_figure, char *sz_out, size_t cb_out) {
    snprintf(sz_out,cb_out,"%s/toypad/%u-%u.bin",runtime_file_user_dir(),p_figure->id,p_figure->copy);
}
static void save_figure(const Figure *p_figure) {
    char sz_path[512];
    format_figure_path(p_figure,sz_path,sizeof(sz_path));
    FILE *p_file=fopen(sz_path,"wb");
    if (!p_file) { printf("Runtime: toypad: cannot write %s: %s\n",sz_path,strerror(errno)); return; }
    const size_t cb_written=fwrite(p_figure->data,1,sizeof(p_figure->data),p_file);
    if (fclose(p_file) || cb_written!=sizeof(p_figure->data)) printf("Runtime: toypad: writing %s failed\n",sz_path);
}
static void load_figure(Figure *p_figure) {
    char sz_path[512];
    format_figure_path(p_figure,sz_path,sizeof(sz_path));
    FILE *p_file=fopen(sz_path,"rb");
    const int is_loaded=p_file && fread(p_figure->data,1,sizeof(p_figure->data),p_file)==sizeof(p_figure->data);
    if (p_file) fclose(p_file);
    if (is_loaded) return;
    if (p_file) printf("Runtime: toypad: %s is not a %zu-byte tag image, made again\n",sz_path,sizeof(p_figure->data));
    create_figure(p_figure);
    save_figure(p_figure);
}
static Figure *figure_at(uint8_t i_reader) {
    return i_reader>=1 && i_reader<=SLOTS && s_figures[i_reader-1].is_present ? &s_figures[i_reader-1] : NULL;
}

/* Replies and figure reports waiting for an IN transfer, under s_lock. */
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed=PTHREAD_COND_INITIALIZER;
enum { QUEUE=64 };
static uint8_t s_replies[QUEUE][REPORT];
static unsigned s_i_reply_head, s_c_replies;
/* Figure reports go out once the game has woken the pad (after its first challenge). */
static uint8_t s_events[QUEUE][REPORT];
static unsigned s_i_event_head, s_c_events;
static int s_is_awake;
static uint64_t s_c_commands, s_c_unknown_commands, s_c_figure_reports, s_c_dropped;

static void note_dropped(const char *sz_what) {
    if (!__atomic_fetch_add(&s_c_dropped,1,__ATOMIC_RELAXED)) {
        printf("Runtime: toypad: %s dropped: the game has not read the last %d\n",sz_what,QUEUE);
        runtime_calls_hle_stub("toy pad report dropped (queue full)",&s_c_dropped);
    }
}
static void queue_event(const Figure *p_figure, int is_removed) {
    if (s_c_events==QUEUE) { note_dropped("figure report"); return; }
    uint8_t *p_event=s_events[(s_i_event_head+s_c_events++)%QUEUE];
    memset(p_event,0,REPORT);
    const uint8_t *p_data=p_figure->data;
    const uint8_t report[13]={0x56,0x0b,p_figure->zone,0,p_figure->i_reader,(uint8_t)is_removed,p_data[0],p_data[1],
                              p_data[2],p_data[4],p_data[5],p_data[6],p_data[7]};
    memcpy(p_event,report,sizeof(report));
    p_event[13]=checksum(p_event,13);
    __atomic_fetch_add(&s_c_figure_reports,1,__ATOMIC_RELAXED);
}
static void queue_reply(const uint8_t reply[REPORT]) {
    if (s_c_replies==QUEUE) { note_dropped("reply"); return; }
    memcpy(s_replies[(s_i_reply_head+s_c_replies++)%QUEUE],reply,REPORT);
}

/* The three zones' lights as the game last set them (0xRRGGBB): the target colour of a colour,
 * fade or flash. Payloads: C0 [zone, r, g, b]; C2 [zone, tick, count, r, g, b]; C3 [zone, on,
 * off, count, r, g, b]; C6 [enable, tick, count, r, g, b] x 3; C7 [enable, on, off, count, r, g,
 * b] x 3; C8 [enable, r, g, b] x 3. Zone 0 is all three. */
static uint32_t s_lights[3];
static uint32_t read_rgb(const uint8_t *p) { return (uint32_t)p[0]<<16|(uint32_t)p[1]<<8|p[2]; }
static void set_zone_light(unsigned zone, uint32_t colour) {
    if (zone==0) s_lights[0]=s_lights[1]=s_lights[2]=colour;
    else if (zone<=3) s_lights[zone-1]=colour;
}
static void set_lights(uint8_t command, const uint8_t *p) {
    switch (command) {
    case 0xC0: set_zone_light(p[0],read_rgb(p+1)); break;
    case 0xC2: set_zone_light(p[0],read_rgb(p+3)); break;
    case 0xC3: set_zone_light(p[0],read_rgb(p+4)); break;
    case 0xC6: for (int z=0;z<3;++z) if (p[z*6]) s_lights[z]=read_rgb(p+z*6+3); break;
    case 0xC7: for (int z=0;z<3;++z) if (p[z*7]) s_lights[z]=read_rgb(p+z*7+4); break;
    case 0xC8: for (int z=0;z<3;++z) if (p[z*4]) s_lights[z]=read_rgb(p+z*4+1); break;
    default: break;
    }
}
void runtime_toypad_lights(uint32_t p_out_colours[3]) {
    pthread_mutex_lock(&s_lock);
    memcpy(p_out_colours,s_lights,sizeof(s_lights));
    pthread_mutex_unlock(&s_lock);
}

/* One request from the game (an OUT report, zero-filled to REPORT bytes), answered into the reply
 * queue. Under s_lock. */
static void handle_request(const uint8_t p_request[REPORT]) {
    if (p_request[0]!=0x55) return;
    const uint8_t command=p_request[2], sequence=p_request[3];
    uint8_t reply[REPORT]={0x55};
    uint8_t value[8];
    __atomic_fetch_add(&s_c_commands,1,__ATOMIC_RELAXED);
    switch (command) {
    case 0xB0: { /* wake: the pad's banner, "(c) LEGO 2014" */
        static const uint8_t s_banner[]={0x55,0x0e,0x01,0x28,0x63,0x29,0x20,0x4c,0x45,
                                         0x47,0x4f,0x20,0x32,0x30,0x31,0x34,0x46};
        memcpy(reply,s_banner,sizeof(s_banner));
        break;
    }
    case 0xB1: { /* seed: the generator's seed and a confirmation, echoed back encrypted */
        memcpy(value,p_request+4,8);
        tea_decrypt(value);
        uint32_t seed;
        memcpy(&seed,value,4);
        seed_random(seed);
        uint8_t answer[8]={value[4],value[5],value[6],value[7],0,0,0,0};
        tea_encrypt_with(answer,s_command_key);
        reply[1]=0x09; reply[2]=sequence;
        memcpy(reply+3,answer,8);
        reply[11]=checksum(reply,11);
        break;
    }
    case 0xB3: { /* challenge: the generator's next number and the confirmation */
        memcpy(value,p_request+4,8);
        tea_decrypt(value);
        const uint32_t next=next_random();
        uint8_t answer[8];
        memcpy(answer,&next,4);
        memcpy(answer+4,value,4);
        tea_encrypt_with(answer,s_command_key);
        reply[1]=0x09; reply[2]=sequence;
        memcpy(reply+3,answer,8);
        reply[11]=checksum(reply,11);
        s_is_awake=1;
        break;
    }
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC6: case 0xC7: case 0xC8:
        set_lights(command,p_request+4);
        reply[1]=0x01; reply[2]=sequence;
        reply[3]=checksum(reply,3);
        break;
    case 0xD2: { /* read 4 pages of the figure at reader [4] from page [5], zeros past the end */
        const Figure *p_figure=figure_at(p_request[4]);
        reply[1]=0x12; reply[2]=sequence; reply[3]=0;
        for (int i=0;p_figure && i<16;++i)
            if (p_request[5]*4+i<PAGES*4) reply[4+i]=p_figure->data[p_request[5]*4+i];
        reply[20]=checksum(reply,20);
        break;
    }
    case 0xD3: { /* write page [5] of the figure at reader [4] */
        Figure *p_figure=figure_at(p_request[4]);
        if (p_figure && p_request[5]<PAGES) {
            memcpy(p_figure->data+p_request[5]*4,p_request+6,4);
            save_figure(p_figure);
        }
        reply[1]=0x02; reply[2]=sequence; reply[3]=0;
        reply[4]=checksum(reply,4);
        break;
    }
    case 0xD4: { /* the model (id) of the figure at the encrypted reader index, and the confirmation */
        memcpy(value,p_request+4,8);
        tea_decrypt(value);
        const Figure *p_figure=figure_at(value[0]);
        uint32_t id=0;
        if (p_figure && p_figure->id<VEHICLE_IDS) id=p_figure->id;
        else if (p_figure) memcpy(&id,p_figure->data+36*4,4);
        uint8_t answer[8]={(uint8_t)id,(uint8_t)(id>>8),(uint8_t)(id>>16),(uint8_t)(id>>24),
                           value[4],value[5],value[6],value[7]};
        tea_encrypt_with(answer,s_command_key);
        reply[1]=0x0a; reply[2]=sequence; reply[3]=0;
        memcpy(reply+4,answer,8);
        reply[12]=checksum(reply,12);
        break;
    }
    default: { /* tag list, passwords, activity, LED queries: an empty acknowledgement */
        const uint64_t c_before=__atomic_fetch_add(&s_c_unknown_commands,1,__ATOMIC_RELAXED);
        if (!c_before) runtime_calls_hle_stub("toy pad command answered empty",&s_c_unknown_commands);
        if (c_before<8) printf("Runtime: toypad: command %#x answered empty\n",command);
        reply[1]=0x01; reply[2]=sequence;
        reply[3]=checksum(reply,3);
        break;
    }
    }
    queue_reply(reply);
}

/* ---- Transfers: OUT completes at once, IN when a report is there ---- */

typedef struct { UsbdTransfer *p_transfer; uint64_t deadline_ns; } PendingTransfer;
enum { MAX_PENDING=32 };
static PendingTransfer s_pending[MAX_PENDING];
static unsigned s_c_pending;
static UsbdTransfer *s_completed[MAX_PENDING*2];
static unsigned s_c_completed;

static void complete_locked(UsbdTransfer *p_transfer, int32_t status) {
    p_transfer->status=status;
    if (s_c_completed<sizeof(s_completed)/sizeof(*s_completed)) s_completed[s_c_completed++]=p_transfer;
    else printf("Runtime: toypad: transfer %p completed with no room left for its callback\n",(void *)p_transfer);
}
/* Under s_lock: the IN transfers that can complete, and those whose time is up. */
static void settle_locked(void) {
    const uint64_t now_ns=ps_host_time_monotonic_ns();
    for (unsigned i=0;i<s_c_pending;) {
        UsbdTransfer *p_transfer=s_pending[i].p_transfer;
        const int is_report_ready=s_c_replies || (s_is_awake && s_c_events);
        if (is_report_ready) {
            const int32_t cb_report=p_transfer->length<REPORT ? p_transfer->length : REPORT;
            if (s_c_replies) {
                memcpy(p_transfer->buffer,s_replies[s_i_reply_head],(size_t)cb_report);
                s_i_reply_head=(s_i_reply_head+1)%QUEUE;
                --s_c_replies;
            } else {
                memcpy(p_transfer->buffer,s_events[s_i_event_head],(size_t)cb_report);
                s_i_event_head=(s_i_event_head+1)%QUEUE;
                --s_c_events;
            }
            p_transfer->actual_length=cb_report;
            complete_locked(p_transfer,TRANSFER_COMPLETED);
        } else if (s_pending[i].deadline_ns && now_ns>=s_pending[i].deadline_ns) {
            p_transfer->actual_length=0;
            complete_locked(p_transfer,TRANSFER_TIMED_OUT);
        } else {
            ++i;
            continue;
        }
        s_pending[i]=s_pending[--s_c_pending];
    }
}

static ABI int32_t toypad_submit_transfer(UsbdTransfer *p_transfer) {
    if (!p_transfer || p_transfer->dev_handle!=&s_handle) return USBD_NO_DEVICE;
    if (p_transfer->endpoint!=ENDPOINT_OUT && p_transfer->endpoint!=ENDPOINT_IN) return USBD_INVALID_ARG;
    if (p_transfer->length<0 || (p_transfer->length && !p_transfer->buffer)) return USBD_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    if (p_transfer->endpoint==ENDPOINT_OUT) {
        uint8_t request[REPORT]={0};
        if (p_transfer->length) memcpy(request,p_transfer->buffer,(size_t)(p_transfer->length<REPORT ? p_transfer->length : REPORT));
        handle_request(request);
        p_transfer->actual_length=p_transfer->length;
        complete_locked(p_transfer,TRANSFER_COMPLETED);
    } else {
        if (s_c_pending==MAX_PENDING) {
            pthread_mutex_unlock(&s_lock);
            return USBD_NO_MEMORY;
        }
        const uint64_t deadline_ns=p_transfer->timeout ? ps_host_time_monotonic_ns()+(uint64_t)p_transfer->timeout*1000000u : 0;
        s_pending[s_c_pending++]=(PendingTransfer){p_transfer,deadline_ns};
    }
    settle_locked();
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t toypad_cancel_transfer(UsbdTransfer *p_transfer) {
    int32_t result=USBD_NOT_FOUND;
    pthread_mutex_lock(&s_lock);
    for (unsigned i=0;i<s_c_pending;++i)
        if (s_pending[i].p_transfer==p_transfer) {
            s_pending[i]=s_pending[--s_c_pending];
            p_transfer->actual_length=0;
            complete_locked(p_transfer,TRANSFER_CANCELLED);
            pthread_cond_broadcast(&s_changed);
            result=0;
            break;
        }
    pthread_mutex_unlock(&s_lock);
    return result;
}

/* ---- The figures file ---- */

static char s_sz_figures_file[512];
static int s_is_connected;
static void choose_figures_file(void) {
    const char *sz_env=runtime_env("PS_TOYPAD_FILE","");
    if (*sz_env) snprintf(s_sz_figures_file,sizeof(s_sz_figures_file),"%s",sz_env);
    else snprintf(s_sz_figures_file,sizeof(s_sz_figures_file),"%s/toypad/figures.txt",runtime_file_user_dir());
}
const char *runtime_toypad_file(void) { return __atomic_load_n(&s_is_connected,__ATOMIC_ACQUIRE) ? s_sz_figures_file : NULL; }

typedef struct { uint8_t zone; uint32_t id, copy; } WantedFigure;
/* The file's figures, at most SLOTS; the n-th line with an id is copy n of that id. */
static unsigned read_figures_file(FILE *p_file, WantedFigure wanted[SLOTS]) {
    unsigned c_wanted=0, c_per_zone[4]={0};
    char sz_line[256];
    while (fgets(sz_line,sizeof(sz_line),p_file)) {
        char *sz_hash=strchr(sz_line,'#');
        if (sz_hash) *sz_hash=0;
        unsigned zone, id;
        if (sscanf(sz_line,"%u %u",&zone,&id)!=2) continue;
        if (zone<1 || zone>3 || !id || c_per_zone[zone]==(zone==1 ? 1u : 3u) || c_wanted==SLOTS) {
            printf("Runtime: toypad: figure %u in zone %u does not fit (zone 1 holds 1, zones 2 and 3 hold 3, the pad 7)\n",id,zone);
            continue;
        }
        ++c_per_zone[zone];
        uint32_t copy=0;
        for (unsigned i=0;i<c_wanted;++i) copy+=wanted[i].id==id;
        wanted[c_wanted++]=(WantedFigure){(uint8_t)zone,id,copy};
    }
    return c_wanted;
}
/* Under s_lock, at most every 300 ms: the file's figures against the pad's. A figure stays while
 * its line does; lifted ones go first, then new ones take the lowest free reader index. */
static void sync_figures_locked(void) {
    static uint64_t s_last_check_ns;
    static struct timespec s_last_mtime;
    static int s_had_file=-1;
    if (!__atomic_load_n(&s_is_connected,__ATOMIC_ACQUIRE)) return;
    const uint64_t now_ns=ps_host_time_monotonic_ns();
    if (now_ns-s_last_check_ns<300000000u) return;
    s_last_check_ns=now_ns;
    PsHostStat st;
    const int has_file=ps_host_fs_stat(s_sz_figures_file,&st)==0;
    if (has_file==s_had_file && (!has_file || (st.mtime.tv_sec==s_last_mtime.tv_sec && st.mtime.tv_nsec==s_last_mtime.tv_nsec)))
        return;
    if (s_had_file<0 && !has_file) printf("Runtime: toypad: no figures file yet (%s): the pad is empty\n",s_sz_figures_file);
    s_had_file=has_file;
    if (has_file) s_last_mtime=st.mtime;
    WantedFigure wanted[SLOTS];
    unsigned c_wanted=0;
    if (has_file) {
        FILE *p_file=fopen(s_sz_figures_file,"r");
        if (p_file) {
            c_wanted=read_figures_file(p_file,wanted);
            fclose(p_file);
        } else {
            printf("Runtime: toypad: cannot read %s: %s\n",s_sz_figures_file,strerror(errno));
            return;
        }
    }
    int is_kept[SLOTS]={0};
    for (unsigned s=0;s<SLOTS;++s) {
        Figure *p_figure=&s_figures[s];
        if (!p_figure->is_present) continue;
        int is_found=0;
        for (unsigned i=0;i<c_wanted && !is_found;++i)
            if (!is_kept[i] && wanted[i].zone==p_figure->zone && wanted[i].id==p_figure->id && wanted[i].copy==p_figure->copy)
                is_kept[i]=is_found=1;
        if (is_found) continue;
        queue_event(p_figure,1);
        printf("Runtime: toypad: figure %u lifted from zone %u\n",p_figure->id,p_figure->zone);
        p_figure->is_present=0;
    }
    for (unsigned i=0;i<c_wanted;++i) {
        if (is_kept[i]) continue;
        for (unsigned s=0;s<SLOTS;++s) {
            Figure *p_figure=&s_figures[s];
            if (p_figure->is_present) continue;
            p_figure->is_present=1; p_figure->zone=wanted[i].zone; p_figure->i_reader=(uint8_t)(s+1);
            p_figure->id=wanted[i].id; p_figure->copy=wanted[i].copy;
            load_figure(p_figure);
            queue_event(p_figure,0);
            printf("Runtime: toypad: figure %u placed on zone %u (reader index %u)\n",p_figure->id,p_figure->zone,p_figure->i_reader);
            break;
        }
    }
}

/* libusb's event handling: waits until a transfer completes or the timeout passes, then runs the
 * completed transfers' callbacks on the caller's thread, outside the lock. The wait wakes every
 * 100 ms so that figures put on or lifted meanwhile reach a pending IN transfer. */
static int32_t handle_toypad_events(const UsbdTimeval *p_timeout) {
    uint64_t wait_ns;
    if (!usbd_event_wait_ns(p_timeout,&wait_ns)) return USBD_INVALID_ARG;
    const uint64_t start_ns=ps_host_time_monotonic_ns(), deadline_ns=start_ns+wait_ns;
    UsbdTransfer *p_done[sizeof(s_completed)/sizeof(*s_completed)];
    pthread_mutex_lock(&s_lock);
    for (;;) {
        sync_figures_locked();
        settle_locked();
        const uint64_t now_ns=ps_host_time_monotonic_ns();
        if (s_c_completed || now_ns>=deadline_ns) break;
        const uint64_t slice_ns=deadline_ns-now_ns<100000000u ? deadline_ns-now_ns : 100000000u;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME,&until);
        until.tv_sec+=(time_t)(slice_ns/1000000000u);
        until.tv_nsec+=(long)(slice_ns%1000000000u);
        if (until.tv_nsec>=1000000000) { until.tv_nsec-=1000000000; ++until.tv_sec; }
        pthread_cond_timedwait(&s_changed,&s_lock,&until);
    }
    const unsigned c_done=s_c_completed;
    memcpy(p_done,s_completed,c_done*sizeof(*p_done));
    s_c_completed=0;
    pthread_mutex_unlock(&s_lock);
    runtime_wait_note(0,ps_host_time_monotonic_ns()-start_ns);
    for (unsigned i=0;i<c_done;++i)
        if (p_done[i]->callback) ((UsbdTransferCallback)p_done[i]->callback)(p_done[i]);
    return 0;
}
static ABI int32_t toypad_handle_events(void) { return handle_toypad_events(NULL); }
static ABI int32_t toypad_handle_events_timeout(const UsbdTimeval *p_timeout) { return handle_toypad_events(p_timeout); }

/* ---- libSceUsbd with the pad plugged in ---- */

static ABI int32_t toypad_init(void) {
    static int s_is_logged;
    if (s_is_logged) return 0;
    s_is_logged=1;
    make_toypad_directory();
    choose_figures_file();
    __atomic_store_n(&s_is_connected,1,__ATOMIC_RELEASE);
    printf("Runtime: Usbd initialized; the emulated toy pad is connected (figures from %s)\n",s_sz_figures_file);
    return 0;
}
static ABI int64_t toypad_get_device_list(void ***p_out_list) {
    if (!p_out_list) return USBD_INVALID_ARG;
    void **pp_list=calloc(2,sizeof(void *));
    if (!pp_list) return USBD_NO_MEMORY;
    pp_list[0]=&s_device;
    *p_out_list=pp_list;
    return 1;
}
static ABI int32_t toypad_get_device_descriptor(void *p_device, DeviceDescriptor *p_out) {
    if (p_device!=&s_device) return USBD_NO_DEVICE;
    if (!p_out) return USBD_INVALID_ARG;
    *p_out=(DeviceDescriptor){18,1,0x0200,0,0,0,REPORT,VENDOR,PRODUCT,0x0200,1,2,0,1};
    return 0;
}
static ABI int32_t toypad_get_config_descriptor(void *p_device, uint8_t i_config, const ConfigDescriptor **pp_out) {
    if (p_device!=&s_device) return USBD_NO_DEVICE;
    if (i_config!=0) return USBD_NOT_FOUND;
    if (!pp_out) return USBD_INVALID_ARG;
    *pp_out=&s_config;
    return 0;
}
static ABI int32_t toypad_open(void *p_device, void **pp_out_handle) {
    if (p_device!=&s_device) return USBD_NO_DEVICE;
    if (!pp_out_handle) return USBD_INVALID_ARG;
    *pp_out_handle=&s_handle;
    return 0;
}
static ABI void *toypad_open_with_vid_pid(uint16_t vendor, uint16_t product) {
    return vendor==VENDOR && product==PRODUCT ? &s_handle : NULL;
}
static ABI void toypad_close(void *p_handle) { (void)p_handle; }
static ABI int32_t toypad_max_packet_size(void *p_device, uint8_t endpoint) {
    (void)endpoint;
    return p_device==&s_device ? REPORT : USBD_NO_DEVICE;
}
static ABI uint8_t toypad_device_number(void *p_device) { return p_device==&s_device ? 1 : 0; }
/* 0 while connected: anything else and the game takes the pad as unplugged. */
static ABI int32_t toypad_handle_ok(void *p_handle) { return p_handle==&s_handle ? 0 : USBD_NO_DEVICE; }
static ABI int32_t toypad_interface_ok(void *p_handle, int32_t i_interface) {
    (void)i_interface;
    return p_handle==&s_handle ? 0 : USBD_NO_DEVICE;
}
/* The console's HID driver holds the pad: 1 (active), so the game detaches it, then claims the
 * interface (with 0 it resets the device and starts over). */
static ABI int32_t toypad_kernel_driver_active(void *p_handle, int32_t i_interface) {
    (void)i_interface;
    return p_handle==&s_handle ? 1 : USBD_NO_DEVICE;
}

static const RuntimeExport s_exports[]={
    {"sceUsbdInit",toypad_init}, {"sceUsbdGetDeviceList",toypad_get_device_list},
    {"sceUsbdGetDeviceDescriptor",toypad_get_device_descriptor},
    {"sceUsbdGetConfigDescriptor",toypad_get_config_descriptor},
    {"sceUsbdOpen",toypad_open}, {"sceUsbdOpenDeviceWithVidPid",toypad_open_with_vid_pid},
    {"sceUsbdClose",toypad_close}, {"sceUsbdGetMaxPacketSize",toypad_max_packet_size},
    {"sceUsbdGetBusNumber",toypad_device_number}, {"sceUsbdGetDeviceAddress",toypad_device_number},
    {"sceUsbdCheckConnected",toypad_handle_ok}, {"sceUsbdResetDevice",toypad_handle_ok},
    {"sceUsbdKernelDriverActive",toypad_kernel_driver_active},
    {"sceUsbdDetachKernelDriver",toypad_interface_ok}, {"sceUsbdAttachKernelDriver",toypad_interface_ok},
    {"sceUsbdClaimInterface",toypad_interface_ok}, {"sceUsbdReleaseInterface",toypad_interface_ok},
    {"sceUsbdSubmitTransfer",toypad_submit_transfer}, {"sceUsbdCancelTransfer",toypad_cancel_transfer},
    {"sceUsbdHandleEvents",toypad_handle_events}, {"sceUsbdHandleEventsTimeout",toypad_handle_events_timeout},
    {"sceUsbdHandleEventsLocked",toypad_handle_events_timeout},
};
static int s_is_enabled;
static pthread_once_t s_enabled_once=PTHREAD_ONCE_INIT;
static void read_enabled(void) {
    const char *sz_value=runtime_config_get("toypad");
    s_is_enabled=runtime_env_flag("PS_TOYPAD",sz_value && *sz_value && *sz_value!='0');
}
uintptr_t runtime_toypad_resolve(const char *sz_name) {
    pthread_once(&s_enabled_once,read_enabled);
    return s_is_enabled ? RUNTIME_LOOKUP(s_exports,sz_name) : 0;
}
void runtime_toypad_report(void) {
    if (!__atomic_load_n(&s_is_connected,__ATOMIC_ACQUIRE)) return;
    printf("Runtime: toypad commands=%" PRIu64 " (answered empty=%" PRIu64 "), figure reports=%" PRIu64 ", dropped=%" PRIu64 "\n",
           __atomic_load_n(&s_c_commands,__ATOMIC_RELAXED),__atomic_load_n(&s_c_unknown_commands,__ATOMIC_RELAXED),
           __atomic_load_n(&s_c_figure_reports,__ATOMIC_RELAXED),__atomic_load_n(&s_c_dropped,__ATOMIC_RELAXED));
}
