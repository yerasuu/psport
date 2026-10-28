/* Hardware test for libSceNgs2's waveforms (kb #1096, phase F4 of #625): parses synthetic RIFF WAVE,
 * ATRAC9 and VAGp files from memory, a file and a read handler, asks for frame shapes and blocks, and
 * renders VAG, the generators and, when one is given, an .at9 file, so that a PS4, psport with the
 * firmware module (PS_LLE=libSceNgs2) and psport's native Ngs2 (PS_NGS2=native) can be compared. Each
 * render writes its PCM to /data/psport-hwtest/ngs2_waveform/<case>.csv and every check goes to
 * results.csv. rea has not read the parse functions yet: their expected values are psport's reading. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SZ_OUT_DIR "/data/psport-hwtest/ngs2_waveform"
#define SZ_INPUT_AT9 SZ_OUT_DIR "/input.at9"
#define SYSMODULE_NGS2 0x000B
#define GRAIN 256
#define RATE_HZ 48000
#define MAX_CHANNELS 8
#define INFINITE 0xffffffffu

#define RACK_SAMPLER 0x1000
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
    uint32_t waveform_type, c_channels, rate_hz, config_data, off_frame, cb_frame_margin;
} Ngs2WaveformFormat;

typedef struct {
    Ngs2ParamHeader header;
    Ngs2WaveformFormat format;
    uint32_t flags, reserved;
} Ngs2SamplerSetupParam;

typedef struct {
    uint32_t off_data, cb_data, c_repeats, c_skip_samples, c_samples, reserved;
    uintptr_t user_data;
} Ngs2WaveformBlock;

typedef struct {
    Ngs2WaveformFormat format;
    uint32_t off_data, cb_data;
    uint32_t loop_begin, loop_end, c_samples;
    uint32_t cb_unit, c_unit_samples, c_units_per_frame;
    uint32_t cb_frame, c_frame_samples;
    uint32_t c_delay_samples;
    uint32_t c_blocks;
    Ngs2WaveformBlock blocks[4];
} Ngs2WaveformInfo;

typedef struct {
    Ngs2ParamHeader header;
    const void *p_data;
    uint32_t flags, c_blocks;
    const Ngs2WaveformBlock *p_blocks;
} Ngs2BlocksParam;

typedef struct {
    void *p_buffer;
    size_t cb_buffer;
    uint32_t waveform_type, c_channels;
} Ngs2RenderBufferInfo;

typedef int32_t (*Ngs2ParseReadHandler)(uintptr_t user_data, uint32_t off, void *p_data, size_t cb_data);

_Static_assert(sizeof(Ngs2SamplerRackOption) == 0xa8, "sampler option");
_Static_assert(sizeof(Ngs2SamplerSetupParam) == 0x28, "sampler setup");
_Static_assert(sizeof(Ngs2WaveformBlock) == 0x20, "waveform block");
_Static_assert(sizeof(Ngs2WaveformInfo) == 0xc8, "waveform info");
_Static_assert(offsetof(Ngs2WaveformInfo, blocks) == 0x48, "waveform info blocks");

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
int sceNgs2ParseWaveformData(const void *p_data, size_t cb_data, Ngs2WaveformInfo *p_out_info);
int sceNgs2ParseWaveformFile(const char *sz_path, uint64_t off_file, Ngs2WaveformInfo *p_out_info);
int sceNgs2ParseWaveformUser(Ngs2ParseReadHandler pfn_read, uintptr_t user_data, Ngs2WaveformInfo *p_out_info);
int sceNgs2CalcWaveformBlock(const Ngs2WaveformFormat *p_format, uint32_t i_sample, uint32_t c_samples, Ngs2WaveformBlock *p_out_block);
int sceNgs2GetWaveformFrameInfo(const Ngs2WaveformFormat *p_format, uint32_t *p_out_cb_frame, uint32_t *p_out_c_frame_samples,
                                uint32_t *p_out_c_units_per_frame, uint32_t *p_out_c_delay_samples);

static FILE *s_p_results;
static int s_c_mismatches;

static void record(const char *sz_case, uint32_t value, uint32_t expected) {
    const int is_match = value == expected;
    s_c_mismatches += !is_match;
    fprintf(s_p_results, "%s,%#x,%#x,%s\n", sz_case, value, expected, is_match ? "ok" : "MISMATCH");
}

/* A row nothing predicts: the console's value is what psport learns from it. */
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

static uint32_t event(Ngs2Handle h_voice, uint32_t event_id) {
    Ngs2EventParam param;
    header(&param.header, sizeof(param), 6);
    param.event_id = event_id;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t exit_loop(Ngs2Handle h_voice) {
    Ngs2ParamHeader param;
    header(&param, sizeof(param), 0x10000004);
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param);
}

