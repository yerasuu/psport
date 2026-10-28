// SPDX-License-Identifier: GPL-2.0-or-later
// The reverb module's processing (0x23110, spec 07 §3, spec 11): the input mixed to mono, the reflection
// line's taps (0x285c0, configured by 0x282e0 or 0x28440), sixteen damped combs in parallel on the late tap
// (0x27fc0, configured by 0x293a0 or 0x292d0), per output channel a selection of combs (0x296e0) through four
// all-passes in series (0x27860), the group gains, the room's high-frequency filter (0x28800) and the dry/wet
// mix (0x28fb0). Only the full-rate network: the half-rate mode (decimation 0x297e0, interpolation 0x29990,
// comb row 2) hangs on the instance's +0x10, which nothing in firmware 11.00 sets (spec 11 §7).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>

#include "Internal.h"
#include "Reverb.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kPatternGroups = 8;
// Reflection patterns (0x3a530, ms) and their gains (0x3a5b0), 06-tablas §E.
constexpr float kPatternMs[kPatternGroups][4] = {
    {0.0f, 11.0f, 23.0f, 35.0f},  {0.0f, 14.75f, 26.55f, 38.03f}, {0.0f, 6.0f, 17.0f, 29.0f},
    {0.0f, 8.0f, 20.0f, 53.0f},   {0.0f, 11.0f, 17.0f, 30.0f},    {0.0f, 16.0f, 30.0f, 55.0f},
    {0.0f, 20.0f, 29.0f, 40.0f},  {0.0f, 37.0f, 47.0f, 62.0f},
};
constexpr float kPatternGains[kPatternGroups][4] = {
    {0.7f, 0.4f, 0.134f, 0.1f}, {0.7f, 0.4f, 0.3f, 0.1f},  {0.7f, 0.45f, -0.6f, 0.2f}, {0.7f, 0.55f, -0.6f, 0.2f},
    {0.7f, -0.45f, 0.6f, 0.3f}, {0.7f, 0.7f, 0.7f, 0.4f},  {0.7f, 0.7f, -0.4f, 0.3f},  {0.7f, 0.7f, -0.4f, 0.4f},
};
// The combs each output channel sums (0x37c00) and its all-passes' length offsets in samples (0x37b80).
constexpr u32 kChannelCombMasks[kMaxChannels] = {0xaaaa, 0x5555, 0xcccc, 0x3333, 0x0ff0, 0x9ab2, 0x3c3c, 0x399c};
constexpr s32 kAllpassOffsets[kMaxChannels][kReverbAllpasses] = {
    {0, -32, -6, -11}, {-10, -5, 0, -34}, {-8, -3, -33, -5}, {-7, -9, -5, -8},
    {-33, -8, -8, 0},  {-11, -21, -10, -7}, {-9, -10, -27, -3}, {-6, -11, -9, -24},
};
// The tap that feeds the combs: tap 4, the one at the late delay.
constexpr u32 kLateTapMask = 0x10;
// 0x27c00's constants (spec 11 §4), as the firmware stores them: decayHFRatio above 1 takes a fixed damping
// and its complement; at most 1 a damping from the slope, bounded by the maximum.
constexpr float kBoostDamping = std::bit_cast<float>(0x3f730a81u);
constexpr float kBoostDampingComplement = std::bit_cast<float>(0x3d4f57f0u);
constexpr float kDampingSlope = std::bit_cast<float>(0xbf5b6db8u);
constexpr float kMaxDamping = std::bit_cast<float>(0x3f7ff194u);
// The mono mix's scales for 8, 6 and 2 input channels (0x37b48, 0x37b40, 0x37b44).
constexpr float kMix8 = std::bit_cast<float>(0x3e88b439u);
constexpr float kMix6 = std::bit_cast<float>(0x3ea1cac1u);
constexpr float kMix2 = std::bit_cast<float>(0x3f34fdf4u);

bool IsBitSet(u32 mask, u32 i_bit) {
    return (mask >> i_bit) & 1;
}

// The firmware's order of operations for a delay in seconds (0x282e0) and one in ms (0x28440, 0x292d0).
s32 SecondsToSamples(float seconds, float fs) {
    return s32(seconds * fs * 1000.0f * 0.001f + 0.5f);
}

s32 MsToSamples(float ms, float fs) {
    return s32(fs * ms * 0.001f + 0.5f);
}

