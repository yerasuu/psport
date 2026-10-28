// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler rack's processing (0x15640; spec 04 §2.2) and its source, modules 0x70 and 0x71
// (0x1e6a0, 0x21890, 0x21a30, 0x21fe0, 0x223c0; spec 05 §1 to §3, §7, §9): linear PCM read frame by
// frame from the queued waveform blocks, with their loops, exit-loop requests and streaming, then
// resampled by the pitch with the firmware's 4-tap windowed sinc at 512 phases.
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <numbers>
#include <thread>

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

const WaveformBlock& QueuedSlot(const ActiveWaveformQueue& queue, u32 c_capacity, u32 i_queued) {
    return queue.p_slots[(queue.i_write + c_capacity - queue.c_queued + i_queued) % c_capacity];
}

const u8* SlotData(const WaveformBlock& slot) {
    return slot.p_data + slot.off_data;
}

bool IsEmptyBlock(const WaveformBlock& slot) {
    const bool has_no_samples = slot.num_skip_samples == 0 && slot.num_samples == 0;
    if (IsCompiledSdkAtLeast(kEmptyBlockSdkVersion)) {
        return has_no_samples;
    }
    return has_no_samples || (slot.num_samples == 0 && slot.num_repeats == kInfinite);
}

u32 RegionEnd(const SourceState& source) {
    if ((source.num_skip_samples | source.num_samples) == kInfinite) {
        return kInfinite;
    }
    return source.num_skip_samples + source.num_samples;
}

// The oldest slot leaves the queue; its block-end callback hands back the bytes not handed back yet.
void PopSlot(Voice* p_voice, const SourceState& source) {
    ActiveWaveformQueue& queue = p_voice->active_queue;
    const WaveformBlock& slot = QueuedSlot(queue, p_voice->queue.c_capacity, 0);
    if (source.cb_notified < slot.cb_data) {
        QueueVoiceCallback(p_voice, CALLBACK_BLOCK_END, slot, source.block_user_data, source.cb_notified,
                           source.c_repeats);
    }
    --queue.c_queued;
    ++queue.c_popped;
}

// 0x21fe0: the oldest slot starts a block unless it continues another block, was marked by a later
// 0x10000001 with bit 3, or has no samples; those leave the queue unplayed.
bool StartBlock(Voice* p_voice, SourceState* p_source) {
    ActiveWaveformQueue& queue = p_voice->active_queue;
    while (queue.c_queued > 0) {
        const WaveformBlock& slot = QueuedSlot(queue, p_voice->queue.c_capacity, 0);
        if (!slot.is_continuation && !slot.is_marked && !IsEmptyBlock(slot)) {
            p_source->num_skip_samples = slot.num_skip_samples;
            p_source->num_samples = slot.num_samples;
            p_source->num_repeats = slot.num_repeats;
            p_source->block_user_data = slot.user_data;
            p_source->p_read = SlotData(slot);
            return true;
        }
        p_source->cb_decoded += slot.cb_data;
        PopSlot(p_voice, *p_source);
    }
    if (queue.is_terminated) {
        p_source->is_end = true;
    }
    return false;
}