static uint32_t patch(Ngs2Handle h_voice, Ngs2Handle h_dest) {
    Ngs2PatchParam param;
    header(&param.header, sizeof(param), 5);
    param.i_port = 0, param.i_dest_input = 0, param.h_dest = h_dest;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t setup_format(Ngs2Handle h_voice, const Ngs2WaveformFormat *p_format, uint32_t flags) {
    Ngs2SamplerSetupParam param = {0};
    header(&param.header, sizeof(param), 0x10000000);
    param.format = *p_format;
    param.flags = flags;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t add_blocks(Ngs2Handle h_voice, const void *p_data, const Ngs2WaveformBlock *p_blocks, uint32_t c_blocks) {
    Ngs2BlocksParam param;
    header(&param.header, sizeof(param), 0x10000001);
    param.p_data = p_data, param.flags = 0, param.c_blocks = c_blocks, param.p_blocks = p_blocks;
    return (uint32_t)sceNgs2VoiceControl(h_voice, &param.header);
}

static uint32_t flags_of(Ngs2Handle h_voice) {
    uint32_t flags = 0xdead;
    sceNgs2VoiceGetStateFlags(h_voice, &flags);
    return flags;
}

typedef struct {
    Ngs2Handle h_system, h_sampler, h_mastering, h_voice, h_master_voice;
} Chain;

/* A sampler voice patched to a playing mastering voice of c_channels whose limiter and LFE low-pass
 * are off, so that the buffer holds the sampler's own output. */
static Chain create_chain(const Ngs2SamplerRackOption *p_option, uint32_t c_channels) {
    Chain chain = {0};
    Ngs2SystemOption option = {.cb_option = sizeof(option), .max_grain_samples = 1024, .num_grain_samples = GRAIN, .rate_hz = RATE_HZ};
    record("setup_system", (uint32_t)sceNgs2SystemCreateWithAllocator(&option, &s_allocator, &chain.h_system), 0);
    record("setup_sampler", (uint32_t)sceNgs2RackCreateWithAllocator(chain.h_system, RACK_SAMPLER, p_option, &s_allocator,
                                                                      &chain.h_sampler), 0);
    record("setup_mastering", (uint32_t)sceNgs2RackCreateWithAllocator(chain.h_system, RACK_MASTERING, NULL, &s_allocator,
                                                                        &chain.h_mastering), 0);
    sceNgs2RackGetVoiceHandle(chain.h_sampler, 0, &chain.h_voice);
    sceNgs2RackGetVoiceHandle(chain.h_mastering, 0, &chain.h_master_voice);
    record("setup_mastering_voice", control_word(chain.h_master_voice, 0x30000000, c_channels, 0), 0);
    record("setup_mastering_lfe_off", control_word(chain.h_master_voice, 0x30000002, 0, 120), 0);
    record("setup_mastering_limiter_off", control_word(chain.h_master_voice, 0x30000003, 0, 0x3f800000), 0);
    record("setup_mastering_play", event(chain.h_master_voice, 0), 0);
    record("setup_patch", patch(chain.h_voice, chain.h_master_voice), 0);
    return chain;
}

static void destroy_chain(const Chain *p_chain) {
    sceNgs2RackDestroy(p_chain->h_sampler, NULL);
    sceNgs2RackDestroy(p_chain->h_mastering, NULL);
    sceNgs2SystemDestroy(p_chain->h_system, NULL);
}

/* Renders c_grains into p_out (interleaved F32) and writes them to <case>.csv. */
static void render_f32(const char *sz_case, Ngs2Handle h_system, uint32_t c_channels, uint32_t c_grains, float *p_out) {
    char sz_path[256];
    snprintf(sz_path, sizeof(sz_path), SZ_OUT_DIR "/%s.csv", sz_case);
    FILE *p_csv = fopen(sz_path, "w");
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

static float s_pcm[400 * GRAIN * 2];

/* ---- synthetic files ---- */

typedef struct {
    uint8_t bytes[8192];
    size_t cb;
} File;

static void put8(File *p_file, uint32_t value) {
    if (p_file->cb < sizeof(p_file->bytes)) p_file->bytes[p_file->cb++] = (uint8_t)value;
}
static void put16(File *p_file, uint32_t value) { put8(p_file, value), put8(p_file, value >> 8); }
static void put32(File *p_file, uint32_t value) { put16(p_file, value), put16(p_file, value >> 16); }
static void put32_be(File *p_file, uint32_t value) {
    put8(p_file, value >> 24), put8(p_file, value >> 16), put8(p_file, value >> 8), put8(p_file, value);
}
static void put_tag(File *p_file, const char *sz_tag) {
    for (int i = 0; i < 4; i++) put8(p_file, (uint8_t)sz_tag[i]);
}
static void patch32(File *p_file, size_t off, uint32_t value) {
    for (int i = 0; i < 4; i++) p_file->bytes[off + i] = (uint8_t)(value >> (8 * i));
}

static const uint8_t s_atrac9_subformat[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                               0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

/* RIFF WAVE: fmt (tag, channels, rate, bits; extensible when is_extensible, its subformat the tag),
 * a smpl loop when loop_end > 0, an odd-sized LIST chunk, then cb_data bytes of data. */
static void make_wave(File *p_file, uint32_t tag, uint32_t c_channels, uint32_t rate_hz, uint32_t c_bits, int is_extensible,
                      uint32_t loop_begin, uint32_t loop_end, size_t cb_data) {
    p_file->cb = 0;
    put_tag(p_file, "RIFF"), put32(p_file, 0), put_tag(p_file, "WAVE");
    const uint32_t cb_align = c_channels * c_bits / 8;
    put_tag(p_file, "fmt "), put32(p_file, is_extensible ? 40 : 16);
    put16(p_file, is_extensible ? 0xfffe : tag), put16(p_file, c_channels), put32(p_file, rate_hz);
    put32(p_file, rate_hz * cb_align), put16(p_file, cb_align), put16(p_file, c_bits);
    if (is_extensible) {
        put16(p_file, 22), put16(p_file, c_bits), put32(p_file, 3);
        const uint8_t guid[16] = {(uint8_t)tag, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        for (int i = 0; i < 16; i++) put8(p_file, guid[i]);
    }
    if (loop_end > 0) {
        put_tag(p_file, "smpl"), put32(p_file, 60);
        for (int i = 0; i < 7; i++) put32(p_file, 0);
        put32(p_file, 1), put32(p_file, 0);
        put32(p_file, 0), put32(p_file, 0), put32(p_file, loop_begin), put32(p_file, loop_end), put32(p_file, 0), put32(p_file, 0);
    }
    put_tag(p_file, "LIST"), put32(p_file, 3), put8(p_file, 'a'), put8(p_file, 'b'), put8(p_file, 'c'), put8(p_file, 0);
    put_tag(p_file, "data"), put32(p_file, (uint32_t)cb_data);
    for (size_t i = 0; i < cb_data; i++) put8(p_file, (uint8_t)(i * 7));
    patch32(p_file, 4, (uint32_t)(p_file->cb - 8));
}

/* An .at9 header as the files of the PS4 SDK's tools lay it out: 2 channels, 48 kHz, config
 * FE 74 0D F0 (superframes of 4 frames of 112 bytes, 256 samples each), fact {samples, 256, 256}, a
 * smpl loop counted with the 256 samples of delay, and 12 superframes of zeros. */
static void make_at9(File *p_file) {
    p_file->cb = 0;
    put_tag(p_file, "RIFF"), put32(p_file, 0), put_tag(p_file, "WAVE");
    put_tag(p_file, "fmt "), put32(p_file, 52);
    put16(p_file, 0xfffe), put16(p_file, 2), put32(p_file, 48000), put32(p_file, 21000), put16(p_file, 448), put16(p_file, 0);
    put16(p_file, 34), put16(p_file, 1024), put32(p_file, 3);
    for (int i = 0; i < 16; i++) put8(p_file, s_atrac9_subformat[i]);
    put32(p_file, 1), put32_be(p_file, 0xfe740df0), put32(p_file, 0);
    put_tag(p_file, "fact"), put32(p_file, 12), put32(p_file, 11000), put32(p_file, 256), put32(p_file, 256);
    put_tag(p_file, "smpl"), put32(p_file, 60);
    for (int i = 0; i < 7; i++) put32(p_file, 0);
    put32(p_file, 1), put32(p_file, 0);
    put32(p_file, 0), put32(p_file, 0), put32(p_file, 1000 + 256), put32(p_file, 9999 + 256), put32(p_file, 0), put32(p_file, 0);
    put_tag(p_file, "data"), put32(p_file, 12 * 448);
    for (int i = 0; i < 12 * 448; i++) put8(p_file, 0);
    patch32(p_file, 4, (uint32_t)(p_file->cb - 8));
}

/* VAGp, mono at rate_hz, then c_frames 16-byte blocks: predictor 1, shift 8, nibbles from a counter;
 * flags 6 on loop_start and 3 on loop_end when they are below c_frames. */
static void make_vag(File *p_file, uint32_t rate_hz, uint32_t c_frames, uint32_t loop_start, uint32_t loop_end) {
    p_file->cb = 0;
    put_tag(p_file, "VAGp"), put32_be(p_file, 0x20), put32_be(p_file, 0), put32_be(p_file, c_frames * 16), put32_be(p_file, rate_hz);
    for (int i = 0; i < 12; i++) put8(p_file, 0);
    const char sz_name[16] = "ngs2_waveform";
    for (int i = 0; i < 16; i++) put8(p_file, (uint8_t)sz_name[i]);
    for (uint32_t f = 0; f < c_frames; f++) {
        put8(p_file, 0x18), put8(p_file, f == loop_start ? 6 : f == loop_end ? 3 : 0);
        for (int i = 0; i < 14; i++) put8(p_file, (uint8_t)((f * 14 + i) * 37));
    }
}

static void record_info(const char *sz_case, const Ngs2WaveformInfo *p_info, const uint32_t *p_expected, int is_open) {
    static const char *const s_sz_fields[] = {"type",       "channels", "rate",       "config",      "data_offset", "data_size",
                                              "loop_begin", "loop_end", "samples",    "unit_size",   "unit_samples",
                                              "units",      "frame_size", "frame_samples", "delay",  "blocks"};
    const uint32_t values[] = {p_info->format.waveform_type, p_info->format.c_channels, p_info->format.rate_hz, p_info->format.config_data,
                               p_info->off_data,  p_info->cb_data,   p_info->loop_begin, p_info->loop_end,   p_info->c_samples,
                               p_info->cb_unit,   p_info->c_unit_samples, p_info->c_units_per_frame, p_info->cb_frame,
                               p_info->c_frame_samples, p_info->c_delay_samples, p_info->c_blocks};
    char sz_row[96];
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        snprintf(sz_row, sizeof(sz_row), "%s_%s", sz_case, s_sz_fields[i]);
        if (is_open) record_open(sz_row, values[i]);
        else record(sz_row, values[i], p_expected[i]);
    }
    for (uint32_t i = 0; i < p_info->c_blocks && i < 4; i++) {
        const Ngs2WaveformBlock *p_block = &p_info->blocks[i];
        snprintf(sz_row, sizeof(sz_row), "%s_block%u_offset", sz_case, i), record_open(sz_row, p_block->off_data);
        snprintf(sz_row, sizeof(sz_row), "%s_block%u_size", sz_case, i), record_open(sz_row, p_block->cb_data);
        snprintf(sz_row, sizeof(sz_row), "%s_block%u_repeats", sz_case, i), record_open(sz_row, p_block->c_repeats);
        snprintf(sz_row, sizeof(sz_row), "%s_block%u_skip", sz_case, i), record_open(sz_row, p_block->c_skip_samples);
        snprintf(sz_row, sizeof(sz_row), "%s_block%u_samples", sz_case, i), record_open(sz_row, p_block->c_samples);
    }
}

static File s_file;

static int32_t read_file_bytes(uintptr_t user_data, uint32_t off, void *p_data, size_t cb_data) {
    const File *p_file = (const File *)user_data;
    if (off > p_file->cb || cb_data > p_file->cb - off) return (int32_t)0x80020005;
    memcpy(p_data, p_file->bytes + off, cb_data);
    return 0;
}

static int32_t read_fails(uintptr_t user_data, uint32_t off, void *p_data, size_t cb_data) {
    (void)user_data, (void)off, (void)p_data, (void)cb_data;
    return (int32_t)0x80020005;
}

/* PCM: 16-bit stereo with a loop at 100..899 of 1000, then the other sample formats. Expected values
 * are psport's reading of the formats and the SDK structure. */
static void test_parse_pcm(void) {
    Ngs2WaveformInfo info;
    make_wave(&s_file, 1, 2, 44100, 16, 0, 100, 899, 4000);
    record("parse_pcm16_result", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info), 0);
    const uint32_t expected[] = {0x12, 2, 44100, 0, (uint32_t)s_file.cb - 4000, 4000, 100, 899, 1000, 4, 1, 1, 4, 1, 0, 3};
    record_info("parse_pcm16", &info, expected, 0);
    static const struct {
        const char *sz_case;
        uint32_t tag, c_bits, is_extensible, type;
    } s_kinds[] = {{"parse_u8", 1, 8, 0, 0x11},       {"parse_s24", 1, 24, 0, 0x14},      {"parse_s32", 1, 32, 0, 0x16},
                   {"parse_f32", 3, 32, 0, 0x18},      {"parse_f64", 3, 64, 0, 0x1a},      {"parse_ext_s24", 1, 24, 1, 0x14},
                   {"parse_ext_f32", 3, 32, 1, 0x18}};
    char sz_row[64];
    for (size_t i = 0; i < sizeof(s_kinds) / sizeof(s_kinds[0]); i++) {
        make_wave(&s_file, s_kinds[i].tag, 1, 48000, s_kinds[i].c_bits, (int)s_kinds[i].is_extensible, 0, 0, 2400);
        snprintf(sz_row, sizeof(sz_row), "%s_result", s_kinds[i].sz_case);
        record(sz_row, (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info), 0);
        snprintf(sz_row, sizeof(sz_row), "%s_type", s_kinds[i].sz_case);
        record(sz_row, info.format.waveform_type, s_kinds[i].type);
        snprintf(sz_row, sizeof(sz_row), "%s_samples", s_kinds[i].sz_case);
        record(sz_row, info.c_samples, 2400 / (s_kinds[i].c_bits / 8));
    }
}

/* ATRAC9 from memory, from a file at an offset and through a read handler, which must agree. */
static void test_parse_at9(void) {
    Ngs2WaveformInfo info, info_file, info_user;
    make_at9(&s_file);
    record("parse_at9_result", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info), 0);
    /* psport moves the smpl loop back by the delay, as the .at9 files of Ryza and Saga need. */
    const uint32_t expected[] = {0x40, 2, 48000, 0xfe740df0, (uint32_t)s_file.cb - 12 * 448, 12 * 448, 1000, 9999, 11000,
                                 112, 256, 4, 448, 1024, 256, 3};
    record_info("parse_at9", &info, expected, 0);
    FILE *p_out = fopen(SZ_OUT_DIR "/synthetic.at9", "wb");
    if (p_out) {
        fwrite("0123456789", 1, 10, p_out);
        fwrite(s_file.bytes, 1, s_file.cb, p_out);
        fclose(p_out);
    }
    record("parse_at9_file_result", (uint32_t)sceNgs2ParseWaveformFile(SZ_OUT_DIR "/synthetic.at9", 10, &info_file), 0);
    record("parse_at9_file_same", memcmp(&info, &info_file, sizeof(info)) == 0, 1);
    record("parse_at9_user_result", (uint32_t)sceNgs2ParseWaveformUser(read_file_bytes, (uintptr_t)&s_file, &info_user), 0);
    record("parse_at9_user_same", memcmp(&info, &info_user, sizeof(info)) == 0, 1);
}

/* VAGp with flags 6 on frame 3 and 3 on frame 6 of 10. */
static void test_parse_vag(void) {
    Ngs2WaveformInfo info;
    make_vag(&s_file, 22050, 10, 3, 6);
    record("parse_vag_result", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info), 0);
    const uint32_t expected[] = {0x1c, 1, 22050, 1, 0x30, 160, 84, 195, 280, 16, 28, 1, 16, 28, 0, 1};
    record_info("parse_vag", &info, expected, 0);
}

/* The codes of malformed input: psport's choices until rea reads 0xfac0, so every row is open. */
static void test_parse_errors(void) {
    Ngs2WaveformInfo info;
    make_wave(&s_file, 1, 2, 48000, 16, 0, 0, 0, 64);
    record_open("parse_error_truncated_8", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, 8, &info));
    record_open("parse_error_truncated_fmt", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, 30, &info));
    record_open("parse_error_null_info", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, NULL));
    record_open("parse_error_null_data", (uint32_t)sceNgs2ParseWaveformData(NULL, s_file.cb, &info));
    record_open("parse_error_null_handler", (uint32_t)sceNgs2ParseWaveformUser(NULL, 0, &info));
    record_open("parse_error_handler_fails", (uint32_t)sceNgs2ParseWaveformUser(read_fails, 0, &info));
    record_open("parse_error_no_file", (uint32_t)sceNgs2ParseWaveformFile(SZ_OUT_DIR "/missing.wav", 0, &info));
    record_open("parse_error_data_past_end", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb - 1, &info));
    s_file.bytes[0] = 'X';
    record_open("parse_error_magic", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info));
    make_wave(&s_file, 1, 1, 48000, 12, 0, 0, 0, 64);
    record_open("parse_error_bits_12", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info));
    make_wave(&s_file, 2, 1, 48000, 4, 0, 0, 0, 64);
    record_open("parse_error_tag_adpcm", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info));
    make_wave(&s_file, 1, 0, 48000, 16, 0, 0, 0, 64);
    record_open("parse_error_channels_0", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info));
    make_wave(&s_file, 1, 9, 48000, 16, 0, 0, 0, 64);
    record_open("parse_error_channels_9", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info));
    make_wave(&s_file, 1, 1, 0, 16, 0, 0, 0, 64);
    record_open("parse_error_rate_0", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info));
    make_at9(&s_file);
    s_file.bytes[0x40] = 0x00;
    record_open("parse_error_at9_config", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info));
}

