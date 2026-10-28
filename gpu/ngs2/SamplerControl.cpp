// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler rack's VoiceControl parameters, 0x1000xxxx (0x153a0, 0x1dbd0; spec 03 §5.1, 05 §3.1,
// 09 §6): the source's setup, waveform block ring and its address change, pitch, envelope,
// distortion, UserFx, peak meter and filters, and their read-back. Phase F3 plays linear PCM; VAG and
// ATRAC9 (F4) and the waveform generators are refused with a counted 0x804A0001.
#include <algorithm>
#include <cstring>

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

// The slots keep what they held: 0x10008001 still lists them (spec 09 §6.3).
void ClearQueue(WaveformQueue* p_queue) {
    p_queue->i_write = 0;
    p_queue->c_queued = 0;
    p_queue->c_valid = 0;
    p_queue->c_exit_requests = 0;
    p_queue->is_terminated = false;
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
    // The source and the pitch are the setup's own; every other module waits for its parameter.
    params.set_modules = 0;
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
    // Bit 3 marks every slot already in the ring, so the render drops the queued blocks after the one
    // it plays when their turn comes; the slots added now are unmarked (spec 09 §7).
    if (p_param->flags & ADD_BLOCKS_MARK) {
        for (u32 i_slot = 0; i_slot < queue.c_capacity; ++i_slot) {
            queue.p_slots[i_slot].is_marked = true;
        }
    }
    for (u32 i_block = 0; i_block < p_param->numBlocks; ++i_block) {
        const OrbisNgs2WaveformBlock& block = p_param->aBlock[i_block];
        if (is_strict && block.numSamples == 0 && block.numRepeats == kInfiniteRepeats) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_BLOCK_NUM_REPEATS,
                        "Invalid waveform block repeats ({})", block.numRepeats);
        }
        if (can_merge && queue.c_queued > 0) {
            WaveformBlock& last = queue.p_slots[(queue.i_write + queue.c_capacity - 1) % queue.c_capacity];
            if (last.p_data == p_data && u64(last.off_data) + last.cb_data == block.dataOffset) {
                last.cb_data += block.dataSize;
                continue;
            }
        }
        if (queue.c_queued >= queue.c_capacity) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS, "Waveform block overflow ({})",
                        queue.c_capacity);
        }
        WaveformBlock slot{p_data,           block.userData,   block.dataOffset, block.dataSize, block.numSkipSamples,
                           block.numSamples, block.numRepeats, is_continuation,  false};
        if (is_continuation) {
            slot.user_data = 0;
            slot.num_skip_samples = 0;
            slot.num_samples = 0;
            slot.num_repeats = 0;
        }
        queue.p_slots[queue.i_write] = slot;
        queue.i_write = (queue.i_write + 1) % queue.c_capacity;
        ++queue.c_queued;
        queue.c_valid = std::min(queue.c_valid + 1, queue.c_capacity);
    }
    if (!(p_param->flags & ADD_BLOCKS_NOT_LAST)) {
        queue.is_terminated = true;
    }
    return ORBIS_OK;
}

// 0x10000002 (0x1dbd0 case 2, spec 09 §6.1): every slot of the ring with the old address, queued or
// not; the render takes it at the next flush.
s32 ReplaceAddress(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2SamplerVoiceWaveformAddressParam* p_param) {
    if (!p_param->from || !p_param->to) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "Invalid waveform address ({},{})",
                    p_param->from, p_param->to);
    }
    WaveformQueue& queue = p_voice->queue;
    for (u32 i_slot = 0; i_slot < queue.c_capacity; ++i_slot) {
        if (queue.p_slots[i_slot].p_data == p_param->from) {
            queue.p_slots[i_slot].p_data = static_cast<const u8*>(p_param->to);
        }
    }
    return ORBIS_OK;
}

// What 0x10008001 writes per slot: the slot's data address and the game's block structure.
struct BlockRecord {
    const void* p_data;
    OrbisNgs2WaveformBlock block;
};
static_assert(sizeof(BlockRecord) == 0x28);