// 0x21a30, ported as rea read it (spec 09 §7): takes cb_bytes and c_samples just decoded from the
// oldest slots and moves the block on. At the end of a slot it pops the slot if the block's region
// goes on with the next one, whatever that slot is; at the end of the region (or of a slot with
// nothing after it in a terminated queue) the block loops back while repeats remain and no exit
// request waits, or leaves the queue with its continuation slots. is_forced ends the block at once
// (mode 2: what is left is not a whole frame).
void AdvanceBlock(Voice* p_voice, SourceState* p_source, u32 cb_bytes, u32 c_samples, bool is_forced) {
    SourceState& source = *p_source;
    if (source.num_samples == 0 && source.num_skip_samples == 0) {
        return;
    }
    ActiveWaveformQueue& queue = p_voice->active_queue;
    const u32 c_capacity = p_voice->queue.c_capacity;
    source.cb_decoded += cb_bytes;
    source.c_decoded_samples += c_samples;
    source.i_sample += c_samples;
    const u32 end_region = RegionEnd(source);
    u32 cb_done = 0;
    bool is_drained = false;
    // Every pass takes bytes from a slot it then pops, or returns.
    for (u32 c_passes = 0; c_passes <= c_capacity; ++c_passes) {
        const WaveformBlock* p_slot = queue.c_queued > 0 ? &QueuedSlot(queue, c_capacity, 0) : nullptr;
        u32 cb_part = 0;
        bool is_region_step = !is_forced;
        if (p_slot) {
            cb_part = std::min(p_slot->cb_data - source.cb_consumed, cb_bytes - cb_done);
            source.cb_consumed += cb_part;
            if (!is_forced && source.cb_consumed >= p_slot->cb_data) {
                is_drained = true;
                is_region_step = queue.c_queued < 2 && !queue.is_terminated;
            }
        }
        if (is_region_step) {
            is_drained = true;
            if (end_region > source.i_sample) {
                // Streaming: the last queued slot of an open queue is read through, so the game may
                // reuse it before the block ends (spec 05 §7).
                if (p_slot && source.cb_consumed >= p_slot->cb_data && queue.c_queued == 1 && !queue.is_terminated &&
                    source.cb_notified < source.cb_consumed && source.cb_notified < p_slot->cb_data) {
                    QueueVoiceCallback(p_voice, CALLBACK_BLOCK_END, *p_slot, source.block_user_data,
                                       source.cb_notified, source.c_repeats);
                    source.cb_notified = p_slot->cb_data;
                }
                source.p_read += cb_part;
                cb_done += cb_part;
                if (cb_done >= cb_bytes || !p_slot) {
                    return;
                }
                continue;
            }
        }
        if (source.num_repeats == kInfinite || source.c_repeats < source.num_repeats) {
            ++source.c_repeats;
            if (queue.c_exit_requests <= source.c_exit_consumed) {
                if (p_slot && source.cb_notified < p_slot->cb_data) {
                    QueueVoiceCallback(p_voice, CALLBACK_LOOP, *p_slot, source.block_user_data, source.cb_notified,
                                       source.c_repeats);
                }
                // The published read position goes on past the block's end until the next slot or block
                // starts (spec 09 §8.4).
                source.i_sample = 0;
                source.cb_consumed = 0;
                source.cb_notified = 0;
                return;
            }
            ++source.c_exit_consumed;
        }
        if (is_drained && end_region > source.i_sample) {
            PopSlot(p_voice, source);
            source.c_repeats = 0;
            source.cb_consumed = 0;
            source.cb_notified = 0;
            if (queue.c_queued > 0) {
                source.p_read = SlotData(QueuedSlot(queue, c_capacity, 0));
            }
            cb_done += cb_part;
            if (cb_done >= cb_bytes) {
                return;
            }
            continue;
        }
        if (p_slot) {
            do {
                // NOTE: the firmware adds an adjustment here that rea did not name; the bytes of the slot
                // left unread are taken, so that decodedDataSize counts whole blocks as 0x21fe0 does for
                // the slots it drops.
                const u32 cb_left = QueuedSlot(queue, c_capacity, 0).cb_data - source.cb_consumed;
                PopSlot(p_voice, source);
                source.c_repeats = 0;
                source.cb_notified = 0;
                source.cb_decoded += cb_left;
                source.cb_consumed = 0;
            } while (queue.c_queued > 0 && QueuedSlot(queue, c_capacity, 0).is_continuation);
        }
        source.i_sample = 0;
        source.cb_consumed = 0;
        source.cb_notified = 0;
        source.num_samples = 0;
        source.num_skip_samples = 0;
        StartBlock(p_voice, p_source);
        return;
    }
}

// The bytes the oldest slot and the continuation slots after it hold for the block being played.
struct Spans {
    const u8* p_data[kMaxWaveformBlocks];
    u32 cb_data[kMaxWaveformBlocks];
    u32 c_spans;
    u64 cb_total;
    u32 c_region;
};