// 0x282e0: the early taps follow the pattern from the reflections' delay, compressed so that its last
// one stays before the late tap (pattern < 8) or within the line (pattern >= 8); tap 4 is the late one.
// The group gains (reflections and reverb, both times room) are not on the taps: Configure keeps them for
// the output.
void ConfigureI3dl2Taps(const ReverbParams& params, u32 max_delay, float fs, ReverbTapSet* p_taps) {
    const u32 pattern = params.reflection_pattern;
    const u32 i_pattern = pattern < kPatternGroups ? pattern : pattern - kPatternGroups;
    const float* p_ms = kPatternMs[i_pattern];
    const s32 line = s32(max_delay);
    const s32 first = std::min(SecondsToSamples(params.reflections_delay_s, fs), line);
    const s32 late = std::min(SecondsToSamples(params.reflections_delay_s + params.reverb_delay_s, fs), line);
    const s32 limit = pattern < kPatternGroups ? late : line;
    const s32 span = MsToSamples(p_ms[3], fs);
    const float scale = u32(first + span) > u32(limit) ? float(limit - first) / float(span) : 1.0f;
    p_taps->delays[0] = u32(first);
    p_taps->gains[0] = kPatternGains[i_pattern][0];
    s32 previous = first;
    for (u32 i_tap = 1; i_tap < 4; ++i_tap) {
        const s32 delay = first + s32(scale * p_ms[i_tap] * fs * 0.001f + 0.5f);
        // The firmware leaves these unbounded; psport keeps them within the ring, so that a tap the
        // rounding takes one past the line does not read outside it.
        p_taps->delays[i_tap] = std::min(u32(delay), max_delay);
        p_taps->gains[i_tap] = delay == previous ? 0.0f : kPatternGains[i_pattern][i_tap];
        previous = delay;
    }
    p_taps->delays[4] = std::min(u32(first + SecondsToSamples(params.reverb_delay_s, fs)), max_delay);
    p_taps->gains[4] = 1.0f;
    p_taps->late_mask = kLateTapMask;
}

// 0x28440: the explicit network's taps, delays align8((int)(fs * ms * 0.001 + 0.5)), routed as I3DL2's. Only
// the masked taps change, and only they are read: the others keep their delays without a gain.
void ConfigureExplicitTaps(const ReverbParams& params, u32 max_delay, float fs, ReverbTapSet* p_taps) {
    for (u32 i_tap = 0; i_tap < kReverbTaps; ++i_tap) {
        if (!IsBitSet(params.tap_mask, i_tap)) {
            p_taps->gains[i_tap] = 0.0f;
            continue;
        }
        const u32 delay = (u32(MsToSamples(params.tap_delays_ms[i_tap], fs)) + 7) & ~7u;
        p_taps->delays[i_tap] = std::min(delay, max_delay);
        p_taps->gains[i_tap] = params.tap_gains[i_tap];
    }
    p_taps->late_mask = kLateTapMask;
}

// Lays comb i_comb's line of c_length floats at *p_off_comb, after the previous one. The firmware has no
// bound here; psport shortens a line that would run past the instance's memory (rates above 48 kHz) and
// says so. The index goes back to 0 once the line no longer holds it, which the firmware does only when
// it is past the new length: psport also does at the length itself, so that it never reads past the line.
bool PlaceComb(u32 i_comb, u32 c_length, u32* p_off_comb, ReverbState* p_state) {
    ReverbState& state = *p_state;
    const u32 c_placed = std::min(c_length, state.sizes.c_comb_floats - *p_off_comb);
    state.comb_offsets[i_comb] = *p_off_comb;
    state.comb_lengths[i_comb] = c_placed;
    if (state.comb_index[i_comb] >= c_placed) {
        state.comb_index[i_comb] = 0;
    }
    *p_off_comb += c_placed;
    return c_placed != c_length;
}

