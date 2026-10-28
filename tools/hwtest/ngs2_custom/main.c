/* Hardware test for libSceNgs2's custom racks (kb #1098, phase F6 of #625): the module list and module option
 * checks of the custom racks 0x4001 to 0x4003, sceNgs2CustomRackGetModuleInfo, the module parameters (ids,
 * sizes, ranges, read-back, 0xc001), and the rendered PCM of chains of filter, gain, compressor, LFE, peak
 * meter, reverb and the game's own UserFx and UserFx2 handlers, so that a PS4, psport with the firmware module
 * (PS_LLE=libSceNgs2) and psport's native Ngs2 (PS_NGS2=native) can be compared sample by sample. The checks,
 * with what rea-ngs2 spec 02, 03, 04, 06, 07 and 09 predict, go to /data/psport-hwtest/ngs2_custom/results.csv,
 * and each chain's PCM to <case>.csv. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_DIR "/data/psport-hwtest/ngs2_custom"
#define SYSMODULE_NGS2 0x000B
#define GRAIN 256
#define RATE_HZ 48000
#define GRAINS 16
#define MAX_CHANNELS 8

#define RACK_SAMPLER 0x1000
#define RACK_REVERB 0x2001
#define RACK_MASTERING 0x3000
#define RACK_CUSTOM_SAMPLER 0x4001
#define RACK_CUSTOM_SUBMIXER 0x4002
#define RACK_CUSTOM_MASTERING 0x4003

#define E(low) (0x804A0000u | (low))
/* A custom module's parameter id (spec 03 §5.5): 0x40000000 | read 0x8000 | moduleId << 8 | sub << 5 | index. */
#define MODULE_ID(module_id, index, sub, is_read) \
    (0x40000000u | ((is_read) ? 0x8000u : 0u) | ((uint32_t)(module_id) << 8) | ((uint32_t)(sub) << 5) | (uint32_t)(index))

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
    const void *p_option;
    uint32_t module_id, source_buffer_id, extra_buffer_id, dest_buffer_id, off_state, cb_state, reserved[2];
} Ngs2CustomModule;

typedef struct {
    Ngs2RackOption rack;
    uint32_t cb_state, c_buffers, c_modules, reserved;
    Ngs2CustomModule modules[24];
    struct {
        uint32_t source_buffer_id, reserved;
    } ports[16];
} Ngs2CustomRackOption;

typedef struct {
    Ngs2CustomRackOption custom;
    uint32_t max_channels, max_inputs;
} Ngs2CustomMixerRackOption;

typedef struct {
    Ngs2CustomRackOption custom;
    uint32_t max_channel_works, max_waveform_blocks, max_atrac9_decoders, max_atrac9_channel_works, max_ajm_atrac9_decoders,
        max_codec_caches;
} Ngs2CustomSamplerRackOption;

typedef struct {
    uint32_t cb_option, value, reserved;
} Ngs2ModuleOption12;

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
} Ngs2SubmixerSetupParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t flags;
    float threshold, ratio, knee, attack_s, release_s, level;
    uint32_t reserved;
} Ngs2CompressorParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t type, channel_mask;
    float f[5];
    uint32_t reserved;
} Ngs2FilterParam;

typedef struct {
    Ngs2ParamHeader header;
    float levels[MAX_CHANNELS];
} Ngs2GainParam;

typedef struct {
    float **pp_channels;
    uintptr_t user_data[3];
    uint32_t flags, c_channels, c_grain_samples, rate_hz;
} Ngs2UserFxContext;

typedef struct {
    Ngs2ParamHeader header;
    int32_t (*pfn_handler)(Ngs2UserFxContext *);
    uintptr_t user_data[3];
} Ngs2UserFxParam;

typedef struct {
    void *p_common, *p_param, *p_work;
    uintptr_t user_data;
    uint32_t max_voices, i_voice;
    uint64_t reserved[4];
} Ngs2UserFx2SetupContext;

typedef struct {
    const void *p_data;
    size_t cb_data;
    void *p_common, *p_param;
    uintptr_t user_data;
    uint64_t reserved[4];
} Ngs2UserFx2ControlContext;

typedef struct {
    float **pp_channels;
    void *p_common;
    const void *p_param;
    void *p_work, *p_state;
    uintptr_t user_data;
    uint32_t flags, c_in_channels, c_out_channels, c_grain_samples, rate_hz, reserved;
    uint64_t reserved2[4];
} Ngs2UserFx2ProcessContext;

typedef struct {
    uint32_t cb_option;
    int32_t (*pfn_setup)(Ngs2UserFx2SetupContext *);
    int32_t (*pfn_cleanup)(Ngs2UserFx2SetupContext *);
    int32_t (*pfn_control)(Ngs2UserFx2ControlContext *);
    int32_t (*pfn_process)(Ngs2UserFx2ProcessContext *);
    size_t cb_common, cb_param, cb_work;
    uintptr_t user_data;
} Ngs2UserFx2Option;

typedef struct {
    Ngs2ParamHeader header;
    const void *p_data;
    size_t cb_data;
} Ngs2UserFx2Param;

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

typedef struct {
    Ngs2ParamHeader header;
    uint32_t i_module, reserved;
    void *p_dst;
    uint32_t cb_dst, reserved2;
} Ngs2ModuleReadParam;

typedef struct {
    uint32_t module_id, source_buffer_id, extra_buffer_id, dest_buffer_id, off_state, cb_state, reserved[2];
} Ngs2ModuleInfo;

typedef struct {
    void *p_buffer;
    size_t cb_buffer;
    uint32_t waveform_type, c_channels;
} Ngs2RenderBufferInfo;

_Static_assert(sizeof(Ngs2CustomRackOption) == 0x4d0, "custom rack option");
_Static_assert(offsetof(Ngs2CustomRackOption, modules) == 0x90, "module list");
_Static_assert(sizeof(Ngs2CustomSamplerRackOption) == 0x4e8, "custom sampler option");
_Static_assert(sizeof(Ngs2CustomMixerRackOption) == 0x4d8, "custom mixer option");
_Static_assert(sizeof(Ngs2CompressorParam) == 0x28 && sizeof(Ngs2FilterParam) == 0x28, "compressor, filter");
_Static_assert(sizeof(Ngs2GainParam) == 0x28 && sizeof(Ngs2UserFxParam) == 0x28, "gain, UserFx");
_Static_assert(sizeof(Ngs2UserFx2Option) == 0x48 && sizeof(Ngs2UserFx2Param) == 0x18, "UserFx2");
_Static_assert(sizeof(Ngs2I3dl2Param) == 0x60 && sizeof(Ngs2ModuleReadParam) == 0x20, "reverb, module read");

