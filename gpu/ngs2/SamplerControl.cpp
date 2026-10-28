// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler rack's VoiceControl parameters, 0x1000xxxx (0x153a0, 0x1dbd0; spec 03 §5.1, 05 §3.1):
// the source's setup and waveform block queue, pitch, envelope, distortion, UserFx, peak meter and
// filters, and their read-back. Phase F3 plays linear PCM; VAG and ATRAC9 (F4) and the waveform
// generators are refused with a counted 0x804A0001.
#include <algorithm>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

enum SamplerParamId : u32 {
    SAMPLER_SETUP = 0x10000000,
    SAMPLER_ADD_BLOCKS = 0x10000001,
    SAMPLER_REPLACE_ADDRESS = 0x10000002,
    SAMPLER_FRAME_OFFSET = 0x10000003,
    SAMPLER_EXIT_LOOP = 0x10000004,
    SAMPLER_PITCH = 0x10000005,
    SAMPLER_ENVELOPE = 0x10000006,
    SAMPLER_DISTORTION = 0x10000007,
    SAMPLER_USER_FX = 0x10000008,
    SAMPLER_PEAK_METER = 0x10000009,
    SAMPLER_FILTER = 0x1000000a,
    SAMPLER_READ_SETUP = 0x10008000,
    SAMPLER_READ_BLOCKS = 0x10008001,
    SAMPLER_READ_PITCH = 0x10008005,
    SAMPLER_READ_ENVELOPE = 0x10008006,
    SAMPLER_READ_DISTORTION = 0x10008007,
    SAMPLER_READ_USER_FX = 0x10008008,
    SAMPLER_READ_PEAK_METER = 0x10008009,
    SAMPLER_READ_FILTER = 0x1000800a,
    SAMPLER_READ_MAX_FILTERS = 0x1000800b,
};

enum WaveformType : u32 {
    WAVEFORM_FIRST_PCM = 0x10,
    WAVEFORM_LAST_PCM = 0x1b,
    WAVEFORM_VAG = 0x1c,
    WAVEFORM_ATRAC9 = 0x40,
    WAVEFORM_FIRST_GENERATOR = 0x80,
    WAVEFORM_LAST_GENERATOR = 0x87,
};

enum AddBlocksFlag : u32 {
    ADD_BLOCKS_NOT_LAST = 0x1,
    ADD_BLOCKS_CONTINUE = 0x2,
    ADD_BLOCKS_RESET = 0x4,
    ADD_BLOCKS_MARK = 0x8,
    ADD_BLOCKS_NO_MERGE = 0x10,
};

constexpr u32 kMaxWaveformRateHz = 192000;
constexpr u32 kMaxFrameField = 0xffff;
constexpr u32 kInfiniteRepeats = 0xffffffffu;
// Blocks with no samples and endless repeats are refused for titles built with SDK 1.70 or later.
constexpr u32 kEndlessEmptyBlockSdkVersion = 0x1700000;

bool IsWaveformType(u32 type) {
    return (type >= WAVEFORM_FIRST_PCM && type <= WAVEFORM_VAG) || type == WAVEFORM_ATRAC9 ||
           (type >= WAVEFORM_FIRST_GENERATOR && type <= WAVEFORM_LAST_GENERATOR);
}

template <class Param>
Param* ParamOf(const OrbisNgs2VoiceParamHeader* p_header) {
    return reinterpret_cast<Param*>(const_cast<OrbisNgs2VoiceParamHeader*>(p_header));
}

void ClearQueue(WaveformQueue* p_queue) {
    p_queue->i_head = 0;
    p_queue->c_blocks = 0;
    p_queue->c_committed = 0;
    p_queue->c_exit_requests = 0;
    p_queue->is_terminated = false;
    p_queue->is_terminated_committed = false;
    p_queue->was_reset = true;
}

