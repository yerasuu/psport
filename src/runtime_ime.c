/* libSceIme's on-screen keyboard over the port's text entry: the game window takes the host
 * keyboard's typing and the overlay draws the panel, as for the IME dialog. sceImeUpdate
 * (runtime_keyboard.c) hands the game's handler the open event, the text as it changes (written to
 * the game's buffer too) and Enter or Esc as the panel's enter or close button; the game then closes
 * the panel. Without a window nobody can type, so the entry ends at once as entered with PS_IME_TEXT
 * or the text it opened with, as the IME dialog does. The caret stays at the end of the text and no
 * input method composes. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define IME_BUSY ((int32_t)0x80BC0001)
#define IME_NOT_OPENED ((int32_t)0x80BC0002)
#define IME_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_INVALID_TYPE ((int32_t)0x80BC0011)
#define IME_INVALID_MAX_TEXT_LENGTH ((int32_t)0x80BC0016)
#define IME_INVALID_INPUT_TEXT_BUFFER ((int32_t)0x80BC0017)
#define IME_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_MAX_TEXT_LENGTH 2048
#define TEXT_BYTES (IME_MAX_TEXT_LENGTH*4+1)

enum { TYPE_DEFAULT, TYPE_BASIC_LATIN, TYPE_URL, TYPE_MAIL, TYPE_NUMBER };
enum { EVENT_OPEN=0, EVENT_UPDATE_TEXT=1, EVENT_PRESS_CLOSE=4, EVENT_PRESS_ENTER=5 };
enum { ENTRY_TYPING, ENTRY_ENTERED, ENTRY_CLOSED };

typedef ABI void (*ImeHandler)(void *p_arg, const void *p_event);
typedef ABI int32_t (*ImeTextFilter)(uint16_t *p_out, uint32_t *p_out_length, const uint16_t *p_in, uint32_t length);
typedef struct {
    int32_t user;
    uint32_t type;
    uint64_t languages;
    uint32_t enter_label, input_method;
    ImeTextFilter pfn_filter;
    uint32_t option, max_text_length;
    uint16_t *p_text;
    float x, y;
    uint32_t horizontal_alignment, vertical_alignment;
    void *p_work, *p_arg;
    ImeHandler pfn_handler;
    int8_t reserved[8];
} ImeParam;
_Static_assert(sizeof(ImeParam)==96,"OrbisImeParam layout");
typedef struct { uint32_t mode, index; int32_t length; } ImeTextArea;
typedef struct { uint16_t *p_text; uint32_t i_caret, c_areas; ImeTextArea areas[4]; } ImeEditText;
typedef struct { float x, y; uint32_t width, height; } ImeRect;
typedef struct {
    uint32_t id;
    union { ImeRect rect; ImeEditText text; int8_t reserved[64]; uint64_t align; } param;
} ImeEvent;
_Static_assert(sizeof(ImeEvent)==72,"OrbisImeEvent layout");
typedef struct { float x, y; uint32_t height, index; } ImeCaret;

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool is_open, is_open_sent, is_typing, is_end_sent;
    int entry;
    ImeParam param;
    char sz_delivered[TEXT_BYTES], sz_entry[TEXT_BYTES];
} s_panel;

void runtime_ime_panel_size(uint32_t type, uint32_t *p_out_width, uint32_t *p_out_height) {
    *p_out_width=type==TYPE_NUMBER ? 370 : 793;
    *p_out_height=type==TYPE_NUMBER ? 402 : 408;
}

static ABI void ime_param_init(ImeParam *p_param) {
    if (!p_param) return;
    memset(p_param,0,sizeof(*p_param));
    p_param->user=-1;
}
static ABI int32_t ime_panel_size(const ImeParam *p_param, uint32_t *p_out_width, uint32_t *p_out_height) {
    if (!p_param || !p_out_width || !p_out_height) return IME_INVALID_ADDRESS;
    if (p_param->type>TYPE_NUMBER) return IME_INVALID_TYPE;
    runtime_ime_panel_size(p_param->type,p_out_width,p_out_height);
    return 0;
}
static ABI int32_t ime_open(const ImeParam *p_param, const void *p_extended) {
    (void)p_extended;
    if (!p_param) return IME_INVALID_ADDRESS;
    if (p_param->user==-1) return IME_INVALID_USER_ID;
    if (p_param->type>TYPE_NUMBER) return IME_INVALID_TYPE;
    if (!p_param->max_text_length || p_param->max_text_length>IME_MAX_TEXT_LENGTH) return IME_INVALID_MAX_TEXT_LENGTH;
    if (!p_param->p_text) return IME_INVALID_INPUT_TEXT_BUFFER;
    if (!p_param->pfn_handler) return IME_INVALID_HANDLER;
    pthread_mutex_lock(&s_lock);
    if (s_panel.is_open) {
        pthread_mutex_unlock(&s_lock);
        return IME_BUSY;
    }
    memset(&s_panel,0,sizeof(s_panel));
    s_panel.is_open=true;
    s_panel.param=*p_param;
    utf16_to_utf8(p_param->p_text,p_param->max_text_length,s_panel.sz_delivered,sizeof(s_panel.sz_delivered));
    const char *sz_preset=getenv("PS_IME_TEXT");
    s_panel.is_typing=!sz_preset && psport_gpu_text_input_begin(s_panel.sz_delivered,"Text");
    if (!s_panel.is_typing) {
        snprintf(s_panel.sz_entry,sizeof(s_panel.sz_entry),"%s",sz_preset ? sz_preset : s_panel.sz_delivered);
        s_panel.entry=ENTRY_ENTERED;
    }
    const bool is_typing=s_panel.is_typing;
    pthread_mutex_unlock(&s_lock);
    printf("Runtime: on-screen keyboard opened for user %d (type %u, up to %u characters)%s\n",p_param->user,p_param->type,
           p_param->max_text_length,is_typing ? ": type in the game window, Enter to confirm, Esc to close" : "; no window, entered as given");
    return 0;
}
static ABI int32_t ime_close(void) {
    pthread_mutex_lock(&s_lock);
    const bool was_open=s_panel.is_open, was_typing=s_panel.is_typing;
    s_panel.is_open=false;
    s_panel.is_typing=false;
    pthread_mutex_unlock(&s_lock);
    if (!was_open) return IME_NOT_OPENED;
    if (was_typing) psport_gpu_text_input_end();
    return 0;
}
/* The game's own text replaces the panel's; it is not echoed back as an update. */
static ABI int32_t ime_set_text(const uint16_t *p_text, uint32_t length) {
    if (!p_text) return IME_INVALID_ADDRESS;
    pthread_mutex_lock(&s_lock);
    if (!s_panel.is_open) {
        pthread_mutex_unlock(&s_lock);
        return IME_NOT_OPENED;
    }
    const uint32_t c_units=length<s_panel.param.max_text_length ? length : s_panel.param.max_text_length;
    utf16_to_utf8(p_text,c_units,s_panel.sz_delivered,sizeof(s_panel.sz_delivered));
    snprintf(s_panel.sz_entry,sizeof(s_panel.sz_entry),"%s",s_panel.sz_delivered);
    utf8_to_utf16(s_panel.sz_delivered,s_panel.param.p_text,s_panel.param.max_text_length);
    if (s_panel.is_typing) psport_gpu_text_input_begin(s_panel.sz_delivered,"Text");
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t ime_set_caret(const ImeCaret *p_caret) {
    static bool s_is_reported;
    if (!p_caret) return IME_INVALID_ADDRESS;
    pthread_mutex_lock(&s_lock);
    const bool is_open=s_panel.is_open;
    pthread_mutex_unlock(&s_lock);
    if (!is_open) return IME_NOT_OPENED;
    if (!__atomic_exchange_n(&s_is_reported,true,__ATOMIC_RELAXED))
        printf("Runtime: on-screen keyboard caret set to %u; the port's entry keeps typing at the end of the text\n",p_caret->index);
    return 0;
}
/* Where the game draws its text field: the port's panel has a place of its own and no candidate list to put by it. */
static ABI int32_t ime_set_text_geometry(uint32_t mode, const ImeRect *p_geometry) {
    (void)mode;
    if (!p_geometry) return IME_INVALID_ADDRESS;
    pthread_mutex_lock(&s_lock);
    const bool is_open=s_panel.is_open;
    pthread_mutex_unlock(&s_lock);
    return is_open ? 0 : IME_NOT_OPENED;
}

static void edit_text(ImeEvent *p_event, uint32_t id) {
    memset(p_event,0,sizeof(*p_event));
    p_event->id=id;
    p_event->param.text.p_text=s_panel.param.p_text;
    uint32_t c_units=0;
    while (c_units<s_panel.param.max_text_length && s_panel.param.p_text[c_units]) ++c_units;
    p_event->param.text.i_caret=c_units;
}
/* The events due since the last update, in order; called under the lock. */
static int collect(ImeEvent *p_events) {
    int c_events=0;
    if (!s_panel.is_open_sent) {
        s_panel.is_open_sent=true;
        ImeEvent *p_event=&p_events[c_events++];
        memset(p_event,0,sizeof(*p_event));
        p_event->id=EVENT_OPEN;
        p_event->param.rect=(ImeRect){.x=s_panel.param.x,.y=s_panel.param.y};
        runtime_ime_panel_size(s_panel.param.type,&p_event->param.rect.width,&p_event->param.rect.height);
    }
    if (s_panel.is_typing) {
        const int state=psport_gpu_text_input_poll(s_panel.sz_entry,sizeof(s_panel.sz_entry));
        if (state) {
            s_panel.is_typing=false;
            s_panel.entry=state==1 ? ENTRY_ENTERED : ENTRY_CLOSED;
        }
    }
    if (strcmp(s_panel.sz_entry,s_panel.sz_delivered) && (s_panel.is_typing || s_panel.entry==ENTRY_ENTERED)) {
        memcpy(s_panel.sz_delivered,s_panel.sz_entry,sizeof(s_panel.sz_delivered));
        utf8_to_utf16(s_panel.sz_delivered,s_panel.param.p_text,s_panel.param.max_text_length);
        edit_text(&p_events[c_events++],EVENT_UPDATE_TEXT);
    }
    if (s_panel.entry!=ENTRY_TYPING && !s_panel.is_end_sent) {
        s_panel.is_end_sent=true;
        edit_text(&p_events[c_events++],s_panel.entry==ENTRY_ENTERED ? EVENT_PRESS_ENTER : EVENT_PRESS_CLOSE);
        printf("Runtime: on-screen keyboard %s\n",s_panel.entry==ENTRY_ENTERED ? "entered" : "closed with Esc");
    }
    return c_events;
}
/* The handler runs outside the lock: it closes the panel or sets its text from there. */
int runtime_ime_update(void *pfn_handler) {
    ImeEvent events[3];
    pthread_mutex_lock(&s_lock);
    if (!s_panel.is_open) {
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    const int c_events=collect(events);
    ImeHandler pfn_call=pfn_handler ? (ImeHandler)pfn_handler : s_panel.param.pfn_handler;
    void *p_arg=s_panel.param.p_arg;
    pthread_mutex_unlock(&s_lock);
    for (int i=0;i<c_events;++i) pfn_call(p_arg,&events[i]);
    return 1;
}

static const RuntimeExport s_exports[]={
    {"sceImeParamInit",ime_param_init}, {"sceImeGetPanelSize",ime_panel_size}, {"sceImeOpen",ime_open},
    {"sceImeClose",ime_close}, {"sceImeSetText",ime_set_text}, {"sceImeSetCaret",ime_set_caret},
    {"sceImeSetTextGeometry",ime_set_text_geometry},
};
uintptr_t runtime_ime_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
