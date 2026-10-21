/* libSceSystemGesture: touch pad gesture recognizers. The primitive touch recognizer follows the
 * firmware (kb #689): every touch the title's pad reports carry becomes a primitive that begins,
 * stays active while its id is reported, and ends or is cancelled. The touch recognizers (tap, drag,
 * press, pinch, rotation, flick) are accepted and updated, but recognize no gesture yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_pad.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#define GESTURE_INVALID_ARGUMENT ((int32_t)0x80890001)
#define GESTURE_NOT_INITIALIZED ((int32_t)0x80890003)
#define GESTURE_INDEX_OUT_OF_RANGE ((int32_t)0x80890004)
#define GESTURE_NOT_FOUND ((int32_t)0x80890005)
#define GESTURE_INVALID_HANDLE ((int32_t)0x80890006)
#define GESTURE_NO_HANDLE ((int32_t)0x8089000A)
#define GESTURE_HANDLES 8
#define GESTURE_HANDLE_TAG 0x47000000u
/* Two touches per report, and the ones that ended in the previous report until the next. */
#define GESTURE_PRIMITIVES 4
#define GESTURE_CANCEL_GAP_US 200000
#define GESTURE_SDK_SKIPS_REPEATED_TIMESTAMP 0x3500000u
#define PAD_BUTTON_INTERCEPTED 0x80000000u
#define RECOGNIZER_MAGIC 0x35547435u
#define OFF_RECOGNIZER_MAGIC 0xB04

enum { TOUCH_BEGIN=1, TOUCH_ACTIVE=2, TOUCH_END=3, TOUCH_CANCELLED=4 };

/* SceSystemGestureTouchPadData: the pad reports since the last update. */
typedef struct { int32_t h_pad, c_reports; const PadData *p_reports; uint8_t reserve[8]; } TouchPadData;
typedef struct { float x, y; } Vector2;
typedef struct {
    uint32_t state;
    uint16_t primitive_id;
    uint8_t is_updated, reserve0;
    Vector2 pressed_position, current_position, delta_vector;
    uint64_t delta_time, elapsed_time;
    uint8_t reserve[32];
} PrimitiveTouchEvent;
_Static_assert(sizeof(PrimitiveTouchEvent)==0x50,"SceSystemGesturePrimitiveTouchEvent layout");
/* Everything up to the reserved tail, which only GetPrimitiveTouchEvents clears. */
#define CB_PRIMITIVE_EVENT_COPY __builtin_offsetof(PrimitiveTouchEvent,reserve)

typedef struct { PrimitiveTouchEvent event; uint64_t begin_timestamp; } Primitive;
typedef struct {
    int32_t h_gesture;
    int is_opened, has_timestamp;
    uint64_t last_timestamp;
    int c_primitives;
    Primitive primitives[GESTURE_PRIMITIVES];
} Gesture;

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_is_initialized;
static uint32_t s_serial;
static Gesture s_gestures[GESTURE_HANDLES];
static uint64_t s_c_primitive_touches, s_c_unrecognized_touches;
static uint64_t s_c_created_recognizers, s_c_appended_recognizers, s_c_removed_recognizers, s_c_event_counts;

static Gesture *find_gesture(int32_t h_gesture) {
    const uint32_t bits=(uint32_t)h_gesture, i_slot=bits&0xFF;
    if ((bits&0xFF000000u)!=GESTURE_HANDLE_TAG || i_slot>=GESTURE_HANDLES) return NULL;
    Gesture *p_gesture=&s_gestures[i_slot];
    return p_gesture->is_opened && p_gesture->h_gesture==h_gesture ? p_gesture : NULL;
}
/* Takes the lock, which leave() drops whatever the result. */
static int32_t enter(int32_t h_gesture, Gesture **pp_out_gesture) {
    pthread_mutex_lock(&s_lock);
    if (!s_is_initialized) return GESTURE_NOT_INITIALIZED;
    *pp_out_gesture=find_gesture(h_gesture);
    return *pp_out_gesture ? 0 : GESTURE_INVALID_HANDLE;
}
static int32_t leave(int32_t result) {
    pthread_mutex_unlock(&s_lock);
    return result;
}

