// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2PanInit and sceNgs2PanGetVolumeMatrix as firmware 11.00 computes them (0x3d60, 0x40c0; rea
// spec 12 §1): speakers on a circle of 360 degrees or 2π, a source panned between the two speakers
// around it with sine weights and blended with its opposite point by `distance`, then folded into
// the matrix format through the fixed port matrices.
#include <bit>
#include <cmath>
#include <cstring>

#include "Internal.h"
#include "Spatial.h"
#include "core/libraries/ngs2/ngs2_error.h"

namespace Libraries::Ngs2 {
namespace {

constexpr float kDegrees = 360.0f;
constexpr float kRadians = std::bit_cast<float>(0x40c90fdbu);
constexpr float kPi = std::bit_cast<float>(0x40490fdbu);
constexpr float kHalfPi = std::bit_cast<float>(0x3fc90fdbu);
constexpr u32 kSpeakerCounts = 0xb4;
constexpr u32 kMatrixFormats = 0x146;
constexpr u32 kMaxMatrixFormat = 8;
constexpr u32 kMaxPanParams = 8;
constexpr u32 kMaxWrapRounds = 1u << 22;

struct DefaultLayout {
    float angles[kMaxChannels];
    u32 c_speakers;
};

// 0x318f0, 0x31968, 0x31940 and 0x31918, in degrees with the closing sentinel; 0 speakers gets the
// 7-speaker layout.
constexpr DefaultLayout kLayout7{{0.0f, 40.0f, 110.0f, 150.0f, 210.0f, 250.0f, 320.0f, 360.0f}, 7};
constexpr DefaultLayout kLayout2{{90.0f, 270.0f, 450.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 2};
constexpr DefaultLayout kLayout4{{40.0f, 110.0f, 250.0f, 320.0f, 400.0f, 0.0f, 0.0f, 0.0f}, 4};
constexpr DefaultLayout kLayout5{{0.0f, 40.0f, 110.0f, 250.0f, 320.0f, 360.0f, 0.0f, 0.0f}, 5};

bool IsSpeakerCount(u32 c_speakers) {
    return c_speakers < 8 && (kSpeakerCounts >> c_speakers & 1) != 0;
}

bool IsUnitAngle(float unit_angle) {
    return unit_angle == kDegrees || unit_angle == kRadians;
}

float MinSs(float a, float b) {
    return a < b ? a : b;
}

float MaxSs(float a, float b) {
    return a > b ? a : b;
}

// 0x3e10: false where the firmware loops forever (an infinite angle, a positive multiple of the unit
// that lands on the unit itself, or an angle the unit no longer moves).
bool WrapSpeakerAngle(float* p_angle, float unit_angle) {
    float angle = *p_angle;
    for (u32 i_round = 0; angle < 0.0f || angle >= unit_angle; ++i_round) {
        const float previous = angle;
        angle = angle < 0.0f ? angle + unit_angle : angle;
        angle = unit_angle < angle ? angle - unit_angle : angle;
        if (std::bit_cast<u32>(angle) == std::bit_cast<u32>(previous) || i_round == kMaxWrapRounds) {
            return false;
        }
    }
    *p_angle = angle;
    return true;
}

// 0x42e0: the source angle into [first speaker, first speaker + unit).
bool WrapSourceAngle(float* p_angle, float first, float top, float unit_angle) {
    float angle = *p_angle;
    for (u32 i_round = 0; first > angle || angle >= top; ++i_round) {
        const float previous = angle;
        if (!(angle < top)) {
            angle = angle - unit_angle;
        }
        angle = angle < first ? unit_angle + angle : angle;
        if (std::bit_cast<u32>(angle) == std::bit_cast<u32>(previous) || i_round == kMaxWrapRounds) {
            return false;
        }
    }
    *p_angle = angle;
    return true;
}

void AddSegmentShare(float previous, float current, float angle, float gain, float* p_levels) {
    if (angle >= previous && current > angle) {
        const float position = (angle - previous) / (current - previous);
        p_levels[0] = std::sin((1.0f - position) * kHalfPi) * gain + p_levels[0];
        p_levels[1] = std::sin(position * kHalfPi) * gain + p_levels[1];
    }
}

// The speakers' shares of the source point and its opposite, segment by segment as the firmware sums
// them; every segment holding a point adds to its two speakers, so unsorted angles may count twice.
// c_speakers is the count already checked, not the work's, which the game may change meanwhile.
void AddPoints(const OrbisNgs2PanWork& work, u32 c_speakers, float near_angle, float near_gain, float far_angle,
               float far_gain, float* p_levels) {
    for (u32 i_speaker = 1; i_speaker <= c_speakers; ++i_speaker) {
        const float previous = work.aSpeakerAngle[i_speaker - 1];
        const float current = work.aSpeakerAngle[i_speaker];
        AddSegmentShare(previous, current, near_angle, near_gain, p_levels + i_speaker - 1);
        AddSegmentShare(previous, current, far_angle, far_gain, p_levels + i_speaker - 1);
    }
}

} // namespace

void NoteDeparture(std::uint64_t* p_c_uses, const char* sz_what) {
    if (__atomic_fetch_add(p_c_uses, 1, __ATOMIC_RELAXED) == 0) {
        Common::Log::Stubbed(sz_what, p_c_uses);
        LOG_WARNING(Lib_Ngs2, "{}: answered instead of the firmware's hang or fault", sz_what);
    }
}

s32 PS4_SYSV_ABI sceNgs2PanInit(OrbisNgs2PanWork* p_work, const float* p_speaker_angles, u32 c_speakers,
                                float unit_angle) {
    if (p_work == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (p_speaker_angles != nullptr) {
        if (!IsUnitAngle(unit_angle)) {
            return ORBIS_NGS2_ERROR_INVALID_PAN_UNIT_ANGLE;
        }
        if (!IsSpeakerCount(c_speakers)) {
            return ORBIS_NGS2_ERROR_INVALID_PAN_SPEAKER;
        }
        p_work->unitAngle = unit_angle;
        float angles[kMaxChannels];
        for (u32 i_speaker = 0; i_speaker < c_speakers; ++i_speaker) {
            angles[i_speaker] = p_speaker_angles[i_speaker];
            if (!WrapSpeakerAngle(&angles[i_speaker], unit_angle)) {
                static std::uint64_t s_c_hangs;
                NoteDeparture(&s_c_hangs, "sceNgs2PanInit with a speaker angle that never wraps");
                return ORBIS_NGS2_ERROR_INVALID_PAN_SPEAKER;
            }
        }
        p_work->numSpeakers = c_speakers;
        // The game's order is L, R[, C], Ls, Rs[, Lb, Rb]; the work's is round the circle from the
        // front speaker, closed by that speaker one unit on.
        float* p_angles = p_work->aSpeakerAngle;
        switch (c_speakers) {
        case 2:
            p_angles[0] = angles[1];
            p_angles[1] = angles[0];
            p_angles[2] = angles[1] + unit_angle;
            break;
        case 4:
            p_angles[0] = angles[1];
            p_angles[1] = angles[3];
            p_angles[2] = angles[2];
            p_angles[3] = angles[0];
            p_angles[4] = angles[1] + unit_angle;
            break;
        case 5:
            p_angles[0] = angles[2];
            p_angles[1] = angles[1];
            p_angles[2] = angles[4];
            p_angles[3] = angles[3];
            p_angles[4] = angles[0];
            p_angles[5] = angles[2] + unit_angle;
            break;
        default:
            p_angles[0] = angles[2];
            p_angles[1] = angles[1];
            p_angles[2] = angles[4];
            p_angles[3] = angles[6];
            p_angles[4] = angles[5];
            p_angles[5] = angles[3];
            p_angles[6] = angles[0];
            p_angles[7] = angles[2] + unit_angle;
            break;
        }
        return ORBIS_OK;
    }

    const DefaultLayout* p_layout = nullptr;
    switch (c_speakers) {
    case 0:
    case 7:
        p_layout = &kLayout7;
        break;
    case 2:
        p_layout = &kLayout2;
        break;
    case 4:
        p_layout = &kLayout4;
        break;
    case 5:
        p_layout = &kLayout5;
        break;
    default:
        return ORBIS_NGS2_ERROR_INVALID_PAN_SPEAKER;
    }
    std::memcpy(p_work->aSpeakerAngle, p_layout->angles, sizeof(p_layout->angles));
    p_work->unitAngle = kDegrees;
    p_work->numSpeakers = p_layout->c_speakers;
    if (unit_angle != kRadians) {
        return ORBIS_OK;
    }
    // The firmware converts the speakers it was asked for, from the game's count, and leaves the
    // closing sentinel in degrees; with 0 speakers nothing is converted at all.
    p_work->unitAngle = kRadians;
    for (u32 i_speaker = 0; i_speaker < c_speakers; ++i_speaker) {
        p_work->aSpeakerAngle[i_speaker] = (p_work->aSpeakerAngle[i_speaker] / 180.0f) * kPi;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2PanGetVolumeMatrix(const OrbisNgs2PanWork* p_work, const OrbisNgs2PanParam* p_params,
                                           u32 c_params, u32 matrix_format, float* p_out_levels) {
    if (p_work == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_PAN_WORK;
    }
    if (p_params == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_PAN_PARAM;
    }
    if (p_out_levels == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (matrix_format > kMaxMatrixFormat || (kMatrixFormats >> matrix_format & 1) == 0) {
        return ORBIS_NGS2_ERROR_INVALID_PAN_MATRIX_FORMAT;
    }
    const u32 c_speakers = p_work->numSpeakers;
    if (!IsSpeakerCount(c_speakers)) {
        return ORBIS_NGS2_ERROR_INVALID_PAN_SPEAKER;
    }
    if (!IsUnitAngle(p_work->unitAngle)) {
        return ORBIS_NGS2_ERROR_INVALID_PAN_UNIT_ANGLE;
    }
    if (c_params > kMaxPanParams) {
        return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
    }
    std::memset(p_out_levels, 0, std::size_t(c_params) * matrix_format * sizeof(float));

    const u32 c_sources = c_speakers == 2 ? 2 : c_speakers == 7 ? 8 : 6;
    const float* p_fold = FixedMatrix(c_sources, 0, matrix_format);
    for (u32 i_param = 0; i_param < c_params; ++i_param) {
        const OrbisNgs2PanParam& param = p_params[i_param];
        const float distance = MaxSs(-1.0f, MinSs(1.0f, param.distance));
        const float gain_near = std::sin((((distance + 1.0f) * 0.5f) * kPi) * 0.5f);
        const float gain_far = std::sin((((1.0f - distance) * 0.5f) * kPi) * 0.5f);

        const float first = p_work->aSpeakerAngle[0];
        const float unit_angle = p_work->unitAngle;
        const float top = first + unit_angle;
        float angle = param.angle;
        if (!WrapSourceAngle(&angle, first, top, unit_angle)) {
            static std::uint64_t s_c_hangs;
            NoteDeparture(&s_c_hangs, "sceNgs2PanGetVolumeMatrix with an angle that never wraps");
            return ORBIS_NGS2_ERROR_INVALID_PAN_PARAM;
        }
        const float opposite = angle + unit_angle * 0.5f;
        const float far_angle = !(top <= opposite) ? opposite : opposite - unit_angle;

        float speakers[kMaxChannels] = {};
        AddPoints(*p_work, c_speakers, angle, gain_near, far_angle, gain_far, speakers);

        float channels[kMaxChannels] = {};
        switch (c_speakers) {
        case 2:
            channels[0] = speakers[1];
            channels[1] = speakers[0] + speakers[2];
            break;
        case 4:
            channels[0] = speakers[3];
            channels[1] = speakers[0] + speakers[4];
            channels[4] = speakers[2];
            channels[5] = speakers[1];
            break;
        case 5:
            channels[0] = speakers[4];
            channels[1] = speakers[1];
            channels[2] = speakers[0] + speakers[5];
            channels[4] = speakers[3];
            channels[5] = speakers[2];
            break;
        default:
            channels[0] = speakers[6];
            channels[1] = speakers[1];
            channels[2] = speakers[0] + speakers[7];
            channels[4] = speakers[5];
            channels[5] = speakers[2];
            channels[6] = speakers[4];
            channels[7] = speakers[3];
            break;
        }
        for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
            channels[i_channel] = i_channel == kLfeChannel ? param.lfeLevel : param.fbwLevel * channels[i_channel];
        }

        for (u32 i_out = 0; i_out < matrix_format; ++i_out) {
            float& level = p_out_levels[std::size_t(i_out) * c_params + i_param];
            for (u32 i_source = 0; i_source < c_sources; ++i_source) {
                level = level + p_fold[i_source * kMaxChannels + i_out] * channels[i_source];
            }
        }
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
