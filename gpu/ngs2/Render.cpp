#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static constexpr double PHASE_ONE = 4294967296.0;
static constexpr std::uint64_t PHASE_FRACTION = 0xffffffff;

static const std::uint8_t* FrameAt(const Ngs2Voice& voice, const Ngs2Block& block, std::uint32_t frame) {
    const std::size_t frameBytes = Ngs2FrameBytes(voice);
    if (block.streaming) {
        for (const auto& piece : block.pieces) {
            if (frame >= piece.firstFrame && frame - piece.firstFrame < piece.frames) return piece.data + (frame - piece.firstFrame) * frameBytes;
        }
        return nullptr;
    }
    return block.data + (static_cast<std::size_t>(block.info.num_skip_samples) + frame) * frameBytes;
}

static void ReadPcm(const Ngs2Voice& voice, const std::uint8_t* frame, float* out) {
    if (frame == nullptr) {
        for (std::uint32_t channel = 0; channel < voice.channels; channel++) out[channel] = 0.0f;
        return;
    }
    if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_PCM_F32L) {
        std::memcpy(out, frame, voice.channels * sizeof(float));
        return;
    }
    for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
        std::int16_t sample;
        std::memcpy(&sample, frame + channel * sizeof(sample), sizeof(sample));
        out[channel] = sample / 32768.0f;
    }
}

static void ReadFrame(Ngs2Voice& voice, const Ngs2Block& block, std::uint32_t frame, float* out) {
    if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_VAG) Ngs2ReadVag(voice, block, frame, out);
    else ReadPcm(voice, FrameAt(voice, block, frame), out);
}

static void ReadFrames(Ngs2Voice& voice, float* current, float* next) {
    auto& block = voice.blocks.front();
    ReadFrame(voice, block, block.cursor, current);
    if (block.streaming && block.cursor + 1 >= block.availableFrames) std::memcpy(next, current, voice.channels * sizeof(float));
    else if (block.cursor + 1 < block.info.num_samples) ReadFrame(voice, block, block.cursor + 1, next);
    else if (block.info.num_repeats != 0) ReadFrame(voice, block, 0, next);
    else if (voice.blocks.size() > 1) ReadFrame(voice, voice.blocks[1], 0, next);
    else std::memcpy(next, current, voice.channels * sizeof(float));
}

bool Ngs2FinishBlock(Ngs2Voice& voice) {
    auto& block = voice.blocks.front();
    const bool repeat = block.info.num_repeats != 0;
    if (repeat) {
        if (block.info.num_repeats != UINT32_MAX) block.info.num_repeats--;
        block.numRepeated++;
        block.cursor = 0;
        block.started = false;
        if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) {
            block.dataCursor = 0;
            Ngs2RestartAtrac9(voice);
        }
    }
    const Ngs2VoiceCallbackInfo info{
        voice.callbackData,
        reinterpret_cast<Ngs2Handle>(&voice),
        repeat ? SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_REPEAT : SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END,
        0,
        block.info.user_data,
        block.data,
        static_cast<std::uint32_t>(block.info.data_size),
        block.numRepeated,
        0,
        0,
    };
    if (!repeat) {
        voice.waveformEnd = voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9 ? block.data + block.dataCursor
                          : voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_VAG   ? block.data + Ngs2VagBlockBytes(voice, block.info)
                          : block.streaming                                    ? Ngs2StreamEnd(voice, block)
                                                                               : FrameAt(voice, block, block.info.num_samples);
        voice.blocks.pop_front();
    }
    const auto revision = voice.waveformRevision;
    if (voice.callback != nullptr && (voice.callbackFlags & info.flag) != 0) voice.callback(&info);
    return voice.waveformRevision == revision && voice.state == Ngs2PlayState::Playing;
}

static void Advance(Ngs2Voice& voice, std::uint64_t frames) {
    while (frames != 0 && voice.state == Ngs2PlayState::Playing && !voice.blocks.empty()) {
        auto& block = voice.blocks.front();
        const std::uint64_t limit = block.streaming ? std::min<std::uint64_t>(block.info.num_samples, block.availableFrames) : block.info.num_samples;
        if (block.cursor >= limit && block.cursor != block.info.num_samples && !(block.streaming && !voice.acceptsBlocks)) break;
        const auto step = std::min<std::uint64_t>(frames, limit - block.cursor);
        const auto* vagFrame = voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_VAG ? Ngs2VagFrameAt(voice, block, block.cursor) : nullptr;
        block.cursor += static_cast<std::uint32_t>(step);
        voice.decodedSamples += step;
        if (vagFrame != nullptr) voice.decodedBytes += Ngs2VagFrameAt(voice, block, block.cursor) - vagFrame;
        else if (voice.waveformType != SCE_NGS2_WAVEFORM_TYPE_ATRAC9) voice.decodedBytes += step * Ngs2FrameBytes(voice);
        frames -= step;
        if (block.streaming) {
            while (block.pieces.size() > 1 && block.cursor >= block.pieces.front().firstFrame + block.pieces.front().frames) block.pieces.erase(block.pieces.begin());
        }
        if (block.cursor == block.info.num_samples || (block.streaming && !voice.acceptsBlocks && block.cursor == block.availableFrames)) Ngs2FinishBlock(voice);
    }
}

