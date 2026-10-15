// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_occlusion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "pslayer_write_traps.h"
#include "psport_toggles.h"

#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/occlusion_resolve_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
struct Push {
    u32 c_slots;
    u32 events;
};

struct EventEntry {
    u32 address_lo;
    u32 address_hi;
    u32 pairs;
    u32 upto;
};

/// Everything before it before everything after it (resolves are a few per frame).
void FullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}
} // namespace

OcclusionQueries::OcclusionQueries(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::BufferCache& buffer_cache_)
    : m_instance{instance_}, m_scheduler{scheduler_}, m_buffer_cache{buffer_cache_} {
    const auto device = m_instance.GetDevice();
    m_pool = Check(device.createQueryPoolUnique({
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = NumSlots,
    }));
    m_results = std::make_unique<VideoCore::Buffer>(m_instance, 0, NumSlots * sizeof(u64),
                                                  VideoCore::MemoryType::DeviceLocal,
                                                  "Occlusion results");
    m_total = std::make_unique<VideoCore::Buffer>(m_instance, 0, 256, VideoCore::MemoryType::DeviceLocal,
                                                "Occlusion total");
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    m_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(Push)};
    m_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*m_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(OCCLUSION_RESOLVE_COMP, device);
    m_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *m_pipeline_layout,
            }));
    device.destroyShaderModule(module);
    std::printf("GPU: the game's occlusion queries are Vulkan occlusion queries (%s counts)\n",
                m_instance.IsOcclusionQueryPrecise() ? "exact" : "approximate");
}

OcclusionQueries::~OcclusionQueries() {
    m_scheduler.SetCarriedScope(nullptr);
}

void OcclusionQueries::BeginSegment(bool is_inside_render_pass) {
    if (m_begun == m_reset_until) {
        if (is_inside_render_pass) {
            ++m_c_starved; // no reset can be recorded in a render pass: these samples go uncounted
            return;
        }
        ResetAhead();
    }
    const u32 slot = static_cast<u32>(m_begun % NumSlots);
    const bool is_precise = m_instance.IsOcclusionQueryPrecise();
    m_scheduler.Record([pool = *m_pool, slot, is_precise](vk::CommandBuffer cmdbuf) {
        cmdbuf.beginQuery(pool, slot,
                          is_precise ? vk::QueryControlFlagBits::ePrecise : vk::QueryControlFlags{});
    });
    ++m_begun;
    m_is_active = true;
}

void OcclusionQueries::EndSegment() {
    const u32 slot = static_cast<u32>((m_begun - 1) % NumSlots);
    m_scheduler.Record([pool = *m_pool, slot](vk::CommandBuffer cmdbuf) { cmdbuf.endQuery(pool, slot); });
    m_is_active = false;
}

void OcclusionQueries::ResetAhead() {
    // Slots from `m_resolved` on hold results not copied yet; those below `m_begun` were used.
    m_reset_until = std::max(m_reset_until, m_begun);
    const u64 target = std::min<u64>(m_begun + ResetBatch, m_resolved + NumSlots);
    if (target <= m_reset_until || m_reset_until - m_begun >= ResetBatch / 2) {
        return;
    }
    const u32 first = static_cast<u32>(m_reset_until % NumSlots);
    const u32 c_slots = static_cast<u32>(target - m_reset_until);
    const u32 head = std::min(c_slots, NumSlots - first);
    m_scheduler.Record([pool = *m_pool, first, c_slots, head](vk::CommandBuffer cmdbuf) {
        cmdbuf.resetQueryPool(pool, first, head);
        if (c_slots > head) {
            cmdbuf.resetQueryPool(pool, 0, c_slots - head);
        }
    });
    m_reset_until = target;
}

