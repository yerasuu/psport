#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static constexpr std::uint32_t CURVE_LINEAR = 0;
static constexpr std::uint32_t CURVE_EXPONENTIAL = 1;
static constexpr std::uint32_t CURVE_LOGARITHMIC = 2;
static constexpr std::uint32_t CURVE_COSINE = 3;
static constexpr std::uint32_t CURVE_ARCSINE = 4;
static constexpr std::uint32_t CURVE_QUARTER_SINE = 5;

void Ngs2SetEnvelope(Ngs2Voice& voice, const Ngs2SamplerVoiceEnvelopeParam& param) {
    const std::uint32_t total = param.num_forward_points + param.num_release_points;
    if (total < param.num_forward_points || total > voice.rack->maxEnvelopePoints || (param.points == nullptr && total != 0)) APS5_INVALID_ARG_EX;
    if (std::any_of(param.points, param.points + total, [](const Ngs2EnvelopePoint& point) { return point.curve > CURVE_QUARTER_SINE; })) APS5_INVALID_ARG_EX;
    auto& envelope = voice.envelope;
    envelope.enabled = true;
    envelope.forwardPoints = param.num_forward_points;
    envelope.points.assign(param.points, param.points + total);
    for (auto& point : envelope.points) point.height = point.height < 0.0f ? 0.0f : std::min(1.0f, point.height);
}

void Ngs2StartEnvelope(Ngs2Envelope& envelope) {
    envelope.phase = Ngs2EnvelopePhase::Forward;
    envelope.index = 0;
    envelope.position = 0;
    envelope.level = envelope.start = envelope.forwardPoints != 0 ? 0.0f : 1.0f;
}

static float Curve(std::uint32_t curve, float x, bool rising) {
    switch (curve) {
        case CURVE_EXPONENTIAL: return (std::pow(10.0f, x) - 1.0f) / 9.0f;
        case CURVE_LOGARITHMIC: return std::log10(1.0f + 9.0f * x);
        case CURVE_COSINE: return (1.0f - std::cos(std::numbers::pi_v<float> * x)) * 0.5f;
        case CURVE_ARCSINE: return 1.0f - std::acos(2.0f * x - 1.0f) / std::numbers::pi_v<float>;
        default: return rising ? std::sin(std::numbers::pi_v<float> * 0.5f * x) : 1.0f - std::cos(std::numbers::pi_v<float> * 0.5f * x);
    }
}

static void Scale(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t offset, std::uint32_t count, float from, float to) {
    for (std::uint32_t c = 0; c < voice.channels; c++) {
        float* samples = voice.samples.data() + static_cast<std::size_t>(c) * grain + offset;
        Ngs2Ramp(samples, from, to, count, samples, false);
    }
}

// Runs the envelope over the grain, from its forward points after a play and its release points after
// a stop; false once the voice ends: its release is over, or its forward points end at silence.
bool Ngs2ApplyEnvelope(Ngs2Voice& voice, std::uint32_t grain) {
    auto& envelope = voice.envelope;
    if (!envelope.enabled || envelope.phase == Ngs2EnvelopePhase::Idle) return true;
    const auto end = envelope.phase == Ngs2EnvelopePhase::Forward ? envelope.forwardPoints : static_cast<std::uint32_t>(envelope.points.size());
    for (std::uint32_t done = 0; done < grain;) {
        if (envelope.index >= end) {
            const bool released = envelope.phase == Ngs2EnvelopePhase::Release;
            if (released) envelope.level = 0.0f;
            Scale(voice, grain, done, grain - done, envelope.level, envelope.level);
            return !released && envelope.level != 0.0f;
        }
        const auto& point = envelope.points[envelope.index];
        const std::uint32_t count = point.duration > envelope.position ? std::min(grain - done, point.duration - envelope.position) : 0;
        const float delta = point.height - envelope.start;
        if (count != 0 && point.curve == CURVE_LINEAR) {
            const float level = envelope.level + delta * static_cast<float>(count) / static_cast<float>(point.duration);
            Scale(voice, grain, done, count, envelope.level, level);
            envelope.level = level;
        } else if (count != 0 && delta == 0.0f) {
            Scale(voice, grain, done, count, envelope.level, envelope.level);
        } else if (count != 0) {
            const float step = 1.0f / static_cast<float>(point.duration);
            for (std::uint32_t i = 0; i < count; i++) {
                envelope.level = envelope.start + delta * Curve(point.curve, static_cast<float>(envelope.position + i) * step, delta > 0.0f);
                for (std::uint32_t c = 0; c < voice.channels; c++) voice.samples[static_cast<std::size_t>(c) * grain + done + i] *= envelope.level;
            }
        }
        envelope.position += count;
        done += count;
        if (envelope.position < point.duration) continue;
        if (point.duration == 0) envelope.level = point.height;
        envelope.position = 0;
        envelope.start = point.height;
        envelope.index++;
    }
    return true;
}
