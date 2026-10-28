/* Hardware test for libSceNgs2's reverb (kb #1097, phase F5 of #625): the reverb rack 0x2001's option and
 * parameter checks, its I3DL2 read-back, and the impulse response of a set of test rooms played through a
 * sampler, a reverb and a mastering voice, so that a PS4, psport with the firmware module
 * (PS_LLE=libSceNgs2) and psport's native Ngs2 (PS_NGS2=native) can be compared sample by sample. Each
 * room writes its PCM to /data/psport-hwtest/ngs2_reverb/<room>.csv and the checks, with the value
 * rea-ngs2 spec 07 predicts, go to results.csv. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_DIR "/data/psport-hwtest/ngs2_reverb"
#define SYSMODULE_NGS2 0x000B
#define GRAIN 256
#define RATE_HZ 48000
#define IR_GRAINS 188 /* 48128 samples, a little over a second */

#define RACK_SAMPLER 0x1000
#define RACK_REVERB 0x2001
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
    uint32_t max_channels, reverb_size;
} Ngs2ReverbRackOption;

typedef struct {
    uint16_t cb_param;
    int16_t off_next;
    uint32_t id;
} Ngs2ParamHeader;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t value, value2;
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
    uint32_t c_in_channels, c_out_channels, flags, reserved;
} Ngs2ReverbSetupParam;

typedef struct {
    float wet, dry;
    int32_t room, room_hf;
    uint32_t reflection_pattern;
    float decay_time_s, decay_hf_ratio;
    int32_t reflections;
    float reflections_delay_s;
    int32_t reverb;
    float reverb_delay_s, diffusion, density, hf_reference_hz;
    uint32_t reserved[8];
} Ngs2I3dl2;

typedef struct {
    Ngs2ParamHeader header;
    Ngs2I3dl2 i3dl2;
} Ngs2I3dl2Param;

/* 0x20010002 as rea read the setter (spec 07 §2.2, spec 11 §6); the SDK's own name for it is not known.
 * The level names follow what the process does: the first scales the input (the reverb on the LFE), the
 * second the reverb. */
typedef struct {
    Ngs2ParamHeader header;
    float dry, wet;
    float tap_delays_ms[5], tap_gains[5];
    uint32_t tap_mask;
    float comb_delays_ms[16];
    uint32_t comb_mask;
    float feedback, damping;
    uint32_t allpass_mask;
    float allpass_delays_ms[4], allpass_gain;
    float unread[7];
} Ngs2ReverbExplicitParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t i_module, reserved;
    void *p_dst;
    uint32_t cb_dst, reserved2;
} Ngs2ModuleReadParam;

typedef struct {
    void *p_buffer;
    size_t cb_buffer;
    uint32_t waveform_type, c_channels;
} Ngs2RenderBufferInfo;

_Static_assert(sizeof(Ngs2SystemOption) == 0x40, "system option");
_Static_assert(sizeof(Ngs2ReverbRackOption) == 0x88, "reverb option");
_Static_assert(sizeof(Ngs2ReverbSetupParam) == 0x18, "reverb setup");
_Static_assert(sizeof(Ngs2I3dl2Param) == 0x60, "reverb I3DL2");
_Static_assert(sizeof(Ngs2ReverbExplicitParam) == 0xbc, "reverb explicit");
_Static_assert(offsetof(Ngs2ReverbExplicitParam, tap_mask) == 0x38, "tap mask");
_Static_assert(offsetof(Ngs2ReverbExplicitParam, comb_mask) == 0x7c, "comb mask");
_Static_assert(offsetof(Ngs2ReverbExplicitParam, allpass_gain) == 0x9c, "all-pass gain");
_Static_assert(sizeof(Ngs2ModuleReadParam) == 0x20, "module read");

