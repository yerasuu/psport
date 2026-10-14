/* Streaming, capture and crash reporting on a console where none of them is running: no Remote
 * Play or Share Play session ever connects, nothing is recorded, and no debugger collects core
 * dumps. The game's permissions and handlers are accepted and kept. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32

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
static CoredumpHandler s_pfn_coredump_handler;
static ABI int32_t coredump_register_handler(CoredumpHandler pfn_handler,uint64_t cb_stack,void *p_argument) {
    (void)cb_stack; (void)p_argument;
    s_pfn_coredump_handler=pfn_handler;
    return 0;
}
static ABI int32_t coredump_unregister_handler(void) { s_pfn_coredump_handler=NULL; return 0; }
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

static const RuntimeExport exports[]={
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
uintptr_t runtime_capture_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
#else
uintptr_t runtime_capture_resolve(const char *sz_name) { (void)sz_name; return 0; }
#endif