void OcclusionQueries::Resolve() {
    const u64 ended = m_is_active ? m_begun - 1 : m_begun;
    if (m_pending.empty() && ended - m_resolved < NumSlots / 2) {
        return; // nothing to write, and room left
    }
    const u32 c_segments = static_cast<u32>(ended - m_resolved);
    const u32 first = static_cast<u32>(m_resolved % NumSlots);
    std::vector<EventEntry> entries;
    entries.reserve(std::max<size_t>(m_pending.size(), 1));
    const u64 tick = m_scheduler.CurrentTick();
    for (const auto& event : m_pending) {
        entries.push_back({u32(event.va_counter), u32(event.va_counter >> 32), event.pairs,
                           u32(event.upto - m_resolved)});
        if (PsStats::g_is_enabled && event.va_guest != 0) {
            m_written_at[event.va_guest] = tick;
        }
        if (event.id != 0 && ReadTrace()) {
            NoteResolved(event.va_guest, event.id, tick);
        }
    }
    const u32 c_events = static_cast<u32>(entries.size());
    if (entries.empty()) {
        entries.push_back({}); // a binding to bind
    }
    auto& stream = m_buffer_cache.GetStreamBuffer();
    const u64 cb_entries = entries.size() * sizeof(EventEntry);
    const u64 off_entries =
        stream.Copy(entries.data(), cb_entries, m_instance.StorageMinAlignment());
    const bool should_clear_total = !m_is_total_cleared;
    m_is_total_cleared = true;
    m_scheduler.Record([this, first, c_segments, c_events, should_clear_total, stream = stream.Handle(),
                      off_entries, cb_entries](vk::CommandBuffer cmdbuf) {
        if (should_clear_total) {
            cmdbuf.fillBuffer(m_total->Handle(), 0, 8, 0);
        }
        const u32 head = std::min(c_segments, NumSlots - first);
        if (head != 0) {
            cmdbuf.copyQueryPoolResults(*m_pool, first, head, m_results->Handle(), 0, sizeof(u64),
                                        vk::QueryResultFlagBits::e64 |
                                            vk::QueryResultFlagBits::eWait);
        }
        if (c_segments > head) {
            cmdbuf.copyQueryPoolResults(*m_pool, 0, c_segments - head, m_results->Handle(),
                                        head * sizeof(u64), sizeof(u64),
                                        vk::QueryResultFlagBits::e64 |
                                            vk::QueryResultFlagBits::eWait);
        }
        FullBarrier(cmdbuf);
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {m_results->Handle(), 0, NumSlots * sizeof(u64)},
            {m_total->Handle(), 0, 8},
            {stream, off_entries, cb_entries},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        const Push push{c_segments, c_events};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(1, 1, 1);
        FullBarrier(cmdbuf);
    });
    m_resolved = ended;
    m_pending.clear();
}

void OcclusionQueries::WriteBeforeLabel() {
    std::scoped_lock lk{m_mutex};
    if (m_pending.empty()) {
        return;
    }
    const bool is_in_pass = m_scheduler.IsRendering();
    ++(is_in_pass ? m_c_labels_ahead_in_pass : m_c_labels_ahead);
    // A label ahead of results not written yet would let the CPU see the label before them. (Once
    // taken for the cause of slow launches; that was volatile blocks' refresh copies breaking
    // render passes, BufferCache::LayerVolatileAllowed. The game reads its results 2-10 ms after
    // the event and finds them written either way: PS_OCCLUSION_READ_TRACE.)
    static const bool s_is_ordered = [] {
        const char* sz_env = std::getenv("PS_OCCLUSION_LABEL_ORDER");
        return !sz_env || sz_env[0] != '0';
    }();
    if (!s_is_ordered) {
        return;
    }
    m_scheduler.EndRendering();
    if (m_is_active) {
        EndSegment();
    }
    Resolve();
    ResetAhead();
    BeginSegment(false);
}

void OcclusionQueries::Suspend(bool is_inside_render_pass) {
    std::scoped_lock lk{m_mutex};
    if (!m_has_started) {
        return;
    }
    if (m_is_active) {
        EndSegment();
    }
    if (!is_inside_render_pass) {
        Resolve();
        ResetAhead();
    }
}

void OcclusionQueries::Resume(bool is_inside_render_pass) {
    std::scoped_lock lk{m_mutex};
    if (m_has_started && !m_is_active) {
        BeginSegment(is_inside_render_pass);
    }
}

