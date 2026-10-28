// SPDX-License-Identifier: GPL-2.0-or-later
// The native Ngs2's DSP modules (phase F3, kb #1095), written from firmware 11.00's module table at
// 0x50000 as rea read it (spec 04 §4, 06, 06-tablas, notes #151, #395 and #790): their parameters as
// the render holds them, their state per voice and their processing over a grain of planar float
// channels. Nothing here allocates: every array lives in the rack's buffer.
#pragma once

#include <cstddef>
#include <type_traits>

#include "core/libraries/ngs2/ngs2.h"

namespace Libraries::Ngs2 {

constexpr u32 kMaxChannels = 8;
constexpr u32 kMaxEnvelopePoints = 16;
constexpr u32 kMasteringMatrixTypes = 16;
constexpr u32 kMatrixCells = kMaxChannels * kMaxChannels;
constexpr u32 kLfeChannel = 3;
constexpr u32 kPeakValues = kMaxChannels + 1;

// The state flags a voice publishes (VoiceGetStateFlags, spec 03 §3, 05 §3.4).
enum VoiceStateFlag : u32 {
    STATE_IN_USE = 0x1,
    STATE_PLAYING = 0x2,
    STATE_PAUSED = 0x4,
    STATE_STOPPED = 0x8,
    STATE_ERROR = 0x10,
    STATE_EMPTY = 0x20,
};

// What a grain's events do to a voice's output before it reaches ports or buffers (spec 04 §2.1).
enum EventCode : u32 {
    EVENT_CODE_NONE = 0,
    EVENT_CODE_FADE_OUT_TAIL = 1,
    EVENT_CODE_FADE_IN_HEAD = 2,
    EVENT_CODE_SILENT = 3,
    EVENT_CODE_STOP = 4,
    EVENT_CODE_CUT = 5,
};

// One grain of up to eight channels, each plane c_samples long.
struct Planes {
    float* p_channel[kMaxChannels];
};

// The firmware's gain ramps (0x24820 writes, 0x24b10 accumulates; spec 06 §0): sample i of n gets
// gain_start + i * (gain_end - gain_start) / n, so the last one stops one step short of gain_end.
// i_first and c_total place a part of a ramp that a port delay splits over two blocks.
void RampWrite(float* p_out, const float* p_in, u32 c_samples, float gain_start, float gain_end);
void RampAdd(float* p_out, const float* p_in, u32 c_samples, float gain_start, float gain_end, u32 i_first,
             u32 c_total);

enum EnvelopePhase : u8 {
    ENVELOPE_OFF = 0,
    ENVELOPE_FORWARD = 1,
    ENVELOPE_RELEASE = 2,
};

struct EnvelopeParams {
    u32 c_forward;
    u32 c_release;
    OrbisNgs2EnvelopePoint points[kMaxEnvelopePoints];
};

struct EnvelopeState {
    EnvelopePhase phase;
    u32 i_point;
    u32 pos;
    float start;
    float current;
};

struct DistortionParams {
    u32 flags;
    float a;
    float b;
    float clip;
    float gate;
    float wet;
    float dry;
};

// Module 0x12: sub 0 sets all of it (the submixer's compressor, the mastering setup), sub 1 only
// enable and threshold (the mastering limiter 0x30000003, kb #395). Flag bits (spec 09 §3.2): 0 off,
// 1 RMS detector instead of peak, 2 detector on the submixer's second input, 3 LFE (channel 3) kept
// out of the channel link.
enum CompressorFlag : u32 {
    COMPRESSOR_DISABLED = 0x1,
    COMPRESSOR_RMS = 0x2,
    COMPRESSOR_SIDECHAIN = 0x4,
    COMPRESSOR_LINK_WITHOUT_LFE = 0x8,
};

struct CompressorParams {
    u32 flags;
    float threshold;
    float slope;
    float knee;
    float attack_s;
    float release_s;
    float level;
};

// peak_level and peak_gain are the loudest channel's detector level and gain at the end of the
// grain's last block of up to 32 frames.
struct CompressorState {
    float gain[kMaxChannels];
    float peak_level;
    float peak_gain;
};

// Module 0x13: f holds frequency, Q and linear gain (or the five direct coefficients of type 0x20),
// coefficients what the render uses, designed whenever the parameters change.
struct FilterParams {
    u32 location;
    u32 type;
    u32 bypass_mask;
    float f[5];
    float coefficients[5];
};

struct BiquadState {
    float x1;
    float x2;
    float y1;
    float y2;
};

struct LfeParams {
    u32 is_enabled;
    u32 cutoff_hz;
};

struct LfeState {
    BiquadState stages[2];
};

struct UserFxParams {
    OrbisNgs2UserFxProcessHandler pfn_handler;
    uintptr_t user_data[3];
};

// Module 0x18: a ring of c_blocks grains, each kPeakValues floats: one level per channel and the
// grain's sample count (the RMS mode divides by it).
struct PeakMeterState {
    float* p_ring;
    u32 c_blocks;
    u32 i_block;
};

void DesignFilter(FilterParams* p_filter, u32 rate_hz);
// Filters the channels when the filter sits at location; true when it did. is_first_grain is the
// filter's own first grain with a parameter (spec 09 §10), as for the other modules.
bool ProcessFilter(const FilterParams& filter, u32 location, BiquadState* p_states, const Planes& planes,
                   u32 c_channels, u32 c_samples, bool is_first_grain);
void ProcessLfe(const LfeParams& lfe, LfeState* p_state, float* p_lfe, u32 c_samples, u32 rate_hz);

// Returns the envelope height; clears *p_flags when the envelope ends the voice and *p_event_code
// when it takes over a STOP's fade (release).
float ProcessEnvelope(const EnvelopeParams& params, EnvelopeState* p_state, const Planes& planes, u32 c_channels,
                      u32 c_samples, bool is_first_grain, u32* p_flags, u32* p_event_code);
void ProcessDistortion(const DistortionParams& params, const Planes& planes, u32 c_channels, u32 c_samples);
// detector is what the levels are measured on: planes itself, or the submixer's second input.
void ProcessCompressor(const CompressorParams& params, CompressorState* p_state, const Planes& planes,
                       const Planes& detector, u32 c_channels, u32 c_samples, u32 rate_hz, bool is_first_grain);
// p_out_peak gets kPeakValues floats: the loudest channel, then each channel.
void ProcessPeakMeter(u32 mode, PeakMeterState* p_state, const Planes& planes, u32 c_channels, u32 c_samples,
                      bool is_first_grain, float* p_out_peak);
// out[d] = sum over s of the ramp from previous[s][d] to current[s][d] times in[s] (module 0x17);
// matrices are kMaxChannels x kMaxChannels by source row.
void ProcessChannelMatrix(const float* p_previous, const float* p_current, const Planes& in, u32 c_in,
                          const Planes& out, u32 c_out, u32 c_samples);

static_assert(std::is_trivially_copyable_v<EnvelopeParams> && sizeof(EnvelopeParams) == 0xc8);
static_assert(std::is_trivially_copyable_v<EnvelopeState> && sizeof(EnvelopeState) == 0x14);
static_assert(std::is_trivially_copyable_v<DistortionParams> && sizeof(DistortionParams) == 0x1c);
static_assert(std::is_trivially_copyable_v<CompressorParams> && sizeof(CompressorParams) == 0x1c);
static_assert(std::is_trivially_copyable_v<CompressorState> && sizeof(CompressorState) == 0x28);
static_assert(std::is_trivially_copyable_v<FilterParams> && sizeof(FilterParams) == 0x34);
static_assert(std::is_trivially_copyable_v<BiquadState> && sizeof(BiquadState) == 0x10);
static_assert(std::is_trivially_copyable_v<LfeState> && sizeof(LfeState) == 0x20);
static_assert(std::is_trivially_copyable_v<UserFxParams> && sizeof(UserFxParams) == 0x20);
static_assert(std::is_trivially_copyable_v<PeakMeterState> && sizeof(PeakMeterState) == 0x10);

} // namespace Libraries::Ngs2