// 0x293a0 with 0x27c00 and 0x27eb0: every comb's length from density, rounded up to odd, the lines back to
// back; gain from decayTime, damping from decayHFRatio; the prefilter always on, from its square root.
// Returns whether a line was shortened.
bool ConfigureI3dl2Combs(const ReverbParams& params, float fs, ReverbState* p_state) {
    ReverbState& state = *p_state;
    const float ratio = params.decay_hf_ratio;
    const float divisor = params.density * -3.0f / 100.0f + 4.0f;
    bool is_clamped = false;
    u32 off_comb = 0;
    for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
        s32 length = s32(fs * kReverbCombLengthsMs[i_comb] / divisor * 0.001f + 0.5f);
        length += ~length & 1;
        is_clamped |= PlaceComb(i_comb, u32(length), &off_comb, &state);
        const float gain = std::pow(10.0f, float(length) / fs / params.decay_time_s * -3.0f);
        state.comb_gains[i_comb] = gain;
        if (ratio > 1.0f) {
            state.comb_damping[i_comb] = kBoostDamping;
            state.comb_damping_complement[i_comb] = kBoostDampingComplement;
            state.comb_direct[i_comb] = 1.0f;
            state.comb_lowpass[i_comb] = 1.0f - std::pow(10.0f, (ratio - 1.0f) * std::log10(gain));
        } else {
            float k = ratio * ratio;
            if (ratio < 0.8f) {
                const float base = (0.8f - ratio) * kDampingSlope + 0.8f;
                k = base * base;
            }
            const float damping = std::min(kMaxDamping, std::log10(gain) * 2.3025851f * 0.25f * (1.0f - 1.0f / k));
            state.comb_damping[i_comb] = damping;
            state.comb_damping_complement[i_comb] = 1.0f - damping;
            state.comb_direct[i_comb] = 0.0f;
            state.comb_lowpass[i_comb] = 1.0f;
        }
    }
    const float root = std::sqrt(ratio);
    const float a = (1.0f - root) / (root + 1.0f);
    state.has_prefilter = true;
    state.prefilter_k0 = a / (1.0f - a) + 1.0f;
    state.prefilter_k1 = -a / (1.0f - a);
    return is_clamped;
}

// 0x292d0: only the combs of the mask change, align4 of their delay, laid back to back from the start of
// the memory, with lp = buf (1 - damping) + lp damping and y = lp feedback. The others keep their lines and
// gains and still run, and nothing is cleared; the prefilter stays as the last I3DL2 grain left it.
bool ConfigureExplicitCombs(const ReverbParams& params, float fs, ReverbState* p_state) {
    ReverbState& state = *p_state;
    bool is_clamped = false;
    bool has_empty_comb = false;
    u32 off_comb = 0;
    for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
        if (IsBitSet(params.comb_mask, i_comb)) {
            const u32 length = (u32(MsToSamples(params.comb_delays_ms[i_comb], fs)) + 3) & ~3u;
            is_clamped |= PlaceComb(i_comb, length, &off_comb, &state);
            state.comb_gains[i_comb] = params.feedback;
            state.comb_damping[i_comb] = params.damping;
            state.comb_damping_complement[i_comb] = 1.0f - params.damping;
            state.comb_direct[i_comb] = 0.0f;
            state.comb_lowpass[i_comb] = 1.0f;
        }
        has_empty_comb |= state.comb_lengths[i_comb] == 0;
    }
    if (has_empty_comb) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 reverb explicit comb of length 0: firmware 11.00 hangs in 0x27fc0, "
                                     "psport keeps it silent");
    }
    return is_clamped;
}

// 0x296e0: the j-th comb a channel selects enters with gain 1 when j is odd, else 1 + (density - 100) /
// (10j + 40), computed as the firmware does. density is the block's +0x3c in both modes.
void ConfigureChannelCombGains(float density, ReverbState* p_state) {
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        u32 j = 0;
        for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
            float gain = 0.0f;
            if (IsBitSet(kChannelCombMasks[i_channel], i_comb)) {
                gain = (j & 1) ? 1.0f : (1.0f / (float(j) * 0.5f * 20.0f + 40.0f)) * (density + -100.0f) + 1.0f;
                ++j;
            }
            p_state->channel_comb_gains[i_channel][i_comb] = gain;
        }
    }
}

