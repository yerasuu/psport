// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2Rack* and sceNgs2CustomRackGetModuleInfo (spec 02 §3, §4). A rack's buffer holds the rack, its
// voices (all of them exist from creation on), each voice's matrices and ports, and everything the
// render keeps for a voice: parameters, DSP state, input rings, the waveform queue, and a sampler's
// ATRAC9 decoders. QueryBufferSize answers what this layout needs, which differs from the console's sum
// (spec 08 §2): LibAtrac9's state is about 90 KiB a decoder where the firmware's takes 0xF0 bytes plus
// 0x280 per channel work. A reverb voice's delay lines take what the firmware's module 0x1b sizes for them
// (spec 07 §1) plus one float.
#include <algorithm>
#include <cstring>
#include <new>

#include "Internal.h"
#include "Reverb.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_reverb.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kSamplerRackId = 0x1000;
constexpr u32 kReverbRackId = 0x2001;
constexpr u32 kMasteringRackId = 0x3000;
constexpr u32 kRackFlagCallbacks = 0x40000000;
constexpr u32 kDefaultLfeCutoffHz = 120;
// The callbacks a rack can queue per voice and render beyond its waveform blocks (0x65f0, spec 09
// §8.1): (maxWaveformBlocks + 8) per voice for the sampler, 8 for the other racks. What does not fit
// is dropped.
constexpr u32 kExtraCallbacksPerVoice = 8;

template <class T>
T* AllocateArray(Arena* p_arena, std::size_t c_items) {
    return c_items ? static_cast<T*>(p_arena->Allocate(c_items * sizeof(T), alignof(T))) : nullptr;
}

// Everything the render needs for a voice lives in the rack's buffer, so that a render never
// allocates (spec 08 §2 lists the firmware's own blocks).
struct VoiceBlocks {
    VoiceParams* p_params;
    VoiceParams* p_active_params;
    FilterParams* p_filters;
    FilterParams* p_active_filters;
    BiquadState* p_filter_states;
    MasteringMatrices* p_mastering[3];
    ReverbParams* p_reverb_params[2];
    ReverbState* p_reverb;
    float* p_reverb_lines;
    VoiceDsp* p_dsp;
    float* p_meter_rings;
    ActivePort* p_active_ports;
    float* p_matrix_levels;
    float* p_previous_matrix_levels;
    bool* p_matrix_is_set;
    float* p_input_ring;
    bool* p_input_started;
    WaveformBlock* p_blocks;
    WaveformBlock* p_active_blocks;
    u8* p_state;
};

u32 MeterCount(const RackKind& kind) {
    return kind.rack_id == kMasteringRackId ? 2 : 1;
}

VoiceBlocks AllocateVoiceBlocks(Arena* p_arena, const RackKind& kind, const RackLimits& limits, u32 c_input_blocks) {
    VoiceBlocks blocks{};
    blocks.p_params = AllocateArray<VoiceParams>(p_arena, 1);
    blocks.p_active_params = AllocateArray<VoiceParams>(p_arena, 1);
    blocks.p_filters = AllocateArray<FilterParams>(p_arena, limits.max_filters);
    blocks.p_active_filters = AllocateArray<FilterParams>(p_arena, limits.max_filters);
    blocks.p_filter_states = AllocateArray<BiquadState>(p_arena, std::size_t(limits.max_filters) * kMaxChannels);
    if (kind.rack_id == kMasteringRackId) {
        for (MasteringMatrices*& p_matrices : blocks.p_mastering) {
            p_matrices = AllocateArray<MasteringMatrices>(p_arena, 1);
        }
    }
    if (kind.rack_id == kReverbRackId) {
        for (ReverbParams*& p_params : blocks.p_reverb_params) {
            p_params = AllocateArray<ReverbParams>(p_arena, 1);
        }
        blocks.p_reverb = AllocateArray<ReverbState>(p_arena, 1);
        const ReverbSizes sizes = GetReverbSizes(limits.reverb_size);
        const std::size_t c_floats = std::size_t(sizes.max_line_delay) + 1 + sizes.c_comb_floats +
                                     std::size_t(limits.max_channels) * sizes.c_allpass_floats;
        blocks.p_reverb_lines = static_cast<float*>(p_arena->Allocate(c_floats * sizeof(float), 64));
    }
    blocks.p_dsp = AllocateArray<VoiceDsp>(p_arena, 1);
    blocks.p_meter_rings =
        AllocateArray<float>(p_arena, std::size_t(MeterCount(kind)) * limits.num_peak_meter_blocks * kPeakValues);
    blocks.p_active_ports = AllocateArray<ActivePort>(p_arena, limits.max_ports);
    blocks.p_matrix_levels = AllocateArray<float>(p_arena, std::size_t(limits.max_matrices) * kMatrixCells);
    blocks.p_previous_matrix_levels = AllocateArray<float>(p_arena, std::size_t(limits.max_matrices) * kMatrixCells);
    blocks.p_matrix_is_set = AllocateArray<bool>(p_arena, limits.max_matrices);
    const std::size_t c_input_slots = std::size_t(limits.max_inputs) * c_input_blocks;
    if (c_input_slots) {
        blocks.p_input_ring = static_cast<float*>(p_arena->Allocate(
            c_input_slots * limits.max_channels * limits.max_grain_samples * sizeof(float), 64));
        blocks.p_input_started = AllocateArray<bool>(p_arena, c_input_slots);
    }
    if (kind.rack_id == kSamplerRackId) {
        blocks.p_blocks = AllocateArray<WaveformBlock>(p_arena, limits.max_waveform_blocks);
        blocks.p_active_blocks = AllocateArray<WaveformBlock>(p_arena, limits.max_waveform_blocks);
    }
    if (!kind.is_custom) {
        blocks.p_state = AllocateArray<u8>(p_arena, limits.cb_voice_state);
    }
    return blocks;
}

