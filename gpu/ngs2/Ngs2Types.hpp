#ifndef CORE_LIBS_PRX_LIBSCENGS2_INCLUDE_NGS2TYPES_HPP
#define CORE_LIBS_PRX_LIBSCENGS2_INCLUDE_NGS2TYPES_HPP

#include <cstddef>
#include <cstdint>

#include "ngs2_compat.hpp"

// PS4 layouts (sizes checked against shadPS4's Orbis* types), where AnyPS5's are PS5's: 16-byte
// system and rack names, no job scheduler or voice channel options, 32-bit waveform block
// offsets, a 4-byte voice state, a 32-bit sampler filter channel mask.

static constexpr int SCE_NGS2_OK = 0;
static constexpr int SCE_NGS2_ERROR_INVALID_OUT_ADDRESS = static_cast<int>(0x804A8010);
static constexpr int SCE_NGS2_ERROR_INVALID_OUT_SIZE = static_cast<int>(0x804A8011);
static constexpr int SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE = static_cast<int>(0x804A0230);
static constexpr int SCE_NGS2_ERROR_INVALID_RACK_HANDLE = static_cast<int>(0x804A0261);
static constexpr int SCE_NGS2_ERROR_INVALID_PARAM_SIZE = static_cast<int>(0x804A030A);
static constexpr int SCE_NGS2_ERROR_INVALID_WAVEFORM_DATA = static_cast<int>(0x804A8430);
static constexpr int SCE_NGS2_ERROR_INVALID_WAVEFORM_FORMAT = static_cast<int>(0x804A8431);
static constexpr int SCE_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT = static_cast<int>(0x804A8432);

static constexpr std::uint32_t SCE_NGS2_RACK_ID_SAMPLER = 0x1000;
static constexpr std::uint32_t SCE_NGS2_RACK_ID_SUBMIXER = 0x2000;
static constexpr std::uint32_t SCE_NGS2_RACK_ID_REVERB = 0x2001;
static constexpr std::uint32_t SCE_NGS2_RACK_ID_MASTERING = 0x3000;
static constexpr std::uint32_t SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER = 0x4002;

static constexpr std::uint32_t SCE_NGS2_WAVEFORM_TYPE_PCM_I16L = 0x12;
static constexpr std::uint32_t SCE_NGS2_WAVEFORM_TYPE_PCM_F32L = 0x18;
static constexpr std::uint32_t SCE_NGS2_WAVEFORM_TYPE_VAG = 0x1c;
static constexpr std::uint32_t SCE_NGS2_WAVEFORM_TYPE_ATRAC9 = 0x40;

// PS4 numbers its voice events 0 up (AnyPS5's are PS5 bit values): Ryza (CUSA16458) plays each
// voice with 0 after its setup and frees it with 3.
static constexpr std::uint32_t SCE_NGS2_VOICE_EVENT_PLAY = 0;
static constexpr std::uint32_t SCE_NGS2_VOICE_EVENT_STOP = 1;
static constexpr std::uint32_t SCE_NGS2_VOICE_EVENT_STOP_IMM = 2;
static constexpr std::uint32_t SCE_NGS2_VOICE_EVENT_KILL = 3;
static constexpr std::uint32_t SCE_NGS2_VOICE_EVENT_PAUSE = 4;
static constexpr std::uint32_t SCE_NGS2_VOICE_EVENT_RESUME = 5;

static constexpr std::uint32_t SCE_NGS2_VOICE_STATE_FLAG_INUSE = 1;
static constexpr std::uint32_t SCE_NGS2_VOICE_STATE_FLAG_PLAYING = 2;
static constexpr std::uint32_t SCE_NGS2_VOICE_STATE_FLAG_PAUSED = 4;
static constexpr std::uint32_t SCE_NGS2_VOICE_STATE_FLAG_STOPPED = 8;

