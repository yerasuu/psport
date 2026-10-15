/* libScePlayGo for a package installed in full: every chunk listed in
 * sce_sys/playgo-chunk.dat is local (LocalFast), nothing is left to download,
 * and progress reports each chunk's size, summed from its mchunks, as done. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define PLAYGO_INVALID_ARGUMENT ((int32_t)0x80B20004)
#define PLAYGO_NOT_INITIALIZED ((int32_t)0x80B20005)
#define PLAYGO_ALREADY_INITIALIZED ((int32_t)0x80B20006)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define PLAYGO_NOT_SUPPORTED ((int32_t)0x80B2000E)
#define PLAYGO_BAD_LOCUS ((int32_t)0x80B20010)
#define PLAYGO_HANDLE 1
#define PLAYGO_MIN_BUFFER 0x200000
#define LOCUS_NOT_DOWNLOADED 0
#define LOCUS_LOCAL_SLOW 2
#define LOCUS_LOCAL_FAST 3
#define SPEED_SUSPENDED 0
#define SPEED_TRICKLE 1
#define SPEED_FULL 2
#define SUSPEND_MS 30000

typedef struct { const void *buffer; uint32_t size, reserved; } InitParams;
typedef struct { uint16_t chunk; int8_t locus, reserved; } ToDo;
typedef struct { uint64_t progress, total; } Progress;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, supported, open_handle;
static uint32_t chunk_count;
static uint64_t *chunk_sizes, language_mask, speed_since_ms;
static int32_t speed=SPEED_TRICKLE;

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0]|p[1]<<8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t le64(const unsigned char *p) { return le32(p)|(uint64_t)le32(p+4)<<32; }

/* Header: chunk count at 0x0A, file size at 0x10, then (offset, size) pairs: chunk attributes
 * at 0xC0 (32 bytes each), their mchunk id lists at 0xC8, mchunk attributes at 0xD8 (16 bytes:
 * location, then a 48-bit size). */
static int parse_chunks(const unsigned char *file, uint32_t size) {
    const uint32_t count=le16(file+0x0A), attrs=le32(file+0xC0), lists=le32(file+0xC8), lists_size=le32(file+0xCC);
    const uint32_t mchunks=le32(file+0xD8), mchunks_size=le32(file+0xDC);
    if ((uint64_t)attrs+count*32u>size || (uint64_t)lists+lists_size>size || (uint64_t)mchunks+mchunks_size>size) return 0;
    uint64_t *sizes=calloc(count ? count : 1,sizeof(*sizes));
    if (!sizes) return 0;
    for (uint32_t i=0;i<count;++i) {
        const unsigned char *attr=file+attrs+i*32u;
        const uint32_t ids=le16(attr+14), list=le32(attr+24);
        if ((uint64_t)list+ids*2u>lists_size) { free(sizes); return 0; }
        for (uint32_t j=0;j<ids;++j) {
            const uint32_t id=le16(file+lists+list+j*2u);
            if ((uint64_t)id*16u+16u>mchunks_size) { free(sizes); return 0; }
            sizes[i]+=le64(file+mchunks+id*16u+8u)&UINT64_C(0xFFFFFFFFFFFF);
        }
    }
    chunk_sizes=sizes; chunk_count=count;
    return 1;
}
static void load_chunks(void) {
    const int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    if (fd<0) { puts("Runtime: PlayGo: no sce_sys/playgo-chunk.dat; the package does not support PlayGo"); return; }
    unsigned char header[0x100];
    unsigned char *file=NULL;
    if (runtime_file_pread(fd,header,sizeof(header),0)==sizeof(header) && !memcmp(header,"plgo",4)) {
        const uint32_t size=le32(header+0x10);
        if (size>=sizeof(header) && size<=16u<<20 && (file=malloc(size)) && runtime_file_pread(fd,file,size,0)==size)
            supported=parse_chunks(file,size);
    }
    free(file);
    runtime_file_close(fd);
    if (supported) printf("Runtime: PlayGo initialized; %u chunks, all installed locally\n",chunk_count);
    else puts("Runtime: PlayGo: sce_sys/playgo-chunk.dat could not be parsed; the package does not support PlayGo");
}

static int bad_handle(int32_t handle) { return !open_handle || handle!=open_handle; }

