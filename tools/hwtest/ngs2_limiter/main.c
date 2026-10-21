/* Hardware test for libSceNgs2's mastering voice (kb #395): the LFE low-pass the mastering setup
 * enables and the limiter (unit 0x11, parameter 0x30000003). Each case renders known signals
 * through a sampler voice into a mastering voice and writes the rendered frames and the limiter
 * meters of each grain to /data/psport-hwtest/ngs2_limiter/, to compare with psport's output. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_DIR "/data/psport-hwtest/ngs2_limiter"
#define SYSMODULE_NGS2 0x000B
#define RACK_ID_SAMPLER 0x1000
#define RACK_ID_MASTERING 0x3000
#define WAVEFORM_TYPE_PCM_F32L 0x18
#define VOICE_PARAM_PATCH 0x0005
#define VOICE_PARAM_EVENT 0x0006
#define VOICE_EVENT_PLAY 0
#define SAMPLER_VOICE_PARAM_SETUP 0x10000000
#define SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS 0x10000001
#define MASTERING_VOICE_PARAM_SETUP 0x30000000
#define MASTERING_VOICE_PARAM_LFE 0x30000002
#define MASTERING_VOICE_PARAM_LIMITER 0x30000003
#define RATE_HZ 48000
#define C_GRAIN_FRAMES 256
#define C_GRAINS 40
#define C_FRAMES (C_GRAIN_FRAMES * C_GRAINS)
#define C_MAX_CHANNELS 6
#define I_STEP_FRAME 4800
#define PI 3.14159265358979f

typedef uintptr_t Ngs2Handle;

typedef struct {
    void *p_host_buffer;
    size_t cb_host_buffer;
    uintptr_t reserved[5];
    uintptr_t user_data;
} Ngs2ContextBufferInfo;

typedef struct {
    int32_t (*pfn_alloc)(Ngs2ContextBufferInfo *);
    int32_t (*pfn_free)(Ngs2ContextBufferInfo *);
    uintptr_t user_data;
} Ngs2BufferAllocator;

typedef struct {
    uint16_t cb_param;
    int16_t off_next;
    uint32_t id;
} Ngs2ParamHeader;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t waveform_type, c_channels, rate_hz, config_data, off_frame, frame_margin;
    uint32_t flags, reserved;
} Ngs2SamplerSetupParam;

typedef struct {
    uint32_t off_data, cb_data, c_repeats, c_skip_samples, c_samples, reserved;
    uintptr_t user_data;
} Ngs2WaveformBlock;

typedef struct {
    Ngs2ParamHeader header;
    const void *p_data;
    uint32_t flags, c_blocks;
    const Ngs2WaveformBlock *p_blocks;
} Ngs2WaveformBlocksParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t i_port, i_dest_input;
    Ngs2Handle h_dest;
} Ngs2PatchParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t event_id;
} Ngs2EventParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t c_io_channels, flags;
} Ngs2MasteringSetupParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t is_enabled, frequency_hz;
} Ngs2LfeParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t is_enabled;
    float threshold;
    uint8_t padding[8];
} Ngs2LimiterParam;

typedef struct {
    uint32_t state_flags;
    float limiter_peak_level;
    float limiter_press_level;
    float input_peak_height[8];
    float output_peak_height[8];
} Ngs2MasteringVoiceState;

typedef struct {
    void *p_buffer;
    size_t cb_buffer;
    uint32_t waveform_type, c_channels;
} Ngs2RenderBufferInfo;

int sceSysmoduleLoadModule(uint16_t id);
int sceKernelGetCompiledSdkVersion(int32_t *p_out_version);
int sceNgs2SystemCreateWithAllocator(const void *p_option, const Ngs2BufferAllocator *p_allocator, Ngs2Handle *p_out_handle);
int sceNgs2SystemDestroy(Ngs2Handle h_system, Ngs2ContextBufferInfo *p_out_buffer_info);
int sceNgs2RackCreateWithAllocator(Ngs2Handle h_system, uint32_t rack_id, const void *p_option, const Ngs2BufferAllocator *p_allocator,
                                   Ngs2Handle *p_out_handle);
int sceNgs2RackGetVoiceHandle(Ngs2Handle h_rack, uint32_t i_voice, Ngs2Handle *p_out_handle);
int sceNgs2VoiceControl(Ngs2Handle h_voice, const Ngs2ParamHeader *p_params);
int sceNgs2VoiceGetState(Ngs2Handle h_voice, void *p_out_state, size_t cb_state);
int sceNgs2SystemRender(Ngs2Handle h_system, const Ngs2RenderBufferInfo *p_buffers, uint32_t c_buffers);

typedef enum { SIGNAL_SILENCE, SIGNAL_SINE_LOUD, SIGNAL_STEP, SIGNAL_SINE_1KHZ, SIGNAL_SINE_50HZ } Signal;

typedef struct {
    const char *sz_name;
    uint32_t c_channels;
    Signal signals[C_MAX_CHANNELS];
    int has_limiter_param;
    Ngs2LimiterParam limiter;
    int has_lfe_param;
    Ngs2LfeParam lfe;
} Case;

static float s_waveform[C_FRAMES * C_MAX_CHANNELS];
static float s_rendered[C_FRAMES * C_MAX_CHANNELS];
static float s_grain[C_GRAIN_FRAMES * C_MAX_CHANNELS];

static int32_t allocate_buffer(Ngs2ContextBufferInfo *p_info) {
    p_info->p_host_buffer = calloc(1, p_info->cb_host_buffer);
    return p_info->p_host_buffer ? 0 : (int32_t)0x804A0002;
}

static int32_t free_buffer(Ngs2ContextBufferInfo *p_info) {
    free(p_info->p_host_buffer);
    p_info->p_host_buffer = NULL;
    return 0;
}

static float signal_sample(Signal signal, uint32_t i_frame) {
    const float t = (float)i_frame / RATE_HZ;
    switch (signal) {
        case SIGNAL_SINE_LOUD: return 2.0f * sinf(2.0f * PI * 1000.0f * t);
        case SIGNAL_STEP: return i_frame < I_STEP_FRAME ? 0.25f : 1.5f;
        case SIGNAL_SINE_1KHZ: return 0.5f * sinf(2.0f * PI * 1000.0f * t);
        case SIGNAL_SINE_50HZ: return 0.5f * sinf(2.0f * PI * 50.0f * t);
        case SIGNAL_SILENCE: return 0.0f;
    }
    return 0.0f;
}

static int control(Ngs2Handle h_voice, const void *p_param, uint16_t cb_param, uint32_t id) {
    Ngs2ParamHeader *p_header = (Ngs2ParamHeader *)p_param;
    p_header->cb_param = cb_param;
    p_header->off_next = 0;
    p_header->id = id;
    return sceNgs2VoiceControl(h_voice, p_header);
}

static int write_case(const Case *p_case, const Ngs2MasteringVoiceState *p_states, int rc_limiter) {
    char sz_path[256];
    snprintf(sz_path, sizeof(sz_path), "%s/%s.csv", SZ_OUT_DIR, p_case->sz_name);
    FILE *p_file = fopen(sz_path, "w");
    if (!p_file) {
        printf("ngs2_limiter: cannot write %s (errno %d)\n", sz_path, errno);
        return -1;
    }
    fprintf(p_file, "# limiter param rc %#x\nframe", (unsigned)rc_limiter);
    for (uint32_t c = 0; c < p_case->c_channels; c++) fprintf(p_file, ",ch%u", c);
    fprintf(p_file, ",limiter_peak_level,limiter_press_level\n");
    for (uint32_t i = 0; i < C_FRAMES; i++) {
        fprintf(p_file, "%u", i);
        for (uint32_t c = 0; c < p_case->c_channels; c++) fprintf(p_file, ",%.9g", s_rendered[i * p_case->c_channels + c]);
        if (i % C_GRAIN_FRAMES == C_GRAIN_FRAMES - 1) {
            const Ngs2MasteringVoiceState *p_state = &p_states[i / C_GRAIN_FRAMES];
            fprintf(p_file, ",%.9g,%.9g\n", p_state->limiter_peak_level, p_state->limiter_press_level);
        } else {
            fprintf(p_file, ",,\n");
        }
    }
    fclose(p_file);
    printf("ngs2_limiter: %s written\n", sz_path);
    return 0;
}

static int run_case(const Case *p_case) {
    static Ngs2MasteringVoiceState s_states[C_GRAINS];
    const Ngs2BufferAllocator allocator = {allocate_buffer, free_buffer, 0};
    Ngs2Handle h_system = 0, h_sampler_rack = 0, h_mastering_rack = 0, h_sampler = 0, h_mastering = 0;
    int rc = sceNgs2SystemCreateWithAllocator(NULL, &allocator, &h_system);
    if (rc == 0) rc = sceNgs2RackCreateWithAllocator(h_system, RACK_ID_SAMPLER, NULL, &allocator, &h_sampler_rack);
    if (rc == 0) rc = sceNgs2RackCreateWithAllocator(h_system, RACK_ID_MASTERING, NULL, &allocator, &h_mastering_rack);
    if (rc == 0) rc = sceNgs2RackGetVoiceHandle(h_sampler_rack, 0, &h_sampler);
    if (rc == 0) rc = sceNgs2RackGetVoiceHandle(h_mastering_rack, 0, &h_mastering);
    if (rc != 0) {
        printf("ngs2_limiter: %s: creating the system failed: %#x\n", p_case->sz_name, (unsigned)rc);
        return rc;
    }

    for (uint32_t i = 0; i < C_FRAMES; i++) {
        for (uint32_t c = 0; c < p_case->c_channels; c++) s_waveform[i * p_case->c_channels + c] = signal_sample(p_case->signals[c], i);
    }
    Ngs2MasteringSetupParam mastering_setup = {.c_io_channels = p_case->c_channels};
    Ngs2EventParam play = {.event_id = VOICE_EVENT_PLAY};
    Ngs2SamplerSetupParam sampler_setup = {.waveform_type = WAVEFORM_TYPE_PCM_F32L, .c_channels = p_case->c_channels, .rate_hz = RATE_HZ};
    const Ngs2WaveformBlock block = {.cb_data = (uint32_t)(C_FRAMES * p_case->c_channels * sizeof(float)), .c_samples = C_FRAMES};
    Ngs2WaveformBlocksParam blocks = {.p_data = s_waveform, .c_blocks = 1, .p_blocks = &block};
    Ngs2PatchParam patch = {.h_dest = h_mastering};

    rc = control(h_mastering, &mastering_setup, sizeof(mastering_setup), MASTERING_VOICE_PARAM_SETUP);
    int rc_limiter = 0;
    if (rc == 0 && p_case->has_limiter_param) {
        Ngs2LimiterParam limiter = p_case->limiter;
        rc_limiter = control(h_mastering, &limiter, limiter.header.cb_param, MASTERING_VOICE_PARAM_LIMITER);
        printf("ngs2_limiter: %s: limiter param size %u -> %#x\n", p_case->sz_name, limiter.header.cb_param, (unsigned)rc_limiter);
    }
    if (rc == 0 && p_case->has_lfe_param) {
        Ngs2LfeParam lfe = p_case->lfe;
        rc = control(h_mastering, &lfe, sizeof(lfe), MASTERING_VOICE_PARAM_LFE);
    }
    if (rc == 0) rc = control(h_mastering, &play, sizeof(play), VOICE_PARAM_EVENT);
    if (rc == 0) rc = control(h_sampler, &sampler_setup, sizeof(sampler_setup), SAMPLER_VOICE_PARAM_SETUP);
    if (rc == 0) rc = control(h_sampler, &blocks, sizeof(blocks), SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS);
    if (rc == 0) rc = control(h_sampler, &patch, sizeof(patch), VOICE_PARAM_PATCH);
    if (rc == 0) rc = control(h_sampler, &play, sizeof(play), VOICE_PARAM_EVENT);
    if (rc != 0) {
        printf("ngs2_limiter: %s: voice setup failed: %#x\n", p_case->sz_name, (unsigned)rc);
        sceNgs2SystemDestroy(h_system, NULL);
        return rc;
    }

    const Ngs2RenderBufferInfo render = {s_grain, sizeof(float) * C_GRAIN_FRAMES * p_case->c_channels, WAVEFORM_TYPE_PCM_F32L, p_case->c_channels};
    for (uint32_t i_grain = 0; i_grain < C_GRAINS && rc == 0; i_grain++) {
        rc = sceNgs2SystemRender(h_system, &render, 1);
        if (rc == 0) rc = sceNgs2VoiceGetState(h_mastering, &s_states[i_grain], sizeof(s_states[i_grain]));
        memcpy(s_rendered + (size_t)i_grain * C_GRAIN_FRAMES * p_case->c_channels, s_grain, render.cb_buffer);
    }
    if (rc != 0) printf("ngs2_limiter: %s: render failed: %#x\n", p_case->sz_name, (unsigned)rc);
    sceNgs2SystemDestroy(h_system, NULL);
    return rc != 0 ? rc : write_case(p_case, s_states, rc_limiter);
}

int main(void) {
    int rc = sceSysmoduleLoadModule(SYSMODULE_NGS2);
    if (rc != 0) {
        printf("ngs2_limiter: loading libSceNgs2 failed: %#x\n", (unsigned)rc);
        return 1;
    }
    int32_t sdk_version = 0;
    sceKernelGetCompiledSdkVersion(&sdk_version);
    /* The limiter's soft knee only runs for titles built with SDK 0x1700000 or later. */
    printf("ngs2_limiter: compiled SDK version %#x\n", (unsigned)sdk_version);
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_DIR, 0777);