static ABI int32_t gesture_open(int32_t input_type,const void *p_param) {
    (void)p_param;
    if (input_type!=0) return GESTURE_INVALID_ARGUMENT;
    pthread_mutex_lock(&s_lock);
    if (!s_is_initialized) return leave(GESTURE_NOT_INITIALIZED);
    for (uint32_t i=0;i<GESTURE_HANDLES;++i) {
        Gesture *p_gesture=&s_gestures[i];
        if (p_gesture->is_opened) continue;
        s_serial=(s_serial+1)&0xFFFF;
        *p_gesture=(Gesture){.h_gesture=(int32_t)(GESTURE_HANDLE_TAG|s_serial<<8|i),.is_opened=1};
        return leave(p_gesture->h_gesture);
    }
    return leave(GESTURE_NO_HANDLE);
}
static ABI int32_t gesture_close(int32_t h_gesture) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (!result) p_gesture->is_opened=0;
    return leave(result);
}
static ABI int32_t primitive_initialize(const void *p_param) {
    (void)p_param;
    pthread_mutex_lock(&s_lock);
    s_is_initialized=1;
    return leave(0);
}
static ABI int32_t primitive_finalize(void) {
    pthread_mutex_lock(&s_lock);
    s_is_initialized=0;
    memset(s_gestures,0,sizeof(s_gestures));
    return leave(0);
}
static ABI int32_t primitive_reset(int32_t h_gesture) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (!result) { p_gesture->c_primitives=0; p_gesture->has_timestamp=0; }
    return leave(result);
}

static const PadTouch *find_touch(const PadData *p_report, uint16_t primitive_id) {
    for (int i=0;i<p_report->touch_count && i<2;++i)
        if (p_report->touches[i].id==primitive_id) return &p_report->touches[i];
    return NULL;
}
static int is_tracked(const Gesture *p_gesture, uint16_t primitive_id) {
    for (int i=0;i<p_gesture->c_primitives;++i)
        if (p_gesture->primitives[i].event.primitive_id==primitive_id) return 1;
    return 0;
}
/* Ended and cancelled primitives are listed for one update, then released at the next. */
static void sample_report(Gesture *p_gesture, const PadData *p_report) {
    int c_kept=0;
    for (int i=0;i<p_gesture->c_primitives;++i) {
        const uint32_t state=p_gesture->primitives[i].event.state;
        if (state==TOUCH_BEGIN || state==TOUCH_ACTIVE) p_gesture->primitives[c_kept++]=p_gesture->primitives[i];
    }
    p_gesture->c_primitives=c_kept;
    if (!p_report->connected) { p_gesture->c_primitives=0; p_gesture->has_timestamp=0; return; }
    const uint64_t timestamp=p_report->timestamp;
    const uint64_t delta_time=p_gesture->has_timestamp && timestamp>=p_gesture->last_timestamp
                                  ? timestamp-p_gesture->last_timestamp : 0;
    const int is_cancelled=(p_report->buttons&PAD_BUTTON_INTERCEPTED) || delta_time>GESTURE_CANCEL_GAP_US;
    p_gesture->last_timestamp=timestamp; p_gesture->has_timestamp=1;
    for (int i=0;i<p_gesture->c_primitives;++i) {
        Primitive *p_primitive=&p_gesture->primitives[i];
        PrimitiveTouchEvent *p_event=&p_primitive->event;
        const PadTouch *p_touch=is_cancelled ? NULL : find_touch(p_report,p_event->primitive_id);
        p_event->delta_time=delta_time;
        p_event->elapsed_time=timestamp>=p_primitive->begin_timestamp ? timestamp-p_primitive->begin_timestamp : 0;
        if (!p_touch) {
            p_event->state=is_cancelled ? TOUCH_CANCELLED : TOUCH_END;
            p_event->delta_vector=(Vector2){0,0}; p_event->is_updated=1;
            continue;
        }
        const Vector2 position={(float)p_touch->x,(float)p_touch->y};
        p_event->delta_vector=(Vector2){position.x-p_event->current_position.x,position.y-p_event->current_position.y};
        p_event->current_position=position;
        p_event->state=TOUCH_ACTIVE;
        p_event->is_updated=p_event->delta_vector.x!=0 || p_event->delta_vector.y!=0;
    }
    if (is_cancelled) return;
    for (int i=0;i<p_report->touch_count && i<2;++i) {
        const PadTouch *p_touch=&p_report->touches[i];
        if (is_tracked(p_gesture,p_touch->id) || p_gesture->c_primitives>=GESTURE_PRIMITIVES) continue;
        const Vector2 position={(float)p_touch->x,(float)p_touch->y};
        p_gesture->primitives[p_gesture->c_primitives++]=(Primitive){
            .event={.state=TOUCH_BEGIN,.primitive_id=p_touch->id,.is_updated=1,.pressed_position=position,
                    .current_position=position},
            .begin_timestamp=timestamp};
        if (!__atomic_fetch_add(&s_c_primitive_touches,1,__ATOMIC_RELAXED))
            printf("Runtime: SystemGesture: first primitive touch (id %u at %u,%u) on handle 0x%x\n",p_touch->id,
                   p_touch->x,p_touch->y,(uint32_t)p_gesture->h_gesture);
    }
}
static ABI int32_t primitive_update(int32_t h_gesture,const TouchPadData *p_data) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (result) return leave(result);
    if (!p_data || p_data->c_reports>1 || (p_data->c_reports==1 && !p_data->p_reports))
        return leave(GESTURE_INVALID_ARGUMENT);
    if (p_data->c_reports<1) return leave(0);
    const PadData *p_report=p_data->p_reports;
    if (runtime_process_sdk_version()>=GESTURE_SDK_SKIPS_REPEATED_TIMESTAMP && p_gesture->has_timestamp &&
        p_report->timestamp==p_gesture->last_timestamp)
        return leave(0);
    sample_report(p_gesture,p_report);
    return leave(0);
}