int sceSysmoduleLoadModule(uint16_t id);
int sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption *p_option, const Ngs2BufferAllocator *p_allocator, Ngs2Handle *p_out_handle);
int sceNgs2SystemDestroy(Ngs2Handle h_system, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2SystemRender(Ngs2Handle h_system, const Ngs2RenderBufferInfo *p_buffers, uint32_t c_buffers);
int sceNgs2RackQueryBufferSize(uint32_t rack_id, const void *p_option, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2RackCreateWithAllocator(Ngs2Handle h_system, uint32_t rack_id, const void *p_option, const Ngs2BufferAllocator *p_allocator,
                                   Ngs2Handle *p_out_handle);
int sceNgs2RackDestroy(Ngs2Handle h_rack, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2RackGetVoiceHandle(Ngs2Handle h_rack, uint32_t i_voice, Ngs2Handle *p_out_handle);
int sceNgs2CustomRackGetModuleInfo(Ngs2Handle h_rack, uint32_t i_module, void *p_out_info, size_t cb_info);
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

static uint32_t control(Ngs2Handle h_voice, void *p_param, uint16_t cb_param, uint32_t id) {
    header((Ngs2ParamHeader *)p_param, cb_param, id);
    return (uint32_t)sceNgs2VoiceControl(h_voice, (const Ngs2ParamHeader *)p_param);
}

static uint32_t event(Ngs2Handle h_voice, uint32_t event_id) {
    Ngs2WordParam param = {{0}, event_id, 0};
    return control(h_voice, &param, 0xc, 6);
}

static uint32_t patch(Ngs2Handle h_voice, Ngs2Handle h_dest, uint32_t i_input) {
    Ngs2PatchParam param = {{0}, 0, i_input, h_dest};
    return control(h_voice, &param, sizeof(param), 5);
}

static void custom_option(Ngs2CustomRackOption *p_option, size_t cb_option, uint32_t max_voices, uint32_t c_buffers, uint32_t cb_state) {
    p_option->rack.cb_option = cb_option;
    p_option->rack.max_grain_samples = 512;
    p_option->rack.max_voices = max_voices;
    p_option->cb_state = cb_state;
    p_option->c_buffers = c_buffers;
}

static void add_module(Ngs2CustomRackOption *p_option, uint32_t module_id, uint32_t source, uint32_t dest, uint32_t off_state,
                       uint32_t cb_state, const void *p_module_option) {
    Ngs2CustomModule *p_module = &p_option->modules[p_option->c_modules++];
    p_module->p_option = p_module_option;
    p_module->module_id = module_id;
    p_module->source_buffer_id = source;
    p_module->dest_buffer_id = dest;
    p_module->off_state = off_state;
    p_module->cb_state = cb_state;
}

static Ngs2CustomMixerRackOption mastering_option(void) {
    Ngs2CustomMixerRackOption option;
    memset(&option, 0, sizeof(option));
    custom_option(&option.custom, sizeof(option), 1, 2, 0x40);
    option.custom.rack.max_input_delay_blocks = 1;
    option.max_channels = 8, option.max_inputs = 1;
    return option;
}

static uint32_t query(uint32_t rack_id, const void *p_option) {
    Ngs2ContextBufferInfo info;
    memset(&info, 0, sizeof(info));
    return (uint32_t)sceNgs2RackQueryBufferSize(rack_id, p_option, &info);
}

/* ------------------------------------------------------------------ options and module list */

static void test_options(void) {
    Ngs2CustomMixerRackOption base = mastering_option();
    add_module(&base.custom, 0x15, 0, 0, 0, 0, NULL);
    Ngs2CustomMixerRackOption option = base;
    record("option_valid", query(RACK_CUSTOM_MASTERING, &option), 0);
    option.custom.cb_state = 3;
    record("option_state_size_3", query(RACK_CUSTOM_MASTERING, &option), E(0xA06));
    option = base, option.custom.c_buffers = 9;
    record("option_buffers_9", query(RACK_CUSTOM_MASTERING, &option), E(0xA02));
    option = base, option.custom.c_modules = 25;
    record("option_modules_25", query(RACK_CUSTOM_MASTERING, &option), E(0xA01));
    option = base, option.custom.modules[0].module_id = 0x21;
    record("option_module_id_0x21", query(RACK_CUSTOM_MASTERING, &option), E(0xA00));
    option = base, option.custom.modules[0].module_id = 0x80;
    record("option_module_id_0x80", query(RACK_CUSTOM_MASTERING, &option), E(0xA00));
    option = base, option.custom.modules[0].module_id = 0x70;
    record("option_module_id_0x70", query(RACK_CUSTOM_MASTERING, &option), 0);
    option = base, option.custom.modules[0].dest_buffer_id = 2;
    record("option_dest_buffer_2", query(RACK_CUSTOM_MASTERING, &option), E(0xA03));
    option = base, option.custom.modules[0].extra_buffer_id = 2;
    record("option_extra_buffer_2", query(RACK_CUSTOM_MASTERING, &option), E(0xA03));
    option = base, option.custom.modules[0].off_state = 0xb;
    record("option_state_offset_below_fixed", query(RACK_CUSTOM_MASTERING, &option), E(0xA04));
    option = base, option.custom.modules[0].off_state = 0x41;
    record("option_state_offset_past_state", query(RACK_CUSTOM_MASTERING, &option), E(0xA04));
    option = base, option.custom.modules[0].off_state = 0x20, option.custom.modules[0].cb_state = 0x21;
    record("option_state_size_past_state", query(RACK_CUSTOM_MASTERING, &option), E(0xA05));
    Ngs2CustomSamplerRackOption sampler;
    memset(&sampler, 0, sizeof(sampler));
    custom_option(&sampler.custom, sizeof(sampler), 2, 1, 0x40);
    sampler.custom.rack.max_matrices = 1, sampler.custom.rack.max_ports = 1;
    sampler.max_channel_works = 16, sampler.max_waveform_blocks = 4;
    add_module(&sampler.custom, 0x15, 0, 0, 0x2c, 4, NULL);
    record("option_sampler_state_below_0x30", query(RACK_CUSTOM_SAMPLER, &sampler), E(0xA04));
    sampler.custom.modules[0].off_state = 0x30;
    record("option_sampler_state_0x30", query(RACK_CUSTOM_SAMPLER, &sampler), 0);
    sampler.custom.ports[0].source_buffer_id = 1;
    record("option_port_buffer_past", query(RACK_CUSTOM_SAMPLER, &sampler), E(0xA00));
    /* Module options (spec 03 §6, 07 §1). Unconfirmed: the envelope's and peak meter's option sizes (0xc,
     * shadPS4's), the codes of the envelope's and the chorus' maxima (shadPS4's names), and that options are
     * checked after the rack's common fields. */
    Ngs2ModuleOption12 meter = {0xc, 0, 0};
    option = mastering_option();
    add_module(&option.custom, 0x18, 0, 0, 0, 0, &meter);
    record("option_meter_blocks_0", query(RACK_CUSTOM_MASTERING, &option), E(0x156));
    meter.value = 257;
    record("option_meter_blocks_257", query(RACK_CUSTOM_MASTERING, &option), E(0x156));
    meter.value = 256;
    record("option_meter_blocks_256", query(RACK_CUSTOM_MASTERING, &option), 0);
    meter.cb_option = 8;
    record("option_meter_size_8", query(RACK_CUSTOM_MASTERING, &option), E(0x81));
    meter.cb_option = 0xc, meter.value = 0, option.custom.rack.max_grain_samples = 65;
    record("option_order_common_first", query(RACK_CUSTOM_MASTERING, &option), E(0x50));
    Ngs2ModuleOption12 reverb = {0x10, 1, 0};
    option = mastering_option();
    add_module(&option.custom, 0x1b, 0, 0, 0, 0, &reverb);
    record("option_reverb_size_0x10", query(RACK_CUSTOM_MASTERING, &option), E(0x81));
    Ngs2ModuleOption12 envelope = {0xc, 33, 0};
    option = mastering_option();
    add_module(&option.custom, 0x10, 0, 0, 0, 0, &envelope);
    record("option_envelope_points_33", query(RACK_CUSTOM_MASTERING, &option), E(0x107));
    Ngs2ModuleOption12 chorus = {0xc, 5, 0};
    option = mastering_option();
    add_module(&option.custom, 0x1c, 0, 0, 0, 0, &chorus);
    record("option_chorus_phases_5", query(RACK_CUSTOM_MASTERING, &option), E(0x520));
    struct {
        uint32_t cb_option, type, max_taps;
        float max_length_ms;
        uint32_t reserved;
    } delay = {0x14, 2, 8, 100, 0};
    option = mastering_option();
    add_module(&option.custom, 0x1d, 0, 0, 0, 0, &delay);
    record("option_delay_type_2", query(RACK_CUSTOM_MASTERING, &option), E(0x532));
    delay.type = 1, delay.max_taps = 9;
    record("option_delay_taps_9", query(RACK_CUSTOM_MASTERING, &option), E(0x531));
    delay.max_taps = 8, delay.max_length_ms = 0.5f;
    record("option_delay_length_0_5", query(RACK_CUSTOM_MASTERING, &option), E(0x530));
    uint32_t pitch[2] = {8, 1};
    option = mastering_option();
    add_module(&option.custom, 0x20, 0, 0, 0, 0, pitch);
    record("option_pitch_quality_1", query(RACK_CUSTOM_MASTERING, &option), E(0x540));
    Ngs2UserFx2Option fx2;
    memset(&fx2, 0, sizeof(fx2));
    fx2.cb_option = 0x40;
    option = mastering_option();
    add_module(&option.custom, 0x1f, 0, 0, 0, 0, &fx2);
    record("option_user_fx2_size_0x40", query(RACK_CUSTOM_MASTERING, &option), E(0x81));
    /* Open: the sizes themselves (psport's layout is not the firmware's). */
    Ngs2ContextBufferInfo info;
    option = mastering_option();
    sceNgs2RackQueryBufferSize(RACK_CUSTOM_MASTERING, &option, &info);
    record_open("option_buffer_size_no_modules", (double)info.cb_host_buffer);
    add_module(&option.custom, 0x1b, 0, 0, 0, 0, NULL);
    sceNgs2RackQueryBufferSize(RACK_CUSTOM_MASTERING, &option, &info);
    record_open("option_buffer_size_reverb", (double)info.cb_host_buffer);
}

/* ------------------------------------------------------------------ chains */

typedef struct {
    Ngs2Handle h_system, h_sampler, h_custom, h_voice, h_custom_voice;
} Chain;

static float s_input[(GRAINS * GRAIN + 64) * 6];
static float s_pcm[GRAINS * GRAIN * 6];
static float s_pass[GRAINS * GRAIN * 6];

/* A sum of two sines and a slow ramp of amplitude per channel, the same on every run. */
static void fill_input(uint32_t c_channels) {
    for (uint32_t i = 0; i < GRAINS * GRAIN + 64; i++) {
        for (uint32_t c = 0; c < c_channels; c++) {
            const double t = (double)i / RATE_HZ;
            s_input[i * c_channels + c] = (float)((0.1 + 0.6 * i / (GRAINS * GRAIN)) *
                                                  (0.6 * sin(2 * M_PI * (110.0 * (c + 1)) * t) + 0.4 * sin(2 * M_PI * 3500.0 * t)));
        }
    }
}

/* sampler (F32, c channels) -> custom mastering (the option's modules) -> F32 buffer of c channels. */
static Chain create_chain(const Ngs2CustomMixerRackOption *p_option, uint32_t c_channels) {
    Chain chain;
    memset(&chain, 0, sizeof(chain));
    Ngs2SystemOption system = {.cb_option = sizeof(system), .max_grain_samples = 1024, .num_grain_samples = GRAIN, .rate_hz = RATE_HZ};
    sceNgs2SystemCreateWithAllocator(&system, &s_allocator, &chain.h_system);
    sceNgs2RackCreateWithAllocator(chain.h_system, RACK_SAMPLER, NULL, &s_allocator, &chain.h_sampler);
    record("chain_custom_rack", (uint32_t)sceNgs2RackCreateWithAllocator(chain.h_system, RACK_CUSTOM_MASTERING, p_option, &s_allocator,
                                                                         &chain.h_custom),
           0);
    sceNgs2RackGetVoiceHandle(chain.h_sampler, 0, &chain.h_voice);
    sceNgs2RackGetVoiceHandle(chain.h_custom, 0, &chain.h_custom_voice);
    Ngs2WordParam setup = {{0}, c_channels, 0};
    record("chain_custom_setup", control(chain.h_custom_voice, &setup, 0x10, 0x40030000), 0);
    Ngs2SamplerSetupParam sampler = {{0}, 0x18, c_channels, RATE_HZ, 0, 0, 0, 0, 0};
    control(chain.h_voice, &sampler, sizeof(sampler), 0x10000000);
    Ngs2WaveformBlock block = {0, GRAINS * GRAIN * c_channels * 4, 0, 0, GRAINS * GRAIN, 0, 0};
    Ngs2BlocksParam blocks = {{0}, s_input, 0, 1, &block};
    control(chain.h_voice, &blocks, sizeof(blocks), 0x10000001);
    patch(chain.h_voice, chain.h_custom_voice, 0);
    return chain;
}

static void render_chain(const Chain *p_chain, const char *sz_case, uint32_t c_channels, float *p_out) {
    event(p_chain->h_custom_voice, 0);
    event(p_chain->h_voice, 0);
    char sz_path[128];
    snprintf(sz_path, sizeof(sz_path), SZ_OUT_DIR "/%s.csv", sz_case);
    FILE *p_csv = fopen(sz_path, "w");
    for (uint32_t i_grain = 0; i_grain < GRAINS; i_grain++) {
        float *p_grain = p_out + (size_t)i_grain * GRAIN * c_channels;
        Ngs2RenderBufferInfo buffer = {p_grain, GRAIN * c_channels * sizeof(float), 0x18, c_channels};
        sceNgs2SystemRender(p_chain->h_system, &buffer, 1);
        for (uint32_t i = 0; p_csv && i < GRAIN; i++) {
            fprintf(p_csv, "%u", i_grain * GRAIN + i);
            for (uint32_t c = 0; c < c_channels; c++) fprintf(p_csv, ",%.9g", p_grain[i * c_channels + c]);
            fprintf(p_csv, "\n");
        }
    }
    if (p_csv) fclose(p_csv);
}

static void destroy_chain(const Chain *p_chain) {
    sceNgs2RackDestroy(p_chain->h_sampler, NULL);
    sceNgs2RackDestroy(p_chain->h_custom, NULL);
    sceNgs2SystemDestroy(p_chain->h_system, NULL);
}

/* The modules without parameters pass the input through: what the chain renders then is the reference x
 * the other cases are compared with. */
static void render_pass(const Ngs2CustomMixerRackOption *p_option, uint32_t c_channels, const char *sz_case) {
    Chain chain = create_chain(p_option, c_channels);
    render_chain(&chain, sz_case, c_channels, s_pass);
    destroy_chain(&chain);
}

static double max_error(uint32_t c_samples, double (*pfn_expected)(uint32_t i, float x)) {
    double worst = 0;
    for (uint32_t i = 0; i < c_samples; i++) {
        const double error = fabs(s_pcm[i] - pfn_expected(i, s_pass[i]));
        if (error > worst) worst = error;
    }
    return worst;
}

static double half(uint32_t i, float x) {
    (void)i;
    return 0.5 * x;
}

static double gain_two_channels(uint32_t i, float x) {
    return (i % 2 ? -2.0 : 0.5) * x;
}

static void test_gain(void) {
    fill_input(2);
    Ngs2CustomMixerRackOption option = mastering_option();
    add_module(&option.custom, 0x15, 0, 0, 0, 0, NULL);
    render_pass(&option, 2, "gain_pass");
    Chain chain = create_chain(&option, 2);
    Ngs2GainParam gain = {{0}, {0.5f, -2.0f, 1, 1, 1, 1, 1, 1}};
    record("gain_set", control(chain.h_custom_voice, &gain, sizeof(gain), MODULE_ID(0x15, 0, 0, 0)), 0);
    render_chain(&chain, "gain", 2, s_pcm);
    record_near("gain_error", max_error(GRAINS * GRAIN * 2, gain_two_channels), 0, 1e-6);
    destroy_chain(&chain);
    /* Routing: gain 0 -> 1, a module without a parameter copies 1 -> 0 (the output reads buffer 0). */
    option = mastering_option();
    add_module(&option.custom, 0x15, 0, 1, 0, 0, NULL);
    add_module(&option.custom, 0x15, 1, 0, 0, 0, NULL);
    chain = create_chain(&option, 2);
    control(chain.h_custom_voice, &gain, sizeof(gain), MODULE_ID(0x15, 0, 0, 0));
    render_chain(&chain, "gain_routed", 2, s_pcm);
    record_near("gain_routed_error", max_error(GRAINS * GRAIN * 2, gain_two_channels), 0, 1e-6);
    destroy_chain(&chain);
}

/* The filter, gain and UserFx chain of the plan's F6 row: low-pass 1 kHz, gain 0.5, a UserFx that halves the
 * grain and records its context. */
static uint32_t s_c_user_fx_calls;
static Ngs2UserFxContext s_user_fx_first, s_user_fx_second;

static int32_t user_fx(Ngs2UserFxContext *p_context) {
    if (s_c_user_fx_calls == 0) s_user_fx_first = *p_context;
    if (s_c_user_fx_calls == 1) s_user_fx_second = *p_context;
    s_c_user_fx_calls++;
    for (uint32_t c = 0; c < p_context->c_channels; c++) {
        for (uint32_t i = 0; i < p_context->c_grain_samples; i++) p_context->pp_channels[c][i] *= 0.5f;
    }
    return p_context->user_data[1] ? -1 : 0;
}

static void test_filter_gain_user_fx(void) {
    fill_input(2);
    Ngs2CustomMixerRackOption option = mastering_option();
    add_module(&option.custom, 0x13, 0, 0, 0, 0, NULL);
    add_module(&option.custom, 0x15, 0, 0, 0, 0, NULL);
    add_module(&option.custom, 0x19, 0, 0, 0, 0, NULL);
    render_pass(&option, 2, "chain_pass");
    Chain chain = create_chain(&option, 2);
    Ngs2FilterParam filter = {{0}, 1, 0, {1000, 0.707f, 1, 0, 0}, 0};
    record("chain_filter", control(chain.h_custom_voice, &filter, sizeof(filter), MODULE_ID(0x13, 0, 0, 0)), 0);
    Ngs2GainParam gain = {{0}, {0.5f, 0.5f, 1, 1, 1, 1, 1, 1}};
    record("chain_gain", control(chain.h_custom_voice, &gain, sizeof(gain), MODULE_ID(0x15, 1, 0, 0)), 0);
    Ngs2UserFxParam fx = {{0}, user_fx, {0x11, 0, 0x33}};
    record("chain_user_fx", control(chain.h_custom_voice, &fx, sizeof(fx), MODULE_ID(0x19, 2, 0, 0)), 0);
    s_c_user_fx_calls = 0;
    render_chain(&chain, "chain_filter_gain_user_fx", 2, s_pcm);
    record("user_fx_calls", s_c_user_fx_calls, GRAINS);
    record("user_fx_channels", s_user_fx_first.c_channels, 2);
    record("user_fx_grain", s_user_fx_first.c_grain_samples, GRAIN);
    record("user_fx_rate", s_user_fx_first.rate_hz, RATE_HZ);
    record("user_fx_user_data_0", (uint32_t)s_user_fx_first.user_data[0], 0x11);
    record("user_fx_user_data_2", (uint32_t)s_user_fx_first.user_data[2], 0x33);
    /* Unconfirmed: the flags are the module's grain flags, first grain and parameters changed. */
    record("user_fx_flags_first", s_user_fx_first.flags, 3);
    record("user_fx_flags_second", s_user_fx_second.flags, 0);
    /* The low-pass of spec 06 §4 (RBJ, A = sqrt(gain)) then x 0.5 x 0.5. */
    const double w = 2 * M_PI * 1000.0 / RATE_HZ, s = sin(w), c = cos(w), alpha = s / (2 * 0.707);
    const double a0 = 1 + alpha, k[5] = {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, 2 * c / a0, -(1 - alpha) / a0};
    double worst = 0;
    for (uint32_t ch = 0; ch < 2; ch++) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (uint32_t i = 0; i < GRAINS * GRAIN; i++) {
            const double x = s_pass[i * 2 + ch];
            const double y = k[0] * x + k[1] * x1 + k[2] * x2 + k[3] * y1 + k[4] * y2;
            x2 = x1, x1 = x, y2 = y1, y1 = y;
            const double error = fabs(s_pcm[i * 2 + ch] - 0.25 * y);
            if (error > worst) worst = error;
        }
    }
    record_near("chain_error", worst, 0, 2e-5);
    uint32_t flags = 0;
    sceNgs2VoiceGetStateFlags(chain.h_custom_voice, &flags);
    record("chain_flags", flags, 3);
    /* A handler answering < 0: ERROR without PLAYING (spec 04 §2.2, for the sampler's UserFx). */
    fx.user_data[1] = 1;
    destroy_chain(&chain);
    chain = create_chain(&option, 2);
    control(chain.h_custom_voice, &fx, sizeof(fx), MODULE_ID(0x19, 2, 0, 0));
    event(chain.h_custom_voice, 0);
    Ngs2RenderBufferInfo buffer = {s_pcm, GRAIN * 2 * sizeof(float), 0x18, 2};
    sceNgs2SystemRender(chain.h_system, &buffer, 1);
    sceNgs2VoiceGetStateFlags(chain.h_custom_voice, &flags);
    record("user_fx_failure_flags", flags, 0x11);
    destroy_chain(&chain);
}

static void test_compressor_and_meter(void) {
    fill_input(2);
    Ngs2ModuleOption12 meter_option = {0xc, 4, 0};
    Ngs2CustomMixerRackOption option = mastering_option();
    add_module(&option.custom, 0x12, 0, 0, 0x10, 8, NULL);
    add_module(&option.custom, 0x18, 0, 0, 0x18, 0x28, &meter_option);
    render_pass(&option, 2, "compressor_pass");
    Chain chain = create_chain(&option, 2);
    Ngs2CompressorParam compressor = {{0}, 0, 0.2f, 0.25f, 0, 0.001f, 0.05f, 1, 0};
    record("compressor_set", control(chain.h_custom_voice, &compressor, sizeof(compressor), MODULE_ID(0x12, 0, 0, 0)), 0);
    Ngs2WordParam meter = {{0}, 1, 0};
    record("meter_set", control(chain.h_custom_voice, &meter, 0x10, MODULE_ID(0x18, 1, 0, 0)), 0);
    render_chain(&chain, "compressor", 2, s_pcm);
    /* The voice state: flags, then the compressor's {level, gain} at 0x10 and the meter's {peak, channels} at
     * 0x18 (spec 03 §3, 09 §3.3, 06 §9). The values are open; the PCM files hold what they measure. */
    float state[0x40 / 4];
    memset(state, 0, sizeof(state));
    record("state_read", (uint32_t)sceNgs2VoiceGetState(chain.h_custom_voice, state, sizeof(state)), 0);
    record_open("state_compressor_level", state[4]);
    record_open("state_compressor_gain", state[5]);
    record_open("state_meter_peak", state[6]);
    record_open("state_meter_left", state[7]);
    record_open("state_meter_right", state[8]);
    record("state_meter_channel_3", *(uint32_t *)&state[6 + 1 + 3], 0);
    destroy_chain(&chain);
}

static void test_lfe(void) {
    fill_input(6);
    for (uint32_t i_case = 0; i_case < 2; i_case++) {
        Ngs2CustomMixerRackOption option = mastering_option();
        if (i_case == 0) {
            add_module(&option.custom, 0x14, 0, 0, 0, 0, NULL);
        } else {
            /* Source != dest: the second stage reads the source again (spec 06 §5), unconfirmed in effect. */
            add_module(&option.custom, 0x14, 0, 1, 0, 0, NULL);
            add_module(&option.custom, 0x15, 1, 0, 0, 0, NULL);
        }
        render_pass(&option, 6, i_case ? "lfe_routed_pass" : "lfe_pass");
        Chain chain = create_chain(&option, 6);
        Ngs2WordParam lfe = {{0}, 1, 120};
        record("lfe_set", control(chain.h_custom_voice, &lfe, 0x10, MODULE_ID(0x14, 0, 0, 0)), 0);
        render_chain(&chain, i_case ? "lfe_routed" : "lfe", 6, s_pcm);
        destroy_chain(&chain);
    }
}

/* Module 0x1b with the option's word at 4 = 1 against the reverb rack 0x2001 (reverbSize 1): the same
 * network, so the same PCM. */
static void test_reverb(void) {
    fill_input(2);
    const Ngs2I3dl2 room = {0.8f, 0.3f, -1000, -100, 0, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100, 100, 5000, {0}};
    Ngs2ModuleOption12 reverb_option = {0xc, 1, 0};
    Ngs2CustomMixerRackOption option = mastering_option();
    add_module(&option.custom, 0x1b, 0, 0, 0, 0, &reverb_option);
    Chain chain = create_chain(&option, 2);
    Ngs2I3dl2Param param = {{0}, room};
    record("reverb_set", control(chain.h_custom_voice, &param, sizeof(param), MODULE_ID(0x1b, 0, 0, 0)), 0);
    render_chain(&chain, "reverb_module", 2, s_pcm);
    Ngs2I3dl2Param read;
    memset(&read, 0, sizeof(read));
    record("reverb_read", control(chain.h_custom_voice, &read, sizeof(read), MODULE_ID(0x1b, 0, 0, 1)), 0);
    record("reverb_read_room", (uint32_t)read.i3dl2.room, (uint32_t)-1000);
    destroy_chain(&chain);
    /* The rack: sampler -> reverb (2 in, 2 out) -> mastering (2). */
    Ngs2Handle h_system, h_sampler, h_reverb, h_master, h_voice, h_reverb_voice, h_master_voice;
    Ngs2SystemOption system = {.cb_option = sizeof(system), .max_grain_samples = 1024, .num_grain_samples = GRAIN, .rate_hz = RATE_HZ};
    sceNgs2SystemCreateWithAllocator(&system, &s_allocator, &h_system);
    sceNgs2RackCreateWithAllocator(h_system, RACK_SAMPLER, NULL, &s_allocator, &h_sampler);
    sceNgs2RackCreateWithAllocator(h_system, RACK_REVERB, NULL, &s_allocator, &h_reverb);
    sceNgs2RackCreateWithAllocator(h_system, RACK_MASTERING, NULL, &s_allocator, &h_master);
    sceNgs2RackGetVoiceHandle(h_sampler, 0, &h_voice);
    sceNgs2RackGetVoiceHandle(h_reverb, 0, &h_reverb_voice);
    sceNgs2RackGetVoiceHandle(h_master, 0, &h_master_voice);
    Ngs2WordParam master = {{0}, 2, 0};
    control(h_master_voice, &master, 0x10, 0x30000000);
    Ngs2WordParam reverb_setup[2] = {{{0}, 2, 2}};
    control(h_reverb_voice, reverb_setup, 0x18, 0x20010000);
    Ngs2I3dl2Param rack_param = {{0}, room};
    control(h_reverb_voice, &rack_param, sizeof(rack_param), 0x20010001);
    Ngs2SamplerSetupParam sampler = {{0}, 0x18, 2, RATE_HZ, 0, 0, 0, 0, 0};
    control(h_voice, &sampler, sizeof(sampler), 0x10000000);
    Ngs2WaveformBlock block = {0, GRAINS * GRAIN * 2 * 4, 0, 0, GRAINS * GRAIN, 0, 0};
    Ngs2BlocksParam blocks = {{0}, s_input, 0, 1, &block};
    control(h_voice, &blocks, sizeof(blocks), 0x10000001);
    patch(h_voice, h_reverb_voice, 0);
    patch(h_reverb_voice, h_master_voice, 0);
    event(h_master_voice, 0);
    event(h_reverb_voice, 0);
    event(h_voice, 0);
    double worst = 0;
    for (uint32_t i_grain = 0; i_grain < GRAINS; i_grain++) {
        float grain[GRAIN * 2];
        Ngs2RenderBufferInfo buffer = {grain, sizeof(grain), 0x18, 2};
        sceNgs2SystemRender(h_system, &buffer, 1);
        for (uint32_t i = 0; i < GRAIN * 2; i++) {
            const double error = fabs(grain[i] - s_pcm[i_grain * GRAIN * 2 + i]);
            if (error > worst) worst = error;
        }
    }
    record_near("reverb_module_vs_rack", worst, 0, 1e-6);
    sceNgs2RackDestroy(h_sampler, NULL);
    sceNgs2RackDestroy(h_reverb, NULL);
    sceNgs2RackDestroy(h_master, NULL);
    sceNgs2SystemDestroy(h_system, NULL);
}

/* UserFx2 (spec 03 §6.9): unconfirmed, when its setup and cleanup run (psport: once per voice at the rack's
 * creation and destruction) and what its process context carries. */
static uint32_t s_c_setups, s_c_cleanups, s_c_controls, s_c_processes;
static Ngs2UserFx2ProcessContext s_process_first;

static int32_t fx2_setup(Ngs2UserFx2SetupContext *p_context) {
    s_c_setups++;
    *(float *)p_context->p_param = 1.0f;
    return 0;
}

static int32_t fx2_cleanup(Ngs2UserFx2SetupContext *p_context) {
    (void)p_context;
    s_c_cleanups++;
    return 0;
}

static int32_t fx2_control(Ngs2UserFx2ControlContext *p_context) {
    s_c_controls++;
    if (p_context->cb_data != 4) return (int32_t)E(0x209);
    memcpy(p_context->p_param, p_context->p_data, 4);
    return 0;
}

static int32_t fx2_process(Ngs2UserFx2ProcessContext *p_context) {
    if (s_c_processes++ == 0) s_process_first = *p_context;
    const float factor = *(const float *)p_context->p_param;
    for (uint32_t c = 0; c < p_context->c_out_channels; c++) {
        for (uint32_t i = 0; i < p_context->c_grain_samples; i++) p_context->pp_channels[c][i] *= factor;
    }
    return 0;
}

static void test_user_fx2(void) {
    fill_input(2);
    Ngs2UserFx2Option fx2 = {0x48, fx2_setup, fx2_cleanup, fx2_control, fx2_process, 16, 4, 64, 0x55};
    Ngs2CustomMixerRackOption option = mastering_option();
    add_module(&option.custom, 0x1f, 0, 0, 0x10, 4, &fx2);
    s_c_setups = s_c_cleanups = s_c_controls = s_c_processes = 0;
    Chain chain = create_chain(&option, 2);
    record("user_fx2_setups_at_create", s_c_setups, 1);
    Ngs2UserFx2Param param = {{0}, NULL, 4};
    record("user_fx2_null_data", control(chain.h_custom_voice, &param, sizeof(param), MODULE_ID(0x1f, 0, 0, 0)), E(0x207));
    static const float s_factor = 0.25f;
    param.p_data = &s_factor;
    record("user_fx2_control", control(chain.h_custom_voice, &param, sizeof(param), MODULE_ID(0x1f, 0, 0, 0)), 0);
    record("user_fx2_controls", s_c_controls, 1);
    render_chain(&chain, "user_fx2", 2, s_pcm);
    record("user_fx2_processes", s_c_processes, GRAINS);
    record("user_fx2_process_in", s_process_first.c_in_channels, 2);
    record("user_fx2_process_out", s_process_first.c_out_channels, 2);
    record("user_fx2_process_user_data", (uint32_t)s_process_first.user_data, 0x55);
    record("user_fx2_process_flags", s_process_first.flags, 3);
    record("user_fx2_process_state", s_process_first.p_state != NULL, 1);
    destroy_chain(&chain);
    record("user_fx2_cleanups_at_destroy", s_c_cleanups, 1);
    /* Without a control handler the data must be exactly paramSize (0x804A0209). */
    fx2.pfn_control = NULL, fx2.pfn_setup = NULL, fx2.pfn_cleanup = NULL;
    chain = create_chain(&option, 2);
    param.cb_data = 3;
    record("user_fx2_copy_size_3", control(chain.h_custom_voice, &param, sizeof(param), MODULE_ID(0x1f, 0, 0, 0)), E(0x209));
    param.cb_data = 4;
    record("user_fx2_copy_size_4", control(chain.h_custom_voice, &param, sizeof(param), MODULE_ID(0x1f, 0, 0, 0)), 0);
    destroy_chain(&chain);
}

/* ------------------------------------------------------------------ parameters, reads, module info */

static void test_parameters(void) {
    fill_input(2);
    Ngs2CustomMixerRackOption option = mastering_option();
    add_module(&option.custom, 0x15, 1, 0, 0xc, 8, NULL);
    add_module(&option.custom, 0x14, 0, 1, 0, 0, NULL);
    add_module(&option.custom, 0x13, 0, 0, 0, 0, NULL);
    Chain chain = create_chain(&option, 2);
    const Ngs2Handle h = chain.h_custom_voice;
    Ngs2ModuleInfo info;
    record("info_size", (uint32_t)sceNgs2CustomRackGetModuleInfo(chain.h_custom, 0, &info, 0x1f), E(0x54));
    record("info_index", (uint32_t)sceNgs2CustomRackGetModuleInfo(chain.h_custom, 3, &info, sizeof(info)), E(0xB00));
    record("info_read", (uint32_t)sceNgs2CustomRackGetModuleInfo(chain.h_custom, 0, &info, sizeof(info)), 0);
    record("info_module", info.module_id, 0x15);
    record("info_source", info.source_buffer_id, 1);
    record("info_state", info.off_state, 0xc);
    Ngs2GainParam gain;
    memset(&gain, 0, sizeof(gain));
    record("param_wrong_module", control(h, &gain, sizeof(gain), MODULE_ID(0x14, 0, 0, 0)), E(0x308));
    record("param_index_past", control(h, &gain, sizeof(gain), MODULE_ID(0x15, 3, 0, 0)), E(0x308));
    record("param_gain_size", control(h, &gain, 0x20, MODULE_ID(0x15, 0, 0, 0)), E(0x30A));
    /* Unconfirmed: unset reads give gain 1.0 and LFE {0, 120} (spec 03 §6.8's defaults). */
    record("param_gain_read_unset", control(h, &gain, sizeof(gain), MODULE_ID(0x15, 0, 0, 1)), 0);
    record_near("param_gain_unset", gain.levels[0], 1, 0);
    Ngs2WordParam lfe = {{0}, 0, 0};
    control(h, &lfe, 0x10, MODULE_ID(0x14, 1, 0, 1));
    record("param_lfe_unset_enable", lfe.value, 0);
    record("param_lfe_unset_fc", lfe.value2, 120);
    lfe.value = 1, lfe.value2 = 100;
    record("param_lfe_fc_100", control(h, &lfe, 0x10, MODULE_ID(0x14, 1, 0, 0)), E(0x603));
    for (uint32_t i = 0; i < MAX_CHANNELS; i++) gain.levels[i] = i == 0 ? 40.0f : -40.0f;
    control(h, &gain, sizeof(gain), MODULE_ID(0x15, 0, 0, 0));
    control(h, &gain, sizeof(gain), MODULE_ID(0x15, 0, 0, 1));
    record_near("param_gain_clamp_high", gain.levels[0], 32, 0);
    record_near("param_gain_clamp_low", gain.levels[1], -32, 0);
    /* Filter sub 0 keeps type and mask as bytes (spec 06 §4). */
    Ngs2FilterParam filter = {{0}, 0x101, 0x1f0, {1000, 0.7f, 1, 0, 0}, 0};
    control(h, &filter, sizeof(filter), MODULE_ID(0x13, 2, 0, 0));
    memset(&filter, 0, sizeof(filter));
    control(h, &filter, sizeof(filter), MODULE_ID(0x13, 2, 0, 1));
    record("param_filter_type_byte", filter.type, 1);
    record("param_filter_mask_byte", filter.channel_mask, 0xf0);
    /* 0xc001: the pending blocks in the firmware's layout (spec 09 §12). */
    uint8_t block[0x40];
    memset(block, 0xcc, sizeof(block));
    Ngs2ModuleReadParam read = {{0}, 0, 0, block, sizeof(block), 0};
    record("module_read_gain", control(h, &read, sizeof(read), 0xc001), 0);
    float level;
    memcpy(&level, block, 4), record_near("module_read_gain_0", level, 32, 0);
    record("module_read_gain_tail", block[0x20], 0);
    memset(block, 0xcc, sizeof(block));
    read.i_module = 1;
    control(h, &read, sizeof(read), 0xc001);
    record("module_read_lfe_unset", block[0], 0);
    memset(block, 0xcc, sizeof(block));
    read.i_module = 2;
    control(h, &read, sizeof(read), 0xc001);
    record("module_read_filter_type", block[1], 1);
    record("module_read_filter_mask", block[2], 0xf0);
    memset(block, 0xcc, sizeof(block));
    read.i_module = 3; /* the output after the list: buffer 0 since the setup */
    control(h, &read, sizeof(read), 0xc001);
    uint32_t word;
    memcpy(&word, block, 4), record("module_read_output", word, 0);
    destroy_chain(&chain);
}

/* The custom sampler: the source into buffer 0, gain 0.5 into buffer 1, port 0 from buffer 1. Unconfirmed:
 * the source's data in the voice state at +0x10 (numDecodedSamples, decodedDataSize, userData, read
 * position), +0x08 zero. */
static void test_custom_sampler(void) {
    fill_input(2);
    Ngs2CustomMixerRackOption pass = mastering_option();
    render_pass(&pass, 2, "custom_sampler_pass");
    Ngs2SystemOption system = {.cb_option = sizeof(system), .max_grain_samples = 1024, .num_grain_samples = GRAIN, .rate_hz = RATE_HZ};
    Ngs2Handle h_system, h_rack, h_master, h_voice, h_master_voice;
    sceNgs2SystemCreateWithAllocator(&system, &s_allocator, &h_system);
    Ngs2CustomSamplerRackOption option;
    memset(&option, 0, sizeof(option));
    custom_option(&option.custom, sizeof(option), 2, 2, 0x40);
    option.custom.rack.max_matrices = 1, option.custom.rack.max_ports = 1;
    option.max_channel_works = 16, option.max_waveform_blocks = 4;
    add_module(&option.custom, 0x15, 0, 1, 0, 0, NULL);
    option.custom.ports[0].source_buffer_id = 1;
    record("custom_sampler_rack", (uint32_t)sceNgs2RackCreateWithAllocator(h_system, RACK_CUSTOM_SAMPLER, &option, &s_allocator, &h_rack), 0);
    Ngs2CustomMixerRackOption master_option = mastering_option();
    sceNgs2RackCreateWithAllocator(h_system, RACK_CUSTOM_MASTERING, &master_option, &s_allocator, &h_master);
    sceNgs2RackGetVoiceHandle(h_rack, 0, &h_voice);
    sceNgs2RackGetVoiceHandle(h_master, 0, &h_master_voice);
    Ngs2WordParam setup = {{0}, 2, 0};
    control(h_master_voice, &setup, 0x10, 0x40030000);
    Ngs2SamplerSetupParam sampler = {{0}, 0x18, 2, RATE_HZ, 0, 0, 0, 0, 0};
    record("custom_sampler_setup", control(h_voice, &sampler, sizeof(sampler), 0x40010000), 0);
    Ngs2WaveformBlock block = {0, GRAINS * GRAIN * 2 * 4, 0, 0, GRAINS * GRAIN, 0, 0x77};
    Ngs2BlocksParam blocks = {{0}, s_input, 0, 1, &block};
    record("custom_sampler_blocks", control(h_voice, &blocks, sizeof(blocks), 0x40010001), 0);
    Ngs2WordParam pitch = {{0}, 0, 0};
    float ratio = 1.0f;
    memcpy(&pitch.value, &ratio, 4);
    record("custom_sampler_pitch", control(h_voice, &pitch, 0x10, 0x40010005), 0);
    record("custom_sampler_pitch_read", control(h_voice, &pitch, 0x10, 0x40018005), E(0x308));
    Ngs2GainParam gain = {{0}, {0.5f, 0.5f, 1, 1, 1, 1, 1, 1}};
    control(h_voice, &gain, sizeof(gain), MODULE_ID(0x15, 0, 0, 0));
    patch(h_voice, h_master_voice, 0);
    event(h_master_voice, 0);
    event(h_voice, 0);
    for (uint32_t i_grain = 0; i_grain < GRAINS; i_grain++) {
        Ngs2RenderBufferInfo buffer = {s_pcm + (size_t)i_grain * GRAIN * 2, GRAIN * 2 * sizeof(float), 0x18, 2};
        sceNgs2SystemRender(h_system, &buffer, 1);
        if (i_grain == 1) {
            uint64_t state[8];
            memset(state, 0xcc, sizeof(state));
            sceNgs2VoiceGetState(h_voice, state, sizeof(state));
            record("custom_sampler_state_flags", (uint32_t)state[0], 3);
            record_open("custom_sampler_state_8", (double)state[1]);
            record_open("custom_sampler_state_decoded", (double)state[2]);
            record_open("custom_sampler_state_bytes", (double)state[3]);
            record("custom_sampler_state_user_data", (uint32_t)state[4], 0x77);
            record_open("custom_sampler_state_read_offset", (double)(state[5] - (uintptr_t)s_input));
        }
    }
    record_near("custom_sampler_error", max_error(GRAINS * GRAIN * 2, half), 0, 1e-6);
    sceNgs2RackDestroy(h_rack, NULL);
    sceNgs2RackDestroy(h_master, NULL);
    sceNgs2SystemDestroy(h_system, NULL);
}

int main(void) {
    int rc = sceSysmoduleLoadModule(SYSMODULE_NGS2);
    if (rc != 0) {
        printf("ngs2_custom: loading libSceNgs2 failed: %#x\n", (unsigned)rc);
        return 1;
    }
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_DIR, 0777);
    s_p_results = fopen(SZ_OUT_DIR "/results.csv", "w");
    if (!s_p_results) {
        printf("ngs2_custom: cannot write %s/results.csv (errno %d)\n", SZ_OUT_DIR, errno);
        return 1;
    }
    fprintf(s_p_results, "case,value,expected,match\n");
    test_options();
    test_parameters();
    test_gain();
    test_filter_gain_user_fx();
    test_compressor_and_meter();
    test_lfe();
    test_reverb();
    test_user_fx2();
    test_custom_sampler();
    fclose(s_p_results);
    printf("ngs2_custom: done, %d mismatch(es) against the values the spec predicts\n", s_c_mismatches);
    return s_c_mismatches != 0;
}
