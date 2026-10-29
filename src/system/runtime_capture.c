// SPDX-License-Identifier: GPL-2.0-or-later
/* Streaming, capture and crash reporting on a console where none of them is running: no Remote
 * Play or Share Play session ever connects, nothing is recorded, and no debugger collects core
 * dumps. The game's permissions and handlers are accepted and kept. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* ---- libSceRemoteplay ---- */
#define REMOTEPLAY_INVALID_ARGUMENT ((int32_t)0x80B10002)
#define REMOTEPLAY_CONNECTION_STATUS_DISCONNECT 0
static ABI int32_t remoteplay_initialize(void *p_heap,uint64_t cb_heap) {
    (void)p_heap; (void)cb_heap;
    puts("Runtime: Remoteplay initialized; no Remote Play session connects");
    return 0;
}
static ABI int32_t remoteplay_terminate(void) { return 0; }
/* With no session to allow or cut, the permission has nothing to act on. */
static ABI int32_t remoteplay_permission(void) { return 0; }
static ABI int32_t remoteplay_prohibit_streaming(int32_t mode) { (void)mode; return 0; }
static ABI int32_t remoteplay_connection_status(int32_t h_user,int32_t *p_out_status) {
    (void)h_user;
    if (!p_out_status) return REMOTEPLAY_INVALID_ARGUMENT;
    *p_out_status=REMOTEPLAY_CONNECTION_STATUS_DISCONNECT;
    return 0;
}

/* ---- libSceSharePlay ---- */
static ABI int32_t shareplay_initialize(void *p_heap,uint64_t cb_heap) { (void)p_heap; (void)cb_heap; return 0; }
static ABI int32_t shareplay_terminate(void) { return 0; }
static ABI int32_t shareplay_set_prohibition(int32_t mode) { (void)mode; return 0; }

/* ---- libSceVideoRecording ---- */
#define VIDEO_RECORDING_STATUS_NONE 0
static ABI int32_t video_recording_status(void) { return VIDEO_RECORDING_STATUS_NONE; }

/* ---- libSceCoredump ---- */
typedef int32_t (ABI *CoredumpHandler)(void *);
/* A console calls the handler only while it writes a core dump, which psport never does. */
static ABI int32_t coredump_register_handler(CoredumpHandler pfn_handler,uint64_t cb_stack,void *p_argument) {
    (void)cb_stack; (void)p_argument;
    printf("Coredump: handler %p registered; psport writes no core dumps, so it is never called\n",(void *)pfn_handler);
    return 0;
}
static ABI int32_t coredump_unregister_handler(void) { return 0; }
/* Only a running handler writes, and none runs without a debugger: the data is dropped. */
static ABI int64_t coredump_write_user_data(const void *p_data,uint64_t cb_data) { (void)p_data; return (int64_t)cb_data; }
static ABI int32_t coredump_attach_user_file(uint64_t id,const char *sz_path) { (void)id; (void)sz_path; return 0; }
static ABI int32_t coredump_attach_memory_region(uint64_t id,const void *p_region,uint64_t cb_region,const char *sz_name) {
    (void)id; (void)p_region; (void)cb_region; (void)sz_name;
    return 0;
}
static ABI int32_t coredump_debug_text_out(const char *p_text,int32_t length) {
    if (!p_text) return 0;
    const size_t c_chars=length<0 ? strlen(p_text) : strnlen(p_text,(size_t)length);
    printf("Coredump: %.*s\n",(int)c_chars,p_text);
    return 0;
}

/* ---- libSceVideoOut's driver registry ---- */
/* The system's capture path registers a video out driver (bus type 4): a 0x108-byte table of
 * callbacks, those at 0 to 0x28, 0x38 and 0x40 mandatory, and a 32-byte context. psport drives video
 * out itself, so the table is kept and never called. The console returns a slot value whose meaning
 * was not read; 0 is what the callers have worked with so far. */
