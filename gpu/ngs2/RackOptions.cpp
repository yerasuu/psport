// SPDX-License-Identifier: GPL-2.0-or-later
// The seven rack kinds of firmware 11.00's table at 0x50140 and their options: ranges, defaults and
// the order the firmware checks them in (spec 02 §1, §2, §4, 08 §2).
#include <algorithm>
#include <iterator>

#include "Internal.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_reverb.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kSamplerRackId = 0x1000;
constexpr u32 kSubmixerRackId = 0x2000;
constexpr u32 kReverbRackId = 0x2001;
constexpr u32 kMasteringRackId = 0x3000;
constexpr u32 kCustomSamplerRackId = 0x4001;
constexpr u32 kCustomSubmixerRackId = 0x4002;
constexpr u32 kCustomMasteringRackId = 0x4003;
constexpr u32 kCustomModuleParameterFamily = 0x4000;

constexpr RackKind kRackKinds[] = {
    {kSamplerRackId, RACK_CLASS_SOURCE, sizeof(OrbisNgs2SamplerRackOption), 0x30, sizeof(OrbisNgs2SamplerRackInfo),
     false},
    {kSubmixerRackId, RACK_CLASS_INTERMEDIATE, sizeof(OrbisNgs2SubmixerRackOption), 0x10,
     sizeof(OrbisNgs2SubmixerRackInfo), false},
    {kReverbRackId, RACK_CLASS_INTERMEDIATE, sizeof(OrbisNgs2ReverbRackOption), 0x4, sizeof(OrbisNgs2ReverbRackInfo),
     false},
    {kMasteringRackId, RACK_CLASS_MASTERING, sizeof(OrbisNgs2MasteringRackOption), 0x4c,
     sizeof(OrbisNgs2MasteringRackInfo), false},
    {kCustomSamplerRackId, RACK_CLASS_SOURCE, sizeof(OrbisNgs2CustomSamplerRackOption), 0,
     sizeof(OrbisNgs2CustomSamplerRackInfo), true},
    {kCustomSubmixerRackId, RACK_CLASS_INTERMEDIATE, sizeof(OrbisNgs2CustomSubmixerRackOption), 0,
     sizeof(OrbisNgs2CustomSubmixerRackInfo), true},
    {kCustomMasteringRackId, RACK_CLASS_MASTERING, sizeof(OrbisNgs2CustomMasteringRackOption), 0,
     sizeof(OrbisNgs2CustomMasteringRackInfo), true},
};

// The voice state every custom rack keeps ahead of its modules' (spec 02 §4.5).
constexpr u32 kCustomSamplerFixedState = 0x30;
constexpr u32 kCustomFixedState = 0xc;

// Ids in the module table at 0x50000 (spec 02 §2.4).
bool IsModuleId(u32 module_id) {
    return (module_id >= 0x10 && module_id <= 0x20) || module_id == 0x70 || module_id == 0x71;
}

// Takes a field from the option, or its default without one.
template <class Option>
u32 Field(const Option* p_option, u32 Option::* p_field, u32 default_value) {
    return p_option ? p_option->*p_field : default_value;
}

s32 OutOfRange(s32 code, const char* sz_field, u32 value, u32 min_value, u32 max_value) {
    return Fail(0, code, "Invalid rack option ({}={}:{}~{})", sz_field, value, min_value, max_value);
}

bool IsOutOfRange(u32 value, u32 min_value, u32 max_value) {
    return value < min_value || value > max_value;
}

#define NGS2_CHECK_RANGE(code, sz_field, value, min_value, max_value)                                          \
    do {                                                                                                       \
        if (IsOutOfRange(value, min_value, max_value)) {                                                       \
            return OutOfRange(code, sz_field, value, min_value, max_value);                                    \
        }                                                                                                      \
    } while (false)

s32 ParseAtrac9Limits(u32 c_ajm_decoders, u32 max_codec_caches, u32 c_atrac9_channel_works, RackLimits* p_limits) {
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_ATRAC9_DECODERS, "maxAjmAtrac9Decoders", c_ajm_decoders, 0u,
                     std::min(p_limits->max_voices, 256u));
    p_limits->max_ajm_atrac9_decoders = c_ajm_decoders;
    p_limits->max_atrac9_channel_works = c_atrac9_channel_works;
    if (p_limits->max_atrac9_decoders > 0) {
        NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_ATRAC9_CHANNEL_WORKS, "maxAtrac9ChannelWorks",
                         c_atrac9_channel_works, p_limits->max_atrac9_decoders, p_limits->max_atrac9_decoders * 8);
    }
    p_limits->max_codec_caches = max_codec_caches;
    if (c_ajm_decoders > 0) {
        NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_CODEC_CACHES, "maxCodecCaches", max_codec_caches, 10u,
                         c_ajm_decoders * 8);
    }
    return ORBIS_OK;
}

