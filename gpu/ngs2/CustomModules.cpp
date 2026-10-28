// SPDX-License-Identifier: GPL-2.0-or-later
// The modules a custom rack's list may hold (spec 03 §6, 04 §4, 06, 07 §6, 09 §3, §10, §12): their blocks in
// the rack's buffer, their parameters by selector (sub, with 0x8000 for a read) and their processing in the
// chain. Implemented: 0x12 compressor and limiter, 0x13 filter, 0x14 LFE filter, 0x15 gain, 0x18 peak meter,
// 0x19 UserFx, 0x1b reverb and 0x1f UserFx2; every other module passes its source buffer through, as one
// without a parameter does, and is counted.
#include <algorithm>
#include <cstring>
#include <new>

#include "Custom.h"
#include "core/libraries/ngs2/ngs2_custom.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kReadSelector = 0x8000;
constexpr u32 kMaxPeakMeterMode = 2;
constexpr u32 kMaxFilterLocation = 0x11;
constexpr u32 kLfeCutoffsHz[] = {80, 120, 240};
// What a read answers before the module's first parameter (spec 03 §6.8): the LFE filter off at 120 Hz, unit
// gains.
constexpr u32 kUnsetLfeCutoffHz = 120;
constexpr float kMaxGain = 32.0f;
// The reverb setter answers -1 to an I3DL2 read while it holds the explicit network (spec 07 §2.1).
constexpr s32 kReadOtherModeResult = -1;

struct CompressorLimiterParam {
    OrbisNgs2VoiceParamHeader header;
    u32 enable;
    float threshold;
};

// Sub 0 of module 0x13, a custom rack's (spec 06 §4): type and mask become bytes, f is the five values.
struct FilterBlockParam {
    OrbisNgs2VoiceParamHeader header;
    u32 type;
    u32 bypass_mask;
    float f[5];
    u32 reserved;
};

static_assert(sizeof(CompressorLimiterParam) == 0x10);
static_assert(sizeof(FilterBlockParam) == 0x28 && sizeof(OrbisNgs2CustomVoiceFilterParam) == 0x28);
static_assert(sizeof(OrbisNgs2CustomVoiceCompressorParam) == 0x28);
static_assert(sizeof(OrbisNgs2CustomVoiceLfeFilterParam) == 0x10);
static_assert(sizeof(OrbisNgs2CustomVoiceGainParam) == 0x28);
static_assert(sizeof(OrbisNgs2CustomVoicePeakMeterParam) == 0x10);
static_assert(sizeof(OrbisNgs2CustomVoiceUserFxParam) == 0x28);
static_assert(sizeof(OrbisNgs2CustomVoiceReverbParam) == 0x60);
static_assert(sizeof(OrbisNgs2CustomVoiceUserFx2Param) == 0x18);
static_assert(sizeof(OrbisNgs2SamplerVoiceFilterParam) == 0x30);
static_assert(sizeof(UserFxParams) == 0x20, "module 0x19's block is the game's 32 bytes as they come");

template <class Param>
Param* ParamOf(const OrbisNgs2VoiceParamHeader* p_header) {
    return reinterpret_cast<Param*>(const_cast<OrbisNgs2VoiceParamHeader*>(p_header));
}

template <class T>
T* BlockOf(u8* p_block) {
    return std::launder(reinterpret_cast<T*>(p_block));
}