static void frame_info_row(const char *sz_case, Ngs2WaveformFormat format, const uint32_t *p_expected) {
    uint32_t values[4] = {0xdead, 0xdead, 0xdead, 0xdead};
    const uint32_t result = (uint32_t)sceNgs2GetWaveformFrameInfo(&format, &values[0], &values[1], &values[2], &values[3]);
    static const char *const s_sz_fields[] = {"result", "frame_size", "frame_samples", "units", "delay"};
    char sz_row[64];
    snprintf(sz_row, sizeof(sz_row), "%s_%s", sz_case, s_sz_fields[0]);
    record(sz_row, result, p_expected[0]);
    for (int i = 0; i < 4 && result == 0; i++) {
        snprintf(sz_row, sizeof(sz_row), "%s_%s", sz_case, s_sz_fields[i + 1]);
        record(sz_row, values[i], p_expected[i + 1]);
    }
}

static void calc_row(const char *sz_case, Ngs2WaveformFormat format, uint32_t i_sample, uint32_t c_samples, const uint32_t *p_expected) {
    Ngs2WaveformBlock block;
    memset(&block, 0xcd, sizeof(block));
    const uint32_t result = (uint32_t)sceNgs2CalcWaveformBlock(&format, i_sample, c_samples, &block);
    const uint32_t values[] = {result, block.off_data, block.cb_data, block.c_skip_samples, block.c_samples, block.c_repeats};
    static const char *const s_sz_fields[] = {"result", "offset", "size", "skip", "samples", "repeats"};
    char sz_row[64];
    for (int i = 0; i < 6 && (i == 0 || result == 0); i++) {
        snprintf(sz_row, sizeof(sz_row), "%s_%s", sz_case, s_sz_fields[i]);
        record(sz_row, values[i], p_expected[i]);
    }
}

