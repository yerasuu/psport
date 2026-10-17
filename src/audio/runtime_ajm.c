/* libSceAjm: audio decoder batches. Batches use the SDK chunk format (the
 * builder functions are imports, but games may also build chunks inline), and
 * run synchronously at submission: decoding a batch takes well under a
 * millisecond, so waiting always finds it complete. ATRAC9 is decoded by
 * LibAtrac9 (MIT, third_party/LibAtrac9), MP3 and AAC by libSceAudiodec's FFmpeg decoders in the GPU
 * library (psport_gpu_audio_decoder_*). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"
#include "gpu/psport_gpu.h"

#define ERR_INVALID_CONTEXT ((int32_t)0x80930002)
#define ERR_INVALID_INSTANCE ((int32_t)0x80930003)
#define ERR_INVALID_BATCH ((int32_t)0x80930004)
#define ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80930007)
#define ERR_ALREADY_REGISTERED ((int32_t)0x80930009)
#define ERR_NOT_REGISTERED ((int32_t)0x8093000A)
#define ERR_WRONG_REVISION ((int32_t)0x8093000B)
#define ERR_MALFORMED_BATCH ((int32_t)0x80930011)
#define ERR_CANCELLED ((int32_t)0x80930017)
#define RESULT_NOT_INITIALIZED 0x1
#define RESULT_INVALID_PARAMETER 0x4
#define RESULT_PARTIAL_INPUT 0x8
#define RESULT_NOT_ENOUGH_ROOM 0x10
#define RESULT_CODEC_ERROR 0x40000000u
#define RESULT_FATAL 0x80000000u
#define STATISTICS_INSTANCE 0x80000u
#define MAX_INSTANCES 0x3fff
#define MAX_CONTEXTS 8
#define MAX_BATCHES 256
enum { IDENT_JOB=0, IDENT_INPUT_RUN=1, IDENT_INPUT_CONTROL=2, IDENT_CONTROL_FLAGS=3, IDENT_RUN_FLAGS=4,
       IDENT_RETURN_ADDRESS=6, IDENT_INLINE=7, IDENT_OUTPUT_RUN=17, IDENT_OUTPUT_CONTROL=18 };
enum { FORMAT_S16=0, FORMAT_S32=1, FORMAT_FLOAT=2 };
enum { CODEC_MP3=0, CODEC_AT9=1, CODEC_AAC=2 };
enum { AAC_ADTS=1, AAC_RAW=2 };
enum { MP3_IGNORE_OFL=1, AAC_NONDELAY_OUTPUT=2 };
/* An HE-AAC frame: 2048 samples, up to 8 channels. */
#define MPEG_PCM_SAMPLES (2048*8)
/* Job flags: version:3 codec:8 run:2 control:3 reserved:29 sideband:3 */
#define RUN_CODEC_INFO(f) (((f)>>11)&1)
#define RUN_MULTIPLE_FRAMES(f) (((f)>>12)&1)
#define CONTROL_RESET(f) (((f)>>13)&1)
#define CONTROL_INITIALIZE(f) (((f)>>14)&1)
#define CONTROL_RESAMPLE(f) (((f)>>15)&1)
#define SIDEBAND_GAPLESS(f) (((f)>>45)&1)
#define SIDEBAND_FORMAT(f) (((f)>>46)&1)
#define SIDEBAND_STREAM(f) (((f)>>47)&1)

typedef struct { uint32_t word; uint32_t size; } Chunk;          /* ident:6 payload:20 reserved:6 */
typedef struct { uint32_t word; uint32_t size; void *address; } ChunkBuffer;
typedef struct { int32_t result, internal_result; } SidebandResult;
typedef struct { int32_t input_consumed, output_written; uint64_t total_decoded_samples; } SidebandStream;
typedef struct { uint32_t channels, channel_mask, sample_rate, encoding, bitrate, reserved; } SidebandFormat;
typedef struct { uint32_t total_samples; uint16_t skip_samples, skipped_samples; } SidebandGapless;
typedef struct { uint32_t cb_super_frame, frames_in_super_frame, cb_next_frame, frame_samples; } At9Info;
typedef struct { uint32_t header; uint8_t has_crc, channel_mode, mode_extension, copyright, original, emphasis; uint16_t reserved[3]; } Mp3Info;
typedef struct { uint32_t heaac, reserved; } AacInfo;
typedef struct { int error_code; const void *job_address; uint32_t command_offset; const void *job_return_address; } BatchError;
_Static_assert(sizeof(ChunkBuffer)==16,"Ajm buffer chunk");
_Static_assert(sizeof(Mp3Info)==16 && sizeof(AacInfo)==8,"Ajm codec info layout");
_Static_assert(sizeof(SidebandFormat)==24 && sizeof(SidebandGapless)==8 && sizeof(SidebandStream)==16,"Ajm sideband layout");

typedef struct {
    int used, codec, format, channels_hint, gapless_loop;
    uint32_t codec_flags;
    void *p_handle;
    unsigned char config[4];
    int is_initialized;
    Atrac9CodecInfo info;
    uint32_t superframe_remain, frames;
    SidebandGapless gapless_init, gapless;
    uint64_t total_samples;
    void *p_decoder;
    uint32_t aac_header_type, skip_frames;
    float *p_pcm;
    PsAudioFrame frame;
} Instance;
typedef struct { int used, registered[24]; Instance instances[MAX_INSTANCES+1]; } Context;
typedef struct { int used, context, is_canceled; } Batch;

static pthread_mutex_t s_lock=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static Context *s_contexts[MAX_CONTEXTS+1];
static Batch s_batches[MAX_BATCHES];
static size_t s_c_jobs_run, s_c_frames_decoded, s_c_mp3_frames, s_c_aac_frames, s_c_batches_run, s_c_codec_errors;

