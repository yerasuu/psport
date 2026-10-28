// SPDX-License-Identifier: GPL-2.0-or-later
// The peak meter, module 0x18 (0x1d250; spec 06 §9, kb #151): per channel, the peak (mode 1) or RMS
// (mode 2) over a window of the rack's numPeakMeterBlocks grains.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "Dsp.h"

namespace Libraries::Ngs2 {
namespace {

enum PeakMeterMode : u32 {
    PEAK_METER_OFF = 0,
    PEAK_METER_PEAK = 1,
    PEAK_METER_RMS = 2,
};

constexpr u32 kSampleCountSlot = kMaxChannels;

} // namespace

void ProcessPeakMeter(u32 mode, PeakMeterState* p_state, const Planes& planes, u32 c_channels, u32 c_samples,
                      bool is_first_grain, float* p_out_peak) {
    std::fill_n(p_out_peak, kPeakValues, 0.0f);
    PeakMeterState& state = *p_state;
    if (mode == PEAK_METER_OFF || !state.p_ring || state.c_blocks == 0) {
        return;
    }
    if (is_first_grain) {
        std::memset(state.p_ring, 0, std::size_t(state.c_blocks) * kPeakValues * sizeof(float));
        state.i_block = 0;
    }
    float* p_slot = state.p_ring + std::size_t(state.i_block) * kPeakValues;
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        float value = 0.0f;
        if (i_channel < c_channels) {
            const float* p_samples = planes.p_channel[i_channel];
            for (u32 i = 0; i < c_samples; ++i) {
                value = mode == PEAK_METER_PEAK ? std::max(value, std::fabs(p_samples[i]))
                                                : value + p_samples[i] * p_samples[i];
            }
        }
        p_slot[i_channel] = value;
    }
    p_slot[kSampleCountSlot] = float(c_samples);
    state.i_block = (state.i_block + 1) % state.c_blocks;
    float c_window_samples = 0.0f;
    for (u32 i_block = 0; i_block < state.c_blocks; ++i_block) {
        c_window_samples += state.p_ring[std::size_t(i_block) * kPeakValues + kSampleCountSlot];
    }
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        float value = 0.0f;
        for (u32 i_block = 0; i_block < state.c_blocks; ++i_block) {
            const float slot_value = state.p_ring[std::size_t(i_block) * kPeakValues + i_channel];
            value = mode == PEAK_METER_PEAK ? std::max(value, slot_value) : value + slot_value;
        }
        if (mode == PEAK_METER_RMS) {
            value = c_window_samples > 0.0f ? std::sqrt(value / c_window_samples) : 0.0f;
        }
        p_out_peak[1 + i_channel] = value;
        p_out_peak[0] = std::max(p_out_peak[0], value);
    }
}

} // namespace Libraries::Ngs2