// The parameter size each selector takes; 0 for a selector the module's setter refuses.
u32 SelectorSize(const CustomModule& module, u32 selector) {
    switch (module.module_id) {
    case CUSTOM_MODULE_COMPRESSOR:
        return (selector & ~kReadSelector) == 0   ? u32(sizeof(OrbisNgs2CustomVoiceCompressorParam))
               : (selector & ~kReadSelector) == 1 ? u32(sizeof(CompressorLimiterParam))
                                                  : 0;
    case CUSTOM_MODULE_FILTER:
        return (selector & ~kReadSelector) == 0   ? u32(sizeof(FilterBlockParam))
               : (selector & ~kReadSelector) == 1 ? u32(sizeof(OrbisNgs2SamplerVoiceFilterParam))
                                                  : 0;
    case CUSTOM_MODULE_LFE:
        return (selector & ~kReadSelector) == 0 ? u32(sizeof(OrbisNgs2CustomVoiceLfeFilterParam)) : 0;
    case CUSTOM_MODULE_GAIN:
        return (selector & ~kReadSelector) == 0 ? u32(sizeof(OrbisNgs2CustomVoiceGainParam)) : 0;
    case CUSTOM_MODULE_PEAK_METER:
        return (selector & ~kReadSelector) == 0 ? u32(sizeof(OrbisNgs2CustomVoicePeakMeterParam)) : 0;
    case CUSTOM_MODULE_USER_FX:
        return (selector & ~kReadSelector) == 0 ? u32(sizeof(OrbisNgs2CustomVoiceUserFxParam)) : 0;
    case CUSTOM_MODULE_REVERB:
        // Sub 0 is I3DL2, sub 1 the explicit network; only I3DL2 reads back (spec 07 §6).
        return selector == 0 || selector == kReadSelector ? u32(sizeof(OrbisNgs2CustomVoiceReverbParam))
               : selector == 1                            ? u32(sizeof(ReverbExplicitParam))
                                                          : 0;
    case CUSTOM_MODULE_USER_FX2:
        return selector == 0 ? u32(sizeof(OrbisNgs2CustomVoiceUserFx2Param)) : 0;
    default:
        return 0;
    }
}

s32 SetCompressorModule(const Voice& voice, u32 selector, const OrbisNgs2VoiceParamHeader* p_header,
                        CompressorParams* p_block, bool is_set) {
    switch (selector) {
    case 0:
        SetCompressor(*ParamOf<OrbisNgs2SubmixerVoiceCompressorParam>(p_header), voice.p_rack->limits.max_inputs,
                      p_block);
        return ORBIS_OK;
    case 1: {
        // The limiter's sub 1 writes into the block sub 0 fills (spec 09 §3.2).
        const auto* p_param = ParamOf<CompressorLimiterParam>(p_header);
        p_block->flags = p_param->enable ? p_block->flags & ~COMPRESSOR_DISABLED : p_block->flags | COMPRESSOR_DISABLED;
        p_block->threshold = std::clamp(p_param->threshold, 0.0f, 4.0f);
        return ORBIS_OK;
    }
    case kReadSelector:
        ReadCompressor(is_set ? p_block : nullptr, ParamOf<OrbisNgs2SubmixerVoiceCompressorParam>(p_header));
        return ORBIS_OK;
    default: {
        auto* p_param = ParamOf<CompressorLimiterParam>(p_header);
        p_param->enable = is_set && !(p_block->flags & COMPRESSOR_DISABLED);
        p_param->threshold = is_set ? p_block->threshold : 0.0f;
        return ORBIS_OK;
    }
    }
}

s32 SetFilterModule(OrbisNgs2Handle h_voice, u32 selector, const OrbisNgs2VoiceParamHeader* p_header,
                    FilterParams* p_block, bool is_set) {
    switch (selector) {
    case 0: {
        // The setter keeps type and mask as bytes and a custom rack's filter at location 0.
        const auto* p_param = ParamOf<FilterBlockParam>(p_header);
        *p_block = {0, u8(p_param->type), u8(p_param->bypass_mask), {}, {}};
        std::copy_n(p_param->f, 5, p_block->f);
        return ORBIS_OK;
    }
    case 1: {
        // NOTE: inferred: the index at 8 is the rack dispatcher's to check (0x804A0600 on the sampler); the
        // module's own setter is taken to check the location only.
        const auto* p_param = ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header);
        if (p_param->location > kMaxFilterLocation) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_FILTER_LOCATION, "Invalid filter location ({})",
                        p_param->location);
        }
        const auto& direct = p_param->param.direct;
        *p_block = {u8(p_param->location),
                    u8(p_param->type),
                    u8(p_param->channelMask),
                    {direct.i0, direct.i1, direct.i2, direct.o1, direct.o2},
                    {}};
        return ORBIS_OK;
    }
    case kReadSelector: {
        auto* p_param = ParamOf<FilterBlockParam>(p_header);
        const FilterParams filter = is_set ? *p_block : FilterParams{};
        p_param->type = filter.type;
        p_param->bypass_mask = filter.bypass_mask;
        std::copy_n(filter.f, 5, p_param->f);
        return ORBIS_OK;
    }
    default: {
        auto* p_param = ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header);
        const FilterParams filter = is_set ? *p_block : FilterParams{};
        if (!is_set) {
            p_param->reserved3 = 0;
        }
        p_param->location = filter.location;
        p_param->type = filter.type;
        p_param->channelMask = filter.bypass_mask;
        p_param->param.direct = {filter.f[0], filter.f[1], filter.f[2], filter.f[3], filter.f[4]};
        return ORBIS_OK;
    }
    }
}

