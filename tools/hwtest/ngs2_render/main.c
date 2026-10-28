/* Hardware test for libSceNgs2's render (kb #1095, phase F3 of #625): plays synthetic waveforms
 * through sampler, submixer and mastering voices and writes what each render produces, so that a PS4,
 * psport with the firmware module (PS_LLE=libSceNgs2) and psport's native Ngs2 (PS_NGS2=native) can
 * be compared sample by sample. Each case writes its PCM to /data/psport-hwtest/ngs2_render/<case>.csv
 * and its checks, with the value rea-ngs2 spec 03 to 06 predicts, to results.csv. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_DIR "/data/psport-hwtest/ngs2_render"
#define SYSMODULE_NGS2 0x000B
#define GRAIN 256
#define RATE_HZ 48000
#define MAX_CHANNELS 8
#define INFINITE 0xffffffffu

#define RACK_SAMPLER 0x1000
#define RACK_SUBMIXER 0x2000
#define RACK_MASTERING 0x3000

#define E(low) (0x804A0000u | (low))

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
    size_t cb_option;
    char sz_name[16];
    uint32_t flags, max_grain_samples, num_grain_samples, rate_hz, reserved[6];
} Ngs2SystemOption;

typedef struct {
    size_t cb_option;
    char sz_name[16];
    uint32_t flags, max_grain_samples, max_voices, max_input_delay_blocks, max_matrices, max_ports;
    uint32_t reserved[20];
} Ngs2RackOption;

typedef struct {
    Ngs2RackOption rack;
    uint32_t max_channel_works, max_codec_caches, max_waveform_blocks, max_envelope_points, max_filters;
    uint32_t max_atrac9_decoders, max_atrac9_channel_works, max_ajm_atrac9_decoders, num_peak_meter_blocks, reserved;
} Ngs2SamplerRackOption;

typedef struct {
    Ngs2RackOption rack;
    uint32_t max_channels, max_envelope_points, max_filters, max_inputs, num_peak_meter_blocks, reserved;
} Ngs2SubmixerRackOption;

typedef struct {
    uint16_t cb_param;
    int16_t off_next;
    uint32_t id;
} Ngs2ParamHeader;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t value;
    uint32_t value2;
} Ngs2WordParam;

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
    void (*pfn_handler)(const void *);
    uintptr_t callback_data;
    uint32_t flags, reserved;
} Ngs2CallbackParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t waveform_type, c_channels, rate_hz, config_data, off_frame, cb_frame_margin, flags, reserved;
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
} Ngs2BlocksParam;

typedef struct {
    Ngs2ParamHeader header;
    const void *p_from;
    const void *p_to;
} Ngs2AddressParam;

typedef struct {
    const void *p_data;
    Ngs2WaveformBlock block;
} Ngs2BlockRecord;

typedef struct {
    Ngs2ParamHeader header;
    float value, value2;
} Ngs2FloatParam;

typedef struct {
    uint32_t curve, duration;
    float height;
} Ngs2EnvelopePoint;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t c_forward, c_release;
    const Ngs2EnvelopePoint *p_points;
} Ngs2EnvelopeParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t index, location, type, channel_mask;
    float f[5];
    uint32_t reserved;
} Ngs2FilterParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t type, c_levels;
    const float *p_levels;
} Ngs2MasteringMatrixParam;

typedef struct {
    void *p_buffer;
    size_t cb_buffer;
    uint32_t waveform_type, c_channels;
} Ngs2RenderBufferInfo;

typedef struct {
    uintptr_t callback_data;
    Ngs2Handle h_voice;
    uint32_t flag, reserved;
    uintptr_t user_data;
    const void *p_data;
    uint32_t cb_data, c_repeats, attributes, reserved2;
} Ngs2CallbackInfo;

typedef struct {
    uint32_t state_flags;
    float envelope_height, peak_height;
    uint32_t reserved;
    uint64_t c_decoded_samples, cb_decoded;
    uint64_t user_data;
    const void *p_read;
} Ngs2SamplerState;

typedef struct {
    uint32_t state_flags;
    float envelope_height, peak_height, compressor_height;
} Ngs2SubmixerState;

typedef struct {
    uint32_t state_flags, reserved[2];
    float input_peaks[8], output_peaks[8];
} Ngs2MasteringState;

_Static_assert(sizeof(Ngs2SystemOption) == 0x40, "system option");
_Static_assert(sizeof(Ngs2SamplerRackOption) == 0xa8, "sampler option");
_Static_assert(sizeof(Ngs2SubmixerRackOption) == 0x98, "submixer option");
_Static_assert(sizeof(Ngs2SamplerSetupParam) == 0x28, "sampler setup");
_Static_assert(sizeof(Ngs2BlocksParam) == 0x20, "blocks param");
_Static_assert(sizeof(Ngs2WaveformBlock) == 0x20, "waveform block");
_Static_assert(sizeof(Ngs2EnvelopeParam) == 0x18, "envelope param");
_Static_assert(sizeof(Ngs2FilterParam) == 0x30, "filter param");
_Static_assert(sizeof(Ngs2MasteringMatrixParam) == 0x18, "mastering matrix param");
_Static_assert(sizeof(Ngs2CallbackInfo) == 0x38, "callback info");
_Static_assert(sizeof(Ngs2AddressParam) == 0x18, "address param");
_Static_assert(sizeof(Ngs2BlockRecord) == 0x28, "block record");
_Static_assert(sizeof(Ngs2SamplerState) == 0x30, "sampler state");
_Static_assert(sizeof(Ngs2MasteringState) == 0x4c, "mastering state");

int sceSysmoduleLoadModule(uint16_t id);
int sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption *p_option, const Ngs2BufferAllocator *p_allocator, Ngs2Handle *p_out_handle);
int sceNgs2SystemDestroy(Ngs2Handle h_system, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2SystemRender(Ngs2Handle h_system, const Ngs2RenderBufferInfo *p_buffers, uint32_t c_buffers);
int sceNgs2RackCreateWithAllocator(Ngs2Handle h_system, uint32_t rack_id, const void *p_option, const Ngs2BufferAllocator *p_allocator,
                                   Ngs2Handle *p_out_handle);
int sceNgs2RackDestroy(Ngs2Handle h_rack, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2RackGetVoiceHandle(Ngs2Handle h_rack, uint32_t i_voice, Ngs2Handle *p_out_handle);
int sceNgs2VoiceControl(Ngs2Handle h_voice, const Ngs2ParamHeader *p_params);
int sceNgs2VoiceGetStateFlags(Ngs2Handle h_voice, uint32_t *p_out_flags);
int sceNgs2VoiceGetState(Ngs2Handle h_voice, void *p_out_state, size_t cb_state);

static FILE *s_p_results;
static int s_c_mismatches;

static void record(const char *sz_case, uint32_t value, uint32_t expected) {
    const int is_match = value == expected;
    s_c_mismatches += !is_match;
    fprintf(s_p_results, "%s,%#x,%#x,%s\n", sz_case, value, expected, is_match ? "ok" : "MISMATCH");
}

/* A row the spec leaves open: the console's value is what psport learns from it. */
static void record_open(const char *sz_case, uint32_t value) {
    fprintf(s_p_results, "%s,%#x,,\n", sz_case, value);
}

