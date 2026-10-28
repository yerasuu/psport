/* Hardware test for libSceNgs2's system, rack, voice and report API (kb #1094, phase F2 of #625):
 * calls every export psport's native Ngs2 implements with valid and invalid arguments and writes
 * each return code, with the one firmware 11.00 is expected to give (rea-ngs2 spec 01, 02, 03, 08),
 * and the sizes and info fields it reads back, to /data/psport-hwtest/ngs2_api/results.csv. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_DIR "/data/psport-hwtest/ngs2_api"
#define SYSMODULE_NGS2 0x000B
#define NO_EXPECTATION 0xffffffffu

#define RACK_SAMPLER 0x1000
#define RACK_SUBMIXER 0x2000
#define RACK_REVERB 0x2001
#define RACK_MASTERING 0x3000
#define RACK_CUSTOM_SAMPLER 0x4001
#define RACK_CUSTOM_SUBMIXER 0x4002
#define RACK_CUSTOM_MASTERING 0x4003

#define E(low) (0x804A0000u | (low))
#define KERNEL_EPERM 0x80020001u

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
    char sz_name[16];
    Ngs2Handle h_system;
    Ngs2ContextBufferInfo buffer_info;
    uint32_t uid, min_grain_samples, max_grain_samples, state_flags, c_racks;
    float last_render_ratio;
    int64_t last_render_tick, c_renders;
    uint32_t rate_hz, num_grain_samples;
} Ngs2SystemInfo;

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
    Ngs2RackOption rack;
    uint32_t max_channels, reverb_size;
} Ngs2ReverbRackOption;

typedef struct {
    Ngs2RackOption rack;
    uint32_t max_channels, num_peak_meter_blocks;
} Ngs2MasteringRackOption;

typedef struct {
    const void *p_option;
    uint32_t module_id, source_buffer_id, extra_buffer_id, dest_buffer_id, off_state, cb_state, reserved[2];
} Ngs2CustomModule;

typedef struct {
    Ngs2RackOption rack;
    uint32_t cb_state, c_buffers, c_modules, reserved;
    Ngs2CustomModule modules[24];
    uint32_t ports[16][2];
    uint32_t max_channels, max_inputs;
} Ngs2CustomSubmixerRackOption;

typedef struct {
    char sz_name[16];
    Ngs2Handle h_rack;
    Ngs2ContextBufferInfo buffer_info;
    Ngs2Handle h_owner;
    uint32_t rack_class, rack_id, uid, min_grain_samples, max_grain_samples, max_voices, max_channel_works;
    uint32_t max_inputs, max_matrices, max_ports, state_flags;
    float last_process_ratio;
    uint64_t last_process_tick, c_renders;
    uint32_t c_active_voices, c_active_channel_works;
    uint32_t extra[0x500 / 4];
} Ngs2RackInfo;

typedef struct {
    uint32_t module_id, source_buffer_id, extra_buffer_id, dest_buffer_id, off_state, cb_state, reserved[2];
} Ngs2CustomModuleInfo;

typedef struct {
    uint16_t cb_param;
    int16_t off_next;
    uint32_t id;
} Ngs2ParamHeader;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t waveform_type, c_channels, rate_hz, config_data, off_frame, cb_frame_margin, flags, reserved;
} Ngs2SamplerSetupParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t matrix_id, c_levels;
    const float *p_levels;
} Ngs2MatrixParam;

typedef struct {
    Ngs2ParamHeader header;
    uint32_t i_port;
    union {
        float level;
        int32_t matrix_id;
        uint32_t delay_samples;
    };
} Ngs2PortParam;

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
    int32_t matrix_id;
    float volume;
    uint32_t delay_samples, i_dest_input;
    Ngs2Handle h_dest;
} Ngs2PortInfo;

typedef struct {
    uint32_t c_levels;
    float levels[64];
} Ngs2MatrixInfo;

typedef struct {
    void *p_buffer;
    size_t cb_buffer;
    uint32_t waveform_type, c_channels;
} Ngs2RenderBufferInfo;

typedef struct {
    size_t cb_data;
    Ngs2Handle handle;
    uint32_t type;
    int32_t result;
    const char *sz_message;
} Ngs2ReportMessage;

_Static_assert(sizeof(Ngs2SystemOption) == 0x40, "system option");
_Static_assert(sizeof(Ngs2SystemInfo) == 0x88, "system info");
_Static_assert(sizeof(Ngs2RackOption) == 0x80, "rack option");
_Static_assert(sizeof(Ngs2SamplerRackOption) == 0xa8, "sampler option");
_Static_assert(sizeof(Ngs2SubmixerRackOption) == 0x98, "submixer option");
_Static_assert(sizeof(Ngs2ReverbRackOption) == 0x88, "reverb option");
_Static_assert(sizeof(Ngs2MasteringRackOption) == 0x88, "mastering option");
_Static_assert(sizeof(Ngs2CustomSubmixerRackOption) == 0x4d8, "custom submixer option");
_Static_assert(offsetof(Ngs2RackInfo, extra) == 0xa8, "rack info");
_Static_assert(sizeof(Ngs2MatrixInfo) == 0x104, "matrix info");
_Static_assert(sizeof(Ngs2PortInfo) == 0x18, "port info");
_Static_assert(sizeof(Ngs2CallbackParam) == 0x20, "callback param");

typedef void *ScePthread;
int sceSysmoduleLoadModule(uint16_t id);
int scePthreadCreate(ScePthread *p_out_thread, const void *p_attr, void *(*pfn_entry)(void *), void *p_arg, const char *sz_name);
int scePthreadJoin(ScePthread thread, void **pp_out_value);
int sceNgs2SystemQueryBufferSize(const Ngs2SystemOption *p_option, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2SystemCreate(const Ngs2SystemOption *p_option, const Ngs2ContextBufferInfo *p_buffer, Ngs2Handle *p_out_handle);
int sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption *p_option, const Ngs2BufferAllocator *p_allocator, Ngs2Handle *p_out_handle);
int sceNgs2SystemDestroy(Ngs2Handle h_system, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2SystemLock(Ngs2Handle h_system);
int sceNgs2SystemUnlock(Ngs2Handle h_system);
int sceNgs2SystemSetUserData(Ngs2Handle h_system, uintptr_t user_data);
int sceNgs2SystemGetUserData(Ngs2Handle h_system, uintptr_t *p_out_user_data);
int sceNgs2SystemGetInfo(Ngs2Handle h_system, Ngs2SystemInfo *p_out_info, size_t cb_info);
int sceNgs2SystemSetGrainSamples(Ngs2Handle h_system, uint32_t num_grain_samples);
int sceNgs2SystemSetSampleRate(Ngs2Handle h_system, uint32_t rate_hz);
int sceNgs2SystemRender(Ngs2Handle h_system, const Ngs2RenderBufferInfo *p_buffers, uint32_t c_buffers);
int sceNgs2SystemEnumHandles(Ngs2Handle *p_out_handles, uint32_t c_max);
int sceNgs2SystemEnumRackHandles(Ngs2Handle h_system, Ngs2Handle *p_out_handles, uint32_t c_max);
int sceNgs2RackQueryBufferSize(uint32_t rack_id, const void *p_option, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2RackCreate(Ngs2Handle h_system, uint32_t rack_id, const void *p_option, const Ngs2ContextBufferInfo *p_buffer, Ngs2Handle *p_out_handle);
int sceNgs2RackCreateWithAllocator(Ngs2Handle h_system, uint32_t rack_id, const void *p_option, const Ngs2BufferAllocator *p_allocator,
                                   Ngs2Handle *p_out_handle);
int sceNgs2RackDestroy(Ngs2Handle h_rack, Ngs2ContextBufferInfo *p_out_info);
int sceNgs2RackLock(Ngs2Handle h_rack);
int sceNgs2RackUnlock(Ngs2Handle h_rack);
int sceNgs2RackSetUserData(Ngs2Handle h_rack, uintptr_t user_data);
int sceNgs2RackGetUserData(Ngs2Handle h_rack, uintptr_t *p_out_user_data);
int sceNgs2RackGetVoiceHandle(Ngs2Handle h_rack, uint32_t i_voice, Ngs2Handle *p_out_handle);
int sceNgs2RackGetInfo(Ngs2Handle h_rack, Ngs2RackInfo *p_out_info, size_t cb_info);
int sceNgs2CustomRackGetModuleInfo(Ngs2Handle h_rack, uint32_t i_module, Ngs2CustomModuleInfo *p_out_info, size_t cb_info);
int sceNgs2VoiceControl(Ngs2Handle h_voice, const Ngs2ParamHeader *p_params);
int sceNgs2VoiceGetStateFlags(Ngs2Handle h_voice, uint32_t *p_out_flags);
int sceNgs2VoiceGetState(Ngs2Handle h_voice, void *p_out_state, size_t cb_state);
int sceNgs2VoiceGetOwner(Ngs2Handle h_voice, Ngs2Handle *p_out_rack, uint32_t *p_out_index);
int sceNgs2VoiceGetMatrixInfo(Ngs2Handle h_voice, uint32_t matrix_id, Ngs2MatrixInfo *p_out_info, size_t cb_info);
int sceNgs2VoiceGetPortInfo(Ngs2Handle h_voice, uint32_t i_port, Ngs2PortInfo *p_out_info, size_t cb_info);
int sceNgs2ReportRegisterHandler(uint32_t type, void (*pfn_handler)(const Ngs2ReportMessage *, uintptr_t), uintptr_t user_data,
                                 Ngs2Handle *p_out_handle);
int sceNgs2ReportUnregisterHandler(Ngs2Handle h_report);

static FILE *s_p_results;
static int s_c_mismatches;

/* One CSV row: case, what was read, the value, the firmware's expected value (empty when the
 * spec does not give one: those rows are what the console should teach psport), match. */
