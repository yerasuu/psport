// SPDX-License-Identifier: GPL-2.0-or-later
// The native Ngs2's objects (kb #625; system, rack and voice API in phase F2, kb #1094), written from
// firmware 11.00's libSceNgs2.sprx as rea read it (spec 01, 02, 03, 08). Every object lives in the
// buffer the game hands to its Create call, laid out by an Arena, and starts with the 0x20-byte
// handle block whose address the game holds as its handle.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_report.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"

#include "Dsp.h"
#include "VoiceData.h"

namespace Libraries::Ngs2 {

// scePthreadMutexUnlock's answer for a mutex the calling thread does not hold, which
// sceNgs2SystemUnlock and sceNgs2RackUnlock return as it comes.
constexpr s32 ORBIS_KERNEL_ERROR_EPERM = static_cast<s32>(0x80020001);

constexpr u32 kMinGrainSamples = 64;
constexpr u32 kMaxGrainSamples = 1024;
constexpr u32 kMaxRenderBuffers = 16;
constexpr u32 kMaxMatrixLevels = 64;
constexpr u32 kReportTypes = 9;
constexpr u32 kReportHandlersPerType = 8;

enum HandleType : u32 {
    HANDLE_SYSTEM = 1,
    HANDLE_RACK = 2,
    HANDLE_VOICE = 4,
    HANDLE_REPORT = 8,
};

// The firmware's handle block (spec 01 §1): a handle is valid while it points at itself, and an API
// call holds a reference on the counter so that Destroy waits for it.
struct HandleBlock {
    HandleBlock* p_self;
    void* p_owner;
    std::atomic<s32>* p_refs;
    u32 type;
    u32 i_voice;
};
static_assert(sizeof(HandleBlock) == 0x20);

void InitHandle(HandleBlock* p_handle, void* p_owner, std::atomic<s32>* p_refs, u32 type, u32 i_voice);
HandleBlock* AcquireHandle(OrbisNgs2Handle h_object, u32 type_mask);
void ReleaseHandle(const HandleBlock* p_handle);
// Waits until no other call holds the object, then invalidates it; false when h_object is not a
// live handle of exactly that type.
bool RetireHandle(OrbisNgs2Handle h_object, u32 type);
s32 InvalidHandleError(OrbisNgs2Handle h_object, u32 type_mask);

class HandleRef {
public:
    HandleRef(OrbisNgs2Handle h_object, u32 type_mask) : m_p_handle{AcquireHandle(h_object, type_mask)} {}
    ~HandleRef() {
        if (m_p_handle) {
            ReleaseHandle(m_p_handle);
        }
    }
    HandleRef(const HandleRef&) = delete;
    HandleRef& operator=(const HandleRef&) = delete;
    explicit operator bool() const {
        return m_p_handle != nullptr;
    }
    template <class T>
    T* Owner() const {
        return static_cast<T*>(m_p_handle->p_owner);
    }
    u32 VoiceIndex() const {
        return m_p_handle->i_voice;
    }

private:
    HandleBlock* m_p_handle;
};

// A recursive lock the game takes and releases through sceNgs2SystemLock/RackLock, so releasing it
// from a thread that does not hold it must fail instead of being undefined as with
// std::recursive_mutex.
class RecursiveLock {
public:
    void Lock();
    bool TryLock();
    bool Unlock();

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread::id m_owner;
    u32 m_c_depth = 0;
};

// The firmware's linear allocator (spec 01 §4.1, shadPS4's StackBuffer): measures when it has no
// buffer, otherwise hands out zeroed blocks of the game's buffer. With option flag bit 31 every
// block is followed by 16 guard bytes chained as {previous guard, previous guard ^ this guard}.
class Arena {
public:
    Arena(void* p_buffer, std::size_t cb_buffer, bool has_guards);
    void* Allocate(std::size_t cb_block, std::size_t cb_align = 8);
    bool HasOverflowed() const {
        return m_has_overflowed;
    }
    std::size_t RequiredSize() const {
        return m_va_next - m_va_base + m_cb_max_align;
    }

private:
    uintptr_t m_va_base;
    uintptr_t m_va_next;
    uintptr_t m_va_end;
    uintptr_t m_va_last_guard = 0;
    std::size_t m_cb_max_align = 8;
    bool m_is_measuring;
    bool m_has_guards;
    bool m_has_overflowed = false;
};

enum RackClass : u32 {
    RACK_CLASS_SOURCE = 0,
    RACK_CLASS_INTERMEDIATE = 1,
    RACK_CLASS_MASTERING = 2,
    RACK_CLASS_COUNT = 3,
};

struct RackKind {
    u32 rack_id;
    RackClass rack_class;
    u32 cb_option;
    u32 cb_voice_state;
    u32 cb_info;
    bool is_custom;
};

// What a custom module's option settles (spec 03 §6, 07 §1): value is the peak meter's blocks or the
// reverb's word at 4; the rest is UserFx2's handlers, block sizes and user data.
struct CustomModuleOption {
    u32 value;
    OrbisNgs2UserFx2SetupHandler pfn_setup;
    OrbisNgs2UserFx2CleanupHandler pfn_cleanup;
    OrbisNgs2UserFx2ControlHandler pfn_control;
    OrbisNgs2UserFx2ProcessHandler pfn_process;
    u64 cb_common;
    u64 cb_param;
    u64 cb_work;
    uintptr_t user_data;
};

struct CustomModule {
    u32 module_id;
    u32 source_buffer_id;
    u32 extra_buffer_id;
    u32 dest_buffer_id;
    u32 off_state;
    u32 cb_state;
    CustomModuleOption option;
};

// What a rack's option settles, defaults applied (spec 02 §2, §4).
struct RackLimits {
    u32 flags;
    u32 max_grain_samples;
    u32 max_voices;
    u32 max_input_delay_blocks;
    u32 max_matrices;
    u32 max_ports;
    u32 max_channel_works;
    u32 max_inputs;
    u32 max_channels;
    u32 cb_voice_state;
    u32 max_codec_caches;
    u32 max_waveform_blocks;
    u32 max_envelope_points;
    u32 max_filters;
    u32 max_atrac9_decoders;
    u32 max_atrac9_channel_works;
    u32 max_ajm_atrac9_decoders;
    u32 num_peak_meter_blocks;
    u32 reverb_size;
    u32 cb_custom_state;
    u32 c_custom_buffers;
    u32 c_custom_modules;
    CustomModule custom_modules[ORBIS_NGS2_CUSTOM_MAX_MODULES];
    u32 custom_port_sources[ORBIS_NGS2_CUSTOM_MAX_PORTS];
};

struct System;
struct Rack;
struct Voice;
struct ReverbParams;
struct ReverbState;
struct CustomRack;
struct CustomVoice;

// One of a sampler rack's ATRAC9 decoders (spec 05 §6, 08 §2): LibAtrac9's state and the planes of the
// frame it decoded last, both in the rack's buffer. The first maxAtrac9Decoders stand for the
// firmware's internal decoders, the maxAjmAtrac9Decoders after them for its AJM instances. PLAY reserves
// one for a voice until it gives its channel works back (spec 10 §5); p_owner is that voice.
struct Atrac9Decoder {
    void* p_state;
    float* p_frame;
    std::atomic<Voice*> p_owner;
    bool is_ajm;
};

struct VoiceMatrix {
    u32 c_levels;
    float levels[kMaxMatrixLevels];
};
static_assert(sizeof(VoiceMatrix) == 0x104);

struct VoicePort {
    OrbisNgs2Handle h_dest;
    u32 dest_rack_uid;
    u32 i_dest_voice;
    u32 i_dest_input;
    u32 delay_samples;
    float volume;
    s32 matrix_id;
    bool is_used;
};

// The voice's control word (spec 03 §1.3, VoiceGetState): the low byte is the state flags the
// game reads, the next two bytes the events waiting for the render's flush.
enum VoiceControlBits : u32 {
    VOICE_STATE_MASK = 0xff,
    VOICE_FLAG_IN_USE = 0x1,
    VOICE_PENDING_START = 0x100,
    VOICE_PENDING_STOP = 0x200,
    VOICE_PENDING_STOP_IMMEDIATE = 0x400,
    VOICE_PENDING_KILL = 0x800,
    VOICE_PENDING_PAUSE = 0x1000,
    VOICE_PENDING_RESUME = 0x2000,
    VOICE_PENDING_EVENTS = 0xffff00,
    VOICE_INITIAL_STATE = 0x20000000,
    VOICE_PENDING_SETUP = 0x40000000,
    VOICE_BUSY = 0x80000000,
};

struct Voice {
    HandleBlock handle;
    Rack* p_rack;
    std::atomic<u32> control;
    std::atomic<bool> is_dirty;
    OrbisNgs2VoiceCallbackHandler pfn_callback;
    uintptr_t callback_data;
    u32 callback_flags;
    VoiceMatrix* p_matrices;
    VoicePort* p_ports;
    // Under the voice lock: what VoiceControl sets and the voice state the game reads.
    u32 c_channel_works;
    // The ATRAC9 decoder and channel works PLAY reserved last; the render decodes with p_atrac9 until the
    // next PLAY, even after the reservation is given back, as the firmware's voice keeps its index.
    Atrac9Decoder* p_atrac9;
    u32 c_atrac9_channel_works;
    VoiceParams* p_params;
    FilterParams* p_filters;
    MasteringMatrices* p_mastering_matrices;
    ReverbParams* p_reverb_params;
    WaveformQueue queue;
    ActiveWaveformQueue active_queue;
    // The sampler's word of the rack's data lock bitmap and this voice's two bits in it.
    std::atomic<u32>* p_data_lock;
    u32 data_lock_shift;
    u8* p_state;
    // The render's own, under the system's process lock.
    VoiceParams* p_active_params;
    FilterParams* p_active_filters;
    MasteringMatrices* p_active_mastering_matrices;
    MasteringMatrices* p_previous_mastering_matrices;
    ReverbParams* p_active_reverb_params;
    ReverbState* p_reverb;
    CustomVoice* p_custom;
    VoiceDsp* p_dsp;
    BiquadState* p_filter_states;
    PeakMeterState meters[2];
    ActivePort* p_active_ports;
    float* p_matrix_levels;
    float* p_previous_matrix_levels;
    bool* p_matrix_is_set;
    float* p_input_ring;
    bool* p_input_started;
    Voice* p_next_active;
    Voice* p_prev_active;
    Voice* p_next_ordered;
    Voice* p_graph_parent;
    GraphCursor graph_cursor;
    OrbisNgs2VoiceCallbackHandler pfn_active_callback;
    uintptr_t active_callback_data;
    u32 active_callback_flags;
    u32 flags;
    u32 grain_events;
    u32 c_in_channels;
    u32 c_out_channels;
    // ModuleBit sets: the modules still waiting for their first grain since PLAY, and the ones whose
    // first grain this is.
    u32 first_grain_modules;
    u32 first_active_modules;
    GraphMark graph_mark;
    bool is_active;
    bool is_first_grain;
    bool are_params_changed;
    bool is_in_cycle;
};

struct QueuedCallback {
    OrbisNgs2VoiceCallbackHandler pfn_handler;
    OrbisNgs2VoiceCallbackInfo info;
};

// The rack queues these by copy.
static_assert(std::is_trivially_copyable_v<QueuedCallback> && sizeof(QueuedCallback) == 0x40);

// The voice's busy bit, the firmware's per-voice spinlock.
void LockVoice(Voice* p_voice);
void UnlockVoice(Voice* p_voice);

// Holds the voice's busy bit for a whole VoiceControl chain or a state read.
class VoiceLock {
public:
    explicit VoiceLock(Voice* p_voice);
    ~VoiceLock();
    VoiceLock(const VoiceLock&) = delete;
    VoiceLock& operator=(const VoiceLock&) = delete;

private:
    Voice* m_p_voice;
};

struct Rack {
    HandleBlock handle;
    std::atomic<s32> refs;
    const RackKind* p_kind;
    RackLimits limits;
    char name[ORBIS_NGS2_RACK_NAME_LENGTH];
    OrbisNgs2ContextBufferInfo buffer_info;
    OrbisNgs2BufferFreeHandler pfn_free;
    uintptr_t user_data;
    std::atomic<System*> p_system;
    Rack* p_next;
    Rack* p_prev;
    u32 uid;
    RecursiveLock flush_lock;
    std::atomic<u32> c_flush_locks;
    Voice* p_voices;
    // Channel works reserved by PLAY (spec 03 §1.3): the rack's pool, and one per input for the
    // racks with inputs.
    std::atomic<u32> c_channel_works_used;
    std::atomic<u32> c_input_works_used[2];
    // maxAtrac9ChannelWorks, the ATRAC9 channels the internal decoders may decode at once.
    std::atomic<u32> c_atrac9_channel_works_used;
    std::atomic<u32> c_active_voices;
    std::atomic<s64> c_renders;
    // The render's own, under the system's process lock.
    u32 c_input_blocks;
    u32 i_input_block;
    Voice* p_first_active;
    Voice* p_last_active;
    float* p_decode_scratch;
    u32 c_decode_stride;
    Atrac9Decoder* p_atrac9_decoders;
    u32 c_atrac9_decoders;
    QueuedCallback* p_callbacks;
    u32 c_callbacks;
    u32 c_callback_capacity;
    CustomRack* p_custom;
};

struct System {
    HandleBlock handle;
    std::atomic<s32> refs;
    char name[ORBIS_NGS2_SYSTEM_NAME_LENGTH];
    OrbisNgs2ContextBufferInfo buffer_info;
    OrbisNgs2BufferFreeHandler pfn_free;
    std::atomic<uintptr_t> user_data;
    RecursiveLock flush_lock;
    std::recursive_mutex process_lock;
    std::atomic<std::thread::id> tid_render;
    std::atomic<u32> c_flush_locks;
    std::atomic<u32> state_flags;
    std::atomic<u32> num_grain_samples;
    std::atomic<u32> sample_rate_hz;
    std::atomic<u32> max_grain_samples;
    std::atomic<u32> c_racks;
    std::atomic<float> last_render_ratio;
    std::atomic<s64> last_render_tick_us;
    std::atomic<s64> c_renders;
    u32 max_grain_option;
    u32 uid;
    u32 next_rack_uid;
    Rack* p_first_rack[RACK_CLASS_COUNT];
    Rack* p_last_rack[RACK_CLASS_COUNT];
    System* p_next;
    System* p_prev;
    void* p_grain_work;
    // The class 1 and 2 voices in graph order (spec 04 §1.3), rebuilt when the topology changes.
    Voice* p_first_ordered;
    bool is_topology_dirty;
};

// sceNgs2SystemGetInfo stateFlags (spec 01 §5).
enum SystemStateBits : u32 {
    SYSTEM_STATE_VALID = 0x1,
    SYSTEM_STATE_RENDERING = 0x2,
    SYSTEM_STATE_CYCLE = 0x4,
    SYSTEM_STATE_LOCKED = 0x8,
    SYSTEM_STATE_RENDER_ERROR = 0x80000000,
};

// The waveform types a sampler setup accepts (spec 05 §2, 03 §5.1): linear PCM, VAG, ATRAC9 and the
// generators, which read no data.
enum WaveformType : u32 {
    WAVEFORM_FIRST_PCM = 0x10,
    WAVEFORM_LAST_PCM = 0x1b,
    WAVEFORM_VAG = 0x1c,
    WAVEFORM_ATRAC9 = 0x40,
    WAVEFORM_FIRST_GENERATOR = 0x80,
    WAVEFORM_LAST_GENERATOR = 0x87,
};

constexpr u32 kVagFrameSamples = 28;
constexpr u32 kVagBlockBytes = 16;
constexpr u32 kMaxAtrac9FrameSamples = 256;

bool IsWaveformType(u32 type);
u32 BytesPerSample(u32 waveform_type);

// An ATRAC9 configData (spec 05 §6.1): the four bytes of the codec's header read big-endian.
struct Atrac9Config {
    u32 c_channels;
    u32 cb_frame;
    u32 c_frames_per_superframe;
    u32 c_frame_samples;
};

// False unless the top byte is 0xFE. c_frame_samples is 0 for the sample rate indices the firmware's
// table at 0x35860 leaves out (all but 12, 24 and 48 kHz), c_channels 0 for configurations 6 and 7.
bool ParseAtrac9Config(u32 config_data, Atrac9Config* p_out_config);

// Rack topology, under the system's process lock.
s32 LinkRack(System* p_system, Rack* p_rack);
void UnlinkRack(System* p_system, Rack* p_rack);
const RackKind* FindRackKind(u32 rack_id);
s32 ParseRackOption(const RackKind& kind, const OrbisNgs2RackOption* p_option, RackLimits* p_out_limits);
bool IsRackParameter(const RackKind& kind, u32 id);

bool HasReportHandlers(u32 type);
void EmitErrorReport(OrbisNgs2Handle h_object, s32 code, const std::string& message);

// The firmware's error report (spec 01 §2, 0xbf30): returns code, after passing the message to the
// game's report handlers of type 0 when there are any. The message is only formatted when someone
// reads it.
template <class... Args>
s32 Fail(OrbisNgs2Handle h_object, s32 code, fmt::format_string<Args...> format, Args&&... args) {
    const bool is_reported = HasReportHandlers(0);
    if (!is_reported && !Common::Log::ShouldLog(Common::Log::Level::debug)) {
        return code;
    }
    std::string message = fmt::format(format, std::forward<Args>(args)...);
    LOG_DEBUG(Lib_Ngs2, "{:#x}: {}", static_cast<u32>(code), message);
    if (is_reported) {
        EmitErrorReport(h_object, code, message);
    }
    return code;
}

// Logs the first use of something the native Ngs2 accepts but does not do yet, and counts every use
// in the call report, so that nothing it skips is silent.
void NoteUnimplemented(std::uint64_t* p_c_uses, const char* sz_what);
// Logs the first time the render drops something as the firmware does (a full callback queue, an
// output to a buffer the render was not given), and counts every time in the call report.
void NoteDropped(std::uint64_t* p_c_uses, const char* sz_what);
bool IsCompiledSdkAtLeast(u32 version);

// One sceNgs2SystemRender (spec 04 §1.4): the game's buffers, the grain, and the system's work area
// split into sets of kMaxChannels planes.
struct RenderContext {
    System* p_system;
    const OrbisNgs2RenderBufferInfo* p_buffers;
    u32 c_buffers;
    u32 written_mask;
    u32 c_grain_samples;
    u32 sample_rate_hz;
    float* p_work;
};

enum WorkSet : u32 {
    WORK_VOICE = 0,
    WORK_PORT = 1,
    WORK_MATRIX = 2,
    WORK_SETS = 3,
};
Planes WorkPlanes(const RenderContext& context, WorkSet set);

// The racks' own VoiceControl parameters (spec 03 §5), under the voice lock; the id's family has
// been checked.
s32 ControlSampler(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_param);
s32 ControlSubmixer(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_param);
s32 ControlMastering(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_param);
s32 ControlReverb(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_param);
s32 InvalidControlSize(OrbisNgs2Handle h_voice, const OrbisNgs2VoiceParamHeader* p_param, u32 cb_expected);
s32 InvalidControlId(OrbisNgs2Handle h_voice, const OrbisNgs2VoiceParamHeader* p_param);
// 0xc001 (ModuleRead.cpp), under the voice lock.
void ReadModuleBlock(const Voice& voice, OrbisNgs2VoiceParamHeader* p_header);

// The modules the sampler and the submixer share (ModuleParams.cpp); the submixer's parameters have
// the sampler's layout (Abi.cpp). Each setter marks its module set (ModuleBit); each read of a module
// without a parameter answers zeros.
s32 SetEnvelope(OrbisNgs2Handle h_voice, const Voice& voice, const OrbisNgs2SamplerVoiceEnvelopeParam* p_param);
void ReadEnvelope(const VoiceParams& params, OrbisNgs2SamplerVoiceEnvelopeParam* p_param);
void SetDistortion(const OrbisNgs2SamplerVoiceDistortionParam* p_param, VoiceParams* p_params);
void ReadDistortion(const VoiceParams& params, OrbisNgs2SamplerVoiceDistortionParam* p_param);
void SetUserFx(const OrbisNgs2SamplerVoiceUserFxParam* p_param, VoiceParams* p_params);
void ReadUserFx(const VoiceParams& params, OrbisNgs2SamplerVoiceUserFxParam* p_param);
s32 SetPeakMeter(OrbisNgs2Handle h_voice, const OrbisNgs2SamplerVoicePeakMeterParam* p_param, VoiceParams* p_params);
u32 ReadPeakMeter(const VoiceParams& params);
// Module 0x12 sub 0, the submixer's 0x20000002 and a custom rack's compressor; p_compressor is null
// while the module has no parameter.
void SetCompressor(const OrbisNgs2SubmixerVoiceCompressorParam& param, u32 max_inputs, CompressorParams* p_out);
void ReadCompressor(const CompressorParams* p_compressor, OrbisNgs2SubmixerVoiceCompressorParam* p_param);
// Module 0x19 (0x24020, spec 04 §2.2): calls the game's handler on planes, without the voice lock so
// that the handler may call back into Ngs2; a negative answer sets ERROR and clears PLAYING. module is
// the voice's set_modules bit of the module, whose first grain and change the context's flags carry.
void RunVoiceUserFx(const RenderContext& context, Voice* p_voice, const UserFxParams& user_fx, const Planes& planes,
                    u32 c_channels, u32 module);
s32 SetFilter(OrbisNgs2Handle h_voice, const Voice& voice, const OrbisNgs2SamplerVoiceFilterParam* p_param);
s32 ReadFilter(OrbisNgs2Handle h_voice, const Voice& voice, OrbisNgs2SamplerVoiceFilterParam* p_param);
// Runs the voice's filters that sit at location (spec 06 §4; 2 + p before port p).
void RunFilters(const Voice& voice, u32 location, const Planes& planes, u32 c_channels, u32 c_samples);
bool HasFilterAt(const Voice& voice, u32 location);

// The render's steps (Graph.cpp), under the system's process lock.
void FlushRack(System* p_system, Rack* p_rack, bool can_flush);
void OrderGraph(System* p_system);
void ProcessVoice(RenderContext& context, Voice* p_voice);
void PostRack(Rack* p_rack);
// Drops every reference the render holds to a rack's voices before the rack leaves its system.
void ForgetRackVoices(System* p_system, Rack* p_rack);
// The payload of a sampler's block-end (1) or loop (2) callback (0x273f0, spec 09 §8.3): the slot's
// bytes past the ones already handed back, with the userData of the block being played.
void QueueVoiceCallback(Voice* p_voice, u32 type, const WaveformBlock& slot, u64 user_data, u32 cb_notified,
                        u32 c_repeats);
// Gives a voice's channel works back to its rack's pools; under the voice lock.
void ReleaseChannelWorks(Voice* p_voice);

// The racks' processing of one voice over a grain; *p_event_code is the grain's event code (spec 04
// §2.1), which the envelope may take over.
void ProcessSamplerVoice(RenderContext& context, Voice* p_voice, u32* p_event_code);
void ProcessSubmixerVoice(RenderContext& context, Voice* p_voice, u32* p_event_code);
void ProcessMasteringVoice(RenderContext& context, Voice* p_voice, u32* p_event_code);
void ProcessReverbVoice(RenderContext& context, Voice* p_voice, u32* p_event_code);
// The sampler source (module 0x70) over the voice's c_out_channels planes of out; answers the state
// flags it leaves. A custom sampler runs it into its buffer 0.
u32 RunSamplerSource(RenderContext& context, Voice* p_voice, const Planes& out);
void ResetSource(Voice* p_voice);
// The bytes a rack lays out for one ATRAC9 decoder's state, 16-byte aligned.
std::size_t GetAtrac9StateSize();
// Gives the voice's ATRAC9 decoder and channel works back to their pools; ReleaseChannelWorks does.
void ReleaseAtrac9Decoder(Voice* p_voice);
// The data lock (spec 09 §6.2): once KILL returns, the render no longer reads the voice's waveform,
// which the game may free at once, until a new PLAY reaches the render.
void LockVoiceData(Voice* p_voice);
void UnlockVoiceData(Voice* p_voice);
// The render's side of a sampler voice's queue, at the flush and at the render's post.
void FlushWaveformQueue(Voice* p_voice);
void PostWaveformQueue(Voice* p_voice);

// Ports, inputs and outputs (Mixer.cpp, 0x265a0, 0x26210, 0x27060).
void ApplyEventCode(const Planes& planes, u32 c_channels, u32 c_samples, u32 event_code);
void MixPorts(RenderContext& context, Voice* p_voice, const Planes& planes, u32 c_channels);
// A custom rack's ports (spec 04 §4.3): port p mixes the buffer aPort[p].sourceBufferId of p_buffers
// when its bit is in valid_mask.
void MixCustomPorts(RenderContext& context, Voice* p_voice, const Planes* p_buffers, u32 valid_mask, u32 c_channels);
Planes ReadInput(RenderContext& context, Voice* p_voice, u32 i_input);
void WriteRenderBuffer(RenderContext& context, u32 i_buffer, const Planes& planes, u32 c_channels);
void ExpandVoiceMatrices(Voice* p_voice);
void ResetMasteringMatrices(MasteringMatrices* p_matrices);
// The class of a channel count in the mastering's matrix types (0x14280, spec 09 §4.2): 1, 2 and 8
// channels are classes 0, 1 and 3, every other count the 6-channel class 2.
u32 ChannelClass(u32 c_channels);

s32 PS4_SYSV_ABI sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* p_option,
                                              OrbisNgs2ContextBufferInfo* p_out_info);
