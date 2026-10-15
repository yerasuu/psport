// SPDX-License-Identifier: GPL-2.0-or-later
// psport: indirect dispatches run from checked group counts.
//
// The game's indirect dispatches take their group counts from memory a shader wrote. Twice
// (issues #34 and #32, the PC memory model) those counts were garbage — 1.2 and 3.8 billion
// groups of the particle simulations — and the GPU, still running them after the kernel's
// timeout, was reset: device lost, the game gone. Real counts seen in games are at most a few
// hundred. Before each indirect dispatch a one-thread shader (indirect_guard.comp) copies the
// counts to a slot of our own, zero if they are implausible (past the device's limits or
// PS_INDIRECT_GUARD_MAX groups in all, default 4M), and the dispatch reads that slot. Skipped
// dispatches are counted and reported. PS_INDIRECT_GUARD=0: the game's counts as they are.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
struct Buffer;
}

namespace Vulkan {

class Instance;
class Scheduler;

class IndirectGuard {
public:
    IndirectGuard(const Instance& instance, Scheduler& scheduler);
    ~IndirectGuard();

    static bool Enabled();

    /// Records the check of the arguments at `off_args` in `args` (outside a render pass); returns
    /// the buffer and offset to dispatch from.
    std::pair<vk::Buffer, u64> CheckArgs(const VideoCore::Buffer& args, u64 off_args);

private:
    static constexpr u32 NumSlots = 1024;

    void Report();

    const Instance& m_instance;
    Scheduler& m_scheduler;
    std::unique_ptr<VideoCore::Buffer> m_checked; ///< NumSlots x 16 bytes of group counts
    std::unique_ptr<VideoCore::Buffer> m_skipped; ///< count and the last skipped counts (host)
    vk::UniqueDescriptorSetLayout m_set_layout;
    vk::UniquePipelineLayout m_pipeline_layout;
    vk::UniquePipeline m_pipeline;
    u32 m_max_count[3]{};
    u32 m_max_total = 0;
    std::atomic<u32> m_next_slot{0};
    std::atomic<u32> m_c_reported{0};
    std::atomic<s64> m_last_report_ns{0};
};

} // namespace Vulkan
