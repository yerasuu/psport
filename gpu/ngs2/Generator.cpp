// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler's waveform generators, types 0x80 to 0x87 (0x21210; spec 05 §5): the setup's sampleRate
// is the frequency in Hz and configData (already modulo 360) the starting phase in degrees; every
// channel gets the same waveform, made at the system's rate.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "Source.h"

namespace Libraries::Ngs2 {
namespace {

enum Generator : u32 {
    GENERATOR_NONE = 0x80,
    GENERATOR_SINE = 0x81,
    GENERATOR_SAW_UP = 0x82,
    GENERATOR_SAW_DOWN = 0x83,
    GENERATOR_TRIANGLE = 0x84,
    GENERATOR_SQUARE = 0x85,
    GENERATOR_NOISE = 0x86,
    GENERATOR_PERIODIC_NOISE = 0x87,
};

constexpr float kTwoToMinus31 = 1.0f / 2147483648.0f;

float Shape(u32 type, float phase) {
    switch (type) {
    case GENERATOR_SINE:
        return std::sin(2.0f * std::numbers::pi_v<float> * phase);
    case GENERATOR_SAW_UP:
        return phase < 0.5f ? 2.0f * phase : 2.0f * phase - 2.0f;
    case GENERATOR_SAW_DOWN:
        return phase < 0.5f ? -2.0f * phase : 2.0f - 2.0f * phase;
    case GENERATOR_TRIANGLE:
        return phase < 0.25f ? 4.0f * phase : phase >= 0.75f ? 4.0f * phase - 4.0f : 2.0f - 4.0f * phase;
    default:
        return phase < 0.5f ? 1.0f : -1.0f;
    }
}

// 0x87's shift register: configData picks a period of 0x4000 >> (v >> 2) samples and a 32-bit pattern
// whose bits say which periods step the register, read from bit 0 down, modulo 32.
float StepPeriodicNoise(u32 config_data, SourceState* p_source) {
    SourceState& source = *p_source;
    const u32 v = std::min(config_data, 63u);
    const u32 c_period = v < 60 ? 0x4000u >> (v >> 2) : 1;
    const u32 pattern = v < 60 ? ((0xffffeeaau >> ((v & 3) * 4)) & 0xff) * 0x01010101u : 0xffffffffu;
    if (source.i_noise_period == 0) {
        if ((pattern >> source.i_noise_bit) & 1) {
            const u32 x = source.noise;
            source.noise = 2 * x + (((x >> 14) ^ (0x69u >> ((x >> 9) & 7))) & 1);
        }
        source.i_noise_bit = (source.i_noise_bit - 1) & 31;
        source.i_noise_period = c_period;
    }
    --source.i_noise_period;
    return float(s32(source.noise)) * kTwoToMinus31;
}

} // namespace

// NOTE: the noise registers start at 0 and the phase at configData / 360, the voice's fresh state; rea
// has not read where 0x21210 seeds them.
void Generate(const SourceFormat& format, SourceState* p_source, const Planes& out, u32 c_channels, u32 c_samples,
              u32 rate_hz, bool is_first_grain) {
    SourceState& source = *p_source;
    const u32 type = format.waveform_type;
    if (is_first_grain) {
        source.phase = float(format.config_data) / 360.0f;
    }
    float* p_first = out.p_channel[0];
    if (type == GENERATOR_NONE) {
        std::fill_n(p_first, c_samples, 0.0f);
    } else if (type == GENERATOR_NOISE) {
        for (u32 i = 0; i < c_samples; ++i) {
            source.noise = source.noise * 0x19660du + 1;
            p_first[i] = float(s32(source.noise)) * kTwoToMinus31;
        }
    } else if (type == GENERATOR_PERIODIC_NOISE) {
        for (u32 i = 0; i < c_samples; ++i) {
            p_first[i] = StepPeriodicNoise(format.config_data, &source);
        }
    } else {
        const float increment = float(format.sample_rate_hz) / float(rate_hz);
        for (u32 i = 0; i < c_samples; ++i) {
            p_first[i] = Shape(type, source.phase);
            source.phase += increment;
            source.phase -= std::floor(source.phase);
        }
    }
    for (u32 i_channel = 1; i_channel < c_channels; ++i_channel) {
        std::memcpy(out.p_channel[i_channel], p_first, c_samples * sizeof(float));
    }
}

} // namespace Libraries::Ngs2