#define VIDEO_OUT_INVALID_VALUE ((int32_t)0x80290001)
#define VIDEO_OUT_INVALID_ADDRESS ((int32_t)0x80290002)
#define CB_VIDEO_OUT_DRIVER_OPS 0x108
#define CB_VIDEO_OUT_DRIVER_CONTEXT 32
#define VIDEO_OUT_BUS_TYPES 8
static const void *s_p_video_out_drivers[VIDEO_OUT_BUS_TYPES];
static uint64_t s_c_video_out_drivers;
static ABI int64_t video_out_add_driver(int32_t bus_type, const void *const *pp_callbacks, const void *p_context, const void *p_reserved) {
    static const unsigned char s_i_mandatory[]={0,1,2,3,4,5,7,8};
    if (p_reserved || bus_type<0 || bus_type>=VIDEO_OUT_BUS_TYPES) return VIDEO_OUT_INVALID_VALUE;
    if (!runtime_memory_is_guest_range(pp_callbacks,CB_VIDEO_OUT_DRIVER_OPS) ||
        (p_context && !runtime_memory_is_guest_range(p_context,CB_VIDEO_OUT_DRIVER_CONTEXT)))
        return VIDEO_OUT_INVALID_ADDRESS;
    for (size_t i=0;i<sizeof(s_i_mandatory);++i)
        if (!pp_callbacks[s_i_mandatory[i]]) return VIDEO_OUT_INVALID_VALUE;
    __atomic_store_n(&s_p_video_out_drivers[bus_type],(const void *)pp_callbacks,__ATOMIC_RELEASE);
    if (!__atomic_fetch_add(&s_c_video_out_drivers,1,__ATOMIC_RELAXED))
        runtime_calls_hle_stub("sceVideoOutAddDriver (callbacks kept, never called: psport drives video out)",&s_c_video_out_drivers);
    printf("Runtime: VideoOut driver for bus type %d registered (callbacks %p); psport never calls it\n",bus_type,(const void *)pp_callbacks);
    return 0;
}
/* AddDriver hands out no slot, so there is none to find: the call is accepted. */
static ABI int32_t video_out_delete_driver(int64_t slot) { (void)slot; return 0; }

/* ---- libSceAvcap ---- */
/* sceVshAvcapInit connects to the system's capture service, which psport does not have: Init and
 * Term only succeed, counted, and the capture calls stay unimplemented. */
static uint64_t s_c_avcap_inits;
static ABI int32_t avcap_init(void) {
    if (!__atomic_fetch_add(&s_c_avcap_inits,1,__ATOMIC_RELAXED))
        runtime_calls_hle_stub("sceVshAvcapInit (no capture service: counted only)",&s_c_avcap_inits);
    return 0;
}
static ABI int32_t avcap_term(void) { return 0; }

static const RuntimeExport s_exports[]={
    {"sceVideoOutAddDriver",video_out_add_driver}, {"sceVideoOutDeleteDriver",video_out_delete_driver},
    {"sceVshAvcapInit",avcap_init}, {"sceVshAvcapTerm",avcap_term},
    {"sceRemoteplayInitialize",remoteplay_initialize}, {"sceRemoteplayTerminate",remoteplay_terminate},
    {"sceRemoteplayApprove",remoteplay_permission}, {"sceRemoteplayProhibit",remoteplay_permission},
    {"sceRemoteplayProhibitStreaming",remoteplay_prohibit_streaming},
    {"sceRemoteplayGetConnectionStatus",remoteplay_connection_status},
    {"sceSharePlayInitialize",shareplay_initialize}, {"sceSharePlayTerminate",shareplay_terminate},
    {"sceSharePlaySetProhibition",shareplay_set_prohibition},
    {"sceVideoRecordingGetStatus",video_recording_status}, {"sceVideoRecordingSetInfo",runtime_stub},
    {"sceVideoRecordingStart",runtime_stub}, {"sceVideoRecordingStop",runtime_stub},
    {"sceVideoRecordingClose",runtime_stub},
    {"sceCoredumpRegisterCoredumpHandler",coredump_register_handler},
    {"sceCoredumpUnregisterCoredumpHandler",coredump_unregister_handler},
    {"sceCoredumpWriteUserData",coredump_write_user_data}, {"sceCoredumpAttachUserFile",coredump_attach_user_file},
    {"sceCoredumpAttachMemoryRegionAsUserFile",coredump_attach_memory_region},
    {"sceCoredumpDebugTextOut",coredump_debug_text_out},
};
uintptr_t runtime_capture_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
