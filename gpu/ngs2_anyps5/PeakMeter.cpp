// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

// As firmware 11.00's peak meter module: each grain is one block (its peak magnitude, or its sum of
// squares), and the height is the peak, or the RMS, over the last numPeakMeterBlocks blocks.
void Ngs2SetPeakMeter(Ngs2PeakMeter& meter, std::uint32_t mode, std::uint32_t blocks) {
    if (mode > SCE_NGS2_PEAK_METER_MODE_RMS) APS5_INVALID_ARG_EX;
    if (mode == meter.mode) return;
    meter = {};
    meter.mode = mode;
    if (mode == 0) return;
    meter.blockSamples.assign(blocks, 0);
    for (auto& channel : meter.blocks) channel.assign(blocks, 0.0f);
}

void Ngs2MeasurePeaks(Ngs2PeakMeter& meter, const float* samples, std::uint32_t channels, std::uint32_t grain) {
    if (meter.mode == 0 || meter.blockSamples.empty()) return;
    const auto blocks = static_cast<std::uint32_t>(meter.blockSamples.size());
    meter.blockSamples[meter.next] = grain;
    meter.filled = std::min(meter.filled + 1, blocks);
    std::uint64_t total = 0;
    for (std::uint32_t b = 0; b < meter.filled; b++) total += meter.blockSamples[b];
    for (std::uint32_t c = 0; c < NGS2_MAX_CHANNELS; c++) {
        auto& ring = meter.blocks[c];
        float block = 0.0f;
        if (c < channels) {
            const float* channel = samples + static_cast<std::size_t>(c) * grain;
            for (std::uint32_t i = 0; i < grain; i++) {
                block = meter.mode == SCE_NGS2_PEAK_METER_MODE_PEAK ? std::max(block, std::fabs(channel[i])) : block + channel[i] * channel[i];
            }
        }
        ring[meter.next] = block;
        if (meter.mode == SCE_NGS2_PEAK_METER_MODE_PEAK) {
            meter.heights[c] = *std::max_element(ring.begin(), ring.begin() + meter.filled);
            continue;
        }
        float sum = 0.0f;
        for (std::uint32_t b = 0; b < meter.filled; b++) sum += ring[b];
        meter.heights[c] = total == 0 ? 0.0f : std::sqrt(sum / static_cast<float>(total));
    }
    meter.next = (meter.next + 1) % blocks;
}

float Ngs2PeakHeight(const Ngs2PeakMeter& meter) {
    return *std::max_element(meter.heights.begin(), meter.heights.end());
}