static constexpr std::uint32_t SCE_NGS2_VOICE_PARAM_MATRIX_LEVELS = 0x0001;
static constexpr std::uint32_t SCE_NGS2_VOICE_PARAM_PORT_VOLUME = 0x0002;
static constexpr std::uint32_t SCE_NGS2_VOICE_PARAM_PORT_MATRIX = 0x0003;
static constexpr std::uint32_t SCE_NGS2_VOICE_PARAM_PORT_DELAY = 0x0004;
static constexpr std::uint32_t SCE_NGS2_VOICE_PARAM_PATCH = 0x0005;
static constexpr std::uint32_t SCE_NGS2_VOICE_PARAM_EVENT = 0x0006;
static constexpr std::uint32_t SCE_NGS2_VOICE_PARAM_CALLBACK = 0x0007;
static constexpr std::uint32_t SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP = 0x10000000;
static constexpr std::uint32_t SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS = 0x10000001;
static constexpr std::uint32_t SCE_NGS2_SAMPLER_VOICE_PARAM_EXIT_LOOP = 0x10000004;
static constexpr std::uint32_t SCE_NGS2_SAMPLER_VOICE_PARAM_PITCH = 0x10000005;
static constexpr std::uint32_t SCE_NGS2_SAMPLER_VOICE_PARAM_FILTER = 0x1000000a;
// Named by their place in each rack's list (setup 0 up, as the known ids above sit); used by Ryza.
static constexpr std::uint32_t SCE_NGS2_SAMPLER_VOICE_PARAM_ENVELOPE = 0x10000006;
static constexpr std::uint32_t SCE_NGS2_SAMPLER_VOICE_PARAM_USER_FX = 0x10000008;
static constexpr std::uint32_t SCE_NGS2_SUBMIXER_VOICE_PARAM_PEAK_METER = 0x20000005;
static constexpr std::uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_MATRIX = 0x30000001;
static constexpr std::uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_LFE = 0x30000002;
static constexpr std::uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_LIMITER = 0x30000003;
static constexpr std::uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_PEAK_METER = 0x30000006;
static constexpr std::uint32_t SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP = 0x20000000;
static constexpr std::uint32_t SCE_NGS2_SUBMIXER_VOICE_PARAM_USER_FX = 0x20000004;
static constexpr std::uint32_t SCE_NGS2_REVERB_VOICE_PARAM_SETUP = 0x20010000;
static constexpr std::uint32_t SCE_NGS2_REVERB_VOICE_PARAM_I3DL2 = 0x20010001;
static constexpr std::uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_SETUP = 0x30000000;
static constexpr std::uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_GAIN = 0x30000004;
static constexpr std::uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_OUTPUT = 0x30000005;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_SUBMIXER_VOICE_PARAM_SETUP = 0x40020000;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_FILTER = 0x40001300;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_GAIN = 0x40001500;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX = 0x40001900;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_REVERB = 0x40001b00;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX2 = 0x40001f00;

static constexpr std::uint32_t SCE_NGS2_CUSTOM_MAX_MODULES = 24;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_MAX_PORTS = 16;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_MODULE_ID_FILTER = 0x13;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_MODULE_ID_GAIN = 0x15;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_MODULE_ID_USER_FX = 0x19;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_MODULE_ID_REVERB = 0x1b;
static constexpr std::uint32_t SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2 = 0x1f;

static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_LOW_PASS = 1;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_HIGH_PASS = 2;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_BAND_PASS = 3;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_BAND_PASS_PEAK = 4;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_NOTCH = 5;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_HIGH_SHELF = 6;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_LOW_SHELF = 7;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_PEAKING = 8;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_FIRST_ORDER_LOW_PASS = 9;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_FIRST_ORDER_HIGH_PASS = 10;
static constexpr std::uint32_t SCE_NGS2_FILTER_TYPE_COEFFICIENTS = 0x20;

static constexpr std::uint32_t SCE_NGS2_WAVEFORM_BLOCKS_FLAG_CONTINUE = 1;
static constexpr std::uint32_t SCE_NGS2_WAVEFORM_BLOCKS_FLAG_APPEND = 2;
static constexpr std::uint32_t SCE_NGS2_WAVEFORM_BLOCKS_FLAG_RESET = 4;