void OcclusionQueries::Event(VAddr va_guest, u32 pairs) {
    std::scoped_lock lk{m_mutex};
    if (!m_has_started) {
        // The first event: from here on a segment is always active (slots reset outside a
        // render pass first).
        m_scheduler.EndRendering();
        ResetAhead();
        m_has_started = true;
        m_scheduler.SetCarriedScope(this);
    }
    // PS_OCCLUSION_TRACE=N (diagnostics): the first N events, with the frame and the render pass.
    static const u64 s_trace = [] {
        const char* sz_env = std::getenv("PS_OCCLUSION_TRACE");
        return sz_env ? std::strtoull(sz_env, nullptr, 10) : 0ull;
    }();
    if (m_c_events < s_trace) {
        const auto& state = m_scheduler.GetRenderState();
        std::printf("Occlusion event %llu: %#llx pairs %u, frame %llu, %s %ux%u (%u colors, "
                    "depth %s), %zu pending\n",
                    (unsigned long long)m_c_events, (unsigned long long)va_guest, pairs,
                    (unsigned long long)PsStats::g_frame_number.load(std::memory_order_relaxed),
                    m_scheduler.IsRendering() ? "in a render pass" : "outside the last pass",
                    state.width, state.height, state.num_color_attachments,
                    state.depth_stencil_attachment.image_view ? "yes" : "no", m_pending.size());
    }
    // The counters, written by the GPU through their device address; in place (the CPU reads
    // them, a VRAM copy would show it stale ones).
    const u32 cb_results = (pairs - 1) * 16 + 8;
    m_buffer_cache.force_writes_in_place = true;
    const auto [buffer, offset] = m_buffer_cache.ObtainBuffer(va_guest, cb_results, true);
    m_buffer_cache.force_writes_in_place = false;
    if (m_is_active) {
        EndSegment();
    }
    if (const auto it = PsStats::g_is_enabled ? m_written_at.find(va_guest) : m_written_at.end();
        it != m_written_at.end()) {
        ++m_c_reused;
        m_c_reused_late += !m_scheduler.IsFree(it->second);
        // A query's begin counters at 16-byte steps, its end counters 8 bytes after them: what the
        // game read from its last use (statistics: none passed, some passed, not valid).
        if (va_guest % 16 == 0 && m_scheduler.IsFree(it->second) && !ReadTrace()) {
            constexpr u64 Valid = 1ull << 63;
            u64 samples = 0;
            bool is_valid = true;
            for (u32 i = 0; i < pairs; ++i) {
                const u64 begin = *reinterpret_cast<const volatile u64*>(va_guest + i * 16);
                const u64 end = *reinterpret_cast<const volatile u64*>(va_guest + i * 16 + 8);
                is_valid = is_valid && (begin & Valid) && (end & Valid);
                samples += (end & ~Valid) - (begin & ~Valid);
            }
            ++(!is_valid ? m_c_queries_invalid : samples ? m_c_queries_passed : m_c_queries_hidden);
        }
    }
    m_pending.push_back({buffer->BufferDeviceAddress() + offset, pairs, m_begun, va_guest, m_c_events + 1});
    if (ReadTrace()) {
        WatchReads(va_guest, m_c_events + 1);
    }
    BeginSegment(m_scheduler.IsRendering());
    ++m_c_events;
    if (!m_scheduler.IsRendering() && m_pending.size() >= 64) {
        if (m_is_active) {
            EndSegment();
        }
        Resolve();
        ResetAhead();
        BeginSegment(false);
    }
    // PS_FRAME_STATS: how often the game measures (every 5 s).
    static auto s_last_report = std::chrono::steady_clock::now();
    static u64 s_c_reported = 0;
    if (const auto now = std::chrono::steady_clock::now();
        PsStats::g_is_enabled && now - s_last_report >= std::chrono::seconds(5)) {
        std::printf("Occlusion queries: %llu events in %.0f s, %llu in all; %llu segments, "
                    "%llu not counted; labels after unwritten events: %llu (%llu in a render "
                    "pass); counters reused before their results were written: %llu of %llu; queries "
                    "read: %llu none passed, %llu some passed, %llu not valid\n",
                    (unsigned long long)(m_c_events - s_c_reported),
                    std::chrono::duration<double>(now - s_last_report).count(),
                    (unsigned long long)m_c_events, (unsigned long long)m_begun,
                    (unsigned long long)m_c_starved, (unsigned long long)(m_c_labels_ahead + m_c_labels_ahead_in_pass),
                    (unsigned long long)m_c_labels_ahead_in_pass, (unsigned long long)m_c_reused_late,
                    (unsigned long long)m_c_reused, (unsigned long long)m_c_queries_hidden,
                    (unsigned long long)m_c_queries_passed, (unsigned long long)m_c_queries_invalid);
        m_c_labels_ahead = m_c_labels_ahead_in_pass = 0;
        m_c_reused = m_c_reused_late = 0;
        m_c_queries_hidden = m_c_queries_passed = m_c_queries_invalid = 0;
        s_last_report = now;
        s_c_reported = m_c_events;
        if (ReadTrace()) {
            PrintReadTrace();
        }
    }
}

