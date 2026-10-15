// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void GpuProfiler::Init(const Instance& instance, Scheduler& scheduler) {
    const char* sz_env = std::getenv("PS_GPU_PROFILE");
    if (!sz_env || sz_env[0] != '1' || m_p_instance) {
        return;
    }
    m_p_instance = new GpuProfiler(instance, scheduler);
}

GpuProfiler::GpuProfiler(const Instance& instance, Scheduler& scheduler_)
    : m_device{instance.GetDevice()}, m_scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlices * SliceQueries,
    };
    m_pool = Check<"create timestamp query pool">(m_device.createQueryPoolUnique(info));
    m_device.resetQueryPool(*m_pool, 0, NumSlices * SliceQueries);
    m_period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    for (auto& k : m_keys) {
        k.reserve(SliceQueries);
    }
    std::printf("GPU profile: on (timestamp period %.2f ns)\n", m_period_ns);
}

void GpuProfiler::WriteTimestamp(u64 key) {
    if (m_c_used[m_slice] + 1 >= SliceQueries) {
        return; // the frame's slice is full: the rest of the frame goes to the last label
    }
    // Outside render passes: radv_CmdWriteTimestamp2 crashed inside some. Marks sit where a
    // pass, dispatch or submission ends anyway.
    m_scheduler.EndRendering();
    const u32 query = m_slice * SliceQueries + m_c_used[m_slice]++;
    m_keys[m_slice].push_back(key);
    m_current = key;
    m_scheduler.Record([pool = *m_pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

void GpuProfiler::BeginFrame() {
    // Close the frame: one more timestamp without a label.
    if (m_c_used[m_slice] > 0 && m_c_used[m_slice] < SliceQueries) {
        m_scheduler.EndRendering();
        const u32 query = m_slice * SliceQueries + m_c_used[m_slice]++;
        m_scheduler.Record([pool = *m_pool, query](vk::CommandBuffer cmdbuf) {
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        });
        m_pending[m_slice] = true;
    }
    m_slice = (m_slice + 1) % NumSlices;
    // The oldest slice: its frame was submitted four frames ago.
    if (m_pending[m_slice]) {
        Collect(m_slice);
    }
    if (m_c_used[m_slice]) {
        m_device.resetQueryPool(*m_pool, m_slice * SliceQueries, m_c_used[m_slice]);
    }
    m_c_used[m_slice] = 0;
    m_keys[m_slice].clear();
    m_pending[m_slice] = false;
    Print();
}

void GpuProfiler::Collect(u32 which) {
    const u32 c_timestamps = m_c_used[which];
    std::vector<u64> stamps(c_timestamps);
    // Four frames on this is complete unless the GPU lags that far: then wait for it.
    const auto result = m_device.getQueryPoolResults(
        *m_pool, which * SliceQueries, c_timestamps, c_timestamps * sizeof(u64), stamps.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    if (result != vk::Result::eSuccess) {
        return;
    }
    for (u32 i = 0; i + 1 < c_timestamps; ++i) {
        const double ms = double(stamps[i + 1] - stamps[i]) * m_period_ns / 1e6;
        auto& total = m_totals[m_keys[which][i]];
        total.ms += ms;
        ++total.segments;
    }
    ++m_c_frames;
}

void GpuProfiler::Print() {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_window < std::chrono::seconds(5) || m_c_frames == 0) {
        return;
    }
    m_window = now;
    std::vector<std::pair<u64, Total>> sorted(m_totals.begin(), m_totals.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double sum = 0;
    for (const auto& [key, total] : sorted) {
        sum += total.ms;
    }
    std::printf("GPU profile: %.2f ms/frame over %llu frames, %zu labels\n", sum / m_c_frames,
                static_cast<unsigned long long>(m_c_frames), sorted.size());
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 30); ++i) {
        const auto& [key, total] = sorted[i];
        std::printf("  %6.3f ms/frame %5.1f/frame  %s\n", total.ms / m_c_frames,
                    double(total.segments) / m_c_frames, m_described[key].c_str());
    }
    m_totals.clear();
    m_c_frames = 0;
}

} // namespace Vulkan
