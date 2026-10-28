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

// A sampler voice's waveform block queue (spec 03 §5.1, 05 §3): the game appends under the voice
// lock, the render reads the first c_committed entries (those the last flush let through) and drops
// the ones it finishes.
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
    bool is_notified;
};

struct WaveformQueue {
    WaveformBlock* p_blocks;
    u32 c_capacity;
    u32 i_head;
    u32 c_blocks;
    u32 c_committed;
    u32 c_exit_requests;
    bool is_terminated;
    bool is_terminated_committed;
    bool was_reset;
};

// The sampler source's progress through its queue and its resampler (spec 05 §1, §9).
struct SourceState {
    u64 c_decoded_samples;
    u64 cb_decoded;
    u64 block_user_data;
    const u8* p_read;
    u32 cb_consumed;
    u32 i_sample;
    u32 c_exit_consumed;
    u32 c_repeats;
    u32 num_skip_samples;
    u32 num_samples;
    u32 num_repeats;
    u32 step;
    u32 fraction;
    u32 c_history;
    bool has_block;
    bool is_end;
    bool is_starved;
    float history[kMaxChannels][kResampleHistory];
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