static void record(const char *sz_case, uint64_t value, uint32_t expected) {
    const int is_match = expected == NO_EXPECTATION || (uint32_t)value == expected;
    if (!is_match) s_c_mismatches++;
    if (expected == NO_EXPECTATION) {
        fprintf(s_p_results, "%s,%#llx,,\n", sz_case, (unsigned long long)value);
    } else {
        fprintf(s_p_results, "%s,%#llx,%#x,%s\n", sz_case, (unsigned long long)value, expected, is_match ? "ok" : "MISMATCH");
    }
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

static int32_t allocate_fails(Ngs2ContextBufferInfo *p_info) {
    (void)p_info;
    return -5;
}

static const Ngs2BufferAllocator s_allocator = {allocate_buffer, free_buffer, 0x1234};
static const uintptr_t s_zero_block[4];

static Ngs2Handle create_rack(Ngs2Handle h_system, uint32_t rack_id, const void *p_option) {
    Ngs2Handle h_rack = 0;
    record("setup_rack_create", (uint32_t)sceNgs2RackCreateWithAllocator(h_system, rack_id, p_option, &s_allocator, &h_rack), 0);
    return h_rack;
}

static Ngs2Handle voice_of(Ngs2Handle h_rack, uint32_t i_voice) {
    Ngs2Handle h_voice = 0;
    record("setup_voice_handle", (uint32_t)sceNgs2RackGetVoiceHandle(h_rack, i_voice, &h_voice), 0);
    return h_voice;
}

static void header(Ngs2ParamHeader *p_header, uint16_t cb_param, uint32_t id) {
    p_header->cb_param = cb_param;
    p_header->off_next = 0;
    p_header->id = id;
}

static Ngs2SystemOption system_option(void) {
    Ngs2SystemOption option = {.cb_option = sizeof(option), .max_grain_samples = 512, .num_grain_samples = 256, .rate_hz = 48000};
    return option;
}

static void test_system(void) {
    Ngs2ContextBufferInfo info = {.user_data = 0xabc};
    record("sys_query_null_out", (uint32_t)sceNgs2SystemQueryBufferSize(NULL, NULL), E(0x53));
    record("sys_query_default", (uint32_t)sceNgs2SystemQueryBufferSize(NULL, &info), 0);
    record("sys_query_default_size", info.cb_host_buffer, 0x10400);
    record("sys_query_user_data_kept", info.user_data, 0xabc);
    Ngs2SystemOption option = system_option();
    record("sys_query_option", (uint32_t)sceNgs2SystemQueryBufferSize(&option, &info), 0);
    record("sys_query_option_size", info.cb_host_buffer, 0x10400);
    struct {
        const char *sz_case;
        uint32_t cb_option, max_grain, num_grain, rate_hz, flags, expected, cb_expected;
    } cases[] = {
        {"sys_query_option_size_3f", 0x3f, 512, 256, 48000, 0, E(0x81), 0},
        {"sys_query_max_grain_32", 0x40, 32, 256, 48000, 0, E(0x50), 0},
        {"sys_query_max_grain_100", 0x40, 100, 256, 48000, 0, E(0x50), 0},
        {"sys_query_max_grain_1088", 0x40, 1088, 256, 48000, 0, E(0x50), 0},
        {"sys_query_num_grain_65", 0x40, 512, 65, 48000, 0, E(0x51), 0},
        {"sys_query_rate_44000", 0x40, 512, 256, 44000, 0, E(0x201), 0},
        {"sys_query_order_max_first", 0x40, 32, 65, 1, 0, E(0x50), 0},
        {"sys_query_order_num_second", 0x40, 512, 65, 1, 0, E(0x51), 0},
        {"sys_query_num_over_max_ok", 0x40, 512, 1024, 48000, 0, 0, 0x10400},
        {"sys_query_max_grain_1024", 0x40, 1024, 256, 48000, 0, 0, 0x20400},
        {"sys_query_max_grain_64", 0x40, 64, 64, 48000, 0, 0, 0x2400},
        {"sys_query_guards", 0x40, 512, 256, 48000, 0x80000000u, 0, 0x10410},
        {"sys_query_rate_11025", 0x40, 512, 256, 11025, 0, 0, 0x10400},
        {"sys_query_rate_192000", 0x40, 512, 256, 192000, 0, 0, 0x10400},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Ngs2SystemOption o = {.cb_option = cases[i].cb_option, .flags = cases[i].flags, .max_grain_samples = cases[i].max_grain,
                              .num_grain_samples = cases[i].num_grain, .rate_hz = cases[i].rate_hz};
        info.cb_host_buffer = 0;
        record(cases[i].sz_case, (uint32_t)sceNgs2SystemQueryBufferSize(&o, &info), cases[i].expected);
        if (cases[i].expected == 0) record(cases[i].sz_case, info.cb_host_buffer, cases[i].cb_expected);
    }

    static uint8_t s_buffer[0x10400];
    Ngs2ContextBufferInfo buffer = {s_buffer, sizeof(s_buffer), {0}, 0x55};
    Ngs2Handle h_system = 0;
    record("sys_create_null_buffer", (uint32_t)sceNgs2SystemCreate(NULL, NULL, NULL), E(0x206));
    record("sys_create_null_out", (uint32_t)sceNgs2SystemCreate(NULL, &buffer, NULL), E(0x53));
    Ngs2ContextBufferInfo bad = buffer;
    bad.p_host_buffer = NULL;
    record("sys_create_null_host", (uint32_t)sceNgs2SystemCreate(NULL, &bad, &h_system), E(0x207));
    bad = buffer, bad.cb_host_buffer = 0x103ff;
    record("sys_create_small", (uint32_t)sceNgs2SystemCreate(NULL, &bad, &h_system), E(0x209));
    strcpy(option.sz_name, "hwtest");
    record("sys_create", (uint32_t)sceNgs2SystemCreate(&option, &buffer, &h_system), 0);
    record("sys_handle_offset_in_buffer", h_system - (uintptr_t)s_buffer, NO_EXPECTATION);

    Ngs2SystemInfo sys_info;
    record("sys_info_null", (uint32_t)sceNgs2SystemGetInfo(h_system, NULL, sizeof(sys_info)), E(0x53));
    record("sys_info_size_87", (uint32_t)sceNgs2SystemGetInfo(h_system, &sys_info, 0x87), E(0x54));
    record("sys_info_handle_0", (uint32_t)sceNgs2SystemGetInfo(0, &sys_info, sizeof(sys_info)), E(0x200));
    record("sys_info_handle_zeros", (uint32_t)sceNgs2SystemGetInfo((Ngs2Handle)s_zero_block, &sys_info, sizeof(sys_info)), E(0x230));
    record("sys_info", (uint32_t)sceNgs2SystemGetInfo(h_system, &sys_info, sizeof(sys_info)), 0);
    record("sys_info_name_matches", strcmp(sys_info.sz_name, "hwtest") == 0, 1);
    record("sys_info_handle", sys_info.h_system == h_system, 1);
    record("sys_info_uid", sys_info.uid, NO_EXPECTATION);
    record("sys_info_min_grain", sys_info.min_grain_samples, 64);
    record("sys_info_max_grain", sys_info.max_grain_samples, 512);
    record("sys_info_state_flags", sys_info.state_flags, 1);
    record("sys_info_racks", sys_info.c_racks, 0);
    record("sys_info_rate", sys_info.rate_hz, 48000);
    record("sys_info_grain", sys_info.num_grain_samples, 256);
    record("sys_info_user_data", sys_info.buffer_info.user_data, 0x55);

    uintptr_t user_data = 99;
    record("sys_get_user_data_null", (uint32_t)sceNgs2SystemGetUserData(h_system, NULL), E(0x53));
    record("sys_get_user_data_bad_handle", (uint32_t)sceNgs2SystemGetUserData((Ngs2Handle)s_zero_block, &user_data), E(0x230));
    record("sys_get_user_data_bad_handle_zeroed", user_data, 0);
    sceNgs2SystemSetUserData(h_system, 0xfeed);
    record("sys_get_user_data", (uint32_t)sceNgs2SystemGetUserData(h_system, &user_data), 0);
    record("sys_user_data_round_trip", user_data, 0xfeed);

    record("sys_set_grain_100", (uint32_t)sceNgs2SystemSetGrainSamples(h_system, 100), E(0x51));
    record("sys_set_grain_0", (uint32_t)sceNgs2SystemSetGrainSamples(h_system, 0), E(0x51));
    record("sys_set_grain_576", (uint32_t)sceNgs2SystemSetGrainSamples(h_system, 576), E(0x51));
    record("sys_set_grain_512", (uint32_t)sceNgs2SystemSetGrainSamples(h_system, 512), 0);
    record("sys_set_rate_44000", (uint32_t)sceNgs2SystemSetSampleRate(h_system, 44000), E(0x201));
    record("sys_set_rate_44100", (uint32_t)sceNgs2SystemSetSampleRate(h_system, 44100), 0);
    sceNgs2SystemGetInfo(h_system, &sys_info, sizeof(sys_info));
    record("sys_info_grain_after_set", sys_info.num_grain_samples, 512);
    record("sys_info_rate_after_set", sys_info.rate_hz, 44100);
    sceNgs2SystemSetGrainSamples(h_system, 256);
    sceNgs2SystemSetSampleRate(h_system, 48000);

    record("sys_lock", (uint32_t)sceNgs2SystemLock(h_system), 0);
    sceNgs2SystemGetInfo(h_system, &sys_info, sizeof(sys_info));
    record("sys_info_state_flags_locked", sys_info.state_flags, 9);
    record("sys_unlock", (uint32_t)sceNgs2SystemUnlock(h_system), 0);
    record("sys_unlock_again", (uint32_t)sceNgs2SystemUnlock(h_system), KERNEL_EPERM);

    /* Render: channels, type, address, size; a failure clears every buffer as declared. */
    static float s_out[256 * 2 + 16];
    Ngs2RenderBufferInfo render = {s_out, sizeof(s_out), 0x18, 2};
    record("render_bad_handle", (uint32_t)sceNgs2SystemRender((Ngs2Handle)s_zero_block, &render, 1), E(0x230));
    record("render_null_buffers", (uint32_t)sceNgs2SystemRender(h_system, NULL, 1), E(0x207));
    record("render_0_buffers", (uint32_t)sceNgs2SystemRender(h_system, &render, 0), E(0x207));
    record("render_17_buffers", (uint32_t)sceNgs2SystemRender(h_system, &render, 17), E(0x207));
    for (size_t i = 0; i < sizeof(s_out) / sizeof(s_out[0]); i++) s_out[i] = 1.0f;
    Ngs2RenderBufferInfo r = render;
    r.c_channels = 3;
    record("render_3_channels", (uint32_t)sceNgs2SystemRender(h_system, &r, 1), E(0x52));
    record("render_failed_clears_declared_size", s_out[256 * 2 + 15] == 0.0f, 1);
    r = render, r.waveform_type = 0x14;
    record("render_type_14", (uint32_t)sceNgs2SystemRender(h_system, &r, 1), E(0x402));
    r = render, r.p_buffer = NULL;
    record("render_null_buffer", (uint32_t)sceNgs2SystemRender(h_system, &r, 1), E(0x207));
    r = render, r.cb_buffer = 256 * 2 * 4 - 1;
    record("render_small_buffer", (uint32_t)sceNgs2SystemRender(h_system, &r, 1), E(0x209));
    for (size_t i = 0; i < sizeof(s_out) / sizeof(s_out[0]); i++) s_out[i] = 1.0f;
    record("render", (uint32_t)sceNgs2SystemRender(h_system, &render, 1), 0);
    record("render_silence", s_out[0] == 0.0f && s_out[511] == 0.0f, 1);
    record("render_past_grain_untouched", s_out[512] == 1.0f, 1);
    sceNgs2SystemGetInfo(h_system, &sys_info, sizeof(sys_info));
    record("sys_info_render_count", (uint64_t)sys_info.c_renders, 1);

    Ngs2Handle handles[8] = {0};
    record("enum_systems_null", (uint32_t)sceNgs2SystemEnumHandles(NULL, 1), E(0x53));
    record("enum_systems_count", (uint32_t)sceNgs2SystemEnumHandles(NULL, 0), 1);
    record("enum_systems", (uint32_t)sceNgs2SystemEnumHandles(handles, 8), 1);
    record("enum_systems_first", handles[0] == h_system, 1);

    Ngs2ContextBufferInfo out_info = {0};
    record("sys_destroy", (uint32_t)sceNgs2SystemDestroy(h_system, &out_info), 0);
    record("sys_destroy_buffer_back", out_info.p_host_buffer == s_buffer && out_info.user_data == 0x55, 1);
    record("sys_destroy_again", (uint32_t)sceNgs2SystemDestroy(h_system, NULL), E(0x230));
    record("sys_info_after_destroy", (uint32_t)sceNgs2SystemGetInfo(h_system, &sys_info, sizeof(sys_info)), E(0x230));

    const Ngs2BufferAllocator no_alloc = {NULL, free_buffer, 0};
    const Ngs2BufferAllocator failing = {allocate_fails, free_buffer, 0};
    record("sys_alloc_null", (uint32_t)sceNgs2SystemCreateWithAllocator(NULL, NULL, &h_system), E(0x20a));
    record("sys_alloc_null_handler", (uint32_t)sceNgs2SystemCreateWithAllocator(NULL, &no_alloc, &h_system), E(0x20a));
    record("sys_alloc_null_out", (uint32_t)sceNgs2SystemCreateWithAllocator(NULL, &s_allocator, NULL), E(0x53));
    record("sys_alloc_error_propagated", (uint32_t)sceNgs2SystemCreateWithAllocator(NULL, &failing, &h_system), (uint32_t)-5);
}

static void *unlock_on_other_thread(void *p_handle) {
    return (void *)(intptr_t)sceNgs2SystemUnlock(*(Ngs2Handle *)p_handle);
}

static void test_locks(void) {
    Ngs2Handle h_system = 0;
    sceNgs2SystemCreateWithAllocator(NULL, &s_allocator, &h_system);
    sceNgs2SystemLock(h_system);
    ScePthread thread;
    void *p_result = NULL;
    if (scePthreadCreate(&thread, NULL, unlock_on_other_thread, &h_system, "ngs2_api_unlock") == 0) {
        scePthreadJoin(thread, &p_result);
        record("sys_unlock_other_thread", (uint32_t)(intptr_t)p_result, KERNEL_EPERM);
    }
    record("sys_unlock_owner", (uint32_t)sceNgs2SystemUnlock(h_system), 0);
    sceNgs2SystemDestroy(h_system, NULL);
}

#define CHECK_OPTION(sz_case, rack_id, option, expected)                                                                         \
    do {                                                                                                                         \
        info.cb_host_buffer = 0;                                                                                                 \
        record(sz_case, (uint32_t)sceNgs2RackQueryBufferSize(rack_id, &(option), &info), expected);                              \
        if ((expected) == 0) record(sz_case "_size", info.cb_host_buffer, NO_EXPECTATION);                                     \
    } while (0)

static Ngs2SamplerRackOption sampler_option(void) {
    Ngs2SamplerRackOption o = {.rack = {sizeof(o), "", 0, 512, 256, 0, 1, 8, {0}}};
    o.max_channel_works = 256, o.max_codec_caches = 10, o.max_waveform_blocks = 4, o.max_envelope_points = 4, o.max_filters = 8;
    o.max_atrac9_decoders = 256, o.max_atrac9_channel_works = 256, o.num_peak_meter_blocks = 8;
    return o;
}

static Ngs2CustomSubmixerRackOption custom_option(void) {
    static Ngs2CustomSubmixerRackOption s_option;
    Ngs2CustomSubmixerRackOption o = s_option;
    o.rack = (Ngs2RackOption){sizeof(o), "", 0, 512, 1, 1, 1, 8, {0}};
    o.cb_state = 0x40, o.c_buffers = 2, o.c_modules = 2;
    o.modules[0] = (Ngs2CustomModule){NULL, 0x15, 0, 1, 1, 0xc, 8, {0}};
    o.modules[1] = (Ngs2CustomModule){NULL, 0x13, 1, 0, 0, 0x14, 0x2c, {0}};
    o.max_channels = 8, o.max_inputs = 2;
    return o;
}

static void test_rack_options(void) {
    Ngs2ContextBufferInfo info = {0};
    record("rack_query_bad_id", (uint32_t)sceNgs2RackQueryBufferSize(0x9999, NULL, &info), E(0x260));
    record("rack_query_null_out", (uint32_t)sceNgs2RackQueryBufferSize(RACK_SAMPLER, NULL, NULL), E(0x53));
    const struct {
        const char *sz_case;
        uint32_t rack_id;
    } racks[] = {{"rack_query_default_sampler", RACK_SAMPLER},
                 {"rack_query_default_submixer", RACK_SUBMIXER},
                 {"rack_query_default_reverb", RACK_REVERB},
                 {"rack_query_default_mastering", RACK_MASTERING},
                 {"rack_query_default_custom_sampler", RACK_CUSTOM_SAMPLER},
                 {"rack_query_default_custom_submixer", RACK_CUSTOM_SUBMIXER},
                 {"rack_query_default_custom_mastering", RACK_CUSTOM_MASTERING}};
    for (size_t i = 0; i < sizeof(racks) / sizeof(racks[0]); i++) {
        record(racks[i].sz_case, (uint32_t)sceNgs2RackQueryBufferSize(racks[i].rack_id, NULL, &info), 0);
        /* The console's exact rack sizes are what psport does not know yet (spec 08 §2). */
        record(racks[i].sz_case, info.cb_host_buffer, NO_EXPECTATION);
    }

    const Ngs2SamplerRackOption s = sampler_option();
    Ngs2SamplerRackOption t = s;
    CHECK_OPTION("sampler_option_default_values", RACK_SAMPLER, t, 0);
    t = s, t.rack.flags = 0x80000000u;
    CHECK_OPTION("sampler_option_guards", RACK_SAMPLER, t, 0);
    t = s, t.rack.flags = 0x40000000u;
    CHECK_OPTION("sampler_option_callbacks", RACK_SAMPLER, t, 0);
    t = s, t.rack.max_voices = 1, t.max_channel_works = 1, t.max_atrac9_decoders = 0;
    CHECK_OPTION("sampler_option_1_voice", RACK_SAMPLER, t, 0);
    t = s, t.rack.cb_option = 0x7f;
    CHECK_OPTION("sampler_option_size_7f", RACK_SAMPLER, t, E(0x81));
    t = s, t.rack.flags = 1;
    CHECK_OPTION("sampler_option_flags_1", RACK_SAMPLER, t, E(0x82));
    t = s, t.rack.cb_option = 0x80;
    CHECK_OPTION("sampler_option_size_80", RACK_SAMPLER, t, E(0x81));
    t = s, t.rack.cb_option = 0x80, t.rack.flags = 1;
    CHECK_OPTION("sampler_option_flags_before_size", RACK_SAMPLER, t, E(0x82));
    t = s, t.rack.max_grain_samples = 32;
    CHECK_OPTION("sampler_option_grain_32", RACK_SAMPLER, t, E(0x50));
    t = s, t.rack.max_grain_samples = 1000;
    CHECK_OPTION("sampler_option_grain_1000", RACK_SAMPLER, t, E(0x50));
    t = s, t.rack.max_voices = 0;
    CHECK_OPTION("sampler_option_voices_0", RACK_SAMPLER, t, E(0x103));
    t = s, t.rack.max_voices = 1025;
    CHECK_OPTION("sampler_option_voices_1025", RACK_SAMPLER, t, E(0x103));
    t = s, t.max_channel_works = 255;
    CHECK_OPTION("sampler_option_channel_works_255", RACK_SAMPLER, t, E(0x105));
    t = s, t.max_channel_works = 2049;
    CHECK_OPTION("sampler_option_channel_works_2049", RACK_SAMPLER, t, E(0x105));
    t = s, t.max_atrac9_decoders = 257;
    CHECK_OPTION("sampler_option_atrac9_decoders_257", RACK_SAMPLER, t, E(0x10a));
    t = s, t.max_waveform_blocks = 0;
    CHECK_OPTION("sampler_option_blocks_0", RACK_SAMPLER, t, E(0x106));
    t = s, t.max_waveform_blocks = 17;
    CHECK_OPTION("sampler_option_blocks_17", RACK_SAMPLER, t, E(0x106));
    t = s, t.max_envelope_points = 17;
    CHECK_OPTION("sampler_option_envelope_17", RACK_SAMPLER, t, E(0x107));
    t = s, t.max_filters = 17;
    CHECK_OPTION("sampler_option_filters_17", RACK_SAMPLER, t, E(0x108));
    t = s, t.max_ajm_atrac9_decoders = 257;
    CHECK_OPTION("sampler_option_ajm_257", RACK_SAMPLER, t, E(0x10a));
    t = s, t.num_peak_meter_blocks = 0;
    CHECK_OPTION("sampler_option_peak_0", RACK_SAMPLER, t, E(0x156));
    t = s, t.num_peak_meter_blocks = 257;
    CHECK_OPTION("sampler_option_peak_257", RACK_SAMPLER, t, E(0x156));
    t = s, t.max_atrac9_channel_works = 255;
    CHECK_OPTION("sampler_option_atrac9_works_255", RACK_SAMPLER, t, E(0x10b));
    t = s, t.max_atrac9_decoders = 0, t.max_atrac9_channel_works = 0;
    CHECK_OPTION("sampler_option_atrac9_works_unchecked", RACK_SAMPLER, t, 0);
    t = s, t.max_ajm_atrac9_decoders = 2, t.max_codec_caches = 9;
    CHECK_OPTION("sampler_option_caches_9", RACK_SAMPLER, t, E(0x109));
    t = s, t.max_ajm_atrac9_decoders = 2, t.max_codec_caches = 16;
    CHECK_OPTION("sampler_option_caches_16", RACK_SAMPLER, t, 0);
    t = s, t.max_ajm_atrac9_decoders = 2, t.max_codec_caches = 17;
    CHECK_OPTION("sampler_option_caches_17", RACK_SAMPLER, t, E(0x109));
    t = s, t.rack.max_input_delay_blocks = 1;
    CHECK_OPTION("sampler_option_delay_1", RACK_SAMPLER, t, E(0x102));
    t = s, t.rack.max_matrices = 0;
    CHECK_OPTION("sampler_option_matrices_0", RACK_SAMPLER, t, E(0x100));
    t = s, t.rack.max_matrices = 17;
    CHECK_OPTION("sampler_option_matrices_17", RACK_SAMPLER, t, E(0x100));
    t = s, t.rack.max_ports = 0;
    CHECK_OPTION("sampler_option_ports_0", RACK_SAMPLER, t, E(0x101));
    t = s, t.rack.max_ports = 17;
    CHECK_OPTION("sampler_option_ports_17", RACK_SAMPLER, t, E(0x101));
    t = s, t.rack.max_voices = 0, t.rack.max_ports = 0;
    CHECK_OPTION("sampler_option_rack_fields_first", RACK_SAMPLER, t, E(0x103));

    const Ngs2SubmixerRackOption m = {{sizeof(m), "", 0, 512, 1, 1, 1, 8, {0}}, 8, 4, 8, 1, 8, 0};
    Ngs2SubmixerRackOption u = m;
    CHECK_OPTION("submixer_option", RACK_SUBMIXER, u, 0);
    u = m, u.max_channels = 9;
    CHECK_OPTION("submixer_option_channels_9", RACK_SUBMIXER, u, E(0x104));
    u = m, u.max_inputs = 3;
    CHECK_OPTION("submixer_option_inputs_3", RACK_SUBMIXER, u, E(0x10c));
    u = m, u.rack.max_input_delay_blocks = 0;
    CHECK_OPTION("submixer_option_delay_0", RACK_SUBMIXER, u, E(0x102));
    u = m, u.rack.max_input_delay_blocks = 1025;
    CHECK_OPTION("submixer_option_delay_1025", RACK_SUBMIXER, u, E(0x102));
    u = m;
    CHECK_OPTION("submixer_option_for_reverb", RACK_REVERB, u, E(0x81));

    const Ngs2ReverbRackOption v = {{sizeof(v), "", 0, 512, 1, 1, 1, 8, {0}}, 8, 1};
    Ngs2ReverbRackOption w = v;
    CHECK_OPTION("reverb_option", RACK_REVERB, w, 0);
    w = v, w.rack.max_voices = 17;
    CHECK_OPTION("reverb_option_voices_17", RACK_REVERB, w, E(0x103));
    w = v, w.reverb_size = 2;
    CHECK_OPTION("reverb_option_size_2", RACK_REVERB, w, E(0x900));

    const Ngs2MasteringRackOption a = {{sizeof(a), "", 0, 512, 2, 1, 0, 0, {0}}, 8, 8};
    Ngs2MasteringRackOption b = a;
    CHECK_OPTION("mastering_option", RACK_MASTERING, b, 0);
    b = a, b.rack.max_voices = 257;
    CHECK_OPTION("mastering_option_voices_257", RACK_MASTERING, b, E(0x103));
    b = a, b.rack.max_matrices = 1;
    CHECK_OPTION("mastering_option_matrices_1", RACK_MASTERING, b, E(0x100));
    b = a, b.rack.max_ports = 1;
    CHECK_OPTION("mastering_option_ports_1", RACK_MASTERING, b, E(0x101));

    const Ngs2CustomSubmixerRackOption c = custom_option();
    Ngs2CustomSubmixerRackOption d = c;
    CHECK_OPTION("custom_option", RACK_CUSTOM_SUBMIXER, d, 0);
    d = c, d.cb_state = 3;
    CHECK_OPTION("custom_option_state_3", RACK_CUSTOM_SUBMIXER, d, E(0xa06));
    d = c, d.c_buffers = 9;
    CHECK_OPTION("custom_option_buffers_9", RACK_CUSTOM_SUBMIXER, d, E(0xa02));
    d = c, d.c_modules = 25;
    CHECK_OPTION("custom_option_modules_25", RACK_CUSTOM_SUBMIXER, d, E(0xa01));
    d = c, d.modules[1].module_id = 0x21;
    CHECK_OPTION("custom_option_module_21", RACK_CUSTOM_SUBMIXER, d, E(0xa00));
    d = c, d.modules[1].dest_buffer_id = 2;
    CHECK_OPTION("custom_option_dest_buffer_2", RACK_CUSTOM_SUBMIXER, d, E(0xa03));
    d = c, d.modules[0].off_state = 8;
    CHECK_OPTION("custom_option_state_offset_8", RACK_CUSTOM_SUBMIXER, d, E(0xa04));
    d = c, d.modules[1].cb_state = 0x2d;
    CHECK_OPTION("custom_option_module_state_2d", RACK_CUSTOM_SUBMIXER, d, E(0xa05));
    d = c, d.ports[7][0] = 2;
    CHECK_OPTION("custom_option_port_buffer_2", RACK_CUSTOM_SUBMIXER, d, E(0xa00));
    d = c, d.ports[8][0] = 2;
    CHECK_OPTION("custom_option_port_past_max", RACK_CUSTOM_SUBMIXER, d, 0);
    d = c;
    CHECK_OPTION("custom_submixer_option_for_mastering", RACK_CUSTOM_MASTERING, d, E(0x100));
}

static void test_racks(void) {
    Ngs2Handle h_system = 0, h = 0;
    sceNgs2SystemCreateWithAllocator(NULL, &s_allocator, &h_system);
    Ngs2ContextBufferInfo info = {0};
    sceNgs2RackQueryBufferSize(RACK_SAMPLER, NULL, &info);
    void *p_buffer = calloc(1, info.cb_host_buffer);
    Ngs2ContextBufferInfo buffer = {p_buffer, info.cb_host_buffer, {0}, 0x77};
    record("rack_create_bad_id", (uint32_t)sceNgs2RackCreate(h_system, 0x9999, NULL, NULL, NULL), E(0x260));
    record("rack_create_null_buffer", (uint32_t)sceNgs2RackCreate(h_system, RACK_SAMPLER, NULL, NULL, NULL), E(0x206));
    record("rack_create_null_out", (uint32_t)sceNgs2RackCreate(h_system, RACK_SAMPLER, NULL, &buffer, NULL), E(0x53));
    Ngs2ContextBufferInfo bad = buffer;
    bad.p_host_buffer = NULL;
    record("rack_create_null_host", (uint32_t)sceNgs2RackCreate(h_system, RACK_SAMPLER, NULL, &bad, &h), E(0x207));
    bad = buffer, bad.cb_host_buffer -= 1;
    record("rack_create_small", (uint32_t)sceNgs2RackCreate(h_system, RACK_SAMPLER, NULL, &bad, &h), E(0x209));
    record("rack_create_bad_system_last", (uint32_t)sceNgs2RackCreate((Ngs2Handle)s_zero_block, RACK_SAMPLER, NULL, &buffer, &h), E(0x230));
    record("rack_create", (uint32_t)sceNgs2RackCreate(h_system, RACK_SAMPLER, NULL, &buffer, &h), 0);
    const Ngs2Handle h_sampler = h;
    const Ngs2Handle h_mastering = create_rack(h_system, RACK_MASTERING, NULL);
    const Ngs2Handle h_submixer = create_rack(h_system, RACK_SUBMIXER, NULL);
    const Ngs2Handle h_reverb = create_rack(h_system, RACK_REVERB, NULL);
    Ngs2CustomSubmixerRackOption c = custom_option();
    c.ports[3][0] = 1;
    c.max_channels = 6;
    const Ngs2Handle h_custom = create_rack(h_system, RACK_CUSTOM_SUBMIXER, &c);

    Ngs2Handle handles[8] = {0};
    record("enum_racks_count", (uint32_t)sceNgs2SystemEnumRackHandles(h_system, NULL, 0), 5);
    record("enum_racks_null", (uint32_t)sceNgs2SystemEnumRackHandles(h_system, NULL, 1), E(0x53));
    sceNgs2SystemEnumRackHandles(h_system, handles, 8);
    record("enum_racks_class_order",
           handles[0] == h_sampler && handles[1] == h_submixer && handles[2] == h_reverb && handles[3] == h_custom && handles[4] == h_mastering,
           1);

    static Ngs2RackInfo s_info;
    record("rack_info_null", (uint32_t)sceNgs2RackGetInfo(h_sampler, NULL, 0xc8), E(0x53));
    record("rack_info_size_a7", (uint32_t)sceNgs2RackGetInfo(h_sampler, &s_info, 0xa7), E(0x54));
    record("rack_info_bad_handle", (uint32_t)sceNgs2RackGetInfo((Ngs2Handle)s_zero_block, &s_info, 0xc8), E(0x261));
    memset(&s_info, 0xee, sizeof(s_info));
    record("rack_info_common_only", (uint32_t)sceNgs2RackGetInfo(h_sampler, &s_info, 0xa8), 0);
    record("rack_info_common_only_extra_untouched", s_info.extra[0], 0xeeeeeeee);
    record("rack_info_sampler", (uint32_t)sceNgs2RackGetInfo(h_sampler, &s_info, 0xc8), 0);
    record("rack_info_sampler_handle", s_info.h_rack == h_sampler && s_info.h_owner == h_system, 1);
    record("rack_info_sampler_class", s_info.rack_class, 0);
    record("rack_info_sampler_id", s_info.rack_id, RACK_SAMPLER);
    record("rack_info_sampler_uid", s_info.uid, 1);
    record("rack_info_sampler_grain", s_info.min_grain_samples << 16 | s_info.max_grain_samples, 64 << 16 | 512);
    record("rack_info_sampler_voices", s_info.max_voices, 256);
    record("rack_info_sampler_channel_works", s_info.max_channel_works, 256);
    record("rack_info_sampler_inputs", s_info.max_inputs, 0);
    record("rack_info_sampler_matrices", s_info.max_matrices, 1);
    record("rack_info_sampler_ports", s_info.max_ports, 8);
    record("rack_info_sampler_state_flags", s_info.state_flags, 1);
    const char *sz_sampler_extra[] = {"channel_works", "codec_caches", "waveform_blocks", "envelope_points", "filters",
                                      "atrac9_decoders", "atrac9_channel_works", "ajm_atrac9_decoders"};
    const uint32_t sampler_extra[] = {256, 10, 4, 4, 8, 256, 256, 0};
    for (int i = 0; i < 8; i++) {
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "rack_info_sampler_extra_%s", sz_sampler_extra[i]);
        record(sz_case, s_info.extra[i], sampler_extra[i]);
    }
    sceNgs2RackGetInfo(h_submixer, &s_info, 0xb8);
    record("rack_info_submixer_class_uid", s_info.rack_class << 16 | s_info.uid, 1 << 16 | 3);
    record("rack_info_submixer_voices_inputs", s_info.max_voices << 16 | s_info.max_inputs, 1 << 16 | 1);
    record("rack_info_submixer_channel_works", s_info.max_channel_works, 8);
    record("rack_info_submixer_extra", s_info.extra[0] << 24 | s_info.extra[1] << 16 | s_info.extra[2] << 8 | s_info.extra[3], 0x08040801);
    sceNgs2RackGetInfo(h_reverb, &s_info, 0xb0);
    record("rack_info_reverb_extra", s_info.extra[0] << 8 | s_info.extra[1], 0x0801);
    record("rack_info_reverb_channel_works", s_info.max_channel_works, NO_EXPECTATION);
    sceNgs2RackGetInfo(h_mastering, &s_info, 0xb0);
    record("rack_info_mastering_class", s_info.rack_class, 2);
    record("rack_info_mastering_voices", s_info.max_voices, 2);
    record("rack_info_mastering_ports_matrices", s_info.max_ports << 8 | s_info.max_matrices, 0);
    record("rack_info_mastering_inputs", s_info.max_inputs, 1);
    record("rack_info_mastering_channel_works", s_info.max_channel_works, NO_EXPECTATION);
    record("rack_info_mastering_extra", s_info.extra[0], 8);
    sceNgs2RackGetInfo(h_custom, &s_info, 0x500);
    record("rack_info_custom_state_buffers_modules", s_info.extra[0] << 16 | s_info.extra[1] << 8 | s_info.extra[2], 0x400202);
    record("rack_info_custom_module0_id", s_info.extra[(0xb8 - 0xa8) / 4 + 2], 0x15);
    record("rack_info_custom_module0_extra_buffer", s_info.extra[(0xb8 - 0xa8) / 4 + 4], 0);
    record("rack_info_custom_port3", s_info.extra[(0x478 - 0xa8) / 4 + 3 * 2], 1);
    record("rack_info_custom_channels_inputs", s_info.extra[(0x4f8 - 0xa8) / 4] << 8 | s_info.extra[(0x4fc - 0xa8) / 4], 0x0602);

    Ngs2CustomModuleInfo module_info;
    record("module_info_null", (uint32_t)sceNgs2CustomRackGetModuleInfo(h_custom, 0, NULL, 0x20), E(0x53));
    record("module_info_size_1f", (uint32_t)sceNgs2CustomRackGetModuleInfo(h_custom, 0, &module_info, 0x1f), E(0x54));
    record("module_info_index_2", (uint32_t)sceNgs2CustomRackGetModuleInfo(h_custom, 2, &module_info, 0x20), E(0xb00));
    record("module_info_not_custom", (uint32_t)sceNgs2CustomRackGetModuleInfo(h_sampler, 0, &module_info, 0x20), E(0xb00));
    record("module_info", (uint32_t)sceNgs2CustomRackGetModuleInfo(h_custom, 0, &module_info, 0x20), 0);
    record("module_info_ids", module_info.module_id << 24 | module_info.source_buffer_id << 16 | module_info.extra_buffer_id << 8 | module_info.dest_buffer_id,
           0x15000101);
    record("module_info_state", module_info.off_state << 16 | module_info.cb_state, 0xc0008);

    uintptr_t user_data = 5;
    record("rack_get_user_data_null", (uint32_t)sceNgs2RackGetUserData(h_sampler, NULL), E(0x53));
    record("rack_get_user_data_bad_handle", (uint32_t)sceNgs2RackGetUserData((Ngs2Handle)s_zero_block, &user_data), E(0x261));
    sceNgs2RackSetUserData(h_sampler, 0xbeef);
    sceNgs2RackGetUserData(h_sampler, &user_data);
    record("rack_user_data_round_trip", user_data, 0xbeef);
    record("rack_lock", (uint32_t)sceNgs2RackLock(h_sampler), 0);
    sceNgs2RackGetInfo(h_sampler, &s_info, 0xa8);
    record("rack_info_state_flags_locked", s_info.state_flags, 5);
    record("rack_unlock", (uint32_t)sceNgs2RackUnlock(h_sampler), 0);
    record("rack_unlock_again", (uint32_t)sceNgs2RackUnlock(h_sampler), KERNEL_EPERM);

    Ngs2Handle h_voice = 1;
    record("voice_handle_null", (uint32_t)sceNgs2RackGetVoiceHandle(h_sampler, 0, NULL), E(0x53));
    record("voice_handle_bad_rack", (uint32_t)sceNgs2RackGetVoiceHandle((Ngs2Handle)s_zero_block, 0, &h_voice), E(0x261));
    record("voice_handle_bad_rack_zeroed", h_voice, 0);
    h_voice = 1;
    record("voice_handle_index_256", (uint32_t)sceNgs2RackGetVoiceHandle(h_sampler, 256, &h_voice), E(0x302));
    record("voice_handle_index_256_zeroed", h_voice, 0);
    record("voice_handle_255", (uint32_t)sceNgs2RackGetVoiceHandle(h_sampler, 255, &h_voice), 0);
    record("voice_handle_is_not_rack", (uint32_t)sceNgs2RackGetInfo(h_voice, &s_info, 0xa8), E(0x261));

    Ngs2SamplerRackOption small = sampler_option();
    small.rack.max_grain_samples = 128, small.rack.max_voices = 4, small.max_channel_works = 4, small.max_atrac9_decoders = 0;
    record("rack_grain_below_system", (uint32_t)sceNgs2RackCreateWithAllocator(h_system, RACK_SAMPLER, &small, &s_allocator, &h), E(0x51));
    sceNgs2SystemSetGrainSamples(h_system, 128);
    const Ngs2Handle h_small = create_rack(h_system, RACK_SAMPLER, &small);
    Ngs2SystemInfo sys_info;
    sceNgs2SystemGetInfo(h_system, &sys_info, sizeof(sys_info));
    record("sys_info_max_grain_with_small_rack", sys_info.max_grain_samples, 128);
    record("sys_set_grain_over_rack", (uint32_t)sceNgs2SystemSetGrainSamples(h_system, 256), E(0x51));
    sceNgs2RackDestroy(h_small, NULL);
    sceNgs2SystemGetInfo(h_system, &sys_info, sizeof(sys_info));
    record("sys_info_max_grain_after_destroy", sys_info.max_grain_samples, 512);

    Ngs2ContextBufferInfo out_info = {0};
    record("rack_destroy_voice_handle", (uint32_t)sceNgs2RackDestroy(h_voice, NULL), E(0x261));
    record("sys_destroy_under_racks", (uint32_t)sceNgs2SystemDestroy(h_system, NULL), 0);
    record("rack_info_orphan", (uint32_t)sceNgs2RackGetInfo(h_sampler, &s_info, 0xa8), 0);
    record("rack_info_orphan_owner", s_info.h_owner, 0);
    record("rack_destroy_orphan", (uint32_t)sceNgs2RackDestroy(h_sampler, &out_info), 0);
    record("rack_destroy_buffer_back", out_info.p_host_buffer == p_buffer && out_info.user_data == 0x77, 1);
    record("rack_info_after_destroy", (uint32_t)sceNgs2RackGetInfo(h_sampler, &s_info, 0xa8), E(0x261));
    sceNgs2RackDestroy(h_mastering, NULL);
    sceNgs2RackDestroy(h_submixer, NULL);
    sceNgs2RackDestroy(h_reverb, NULL);
    sceNgs2RackDestroy(h_custom, NULL);
    free(p_buffer);
}

