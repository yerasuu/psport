// SPDX-License-Identifier: GPL-2.0-or-later
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

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_initialized, s_is_supported, s_has_open_handle;
static uint32_t s_c_chunks;
static uint64_t *s_p_chunk_sizes, s_language_mask, s_speed_since_ms;
static int32_t s_speed=SPEED_TRICKLE;

static uint64_t now_ms(void) { return ps_host_time_monotonic_ns()/1000000u; }
static uint16_t le16(const unsigned char *p_bytes) { return (uint16_t)(p_bytes[0]|p_bytes[1]<<8); }
static uint32_t le32(const unsigned char *p_bytes) { return (uint32_t)p_bytes[0]|(uint32_t)p_bytes[1]<<8|(uint32_t)p_bytes[2]<<16|(uint32_t)p_bytes[3]<<24; }
static uint64_t le64(const unsigned char *p_bytes) { return le32(p_bytes)|(uint64_t)le32(p_bytes+4)<<32; }

/* Header: chunk count at 0x0A, file size at 0x10, then (offset, size) pairs: chunk attributes
 * at 0xC0 (32 bytes each), their mchunk id lists at 0xC8, mchunk attributes at 0xD8 (16 bytes:
 * location, then a 48-bit size). */
static int parse_chunks(const unsigned char *p_file, uint32_t cb_file) {
    const uint32_t c_chunks=le16(p_file+0x0A), attrs=le32(p_file+0xC0), lists=le32(p_file+0xC8), cb_lists=le32(p_file+0xCC);
    const uint32_t mchunks=le32(p_file+0xD8), cb_mchunks=le32(p_file+0xDC);
    if ((uint64_t)attrs+c_chunks*32u>cb_file || (uint64_t)lists+cb_lists>cb_file || (uint64_t)mchunks+cb_mchunks>cb_file) return 0;
    uint64_t *p_sizes=calloc(c_chunks ? c_chunks : 1,sizeof(*p_sizes));
    if (!p_sizes) return 0;
    for (uint32_t i=0;i<c_chunks;++i) {
        const unsigned char *p_attr=p_file+attrs+i*32u;
        const uint32_t ids=le16(p_attr+14), list=le32(p_attr+24);
        if ((uint64_t)list+ids*2u>cb_lists) { free(p_sizes); return 0; }
        for (uint32_t j=0;j<ids;++j) {
            const uint32_t id=le16(p_file+lists+list+j*2u);
            if ((uint64_t)id*16u+16u>cb_mchunks) { free(p_sizes); return 0; }
            p_sizes[i]+=le64(p_file+mchunks+id*16u+8u)&UINT64_C(0xFFFFFFFFFFFF);
        }
    }
    s_p_chunk_sizes=p_sizes; s_c_chunks=c_chunks;
    return 1;
}
static void load_chunks(void) {
    const int h_fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    if (h_fd<0) { puts("Runtime: PlayGo: no sce_sys/playgo-chunk.dat; the package does not support PlayGo"); return; }
    unsigned char header[0x100];
    unsigned char *p_file=NULL;
    if (runtime_file_pread(h_fd,header,sizeof(header),0)==sizeof(header) && !memcmp(header,"plgo",4)) {
        const uint32_t cb_file=le32(header+0x10);
        if (cb_file>=sizeof(header) && cb_file<=16u<<20 && (p_file=malloc(cb_file)) && runtime_file_pread(h_fd,p_file,cb_file,0)==cb_file)
            s_is_supported=parse_chunks(p_file,cb_file);
    }
    free(p_file);
    runtime_file_close(h_fd);
    if (s_is_supported) printf("Runtime: PlayGo initialized; %u chunks, all installed locally\n",s_c_chunks);
    else puts("Runtime: PlayGo: sce_sys/playgo-chunk.dat could not be parsed; the package does not support PlayGo");
}

static int bad_handle(int32_t h_handle) { return !s_has_open_handle || h_handle!=s_has_open_handle; }