// 0x21890 (spec 09 §7): false when not even one frame of cb_stride bytes is there. A block whose
// slots end before its region does is ended (forced) when another block follows or the queue is
// terminated, and the rest of the grain is silent; with nothing queued after it the voice starves.
bool GatherSpans(Voice* p_voice, SourceState* p_source, u32 cb_stride, Spans* p_out) {
    SourceState& source = *p_source;
    const ActiveWaveformQueue& queue = p_voice->active_queue;
    const u32 c_capacity = p_voice->queue.c_capacity;
    p_out->c_spans = 0;
    p_out->cb_total = 0;
    bool is_other_block_next = false;
    const bool has_block = source.num_samples != 0 || source.num_skip_samples != 0;
    if (!source.is_end && (has_block || StartBlock(p_voice, p_source))) {
        const u32 end_region = RegionEnd(source);
        p_out->c_region = end_region == kInfinite ? kInfinite - source.i_sample : end_region - source.i_sample;
        for (u32 i_slot = 0; i_slot < queue.c_queued && i_slot < kMaxWaveformBlocks; ++i_slot) {
            const WaveformBlock& slot = QueuedSlot(queue, c_capacity, i_slot);
            if (i_slot > 0 && !slot.is_continuation) {
                is_other_block_next = true;
                break;
            }
            const u32 off = i_slot == 0 ? source.cb_consumed : 0;
            p_out->p_data[p_out->c_spans] = SlotData(slot) + off;
            p_out->cb_data[p_out->c_spans] = slot.cb_data - off;
            p_out->cb_total += slot.cb_data - off;
            ++p_out->c_spans;
        }
        if (cb_stride <= p_out->cb_total) {
            source.is_starved = false;
            return true;
        }
        if (is_other_block_next) {
            AdvanceBlock(p_voice, p_source, 0, 0, true);
        }
    }
    if (!is_other_block_next && queue.is_terminated) {
        AdvanceBlock(p_voice, p_source, 0, 0, true);
    }
    p_out->c_spans = 0;
    p_out->cb_total = 0;
    source.is_starved = !source.is_end;
    return false;
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

// A frame's samples may straddle two slots; they are gathered into p_scratch then (0x252f0).
const u8* SampleBytes(const Spans& spans, u64 off, u32 cb_samples, u8* p_scratch) {
    u32 i_span = 0;
    while (i_span < spans.c_spans && off >= spans.cb_data[i_span]) {
        off -= spans.cb_data[i_span];
        ++i_span;
    }
    if (off + cb_samples <= spans.cb_data[i_span]) {
        return spans.p_data[i_span] + off;
    }
    for (u32 i_byte = 0; i_byte < cb_samples && i_span < spans.c_spans; ++i_span, off = 0) {
        const u32 cb_part = u32(std::min<u64>(spans.cb_data[i_span] - off, cb_samples - i_byte));
        std::memcpy(p_scratch + i_byte, spans.p_data[i_span] + off, cb_part);
        i_byte += cb_part;
    }
    return p_scratch;
}

void DecodeFrames(const Spans& spans, const SourceFormat& format, const FrameLayout& layout, float* const* pp_in,
                  u32 i_out, u32 c_frames) {
    u8 scratch[kMaxSampleBytes];
    for (u32 i_frame = 0; i_frame < c_frames; ++i_frame) {
        const u64 off = u64(i_frame) * layout.cb_stride + layout.off_samples;
        const u8* p_samples = SampleBytes(spans, off, layout.cb_samples, scratch);
        for (u32 i_channel = 0; i_channel < format.c_channels; ++i_channel) {
            pp_in[i_channel][i_out + i_frame] = ToFloat(format.waveform_type, p_samples + i_channel * layout.cb_sample);
        }
    }
}

// 0x1e6a0's read loop: whole frames only, as many as the slots hold, the grain needs and the region
// has left; returns how many it read.
u32 Decode(Voice* p_voice, SourceState* p_source, const SourceFormat& format, float* const* pp_in, u32 i_out,
           u32 c_wanted) {
    const FrameLayout layout = LayoutOf(format);
    u32 c_done = 0;
    while (c_done < c_wanted) {
        Spans spans;
        if (!GatherSpans(p_voice, p_source, layout.cb_stride, &spans)) {
            break;
        }
        const u32 c_frames = u32(std::min<u64>({c_wanted - c_done, spans.cb_total / layout.cb_stride, spans.c_region}));
        if (c_frames == 0) {
            break;
        }
        DecodeFrames(spans, format, layout, pp_in, i_out + c_done, c_frames);
        AdvanceBlock(p_voice, p_source, c_frames * layout.cb_stride, c_frames, false);
        c_done += c_frames;
    }
    return c_done;
}

u32 DataLockBit(const Voice& voice) {
    return 1u << voice.data_lock_shift;
}

u32 DataReadingBit(const Voice& voice) {
    return 2u << voice.data_lock_shift;
}

bool BeginDataRead(Voice* p_voice) {
    std::atomic<u32>& word = *p_voice->p_data_lock;
    u32 value = word.load(std::memory_order_acquire);
    do {
        if (value & DataLockBit(*p_voice)) {
            return false;
        }
    } while (!word.compare_exchange_weak(value, value | DataReadingBit(*p_voice), std::memory_order_acq_rel));
    return true;
}

void EndDataRead(Voice* p_voice) {
    p_voice->p_data_lock->fetch_and(~DataReadingBit(*p_voice), std::memory_order_release);
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
    const u32 c_channels = p_voice->c_out_channels;
    const u32 c_samples = context.c_grain_samples;
    if (p_voice->are_params_changed) {
        const double step = double(params.pitch_ratio) * kUnitStep * format.sample_rate_hz / context.sample_rate_hz;
        source.step = step > 0.0 ? u32(std::min(step, double(kMaxStep))) : 0;
    }
    // The voice starts with four zero samples of history, so its first decoded sample comes out at
    // output sample 3 at pitch 1 (0x1e6a0, spec 09 §2).
    if (p_voice->is_first_grain) {
        source.fraction = 0;
        source.c_history = kHistoryTaps;
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            std::fill_n(source.history[i_channel], kHistoryTaps, 0.0f);
        }
    }
    if (p_voice->flags & STATE_PAUSED) {
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            std::memset(out.p_channel[i_channel], 0, c_samples * sizeof(float));
        }
        return p_voice->flags;
    }
    const u32 kept_flags = p_voice->flags & (STATE_IN_USE | STATE_PAUSED | STATE_STOPPED);
    // A locked voice fades its last history sample (the firmware's channel work +0x24) to 0 over the
    // grain without reading or moving on (spec 09 §6.2).
    if (!BeginDataRead(p_voice)) {
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            const float last = source.history[i_channel][kHistoryTaps - 1];
            const float step = last / float(c_samples);
            for (u32 i = 0; i < c_samples; ++i) {
                out.p_channel[i_channel][i] = last - float(i) * step;
            }
        }
        return kept_flags | STATE_PLAYING;
    }
    const u32 c_needed = ((source.step * c_samples + source.fraction) >> kFractionBits) + kHistoryTaps;
    float* pp_in[kMaxChannels];
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        pp_in[i_channel] = p_voice->p_rack->p_decode_scratch + std::size_t(i_channel) * p_voice->p_rack->c_decode_stride;
        std::copy_n(source.history[i_channel], source.c_history, pp_in[i_channel]);
    }
    u32 c_input = source.c_history;
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
    EndDataRead(p_voice);
    if (source.is_end && source.c_history <= kHistoryTaps) {
        return 0;
    }
    if (source.is_starved) {
        return kept_flags | STATE_EMPTY;
    }
    return kept_flags | STATE_PLAYING;
}

} // namespace