s32 Setup(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2SamplerVoiceSetupParam* p_param) {
    const OrbisNgs2WaveformFormat& format = p_param->format;
    if (!IsWaveformType(format.waveformType)) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE, "Invalid waveform type ({:#x})",
                    format.waveformType);
    }
    if (format.numChannels < 1 || format.numChannels > kMaxChannels) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({})",
                    format.numChannels);
    }
    if (format.sampleRate < 1 || format.sampleRate > kMaxWaveformRateHz) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SAMPLE_RATE, "Invalid waveform sample rate ({})",
                    format.sampleRate);
    }
    if (format.frameMargin > kMaxFrameField || format.frameMargin < format.frameOffset) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FRAME, "Invalid waveform frame ({},{})",
                    format.frameOffset, format.frameMargin);
    }
    if (format.waveformType > WAVEFORM_LAST_PCM) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 sampler setup: VAG, ATRAC9 and generator waveforms (F4)");
        return Fail(h_voice, ORBIS_NGS2_ERROR_FAIL, "Waveform type ({:#x}) not implemented yet",
                    format.waveformType);
    }
    VoiceParams& params = *p_voice->p_params;
    params.format = {format.waveformType, format.numChannels, format.sampleRate, format.configData,
                     format.frameOffset,  format.frameMargin, p_param->flags};
    params.pitch_ratio = 1.0f;
    params.c_in_channels = 0;
    params.c_out_channels = format.numChannels;
    params.is_setup = true;
    ClearQueue(&p_voice->queue);
    p_voice->control.fetch_or(VOICE_PENDING_SETUP, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 AddBlocks(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2SamplerVoiceWaveformBlocksParam* p_param) {
    WaveformQueue& queue = p_voice->queue;
    if (p_param->flags & ADD_BLOCKS_RESET) {
        ClearQueue(&queue);
    }
    if (p_param->numBlocks > 0 && !p_param->aBlock) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS, "Invalid waveform block address ({})",
                    static_cast<const void*>(p_param->aBlock));
    }
    if (!p_param->data) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "Invalid waveform data address ({})",
                    p_param->data);
    }
    if (queue.is_terminated) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS, "Waveform has terminated");
    }
    const bool is_continuation = p_param->flags & ADD_BLOCKS_CONTINUE;
    const bool can_merge = is_continuation && !(p_param->flags & ADD_BLOCKS_NO_MERGE);
    const bool is_strict = IsCompiledSdkAtLeast(kEndlessEmptyBlockSdkVersion);
    const auto* p_data = static_cast<const u8*>(p_param->data);
    for (u32 i_block = 0; i_block < p_param->numBlocks; ++i_block) {
        const OrbisNgs2WaveformBlock& block = p_param->aBlock[i_block];
        if (is_strict && block.numSamples == 0 && block.numRepeats == kInfiniteRepeats) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_BLOCK_NUM_REPEATS,
                        "Invalid waveform block repeats ({})", block.numRepeats);
        }
        if (can_merge && queue.c_blocks > 0) {
            WaveformBlock& last = queue.p_blocks[(queue.i_head + queue.c_blocks - 1) % queue.c_capacity];
            if (last.p_data == p_data && u64(last.off_data) + last.cb_data == block.dataOffset) {
                last.cb_data += block.dataSize;
                continue;
            }
        }
        if (queue.c_blocks >= queue.c_capacity) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS, "Waveform block overflow ({})",
                        queue.c_capacity);
        }
        WaveformBlock entry{p_data, block.userData, block.dataOffset, block.dataSize, block.numSkipSamples,
                            block.numSamples, block.numRepeats, is_continuation, (p_param->flags & ADD_BLOCKS_MARK) != 0,
                            false};
        if (is_continuation) {
            entry.user_data = 0;
            entry.num_skip_samples = 0;
            entry.num_samples = 0;
            entry.num_repeats = 0;
        }
        queue.p_blocks[(queue.i_head + queue.c_blocks) % queue.c_capacity] = entry;
        ++queue.c_blocks;
    }
    if (!(p_param->flags & ADD_BLOCKS_NOT_LAST)) {
        queue.is_terminated = true;
    }
    return ORBIS_OK;
}

void ReadSetup(const VoiceParams& params, OrbisNgs2SamplerVoiceSetupParam* p_param) {
    const SourceFormat& format = params.format;
    p_param->format = {format.waveform_type, format.c_channels,  format.sample_rate_hz,
                       format.config_data,   format.off_frame,   format.cb_frame_margin};
    p_param->flags = format.flags;
}

u32 ParamSize(u32 id) {
    switch (id) {
    case SAMPLER_SETUP:
    case SAMPLER_READ_SETUP:
        return sizeof(OrbisNgs2SamplerVoiceSetupParam);
    case SAMPLER_ADD_BLOCKS:
        return sizeof(OrbisNgs2SamplerVoiceWaveformBlocksParam);
    case SAMPLER_REPLACE_ADDRESS:
    case SAMPLER_READ_BLOCKS:
        return sizeof(OrbisNgs2SamplerVoiceWaveformAddressParam);
    case SAMPLER_FRAME_OFFSET:
        return sizeof(OrbisNgs2SamplerVoiceWaveformFrameOffsetParam);
    case SAMPLER_EXIT_LOOP:
        return sizeof(OrbisNgs2SamplerVoiceExitLoopParam);
    case SAMPLER_PITCH:
    case SAMPLER_READ_PITCH:
        return sizeof(OrbisNgs2SamplerVoicePitchParam);
    case SAMPLER_ENVELOPE:
    case SAMPLER_READ_ENVELOPE:
        return sizeof(OrbisNgs2SamplerVoiceEnvelopeParam);
    case SAMPLER_DISTORTION:
    case SAMPLER_READ_DISTORTION:
        return sizeof(OrbisNgs2SamplerVoiceDistortionParam);
    case SAMPLER_USER_FX:
    case SAMPLER_READ_USER_FX:
        return sizeof(OrbisNgs2SamplerVoiceUserFxParam);
    case SAMPLER_PEAK_METER:
    case SAMPLER_READ_PEAK_METER:
        return sizeof(OrbisNgs2SamplerVoicePeakMeterParam);
    case SAMPLER_FILTER:
    case SAMPLER_READ_FILTER:
        return sizeof(OrbisNgs2SamplerVoiceFilterParam);
    case SAMPLER_READ_MAX_FILTERS:
        return sizeof(OrbisNgs2SamplerVoiceNumFilters);
    default:
        return 0;
    }
}

} // namespace

