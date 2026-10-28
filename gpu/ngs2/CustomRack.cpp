// SPDX-License-Identifier: GPL-2.0-or-later
// Custom racks (spec 02 §4.5, 03 §5.5, 04 §2.2, §2.5, §2.6, §4.3): their blocks in the rack's buffer, their
// setups and the routing of module parameters, and each voice's grain: the source or the inputs into buffer 0
// (and 1), the game's module chain over the work buffers, the grain's events on every buffer written, then
// the ports or the output.
#include <algorithm>
#include <cstring>
#include <new>

#include "Custom.h"
#include "core/libraries/ngs2/ngs2_custom.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kCustomSamplerRackId = 0x4001;
constexpr u32 kCustomSubmixerRackId = 0x4002;
constexpr u32 kCustomMasteringRackId = 0x4003;
constexpr u32 kModuleParameterFamily = 0x4000;
constexpr u32 kSamplerParameterFamily = 0x1000;
constexpr u32 kReadFlag = 0x8000;
constexpr std::size_t kBlockAlign = 16;
constexpr std::size_t kLineAlign = 64;

enum CustomParamId : u32 {
    CUSTOM_SUBMIXER_SETUP = 0x40020000,
    CUSTOM_SUBMIXER_READ_SETUP = 0x40028000,
    CUSTOM_MASTERING_SETUP = 0x40030000,
    CUSTOM_MASTERING_OUTPUT = 0x40030001,
    CUSTOM_MASTERING_READ_SETUP = 0x40038000,
    CUSTOM_MASTERING_READ_OUTPUT = 0x40038001,
};

// The voice state a custom rack keeps ahead of its modules' (spec 02 §4.5): the flags, and the custom
// sampler's source data. NOTE: inferred: the source's 0x28 bytes at +0x08 end with what the sampler's state
// holds at +0x10, so they are written there and +0x08 stays 0.
constexpr u32 kSourceStateOffset = 0x10;

template <class Param>
Param* ParamOf(const OrbisNgs2VoiceParamHeader* p_header) {
    return reinterpret_cast<Param*>(const_cast<OrbisNgs2VoiceParamHeader*>(p_header));
}

std::size_t AlignUp(std::size_t value, std::size_t align) {
    return (value + align - 1) & ~(align - 1);
}

void PlanCustomLayout(const RackLimits& limits, CustomLayout* p_layout) {
    std::size_t off_next = 0;
    for (u32 i_module = 0; i_module < limits.c_custom_modules; ++i_module) {
        const CustomModule& module = limits.custom_modules[i_module];
        CustomModuleLayout& layout = p_layout->modules[i_module];
        layout.module_id = module.module_id;
        layout.cb_params = GetCustomParamsSize(module);
        layout.off_pending = AlignUp(off_next, kBlockAlign);
        layout.off_active = AlignUp(layout.off_pending + layout.cb_params, kBlockAlign);
        layout.off_dsp = AlignUp(layout.off_active + layout.cb_params, kBlockAlign);
        layout.cb_extra = GetCustomExtraSize(module, limits);
        layout.off_extra = AlignUp(layout.off_dsp + GetCustomDspSize(module), kLineAlign);
        off_next = layout.off_extra + layout.cb_extra;
    }
    p_layout->cb_voice = off_next;
}

bool IsMixerChannelCount(u32 c_channels) {
    return c_channels == 1 || c_channels == 2 || c_channels == 6 || c_channels == 8;
}

s32 CheckChannels(OrbisNgs2Handle h_voice, const Voice& voice, u32 c_channels) {
    if (!IsMixerChannelCount(c_channels) || c_channels > voice.p_rack->limits.max_channels) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({})", c_channels);
    }
    return ORBIS_OK;
}

