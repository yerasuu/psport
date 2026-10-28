// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler rack's processing (0x15640; spec 04 §2.2) and its source, modules 0x70 and 0x71
// (0x1e6a0, 0x21890, 0x21a30, 0x21fe0, 0x223c0; spec 05 §1 to §3, §7, §9): linear PCM read frame by
// frame from the queued waveform blocks, with their loops, exit-loop requests and streaming, then
// resampled by the pitch with the firmware's 4-tap windowed sinc at 512 phases.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kFractionBits = 14;
constexpr u32 kUnitStep = 1u << kFractionBits;
constexpr u32 kMaxStep = 0xffff;
constexpr u32 kPhases = 512;
constexpr u32 kTaps = 4;
constexpr u32 kHistoryTaps = 4;
constexpr u32 kInfinite = 0xffffffffu;
constexpr u32 kMaxSampleBytes = 8 * kMaxChannels;
// Blocks with neither skipped nor played samples are dropped; older SDKs also drop the ones with no
// samples that repeat forever (spec 05 §3.2).
constexpr u32 kEmptyBlockSdkVersion = 0x1700000;

enum CallbackType : u32 {
    CALLBACK_BLOCK_END = 1,
    CALLBACK_LOOP = 2,
};

// Firmware 11.00's table at 0x35970 (spec 06-tablas §C): for phase t = k / 512, the taps at -1 - t,
// -t, 1 - t and 2 - t of a sinc with a Hann window of half-width 2, normalized to sum 1.
struct InterpolationTable {
    float taps[kPhases][kTaps];
};

const InterpolationTable& GetInterpolationTable() {
    static const InterpolationTable s_table = [] {
        InterpolationTable table{};
        for (u32 i_phase = 0; i_phase < kPhases; ++i_phase) {
            const double t = double(i_phase) / kPhases;
            double weights[kTaps];
            double sum = 0.0;
            for (u32 i_tap = 0; i_tap < kTaps; ++i_tap) {
                const double d = double(i_tap) - 1.0 - t;
                const double sinc = d == 0.0 ? 1.0 : std::sin(std::numbers::pi * d) / (std::numbers::pi * d);
                weights[i_tap] = sinc * 0.5 * (1.0 + std::cos(std::numbers::pi * d / 2.0));
                sum += weights[i_tap];
            }
            for (u32 i_tap = 0; i_tap < kTaps; ++i_tap) {
                table.taps[i_phase][i_tap] = float(weights[i_tap] / sum);
            }
        }
        return table;
    }();
    return s_table;
}

u32 BytesPerSample(u32 waveform_type) {
    switch (waveform_type) {
    case 0x10:
    case 0x11:
        return 1;
    case 0x12:
    case 0x13:
        return 2;
    case 0x14:
    case 0x15:
        return 3;
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
        return 4;
    default:
        return 8;
    }
}

template <class T>
T Load(const u8* p_bytes, bool is_big_endian) {
    T value;
    std::memcpy(&value, p_bytes, sizeof(value));
    return is_big_endian ? std::byteswap(value) : value;
}

// The conversions of spec 05 §2: every integer format is scaled so that its full range is [-1, 1).
float ToFloat(u32 waveform_type, const u8* p_sample) {
    const bool is_big_endian = (waveform_type & 1) && waveform_type >= 0x13;
    switch (waveform_type) {
    case 0x10:
        return float(s8(p_sample[0])) / 128.0f;
    case 0x11:
        return float(s32(p_sample[0]) - 128) / 128.0f;
    case 0x12:
    case 0x13:
        return float(s16(Load<u16>(p_sample, is_big_endian))) / 32768.0f;
    case 0x14:
        return float(s32(u32(p_sample[0]) << 8 | u32(p_sample[1]) << 16 | u32(p_sample[2]) << 24)) / 2147483648.0f;
    case 0x15:
        return float(s32(u32(p_sample[2]) << 8 | u32(p_sample[1]) << 16 | u32(p_sample[0]) << 24)) / 2147483648.0f;
    case 0x16:
    case 0x17:
        return float(s32(Load<u32>(p_sample, is_big_endian))) / 2147483648.0f;
    case 0x18:
    case 0x19:
        return std::bit_cast<float>(Load<u32>(p_sample, is_big_endian));
    default:
        return float(std::bit_cast<double>(Load<u64>(p_sample, is_big_endian)));
    }
}

