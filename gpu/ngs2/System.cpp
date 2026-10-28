// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2System* (spec 01 §3 to §5) except Render, and the rack topology each system keeps: racks by
// class in creation order, and the grain range they leave the system.
#include <algorithm>
#include <cstring>
#include <new>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

constexpr std::size_t kCbSystemObject = 0x260;
constexpr std::size_t kCbGrainWorkPerSample = 128;
constexpr std::size_t kCbGrainWorkAlign = 0x100;
constexpr u32 kDefaultMaxGrainSamples = 512;
constexpr u32 kDefaultNumGrainSamples = 256;
constexpr u32 kDefaultSampleRateHz = 48000;
static_assert(sizeof(System) <= kCbSystemObject);

struct SystemSettings {
    const char* sz_name;
    bool has_guards;
    u32 max_grain_samples;
    u32 num_grain_samples;
    u32 sample_rate_hz;
};

std::mutex s_systems_mutex;
System* s_p_first_system;
System* s_p_last_system;
// Firmware 11.00 starts its system counter at 1 (.data 0x5402c).
std::atomic<u32> s_next_system_uid{1};

bool IsGrainSize(u32 c_samples) {
    return c_samples >= kMinGrainSamples && c_samples <= kMaxGrainSamples && c_samples % kMinGrainSamples == 0;
}

bool IsSystemSampleRate(u32 rate_hz) {
    constexpr u32 kRates[] = {11025, 12000, 22050, 24000, 44100, 48000, 88200, 96000, 176400, 192000};
    return std::find(std::begin(kRates), std::end(kRates), rate_hz) != std::end(kRates);
}

s32 ParseSystemOption(const OrbisNgs2SystemOption* p_option, SystemSettings* p_out) {
    *p_out = {"", false, kDefaultMaxGrainSamples, kDefaultNumGrainSamples, kDefaultSampleRateHz};
    if (!p_option) {
        return ORBIS_OK;
    }
    if (p_option->size != sizeof(OrbisNgs2SystemOption)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE, "Invalid system option size ({})", p_option->size);
    }
    *p_out = {p_option->name, (p_option->flags & 0x80000000u) != 0, p_option->maxGrainSamples,
              p_option->numGrainSamples, p_option->sampleRate};
    if (!IsGrainSize(p_out->max_grain_samples)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES,
                    "Invalid system option (maxGrainSamples={}:{}~{},x64)", p_out->max_grain_samples,
                    kMinGrainSamples, kMaxGrainSamples);
    }
    if (!IsGrainSize(p_out->num_grain_samples)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES,
                    "Invalid system option (numGrainSamples={}:{}~{},x64)", p_out->num_grain_samples,
                    kMinGrainSamples, kMaxGrainSamples);
    }
    if (!IsSystemSampleRate(p_out->sample_rate_hz)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE,
                    "Invalid system option (sampleRate={}:44.1/48kHz series)", p_out->sample_rate_hz);
    }
    return ORBIS_OK;
}

// The firmware's two blocks (spec 01 §4.1): the 0x260-byte system object and the grain work area,
// 0x10400 bytes in all with the default option.
System* LayOutSystem(Arena* p_arena, const SystemSettings& settings) {
    void* p_object = p_arena->Allocate(kCbSystemObject);
    void* p_grain_work = p_arena->Allocate(settings.max_grain_samples * kCbGrainWorkPerSample, kCbGrainWorkAlign);
    if (!p_object || !p_grain_work) {
        return nullptr;
    }
    auto* p_system = new (p_object) System{};
    p_system->p_grain_work = p_grain_work;
    return p_system;
}

std::size_t MeasureSystem(const SystemSettings& settings) {
    Arena arena{nullptr, 0, settings.has_guards};
    LayOutSystem(&arena, settings);
    return arena.RequiredSize();
}

void CopyName(char (&name)[16], const char* sz_source) {
    // strcpy_s(dst, 16, src) leaves an empty name when the source does not fit.
    const std::size_t cb_name = strnlen(sz_source, sizeof(name));
    if (cb_name < sizeof(name)) {
        std::memcpy(name, sz_source, cb_name + 1);
    } else {
        name[0] = '\0';
    }
}

