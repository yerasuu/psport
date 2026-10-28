// SPDX-License-Identifier: GPL-2.0-or-later
// psport PS_SECTIONS=1 (diagnostics): time the draw recording thread spends per part of a draw,
// inclusive (nested parts count in their parent too), printed with the draw pipe statistics.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <x86intrin.h>

namespace PsSections {
enum Id : std::uint32_t {
    Packet,          ///< a whole packet of the draw pipe
    Task,            ///< ordered tasks (fences, signals, DMA, flips)
    DrawRecord,
    PrepareRenderState,
    BindResources,
    BindBuffers,
    BindTextures,
    BeginRendering,
    ResolveVertexBuffers,
    ResolveIndexBuffer,
    EmitVertexBuffers,
    UpdateDynamicState,
    DispatchRecord,
    ShaderHle,
    ObtainBuffer,
    Flush,
    Motion,          ///< object motion (bone palettes, index ranges)
    PipelineBind,    ///< Pipeline::BindResources (descriptors, push constants)
    Kick,            ///< ResetBindings and the hand-over to the Vulkan recording threads
    EnsureResident,
    ObtainStream,  ///< ObtainBuffer's copy of a small read-only buffer into the stream buffer
    ObtainVram,    ///< ObtainBuffer's VRAM path (SynchronizeMemory)
    LayerBind,     ///< PS_LAYER_MEMORY: BufferCache::LayerBind
    Count,
};
inline constexpr const char* Names[Count] = {
    "packet",       "tasks",       "draw",          "render state", "bind",
    "buffers",      "textures",    "begin render",  "vertex",       "index",
    "emit vertex",  "dynamic",     "dispatch",      "HLE shader",   "ObtainBuffer",
    "flush",        "motion",      "descriptors",   "kick",         "EnsureResident",
    "stream copy",  "VRAM path",   "layer bind",
};
inline std::array<std::atomic<std::uint64_t>, Count> g_cycles{};

inline bool Enabled() {
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_SECTIONS");
        return sz_env && sz_env[0] == '1';
    }();
    return s_is_on;
}
/// Set on the draw recording thread (DrawPipe::Run): only its time is counted.
inline thread_local bool g_is_recording_thread = false;

struct Scope {
    explicit Scope(Id id_) : m_id{id_}, m_start{Enabled() && g_is_recording_thread ? __rdtsc() : 0} {}
    ~Scope() {
        if (m_start) {
            g_cycles[m_id].fetch_add(__rdtsc() - m_start, std::memory_order_relaxed);
        }
    }
    Id m_id;
    std::uint64_t m_start;
};
} // namespace PsSections

#define PS_SECTION_JOIN2(a, b) a##b
#define PS_SECTION_JOIN(a, b) PS_SECTION_JOIN2(a, b)
#define PS_SECTION(id) PsSections::Scope PS_SECTION_JOIN(ps_section_scope_, __LINE__){PsSections::id}