const WaveformBlock& BlockAt(const WaveformQueue& queue, u32 i_block) {
    return queue.p_blocks[(queue.i_head + i_block) % queue.c_capacity];
}

const u8* BlockData(const WaveformBlock& block) {
    return block.p_data + block.off_data;
}

// The head block and the continuation blocks after it: the bytes the head block plays (spec 05 §3.1).
struct Chain {
    u32 c_blocks;
    u64 cb_total;
};

Chain ChainOf(const WaveformQueue& queue) {
    Chain chain{0, 0};
    while (chain.c_blocks < queue.c_committed &&
           (chain.c_blocks == 0 || BlockAt(queue, chain.c_blocks).is_continuation)) {
        chain.cb_total += BlockAt(queue, chain.c_blocks).cb_data;
        ++chain.c_blocks;
    }
    return chain;
}

// Locates a byte of the chain: the block that holds it and the offset in that block.
struct ChainCursor {
    u32 i_block;
    u64 off;
};

ChainCursor Locate(const WaveformQueue& queue, const Chain& chain, u64 off) {
    ChainCursor cursor{0, off};
    while (cursor.i_block < chain.c_blocks && cursor.off >= BlockAt(queue, cursor.i_block).cb_data) {
        cursor.off -= BlockAt(queue, cursor.i_block).cb_data;
        ++cursor.i_block;
    }
    return cursor;
}

// A frame's samples may straddle two blocks of the chain; they are gathered into p_scratch then.
const u8* SampleBytes(const WaveformQueue& queue, const Chain& chain, u64 off, u32 cb_samples, u8* p_scratch) {
    ChainCursor cursor = Locate(queue, chain, off);
    const WaveformBlock& block = BlockAt(queue, cursor.i_block);
    if (cursor.off + cb_samples <= block.cb_data) {
        return BlockData(block) + cursor.off;
    }
    for (u32 i_byte = 0; i_byte < cb_samples && cursor.i_block < chain.c_blocks;) {
        const WaveformBlock& part = BlockAt(queue, cursor.i_block);
        const u32 cb_part = u32(std::min<u64>(part.cb_data - cursor.off, cb_samples - i_byte));
        std::memcpy(p_scratch + i_byte, BlockData(part) + cursor.off, cb_part);
        i_byte += cb_part;
        cursor = {cursor.i_block + 1, 0};
    }
    return p_scratch;
}

void PopBlock(Voice* p_voice, u32 c_repeats) {
    WaveformQueue& queue = p_voice->queue;
    const WaveformBlock& block = BlockAt(queue, 0);
    if (!block.is_notified) {
        QueueVoiceCallback(p_voice, CALLBACK_BLOCK_END, block, c_repeats);
    }
    queue.i_head = (queue.i_head + 1) % queue.c_capacity;
    --queue.c_blocks;
    --queue.c_committed;
}

// The head block and its continuation blocks leave the queue together (spec 05 §3.3 step 2).
void EndBlock(Voice* p_voice, SourceState* p_source) {
    const Chain chain = ChainOf(p_voice->queue);
    for (u32 i_block = 0; i_block < chain.c_blocks; ++i_block) {
        PopBlock(p_voice, p_source->c_repeats);
    }
    p_source->c_repeats = 0;
    p_source->has_block = false;
}

bool IsEmptyBlock(const WaveformBlock& block) {
    const bool has_no_samples = block.num_skip_samples == 0 && block.num_samples == 0;
    if (IsCompiledSdkAtLeast(kEmptyBlockSdkVersion)) {
        return has_no_samples;
    }
    return has_no_samples || (block.num_samples == 0 && block.num_repeats == kInfinite);
}

// 0x21fe0: starts the first block with samples, or marks the end (terminated queue) or a starved
// voice (the game has not queued more yet).
bool StartBlock(Voice* p_voice, SourceState* p_source) {
    WaveformQueue& queue = p_voice->queue;
    while (queue.c_committed > 0) {
        const WaveformBlock& block = BlockAt(queue, 0);
        if (IsEmptyBlock(block)) {
            PopBlock(p_voice, 0);
            continue;
        }
        p_source->num_skip_samples = block.num_skip_samples;
        p_source->num_samples = block.num_samples;
        p_source->num_repeats = block.num_repeats;
        p_source->block_user_data = block.user_data;
        p_source->cb_consumed = 0;
        p_source->i_sample = 0;
        p_source->c_repeats = 0;
        p_source->has_block = true;
        return true;
    }
    if (queue.is_terminated_committed) {
        p_source->is_end = true;
    } else {
        p_source->is_starved = true;
    }
    return false;
}