static ABI int32_t playgo_initialize(const InitParams *params) {
    if (!params || !params->buffer) return PLAYGO_BAD_POINTER;
    if (params->size<PLAYGO_MIN_BUFFER) return PLAYGO_BAD_SIZE;
    if (initialized) return PLAYGO_ALREADY_INITIALIZED;
    load_chunks();
    const int32_t language=runtime_system_language();
    language_mask=language>=0 && language<48 ? UINT64_C(1)<<(63-language) : 0;
    speed=SPEED_TRICKLE;
    initialized=1;
    return 0;
}
static ABI int32_t playgo_terminate(void) {
    if (!initialized) return PLAYGO_NOT_INITIALIZED;
    free(chunk_sizes);
    chunk_sizes=NULL; chunk_count=0; supported=0; open_handle=0; initialized=0;
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle, const void *param) {
    if (!handle) return PLAYGO_BAD_POINTER;
    if (param) return PLAYGO_INVALID_ARGUMENT;
    if (!initialized) return PLAYGO_NOT_INITIALIZED;
    if (!supported) return PLAYGO_NOT_SUPPORTED;
    open_handle=*handle=PLAYGO_HANDLE;
    return 0;
}
static ABI int32_t playgo_close(int32_t handle) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    open_handle=0;
    return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle, uint16_t *ids, uint32_t count, uint32_t *out) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=chunk_count; return 0; }
    const uint32_t n=count<chunk_count ? count : chunk_count;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n;
    return 0;
}
static ABI int32_t playgo_locus(int32_t handle, const uint16_t *ids, uint32_t count, int8_t *loci) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=chunk_count) { loci[i]=LOCUS_NOT_DOWNLOADED; return PLAYGO_BAD_CHUNK; }
        loci[i]=LOCUS_LOCAL_FAST;
    }
    return 0;
}
static ABI int32_t playgo_progress(int32_t handle, const uint16_t *ids, uint32_t count, Progress *out) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!ids || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    uint64_t total=0;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=chunk_count) return PLAYGO_BAD_CHUNK;
        total+=chunk_sizes[ids[i]];
    }
    out->progress=out->total=total;
    return 0;
}
static ABI int32_t playgo_eta(int32_t handle, const uint16_t *ids, uint32_t count, int64_t *eta) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!ids || !eta) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *eta=0;
    return 0;
}
static ABI int32_t playgo_prefetch(int32_t handle, const uint16_t *ids, uint32_t count, int8_t minimum) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!ids) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    if (minimum!=LOCUS_NOT_DOWNLOADED && minimum!=LOCUS_LOCAL_SLOW && minimum!=LOCUS_LOCAL_FAST) return PLAYGO_BAD_LOCUS;
    return 0;
}
static ABI int32_t playgo_get_todo(int32_t handle, ToDo *list, uint32_t count, uint32_t *out) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!list || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *out=0;
    return 0;
}
static ABI int32_t playgo_set_todo(int32_t handle, const ToDo *list, uint32_t count) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!list) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    return 0;
}
/* A suspended install resumes at trickle speed after 30 seconds, as the system does on its own. */
static ABI int32_t playgo_get_speed(int32_t handle, int32_t *out) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    pthread_mutex_lock(&lock);
    if (speed==SPEED_SUSPENDED && now_ms()-speed_since_ms>SUSPEND_MS) speed=SPEED_TRICKLE;
    *out=speed;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t playgo_set_speed(int32_t handle, int32_t value) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (value!=SPEED_SUSPENDED && value!=SPEED_TRICKLE && value!=SPEED_FULL) return PLAYGO_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    speed=value; speed_since_ms=now_ms();
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t playgo_get_language_mask(int32_t handle, uint64_t *out) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    *out=language_mask;
    return 0;
}
static ABI int32_t playgo_set_language_mask(int32_t handle, uint64_t mask) {
    if (bad_handle(handle)) return PLAYGO_BAD_HANDLE;
    language_mask=mask;
    return 0;
}

static const RuntimeExport exports[]={
    {"scePlayGoInitialize",playgo_initialize}, {"scePlayGoTerminate",playgo_terminate},
    {"scePlayGoOpen",playgo_open}, {"scePlayGoClose",playgo_close},
    {"scePlayGoGetChunkId",playgo_chunk_ids}, {"scePlayGoGetLocus",playgo_locus},
    {"scePlayGoGetProgress",playgo_progress}, {"scePlayGoGetEta",playgo_eta}, {"scePlayGoPrefetch",playgo_prefetch},
    {"scePlayGoGetToDoList",playgo_get_todo}, {"scePlayGoSetToDoList",playgo_set_todo},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoSetInstallSpeed",playgo_set_speed},
    {"scePlayGoGetLanguageMask",playgo_get_language_mask}, {"scePlayGoSetLanguageMask",playgo_set_language_mask},
};
uintptr_t runtime_playgo_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