/* PS_AUDIO_TRACE=1: instance configurations and jobs that return a non-zero result. */
static int s_c_trace_left=-1;
static int ajm_trace(void) {
    if (s_c_trace_left<0) { const char *sz_value=runtime_env("PS_AUDIO_TRACE",NULL); s_c_trace_left=sz_value && sz_value[0]=='1' ? 400 : 0; }
    return s_c_trace_left>0 && s_c_trace_left--;
}
static uint32_t ident(uint32_t word) { return word & 0x3f; }
static uint32_t payload(uint32_t word) { return (word>>6) & 0xfffff; }
static uint32_t header(uint32_t id, uint32_t value) { return (id & 0x3f) | ((value & 0xfffff)<<6); }
static uint32_t channel_mask(int channels) {
    switch (channels) { case 1: return 4; case 2: return 3; case 4: return 0x33; case 6: return 0x60f; case 8: return 0x63f; default: return 0; }
}
static int pcm_size(int format) { return format==FORMAT_S16 ? 2 : 4; }

/* ---- batch builders ---- */
static unsigned char *put_buffer(unsigned char *p_cursor, uint32_t id, const void *p_address, uint64_t cb_buffer) {
    ChunkBuffer c={header(id,0),(uint32_t)cb_buffer,(void *)p_address}; memcpy(p_cursor,&c,sizeof(c)); return p_cursor+sizeof(c);
}
static unsigned char *put_flags(unsigned char *p_cursor, uint32_t id, uint64_t flags) {
    Chunk c={header(id,(uint32_t)(flags>>32)),(uint32_t)flags}; memcpy(p_cursor,&c,sizeof(c)); return p_cursor+sizeof(c);
}
static void *finish_job(unsigned char *p_start, unsigned char *p_end, uint32_t instance) {
    Chunk job={header(IDENT_JOB,instance),(uint32_t)(p_end-p_start-sizeof(Chunk))};
    memcpy(p_start,&job,sizeof(job));
    return p_end;
}
static ABI void *job_control(void *p_buffer, uint32_t instance, uint64_t flags, void *p_in, uint64_t cb_in,
                             void *p_out, uint64_t cb_out, void *p_return_address) {
    unsigned char *p_cursor=(unsigned char *)p_buffer+sizeof(Chunk);
    if (p_return_address) p_cursor=put_buffer(p_cursor,IDENT_RETURN_ADDRESS,p_return_address,0);
    p_cursor=put_buffer(p_cursor,IDENT_INPUT_CONTROL,p_in,cb_in);
    flags &= instance==STATISTICS_INSTANCE ? UINT64_C(0xC0018007) : UINT64_C(0x60000000E7FF);
    p_cursor=put_flags(p_cursor,IDENT_CONTROL_FLAGS,flags);
    p_cursor=put_buffer(p_cursor,IDENT_OUTPUT_CONTROL,p_out,cb_out);
    return finish_job(p_buffer,p_cursor,instance);
}
static ABI void *job_run(void *p_buffer, uint32_t instance, uint64_t flags, void *p_in, uint64_t cb_in,
                         void *p_out, uint64_t cb_out, void *p_sideband, uint64_t cb_sideband, void *p_return_address) {
    unsigned char *p_cursor=(unsigned char *)p_buffer+sizeof(Chunk);
    if (p_return_address) p_cursor=put_buffer(p_cursor,IDENT_RETURN_ADDRESS,p_return_address,0);
    p_cursor=put_buffer(p_cursor,IDENT_INPUT_RUN,p_in,cb_in);
    p_cursor=put_flags(p_cursor,IDENT_RUN_FLAGS,flags & UINT64_C(0xE00000001FFF));
    p_cursor=put_buffer(p_cursor,IDENT_OUTPUT_RUN,p_out,cb_out);
    p_cursor=put_buffer(p_cursor,IDENT_OUTPUT_CONTROL,p_sideband,cb_sideband);
    return finish_job(p_buffer,p_cursor,instance);
}
typedef struct { void *address; uint64_t size; } AjmBuffer;
static ABI void *job_run_split(void *p_buffer, uint32_t instance, uint64_t flags, const AjmBuffer *p_ajm_buffer, uint64_t c_ins,
                               const AjmBuffer *p_out, uint64_t c_outs, void *p_sideband, uint64_t cb_sideband,
                               void *p_return_address) {
    unsigned char *p_cursor=(unsigned char *)p_buffer+sizeof(Chunk);
    if (p_return_address) p_cursor=put_buffer(p_cursor,IDENT_RETURN_ADDRESS,p_return_address,0);
    for (uint64_t i=0;i<c_ins;++i) p_cursor=put_buffer(p_cursor,IDENT_INPUT_RUN,p_ajm_buffer[i].address,p_ajm_buffer[i].size);
    p_cursor=put_flags(p_cursor,IDENT_RUN_FLAGS,flags & UINT64_C(0xE00000001FFF));
    for (uint64_t i=0;i<c_outs;++i) p_cursor=put_buffer(p_cursor,IDENT_OUTPUT_RUN,p_out[i].address,p_out[i].size);
    p_cursor=put_buffer(p_cursor,IDENT_OUTPUT_CONTROL,p_sideband,cb_sideband);
    return finish_job(p_buffer,p_cursor,instance);
}
static ABI void *job_inline(void *p_buffer, const void *p_data, uint64_t cb_data, const void **pp_address) {
    Chunk c={header(IDENT_INLINE,0),(uint32_t)((cb_data+7)&~UINT64_C(7))};
    memcpy(p_buffer,&c,sizeof(c));
    unsigned char *p_cursor=(unsigned char *)p_buffer+sizeof(c);
    memcpy(p_cursor,p_data,cb_data);
    if (pp_address) *pp_address=p_cursor;
    return p_cursor+c.size;
}

