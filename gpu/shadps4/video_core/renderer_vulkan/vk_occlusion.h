// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the game's occlusion queries (ZPASS_DONE events) as Vulkan occlusion queries.
//
// On the PS4 every depth block keeps a running count of the samples that pass the depth test; a
// ZPASS_DONE event has each block write its count, 16 bytes apart, with the valid bit (63). The
// game issues one event before the draws it measures and one after, and takes the difference
// (dozens of events a frame).
//
// Here the samples are counted by a chain of Vulkan occlusion query segments: one is active at
// all times once the game has used them, and a segment ends at every event and at every boundary
// a query may not span (a render pass beginning or ending, a command buffer: Scheduler::
// CarriedScope). An event remembers how many segments ended before it. Where no render pass is
// open (the next boundary), the segments' results are copied and one dispatch
// (occlusion_resolve.comp) adds them up in order, writing each pending event's running total into
// the game's memory through its device address. Results land a little after the PS4 would write
// them; until then their valid bits are clear, which the game checks as on the PS4.
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace VideoCore {
class BufferCache;
struct Buffer;
} // namespace VideoCore

namespace Vulkan {

class Instance;

class OcclusionQueries final : public Scheduler::CarriedScope {
public:
    OcclusionQueries(const Instance& instance, Scheduler& scheduler,
                     VideoCore::BufferCache& buffer_cache);
    ~OcclusionQueries();

    /// A ZPASS_DONE event writing the counters of `pairs` depth blocks at `va_guest` (recording
    /// thread, in stream order).
    void Event(VAddr va_guest, u32 pairs);

    /// Before a label (an end-of-pipe write the CPU may poll): the events recorded before it are
    /// written first, as on the PS4, where the counters land in stream order ahead of later
    /// labels. Ends the render pass when one is open (copies and dispatches are outside them).
    /// PS_OCCLUSION_LABEL_ORDER=0: labels may go ahead (statistics only).
    void WriteBeforeLabel();

    /// Events so far (PS_PM4_SELFTEST and the frame stats).
    u64 Events() const noexcept {
        return m_c_events;
    }

    /// PS_OCCLUSION_READ_TRACE=1 (diagnostics): a page with counters is no access after an event
    /// there until the game's first access, which comes here from the fault handler (any thread):
    /// whether the event's results were on the CPU by then. False: not such a page.
    bool OnAccess(VAddr va_guest, u64 rip, bool is_write, bool is_gpu_thread);

    void Suspend(bool is_inside_render_pass) override;
    void Resume(bool is_inside_render_pass) override;

private:
    static constexpr u32 NumSlots = 4096;
    static constexpr u32 ResetBatch = 512;

    struct Pending {
        u64 va_counter; ///< device address of the first counter
        u32 pairs;
        u64 upto; ///< segments ended before the event
        VAddr va_guest = 0; ///< statistics (m_written_at)
        u64 id = 0;              ///< the event's number (PS_OCCLUSION_READ_TRACE)
    };
    /// An event on a page watched for the game's first access (PS_OCCLUSION_READ_TRACE).
    struct Watched {
        u64 id;
        u64 resolve_tick; ///< of the resolve writing its results; 0 while not recorded
        std::chrono::steady_clock::time_point at;
    };
    static bool ReadTrace();
    void WatchReads(VAddr va_guest, u64 id);
    void NoteResolved(VAddr va_guest, u64 id, u64 tick);
    void PrintReadTrace();

    void BeginSegment(bool is_inside_render_pass);
    void EndSegment();
    /// Outside a render pass: the ended segments added up, the pending events written.
    void Resolve();
    /// Outside a render pass: query slots reset ahead of the segments that will use them.
    void ResetAhead();

    const Instance& m_instance;
    Scheduler& m_scheduler;
    VideoCore::BufferCache& m_buffer_cache;
    std::recursive_mutex m_mutex;
    vk::UniqueQueryPool m_pool;
    std::unique_ptr<VideoCore::Buffer> m_results; ///< segment results copied for the resolve
    std::unique_ptr<VideoCore::Buffer> m_total;   ///< the running sample count (64-bit)
    vk::UniqueDescriptorSetLayout m_set_layout;
    vk::UniquePipelineLayout m_pipeline_layout;
    vk::UniquePipeline m_pipeline;
    std::vector<Pending> m_pending;
    u64 m_begun = 0;       ///< segments begun (the next one uses slot begun % NumSlots)
    u64 m_resolved = 0;    ///< segments added to the total
    u64 m_reset_until = 0; ///< slots below it (absolute) are reset and unused
    bool m_is_active = false;
    bool m_has_started = false; ///< the game has used occlusion queries
    bool m_is_total_cleared = false;
    u64 m_c_events = 0;
    u64 m_c_starved = 0; ///< segments not begun in a render pass for want of a reset slot
    u64 m_c_labels_ahead = 0, m_c_labels_ahead_in_pass = 0; ///< statistics (NoteLabel)
    /// Statistics: counters the game reused (a new event at the address) while the GPU had not yet
    /// written their previous results (the game saw them not ready), of all reuses.
    std::unordered_map<u64, u64> m_written_at; ///< counter address -> tick of the resolve writing it
    u64 m_c_reused = 0, m_c_reused_late = 0;
    u64 m_c_queries_hidden = 0, m_c_queries_passed = 0, m_c_queries_invalid = 0; ///< statistics
    /// PS_OCCLUSION_READ_TRACE: watched pages (page -> events since its last access) and what the
    /// accesses found: results not recorded yet, recorded but the GPU not there, on the CPU.
    std::mutex m_watch_mutex;
    std::unordered_map<u64, std::vector<Watched>> m_watched;
    std::array<u64, 3> m_access_state{};
    std::array<u64, 5> m_access_age{}; ///< ms after the event: <2, <5, <10, <20, more
    std::unordered_map<u64, u64> m_access_rips;
    u64 m_c_accesses = 0, m_c_accesses_by_port = 0, m_c_access_writes = 0;
};

} // namespace Vulkan
