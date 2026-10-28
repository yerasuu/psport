// SPDX-License-Identifier: GPL-2.0-or-later
// The reverb module's processing (0x23110, spec 07 §3): the input mixed to mono, the reflection line's
// taps (0x285c0, configured by 0x282e0), sixteen damped combs in parallel on the late tap (0x27fc0,
// configured by 0x293a0 or 0x292d0), per output channel a selection of combs (0x296e0) through four
// all-passes in series (0x27860), the room's high-frequency filter (0x28800) and the dry/wet mix
// (0x28fb0). Only the full-rate network: the half-rate mode (decimation 0x297e0, interpolation 0x29990,
// comb row 2) is never switched on by anything rea read.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

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
// decayHFRatio above 1: a fixed damping and the lowpass added to the direct path (spec 07 §3.4).
constexpr float kBoostDamping = 0.9494f;

bool IsBitSet(u32 mask, u32 i_bit) {
    return (mask >> i_bit) & 1;
}

u32 ToLineSamples(float seconds, float rate_hz, u32 max_delay) {
    return std::min(u32(seconds * rate_hz + 0.5f), max_delay);
}

// 0x282e0: the early taps follow the pattern from the reflections' delay, compressed so that its last
// one stays before the late tap (pattern < 8) or within the line (pattern >= 8); tap 4 is the late one.
// The group gains (reflections and reverb, both times room) ride on the taps.
void ConfigureI3dl2Taps(const ReverbParams& params, u32 max_delay, float rate_hz, ReverbTapSet* p_taps) {
    const u32 pattern = params.reflection_pattern;
    const u32 i_pattern = pattern < kPatternGroups ? pattern : pattern - kPatternGroups;
    const float* p_ms = kPatternMs[i_pattern];
    const u32 first = ToLineSamples(params.reflections_delay_s, rate_hz, max_delay);
    const u32 late = ToLineSamples(params.reflections_delay_s + params.reverb_delay_s, rate_hz, max_delay);
    const u32 limit = pattern < kPatternGroups ? late : max_delay;
    // NOTE: the spec writes the test as t0 + P[3] > tL with P in ms and t0 in samples; psport compares
    // samples with samples.
    const float span = p_ms[3] * rate_hz / 1000.0f;
    const float scale = float(first) + span > float(limit) ? float(limit - first) / span : 1.0f;
    const float early_gain = params.reflections * params.room;
    p_taps->delays[0] = first;
    p_taps->gains[0] = kPatternGains[i_pattern][0] * early_gain;
    for (u32 i_tap = 1; i_tap < 4; ++i_tap) {
        const float delay = float(first) + scale * p_ms[i_tap] * rate_hz / 1000.0f;
        p_taps->delays[i_tap] = std::min(u32(delay + 0.5f), max_delay);
        const bool is_repeated = p_taps->delays[i_tap] == p_taps->delays[i_tap - 1];
        p_taps->gains[i_tap] = is_repeated ? 0.0f : kPatternGains[i_pattern][i_tap] * early_gain;
    }
    p_taps->delays[4] = late;
    p_taps->gains[4] = params.reverb * params.room;
    p_taps->late_mask = kLateTapMask;
}

// The explicit network's taps: delays align8((int)(ms * fs / 1000 + 0.5)), group gains 1.0. NOTE: which
// taps the firmware routes to the combs in this mode is not read yet; psport routes tap 4 as I3DL2 does.
void ConfigureExplicitTaps(const ReverbParams& params, u32 max_delay, float rate_hz, ReverbTapSet* p_taps) {
    for (u32 i_tap = 0; i_tap < kReverbTaps; ++i_tap) {
        const u32 delay = (u32(s32(params.tap_delays_ms[i_tap] * rate_hz * 0.001f + 0.5f)) + 7) & ~7u;
        p_taps->delays[i_tap] = std::min(delay, max_delay);
        p_taps->gains[i_tap] = IsBitSet(params.tap_mask, i_tap) ? params.tap_gains[i_tap] : 0.0f;
    }
    p_taps->late_mask = kLateTapMask;
}

