// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "core/emulator_settings.h"
#include "psport_toggles.h"
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

void GpuBusySampler::Init(const Instance& instance, Scheduler& scheduler) {
    if (!EmulatorSettingsImpl::Flag("PS_FRAME_STATS", false) || m_p_instance) {
        return;
    }
    const auto physical = instance.GetPhysicalDevice();
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    const u32 valid_bits = family < families.size() ? families[family].timestampValidBits : 0;
    const double period_ns = physical.getProperties().limits.timestampPeriod;
    if (valid_bits == 0 || period_ns <= 0.0) {
        std::printf("Frame stats: no timestamps on the graphics queue: GPU busy n/a\n");
        return;
    }
    m_p_instance = new GpuBusySampler(instance, scheduler, valid_bits, period_ns);
    PsStats::g_is_gpu_busy_sampled.store(true, std::memory_order_relaxed);
}

GpuBusySampler::GpuBusySampler(const Instance& instance, Scheduler& scheduler_, u32 valid_bits,
                               double period_ns)
    : m_device{instance.GetDevice()}, m_scheduler{scheduler_},
      m_valid_mask{valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1}, m_period_ns{period_ns} {
    m_pool = Check<"create timestamp query pool">(m_device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlices * SliceQueries,
    }));
}

void GpuBusySampler::WriteTimestamp(u32 query) {
    m_scheduler.Record([pool = *m_pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

void GpuBusySampler::EndSubmission() {
    if (!m_is_open) {
        return;
    }
    WriteTimestamp(m_slice * SliceQueries + m_c_used[m_slice]++);
    m_is_open = false;
}

void GpuBusySampler::BeginSubmission() {
    const u32 frame = PsStats::g_frame_number.load(std::memory_order_relaxed);
    if (m_is_sampling && frame != m_sampled_frame) {
        m_is_pending[m_slice] = m_c_used[m_slice] != 0;
        m_slice = (m_slice + 1) % NumSlices;
        // The other slice's frame was sampled SampleEvery frames before this one.
        if (m_is_pending[m_slice]) {
            Collect(m_slice);
        }
        m_is_sampling = false;
    }
    if (!m_is_sampling && frame % SampleEvery == 0 && frame != m_sampled_frame) {
        m_is_sampling = true;
        m_sampled_frame = frame;
        m_c_used[m_slice] = 0;
        m_is_pending[m_slice] = false;
        m_scheduler.Record([pool = *m_pool, first = m_slice * SliceQueries](vk::CommandBuffer cmdbuf) {
            cmdbuf.resetQueryPool(pool, first, SliceQueries);
        });
    }
    if (m_is_sampling && m_c_used[m_slice] + 2 <= SliceQueries) {
        WriteTimestamp(m_slice * SliceQueries + m_c_used[m_slice]++);
        m_is_open = true;
    }
}

void GpuBusySampler::Collect(u32 slice) {
    m_is_pending[slice] = false;
    const u32 c_stamps = m_c_used[slice];
    std::array<u64, SliceQueries> stamps;
    const auto result = m_device.getQueryPoolResults(*m_pool, slice * SliceQueries, c_stamps,
                                                     c_stamps * sizeof(u64), stamps.data(), sizeof(u64),
                                                     vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        PsStats::g_c_gpu_busy_unread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    u64 ticks = 0;
    for (u32 i = 0; i + 1 < c_stamps; i += 2) {
        ticks += (stamps[i + 1] - stamps[i]) & m_valid_mask;
    }
    PsStats::g_gpu_busy_ns.fetch_add(u64(double(ticks) * m_period_ns), std::memory_order_relaxed);
    PsStats::g_c_gpu_busy_frames.fetch_add(1, std::memory_order_relaxed);
}

} // namespace Vulkan