// 0x40010000 to 0x40010005, 0x40018000 and 0x40018001 are the sampler's 0x10000000 to 0x10000005, 0x10008000
// and 0x10008001 (spec 03 §5.5), run on a copy carrying the sampler's id.
s32 ControlCustomSampler(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    const u32 low = p_header->id & 0xffff;
    if (low > 5 && low != kReadFlag && low != (kReadFlag | 1)) {
        return InvalidControlId(h_voice, p_header);
    }
    alignas(8) u8 param[0x40] = {};
    // The size is checked on the copy before anything past the header is read, so a size past the copy only
    // brings the header along.
    const std::size_t cb_copied = p_header->size <= sizeof(param) ? std::max<std::size_t>(p_header->size, 8) : 8;
    std::memcpy(param, p_header, cb_copied);
    auto* p_copy = reinterpret_cast<OrbisNgs2VoiceParamHeader*>(param);
    p_copy->id = (kSamplerParameterFamily << 16) | low;
    const s32 result = ControlSampler(h_voice, p_voice, p_copy);
    if (result >= 0 && (low & kReadFlag)) {
        constexpr std::size_t kCbHeader = sizeof(OrbisNgs2VoiceParamHeader);
        std::memcpy(reinterpret_cast<u8*>(const_cast<OrbisNgs2VoiceParamHeader*>(p_header)) + kCbHeader,
                    param + kCbHeader, cb_copied - kCbHeader);
    }
    return result;
}

s32 ControlCustomSubmixer(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    if (p_header->id != CUSTOM_SUBMIXER_SETUP && p_header->id != CUSTOM_SUBMIXER_READ_SETUP) {
        return InvalidControlId(h_voice, p_header);
    }
    if (p_header->size != sizeof(OrbisNgs2CustomSubmixerVoiceSetupParam)) {
        return InvalidControlSize(h_voice, p_header, sizeof(OrbisNgs2CustomSubmixerVoiceSetupParam));
    }
    auto* p_param = ParamOf<OrbisNgs2CustomSubmixerVoiceSetupParam>(p_header);
    VoiceParams& params = *p_voice->p_params;
    if (p_header->id == CUSTOM_SUBMIXER_READ_SETUP) {
        p_param->numInputChannels = params.c_in_channels;
        p_param->numOutputChannels = params.c_out_channels;
        return ORBIS_OK;
    }
    for (const u32 c_channels : {p_param->numInputChannels, p_param->numOutputChannels}) {
        if (const s32 result = CheckChannels(h_voice, *p_voice, c_channels); result < 0) {
            return result;
        }
    }
    params.c_in_channels = p_param->numInputChannels;
    params.c_out_channels = p_param->numOutputChannels;
    params.is_setup = true;
    params.set_modules = 0;
    p_voice->control.fetch_or(VOICE_PENDING_SETUP, std::memory_order_relaxed);
    return ORBIS_OK;
}

// The output module 0x1a sits after the game's modules, at index numModules.
u32 OutputModuleBit(const Voice& voice) {
    return 1u << voice.p_rack->limits.c_custom_modules;
}