static void voice_callback(const void *p_info) {
    (void)p_info;
}

static void test_voices(void) {
    Ngs2Handle h_system = 0, h_other_system = 0;
    sceNgs2SystemCreateWithAllocator(NULL, &s_allocator, &h_system);
    sceNgs2SystemCreateWithAllocator(NULL, &s_allocator, &h_other_system);
    Ngs2SamplerRackOption s = sampler_option();
    s.rack.flags = 0x40000000u, s.rack.max_voices = 4, s.rack.max_matrices = 2, s.rack.max_ports = 4;
    s.max_channel_works = 4, s.max_atrac9_decoders = 0;
    const Ngs2Handle h_sampler = create_rack(h_system, RACK_SAMPLER, &s);
    const Ngs2Handle h_plain_sampler = create_rack(h_system, RACK_SAMPLER, NULL);
    const Ngs2Handle h_submixer = create_rack(h_system, RACK_SUBMIXER, NULL);
    const Ngs2Handle h_mastering = create_rack(h_system, RACK_MASTERING, NULL);
    const Ngs2Handle h_other_mastering = create_rack(h_other_system, RACK_MASTERING, NULL);
    const Ngs2Handle h_voice = voice_of(h_sampler, 0), h_plain_voice = voice_of(h_plain_sampler, 0);
    const Ngs2Handle h_sub_voice = voice_of(h_submixer, 0), h_master_voice = voice_of(h_mastering, 0);
    const Ngs2Handle h_bad = (Ngs2Handle)s_zero_block;

    uint32_t flags = 9;
    record("voice_flags_null", (uint32_t)sceNgs2VoiceGetStateFlags(h_voice, NULL), E(0x53));
    record("voice_flags_bad_handle", (uint32_t)sceNgs2VoiceGetStateFlags(h_bad, &flags), E(0x300));
    record("voice_flags_bad_handle_zeroed", flags, 0);
    record("voice_flags_rack_handle", (uint32_t)sceNgs2VoiceGetStateFlags(h_sampler, &flags), E(0x300));
    record("voice_flags_idle", (uint32_t)sceNgs2VoiceGetStateFlags(h_voice, &flags), 0);
    record("voice_flags_idle_value", flags, 0);

    Ngs2Handle h_owner = 1;
    uint32_t i_owner = 1;
    record("voice_owner_bad_handle", (uint32_t)sceNgs2VoiceGetOwner(h_bad, &h_owner, &i_owner), E(0x300));
    record("voice_owner_bad_handle_zeroed", h_owner | i_owner, 0);
    record("voice_owner_null_outs", (uint32_t)sceNgs2VoiceGetOwner(h_voice, NULL, NULL), 0);
    sceNgs2VoiceGetOwner(voice_of(h_sampler, 3), &h_owner, &i_owner);
    record("voice_owner", h_owner == h_sampler && i_owner == 3, 1);

    Ngs2EventParam event;
    header(&event.header, sizeof(event), 6);
    event.event_id = 0;
    record("control_null", (uint32_t)sceNgs2VoiceControl(h_voice, NULL), E(0x309));
    record("control_bad_handle", (uint32_t)sceNgs2VoiceControl(h_bad, &event.header), E(0x300));
    Ngs2EventParam unknown = event;
    unknown.header.id = 0x99;
    record("control_id_99", (uint32_t)sceNgs2VoiceControl(h_voice, &unknown.header), E(0x308));
    unknown.header.id = 0x8001;
    record("control_id_8001", (uint32_t)sceNgs2VoiceControl(h_voice, &unknown.header), E(0x308));
    unknown.header.id = 0x20000000;
    record("control_submixer_id_on_sampler", (uint32_t)sceNgs2VoiceControl(h_voice, &unknown.header), E(0x308));
    Ngs2EventParam wrong_size = event;
    wrong_size.header.cb_param = 0x10;
    record("control_event_size_10", (uint32_t)sceNgs2VoiceControl(h_voice, &wrong_size.header), E(0x30a));

    const float levels[3] = {5.0f, -5.0f, 0.5f};
    Ngs2MatrixParam matrix;
    header(&matrix.header, sizeof(matrix), 1);
    matrix.matrix_id = 2, matrix.c_levels = 3, matrix.p_levels = levels;
    record("matrix_id_2", (uint32_t)sceNgs2VoiceControl(h_voice, &matrix.header), E(0x700));
    matrix.matrix_id = 1, matrix.c_levels = 0;
    record("matrix_levels_0", (uint32_t)sceNgs2VoiceControl(h_voice, &matrix.header), E(0x150));
    matrix.c_levels = 65;
    record("matrix_levels_65", (uint32_t)sceNgs2VoiceControl(h_voice, &matrix.header), E(0x150));
    matrix.c_levels = 3, matrix.p_levels = NULL;
    record("matrix_null_levels", (uint32_t)sceNgs2VoiceControl(h_voice, &matrix.header), E(0x153));
    matrix.p_levels = levels;
    record("matrix", (uint32_t)sceNgs2VoiceControl(h_voice, &matrix.header), 0);
    Ngs2MatrixInfo matrix_info;
    record("matrix_info_null", (uint32_t)sceNgs2VoiceGetMatrixInfo(h_voice, 1, NULL, sizeof(matrix_info)), E(0x53));
    record("matrix_info_size_103", (uint32_t)sceNgs2VoiceGetMatrixInfo(h_voice, 1, &matrix_info, 0x103), E(0x54));
    record("matrix_info_id_2", (uint32_t)sceNgs2VoiceGetMatrixInfo(h_voice, 2, &matrix_info, sizeof(matrix_info)), E(0x700));
    record("matrix_info_mastering", (uint32_t)sceNgs2VoiceGetMatrixInfo(h_master_voice, 0, &matrix_info, sizeof(matrix_info)), E(0x700));
    sceNgs2VoiceGetMatrixInfo(h_voice, 0, &matrix_info, sizeof(matrix_info));
    record("matrix_info_unset", matrix_info.c_levels, 0);
    sceNgs2VoiceGetMatrixInfo(h_voice, 1, &matrix_info, sizeof(matrix_info));
    record("matrix_info_levels", matrix_info.c_levels, 3);
    record("matrix_info_clamped", matrix_info.levels[0] == 4.0f && matrix_info.levels[1] == -4.0f && matrix_info.levels[2] == 0.5f, 1);

    Ngs2PortInfo port_info;
    record("port_info_null", (uint32_t)sceNgs2VoiceGetPortInfo(h_voice, 0, NULL, sizeof(port_info)), E(0x53));
    record("port_info_size_17", (uint32_t)sceNgs2VoiceGetPortInfo(h_voice, 0, &port_info, 0x17), E(0x54));
    record("port_info_port_4", (uint32_t)sceNgs2VoiceGetPortInfo(h_voice, 4, &port_info, sizeof(port_info)), E(0x304));
    sceNgs2VoiceGetPortInfo(h_voice, 3, &port_info, sizeof(port_info));
    record("port_info_untouched", port_info.matrix_id == -1 && port_info.volume == 1.0f && port_info.h_dest == 0, 1);
    Ngs2PortParam port;
    header(&port.header, sizeof(port), 2);
    port.i_port = 4, port.level = 9.0f;
    record("port_volume_port_4", (uint32_t)sceNgs2VoiceControl(h_voice, &port.header), E(0x304));
    port.i_port = 1;
    record("port_volume", (uint32_t)sceNgs2VoiceControl(h_voice, &port.header), 0);
    sceNgs2VoiceGetPortInfo(h_voice, 1, &port_info, sizeof(port_info));
    record("port_volume_clamped", port_info.volume == 4.0f && port_info.matrix_id == -1, 1);
    header(&port.header, sizeof(port), 3);
    const int32_t matrix_ids[] = {-1, 0, 1, 2, 0x10, 0x13, 3, 0xf, 0x14, -2};
    const uint32_t matrix_expected[] = {0, 0, 0, 0, 0, 0, E(0x700), E(0x700), E(0x700), E(0x700)};
    for (int i = 0; i < 10; i++) {
        char sz_case[64];
        snprintf(sz_case, sizeof(sz_case), "port_matrix_%d", matrix_ids[i]);
        port.matrix_id = matrix_ids[i];
        record(sz_case, (uint32_t)sceNgs2VoiceControl(h_voice, &port.header), matrix_expected[i]);
    }
    header(&port.header, sizeof(port), 4);
    port.i_port = 2, port.delay_samples = 70000;
    sceNgs2VoiceControl(h_voice, &port.header);
    sceNgs2VoiceGetPortInfo(h_voice, 2, &port_info, sizeof(port_info));
    record("port_delay_saturated", port_info.delay_samples, 0xffff);

    Ngs2PatchParam patch;
    header(&patch.header, sizeof(patch), 5);
    patch.i_port = 0, patch.i_dest_input = 0, patch.h_dest = h_sub_voice;
    record("patch_voice", (uint32_t)sceNgs2VoiceControl(h_voice, &patch.header), 0);
    patch.i_dest_input = 1;
    record("patch_input_1", (uint32_t)sceNgs2VoiceControl(h_voice, &patch.header), E(0x305));
    patch.i_dest_input = 0, patch.h_dest = h_mastering;
    record("patch_rack", (uint32_t)sceNgs2VoiceControl(h_voice, &patch.header), 0);
    patch.h_dest = h_plain_voice;
    record("patch_to_sampler", (uint32_t)sceNgs2VoiceControl(h_voice, &patch.header), E(0x305));
    patch.h_dest = h_other_mastering;
    record("patch_other_system", (uint32_t)sceNgs2VoiceControl(h_voice, &patch.header), E(0x305));
    patch.h_dest = h_bad;
    record("patch_bad_handle", (uint32_t)sceNgs2VoiceControl(h_voice, &patch.header), E(0x200));
    patch.h_dest = h_system;
    record("patch_system_handle", (uint32_t)sceNgs2VoiceControl(h_voice, &patch.header), E(0x200));

    event.event_id = 6;
    record("event_6", (uint32_t)sceNgs2VoiceControl(h_voice, &event.header), E(0x303));
    event.event_id = 1;
    record("event_stop_idle", (uint32_t)sceNgs2VoiceControl(h_voice, &event.header), 0);
    static uint32_t s_state[0x30 / 4];
    memset(s_state, 0xcc, sizeof(s_state));
    record("state_null", (uint32_t)sceNgs2VoiceGetState(h_voice, NULL, sizeof(s_state)), E(0x53));
    record("state_size_3", (uint32_t)sceNgs2VoiceGetState(h_voice, s_state, 3), E(0x54));
    record("state_idle", (uint32_t)sceNgs2VoiceGetState(h_voice, s_state, sizeof(s_state)), 0);
    record("state_idle_zeros", s_state[0] | s_state[1] | s_state[11], 0);
    event.event_id = 0;
    record("event_play_without_setup", (uint32_t)sceNgs2VoiceControl(h_voice, &event.header), E(0x52));
    sceNgs2VoiceGetStateFlags(h_voice, &flags);
    record("voice_flags_after_play_without_setup", flags, 0);
    Ngs2SamplerSetupParam setup = {0};
    header(&setup.header, sizeof(setup), 0x10000000);
    setup.waveform_type = 0x12, setup.c_channels = 2, setup.rate_hz = 48000;
    record("event_play_setup", (uint32_t)sceNgs2VoiceControl(h_voice, &setup.header), 0);
    record("event_play", (uint32_t)sceNgs2VoiceControl(h_voice, &event.header), 0);
    sceNgs2VoiceGetStateFlags(h_voice, &flags);
    record("voice_flags_after_play", flags, 1);
    memset(s_state, 0xcc, sizeof(s_state));
    sceNgs2VoiceGetState(h_voice, s_state, sizeof(s_state));
    record("state_after_play", s_state[0], 1);
    record("state_after_play_rest", s_state[1] | s_state[2] | s_state[11], 0);
    memset(s_state, 0xcc, sizeof(s_state));
    record("state_size_4", (uint32_t)sceNgs2VoiceGetState(h_voice, s_state, 4), 0);
    record("state_size_4_value", s_state[0] << 8 | (s_state[1] & 0xff), 0x1cc);
    memset(s_state, 0xcc, sizeof(s_state));
    record("state_size_2f", (uint32_t)sceNgs2VoiceGetState(h_voice, s_state, 0x2f), 0);
    record("state_size_2f_untouched", s_state[0], 0xcccccccc);

    Ngs2CallbackParam callback;
    header(&callback.header, sizeof(callback), 7);
    callback.pfn_handler = voice_callback, callback.callback_data = 0, callback.flags = 1, callback.reserved = 0;
    record("callback_rack_without_flag", (uint32_t)sceNgs2VoiceControl(h_plain_voice, &callback.header), E(0x30c));
    callback.flags = 4;
    record("callback_flags_4", (uint32_t)sceNgs2VoiceControl(h_voice, &callback.header), E(0x30d));
    callback.flags = 1, callback.pfn_handler = NULL;
    record("callback_null_handler", (uint32_t)sceNgs2VoiceControl(h_voice, &callback.header), E(0x30e));
    callback.flags = 0;
    record("callback_cleared", (uint32_t)sceNgs2VoiceControl(h_voice, &callback.header), 0);

    /* A chain stops at its first error, keeping what came before; a cycle is caught. */
    struct {
        Ngs2PortParam volume;
        Ngs2PortParam delay;
        Ngs2EventParam bad_event;
        Ngs2PortParam last;
    } chain;
    header(&chain.volume.header, sizeof(Ngs2PortParam), 2);
    chain.volume.header.off_next = offsetof(__typeof__(chain), delay);
    chain.volume.i_port = 3, chain.volume.level = 0.25f;
    header(&chain.delay.header, sizeof(Ngs2PortParam), 4);
    chain.delay.header.off_next = offsetof(__typeof__(chain), bad_event) - offsetof(__typeof__(chain), delay);
    chain.delay.i_port = 3, chain.delay.delay_samples = 12;
    header(&chain.bad_event.header, sizeof(Ngs2EventParam), 6);
    chain.bad_event.header.off_next = offsetof(__typeof__(chain), last) - offsetof(__typeof__(chain), bad_event);
    chain.bad_event.event_id = 9;
    header(&chain.last.header, sizeof(Ngs2PortParam), 2);
    chain.last.i_port = 3, chain.last.level = 2.0f;
    record("chain_error", (uint32_t)sceNgs2VoiceControl(h_voice, &chain.volume.header), E(0x303));
    sceNgs2VoiceGetPortInfo(h_voice, 3, &port_info, sizeof(port_info));
    record("chain_error_keeps_earlier", port_info.volume == 0.25f && port_info.delay_samples == 12, 1);
    chain.bad_event.event_id = 4;
    record("chain", (uint32_t)sceNgs2VoiceControl(h_voice, &chain.volume.header), 0);
    sceNgs2VoiceGetPortInfo(h_voice, 3, &port_info, sizeof(port_info));
    record("chain_applied", port_info.volume == 2.0f, 1);
    chain.delay.header.off_next = -(int16_t)offsetof(__typeof__(chain), delay);
    record("chain_cycle", (uint32_t)sceNgs2VoiceControl(h_voice, &chain.volume.header), E(0x30b));

    patch.i_port = 2, patch.h_dest = h_sub_voice;
    sceNgs2VoiceControl(h_voice, &patch.header);
    sceNgs2RackDestroy(h_submixer, NULL);
    sceNgs2VoiceGetPortInfo(h_voice, 2, &port_info, sizeof(port_info));
    record("patch_cleared_by_rack_destroy", port_info.h_dest, 0);

    sceNgs2SystemDestroy(h_system, NULL);
    sceNgs2SystemDestroy(h_other_system, NULL);
    sceNgs2RackDestroy(h_sampler, NULL);
    sceNgs2RackDestroy(h_plain_sampler, NULL);
    sceNgs2RackDestroy(h_mastering, NULL);
    sceNgs2RackDestroy(h_other_mastering, NULL);
}

