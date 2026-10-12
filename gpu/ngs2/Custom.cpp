#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static constexpr std::uint32_t USER_FX2_FLAG_FIRST_PROCESS = 1;
static constexpr std::uint32_t USER_FX2_FLAG_PARAM_CHANGED = 2;
static constexpr float MAX_GAIN_LEVEL = 32.0f;

// The built-in modules other than UserFx, filter and gain (envelope, compressor, reverb, peak meter and the
// rest) are not implemented: one passes its input through unchanged and ignores its parameters, so a
// rack that uses them still sounds, without that effect. In the rack it is a UserFx2 slot with no process handler.
static bool IsUserFx2(const Ngs2CustomRackModuleInfo& info) {
    return info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2;
}

static bool IsUserFx(const Ngs2CustomRackModuleInfo& info) {
    return info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX;
}

static bool IsBuiltIn(std::uint32_t moduleId) {
    return moduleId == SCE_NGS2_CUSTOM_MODULE_ID_FILTER || moduleId == SCE_NGS2_CUSTOM_MODULE_ID_GAIN;
}

static void NoteMissingModule(std::uint32_t module_id) {
    static std::atomic<std::uint32_t> told{0};
    const std::uint32_t bit = 1u << (module_id & 0x1f);
    if (!(told.fetch_or(bit) & bit)) {
        std::printf("Ngs2: custom module id %#x is not implemented; it passes audio through\n", module_id);
    }
}

