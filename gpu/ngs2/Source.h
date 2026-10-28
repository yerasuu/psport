// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler source's waveform queue (Sampler.cpp: 0x21890, 0x21a30, 0x21fe0; spec 05 §3, 09 §7) as
// its decoders use it (Sampler.cpp linear PCM, Vag.cpp, Atrac9Source.cpp; spec 05 §2, §4, §6), and the
// generators (Generator.cpp, §5). All of it runs in the render, under the system's process lock.
#pragma once

#include "Internal.h"

namespace Libraries::Ngs2 {

// The bytes the oldest slot and the continuation slots after it hold for the block being played.
struct Spans {
    const u8* p_data[kMaxWaveformBlocks];
    u32 cb_data[kMaxWaveformBlocks];
    u32 c_spans;
    u64 cb_total;
    u32 c_region;
};

// 0x21a30's modes: a step after decoding, the forced repeat of a VAG loop end (bit 0, spec 05 §4), and
// the end of a block whose data is not a whole frame (mode 2).
enum AdvanceMode : u32 {
    ADVANCE_STEP = 0,
    ADVANCE_REPEAT = 1,
    ADVANCE_END = 2,
};

u32 RegionEnd(const SourceState& source);
void AdvanceBlock(Voice* p_voice, SourceState* p_source, u32 cb_bytes, u32 c_samples, AdvanceMode mode);
// False when not even one frame of cb_stride bytes is there.
bool GatherSpans(Voice* p_voice, SourceState* p_source, u32 cb_stride, Spans* p_out);
// cb_samples bytes at off into the spans, gathered into p_scratch when they straddle two slots.
const u8* SampleBytes(const Spans& spans, u64 off, u32 cb_samples, u8* p_scratch);
// Moves the block being played to i_sample, cb_offset bytes into its oldest slot; false, leaving it
// where it was, when the slot is shorter.
bool SeekBlock(Voice* p_voice, SourceState* p_source, u32 i_sample, u64 cb_offset);

// The decoders fill pp_in[channel][i_out...] with up to c_wanted samples, whole frames, so they may
// write up to one frame more; they answer how many they wrote.
u32 DecodeVag(Voice* p_voice, SourceState* p_source, const SourceFormat& format, float* const* pp_in, u32 i_out,
              u32 c_wanted);
u32 DecodeAtrac9(Voice* p_voice, SourceState* p_source, const SourceFormat& format, float* const* pp_in, u32 i_out,
                 u32 c_wanted);
// Fills pp_in[channel][i_out...] with c_samples of the generator, rate_hz being the system's rate.
void Generate(const SourceFormat& format, SourceState* p_source, float* const* pp_in, u32 c_channels, u32 i_out,
              u32 c_samples, u32 rate_hz);

} // namespace Libraries::Ngs2