/* ---- ATRAC9 decoding ---- */
static void at9_reset(Instance *p_instance) {
    if (p_instance->p_handle) Atrac9ReleaseHandle(p_instance->p_handle);
    p_instance->p_handle=Atrac9GetHandle();
    p_instance->is_initialized = Atrac9InitDecoder(p_instance->p_handle,p_instance->config)==0;
    if (p_instance->is_initialized) Atrac9GetCodecInfo(p_instance->p_handle,&p_instance->info);
    p_instance->frames=0; p_instance->superframe_remain=(uint32_t)p_instance->info.superframeSize;
    if (ajm_trace())
        printf("Audio trace: ATRAC9 init %s: %d ch, %d Hz, frame %d samples, %d frames/superframe, superframe %d bytes, format %d, flags %#x\n",
               p_instance->is_initialized ? "ok" : "FAILED",p_instance->info.channels,p_instance->info.samplingRate,p_instance->info.frameSamples,
               p_instance->info.framesInSuperframe,p_instance->info.superframeSize,p_instance->format,p_instance->codec_flags);
}
static void gapless_reset(Instance *p_instance) {
    p_instance->gapless.total_samples=p_instance->gapless_init.total_samples;
    p_instance->gapless.skip_samples=p_instance->gapless_init.skip_samples;
    p_instance->gapless.skipped_samples=0;
}
static int gapless_end(const Instance *p_instance) { return p_instance->gapless_init.total_samples && !p_instance->gapless.total_samples; }
static uint32_t frame_channels(const Instance *p_instance) {
    return p_instance->codec==CODEC_AT9 ? (uint32_t)p_instance->info.channels : p_instance->frame.channels;
}
/* 0 for MP3 and AAC before their first frame: its size is only known once decoded. */
static uint32_t next_frame_bytes(const Instance *p_instance) {
    uint32_t frame=p_instance->codec==CODEC_AT9 ? (uint32_t)p_instance->info.frameSamples : p_instance->frame.samples;
    uint32_t skip=p_instance->gapless.skip_samples<frame ? p_instance->gapless.skip_samples : frame;
    uint32_t samples=frame-skip;
    if (p_instance->gapless_init.total_samples && p_instance->gapless.total_samples<samples) samples=p_instance->gapless.total_samples;
    return samples*frame_channels(p_instance)*(uint32_t)pcm_size(p_instance->format);
}
typedef struct { ChunkBuffer *p_chunks; int c_chunks, current; uint32_t off_chunk; } Output;
static uint64_t output_room(const Output *p_output) {
    uint64_t room=0;
    for (int i=p_output->current;i<p_output->c_chunks;++i) room+=p_output->p_chunks[i].size-(i==p_output->current ? p_output->off_chunk : 0);
    return room;
}
/* Samples of a decoded frame that reach the output; *skip are dropped first for gapless decoding. */
static uint32_t frame_window(Instance *p_instance, const Output *p_out, uint32_t frame, uint32_t channels, uint32_t *p_skip) {
    *p_skip=0;
    if (p_instance->gapless.skip_samples) {
        *p_skip=p_instance->gapless.skip_samples<frame ? p_instance->gapless.skip_samples : frame;
        p_instance->gapless.skip_samples-=(uint16_t)*p_skip;
    }
    uint32_t samples=frame-*p_skip;
    if (p_instance->gapless_init.total_samples && samples>p_instance->gapless.total_samples) samples=p_instance->gapless.total_samples;
    uint64_t room=output_room(p_out)/((uint64_t)channels*(uint64_t)pcm_size(p_instance->format));
    return samples>room ? (uint32_t)room : samples;
}
static void frame_account(Instance *p_instance, uint32_t frame, uint32_t samples) {
    p_instance->gapless.skipped_samples+=(uint16_t)(frame-samples);
    if (p_instance->gapless_init.total_samples) p_instance->gapless.total_samples-=samples;
}
static void output_write(Output *p_output, const unsigned char *p_data, uint64_t cb_data) {
    while (cb_data && p_output->current<p_output->c_chunks) {
        ChunkBuffer *p_chunk_buffer=&p_output->p_chunks[p_output->current];
        uint64_t cb_chunk=p_chunk_buffer->size-p_output->off_chunk; if (cb_chunk>cb_data) cb_chunk=cb_data;
        memcpy((unsigned char *)p_chunk_buffer->address+p_output->off_chunk,p_data,cb_chunk);
        p_data+=cb_chunk; cb_data-=cb_chunk; p_output->off_chunk+=(uint32_t)cb_chunk;
        if (p_output->off_chunk==p_chunk_buffer->size) { ++p_output->current; p_output->off_chunk=0; }
    }
}
/* One frame; returns result flags. */
static uint32_t at9_frame(Instance *p_instance, const unsigned char **pp_input, uint64_t *p_input_size, Output *p_out,
                          uint32_t *p_samples_written, int32_t *p_internal) {
    int channels=p_instance->info.channels, frame=p_instance->info.frameSamples, used=0;
    int16_t pcm[256*8];
    if (frame*channels>(int)(sizeof(pcm)/sizeof(*pcm))) return RESULT_CODEC_ERROR|RESULT_FATAL;
    uint64_t available=p_instance->superframe_remain<*p_input_size ? p_instance->superframe_remain : *p_input_size;
    int e=Atrac9Decode(p_instance->p_handle,*pp_input,(int)available,pcm,&used);
    if (e) { *p_internal=e; return RESULT_CODEC_ERROR|RESULT_FATAL; }
    *pp_input+=used; *p_input_size-=(uint64_t)used; p_instance->superframe_remain-=(uint32_t)used;
    ++s_c_frames_decoded;
    uint32_t skip, samples=frame_window(p_instance,p_out,(uint32_t)frame,(uint32_t)channels,&skip);
    /* Convert (and optionally de-interleave) into the requested encoding. */
    unsigned char converted[256*8*4];
    int non_interleaved=(p_instance->codec_flags>>8)&1;
    for (uint32_t s=0;s<samples;++s) for (int c=0;c<channels;++c) {
        int16_t v=pcm[(skip+s)*(uint32_t)channels+(uint32_t)c];
        size_t i_sample=non_interleaved ? (size_t)c*samples+s : (size_t)s*(size_t)channels+(size_t)c;
        if (p_instance->format==FORMAT_S16) memcpy(converted+i_sample*2,&v,2);
        else if (p_instance->format==FORMAT_S32) { int32_t w=(int32_t)v<<16; memcpy(converted+i_sample*4,&w,4); }
        else { float f=(float)v/32768.0f; memcpy(converted+i_sample*4,&f,4); }
    }
    output_write(p_out,converted,(uint64_t)samples*(uint64_t)channels*(uint64_t)pcm_size(p_instance->format));
    *p_samples_written=samples;
    frame_account(p_instance,(uint32_t)frame,samples);
    if (++p_instance->frames%(uint32_t)p_instance->info.framesInSuperframe==0) {
        uint64_t cb_skip=p_instance->superframe_remain<*p_input_size ? p_instance->superframe_remain : *p_input_size;
        *pp_input+=cb_skip; *p_input_size-=cb_skip;
        p_instance->superframe_remain=(uint32_t)p_instance->info.superframeSize; p_instance->frames=0;
    } else if (gapless_end(p_instance)) {
        /* Drain the rest of the superframe so the next stream starts aligned. */
        while (p_instance->frames%(uint32_t)p_instance->info.framesInSuperframe && *p_input_size) {
            uint64_t available=p_instance->superframe_remain<*p_input_size ? p_instance->superframe_remain : *p_input_size;
            if (Atrac9Decode(p_instance->p_handle,*pp_input,(int)available,pcm,&used)) break;
            *pp_input+=used; *p_input_size-=(uint64_t)used; p_instance->superframe_remain-=(uint32_t)used; ++p_instance->frames;
        }
        uint64_t cb_skip=p_instance->superframe_remain<*p_input_size ? p_instance->superframe_remain : *p_input_size;
        *pp_input+=cb_skip; *p_input_size-=cb_skip;
        p_instance->superframe_remain=(uint32_t)p_instance->info.superframeSize; p_instance->frames=0;
    }
    return 0;
}
static void parse_riff(Instance *p_instance, const unsigned char **pp_input, uint64_t *p_size) {
    const unsigned char *p_cursor=*pp_input, *p_end=p_cursor+*p_size;
    p_cursor+=12;
    while (p_cursor+8<=p_end) {
        uint32_t tag, length; memcpy(&tag,p_cursor,4); memcpy(&length,p_cursor+4,4); p_cursor+=8;
        if (tag==0x61746164) break;                                   /* "data" */
        if (tag==0x20746d66 && length>=52 && p_cursor+52<=p_end) {             /* "fmt " */
            memcpy(p_instance->config,p_cursor+44,4); at9_reset(p_instance);
        } else if (tag==0x74636166 && length>=12 && p_cursor+12<=p_end) {      /* "fact" */
            uint32_t c_samples, delay; memcpy(&c_samples,p_cursor,4); memcpy(&delay,p_cursor+4,4);
            p_instance->gapless_init.total_samples=c_samples; p_instance->gapless_init.skip_samples=(uint16_t)delay;
            gapless_reset(p_instance);
        }
        p_cursor+=length;
    }
    *p_size-=(uint64_t)(p_cursor-*pp_input); *pp_input=p_cursor;
}

