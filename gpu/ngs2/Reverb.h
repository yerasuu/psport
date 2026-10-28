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

// The game's explicit network parameter (sub 1 of the module, 0x20010002 on the rack; spec 07 §2.2, spec 11
// §6), which shadPS4's headers do not describe. The level names are inferred from what the process does
// with them (no public header): the first scales the input and the second the reverb, except on the LFE.
// The setter stores unread[] (game offsets 0xa0 to 0xb8), but nothing in the process reads it.
struct ReverbExplicitParam {
    OrbisNgs2VoiceParamHeader header;
    float dry;
    float wet;
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

constexpr u32 kReverbBlockSize = 0x150;

// The module's parameter block in psport's layout: the I3DL2 values converted as the setter stores them
// (levels linear, the -24 dB of reverb included), the explicit network, and the per-channel dry and wet
// levels both modes fill; with the firmware's own 0x150 bytes, which 0xc001 copies.
struct ReverbParams {
    u32 mode;
    float wet;
    float dry;
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
    // As the game gave it: the block's +0x3c, which the process also reads as I3DL2's density.
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
    // The block as the setter 0x22750 leaves it (spec 11 §8.2): each mode writes its own fields over what
    // the other left, and the I3DL2 one also sets bit 3 of +0x132 (the LFE bypasses the output filter),
    // which nothing clears. The fields the process writes into the active block are not here, since
    // 0xc001 reads the pending one.
    u8 block[kReverbBlockSize];
};

// Firmware 11.00's tables (06-tablas §E): the combs' lengths at full rate (row 1 at 0x3a790) and the
// all-passes' (0x3a490), in ms.
inline constexpr float kReverbCombLengthsMs[kReverbCombs] = {22.01f, 23.25f, 24.75f, 26.6f,  28.25f, 29.63f,
                                                             31.06f, 32.44f, 33.69f, 34.95f, 36.29f, 37.65f,
                                                             38.99f, 40.35f, 41.67f, 43.05f};
inline constexpr float kReverbAllpassLengthsMs[kReverbAllpasses] = {11.58f, 9.19f, 7.1f, 4.69f};

// The samples a line of ms takes in the instance's memory, which the firmware sizes at 48 kHz:
// align4((int)(ms * 48 + 0.5)).
u32 GetReverbSlotSamples(float ms);

// What an instance's delay lines hold (spec 07 §1): the reflection line's longest delay Lr (the firmware's
// ring takes Lr floats and reads before it writes; psport's takes Lr + 1 and writes first, the same delays),
// the floats of every comb, and those of one output channel's all-passes.
struct ReverbSizes {
    u32 max_line_delay;
    u32 c_comb_floats;
    u32 c_allpass_floats;
};

// reverb_size is the rack option's reverbSize, which a custom rack's module option holds at 4 (spec 11 §1):
// Lr is 14400 samples when it is 1, 4800 for any other value.
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
    // Reflections and reverb times room, applied at the output without a fade (+0xe5c, +0xe60): 1 after a
    // reset, set only by I3DL2, so that the explicit network keeps the last I3DL2 values.
    float early_gain;
    float late_gain;
    u32 comb_offsets[kReverbCombs];
    // A comb of length 0 is silent (the firmware hangs on one).
    u32 comb_lengths[kReverbCombs];
    float comb_gains[kReverbCombs];
    float comb_damping[kReverbCombs];
    float comb_damping_complement[kReverbCombs];
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
// Checks the explicit network against the instance's lines and stores it as the firmware does: a negative
// delay becomes 0, and only the masked combs' and all-passes' delays are written. The error is the
// firmware's 0x804A030A for a delay past the line or lines that do not fit, and psport's for a NaN delay.
s32 SetReverbExplicit(OrbisNgs2Handle h_voice, const ReverbExplicitParam& param, const ReverbSizes& sizes,
                      ReverbParams* p_params);
// The firmware's own layout of the block (0xc001): p_block takes kReverbBlockSize bytes.
void WriteReverbBlock(const ReverbParams& params, u8* p_block);

// Clears the lines and running state, as on the module's first grain after PLAY: the next grain
// configures the instance afresh, its taps fading in from silence and without dry/wet ramps.
void ResetReverb(ReverbState* p_state);
// out gets c_out channels of the reverb of in's c_in channels; in and out are distinct planes. The
// configuration is recomputed when are_params_changed or the sample rate moved.
void ProcessReverb(const ReverbParams& params, ReverbState* p_state, const Planes& in, u32 c_in, const Planes& out,
                   u32 c_out, u32 c_samples, u32 rate_hz, bool are_params_changed);

static_assert(std::is_trivially_copyable_v<ReverbExplicitParam> && sizeof(ReverbExplicitParam) == 0xbc);
static_assert(std::is_trivially_copyable_v<ReverbParams>);
static_assert(std::is_trivially_copyable_v<ReverbState>);

} // namespace Libraries::Ngs2