int sceSysmoduleLoadModule(uint16_t id);
int sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption *p_option, const Ngs2BufferAllocator *p_allocator, Ngs2Handle *p_out_handle);
int sceNgs2SystemDestroy(Ngs2Handle h_system, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2SystemRender(Ngs2Handle h_system, const Ngs2RenderBufferInfo *p_buffers, uint32_t c_buffers);
int sceNgs2RackQueryBufferSize(uint32_t rack_id, const void *p_option, Ngs2ContextBufferInfo *p_out_info);
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
static void record_open(const char *sz_case, double value) {
    fprintf(s_p_results, "%s,%.9g,,\n", sz_case, value);
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

static uint32_t event(Ngs2Handle h_voice, uint32_t event_id) {
    Ngs2EventParam param;
    header(&param.header, sizeof(param), 6);
    param.event_id = event_id;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t patch(Ngs2Handle h_voice, Ngs2Handle h_dest) {
    Ngs2PatchParam param;
    header(&param.header, sizeof(param), 5);
    param.i_port = 0, param.i_dest_input = 0, param.h_dest = h_dest;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t setup_reverb(Ngs2Handle h_voice, uint32_t c_in, uint32_t c_out) {
    Ngs2ReverbSetupParam param = {0};
    header(&param.header, sizeof(param), 0x20010000);
    param.c_in_channels = c_in, param.c_out_channels = c_out;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t set_i3dl2(Ngs2Handle h_voice, const Ngs2I3dl2 *p_i3dl2) {
    Ngs2I3dl2Param param;
    header(&param.header, sizeof(param), 0x20010001);
    param.i3dl2 = *p_i3dl2;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t read_i3dl2(Ngs2Handle h_voice, Ngs2I3dl2 *p_out) {
    Ngs2I3dl2Param param;
    memset(&param, 0x5a, sizeof(param));
    header(&param.header, sizeof(param), 0x20018001);
    const uint32_t result = (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
    *p_out = param.i3dl2;
    return result;
}

static Ngs2I3dl2 room(float wet, float dry, int32_t room_mb, int32_t room_hf, uint32_t pattern, float decay, float hf_ratio,
                      int32_t reflections, float reflections_delay, int32_t reverb, float reverb_delay, float diffusion, float density,
                      float hf_reference) {
    Ngs2I3dl2 i3dl2 = {wet, dry, room_mb, room_hf, pattern, decay, hf_ratio, reflections, reflections_delay, reverb,
                       reverb_delay, diffusion, density, hf_reference, {0}};
    return i3dl2;
}

/* Test rooms modeled on the I3DL2 presets, plus edges (high-frequency ratio 2, pattern 15, a sparse room
 * with the line's longest delays); the same ones psport's host check compares with its reference. */
static const struct {
    const char *sz_name;
    Ngs2I3dl2 i3dl2;
} s_rooms[] = {
    {"generic", {1, 0, -1000, -100, 0, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100, 100, 5000, {0}}},
    {"padded_cell", {1, 0, -1000, -6000, 1, 0.17f, 0.10f, -1204, 0.001f, 207, 0.002f, 100, 100, 5000, {0}}},
    {"bathroom", {1, 0, -1000, -1200, 2, 1.49f, 0.54f, -370, 0.007f, 1030, 0.011f, 100, 60, 5000, {0}}},
    {"stone_room", {1, 0, -1000, -300, 3, 2.31f, 0.64f, -711, 0.012f, 83, 0.017f, 100, 100, 5000, {0}}},
    {"concert_hall", {1, 0, -1000, -500, 4, 3.92f, 0.70f, -1230, 0.020f, -2, 0.029f, 100, 100, 5000, {0}}},
    {"cave", {1, 0, -1000, 0, 5, 2.91f, 1.30f, -602, 0.015f, -302, 0.022f, 100, 100, 5000, {0}}},
    {"arena", {1, 0, -1000, -698, 6, 7.24f, 0.33f, -1166, 0.020f, 16, 0.030f, 100, 100, 5000, {0}}},
    {"hangar", {1, 0, -1000, -1000, 7, 10.05f, 0.23f, -602, 0.020f, 198, 0.030f, 100, 100, 5000, {0}}},
    {"plate", {1, 0, -1000, -200, 9, 1.30f, 0.90f, 0, 0.002f, 0, 0.010f, 100, 75, 5000, {0}}},
    {"sparse", {0.8f, 0.3f, -500, -2000, 12, 0.6f, 0.2f, 500, 0.25f, 1500, 0.09f, 20, 0, 1200, {0}}},
    {"bright", {1, 0, 0, 0, 15, 1.0f, 2.0f, 0, 0.0f, 0, 0.0f, 0, 100, 20000, {0}}},
};

typedef struct {
    Ngs2Handle h_system, h_sampler, h_reverb, h_mastering;
    Ngs2Handle h_voice, h_reverb_voice, h_master_voice;
} Graph;

static float s_impulse[IR_GRAINS * GRAIN];
static float s_pcm[IR_GRAINS * GRAIN * 2];

/* sampler (mono F32 impulse of 0.25) -> reverb voice (1 in, 2 out) -> mastering (2) -> F32 stereo. The
 * sampler's four samples of history put the impulse at output sample 3 (spec 09 §2). */
static Graph create_graph(void) {
    Graph graph = {0};
    Ngs2SystemOption option = {.cb_option = sizeof(option), .max_grain_samples = 1024, .num_grain_samples = GRAIN, .rate_hz = RATE_HZ};
    record("setup_system", (uint32_t)sceNgs2SystemCreateWithAllocator(&option, &s_allocator, &graph.h_system), 0);
    record("setup_sampler", (uint32_t)sceNgs2RackCreateWithAllocator(graph.h_system, RACK_SAMPLER, NULL, &s_allocator, &graph.h_sampler), 0);
    record("setup_reverb", (uint32_t)sceNgs2RackCreateWithAllocator(graph.h_system, RACK_REVERB, NULL, &s_allocator, &graph.h_reverb), 0);
    record("setup_mastering",
           (uint32_t)sceNgs2RackCreateWithAllocator(graph.h_system, RACK_MASTERING, NULL, &s_allocator, &graph.h_mastering), 0);
    sceNgs2RackGetVoiceHandle(graph.h_sampler, 0, &graph.h_voice);
    sceNgs2RackGetVoiceHandle(graph.h_reverb, 0, &graph.h_reverb_voice);
    sceNgs2RackGetVoiceHandle(graph.h_mastering, 0, &graph.h_master_voice);
    Ngs2WordParam master = {{sizeof(master), 0, 0x30000000}, 2, 0};
    record("setup_mastering_voice", (uint32_t)sceNgs2VoiceControl(graph.h_master_voice, &master.header), 0);
    record("setup_mastering_play", event(graph.h_master_voice, 0), 0);
    Ngs2SamplerSetupParam sampler = {{sizeof(sampler), 0, 0x10000000}, 0x18, 1, RATE_HZ, 0, 0, 0, 0, 0};
    record("setup_sampler_voice", (uint32_t)sceNgs2VoiceControl(graph.h_voice, &sampler.header), 0);
    record("setup_patch_sampler", patch(graph.h_voice, graph.h_reverb_voice), 0);
    record("setup_patch_reverb", patch(graph.h_reverb_voice, graph.h_master_voice), 0);
    record("setup_reverb_voice", setup_reverb(graph.h_reverb_voice, 1, 2), 0);
    return graph;
}

static void destroy_graph(const Graph *p_graph) {
    sceNgs2RackDestroy(p_graph->h_sampler, NULL);
    sceNgs2RackDestroy(p_graph->h_reverb, NULL);
    sceNgs2RackDestroy(p_graph->h_mastering, NULL);
    sceNgs2SystemDestroy(p_graph->h_system, NULL);
}

/* Grains of silence through a playing reverb voice, so that its parameters settle before an impulse. */
static void render_silence(const Graph *p_graph, uint32_t c_grains) {
    for (uint32_t i_grain = 0; i_grain < c_grains; i_grain++) {
        Ngs2RenderBufferInfo buffer = {s_pcm, GRAIN * 2 * sizeof(float), 0x18, 2};
        record("render_silence", (uint32_t)sceNgs2SystemRender(p_graph->h_system, &buffer, 1), 0);
    }
}

static void render_ir_playing(const char *sz_case, const Graph *p_graph, int is_reverb_playing) {
    memset(s_impulse, 0, sizeof(s_impulse));
    s_impulse[0] = 0.25f;
    Ngs2WaveformBlock b = {0, sizeof(s_impulse), 0, 0, IR_GRAINS * GRAIN, 0, 0};
    Ngs2BlocksParam blocks = {{sizeof(blocks), 0, 0x10000001}, s_impulse, 0, 1, &b};
    record("ir_blocks", (uint32_t)sceNgs2VoiceControl(p_graph->h_voice, &blocks.header), 0);
    if (!is_reverb_playing) record("ir_play_reverb", event(p_graph->h_reverb_voice, 0), 0);
    record("ir_play_sampler", event(p_graph->h_voice, 0), 0);
    char sz_path[128];
    snprintf(sz_path, sizeof(sz_path), SZ_OUT_DIR "/%s.csv", sz_case);
    FILE *p_csv = fopen(sz_path, "w");
    for (uint32_t i_grain = 0; i_grain < IR_GRAINS; i_grain++) {
        float *p_grain = s_pcm + (size_t)i_grain * GRAIN * 2;
        Ngs2RenderBufferInfo buffer = {p_grain, GRAIN * 2 * sizeof(float), 0x18, 2};
        record("render", (uint32_t)sceNgs2SystemRender(p_graph->h_system, &buffer, 1), 0);
        for (uint32_t i = 0; p_csv && i < GRAIN; i++) {
            fprintf(p_csv, "%u,%.9g,%.9g\n", i_grain * GRAIN + i, p_grain[2 * i], p_grain[2 * i + 1]);
        }
    }
    if (p_csv) fclose(p_csv);
}

static void render_ir(const char *sz_case, const Graph *p_graph) {
    render_ir_playing(sz_case, p_graph, 0);
}

static uint32_t first_nonzero(void) {
    for (uint32_t i = 0; i < IR_GRAINS * GRAIN; i++) {
        if (s_pcm[2 * i] != 0 || s_pcm[2 * i + 1] != 0) return i;
    }
    return 0xffffffffu;
}

static void test_rack_options(void) {
    Ngs2ReverbRackOption option = {{sizeof(option), "", 0, 512, 1, 1, 1, 8, {0}}, 8, 1};
    Ngs2ContextBufferInfo info = {0};
    record("option_default", (uint32_t)sceNgs2RackQueryBufferSize(RACK_REVERB, &option, &info), 0);
    record_open("option_buffer_size_large", (double)info.cb_host_buffer);
    const size_t cb_large = info.cb_host_buffer;
    option.reverb_size = 0;
    record("option_size_0", (uint32_t)sceNgs2RackQueryBufferSize(RACK_REVERB, &option, &info), 0);
    record_open("option_buffer_size_small", (double)info.cb_host_buffer);
    /* The reflection line: 14400 samples with reverbSize 1, 4800 with 0 (spec 02 §4.3, 07 §1). */
    record("option_size_difference", (uint32_t)(cb_large - info.cb_host_buffer), (14400 - 4800) * 4);
    option.reverb_size = 2;
    record("option_size_2", (uint32_t)sceNgs2RackQueryBufferSize(RACK_REVERB, &option, &info), E(0x900));
    option.reverb_size = 1, option.rack.max_voices = 0;
    record("option_voices_0", (uint32_t)sceNgs2RackQueryBufferSize(RACK_REVERB, &option, &info), E(0x103));
    option.rack.max_voices = 17;
    record("option_voices_17", (uint32_t)sceNgs2RackQueryBufferSize(RACK_REVERB, &option, &info), E(0x103));
    option.rack.max_voices = 1, option.max_channels = 9;
    record("option_channels_9", (uint32_t)sceNgs2RackQueryBufferSize(RACK_REVERB, &option, &info), E(0x104));
    option.max_channels = 0;
    record("option_channels_0", (uint32_t)sceNgs2RackQueryBufferSize(RACK_REVERB, &option, &info), E(0x104));
}

static void test_parameters(void) {
    Graph graph = create_graph();
    const Ngs2Handle h = graph.h_reverb_voice;
    record("setup_channels_3", setup_reverb(h, 3, 2), E(0x52));
    record("setup_channels_out_0", setup_reverb(h, 1, 0), E(0x52));
    Ngs2ReverbSetupParam setup = {{0x10, 0, 0x20010000}, 1, 2, 0, 0};
    record("setup_size", (uint32_t)sceNgs2VoiceControl(h, &setup.header), E(0x30A));
    setup.header.cb_param = sizeof(setup), setup.header.id = 0x20010003;
    record("unknown_id", (uint32_t)sceNgs2VoiceControl(h, &setup.header), E(0x308));
    Ngs2ReverbSetupParam read_setup = {{sizeof(read_setup), 0, 0x20018000}, 0, 0, 0, 0};
    record("read_setup", (uint32_t)sceNgs2VoiceControl(h, &read_setup.header), 0);
    record("read_setup_in", read_setup.c_in_channels, 1);
    record("read_setup_out", read_setup.c_out_channels, 2);
    /* The setup's defaults (0x335a8, spec 03 §5.3). reverb -10000 mB is stored 24 dB lower, below the
     * -10000 floor, as 0; its read-back converts log10(0): unconfirmed, psport answers what x86's
     * truncation of -inf gives (INT32_MIN) plus 2400. */
    Ngs2I3dl2 i3dl2;
    record("defaults_read", read_i3dl2(h, &i3dl2), 0);
    record_near("defaults_wet", i3dl2.wet, 1.0, 0);
    record("defaults_room", (uint32_t)i3dl2.room, (uint32_t)-10000);
    record("defaults_reflections", (uint32_t)i3dl2.reflections, (uint32_t)-10000);
    record("defaults_reverb", (uint32_t)i3dl2.reverb, 0x80000000u + 2400);
    record_near("defaults_reflections_delay", i3dl2.reflections_delay_s, 0.02, 1e-7);
    record_near("defaults_hf_reference", i3dl2.hf_reference_hz, 5000, 0);
    record("defaults_reserved", i3dl2.reserved[0], 0);
    /* Clamps (spec 07 §2.1); wet and dry reach 32 from SDK 6.00 on, which this build's SDK is. */
    Ngs2I3dl2 wild = room(40, -1, 3000, 500, 16, 0.01f, 3, 2000, 1, 5000, 1, 200, -5, 1);
    record("clamp_set", set_i3dl2(h, &wild), 0);
    record("clamp_read", read_i3dl2(h, &i3dl2), 0);
    record_open("clamp_wet", i3dl2.wet);
    record_near("clamp_dry", i3dl2.dry, 0, 0);
    record("clamp_room", (uint32_t)i3dl2.room, 0);
    record("clamp_room_hf", (uint32_t)i3dl2.room_hf, 0);
    record("clamp_pattern", i3dl2.reflection_pattern, 0);
    record_near("clamp_decay", i3dl2.decay_time_s, 0.1, 1e-7);
    record_near("clamp_hf_ratio", i3dl2.decay_hf_ratio, 2, 0);
    record("clamp_reflections", (uint32_t)i3dl2.reflections, 1000);
    record_near("clamp_reflections_delay", i3dl2.reflections_delay_s, 0.3, 1e-7);
    record("clamp_reverb", (uint32_t)i3dl2.reverb, 2000);
    record_near("clamp_reverb_delay", i3dl2.reverb_delay_s, 0.1, 1e-7);
    record_near("clamp_diffusion", i3dl2.diffusion, 100, 0);
    record_near("clamp_density", i3dl2.density, 0, 0);
    record_near("clamp_hf_reference", i3dl2.hf_reference_hz, 20, 0);
    for (uint32_t i = 0; i < sizeof(s_rooms) / sizeof(s_rooms[0]); i++) {
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "round_trip_%s", s_rooms[i].sz_name);
        set_i3dl2(h, &s_rooms[i].i3dl2);
        read_i3dl2(h, &i3dl2);
        const int is_same = i3dl2.room == (s_rooms[i].i3dl2.room < 0 ? s_rooms[i].i3dl2.room : 0) &&
                            i3dl2.room_hf == s_rooms[i].i3dl2.room_hf && i3dl2.reflections == s_rooms[i].i3dl2.reflections &&
                            i3dl2.reverb == s_rooms[i].i3dl2.reverb;
        record(sz_case, (uint32_t)is_same, 1);
    }
    /* The explicit network (spec 07 §2.2): a tap past the line (14400 samples with reverbSize 1), combs
     * over their memory (the 16 full-rate lengths fill it exactly), then a valid set, after which the
     * I3DL2 read answers -1 (unconfirmed: the setter's return value as rea read it). */
    Ngs2ReverbExplicitParam explicit_param;
    memset(&explicit_param, 0, sizeof(explicit_param));
    header(&explicit_param.header, sizeof(explicit_param), 0x20010002);
    static const float s_comb_ms[16] = {22.01f, 23.25f, 24.75f, 26.6f,  28.25f, 29.63f, 31.06f, 32.44f,
                                        33.69f, 34.95f, 36.29f, 37.65f, 38.99f, 40.35f, 41.67f, 43.05f};
    explicit_param.tap_mask = 1, explicit_param.tap_delays_ms[0] = 14400.5f;
    record("explicit_tap_past_line", (uint32_t)sceNgs2VoiceControl(h, &explicit_param.header), E(0x30A));
    explicit_param.tap_delays_ms[0] = 10;
    explicit_param.comb_mask = 0xffff;
    memcpy(explicit_param.comb_delays_ms, s_comb_ms, sizeof(s_comb_ms));
    explicit_param.comb_delays_ms[15] = 43.15f;
    record("explicit_combs_over_memory", (uint32_t)sceNgs2VoiceControl(h, &explicit_param.header), E(0x30A));
    explicit_param.comb_delays_ms[15] = 43.05f;
    record("explicit_combs_fill_memory", (uint32_t)sceNgs2VoiceControl(h, &explicit_param.header), 0);
    explicit_param.allpass_mask = 1, explicit_param.allpass_delays_ms[0] = 40;
    record("explicit_allpass_over_memory", (uint32_t)sceNgs2VoiceControl(h, &explicit_param.header), E(0x30A));
    record("explicit_read_i3dl2", read_i3dl2(h, &i3dl2), 0xffffffffu);
    destroy_graph(&graph);
}

static void test_impulse_responses(void) {
    for (uint32_t i = 0; i < sizeof(s_rooms) / sizeof(s_rooms[0]); i++) {
        Graph graph = create_graph();
        const Ngs2I3dl2 *p_room = &s_rooms[i].i3dl2;
        record("ir_set", set_i3dl2(graph.h_reverb_voice, p_room), 0);
        render_ir(s_rooms[i].sz_name, &graph);
        char sz_case[64];
        /* Nothing reaches the output before the first tap at reflectionsDelay (spec 07 §3.3); dry is 0
         * but for "sparse". */
        snprintf(sz_case, sizeof(sz_case), "ir_%s_first_sample", s_rooms[i].sz_name);
        const uint32_t first = p_room->dry > 0 ? 3 : 3 + (uint32_t)(p_room->reflections_delay_s * RATE_HZ + 0.5f);
        record(sz_case, first_nonzero(), first);
        double energy = 0;
        for (uint32_t n = 0; n < IR_GRAINS * GRAIN * 2; n++) energy += (double)s_pcm[n] * s_pcm[n];
        snprintf(sz_case, sizeof(sz_case), "ir_%s_energy", s_rooms[i].sz_name);
        record_open(sz_case, energy);
        if (i + 1 == sizeof(s_rooms) / sizeof(s_rooms[0])) {
            /* "bright": room, reflections 0 mB, no delay, roomHF 0 (no filter): tap 0 at once with the
             * pattern's 0.7 on both channels. */
            record_near("ir_bright_first_tap_left", s_pcm[2 * 3], 0.25 * 0.7, 1e-7);
            record_near("ir_bright_first_tap_right", s_pcm[2 * 3 + 1], 0.25 * 0.7, 1e-7);
        }
        uint32_t state = 0xdead;
        sceNgs2VoiceGetState(graph.h_reverb_voice, &state, sizeof(state));
        snprintf(sz_case, sizeof(sz_case), "ir_%s_state", s_rooms[i].sz_name);
        record(sz_case, state, 3);
        destroy_graph(&graph);
    }
}

/* One explicit tap of 10.1 ms, gain 0.5: 0.125 at 3 + align8(485) = 491 (spec 11 §6.3: routing 0x10, tap
 * 0 early). An explicit network without all its comb bits right after a reset leaves combs of length 0,
 * on which the firmware hangs (spec 11 §6.4), so an I3DL2 grain comes first: its combs keep running, but
 * with reverb at -10000 mB its late group gain is 0, and room and reflections at 0 mB leave the early
 * group gain the explicit network inherits at 1. A silent grain then takes the taps' fade. */
static void test_explicit_tap(void) {
    Graph graph = create_graph();
    Ngs2I3dl2 prime = room(1, 0, 0, 0, 0, 1.0f, 1.0f, 0, 0.0f, -10000, 0.0f, 100, 100, 5000);
    record("explicit_tap_prime", set_i3dl2(graph.h_reverb_voice, &prime), 0);
    record("explicit_tap_play_reverb", event(graph.h_reverb_voice, 0), 0);
    render_silence(&graph, 2);
    Ngs2ReverbExplicitParam param;
    memset(&param, 0, sizeof(param));
    header(&param.header, sizeof(param), 0x20010002);
    param.wet = 1, param.tap_mask = 1, param.tap_delays_ms[0] = 10.1f, param.tap_gains[0] = 0.5f;
    record("explicit_tap_set", (uint32_t)sceNgs2VoiceControl(graph.h_reverb_voice, &param.header), 0);
    render_silence(&graph, 1);
    render_ir_playing("explicit_tap", &graph, 1);
    record("explicit_tap_first_sample", first_nonzero(), 491);
    record_near("explicit_tap_value", s_pcm[2 * 491], 0.125, 1e-7);
    destroy_graph(&graph);
}

/* The explicit network's comb delays: the 16 full-rate lengths (they fill the comb memory exactly), so
 * that no comb is left at length 0. */
static void set_explicit_combs(Ngs2ReverbExplicitParam *p_param, uint32_t comb_mask) {
    static const float s_comb_ms[16] = {22.01f, 23.25f, 24.75f, 26.6f,  28.25f, 29.63f, 31.06f, 32.44f,
                                        33.69f, 34.95f, 36.29f, 37.65f, 38.99f, 40.35f, 41.67f, 43.05f};
    static const float s_allpass_ms[4] = {11.58f, 9.19f, 7.1f, 4.69f};
    memset(p_param, 0, sizeof(*p_param));
    header(&p_param->header, sizeof(*p_param), 0x20010002);
    p_param->wet = 1;
    p_param->tap_mask = 0x10, p_param->tap_delays_ms[4] = 1, p_param->tap_gains[4] = 1;
    p_param->comb_mask = comb_mask;
    memcpy(p_param->comb_delays_ms, s_comb_ms, sizeof(s_comb_ms));
    p_param->feedback = 0.5f, p_param->damping = 0.2f;
    p_param->allpass_mask = 0xf;
    memcpy(p_param->allpass_delays_ms, s_allpass_ms, sizeof(s_allpass_ms));
    p_param->allpass_gain = 0.5f;
}

/* The explicit network right after a reset, every comb and all-pass set, only the late tap: the reset
 * leaves the prefilter off with its table zeroed, and the firmware's four-sample comb path reads that
 * table, so by rea's reading (spec 11 §6.4) the combs are fed 0 and the response is silent; psport passes
 * the late tap on as the firmware's one-sample path does and rings (FORK.md). */
static void test_explicit_after_reset(void) {
    Graph graph = create_graph();
    Ngs2ReverbExplicitParam param;
    set_explicit_combs(&param, 0xffff);
    record("explicit_after_reset_set", (uint32_t)sceNgs2VoiceControl(graph.h_reverb_voice, &param.header), 0);
    render_ir("explicit_after_reset", &graph);
    double energy = 0;
    for (uint32_t n = 0; n < IR_GRAINS * GRAIN * 2; n++) energy += (double)s_pcm[n] * s_pcm[n];
    record_open("explicit_after_reset_energy", energy);
    record("explicit_after_reset_silent", energy == 0, 1);
    destroy_graph(&graph);
}

/* RISK, run last: the explicit network right after a reset with one comb bit leaves 15 combs of length
 * 0, on which firmware 11.00's 0x27fc0 loops forever by rea's reading (spec 11 §6.4). The start row is
 * flushed first: a results.csv that ends with it means the console hung. */
static void test_explicit_partial_mask(void) {
    Graph graph = create_graph();
    Ngs2ReverbExplicitParam param;
    set_explicit_combs(&param, 0x0001);
    record("explicit_partial_mask_set", (uint32_t)sceNgs2VoiceControl(graph.h_reverb_voice, &param.header), 0);
    record_open("explicit_partial_mask_start", 1);
    fflush(s_p_results);
    printf("ngs2_reverb: explicit_partial_mask may hang the console\n");
    render_ir("explicit_partial_mask", &graph);
    record_open("explicit_partial_mask_done", 1);
    destroy_graph(&graph);
}

/* 0xc001 on the reverb voice: the parameter block in the firmware's layout (spec 07 §2.1). */
static void test_module_read(void) {
    Graph graph = create_graph();
    record("module_read_set", set_i3dl2(graph.h_reverb_voice, &s_rooms[4].i3dl2), 0);
    static uint8_t s_block[0x150];
    Ngs2ModuleReadParam param = {{sizeof(param), 0, 0xc001}, 0, 0, s_block, sizeof(s_block), 0};
    record("module_read", (uint32_t)sceNgs2VoiceControl(graph.h_reverb_voice, &param.header), 0);
    float value;
    uint32_t word;
    memcpy(&word, s_block + 0x08, 4), record("module_read_mode", word, 0);
    memcpy(&value, s_block + 0x0c, 4), record_near("module_read_wet", value, 1, 0);
    memcpy(&value, s_block + 0x14, 4), record_near("module_read_room", value, 0.316227766, 1e-7);
    memcpy(&value, s_block + 0x20, 4), record_near("module_read_decay", value, 3.92, 1e-6);
    memcpy(&value, s_block + 0x30, 4), record_near("module_read_reverb", value, pow(10.0, -2402 / 2000.0), 1e-7);
    memcpy(&value, s_block + 0x40, 4), record_near("module_read_hf_reference", value, 5000, 0);
    memcpy(&value, s_block + 0xd0, 4), record_near("module_read_dry_0", value, 0, 0);
    memcpy(&value, s_block + 0xf0 + 12, 4), record_near("module_read_wet_3", value, 0, 0);
    memcpy(&value, s_block + 0xf0 + 16, 4), record_near("module_read_wet_4", value, 1, 0);
    /* The I3DL2 setter sets bit 3 of +0x132, the LFE's bypass of the output filter (spec 11 §8.2). */
    record("module_read_bypass_mask", s_block[0x132], 0x08);
    /* +0x44 to +0xbc hold the explicit network (none set here), +0xc0 to +0xcf nothing; from +0x110 the
     * process writes the output filter's descriptor into the active block, which 0xc001 does not copy. */
    for (uint32_t off = 0x44; off < 0xd0; off += 4) {
        memcpy(&word, s_block + off, 4);
        if (word != 0) {
            char sz_case[64];
            snprintf(sz_case, sizeof(sz_case), "module_read_unread_%#x", off);
            record_open(sz_case, (double)word);
        }
    }
    for (uint32_t off = 0x110; off < 0x150; off += 4) {
        memcpy(&word, s_block + off, 4);
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "module_read_tail_%#x", off);
        record_open(sz_case, (double)word);
    }
    destroy_graph(&graph);
}

int main(void) {
    int rc = sceSysmoduleLoadModule(SYSMODULE_NGS2);
    if (rc != 0) {
        printf("ngs2_reverb: loading libSceNgs2 failed: %#x\n", (unsigned)rc);
        return 1;
    }
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_DIR, 0777);
    s_p_results = fopen(SZ_OUT_DIR "/results.csv", "w");
    if (!s_p_results) {
        printf("ngs2_reverb: cannot write %s/results.csv (errno %d)\n", SZ_OUT_DIR, errno);
        return 1;
    }
    fprintf(s_p_results, "case,value,expected,match\n");
    test_rack_options();
    test_parameters();
    test_impulse_responses();
    test_explicit_tap();
    test_module_read();
    test_explicit_after_reset();
    test_explicit_partial_mask();
    fclose(s_p_results);
    printf("ngs2_reverb: done, %d mismatch(es) against the values the spec predicts\n", s_c_mismatches);
    return s_c_mismatches != 0;
}