s32 PS4_SYSV_ABI sceNgs2SystemCreate(const OrbisNgs2SystemOption* p_option, const OrbisNgs2ContextBufferInfo* p_buffer,
                                     OrbisNgs2Handle* p_out_handle);
s32 PS4_SYSV_ABI sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* p_option,
                                                  const OrbisNgs2BufferAllocator* p_allocator,
                                                  OrbisNgs2Handle* p_out_handle);
s32 PS4_SYSV_ABI sceNgs2SystemDestroy(OrbisNgs2Handle h_system, OrbisNgs2ContextBufferInfo* p_out_info);
s32 PS4_SYSV_ABI sceNgs2SystemLock(OrbisNgs2Handle h_system);
s32 PS4_SYSV_ABI sceNgs2SystemUnlock(OrbisNgs2Handle h_system);
s32 PS4_SYSV_ABI sceNgs2SystemSetUserData(OrbisNgs2Handle h_system, uintptr_t user_data);
s32 PS4_SYSV_ABI sceNgs2SystemGetUserData(OrbisNgs2Handle h_system, uintptr_t* p_out_user_data);
s32 PS4_SYSV_ABI sceNgs2SystemGetInfo(OrbisNgs2Handle h_system, OrbisNgs2SystemInfo* p_out_info, std::size_t cb_info);
s32 PS4_SYSV_ABI sceNgs2SystemSetGrainSamples(OrbisNgs2Handle h_system, u32 num_grain_samples);
s32 PS4_SYSV_ABI sceNgs2SystemSetSampleRate(OrbisNgs2Handle h_system, u32 sample_rate_hz);
s32 PS4_SYSV_ABI sceNgs2SystemRender(OrbisNgs2Handle h_system, const OrbisNgs2RenderBufferInfo* p_buffers,
                                     u32 c_buffers);
