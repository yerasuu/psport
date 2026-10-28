// SPDX-License-Identifier: GPL-2.0-or-later
// The reverb rack, 0x2001: its VoiceControl parameters (0x14920; spec 03 §5.3, 07 §2) and its processing
// (0x14b60; spec 04 §2.4, 07 §4): the voice's input through module 0x1b, then its ports without filters.
#include <algorithm>
#include <cstring>

#include "Internal.h"
#include "Reverb.h"

namespace Libraries::Ngs2 {
namespace {

enum ReverbParamId : u32 {
    REVERB_PARAM_SETUP = 0x20010000,
    REVERB_PARAM_I3DL2 = 0x20010001,
    REVERB_PARAM_EXPLICIT = 0x20010002,
    REVERB_PARAM_READ_SETUP = 0x20018000,
    REVERB_PARAM_READ_I3DL2 = 0x20018001,
};

// Without the module (no setup yet), SDK 6.00 on passes the input on and older ones output their
// work buffer (spec 07 §5).
constexpr u32 kInPlaceSdkVersion = 0x6000000;

// What 0x20018001 answers while the block is in explicit mode (spec 07 §2.1): the setter's -1, not an
// Ngs2 error code.
constexpr s32 kReadOtherModeResult = -1;

template <class Param>
Param* ParamOf(const OrbisNgs2VoiceParamHeader* p_header) {
    return reinterpret_cast<Param*>(const_cast<OrbisNgs2VoiceParamHeader*>(p_header));
}

u32 ParamSize(u32 id) {
    switch (id) {
    case REVERB_PARAM_SETUP:
    case REVERB_PARAM_READ_SETUP:
        return sizeof(OrbisNgs2ReverbVoiceSetupParam);
    case REVERB_PARAM_I3DL2:
    case REVERB_PARAM_READ_I3DL2:
        return sizeof(OrbisNgs2ReverbVoiceI3DL2Param);
    case REVERB_PARAM_EXPLICIT:
        return sizeof(ReverbExplicitParam);
    default:
        return 0;
    }
}

bool IsMixerChannelCount(u32 c_channels) {
    return c_channels == 1 || c_channels == 2 || c_channels == 6 || c_channels == 8;
}

s32 Setup(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2ReverbVoiceSetupParam* p_param) {
    for (const u32 c_channels : {p_param->numInputChannels, p_param->numOutputChannels}) {
        if (!IsMixerChannelCount(c_channels) || c_channels > p_voice->p_rack->limits.max_channels) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({})", c_channels);
        }
    }
    VoiceParams& params = *p_voice->p_params;
    params.c_in_channels = p_param->numInputChannels;
    params.c_out_channels = p_param->numOutputChannels;
    params.is_setup = true;
    params.set_modules = MODULE_REVERB;
    SetReverbDefaults(p_voice->p_reverb_params);
    p_voice->control.fetch_or(VOICE_PENDING_SETUP, std::memory_order_relaxed);
    return ORBIS_OK;
}

} // namespace

s32 ControlReverb(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    const u32 cb_expected = ParamSize(p_header->id);
    if (cb_expected == 0) {
        return InvalidControlId(h_voice, p_header);
    }
    if (p_header->size != cb_expected) {
        return InvalidControlSize(h_voice, p_header, cb_expected);
    }
    VoiceParams& params = *p_voice->p_params;
    switch (p_header->id) {
    case REVERB_PARAM_SETUP:
        return Setup(h_voice, p_voice, ParamOf<OrbisNgs2ReverbVoiceSetupParam>(p_header));
    case REVERB_PARAM_I3DL2:
        SetReverbI3dl2(ParamOf<OrbisNgs2ReverbVoiceI3DL2Param>(p_header)->i3dl2, p_voice->p_reverb_params);
        params.set_modules |= MODULE_REVERB;
        return ORBIS_OK;
    case REVERB_PARAM_EXPLICIT: {
        const ReverbSizes sizes = GetReverbSizes(p_voice->p_rack->limits.reverb_size);
        if (const s32 result = SetReverbExplicit(h_voice, *ParamOf<ReverbExplicitParam>(p_header), sizes,
                                                 p_voice->p_reverb_params);
            result < 0) {
            return result;
        }
        params.set_modules |= MODULE_REVERB;
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 reverb explicit network 0x20010002: tap routing, comb filter, "
                                     "per-channel levels and the fields at 0xa0-0xb8 not read yet");
        return ORBIS_OK;
    }
    case REVERB_PARAM_READ_SETUP: {
        auto* p_param = ParamOf<OrbisNgs2ReverbVoiceSetupParam>(p_header);
        p_param->numInputChannels = params.c_in_channels;
        p_param->numOutputChannels = params.c_out_channels;
        return ORBIS_OK;
    }
    case REVERB_PARAM_READ_I3DL2:
        if (!ReadReverbI3dl2(*p_voice->p_reverb_params, &ParamOf<OrbisNgs2ReverbVoiceI3DL2Param>(p_header)->i3dl2)) {
            return kReadOtherModeResult;
        }
        return ORBIS_OK;
    default:
        return InvalidControlId(h_voice, p_header);
    }
}

void ProcessReverbVoice(RenderContext& context, Voice* p_voice, u32* p_event_code) {
    const VoiceParams& params = *p_voice->p_active_params;
    const u32 c_in = p_voice->c_in_channels;
    const u32 c_out = p_voice->c_out_channels;
    const u32 c_samples = context.c_grain_samples;
    const bool is_paused = p_voice->flags & STATE_PAUSED;
    p_voice->flags = is_paused ? p_voice->flags & ~u32(STATE_PLAYING) : p_voice->flags | STATE_PLAYING;
    const Planes input = ReadInput(context, p_voice, 0);
    const Planes output = WorkPlanes(context, WORK_VOICE);
    if (params.set_modules & MODULE_REVERB) {
        if (p_voice->first_active_modules & MODULE_REVERB) {
            ResetReverb(p_voice->p_reverb);
        }
        ProcessReverb(*p_voice->p_active_reverb_params, p_voice->p_reverb, input, c_in, output, c_out, c_samples,
                      context.sample_rate_hz, p_voice->are_params_changed);
    } else {
        const bool is_passed_on = IsCompiledSdkAtLeast(kInPlaceSdkVersion);
        for (u32 i_channel = 0; i_channel < c_out; ++i_channel) {
            if (is_passed_on && i_channel < c_in) {
                std::memcpy(output.p_channel[i_channel], input.p_channel[i_channel], c_samples * sizeof(float));
            } else {
                std::memset(output.p_channel[i_channel], 0, c_samples * sizeof(float));
            }
        }
        if (!is_passed_on) {
            static std::uint64_t s_c_uses;
            NoteUnimplemented(&s_c_uses, "Ngs2 reverb voice without its module before SDK 6.00: work buffer "
                                         "contents not read, silence");
        }
    }
    ApplyEventCode(output, c_out, c_samples, *p_event_code);
    MixPorts(context, p_voice, output, c_out);
    OrbisNgs2ReverbVoiceState state{};
    state.voiceState.stateFlags = p_voice->flags;
    std::memcpy(p_voice->p_state, &state, sizeof(state));
}

} // namespace Libraries::Ngs2
