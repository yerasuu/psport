// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2Rack* and sceNgs2CustomRackGetModuleInfo (spec 02 §3, §4). A rack's buffer holds the rack, its
// voices (all of them exist from creation on) and each voice's matrices and ports. QueryBufferSize
// answers what this layout needs: the firmware's own sum also counts the DSP modules' blocks, whose
// sizes are not all read yet (spec 08 §2), so the native size stays below the console's.
#include <algorithm>
#include <cstring>
#include <new>

#include "Internal.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_reverb.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"

namespace Libraries::Ngs2 {
namespace {

Rack* LayOutRack(Arena* p_arena, const RackKind& kind, const RackLimits& limits) {
    const std::size_t c_voices = limits.max_voices;
    void* p_object = p_arena->Allocate(sizeof(Rack), alignof(Rack));
    void* p_voices = p_arena->Allocate(c_voices * sizeof(Voice), alignof(Voice));
    void* p_matrices =
        limits.max_matrices ? p_arena->Allocate(c_voices * limits.max_matrices * sizeof(VoiceMatrix)) : nullptr;
    void* p_ports = limits.max_ports ? p_arena->Allocate(c_voices * limits.max_ports * sizeof(VoicePort)) : nullptr;
    if (!p_object || !p_voices || p_arena->HasOverflowed()) {
        return nullptr;
    }
    auto* p_rack = new (p_object) Rack{};
    p_rack->p_kind = &kind;
    p_rack->limits = limits;
    p_rack->refs = 1;
    p_rack->p_voices = static_cast<Voice*>(p_voices);
    InitHandle(&p_rack->handle, p_rack, &p_rack->refs, HANDLE_RACK, 0);
    for (u32 i_voice = 0; i_voice < limits.max_voices; ++i_voice) {
        auto* p_voice = new (&p_rack->p_voices[i_voice]) Voice{};
        p_voice->p_rack = p_rack;
        p_voice->p_matrices = static_cast<VoiceMatrix*>(p_matrices) + std::size_t(i_voice) * limits.max_matrices;
        p_voice->p_ports = static_cast<VoicePort*>(p_ports) + std::size_t(i_voice) * limits.max_ports;
        InitHandle(&p_voice->handle, p_rack, &p_rack->refs, HANDLE_VOICE, i_voice);
    }
    return p_rack;
}

std::size_t MeasureRack(const RackKind& kind, const RackLimits& limits) {
    Arena arena{nullptr, 0, (limits.flags & 0x80000000u) != 0};
    LayOutRack(&arena, kind, limits);
    return arena.RequiredSize();
}

// Ends a rack the game can no longer reach: its handles are already retired or were never handed out.
void TearDownRack(Rack* p_rack) {
    for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
        Voice* p_voice = &p_rack->p_voices[i_voice];
        std::memset(static_cast<void*>(&p_voice->handle), 0, sizeof(p_voice->handle));
        p_voice->~Voice();
    }
    std::memset(static_cast<void*>(&p_rack->handle), 0, sizeof(p_rack->handle));
    p_rack->~Rack();
}

s32 CreateRack(OrbisNgs2Handle h_system, const RackKind& kind, const OrbisNgs2RackOption* p_option,
               const OrbisNgs2ContextBufferInfo& buffer_info, OrbisNgs2BufferFreeHandler pfn_free,
               OrbisNgs2Handle* p_out_handle) {
    RackLimits limits;
    if (const s32 result = ParseRackOption(kind, p_option, &limits); result < 0) {
        return result;
    }
    const std::size_t cb_required = MeasureRack(kind, limits);
    if (!buffer_info.hostBuffer) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS, "Invalid rack buffer address ({})",
                    buffer_info.hostBuffer);
    }
    if (buffer_info.hostBufferSize < cb_required) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid rack buffer size ({}<{}[byte])",
                    buffer_info.hostBufferSize, cb_required);
    }
    Arena arena{buffer_info.hostBuffer, buffer_info.hostBufferSize, (limits.flags & 0x80000000u) != 0};
    Rack* p_rack = LayOutRack(&arena, kind, limits);
    if (!p_rack) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid rack buffer size ({}<{}[byte])",
                    buffer_info.hostBufferSize, cb_required);
    }
    if (p_option) {
        const std::size_t cb_name = strnlen(p_option->name, sizeof(p_rack->name));
        if (cb_name < sizeof(p_rack->name)) {
            std::memcpy(p_rack->name, p_option->name, cb_name + 1);
        }
    }
    p_rack->buffer_info = buffer_info;
    p_rack->pfn_free = pfn_free;
    // The firmware validates the system handle last, once the rack is built (spec 02 §3).
    HandleRef system_ref{h_system, HANDLE_SYSTEM};
    if (!system_ref) {
        TearDownRack(p_rack);
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    auto* p_system = system_ref.Owner<System>();
    {
        std::scoped_lock lock{p_system->process_lock};
        if (const s32 result = LinkRack(p_system, p_rack); result < 0) {
            TearDownRack(p_rack);
            return result;
        }
    }
    if (limits.c_custom_modules > 0) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "sceNgs2RackCreate: custom rack modules (options unchecked, no DSP until F6)");
    }
    *p_out_handle = reinterpret_cast<OrbisNgs2Handle>(&p_rack->handle);
    return ORBIS_OK;
}