// 0x10008001 (0x7490, spec 09 §6.3) as the firmware walks the ring: from the oldest of the c_valid
// slots written since the last reset up to the newest one, which it leaves out; with one valid slot it
// goes round the whole ring. The render never lowers c_valid, so this is the ring's history rather
// than the live queue. The game's parameter has the address param's size: a word at 8 that reads 0,
// the record count at 0xc and the record array at 0x10, filled up to maxWaveformBlocks records.
void ReadBlocks(const Voice& voice, OrbisNgs2VoiceParamHeader* p_header) {
    auto* p_bytes = reinterpret_cast<u8*>(p_header);
    void* p_records;
    std::memcpy(&p_records, p_bytes + 0x10, sizeof(p_records));
    std::memset(p_bytes + 8, 0, 8);
    const WaveformQueue& queue = voice.queue;
    u32 c_records = 0;
    if (voice.p_params->is_setup && p_records && queue.c_valid > 0) {
        const u32 c_capacity = queue.c_capacity;
        u32 i_slot = (queue.i_write + c_capacity - queue.c_valid) % c_capacity;
        const u32 i_last = (queue.i_write + c_capacity - 1) % c_capacity;
        for (c_records = 1;; ++c_records) {
            const WaveformBlock& slot = queue.p_slots[i_slot];
            const BlockRecord record{slot.p_data,
                                     {slot.off_data, slot.cb_data, slot.num_repeats, slot.num_skip_samples,
                                      slot.num_samples, 0, slot.user_data}};
            std::memcpy(static_cast<u8*>(p_records) + std::size_t(c_records - 1) * sizeof(record), &record,
                        sizeof(record));
            i_slot = (i_slot + 1) % c_capacity;
            if (i_slot == i_last || c_records >= c_capacity) {
                break;
            }
        }
    }
    std::memcpy(p_bytes + 0xc, &c_records, sizeof(c_records));
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
    case SAMPLER_REPLACE_ADDRESS:
        return ReplaceAddress(h_voice, p_voice, ParamOf<OrbisNgs2SamplerVoiceWaveformAddressParam>(p_header));
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
        return SetEnvelope(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header));
    case SAMPLER_DISTORTION:
        SetDistortion(ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header), &params);
        return ORBIS_OK;
    case SAMPLER_USER_FX:
        SetUserFx(ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header), &params);
        return ORBIS_OK;
    case SAMPLER_PEAK_METER:
        return SetPeakMeter(h_voice, ParamOf<OrbisNgs2SamplerVoicePeakMeterParam>(p_header), &params);
    case SAMPLER_FILTER:
        return SetFilter(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header));
    case SAMPLER_READ_SETUP:
        ReadSetup(params, ParamOf<OrbisNgs2SamplerVoiceSetupParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_PITCH:
        ParamOf<OrbisNgs2SamplerVoicePitchParam>(p_header)->ratio = params.pitch_ratio;
        return ORBIS_OK;
    case SAMPLER_READ_ENVELOPE:
        ReadEnvelope(params, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_DISTORTION:
        ReadDistortion(params, ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_USER_FX:
        ReadUserFx(params, ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header));
        return ORBIS_OK;
    case SAMPLER_READ_PEAK_METER:
        ParamOf<OrbisNgs2SamplerVoicePeakMeterParam>(p_header)->enableFlag = ReadPeakMeter(params);
        return ORBIS_OK;
    case SAMPLER_READ_FILTER:
        return ReadFilter(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header));
    case SAMPLER_READ_MAX_FILTERS:
        ParamOf<OrbisNgs2SamplerVoiceNumFilters>(p_header)->numFilters = p_voice->p_rack->limits.max_filters;
        return ORBIS_OK;
    case SAMPLER_READ_BLOCKS:
        ReadBlocks(*p_voice, ParamOf<OrbisNgs2VoiceParamHeader>(p_header));
        return ORBIS_OK;
    default:
        return InvalidControlId(h_voice, p_header);
    }
}

} // namespace Libraries::Ngs2
