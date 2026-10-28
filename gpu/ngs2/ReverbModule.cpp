// SPDX-License-Identifier: GPL-2.0-or-later
// The reverb module's parameters (0x22750, spec 07 §1, §2): the I3DL2 values converted as the setter
// stores them and read back by undoing the conversion, the explicit network checked against the
// instance's lines, and the block 0xc001 copies.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "Internal.h"
#include "Reverb.h"

namespace Libraries::Ngs2 {
namespace {

// The reflection line's lengths at 0x3a520; reverbSize picks the last two (0x282c0, spec 02 §4.3).
constexpr u32 kReflectionLineSamples[] = {2400, 4800, 14400};
constexpr u32 kLargeReverbSize = 1;
constexpr s32 kMinMillibels = -10000;
// The setter stores reverb 24 dB below what the game asked for, and the read adds it back.
constexpr s32 kReverbOffsetMillibels = 2400;
// Since SDK 6.00 wet and dry reach 32 (spec 07 §5).
constexpr u32 kWideMixSdkVersion = 0x6000000;
constexpr u32 kPatterns = 16;

// The firmware's clamps keep NaN out of the block: a NaN lands on the lower bound.
float ClampFloat(float value, float min_value, float max_value) {
    return value >= min_value ? std::min(value, max_value) : min_value;
}

// 0x253b0.
float MillibelsToLinear(s32 millibels) {
    return millibels >= kMinMillibels ? std::pow(10.0f, float(millibels) / 2000.0f) : 0.0f;
}

// Rounded away from zero. A level of 0 gives -inf, which x86's truncating conversion turns into
// INT32_MIN; psport answers the same instead of converting out of range.
s32 LinearToMillibels(float level) {
    const float millibels = 2000.0f * std::log10(level);
    const float rounded = millibels + (millibels >= 0.0f ? 0.5f : -0.5f);
    if (!(rounded > -2147483648.0f && rounded < 2147483648.0f)) {
        return INT32_MIN;
    }
    return s32(rounded);
}

void FillChannelLevels(ReverbParams* p_params) {
    std::fill_n(p_params->dry_levels, kMaxChannels, p_params->dry);
    std::fill_n(p_params->wet_levels, kMaxChannels, p_params->wet);
    p_params->wet_levels[kLfeChannel] = 0.0f;
}

s32 OutOfRange(OrbisNgs2Handle h_voice, const char* sz_what, u32 i_item, float value) {
    return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE, "Invalid reverb {} ({}:{})", sz_what, i_item,
                value);
}

// Every masked line must have a delay of at least 0 ms, and together they must fit the memory the
// instance sized at 48 kHz.
s32 CheckLines(OrbisNgs2Handle h_voice, const char* sz_what, const float* p_delays_ms, u32 c_lines, u32 mask,
               u32 c_floats) {
    u64 c_used = 0;
    for (u32 i_line = 0; i_line < c_lines; ++i_line) {
        if (!(mask & (1u << i_line))) {
            continue;
        }
        const float delay_ms = p_delays_ms[i_line];
        if (!(delay_ms >= 0.0f) || delay_ms * 48.0f > float(c_floats)) {
            return OutOfRange(h_voice, sz_what, i_line, delay_ms);
        }
        c_used += GetReverbSlotSamples(delay_ms);
    }
    if (c_used > c_floats) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE, "Invalid reverb {} memory ({}>{})", sz_what,
                    c_used, c_floats);
    }
    return ORBIS_OK;
}

template <class T>
void Put(u8* p_block, u32 off, T value) {
    std::memcpy(p_block + off, &value, sizeof(value));
}

} // namespace

u32 GetReverbSlotSamples(float ms) {
    return (u32(s32(ms * 48.0f + 0.5f)) + 3) & ~3u;
}

ReverbSizes GetReverbSizes(u32 reverb_size) {
    ReverbSizes sizes{kReflectionLineSamples[reverb_size == kLargeReverbSize ? 2 : 1], 0, 0};
    for (const float ms : kReverbCombLengthsMs) {
        sizes.c_comb_floats += GetReverbSlotSamples(ms);
    }
    for (const float ms : kReverbAllpassLengthsMs) {
        sizes.c_allpass_floats += GetReverbSlotSamples(ms);
    }
    return sizes;
}