s32 SetReverbModule(OrbisNgs2Handle h_voice, const CustomModule& module, u32 selector,
                    const OrbisNgs2VoiceParamHeader* p_header, ReverbParams* p_block, bool is_set) {
    switch (selector) {
    case 0:
        SetReverbI3dl2(ParamOf<OrbisNgs2CustomVoiceReverbParam>(p_header)->i3dl2, p_block);
        return ORBIS_OK;
    case 1: {
        if (const s32 result = SetReverbExplicit(h_voice, *ParamOf<ReverbExplicitParam>(p_header),
                                                 GetReverbSizes(module.option.value), p_block);
            result < 0) {
            return result;
        }
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 custom reverb explicit network (module 0x1b sub 1): tap routing, "
                                     "comb "
                                     "filter, per-channel levels and the fields at 0xa0-0xb8 not read yet");
        return ORBIS_OK;
    }
    default: {
        auto& i3dl2 = ParamOf<OrbisNgs2CustomVoiceReverbParam>(p_header)->i3dl2;
        if (!is_set) {
            i3dl2 = {};
            return ORBIS_OK;
        }
        return ReadReverbI3dl2(*p_block, &i3dl2) ? ORBIS_OK : kReadOtherModeResult;
    }
    }
}

// UserFx2's parameter (spec 03 §6.9): the game's control handler takes the data when it has one, otherwise
// the data must be exactly the block the option sized.
s32 SetUserFx2Module(OrbisNgs2Handle h_voice, Voice* p_voice, const CustomModule& module, u32 i_module,
                     const OrbisNgs2VoiceParamHeader* p_header, u8* p_block) {
    const auto* p_param = ParamOf<OrbisNgs2CustomVoiceUserFx2Param>(p_header);
    if (!p_param->data) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS, "Invalid user fx parameter address ({})",
                    p_param->data);
    }
    const CustomModuleOption& option = module.option;
    if (option.pfn_control) {
        // NOTE: inferred: what the handler answers is what VoiceControl answers.
        return CallUserFx2Control(p_voice, option, p_voice->p_rack->p_custom->p_commons[i_module], p_block,
                                  p_param->data, p_param->dataSize);
    }
    if (p_param->dataSize != option.cb_param) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid user fx parameter size ({}!={})",
                    p_param->dataSize, option.cb_param);
    }
    std::memcpy(p_block, p_param->data, p_param->dataSize);
    return ORBIS_OK;
}

void CopyPlanes(const Planes& out, const Planes& in, u32 c_channels, u32 c_samples) {
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        if (out.p_channel[i_channel] != in.p_channel[i_channel]) {
            std::memcpy(out.p_channel[i_channel], in.p_channel[i_channel], c_samples * sizeof(float));
        }
    }
}

// A module's part of the voice state takes what fits of what the module reports.
void WriteModuleState(const CustomModuleRun& run, const void* p_values, std::size_t cb_values) {
    if (run.p_state) {
        std::memcpy(run.p_state, p_values, std::min<std::size_t>(cb_values, run.cb_state));
    }
}

} // namespace

bool IsCustomModuleImplemented(u32 module_id) {
    switch (module_id) {
    case CUSTOM_MODULE_COMPRESSOR:
    case CUSTOM_MODULE_FILTER:
    case CUSTOM_MODULE_LFE:
    case CUSTOM_MODULE_GAIN:
    case CUSTOM_MODULE_PEAK_METER:
    case CUSTOM_MODULE_USER_FX:
    case CUSTOM_MODULE_REVERB:
    case CUSTOM_MODULE_USER_FX2:
        return true;
    default:
        return false;
    }
}

std::size_t GetCustomParamsSize(const CustomModule& module) {
    switch (module.module_id) {
    case CUSTOM_MODULE_COMPRESSOR:
        return sizeof(CompressorParams);
    case CUSTOM_MODULE_FILTER:
        return sizeof(FilterParams);
    case CUSTOM_MODULE_LFE:
        return sizeof(LfeParams);
    case CUSTOM_MODULE_GAIN:
        return kMaxChannels * sizeof(float);
    case CUSTOM_MODULE_PEAK_METER:
        return sizeof(u32);
    case CUSTOM_MODULE_USER_FX:
        return sizeof(UserFxParams);
    case CUSTOM_MODULE_REVERB:
        return sizeof(ReverbParams);
    case CUSTOM_MODULE_USER_FX2:
        return module.option.cb_param;
    default:
        return 0;
    }
}