static constexpr std::uint32_t SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END = 1;
static constexpr std::uint32_t SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_REPEAT = 2;

using Ngs2Handle = std::uintptr_t;

struct Ngs2ContextBufferInfo {
    void* host_buffer;
    std::size_t host_buffer_size;
    std::uintptr_t reserved[5];
    std::uintptr_t user_data;
};
static_assert(sizeof(Ngs2ContextBufferInfo) == 64);

using Ngs2BufferAllocHandler = std::int32_t (APS5_VABI *)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler = std::int32_t (APS5_VABI *)(Ngs2ContextBufferInfo*);

struct Ngs2BufferAllocator {
    Ngs2BufferAllocHandler alloc_handler;
    Ngs2BufferFreeHandler free_handler;
    std::uintptr_t user_data;
};

struct Ngs2SystemOption {
    std::size_t size;
    char name[16];
    std::uint32_t flags;
    std::uint32_t max_grain_samples;
    std::uint32_t num_grain_samples;
    std::uint32_t sample_rate;
    std::uint32_t reserved[6];
};
static_assert(sizeof(Ngs2SystemOption) == 64);

struct Ngs2SystemInfo {
    char name[16];
    Ngs2Handle system_handle;
    Ngs2ContextBufferInfo buffer_info;
    std::uint32_t uid;
    std::uint32_t min_grain_samples;
    std::uint32_t max_grain_samples;
    std::uint32_t state_flags;
    std::uint32_t rack_count;
    float last_render_ratio;
    std::int64_t last_render_tick;
    std::int64_t render_count;
    std::uint32_t sample_rate;
    std::uint32_t num_grain_samples;
};
static_assert(sizeof(Ngs2SystemInfo) == 136);

struct Ngs2RackOption {
    std::size_t size;
    char name[16];
    std::uint32_t flags;
    std::uint32_t max_grain_samples;
    std::uint32_t max_voices;
    std::uint32_t max_input_delay_blocks;
    std::uint32_t max_matrices;
    std::uint32_t max_ports;
    std::uint32_t reserved[20];
};
static_assert(sizeof(Ngs2RackOption) == 128);

struct Ngs2SamplerRackOption {
    Ngs2RackOption rack_option;
    std::uint32_t max_channel_works;
    std::uint32_t max_codec_caches;
    std::uint32_t max_waveform_blocks;
    std::uint32_t max_envelope_points;
    std::uint32_t max_filters;
    std::uint32_t max_atrac9_decoders;
    std::uint32_t max_atrac9_channel_works;
    std::uint32_t max_ajm_atrac9_decoders;
    std::uint32_t num_peak_meter_blocks;
};

struct Ngs2SubmixerRackOption {
    Ngs2RackOption rack_option;
    std::uint32_t max_channels;
    std::uint32_t max_envelope_points;
    std::uint32_t max_filters;
    std::uint32_t max_inputs;
    std::uint32_t num_peak_meter_blocks;
};

struct Ngs2MasteringRackOption {
    Ngs2RackOption rack_option;
    std::uint32_t max_channels;
    std::uint32_t num_peak_meter_blocks;
};

struct Ngs2ReverbRackOption {
    Ngs2RackOption rack_option;
    std::uint32_t max_channels;
    std::uint32_t reverb_size;
};

struct Ngs2CustomModuleOption {
    std::uint32_t size;
};

struct Ngs2UserFx2SetupContext {
    void* common;
    void* param;
    void* work;
    std::uintptr_t user_data;
    std::uint32_t max_voices;
    std::uint32_t voice_index;
    std::uint64_t reserved[4];
};
static_assert(sizeof(Ngs2UserFx2SetupContext) == 72);

struct Ngs2UserFxProcessContext {
    float** channel_data;
    std::uintptr_t user_data0;
    std::uintptr_t user_data1;
    std::uintptr_t user_data2;
    std::uint32_t flags;
    std::uint32_t num_channels;
    std::uint32_t num_grain_samples;
    std::uint32_t sample_rate;
};
static_assert(sizeof(Ngs2UserFxProcessContext) == 48);