void SetReverbDefaults(ReverbParams* p_params) {
    OrbisNgs2ReverbI3DL2Param i3dl2{};
    i3dl2.wet = 1.0f;
    i3dl2.dry = 0.0f;
    i3dl2.room = kMinMillibels;
    i3dl2.roomHF = 0;
    i3dl2.reflectionPattern = 0;
    i3dl2.decayTime = 1.0f;
    i3dl2.decayHFRatio = 0.5f;
    i3dl2.reflections = kMinMillibels;
    i3dl2.reflectionsDelay = 0.02f;
    i3dl2.reverb = kMinMillibels;
    i3dl2.reverbDelay = 0.04f;
    i3dl2.diffusion = 100.0f;
    i3dl2.density = 100.0f;
    i3dl2.HFReference = 5000.0f;
    SetReverbI3dl2(i3dl2, p_params);
}

void SetReverbI3dl2(const OrbisNgs2ReverbI3DL2Param& i3dl2, ReverbParams* p_params) {
    const float max_mix = IsCompiledSdkAtLeast(kWideMixSdkVersion) ? 32.0f : 1.0f;
    ReverbParams& params = *p_params;
    params.mode = REVERB_I3DL2;
    params.wet = ClampFloat(i3dl2.wet, 0.0f, max_mix);
    params.dry = ClampFloat(i3dl2.dry, 0.0f, max_mix);
    params.room = std::min(1.0f, MillibelsToLinear(i3dl2.room));
    params.room_hf = MillibelsToLinear(std::clamp(i3dl2.roomHF, kMinMillibels, 0));
    params.reflection_pattern = i3dl2.reflectionPattern < kPatterns ? i3dl2.reflectionPattern : 0;
    params.decay_time_copy_s = ClampFloat(i3dl2.decayTime, 0.001f, 20.0f);
    params.decay_time_s = ClampFloat(i3dl2.decayTime, 0.1f, 20.0f);
    params.decay_hf_ratio_copy = ClampFloat(i3dl2.decayHFRatio, 0.0f, 1.0f);
    params.decay_hf_ratio = ClampFloat(i3dl2.decayHFRatio, 0.01f, 2.0f);
    params.reflections = MillibelsToLinear(std::clamp(i3dl2.reflections, kMinMillibels, 1000));
    // NOTE: the spec gives only the upper bound 0.3 s; a negative delay would read the line ahead of
    // its writer, so psport keeps 0 as the lower one, as for reverbDelay.
    params.reflections_delay_s = ClampFloat(i3dl2.reflectionsDelay, 0.0f, 0.3f);
    params.reverb = MillibelsToLinear(std::clamp(i3dl2.reverb, kMinMillibels, 2000) - kReverbOffsetMillibels);
    params.reverb_delay_s = ClampFloat(i3dl2.reverbDelay, 0.0f, 0.1f);
    params.diffusion = ClampFloat(i3dl2.diffusion, 0.0f, 100.0f);
    params.density = ClampFloat(i3dl2.density, 0.0f, 100.0f);
    params.hf_reference_hz = ClampFloat(i3dl2.HFReference, 20.0f, 20000.0f);
    FillChannelLevels(&params);
}

bool ReadReverbI3dl2(const ReverbParams& params, OrbisNgs2ReverbI3DL2Param* p_out_i3dl2) {
    if (params.mode != REVERB_I3DL2) {
        return false;
    }
    OrbisNgs2ReverbI3DL2Param& i3dl2 = *p_out_i3dl2;
    i3dl2 = {};
    i3dl2.wet = params.wet;
    i3dl2.dry = params.dry;
    i3dl2.room = LinearToMillibels(params.room);
    i3dl2.roomHF = LinearToMillibels(params.room_hf);
    i3dl2.reflectionPattern = params.reflection_pattern;
    i3dl2.decayTime = params.decay_time_s;
    i3dl2.decayHFRatio = params.decay_hf_ratio;
    i3dl2.reflections = LinearToMillibels(params.reflections);
    i3dl2.reflectionsDelay = params.reflections_delay_s;
    i3dl2.reverb = LinearToMillibels(params.reverb) + kReverbOffsetMillibels;
    i3dl2.reverbDelay = params.reverb_delay_s;
    i3dl2.diffusion = params.diffusion;
    i3dl2.density = params.density;
    i3dl2.HFReference = params.hf_reference_hz;
    return true;
}