void Ngs2CheckCustomRack(const Ngs2CustomRackOption& option) {
    if (option.num_buffers != 1) throw std::runtime_error("NGS2: custom racks with " + std::to_string(option.num_buffers) + " buffers are not implemented");
    if (option.num_modules > SCE_NGS2_CUSTOM_MAX_MODULES || option.rack_option.max_ports > SCE_NGS2_CUSTOM_MAX_PORTS) APS5_INVALID_ARG_EX;
    for (std::uint32_t m = 0; m < option.num_modules; m++) {
        const auto& info = option.module[m];
        if (info.source_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
        if (IsUserFx(info) || IsBuiltIn(info.module_id)) continue;
        if (!IsUserFx2(info)) {
            NoteMissingModule(info.module_id);
            continue;
        }
        if (info.option == nullptr || info.option->size != sizeof(Ngs2CustomUserFx2ModuleOption)) APS5_INVALID_ARG_EX;
        if (reinterpret_cast<const Ngs2CustomUserFx2ModuleOption*>(info.option)->process_handler == nullptr) APS5_INVALID_ARG_EX;
    }
    for (std::uint32_t p = 0; p < option.rack_option.max_ports; p++) {
        if (option.port[p].source_buffer_id != 0) APS5_INVALID_ARG_EX;
    }
}

static Ngs2UserFx2SetupContext UserFxContext(Ngs2Rack& rack, std::size_t module, std::uint32_t voiceIndex) {
    auto& fx = rack.voices[voiceIndex].userFx[module];
    return {rack.userFxCommon[module].data(), fx.param.data(), fx.work.data(), rack.userFx[module].user_data,
            static_cast<std::uint32_t>(rack.voices.size()), voiceIndex, {}};
}

void Ngs2SetupUserFx(Ngs2Rack& rack, const Ngs2CustomRackOption& option) {
    rack.moduleIds.resize(option.num_modules);
    for (std::uint32_t m = 0; m < option.num_modules; m++) rack.moduleIds[m] = option.module[m].module_id;
    rack.userFx.resize(option.num_modules);
    rack.userFxCommon.resize(option.num_modules);
    for (auto& voice : rack.voices) voice.userFx.resize(option.num_modules);
    for (std::uint32_t m = 0; m < option.num_modules; m++) {
        if (!IsUserFx2(option.module[m])) {
            rack.userFx[m] = {};
            if (!IsBuiltIn(option.module[m].module_id)) continue;
            for (auto& voice : rack.voices) {
                voice.userFx[m].levels.fill(1.0f);
                voice.userFx[m].flags = USER_FX2_FLAG_FIRST_PROCESS;
            }
            continue;
        }
        const auto& module = *reinterpret_cast<const Ngs2CustomUserFx2ModuleOption*>(option.module[m].option);
        rack.userFx[m] = module;
        rack.userFxCommon[m].resize(module.common_size);
        for (std::uint32_t i = 0; i < rack.voices.size(); i++) {
            auto& fx = rack.voices[i].userFx[m];
            fx.param.resize(module.param_size);
            fx.work.resize(module.work_size);
            fx.state.resize(option.module[m].state_size);
            fx.flags = USER_FX2_FLAG_FIRST_PROCESS;
            if (module.setup_handler == nullptr) continue;
            auto context = UserFxContext(rack, m, i);
            const int result = module.setup_handler(&context);
            if (result != SCE_NGS2_OK) throw std::runtime_error("NGS2: the UserFx2 setup handler failed with " + std::to_string(result));
        }
    }
}

void Ngs2CleanupUserFx(Ngs2Rack& rack) {
    for (std::uint32_t i = 0; i < rack.voices.size(); i++) {
        for (std::size_t m = 0; m < rack.userFx.size(); m++) {
            if (rack.userFx[m].cleanup_handler == nullptr) continue;
            auto context = UserFxContext(rack, m, i);
            const int result = rack.userFx[m].cleanup_handler(&context);
            if (result != SCE_NGS2_OK) throw std::runtime_error("NGS2: the UserFx2 cleanup handler failed with " + std::to_string(result));
        }
    }
}

static void SetupCustomSubmixer(Ngs2Voice& voice, const Ngs2CustomSubmixerVoiceSetupParam& setup) {
    if (setup.flags != 0) throw std::runtime_error("NGS2: custom submixer setup flags are not implemented");
    if (setup.num_input_channels != setup.num_output_channels) throw std::runtime_error("NGS2: custom submixer channel conversion is not implemented");
    if (setup.num_input_channels == 0 || setup.num_input_channels > voice.rack->maxChannels) APS5_INVALID_ARG_EX;
    voice.channels = setup.num_input_channels;
}

static void SetUserFxParam(Ngs2Voice& voice, std::uint32_t index, const Ngs2CustomVoiceUserFx2Param& param) {
    if (index >= voice.userFx.size()) APS5_INVALID_ARG_EX;
    const auto& module = voice.rack->userFx[index];
    if (module.control_handler != nullptr) throw std::runtime_error("NGS2: the UserFx2 control handler is not implemented");
    auto& fx = voice.userFx[index];
    if (param.data_size != fx.param.size() || (param.data == nullptr && param.data_size != 0)) APS5_INVALID_ARG_EX;
    if (param.data_size != 0) std::memcpy(fx.param.data(), param.data, param.data_size);
    fx.flags |= USER_FX2_FLAG_PARAM_CHANGED;
}

void Ngs2ApplyCustomParam(Ngs2Voice& voice, const Ngs2VoiceParamHeader& param) {
    if (voice.rack->rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) throw std::invalid_argument("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
    if (param.id == SCE_NGS2_CUSTOM_SUBMIXER_VOICE_PARAM_SETUP) {
        SetupCustomSubmixer(voice, ParamAs<Ngs2CustomSubmixerVoiceSetupParam>(param));
        return;
    }
    // Module parameters are 0x4000 << 16 | module id << 8 | module index.
    if ((param.id & ~0x1fu) == SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX && (param.id & 0x1f) < voice.userFx.size() &&
        voice.rack->moduleIds[param.id & 0x1f] == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
        const auto& fx = ParamAs<Ngs2SubmixerVoiceUserFxParam>(param);
        auto& module = voice.userFx[param.id & 0x1f];
        module.handler = fx.handler;
        module.userData = {fx.user_data0, fx.user_data1, fx.user_data2};
        module.flags = USER_FX2_FLAG_FIRST_PROCESS;
        return;
    }
    if ((param.id & ~0x1fu) == SCE_NGS2_CUSTOM_VOICE_PARAM_FILTER && (param.id & 0x1f) < voice.userFx.size() &&
        voice.rack->moduleIds[param.id & 0x1f] == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) {
        auto& fx = voice.userFx[param.id & 0x1f];
        fx.filterParam = ParamAs<Ngs2CustomFilterParam>(param);
        fx.flags |= USER_FX2_FLAG_PARAM_CHANGED;
        return;
    }
    if ((param.id & ~0x1fu) == SCE_NGS2_CUSTOM_VOICE_PARAM_GAIN && (param.id & 0x1f) < voice.userFx.size() &&
        voice.rack->moduleIds[param.id & 0x1f] == SCE_NGS2_CUSTOM_MODULE_ID_GAIN) {
        const auto& gain = ParamAs<Ngs2CustomGainParam>(param);
        auto& levels = voice.userFx[param.id & 0x1f].levels;
        for (std::uint32_t c = 0; c < levels.size(); c++) levels[c] = std::clamp(gain.levels[c], -MAX_GAIN_LEVEL, MAX_GAIN_LEVEL);
        return;
    }
    if ((param.id & 0xffff0000u) == 0x40000000u && (param.id & 0x1f) < voice.userFx.size() &&
        voice.rack->userFx[param.id & 0x1f].process_handler == nullptr) {
        return;
    }
    if ((param.id & ~0x1fu) == SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX2) {
        SetUserFxParam(voice, param.id & 0x1f, ParamAs<Ngs2CustomVoiceUserFx2Param>(param));
        return;
    }
    throw std::runtime_error("NGS2: voice param " + Ngs2Hex(param.id) + " is not implemented");
}

static void SetFilterCoefficients(Ngs2Filter& filter, const Ngs2CustomFilterParam& param, std::uint32_t sampleRate) {
    const auto type = param.type & 0xff;
    if (type == SCE_NGS2_FILTER_TYPE_COEFFICIENTS) {
        filter.b0 = param.values[0];
        filter.b1 = param.values[1];
        filter.b2 = param.values[2];
        filter.a1 = -param.values[3];
        filter.a2 = -param.values[4];
        return;
    }
    if (type < SCE_NGS2_FILTER_TYPE_LOW_PASS || type > SCE_NGS2_FILTER_TYPE_FIRST_ORDER_HIGH_PASS) {
        filter.b0 = 1.0;
        filter.b1 = filter.b2 = filter.a1 = filter.a2 = 0.0;
        return;
    }
    const float ratio = param.values[0] / static_cast<float>(sampleRate);
    const float omega = ratio < 0.001f ? 0.0062831859f : std::min(0.499f, ratio) * 6.2831855f;
    const float q = param.values[1] < 1e-4f ? 1e-4f : std::min(32.0f, param.values[1]);
    const float sine = std::sin(omega);
    const float cosine = std::cos(omega);
    const float alpha = sine / (q + q);
    const float amplitude = std::sqrt(std::max(param.values[2], 1e-4f));
    const float shelf = std::sqrt(amplitude) / q * sine;
    float b0, b1, b2, a0, a1, a2;
    switch (type) {
        case SCE_NGS2_FILTER_TYPE_LOW_PASS:
            b1 = 1.0f - cosine;
            b0 = b2 = b1 * 0.5f;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_HIGH_PASS:
            b1 = -(1.0f + cosine);
            b0 = b2 = (1.0f + cosine) * 0.5f;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_BAND_PASS:
            b0 = sine * 0.5f, b1 = 0.0f, b2 = -b0;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_BAND_PASS_PEAK:
            b0 = alpha, b1 = 0.0f, b2 = -alpha;
            a0 = 1.0f + alpha, a1 = -2.0f * cosine, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_NOTCH:
            b0 = 1.0f, b1 = -2.0f * cosine, b2 = 1.0f;
            a0 = 1.0f + alpha, a1 = b1, a2 = 1.0f - alpha;
            break;
        case SCE_NGS2_FILTER_TYPE_HIGH_SHELF:
            b0 = amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cosine + shelf);
            b1 = -2.0f * amplitude * ((amplitude - 1.0f) + (amplitude + 1.0f) * cosine);
            b2 = amplitude * ((amplitude + 1.0f) + (amplitude - 1.0f) * cosine - shelf);
            a0 = (amplitude + 1.0f) - (amplitude - 1.0f) * cosine + shelf;
            a1 = 2.0f * ((amplitude - 1.0f) - (amplitude + 1.0f) * cosine);
            a2 = (amplitude + 1.0f) - (amplitude - 1.0f) * cosine - shelf;
            break;
        case SCE_NGS2_FILTER_TYPE_LOW_SHELF:
            b0 = amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cosine + shelf);
            b1 = 2.0f * amplitude * ((amplitude - 1.0f) - (amplitude + 1.0f) * cosine);
            b2 = amplitude * ((amplitude + 1.0f) - (amplitude - 1.0f) * cosine - shelf);
            a0 = (amplitude + 1.0f) + (amplitude - 1.0f) * cosine + shelf;
            a1 = -2.0f * ((amplitude - 1.0f) + (amplitude + 1.0f) * cosine);
            a2 = (amplitude + 1.0f) + (amplitude - 1.0f) * cosine - shelf;
            break;
        case SCE_NGS2_FILTER_TYPE_PEAKING:
            b0 = 1.0f + alpha * amplitude, b1 = -2.0f * cosine, b2 = 1.0f - alpha * amplitude;
            a0 = 1.0f + alpha / amplitude, a1 = b1, a2 = 1.0f - alpha / amplitude;
            break;
        case SCE_NGS2_FILTER_TYPE_FIRST_ORDER_LOW_PASS:
            b0 = b1 = sine, b2 = 0.0f;
            a0 = sine + cosine + 1.0f, a1 = sine - cosine - 1.0f, a2 = 0.0f;
            break;
        default:
            b0 = 1.0f + cosine, b1 = -b0, b2 = 0.0f;
            a0 = sine + b0, a1 = sine - b0, a2 = 0.0f;
            break;
    }
    const float inverse = 1.0f / a0;
    filter.b0 = b0 * inverse;
    filter.b1 = b1 * inverse;
    filter.b2 = b2 * inverse;
    filter.a1 = a1 * inverse;
    filter.a2 = a2 * inverse;
}