// Lays out the combs from their 48 kHz slots and sets their gains (0x293a0 for I3DL2, 0x292d0 for the
// explicit network). Returns whether a line was longer than its slot (rates above 48 kHz).
bool ConfigureCombs(const ReverbParams& params, float rate_hz, ReverbState* p_state) {
    ReverbState& state = *p_state;
    const bool is_i3dl2 = params.mode == REVERB_I3DL2;
    const float ratio = params.decay_hf_ratio;
    const float divisor = 4.0f - 0.03f * params.density;
    bool is_clamped = false;
    u32 off_comb = 0;
    state.comb_mask = 0;
    for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
        const bool is_used = is_i3dl2 || IsBitSet(params.comb_mask, i_comb);
        const float ms = is_i3dl2 ? kReverbCombLengthsMs[i_comb] : params.comb_delays_ms[i_comb];
        const u32 c_slot = is_used ? GetReverbSlotSamples(ms) : 0;
        state.comb_offsets[i_comb] = off_comb;
        state.comb_lengths[i_comb] = 0;
        if (c_slot == 0) {
            continue;
        }
        off_comb += c_slot;
        state.comb_mask |= 1u << i_comb;
        u32 length;
        if (is_i3dl2) {
            length = u32(rate_hz * ms * 0.001f / divisor + 0.5f);
            length += length & 1;
        } else {
            // NOTE: the rounding of an explicit comb's length is not read yet.
            length = u32(s32(rate_hz * ms * 0.001f + 0.5f));
        }
        is_clamped |= length > c_slot;
        length = std::clamp(length, 1u, c_slot);
        state.comb_lengths[i_comb] = length;
        if (!is_i3dl2) {
            // NOTE: the explicit network gives feedback and damping; the lowpass in the loop without a
            // direct path is psport's reading of how they combine.
            state.comb_gains[i_comb] = params.feedback;
            state.comb_damping[i_comb] = params.damping;
            state.comb_direct[i_comb] = 0.0f;
            state.comb_lowpass[i_comb] = 1.0f;
            continue;
        }
        const float gain = std::pow(10.0f, -3.0f * (float(length) / rate_hz) / params.decay_time_s);
        state.comb_gains[i_comb] = gain;
        if (ratio > 1.0f) {
            state.comb_damping[i_comb] = kBoostDamping;
            state.comb_direct[i_comb] = 1.0f;
            state.comb_lowpass[i_comb] = 1.0f - std::pow(gain, ratio - 1.0f);
        } else {
            const float base = ratio >= 0.8f ? ratio : (0.8f - ratio) * -0.857143f + 0.8f;
            const float k = base * base;
            state.comb_damping[i_comb] = std::min(0.99978f, 0.25f * std::log(gain) * (1.0f - 1.0f / k));
            state.comb_direct[i_comb] = 0.0f;
            state.comb_lowpass[i_comb] = 1.0f;
        }
    }
    // 0x27eb0. NOTE: when the firmware turns the prefilter on is not read; psport pairs it with the
    // lowpass loop of decayHFRatio <= 1, where it is the matching tone correction.
    state.has_prefilter = is_i3dl2 && ratio <= 1.0f;
    const float a = (1.0f - ratio) / (1.0f + ratio);
    state.prefilter_k0 = 1.0f + a / (1.0f - a);
    state.prefilter_k1 = -a / (1.0f - a);
    // The j-th comb a channel selects enters with gain 1 when j is odd, else 1 + (density - 100) / (10j + 40).
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        u32 j = 0;
        for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
            float gain = 0.0f;
            if (IsBitSet(kChannelCombMasks[i_channel], i_comb)) {
                gain = (j & 1) ? 1.0f : 1.0f + (params.density - 100.0f) / (10.0f * float(j) + 40.0f);
                ++j;
            }
            state.channel_comb_gains[i_channel][i_comb] = gain;
        }
    }
    return is_clamped;
}