// I3DL2's all-passes (0x27770) sit in their 48 kHz slots; the explicit ones (0x27720) take align4 of their
// delay at the rate, back to back. Returns whether a line was shortened.
bool ConfigureAllpasses(const ReverbParams& params, float fs, ReverbState* p_state) {
    ReverbState& state = *p_state;
    const bool is_i3dl2 = params.mode == REVERB_I3DL2;
    state.allpass_gain = is_i3dl2 ? params.diffusion * 0.005f : params.allpass_gain;
    state.allpass_mask = 0;
    bool is_clamped = false;
    u32 off_allpass = 0;
    for (u32 i_allpass = 0; i_allpass < kReverbAllpasses; ++i_allpass) {
        state.allpass_offsets[i_allpass] = off_allpass;
        if (!is_i3dl2 && !IsBitSet(params.allpass_mask, i_allpass)) {
            continue;
        }
        const float ms = is_i3dl2 ? kReverbAllpassLengthsMs[i_allpass] : params.allpass_delays_ms[i_allpass];
        const s32 delay = MsToSamples(ms, fs);
        u32 c_slot = is_i3dl2 ? GetReverbSlotSamples(ms) : (u32(delay) + 3) & ~3u;
        const s32 base = is_i3dl2 ? delay : s32(c_slot);
        const u32 c_free = state.sizes.c_allpass_floats - off_allpass;
        if (c_slot > c_free) {
            is_clamped = true;
            c_slot = c_free;
        }
        if (c_slot == 0) {
            continue;
        }
        off_allpass += c_slot;
        state.allpass_mask |= 1u << i_allpass;
        for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
            // NOTE: an explicit delay shorter than the channel's offset gives the firmware a length of 0 or
            // less; psport keeps one sample, or leaves out an all-pass whose slot is empty.
            const s32 length = base + kAllpassOffsets[i_channel][i_allpass];
            is_clamped |= length > s32(c_slot);
            state.allpass_lengths[i_channel][i_allpass] = u32(std::clamp(length, 1, s32(c_slot)));
        }
    }
    return is_clamped;
}

// 0x28800 in I3DL2 mode: two equal poles a whose gain at HFReference is roomHF; none from roomHF 1 on.
// The firmware's form y = (1 - a)^2 x + 2a y1 - a^2 y2 is run as two one-pole sections, the same response,
// because rounding a^2 to float puts a pole on or past the unit circle once a nears 1 (roomHF near
// -100 dB at a low HFReference), and the output then grows without bound.
void ConfigureOutputFilter(const ReverbParams& params, float rate_hz, ReverbState* p_state) {
    ReverbState& state = *p_state;
    const float level = params.room_hf;
    state.has_output_filter = params.mode == REVERB_I3DL2 && level < 1.0f;
    if (!state.has_output_filter) {
        return;
    }
    const float c = std::cos(2.0f * std::numbers::pi_v<float> * params.hf_reference_hz / rate_hz);
    const float q = 1.0f - level * c;
    // The radicand is level (1 - c)(2 - level (1 + c)) >= 0; rounding may take it just below.
    const float root = std::sqrt(std::max(0.0f, 2.0f * level * (1.0f - c) - level * level * (1.0f - c * c)));
    state.filter_pole = (q - root) / (1.0f - level);
}

void ClearCombs(ReverbState* p_state) {
    ReverbState& state = *p_state;
    std::memset(state.p_combs, 0, std::size_t(state.sizes.c_comb_floats) * sizeof(float));
    std::fill_n(state.comb_index, kReverbCombs, 0u);
    std::fill_n(state.comb_state, kReverbCombs, 0.0f);
    state.prefilter_x1 = 0.0f;
}

void ClearAllpasses(ReverbState* p_state) {
    ReverbState& state = *p_state;
    std::memset(state.p_allpasses, 0,
                std::size_t(state.c_allpass_channels) * state.sizes.c_allpass_floats * sizeof(float));
    for (auto& indexes : state.allpass_index) {
        std::fill_n(indexes, kReverbAllpasses, 0u);
    }
}

void Configure(const ReverbParams& params, ReverbState* p_state, u32 rate_hz) {
    ReverbState& state = *p_state;
    const float fs = float(rate_hz);
    const bool was_configured = state.is_configured;
    ReverbTapSet taps = state.taps;
    if (params.mode == REVERB_I3DL2) {
        ConfigureI3dl2Taps(params, state.sizes.max_line_delay, fs, &taps);
        state.early_gain = params.reflections * params.room;
        state.late_gain = params.reverb * params.room;
    } else {
        ConfigureExplicitTaps(params, state.sizes.max_line_delay, fs, &taps);
    }
    // Every reconfiguring grain fades from the old taps to the new ones, changed or not; the first after a
    // reset fades in from the silent set ResetReverb leaves.
    state.is_fading = true;
    state.previous_taps = state.taps;
    state.taps = taps;
    bool is_clamped;
    if (params.mode == REVERB_I3DL2) {
        is_clamped = ConfigureI3dl2Combs(params, fs, &state);
        if (!was_configured || params.density != state.configured_density) {
            ClearCombs(&state);
        }
        state.configured_density = params.density;
        ConfigureChannelCombGains(params.density, &state);
    } else {
        is_clamped = ConfigureExplicitCombs(params, fs, &state);
        ConfigureChannelCombGains(std::bit_cast<float>(params.tap_mask), &state);
    }
    u32 allpass_layout[kMaxChannels + 1][kReverbAllpasses];
    std::copy_n(state.allpass_offsets, kReverbAllpasses, allpass_layout[kMaxChannels]);
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        std::copy_n(state.allpass_lengths[i_channel], kReverbAllpasses, allpass_layout[i_channel]);
    }
    is_clamped |= ConfigureAllpasses(params, fs, &state);
    bool is_allpass_moved =
        !std::equal(state.allpass_offsets, state.allpass_offsets + kReverbAllpasses, allpass_layout[kMaxChannels]);
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        is_allpass_moved |= !std::equal(state.allpass_lengths[i_channel],
                                        state.allpass_lengths[i_channel] + kReverbAllpasses, allpass_layout[i_channel]);
    }
    if (!was_configured || is_allpass_moved) {
        ClearAllpasses(&state);
    }
    ConfigureOutputFilter(params, fs, &state);
    if (!was_configured) {
        std::copy_n(params.dry_levels, kMaxChannels, state.previous_dry);
        std::copy_n(params.wet_levels, kMaxChannels, state.previous_wet);
    }
    if (is_clamped) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 reverb above 48 kHz: delay lines clamped to the memory the firmware "
                                     "sizes at 48 kHz");
    }
    state.configured_rate_hz = rate_hz;
    state.is_configured = true;
}