static void ProcessFilter(Ngs2Voice& voice, Ngs2UserFx2& fx, std::uint32_t grain, std::uint32_t sampleRate) {
    auto& filter = fx.filter;
    filter.enabled = true;
    filter.bypassMask = fx.filterParam.channel_mask & 0xff;
    filter.history.resize(voice.channels);
    if ((fx.flags & USER_FX2_FLAG_FIRST_PROCESS) != 0) std::fill(filter.history.begin(), filter.history.end(), Ngs2FilterHistory{});
    if ((fx.flags & USER_FX2_FLAG_PARAM_CHANGED) != 0 || fx.filterRate != sampleRate) {
        SetFilterCoefficients(filter, fx.filterParam, sampleRate);
        fx.filterRate = sampleRate;
    }
    fx.flags = 0;
    if (!voice.hasSamples && !Ngs2FilterHasHistory(filter)) return;
    Ngs2ApplyFilter(voice, filter, grain);
    for (std::uint32_t c = 0; c < voice.channels; c++) {
        if ((filter.bypassMask >> c & 1) != 0) filter.history[c] = {};
    }
    voice.hasSamples = true;
}

static void ProcessGain(Ngs2UserFx2& fx, float* const* channels, std::uint32_t numChannels, std::uint32_t grain, bool hasSamples) {
    for (std::uint32_t c = 0; c < numChannels; c++) {
        const float from = (fx.flags & USER_FX2_FLAG_FIRST_PROCESS) != 0 ? fx.levels[c] : fx.appliedLevels[c];
        if (hasSamples) Ngs2Ramp(channels[c], from, fx.levels[c], grain, channels[c], false);
        fx.appliedLevels[c] = fx.levels[c];
    }
    fx.flags = 0;
}