std::size_t GetCustomDspSize(const CustomModule& module) {
    switch (module.module_id) {
    case CUSTOM_MODULE_COMPRESSOR:
        return sizeof(CompressorState);
    case CUSTOM_MODULE_FILTER:
        return sizeof(CustomFilterDsp);
    case CUSTOM_MODULE_LFE:
        return sizeof(LfeState);
    case CUSTOM_MODULE_GAIN:
        return sizeof(CustomGainDsp);
    case CUSTOM_MODULE_PEAK_METER:
        return sizeof(PeakMeterState);
    case CUSTOM_MODULE_REVERB:
        return sizeof(ReverbState);
    default:
        return 0;
    }
}

// The peak meter's ring of numBlocks grains, the reverb's lines sized as the firmware sizes them at 48 kHz
// for the option's word at 4 (spec 07 §1), UserFx2's work block.
std::size_t GetCustomExtraSize(const CustomModule& module, const RackLimits& limits) {
    switch (module.module_id) {
    case CUSTOM_MODULE_PEAK_METER:
        return std::size_t(module.option.value) * kPeakValues * sizeof(float);
    case CUSTOM_MODULE_REVERB: {
        const ReverbSizes sizes = GetReverbSizes(module.option.value);
        return (std::size_t(sizes.max_line_delay) + 1 + sizes.c_comb_floats +
                std::size_t(limits.max_channels) * sizes.c_allpass_floats) *
               sizeof(float);
    }
    case CUSTOM_MODULE_USER_FX2:
        return module.option.cb_work;
    default:
        return 0;
    }
}

void InitCustomModule(const CustomModule& module, const RackLimits& limits, u8* p_dsp, u8* p_extra) {
    switch (module.module_id) {
    case CUSTOM_MODULE_COMPRESSOR:
        new (p_dsp) CompressorState{};
        break;
    case CUSTOM_MODULE_FILTER:
        new (p_dsp) CustomFilterDsp{};
        break;
    case CUSTOM_MODULE_LFE:
        new (p_dsp) LfeState{};
        break;
    case CUSTOM_MODULE_GAIN:
        new (p_dsp) CustomGainDsp{};
        break;
    case CUSTOM_MODULE_PEAK_METER:
        new (p_dsp) PeakMeterState{reinterpret_cast<float*>(p_extra), module.option.value, 0};
        break;
    case CUSTOM_MODULE_REVERB: {
        const ReverbSizes sizes = GetReverbSizes(module.option.value);
        auto* p_lines = reinterpret_cast<float*>(p_extra);
        auto* p_state = new (p_dsp) ReverbState{};
        p_state->p_line = p_lines;
        p_state->p_combs = p_lines + sizes.max_line_delay + 1;
        p_state->p_allpasses = p_state->p_combs + sizes.c_comb_floats;
        p_state->sizes = sizes;
        p_state->c_allpass_channels = limits.max_channels;
        break;
    }
    default:
        break;
    }
}