static void record_near(const char *sz_case, double value, double expected, double tolerance) {
    const int is_match = fabs(value - expected) <= tolerance;
    s_c_mismatches += !is_match;
    fprintf(s_p_results, "%s,%.9g,%.9g,%s\n", sz_case, value, expected, is_match ? "ok" : "MISMATCH");
}

static int32_t allocate_buffer(Ngs2ContextBufferInfo *p_info) {
    p_info->p_host_buffer = calloc(1, p_info->cb_host_buffer);
    return p_info->p_host_buffer ? 0 : (int32_t)E(0x0002);
}

static int32_t free_buffer(Ngs2ContextBufferInfo *p_info) {
    free(p_info->p_host_buffer);
    p_info->p_host_buffer = NULL;
    return 0;
}

static const Ngs2BufferAllocator s_allocator = {allocate_buffer, free_buffer, 0};

static void header(Ngs2ParamHeader *p_header, uint16_t cb_param, uint32_t id) {
    p_header->cb_param = cb_param;
    p_header->off_next = 0;
    p_header->id = id;
}

static uint32_t control_word(Ngs2Handle h_voice, uint32_t id, uint32_t value, uint32_t value2) {
    Ngs2WordParam param;
    header(&param.header, sizeof(param), id);
    param.value = value, param.value2 = value2;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t control_float(Ngs2Handle h_voice, uint32_t id, float value, float value2) {
    Ngs2FloatParam param;
    header(&param.header, sizeof(param), id);
    param.value = value, param.value2 = value2;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t event(Ngs2Handle h_voice, uint32_t event_id) {
    Ngs2EventParam param;
    header(&param.header, sizeof(param), 6);
    param.event_id = event_id;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t patch(Ngs2Handle h_voice, uint32_t i_port, Ngs2Handle h_dest) {
    Ngs2PatchParam param;
    header(&param.header, sizeof(param), 5);
    param.i_port = i_port, param.i_dest_input = 0, param.h_dest = h_dest;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t setup_sampler(Ngs2Handle h_voice, uint32_t waveform_type, uint32_t c_channels, uint32_t rate_hz) {
    Ngs2SamplerSetupParam param = {0};
    header(&param.header, sizeof(param), 0x10000000);
    param.waveform_type = waveform_type, param.c_channels = c_channels, param.rate_hz = rate_hz;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t add_blocks(Ngs2Handle h_voice, const void *p_data, const Ngs2WaveformBlock *p_blocks, uint32_t c_blocks,
                           uint32_t flags) {
    Ngs2BlocksParam param;
    header(&param.header, sizeof(param), 0x10000001);
    param.p_data = p_data, param.flags = flags, param.c_blocks = c_blocks, param.p_blocks = p_blocks;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static Ngs2WaveformBlock block(uint32_t off, uint32_t cb, uint32_t c_samples, uint32_t c_repeats, uintptr_t user_data) {
    Ngs2WaveformBlock b = {off, cb, c_repeats, 0, c_samples, 0, user_data};
    return b;
}

static uint32_t flags_of(Ngs2Handle h_voice) {
    uint32_t flags = 0xdead;
    sceNgs2VoiceGetStateFlags(h_voice, &flags);
    return flags;
}

typedef struct {
    Ngs2Handle h_system, h_sampler, h_submixer, h_mastering;
    Ngs2Handle h_voice, h_sub_voice, h_master_voice;
} Graph;

static Graph create_graph(const void *p_sampler_option, const void *p_submixer_option) {
    Graph graph = {0};
    Ngs2SystemOption option = {.cb_option = sizeof(option), .max_grain_samples = 1024, .num_grain_samples = GRAIN, .rate_hz = RATE_HZ};
    record("setup_system", (uint32_t)sceNgs2SystemCreateWithAllocator(&option, &s_allocator, &graph.h_system), 0);
    record("setup_sampler", (uint32_t)sceNgs2RackCreateWithAllocator(graph.h_system, RACK_SAMPLER, p_sampler_option, &s_allocator,
                                                                      &graph.h_sampler), 0);
    if (p_submixer_option) {
        record("setup_submixer", (uint32_t)sceNgs2RackCreateWithAllocator(graph.h_system, RACK_SUBMIXER, p_submixer_option,
                                                                          &s_allocator, &graph.h_submixer), 0);
        sceNgs2RackGetVoiceHandle(graph.h_submixer, 0, &graph.h_sub_voice);
    }
    record("setup_mastering", (uint32_t)sceNgs2RackCreateWithAllocator(graph.h_system, RACK_MASTERING, NULL, &s_allocator,
                                                                        &graph.h_mastering), 0);
    sceNgs2RackGetVoiceHandle(graph.h_sampler, 0, &graph.h_voice);
    sceNgs2RackGetVoiceHandle(graph.h_mastering, 0, &graph.h_master_voice);
    return graph;
}

/* The mastering voice with c_channels inputs, playing; the sampler voice set up and patched to it. */
static Graph create_chain(uint32_t waveform_type, uint32_t c_sampler_channels, uint32_t c_master_channels, const void *p_sampler_option) {
    Graph graph = create_graph(p_sampler_option, NULL);
    record("setup_mastering_voice", control_word(graph.h_master_voice, 0x30000000, c_master_channels, 0), 0);
    record("setup_mastering_play", event(graph.h_master_voice, 0), 0);
    record("setup_sampler_voice", setup_sampler(graph.h_voice, waveform_type, c_sampler_channels, RATE_HZ), 0);
    record("setup_patch", patch(graph.h_voice, 0, graph.h_master_voice), 0);
    return graph;
}

static void destroy_graph(const Graph *p_graph) {
    sceNgs2RackDestroy(p_graph->h_sampler, NULL);
    if (p_graph->h_submixer) sceNgs2RackDestroy(p_graph->h_submixer, NULL);
    sceNgs2RackDestroy(p_graph->h_mastering, NULL);
    sceNgs2SystemDestroy(p_graph->h_system, NULL);
}

/* A case's CSV starts empty on its first render of a run and grows with the next ones. */
static FILE *open_case_csv(const char *sz_case) {
    static const char *s_sz_opened[64];
    static int s_c_opened;
    int is_opened = 0;
    for (int i = 0; i < s_c_opened; i++) is_opened |= strcmp(s_sz_opened[i], sz_case) == 0;
    if (!is_opened && s_c_opened < 64) s_sz_opened[s_c_opened++] = sz_case;
    char sz_path[128];
    snprintf(sz_path, sizeof(sz_path), SZ_OUT_DIR "/%s.csv", sz_case);
    return fopen(sz_path, is_opened ? "a" : "w");
}

/* Renders c_grains into p_out (interleaved F32, c_channels) and writes them to <case>.csv. */
static void render_f32(const char *sz_case, Ngs2Handle h_system, uint32_t c_channels, uint32_t c_grains, float *p_out) {
    FILE *p_csv = open_case_csv(sz_case);
    for (uint32_t i_grain = 0; i_grain < c_grains; i_grain++) {
        float *p_grain = p_out + (size_t)i_grain * GRAIN * c_channels;
        Ngs2RenderBufferInfo buffer = {p_grain, GRAIN * c_channels * sizeof(float), 0x18, c_channels};
        record("render", (uint32_t)sceNgs2SystemRender(h_system, &buffer, 1), 0);
        for (uint32_t i = 0; p_csv && i < GRAIN; i++) {
            fprintf(p_csv, "%u", i_grain * GRAIN + i);
            for (uint32_t c = 0; c < c_channels; c++) fprintf(p_csv, ",%.9g", p_grain[i * c_channels + c]);
            fputc('\n', p_csv);
        }
    }
    if (p_csv) fclose(p_csv);
}

static float s_pcm[64 * GRAIN * MAX_CHANNELS];
static float s_waveform[48000 * 2];

static void test_silence(void) {
    Graph graph = create_graph(NULL, NULL);
    record("silence_mastering_setup", control_word(graph.h_master_voice, 0x30000000, 2, 0), 0);
    record("silence_mastering_play", event(graph.h_master_voice, 0), 0);
    render_f32("silence", graph.h_system, 2, 2, s_pcm);
    float peak = 0;
    for (int i = 0; i < 2 * GRAIN * 2; i++) peak = fmaxf(peak, fabsf(s_pcm[i]));
    record_near("silence_peak", peak, 0, 0);
    record("silence_mastering_flags", flags_of(graph.h_master_voice), 3);
    destroy_graph(&graph);
}

/* Mono DC 0.5 through the fixed 1->2 port matrix (0.7071) and the identity 2->2 mastering matrix.
 * The voice starts with 4 zero samples of history (spec 09 §2), so sample 0 comes out at output 3,
 * and ends once its block is read with those 4 samples of history left (spec 05 §3.4): 1021 of 1024. */
static void test_dc(void) {
    Graph graph = create_chain(0x18, 1, 2, NULL);
    for (int n = 0; n < 1024; n++) s_waveform[n] = 0.5f;
    Ngs2WaveformBlock b = block(0, 4096, 1024, 0, 0);
    record("dc_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    record("dc_play", event(graph.h_voice, 0), 0);
    render_f32("dc", graph.h_system, 2, 6, s_pcm);
    uint32_t c_nonzero = 0;
    double max_error = 0;
    for (int i = 0; i < 6 * GRAIN; i++) {
        if (s_pcm[2 * i] != 0) {
            c_nonzero++;
            max_error = fmax(max_error, fabs(s_pcm[2 * i] - 0.5 * 0.70710678));
        }
    }
    record("dc_nonzero_samples", c_nonzero, 1021);
    record_near("dc_max_error", max_error, 0, 1e-6);
    record("dc_flags_after", flags_of(graph.h_voice), 0);
    destroy_graph(&graph);
}

/* A ramp n/65536 at pitch 0.5, 1, 2, 3 and at half the system rate: the output follows
 * (slope * i - 3) / 65536 exactly at phase 0 and at the symmetric phase 256 once the 4 zero samples
 * of history are past (from output sample 8). */
static void test_pitch(void) {
    static const struct {
        const char *sz_case;
        float ratio;
        uint32_t rate_hz;
        double slope;
    } cases[] = {{"pitch_1", 1, 48000, 1}, {"pitch_2", 2, 48000, 2}, {"pitch_0_5", 0.5f, 48000, 0.5},
                 {"pitch_rate_24k", 1, 24000, 0.5}, {"pitch_3", 3, 48000, 3}};
    for (int n = 0; n < 8192; n++) s_waveform[n] = (float)n / 65536.0f;
    for (size_t i_case = 0; i_case < sizeof(cases) / sizeof(cases[0]); i_case++) {
        Graph graph = create_chain(0x18, 1, 1, NULL);
        record("pitch_setup", setup_sampler(graph.h_voice, 0x18, 1, cases[i_case].rate_hz), 0);
        record("pitch_ratio", control_float(graph.h_voice, 0x10000005, cases[i_case].ratio, 0), 0);
        Ngs2WaveformBlock b = block(0, 8192 * 4, 8192, 0, 0);
        record("pitch_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
        record("pitch_play", event(graph.h_voice, 0), 0);
        render_f32(cases[i_case].sz_case, graph.h_system, 1, 4, s_pcm);
        double max_error = 0;
        for (int i = 8; i < 4 * GRAIN; i++) max_error = fmax(max_error, fabs(s_pcm[i] - (cases[i_case].slope * i - 3) / 65536.0));
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "%s_max_error", cases[i_case].sz_case);
        record_near(sz_case, max_error, 0, 1e-6);
        destroy_graph(&graph);
    }
}

static Ngs2CallbackInfo s_callbacks[16];
static uint32_t s_c_callbacks;
static void on_callback(const void *p_info) {
    if (s_c_callbacks < 16) memcpy(&s_callbacks[s_c_callbacks], p_info, sizeof(Ngs2CallbackInfo));
    s_c_callbacks++;
}

/* Block A (0..99) with 2 repeats, then block B (1000..1099): A A A B from output sample 3 = A[0].
 * Then A forever with an exit-loop request during the second grain: B begins at stream sample 300,
 * output sample 303. */
static void test_loops(void) {
    Ngs2SamplerRackOption option = {0};
    option.rack.cb_option = sizeof(option), option.rack.flags = 0x40000000u, option.rack.max_grain_samples = 1024;
    option.rack.max_voices = 4, option.rack.max_matrices = 1, option.rack.max_ports = 8;
    option.max_channel_works = 8, option.max_waveform_blocks = 4, option.max_envelope_points = 4, option.max_filters = 2;
    option.num_peak_meter_blocks = 8;
    Graph graph = create_chain(0x18, 1, 1, &option);
    for (int n = 0; n < 100; n++) s_waveform[n] = n / 4096.0f, s_waveform[100 + n] = (1000 + n) / 4096.0f;
    Ngs2CallbackParam callback = {{sizeof(callback), 0, 7}, on_callback, 0x77, 3, 0};
    record("loops_callback", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &callback.header), 0);
    Ngs2WaveformBlock blocks[2] = {block(0, 400, 100, 2, 0xa), block(400, 400, 100, 0, 0xb)};
    record("loops_blocks", add_blocks(graph.h_voice, s_waveform, blocks, 2, 0), 0);
    record("loops_play", event(graph.h_voice, 0), 0);
    s_c_callbacks = 0;
    render_f32("loops", graph.h_system, 1, 2, s_pcm);
    uint32_t c_mismatch = 0;
    for (int i = 0; i < 2 * GRAIN; i++) {
        const int stream = i - 3;
        const float expected = stream < 0     ? 0.0f
                               : stream < 300 ? (stream % 100) / 4096.0f
                               : stream < 400 ? (1000 + stream - 300) / 4096.0f
                                              : 0.0f;
        c_mismatch += s_pcm[i] != expected;
    }
    record("loops_mismatches", c_mismatch, 0);
    uint32_t c_loops = 0, c_ends = 0;
    for (uint32_t i = 0; i < s_c_callbacks && i < 16; i++) c_loops += s_callbacks[i].flag == 2, c_ends += s_callbacks[i].flag == 1;
    record("loops_loop_callbacks", c_loops, 2);
    record("loops_block_end_callbacks", c_ends, 2);
    record("loops_last_callback_user_data", s_c_callbacks >= 4 ? (uint32_t)s_callbacks[3].user_data : 0, 0xb);
    record_open("loops_loop_callback_repeats", s_c_callbacks ? s_callbacks[0].c_repeats : 0xffffffffu);
    record("loops_flags_after", flags_of(graph.h_voice), 0);

    record("exit_setup", setup_sampler(graph.h_voice, 0x18, 1, RATE_HZ), 0);
    blocks[0] = block(0, 400, 100, INFINITE, 0), blocks[1] = block(400, 400, 100, 0, 0);
    record("exit_blocks", add_blocks(graph.h_voice, s_waveform, blocks, 2, 0), 0);
    record("exit_play", event(graph.h_voice, 0), 0);
    render_f32("exit_loop", graph.h_system, 1, 1, s_pcm);
    Ngs2ParamHeader exit_loop = {8, 0, 0x10000004};
    record("exit_request", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &exit_loop), 0);
    render_f32("exit_loop", graph.h_system, 1, 1, s_pcm + GRAIN);
    uint32_t i_first_b = 0;
    for (int i = 0; i < 2 * GRAIN; i++) {
        if (s_pcm[i] >= 1000 / 4096.0f) {
            i_first_b = i;
            break;
        }
    }
    record("exit_first_b_sample", i_first_b, 303);
    destroy_graph(&graph);
}

/* A block queued as "not last" runs out: the voice is starved (0x21) until the next block arrives. */
static void test_streaming(void) {
    Graph graph = create_chain(0x18, 1, 1, NULL);
    for (int n = 0; n < 300; n++) s_waveform[n] = 0.25f;
    Ngs2WaveformBlock b = block(0, 400, 100, 0, 0);
    record("stream_first", add_blocks(graph.h_voice, s_waveform, &b, 1, 1), 0);
    record("stream_play", event(graph.h_voice, 0), 0);
    render_f32("streaming", graph.h_system, 1, 1, s_pcm);
    uint32_t c_nonzero = 0;
    for (int i = 0; i < GRAIN; i++) c_nonzero += s_pcm[i] != 0;
    record("stream_first_nonzero", c_nonzero, 100);
    record("stream_starved_flags", flags_of(graph.h_voice), 0x21);
    b = block(400, 800, 200, 0, 0);
    record("stream_second", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    render_f32("streaming", graph.h_system, 1, 1, s_pcm);
    c_nonzero = 0;
    for (int i = 0; i < GRAIN; i++) c_nonzero += s_pcm[i] != 0;
    record("stream_second_nonzero", c_nonzero, 200);
    record("stream_flags_after", flags_of(graph.h_voice), 0);
    record("stream_terminated", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), E(0x151));
    destroy_graph(&graph);
}

/* PAUSE fades out the last 64 samples, RESUME fades in the first 64, STOP fades 64 and ends. With an
 * envelope (linear attack 128, release 384), STOP releases instead. */
static void test_events(void) {
    Graph graph = create_chain(0x18, 1, 1, NULL);
    for (int n = 0; n < 256; n++) s_waveform[n] = 0.5f;
    Ngs2WaveformBlock b = block(0, 1024, 256, INFINITE, 0);
    record("events_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    record("events_play", event(graph.h_voice, 0), 0);
    render_f32("events", graph.h_system, 1, 1, s_pcm);
    record("events_pause", event(graph.h_voice, 4), 0);
    render_f32("events", graph.h_system, 1, 1, s_pcm);
    record_near("events_pause_224", s_pcm[224], 0.25, 1e-6);
    record("events_paused_flags", flags_of(graph.h_voice), 5);
    render_f32("events", graph.h_system, 1, 1, s_pcm);
    record_near("events_paused_sample", s_pcm[100], 0, 0);
    record("events_resume", event(graph.h_voice, 5), 0);
    render_f32("events", graph.h_system, 1, 1, s_pcm);
    record_near("events_resume_32", s_pcm[32], 0.25, 1e-6);
    record("events_stop", event(graph.h_voice, 1), 0);
    render_f32("events", graph.h_system, 1, 1, s_pcm);
    record_near("events_stop_32", s_pcm[32], 0.25, 1e-6);
    record_near("events_stop_64", s_pcm[64], 0, 0);
    record("events_stopped_flags", flags_of(graph.h_voice), 0);

    record("envelope_setup", setup_sampler(graph.h_voice, 0x18, 1, RATE_HZ), 0);
    record("envelope_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    static const Ngs2EnvelopePoint s_points[2] = {{0, 128, 1.0f}, {0, 384, 0.0f}};
    Ngs2EnvelopeParam envelope = {{sizeof(envelope), 0, 0x10000006}, 1, 1, s_points};
    record("envelope_param", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &envelope.header), 0);
    record("envelope_play", event(graph.h_voice, 0), 0);
    render_f32("envelope", graph.h_system, 1, 1, s_pcm);
    record_near("envelope_attack_64", s_pcm[64], 0.25, 1e-6);
    record("envelope_stop", event(graph.h_voice, 1), 0);
    render_f32("envelope", graph.h_system, 1, 2, s_pcm);
    record_near("envelope_release_192", s_pcm[192], 0.25, 1e-5);
    record_near("envelope_release_end", s_pcm[GRAIN + 128], 0, 0);
    record("envelope_flags_after", flags_of(graph.h_voice), 0);
    destroy_graph(&graph);
}

/* An 8-channel DC (0.05 * (c + 1)) into an 8-input mastering voice rendered to 2, 6 and 8 channels:
 * the default 8->N matrices of spec 04 §3.2, and channel 3 through the LFE low-pass for 6 and 8. */
static void test_mastering_matrices(void) {
    static const uint32_t s_channels[] = {2, 6, 8};
    for (size_t i_case = 0; i_case < 3; i_case++) {
        const uint32_t c_out = s_channels[i_case];
        Graph graph = create_chain(0x18, 8, 8, NULL);
        for (int n = 0; n < 256; n++)
            for (int c = 0; c < 8; c++) s_waveform[8 * n + c] = 0.05f * (c + 1);
        Ngs2WaveformBlock b = block(0, 8 * 4 * 256, 256, INFINITE, 0);
        record("matrix_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
        record("matrix_play", event(graph.h_voice, 0), 0);
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "mastering_8_to_%u", c_out);
        render_f32(sz_case, graph.h_system, c_out, 40, s_pcm);
        const float *p_last = s_pcm + (size_t)39 * GRAIN * c_out + (GRAIN - 1) * c_out;
        const double in[8] = {0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40};
        double expected[8] = {0};
        if (c_out == 2) {
            expected[0] = in[0] + in[2] * 0.70710678 + in[4] * 0.5 + in[6] * 0.5;
            expected[1] = in[1] + in[2] * 0.70710678 + in[5] * 0.5 + in[7] * 0.5;
        } else if (c_out == 6) {
            for (int c = 0; c < 6; c++) expected[c] = in[c];
            expected[4] += in[6], expected[5] += in[7];
        } else {
            for (int c = 0; c < 8; c++) expected[c] = in[c];
        }
        for (uint32_t c = 0; c < c_out; c++) {
            snprintf(sz_case, sizeof(sz_case), "mastering_8_to_%u_ch%u", c_out, c);
            /* Channel 3 crosses the LFE low-pass (DC gain about 0.99) when there are 6 or 8 outputs. */
            record_near(sz_case, p_last[c], expected[c], c == 3 && c_out > 2 ? 0.01 : 1e-6);
        }
        destroy_graph(&graph);
    }
}

/* A 0.9 sine at 1 kHz through a 2->2 mastering matrix of 2.0 reaches the limiter at 1.8; the
 * default limiter (threshold 1.0, slope 0) brings it to 1.0; then gain 0.5 halves it. Meters in mode 1. */
static void test_limiter_and_gain(void) {
    Graph graph = create_chain(0x18, 2, 2, NULL);
    for (int n = 0; n < 48000; n++) s_waveform[2 * n] = s_waveform[2 * n + 1] = 0.9f * sinf(2 * 3.14159265f * 1000 * n / 48000.0f);
    static const float s_levels[4] = {2, 0, 0, 2};
    Ngs2MasteringMatrixParam matrix = {{sizeof(matrix), 0, 0x30000001}, 5, 4, s_levels};
    record("limiter_matrix", (uint32_t)sceNgs2VoiceControl(graph.h_master_voice, &matrix.header), 0);
    record("limiter_meter", control_word(graph.h_master_voice, 0x30000006, 1, 0), 0);
    Ngs2WaveformBlock b = block(0, 48000 * 8, 48000, INFINITE, 0);
    record("limiter_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    record("limiter_play", event(graph.h_voice, 0), 0);
    float peak = 0;
    for (int i_grain = 0; i_grain < 100; i_grain++) {
        render_f32("limiter", graph.h_system, 2, 1, s_pcm);
        peak = 0;
        for (int i = 0; i < 2 * GRAIN; i++) peak = fmaxf(peak, fabsf(s_pcm[i]));
    }
    record_near("limiter_settled_peak", peak, 1.0, 0.05);
    Ngs2MasteringState state;
    record("limiter_state", (uint32_t)sceNgs2VoiceGetState(graph.h_master_voice, &state, sizeof(state)), 0);
    record_near("limiter_input_peak", state.input_peaks[0], 0.9, 0.001);
    record_near("limiter_output_peak", state.output_peaks[0], peak, 0.05);
    record("limiter_state_word_1", state.reserved[0], 0);
    record("limiter_state_word_2", state.reserved[1], 0);
    record("gain_param", control_float(graph.h_master_voice, 0x30000004, 0.5f, 9.0f), 0);
    render_f32("gain", graph.h_system, 2, 5, s_pcm);
    peak = 0;
    for (int i = 2 * GRAIN; i < 5 * 2 * GRAIN; i++) peak = fmaxf(peak, fabsf(s_pcm[i]));
    record_near("gain_peak", peak, 0.5, 0.03);
    Ngs2FloatParam gain;
    header(&gain.header, sizeof(gain), 0x30008004);
    sceNgs2VoiceControl(graph.h_master_voice, &gain.header);
    record_near("gain_read_lfe_clamped", gain.value2, 4.0, 0);
    static int16_t s_i16[GRAIN * 2];
    Ngs2RenderBufferInfo buffer = {s_i16, sizeof(s_i16), 0x12, 2};
    record("i16_render", (uint32_t)sceNgs2SystemRender(graph.h_system, &buffer, 1), 0);
    int max_i16 = 0;
    for (int i = 0; i < GRAIN * 2; i++) max_i16 = abs(s_i16[i]) > max_i16 ? abs(s_i16[i]) : max_i16;
    record_near("i16_peak", max_i16, 0.5 * 32767, 400);
    destroy_graph(&graph);
}

/* DC 0.9 through a 2->2 mastering matrix of 2.0 reaches the setup's limiter at 1.8: target 1/1.8,
 * attack 1 ms (rea-ngs2 spec 09 §3.1), so sample i of the first grain is 1.8 (T + kA^(i+1) (1 - T)) with
 * kA = exp(-1/48). */
static void test_limiter_attack(void) {
    Graph graph = create_chain(0x18, 2, 2, NULL);
    for (int n = 0; n < 2 * 256; n++) s_waveform[n] = 0.9f;
    static const float s_levels[4] = {2, 0, 0, 2};
    Ngs2MasteringMatrixParam matrix = {{sizeof(matrix), 0, 0x30000001}, 5, 4, s_levels};
    record("attack_matrix", (uint32_t)sceNgs2VoiceControl(graph.h_master_voice, &matrix.header), 0);
    Ngs2WaveformBlock b = block(0, 2 * 4 * 256, 256, INFINITE, 0);
    record("attack_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    record("attack_play", event(graph.h_voice, 0), 0);
    render_f32("limiter_attack", graph.h_system, 2, 1, s_pcm);
    const double target = 1 / 1.8, k_attack = exp(-1 / 48.0);
    record_near("limiter_attack_48", s_pcm[2 * 48], 1.8 * (target + pow(k_attack, 49) * (1 - target)), 1e-4);
    record_near("limiter_attack_200", s_pcm[2 * 200], 1.8 * (target + pow(k_attack, 201) * (1 - target)), 1e-4);
    destroy_graph(&graph);
}

/* Filters at location 2 (before port 0): a 1 kHz low-pass keeps DC 0.5, a high-pass removes it. */
static void test_filters(void) {
    for (uint32_t type = 1; type <= 2; type++) {
        Graph graph = create_chain(0x18, 1, 1, NULL);
        for (int n = 0; n < 256; n++) s_waveform[n] = 0.5f;
        Ngs2WaveformBlock b = block(0, 1024, 256, INFINITE, 0);
        record("filter_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
        Ngs2FilterParam filter = {{sizeof(filter), 0, 0x1000000a}, 0, 2, type, 0, {1000, 0.7071f, 1, 0, 0}, 0};
        record("filter_param", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &filter.header), 0);
        filter.location = 0x12;
        record("filter_location_0x12", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &filter.header), E(0x602));
        record("filter_play", event(graph.h_voice, 0), 0);
        render_f32(type == 1 ? "filter_low_pass" : "filter_high_pass", graph.h_system, 1, 20, s_pcm);
        record_near(type == 1 ? "filter_low_pass_dc" : "filter_high_pass_dc", s_pcm[20 * GRAIN - 1], type == 1 ? 0.5 : 0, 1e-4);
        destroy_graph(&graph);
    }
}

/* The sampler's meter: peak (mode 1) then RMS (mode 2) of an alternating -0.3 / 0.6 signal. */
static void test_meters(void) {
    Graph graph = create_chain(0x18, 1, 1, NULL);
    for (int n = 0; n < 256; n++) s_waveform[n] = n % 2 ? 0.6f : -0.3f;
    Ngs2WaveformBlock b = block(0, 1024, 256, INFINITE, 0);
    record("meter_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    record("meter_mode_3", control_word(graph.h_voice, 0x10000009, 3, 0), E(0x510));
    record("meter_mode_1", control_word(graph.h_voice, 0x10000009, 1, 0), 0);
    record("meter_play", event(graph.h_voice, 0), 0);
    render_f32("meters", graph.h_system, 1, 2, s_pcm);
    Ngs2SamplerState state;
    sceNgs2VoiceGetState(graph.h_voice, &state, sizeof(state));
    record_near("meter_peak", state.peak_height, 0.6, 1e-6);
    record_near("meter_envelope_height", state.envelope_height, 1.0, 0);
    record_open("meter_decoded_samples", (uint32_t)state.c_decoded_samples);
    record_open("meter_decoded_bytes", (uint32_t)state.cb_decoded);
    record("meter_mode_2", control_word(graph.h_voice, 0x10000009, 2, 0), 0);
    render_f32("meters", graph.h_system, 1, 10, s_pcm);
    sceNgs2VoiceGetState(graph.h_voice, &state, sizeof(state));
    record_near("meter_rms", state.peak_height, sqrt((0.36 + 0.09) / 2), 1e-5);
    destroy_graph(&graph);
}

/* UNCONFIRMED (rea-ngs2 spec 09 §7, case 1): block A holds 100 samples of data but asks for 150 and
 * block B follows. When A's bytes end on a frame boundary the firmware pops A and goes on reading B
 * for the rest of A's region with A's userData, then drops B with A at the region's end: A[0..99],
 * B[0..49], then silence, two block-end callbacks (A, then B with A's userData) and the voice ends.
 * Then the same with A two bytes past a frame: the partial frame ends A at once, the rest of that
 * grain is silent with flags 0x21, and B plays as its own block from the next grain. */
static void test_short_block(void) {
    Ngs2SamplerRackOption option = {0};
    option.rack.cb_option = sizeof(option), option.rack.flags = 0x40000000u, option.rack.max_grain_samples = 1024;
    option.rack.max_voices = 1, option.rack.max_matrices = 1, option.rack.max_ports = 1;
    option.max_channel_works = 1, option.max_waveform_blocks = 4, option.max_envelope_points = 1, option.max_filters = 1;
    option.num_peak_meter_blocks = 1;
    Graph graph = create_chain(0x18, 1, 1, &option);
    for (int n = 0; n < 1000; n++) s_waveform[n] = (n + 1) / 4096.0f, s_waveform[1000 + n] = (2000 + n) / 4096.0f;
    Ngs2CallbackParam callback = {{sizeof(callback), 0, 7}, on_callback, 0x77, 1, 0};
    record("short_block_callback", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &callback.header), 0);
    Ngs2WaveformBlock blocks[2] = {block(0, 400, 150, 0, 0xa), block(4000, 400, 100, 0, 0xb)};
    record("short_block_blocks", add_blocks(graph.h_voice, s_waveform, blocks, 2, 0), 0);
    record("short_block_play", event(graph.h_voice, 0), 0);
    s_c_callbacks = 0;
    render_f32("short_block", graph.h_system, 1, 2, s_pcm);
    record_near("short_block_a_last", s_pcm[3 + 99], s_waveform[99], 0);
    record_near("short_block_b_first", s_pcm[3 + 100], s_waveform[1000], 0);
    record_near("short_block_b_49", s_pcm[3 + 149], s_waveform[1049], 0);
    record_near("short_block_after_region", s_pcm[3 + 150], 0, 0);
    record("short_block_callbacks", s_c_callbacks, 2);
    record("short_block_second_user_data", s_c_callbacks >= 2 ? (uint32_t)s_callbacks[1].user_data : 0, 0xa);
    record("short_block_second_data", s_c_callbacks >= 2 ? (uint32_t)((const char *)s_callbacks[1].p_data - (const char *)s_waveform) : 0,
           4000);
    record("short_block_flags_after", flags_of(graph.h_voice), 0);

    record("partial_frame_setup", setup_sampler(graph.h_voice, 0x18, 1, RATE_HZ), 0);
    blocks[0] = block(0, 402, 150, 0, 0xa), blocks[1] = block(4000, 1600, 400, 0, 0xb);
    record("partial_frame_blocks", add_blocks(graph.h_voice, s_waveform, blocks, 2, 0), 0);
    record("partial_frame_play", event(graph.h_voice, 0), 0);
    render_f32("partial_frame", graph.h_system, 1, 1, s_pcm);
    record_near("partial_frame_a_last", s_pcm[3 + 99], s_waveform[99], 0);
    record_near("partial_frame_silence", s_pcm[3 + 100], 0, 0);
    record("partial_frame_flags", flags_of(graph.h_voice), 0x21);
    render_f32("partial_frame", graph.h_system, 1, 1, s_pcm);
    record_near("partial_frame_b_first", s_pcm[3], s_waveform[1000], 0);
    destroy_graph(&graph);
}

/* 0x10008001 with 0x10000001 (not last) of 1, 2 and 4 blocks on fresh voices of a rack of 4 blocks.
 * UNCONFIRMED (rea-ngs2 spec 09 §6.3): the firmware walks the ring from the oldest of the slots written
 * since the last reset and stops one short of the newest, so it answers 4 records (the valid slot and
 * the three zeroed slots after it) for 1 block, 1 record for 2 and 3 for 4. Then 0x10000002 moves
 * every slot's address, and KILL locks the waveform: the killed voice's last grain fades its last
 * sample to 0 instead of reading, under the KILL's own 64-sample cut. */
static uint32_t read_blocks(Ngs2Handle h_voice, Ngs2BlockRecord *p_records, uint32_t *p_out_word) {
    Ngs2AddressParam param = {{sizeof(param), 0, 0x10008001}, (const void *)(uintptr_t)0x12345678u, p_records};
    const uint32_t rc = (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
    uint32_t words[2];
    memcpy(words, (const char *)&param + 8, sizeof(words));
    *p_out_word = words[0];
    return rc == 0 ? words[1] : rc;
}

static void test_block_read(void) {
    Ngs2SamplerRackOption option = {0};
    option.rack.cb_option = sizeof(option), option.rack.max_grain_samples = 1024, option.rack.max_voices = 3;
    option.rack.max_matrices = 1, option.rack.max_ports = 1;
    option.max_channel_works = 3, option.max_waveform_blocks = 4, option.max_envelope_points = 1, option.max_filters = 1;
    option.num_peak_meter_blocks = 1;
    Graph graph = create_graph(&option, NULL);
    static const uint32_t s_c_blocks[3] = {1, 2, 4};
    static const uint32_t s_c_expected[3] = {4, 1, 3};
    static Ngs2BlockRecord s_records[8];
    for (int i_case = 0; i_case < 3; i_case++) {
        Ngs2Handle h_voice;
        sceNgs2RackGetVoiceHandle(graph.h_sampler, (uint32_t)i_case, &h_voice);
        record("blocks_read_setup", setup_sampler(h_voice, 0x18, 1, RATE_HZ), 0);
        Ngs2WaveformBlock blocks[4];
        for (uint32_t i = 0; i < 4; i++) blocks[i] = block(400 * i, 400, 100, i, 0xa + i);
        record("blocks_read_add", add_blocks(h_voice, s_waveform, blocks, s_c_blocks[i_case], 1), 0);
        memset(s_records, 0xcc, sizeof(s_records));
        uint32_t word = 0xdead;
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "blocks_read_%u_count", s_c_blocks[i_case]);
        record(sz_case, read_blocks(h_voice, s_records, &word), s_c_expected[i_case]);
        snprintf(sz_case, sizeof(sz_case), "blocks_read_%u_word_8", s_c_blocks[i_case]);
        record(sz_case, word, 0);
        snprintf(sz_case, sizeof(sz_case), "blocks_read_%u_first_user_data", s_c_blocks[i_case]);
        record(sz_case, (uint32_t)s_records[0].block.user_data, 0xa);
        snprintf(sz_case, sizeof(sz_case), "blocks_read_%u_first_data", s_c_blocks[i_case]);
        record(sz_case, s_records[0].p_data == s_waveform, 1);
        snprintf(sz_case, sizeof(sz_case), "blocks_read_%u_first_size_samples", s_c_blocks[i_case]);
        record(sz_case, s_records[0].block.cb_data << 16 | s_records[0].block.c_samples, 400u << 16 | 100);
        if (s_c_blocks[i_case] == 1) {
            record("blocks_read_1_second_size", s_records[1].block.cb_data, 0);
            record("blocks_read_1_fifth_untouched", s_records[4].block.cb_data, 0xccccccccu);
        } else if (s_c_blocks[i_case] == 4) {
            record("blocks_read_4_third_user_data", (uint32_t)s_records[2].block.user_data, 0xc);
            record("blocks_read_4_third_repeats", s_records[2].block.c_repeats, 2);
            record("blocks_read_4_fourth_untouched", s_records[3].block.cb_data, 0xccccccccu);
            Ngs2AddressParam replace = {{sizeof(replace), 0, 0x10000002}, s_waveform, s_pcm};
            record("replace_address", (uint32_t)sceNgs2VoiceControl(h_voice, &replace.header), 0);
            read_blocks(h_voice, s_records, &word);
            record("replace_address_slot_0", s_records[0].p_data == s_pcm, 1);
            record("replace_address_slot_2", s_records[2].p_data == s_pcm, 1);
            replace.p_to = NULL;
            record("replace_address_null", (uint32_t)sceNgs2VoiceControl(h_voice, &replace.header), E(0x406));
        }
    }
    destroy_graph(&graph);

    graph = create_chain(0x18, 1, 1, NULL);
    for (int n = 0; n < 256; n++) s_waveform[n] = 0.5f;
    Ngs2WaveformBlock b = block(0, 1024, 256, INFINITE, 0);
    record("kill_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    record("kill_play", event(graph.h_voice, 0), 0);
    render_f32("kill_ramp", graph.h_system, 1, 2, s_pcm);
    record("kill_event", event(graph.h_voice, 3), 0);
    render_f32("kill_ramp", graph.h_system, 1, 1, s_pcm);
    record_near("kill_ramp_32", s_pcm[32], (0.5 - 32 * 0.5 / 256) * 0.5, 1e-6);
    record_near("kill_ramp_64", s_pcm[64], 0, 0);
    record("kill_flags_after", flags_of(graph.h_voice), 0);
    destroy_graph(&graph);
}

/* 0xc001 (rea-ngs2 spec 09 §12): the pending block of a module in the firmware's own layout. The
 * sampler's pitch (module 1) is one float, the mastering limiter (module 17) after a setup reads
 * {active 1, threshold 1, slope 0, knee 0, attack 0.001, release 0.01, gain 1}, and a module without a
 * parameter reads zeros (inferred from memcpy_s with a NULL source). */
typedef struct {
    Ngs2ParamHeader header;
    uint32_t i_module, reserved;
    void *p_dst;
    uint32_t cb_dst, reserved2;
} Ngs2ModuleReadParam;

static void test_module_read(void) {
    Graph graph = create_chain(0x18, 1, 2, NULL);
    static uint8_t s_block[0x40];
    Ngs2ModuleReadParam read = {{sizeof(read), 0, 0xc001}, 1, 0, s_block, 8, 0};
    memset(s_block, 0xcc, sizeof(s_block));
    record("module_read_pitch_rc", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &read.header), 0);
    float value;
    memcpy(&value, s_block, sizeof(value));
    record_near("module_read_pitch", value, 1.0, 0);
    read.i_module = 3, read.cb_dst = 0x10;
    memset(s_block, 0xcc, sizeof(s_block));
    sceNgs2VoiceControl(graph.h_voice, &read.header);
    uint32_t word;
    memcpy(&word, s_block + 4, sizeof(word));
    record("module_read_unset_envelope", word, 0);
    read.i_module = 17, read.cb_dst = 0x20;
    sceNgs2VoiceControl(graph.h_master_voice, &read.header);
    record("module_read_limiter_active", s_block[0], 1);
    float limiter[6];
    memcpy(limiter, s_block + 4, sizeof(limiter));
    static const float s_expected[6] = {1.0f, 0.0f, 0.0f, 0.001f, 0.01f, 1.0f};
    for (int i = 0; i < 6; i++) {
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "module_read_limiter_%d", i);
        record_near(sz_case, limiter[i], s_expected[i], 0);
    }
    destroy_graph(&graph);
}

/* Channel works: 2-channel voices in a rack of 4 works; PLAY on a voice without setup reserves none and
 * fails with 0x804A0052 (spec 09 §1). */
static void test_channel_works(void) {
    Ngs2SamplerRackOption option = {0};
    option.rack.cb_option = sizeof(option), option.rack.max_grain_samples = 512, option.rack.max_voices = 4;
    option.rack.max_matrices = 1, option.rack.max_ports = 1;
    option.max_channel_works = 4, option.max_waveform_blocks = 1, option.num_peak_meter_blocks = 1;
    Graph graph = create_graph(&option, NULL);
    Ngs2Handle h_voices[4];
    for (uint32_t i = 0; i < 4; i++) sceNgs2RackGetVoiceHandle(graph.h_sampler, i, &h_voices[i]);
    record("works_play_without_setup", event(h_voices[3], 0), E(0x52));
    record("works_flags_without_setup", flags_of(h_voices[3]), 0);
    for (uint32_t i = 0; i < 3; i++) record("works_setup", setup_sampler(h_voices[i], 0x12, 2, RATE_HZ), 0);
    record("works_play_0", event(h_voices[0], 0), 0);
    record("works_play_1", event(h_voices[1], 0), 0);
    record("works_play_2", event(h_voices[2], 0), E(0x306));
    record("works_kill_1", event(h_voices[1], 3), 0);
    record("works_play_2_after_kill", event(h_voices[2], 0), 0);
    destroy_graph(&graph);
}

/* sampler -> submixer (port delay 100, 3 delay blocks) -> mastering: DC 0.5 arrives at sample 103, the
 * sampler's own 3 samples of history ahead of the delay. */
static void test_submixer(void) {
    Ngs2SubmixerRackOption sub = {0};
    sub.rack.cb_option = sizeof(sub), sub.rack.max_grain_samples = 512, sub.rack.max_voices = 1;
    sub.rack.max_input_delay_blocks = 3, sub.rack.max_matrices = 1, sub.rack.max_ports = 1;
    sub.max_channels = 2, sub.max_filters = 1, sub.max_inputs = 1, sub.num_peak_meter_blocks = 1;
    Graph graph = create_graph(NULL, &sub);
    for (int n = 0; n < 512; n++) s_waveform[n] = 0.5f;
    record("sub_sampler_setup", setup_sampler(graph.h_voice, 0x18, 2, RATE_HZ), 0);
    Ngs2WaveformBlock b = block(0, 2048, 256, INFINITE, 0);
    record("sub_blocks", add_blocks(graph.h_voice, s_waveform, &b, 1, 0), 0);
    record("sub_patch", patch(graph.h_voice, 0, graph.h_sub_voice), 0);
    record("sub_delay", control_word(graph.h_voice, 4, 0, 100), 0);
    record("sub_setup", control_word(graph.h_sub_voice, 0x20000000, 2, 0), 0);
    record("sub_patch_rack", patch(graph.h_sub_voice, 0, graph.h_mastering), 0);
    record("sub_play", event(graph.h_sub_voice, 0), 0);
    record("sub_mastering_setup", control_word(graph.h_master_voice, 0x30000000, 2, 0), 0);
    record("sub_mastering_play", event(graph.h_master_voice, 0), 0);
    record("sub_sampler_play", event(graph.h_voice, 0), 0);
    render_f32("submixer_delay", graph.h_system, 2, 2, s_pcm);
    uint32_t i_first = GRAIN;
    for (int i = 0; i < GRAIN; i++) {
        if (s_pcm[2 * i] != 0) {
            i_first = i;
            break;
        }
    }
    record("sub_first_sample", i_first, 103);
    record_near("sub_value", s_pcm[2 * 150], 0.5, 1e-6);
    Ngs2SubmixerState state;
    sceNgs2VoiceGetState(graph.h_sub_voice, &state, sizeof(state));
    record("sub_flags", state.state_flags, 3);
    record_near("sub_compressor_height", state.compressor_height, 1.0, 0);
    destroy_graph(&graph);
}

int main(void) {
    int rc = sceSysmoduleLoadModule(SYSMODULE_NGS2);
    if (rc != 0) {
        printf("ngs2_render: loading libSceNgs2 failed: %#x\n", (unsigned)rc);
        return 1;
    }
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_DIR, 0777);
    s_p_results = fopen(SZ_OUT_DIR "/results.csv", "w");
    if (!s_p_results) {
        printf("ngs2_render: cannot write %s/results.csv (errno %d)\n", SZ_OUT_DIR, errno);
        return 1;
    }
    fprintf(s_p_results, "case,value,expected,match\n");
    test_silence();
    test_dc();
    test_pitch();
    test_loops();
    test_streaming();
    test_short_block();
    test_block_read();
    test_module_read();
    test_events();
    test_mastering_matrices();
    test_limiter_and_gain();
    test_limiter_attack();
    test_filters();
    test_meters();
    test_channel_works();
    test_submixer();
    fclose(s_p_results);
    printf("ngs2_render: done, %d mismatch(es) against the values the spec predicts\n", s_c_mismatches);
    return s_c_mismatches != 0;
}