void ResetSource(Voice* p_voice) {
    p_voice->p_dsp->source = {};
}

// The render sets the reading bit only while it holds the voice lock, which the caller holds too, so
// the wait is the firmware's (0x87f0) kept for a render that would read without it.
void LockVoiceData(Voice* p_voice) {
    if (!p_voice->p_data_lock) {
        return;
    }
    std::atomic<u32>& word = *p_voice->p_data_lock;
    u32 value = word.load(std::memory_order_acquire);
    for (;;) {
        if (value & DataReadingBit(*p_voice)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            value = word.load(std::memory_order_acquire);
        } else if (word.compare_exchange_weak(value, value | DataLockBit(*p_voice), std::memory_order_acq_rel)) {
            return;
        }
    }
}

void UnlockVoiceData(Voice* p_voice) {
    if (p_voice->p_data_lock) {
        p_voice->p_data_lock->fetch_and(~DataLockBit(*p_voice), std::memory_order_release);
    }
}

// After a reset of the queue (a setup, 0x10000001 with bit 2) the source starts on its new first block.
void FlushWaveformQueue(Voice* p_voice) {
    WaveformQueue& queue = p_voice->queue;
    ActiveWaveformQueue& active = p_voice->active_queue;
    if (!queue.p_slots) {
        return;
    }
    std::copy_n(queue.p_slots, queue.c_capacity, active.p_slots);
    active.i_write = queue.i_write;
    active.c_queued = queue.c_queued;
    active.c_exit_requests = queue.c_exit_requests;
    active.is_terminated = queue.is_terminated;
    if (queue.was_reset) {
        queue.was_reset = false;
        SourceState& source = p_voice->p_dsp->source;
        source.num_samples = 0;
        source.num_skip_samples = 0;
        source.cb_consumed = 0;
        source.cb_notified = 0;
        source.i_sample = 0;
        source.c_repeats = 0;
        source.c_exit_consumed = 0;
        source.is_end = false;
    }
}