s32 ControlCustomMastering(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    static_assert(sizeof(OrbisNgs2CustomMasteringVoiceSetupParam) == 0x10);
    static_assert(sizeof(OrbisNgs2CustomMasteringVoiceOutputParam) == 0x10);
    switch (p_header->id) {
    case CUSTOM_MASTERING_SETUP:
    case CUSTOM_MASTERING_OUTPUT:
    case CUSTOM_MASTERING_READ_SETUP:
    case CUSTOM_MASTERING_READ_OUTPUT:
        break;
    default:
        return InvalidControlId(h_voice, p_header);
    }
    if (p_header->size != 0x10) {
        return InvalidControlSize(h_voice, p_header, 0x10);
    }
    VoiceParams& params = *p_voice->p_params;
    const u32 output_bit = OutputModuleBit(*p_voice);
    switch (p_header->id) {
    case CUSTOM_MASTERING_SETUP: {
        // NOTE: inferred: the channel check is the mastering setup's (0x30000000), which spec 03 §5.5 does
        // not repeat for this one.
        const u32 c_channels = ParamOf<OrbisNgs2CustomMasteringVoiceSetupParam>(p_header)->numInputChannels;
        if (const s32 result = CheckChannels(h_voice, *p_voice, c_channels); result < 0) {
            return result;
        }
        params.c_in_channels = c_channels;
        params.c_out_channels = c_channels;
        params.is_setup = true;
        params.i_output_buffer = 0;
        params.set_modules = output_bit;
        p_voice->control.fetch_or(VOICE_PENDING_SETUP, std::memory_order_relaxed);
        return ORBIS_OK;
    }
    case CUSTOM_MASTERING_OUTPUT:
        params.i_output_buffer = ParamOf<OrbisNgs2CustomMasteringVoiceOutputParam>(p_header)->outputId;
        params.set_modules |= output_bit;
        return ORBIS_OK;
    case CUSTOM_MASTERING_READ_SETUP:
        ParamOf<OrbisNgs2CustomMasteringVoiceSetupParam>(p_header)->numInputChannels = params.c_in_channels;
        return ORBIS_OK;
    default:
        ParamOf<OrbisNgs2CustomMasteringVoiceOutputParam>(p_header)->outputId =
            (params.set_modules & output_bit) ? params.i_output_buffer : 0;
        return ORBIS_OK;
    }
}

Planes BufferPlanes(const CustomRack& custom, u32 i_buffer) {
    Planes planes;
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        planes.p_channel[i_channel] =
            custom.p_buffers + (std::size_t(i_buffer) * kMaxChannels + i_channel) * custom.c_plane_samples;
    }
    return planes;
}

void ClearPlanes(const Planes& planes, u32 i_first, u32 c_channels, u32 c_samples) {
    for (u32 i_channel = i_first; i_channel < c_channels; ++i_channel) {
        std::memset(planes.p_channel[i_channel], 0, c_samples * sizeof(float));
    }
}

void ReadInputInto(RenderContext& context, Voice* p_voice, u32 i_input, const Planes& buffer, u32 c_channels) {
    const Planes input = ReadInput(context, p_voice, i_input);
    const u32 c_in = std::min(p_voice->c_in_channels, c_channels);
    for (u32 i_channel = 0; i_channel < c_in; ++i_channel) {
        std::memcpy(buffer.p_channel[i_channel], input.p_channel[i_channel], context.c_grain_samples * sizeof(float));
    }
    ClearPlanes(buffer, c_in, c_channels, context.c_grain_samples);
}

