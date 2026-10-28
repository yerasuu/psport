// SPDX-License-Identifier: GPL-2.0-or-later
// Gain ramps (0x24820, 0x24b10), distortion (module 0x11, 0x1acd0) and the channel matrix (module 0x17,
// 0x176d0), spec 06 §0, §2, §8.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "Dsp.h"

namespace Libraries::Ngs2 {

void RampWrite(float* p_out, const float* p_in, u32 c_samples, float gain_start, float gain_end) {
    if (gain_start == gain_end) {
        if (gain_start == 0.0f) {
            std::memset(p_out, 0, c_samples * sizeof(float));
        } else if (gain_start == 1.0f) {
            if (p_out != p_in) {
                std::memmove(p_out, p_in, c_samples * sizeof(float));
            }
        } else {
            for (u32 i = 0; i < c_samples; ++i) {
                p_out[i] = p_in[i] * gain_start;
            }
        }
        return;
    }
    const float step = (gain_end - gain_start) / float(c_samples);
    for (u32 i = 0; i < c_samples; ++i) {
        p_out[i] = (gain_start + float(i) * step) * p_in[i];
    }
}

void RampAdd(float* p_out, const float* p_in, u32 c_samples, float gain_start, float gain_end, u32 i_first,
             u32 c_total) {
    if (gain_start == gain_end) {
        if (gain_start == 0.0f) {
            return;
        }
        for (u32 i = 0; i < c_samples; ++i) {
            p_out[i] += p_in[i] * gain_start;
        }
        return;
    }
    const float step = (gain_end - gain_start) / float(c_total);
    for (u32 i = 0; i < c_samples; ++i) {
        p_out[i] += (gain_start + float(i_first + i) * step) * p_in[i];
    }
}

void ProcessDistortion(const DistortionParams& params, const Planes& planes, u32 c_channels, u32 c_samples) {
    if (!params.is_set || (params.flags & 1)) {
        return;
    }
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        float* p_samples = planes.p_channel[i_channel];
        for (u32 i = 0; i < c_samples; ++i) {
            const float x = p_samples[i];
            float y = std::clamp(x * (params.a + params.b * std::fabs(x)), -params.clip, params.clip);
            if (-params.gate < y && y < params.gate) {
                y = 0.0f;
            }
            p_samples[i] = params.dry * x + params.wet * y;
        }
    }
}

void ProcessChannelMatrix(const float* p_previous, const float* p_current, const Planes& in, u32 c_in,
                          const Planes& out, u32 c_out, u32 c_samples) {
    for (u32 i_out = 0; i_out < c_out; ++i_out) {
        float* p_out = out.p_channel[i_out];
        std::memset(p_out, 0, c_samples * sizeof(float));
        for (u32 i_in = 0; i_in < c_in; ++i_in) {
            const u32 i_cell = i_in * kMaxChannels + i_out;
            RampAdd(p_out, in.p_channel[i_in], c_samples, p_previous[i_cell], p_current[i_cell], 0, c_samples);
        }
    }
}

} // namespace Libraries::Ngs2