/* Frame shapes follow the sampler's own (spec 05 §2, §6.1); the ATRAC9 delay and the block a
 * position maps to are psport's reading. */
static void test_frames(void) {
    const Ngs2WaveformFormat pcm = {0x12, 2, 48000, 0, 0, 0};
    const Ngs2WaveformFormat vag = {0x1c, 3, 48000, 0, 2, 8};
    const Ngs2WaveformFormat at9 = {0x40, 2, 48000, 0xfe740df0, 0, 0};
    frame_info_row("frame_pcm16", pcm, (const uint32_t[]){0, 4, 1, 1, 0});
    frame_info_row("frame_vag", vag, (const uint32_t[]){0, 56, 28, 1, 0});
    frame_info_row("frame_at9", at9, (const uint32_t[]){0, 448, 1024, 4, 256});
    frame_info_row("frame_generator", (Ngs2WaveformFormat){0x81, 1, 440, 0, 0, 0}, (const uint32_t[]){E(0x402)});
    frame_info_row("frame_bad_type", (Ngs2WaveformFormat){0x1d, 1, 48000, 0, 0, 0}, (const uint32_t[]){E(0x402)});
    frame_info_row("frame_bad_channels", (Ngs2WaveformFormat){0x12, 9, 48000, 0, 0, 0}, (const uint32_t[]){E(0x52)});
    frame_info_row("frame_bad_rate", (Ngs2WaveformFormat){0x12, 1, 0, 0, 0, 0}, (const uint32_t[]){E(0x404)});
    frame_info_row("frame_bad_offset", (Ngs2WaveformFormat){0x12, 1, 48000, 0, 5, 4}, (const uint32_t[]){E(0x405)});
    frame_info_row("frame_at9_no_fe", (Ngs2WaveformFormat){0x40, 2, 48000, 0x00740df0, 0, 0}, (const uint32_t[]){E(0x403)});
    frame_info_row("frame_at9_channels", (Ngs2WaveformFormat){0x40, 1, 48000, 0xfe740df0, 0, 0}, (const uint32_t[]){E(0x52)});
    frame_info_row("frame_at9_rate_index_5", (Ngs2WaveformFormat){0x40, 2, 48000, 0xfe540df0, 0, 0}, (const uint32_t[]){E(0x403)});
    record_open("frame_null_format", (uint32_t)sceNgs2GetWaveformFrameInfo(NULL, NULL, NULL, NULL, NULL));
    record_open("frame_null_outputs", (uint32_t)sceNgs2GetWaveformFrameInfo(&at9, NULL, NULL, NULL, NULL));
    calc_row("calc_pcm16", pcm, 1000, 500, (const uint32_t[]){0, 4000, 2000, 0, 500, 0});
    calc_row("calc_vag", (Ngs2WaveformFormat){0x1c, 1, 48000, 0, 0, 0}, 30, 60, (const uint32_t[]){0, 16, 48, 2, 60, 0});
    calc_row("calc_at9", at9, 1000, 2000, (const uint32_t[]){0, 448, 3 * 448, 232, 2000, 0});
    calc_row("calc_overflow", (Ngs2WaveformFormat){0x12, 1, 48000, 0, 0, 0}, 0xffffffffu, 0xffffffffu,
             (const uint32_t[]){E(0x407)});
    record_open("calc_null_block", (uint32_t)sceNgs2CalcWaveformBlock(&pcm, 0, 10, NULL));
}