s32 InvalidRackId(u32 rack_id) {
    return Fail(0, ORBIS_NGS2_ERROR_INVALID_RACK_ID, "Invalid rack ID ({:X})", rack_id);
}

void FillCustomInfo(const RackLimits& limits, OrbisNgs2CustomRackInfo* p_out) {
    p_out->stateSize = limits.cb_custom_state;
    p_out->numBuffers = limits.c_custom_buffers;
    p_out->numModules = limits.c_custom_modules;
    p_out->reserved = 0;
    for (u32 i_module = 0; i_module < ORBIS_NGS2_CUSTOM_MAX_MODULES; ++i_module) {
        const CustomModule& module = limits.custom_modules[i_module];
        // The firmware leaves the option pointer and the extra buffer out of this copy (spec 02 §4.5).
        p_out->aModule[i_module] = {nullptr, module.module_id, module.source_buffer_id, 0, module.dest_buffer_id,
                                    module.off_state, module.cb_state, 0, 0};
    }
    for (u32 i_port = 0; i_port < ORBIS_NGS2_CUSTOM_MAX_PORTS; ++i_port) {
        p_out->aPort[i_port] = {limits.custom_port_sources[i_port], 0};
    }
}

// The part after the common 0xa8 bytes, written when the game's buffer holds all of it.
void FillRackKindInfo(const Rack& rack, OrbisNgs2RackInfo* p_out_info, std::size_t cb_info) {
    const RackLimits& limits = rack.limits;
    if (cb_info < rack.p_kind->cb_info) {
        return;
    }
    switch (rack.p_kind->rack_id) {
    case 0x1000: {
        auto* p_out = reinterpret_cast<OrbisNgs2SamplerRackInfo*>(p_out_info);
        p_out->maxChannelWorks = limits.max_channel_works;
        p_out->maxCodecCaches = limits.max_codec_caches;
        p_out->maxWaveformBlocks = limits.max_waveform_blocks;
        p_out->maxEnvelopePoints = limits.max_envelope_points;
        p_out->maxFilters = limits.max_filters;
        p_out->maxAtrac9Decoders = limits.max_atrac9_decoders;
        p_out->maxAtrac9ChannelWorks = limits.max_atrac9_channel_works;
        p_out->maxAjmAtrac9Decoders = limits.max_ajm_atrac9_decoders;
        break;
    }
    case 0x2000: {
        auto* p_out = reinterpret_cast<OrbisNgs2SubmixerRackInfo*>(p_out_info);
        p_out->maxChannels = limits.max_channels;
        p_out->maxEnvelopePoints = limits.max_envelope_points;
        p_out->maxFilters = limits.max_filters;
        p_out->maxInputs = limits.max_inputs;
        break;
    }
    case 0x2001: {
        auto* p_out = reinterpret_cast<OrbisNgs2ReverbRackInfo*>(p_out_info);
        p_out->maxChannels = limits.max_channels;
        p_out->reverbSize = limits.reverb_size;
        break;
    }
    case 0x3000: {
        auto* p_out = reinterpret_cast<OrbisNgs2MasteringRackInfo*>(p_out_info);
        p_out->maxChannels = limits.max_channels;
        p_out->reserved = 0;
        break;
    }
    case 0x4001: {
        auto* p_out = reinterpret_cast<OrbisNgs2CustomSamplerRackInfo*>(p_out_info);
        FillCustomInfo(limits, &p_out->customRackInfo);
        p_out->maxChannelWorks = limits.max_channel_works;
        p_out->maxWaveformBlocks = limits.max_waveform_blocks;
        p_out->maxAtrac9Decoders = limits.max_atrac9_decoders;
        p_out->maxAtrac9ChannelWorks = limits.max_atrac9_channel_works;
        p_out->maxAjmAtrac9Decoders = limits.max_ajm_atrac9_decoders;
        p_out->maxCodecCaches = limits.max_codec_caches;
        break;
    }
    case 0x4002:
    case 0x4003: {
        auto* p_out = reinterpret_cast<OrbisNgs2CustomSubmixerRackInfo*>(p_out_info);
        FillCustomInfo(limits, &p_out->customRackInfo);
        p_out->maxChannels = limits.max_channels;
        p_out->maxInputs = limits.max_inputs;
        break;
    }
    default:
        break;
    }
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2RackQueryBufferSize(u32 rack_id, const OrbisNgs2RackOption* p_option,
                                            OrbisNgs2ContextBufferInfo* p_out_info) {
    const RackKind* p_kind = FindRackKind(rack_id);
    if (!p_kind) {
        return InvalidRackId(rack_id);
    }
    if (!p_out_info) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack buffer info ({})",
                    static_cast<void*>(p_out_info));
    }
    RackLimits limits;
    if (const s32 result = ParseRackOption(*p_kind, p_option, &limits); result < 0) {
        return result;
    }
    p_out_info->hostBuffer = nullptr;
    p_out_info->hostBufferSize = MeasureRack(*p_kind, limits);
    std::memset(p_out_info->reserved, 0, sizeof(p_out_info->reserved));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackCreate(OrbisNgs2Handle h_system, u32 rack_id, const OrbisNgs2RackOption* p_option,
                                   const OrbisNgs2ContextBufferInfo* p_buffer, OrbisNgs2Handle* p_out_handle) {
    const RackKind* p_kind = FindRackKind(rack_id);
    if (!p_kind) {
        return InvalidRackId(rack_id);
    }
    if (!p_buffer) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO, "Invalid rack buffer info ({})",
                    static_cast<const void*>(p_buffer));
    }
    if (!p_out_handle) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    return CreateRack(h_system, *p_kind, p_option, *p_buffer, nullptr, p_out_handle);
}

