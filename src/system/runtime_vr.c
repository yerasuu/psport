/* PlayStation VR on a console with no headset connected: libSceHmd initializes and reports no
 * device, so no headset can be opened and reprojection, though it can be set up, never starts;
 * libSceVrTracker initializes but has no device to track, since the headset cannot be opened and
 * the PlayStation Camera is absent (runtime_devices.c); libSceSocialScreen only mirrors the TV,
 * since separate mode needs the headset's processor unit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* ---- libSceHmd ---- */
#define HMD_ALREADY_INITIALIZED ((int32_t)0x81110001)
#define HMD_NOT_INITIALIZED ((int32_t)0x81110002)
#define HMD_HANDLE_INVALID ((int32_t)0x81110003)
/* NOTE: from memory, not confirmed against the SDK. */
#define HMD_DEVICE_DISCONNECTED ((int32_t)0x81110005)
#define HMD_PARAMETER_NULL ((int32_t)0x81110008)
#define HMD_DEVICE_STATUS_NOT_DETECTED 2
/* NOTE: the reprojection buffers' sizes and alignments are from memory, not confirmed. */
#define HMD_REPROJECTION_ONION_SIZE 0x810
#define HMD_REPROJECTION_ONION_ALIGN 0x100
#define HMD_REPROJECTION_GARLIC_SIZE 0x100000
#define HMD_REPROJECTION_GARLIC_ALIGN 0x100
static atomic_int s_is_hmd_initialized, s_is_reprojection_initialized;
static ABI int32_t hmd_init(const void *p_param) {
    if (!p_param) return HMD_PARAMETER_NULL;
    int is_initialized=0;
    if (!atomic_compare_exchange_strong(&s_is_hmd_initialized,&is_initialized,1)) return HMD_ALREADY_INITIALIZED;
    puts("Runtime: Hmd initialized; no PlayStation VR headset is connected");
    return 0;
}
static ABI int32_t hmd_term(void) { return atomic_exchange(&s_is_hmd_initialized,0) ? 0 : HMD_NOT_INITIALIZED; }
/* OrbisHmdDeviceInformation (32 bytes): status, user, reserved, panel size, latencies, HMU mount. */
static ABI int32_t hmd_device_info(int32_t *p_info) {
    if (!p_info) return HMD_PARAMETER_NULL;
    if (!atomic_load(&s_is_hmd_initialized)) return HMD_NOT_INITIALIZED;
    memset(p_info,0,32);
    p_info[0]=HMD_DEVICE_STATUS_NOT_DETECTED;
    p_info[1]=-1;
    return 0;
}
static ABI int32_t hmd_handle_invalid(void) { return HMD_HANDLE_INVALID; }
static ABI int32_t hmd_open(int32_t h_user, int32_t type, int32_t i_device, const void *p_param) {
    (void)h_user; (void)type; (void)i_device; (void)p_param;
    return atomic_load(&s_is_hmd_initialized) ? HMD_DEVICE_DISCONNECTED : HMD_NOT_INITIALIZED;
}
static ABI int32_t hmd_close(int32_t h_hmd) {
    (void)h_hmd;
    return atomic_load(&s_is_hmd_initialized) ? HMD_HANDLE_INVALID : HMD_NOT_INITIALIZED;
}
static ABI int32_t reprojection_onion_size(void) { return HMD_REPROJECTION_ONION_SIZE; }
static ABI int32_t reprojection_onion_align(void) { return HMD_REPROJECTION_ONION_ALIGN; }
static ABI int32_t reprojection_garlic_size(void) { return HMD_REPROJECTION_GARLIC_SIZE; }
static ABI int32_t reprojection_garlic_align(void) { return HMD_REPROJECTION_GARLIC_ALIGN; }
static ABI int32_t reprojection_initialize(void) {
    if (!atomic_load(&s_is_hmd_initialized)) return HMD_NOT_INITIALIZED;
    int is_initialized=0;
    return atomic_compare_exchange_strong(&s_is_reprojection_initialized,&is_initialized,1) ? 0 : HMD_ALREADY_INITIALIZED;
}
static ABI int32_t reprojection_finalize(void) { return atomic_exchange(&s_is_reprojection_initialized,0) ? 0 : HMD_NOT_INITIALIZED; }
/* Display buffers, user events and the output colour are kept for a reprojection that never starts
 * without a headset, so nothing ever reads them. */
static ABI int32_t reprojection_setup(void) { return atomic_load(&s_is_reprojection_initialized) ? 0 : HMD_NOT_INITIALIZED; }
static ABI int32_t reprojection_start(void) {
    if (!atomic_load(&s_is_reprojection_initialized)) return HMD_NOT_INITIALIZED;
    return HMD_DEVICE_DISCONNECTED;
}