using Ngs2UserFxProcessHandler = std::int32_t (APS5_VABI *)(Ngs2UserFxProcessContext*);

using Ngs2UserFx2CleanupContext = Ngs2UserFx2SetupContext;

struct Ngs2UserFx2ControlContext {
    const void* data;
    std::size_t data_size;
    void* common;
    void* param;
    std::uintptr_t user_data;
    std::uint64_t reserved[4];
};
static_assert(sizeof(Ngs2UserFx2ControlContext) == 72);

struct Ngs2UserFx2ProcessContext {
    float** channel_data;
    void* common;
    const void* param;
    void* work;
    void* state;
    std::uintptr_t user_data;
    std::uint32_t flags;
    std::uint32_t num_input_channels;
    std::uint32_t num_output_channels;
    std::uint32_t num_grain_samples;
    std::uint32_t sample_rate;
    std::uint32_t reserved;
    std::uint64_t reserved2[4];
};
static_assert(sizeof(Ngs2UserFx2ProcessContext) == 104);

using Ngs2UserFx2SetupHandler = std::int32_t (APS5_VABI *)(Ngs2UserFx2SetupContext*);
using Ngs2UserFx2CleanupHandler = std::int32_t (APS5_VABI *)(Ngs2UserFx2CleanupContext*);
using Ngs2UserFx2ControlHandler = std::int32_t (APS5_VABI *)(Ngs2UserFx2ControlContext*);
using Ngs2UserFx2ProcessHandler = std::int32_t (APS5_VABI *)(Ngs2UserFx2ProcessContext*);

struct Ngs2CustomUserFx2ModuleOption {
    Ngs2CustomModuleOption custom_module_option;
    Ngs2UserFx2SetupHandler setup_handler;
    Ngs2UserFx2CleanupHandler cleanup_handler;
    Ngs2UserFx2ControlHandler control_handler;
    Ngs2UserFx2ProcessHandler process_handler;
    std::size_t common_size;
    std::size_t param_size;
    std::size_t work_size;
    std::uintptr_t user_data;
};
static_assert(sizeof(Ngs2CustomUserFx2ModuleOption) == 72);

struct Ngs2CustomRackModuleInfo {
    const Ngs2CustomModuleOption* option;
    std::uint32_t module_id;
    std::uint32_t source_buffer_id;
    std::uint32_t extra_buffer_id;
    std::uint32_t dest_buffer_id;
    std::uint32_t state_offset;
    std::uint32_t state_size;
    std::uint32_t reserved;
    std::uint32_t reserved2;
};
static_assert(sizeof(Ngs2CustomRackModuleInfo) == 40);

struct Ngs2CustomRackPortInfo {
    std::uint32_t source_buffer_id;
    std::uint32_t reserved;
};

struct Ngs2CustomRackOption {
    Ngs2RackOption rack_option;
    std::uint32_t state_size;
    std::uint32_t num_buffers;
    std::uint32_t num_modules;
    std::uint32_t reserved;
    Ngs2CustomRackModuleInfo module[SCE_NGS2_CUSTOM_MAX_MODULES];
    Ngs2CustomRackPortInfo port[SCE_NGS2_CUSTOM_MAX_PORTS];
};
static_assert(sizeof(Ngs2CustomRackOption) == 1232);

struct Ngs2CustomSubmixerRackOption {
    Ngs2CustomRackOption custom_rack_option;
    std::uint32_t max_channels;
    std::uint32_t max_inputs;
};
static_assert(sizeof(Ngs2CustomSubmixerRackOption) == 1240);