s32 ParseSamplerOption(const OrbisNgs2SamplerRackOption* p_option, RackLimits* p_limits) {
    using Option = OrbisNgs2SamplerRackOption;
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES, "maxGrainSamples", p_limits->max_grain_samples,
                     kMinGrainSamples, kMaxGrainSamples);
    const u32 c_voices = p_option ? p_option->rackOption.maxVoices : 256;
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_VOICES, "maxVoices", c_voices, 1u, 1024u);
    p_limits->max_voices = c_voices;
    const u32 c_channel_works = Field(p_option, &Option::maxChannelWorks, c_voices);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_CHANNEL_WORKS, "maxChannelWorks", c_channel_works, c_voices,
                     c_voices * 8);
    p_limits->max_channel_works = c_channel_works;
    const u32 c_atrac9_decoders = Field(p_option, &Option::maxAtrac9Decoders, c_voices);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_ATRAC9_DECODERS, "maxAtrac9Decoders", c_atrac9_decoders, 0u,
                     c_voices);
    p_limits->max_atrac9_decoders = c_atrac9_decoders;
    const u32 c_waveform_blocks = Field(p_option, &Option::maxWaveformBlocks, 4);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_WAVEFORM_BLOCKS, "maxWaveformBlocks", c_waveform_blocks, 1u, 16u);
    p_limits->max_waveform_blocks = c_waveform_blocks;
    const u32 c_envelope_points = Field(p_option, &Option::maxEnvelopePoints, 4);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_ENVELOPE_POINTS, "maxEnvelopePoints", c_envelope_points, 0u, 16u);
    p_limits->max_envelope_points = c_envelope_points;
    const u32 c_filters = Field(p_option, &Option::maxFilters, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_FILTERS, "maxFilters", c_filters, 0u, 16u);
    p_limits->max_filters = c_filters;
    const u32 c_ajm_decoders = Field(p_option, &Option::maxAjmAtrac9Decoders, 0);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_ATRAC9_DECODERS, "maxAjmAtrac9Decoders", c_ajm_decoders, 0u,
                     std::min(c_voices, 256u));
    const u32 c_peak_meter_blocks = Field(p_option, &Option::numPeakMeterBlocks, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_NUM_PEAKMETER_BLOCKS, "numPeakMeterBlocks", c_peak_meter_blocks, 1u,
                     256u);
    p_limits->num_peak_meter_blocks = c_peak_meter_blocks;
    p_limits->max_channels = 8;
    p_limits->max_inputs = 0;
    return ParseAtrac9Limits(c_ajm_decoders,
                             Field(p_option, &Option::maxCodecCaches, std::max(10u, c_ajm_decoders)),
                             Field(p_option, &Option::maxAtrac9ChannelWorks, c_channel_works), p_limits);
}

s32 ParseSubmixerOption(const OrbisNgs2SubmixerRackOption* p_option, RackLimits* p_limits) {
    using Option = OrbisNgs2SubmixerRackOption;
    const u32 c_voices = p_option ? p_option->rackOption.maxVoices : 1;
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_VOICES, "maxVoices", c_voices, 1u, 1024u);
    const u32 c_channels = Field(p_option, &Option::maxChannels, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_CHANNELS, "maxChannels", c_channels, 1u, 8u);
    const u32 c_envelope_points = Field(p_option, &Option::maxEnvelopePoints, 4);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_ENVELOPE_POINTS, "maxEnvelopePoints", c_envelope_points, 0u, 16u);
    const u32 c_filters = Field(p_option, &Option::maxFilters, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_FILTERS, "maxFilters", c_filters, 0u, 16u);
    const u32 c_inputs = Field(p_option, &Option::maxInputs, 1);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_INPUTS, "maxInputs", c_inputs, 1u, 2u);
    const u32 c_peak_meter_blocks = Field(p_option, &Option::numPeakMeterBlocks, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_NUM_PEAKMETER_BLOCKS, "numPeakMeterBlocks", c_peak_meter_blocks, 1u,
                     256u);
    p_limits->max_voices = c_voices;
    p_limits->max_channels = c_channels;
    p_limits->max_channel_works = c_channels * c_voices;
    p_limits->max_envelope_points = c_envelope_points;
    p_limits->max_filters = c_filters;
    p_limits->max_inputs = c_inputs;
    p_limits->num_peak_meter_blocks = c_peak_meter_blocks;
    return ORBIS_OK;
}