s32 SetCustomModule(OrbisNgs2Handle h_voice, Voice* p_voice, u32 i_module, u32 selector,
                    const OrbisNgs2VoiceParamHeader* p_header) {
    const CustomModule& module = p_voice->p_rack->limits.custom_modules[i_module];
    if (!IsCustomModuleImplemented(module.module_id)) {
        // The parameter is accepted and forgotten: the module passes its input through whatever it is.
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 custom rack module parameter of a module without DSP yet (0x10, "
                                     "0x11, "
                                     "0x16, 0x17, 0x1a, 0x1c, 0x1d, 0x1e, 0x20, 0x70, 0x71 pass through)");
        return ORBIS_OK;
    }
    const u32 cb_expected = SelectorSize(module, selector);
    if (cb_expected == 0) {
        return InvalidControlId(h_voice, p_header);
    }
    if (p_header->size != cb_expected) {
        return InvalidControlSize(h_voice, p_header, cb_expected);
    }
    CustomVoice& custom = *p_voice->p_custom;
    u8* p_block = custom.p_blocks + p_voice->p_rack->p_custom->layout.modules[i_module].off_pending;
    const u32 bit = 1u << i_module;
    const bool is_set = p_voice->p_params->set_modules & bit;
    const bool is_read = selector & kReadSelector;
    s32 result = ORBIS_OK;
    switch (module.module_id) {
    case CUSTOM_MODULE_COMPRESSOR:
        result = SetCompressorModule(*p_voice, selector, p_header, BlockOf<CompressorParams>(p_block), is_set);
        break;
    case CUSTOM_MODULE_FILTER:
        result = SetFilterModule(h_voice, selector, p_header, BlockOf<FilterParams>(p_block), is_set);
        break;
    case CUSTOM_MODULE_LFE: {
        auto* p_param = ParamOf<OrbisNgs2CustomVoiceLfeFilterParam>(p_header);
        auto* p_lfe = BlockOf<LfeParams>(p_block);
        if (is_read) {
            p_param->enableFlag = is_set ? p_lfe->is_enabled : 0;
            p_param->fc = is_set ? p_lfe->cutoff_hz : kUnsetLfeCutoffHz;
        } else if (std::find(std::begin(kLfeCutoffsHz), std::end(kLfeCutoffsHz), p_param->fc) ==
                   std::end(kLfeCutoffsHz)) {
            result = Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_LFE_FC, "Invalid LFE cut off frequency ({})", p_param->fc);
        } else {
            *p_lfe = {p_param->enableFlag != 0 ? 1u : 0u, p_param->fc};
        }
        break;
    }
    case CUSTOM_MODULE_GAIN: {
        auto* p_param = ParamOf<OrbisNgs2CustomVoiceGainParam>(p_header);
        auto* p_levels = BlockOf<float>(p_block);
        for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
            if (is_read) {
                p_param->aLevel[i_channel] = is_set ? p_levels[i_channel] : 1.0f;
            } else {
                p_levels[i_channel] = std::clamp(p_param->aLevel[i_channel], -kMaxGain, kMaxGain);
            }
        }
        break;
    }
    case CUSTOM_MODULE_PEAK_METER: {
        auto* p_param = ParamOf<OrbisNgs2CustomVoicePeakMeterParam>(p_header);
        auto* p_mode = BlockOf<u32>(p_block);
        if (is_read) {
            p_param->enableFlag = is_set ? *p_mode : 0;
        } else if (p_param->enableFlag > kMaxPeakMeterMode) {
            result = Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_PEAKMETER_FLAG, "Invalid peak meter mode ({})",
                          p_param->enableFlag);
        } else {
            *p_mode = p_param->enableFlag;
        }
        break;
    }
    case CUSTOM_MODULE_USER_FX: {
        auto* p_param = ParamOf<OrbisNgs2CustomVoiceUserFxParam>(p_header);
        auto* p_user_fx = BlockOf<UserFxParams>(p_block);
        if (is_read) {
            const UserFxParams user_fx = is_set ? *p_user_fx : UserFxParams{};
            p_param->handler = user_fx.pfn_handler;
            p_param->userData0 = user_fx.user_data[0];
            p_param->userData1 = user_fx.user_data[1];
            p_param->userData2 = user_fx.user_data[2];
        } else {
            *p_user_fx = {p_param->handler, {p_param->userData0, p_param->userData1, p_param->userData2}};
        }
        break;
    }
    case CUSTOM_MODULE_REVERB:
        result = SetReverbModule(h_voice, module, selector, p_header, BlockOf<ReverbParams>(p_block), is_set);
        break;
    case CUSTOM_MODULE_USER_FX2:
        result = SetUserFx2Module(h_voice, p_voice, module, i_module, p_header, p_block);
        break;
    default:
        break;
    }
    if (result >= 0 && !is_read) {
        p_voice->p_params->set_modules |= bit;
        custom.changed_modules |= bit;
    }
    return result;
}