struct Ngs2RackInfo {
    char name[16];
    Ngs2Handle rack_handle;
    Ngs2ContextBufferInfo buffer_info;
    Ngs2Handle owner_system_handle;
    std::uint32_t type;
    std::uint32_t rack_id;
    std::uint32_t uid;
    std::uint32_t min_grain_samples;
    std::uint32_t max_grain_samples;
    std::uint32_t max_voices;
    std::uint32_t max_channel_works;
    std::uint32_t max_inputs;
    std::uint32_t max_matrices;
    std::uint32_t max_ports;
    std::uint32_t state_flags;
    float last_process_ratio;
    std::uint64_t last_processed_tick;
    std::uint64_t render_count;
    std::uint32_t active_voice_count;
    std::uint32_t active_channel_work_count;
};
static_assert(sizeof(Ngs2RackInfo) == 168);

struct Ngs2VoiceParamHeader {
    std::uint16_t size;
    std::int16_t next;
    std::uint32_t id;
};

struct Ngs2VoiceMatrixLevelsParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t matrix_id;
    std::uint32_t num_levels;
    const float* levels;
};

struct Ngs2VoicePortVolumeParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t port;
    float level;
};

struct Ngs2VoicePortMatrixParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t port;
    std::int32_t matrix_id;
};

struct Ngs2VoicePortDelayParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t port;
    std::uint32_t num_samples;
};

struct Ngs2VoicePatchParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t port;
    std::uint32_t dest_input_id;
    Ngs2Handle dest_handle;
};

struct Ngs2VoiceEventParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t event_id;
};

struct Ngs2VoiceCallbackInfo {
    std::uintptr_t callback_data;
    Ngs2Handle voice_handle;
    std::uint32_t flag;
    std::uint32_t reserved;
    std::uintptr_t user_data;
    const void* block_data;
    std::uint32_t block_size;
    std::uint32_t num_repeated;
    std::uint32_t attributes;
    std::uint32_t reserved2;
};
static_assert(sizeof(Ngs2VoiceCallbackInfo) == 56);

using Ngs2VoiceCallbackHandler = void (APS5_VABI *)(const Ngs2VoiceCallbackInfo*);

struct Ngs2VoiceCallbackParam {
    Ngs2VoiceParamHeader header;
    Ngs2VoiceCallbackHandler callback;
    std::uintptr_t callback_data;
    std::uint32_t flags;
    std::uint32_t reserved;
};

struct Ngs2WaveformFormat {
    std::uint32_t waveform_type;
    std::uint32_t num_channels;
    std::uint32_t sample_rate;
    std::uint32_t config_data;
    std::uint32_t frame_offset;
    std::uint32_t frame_margin;
};
static_assert(sizeof(Ngs2WaveformFormat) == 24);

struct Ngs2WaveformBlock {
    std::uint32_t data_offset;
    std::uint32_t data_size;
    std::uint32_t num_repeats;
    std::uint32_t num_skip_samples;
    std::uint32_t num_samples;
    std::uint32_t reserved;
    std::uintptr_t user_data;
};
static_assert(sizeof(Ngs2WaveformBlock) == 32);

struct Ngs2SamplerVoiceSetupParam {
    Ngs2VoiceParamHeader header;
    Ngs2WaveformFormat format;
};

struct Ngs2SamplerVoiceWaveformBlocksParam {
    Ngs2VoiceParamHeader header;
    const void* data;
    std::uint32_t flags;
    std::uint32_t num_blocks;
    const Ngs2WaveformBlock* blocks;
};

struct Ngs2SamplerVoicePitchParam {
    Ngs2VoiceParamHeader header;
    float ratio;
};

struct Ngs2EnvelopePoint {
    std::uint32_t curve;
    std::uint32_t duration;
    float height;
};
static_assert(sizeof(Ngs2EnvelopePoint) == 12);

struct Ngs2SamplerVoiceEnvelopeParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t num_forward_points;
    std::uint32_t num_release_points;
    const Ngs2EnvelopePoint* points;
};
static_assert(sizeof(Ngs2SamplerVoiceEnvelopeParam) == 24);

// values: frequency, q and gain, or for SCE_NGS2_FILTER_TYPE_COEFFICIENTS b0, b1, b2, -a1 and -a2.
struct Ngs2FilterParam {
    std::uint32_t type;
    std::uint32_t channel_mask;
    float values[5];
};