// The slots the render took this grain leave the game's count, unless the game reset the queue since
// the flush, which already emptied it.
void PostWaveformQueue(Voice* p_voice) {
    WaveformQueue& queue = p_voice->queue;
    ActiveWaveformQueue& active = p_voice->active_queue;
    if (!queue.was_reset) {
        queue.c_queued -= std::min(active.c_popped, queue.c_queued);
    }
    active.c_popped = 0;
}

void ProcessSamplerVoice(RenderContext& context, Voice* p_voice, u32* p_event_code) {
    const VoiceParams& params = *p_voice->p_active_params;
    const u32 c_channels = p_voice->c_out_channels;
    const u32 c_samples = context.c_grain_samples;
    const Planes planes = WorkPlanes(context, WORK_VOICE);
    VoiceDsp& dsp = *p_voice->p_dsp;
    p_voice->flags = ProcessSource(context, p_voice, planes);
    const u32 set_modules = params.set_modules;
    const u32 first_modules = p_voice->first_active_modules;
    RunFilters(*p_voice, 0, planes, c_channels, c_samples);
    dsp.envelope_height = 1.0f;
    if (set_modules & MODULE_ENVELOPE) {
        dsp.envelope_height = ProcessEnvelope(params.envelope, &dsp.envelope, planes, c_channels, c_samples,
                                              first_modules & MODULE_ENVELOPE, &p_voice->flags, p_event_code);
    }
    if (set_modules & MODULE_DISTORTION) {
        ProcessDistortion(params.distortion, planes, c_channels, c_samples);
    }
    if ((set_modules & MODULE_USER_FX) && params.user_fx.pfn_handler) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 sampler UserFx 0x10000008: the game's handler is not called yet (F6)");
    }
    RunFilters(*p_voice, 1, planes, c_channels, c_samples);
    if (!(set_modules & MODULE_METER)) {
        std::fill_n(dsp.peaks[0], kPeakValues, 0.0f);
    } else if (!(p_voice->flags & STATE_PAUSED)) {
        ProcessPeakMeter(params.meter_mode, &p_voice->meters[0], planes, c_channels, c_samples,
                         first_modules & MODULE_METER, dsp.peaks[0]);
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