s32 PS4_SYSV_ABI sceNgs2RackCreateWithAllocator(OrbisNgs2Handle h_system, u32 rack_id,
                                                const OrbisNgs2RackOption* p_option,
                                                const OrbisNgs2BufferAllocator* p_allocator,
                                                OrbisNgs2Handle* p_out_handle) {
    const RackKind* p_kind = FindRackKind(rack_id);
    if (!p_kind) {
        return InvalidRackId(rack_id);
    }
    if (!p_allocator || !p_allocator->allocHandler) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR, "Invalid rack buffer allocator ({},{})",
                    static_cast<const void*>(p_allocator),
                    reinterpret_cast<void*>(p_allocator ? p_allocator->allocHandler : nullptr));
    }
    if (!p_out_handle) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    RackLimits limits;
    if (const s32 result = ParseRackOption(*p_kind, p_option, &limits); result < 0) {
        return result;
    }
    OrbisNgs2ContextBufferInfo buffer_info{};
    buffer_info.hostBufferSize = MeasureRack(*p_kind, limits);
    buffer_info.userData = p_allocator->userData;
    if (const s32 result = p_allocator->allocHandler(&buffer_info); result < 0) {
        return result;
    }
    const s32 result = CreateRack(h_system, *p_kind, p_option, buffer_info, p_allocator->freeHandler, p_out_handle);
    if (result < 0 && p_allocator->freeHandler) {
        (void)p_allocator->freeHandler(&buffer_info); // the creation's error is what the game needs
    }
    return result;
}