// 0x12a90 (spec 04 §4.3): a module without a parameter, and one rea has not read yet, copies its source into
// its destination when they differ. A module that runs gets a cleared source if nothing wrote it this grain,
// and its destination holds data afterwards. NOTE: inferred: the descriptors' buffer flags (+0x28: 0x10 extra
// input, 0x20 keep the source, 0x40 no output, 0x80 clear the destination) are not read for these modules,
// which are taken to set none but the compressor's sidechain extra input.
void RunChain(RenderContext& context, Voice* p_voice, const Planes* p_buffers, const Planes& scratch, u32 c_channels,
              u32* p_valid_mask) {
    const Rack& rack = *p_voice->p_rack;
    const CustomRack& custom = *rack.p_custom;
    CustomVoice& voice_custom = *p_voice->p_custom;
    const u32 set_modules = p_voice->p_active_params->set_modules;
    u32& valid_mask = *p_valid_mask;
    for (u32 i_module = 0; i_module < rack.limits.c_custom_modules; ++i_module) {
        const CustomModule& module = rack.limits.custom_modules[i_module];
        const u32 bit = 1u << i_module;
        const u32 source_bit = 1u << module.source_buffer_id;
        const u32 dest_bit = 1u << module.dest_buffer_id;
        const Planes& in = p_buffers[module.source_buffer_id];
        const Planes& out = p_buffers[module.dest_buffer_id];
        if (!(set_modules & bit)) {
            if (module.source_buffer_id != module.dest_buffer_id) {
                for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
                    std::memcpy(out.p_channel[i_channel], in.p_channel[i_channel],
                                context.c_grain_samples * sizeof(float));
                }
                valid_mask = (valid_mask & ~dest_bit) | ((valid_mask & source_bit) ? dest_bit : 0);
            }
            continue;
        }
        if (!(valid_mask & source_bit)) {
            ClearPlanes(in, 0, c_channels, context.c_grain_samples);
            valid_mask |= source_bit;
        }
        const Planes& extra = p_buffers[module.extra_buffer_id];
        if (module.module_id == CUSTOM_MODULE_COMPRESSOR && !(valid_mask & (1u << module.extra_buffer_id))) {
            ClearPlanes(extra, 0, c_channels, context.c_grain_samples);
        }
        const CustomModuleLayout& layout = custom.layout.modules[i_module];
        u8* p_blocks = voice_custom.p_blocks;
        const bool has_state = module.off_state != 0;
        const CustomModuleRun run{&module,
                                  p_blocks + layout.off_active,
                                  p_blocks + layout.off_dsp,
                                  p_blocks + layout.off_extra,
                                  custom.p_commons[i_module],
                                  has_state ? p_voice->p_state + module.off_state : nullptr,
                                  has_state ? module.cb_state : 0,
                                  ((p_voice->first_active_modules & bit) ? 1u : 0u) |
                                      ((voice_custom.active_changed_modules & bit) ? 2u : 0u),
                                  p_voice->c_in_channels,
                                  p_voice->c_out_channels};
        RunCustomModule(context, p_voice, run, in, extra, out, scratch, c_channels);
        valid_mask |= dest_bit;
    }
}

} // namespace

CustomRack* LayOutCustomRack(Arena* p_arena, const RackLimits& limits) {
    auto* p_custom = static_cast<CustomRack*>(p_arena->Allocate(sizeof(CustomRack), alignof(CustomRack)));
    const std::size_t c_floats =
        std::size_t(limits.c_custom_buffers + 1) * kMaxChannels * std::size_t(limits.max_grain_samples);
    auto* p_buffers = static_cast<float*>(p_arena->Allocate(c_floats * sizeof(float), kLineAlign));
    void* p_commons[ORBIS_NGS2_CUSTOM_MAX_MODULES] = {};
    for (u32 i_module = 0; i_module < limits.c_custom_modules; ++i_module) {
        const CustomModule& module = limits.custom_modules[i_module];
        if (module.module_id == CUSTOM_MODULE_USER_FX2 && module.option.cb_common > 0) {
            p_commons[i_module] = p_arena->Allocate(module.option.cb_common, kLineAlign);
        }
    }
    if (!p_custom || !p_buffers || p_arena->HasOverflowed()) {
        return nullptr;
    }
    new (p_custom) CustomRack{};
    PlanCustomLayout(limits, &p_custom->layout);
    p_custom->p_buffers = p_buffers;
    p_custom->c_buffers = limits.c_custom_buffers;
    p_custom->c_plane_samples = limits.max_grain_samples;
    std::copy(std::begin(p_commons), std::end(p_commons), p_custom->p_commons);
    return p_custom;
}

CustomVoice* LayOutCustomVoice(Arena* p_arena, const RackLimits& limits, const CustomRack* p_custom_rack) {
    CustomLayout planned;
    const CustomLayout* p_layout = p_custom_rack ? &p_custom_rack->layout : nullptr;
    if (!p_layout) {
        PlanCustomLayout(limits, &planned);
        p_layout = &planned;
    }
    auto* p_custom = static_cast<CustomVoice*>(p_arena->Allocate(sizeof(CustomVoice), alignof(CustomVoice)));
    auto* p_blocks = p_layout->cb_voice ? static_cast<u8*>(p_arena->Allocate(p_layout->cb_voice, kLineAlign)) : nullptr;
    if (!p_custom || (p_layout->cb_voice && !p_blocks) || p_arena->HasOverflowed()) {
        return nullptr;
    }
    new (p_custom) CustomVoice{0, 0, p_blocks};
    for (u32 i_module = 0; i_module < limits.c_custom_modules; ++i_module) {
        const CustomModuleLayout& layout = p_layout->modules[i_module];
        InitCustomModule(limits.custom_modules[i_module], limits, p_blocks + layout.off_dsp,
                         p_blocks + layout.off_extra);
    }
    return p_custom;
}