static ABI int32_t playgo_initialize(const InitParams *p_params) {
    if (!p_params || !p_params->buffer) return PLAYGO_BAD_POINTER;
    if (p_params->size<PLAYGO_MIN_BUFFER) return PLAYGO_BAD_SIZE;
    if (s_is_initialized) return PLAYGO_ALREADY_INITIALIZED;
    load_chunks();
    const int32_t language=runtime_system_language();
    s_language_mask=language>=0 && language<48 ? UINT64_C(1)<<(63-language) : 0;
    s_speed=SPEED_TRICKLE;
    s_is_initialized=1;
    return 0;
}
static ABI int32_t playgo_terminate(void) {
    if (!s_is_initialized) return PLAYGO_NOT_INITIALIZED;
    free(s_p_chunk_sizes);
    s_p_chunk_sizes=NULL; s_c_chunks=0; s_is_supported=0; s_has_open_handle=0; s_is_initialized=0;
    return 0;
}
static ABI int32_t playgo_open(int32_t *p_handle, const void *p_param) {
    if (!p_handle) return PLAYGO_BAD_POINTER;
    if (p_param) return PLAYGO_INVALID_ARGUMENT;
    if (!s_is_initialized) return PLAYGO_NOT_INITIALIZED;
    if (!s_is_supported) return PLAYGO_NOT_SUPPORTED;
    s_has_open_handle=*p_handle=PLAYGO_HANDLE;
    return 0;
}
static ABI int32_t playgo_close(int32_t h_handle) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    s_has_open_handle=0;
    return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t h_handle, uint16_t *p_ids, uint32_t c_ids, uint32_t *p_out) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_out) return PLAYGO_BAD_POINTER;
    if (p_ids && !c_ids) return PLAYGO_BAD_SIZE;
    if (!p_ids) { *p_out=s_c_chunks; return 0; }
    const uint32_t c_copied=c_ids<s_c_chunks ? c_ids : s_c_chunks;
    for (uint32_t i=0;i<c_copied;++i) p_ids[i]=(uint16_t)i;
    *p_out=c_copied;
    return 0;
}
static ABI int32_t playgo_locus(int32_t h_handle, const uint16_t *p_ids, uint32_t c_ids, int8_t *p_loci) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_ids || !p_loci) return PLAYGO_BAD_POINTER;
    if (!c_ids) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<c_ids;++i) {
        if (p_ids[i]>=s_c_chunks) { p_loci[i]=LOCUS_NOT_DOWNLOADED; return PLAYGO_BAD_CHUNK; }
        p_loci[i]=LOCUS_LOCAL_FAST;
    }
    return 0;
}
static ABI int32_t playgo_progress(int32_t h_handle, const uint16_t *p_ids, uint32_t c_ids, Progress *p_out) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_ids || !p_out) return PLAYGO_BAD_POINTER;
    if (!c_ids) return PLAYGO_BAD_SIZE;
    uint64_t cb_total=0;
    for (uint32_t i=0;i<c_ids;++i) {
        if (p_ids[i]>=s_c_chunks) return PLAYGO_BAD_CHUNK;
        cb_total+=s_p_chunk_sizes[p_ids[i]];
    }
    p_out->progress=p_out->total=cb_total;
    return 0;
}
static ABI int32_t playgo_eta(int32_t h_handle, const uint16_t *p_ids, uint32_t c_ids, int64_t *p_eta) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_ids || !p_eta) return PLAYGO_BAD_POINTER;
    if (!c_ids) return PLAYGO_BAD_SIZE;
    *p_eta=0;
    return 0;
}
static ABI int32_t playgo_prefetch(int32_t h_handle, const uint16_t *p_ids, uint32_t c_ids, int8_t minimum) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_ids) return PLAYGO_BAD_POINTER;
    if (!c_ids) return PLAYGO_BAD_SIZE;
    if (minimum!=LOCUS_NOT_DOWNLOADED && minimum!=LOCUS_LOCAL_SLOW && minimum!=LOCUS_LOCAL_FAST) return PLAYGO_BAD_LOCUS;
    return 0;
}
static ABI int32_t playgo_get_todo(int32_t h_handle, ToDo *p_list, uint32_t c_list, uint32_t *p_out) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_list || !p_out) return PLAYGO_BAD_POINTER;
    if (!c_list) return PLAYGO_BAD_SIZE;
    *p_out=0;
    return 0;
}
static ABI int32_t playgo_set_todo(int32_t h_handle, const ToDo *p_list, uint32_t c_list) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_list) return PLAYGO_BAD_POINTER;
    if (!c_list) return PLAYGO_BAD_SIZE;
    return 0;
}
/* A suspended install resumes at trickle speed after 30 seconds, as the system does on its own. */
static ABI int32_t playgo_get_speed(int32_t h_handle, int32_t *p_out) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_out) return PLAYGO_BAD_POINTER;
    pthread_mutex_lock(&s_lock);
    if (s_speed==SPEED_SUSPENDED && now_ms()-s_speed_since_ms>SUSPEND_MS) s_speed=SPEED_TRICKLE;
    *p_out=s_speed;
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t playgo_set_speed(int32_t h_handle, int32_t value) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (value!=SPEED_SUSPENDED && value!=SPEED_TRICKLE && value!=SPEED_FULL) return PLAYGO_INVALID_ARGUMENT;
    pthread_mutex_lock(&s_lock);
    s_speed=value; s_speed_since_ms=now_ms();
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t playgo_get_language_mask(int32_t h_handle, uint64_t *p_out) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    if (!p_out) return PLAYGO_BAD_POINTER;
    *p_out=s_language_mask;
    return 0;
}
static ABI int32_t playgo_set_language_mask(int32_t h_handle, uint64_t mask) {
    if (bad_handle(h_handle)) return PLAYGO_BAD_HANDLE;
    s_language_mask=mask;
    return 0;
}

static const RuntimeExport s_exports[]={
    {"scePlayGoInitialize",playgo_initialize}, {"scePlayGoTerminate",playgo_terminate},
    {"scePlayGoOpen",playgo_open}, {"scePlayGoClose",playgo_close},
    {"scePlayGoGetChunkId",playgo_chunk_ids}, {"scePlayGoGetLocus",playgo_locus},
    {"scePlayGoGetProgress",playgo_progress}, {"scePlayGoGetEta",playgo_eta}, {"scePlayGoPrefetch",playgo_prefetch},
    {"scePlayGoGetToDoList",playgo_get_todo}, {"scePlayGoSetToDoList",playgo_set_todo},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoSetInstallSpeed",playgo_set_speed},
    {"scePlayGoGetLanguageMask",playgo_get_language_mask}, {"scePlayGoSetLanguageMask",playgo_set_language_mask},
};
uintptr_t runtime_playgo_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
