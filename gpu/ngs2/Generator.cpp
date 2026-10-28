// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler's waveform generators, types 0x80 to 0x87 (0x21210; spec 05 §5, 10 §4): the setup's
// sampleRate is the frequency in Hz and configData (already modulo 360) the starting phase in degrees.
// They fill the source's input like a decoder, every channel the same, and the resampler plays them at
// the voice's pitch as if they were waveforms at the system's rate. Their state starts at 0 with the
// voice's.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

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
// The firmware's float constants: 2 pi (0x40c90fdb) and 1/360 (0x3b360b61).
constexpr float kTwoPi = std::bit_cast<float>(0x40c90fdbu);
constexpr float kPerDegree = std::bit_cast<float>(0x3b360b61u);

float Shape(u32 type, float phase) {
    switch (type) {
    case GENERATOR_SINE:
        return std::sin(phase * kTwoPi);
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
// whose bits, from bit 0 then down from 31, say which periods step the register. A sample shows the
// register before its step, and a step comes every period + 1 samples.
float StepPeriodicNoise(u32 config_data, SourceState* p_source) {
    SourceState& source = *p_source;
    const u32 v = std::min(config_data, 63u);
    const u32 c_period = v < 60 ? 0x4000u >> (v >> 2) : 1;
    const u32 pattern = v < 60 ? ((0xffffeeaau >> ((v & 3) * 4)) & 0xff) * 0x01010101u : 0xffffffffu;
    const float sample = float(s32(source.noise)) * kTwoToMinus31;
    if (s32(source.i_noise_period) < 1) {
        if ((pattern >> source.i_noise_bit) & 1) {
            const u32 x = source.noise;
            source.noise = 2 * x + (((x >> 14) ^ (0x69u >> ((x >> 9) & 7))) & 1);
        }
        source.i_noise_bit = (source.i_noise_bit - 1) & 31;
        source.i_noise_period = c_period;
    } else {
        --source.i_noise_period;
    }
    return sample;
}

} // namespace

// NOTE: 0x80 writes nothing and lets the resampler read the rack's shared decode buffer as the previous
// voice left it (spec 10 §4), which depends on the order of the voices; it is silence here.
void Generate(const SourceFormat& format, SourceState* p_source, float* const* pp_in, u32 c_channels, u32 i_out,
              u32 c_samples, u32 rate_hz) {
    SourceState& source = *p_source;
    const u32 type = format.waveform_type;
    float* p_first = pp_in[0] + i_out;
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
        const float start_phase = float(format.config_data) * kPerDegree;
        const float increment = 1.0f / float(rate_hz) * float(format.sample_rate_hz);
        float phase = start_phase + source.phase;
        for (u32 i = 0; i < c_samples; ++i) {
            p_first[i] = Shape(type, phase);
            const float x = phase + increment;
            phase = x - float(s32(x));
            if (x < 0.0f) {
                phase += 1.0f;
            }
        }
        source.phase = phase - start_phase;
    }
    for (u32 i_channel = 1; i_channel < c_channels; ++i_channel) {
        std::memcpy(pp_in[i_channel] + i_out, p_first, c_samples * sizeof(float));
    }
}

} // namespace Libraries::Ngs2
