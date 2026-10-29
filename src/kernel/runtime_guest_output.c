// SPDX-License-Identifier: GPL-2.0-or-later
/* The guest's console output (its writes to descriptors 1 and 2, sceKernelDebugOutText) as lines of
 * psport's log. The text is the game's: printed as data, never as a format, its control characters
 * escaped, each line cut to MAX_LINE bytes and at most LINES_PER_SECOND lines, the rest counted. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define MAX_LINE 512
#define LINES_PER_SECOND 200
#define MAX_DEBUG_TEXT 65536
static const char *const s_sz_channels[RUNTIME_GUEST_OUTPUT_CHANNELS]={"stdout","stderr","debug"};
typedef struct { unsigned char line[MAX_LINE]; size_t cb_line; uint64_t cb_cut; } Channel;
static Channel s_channels[RUNTIME_GUEST_OUTPUT_CHANNELS];
static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_window_ns, s_c_window_lines, s_c_window_dropped;
static uint64_t s_c_lines, s_c_dropped, s_c_cut;
static int s_has_summary;

/* Part of the call report: no stdio, no lock. */
static void summary(char *sz_out,size_t cb_out) {
    snprintf(sz_out,cb_out,"Runtime: summary guest output: %llu lines logged, %llu dropped (over %d per second), %llu cut at %d bytes\n",
             (unsigned long long)__atomic_load_n(&s_c_lines,__ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_dropped,__ATOMIC_RELAXED),LINES_PER_SECOND,
             (unsigned long long)__atomic_load_n(&s_c_cut,__ATOMIC_RELAXED),MAX_LINE);
}
/* Lock held. */
static void emit(int i_channel) {
    Channel *p_channel=&s_channels[i_channel];
    const uint64_t now_ns=runtime_wait_clock();
    if (now_ns-s_window_ns>=1000000000u) {
        if (s_c_window_dropped)
            printf("Runtime: %llu guest output lines dropped in the last second (over %d per second)\n",
                   (unsigned long long)s_c_window_dropped,LINES_PER_SECOND);
        s_window_ns=now_ns; s_c_window_lines=0; s_c_window_dropped=0;
    }
    if (s_c_window_lines>=LINES_PER_SECOND) {
        ++s_c_window_dropped; __atomic_add_fetch(&s_c_dropped,1,__ATOMIC_RELAXED);
        p_channel->cb_line=0; p_channel->cb_cut=0;
        return;
    }
    ++s_c_window_lines;
    char sz_text[MAX_LINE*4+1];
    size_t cb_text=0;
    for (size_t i=0;i<p_channel->cb_line;++i) {
        const unsigned char byte=p_channel->line[i];
        if (byte=='\\') { sz_text[cb_text++]='\\'; sz_text[cb_text++]='\\'; }
        else if ((byte<0x20 && byte!='\t') || byte==0x7f) cb_text+=(size_t)snprintf(sz_text+cb_text,5,"\\x%02x",byte);
        else sz_text[cb_text++]=(char)byte;
    }
    sz_text[cb_text]=0;
    __atomic_add_fetch(&s_c_lines,1,__ATOMIC_RELAXED);
    if (p_channel->cb_cut) {
        __atomic_add_fetch(&s_c_cut,1,__ATOMIC_RELAXED);
        printf("Guest %s: %s [%llu more bytes cut]\n",s_sz_channels[i_channel],sz_text,(unsigned long long)p_channel->cb_cut);
    } else printf("Guest %s: %s\n",s_sz_channels[i_channel],sz_text);
    p_channel->cb_line=0; p_channel->cb_cut=0;
}
void runtime_guest_output(int i_channel,const void *p_text,uint64_t cb_text) {
    if (i_channel<0 || i_channel>=RUNTIME_GUEST_OUTPUT_CHANNELS || !cb_text) return;
    if (!__atomic_exchange_n(&s_has_summary,1,__ATOMIC_ACQ_REL)) runtime_calls_add_summary(summary);
    Channel *p_channel=&s_channels[i_channel];
    pthread_mutex_lock(&s_lock);
    for (uint64_t i=0;i<cb_text;++i) {
        const unsigned char byte=((const unsigned char *)p_text)[i];
        if (byte=='\n') {
            if (p_channel->cb_line && !p_channel->cb_cut && p_channel->line[p_channel->cb_line-1]=='\r') --p_channel->cb_line;
            emit(i_channel);
        } else if (p_channel->cb_line<MAX_LINE) p_channel->line[p_channel->cb_line++]=byte;
        else ++p_channel->cb_cut;
    }
    pthread_mutex_unlock(&s_lock);
}
static ABI int32_t debug_out_text(int32_t channel,const char *sz_text) {
    (void)channel;
    if (sz_text) runtime_guest_output(RUNTIME_GUEST_OUTPUT_DEBUG,sz_text,strnlen(sz_text,MAX_DEBUG_TEXT));
    return 0;
}
static const RuntimeExport s_exports[]={
    {"sceKernelDebugOutText",debug_out_text},
};
uintptr_t runtime_guest_output_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