s32 CreateSystem(const OrbisNgs2SystemOption* p_option, const OrbisNgs2ContextBufferInfo& buffer_info,
                 OrbisNgs2BufferFreeHandler pfn_free, OrbisNgs2Handle* p_out_handle) {
    SystemSettings settings;
    if (const s32 result = ParseSystemOption(p_option, &settings); result < 0) {
        return result;
    }
    const std::size_t cb_required = MeasureSystem(settings);
    if (!buffer_info.hostBuffer) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS, "Invalid system buffer address ({})",
                    buffer_info.hostBuffer);
    }
    if (buffer_info.hostBufferSize < cb_required) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid system buffer size ({}<{}[byte])",
                    buffer_info.hostBufferSize, cb_required);
    }
    Arena arena{buffer_info.hostBuffer, buffer_info.hostBufferSize, settings.has_guards};
    System* p_system = LayOutSystem(&arena, settings);
    if (!p_system) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid system buffer size ({}<{}[byte])",
                    buffer_info.hostBufferSize, cb_required);
    }
    CopyName(p_system->name, settings.sz_name);
    p_system->buffer_info = buffer_info;
    p_system->pfn_free = pfn_free;
    p_system->refs = 1;
    p_system->state_flags = SYSTEM_STATE_VALID;
    p_system->num_grain_samples = settings.num_grain_samples;
    p_system->sample_rate_hz = settings.sample_rate_hz;
    p_system->max_grain_samples = settings.max_grain_samples;
    p_system->max_grain_option = settings.max_grain_samples;
    p_system->uid = s_next_system_uid.fetch_add(1, std::memory_order_relaxed);
    p_system->next_rack_uid = 1;
    InitHandle(&p_system->handle, p_system, &p_system->refs, HANDLE_SYSTEM, 0);
    {
        std::scoped_lock lock{s_systems_mutex};
        p_system->p_prev = s_p_last_system;
        (s_p_last_system ? s_p_last_system->p_next : s_p_first_system) = p_system;
        s_p_last_system = p_system;
    }
    *p_out_handle = reinterpret_cast<OrbisNgs2Handle>(&p_system->handle);
    return ORBIS_OK;
}

void UnlinkSystem(System* p_system) {
    std::scoped_lock lock{s_systems_mutex};
    (p_system->p_prev ? p_system->p_prev->p_next : s_p_first_system) = p_system->p_next;
    (p_system->p_next ? p_system->p_next->p_prev : s_p_last_system) = p_system->p_prev;
}

void UpdateMaxGrain(System* p_system) {
    u32 max_grain_samples = p_system->max_grain_option;
    for (const Rack* p_first : p_system->p_first_rack) {
        for (const Rack* p_rack = p_first; p_rack; p_rack = p_rack->p_next) {
            max_grain_samples = std::min(max_grain_samples, p_rack->limits.max_grain_samples);
        }
    }
    p_system->max_grain_samples = max_grain_samples;
}

void DisconnectPatchesTo(System* p_system, u32 rack_uid) {
    for (Rack* p_first : p_system->p_first_rack) {
        for (Rack* p_rack = p_first; p_rack; p_rack = p_rack->p_next) {
            for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
                Voice* p_voice = &p_rack->p_voices[i_voice];
                VoiceLock lock{p_voice};
                for (u32 i_port = 0; i_port < p_rack->limits.max_ports; ++i_port) {
                    VoicePort& port = p_voice->p_ports[i_port];
                    if (port.h_dest && port.dest_rack_uid == rack_uid) {
                        port.h_dest = 0;
                        port.dest_rack_uid = 0;
                        port.i_dest_input = 0;
                    }
                }
            }
        }
    }
}

} // namespace

s32 LinkRack(System* p_system, Rack* p_rack) {
    const u32 rack_class = p_rack->p_kind->rack_class;
    const u32 max_grain_samples = std::min(p_system->max_grain_samples.load(), p_rack->limits.max_grain_samples);
    const u32 num_grain_samples = p_system->num_grain_samples;
    if (num_grain_samples < kMinGrainSamples || num_grain_samples > max_grain_samples) {
        return Fail(reinterpret_cast<OrbisNgs2Handle>(&p_rack->handle), ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES,
                    "Invalid system grain samples ({})", num_grain_samples);
    }
    p_rack->uid = p_system->next_rack_uid++;
    p_rack->p_prev = p_system->p_last_rack[rack_class];
    (p_rack->p_prev ? p_rack->p_prev->p_next : p_system->p_first_rack[rack_class]) = p_rack;
    p_system->p_last_rack[rack_class] = p_rack;
    p_rack->p_system = p_system;
    p_system->max_grain_samples = max_grain_samples;
    p_system->c_racks.fetch_add(1, std::memory_order_relaxed);
    return ORBIS_OK;
}

