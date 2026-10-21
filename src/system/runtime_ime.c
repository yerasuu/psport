/* libSceIme's on-screen keyboard over the port's text entry: the game window takes the host
 * keyboard's typing or the controller's on the overlay's keyboard, which draws the panel, as for
 * the IME dialog. sceImeUpdate (runtime_keyboard.c) hands the game's handler the open event, the
 * text as it changes (written to the game's buffer too) and Enter or Esc as the panel's enter or
 * close button; the game then closes the panel. Without a window nobody can type, so the entry ends
 * at once as entered with PS_IME_TEXT or the text it opened with, as the IME dialog does. The caret
 * stays at the end of the text and no input method composes. */
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
#define IME_INVALID_PARAM ((int32_t)0x80BC0030)
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

int runtime_ime_charset(uint32_t type) {
    return type==TYPE_NUMBER ? PS_TEXT_DIGITS : type==TYPE_DEFAULT ? PS_TEXT_ANY : PS_TEXT_LATIN;
}

void runtime_ime_filter_entered(const char *sz_entered, uint32_t type, uint32_t max_length, char *sz_out, size_t cb_out) {
    const int charset=runtime_ime_charset(type);
    size_t cb_written=0, c_dropped=0;
    uint32_t c_units=0;
    for (const unsigned char *p_cursor=(const unsigned char *)sz_entered; *p_cursor;) {
        const unsigned char lead=*p_cursor;
        const size_t cb_lead=lead<0x80 ? 1 : (lead&0xE0)==0xC0 ? 2 : (lead&0xF0)==0xE0 ? 3 : 4;
        size_t cb_character=1;
        while (cb_character<cb_lead && p_cursor[cb_character]) ++cb_character;
        const uint32_t c_character_units=cb_lead==4 ? 2 : 1;
        const bool is_accepted=charset==PS_TEXT_DIGITS ? lead>='0' && lead<='9'
                              : charset==PS_TEXT_LATIN ? lead>=0x20 && lead<0x7F : lead>=0x20 && lead!=0x7F;
        if (is_accepted && c_units+c_character_units<=max_length && cb_written+cb_character<cb_out) {
            memcpy(sz_out+cb_written,p_cursor,cb_character);
            cb_written+=cb_character; c_units+=c_character_units;
        } else ++c_dropped;
        p_cursor+=cb_character;
    }
    sz_out[cb_written]=0;
    if (c_dropped) printf("Runtime: entered text \"%s\" lost %zu characters to the entry's type %u and length %u\n",sz_entered,
                          c_dropped,type,max_length);
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
    const char *sz_preset=runtime_env("PS_IME_TEXT",NULL);
    s_panel.is_typing=!sz_preset && psport_gpu_text_input_begin(s_panel.sz_delivered,"Text",p_param->max_text_length,
                                                                runtime_ime_charset(p_param->type));
    if (!s_panel.is_typing) {
        if (sz_preset) runtime_ime_filter_entered(sz_preset,p_param->type,p_param->max_text_length,s_panel.sz_entry,sizeof(s_panel.sz_entry));
        else snprintf(s_panel.sz_entry,sizeof(s_panel.sz_entry),"%s",s_panel.sz_delivered);
        s_panel.entry=ENTRY_ENTERED;
    }
    const bool is_typing=s_panel.is_typing;
    pthread_mutex_unlock(&s_lock);
    printf("Runtime: on-screen keyboard opened for user %d (type %u, up to %u characters)%s\n",p_param->user,p_param->type,
           p_param->max_text_length,is_typing ? ": type in the game window or with the controller, Enter or Options to confirm, Esc to close"
                                              : "; no window, entered as given");
    if (p_param->pfn_filter) printf("Runtime: on-screen keyboard text filter not called: the entry checks only its type\n");
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
    static size_t s_c_truncated;
    if (!p_text) return IME_INVALID_ADDRESS;
    pthread_mutex_lock(&s_lock);
    if (!s_panel.is_open) {
        pthread_mutex_unlock(&s_lock);
        return IME_NOT_OPENED;
    }
    const uint32_t c_units=length<s_panel.param.max_text_length ? length : s_panel.param.max_text_length;
    if (c_units<length && __atomic_fetch_add(&s_c_truncated,1,__ATOMIC_RELAXED)<8)
        printf("Runtime: on-screen keyboard text set with %u characters, cut to the entry's %u\n",length,c_units);
    utf16_to_utf8(p_text,c_units,s_panel.sz_delivered,sizeof(s_panel.sz_delivered));
    snprintf(s_panel.sz_entry,sizeof(s_panel.sz_entry),"%s",s_panel.sz_delivered);
    utf8_to_utf16(s_panel.sz_delivered,s_panel.param.p_text,s_panel.param.max_text_length);
    if (s_panel.is_typing) psport_gpu_text_input_replace(s_panel.sz_delivered);
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

/* sceImeFilterText's languages are SceImeLanguage bits. A mask of none takes the default the firmware
 * derives from the parameter's version; every language it knows stands in for that default here. */
#define LANGUAGE_RUSSIAN 0x400ull
#define LANGUAGE_JAPANESE 0x2000ull
#define LANGUAGE_KOREAN 0x4000ull
#define LANGUAGE_CHINESE 0x18000ull
#define LANGUAGE_ARABIC 0x1000000ull
#define LANGUAGE_THAI 0x4000000ull
#define LANGUAGE_GREEK 0x10000000ull
#define LANGUAGE_VIETNAMESE 0x40000000ull
#define LANGUAGE_ROMANIAN 0x80000000ull
#define LANGUAGE_UKRAINIAN 0x200000000ull
#define LANGUAGE_CARONS 0x188000000ull
#define LANGUAGE_ALL 0x3FFFFFFFFull
#define LANGUAGES_WITHOUT_LATIN_EXTENDED (0x4ull|0x40000ull|LANGUAGE_RUSSIAN|LANGUAGE_JAPANESE|LANGUAGE_KOREAN| \
                                          LANGUAGE_CHINESE|LANGUAGE_ARABIC|LANGUAGE_THAI|LANGUAGE_GREEK|LANGUAGE_UKRAINIAN)