void InitVoice(Voice* p_voice, const RackKind& kind, const RackLimits& limits, const VoiceBlocks& blocks) {
    p_voice->p_params = new (blocks.p_params) VoiceParams{};
    p_voice->p_params->pitch_ratio = 1.0f;
    p_voice->p_params->lfe = {0, kDefaultLfeCutoffHz};
    p_voice->p_params->gain_fbw = 1.0f;
    p_voice->p_params->gain_lfe = 1.0f;
    p_voice->p_active_params = new (blocks.p_active_params) VoiceParams{*p_voice->p_params};
    p_voice->p_filters = blocks.p_filters;
    p_voice->p_active_filters = blocks.p_active_filters;
    p_voice->p_filter_states = blocks.p_filter_states;
    p_voice->p_mastering_matrices = blocks.p_mastering[0];
    p_voice->p_active_mastering_matrices = blocks.p_mastering[1];
    p_voice->p_previous_mastering_matrices = blocks.p_mastering[2];
    if (blocks.p_reverb) {
        p_voice->p_reverb_params = new (blocks.p_reverb_params[0]) ReverbParams{};
        SetReverbDefaults(p_voice->p_reverb_params);
        p_voice->p_active_reverb_params = new (blocks.p_reverb_params[1]) ReverbParams{*p_voice->p_reverb_params};
        const ReverbSizes sizes = GetReverbSizes(limits.reverb_size);
        float* p_combs = blocks.p_reverb_lines + sizes.max_line_delay + 1;
        ReverbState* p_reverb = new (blocks.p_reverb) ReverbState{};
        p_reverb->p_line = blocks.p_reverb_lines;
        p_reverb->p_combs = p_combs;
        p_reverb->p_allpasses = p_combs + sizes.c_comb_floats;
        p_reverb->sizes = sizes;
        p_reverb->c_allpass_channels = limits.max_channels;
        p_voice->p_reverb = p_reverb;
    }
    p_voice->p_dsp = new (blocks.p_dsp) VoiceDsp{};
    for (u32 i_meter = 0; i_meter < MeterCount(kind); ++i_meter) {
        float* p_ring = blocks.p_meter_rings
                            ? blocks.p_meter_rings + std::size_t(i_meter) * limits.num_peak_meter_blocks * kPeakValues
                            : nullptr;
        p_voice->meters[i_meter] = {p_ring, limits.num_peak_meter_blocks, 0};
    }
    p_voice->p_active_ports = blocks.p_active_ports;
    for (u32 i_port = 0; i_port < limits.max_ports; ++i_port) {
        new (&p_voice->p_active_ports[i_port]) ActivePort{nullptr, 0, 0, 1.0f, 1.0f, -1, -1, true};
    }
    p_voice->p_matrix_levels = blocks.p_matrix_levels;
    p_voice->p_previous_matrix_levels = blocks.p_previous_matrix_levels;
    p_voice->p_matrix_is_set = blocks.p_matrix_is_set;
    p_voice->p_input_ring = blocks.p_input_ring;
    p_voice->p_input_started = blocks.p_input_started;
    p_voice->queue.p_slots = blocks.p_blocks;
    p_voice->queue.c_capacity = blocks.p_blocks ? limits.max_waveform_blocks : 0;
    p_voice->active_queue.p_slots = blocks.p_active_blocks;
    p_voice->p_state = blocks.p_state;
}

