/* libSceSystemGesture: touch pad gesture recognizers. Handles and recognizers are accepted and
 * updated, but no gesture is recognized yet, so every recognizer reports no events; while the
 * touch pad is untouched (no pad, or nobody touching it) that is what the console reports too. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#ifndef _WIN32
#include <pthread.h>
#define GESTURE_INVALID_ARGUMENT ((int32_t)0x80D80001)
#define GESTURE_HANDLES 8
#define PAD_DATA_SIZE 120
#define PAD_TOUCH_COUNT 56

/* SceSystemGestureTouchPadData: the pad's reports since the last update. */
typedef struct { int32_t pad_handle, report_count; const uint8_t *reports; } TouchPadData;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int opened[GESTURE_HANDLES];
static uint64_t unrecognized_touches;

static int valid(int32_t handle) {
    pthread_mutex_lock(&lock);
    int ok=handle>=0 && handle<GESTURE_HANDLES && opened[handle];
    pthread_mutex_unlock(&lock);
    return ok;
}
static ABI int32_t gesture_open(int32_t input_type,const void *param) {
    (void)param;
    if (input_type!=0) return GESTURE_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int32_t handle=GESTURE_INVALID_ARGUMENT;
    for (int i=0;i<GESTURE_HANDLES;++i) if (!opened[i]) { opened[i]=1; handle=i; break; }
    pthread_mutex_unlock(&lock);
    return handle;
}
static ABI int32_t gesture_close(int32_t handle) {
    if (!valid(handle)) return GESTURE_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock); opened[handle]=0; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t primitive_initialize(const void *param) { (void)param; return 0; }
static ABI int32_t primitive_finalize(void) { return 0; }
static ABI int32_t primitive_update(int32_t handle,const TouchPadData *data) {
    if (!valid(handle) || !data || (data->report_count>0 && !data->reports)) return GESTURE_INVALID_ARGUMENT;
    for (int32_t i=0;i<data->report_count;++i)
        if (data->reports[(size_t)i*PAD_DATA_SIZE+PAD_TOUCH_COUNT]) {
            if (!__atomic_fetch_add(&unrecognized_touches,1,__ATOMIC_RELAXED)) {
                puts("Runtime: SystemGesture: the touch pad is touched, but no gesture is recognized yet");
                runtime_calls_hle_stub("sceSystemGestureUpdatePrimitiveTouchRecognizer touch",&unrecognized_touches);
            }
            break;
        }
    return 0;
}
static ABI int32_t recognizer_create(int32_t handle,void *recognizer,uint32_t type,const void *rect,const void *param) {
    (void)rect; (void)param;
    if (!valid(handle) || !recognizer || !type || type>32 || (type&(type-1))) return GESTURE_INVALID_ARGUMENT;
    return 0;
}
static ABI int32_t recognizer_attach(int32_t handle,void *recognizer) {
    return valid(handle) && recognizer ? 0 : GESTURE_INVALID_ARGUMENT;
}
static ABI int32_t recognizers_update(int32_t handle) { return valid(handle) ? 0 : GESTURE_INVALID_ARGUMENT; }
static ABI int32_t recognizer_events(int32_t handle,const void *recognizer) {
    return valid(handle) && recognizer ? 0 : GESTURE_INVALID_ARGUMENT;
}
/* With no event ever counted, every index is past the end. */
static ABI int32_t recognizer_event_by_index(int32_t h_gesture,const void *p_recognizer,uint32_t i_event,void *p_out_event) {
    (void)h_gesture; (void)p_recognizer; (void)i_event; (void)p_out_event;
    return GESTURE_INVALID_ARGUMENT;
}

static const RuntimeExport exports[]={
    {"sceSystemGestureOpen",gesture_open}, {"sceSystemGestureClose",gesture_close},
    {"sceSystemGestureInitializePrimitiveTouchRecognizer",primitive_initialize},
    {"sceSystemGestureFinalizePrimitiveTouchRecognizer",primitive_finalize},
    {"sceSystemGestureUpdatePrimitiveTouchRecognizer",primitive_update},
    {"sceSystemGestureCreateTouchRecognizer",recognizer_create},
    {"sceSystemGestureAppendTouchRecognizer",recognizer_attach},
    {"sceSystemGestureRemoveTouchRecognizer",recognizer_attach},
    {"sceSystemGestureUpdateAllTouchRecognizer",recognizers_update},
    {"sceSystemGestureGetTouchEventsCount",recognizer_events},
    {"sceSystemGestureGetTouchEventByIndex",recognizer_event_by_index},
};
uintptr_t runtime_gesture_resolve(const char *sz_name) { return RUNTIME_LOOKUP(exports,sz_name); }
#else
uintptr_t runtime_gesture_resolve(const char *name) { (void)name; return 0; }
#endif
