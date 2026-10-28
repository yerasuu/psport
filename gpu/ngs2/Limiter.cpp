// SPDX-License-Identifier: GPL-2.0-or-later
// The compressor and limiter, module 0x12 (0x18a10; spec 09 §3, kb #790): a gain per channel computed
// per block of up to 32 frames from the block's peak or RMS level, approached with one-pole attack and
// release, without look-ahead. The submixer's 0x20000002 sets all of it, the mastering's 0x30000003
// enable and threshold.
#include <algorithm>
#include <cmath>

#include "Dsp.h"
#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kBlockFrames = 32;
// The knee only counts for titles built with SDK 1.70 or later (kb #790).
constexpr u32 kKneeSdkVersion = 0x1700000;
constexpr float kSettleDistance = 0.01f;

float TargetGain(const CompressorParams& params, float level, bool has_knee) {
    const float threshold = params.threshold;
    float target = level > threshold ? (threshold + (level - threshold) * params.slope) / level : 1.0f;
    if (has_knee && params.knee > 0.0f) {
        const float knee_start = threshold * (1.0f - 0.5f * params.knee);
        const float over = level - knee_start;
        const float width = threshold - knee_start;
        if (over > 0.0f && over < width) {
            target = (knee_start + over * (1.0f + (over / width) * 0.5f * (params.slope - 1.0f))) / level;
        }
    }
    return target;
}

float Coefficient(u32 rate_hz, float time_s) {
    const u32 c_samples = u32(float(rate_hz) * time_s);
    return c_samples ? std::exp(-1.0f / float(c_samples)) : 0.0f;
}

float Level(const float* p_samples, u32 c_frames, bool is_rms) {
    float level = 0.0f;
    if (is_rms) {
        for (u32 i = 0; i < c_frames; ++i) {
            level += p_samples[i] * p_samples[i];
        }
        return std::sqrt(level / float(c_frames));
    }
    for (u32 i = 0; i < c_frames; ++i) {
        level = std::max(level, std::fabs(p_samples[i]));
    }
    return level;
}

// Without bit 3 every channel follows the loudest; with it the LFE channel follows itself and the
// others the loudest of them.
void LinkLevels(float* p_levels, u32 c_channels, bool is_lfe_apart) {
    float loudest = 0.0f;
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        if (!is_lfe_apart || i_channel != kLfeChannel) {
            loudest = std::max(loudest, p_levels[i_channel]);
        }
    }
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        if (!is_lfe_apart || i_channel != kLfeChannel) {
            p_levels[i_channel] = loudest;
        }
    }
}

} // namespace

void ProcessCompressor(const CompressorParams& params, CompressorState* p_state, const Planes& planes,
                       const Planes& detector, u32 c_channels, u32 c_samples, u32 rate_hz, bool is_first_grain) {
    CompressorState& state = *p_state;
    if (is_first_grain) {
        std::fill_n(state.gain, kMaxChannels, 1.0f);
    }
    if (params.flags & COMPRESSOR_DISABLED) {
        std::fill_n(state.gain, kMaxChannels, 1.0f);
        // The firmware leaves its meter unwritten here and the voice reports whatever an earlier voice
        // left on its stack (spec 09 §3.4, not determinable); unit gain is what the output gets.
        state.peak_gain = 1.0f;
        return;
    }
    const bool has_knee = IsCompiledSdkAtLeast(kKneeSdkVersion);
    const bool is_rms = params.flags & COMPRESSOR_RMS;
    const float attack = Coefficient(rate_hz, params.attack_s);
    const float release = Coefficient(rate_hz, params.release_s);
    for (u32 i_first = 0; i_first < c_samples; i_first += kBlockFrames) {
        const u32 c_frames = std::min(kBlockFrames, c_samples - i_first);
        float levels[kMaxChannels];
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            levels[i_channel] = Level(detector.p_channel[i_channel] + i_first, c_frames, is_rms);
        }
        LinkLevels(levels, c_channels, params.flags & COMPRESSOR_LINK_WITHOUT_LFE);
        state.peak_level = 0.0f;
        state.peak_gain = 0.0f;
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            const float target = TargetGain(params, levels[i_channel], has_knee);
            float& gain = state.gain[i_channel];
            const float k = gain <= target ? release : attack;
            float* p_samples = planes.p_channel[i_channel] + i_first;
            for (u32 i = 0; i < c_frames; ++i) {
                gain = target + k * (gain - target);
                p_samples[i] *= params.level * gain;
            }
            if (std::fabs(target - gain) < kSettleDistance && target + k * (gain - target) == gain) {
                gain = target;
            }
            state.peak_level = std::max(state.peak_level, levels[i_channel]);
            state.peak_gain = std::max(state.peak_gain, gain);
        }
    }
}

} // namespace Libraries::Ngs2
