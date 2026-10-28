// SPDX-License-Identifier: GPL-2.0-or-later
// The VoiceControl parameters of the modules the sampler and the submixer share (spec 03 §6): their
// checks, the ranges the firmware clamps them to, and the read-back of the 0x8000 ids.
#include <algorithm>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kMaxEnvelopeCurve = 5;
constexpr u32 kMaxPeakMeterMode = 2;
// Location 0 is after the source, 1 after the envelope, distortion and UserFx, 2 + p before port p.
constexpr u32 kMaxFilterLocation = 0x11;

} // namespace

s32 InvalidControlSize(OrbisNgs2Handle h_voice, const OrbisNgs2VoiceParamHeader* p_param, u32 cb_expected) {
    return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE, "Invalid voice parameter size ({}!={})",
                p_param->size, cb_expected);
}

s32 InvalidControlId(OrbisNgs2Handle h_voice, const OrbisNgs2VoiceParamHeader* p_param) {
    return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID, "Invalid voice control ID ({:#x})",
                p_param->id);
}

s32 SetEnvelope(OrbisNgs2Handle h_voice, const Voice& voice, const OrbisNgs2SamplerVoiceEnvelopeParam* p_param) {
    const u64 c_points = u64(p_param->numForwardPoints) + p_param->numReleasePoints;
    if (c_points > 0 && !p_param->aPoint) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_ENVELOPE_POINT_ADDRESS, "Invalid envelope point address ({})",
                    static_cast<const void*>(p_param->aPoint));
    }
    if (c_points > voice.p_rack->limits.max_envelope_points) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_ENVELOPE_POINTS, "Invalid number of envelope points ({})",
                    c_points);
    }
    for (u64 i_point = 0; i_point < c_points; ++i_point) {
        if (p_param->aPoint[i_point].curve > kMaxEnvelopeCurve) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_ENVELOPE_CURVE, "Invalid envelope curve ({})",
                        p_param->aPoint[i_point].curve);
        }
    }
    EnvelopeParams envelope{};
    envelope.c_forward = p_param->numForwardPoints;
    envelope.c_release = p_param->numReleasePoints;
    for (u64 i_point = 0; i_point < c_points; ++i_point) {
        const OrbisNgs2EnvelopePoint& point = p_param->aPoint[i_point];
        envelope.points[i_point] = {point.curve, point.duration, std::clamp(point.height, 0.0f, 1.0f)};
    }
    voice.p_params->envelope = envelope;
    voice.p_params->set_modules |= MODULE_ENVELOPE;
    return ORBIS_OK;
}

// Without a parameter the counts read 0 and the game's point array is left alone (spec 09 §13).
void ReadEnvelope(const VoiceParams& params, OrbisNgs2SamplerVoiceEnvelopeParam* p_param) {
    if (!(params.set_modules & MODULE_ENVELOPE)) {
        p_param->numForwardPoints = 0;
        p_param->numReleasePoints = 0;
        return;
    }
    const EnvelopeParams& envelope = params.envelope;
    p_param->numForwardPoints = envelope.c_forward;
    p_param->numReleasePoints = envelope.c_release;
    if (p_param->aPoint) {
        auto* p_points = const_cast<OrbisNgs2EnvelopePoint*>(p_param->aPoint);
        std::copy_n(envelope.points, envelope.c_forward + envelope.c_release, p_points);
    }
}

void SetDistortion(const OrbisNgs2SamplerVoiceDistortionParam* p_param, VoiceParams* p_params) {
    p_params->distortion = {p_param->flags,
                            std::clamp(p_param->a, 0.0f, 10.0f),
                            std::clamp(p_param->b, 0.0f, 10.0f),
                            std::clamp(p_param->clip, 0.0f, 1.0f),
                            std::clamp(p_param->gate, 0.0f, 1.0f),
                            std::clamp(p_param->wetLevel, 0.0f, 32.0f),
                            std::clamp(p_param->dryLevel, 0.0f, 32.0f)};
    p_params->set_modules |= MODULE_DISTORTION;
}

void ReadDistortion(const VoiceParams& params, OrbisNgs2SamplerVoiceDistortionParam* p_param) {
    if (!(params.set_modules & MODULE_DISTORTION)) {
        p_param->reserved = 0;
    }
    const DistortionParams distortion =
        (params.set_modules & MODULE_DISTORTION) ? params.distortion : DistortionParams{};
    p_param->flags = distortion.flags;
    p_param->a = distortion.a;
    p_param->b = distortion.b;
    p_param->clip = distortion.clip;
    p_param->gate = distortion.gate;
    p_param->wetLevel = distortion.wet;
    p_param->dryLevel = distortion.dry;
}

void SetUserFx(const OrbisNgs2SamplerVoiceUserFxParam* p_param, VoiceParams* p_params) {
    p_params->user_fx = {p_param->handler, {p_param->userData0, p_param->userData1, p_param->userData2}};
    p_params->set_modules |= MODULE_USER_FX;
}