static int s_c_reports;
static uint32_t s_report_result;
static char s_sz_report_message[256];
static Ngs2Handle s_h_destroy_in_report;
static int s_destroy_in_report_result;

static void on_report(const Ngs2ReportMessage *p_report, uintptr_t user_data) {
    (void)user_data;
    s_c_reports++;
    s_report_result = (uint32_t)p_report->result;
    if (p_report->cb_data == 0x20 && p_report->type == 0 && p_report->sz_message) {
        snprintf(s_sz_report_message, sizeof(s_sz_report_message), "%s", p_report->sz_message);
    }
}

/* The destroy fails and reports too, so the handler disarms itself first. */
static void destroy_from_report(const Ngs2ReportMessage *p_report, uintptr_t user_data) {
    (void)p_report, (void)user_data;
    const Ngs2Handle h_system = s_h_destroy_in_report;
    if (h_system) {
        s_h_destroy_in_report = 0;
        s_destroy_in_report_result = sceNgs2SystemDestroy(h_system, NULL);
    }
}

static void test_reports(void) {
    Ngs2Handle handles[9] = {0};
    record("report_type_9", (uint32_t)sceNgs2ReportRegisterHandler(9, on_report, 0, &handles[0]), E(0x202));
    record("report_null_handler", (uint32_t)sceNgs2ReportRegisterHandler(0, NULL, 0, &handles[0]), E(0x203));
    record("report_unregister_0", (uint32_t)sceNgs2ReportUnregisterHandler(0), E(0x204));
    record("report_unregister_bogus", (uint32_t)sceNgs2ReportUnregisterHandler(0x1234), E(0x204));
    for (int i = 0; i < 8; i++) sceNgs2ReportRegisterHandler(3, on_report, 0, &handles[i]);
    handles[8] = 5;
    record("report_ninth", (uint32_t)sceNgs2ReportRegisterHandler(3, on_report, 0, &handles[8]), E(0x205));
    record("report_ninth_handle_zeroed", handles[8], 0);
    for (int i = 0; i < 8; i++) sceNgs2ReportUnregisterHandler(handles[i]);
    record("report_unregister_twice", (uint32_t)sceNgs2ReportUnregisterHandler(handles[0]), E(0x204));
    Ngs2Handle h_out = 0;
    record("report_null_out", (uint32_t)sceNgs2ReportRegisterHandler(4, on_report, 0, NULL), 0);

    Ngs2Handle h_report = 0, h_system = 0;
    sceNgs2ReportRegisterHandler(0, on_report, 0x42, &h_report);
    sceNgs2SystemCreateWithAllocator(NULL, &s_allocator, &h_system);
    s_c_reports = 0;
    sceNgs2SystemGetInfo(h_system, NULL, 0);
    record("report_error_count", (uint32_t)s_c_reports, 1);
    record("report_error_result", s_report_result, E(0x53));
    fprintf(s_p_results, "report_error_message,\"%s\",,\n", s_sz_report_message);

    sceNgs2ReportRegisterHandler(0, destroy_from_report, 0, &h_out);
    s_h_destroy_in_report = h_system;
    static float s_out[256 * 2];
    const Ngs2RenderBufferInfo render = {s_out, sizeof(s_out), 0x18, 3};
    record("render_error_with_destroy", (uint32_t)sceNgs2SystemRender(h_system, &render, 1), E(0x52));
    record("destroy_inside_render", (uint32_t)s_destroy_in_report_result, E(0x30f));
    sceNgs2ReportUnregisterHandler(h_out);
    sceNgs2ReportUnregisterHandler(h_report);
    sceNgs2SystemDestroy(h_system, NULL);
}

int main(void) {
    int rc = sceSysmoduleLoadModule(SYSMODULE_NGS2);
    if (rc != 0) {
        printf("ngs2_api: loading libSceNgs2 failed: %#x\n", (unsigned)rc);
        return 1;
    }
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_DIR, 0777);
    s_p_results = fopen(SZ_OUT_DIR "/results.csv", "w");
    if (!s_p_results) {
        printf("ngs2_api: cannot write %s/results.csv (errno %d)\n", SZ_OUT_DIR, errno);
        return 1;
    }
    fprintf(s_p_results, "case,value,expected,match\n");
    test_system();
    test_locks();
    test_rack_options();
    test_racks();
    test_voices();
    test_reports();
    fclose(s_p_results);
    printf("ngs2_api: done, %d mismatch(es) against the expected firmware values\n", s_c_mismatches);
    return s_c_mismatches != 0;
}