s32 ParseReverbOption(const OrbisNgs2ReverbRackOption* p_option, RackLimits* p_limits) {
    using Option = OrbisNgs2ReverbRackOption;
    const u32 c_voices = p_option ? p_option->rackOption.maxVoices : 1;
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_VOICES, "maxVoices", c_voices, 1u, 16u);
    const u32 c_channels = Field(p_option, &Option::maxChannels, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_CHANNELS, "maxChannels", c_channels, 1u, 8u);
    const u32 reverb_size = Field(p_option, &Option::reverbSize, 1);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_REVERB_SIZE, "reverbSize", reverb_size, 0u, 1u);
    p_limits->max_voices = c_voices;
    p_limits->max_channels = c_channels;
    p_limits->max_channel_works = c_channels * c_voices;
    p_limits->max_inputs = 1;
    p_limits->reverb_size = reverb_size;
    return ORBIS_OK;
}

s32 ParseMasteringOption(const OrbisNgs2MasteringRackOption* p_option, RackLimits* p_limits) {
    using Option = OrbisNgs2MasteringRackOption;
    const u32 c_voices = p_option ? p_option->rackOption.maxVoices : 2;
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_VOICES, "maxVoices", c_voices, 1u, 256u);
    const u32 c_channels = Field(p_option, &Option::maxChannels, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_CHANNELS, "maxChannels", c_channels, 1u, 8u);
    const u32 c_peak_meter_blocks = Field(p_option, &Option::numPeakMeterBlocks, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_NUM_PEAKMETER_BLOCKS, "numPeakMeterBlocks", c_peak_meter_blocks, 1u,
                     256u);
    p_limits->max_voices = c_voices;
    p_limits->max_channels = c_channels;
    p_limits->max_channel_works = c_channels * c_voices;
    p_limits->max_inputs = 1;
    p_limits->num_peak_meter_blocks = c_peak_meter_blocks;
    return ORBIS_OK;
}

s32 ParseCustomLayout(const OrbisNgs2CustomRackOption* p_option, u32 cb_fixed_state, RackLimits* p_limits) {
    using Option = OrbisNgs2CustomRackOption;
    const u32 cb_state = Field(p_option, &Option::stateSize, 4);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_VOICE_STATE_SIZE, "stateSize", cb_state, 4u, 0x7fffffffu);
    const u32 c_buffers = Field(p_option, &Option::numBuffers, 1);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_NUM_MODULE_BUFFERS, "numBuffers", c_buffers, 1u, 8u);
    const u32 c_modules = Field(p_option, &Option::numModules, 0);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_NUM_MODULES, "numModules", c_modules, 0u,
                     u32(ORBIS_NGS2_CUSTOM_MAX_MODULES));
    p_limits->cb_custom_state = cb_state;
    p_limits->cb_voice_state = cb_state;
    p_limits->c_custom_buffers = c_buffers;
    p_limits->c_custom_modules = c_modules;
    for (u32 i_module = 0; i_module < c_modules; ++i_module) {
        const auto& module = p_option->aModule[i_module];
        if (module.moduleId > 0x7f || !IsModuleId(module.moduleId)) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_MODULE_ID, "Unknown module ID ({})", module.moduleId);
        }
        if (module.sourceBufferId >= c_buffers || module.extraBufferId >= c_buffers ||
            module.destBufferId >= c_buffers) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_MODULE_BUFFER_ID, "Invalid module buffer ID ({},{},{}:<{})",
                        module.sourceBufferId, module.extraBufferId, module.destBufferId, c_buffers);
        }
        if (module.stateOffset > cb_state || (module.stateOffset != 0 && module.stateOffset < cb_fixed_state)) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_MODULE_STATE_OFFSET, "Invalid module state offset ({}<{})",
                        module.stateOffset, cb_fixed_state);
        }
        if (module.stateSize > cb_state - module.stateOffset) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_MODULE_STATE_SIZE, "Invalid module state size ({})",
                        module.stateSize);
        }
        p_limits->custom_modules[i_module] = {module.moduleId,  module.sourceBufferId, module.extraBufferId,
                                              module.destBufferId, module.stateOffset,  module.stateSize};
    }
    // maxPorts is only range-checked after the rack's own fields, so the port table bounds the loop.
    const u32 c_ports = std::min(p_limits->max_ports, u32(ORBIS_NGS2_CUSTOM_MAX_PORTS));
    for (u32 i_port = 0; i_port < c_ports; ++i_port) {
        const u32 source_buffer_id = p_option ? p_option->aPort[i_port].sourceBufferId : 0;
        if (source_buffer_id >= c_buffers) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_MODULE_ID, "Invalid port buffer ID ({}:<{})", source_buffer_id,
                        c_buffers);
        }
        p_limits->custom_port_sources[i_port] = source_buffer_id;
    }
    return ORBIS_OK;
}