// The mono input without the LFE (spec 07 §3.2), each term scaled before the sum as 0x23110 does.
float MixDown(const Planes& in, u32 c_in, u32 i) {
    static constexpr u32 kChannels8[] = {0, 1, 2, 4, 5, 6, 7};
    static constexpr u32 kChannels6[] = {0, 1, 2, 4, 5};
    static constexpr u32 kChannels2[] = {0, 1};
    std::span<const u32> channels;
    float k;
    switch (c_in) {
    case 8:
        channels = kChannels8, k = kMix8;
        break;
    case 6:
        channels = kChannels6, k = kMix6;
        break;
    case 2:
        channels = kChannels2, k = kMix2;
        break;
    default:
        return in.p_channel[0][i];
    }
    float mono = 0.0f;
    for (const u32 i_channel : channels) {
        mono += in.p_channel[i_channel][i] * k;
    }
    return mono;
}

// The early (not routed late) and late sums of a tap set at the line's write position.
void ReadTaps(const ReverbTapSet& taps, const float* p_line, u32 c_ring, u32 i_write, float* p_out_early,
              float* p_out_late) {
    float early = 0.0f;
    float late = 0.0f;
    for (u32 i_tap = 0; i_tap < kReverbTaps; ++i_tap) {
        const u32 delay = taps.delays[i_tap];
        const float value = taps.gains[i_tap] * p_line[i_write >= delay ? i_write - delay : i_write + c_ring - delay];
        (IsBitSet(taps.late_mask, i_tap) ? late : early) += value;
    }
    *p_out_early = early;
    *p_out_late = late;
}

float FlushDenormal(float value) {
    return std::fabs(value) < std::numeric_limits<float>::min() ? 0.0f : value;
}

} // namespace

void ResetReverb(ReverbState* p_state) {
    ReverbState& state = *p_state;
    std::memset(state.p_line, 0, (std::size_t(state.sizes.max_line_delay) + 1) * sizeof(float));
    ClearCombs(&state);
    ClearAllpasses(&state);
    state.i_line = 0;
    for (auto& stages : state.filter_stages) {
        std::fill_n(stages, 2, 0.0f);
    }
    state.taps = {};
    // 0x290c0 leaves every comb without a line until I3DL2 or the explicit network's mask gives it one, and
    // the prefilter off. NOTE: with it off the firmware's four-sample path still reads its zeroed table and
    // feeds the combs 0 (spec 11 §6.4); psport passes the late tap on, as the firmware's one-sample path
    // does, until tools/hwtest/ngs2_reverb says which one a console plays.
    std::fill_n(state.comb_lengths, kReverbCombs, 0u);
    state.has_prefilter = false;
    state.early_gain = 1.0f;
    state.late_gain = 1.0f;
    state.is_fading = false;
    state.is_configured = false;
}