struct Ngs2SamplerVoiceFilterParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t index;
    std::uint32_t location;
    Ngs2FilterParam filter;
    std::uint32_t reserved;
};
static_assert(sizeof(Ngs2SamplerVoiceFilterParam) == 48);

struct Ngs2SubmixerVoiceSetupParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t num_io_channels;
    std::uint32_t flags;
};
static_assert(sizeof(Ngs2SubmixerVoiceSetupParam) == 16);

struct Ngs2ReverbVoiceSetupParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t num_input_channels;
    std::uint32_t num_output_channels;
    std::uint32_t flags;
    std::uint32_t reserved;
};
static_assert(sizeof(Ngs2ReverbVoiceSetupParam) == 24);

struct Ngs2ReverbI3DL2Param {
    float wet;
    float dry;
    std::int32_t room;
    std::int32_t room_hf;
    std::uint32_t reflection_pattern;
    float decay_time;
    float decay_hf_ratio;
    std::int32_t reflections;
    float reflections_delay;
    std::int32_t reverb;
    float reverb_delay;
    float diffusion;
    float density;
    float hf_reference;
    std::uint32_t reserved[8];
};
static_assert(sizeof(Ngs2ReverbI3DL2Param) == 88);

struct Ngs2ReverbVoiceI3DL2Param {
    Ngs2VoiceParamHeader header;
    Ngs2ReverbI3DL2Param i3dl2;
};
static_assert(sizeof(Ngs2ReverbVoiceI3DL2Param) == 96);

struct Ngs2SubmixerVoiceUserFxParam {
    Ngs2VoiceParamHeader header;
    Ngs2UserFxProcessHandler handler;
    std::uintptr_t user_data0;
    std::uintptr_t user_data1;
    std::uintptr_t user_data2;
};
static_assert(sizeof(Ngs2SubmixerVoiceUserFxParam) == 40);

struct Ngs2CustomSubmixerVoiceSetupParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t num_input_channels;
    std::uint32_t num_output_channels;
    std::uint32_t flags;
    std::uint32_t reserved;
};
static_assert(sizeof(Ngs2CustomSubmixerVoiceSetupParam) == 24);

struct Ngs2CustomFilterParam {
    Ngs2VoiceParamHeader header;
    Ngs2FilterParam filter;
    std::uint32_t reserved;
};
static_assert(sizeof(Ngs2CustomFilterParam) == 40);

struct Ngs2CustomGainParam {
    Ngs2VoiceParamHeader header;
    float levels[8];
};
static_assert(sizeof(Ngs2CustomGainParam) == 40);

struct Ngs2CustomVoiceUserFx2Param {
    Ngs2VoiceParamHeader header;
    const void* data;
    std::size_t data_size;
};
static_assert(sizeof(Ngs2CustomVoiceUserFx2Param) == 24);

struct Ngs2MasteringVoiceSetupParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t num_io_channels;
    std::uint32_t flags;
};

struct Ngs2MasteringVoiceGainParam {
    Ngs2VoiceParamHeader header;
    float fbw_level;
    float lfe_level;
};
static_assert(sizeof(Ngs2MasteringVoiceGainParam) == 16);

struct Ngs2MasteringVoiceLfeParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t enabled;
    std::uint32_t frequency;
};
static_assert(sizeof(Ngs2MasteringVoiceLfeParam) == 16);

struct Ngs2MasteringVoiceLimiterParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t enabled;
    float threshold;
};
static_assert(sizeof(Ngs2MasteringVoiceLimiterParam) == 16);

struct Ngs2MasteringVoiceOutputParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t output_id;
    std::uint32_t reserved;
};

struct Ngs2MasteringVoiceMatrixParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t type;
    std::uint32_t num_levels;
    const float* levels;
};
static_assert(sizeof(Ngs2MasteringVoiceMatrixParam) == 24);

static constexpr std::uint32_t SCE_NGS2_PEAK_METER_MODE_PEAK = 1;
static constexpr std::uint32_t SCE_NGS2_PEAK_METER_MODE_RMS = 2;

