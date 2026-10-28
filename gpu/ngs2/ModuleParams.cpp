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

s32 SetEnvelope(OrbisNgs2Handle h_voice, const Voice& voice, const OrbisNgs2SamplerVoiceEnvelopeParam* p_param,
                EnvelopeParams* p_out) {
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
    envelope.is_set = true;
    *p_out = envelope;
    return ORBIS_OK;
}

void ReadEnvelope(const EnvelopeParams& envelope, OrbisNgs2SamplerVoiceEnvelopeParam* p_param) {
    p_param->numForwardPoints = envelope.c_forward;
    p_param->numReleasePoints = envelope.c_release;
    if (p_param->aPoint) {
        auto* p_points = const_cast<OrbisNgs2EnvelopePoint*>(p_param->aPoint);
        std::copy_n(envelope.points, envelope.c_forward + envelope.c_release, p_points);
    }
}

void SetDistortion(const OrbisNgs2SamplerVoiceDistortionParam* p_param, DistortionParams* p_out) {
    *p_out = {p_param->flags,
              std::clamp(p_param->a, 0.0f, 10.0f),
              std::clamp(p_param->b, 0.0f, 10.0f),
              std::clamp(p_param->clip, 0.0f, 1.0f),
              std::clamp(p_param->gate, 0.0f, 1.0f),
              std::clamp(p_param->wetLevel, 0.0f, 32.0f),
              std::clamp(p_param->dryLevel, 0.0f, 32.0f),
              true};
}

void ReadDistortion(const DistortionParams& distortion, OrbisNgs2SamplerVoiceDistortionParam* p_param) {
    p_param->flags = distortion.flags;
    p_param->a = distortion.a;
    p_param->b = distortion.b;
    p_param->clip = distortion.clip;
    p_param->gate = distortion.gate;
    p_param->wetLevel = distortion.wet;
    p_param->dryLevel = distortion.dry;
}

void SetUserFx(const OrbisNgs2SamplerVoiceUserFxParam* p_param, UserFxParams* p_out) {
    *p_out = {p_param->handler, {p_param->userData0, p_param->userData1, p_param->userData2}, true};
}

void ReadUserFx(const UserFxParams& user_fx, OrbisNgs2SamplerVoiceUserFxParam* p_param) {
    p_param->handler = user_fx.pfn_handler;
    p_param->userData0 = user_fx.user_data[0];
    p_param->userData1 = user_fx.user_data[1];
    p_param->userData2 = user_fx.user_data[2];
}

s32 SetPeakMeter(OrbisNgs2Handle h_voice, const OrbisNgs2SamplerVoicePeakMeterParam* p_param, u32* p_out_mode) {
    if (p_param->enableFlag > kMaxPeakMeterMode) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_PEAKMETER_FLAG, "Invalid peak meter mode ({})",
                    p_param->enableFlag);
    }
    *p_out_mode = p_param->enableFlag;
    return ORBIS_OK;
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
                                       {},
                                       true};
    return ORBIS_OK;
}

s32 ReadFilter(OrbisNgs2Handle h_voice, const Voice& voice, OrbisNgs2SamplerVoiceFilterParam* p_param) {
    if (p_param->index >= voice.p_rack->limits.max_filters) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_FILTER_INDEX, "Invalid filter index ({})", p_param->index);
    }
    const FilterParams& filter = voice.p_filters[p_param->index];
    p_param->location = filter.location;
    p_param->type = filter.type;
    p_param->channelMask = filter.bypass_mask;
    p_param->param.direct = {filter.f[0], filter.f[1], filter.f[2], filter.f[3], filter.f[4]};
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
