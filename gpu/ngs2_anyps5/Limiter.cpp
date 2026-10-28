#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/libraries/kernel/process.h"
#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

// As firmware 11.00's mastering limiter process (libSceNgs2.sprx 0x18a10, kb #790): a level per
// block of up to 32 frames, a target gain from it, and a one-pole slide towards it per sample; no
// lookahead.
static constexpr std::uint32_t BLOCK_FRAMES = 32;
static constexpr std::uint32_t KNEE_SDK_VERSION = 0x1700000;
static constexpr float SNAP_DISTANCE = 0.01f;

static float SmoothingCoefficient(std::uint32_t sampleRate, float seconds) {
    const auto frames = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<float>(sampleRate) * seconds));
    return frames == 0 ? 0.0f : std::exp(-1.0f / static_cast<float>(frames));
}

static float BlockLevel(const float* samples, std::uint32_t frames, bool isRms) {
    float level = 0.0f;
    if (!isRms) {
        for (std::uint32_t i = 0; i < frames; i++) level = std::max(level, std::fabs(samples[i]));
        return level;
    }
    for (std::uint32_t i = 0; i < frames; i++) level += samples[i] * samples[i];
    return std::sqrt(level / static_cast<float>(frames));
}

static void LinkLevels(std::array<float, NGS2_MAX_CHANNELS>& levels, std::uint32_t channels, bool isLfeLinkedApart) {
    float level = 0.0f;
    for (std::uint32_t c = 0; c < channels; c++) {
        if (!isLfeLinkedApart || c != NGS2_LFE_CHANNEL) level = std::max(level, levels[c]);
    }
    for (std::uint32_t c = 0; c < channels; c++) {
        if (!isLfeLinkedApart || c != NGS2_LFE_CHANNEL) levels[c] = level;
    }
}

static float TargetGain(const Ngs2Limiter& limiter, float level, bool hasKnee) {
    const float threshold = limiter.threshold;
    if (hasKnee && limiter.knee > 0.0f) {
        const float kneeStart = threshold * (1.0f - 0.5f * limiter.knee);
        const float over = level - kneeStart;
        if (over > 0.0f && over < threshold - kneeStart) {
            return (kneeStart + over * (1.0f + over / (threshold - kneeStart) * 0.5f * (limiter.slope - 1.0f))) / level;
        }
    }
    return threshold < level ? (threshold + (level - threshold) * limiter.slope) / level : 1.0f;
}

static bool HasKnee() {
    static const bool hasKnee = [] {
        s32 version = 0;
        return Libraries::Kernel::sceKernelGetCompiledSdkVersion(&version) == 0 && static_cast<std::uint32_t>(version) >= KNEE_SDK_VERSION;
    }();
    return hasKnee;
}

void Ngs2ProcessLimiter(Ngs2Limiter& limiter, float* samples, std::uint32_t channels, std::uint32_t grain, std::uint32_t sampleRate) {
    if (!limiter.isActive) {
        limiter.gains.fill(1.0f);
        return;
    }
    const float attack = SmoothingCoefficient(sampleRate, limiter.attackSeconds);
    const float release = SmoothingCoefficient(sampleRate, limiter.releaseSeconds);
    const bool hasKnee = HasKnee();
    std::array<float, NGS2_MAX_CHANNELS> levels{};
    for (std::uint32_t start = 0; start < grain; start += BLOCK_FRAMES) {
        const auto frames = std::min(BLOCK_FRAMES, grain - start);
        for (std::uint32_t c = 0; c < channels; c++) levels[c] = BlockLevel(samples + static_cast<std::size_t>(c) * grain + start, frames, limiter.isRms);
        LinkLevels(levels, channels, limiter.isLfeLinkedApart);
        float peakLevel = 0.0f;
        float pressLevel = 0.0f;
        for (std::uint32_t c = 0; c < channels; c++) {
            const float target = TargetGain(limiter, levels[c], hasKnee);
            float gain = limiter.gains[c];
            const float smoothing = gain <= target ? release : attack;
            float* channel = samples + static_cast<std::size_t>(c) * grain + start;
            for (std::uint32_t i = 0; i < frames; i++) {
                gain = target + smoothing * (gain - target);
                channel[i] = limiter.outputGain * gain * channel[i];
            }
            // The slide stops short of the target once float steps no longer move it.
            if (std::fabs(target - gain) < SNAP_DISTANCE && target + smoothing * (gain - target) == gain) gain = target;
            limiter.gains[c] = gain;
            pressLevel = std::max(gain, pressLevel);
            peakLevel = std::max(levels[c], peakLevel);
        }
        limiter.peakLevel = peakLevel;
        limiter.pressLevel = pressLevel;
    }
}