void UnlinkRack(System* p_system, Rack* p_rack) {
    const u32 rack_class = p_rack->p_kind->rack_class;
    (p_rack->p_prev ? p_rack->p_prev->p_next : p_system->p_first_rack[rack_class]) = p_rack->p_next;
    (p_rack->p_next ? p_rack->p_next->p_prev : p_system->p_last_rack[rack_class]) = p_rack->p_prev;
    p_rack->p_next = nullptr;
    p_rack->p_prev = nullptr;
    p_rack->p_system = nullptr;
    p_system->c_racks.fetch_sub(1, std::memory_order_relaxed);
    DisconnectPatchesTo(p_system, p_rack->uid);
    UpdateMaxGrain(p_system);
}

s32 PS4_SYSV_ABI sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* p_option,
                                              OrbisNgs2ContextBufferInfo* p_out_info) {
    if (!p_out_info) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid system buffer info ({})",
                    static_cast<void*>(p_out_info));
    }
    SystemSettings settings;
    if (const s32 result = ParseSystemOption(p_option, &settings); result < 0) {
        return result;
    }
    p_out_info->hostBuffer = nullptr;
    p_out_info->hostBufferSize = MeasureSystem(settings);
    std::memset(p_out_info->reserved, 0, sizeof(p_out_info->reserved));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemCreate(const OrbisNgs2SystemOption* p_option, const OrbisNgs2ContextBufferInfo* p_buffer,
                                     OrbisNgs2Handle* p_out_handle) {
    if (!p_buffer) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO, "Invalid system buffer info ({})",
                    static_cast<const void*>(p_buffer));
    }
    if (!p_out_handle) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid system handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    return CreateSystem(p_option, *p_buffer, nullptr, p_out_handle);
}