s32 PS4_SYSV_ABI sceNgs2RackDestroy(OrbisNgs2Handle h_rack, OrbisNgs2ContextBufferInfo* p_out_info) {
    Rack* p_rack = nullptr;
    {
        HandleRef ref{h_rack, HANDLE_RACK};
        if (!ref) {
            return InvalidHandleError(h_rack, HANDLE_RACK);
        }
        p_rack = ref.Owner<Rack>();
        const System* p_system = p_rack->p_system;
        if (p_system && p_system->tid_render.load() == std::this_thread::get_id()) {
            return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OPERATION, "The rack is still in process ({:#x})", h_rack);
        }
    }
    if (!RetireHandle(h_rack, HANDLE_RACK)) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    if (System* p_system = p_rack->p_system) {
        std::scoped_lock lock{p_system->process_lock};
        if (p_rack->p_system == p_system) {
            UnlinkRack(p_system, p_rack);
        }
    }
    // The buffer may go back to the game's allocator below, so nothing in it is read afterwards.
    OrbisNgs2ContextBufferInfo buffer_info = p_rack->buffer_info;
    const OrbisNgs2BufferFreeHandler pfn_free = p_rack->pfn_free;
    TearDownRack(p_rack);
    if (pfn_free) {
        (void)pfn_free(&buffer_info); // the firmware ignores what the game's free answers
    }
    if (p_out_info) {
        *p_out_info = buffer_info;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackLock(OrbisNgs2Handle h_rack) {
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    auto* p_rack = ref.Owner<Rack>();
    p_rack->flush_lock.Lock();
    p_rack->c_flush_locks.fetch_add(1, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackUnlock(OrbisNgs2Handle h_rack) {
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    auto* p_rack = ref.Owner<Rack>();
    if (!p_rack->flush_lock.Unlock()) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    p_rack->c_flush_locks.fetch_sub(1, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackSetUserData(OrbisNgs2Handle h_rack, uintptr_t user_data) {
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    ref.Owner<Rack>()->user_data = user_data;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetUserData(OrbisNgs2Handle h_rack, uintptr_t* p_out_user_data) {
    if (!p_out_user_data) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack user data address ({})",
                    static_cast<void*>(p_out_user_data));
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        *p_out_user_data = 0;
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    *p_out_user_data = ref.Owner<Rack>()->user_data;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetVoiceHandle(OrbisNgs2Handle h_rack, u32 i_voice, OrbisNgs2Handle* p_out_handle) {
    if (!p_out_handle) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid voice handle address ({})",
                    static_cast<void*>(p_out_handle));
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        *p_out_handle = 0;
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    auto* p_rack = ref.Owner<Rack>();
    if (i_voice >= p_rack->limits.max_voices) {
        *p_out_handle = 0;
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_VOICE_INDEX, "Invalid voice index ({})", i_voice);
    }
    *p_out_handle = reinterpret_cast<OrbisNgs2Handle>(&p_rack->p_voices[i_voice].handle);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetInfo(OrbisNgs2Handle h_rack, OrbisNgs2RackInfo* p_out_info, std::size_t cb_info) {
    if (!p_out_info) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid rack info address ({})",
                    static_cast<void*>(p_out_info));
    }
    if (cb_info < sizeof(OrbisNgs2RackInfo)) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid rack info size ({}!={}[byte])", cb_info,
                    sizeof(OrbisNgs2RackInfo));
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    const auto* p_rack = ref.Owner<Rack>();
    const RackLimits& limits = p_rack->limits;
    FillRackKindInfo(*p_rack, p_out_info, cb_info);
    OrbisNgs2RackInfo info{};
    std::memcpy(info.name, p_rack->name, sizeof(info.name));
    info.rackHandle = h_rack;
    info.bufferInfo = p_rack->buffer_info;
    const System* p_system = p_rack->p_system;
    info.ownerSystemHandle = p_system ? reinterpret_cast<OrbisNgs2Handle>(&p_system->handle) : 0;
    info.type = p_rack->p_kind->rack_class;
    info.rackId = p_rack->p_kind->rack_id;
    info.uid = p_rack->uid;
    info.minGrainSamples = kMinGrainSamples;
    info.maxGrainSamples = limits.max_grain_samples;
    info.maxVoices = limits.max_voices;
    info.maxChannelWorks = limits.max_channel_works;
    info.maxInputs = limits.max_inputs;
    info.maxMatrices = limits.max_matrices;
    info.maxPorts = limits.max_ports;
    info.stateFlags = 1u | (p_rack->c_flush_locks ? 4u : 0u);
    *p_out_info = info;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle h_rack, u32 i_module,
                                                OrbisNgs2CustomModuleInfo* p_out_info, std::size_t cb_info) {
    if (!p_out_info) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid module information address ({})",
                    static_cast<void*>(p_out_info));
    }
    if (cb_info < sizeof(OrbisNgs2CustomModuleInfo)) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid module information size ({})", cb_info);
    }
    HandleRef ref{h_rack, HANDLE_RACK};
    if (!ref) {
        return InvalidHandleError(h_rack, HANDLE_RACK);
    }
    const RackLimits& limits = ref.Owner<Rack>()->limits;
    if (i_module >= limits.c_custom_modules) {
        return Fail(h_rack, ORBIS_NGS2_ERROR_INVALID_MODULE_INDEX, "Invalid module index ({})", i_module);
    }
    const CustomModule& module = limits.custom_modules[i_module];
    *p_out_info = {module.module_id, module.source_buffer_id, module.extra_buffer_id, module.dest_buffer_id,
                   module.off_state, module.cb_state,         0,                      0};
    // A module with its own info callback (UserFx2 at least) adds its part after these 0x20 bytes.
    if (module.module_id == 0x1f) {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "sceNgs2CustomRackGetModuleInfo: module info callbacks (F6)");
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