Rack* LayOutRack(Arena* p_arena, const RackKind& kind, const RackLimits& limits) {
    const std::size_t c_voices = limits.max_voices;
    void* p_object = p_arena->Allocate(sizeof(Rack), alignof(Rack));
    void* p_voices = p_arena->Allocate(c_voices * sizeof(Voice), alignof(Voice));
    void* p_matrices =
        limits.max_matrices ? p_arena->Allocate(c_voices * limits.max_matrices * sizeof(VoiceMatrix)) : nullptr;
    void* p_ports = limits.max_ports ? p_arena->Allocate(c_voices * limits.max_ports * sizeof(VoicePort)) : nullptr;
    const u32 c_input_blocks = kind.rack_class == RACK_CLASS_SOURCE ? 0 : limits.max_input_delay_blocks + 1;
    // The source decodes up to four input samples per output sample (pitch below 4.0) plus its history.
    const u32 c_decode_stride = 4 * limits.max_grain_samples + kResampleHistory + 8;
    float* p_decode_scratch =
        kind.rack_id == kSamplerRackId ? AllocateArray<float>(p_arena, std::size_t(kMaxChannels) * c_decode_stride)
                                       : nullptr;
    const u32 c_atrac9_decoders =
        kind.rack_id == kSamplerRackId ? limits.max_atrac9_decoders + limits.max_ajm_atrac9_decoders : 0;
    auto* p_atrac9_decoders = AllocateArray<Atrac9Decoder>(p_arena, c_atrac9_decoders);
    const std::size_t cb_atrac9_state = GetAtrac9StateSize();
    auto* p_atrac9_states =
        c_atrac9_decoders ? static_cast<u8*>(p_arena->Allocate(c_atrac9_decoders * cb_atrac9_state, 16)) : nullptr;
    const std::size_t c_atrac9_frame_floats = std::size_t(kMaxChannels) * kMaxAtrac9FrameSamples;
    float* p_atrac9_frames = AllocateArray<float>(p_arena, c_atrac9_decoders * c_atrac9_frame_floats);
    // The data lock bitmap at rack+0x130: two bits per sampler voice (spec 09 §6.2).
    const std::size_t c_lock_words = kind.rack_id == kSamplerRackId ? (std::size_t(limits.max_voices) + 15) / 16 : 0;
    auto* p_lock_words = AllocateArray<std::atomic<u32>>(p_arena, c_lock_words);
    const u32 c_slots_per_voice = kind.rack_id == kSamplerRackId ? limits.max_waveform_blocks : 0;
    const u32 c_callbacks =
        (limits.flags & kRackFlagCallbacks) ? (c_slots_per_voice + kExtraCallbacksPerVoice) * limits.max_voices : 0;
    QueuedCallback* p_callbacks = AllocateArray<QueuedCallback>(p_arena, c_callbacks);
    if (!p_object) {
        for (u32 i_voice = 0; i_voice < limits.max_voices; ++i_voice) {
            AllocateVoiceBlocks(p_arena, kind, limits, c_input_blocks);
        }
        return nullptr;
    }
    if (!p_voices || p_arena->HasOverflowed()) {
        return nullptr;
    }
    auto* p_rack = new (p_object) Rack{};
    p_rack->p_kind = &kind;
    p_rack->limits = limits;
    p_rack->refs = 1;
    p_rack->p_voices = static_cast<Voice*>(p_voices);
    p_rack->c_input_blocks = c_input_blocks;
    p_rack->p_decode_scratch = p_decode_scratch;
    p_rack->c_decode_stride = c_decode_stride;
    p_rack->p_callbacks = p_callbacks;
    p_rack->c_callback_capacity = c_callbacks;
    p_rack->p_atrac9_decoders = p_atrac9_decoders;
    p_rack->c_atrac9_decoders = c_atrac9_decoders;
    for (u32 i_decoder = 0; i_decoder < c_atrac9_decoders; ++i_decoder) {
        new (&p_atrac9_decoders[i_decoder])
            Atrac9Decoder{p_atrac9_states + i_decoder * cb_atrac9_state,
                          p_atrac9_frames + i_decoder * c_atrac9_frame_floats, nullptr,
                          i_decoder >= limits.max_atrac9_decoders};
    }
    for (std::size_t i_word = 0; i_word < c_lock_words; ++i_word) {
        new (&p_lock_words[i_word]) std::atomic<u32>{0};
    }
    InitHandle(&p_rack->handle, p_rack, &p_rack->refs, HANDLE_RACK, 0);
    for (u32 i_voice = 0; i_voice < limits.max_voices; ++i_voice) {
        const VoiceBlocks blocks = AllocateVoiceBlocks(p_arena, kind, limits, c_input_blocks);
        if (p_arena->HasOverflowed()) {
            return nullptr;
        }
        auto* p_voice = new (&p_rack->p_voices[i_voice]) Voice{};
        p_voice->p_rack = p_rack;
        p_voice->p_matrices = static_cast<VoiceMatrix*>(p_matrices) + std::size_t(i_voice) * limits.max_matrices;
        p_voice->p_ports = static_cast<VoicePort*>(p_ports) + std::size_t(i_voice) * limits.max_ports;
        InitHandle(&p_voice->handle, p_rack, &p_rack->refs, HANDLE_VOICE, i_voice);
        InitVoice(p_voice, kind, limits, blocks);
        if (p_lock_words) {
            p_voice->p_data_lock = &p_lock_words[i_voice / 16];
            p_voice->data_lock_shift = 2 * (i_voice % 16);
        }
    }
    return p_rack;
}