s32 PS4_SYSV_ABI sceNgs2SystemEnumHandles(OrbisNgs2Handle* p_out_handles, u32 c_max_handles);
s32 PS4_SYSV_ABI sceNgs2SystemEnumRackHandles(OrbisNgs2Handle h_system, OrbisNgs2Handle* p_out_handles,
                                              u32 c_max_handles);

s32 PS4_SYSV_ABI sceNgs2RackQueryBufferSize(u32 rack_id, const OrbisNgs2RackOption* p_option,
                                            OrbisNgs2ContextBufferInfo* p_out_info);
s32 PS4_SYSV_ABI sceNgs2RackCreate(OrbisNgs2Handle h_system, u32 rack_id, const OrbisNgs2RackOption* p_option,
                                   const OrbisNgs2ContextBufferInfo* p_buffer, OrbisNgs2Handle* p_out_handle);
s32 PS4_SYSV_ABI sceNgs2RackCreateWithAllocator(OrbisNgs2Handle h_system, u32 rack_id,
                                                const OrbisNgs2RackOption* p_option,
                                                const OrbisNgs2BufferAllocator* p_allocator,
                                                OrbisNgs2Handle* p_out_handle);
s32 PS4_SYSV_ABI sceNgs2RackDestroy(OrbisNgs2Handle h_rack, OrbisNgs2ContextBufferInfo* p_out_info);
s32 PS4_SYSV_ABI sceNgs2RackLock(OrbisNgs2Handle h_rack);
s32 PS4_SYSV_ABI sceNgs2RackUnlock(OrbisNgs2Handle h_rack);
s32 PS4_SYSV_ABI sceNgs2RackSetUserData(OrbisNgs2Handle h_rack, uintptr_t user_data);
s32 PS4_SYSV_ABI sceNgs2RackGetUserData(OrbisNgs2Handle h_rack, uintptr_t* p_out_user_data);
s32 PS4_SYSV_ABI sceNgs2RackGetVoiceHandle(OrbisNgs2Handle h_rack, u32 i_voice, OrbisNgs2Handle* p_out_handle);
s32 PS4_SYSV_ABI sceNgs2RackGetInfo(OrbisNgs2Handle h_rack, OrbisNgs2RackInfo* p_out_info, std::size_t cb_info);
s32 PS4_SYSV_ABI sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle h_rack, u32 i_module, OrbisNgs2CustomModuleInfo* p_out_info,
                                                std::size_t cb_info);