void RunCustomModule(const RenderContext& context, Voice* p_voice, const CustomModuleRun& run, const Planes& in,
                     const Planes& extra, const Planes& out, const Planes& scratch, u32 c_channels) {
    const u32 c_samples = context.c_grain_samples;
    const u32 rate_hz = context.sample_rate_hz;
    const bool is_first = run.flags & 1;
    const bool is_changed = run.flags & 2;
    switch (run.p_module->module_id) {
    case CUSTOM_MODULE_COMPRESSOR: {
        const auto& params = *static_cast<const CompressorParams*>(run.p_params);
        auto* p_state = BlockOf<CompressorState>(run.p_dsp);
        CopyPlanes(out, in, c_channels, c_samples);
        // NOTE: inferred: the detector reads the extra buffer, the second input the firmware hands over after
        // the source (0x12a90, flag 0x10), as the submixer's compressor reads its second input.
        const Planes& detector = (params.flags & COMPRESSOR_SIDECHAIN) ? extra : out;
        ProcessCompressor(params, p_state, out, detector, c_channels, c_samples, rate_hz, is_first);
        // A disabled compressor leaves its meter unwritten (spec 09 §3.4).
        if (!(params.flags & COMPRESSOR_DISABLED)) {
            const float meter[] = {p_state->peak_level, p_state->peak_gain};
            WriteModuleState(run, meter, sizeof(meter));
        }
        break;
    }
    case CUSTOM_MODULE_FILTER: {
        auto* p_dsp = BlockOf<CustomFilterDsp>(run.p_dsp);
        if (is_changed || p_dsp->designed_rate_hz != rate_hz) {
            p_dsp->filter = *static_cast<const FilterParams*>(run.p_params);
            DesignFilter(&p_dsp->filter, rate_hz);
            p_dsp->designed_rate_hz = rate_hz;
        }
        CopyPlanes(out, in, c_channels, c_samples);
        // NOTE: inferred: the chain calls its modules at location 0 (m+0x74), where sub 0 puts a custom
        // rack's filter.
        ProcessFilter(p_dsp->filter, 0, p_dsp->states, out, c_channels, c_samples, is_first);
        break;
    }
    case CUSTOM_MODULE_LFE: {
        auto* p_state = BlockOf<LfeState>(run.p_dsp);
        CopyPlanes(out, in, c_channels, c_samples);
        if (is_first) {
            *p_state = {};
        }
        if (c_channels > kLfeChannel) {
            ProcessLfe(*static_cast<const LfeParams*>(run.p_params), p_state, in.p_channel[kLfeChannel],
                       out.p_channel[kLfeChannel], c_samples, rate_hz);
        }
        break;
    }
    case CUSTOM_MODULE_GAIN: {
        const auto* p_levels = static_cast<const float*>(run.p_params);
        auto* p_dsp = BlockOf<CustomGainDsp>(run.p_dsp);
        for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
            const float level = p_levels[i_channel];
            RampWrite(out.p_channel[i_channel], in.p_channel[i_channel], c_samples,
                      is_first ? level : p_dsp->previous[i_channel], level);
            p_dsp->previous[i_channel] = level;
        }
        break;
    }
    case CUSTOM_MODULE_PEAK_METER: {
        float peaks[kPeakValues + 1] = {};
        ProcessPeakMeter(*static_cast<const u32*>(run.p_params), BlockOf<PeakMeterState>(run.p_dsp), in, c_channels,
                         c_samples, is_first, peaks);
        CopyPlanes(out, in, c_channels, c_samples);
        WriteModuleState(run, peaks, sizeof(peaks));
        break;
    }
    case CUSTOM_MODULE_USER_FX:
        CopyPlanes(out, in, c_channels, c_samples);
        CallUserFx(context, p_voice, *static_cast<const UserFxParams*>(run.p_params), out, c_channels, run.flags);
        break;
    case CUSTOM_MODULE_REVERB: {
        auto* p_state = BlockOf<ReverbState>(run.p_dsp);
        if (is_first) {
            ResetReverb(p_state);
        }
        // The network reads its input after writing its output, so a reverb writing its own source buffer
        // reads a copy.
        const bool is_in_place = in.p_channel[0] == out.p_channel[0];
        if (is_in_place) {
            CopyPlanes(scratch, in, c_channels, c_samples);
        }
        ProcessReverb(*static_cast<const ReverbParams*>(run.p_params), p_state, is_in_place ? scratch : in, c_channels,
                      out, c_channels, c_samples, rate_hz, is_changed);
        break;
    }
    case CUSTOM_MODULE_USER_FX2:
        CopyPlanes(out, in, c_channels, c_samples);
        CallUserFx2Process(p_voice, run.p_module->option, run, out, c_samples, rate_hz);
        break;
    default:
        CopyPlanes(out, in, c_channels, c_samples);
        break;
    }
}

} // namespace Libraries::Ngs2