/* ---- libSceVrTracker ---- */
/* NOTE: libSceVrTracker's error codes and its query result's layout are from memory, not
 * confirmed against the SDK. */
#define VR_TRACKER_NOT_INITIALIZED ((int32_t)0x81260001)
#define VR_TRACKER_ALREADY_INITIALIZED ((int32_t)0x81260002)
#define VR_TRACKER_INVALID_ARGUMENT ((int32_t)0x81260003)
#define VR_TRACKER_DEVICE_NOT_REGISTERED ((int32_t)0x81260005)
#define VR_TRACKER_ONION_SIZE 0x100000
#define VR_TRACKER_GARLIC_SIZE 0x400000
#define VR_TRACKER_WORK_SIZE 0x100000
#define VR_TRACKER_ALIGN 0x10000
/* SceVrTrackerQueryMemoryResult up to its reserved tail. */
typedef struct {
    uint32_t cb_result;
    uint32_t cb_onion, cb_onion_align, cb_garlic, cb_garlic_align, cb_work, cb_work_align;
} VrTrackerMemory;
static atomic_int s_is_tracker_initialized;
static ABI int32_t tracker_query_memory(const void *p_param, VrTrackerMemory *p_out_memory) {
    if (!p_param || !p_out_memory) return VR_TRACKER_INVALID_ARGUMENT;
    *p_out_memory=(VrTrackerMemory){sizeof(*p_out_memory),VR_TRACKER_ONION_SIZE,VR_TRACKER_ALIGN,VR_TRACKER_GARLIC_SIZE,
                                    VR_TRACKER_ALIGN,VR_TRACKER_WORK_SIZE,VR_TRACKER_ALIGN};
    return 0;
}
static ABI int32_t tracker_init(const void *p_param) {
    if (!p_param) return VR_TRACKER_INVALID_ARGUMENT;
    int is_initialized=0;
    if (!atomic_compare_exchange_strong(&s_is_tracker_initialized,&is_initialized,1)) return VR_TRACKER_ALREADY_INITIALIZED;
    puts("Runtime: VrTracker initialized; no device can be registered (no headset, no camera)");
    return 0;
}
static ABI int32_t tracker_term(void) { return atomic_exchange(&s_is_tracker_initialized,0) ? 0 : VR_TRACKER_NOT_INITIALIZED; }
static ABI int32_t tracker_get_time(uint64_t *p_out_time_us) {
    if (!atomic_load(&s_is_tracker_initialized)) return VR_TRACKER_NOT_INITIALIZED;
    if (!p_out_time_us) return VR_TRACKER_INVALID_ARGUMENT;
    *p_out_time_us=runtime_wait_clock()/1000;
    return 0;
}
/* Registering, tracking and every per-frame step need a device, and none can be registered. */
static ABI int32_t tracker_no_device(void) {
    return atomic_load(&s_is_tracker_initialized) ? VR_TRACKER_DEVICE_NOT_REGISTERED : VR_TRACKER_NOT_INITIALIZED;
}

/* ---- libSceSocialScreen ---- */
/* NOTE: libSceSocialScreen's error codes are from memory, not confirmed against the SDK. */
#define SOCIAL_SCREEN_NOT_INITIALIZED ((int32_t)0x81270001)
#define SOCIAL_SCREEN_ALREADY_INITIALIZED ((int32_t)0x81270002)
#define SOCIAL_SCREEN_INVALID_ARGUMENT ((int32_t)0x81270003)
#define SOCIAL_SCREEN_NOT_SUPPORTED ((int32_t)0x81270004)
#define SOCIAL_SCREEN_MODE_MIRRORING 0
static atomic_int s_is_social_screen_initialized;
static ABI int32_t social_screen_initialize(void) {
    int is_initialized=0;
    if (!atomic_compare_exchange_strong(&s_is_social_screen_initialized,&is_initialized,1)) return SOCIAL_SCREEN_ALREADY_INITIALIZED;
    puts("Runtime: SocialScreen initialized; mirroring only (no PlayStation VR processor unit)");
    return 0;
}
static ABI int32_t social_screen_terminate(void) {
    return atomic_exchange(&s_is_social_screen_initialized,0) ? 0 : SOCIAL_SCREEN_NOT_INITIALIZED;
}
/* The parameter block's layout is not known here, so its defaults stay whatever the caller put
 * there; no separate mode it could configure ever opens. */