// NOTE: inferred: UserFx2's setup is taken to be its per-voice init (descriptor +0x98, 0x243c0, spec 04 §4)
// and to run once per voice when the rack is built, its cleanup when the rack is destroyed; when the firmware
// calls 0x243c0 is not read yet.
void StartCustomModules(Rack* p_rack) {
    const RackLimits& limits = p_rack->limits;
    const CustomRack& custom = *p_rack->p_custom;
    for (u32 i_module = 0; i_module < limits.c_custom_modules; ++i_module) {
        const CustomModule& module = limits.custom_modules[i_module];
        if (module.module_id != CUSTOM_MODULE_USER_FX2) {
            continue;
        }
        const CustomModuleLayout& layout = custom.layout.modules[i_module];
        for (u32 i_voice = 0; i_voice < limits.max_voices; ++i_voice) {
            u8* p_blocks = p_rack->p_voices[i_voice].p_custom->p_blocks;
            CallUserFx2Setup(module.option, custom.p_commons[i_module], p_blocks + layout.off_pending,
                             p_blocks + layout.off_extra, limits.max_voices, i_voice);
            std::memcpy(p_blocks + layout.off_active, p_blocks + layout.off_pending, layout.cb_params);
        }
    }
}

void StopCustomModules(Rack* p_rack) {
    const RackLimits& limits = p_rack->limits;
    const CustomRack& custom = *p_rack->p_custom;
    for (u32 i_module = 0; i_module < limits.c_custom_modules; ++i_module) {
        const CustomModule& module = limits.custom_modules[i_module];
        if (module.module_id != CUSTOM_MODULE_USER_FX2) {
            continue;
        }
        const CustomModuleLayout& layout = custom.layout.modules[i_module];
        for (u32 i_voice = 0; i_voice < limits.max_voices; ++i_voice) {
            u8* p_blocks = p_rack->p_voices[i_voice].p_custom->p_blocks;
            CallUserFx2Cleanup(module.option, custom.p_commons[i_module], p_blocks + layout.off_pending,
                               p_blocks + layout.off_extra, limits.max_voices, i_voice);
        }
    }
}

s32 ControlCustom(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    const u32 id = p_header->id;
    if ((id >> 16) == kModuleParameterFamily) {
        // id = 0x40000000 | read 0x8000 | moduleId << 8 | sub << 5 | moduleIndex (spec 03 §5.5).
        const u32 i_module = id & 0x1f;
        const u32 selector = ((id >> 5) & 7) | (id & kReadFlag);
        const u32 module_id = (id >> 8) & 0x7f;
        const RackLimits& limits = p_voice->p_rack->limits;
        if (i_module >= limits.c_custom_modules || limits.custom_modules[i_module].module_id != module_id) {
            return InvalidControlId(h_voice, p_header);
        }
        return SetCustomModule(h_voice, p_voice, i_module, selector, p_header);
    }
    switch (p_voice->p_rack->p_kind->rack_id) {
    case kCustomSamplerRackId:
        return ControlCustomSampler(h_voice, p_voice, p_header);
    case kCustomSubmixerRackId:
        return ControlCustomSubmixer(h_voice, p_voice, p_header);
    case kCustomMasteringRackId:
        return ControlCustomMastering(h_voice, p_voice, p_header);
    default:
        return InvalidControlId(h_voice, p_header);
    }
}

