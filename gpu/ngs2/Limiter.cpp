// SPDX-License-Identifier: GPL-2.0-or-later
// The compressor and limiter, module 0x12 (0x18a10; kb #790, spec 03 §6.3): a gain computed per block of
// up to 32 frames from the block's peak, approached with one-pole attack and release, without
// look-ahead. The submixer's 0x20000002 sets all of it, the mastering's 0x30000003 enable and threshold.
#include <algorithm>
#include <cmath>

#include "Dsp.h"
#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kBlockFrames = 32;
constexpr u32 kFlagDisabled = 0x1;
// The knee only counts for titles built with SDK 1.70 or later (kb #790).
constexpr u32 kKneeSdkVersion = 0x1700000;

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
    const s32 c_samples = s32(float(rate_hz) * time_s);
    return c_samples ? std::exp(-1.0f / float(c_samples)) : 0.0f;
}

} // namespace

void ProcessCompressor(const CompressorParams& params, CompressorState* p_state, const Planes& planes,
                       u32 c_channels, u32 c_samples, u32 rate_hz, bool is_first_grain) {
    CompressorState& state = *p_state;
    if (is_first_grain) {
        state = {1.0f, 0.0f, 1.0f};
    }
    if (!params.is_set || (params.flags & kFlagDisabled)) {
        state = {1.0f, 0.0f, 1.0f};
        return;
    }
    if (params.flags & ~kFlagDisabled) {
        // The detector (RMS), channel-link and bank-select bits are named by kb #790 but not mapped to
        // flag bits yet; the peak detector over all channels is what flags 0 selects.
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 compressor 0x12: flag bits 1 to 3 (detector, link; spec 03 §6.3)");
    }
    const bool has_knee = IsCompiledSdkAtLeast(kKneeSdkVersion);
    const float attack = Coefficient(rate_hz, params.attack_s);
    const float release = Coefficient(rate_hz, params.release_s);
    float peak_level = 0.0f;
    for (u32 i_first = 0; i_first < c_samples; i_first += kBlockFrames) {
        const u32 c_frames = std::min(kBlockFrames, c_samples - i_first);
        float level = 0.0f;
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            const float* p_samples = planes.p_channel[i_channel] + i_first;
            for (u32 i = 0; i < c_frames; ++i) {
                level = std::max(level, std::fabs(p_samples[i]));
            }
        }
        peak_level = std::max(peak_level, level);
        const float target = TargetGain(params, level, has_knee);
        const float k = state.gain <= target ? release : attack;
        float block_gain = 0.0f;
        for (u32 i = 0; i < c_frames; ++i) {
            state.gain = target + k * (state.gain - target);
            block_gain = std::max(block_gain, state.gain);
            const float gain = params.level * state.gain;
            for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
                planes.p_channel[i_channel][i_first + i] *= gain;
            }
        }
        if (std::fabs(target - state.gain) < 0.01f && target + k * (state.gain - target) == state.gain) {
            state.gain = target;
        }
        state.peak_gain = block_gain;
    }
    state.peak_level = peak_level;
}

} // namespace Libraries::Ngs2