static void ConsumeSamples(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t systemRate) {
    const auto step = static_cast<std::uint64_t>(std::llround(voice.sampleRate * static_cast<double>(voice.pitch) / systemRate * PHASE_ONE));
    std::uint32_t written = 0;
    float current[NGS2_MAX_CHANNELS];
    float next[NGS2_MAX_CHANNELS];
    for (; written < grain && voice.state == Ngs2PlayState::Playing && !voice.blocks.empty(); written++) {
        ReadFrames(voice, current, next);
        const auto fraction = static_cast<float>(static_cast<double>(voice.phase) / PHASE_ONE);
        for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
            voice.samples[channel * grain + written] = std::lerp(current[channel], next[channel], fraction);
        }
        voice.phase += step;
        Advance(voice, voice.phase >> 32);
        voice.phase &= PHASE_FRACTION;
    }
    voice.hasSamples = written != 0;
}

static bool Bypasses(const Ngs2Filter& filter, std::uint32_t channel) {
    return channel < 64 && (filter.bypassMask & (std::uint64_t{1} << channel)) != 0;
}

bool Ngs2FilterHasHistory(const Ngs2Filter& filter) {
    if (!filter.enabled) return false;
    for (std::uint32_t channel = 0; channel < filter.history.size(); channel++) {
        const auto& h = filter.history[channel];
        if (!Bypasses(filter, channel) && (h.x1 != 0.0 || h.x2 != 0.0 || h.y1 != 0.0 || h.y2 != 0.0)) return true;
    }
    return false;
}

void Ngs2ApplyFilter(Ngs2Voice& voice, Ngs2Filter& filter, std::uint32_t grain) {
    for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
        auto& h = filter.history[channel];
        if (Bypasses(filter, channel)) {
            h = {};
            continue;
        }
        for (std::uint32_t i = 0; i < grain; i++) {
            auto& sample = voice.samples[channel * grain + i];
            const double x = sample;
            double y = filter.b0 * x + filter.b1 * h.x1 + filter.b2 * h.x2 - filter.a1 * h.y1 - filter.a2 * h.y2;
            if (std::abs(y) < 1e-20) y = 0.0;
            h = {x, h.x1, y, h.y1};
            sample = static_cast<float>(y);
        }
    }
}

static void ApplyFilters(Ngs2Voice& voice, std::uint32_t location, std::uint32_t grain, std::uint32_t systemRate) {
    for (auto& filter : voice.filters) {
        if (!filter.enabled || filter.location != location || (!voice.hasSamples && !Ngs2FilterHasHistory(filter))) continue;
        Ngs2UpdateFilter(filter, systemRate);
        Ngs2ApplyFilter(voice, filter, grain);
        voice.hasSamples = true;
    }
}

static void RenderSampler(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t systemRate) {
    if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) Ngs2ConsumeAtrac9(voice, grain, systemRate);
    else ConsumeSamples(voice, grain, systemRate);
    ApplyFilters(voice, 0, grain, systemRate);
    if (!Ngs2ApplyEnvelope(voice, grain)) voice.state = Ngs2PlayState::Empty;
    const bool buffered = voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9 && voice.atrac9.windowCursor * voice.channels < voice.atrac9.window.size();
    if (voice.state == Ngs2PlayState::Playing && voice.blocks.empty() && !buffered && !voice.acceptsBlocks && std::none_of(voice.filters.begin(), voice.filters.end(), Ngs2FilterHasHistory)) {
        voice.state = Ngs2PlayState::Empty;
    }
}

static void RenderVoice(Ngs2Voice& voice, const std::vector<Ngs2Voice*>& voices, std::uint32_t grain, std::uint32_t systemRate);

struct Ngs2DefaultRow {
    float mono;
    float stereo[2];
    float surround51[6];
    float surround71[8];
};

static constexpr float HALF_POWER = std::numbers::sqrt2_v<float> / 2.0f;
static constexpr float QUARTER_POWER = std::numbers::sqrt2_v<float> / 4.0f;