void FlushCustomVoice(Voice* p_voice) {
    CustomVoice& custom = *p_voice->p_custom;
    const CustomLayout& layout = p_voice->p_rack->p_custom->layout;
    const u32 changed = custom.changed_modules & p_voice->p_params->set_modules;
    for (u32 i_module = 0; i_module < p_voice->p_rack->limits.c_custom_modules; ++i_module) {
        if (changed & (1u << i_module)) {
            const CustomModuleLayout& module = layout.modules[i_module];
            std::memcpy(custom.p_blocks + module.off_active, custom.p_blocks + module.off_pending, module.cb_params);
        }
    }
    custom.active_changed_modules |= changed;
    custom.changed_modules = 0;
}

void ProcessCustomVoice(RenderContext& context, Voice* p_voice, u32* p_event_code) {
    const Rack& rack = *p_voice->p_rack;
    const CustomRack& custom = *rack.p_custom;
    const u32 c_samples = context.c_grain_samples;
    const u32 c_channels = std::clamp(std::max(p_voice->c_in_channels, p_voice->c_out_channels), 1u, kMaxChannels);
    Planes buffers[kMaxChannels];
    for (u32 i_buffer = 0; i_buffer < custom.c_buffers; ++i_buffer) {
        buffers[i_buffer] = BufferPlanes(custom, i_buffer);
    }
    const Planes scratch = BufferPlanes(custom, custom.c_buffers);
    std::memset(p_voice->p_state, 0, rack.limits.cb_voice_state);
    u32 valid_mask = 0;
    if (rack.p_kind->rack_class == RACK_CLASS_SOURCE) {
        p_voice->flags = RunSamplerSource(context, p_voice, buffers[0]);
        ClearPlanes(buffers[0], p_voice->c_out_channels, c_channels, c_samples);
        valid_mask = 1;
    } else {
        const bool is_paused = p_voice->flags & STATE_PAUSED;
        p_voice->flags = is_paused ? p_voice->flags & ~u32(STATE_PLAYING) : p_voice->flags | STATE_PLAYING;
        ReadInputInto(context, p_voice, 0, buffers[0], c_channels);
        valid_mask = 1;
        if (rack.limits.max_inputs > 1 && custom.c_buffers > 1) {
            ReadInputInto(context, p_voice, 1, buffers[1], c_channels);
            valid_mask |= 2;
        }
    }
    RunChain(context, p_voice, buffers, scratch, c_channels, &valid_mask);
    for (u32 i_buffer = 0; i_buffer < custom.c_buffers; ++i_buffer) {
        if (valid_mask & (1u << i_buffer)) {
            ApplyEventCode(buffers[i_buffer], c_channels, c_samples, *p_event_code);
        }
    }
    const VoiceParams& params = *p_voice->p_active_params;
    if (rack.p_kind->rack_class == RACK_CLASS_MASTERING) {
        if (params.set_modules & OutputModuleBit(*p_voice)) {
            WriteRenderBuffer(context, params.i_output_buffer, buffers[0], p_voice->c_out_channels);
        }
    } else {
        MixCustomPorts(context, p_voice, buffers, valid_mask, p_voice->c_out_channels);
    }
    std::memcpy(p_voice->p_state, &p_voice->flags, std::min<std::size_t>(sizeof(u32), rack.limits.cb_voice_state));
    if (rack.p_kind->rack_class == RACK_CLASS_SOURCE &&
        rack.limits.cb_voice_state >= kSourceStateOffset + 4 * sizeof(u64)) {
        const SourceState& source = p_voice->p_dsp->source;
        const u64 values[] = {source.c_decoded_samples, source.cb_decoded, source.block_user_data,
                              reinterpret_cast<u64>(source.p_read)};
        std::memcpy(p_voice->p_state + kSourceStateOffset, values, sizeof(values));
    }
    p_voice->p_custom->active_changed_modules = 0;
}

} // namespace Libraries::Ngs2
