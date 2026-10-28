// SPDX-License-Identifier: GPL-2.0-or-later
// What the native Ngs2 keeps per voice for the render (phase F3, kb #1095; spec 03 §2, 04, 05):
// the parameters VoiceControl sets and the flush hands over, the sampler's waveform queue and source
// progress, the DSP state, and the ports and graph links the render resolves. All of it lives in the
// rack's buffer (Rack.cpp).
#pragma once

#include <type_traits>

#include "Dsp.h"

namespace Libraries::Ngs2 {

struct Rack;
struct Voice;

// The events a flush hands to the grain (spec 04 §2.1).
enum GrainEvent : u32 {
    GRAIN_PLAY = 0x01,
    GRAIN_STOP = 0x02,
    GRAIN_STOP_IMMEDIATE = 0x04,
    GRAIN_KILL = 0x08,
    GRAIN_PAUSE = 0x10,
    GRAIN_RESUME = 0x20,
};

constexpr u32 kResampleHistory = 32;
// maxWaveformBlocks' upper bound for a sampler rack (spec 02 §4).
constexpr u32 kMaxWaveformBlocks = 16;

// The modules of a voice that have a parameter since its last setup (the firmware's control word
// +0x50, spec 09 §10): every setup clears them but for the ones it sets itself. A module without a
// parameter does not run, keeps its state and reads back zeros; each one starts from its reset state
// on its own first grain with a parameter after PLAY.
enum ModuleBit : u32 {
    MODULE_ENVELOPE = 1u << 0,
    MODULE_DISTORTION = 1u << 1,
    MODULE_COMPRESSOR = 1u << 2,
    MODULE_USER_FX = 1u << 3,
    MODULE_METER = 1u << 4,
    MODULE_LFE = 1u << 5,
    MODULE_GAIN = 1u << 6,
    MODULE_OUTPUT = 1u << 7,
    MODULE_FIRST_FILTER = 1u << 16,
};

constexpr u32 FilterModule(u32 i_filter) {
    return MODULE_FIRST_FILTER << i_filter;
}

struct SourceFormat {
    u32 waveform_type;
    u32 c_channels;
    u32 sample_rate_hz;
    u32 config_data;
    u32 off_frame;
    u32 cb_frame_margin;
    u32 flags;
};

// What VoiceControl sets for a voice: the game's copy under the voice lock, and the render's own,
// which the flush refreshes (spec 03 §2), so that SystemLock and RackLock hold changes back.
struct VoiceParams {
    // The rack's default channels until a setup sets the voice's own (spec 02 §5, 03 §5).
    u32 c_in_channels;
    u32 c_out_channels;
    bool is_setup;
    u32 set_modules;
    SourceFormat format;
    float pitch_ratio;
    EnvelopeParams envelope;
    DistortionParams distortion;
    CompressorParams compressor;
    UserFxParams user_fx;
    u32 meter_mode;
    LfeParams lfe;
    float gain_fbw;
    float gain_lfe;
    u32 i_output_buffer;
};

// The mastering's sixteen channel matrices (module 0x17 at indices 0 to 15), one per pair of input
// and output channel classes, rows by input channel.
struct MasteringMatrices {
    float levels[kMasteringMatrixTypes][kMatrixCells];
    bool is_set[kMasteringMatrixTypes];
};

// A slot of a sampler voice's waveform block ring (spec 05 §3.1, 09 §6, §7). Slots stay in the ring
// after the render is done with them: the queue is the c_queued slots before i_write.
struct WaveformBlock {
    const u8* p_data;
    u64 user_data;
    u32 off_data;
    u32 cb_data;
    u32 num_skip_samples;
    u32 num_samples;
    u32 num_repeats;
    bool is_continuation;
    bool is_marked;
};

// The game's side of the ring, under the voice lock (the module 0x70 block and the control word's
// +0x48 to +0x4b): c_valid counts the slots written since the last reset, up to c_capacity, and only
// a reset lowers it. c_queued drops by what the render took at each render's post.
struct WaveformQueue {
    WaveformBlock* p_slots;
    u32 c_capacity;
    u32 i_write;
    u32 c_queued;
    u32 c_valid;
    u32 c_exit_requests;
    bool is_terminated;
    bool was_reset;
};

// The render's copy, which the flush refreshes whole (process state +0x1b to +0x1e).
struct ActiveWaveformQueue {
    WaveformBlock* p_slots;
    u32 i_write;
    u32 c_queued;
    u32 c_popped;
    u32 c_exit_requests;
    bool is_terminated;
};

// The sampler source's progress through its queue and its resampler (0x1e6a0 state, spec 05 §1, §9,
// 09 §7): bytes consumed from the oldest queued slot and how many of them a callback already handed
// back, the sample position in the block's region, and the read position the voice state publishes.
struct SourceState {
    u64 c_decoded_samples;
    u64 cb_decoded;
    u64 block_user_data;
    const u8* p_read;
    u32 cb_consumed;
    u32 cb_notified;
    u32 i_sample;
    u32 c_exit_consumed;
    u32 c_repeats;
    u32 num_skip_samples;
    u32 num_samples;
    u32 num_repeats;
    u32 step;
    u32 fraction;
    u32 c_history;
    bool is_end;
    bool is_starved;
    float history[kMaxChannels][kResampleHistory];
    // The codecs' own (+0x50 on, spec 05 §1, §4, §5): a VAG block's loop start frame and each channel's
    // y[-1] to y[-4], kept across blocks; the generators' phase and noise registers; a decoding error,
    // which silences the voice until it starts again.
    u32 i_vag_loop_frame;
    float vag_history[kMaxChannels][4];
    float phase;
    u32 noise;
    u32 i_noise_period;
    u32 i_noise_bit;
    s32 codec_error;
    // ATRAC9 (spec 05 §6.2): the next frame of the superframe being read and its byte offset, and the
    // samples of the last decoded frame still to hand out, from i_codec_sample to c_codec_samples.
    u32 i_codec_frame;
    u32 off_codec_frame;
    u32 i_codec_sample;
    u32 c_codec_samples;
};

struct VoiceDsp {
    SourceState source;
    EnvelopeState envelope;
    CompressorState compressor;
    LfeState lfe;
    float gains[kMaxChannels];
    float envelope_height;
    float peaks[2][kPeakValues];
};

// A port as the render uses it: the destination voice the flush resolved from the patch, and the
// volume and matrix of the previous grain that the gain ramps start from (spec 04 §3.3).
struct ActivePort {
    Voice* p_dest;
    u32 i_input;
    u32 delay_samples;
    float volume;
    float previous_volume;
    s32 matrix_id;
    s32 previous_matrix_id;
    bool is_fresh;
};

// Where the iterative graph walk stands in its scan for the voices that feed a voice.
struct GraphCursor {
    Rack* p_rack;
    u32 i_voice;
    u32 i_port;
};

enum GraphMark : u8 {
    GRAPH_UNVISITED = 0,
    GRAPH_VISITING = 1,
    GRAPH_DONE = 2,
};

// The flush and the setups copy these whole, and the rack's buffer holds them without constructors.
static_assert(std::is_trivially_copyable_v<SourceFormat> && sizeof(SourceFormat) == 0x1c);
static_assert(std::is_trivially_copyable_v<VoiceParams>);
static_assert(std::is_trivially_copyable_v<MasteringMatrices> && sizeof(MasteringMatrices) == 0x1010);
static_assert(std::is_trivially_copyable_v<WaveformBlock> && sizeof(WaveformBlock) == 0x28);
static_assert(std::is_trivially_copyable_v<SourceState>);
static_assert(std::is_trivially_copyable_v<VoiceDsp>);
static_assert(std::is_trivially_copyable_v<ActivePort> && sizeof(ActivePort) == 0x28);
static_assert(std::is_trivially_copyable_v<GraphCursor> && sizeof(GraphCursor) == 0x10);

} // namespace Libraries::Ngs2