/* ---- MP3 and AAC decoding ---- */
/* AAC holds back its first two frames unless the instance asks for non-delayed output. */
static void mpeg_reset(Instance *p_instance) {
    if (p_instance->p_decoder) psport_gpu_audio_decoder_reset(p_instance->p_decoder);
    memset(&p_instance->frame,0,sizeof(p_instance->frame));
    p_instance->skip_frames=p_instance->codec==CODEC_AAC && !(p_instance->codec_flags & AAC_NONDELAY_OUTPUT) ? 2 : 0;
}
/* AAC_ADTS or AAC_RAW with its sampling frequency index; ADTS until a control job initializes the instance. */
static void aac_configure(Instance *p_instance, uint32_t header_type, uint32_t i_frequency) {
    if (p_instance->p_decoder) psport_gpu_audio_decoder_destroy(p_instance->p_decoder);
    uint32_t channels=p_instance->channels_hint ? (uint32_t)p_instance->channels_hint : 2;
    p_instance->aac_header_type=header_type;
    p_instance->p_decoder=psport_gpu_audio_decoder_create(1,header_type!=AAC_RAW,i_frequency,channels);
    p_instance->is_initialized=p_instance->p_decoder!=NULL;
    mpeg_reset(p_instance);
    if (ajm_trace())
        printf("Audio trace: AAC init %s: %s, frequency index %u, %u channels, format %d, flags %#x\n",p_instance->is_initialized ? "ok" : "FAILED",
               header_type==AAC_RAW ? "raw" : "ADTS",i_frequency,channels,p_instance->format,p_instance->codec_flags);
}
static void put_sample(unsigned char *p_out, int format, float value) {
    if (format==FORMAT_FLOAT) { memcpy(p_out,&value,4); return; }
    double max=format==FORMAT_S32 ? 2147483647.0 : 32767.0, scaled=(double)value*(max+1.0);
    scaled=scaled>max ? max : scaled<-max-1.0 ? -max-1.0 : scaled;
    if (format==FORMAT_S32) { int32_t w=(int32_t)lrint(scaled); memcpy(p_out,&w,4); }
    else { int16_t v=(int16_t)lrint(scaled); memcpy(p_out,&v,2); }
}
/* One frame; returns result flags, RESULT_PARTIAL_INPUT alone when no whole frame is left. */
static uint32_t mpeg_frame(Instance *p_instance, const unsigned char **pp_input, uint64_t *p_input_size, Output *p_out,
                           uint32_t *p_samples_written, int32_t *p_internal) {
    uint64_t header=p_instance->codec==CODEC_MP3 ? 4 : p_instance->aac_header_type==AAC_RAW ? 1 : 7;
    if (*p_input_size<header) return RESULT_PARTIAL_INPUT;
    PsAudioFrame frame;
    int e=psport_gpu_audio_decoder_decode(p_instance->p_decoder,*pp_input,*p_input_size,p_instance->p_pcm,MPEG_PCM_SAMPLES*sizeof(float),&frame);
    if (e==1) return RESULT_PARTIAL_INPUT;
    if (e) { *p_internal=e; return RESULT_CODEC_ERROR|RESULT_FATAL; }
    *pp_input+=frame.consumed; *p_input_size-=frame.consumed;
    ++*(p_instance->codec==CODEC_MP3 ? &s_c_mp3_frames : &s_c_aac_frames);
    if (p_instance->codec==CODEC_MP3 && !(p_instance->codec_flags & MP3_IGNORE_OFL) && (frame.encoder_delay || frame.total_samples)) {
        p_instance->gapless_init=(SidebandGapless){frame.total_samples,(uint16_t)frame.encoder_delay,0};
        p_instance->gapless=p_instance->gapless_init;
    }
    if (!frame.samples) return 0;
    if (!p_instance->frame.samples && ajm_trace())
        printf("Audio trace: %s first frame: %u ch, %u Hz, %u samples, %u bytes, %u bit/s, format %d, flags %#x, gapless %u/%u\n",
               p_instance->codec==CODEC_MP3 ? "MP3" : "AAC",frame.channels,frame.sample_rate,frame.samples,(unsigned)frame.consumed,
               frame.bitrate,p_instance->format,p_instance->codec_flags,p_instance->gapless_init.skip_samples,p_instance->gapless_init.total_samples);
    p_instance->frame=frame;
    if (p_instance->skip_frames) { --p_instance->skip_frames; return 0; }
    uint32_t skip, samples=frame_window(p_instance,p_out,frame.samples,frame.channels,&skip);
    /* In place: an output sample is never wider than the float it comes from. */
    unsigned char *p_pcm=(unsigned char *)p_instance->p_pcm;
    const float *p_source=p_instance->p_pcm+(size_t)skip*frame.channels;
    size_t width=(size_t)pcm_size(p_instance->format);
    for (size_t i=0, c_samples=(size_t)samples*frame.channels;i<c_samples;++i) put_sample(p_pcm+i*width,p_instance->format,p_source[i]);
    output_write(p_out,p_pcm,(uint64_t)samples*frame.channels*width);
    *p_samples_written=samples;
    frame_account(p_instance,frame.samples,samples);
    return 0;
}