s32 SetReverbExplicit(OrbisNgs2Handle h_voice, const ReverbExplicitParam& param, const ReverbSizes& sizes,
                      ReverbParams* p_params) {
    const u32 tap_mask = param.tap_mask & ((1u << kReverbTaps) - 1);
    for (u32 i_tap = 0; i_tap < kReverbTaps; ++i_tap) {
        const float delay = param.tap_delays_ms[i_tap];
        if ((tap_mask & (1u << i_tap)) && !(delay >= 0.0f && delay <= float(sizes.max_line_delay))) {
            return OutOfRange(h_voice, "tap delay", i_tap, delay);
        }
    }
    const u32 comb_mask = param.comb_mask & ((1u << kReverbCombs) - 1);
    if (const s32 result =
            CheckLines(h_voice, "comb delay", param.comb_delays_ms, kReverbCombs, comb_mask, sizes.c_comb_floats);
        result < 0) {
        return result;
    }
    const u32 allpass_mask = param.allpass_mask & ((1u << kReverbAllpasses) - 1);
    if (const s32 result = CheckLines(h_voice, "all-pass delay", param.allpass_delays_ms, kReverbAllpasses,
                                      allpass_mask, sizes.c_allpass_floats);
        result < 0) {
        return result;
    }
    ReverbParams& params = *p_params;
    params.mode = REVERB_EXPLICIT;
    params.wet = ClampFloat(param.wet, 0.0f, 1.0f);
    params.dry = ClampFloat(param.dry, 0.0f, 1.0f);
    params.tap_mask = tap_mask;
    std::copy_n(param.tap_delays_ms, kReverbTaps, params.tap_delays_ms);
    std::transform(param.tap_gains, param.tap_gains + kReverbTaps, params.tap_gains,
                   [](float gain) { return ClampFloat(gain, 0.0f, 1.0f); });
    params.comb_mask = comb_mask;
    std::copy_n(param.comb_delays_ms, kReverbCombs, params.comb_delays_ms);
    params.feedback = ClampFloat(param.feedback, 0.0f, 1.0f);
    params.damping = ClampFloat(param.damping, 0.0f, 1.0f);
    params.allpass_mask = allpass_mask;
    std::copy_n(param.allpass_delays_ms, kReverbAllpasses, params.allpass_delays_ms);
    params.allpass_gain = ClampFloat(param.allpass_gain, 0.0f, 1.0f);
    // Offsets 0xa0, 0xa4, 0xb0 and 0xb8 are clamped to [0, 1], 0xa8, 0xac and 0xb4 kept as they come.
    constexpr bool kIsUnitRange[] = {true, true, false, false, true, false, true};
    for (u32 i_field = 0; i_field < std::size(params.unread); ++i_field) {
        params.unread[i_field] = kIsUnitRange[i_field] ? ClampFloat(param.unread[i_field], 0.0f, 1.0f)
                                                       : param.unread[i_field];
    }
    // NOTE: the firmware fills the per-channel levels from dry and wet and treats channel 3 apart, but
    // the order of its lanes is not read reliably yet (spec 07 §2.2); psport fills them as I3DL2 does.
    FillChannelLevels(&params);
    return ORBIS_OK;
}

// Offsets rea placed (spec 07 §2.1); the rest of the block, the explicit network's fields and the output
// filter's descriptor at +0x130 among them, is not read yet and stays zero.
void WriteReverbBlock(const ReverbParams& params, u8* p_block) {
    Put(p_block, 0x00, params.decay_time_copy_s);
    Put(p_block, 0x04, params.decay_hf_ratio_copy);
    Put(p_block, 0x08, params.mode);
    Put(p_block, 0x0c, params.wet);
    Put(p_block, 0x10, params.dry);
    Put(p_block, 0x14, params.room);
    Put(p_block, 0x18, params.room_hf);
    Put(p_block, 0x1c, params.reflection_pattern);
    Put(p_block, 0x20, params.decay_time_s);
    Put(p_block, 0x24, params.decay_hf_ratio);
    Put(p_block, 0x28, params.reflections);
    Put(p_block, 0x2c, params.reflections_delay_s);
    Put(p_block, 0x30, params.reverb);
    Put(p_block, 0x34, params.reverb_delay_s);
    Put(p_block, 0x38, params.diffusion);
    Put(p_block, 0x3c, params.density);
    Put(p_block, 0x40, params.hf_reference_hz);
    std::memcpy(p_block + 0xd0, params.dry_levels, sizeof(params.dry_levels));
    std::memcpy(p_block + 0xf0, params.wet_levels, sizeof(params.wet_levels));
}

} // namespace Libraries::Ngs2