static Ngs2SamplerRackOption sampler_option(uint32_t c_atrac9, uint32_t c_ajm) {
    Ngs2SamplerRackOption option = {0};
    option.rack.cb_option = sizeof(option);
    option.rack.max_grain_samples = 512, option.rack.max_voices = 2, option.rack.max_matrices = 1, option.rack.max_ports = 1;
    option.max_channel_works = 4, option.max_waveform_blocks = 4, option.max_envelope_points = 4, option.max_filters = 1;
    option.max_atrac9_decoders = c_atrac9, option.max_atrac9_channel_works = c_atrac9 * 2;
    option.max_ajm_atrac9_decoders = c_ajm, option.max_codec_caches = c_ajm ? (c_ajm * 8 > 10 ? c_ajm * 8 : 10) : 0;
    option.num_peak_meter_blocks = 1;
    return option;
}

/* The VAGp above rendered at its own rate: with configData bit 0 its flags loop frames 3 to 6 until
 * an exit request ends the block at the loop end (spec 05 §3.3, §4); without it all ten frames play
 * once, every sample of them, since whole frames leave the block's tail in the history (the first two
 * are 0). The first frames are checked against a decoding written here from spec 05 §4 (predictor 1
 * is 0.9375 y[n-1]), from output 3 on (the four samples of history the voice starts with). */