s32 ParseCustomSamplerOption(const OrbisNgs2CustomSamplerRackOption* p_option, RackLimits* p_limits) {
    using Option = OrbisNgs2CustomSamplerRackOption;
    const u32 c_voices = p_option ? p_option->customRackOption.rackOption.maxVoices : 256;
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_VOICES, "maxVoices", c_voices, 1u, 1024u);
    p_limits->max_voices = c_voices;
    if (const s32 result =
            ParseCustomLayout(p_option ? &p_option->customRackOption : nullptr, kCustomSamplerFixedState, p_limits);
        result < 0) {
        return result;
    }
    const u32 c_channel_works = Field(p_option, &Option::maxChannelWorks, c_voices);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_CHANNEL_WORKS, "maxChannelWorks", c_channel_works, c_voices,
                     c_voices * 8);
    p_limits->max_channel_works = c_channel_works;
    const u32 c_waveform_blocks = Field(p_option, &Option::maxWaveformBlocks, 4);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_WAVEFORM_BLOCKS, "maxWaveformBlocks", c_waveform_blocks, 0u,
                     0x7fffffffu);
    p_limits->max_waveform_blocks = c_waveform_blocks;
    const u32 c_atrac9_decoders = Field(p_option, &Option::maxAtrac9Decoders, c_voices);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_ATRAC9_DECODERS, "maxAtrac9Decoders", c_atrac9_decoders, 0u,
                     c_voices);
    p_limits->max_atrac9_decoders = c_atrac9_decoders;
    p_limits->max_channels = 8;
    p_limits->max_inputs = 0;
    const u32 c_ajm_decoders = Field(p_option, &Option::maxAjmAtrac9Decoders, 0);
    return ParseAtrac9Limits(c_ajm_decoders,
                             Field(p_option, &Option::maxCodecCaches, std::max(10u, c_ajm_decoders)),
                             Field(p_option, &Option::maxAtrac9ChannelWorks, c_channel_works), p_limits);
}

// Custom submixer and custom mastering share their option's tail.
template <class Option>
s32 ParseCustomMixerOption(const Option* p_option, RackLimits* p_limits) {
    const u32 c_voices = p_option ? p_option->customRackOption.rackOption.maxVoices : 1;
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_VOICES, "maxVoices", c_voices, 1u, 1024u);
    p_limits->max_voices = c_voices;
    if (const s32 result =
            ParseCustomLayout(p_option ? &p_option->customRackOption : nullptr, kCustomFixedState, p_limits);
        result < 0) {
        return result;
    }
    const u32 c_channels = Field(p_option, &Option::maxChannels, 8);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_CHANNELS, "maxChannels", c_channels, 1u, 8u);
    const u32 c_inputs = Field(p_option, &Option::maxInputs, 1);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_INPUTS, "maxInputs", c_inputs, 1u, 2u);
    p_limits->max_channels = c_channels;
    p_limits->max_channel_works = c_channels * c_voices;
    p_limits->max_inputs = c_inputs;
    return ORBIS_OK;
}