struct Ngs2VoicePeakMeterParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t mode;
    std::uint32_t reserved;
};
static_assert(sizeof(Ngs2VoicePeakMeterParam) == 16);

struct Ngs2VoiceCommand {
    std::uint32_t id;
    std::uint8_t flags;
    std::uint8_t type;
    std::uint16_t count;
    union {
        std::uint32_t u;
        std::int32_t i;
        float f;
        const float* levels;
    } value;
};
static_assert(sizeof(Ngs2VoiceCommand) == 16);

struct Ngs2RenderBufferInfo {
    void* buffer;
    std::size_t buffer_size;
    std::uint32_t waveform_type;
    std::uint32_t num_channels;
};

static constexpr std::uint32_t SCE_NGS2_VOICE_INFO_CHANNELS = 0x4001;

struct Ngs2VoiceChannelsInfo {
    std::uint32_t num_channels;
    std::uint32_t reserved;
};
static_assert(sizeof(Ngs2VoiceChannelsInfo) == 8);

struct Ngs2VoicePortInfo {
    std::int32_t matrix_id;
    float volume;
    std::uint32_t num_delay_samples;
    std::uint32_t dest_input_id;
    Ngs2Handle dest_handle;
};
static_assert(sizeof(Ngs2VoicePortInfo) == 24);

struct Ngs2VoiceState {
    std::uint32_t state_flags;
};
static_assert(sizeof(Ngs2VoiceState) == 4);

struct Ngs2MasteringVoiceState {
    Ngs2VoiceState voice_state;
    float limiter_peak_level;
    float limiter_press_level;
    float input_peak_height[8];
    float output_peak_height[8];
};
static_assert(sizeof(Ngs2MasteringVoiceState) == 76);

struct Ngs2CustomSubmixerVoiceState {
    Ngs2VoiceState voice_state;
    std::uint32_t reserved[2];
};
static_assert(sizeof(Ngs2CustomSubmixerVoiceState) == 12);

struct Ngs2SubmixerVoiceState {
    Ngs2VoiceState voice_state;
    float envelope_height;
    float peak_height;
    float compressor_height;
};
static_assert(sizeof(Ngs2SubmixerVoiceState) == 16);

struct Ngs2SamplerVoiceState {
    Ngs2VoiceState voice_state;
    float envelope_height;
    float peak_height;
    std::uint32_t reserved;
    std::uint64_t num_decoded_samples;
    std::uint64_t decoded_data_size;
    std::uintptr_t user_data;
    const void* waveform_data;
};
static_assert(sizeof(Ngs2SamplerVoiceState) == 48);

struct Ngs2WaveformInfo {
    Ngs2WaveformFormat format;
    std::uint32_t data_offset;
    std::uint32_t data_size;
    std::uint32_t loop_begin_position;
    std::uint32_t loop_end_position;
    std::uint32_t num_samples;
    std::uint32_t audio_unit_size;
    std::uint32_t num_audio_unit_samples;
    std::uint32_t num_audio_unit_per_frame;
    std::uint32_t audio_frame_size;
    std::uint32_t num_audio_frame_samples;
    std::uint32_t num_delay_samples;
    std::uint32_t num_blocks;
    Ngs2WaveformBlock block[4];
};
static_assert(sizeof(Ngs2WaveformInfo) == 200);

struct Ngs2PanParam {
    float angle;
    float distance;
    float fbw_level;
    float lfe_level;
};
static_assert(sizeof(Ngs2PanParam) == 16);

struct Ngs2PanWork {
    float speaker_angles[8];
    float unit_angle;
    std::uint32_t num_speakers;
};
static_assert(sizeof(Ngs2PanWork) == 40);

struct Ngs2GeomListenerParam {
    std::uint32_t reserved[32];
};

struct Ngs2GeomListenerWork {
    std::uint32_t reserved[64];
};

struct Ngs2GeomSourceParam {
    std::uint32_t reserved[32];
};

struct Ngs2GeomAttribute {
    std::uint32_t reserved[32];
};

#endif