static void test_render_vag(void) {
    const Ngs2SamplerRackOption option = sampler_option(0, 0);
    Chain chain = create_chain(&option, 1);
    Ngs2WaveformInfo info;
    make_vag(&s_file, 48000, 10, 3, 6);
    record("vag_parse", (uint32_t)sceNgs2ParseWaveformData(s_file.bytes, s_file.cb, &info), 0);
    record("vag_setup", setup_format(chain.h_voice, &info.format, 0), 0);
    record("vag_blocks", add_blocks(chain.h_voice, s_file.bytes, info.blocks, info.c_blocks), 0);
    record("vag_play", event(chain.h_voice, 0), 0);
    render_f32("vag_loop", chain.h_system, 1, 3, s_pcm);
    double y1 = 0, max_error = 0;
    for (uint32_t i = 0; i < 3 * 28; i++) {
        const uint8_t byte = s_file.bytes[0x30 + (i / 28) * 16 + 2 + (i % 28) / 2];
        int nibble = (i & 1) ? byte >> 4 : byte & 0xf;
        if (nibble >= 8) nibble -= 16;
        y1 = nibble / 8.0 / 256.0 + 0.9375 * y1;
        max_error = fmax(max_error, fabs(s_pcm[i + 3] - y1));
    }
    record_near("vag_first_frames_error", max_error, 0, 1e-6);
    record("vag_loop_flags_playing", flags_of(chain.h_voice), 3);
    record("vag_exit", exit_loop(chain.h_voice), 0);
    render_f32("vag_loop_exit", chain.h_system, 1, 2, s_pcm);
    record("vag_loop_flags_after_exit", flags_of(chain.h_voice), 0);
    Ngs2WaveformFormat plain = info.format;
    plain.config_data = 0;
    record("vag_plain_setup", setup_format(chain.h_voice, &plain, 0), 0);
    record("vag_plain_blocks", add_blocks(chain.h_voice, s_file.bytes, info.blocks, info.c_blocks), 0);
    record("vag_plain_play", event(chain.h_voice, 0), 0);
    render_f32("vag_plain", chain.h_system, 1, 2, s_pcm);
    uint32_t c_nonzero = 0;
    for (int i = 0; i < 2 * GRAIN; i++) c_nonzero += s_pcm[i] != 0;
    record("vag_plain_nonzero", c_nonzero, 278);
    destroy_chain(&chain);
}