std::size_t MeasureRack(const RackKind& kind, const RackLimits& limits) {
    Arena arena{nullptr, 0, (limits.flags & 0x80000000u) != 0};
    LayOutRack(&arena, kind, limits);
    return arena.RequiredSize();
}

// Ends a rack the game can no longer reach: its handles are already retired or were never handed out.
void TearDownRack(Rack* p_rack) {
    for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
        Voice* p_voice = &p_rack->p_voices[i_voice];
        std::memset(static_cast<void*>(&p_voice->handle), 0, sizeof(p_voice->handle));
        p_voice->~Voice();
    }
    std::memset(static_cast<void*>(&p_rack->handle), 0, sizeof(p_rack->handle));
    p_rack->~Rack();
}

s32 CreateRack(OrbisNgs2Handle h_system, const RackKind& kind, const OrbisNgs2RackOption* p_option,
               const OrbisNgs2ContextBufferInfo& buffer_info, OrbisNgs2BufferFreeHandler pfn_free,
               OrbisNgs2Handle* p_out_handle) {
    RackLimits limits;
    if (const s32 result = ParseRackOption(kind, p_option, &limits); result < 0) {
        return result;
    }
    const std::size_t cb_required = MeasureRack(kind, limits);
    if (!buffer_info.hostBuffer) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS, "Invalid rack buffer address ({})",
                    buffer_info.hostBuffer);
    }
    if (buffer_info.hostBufferSize < cb_required) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid rack buffer size ({}<{}[byte])",
                    buffer_info.hostBufferSize, cb_required);
    }
    Arena arena{buffer_info.hostBuffer, buffer_info.hostBufferSize, (limits.flags & 0x80000000u) != 0};
    Rack* p_rack = LayOutRack(&arena, kind, limits);
    if (!p_rack) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid rack buffer size ({}<{}[byte])",
                    buffer_info.hostBufferSize, cb_required);
    }
    if (p_option) {
        const std::size_t cb_name = strnlen(p_option->name, sizeof(p_rack->name));
        if (cb_name < sizeof(p_rack->name)) {
            std::memcpy(p_rack->name, p_option->name, cb_name + 1);
        }
    }
    p_rack->buffer_info = buffer_info;
    p_rack->pfn_free = pfn_free;
    // The firmware validates the system handle last, once the rack is built (spec 02 §3).
    HandleRef system_ref{h_system, HANDLE_SYSTEM};
    if (!system_ref) {
        TearDownRack(p_rack);
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    auto* p_system = system_ref.Owner<System>();
    {
        std::scoped_lock lock{p_system->process_lock};
        if (const s32 result = LinkRack(p_system, p_rack); result < 0) {
            TearDownRack(p_rack);
            return result;
        }
    }
    if (limits.c_custom_modules > 0) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "sceNgs2RackCreate: custom rack modules (options unchecked, no DSP until F6)");
    }
    *p_out_handle = reinterpret_cast<OrbisNgs2Handle>(&p_rack->handle);
    return ORBIS_OK;
}