void ProcessReverb(const ReverbParams& params, ReverbState* p_state, const Planes& in, u32 c_in, const Planes& out,
                   u32 c_out, u32 c_samples, u32 rate_hz, bool are_params_changed) {
    ReverbState& state = *p_state;
    if (!state.is_configured || are_params_changed || state.configured_rate_hz != rate_hz) {
        Configure(params, &state, rate_hz);
    }
    // A tap of delay d reads the input d samples back, 0 being the current one: the ring holds Lr + 1.
    const u32 c_ring = state.sizes.max_line_delay + 1;
    const u32 c_channels = std::min(c_out, state.c_allpass_channels);
    const float fade_step = 1.0f / float(c_samples);
    float comb_out[kReverbCombs] = {};
    for (u32 i = 0; i < c_samples; ++i) {
        state.p_line[state.i_line] = MixDown(in, c_in, i);
        float early;
        float late;
        ReadTaps(state.taps, state.p_line, c_ring, state.i_line, &early, &late);
        if (state.is_fading) {
            float old_early;
            float old_late;
            ReadTaps(state.previous_taps, state.p_line, c_ring, state.i_line, &old_early, &old_late);
            const float weight = float(i) * fade_step;
            early = early * weight + old_early * (1.0f - weight);
            late = late * weight + old_late * (1.0f - weight);
        }
        state.i_line = state.i_line + 1 == c_ring ? 0 : state.i_line + 1;
        const float comb_in =
            state.has_prefilter ? late * state.prefilter_k0 + state.prefilter_x1 * state.prefilter_k1 : late;
        state.prefilter_x1 = late;
        for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
            if (state.comb_lengths[i_comb] == 0) {
                comb_out[i_comb] = 0.0f;
                continue;
            }
            float* p_comb = state.p_combs + state.comb_offsets[i_comb];
            u32& i_comb_sample = state.comb_index[i_comb];
            const float delayed = p_comb[i_comb_sample];
            const float damping = state.comb_damping[i_comb];
            float& lowpass = state.comb_state[i_comb];
            lowpass = delayed * state.comb_damping_complement[i_comb] + lowpass * damping;
            const float y =
                (delayed * state.comb_direct[i_comb] + lowpass * state.comb_lowpass[i_comb]) * state.comb_gains[i_comb];
            p_comb[i_comb_sample] = comb_in + y;
            i_comb_sample = i_comb_sample + 1 == state.comb_lengths[i_comb] ? 0 : i_comb_sample + 1;
            comb_out[i_comb] = y;
        }
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            if (i_channel == kLfeChannel) {
                out.p_channel[i_channel][i] = i_channel < c_in ? in.p_channel[i_channel][i] : 0.0f;
                continue;
            }
            float value = 0.0f;
            for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
                value += state.channel_comb_gains[i_channel][i_comb] * comb_out[i_comb];
            }
            float* p_allpasses = state.p_allpasses + std::size_t(i_channel) * state.sizes.c_allpass_floats;
            for (u32 i_allpass = 0; i_allpass < kReverbAllpasses; ++i_allpass) {
                if (!IsBitSet(state.allpass_mask, i_allpass)) {
                    continue;
                }
                float* p_line = p_allpasses + state.allpass_offsets[i_allpass];
                u32& i_line = state.allpass_index[i_channel][i_allpass];
                const float delayed = p_line[i_line];
                const float w = value + state.allpass_gain * delayed;
                p_line[i_line] = w;
                value = delayed - state.allpass_gain * w;
                i_line = i_line + 1 == state.allpass_lengths[i_channel][i_allpass] ? 0 : i_line + 1;
            }
            value = early * state.early_gain + value * state.late_gain;
            if (state.has_output_filter) {
                for (float& stage : state.filter_stages[i_channel]) {
                    stage = (1.0f - state.filter_pole) * value + state.filter_pole * stage;
                    value = stage;
                }
            }
            out.p_channel[i_channel][i] = value;
        }
    }
    // The recursive states would otherwise sink into denormals where the render does not flush them.
    for (float& lowpass : state.comb_state) {
        lowpass = FlushDenormal(lowpass);
    }
    state.prefilter_x1 = FlushDenormal(state.prefilter_x1);
    for (auto& stages : state.filter_stages) {
        for (float& stage : stages) {
            stage = FlushDenormal(stage);
        }
    }
    state.is_fading = false;
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        float* p_out = out.p_channel[i_channel];
        RampWrite(p_out, p_out, c_samples, state.previous_wet[i_channel], params.wet_levels[i_channel]);
        if (i_channel < c_in) {
            RampAdd(p_out, in.p_channel[i_channel], c_samples, state.previous_dry[i_channel],
                    params.dry_levels[i_channel], 0, c_samples);
        }
    }
    std::copy_n(params.dry_levels, kMaxChannels, state.previous_dry);
    std::copy_n(params.wet_levels, kMaxChannels, state.previous_wet);
}

} // namespace Libraries::Ngs2