// The checks every rack makes after its own (0x65f0, 08 §2).
s32 ParseCommonLimits(RackClass rack_class, const RackLimits& limits) {
    if (!(limits.max_grain_samples >= kMinGrainSamples && limits.max_grain_samples <= kMaxGrainSamples &&
          limits.max_grain_samples % kMinGrainSamples == 0)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES, "Invalid maximum of grain samples ({})",
                    limits.max_grain_samples);
    }
    if (rack_class == RACK_CLASS_SOURCE) {
        NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_INPUT_DELAY_BLOCKS, "maxInputDelayBlocks",
                         limits.max_input_delay_blocks, 0u, 0u);
    } else {
        NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_INPUT_DELAY_BLOCKS, "maxInputDelayBlocks",
                         limits.max_input_delay_blocks, 1u, 1024u);
    }
    if (rack_class == RACK_CLASS_MASTERING) {
        if (limits.max_matrices != 0) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_MAX_MATRICES, "Invalid rack option (maxMatrices={}:0)",
                        limits.max_matrices);
        }
        if (limits.max_ports != 0) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_MAX_PORTS, "Invalid rack option (maxPorts={}:0)",
                        limits.max_ports);
        }
        return ORBIS_OK;
    }
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_MATRICES, "maxMatrices", limits.max_matrices, 1u, 16u);
    NGS2_CHECK_RANGE(ORBIS_NGS2_ERROR_INVALID_MAX_PORTS, "maxPorts", limits.max_ports, 1u, 16u);
    return ORBIS_OK;
}

#undef NGS2_CHECK_RANGE

} // namespace

const RackKind* FindRackKind(u32 rack_id) {
    const auto* p_kind = std::find_if(std::begin(kRackKinds), std::end(kRackKinds),
                                      [&](const RackKind& kind) { return kind.rack_id == rack_id; });
    return p_kind != std::end(kRackKinds) ? p_kind : nullptr;
}

s32 ParseRackOption(const RackKind& kind, const OrbisNgs2RackOption* p_option, RackLimits* p_out_limits) {
    RackLimits limits{};
    limits.cb_voice_state = kind.cb_voice_state;
    limits.max_grain_samples = 512;
    limits.max_input_delay_blocks = kind.rack_class == RACK_CLASS_SOURCE ? 0 : 1;
    limits.max_matrices = kind.rack_class == RACK_CLASS_MASTERING ? 0 : 1;
    limits.max_ports = kind.rack_class == RACK_CLASS_MASTERING ? 0 : 8;
    if (p_option) {
        if (p_option->size < sizeof(OrbisNgs2RackOption)) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE, "Invalid rack option size ({})", p_option->size);
        }
        // Bit 31 asks for buffer guards and bit 30 for room for voice callbacks; the rest is reserved.
        if (p_option->flags & 0x3fffffffu) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_OPTION_FLAG, "Invalid rack option flags ({})", p_option->flags);
        }
        if (p_option->size != kind.cb_option) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE, "Invalid option size ({}!={})", p_option->size,
                        kind.cb_option);
        }
        limits.flags = p_option->flags;
        limits.max_grain_samples = p_option->maxGrainSamples;
        limits.max_input_delay_blocks = p_option->maxInputDelayBlocks;
        limits.max_matrices = p_option->maxMatrices;
        limits.max_ports = p_option->maxPorts;
    }
    s32 result = ORBIS_OK;
    switch (kind.rack_id) {
    case kSamplerRackId:
        result = ParseSamplerOption(reinterpret_cast<const OrbisNgs2SamplerRackOption*>(p_option), &limits);
        break;
    case kSubmixerRackId:
        result = ParseSubmixerOption(reinterpret_cast<const OrbisNgs2SubmixerRackOption*>(p_option), &limits);
        break;
    case kReverbRackId:
        result = ParseReverbOption(reinterpret_cast<const OrbisNgs2ReverbRackOption*>(p_option), &limits);
        break;
    case kMasteringRackId:
        result = ParseMasteringOption(reinterpret_cast<const OrbisNgs2MasteringRackOption*>(p_option), &limits);
        break;
    case kCustomSamplerRackId:
        result =
            ParseCustomSamplerOption(reinterpret_cast<const OrbisNgs2CustomSamplerRackOption*>(p_option), &limits);
        break;
    case kCustomSubmixerRackId:
        result = ParseCustomMixerOption(reinterpret_cast<const OrbisNgs2CustomSubmixerRackOption*>(p_option),
                                        &limits);
        break;
    case kCustomMasteringRackId:
        result = ParseCustomMixerOption(reinterpret_cast<const OrbisNgs2CustomMasteringRackOption*>(p_option),
                                        &limits);
        break;
    default:
        break;
    }
    if (result < 0) {
        return result;
    }
    if (const s32 common_result = ParseCommonLimits(kind.rack_class, limits); common_result < 0) {
        return common_result;
    }
    *p_out_limits = limits;
    return ORBIS_OK;
}

bool IsRackParameter(const RackKind& kind, u32 id) {
    const u32 family = id >> 16;
    return family == kind.rack_id || (kind.is_custom && family == kCustomModuleParameterFamily);
}

} // namespace Libraries::Ngs2