/* ---- job execution ---- */
typedef struct {
    uint64_t flags; int have_flags;
    ChunkBuffer inputs[16]; int c_inputs;
    ChunkBuffer outputs[16]; int c_outputs;
    ChunkBuffer input_control, output_control;
} Job;
static int parse_job(const unsigned char *p_cursor, uint32_t cb_job, Job *p_job) {
    memset(p_job,0,sizeof(*p_job));
    for (const unsigned char *p_end=p_cursor+cb_job; p_cursor<p_end;) {
        uint32_t word; memcpy(&word,p_cursor,4);
        switch (ident(word)) {
        case IDENT_CONTROL_FLAGS: case IDENT_RUN_FLAGS: {
            Chunk c; memcpy(&c,p_cursor,8);
            p_job->flags=((uint64_t)payload(c.word)<<32)|c.size; p_job->have_flags=1; p_cursor+=8; break;
        }
        case IDENT_INPUT_RUN: if (p_job->c_inputs==16) return -1; memcpy(&p_job->inputs[p_job->c_inputs++],p_cursor,16); p_cursor+=16; break;
        case IDENT_OUTPUT_RUN: if (p_job->c_outputs==16) return -1; memcpy(&p_job->outputs[p_job->c_outputs++],p_cursor,16); p_cursor+=16; break;
        case IDENT_INPUT_CONTROL: memcpy(&p_job->input_control,p_cursor,16); p_cursor+=16; break;
        case IDENT_OUTPUT_CONTROL: memcpy(&p_job->output_control,p_cursor,16); p_cursor+=16; break;
        case IDENT_RETURN_ADDRESS: p_cursor+=16; break;
        default: return -1;
        }
    }
    return p_job->have_flags ? 0 : -1;
}
static void reset_instance(Instance *p_instance) {
    p_instance->total_samples=0; gapless_reset(p_instance);
    if (p_instance->codec!=CODEC_AT9) mpeg_reset(p_instance);
    else if (p_instance->is_initialized) at9_reset(p_instance);
}
static void initialize_instance(Instance *p_instance, const unsigned char *p_params) {
    if (p_instance->codec==CODEC_AT9) { memcpy(p_instance->config,p_params,4); at9_reset(p_instance); }
    else if (p_instance->codec==CODEC_AAC) { uint32_t p[2]; memcpy(p,p_params,8); aac_configure(p_instance,p[0],p[1]); }
}
/* Input control sideband order: format, gapless, resample, initialize. */
static uint32_t apply_control(Instance *p_instance, const Job *p_job) {
    uint64_t f=p_job->flags;
    uint32_t flags_result=0;
    const unsigned char *p_control=p_job->input_control.size ? p_job->input_control.address : NULL;
    const unsigned char *p_control_end=p_control ? p_control+p_job->input_control.size : NULL;
    if (CONTROL_RESET(f)) reset_instance(p_instance);
    if (p_control && SIDEBAND_FORMAT(f) && p_control+24<=p_control_end) p_control+=24; /* output format fixed per instance */
    if (p_control && SIDEBAND_GAPLESS(f) && p_control+8<=p_control_end) {
        SidebandGapless g; memcpy(&g,p_control,8); p_control+=8;
        uint32_t processed=p_instance->gapless_init.total_samples-p_instance->gapless.total_samples;
        if (g.total_samples || !g.skip_samples) {
            if (g.total_samples>=processed) {
                int64_t diff=(int64_t)p_instance->gapless_init.total_samples-g.total_samples;
                p_instance->gapless_init.total_samples=g.total_samples; p_instance->gapless.total_samples=(uint32_t)(p_instance->gapless.total_samples-diff);
            } else flags_result|=RESULT_INVALID_PARAMETER;
        }
        uint32_t skipped=(uint32_t)p_instance->gapless_init.skip_samples-p_instance->gapless.skip_samples;
        if (g.skip_samples || !g.total_samples) {
            if (g.skip_samples>=skipped) {
                int32_t diff=(int32_t)p_instance->gapless_init.skip_samples-g.skip_samples;
                p_instance->gapless_init.skip_samples=g.skip_samples; p_instance->gapless.skip_samples=(uint16_t)(p_instance->gapless.skip_samples-diff);
            } else flags_result|=RESULT_INVALID_PARAMETER;
        }
    }
    if (p_control && CONTROL_RESAMPLE(f) && p_control+8<=p_control_end) p_control+=8;
    if (p_control && CONTROL_INITIALIZE(f) && p_control+8<=p_control_end) initialize_instance(p_instance,p_control);
    return flags_result;
}
typedef struct { uint64_t in_start, in_left, out_start; uint32_t frames; int32_t internal; } Progress;
static uint32_t decode_input(Instance *p_instance, const Job *p_job, Output *p_out, Progress *p_progress) {
    uint64_t cb_in=0;
    for (int i=0;i<p_job->c_inputs;++i) cb_in+=p_job->inputs[i].size;
    unsigned char *p_joined=NULL;
    const unsigned char *p_input=NULL;
    if (p_job->c_inputs==1) p_input=p_job->inputs[0].address;
    else if (p_job->c_inputs>1) {
        p_joined=malloc(cb_in ? cb_in : 1);
        uint64_t at=0;
        for (int i=0;i<p_job->c_inputs;++i) { memcpy(p_joined+at,p_job->inputs[i].address,p_job->inputs[i].size); at+=p_job->inputs[i].size; }
        p_input=p_joined;
    }
    p_progress->in_start=cb_in; p_progress->out_start=output_room(p_out);
    uint32_t flags_result=0;
    if (cb_in) for (;;) {
        if (p_instance->gapless_loop && gapless_end(p_instance)) { gapless_reset(p_instance); p_instance->total_samples=0; }
        if (p_instance->codec==CODEC_AT9 && (p_instance->codec_flags & 1) && cb_in>=4 && !memcmp(p_input,"RIFF",4)) {
            parse_riff(p_instance,&p_input,&cb_in); p_instance->total_samples=0;
        }
        if (!p_instance->is_initialized) { flags_result|=RESULT_NOT_INITIALIZED; break; }
        if (!gapless_end(p_instance) && output_room(p_out)<next_frame_bytes(p_instance)) flags_result|=RESULT_NOT_ENOUGH_ROOM;
        if (p_instance->codec==CODEC_AT9 && cb_in<p_instance->superframe_remain) flags_result|=RESULT_PARTIAL_INPUT;
        if (flags_result) break;
        uint32_t written=0;
        uint32_t r=p_instance->codec==CODEC_AT9 ? at9_frame(p_instance,&p_input,&cb_in,p_out,&written,&p_progress->internal)
                                        : mpeg_frame(p_instance,&p_input,&cb_in,p_out,&written,&p_progress->internal);
        if (r==RESULT_PARTIAL_INPUT) { flags_result|=r; break; }
        p_instance->total_samples+=written; ++p_progress->frames;
        if (r) { flags_result|=r; break; }
        if (!RUN_MULTIPLE_FRAMES(p_job->flags)) break;
    }
    free(p_joined);
    p_progress->in_left=cb_in;
    return flags_result;
}
static SidebandFormat format_of(const Instance *p_instance) {
    if (p_instance->codec!=CODEC_AT9)
        return (SidebandFormat){p_instance->frame.channels,channel_mask((int)p_instance->frame.channels),p_instance->frame.sample_rate,(uint32_t)p_instance->format,
                                p_instance->frame.bitrate,0};
    SidebandFormat s={(uint32_t)p_instance->info.channels,channel_mask(p_instance->info.channels),(uint32_t)p_instance->info.samplingRate,(uint32_t)p_instance->format,0,0};
    if (p_instance->info.framesInSuperframe && p_instance->info.frameSamples)
        s.bitrate=(uint32_t)((uint64_t)p_instance->info.samplingRate*(uint64_t)p_instance->info.superframeSize*8/
                             ((uint64_t)p_instance->info.framesInSuperframe*(uint64_t)p_instance->info.frameSamples));
    return s;
}
static void write_codec_info(const Instance *p_instance, unsigned char *p_side, const unsigned char *p_side_end) {
    if (p_instance->codec==CODEC_MP3 && p_side+sizeof(Mp3Info)<=p_side_end) {
        uint32_t h=p_instance->frame.mp3_header;
        Mp3Info info={h,!((h>>16)&1),(uint8_t)((h>>6)&3),(uint8_t)((h>>4)&3),(uint8_t)((h>>3)&1),(uint8_t)((h>>2)&1),(uint8_t)(h&3),{0}};
        memcpy(p_side,&info,sizeof(info));
    } else if (p_instance->codec==CODEC_AAC && p_side+sizeof(AacInfo)<=p_side_end) {
        AacInfo info={p_instance->frame.high_efficiency,0};
        memcpy(p_side,&info,sizeof(info));
    } else if (p_instance->codec==CODEC_AT9 && p_side+sizeof(At9Info)<=p_side_end) {
        At9Info info={(uint32_t)p_instance->info.superframeSize,(uint32_t)p_instance->info.framesInSuperframe,p_instance->superframe_remain,(uint32_t)p_instance->info.frameSamples};
        memcpy(p_side,&info,sizeof(info));
    }
}
static void run_job(Context *p_ctx, uint32_t id, Job *p_job) {
    ++s_c_jobs_run;
    unsigned char *p_side=p_job->output_control.size ? p_job->output_control.address : NULL;
    unsigned char *p_side_end=p_side ? p_side+p_job->output_control.size : NULL;
    SidebandResult *p_result=NULL;
    if (p_side && p_side+sizeof(SidebandResult)<=p_side_end) { p_result=(void *)p_side; p_side+=sizeof(*p_result); memset(p_result,0,sizeof(*p_result)); }
    if (id==STATISTICS_INSTANCE) { if (p_side) memset(p_side,0,(size_t)(p_side_end-p_side)); return; }
    Instance *p_instance=&p_ctx->instances[id & MAX_INSTANCES];
    if ((id & MAX_INSTANCES)==0 || !p_instance->used) { if (p_result) p_result->result=(int32_t)(RESULT_FATAL|RESULT_INVALID_PARAMETER); return; }
    uint64_t f=p_job->flags;
    uint32_t flags_result=apply_control(p_instance,p_job);
    Output out={p_job->outputs,p_job->c_outputs,0,0};
    Progress p={0};
    flags_result|=decode_input(p_instance,p_job,&out,&p);
    uint64_t written=p.out_start-output_room(&out);
    if (flags_result & RESULT_CODEC_ERROR) ++s_c_codec_errors;
    if (flags_result && ajm_trace())
        printf("Audio trace: Ajm job instance %u flags %#llx -> result %#x internal %d; input %llu/%llu used, output %llu/%llu written, frames %u\n",
               id,(unsigned long long)f,flags_result,p.internal,(unsigned long long)(p.in_start-p.in_left),(unsigned long long)p.in_start,
               (unsigned long long)written,(unsigned long long)p.out_start,p.frames);
    if (p_result) { p_result->result=(int32_t)flags_result; p_result->internal_result=p.internal; }
    /* Output sideband order: stream, format, gapless, multi-frame, codec info. */
    if (p_side && SIDEBAND_STREAM(f) && p_side+16<=p_side_end) {
        SidebandStream s={(int32_t)(p.in_start-p.in_left),(int32_t)written,p_instance->total_samples};
        memcpy(p_side,&s,16); p_side+=16;
    }
    if (p_side && SIDEBAND_FORMAT(f) && p_side+24<=p_side_end) { SidebandFormat s=format_of(p_instance); memcpy(p_side,&s,24); p_side+=24; }
    if (p_side && SIDEBAND_GAPLESS(f) && p_side+8<=p_side_end) { memcpy(p_side,&p_instance->gapless,8); p_side+=8; }
    if (p_side && RUN_MULTIPLE_FRAMES(f) && p_side+8<=p_side_end) { uint32_t m[2]={p.frames,0}; memcpy(p_side,m,8); p_side+=8; }
    if (p_side && RUN_CODEC_INFO(f)) write_codec_info(p_instance,p_side,p_side_end);
}