// 0x21a30 at the end of a block's region: loop back while repeats remain and no exit request waits,
// otherwise end the block (spec 05 §3.3).
void EndRegion(Voice* p_voice, SourceState* p_source) {
    if (p_source->num_repeats == kInfinite || p_source->c_repeats < p_source->num_repeats) {
        ++p_source->c_repeats;
        if (p_voice->queue.c_exit_requests <= p_source->c_exit_consumed) {
            p_source->i_sample = 0;
            p_source->cb_consumed = 0;
            QueueVoiceCallback(p_voice, CALLBACK_LOOP, BlockAt(p_voice->queue, 0), p_source->c_repeats);
            return;
        }
        ++p_source->c_exit_consumed;
    }
    EndBlock(p_voice, p_source);
}

u64 RegionLeft(const SourceState& source) {
    if (source.num_skip_samples == kInfinite || source.num_samples == kInfinite) {
        return ~u64(0);
    }
    const u64 c_region = u64(source.num_skip_samples) + source.num_samples;
    return c_region > source.i_sample ? c_region - source.i_sample : 0;
}

struct FrameLayout {
    u32 cb_sample;
    u32 cb_samples;
    u32 cb_stride;
    u32 off_samples;
};

FrameLayout LayoutOf(const SourceFormat& format) {
    const u32 cb_sample = BytesPerSample(format.waveform_type);
    const u32 cb_samples = cb_sample * format.c_channels;
    return {cb_sample, cb_samples, format.cb_frame_margin + cb_samples, format.off_frame};
}

u32 FramesAvailable(const Chain& chain, const FrameLayout& layout, u64 cb_consumed) {
    const u64 cb_first = cb_consumed + layout.off_samples + layout.cb_samples;
    if (chain.cb_total < cb_first) {
        return 0;
    }
    return u32(std::min<u64>((chain.cb_total - cb_first) / layout.cb_stride + 1, kInfinite));
}

// Streaming: once every byte of the last queued block is read, the game may refill it before the
// block ends (spec 05 §7).
void NotifyDrainedTail(Voice* p_voice, const SourceState& source, const Chain& chain) {
    WaveformQueue& queue = p_voice->queue;
    if (queue.is_terminated_committed || chain.c_blocks != queue.c_committed || source.cb_consumed < chain.cb_total) {
        return;
    }
    WaveformBlock& tail = queue.p_blocks[(queue.i_head + chain.c_blocks - 1) % queue.c_capacity];
    if (!tail.is_notified) {
        QueueVoiceCallback(p_voice, CALLBACK_BLOCK_END, tail, source.c_repeats);
        tail.is_notified = true;
    }
}

void DecodeFrames(const WaveformQueue& queue, const Chain& chain, const SourceFormat& format,
                  const FrameLayout& layout, u64 cb_consumed, float* const* pp_in, u32 i_out, u32 c_frames) {
    u8 scratch[kMaxSampleBytes];
    for (u32 i_frame = 0; i_frame < c_frames; ++i_frame) {
        const u64 off = cb_consumed + u64(i_frame) * layout.cb_stride + layout.off_samples;
        const u8* p_samples = SampleBytes(queue, chain, off, layout.cb_samples, scratch);
        for (u32 i_channel = 0; i_channel < format.c_channels; ++i_channel) {
            pp_in[i_channel][i_out + i_frame] = ToFloat(format.waveform_type, p_samples + i_channel * layout.cb_sample);
        }
    }
}

