#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static constexpr std::size_t VAG_UNIT_BYTES = 16;
static constexpr std::uint32_t VAG_MAX_SHIFT = 12;
static constexpr std::int32_t VAG_COEFFICIENTS[][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

std::size_t Ngs2VagBlockBytes(const Ngs2Voice& voice, const Ngs2WaveformBlock& block) {
    const std::uint64_t samples = static_cast<std::uint64_t>(block.num_skip_samples) + block.num_samples;
    return static_cast<std::size_t>((samples + NGS2_VAG_FRAME_SAMPLES - 1) / NGS2_VAG_FRAME_SAMPLES * VAG_UNIT_BYTES * voice.channels);
}

const std::uint8_t* Ngs2VagFrameAt(const Ngs2Voice& voice, const Ngs2Block& block, std::uint64_t sample) {
    return block.data + (block.info.num_skip_samples + sample) / NGS2_VAG_FRAME_SAMPLES * VAG_UNIT_BYTES * voice.channels;
}

static void DecodeFrame(Ngs2Voice& voice, const std::uint8_t* data, float* out) {
    for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
        const auto* unit = data + channel * VAG_UNIT_BYTES;
        const std::uint32_t predictor = unit[0] >> 4;
        const std::uint32_t shift = std::min<std::uint32_t>(unit[0] & 0xf, VAG_MAX_SHIFT);
        const auto& coefficients = VAG_COEFFICIENTS[predictor < std::size(VAG_COEFFICIENTS) ? predictor : 0];
        auto& history = voice.vag.history[channel];
        for (std::uint32_t i = 0; i < NGS2_VAG_FRAME_SAMPLES; i++) {
            const std::int32_t nibble = static_cast<std::int16_t>(((unit[2 + i / 2] >> (i % 2 * 4)) & 0xf) << 12);
            const std::int32_t sample = std::clamp((nibble >> shift) + (history[0] * coefficients[0] + history[1] * coefficients[1]) / 64, -32768, 32767);
            history[1] = history[0];
            history[0] = sample;
            out[i * voice.channels + channel] = static_cast<float>(sample) / 32768.0f;
        }
    }
}

void Ngs2ReadVag(Ngs2Voice& voice, const Ngs2Block& block, std::uint64_t sample, float* out) {
    auto& vag = voice.vag;
    const auto* data = Ngs2VagFrameAt(voice, block, sample);
    auto frame = std::find_if(vag.frames.begin(), vag.frames.end(), [data](const Ngs2VagFrame& frame) { return frame.data == data; });
    if (frame == vag.frames.end()) {
        vag.latest ^= 1;
        frame = vag.frames.begin() + vag.latest;
        frame->data = data;
        DecodeFrame(voice, data, frame->samples.data());
    }
    const auto index = (block.info.num_skip_samples + sample) % NGS2_VAG_FRAME_SAMPLES;
    std::memcpy(out, frame->samples.data() + index * voice.channels, voice.channels * sizeof(float));
}