s32 PS4_SYSV_ABI sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* p_option,
                                                  const OrbisNgs2BufferAllocator* p_allocator,
                                                  OrbisNgs2Handle* p_out_handle) {
    if (!p_allocator || !p_allocator->allocHandler) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR, "Invalid system buffer allocator ({},{})",
                    static_cast<const void*>(p_allocator),
                    reinterpret_cast<void*>(p_allocator ? p_allocator->allocHandler : nullptr));
    }
    if (!p_out_handle) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid system handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    SystemSettings settings;
    if (const s32 result = ParseSystemOption(p_option, &settings); result < 0) {
        return result;
    }
    OrbisNgs2ContextBufferInfo buffer_info{};
    buffer_info.hostBufferSize = MeasureSystem(settings);
    buffer_info.userData = p_allocator->userData;
    if (const s32 result = p_allocator->allocHandler(&buffer_info); result < 0) {
        return result;
    }
    const s32 result = CreateSystem(p_option, buffer_info, p_allocator->freeHandler, p_out_handle);
    if (result < 0 && p_allocator->freeHandler) {
        (void)p_allocator->freeHandler(&buffer_info); // the creation's error is what the game needs
    }
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemDestroy(OrbisNgs2Handle h_system, OrbisNgs2ContextBufferInfo* p_out_info) {
    System* p_system = nullptr;
    {
        HandleRef ref{h_system, HANDLE_SYSTEM};
        if (!ref) {
            return InvalidHandleError(h_system, HANDLE_SYSTEM);
        }
        p_system = ref.Owner<System>();
        if (p_system->tid_render.load() == std::this_thread::get_id()) {
            return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_OPERATION, "The system is still in process ({:#x})",
                        h_system);
        }
    }
    if (!RetireHandle(h_system, HANDLE_SYSTEM)) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    UnlinkSystem(p_system);
    {
        std::scoped_lock lock{p_system->process_lock};
        for (Rack* p_first : p_system->p_first_rack) {
            while (Rack* p_rack = p_first) {
                p_first = p_rack->p_next;
                UnlinkRack(p_system, p_rack);
            }
        }
    }
    // The buffer may go back to the game's allocator below, so nothing in it is read afterwards.
    OrbisNgs2ContextBufferInfo buffer_info = p_system->buffer_info;
    const OrbisNgs2BufferFreeHandler pfn_free = p_system->pfn_free;
    p_system->~System();
    if (pfn_free) {
        (void)pfn_free(&buffer_info); // the firmware ignores what the game's free answers
    }
    if (p_out_info) {
        *p_out_info = buffer_info;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemLock(OrbisNgs2Handle h_system) {
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    auto* p_system = ref.Owner<System>();
    p_system->flush_lock.Lock();
    p_system->c_flush_locks.fetch_add(1, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemUnlock(OrbisNgs2Handle h_system) {
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    auto* p_system = ref.Owner<System>();
    if (!p_system->flush_lock.Unlock()) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    p_system->c_flush_locks.fetch_sub(1, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetUserData(OrbisNgs2Handle h_system, uintptr_t user_data) {
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    ref.Owner<System>()->user_data = user_data;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetUserData(OrbisNgs2Handle h_system, uintptr_t* p_out_user_data) {
    if (!p_out_user_data) {
        return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid system user data address ({})",
                    static_cast<void*>(p_out_user_data));
    }
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        *p_out_user_data = 0;
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    *p_out_user_data = ref.Owner<System>()->user_data;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetInfo(OrbisNgs2Handle h_system, OrbisNgs2SystemInfo* p_out_info, std::size_t cb_info) {
    if (!p_out_info) {
        return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid system info address ({})",
                    static_cast<void*>(p_out_info));
    }
    if (cb_info < sizeof(OrbisNgs2SystemInfo)) {
        return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid system info size ({}!={}[byte])", cb_info,
                    sizeof(OrbisNgs2SystemInfo));
    }
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    const auto* p_system = ref.Owner<System>();
    OrbisNgs2SystemInfo info{};
    std::memcpy(info.name, p_system->name, sizeof(info.name));
    info.systemHandle = h_system;
    info.bufferInfo = p_system->buffer_info;
    info.uid = p_system->uid;
    info.minGrainSamples = kMinGrainSamples;
    info.maxGrainSamples = p_system->max_grain_samples;
    info.stateFlags = p_system->state_flags | (p_system->c_flush_locks ? u32(SYSTEM_STATE_LOCKED) : 0);
    info.rackCount = p_system->c_racks;
    info.lastRenderRatio = p_system->last_render_ratio;
    info.lastRenderTick = p_system->last_render_tick_us;
    info.renderCount = p_system->c_renders;
    info.sampleRate = p_system->sample_rate_hz;
    info.numGrainSamples = p_system->num_grain_samples;
    *p_out_info = info;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetGrainSamples(OrbisNgs2Handle h_system, u32 num_grain_samples) {
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    auto* p_system = ref.Owner<System>();
    std::scoped_lock lock{p_system->process_lock};
    if (num_grain_samples % kMinGrainSamples != 0 || num_grain_samples < kMinGrainSamples ||
        num_grain_samples > p_system->max_grain_samples) {
        return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES, "Invalid system grain samples ({})",
                    num_grain_samples);
    }
    p_system->num_grain_samples = num_grain_samples;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetSampleRate(OrbisNgs2Handle h_system, u32 sample_rate_hz) {
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    if (!IsSystemSampleRate(sample_rate_hz)) {
        return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE, "Invalid system sample rate ({})",
                    sample_rate_hz);
    }
    auto* p_system = ref.Owner<System>();
    std::scoped_lock lock{p_system->process_lock};
    p_system->sample_rate_hz = sample_rate_hz;
    return ORBIS_OK;
}

// Both enumerations answer the total count, not an error code, even past c_max_handles (spec 01 §5).
s32 PS4_SYSV_ABI sceNgs2SystemEnumHandles(OrbisNgs2Handle* p_out_handles, u32 c_max_handles) {
    if (c_max_handles != 0 && !p_out_handles) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid enumerate system handle address ({})",
                    static_cast<void*>(p_out_handles));
    }
    std::scoped_lock lock{s_systems_mutex};
    s32 c_systems = 0;
    for (System* p_system = s_p_first_system; p_system; p_system = p_system->p_next, ++c_systems) {
        if (u32(c_systems) < c_max_handles) {
            p_out_handles[c_systems] = reinterpret_cast<OrbisNgs2Handle>(&p_system->handle);
        }
    }
    return c_systems;
}

s32 PS4_SYSV_ABI sceNgs2SystemEnumRackHandles(OrbisNgs2Handle h_system, OrbisNgs2Handle* p_out_handles,
                                              u32 c_max_handles) {
    if (c_max_handles != 0 && !p_out_handles) {
        return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid enumerate rack handle address ({})",
                    static_cast<void*>(p_out_handles));
    }
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    auto* p_system = ref.Owner<System>();
    std::scoped_lock lock{p_system->process_lock};
    s32 c_racks = 0;
    for (Rack* p_first : p_system->p_first_rack) {
        for (Rack* p_rack = p_first; p_rack; p_rack = p_rack->p_next, ++c_racks) {
            if (u32(c_racks) < c_max_handles) {
                p_out_handles[c_racks] = reinterpret_cast<OrbisNgs2Handle>(&p_rack->handle);
            }
        }
    }
    return c_racks;
}

} // namespace Libraries::Ngs2