// Reads up to c_wanted frames into pp_in from i_out on; returns how many it read.
u32 Decode(Voice* p_voice, SourceState* p_source, const SourceFormat& format, float* const* pp_in, u32 i_out,
           u32 c_wanted) {
    const WaveformQueue& queue = p_voice->queue;
    const FrameLayout layout = LayoutOf(format);
    u32 c_done = 0;
    // Each step that reads nothing drops or loops a block, so the queue's size bounds them.
    for (u32 c_idle_steps = 0; c_done < c_wanted && c_idle_steps <= 2 * queue.c_capacity + 2;) {
        if (!p_source->has_block && !StartBlock(p_voice, p_source)) {
            break;
        }
        const u64 c_left = RegionLeft(*p_source);
        if (c_left == 0) {
            EndRegion(p_voice, p_source);
            ++c_idle_steps;
            continue;
        }
        const Chain chain = ChainOf(queue);
        const u32 c_available = FramesAvailable(chain, layout, p_source->cb_consumed);
        if (c_available == 0) {
            const bool can_grow = chain.c_blocks == queue.c_committed && !queue.is_terminated_committed;
            if (can_grow) {
                p_source->is_starved = true;
                break;
            }
            // The block's data ends before its samples do; what the firmware does then is not read.
            static std::uint64_t s_c_uses;
            NoteUnimplemented(&s_c_uses, "Ngs2 sampler block shorter than its samples (ended early; rea: 0x21890)");
            EndBlock(p_voice, p_source);
            ++c_idle_steps;
            continue;
        }
        const u32 c_frames = u32(std::min<u64>({c_wanted - c_done, c_left, c_available}));
        DecodeFrames(queue, chain, format, layout, p_source->cb_consumed, pp_in, i_out + c_done, c_frames);
        p_source->i_sample += c_frames;
        p_source->cb_consumed += c_frames * layout.cb_stride;
        p_source->c_decoded_samples += c_frames;
        p_source->cb_decoded += u64(c_frames) * layout.cb_stride;
        c_done += c_frames;
        c_idle_steps = 0;
        NotifyDrainedTail(p_voice, *p_source, chain);
    }
    return c_done;
}

const u8* ReadPosition(const WaveformQueue& queue, const SourceState& source) {
    if (!source.has_block || queue.c_committed == 0) {
        return nullptr;
    }
    const Chain chain = ChainOf(queue);
    const ChainCursor cursor = Locate(queue, chain, source.cb_consumed);
    if (cursor.i_block >= chain.c_blocks) {
        const WaveformBlock& last = BlockAt(queue, chain.c_blocks - 1);
        return BlockData(last) + last.cb_data;
    }
    return BlockData(BlockAt(queue, cursor.i_block)) + cursor.off;
}

void Resample(const float* p_in, float* p_out, u32 c_samples, u32 step, u32 fraction) {
    if (step == kUnitStep && fraction == 0) {
        std::memcpy(p_out, p_in + 1, c_samples * sizeof(float));
        return;
    }
    const InterpolationTable& table = GetInterpolationTable();
    for (u32 i = 0; i < c_samples; ++i) {
        const u32 pos = fraction + i * step;
        const float* p_taps = table.taps[(pos >> 5) & (kPhases - 1)];
        const float* p_at = p_in + (pos >> kFractionBits);
        p_out[i] = p_at[0] * p_taps[0] + p_at[1] * p_taps[1] + p_at[2] * p_taps[2] + p_at[3] * p_taps[3];
    }
}