/* The generators from spec 05 §5 at 1 kHz from 90 degrees and the rest; the noise registers' seeds
 * are not read yet, so their first values are open. */
static void test_generators(void) {
    const Ngs2SamplerRackOption option = sampler_option(0, 0);
    Chain chain = create_chain(&option, 1);
    static const struct {
        const char *sz_case;
        uint32_t type, rate_hz, config;
    } s_cases[] = {{"gen_sine", 0x81, 1000, 90},  {"gen_saw_up", 0x82, 300, 0},   {"gen_saw_down", 0x83, 300, 45},
                   {"gen_triangle", 0x84, 1200, 0}, {"gen_square", 0x85, 750, 400}, {"gen_none", 0x80, 100, 0},
                   {"gen_noise", 0x86, 1, 0},       {"gen_periodic_noise_63", 0x87, 1, 63}, {"gen_periodic_noise_20", 0x87, 1, 20}};
    char sz_row[64];
    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        const Ngs2WaveformFormat format = {s_cases[i].type, 1, s_cases[i].rate_hz, s_cases[i].config, 0, 0};
        snprintf(sz_row, sizeof(sz_row), "%s_setup", s_cases[i].sz_case);
        record(sz_row, setup_format(chain.h_voice, &format, 0), 0);
        record("gen_play", event(chain.h_voice, 0), 0);
        render_f32(s_cases[i].sz_case, chain.h_system, 1, 2, s_pcm);
        snprintf(sz_row, sizeof(sz_row), "%s_flags", s_cases[i].sz_case);
        record(sz_row, flags_of(chain.h_voice), 3);
        for (int k = 0; k < 4; k++) {
            snprintf(sz_row, sizeof(sz_row), "%s_sample%d", s_cases[i].sz_case, k);
            uint32_t bits;
            memcpy(&bits, &s_pcm[k], 4);
            record_open(sz_row, bits);
        }
        if (s_cases[i].type == 0x81) {
            double max_error = 0;
            for (int k = 0; k < 2 * GRAIN; k++) max_error = fmax(max_error, fabs(s_pcm[k] - cos(2 * M_PI * 1000.0 * k / RATE_HZ)));
            record_near("gen_sine_error", max_error, 0, 1e-3);
        }
        record("gen_kill", event(chain.h_voice, 3), 0);
        render_f32("gen_after_kill", chain.h_system, 1, 1, s_pcm);
    }
    destroy_chain(&chain);
}

static uint8_t *s_p_at9;

/* With SZ_INPUT_AT9 on the console (any .at9, a game's sce_sys/snd0.at9 will do): parsed from the
 * file, played for 400 grains through the internal decoders and then the AJM ones. The first non-zero
 * output of each tells the AJM path's latency (a grain later on the firmware, spec 05 §6.3). */