s32 PS4_SYSV_ABI sceNgs2VoiceControl(OrbisNgs2Handle h_voice, const OrbisNgs2VoiceParamHeader* p_param);
s32 PS4_SYSV_ABI sceNgs2VoiceGetStateFlags(OrbisNgs2Handle h_voice, u32* p_out_flags);
s32 PS4_SYSV_ABI sceNgs2VoiceGetState(OrbisNgs2Handle h_voice, void* p_out_state, std::size_t cb_state);
s32 PS4_SYSV_ABI sceNgs2VoiceGetOwner(OrbisNgs2Handle h_voice, OrbisNgs2Handle* p_out_rack, u32* p_out_index);
s32 PS4_SYSV_ABI sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle h_voice, u32 matrix_id, OrbisNgs2VoiceMatrixInfo* p_out_info,
                                           std::size_t cb_info);
s32 PS4_SYSV_ABI sceNgs2VoiceGetPortInfo(OrbisNgs2Handle h_voice, u32 i_port, OrbisNgs2VoicePortInfo* p_out_info,
                                         std::size_t cb_info);

s32 PS4_SYSV_ABI sceNgs2ParseWaveformData(const void* p_data, std::size_t cb_data, OrbisNgs2WaveformInfo* p_out_info);
s32 PS4_SYSV_ABI sceNgs2ParseWaveformFile(const char* sz_path, u64 off_file, OrbisNgs2WaveformInfo* p_out_info);
s32 PS4_SYSV_ABI sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler pfn_read, uintptr_t user_data,
                                          OrbisNgs2WaveformInfo* p_out_info);
s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* p_format, u32 i_sample, u32 c_samples,
                                          OrbisNgs2WaveformBlock* p_out_block);
s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* p_format, u32* p_out_cb_frame,
                                             u32* p_out_c_frame_samples, u32* p_out_c_units_per_frame,
                                             u32* p_out_c_delay_samples);

s32 PS4_SYSV_ABI sceNgs2ReportRegisterHandler(u32 report_type, OrbisNgs2ReportHandler pfn_handler,
                                              uintptr_t user_data, OrbisNgs2Handle* p_out_handle);
s32 PS4_SYSV_ABI sceNgs2ReportUnregisterHandler(OrbisNgs2Handle h_report);

} // namespace Libraries::Ngs2