void ReadUserFx(const VoiceParams& params, OrbisNgs2SamplerVoiceUserFxParam* p_param) {
    const UserFxParams user_fx = (params.set_modules & MODULE_USER_FX) ? params.user_fx : UserFxParams{};
    p_param->handler = user_fx.pfn_handler;
    p_param->userData0 = user_fx.user_data[0];
    p_param->userData1 = user_fx.user_data[1];
    p_param->userData2 = user_fx.user_data[2];
}

// Bit 2 (sidechain) only holds on a rack with two inputs, and the read-back rebuilds the flags from
// the four bits the firmware keeps (spec 09 §3.2).
void SetCompressor(const OrbisNgs2SubmixerVoiceCompressorParam& param, u32 max_inputs, CompressorParams* p_out) {
    const u32 flag_mask = COMPRESSOR_DISABLED | COMPRESSOR_RMS | COMPRESSOR_LINK_WITHOUT_LFE |
                          (max_inputs >= 2 ? u32(COMPRESSOR_SIDECHAIN) : 0);
    *p_out = {param.flags & flag_mask,
              std::clamp(param.threshold, 0.0f, 1.0f),
              std::clamp(param.ratio, 0.0f, 1.0f),
              std::clamp(param.knee, 0.0f, 1.0f),
              std::clamp(param.attackTime, 0.0f, 10.0f),
              std::clamp(param.releaseTime, 0.0f, 10.0f),
              std::clamp(param.level, 0.0f, 32.0f)};
}

void ReadCompressor(const CompressorParams* p_compressor, OrbisNgs2SubmixerVoiceCompressorParam* p_param) {
    const CompressorParams compressor = p_compressor ? *p_compressor : CompressorParams{};
    p_param->flags = compressor.flags;
    p_param->threshold = compressor.threshold;
    p_param->ratio = compressor.slope;
    p_param->knee = compressor.knee;
    p_param->attackTime = compressor.attack_s;
    p_param->releaseTime = compressor.release_s;
    p_param->level = compressor.level;
    p_param->reserved = 0;
}

s32 SetPeakMeter(OrbisNgs2Handle h_voice, const OrbisNgs2SamplerVoicePeakMeterParam* p_param, VoiceParams* p_params) {
    if (p_param->enableFlag > kMaxPeakMeterMode) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_PEAKMETER_FLAG, "Invalid peak meter mode ({})",
                    p_param->enableFlag);
    }
    p_params->meter_mode = p_param->enableFlag;
    p_params->set_modules |= MODULE_METER;
    return ORBIS_OK;
}

u32 ReadPeakMeter(const VoiceParams& params) {
    return (params.set_modules & MODULE_METER) ? params.meter_mode : 0;
}

s32 SetFilter(OrbisNgs2Handle h_voice, const Voice& voice, const OrbisNgs2SamplerVoiceFilterParam* p_param) {
    if (p_param->index >= voice.p_rack->limits.max_filters) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_FILTER_INDEX, "Invalid filter index ({})", p_param->index);
    }
    if (p_param->location > kMaxFilterLocation) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_FILTER_LOCATION, "Invalid filter location ({})",
                    p_param->location);
    }
    const auto& direct = p_param->param.direct;
    voice.p_filters[p_param->index] = {p_param->location,
                                       p_param->type,
                                       p_param->channelMask,
                                       {direct.i0, direct.i1, direct.i2, direct.o1, direct.o2},
                                       {}};
    voice.p_params->set_modules |= FilterModule(p_param->index);
    return ORBIS_OK;
}

// Without a parameter everything after the index reads 0 (spec 09 §10).
s32 ReadFilter(OrbisNgs2Handle h_voice, const Voice& voice, OrbisNgs2SamplerVoiceFilterParam* p_param) {
    if (p_param->index >= voice.p_rack->limits.max_filters) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_FILTER_INDEX, "Invalid filter index ({})", p_param->index);
    }
    const bool is_set = voice.p_params->set_modules & FilterModule(p_param->index);
    if (!is_set) {
        p_param->reserved3 = 0;
    }
    const FilterParams filter = is_set ? voice.p_filters[p_param->index] : FilterParams{};
    p_param->location = filter.location;
    p_param->type = filter.type;
    p_param->channelMask = filter.bypass_mask;
    p_param->param.direct = {filter.f[0], filter.f[1], filter.f[2], filter.f[3], filter.f[4]};
    return ORBIS_OK;
}

void RunFilters(const Voice& voice, u32 location, const Planes& planes, u32 c_channels, u32 c_samples) {
    const u32 set_modules = voice.p_active_params->set_modules;
    for (u32 i_filter = 0; i_filter < voice.p_rack->limits.max_filters; ++i_filter) {
        const u32 module = FilterModule(i_filter);
        if (set_modules & module) {
            ProcessFilter(voice.p_active_filters[i_filter], location,
                          voice.p_filter_states + std::size_t(i_filter) * kMaxChannels, planes, c_channels, c_samples,
                          (voice.first_active_modules & module) != 0);
        }
    }
}

bool HasFilterAt(const Voice& voice, u32 location) {
    for (u32 i_filter = 0; i_filter < voice.p_rack->limits.max_filters; ++i_filter) {
        if ((voice.p_active_params->set_modules & FilterModule(i_filter)) &&
            voice.p_active_filters[i_filter].location == location) {
            return true;
        }
    }
    return false;
}

} // namespace Libraries::Ngs2