static void test_atrac9_input(void) {
    FILE *p_in = fopen(SZ_INPUT_AT9, "rb");
    if (!p_in) {
        record_open("at9_input_present", 0);
        return;
    }
    fseek(p_in, 0, SEEK_END);
    const long cb_file = ftell(p_in);
    fseek(p_in, 0, SEEK_SET);
    s_p_at9 = cb_file > 0 ? malloc((size_t)cb_file) : NULL;
    const int is_read = s_p_at9 && fread(s_p_at9, 1, (size_t)cb_file, p_in) == (size_t)cb_file;
    fclose(p_in);
    record("at9_input_present", is_read, 1);
    if (!is_read) return;
    Ngs2WaveformInfo info;
    record("at9_input_parse_file", (uint32_t)sceNgs2ParseWaveformFile(SZ_INPUT_AT9, 0, &info), 0);
    record_info("at9_input", &info, NULL, 1);
    for (uint32_t is_ajm = 0; is_ajm < 2; is_ajm++) {
        const Ngs2SamplerRackOption option = sampler_option(is_ajm ? 0 : 1, is_ajm ? 2 : 0);
        Chain chain = create_chain(&option, info.format.c_channels);
        record(is_ajm ? "at9_ajm_setup" : "at9_setup", setup_format(chain.h_voice, &info.format, is_ajm), 0);
        record(is_ajm ? "at9_ajm_blocks" : "at9_blocks", add_blocks(chain.h_voice, s_p_at9, info.blocks, info.c_blocks), 0);
        record(is_ajm ? "at9_ajm_play" : "at9_play", event(chain.h_voice, 0), 0);
        const uint32_t c_channels = info.format.c_channels <= 2 ? info.format.c_channels : 2;
        render_f32(is_ajm ? "at9_ajm" : "at9", chain.h_system, c_channels, 400, s_pcm);
        uint32_t i_first = INFINITE;
        for (uint32_t i = 0; i < 400 * GRAIN * c_channels && i_first == INFINITE; i++) {
            if (s_pcm[i] != 0) i_first = i / c_channels;
        }
        record_open(is_ajm ? "at9_ajm_first_nonzero" : "at9_first_nonzero", i_first);
        record_open(is_ajm ? "at9_ajm_flags" : "at9_flags", flags_of(chain.h_voice));
        destroy_chain(&chain);
    }
    /* Setup checks, in the order spec 03 §5.1 gives. */
    const Ngs2SamplerRackOption none = sampler_option(0, 0);
    Chain chain = create_chain(&none, 2);
    record("at9_no_decoders", setup_format(chain.h_voice, &info.format, 0), E(0x402));
    destroy_chain(&chain);
    const Ngs2SamplerRackOption one = sampler_option(1, 0);
    chain = create_chain(&one, 2);
    record("at9_no_ajm_decoders", setup_format(chain.h_voice, &info.format, 1), E(0x402));
    Ngs2WaveformFormat format = info.format;
    format.config_data &= 0x00ffffffu;
    record("at9_no_fe", setup_format(chain.h_voice, &format, 0), E(0x403));
    format = info.format;
    format.c_channels = info.format.c_channels == 1 ? 2 : 1;
    record("at9_channels", setup_format(chain.h_voice, &format, 0), E(0x52));
    /* A sample rate index the firmware's frame table leaves out (5): the setup takes it, the render
     * reports ERROR. */
    format = info.format;
    format.config_data = (format.config_data & ~0x00f00000u) | 0x00500000u;
    record("at9_rate_index_5_setup", setup_format(chain.h_voice, &format, 0), 0);
    record("at9_rate_index_5_blocks", add_blocks(chain.h_voice, s_p_at9, info.blocks, info.c_blocks), 0);
    record("at9_rate_index_5_play", event(chain.h_voice, 0), 0);
    render_f32("at9_rate_index_5", chain.h_system, 2, 1, s_pcm);
    record("at9_rate_index_5_flags", flags_of(chain.h_voice), 0x11);
    destroy_chain(&chain);
    free(s_p_at9);
}

int main(void) {
    int rc = sceSysmoduleLoadModule(SYSMODULE_NGS2);
    if (rc != 0) {
        printf("ngs2_waveform: loading libSceNgs2 failed: %#x\n", (unsigned)rc);
        return 1;
    }
    mkdir("/data/psport-hwtest", 0777);
    mkdir(SZ_OUT_DIR, 0777);
    s_p_results = fopen(SZ_OUT_DIR "/results.csv", "w");
    if (!s_p_results) {
        printf("ngs2_waveform: cannot write %s/results.csv (errno %d)\n", SZ_OUT_DIR, errno);
        return 1;
    }
    fprintf(s_p_results, "case,value,expected,match\n");
    test_parse_pcm();
    test_parse_at9();
    test_parse_vag();
    test_parse_errors();
    test_frames();
    test_render_vag();
    test_generators();
    test_atrac9_input();
    fclose(s_p_results);
    printf("ngs2_waveform: done, %d mismatch(es) against psport's predictions\n", s_c_mismatches);
    return s_c_mismatches != 0;
}