static constexpr Ngs2DefaultRow MONO_CENTER{1.0f, {HALF_POWER, HALF_POWER}, {0, 0, 1, 0, 0, 0}, {0, 0, 1, 0, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow FRONT_LEFT{HALF_POWER, {1, 0}, {1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow FRONT_RIGHT{HALF_POWER, {0, 1}, {0, 1, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow LFE{0.0f, {0, 0}, {0, 0, 0, 1, 0, 0}, {0, 0, 0, 1, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow QUAD_LEFT{0.5f, {HALF_POWER, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0}};
static constexpr Ngs2DefaultRow QUAD_RIGHT{0.5f, {0, HALF_POWER}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 0, 0}};
static constexpr Ngs2DefaultRow FIVE_SURROUND_LEFT{0.5f, {0, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0}};
static constexpr Ngs2DefaultRow FIVE_SURROUND_RIGHT{0.5f, {0, 0}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 0, 0}};
static constexpr Ngs2DefaultRow SURROUND_LEFT{0.5f, {0.5f, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0}};
static constexpr Ngs2DefaultRow SURROUND_RIGHT{0.5f, {0, 0.5f}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 0, 0}};
static constexpr Ngs2DefaultRow BACK_CENTER{QUARTER_POWER, {QUARTER_POWER, QUARTER_POWER}, {0, 0, 0, 0, HALF_POWER, HALF_POWER}, {0, 0, 0, 0, 0, 0, HALF_POWER, HALF_POWER}};
static constexpr Ngs2DefaultRow BACK_LEFT{0.5f, {0.5f, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 0, 1, 0}};
static constexpr Ngs2DefaultRow BACK_RIGHT{0.5f, {0, 0.5f}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 0, 1}};

static constexpr Ngs2DefaultRow DEFAULT_MAP[NGS2_MAX_CHANNELS][NGS2_MAX_CHANNELS] = {
    {MONO_CENTER},
    {FRONT_LEFT, FRONT_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, LFE},
    {FRONT_LEFT, FRONT_RIGHT, QUAD_LEFT, QUAD_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, FIVE_SURROUND_LEFT, FIVE_SURROUND_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, LFE, SURROUND_LEFT, SURROUND_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, LFE, SURROUND_LEFT, SURROUND_RIGHT, BACK_CENTER},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, LFE, SURROUND_LEFT, SURROUND_RIGHT, BACK_LEFT, BACK_RIGHT},
};

float Ngs2DefaultLevel(std::uint32_t sourceChannels, std::uint32_t source, std::uint32_t destChannels, std::uint32_t dest) {
    const auto& row = DEFAULT_MAP[sourceChannels - 1][source];
    switch (destChannels) {
        case 1: return row.mono;
        case 2: return row.stereo[dest];
        case 6: return row.surround51[dest];
        case 8: return row.surround71[dest];
        default: throw std::runtime_error("NGS2: the default channel map into " + std::to_string(destChannels) + " channels is not implemented");
    }
}

static void MixPort(Ngs2Voice& voice, const Ngs2Voice& source, const Ngs2Port& port, std::uint32_t grain) {
    const auto* matrix = port.matrix < 0 || source.matrices[port.matrix].empty() ? nullptr : &source.matrices[port.matrix];
    const std::size_t outputs = matrix == nullptr ? voice.channels : std::min<std::size_t>(voice.channels, matrix->size() / source.channels);
    for (std::size_t dst = 0; dst < outputs; dst++) {
        for (std::uint32_t src = 0; src < source.channels; src++) {
            const float level = port.volume * (matrix == nullptr ? Ngs2DefaultLevel(source.channels, src, voice.channels, static_cast<std::uint32_t>(dst)) : (*matrix)[dst * source.channels + src]);
            if (level == 0.0f) continue;
            for (std::uint32_t i = 0; i < grain; i++) voice.samples[dst * grain + i] += source.samples[src * grain + i] * level;
        }
    }
}

static void MixInputs(Ngs2Voice& voice, const std::vector<Ngs2Voice*>& voices, std::uint32_t grain, std::uint32_t systemRate) {
    for (auto* source : voices) {
        for (const auto& port : source->ports) {
            if (port.dest != &voice || port.volume == 0.0f) continue;
            RenderVoice(*source, voices, grain, systemRate);
            if (!source->hasSamples) continue;
            MixPort(voice, *source, port, grain);
            voice.hasSamples = true;
        }
    }
}

static void FilterLfe(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t systemRate) {
    if (voice.lfeCutoff == 0 || voice.channels <= NGS2_LFE_CHANNEL) return;
    if (voice.lfeFilters[0].sampleRate != systemRate) Ngs2SetLfeLowPass(voice.lfeFilters, voice.lfeCutoff, voice.channels, systemRate);
    if (!voice.hasSamples && !Ngs2FilterHasHistory(voice.lfeFilters[0]) && !Ngs2FilterHasHistory(voice.lfeFilters[1])) return;
    for (auto& stage : voice.lfeFilters) Ngs2ApplyFilter(voice, stage, grain);
    voice.hasSamples = true;
}

static void RenderVoice(Ngs2Voice& voice, const std::vector<Ngs2Voice*>& voices, std::uint32_t grain, std::uint32_t systemRate) {
    if (voice.rendered) return;
    if (voice.rendering) throw std::invalid_argument("NGS2: the voice patches form a cycle");
    voice.rendering = true;
    struct RenderingScope {
        bool& active;
        ~RenderingScope() { active = false; }
    } renderingScope{voice.rendering};
    voice.samples.assign(static_cast<std::size_t>(grain) * voice.channels, 0.0f);
    voice.hasSamples = false;
    if (voice.state == Ngs2PlayState::Playing && voice.channels != 0) {
        if (voice.rack->rackId == SCE_NGS2_RACK_ID_SAMPLER) RenderSampler(voice, grain, systemRate);
        else MixInputs(voice, voices, grain, systemRate);
        if (voice.rack->rackId == SCE_NGS2_RACK_ID_MASTERING) {
            Ngs2MeasurePeaks(voice.peakMeter, voice.samples.data(), voice.channels, grain);
            FilterLfe(voice, grain, systemRate);
        }
        if (voice.rack->rackId == SCE_NGS2_RACK_ID_REVERB && voice.reverb) voice.hasSamples = Ngs2ProcessReverb(*voice.reverb, voice.samples.data(), voice.channels, grain, systemRate);
        Ngs2ProcessLegacyUserFx(voice, grain, systemRate);
        if (voice.rack->rackId == SCE_NGS2_RACK_ID_SAMPLER) ApplyFilters(voice, 1, grain, systemRate);
        Ngs2ProcessUserFx(voice, grain, systemRate);
        if (voice.rack->rackId == SCE_NGS2_RACK_ID_SUBMIXER) Ngs2MeasurePeaks(voice.peakMeter, voice.samples.data(), voice.channels, grain);
    }
    voice.rendered = true;
}

static std::size_t SampleBytes(std::uint32_t waveformType) {
    if (waveformType == SCE_NGS2_WAVEFORM_TYPE_PCM_I16L) return sizeof(std::int16_t);
    if (waveformType == SCE_NGS2_WAVEFORM_TYPE_PCM_F32L) return sizeof(float);
    throw std::runtime_error("NGS2: render waveform type " + Ngs2Hex(waveformType) + " is not implemented");
}

static bool HasDefaultMap(std::uint32_t channels) {
    return channels == 1 || channels == 2 || channels == 6 || channels == 8;
}

static std::vector<float>& OutputMix(std::vector<std::vector<float>>& mixes, const Ngs2RenderBufferInfo* bufferInfo, std::uint32_t numBufferInfo,
                                     const Ngs2Voice& voice, std::uint32_t grain) {
    if (voice.outputId >= numBufferInfo) throw std::invalid_argument("NGS2: mastering output " + std::to_string(voice.outputId) + " has no render buffer");
    const auto& output = bufferInfo[voice.outputId];
    const auto sampleBytes = SampleBytes(output.waveform_type);
    if (output.num_channels != voice.channels && !HasDefaultMap(output.num_channels)) throw std::runtime_error("NGS2: mixing " + std::to_string(voice.channels) + " mastering channels into " + std::to_string(output.num_channels) + " is not implemented");
    if (output.buffer == nullptr || output.buffer_size < static_cast<std::size_t>(grain) * output.num_channels * sampleBytes) {
        throw std::invalid_argument("NGS2: render buffer " + std::to_string(voice.outputId) + " is missing or too small");
    }
    auto& mix = mixes[voice.outputId];
    if (mix.empty()) mix.resize(static_cast<std::size_t>(grain) * output.num_channels);
    return mix;
}

static float OutputLevel(const Ngs2Voice& voice, std::uint32_t source, std::uint32_t destChannels, std::uint32_t dest) {
    const auto sourceType = std::find(NGS2_MATRIX_CHANNELS.begin(), NGS2_MATRIX_CHANNELS.end(), voice.channels) - NGS2_MATRIX_CHANNELS.begin();
    const auto destType = std::find(NGS2_MATRIX_CHANNELS.begin(), NGS2_MATRIX_CHANNELS.end(), destChannels) - NGS2_MATRIX_CHANNELS.begin();
    if (sourceType < static_cast<std::ptrdiff_t>(NGS2_MATRIX_CHANNELS.size()) && destType < static_cast<std::ptrdiff_t>(NGS2_MATRIX_CHANNELS.size())) {
        const auto& matrix = voice.outputMatrices[sourceType * NGS2_MATRIX_CHANNELS.size() + destType];
        if (!matrix.empty()) return matrix[source * destChannels + dest];
    }
    if (destChannels == voice.channels) return dest == source ? 1.0f : 0.0f;
    return Ngs2DefaultLevel(voice.channels, source, destChannels, dest);
}

// As firmware 11.00 does, the mastering matrix (Ryza masters 5.1 into the stereo buffer it sends to
// AudioOut) comes before the gain, so the LFE level applies to the destination's LFE channel.
static void MixMastering(Ngs2Voice& voice, std::vector<float>& mix, std::uint32_t outputChannels, std::uint32_t grain) {
    std::vector<float> output(static_cast<std::size_t>(grain) * outputChannels);
    for (std::uint32_t dst = 0; dst < outputChannels; dst++) {
        const bool lfe = dst == NGS2_LFE_CHANNEL && (outputChannels == 6 || outputChannels == 8);
        const float level = lfe ? voice.lfeLevel : voice.fbwLevel;
        float* out = output.data() + static_cast<std::size_t>(dst) * grain;
        for (std::uint32_t src = 0; src < voice.channels; src++) {
            const float gain = level * OutputLevel(voice, src, outputChannels, dst);
            if (gain == 0.0f) continue;
            for (std::uint32_t i = 0; i < grain; i++) out[i] += voice.samples[src * grain + i] * gain;
        }
        for (std::uint32_t i = 0; i < grain; i++) mix[i * outputChannels + dst] += out[i];
    }
    Ngs2MeasurePeaks(voice.outputPeakMeter, output.data(), outputChannels, grain);
}

static void WriteOutput(const Ngs2RenderBufferInfo& output, const std::vector<float>& mix) {
    if (output.waveform_type == SCE_NGS2_WAVEFORM_TYPE_PCM_F32L) {
        std::memcpy(output.buffer, mix.data(), mix.size() * sizeof(float));
        return;
    }
    auto* pcm = static_cast<std::int16_t*>(output.buffer);
    for (std::size_t i = 0; i < mix.size(); i++) {
        pcm[i] = static_cast<std::int16_t>(std::min(std::lrint(std::clamp(mix[i], -1.0f, 1.0f) * 32768.0f), 32767L));
    }
}

void Ngs2RenderSystem(Ngs2System& system, const Ngs2RenderBufferInfo* bufferInfo, std::uint32_t numBufferInfo) {
    const auto grain = system.option.num_grain_samples;
    std::vector<Ngs2Voice*> voices;
    for (auto* rack : system.racks) {
        for (auto& voice : rack->voices) {
            voice.rendered = false;
            voices.push_back(&voice);
        }
    }
    std::vector<std::vector<float>> mixes(numBufferInfo);
    for (auto* voice : voices) {
        RenderVoice(*voice, voices, grain, system.option.sample_rate);
        if (voice->rack->rackId != SCE_NGS2_RACK_ID_MASTERING || voice->state != Ngs2PlayState::Playing) continue;
        if (!voice->hasSamples) {
            Ngs2MeasurePeaks(voice->outputPeakMeter, voice->samples.data(), voice->channels, grain);
            continue;
        }
        MixMastering(*voice, OutputMix(mixes, bufferInfo, numBufferInfo, *voice, grain), bufferInfo[voice->outputId].num_channels, grain);
    }
    for (std::uint32_t i = 0; i < numBufferInfo; i++) {
        if (bufferInfo[i].buffer != nullptr) std::memset(bufferInfo[i].buffer, 0, bufferInfo[i].buffer_size);
        if (!mixes[i].empty()) WriteOutput(bufferInfo[i], mixes[i]);
    }
    for (auto* voice : voices) {
        if (voice->state == Ngs2PlayState::Stopped) voice->state = Ngs2PlayState::Empty;
        voice->stateFlags = static_cast<std::uint32_t>(voice->state);
        if (voice->state != Ngs2PlayState::Empty && voice->envelope.phase == Ngs2EnvelopePhase::Release) voice->stateFlags |= SCE_NGS2_VOICE_STATE_FLAG_STOPPED;
    }
    system.renderCount++;
}