/* The i_event-th primitive in the order the firmware lists them: BEGIN, ACTIVE, END, CANCELLED. */
static const Primitive *ordered_primitive(const Gesture *p_gesture, uint32_t i_event) {
    for (uint32_t state=TOUCH_BEGIN;state<=TOUCH_CANCELLED;++state)
        for (int i=0;i<p_gesture->c_primitives;++i)
            if (p_gesture->primitives[i].event.state==state && !i_event--) return &p_gesture->primitives[i];
    return NULL;
}
static ABI int32_t primitive_events(int32_t h_gesture,PrimitiveTouchEvent *p_out_events,uint32_t c_capacity,
                                    uint32_t *p_out_count) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (result) return leave(result);
    if (!p_out_count || (c_capacity && !p_out_events)) return leave(GESTURE_INVALID_ARGUMENT);
    if (c_capacity) memset(p_out_events,0,(size_t)c_capacity*sizeof(*p_out_events));
    uint32_t c_events=0;
    for (const Primitive *p_primitive;c_events<c_capacity && (p_primitive=ordered_primitive(p_gesture,c_events));
         ++c_events)
        memcpy(&p_out_events[c_events],&p_primitive->event,CB_PRIMITIVE_EVENT_COPY);
    *p_out_count=c_events;
    return leave(0);
}
static ABI int32_t primitive_events_count(int32_t h_gesture) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    return leave(result ? result : (uint8_t)p_gesture->c_primitives);
}
static ABI int32_t primitive_event_by_index(int32_t h_gesture,uint32_t i_event,PrimitiveTouchEvent *p_out_event) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (result) return leave(result);
    if (!p_out_event) return leave(GESTURE_INVALID_ARGUMENT);
    const Primitive *p_primitive=ordered_primitive(p_gesture,i_event);
    if (!p_primitive) return leave(GESTURE_INDEX_OUT_OF_RANGE);
    memcpy(p_out_event,&p_primitive->event,CB_PRIMITIVE_EVENT_COPY);
    return leave(0);
}
static ABI int32_t primitive_event_by_id(int32_t h_gesture,uint16_t primitive_id,PrimitiveTouchEvent *p_out_event) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (result) return leave(result);
    if (!p_out_event) return leave(GESTURE_INVALID_ARGUMENT);
    for (int i=0;i<p_gesture->c_primitives;++i)
        if (p_gesture->primitives[i].event.primitive_id==primitive_id) {
            memcpy(p_out_event,&p_gesture->primitives[i].event,CB_PRIMITIVE_EVENT_COPY);
            return leave(0);
        }
    return leave(GESTURE_NOT_FOUND);
}

