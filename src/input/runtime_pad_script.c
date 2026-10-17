/* Scripted pad input for automated runs: PS_PAD_FILE, PS_PAD_RECORD and PS_PAD_REPLAY. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_pad.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

static uint64_t now_us(void) { return ps_host_time_monotonic_ns()/1000u; }
/* PS_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks; ps presses the Guide button (the overlay's close prompt, answered with cross or
 * circle). An empty file releases everything. */
PadInjected g_runtime_pad_injected={0,{-1,-1,-1,-1},-1};
static int s_replay_armed;      /* 1 while a PS_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t s_replay_start; /* 0: (re)start at the next sample */
void runtime_pad_read_inject(void) {
    static const char *s_sz_path; static int s_checked; static uint64_t s_last_check; static struct timespec s_mtime;
    if (!s_checked) { s_sz_path=runtime_env("PS_PAD_FILE",NULL); s_checked=1; }
    if (!s_sz_path || !*s_sz_path) return;
    uint64_t now=now_us();
    if (now-s_last_check<20000) return;
    s_last_check=now;
    PsHostStat st;
    if (ps_host_fs_stat(s_sz_path,&st)) return;
    if (st.mtime.tv_sec==s_mtime.tv_sec && st.mtime.tv_nsec==s_mtime.tv_nsec) return;
    s_mtime=st.mtime;
    FILE *p_file=fopen(s_sz_path,"r");
    if (!p_file) return;
    static const struct { const char *sz_name; uint32_t ps; } s_names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *s_sticks[]={"lx=","ly=","rx=","ry="};
    g_runtime_pad_injected.buttons=0;
    g_runtime_pad_injected.touch_side=-1;
    for (int i=0;i<4;++i) g_runtime_pad_injected.stick[i]=-1;
    int guide=0;
    char sz_token[64];
    while (fscanf(p_file,"%63s",sz_token)==1) {
        if (!strcmp(sz_token,"ps")) guide=1;
        if (!strcmp(sz_token,"replay") && s_replay_armed!=1) { s_replay_armed=1; s_replay_start=0; } /* PS_PAD_REPLAY */
        if (!strcmp(sz_token,"touchpad_left") || !strcmp(sz_token,"touchpad_right")) {
            g_runtime_pad_injected.buttons|=BTN_TOUCHPAD;
            g_runtime_pad_injected.touch_side=!strcmp(sz_token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(s_names)/sizeof(*s_names);++i) if (!strcmp(sz_token,s_names[i].sz_name)) g_runtime_pad_injected.buttons|=s_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(sz_token,s_sticks[i],3)) { int v=atoi(sz_token+3); g_runtime_pad_injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(p_file);
    if (psport_gpu_overlay_pad_file_press(guide,(g_runtime_pad_injected.buttons & BTN_CROSS)!=0,(g_runtime_pad_injected.buttons & BTN_CIRCLE)!=0))
        g_runtime_pad_injected.buttons&=~(uint32_t)(BTN_CROSS|BTN_CIRCLE);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",g_runtime_pad_injected.buttons,
           g_runtime_pad_injected.stick[0],g_runtime_pad_injected.stick[1],g_runtime_pad_injected.stick[2],g_runtime_pad_injected.stick[3]);
}
/* PS_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; PS_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in PS_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *s_p_record_file;
static uint64_t s_record_start;
static PadSample s_record_last;
void runtime_pad_record_sample(const PadData *p_pad_data) {
    static const char *s_sz_path; static int s_checked, s_is_f9_held;
    if (!s_checked) { s_sz_path=runtime_env("PS_PAD_RECORD",NULL); s_checked=1; }
    if (!s_sz_path || !*s_sz_path || !g_runtime_pad_sdl_ready) return;
    const bool *p__bool=SDL_GetKeyboardState(NULL);
    const int f9=p__bool && p__bool[SDL_SCANCODE_F9];
    if (f9 && !s_is_f9_held) {
        if (s_p_record_file) {
            fclose(s_p_record_file); s_p_record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",s_sz_path);
        } else if ((s_p_record_file=fopen(s_sz_path,"w"))) {
            s_record_start=now_us();
            memset(&s_record_last,0xff,sizeof(s_record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",s_sz_path);
        }
    }
    s_is_f9_held=f9;
    if (!s_p_record_file) return;
    PadSample s={(uint32_t)((now_us()-s_record_start)/1000),p_pad_data->buttons,
                 {p_pad_data->left_x,p_pad_data->left_y,p_pad_data->right_x,p_pad_data->right_y},p_pad_data->l2,p_pad_data->r2};
    if (s.buttons==s_record_last.buttons && !memcmp(s.axes,s_record_last.axes,4) &&
        s.l2==s_record_last.l2 && s.r2==s_record_last.r2) return;
    s_record_last=s;
    fprintf(s_p_record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(s_p_record_file);
}
static PadSample *s_p_replay; static size_t s_c_replays, s_replay_next;
void runtime_pad_replay_sample(PadData *p_pad_data) {
    if (!s_replay_armed) return;
    if (!s_replay_start) {
        static int s_is_loaded;
        if (!s_is_loaded) {
            s_is_loaded=1;
            const char *sz_path=runtime_env("PS_PAD_REPLAY",NULL);
            FILE *p_file=sz_path ? fopen(sz_path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (p_file && fscanf(p_file,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (s_c_replays==cap && !(s_p_replay=realloc(s_p_replay,(cap=cap ? cap*2 : 1024)*sizeof(*s_p_replay)))) break;
                s_p_replay[s_c_replays++]=s;
            }
            if (p_file) fclose(p_file);
            printf("Runtime: pad replay of %zu samples from %s\n",s_c_replays,sz_path ? sz_path : "(unset)");
        }
        s_replay_start=now_us();
        s_replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-s_replay_start)/1000);
    while (s_replay_next<s_c_replays && s_p_replay[s_replay_next].ms<=ms) ++s_replay_next;
    if (!s_replay_next) return;
    if (s_replay_next==s_c_replays && ms>s_p_replay[s_c_replays-1].ms+500) {
        if (s_replay_armed==1) { puts("Runtime: pad replay finished"); s_replay_armed=2; }
        return;
    }
    const PadSample *p_pad_sample=&s_p_replay[s_replay_next-1];
    p_pad_data->buttons=p_pad_sample->buttons;
    p_pad_data->left_x=p_pad_sample->axes[0]; p_pad_data->left_y=p_pad_sample->axes[1]; p_pad_data->right_x=p_pad_sample->axes[2]; p_pad_data->right_y=p_pad_sample->axes[3];
    p_pad_data->l2=p_pad_sample->l2; p_pad_data->r2=p_pad_sample->r2;
}