static ABI int32_t social_screen_separate_parameter(void *p_param) {
    if (!p_param) return SOCIAL_SCREEN_INVALID_ARGUMENT;
    static atomic_int s_is_logged;
    if (!atomic_exchange(&s_is_logged,1)) puts("Runtime: SocialScreen separate mode parameters left as the caller set them");
    return 0;
}
static ABI int32_t social_screen_configure_separate(const void *p_param) {
    if (!atomic_load(&s_is_social_screen_initialized)) return SOCIAL_SCREEN_NOT_INITIALIZED;
    return p_param ? 0 : SOCIAL_SCREEN_INVALID_ARGUMENT;
}
static ABI int32_t social_screen_open_separate(void) {
    return atomic_load(&s_is_social_screen_initialized) ? SOCIAL_SCREEN_NOT_SUPPORTED : SOCIAL_SCREEN_NOT_INITIALIZED;
}
static ABI int32_t social_screen_close_separate(void) {
    return atomic_load(&s_is_social_screen_initialized) ? 0 : SOCIAL_SCREEN_NOT_INITIALIZED;
}
static ABI int32_t social_screen_set_mode(int32_t mode) {
    if (!atomic_load(&s_is_social_screen_initialized)) return SOCIAL_SCREEN_NOT_INITIALIZED;
    return mode==SOCIAL_SCREEN_MODE_MIRRORING ? 0 : SOCIAL_SCREEN_NOT_SUPPORTED;
}

static const RuntimeExport s_exports[]={
    {"sceHmdInitialize",hmd_init}, {"sceHmdInitialize315",hmd_init}, {"sceHmdTerminate",hmd_term},
    {"sceHmdGetDeviceInformation",hmd_device_info},
    {"sceHmdGetDeviceInformationByHandle",hmd_handle_invalid}, {"sceHmdGetFieldOfView",hmd_handle_invalid},
    {"sceHmdOpen",hmd_open}, {"sceHmdClose",hmd_close},
    {"sceHmdReprojectionQueryOnionBuffSize",reprojection_onion_size},
    {"sceHmdReprojectionQueryOnionBuffAlign",reprojection_onion_align},
    {"sceHmdReprojectionQueryGarlicBuffSize",reprojection_garlic_size},
    {"sceHmdReprojectionQueryGarlicBuffAlign",reprojection_garlic_align},
    {"sceHmdReprojectionInitialize",reprojection_initialize}, {"sceHmdReprojectionFinalize",reprojection_finalize},
    {"sceHmdReprojectionSetDisplayBuffers",reprojection_setup}, {"sceHmdReprojectionUnsetDisplayBuffers",reprojection_setup},
    {"sceHmdReprojectionSetUserEventStart",reprojection_setup}, {"sceHmdReprojectionSetUserEventEnd",reprojection_setup},
    {"sceHmdReprojectionClearUserEventStart",reprojection_setup}, {"sceHmdReprojectionSetOutputMinColor",reprojection_setup},
    {"sceHmdReprojectionStop",reprojection_setup},
    {"sceHmdReprojectionStart2dVr",reprojection_start}, {"sceHmdReprojectionStartMultilayer",reprojection_start},
    {"sceVrTrackerQueryMemory",tracker_query_memory}, {"sceVrTrackerInit",tracker_init}, {"sceVrTrackerTerm",tracker_term},
    {"sceVrTrackerGetTime",tracker_get_time},
    {"sceVrTrackerRegisterDevice",tracker_no_device}, {"sceVrTrackerUnregisterDevice",tracker_no_device},
    {"sceVrTrackerGpuSubmit",tracker_no_device}, {"sceVrTrackerGpuWait",tracker_no_device},
    {"sceVrTrackerCpuProcess",tracker_no_device}, {"sceVrTrackerNotifyEndOfCpuProcess",tracker_no_device},
    {"sceVrTrackerGetResult",tracker_no_device}, {"sceVrTrackerGetPlayAreaWarningInfo",tracker_no_device},
    {"sceVrTrackerRecalibrate",tracker_no_device}, {"sceVrTrackerResetOrientationRelative",tracker_no_device},
    {"sceVrTrackerUpdateMotionSensorData",tracker_no_device},
    {"sceSocialScreenInitialize",social_screen_initialize}, {"sceSocialScreenTerminate",social_screen_terminate},
    {"sceSocialScreenInitializeSeparateModeParameter",social_screen_separate_parameter},
    {"sceSocialScreenConfigureSeparateMode",social_screen_configure_separate},
    {"sceSocialScreenOpenSeparateMode",social_screen_open_separate},
    {"sceSocialScreenCloseSeparateMode",social_screen_close_separate},
    {"sceSocialScreenSetMode",social_screen_set_mode},
};
uintptr_t runtime_vr_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