/* A touch recognizer call accepted without recognizing anything: logged once, counted always. */
static void count_stub(const char *sz_function, uint64_t *p_count) {
    if (__atomic_fetch_add(p_count,1,__ATOMIC_RELAXED)) return;
    printf("Runtime: SystemGesture: %s accepted, but touch recognizers recognize no gesture yet\n",sz_function);
    runtime_calls_hle_stub(sz_function,p_count);
}
static int is_recognizer(const uint8_t *p_recognizer) {
    uint32_t magic;
    memcpy(&magic,p_recognizer+OFF_RECOGNIZER_MAGIC,sizeof(magic));
    return magic==RECOGNIZER_MAGIC;
}
static ABI int32_t recognizer_create(int32_t h_gesture,uint8_t *p_recognizer,uint32_t type,const void *p_rect,
                                     const void *p_param) {
    (void)p_rect; (void)p_param;
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (result) return leave(result);
    if (!p_recognizer || !type || type>32 || (type&(type-1))) return leave(GESTURE_INVALID_ARGUMENT);
    const uint32_t magic=RECOGNIZER_MAGIC;
    memcpy(p_recognizer+OFF_RECOGNIZER_MAGIC,&magic,sizeof(magic));
    count_stub("sceSystemGestureCreateTouchRecognizer",&s_c_created_recognizers);
    return leave(0);
}
static int32_t check_recognizer_call(int32_t h_gesture,const void *p_recognizer,const char *sz_function,
                                     uint64_t *p_count) {
    Gesture *p_gesture;
    int32_t result=enter(h_gesture,&p_gesture);
    if (!result && !p_recognizer) result=GESTURE_INVALID_ARGUMENT;
    if (!result) count_stub(sz_function,p_count);
    return leave(result);
}
static ABI int32_t recognizer_append(int32_t h_gesture,void *p_recognizer) {
    return check_recognizer_call(h_gesture,p_recognizer,"sceSystemGestureAppendTouchRecognizer",
                                 &s_c_appended_recognizers);
}
static ABI int32_t recognizer_remove(int32_t h_gesture,void *p_recognizer) {
    return check_recognizer_call(h_gesture,p_recognizer,"sceSystemGestureRemoveTouchRecognizer",
                                 &s_c_removed_recognizers);
}
static ABI int32_t recognizers_update(int32_t h_gesture) {
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (!result && p_gesture->c_primitives && !__atomic_fetch_add(&s_c_unrecognized_touches,1,__ATOMIC_RELAXED)) {
        puts("Runtime: SystemGesture: the touch pad is touched, but the touch recognizers recognize no gesture yet");
        runtime_calls_hle_stub("sceSystemGestureUpdateAllTouchRecognizer touch",&s_c_unrecognized_touches);
    }
    return leave(result);
}
static ABI int32_t recognizer_events(int32_t h_gesture,const void *p_recognizer) {
    return check_recognizer_call(h_gesture,p_recognizer,"sceSystemGestureGetTouchEventsCount",&s_c_event_counts);
}
/* With no gesture ever recognized, every index is past the end. */
static ABI int32_t recognizer_event_by_index(int32_t h_gesture,const void *p_recognizer,uint32_t i_event,
                                             void *p_out_event) {
    (void)i_event;
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (result) return leave(result);
    return leave(p_recognizer && p_out_event ? GESTURE_INDEX_OUT_OF_RANGE : GESTURE_INVALID_ARGUMENT);
}
/* The firmware looks the id up among the recognizer's 16 event slots; none is ever filled here. */
static ABI int32_t recognizer_event_by_id(int32_t h_gesture,const uint8_t *p_recognizer,uint32_t event_id,
                                          void *p_out_event) {
    (void)event_id;
    Gesture *p_gesture;
    const int32_t result=enter(h_gesture,&p_gesture);
    if (result) return leave(result);
    if (!p_recognizer || !p_out_event) return leave(GESTURE_INVALID_ARGUMENT);
    return leave(is_recognizer(p_recognizer) ? GESTURE_NOT_FOUND : GESTURE_NOT_INITIALIZED);
}

static const RuntimeExport s_exports[]={
    {"sceSystemGestureOpen",gesture_open}, {"sceSystemGestureClose",gesture_close},
    {"sceSystemGestureInitializePrimitiveTouchRecognizer",primitive_initialize},
    {"sceSystemGestureFinalizePrimitiveTouchRecognizer",primitive_finalize},
    {"sceSystemGestureResetPrimitiveTouchRecognizer",primitive_reset},
    {"sceSystemGestureUpdatePrimitiveTouchRecognizer",primitive_update},
    {"sceSystemGestureGetPrimitiveTouchEvents",primitive_events},
    {"sceSystemGestureGetPrimitiveTouchEventsCount",primitive_events_count},
    {"sceSystemGestureGetPrimitiveTouchEventByIndex",primitive_event_by_index},
    {"sceSystemGestureGetPrimitiveTouchEventByPrimitiveID",primitive_event_by_id},
    {"sceSystemGestureCreateTouchRecognizer",recognizer_create},
    {"sceSystemGestureAppendTouchRecognizer",recognizer_append},
    {"sceSystemGestureRemoveTouchRecognizer",recognizer_remove},
    {"sceSystemGestureUpdateAllTouchRecognizer",recognizers_update},
    {"sceSystemGestureGetTouchEventsCount",recognizer_events},
    {"sceSystemGestureGetTouchEventByIndex",recognizer_event_by_index},
    {"sceSystemGestureGetTouchEventByEventID",recognizer_event_by_id},
};
uintptr_t runtime_gesture_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
#else
uintptr_t runtime_gesture_resolve(const char *name) { (void)name; return 0; }
#endif