s32 ControlSampler(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    const u32 cb_expected = ParamSize(p_header->id);
    if (cb_expected == 0) {
        return InvalidControlId(h_voice, p_header);
    }
    if (p_header->size != cb_expected) {
        return InvalidControlSize(h_voice, p_header, cb_expected);
    }
    VoiceParams& params = *p_voice->p_params;
    switch (p_header->id) {
    case SAMPLER_SETUP:
        return Setup(h_voice, p_voice, ParamOf<OrbisNgs2SamplerVoiceSetupParam>(p_header));
    case SAMPLER_ADD_BLOCKS:
        return AddBlocks(h_voice, p_voice, ParamOf<OrbisNgs2SamplerVoiceWaveformBlocksParam>(p_header));
    case SAMPLER_REPLACE_ADDRESS: {
        const auto* p_param = ParamOf<OrbisNgs2SamplerVoiceWaveformAddressParam>(p_header);
        if (!p_param->from || !p_param->to) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "Invalid waveform address ({},{})",
                        p_param->from, p_param->to);
        }
        // Spec 05 §8 and §10: what it does to the queue and to the data lock bits is not read yet.
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 sampler 0x10000002: waveform address change (rea: 0x1dbd0)");
        return Fail(h_voice, ORBIS_NGS2_ERROR_FAIL, "Voice control ID ({:#x}) not implemented yet", p_header->id);
    }
    case SAMPLER_FRAME_OFFSET: {
        const u32 off_frame = ParamOf<OrbisNgs2SamplerVoiceWaveformFrameOffsetParam>(p_header)->frameOffset;
        if (off_frame > kMaxFrameField) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FRAME, "Invalid waveform frame offset ({})",
                        off_frame);
        }
        params.format.off_frame = off_frame;
        return ORBIS_OK;
    }
    case SAMPLER_EXIT_LOOP:
        ++p_voice->queue.c_exit_requests;
        return ORBIS_OK;
    case SAMPLER_PITCH:
        params.pitch_ratio = std::clamp(ParamOf<OrbisNgs2SamplerVoicePitchParam>(p_header)->ratio, 0.0f, 4.0f);
        return ORBIS_OK;
    case SAMPLER_ENVELOPE:
        return SetEnvelope(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header),
                           &params.envelope);
    case SAMPLER_DISTORTION:
        SetDistortion(ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header), &params.distortion);
        return ORBIS_OK;
    case SAMPLER_USER_FX:
        SetUserFx(ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header), &params.user_fx);
        return ORBIS_OK;
    case SAMPLER_PEAK_METER:
        return SetPeakMeter(h_voice, ParamOf<OrbisNgs2SamplerVoicePeakMeterParam>(p_header), &params.meter_mode);
    case SAMPLER_FILTER:
        return SetFilter(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header));
    case SAMPLER_READ_SETUP:
        ReadSetup(params, ParamOf<OrbisNgs2SamplerVoiceSetupParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_PITCH:
        ParamOf<OrbisNgs2SamplerVoicePitchParam>(p_header)->ratio = params.pitch_ratio;
        return ORBIS_OK;
    case SAMPLER_READ_ENVELOPE:
        ReadEnvelope(params.envelope, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_DISTORTION:
        ReadDistortion(params.distortion, ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_USER_FX:
        ReadUserFx(params.user_fx, ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_PEAK_METER:
        ParamOf<OrbisNgs2SamplerVoicePeakMeterParam>(p_header)->enableFlag = params.meter_mode;
        return ORBIS_OK;
    case SAMPLER_READ_FILTER:
        return ReadFilter(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header));
    case SAMPLER_READ_MAX_FILTERS:
        ParamOf<OrbisNgs2SamplerVoiceNumFilters>(p_header)->numFilters = p_voice->p_rack->limits.max_filters;
        return ORBIS_OK;
    default: {
        // 0x10008001 lists the queued blocks in 0x28-byte records whose layout is not read yet.
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 sampler 0x10008001: queued block read (rea: 0x1dbd0 record layout)");
        return Fail(h_voice, ORBIS_NGS2_ERROR_FAIL, "Voice control ID ({:#x}) not implemented yet", p_header->id);
    }
    }
}

} // namespace Libraries::Ngs2