s32 InvalidRackId(u32 rack_id) {
    return Fail(0, ORBIS_NGS2_ERROR_INVALID_RACK_ID, "Invalid rack ID ({:X})", rack_id);
}

void FillCustomInfo(const RackLimits& limits, OrbisNgs2CustomRackInfo* p_out) {
    p_out->stateSize = limits.cb_custom_state;
    p_out->numBuffers = limits.c_custom_buffers;
    p_out->numModules = limits.c_custom_modules;
    p_out->reserved = 0;
    for (u32 i_module = 0; i_module < ORBIS_NGS2_CUSTOM_MAX_MODULES; ++i_module) {
        const CustomModule& module = limits.custom_modules[i_module];
        // The firmware leaves the option pointer and the extra buffer out of this copy (spec 02 §4.5).
        p_out->aModule[i_module] = {nullptr, module.module_id, module.source_buffer_id, 0, module.dest_buffer_id,
                                    module.off_state, module.cb_state, 0, 0};
    }
    for (u32 i_port = 0; i_port < ORBIS_NGS2_CUSTOM_MAX_PORTS; ++i_port) {
        p_out->aPort[i_port] = {limits.custom_port_sources[i_port], 0};
    }
}

// The part after the common 0xa8 bytes, written when the game's buffer holds all of it.
void FillRackKindInfo(const Rack& rack, OrbisNgs2RackInfo* p_out_info, std::size_t cb_info) {
    const RackLimits& limits = rack.limits;
    if (cb_info < rack.p_kind->cb_info) {
        return;
    }
    switch (rack.p_kind->rack_id) {
    case 0x1000: {
        auto* p_out = reinterpret_cast<OrbisNgs2SamplerRackInfo*>(p_out_info);
        p_out->maxChannelWorks = limits.max_channel_works;
        p_out->maxCodecCaches = limits.max_codec_caches;
        p_out->maxWaveformBlocks = limits.max_waveform_blocks;
        p_out->maxEnvelopePoints = limits.max_envelope_points;
        p_out->maxFilters = limits.max_filters;
        p_out->maxAtrac9Decoders = limits.max_atrac9_decoders;
        p_out->maxAtrac9ChannelWorks = limits.max_atrac9_channel_works;
        p_out->maxAjmAtrac9Decoders = limits.max_ajm_atrac9_decoders;
        break;
    }
    case 0x2000: {
        auto* p_out = reinterpret_cast<OrbisNgs2SubmixerRackInfo*>(p_out_info);
        p_out->maxChannels = limits.max_channels;
        p_out->maxEnvelopePoints = limits.max_envelope_points;
        p_out->maxFilters = limits.max_filters;
        p_out->maxInputs = limits.max_inputs;
        break;
    }
    case 0x2001: {
        auto* p_out = reinterpret_cast<OrbisNgs2ReverbRackInfo*>(p_out_info);
        p_out->maxChannels = limits.max_channels;
        p_out->reverbSize = limits.reverb_size;
        break;
    }
    case 0x3000: {
        auto* p_out = reinterpret_cast<OrbisNgs2MasteringRackInfo*>(p_out_info);
        p_out->maxChannels = limits.max_channels;
        p_out->reserved = 0;
        break;
    }
    case 0x4001: {
        auto* p_out = reinterpret_cast<OrbisNgs2CustomSamplerRackInfo*>(p_out_info);
        FillCustomInfo(limits, &p_out->customRackInfo);
        p_out->maxChannelWorks = limits.max_channel_works;
        p_out->maxWaveformBlocks = limits.max_waveform_blocks;
        p_out->maxAtrac9Decoders = limits.max_atrac9_decoders;
        p_out->maxAtrac9ChannelWorks = limits.max_atrac9_channel_works;
        p_out->maxAjmAtrac9Decoders = limits.max_ajm_atrac9_decoders;
        p_out->maxCodecCaches = limits.max_codec_caches;
        break;
    }
    case 0x4002:
    case 0x4003: {
        auto* p_out = reinterpret_cast<OrbisNgs2CustomSubmixerRackInfo*>(p_out_info);
        FillCustomInfo(limits, &p_out->customRackInfo);
        p_out->maxChannels = limits.max_channels;
        p_out->maxInputs = limits.max_inputs;
        break;
    }
    default:
        break;
    }
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2RackQueryBufferSize(u32 rack_id, const OrbisNgs2RackOption* p_option,
                                            OrbisNgs2ContextBufferInfo* p_out_info) {
    const RackKind* p_kind = FindRackKind(rack_id);
    if (!p_kind) {
        return InvalidRackId(rack_id);
    }
    if (!p_out_info) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack buffer info ({})",
                    static_cast<void*>(p_out_info));
    }
    RackLimits limits;
    if (const s32 result = ParseRackOption(*p_kind, p_option, &limits); result < 0) {
        return result;
    }
    p_out_info->hostBuffer = nullptr;
    p_out_info->hostBufferSize = MeasureRack(*p_kind, limits);
    std::memset(p_out_info->reserved, 0, sizeof(p_out_info->reserved));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackCreate(OrbisNgs2Handle h_system, u32 rack_id, const OrbisNgs2RackOption* p_option,
                                   const OrbisNgs2ContextBufferInfo* p_buffer, OrbisNgs2Handle* p_out_handle) {
    const RackKind* p_kind = FindRackKind(rack_id);
    if (!p_kind) {
        return InvalidRackId(rack_id);
    }
    if (!p_buffer) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO, "Invalid rack buffer info ({})",
                    static_cast<const void*>(p_buffer));
    }
    if (!p_out_handle) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    return CreateRack(h_system, *p_kind, p_option, *p_buffer, nullptr, p_out_handle);
}