// Fills the voice's planes with one grain of the source and answers the state flags it leaves.
u32 ProcessSource(RenderContext& context, Voice* p_voice, const Planes& out) {
    const VoiceParams& params = *p_voice->p_active_params;
    const SourceFormat& format = params.format;
    SourceState& source = p_voice->p_dsp->source;
    WaveformQueue& queue = p_voice->queue;
    const u32 c_channels = p_voice->c_out_channels;
    const u32 c_samples = context.c_grain_samples;
    if (queue.was_reset) {
        queue.was_reset = false;
        source.has_block = false;
        source.is_end = false;
        source.c_exit_consumed = 0;
    }
    if (p_voice->are_params_changed) {
        const double step = double(params.pitch_ratio) * kUnitStep * format.sample_rate_hz / context.sample_rate_hz;
        source.step = step > 0.0 ? u32(std::min(step, double(kMaxStep))) : 0;
    }
    if (p_voice->is_first_grain) {
        source.fraction = 0;
    }
    if (p_voice->flags & STATE_PAUSED) {
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            std::memset(out.p_channel[i_channel], 0, c_samples * sizeof(float));
        }
        return p_voice->flags;
    }
    const u32 c_needed = ((source.step * c_samples + source.fraction) >> kFractionBits) + kHistoryTaps;
    float* pp_in[kMaxChannels];
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        pp_in[i_channel] = p_voice->p_rack->p_decode_scratch + std::size_t(i_channel) * p_voice->p_rack->c_decode_stride;
        std::copy_n(source.history[i_channel], source.c_history, pp_in[i_channel]);
    }
    u32 c_input = source.c_history;
    source.is_starved = false;
    if (c_input < c_needed) {
        const u32 c_decoded = Decode(p_voice, &source, format, pp_in, c_input, c_needed - c_input);
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            std::fill(pp_in[i_channel] + c_input + c_decoded, pp_in[i_channel] + c_needed, 0.0f);
        }
        c_input = c_needed;
    }
    const u32 c_consumed = c_needed - kHistoryTaps;
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        Resample(pp_in[i_channel], out.p_channel[i_channel], c_samples, source.step, source.fraction);
        std::copy(pp_in[i_channel] + c_consumed, pp_in[i_channel] + c_input, source.history[i_channel]);
    }
    source.c_history = c_input - c_consumed;
    source.fraction = (source.fraction + c_samples * source.step) & (kUnitStep - 1);
    source.p_read = ReadPosition(queue, source);
    const u32 kept_flags = p_voice->flags & (STATE_IN_USE | STATE_PAUSED | STATE_STOPPED);
    if (source.is_end && source.c_history <= kHistoryTaps) {
        return 0;
    }
    if (source.is_starved) {
        return kept_flags | STATE_EMPTY;
    }
    return kept_flags | STATE_PLAYING;
}

void RunFilters(const Voice& voice, u32 location, const Planes& planes, u32 c_channels, u32 c_samples) {
    for (u32 i_filter = 0; i_filter < voice.p_rack->limits.max_filters; ++i_filter) {
        ProcessFilter(voice.p_active_filters[i_filter], location,
                      voice.p_filter_states + std::size_t(i_filter) * kMaxChannels, planes, c_channels, c_samples,
                      voice.is_first_grain);
    }
}

} // namespace

void ResetSource(Voice* p_voice) {
    SourceState& source = p_voice->p_dsp->source;
    source = {};
    p_voice->queue.was_reset = false;
}

void ProcessSamplerVoice(RenderContext& context, Voice* p_voice, u32* p_event_code) {
    const VoiceParams& params = *p_voice->p_active_params;
    const u32 c_channels = p_voice->c_out_channels;
    const u32 c_samples = context.c_grain_samples;
    const Planes planes = WorkPlanes(context, WORK_VOICE);
    VoiceDsp& dsp = *p_voice->p_dsp;
    p_voice->flags = ProcessSource(context, p_voice, planes);
    RunFilters(*p_voice, 0, planes, c_channels, c_samples);
    dsp.envelope_height = ProcessEnvelope(params.envelope, &dsp.envelope, planes, c_channels, c_samples,
                                          p_voice->is_first_grain, &p_voice->flags, p_event_code);
    ProcessDistortion(params.distortion, planes, c_channels, c_samples);
    if (params.user_fx.is_set && params.user_fx.pfn_handler) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 sampler UserFx 0x10000008: the game's handler is not called yet (F6)");
    }
    RunFilters(*p_voice, 1, planes, c_channels, c_samples);
    if (!(p_voice->flags & STATE_PAUSED)) {
        ProcessPeakMeter(params.meter_mode, &p_voice->meters[0], planes, c_channels, c_samples,
                         p_voice->is_first_grain, dsp.peaks[0]);
    }
    ApplyEventCode(planes, c_channels, c_samples, *p_event_code);
    MixPorts(context, p_voice, planes, c_channels);
    OrbisNgs2SamplerVoiceState state{};
    state.voiceState.stateFlags = p_voice->flags;
    state.envelopeHeight = dsp.envelope_height;
    state.peakHeight = dsp.peaks[0][0];
    state.numDecodedSamples = dsp.source.c_decoded_samples;
    state.decodedDataSize = dsp.source.cb_decoded;
    state.userData = dsp.source.block_user_data;
    state.waveformData = dsp.source.p_read;
    std::memcpy(p_voice->p_state, &state, sizeof(state));
}

} // namespace Libraries::Ngs2