enum { FILTER_LATIN=0x100, FILTER_DEFAULT_EXTENDED=0x101 };
typedef struct { uint64_t languages; uint32_t type, version; } ImeFilterParam;

static bool is_in(uint32_t c, uint32_t first, uint32_t last) { return c>=first && c<=last; }
static bool is_vietnamese(uint32_t c) {
    return is_in(c,0x300,0x323) || is_in(c,0x1E00,0x1EFF) || c==0x1A0 || c==0x1A1 || c==0x1AF || c==0x1B0 || c==0x20AB;
}
static bool is_latin1_symbol(uint32_t c) {
    static const uint16_t s_symbols[]={0xA1,0xBF,0xB4,0x2018,0x2019,0x201A,0x201C,0x201D,0x201E,0xB0,0x2039,0x203A,
                                       0xAB,0xBB,0xAA,0xBA,0xD7,0xF7,0xA4,0xA2,0x20AC,0xA3,0xA5,0x20A9,0xA7,0xA6,
                                       0xB5,0xAC,0xB7,0xB9,0xB2,0xB3,0xBC,0xBD,0xBE,0x2116};
    for (size_t i=0;i<sizeof(s_symbols)/sizeof(*s_symbols);++i)
        if (c==s_symbols[i]) return true;
    return false;
}
/* The firmware asks its SJIS and GB/Big5 conversion tables; their blocks stand in for them here. */
static bool is_cjk_text(uint32_t c) {
    return is_in(c,0x3000,0x30FF) || is_in(c,0x3400,0x4DBF) || is_in(c,0x4E00,0x9FFF) || is_in(c,0xF900,0xFAFF) ||
           is_in(c,0xFF01,0xFF60) || is_in(c,0xFFE0,0xFFE6);
}
static bool is_kept(uint32_t c, uint32_t type, uint64_t languages) {
    switch (type) {
    case TYPE_BASIC_LATIN:
    case TYPE_MAIL: return c<0x7F;
    case TYPE_NUMBER: return is_in(c,'0','9') || c==',' || c=='.' || c=='-' || c=='\n';
    case FILTER_LATIN:
        return c<0x17F || ((languages&LANGUAGE_VIETNAMESE) && is_vietnamese(c)) ||
               ((languages&LANGUAGE_ROMANIAN) && is_in(c,0x218,0x21B));
    default: break;
    }
    if (c<((languages&~LANGUAGES_WITHOUT_LATIN_EXTENDED) ? 0x17Fu : 0x7Fu) || is_latin1_symbol(c)) return true;
    if ((languages&LANGUAGE_KOREAN) && (is_in(c,0x3130,0x318F) || is_in(c,0xAC00,0xD7AF))) return true;
    if ((languages&(LANGUAGE_RUSSIAN|LANGUAGE_UKRAINIAN)) && is_in(c,0x400,0x4FF)) return true;
    if ((languages&(LANGUAGE_JAPANESE|LANGUAGE_KOREAN|LANGUAGE_CHINESE)) && is_in(c,0xFF10,0xFF19)) return true;
    if ((languages&(LANGUAGE_JAPANESE|LANGUAGE_KOREAN|LANGUAGE_CHINESE)) && is_in(c,0x3000,0x303F)) return true;
    if ((languages&(LANGUAGE_JAPANESE|LANGUAGE_CHINESE)) && is_cjk_text(c)) return true;
    if ((languages&LANGUAGE_ARABIC) && (is_in(c,0x600,0x6FF) || is_in(c,0x200C,0x200F))) return true;
    if ((languages&LANGUAGE_THAI) && is_in(c,0xE00,0xE7F)) return true;
    if ((languages&LANGUAGE_GREEK) && (is_in(c,0x370,0x3E1) || is_in(c,0x3F0,0x3FF))) return true;
    if ((languages&LANGUAGE_VIETNAMESE) && is_vietnamese(c)) return true;
    if ((languages&LANGUAGE_ROMANIAN) && is_in(c,0x218,0x21B)) return true;
    return (languages&LANGUAGE_CARONS) && (c==0x2C7 || is_in(c,0x2D8,0x2DB) || c==0x2DD);
}
static ABI int32_t ime_filter_text(uint16_t *p_out, uint32_t *p_out_length, uint32_t c_out_capacity,
                                   const ImeFilterParam *p_param, const uint16_t *p_in, uint32_t c_in) {
    if (!p_out || !p_out_length || !p_param || !p_in) return IME_INVALID_ADDRESS;
    const uint32_t type=p_param->type;
    const uint64_t languages=(p_param->languages ? p_param->languages : LANGUAGE_ALL)&LANGUAGE_ALL;
    if (c_out_capacity<2 || (type>TYPE_NUMBER && type!=FILTER_LATIN && type!=FILTER_DEFAULT_EXTENDED) || !languages)
        return IME_INVALID_PARAM;
    uint32_t c_kept=0;
    for (uint32_t i=0;i<c_in && p_in[i] && c_kept<c_out_capacity-1;++i)
        if (is_kept(p_in[i],type,languages)) p_out[c_kept++]=p_in[i];
    p_out[c_kept]=0;
    *p_out_length=c_kept;
    return 0;
}

static const RuntimeExport s_exports[]={
    {"sceImeParamInit",ime_param_init}, {"sceImeGetPanelSize",ime_panel_size}, {"sceImeOpen",ime_open},
    {"sceImeClose",ime_close}, {"sceImeSetText",ime_set_text}, {"sceImeSetCaret",ime_set_caret},
    {"sceImeSetTextGeometry",ime_set_text_geometry}, {"sceImeFilterText",ime_filter_text},
};
uintptr_t runtime_ime_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