/* ---- API ---- */
static void release_instance(Instance *p_instance) {
    if (p_instance->p_handle) Atrac9ReleaseHandle(p_instance->p_handle);
    if (p_instance->p_decoder) psport_gpu_audio_decoder_destroy(p_instance->p_decoder);
    free(p_instance->p_pcm);
}
/* MP3 needs no configuration; AAC starts as ADTS until a control job initializes it. */
static int32_t open_mpeg(Instance *p_instance) {
    p_instance->p_pcm=malloc(MPEG_PCM_SAMPLES*sizeof(float));
    if (!p_instance->p_pcm) return ERR_OUT_OF_RESOURCES;
    if (p_instance->codec==CODEC_AAC) aac_configure(p_instance,AAC_ADTS,0);
    else { p_instance->p_decoder=psport_gpu_audio_decoder_create(0,0,0,0); p_instance->is_initialized=p_instance->p_decoder!=NULL; }
    return p_instance->is_initialized ? 0 : ERR_OUT_OF_RESOURCES;
}
static Context *context(uint32_t id) { return id && id<=MAX_CONTEXTS ? s_contexts[id] : NULL; }
static ABI int32_t ajm_initialize(int64_t reserved, uint32_t *p_out) {
    if (!p_out || reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&s_lock);
    for (uint32_t i=1;i<=MAX_CONTEXTS;++i) if (!s_contexts[i]) {
        s_contexts[i]=calloc(1,sizeof(Context));
        pthread_mutex_unlock(&s_lock);
        if (!s_contexts[i]) return ERR_OUT_OF_RESOURCES;
        *p_out=i; puts("Runtime: Ajm context initialized (ATRAC9 via LibAtrac9, MP3 and AAC via FFmpeg)");
        return 0;
    }
    pthread_mutex_unlock(&s_lock);
    return ERR_OUT_OF_RESOURCES;
}
static ABI int32_t ajm_finalize(uint32_t id) {
    pthread_mutex_lock(&s_lock);
    Context *p_context=context(id);
    if (!p_context) { pthread_mutex_unlock(&s_lock); return ERR_INVALID_CONTEXT; }
    for (int i=0;i<=MAX_INSTANCES;++i) release_instance(&p_context->instances[i]);
    free(p_context); s_contexts[id]=NULL;
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t ajm_module_register(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec>=24) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&s_lock);
    Context *p_context=context(id);
    int32_t r=!p_context ? ERR_INVALID_CONTEXT : p_context->registered[codec] ? ERR_ALREADY_REGISTERED : 0;
    if (!r) p_context->registered[codec]=1;
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t ajm_module_unregister(uint32_t id, uint32_t codec) {
    pthread_mutex_lock(&s_lock);
    Context *p_context=context(id);
    int32_t r=!p_context ? ERR_INVALID_CONTEXT : codec>=24 ? ERR_INVALID_PARAMETER : !p_context->registered[codec] ? ERR_NOT_REGISTERED : 0;
    if (!r) p_context->registered[codec]=0;
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t ajm_instance_create(uint32_t id, uint32_t codec, uint64_t flags, uint32_t *p_out) {
    if (!p_out || codec>=24) return ERR_INVALID_PARAMETER;
    if (!(flags & 7)) return ERR_WRONG_REVISION;
    if (codec>CODEC_AAC) { fprintf(stderr,"STOP: Ajm codec %u is not implemented (0=MP3, 1=ATRAC9, 2=AAC are)\n",codec); runtime_stop(21); }
    if (ajm_trace()) printf("Audio trace: Ajm instance create codec %u flags %#llx\n",codec,(unsigned long long)flags);
    pthread_mutex_lock(&s_lock);
    Context *p_context=context(id);
    int32_t r=!p_context ? ERR_INVALID_CONTEXT : !p_context->registered[codec] ? ERR_NOT_REGISTERED : ERR_OUT_OF_RESOURCES;
    if (p_context && p_context->registered[codec]) for (uint32_t i=1;i<=MAX_INSTANCES;++i) if (!p_context->instances[i].used) {
        Instance *p_instance=&p_context->instances[i];
        memset(p_instance,0,sizeof(*p_instance));
        p_instance->used=1; p_instance->codec=(int)codec; p_instance->channels_hint=(int)((flags>>3)&15); p_instance->format=(int)((flags>>7)&7);
        p_instance->gapless_loop=(int)((flags>>10)&1); p_instance->codec_flags=(uint32_t)(flags>>32);
        if (p_instance->format>FORMAT_FLOAT) { p_instance->used=0; r=ERR_INVALID_PARAMETER; break; }
        if (codec!=CODEC_AT9 && (r=open_mpeg(p_instance))) { release_instance(p_instance); memset(p_instance,0,sizeof(*p_instance)); break; }
        *p_out=i|(codec<<14); r=0; break;
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t ajm_instance_destroy(uint32_t id, uint32_t instance) {
    pthread_mutex_lock(&s_lock);
    Context *p_context=context(id);
    Instance *p_instance=p_context ? &p_context->instances[instance & MAX_INSTANCES] : NULL;
    int32_t r=!p_context ? ERR_INVALID_CONTEXT : !(instance & MAX_INSTANCES) || !p_instance->used ? ERR_INVALID_INSTANCE : 0;
    if (!r) { release_instance(p_instance); memset(p_instance,0,sizeof(*p_instance)); }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI uint32_t ajm_instance_codec(uint32_t instance) { return (instance>>14)&0x1f; }
static ABI int32_t ajm_batch_start(uint32_t id, unsigned char *p_buffer, uint32_t cb_buffer, int priority,
                                   BatchError *p_error, uint32_t *p_out) {
    (void)priority;
    if (cb_buffer & 7) return ERR_MALFORMED_BATCH;
    pthread_mutex_lock(&s_lock);
    Context *p_context=context(id);
    if (!p_context) { pthread_mutex_unlock(&s_lock); return ERR_INVALID_CONTEXT; }
    int slot=-1;
    for (int i=0;i<MAX_BATCHES;++i) if (!s_batches[i].used) { slot=i; break; }
    if (slot<0) { pthread_mutex_unlock(&s_lock); return ERR_OUT_OF_RESOURCES; }
    for (unsigned char *p_cursor=p_buffer, *p_end=p_buffer+cb_buffer; p_cursor<p_end;) {
        Chunk chunk; memcpy(&chunk,p_cursor,8);
        unsigned char *p_body=p_cursor+8;
        p_cursor=p_body+chunk.size;
        if (ident(chunk.word)==IDENT_INLINE) continue;
        Job job;
        if (ident(chunk.word)!=IDENT_JOB || p_cursor>p_end || parse_job(p_body,chunk.size,&job)) {
            if (p_error) { p_error->error_code=ERR_MALFORMED_BATCH; p_error->job_address=p_body-8; p_error->command_offset=(uint32_t)(p_body-8-p_buffer); p_error->job_return_address=NULL; }
            pthread_mutex_unlock(&s_lock);
            return ERR_MALFORMED_BATCH;
        }
        run_job(p_context,payload(chunk.word),&job);
    }
    s_batches[slot]=(Batch){1,(int)id,0};
    ++s_c_batches_run;
    *p_out=(uint32_t)slot+1;
    pthread_mutex_unlock(&s_lock);
    return 0;
}
static ABI int32_t ajm_batch_wait(uint32_t id, uint32_t batch, uint32_t timeout, BatchError *p_error) {
    (void)timeout; (void)p_error;
    pthread_mutex_lock(&s_lock);
    int32_t r=!context(id) ? ERR_INVALID_CONTEXT : !batch || batch>MAX_BATCHES || !s_batches[batch-1].used ? ERR_INVALID_BATCH : 0;
    if (!r) { if (s_batches[batch-1].is_canceled) r=ERR_CANCELLED; s_batches[batch-1].used=0; }
    pthread_mutex_unlock(&s_lock);
    return r;
}
static ABI int32_t ajm_batch_cancel(uint32_t id, uint32_t batch) {
    pthread_mutex_lock(&s_lock);
    int32_t r=!context(id) ? ERR_INVALID_CONTEXT : !batch || batch>MAX_BATCHES || !s_batches[batch-1].used ? ERR_INVALID_BATCH : 0;
    pthread_mutex_unlock(&s_lock);
    return r; /* already processed at submission: nothing to cancel */
}
static ABI int32_t ajm_memory_register(uint32_t id, void *p_memory, uint64_t pages) { (void)p_memory; (void)pages; return context(id) ? 0 : ERR_INVALID_CONTEXT; }

static const RuntimeExport s_exports[]={
    {"sceAjmInitialize",ajm_initialize}, {"sceAjmFinalize",ajm_finalize},
    {"sceAjmModuleRegister",ajm_module_register}, {"sceAjmModuleUnregister",ajm_module_unregister},
    {"sceAjmInstanceCreate",ajm_instance_create}, {"sceAjmInstanceDestroy",ajm_instance_destroy},
    {"sceAjmInstanceCodecType",ajm_instance_codec},
    {"sceAjmBatchJobControlBufferRa",job_control}, {"sceAjmBatchJobRunBufferRa",job_run},
    {"sceAjmBatchJobRunSplitBufferRa",job_run_split}, {"sceAjmBatchJobInlineBuffer",job_inline},
    {"sceAjmBatchStartBuffer",ajm_batch_start}, {"sceAjmBatchWait",ajm_batch_wait},
    {"sceAjmBatchCancel",ajm_batch_cancel}, {"sceAjmBatchErrorDump",runtime_stub},
    {"sceAjmMemoryRegister",ajm_memory_register},
};
uintptr_t runtime_ajm_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
void runtime_ajm_report(void) {
    printf("Runtime: Ajm batches=%zu, jobs=%zu, frames decoded: ATRAC9=%zu, MP3=%zu, AAC=%zu, jobs with a codec error=%zu\n",
           s_c_batches_run,s_c_jobs_run,s_c_frames_decoded,s_c_mp3_frames,s_c_aac_frames,s_c_codec_errors);
}