void Ngs2ProcessUserFx(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t sampleRate) {
    if (voice.userFx.empty()) return;
    std::vector<float*> channels(voice.channels);
    for (std::uint32_t c = 0; c < voice.channels; c++) channels[c] = voice.samples.data() + static_cast<std::size_t>(c) * grain;
    for (std::size_t m = 0; m < voice.userFx.size(); m++) {
        auto& fx = voice.userFx[m];
        if (voice.rack->moduleIds[m] == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) {
            ProcessFilter(voice, fx, grain, sampleRate);
            continue;
        }
        if (voice.rack->moduleIds[m] == SCE_NGS2_CUSTOM_MODULE_ID_GAIN) {
            ProcessGain(fx, channels.data(), voice.channels, grain, voice.hasSamples);
            continue;
        }
        if (!voice.hasSamples) continue;
        if (fx.handler != nullptr) {
            Ngs2UserFxProcessContext context{channels.data(), fx.userData[0], fx.userData[1], fx.userData[2], fx.flags, voice.channels, grain, sampleRate};
            const int result = fx.handler(&context);
            if (result != SCE_NGS2_OK) throw std::runtime_error("NGS2: the UserFx process handler failed with " + std::to_string(result));
            fx.flags = 0;
            continue;
        }
        const auto& module = voice.rack->userFx[m];
        if (module.process_handler == nullptr) continue;
        Ngs2UserFx2ProcessContext context{channels.data(), voice.rack->userFxCommon[m].data(), fx.param.data(), fx.work.data(), fx.state.data(),
                                          module.user_data, fx.flags, voice.channels, voice.channels, grain, sampleRate, 0, {}};
        const int result = module.process_handler(&context);
        if (result != SCE_NGS2_OK) throw std::runtime_error("NGS2: the UserFx2 process handler failed with " + std::to_string(result));
        fx.flags = 0;
    }
}

void Ngs2ProcessLegacyUserFx(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t sampleRate) {
    if (voice.userFxHandler == nullptr) return;
    std::array<float*, NGS2_MAX_CHANNELS> channels{};
    for (std::uint32_t c = 0; c < voice.channels; ++c) channels[c] = voice.samples.data() + static_cast<std::size_t>(c) * grain;
    Ngs2UserFxProcessContext context{channels.data(), voice.userFxData[0], voice.userFxData[1], voice.userFxData[2],
                                    voice.userFxFlags, voice.channels, grain, sampleRate};
    const int result = voice.userFxHandler(&context);
    if (result != SCE_NGS2_OK) throw std::runtime_error("NGS2: the UserFx process handler failed with " + std::to_string(result));
    voice.userFxFlags = 0;
    voice.hasSamples = std::any_of(voice.samples.begin(), voice.samples.end(), [](float sample) { return sample != 0.0f; });
}
