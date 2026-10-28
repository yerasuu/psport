// SPDX-License-Identifier: GPL-2.0-or-later
// The reverb, module 0x1b (phase F5, kb #1097), written from firmware 11.00's 0x22630 (getter), 0x22750
// (setter), 0x23110 (process) and their helpers as rea read them (spec 07, 06-tablas §E): a reflection
// line with five taps, sixteen damped combs fed by its late tap, four all-passes per output channel, the
// room's high-frequency filter and the dry/wet mix. The reverb rack 0x2001 is a voice around this module;
// custom racks reach it as module 0x1b (F6). Nothing here allocates: the delay lines live in the rack's
// buffer, sized by GetReverbSizes.
#pragma once

#include <type_traits>

#include "Dsp.h"
#include "core/libraries/ngs2/ngs2_reverb.h"

namespace Libraries::Ngs2 {

constexpr u32 kReverbTaps = 5;
constexpr u32 kReverbCombs = 16;
constexpr u32 kReverbAllpasses = 4;

enum ReverbMode : u32 {
    REVERB_I3DL2 = 0,
    REVERB_EXPLICIT = 1,
};

// The game's explicit network parameter (sub 1 of the module, 0x20010002 on the rack; spec 07 §2.2),
// which shadPS4's headers do not describe. What the DSP does with unread[] (game offsets 0xa0 to 0xb8)
// is not read yet.
struct ReverbExplicitParam {
    OrbisNgs2VoiceParamHeader header;
    float wet;
    float dry;
    float tap_delays_ms[kReverbTaps];
    float tap_gains[kReverbTaps];
    u32 tap_mask;
    float comb_delays_ms[kReverbCombs];
    u32 comb_mask;
    float feedback;
    float damping;
    u32 allpass_mask;
    float allpass_delays_ms[kReverbAllpasses];
    float allpass_gain;
    float unread[7];
};

// The module's parameter block (the firmware's 0x150 bytes) in psport's layout: the I3DL2 values
// converted as the setter stores them (levels linear, the -24 dB of reverb included), the explicit
// network, and the per-channel dry and wet levels both modes fill.
struct ReverbParams {
    u32 mode;
    float wet;
    float dry;
    // The setter's other copies of decayTime and decayHFRatio (+0x00, +0x04), clamped to [0.001, 20] and
    // [0, 1]; nothing rea read uses them.
    float decay_time_copy_s;
    float decay_hf_ratio_copy;
    float room;
    float room_hf;
    u32 reflection_pattern;
    float decay_time_s;
    float decay_hf_ratio;
    float reflections;
    float reflections_delay_s;
    float reverb;
    float reverb_delay_s;
    float diffusion;
    float density;
    float hf_reference_hz;
    float dry_levels[kMaxChannels];
    float wet_levels[kMaxChannels];
    u32 tap_mask;
    float tap_delays_ms[kReverbTaps];
    float tap_gains[kReverbTaps];
    u32 comb_mask;
    float comb_delays_ms[kReverbCombs];
    float feedback;
    float damping;
    u32 allpass_mask;
    float allpass_delays_ms[kReverbAllpasses];
    float allpass_gain;
    float unread[7];
};

// Firmware 11.00's tables (06-tablas §E): the combs' lengths at full rate (row 1 at 0x3a750) and the
// all-passes' (0x3a490), in ms.
inline constexpr float kReverbCombLengthsMs[kReverbCombs] = {22.01f, 23.25f, 24.75f, 26.6f,  28.25f, 29.63f,
                                                             31.06f, 32.44f, 33.69f, 34.95f, 36.29f, 37.65f,
                                                             38.99f, 40.35f, 41.67f, 43.05f};
inline constexpr float kReverbAllpassLengthsMs[kReverbAllpasses] = {11.58f, 9.19f, 7.1f, 4.69f};

// The samples a line of ms takes in the instance's memory, which the firmware sizes at 48 kHz:
// align4((int)(ms * 48 + 0.5)).
u32 GetReverbSlotSamples(float ms);

// What an instance's delay lines hold (spec 07 §1): the reflection line's longest delay Lr (its ring
// takes Lr + 1 floats), the floats of every comb, and those of one output channel's all-passes.
struct ReverbSizes {
    u32 max_line_delay;
    u32 c_comb_floats;
    u32 c_allpass_floats;
};

// reverb_size is the rack option's reverbSize (0 or 1).
ReverbSizes GetReverbSizes(u32 reverb_size);

struct ReverbTapSet {
    u32 delays[kReverbTaps];
    float gains[kReverbTaps];
    u32 late_mask;
};

// One instance's running state and the configuration its parameters and the sample rate settle
// (0x282e0, 0x293a0, 0x292d0, 0x27eb0). p_line, p_combs and p_allpasses point into the rack's buffer.
struct ReverbState {
    float* p_line;
    float* p_combs;
    float* p_allpasses;
    ReverbSizes sizes;
    u32 c_allpass_channels;
    bool is_configured;
    bool is_fading;
    u32 configured_rate_hz;
    float configured_density;
    ReverbTapSet taps;
    ReverbTapSet previous_taps;
    u32 comb_mask;
    u32 comb_offsets[kReverbCombs];
    u32 comb_lengths[kReverbCombs];
    float comb_gains[kReverbCombs];
    float comb_damping[kReverbCombs];
    float comb_direct[kReverbCombs];
    float comb_lowpass[kReverbCombs];
    bool has_prefilter;
    float prefilter_k0;
    float prefilter_k1;
    float channel_comb_gains[kMaxChannels][kReverbCombs];
    u32 allpass_mask;
    u32 allpass_offsets[kReverbAllpasses];
    u32 allpass_lengths[kMaxChannels][kReverbAllpasses];
    float allpass_gain;
    bool has_output_filter;
    float filter_pole;
    float previous_dry[kMaxChannels];
    float previous_wet[kMaxChannels];
    u32 i_line;
    u32 comb_index[kReverbCombs];
    float comb_state[kReverbCombs];
    float prefilter_x1;
    u32 allpass_index[kMaxChannels][kReverbAllpasses];
    float filter_stages[kMaxChannels][2];
};

// What the setup 0x20010000 applies (spec 03 §5.3, defaults at 0x335a8).
void SetReverbDefaults(ReverbParams* p_params);
void SetReverbI3dl2(const OrbisNgs2ReverbI3DL2Param& i3dl2, ReverbParams* p_params);
// False when the block is not in I3DL2 mode, for which the firmware's read answers -1.
bool ReadReverbI3dl2(const ReverbParams& params, OrbisNgs2ReverbI3DL2Param* p_out_i3dl2);
// Checks the explicit network against the instance's lines and stores it; the error is the firmware's
// 0x804A030A for a delay out of range or lines that do not fit.
s32 SetReverbExplicit(OrbisNgs2Handle h_voice, const ReverbExplicitParam& param, const ReverbSizes& sizes,
                      ReverbParams* p_params);
// The firmware's own layout of the block (0xc001; spec 07 §2.1): writes the fields rea placed into
// p_block, which holds kReverbBlockSize zeroed bytes.
constexpr u32 kReverbBlockSize = 0x150;
void WriteReverbBlock(const ReverbParams& params, u8* p_block);

// Clears the lines and running state, as on the module's first grain after PLAY: the next grain
// configures the instance afresh, with neither a tap fade nor dry/wet ramps.
void ResetReverb(ReverbState* p_state);
// out gets c_out channels of the reverb of in's c_in channels; in and out are distinct planes. The
// configuration is recomputed when are_params_changed or the sample rate moved.
void ProcessReverb(const ReverbParams& params, ReverbState* p_state, const Planes& in, u32 c_in, const Planes& out,
                   u32 c_out, u32 c_samples, u32 rate_hz, bool are_params_changed);

static_assert(std::is_trivially_copyable_v<ReverbExplicitParam> && sizeof(ReverbExplicitParam) == 0xbc);
static_assert(std::is_trivially_copyable_v<ReverbParams>);
static_assert(std::is_trivially_copyable_v<ReverbState>);

} // namespace Libraries::Ngs2