bool OcclusionQueries::ReadTrace() {
    static const bool s_is_enabled = [] {
        const char* sz_env = std::getenv("PS_OCCLUSION_READ_TRACE");
        return sz_env && sz_env[0] == '1';
    }();
    return s_is_enabled;
}

void OcclusionQueries::WatchReads(VAddr va_guest, u64 id) {
    const u64 page = va_guest & ~u64{4095};
    std::scoped_lock lk{m_watch_mutex};
    auto& list = m_watched[page];
    if (list.empty()) {
        PsLayer::WriteTraps::Set(page, 4096, PsLayer::WriteTraps::QueryReads, true);
    }
    list.push_back({id, 0, std::chrono::steady_clock::now()});
}

void OcclusionQueries::NoteResolved(VAddr va_guest, u64 id, u64 tick) {
    std::scoped_lock lk{m_watch_mutex};
    if (const auto it = m_watched.find(va_guest & ~u64{4095}); it != m_watched.end()) {
        for (auto& entry : it->second) {
            if (entry.id == id) {
                entry.resolve_tick = tick;
            }
        }
    }
}

bool OcclusionQueries::OnAccess(VAddr va_guest, u64 rip, bool is_write, bool is_gpu_thread) {
    const u64 page = va_guest & ~u64{4095};
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lk{m_watch_mutex};
    if (const auto it = m_watched.find(page); it != m_watched.end()) {
        ++m_c_accesses;
        m_c_accesses_by_port += is_gpu_thread;
        m_c_access_writes += is_write;
        ++m_access_rips[rip];
        for (const auto& entry : it->second) {
            const int state = entry.resolve_tick == 0                ? 0
                              : !m_scheduler.IsFree(entry.resolve_tick) ? 1
                                                                      : 2;
            ++m_access_state[state];
            const double ms = std::chrono::duration<double, std::milli>(now - entry.at).count();
            ++m_access_age[ms < 2 ? 0 : ms < 5 ? 1 : ms < 10 ? 2 : ms < 20 ? 3 : 4];
        }
        m_watched.erase(it);
    } else if (!(PsLayer::WriteTraps::Reasons(va_guest) & PsLayer::WriteTraps::QueryReads)) {
        return false;
    }
    PsLayer::WriteTraps::Set(page, 4096, PsLayer::WriteTraps::QueryReads, false);
    return true;
}

void OcclusionQueries::PrintReadTrace() {
    std::scoped_lock lk{m_watch_mutex};
    std::vector<std::pair<u64, u64>> rips(m_access_rips.begin(), m_access_rips.end());
    std::sort(rips.begin(), rips.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string code;
    for (size_t i = 0; i < std::min<size_t>(rips.size(), 6); ++i) {
        char sz_item[48];
        std::snprintf(sz_item, sizeof(sz_item), " %#llx x%llu", (unsigned long long)rips[i].first,
                      (unsigned long long)rips[i].second);
        code += sz_item;
    }
    std::printf("Occlusion counters, first access after events: %llu pages (%llu by the port, "
                "%llu writes), %zu still watched; events: results not recorded %llu, GPU not "
                "there %llu, on the CPU %llu; after <2 ms %llu, <5 %llu, <10 %llu, <20 %llu, "
                "more %llu; code:%s\n",
                (unsigned long long)m_c_accesses, (unsigned long long)m_c_accesses_by_port,
                (unsigned long long)m_c_access_writes, m_watched.size(),
                (unsigned long long)m_access_state[0], (unsigned long long)m_access_state[1],
                (unsigned long long)m_access_state[2], (unsigned long long)m_access_age[0],
                (unsigned long long)m_access_age[1], (unsigned long long)m_access_age[2],
                (unsigned long long)m_access_age[3], (unsigned long long)m_access_age[4], code.c_str());
    m_c_accesses = m_c_accesses_by_port = m_c_access_writes = 0;
    m_access_state = {};
    m_access_age = {};
    m_access_rips.clear();
}

} // namespace Vulkan
