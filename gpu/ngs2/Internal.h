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
#include <utility>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_report.h"

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

struct CustomModule {
    u32 module_id;
    u32 source_buffer_id;
    u32 extra_buffer_id;
    u32 dest_buffer_id;
    u32 off_state;
    u32 cb_state;
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

struct VoiceMatrix {
    u32 c_levels;
    float levels[kMaxMatrixLevels];
};
static_assert(sizeof(VoiceMatrix) == 0x104);

struct VoicePort {
    OrbisNgs2Handle h_dest;
    u32 dest_rack_uid;
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
    VOICE_INITIAL_STATE = 0x20000000,
    VOICE_BUSY = 0x80000000,
};

struct Voice {
    HandleBlock handle;
    Rack* p_rack;
    std::atomic<u32> control;
    bool is_dirty;
    OrbisNgs2VoiceCallbackHandler pfn_callback;
    uintptr_t callback_data;
    u32 callback_flags;
    VoiceMatrix* p_matrices;
    VoicePort* p_ports;
};

// Holds the voice's busy bit, the firmware's per-voice spinlock, for a whole VoiceControl chain or
// a state read.
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
};

// sceNgs2SystemGetInfo stateFlags (spec 01 §5).
enum SystemStateBits : u32 {
    SYSTEM_STATE_VALID = 0x1,
    SYSTEM_STATE_RENDERING = 0x2,
    SYSTEM_STATE_CYCLE = 0x4,
    SYSTEM_STATE_LOCKED = 0x8,
    SYSTEM_STATE_RENDER_ERROR = 0x80000000,
};

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

s32 PS4_SYSV_ABI sceNgs2ReportRegisterHandler(u32 report_type, OrbisNgs2ReportHandler pfn_handler,
                                              uintptr_t user_data, OrbisNgs2Handle* p_out_handle);
s32 PS4_SYSV_ABI sceNgs2ReportUnregisterHandler(OrbisNgs2Handle h_report);

} // namespace Libraries::Ngs2