#define STEREO_SIGNALS {SIGNAL_SINE_LOUD, SIGNAL_STEP}
#define SURROUND_SIGNALS {SIGNAL_SINE_50HZ, SIGNAL_SILENCE, SIGNAL_SILENCE, SIGNAL_SINE_1KHZ, SIGNAL_SILENCE, SIGNAL_SILENCE}
#define LIMITER(cb, enabled, level) .has_limiter_param = 1, .limiter = {.header = {.cb_param = (cb)}, .is_enabled = (enabled), .threshold = (level)}
    const Case cases[] = {
        {.sz_name = "default", .c_channels = 2, .signals = STEREO_SIGNALS},
        {.sz_name = "threshold_0.5", .c_channels = 2, .signals = STEREO_SIGNALS, LIMITER(0x10, 1, 0.5f)},
        {.sz_name = "threshold_9_clamped", .c_channels = 2, .signals = STEREO_SIGNALS, LIMITER(0x10, 1, 9.0f)},
        {.sz_name = "disabled", .c_channels = 2, .signals = STEREO_SIGNALS, LIMITER(0x10, 0, 0.5f)},
        {.sz_name = "wrong_size", .c_channels = 2, .signals = STEREO_SIGNALS, LIMITER(0x18, 1, 0.5f)},
        {.sz_name = "lfe_default", .c_channels = 6, .signals = SURROUND_SIGNALS},
        {.sz_name = "lfe_off", .c_channels = 6, .signals = SURROUND_SIGNALS, .has_lfe_param = 1, .lfe = {.is_enabled = 0, .frequency_hz = 120}},
    };
    int c_failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (run_case(&cases[i]) != 0) c_failed++;
    }
    printf("ngs2_limiter: done, %d case(s) failed\n", c_failed);
    return c_failed != 0;
}