s32 PS4_SYSV_ABI sceNgs2RackCreateWithAllocator(OrbisNgs2Handle h_system, u32 rack_id,
                                                const OrbisNgs2RackOption* p_option,
                                                const OrbisNgs2BufferAllocator* p_allocator,
                                                OrbisNgs2Handle* p_out_handle) {
    const RackKind* p_kind = FindRackKind(rack_id);
    if (!p_kind) {
        return InvalidRackId(rack_id);
    }
    if (!p_allocator || !p_allocator->allocHandler) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR, "Invalid rack buffer allocator ({},{})",
                    static_cast<const void*>(p_allocator),
                    reinterpret_cast<void*>(p_allocator ? p_allocator->allocHandler : nullptr));
    }
    if (!p_out_handle) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    RackLimits limits;
    if (const s32 result = ParseRackOption(*p_kind, p_option, &limits); result < 0) {
        return result;
    }
    OrbisNgs2ContextBufferInfo buffer_info{};
    buffer_info.hostBufferSize = MeasureRack(*p_kind, limits);
    buffer_info.userData = p_allocator->userData;
    if (const s32 result = p_allocator->allocHandler(&buffer_info); result < 0) {
        return result;
    }
    const s32 result = CreateRack(h_system, *p_kind, p_option, buffer_info, p_allocator->freeHandler, p_out_handle);
    if (result < 0 && p_allocator->freeHandler) {
        (void)p_allocator->freeHandler(&buffer_info); // the creation's error is what the game needs
    }
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackDestroy(OrbisNgs2Handle h_rack, OrbisNgs2ContextBufferInfo* p_out_info) {
    Rack* p_rack = nullptr;
    {
        HandleRef ref{h_rack, HANDLE_RACK};
        if (!ref) {
            return InvalidHandleError(h_rack, HANDLE_RACK);
        }
        p_rack = ref.Owner<Rack>();
        const System* p_system = p_rack->p_system;
        if (p_system && p_system->tid_render.load() == std::this_thread::get_id()) {
            return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OPERATION, "The rack is still in process ({:#x})", h_rack);
        }
    }
    if (!RetireHandle(h_rack, HANDLE_RACK)) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    if (System* p_system = p_rack->p_system) {
        std::scoped_lock lock{p_system->process_lock};
        if (p_rack->p_system == p_system) {
            UnlinkRack(p_system, p_rack);
        }
    }
    // The buffer may go back to the game's allocator below, so nothing in it is read afterwards.
    OrbisNgs2ContextBufferInfo buffer_info = p_rack->buffer_info;
    const OrbisNgs2BufferFreeHandler pfn_free = p_rack->pfn_free;
    TearDownRack(p_rack);
    if (pfn_free) {
        (void)pfn_free(&buffer_info); // the firmware ignores what the game's free answers
    }
    if (p_out_info) {
        *p_out_info = buffer_info;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackLock(OrbisNgs2Handle h_rack) {
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    auto* p_rack = ref.Owner<Rack>();
    p_rack->flush_lock.Lock();
    p_rack->c_flush_locks.fetch_add(1, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackUnlock(OrbisNgs2Handle h_rack) {
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    auto* p_rack = ref.Owner<Rack>();
    if (!p_rack->flush_lock.Unlock()) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    p_rack->c_flush_locks.fetch_sub(1, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackSetUserData(OrbisNgs2Handle h_rack, uintptr_t user_data) {
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    ref.Owner<Rack>()->user_data = user_data;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetUserData(OrbisNgs2Handle h_rack, uintptr_t* p_out_user_data) {
    if (!p_out_user_data) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack user data address ({})",
                    static_cast<void*>(p_out_user_data));
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        *p_out_user_data = 0;
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    *p_out_user_data = ref.Owner<Rack>()->user_data;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetVoiceHandle(OrbisNgs2Handle h_rack, u32 i_voice, OrbisNgs2Handle* p_out_handle) {
    if (!p_out_handle) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid voice handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        *p_out_handle = 0;
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    auto* p_rack = ref.Owner<Rack>();
    if (i_voice >= p_rack->limits.max_voices) {
        *p_out_handle = 0;
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_VOICE_INDEX, "Invalid voice index ({})", i_voice);
    }
    *p_out_handle = reinterpret_cast<OrbisNgs2Handle>(&p_rack->p_voices[i_voice].handle);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetInfo(OrbisNgs2Handle h_rack, OrbisNgs2RackInfo* p_out_info, std::size_t cb_info) {
    if (!p_out_info) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack info address ({})",
                    static_cast<void*>(p_out_info));
    }
    if (cb_info < sizeof(OrbisNgs2RackInfo)) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid rack info size ({}!={}[byte])", cb_info,
                    sizeof(OrbisNgs2RackInfo));
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    const auto* p_rack = ref.Owner<Rack>();
    const RackLimits& limits = p_rack->limits;
    FillRackKindInfo(*p_rack, p_out_info, cb_info);
    OrbisNgs2RackInfo info{};
    std::memcpy(info.name, p_rack->name, sizeof(info.name));
    info.rackHandle = h_rack;
    info.bufferInfo = p_rack->buffer_info;
    const System* p_system = p_rack->p_system;
    info.ownerSystemHandle = p_system ? reinterpret_cast<OrbisNgs2Handle>(&p_system->handle) : 0;
    info.type = p_rack->p_kind->rack_class;
    info.rackId = p_rack->p_kind->rack_id;
    info.uid = p_rack->uid;
    info.minGrainSamples = kMinGrainSamples;
    info.maxGrainSamples = limits.max_grain_samples;
    info.maxVoices = limits.max_voices;
    info.maxChannelWorks = limits.max_channel_works;
    info.maxInputs = limits.max_inputs;
    info.maxMatrices = limits.max_matrices;
    info.maxPorts = limits.max_ports;
    info.stateFlags = 1u | (p_rack->c_flush_locks ? 4u : 0u);
    info.renderCount = u64(p_rack->c_renders.load(std::memory_order_relaxed));
    info.activeVoiceCount = p_rack->c_active_voices;
    info.activeChannelWorkCount = p_rack->c_channel_works_used;
    *p_out_info = info;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle h_rack, u32 i_module,
                                                OrbisNgs2CustomModuleInfo* p_out_info, std::size_t cb_info) {
    if (!p_out_info) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid module information address ({})",
                    static_cast<void*>(p_out_info));
    }
    if (cb_info < sizeof(OrbisNgs2CustomModuleInfo)) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid module information size ({})", cb_info);
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    const RackLimits& limits = ref.Owner<Rack>()->limits;
    if (i_module >= limits.c_custom_modules) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_MODULE_INDEX, "Invalid module index ({})", i_module);
    }
    const CustomModule& module = limits.custom_modules[i_module];
    *p_out_info = {module.module_id, module.source_buffer_id, module.extra_buffer_id, module.dest_buffer_id,
                   module.off_state, module.cb_state,         0,                      0};
    // A module with its own info callback (UserFx2 at least) adds its part after these 0x20 bytes.
    if (module.module_id == 0x1f) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "sceNgs2CustomRackGetModuleInfo: module info callbacks (F6)");
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
