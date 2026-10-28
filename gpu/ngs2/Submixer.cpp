// SPDX-License-Identifier: GPL-2.0-or-later
// The submixer rack, 0x2000: its VoiceControl parameters (0x15f60; spec 03 §5.2) and its processing
// (0x161c0; spec 04 §2.3): the voice's input through filters, envelope, compressor, distortion,
// UserFx and peak meter, then its ports.
#include <algorithm>
#include <cstring>

#include "Internal.h"
#include "core/libraries/ngs2/ngs2_submixer.h"

namespace Libraries::Ngs2 {
namespace {

enum SubmixerParamId : u32 {
    SUBMIXER_SETUP = 0x20000000,
    SUBMIXER_ENVELOPE = 0x20000001,
    SUBMIXER_COMPRESSOR = 0x20000002,
    SUBMIXER_DISTORTION = 0x20000003,
    SUBMIXER_USER_FX = 0x20000004,
    SUBMIXER_PEAK_METER = 0x20000005,
    SUBMIXER_FILTER = 0x20000006,
    SUBMIXER_READ_SETUP = 0x20008000,
    SUBMIXER_READ_ENVELOPE = 0x20008001,
    SUBMIXER_READ_COMPRESSOR = 0x20008002,
    SUBMIXER_READ_DISTORTION = 0x20008003,
    SUBMIXER_READ_USER_FX = 0x20008004,
    SUBMIXER_READ_PEAK_METER = 0x20008005,
    SUBMIXER_READ_FILTER = 0x20008006,
    SUBMIXER_READ_MAX_FILTERS = 0x20008007,
};

template <class Param>
Param* ParamOf(const OrbisNgs2VoiceParamHeader* p_header) {
    return reinterpret_cast<Param*>(const_cast<OrbisNgs2VoiceParamHeader*>(p_header));
}

u32 ParamSize(u32 id) {
    switch (id) {
    case SUBMIXER_SETUP:
    case SUBMIXER_READ_SETUP:
        return sizeof(OrbisNgs2SubmixerVoiceSetupParam);
    case SUBMIXER_ENVELOPE:
    case SUBMIXER_READ_ENVELOPE:
        return sizeof(OrbisNgs2SubmixerVoiceEnvelopeParam);
    case SUBMIXER_COMPRESSOR:
    case SUBMIXER_READ_COMPRESSOR:
        return sizeof(OrbisNgs2SubmixerVoiceCompressorParam);
    case SUBMIXER_DISTORTION:
    case SUBMIXER_READ_DISTORTION:
        return sizeof(OrbisNgs2SubmixerVoiceDistortionParam);
    case SUBMIXER_USER_FX:
    case SUBMIXER_READ_USER_FX:
        return sizeof(OrbisNgs2SubmixerVoiceUserFxParam);
    case SUBMIXER_PEAK_METER:
    case SUBMIXER_READ_PEAK_METER:
        return sizeof(OrbisNgs2SubmixerVoicePeakMeterParam);
    case SUBMIXER_FILTER:
    case SUBMIXER_READ_FILTER:
        return sizeof(OrbisNgs2SubmixerVoiceFilterParam);
    case SUBMIXER_READ_MAX_FILTERS:
        return sizeof(OrbisNgs2SubmixerVoiceNumFilters);
    default:
        return 0;
    }
}

bool IsMixerChannelCount(u32 c_channels) {
    return c_channels == 1 || c_channels == 2 || c_channels == 6 || c_channels == 8;
}

void SetCompressor(const OrbisNgs2SubmixerVoiceCompressorParam* p_param, CompressorParams* p_out) {
    *p_out = {p_param->flags,
              std::clamp(p_param->threshold, 0.0f, 1.0f),
              std::clamp(p_param->ratio, 0.0f, 1.0f),
              std::clamp(p_param->knee, 0.0f, 1.0f),
              std::clamp(p_param->attackTime, 0.0f, 10.0f),
              std::clamp(p_param->releaseTime, 0.0f, 10.0f),
              std::clamp(p_param->level, 0.0f, 32.0f),
              true};
}

void ReadCompressor(const CompressorParams& compressor, OrbisNgs2SubmixerVoiceCompressorParam* p_param) {
    p_param->flags = compressor.flags;
    p_param->threshold = compressor.threshold;
    p_param->ratio = compressor.slope;
    p_param->knee = compressor.knee;
    p_param->attackTime = compressor.attack_s;
    p_param->releaseTime = compressor.release_s;
    p_param->level = compressor.level;
}

void RunFilters(const Voice& voice, u32 location, const Planes& planes, u32 c_channels, u32 c_samples) {
    for (u32 i_filter = 0; i_filter < voice.p_rack->limits.max_filters; ++i_filter) {
        ProcessFilter(voice.p_active_filters[i_filter], location,
                      voice.p_filter_states + std::size_t(i_filter) * kMaxChannels, planes, c_channels, c_samples,
                      voice.is_first_grain);
    }
}

} // namespace

s32 ControlSubmixer(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    const u32 cb_expected = ParamSize(p_header->id);
    if (cb_expected == 0) {
        return InvalidControlId(h_voice, p_header);
    }
    if (p_header->size != cb_expected) {
        return InvalidControlSize(h_voice, p_header, cb_expected);
    }
    VoiceParams& params = *p_voice->p_params;
    switch (p_header->id) {
    case SUBMIXER_SETUP: {
        const u32 c_channels = ParamOf<OrbisNgs2SubmixerVoiceSetupParam>(p_header)->numIoChannels;
        if (!IsMixerChannelCount(c_channels) || c_channels > p_voice->p_rack->limits.max_channels) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({})", c_channels);
        }
        params.c_in_channels = c_channels;
        params.c_out_channels = c_channels;
        params.is_setup = true;
        p_voice->control.fetch_or(VOICE_PENDING_SETUP, std::memory_order_relaxed);
        return ORBIS_OK;
    }
    case SUBMIXER_ENVELOPE:
        return SetEnvelope(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header),
                           &params.envelope);
    case SUBMIXER_COMPRESSOR:
        SetCompressor(ParamOf<OrbisNgs2SubmixerVoiceCompressorParam>(p_header), &params.compressor);
        return ORBIS_OK;
    case SUBMIXER_DISTORTION:
        SetDistortion(ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header), &params.distortion);
        return ORBIS_OK;
    case SUBMIXER_USER_FX:
        SetUserFx(ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header), &params.user_fx);
        return ORBIS_OK;
    case SUBMIXER_PEAK_METER:
        return SetPeakMeter(h_voice, ParamOf<OrbisNgs2SamplerVoicePeakMeterParam>(p_header),
                            &params.meter_mode);
    case SUBMIXER_FILTER:
        return SetFilter(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header));
    case SUBMIXER_READ_SETUP:
        ParamOf<OrbisNgs2SubmixerVoiceSetupParam>(p_header)->numIoChannels = params.c_out_channels;
        return ORBIS_OK;
    case SUBMIXER_READ_ENVELOPE:
        ReadEnvelope(params.envelope, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_COMPRESSOR:
        ReadCompressor(params.compressor, ParamOf<OrbisNgs2SubmixerVoiceCompressorParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_DISTORTION:
        ReadDistortion(params.distortion, ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_USER_FX:
        ReadUserFx(params.user_fx, ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_PEAK_METER:
        ParamOf<OrbisNgs2SubmixerVoicePeakMeterParam>(p_header)->enableFlag = params.meter_mode;
        return ORBIS_OK;
    case SUBMIXER_READ_FILTER:
        return ReadFilter(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header));
    case SUBMIXER_READ_MAX_FILTERS:
        ParamOf<OrbisNgs2SubmixerVoiceNumFilters>(p_header)->numFilters = p_voice->p_rack->limits.max_filters;
        return ORBIS_OK;
    default:
        return InvalidControlId(h_voice, p_header);
    }
}

void ProcessSubmixerVoice(RenderContext& context, Voice* p_voice, u32* p_event_code) {
    const VoiceParams& params = *p_voice->p_active_params;
    const u32 c_channels = p_voice->c_in_channels;
    const u32 c_samples = context.c_grain_samples;
    VoiceDsp& dsp = *p_voice->p_dsp;
    const bool is_paused = p_voice->flags & STATE_PAUSED;
    p_voice->flags = is_paused ? p_voice->flags & ~u32(STATE_PLAYING) : p_voice->flags | STATE_PLAYING;
    const Planes input = ReadInput(context, p_voice, 0);
    const Planes planes = WorkPlanes(context, WORK_VOICE);
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        std::memcpy(planes.p_channel[i_channel], input.p_channel[i_channel], c_samples * sizeof(float));
    }
    const Rack& rack = *p_voice->p_rack;
    if (rack.limits.max_inputs > 1 && p_voice->p_input_started[rack.c_input_blocks + rack.i_input_block]) {
        // Something reached the second input (maxInputs 2), but how the submixer uses it is not read yet.
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 submixer second input (maxInputs 2; rea: 0x161c0)");
    }
    RunFilters(*p_voice, 0, planes, c_channels, c_samples);
    dsp.envelope_height = ProcessEnvelope(params.envelope, &dsp.envelope, planes, c_channels, c_samples,
                                          p_voice->is_first_grain, &p_voice->flags, p_event_code);
    ProcessCompressor(params.compressor, &dsp.compressor, planes, c_channels, c_samples, context.sample_rate_hz,
                      p_voice->is_first_grain);
    ProcessDistortion(params.distortion, planes, c_channels, c_samples);
    if (params.user_fx.is_set && params.user_fx.pfn_handler) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "Ngs2 submixer UserFx 0x20000004: the game's handler is not called yet (F6)");
    }
    RunFilters(*p_voice, 1, planes, c_channels, c_samples);
    if (!is_paused) {
        ProcessPeakMeter(params.meter_mode, &p_voice->meters[0], planes, c_channels, c_samples,
                         p_voice->is_first_grain, dsp.peaks[0]);
    }
    ApplyEventCode(planes, c_channels, c_samples, *p_event_code);
    MixPorts(context, p_voice, planes, c_channels);
    OrbisNgs2SubmixerVoiceState state{};
    state.voiceState.stateFlags = p_voice->flags;
    state.envelopeHeight = dsp.envelope_height;
    state.peakHeight = dsp.peaks[0][0];
    state.compressorHeight = dsp.compressor.peak_gain;
    std::memcpy(p_voice->p_state, &state, sizeof(state));
}

} // namespace Libraries::Ngs2
