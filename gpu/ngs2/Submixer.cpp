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
        params.set_modules = 0;
        p_voice->control.fetch_or(VOICE_PENDING_SETUP, std::memory_order_relaxed);
        return ORBIS_OK;
    }
    case SUBMIXER_ENVELOPE:
        return SetEnvelope(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header));
    case SUBMIXER_COMPRESSOR:
        SetCompressor(*ParamOf<OrbisNgs2SubmixerVoiceCompressorParam>(p_header), p_voice->p_rack->limits.max_inputs,
                      &params.compressor);
        params.set_modules |= MODULE_COMPRESSOR;
        return ORBIS_OK;
    case SUBMIXER_DISTORTION:
        SetDistortion(ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header), &params);
        return ORBIS_OK;
    case SUBMIXER_USER_FX:
        SetUserFx(ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header), &params);
        return ORBIS_OK;
    case SUBMIXER_PEAK_METER:
        return SetPeakMeter(h_voice, ParamOf<OrbisNgs2SamplerVoicePeakMeterParam>(p_header), &params);
    case SUBMIXER_FILTER:
        return SetFilter(h_voice, *p_voice, ParamOf<OrbisNgs2SamplerVoiceFilterParam>(p_header));
    case SUBMIXER_READ_SETUP:
        ParamOf<OrbisNgs2SubmixerVoiceSetupParam>(p_header)->numIoChannels = params.c_out_channels;
        return ORBIS_OK;
    case SUBMIXER_READ_ENVELOPE:
        ReadEnvelope(params, ParamOf<OrbisNgs2SamplerVoiceEnvelopeParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_COMPRESSOR:
        ReadCompressor((params.set_modules & MODULE_COMPRESSOR) ? &params.compressor : nullptr,
                       ParamOf<OrbisNgs2SubmixerVoiceCompressorParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_DISTORTION:
        ReadDistortion(params, ParamOf<OrbisNgs2SamplerVoiceDistortionParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_USER_FX:
        ReadUserFx(params, ParamOf<OrbisNgs2SamplerVoiceUserFxParam>(p_header));
        return ORBIS_OK;
    case SUBMIXER_READ_PEAK_METER:
        ParamOf<OrbisNgs2SubmixerVoicePeakMeterParam>(p_header)->enableFlag = ReadPeakMeter(params);
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
    // The second input only feeds the compressor's detector (spec 09 §11); it is cleared like the
    // first when nothing reached it.
    const Planes sidechain = p_voice->p_rack->limits.max_inputs > 1 ? ReadInput(context, p_voice, 1) : planes;
    const u32 set_modules = params.set_modules;
    const u32 first_modules = p_voice->first_active_modules;
    RunFilters(*p_voice, 0, planes, c_channels, c_samples);
    dsp.envelope_height = 1.0f;
    if (set_modules & MODULE_ENVELOPE) {
        dsp.envelope_height = ProcessEnvelope(params.envelope, &dsp.envelope, planes, c_channels, c_samples,
                                              first_modules & MODULE_ENVELOPE, &p_voice->flags, p_event_code);
    }
    if (set_modules & MODULE_COMPRESSOR) {
        const Planes& detector = (params.compressor.flags & COMPRESSOR_SIDECHAIN) ? sidechain : planes;
        ProcessCompressor(params.compressor, &dsp.compressor, planes, detector, c_channels, c_samples,
                          context.sample_rate_hz, first_modules & MODULE_COMPRESSOR);
    } else {
        dsp.compressor.peak_gain = 1.0f;
    }
    if (set_modules & MODULE_DISTORTION) {
        ProcessDistortion(params.distortion, planes, c_channels, c_samples);
    }
    if (set_modules & MODULE_USER_FX) {
        RunVoiceUserFx(context, p_voice, params.user_fx, planes, c_channels, MODULE_USER_FX);
    }
    RunFilters(*p_voice, 1, planes, c_channels, c_samples);
    if (!(set_modules & MODULE_METER)) {
        std::fill_n(dsp.peaks[0], kPeakValues, 0.0f);
    } else if (!is_paused) {
        ProcessPeakMeter(params.meter_mode, &p_voice->meters[0], planes, c_channels, c_samples,
                         first_modules & MODULE_METER, dsp.peaks[0]);
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