bool ConfigureAllpasses(const ReverbParams& params, float rate_hz, ReverbState* p_state) {
    ReverbState& state = *p_state;
    const bool is_i3dl2 = params.mode == REVERB_I3DL2;
    state.allpass_gain = is_i3dl2 ? params.diffusion * 0.005f : params.allpass_gain;
    state.allpass_mask = 0;
    bool is_clamped = false;
    u32 off_allpass = 0;
    for (u32 i_allpass = 0; i_allpass < kReverbAllpasses; ++i_allpass) {
        const bool is_used = is_i3dl2 || IsBitSet(params.allpass_mask, i_allpass);
        const float ms = is_i3dl2 ? kReverbAllpassLengthsMs[i_allpass] : params.allpass_delays_ms[i_allpass];
        const u32 c_slot = is_used ? GetReverbSlotSamples(ms) : 0;
        state.allpass_offsets[i_allpass] = off_allpass;
        if (c_slot == 0) {
            continue;
        }
        off_allpass += c_slot;
        state.allpass_mask |= 1u << i_allpass;
        const s32 base = s32(rate_hz * ms * 0.001f + 0.5f);
        for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
            // NOTE: an explicit delay shorter than the channel's offset would go negative; psport keeps
            // one sample.
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
    ReverbTapSet taps{};
    if (params.mode == REVERB_I3DL2) {
        ConfigureI3dl2Taps(params, state.sizes.max_line_delay, fs, &taps);
    } else {
        ConfigureExplicitTaps(params, state.sizes.max_line_delay, fs, &taps);
    }
    // Changed taps fade from the old set to the new one over the grain.
    state.is_fading = was_configured && std::memcmp(&taps, &state.taps, sizeof(taps)) != 0;
    state.previous_taps = state.taps;
    state.taps = taps;
    u32 comb_layout[2][kReverbCombs];
    std::copy_n(state.comb_offsets, kReverbCombs, comb_layout[0]);
    std::copy_n(state.comb_lengths, kReverbCombs, comb_layout[1]);
    bool is_clamped = ConfigureCombs(params, fs, &state);
    // The firmware clears the combs when density changes (spec 07 §3.4); psport also does when their
    // layout moves, so that no index points past its line.
    if (!was_configured || params.density != state.configured_density ||
        !std::equal(state.comb_offsets, state.comb_offsets + kReverbCombs, comb_layout[0]) ||
        !std::equal(state.comb_lengths, state.comb_lengths + kReverbCombs, comb_layout[1])) {
        ClearCombs(&state);
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
                                     "sizes at 48 kHz (its half-rate mode is not read yet)");
    }
    state.configured_rate_hz = rate_hz;
    state.configured_density = params.density;
    state.is_configured = true;
}

// The mono input without the LFE (spec 07 §3.2).
float MixDown(const Planes& in, u32 c_in, u32 i) {
    switch (c_in) {
    case 8:
        return 0.267f * (in.p_channel[0][i] + in.p_channel[1][i] + in.p_channel[2][i] + in.p_channel[4][i] +
                         in.p_channel[5][i] + in.p_channel[6][i] + in.p_channel[7][i]);
    case 6:
        return 0.316f * (in.p_channel[0][i] + in.p_channel[1][i] + in.p_channel[2][i] + in.p_channel[4][i] +
                         in.p_channel[5][i]);
    case 2:
        return 0.707f * (in.p_channel[0][i] + in.p_channel[1][i]);
    default:
        return in.p_channel[0][i];
    }
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
            early = old_early + (early - old_early) * weight;
            late = old_late + (late - old_late) * weight;
        }
        state.i_line = state.i_line + 1 == c_ring ? 0 : state.i_line + 1;
        const float comb_in =
            state.has_prefilter ? late * state.prefilter_k0 + state.prefilter_x1 * state.prefilter_k1 : late;
        state.prefilter_x1 = late;
        for (u32 i_comb = 0; i_comb < kReverbCombs; ++i_comb) {
            if (!IsBitSet(state.comb_mask, i_comb)) {
                continue;
            }
            float* p_comb = state.p_combs + state.comb_offsets[i_comb];
            u32& i_comb_sample = state.comb_index[i_comb];
            const float delayed = p_comb[i_comb_sample];
            const float damping = state.comb_damping[i_comb];
            float& lowpass = state.comb_state[i_comb];
            lowpass = delayed * (1.0f - damping) + lowpass * damping;
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
            value += early;
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
