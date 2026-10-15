// SPDX-License-Identifier: GPL-2.0-or-later
// psport: small read-only guest buffers (constants) copied by the GPU command thread (stage A of
// the draw pipeline, vk_draw_pipe.h) into this ring, so the draw recording thread only binds
// them. Positions are monotonic byte counts; a region is reused once the submission that
// recorded its last draw has completed on the GPU. Stage B stamps each packet it records with
// the submission tick (Stamp), stage A retires stamps whose tick the GPU has passed.

#pragma once

#include <array>
#include <atomic>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class ConstantRing {
public:
    static constexpr u64 Capacity = 32ull << 20;

    ConstantRing(const Instance& instance, Scheduler& scheduler_)
        : m_scheduler{scheduler_},
          m_buffer{instance, 0, Capacity, VideoCore::MemoryType::Stream, "psport constant ring"} {
        ASSERT(!m_buffer.mapped_data.empty());
    }

    [[nodiscard]] vk::Buffer Handle() const noexcept {
        return m_buffer.Handle();
    }

    /// Stage A: `cb_data` bytes at `alignment`; returns the ring offset, or nullopt when the ring
    /// is still in use by the GPU (the caller leaves the binding to stage B).
    std::optional<u64> Allocate(u64 cb_data, u64 alignment) {
        u64 at = (m_position + alignment - 1) & ~(alignment - 1);
        if (at % Capacity + cb_data > Capacity) {
            at += Capacity - at % Capacity; // wrap to the ring start
        }
        const auto in_use = [&] { return at + cb_data > Capacity && at + cb_data - Capacity > m_retired; };
        if (in_use()) {
            Retire();
            if (in_use()) {
                return std::nullopt;
            }
        }
        m_position = at + cb_data;
        return at % Capacity;
    }

    [[nodiscard]] u8* Data(u64 off_data) noexcept {
        return m_buffer.mapped_data.data() + off_data;
    }

    /// Stage A, after writing: makes the bytes visible to the GPU.
    void Flush(u64 off_data, u64 cb_data) {
        m_buffer.Flush(off_data, cb_data);
    }

    /// Stage A: the position the next packet's allocations end at.
    [[nodiscard]] u64 Position() const noexcept {
        return m_position;
    }

    /// Stage B, after recording a packet whose allocations end at `end`. Stamps of one
    /// submission tick are merged: only the last end per tick is published.
    void Stamp(u64 end) {
        if (end == m_pending_end) {
            return;
        }
        const u64 tick = m_scheduler.CurrentTick();
        if (tick != m_pending_tick && m_pending_end != m_last_published) {
            Publish(m_pending_end, m_pending_tick);
        }
        m_pending_end = end;
        m_pending_tick = tick;
    }

private:
    void Publish(u64 end, u64 tick) {
        const u64 at = m_stamps_head.load(std::memory_order_relaxed);
        if (at - m_stamps_tail.load(std::memory_order_acquire) >= m_stamps.size()) {
            return; // full: this region retires with a later stamp
        }
        m_stamps[at % m_stamps.size()] = {end, tick};
        m_stamps_head.store(at + 1, std::memory_order_release);
        m_last_published = end;
    }

    void Retire() {
        auto* p_semaphore = m_scheduler.GetWorkSemaphore();
        p_semaphore->Refresh();
        u64 at = m_stamps_tail.load(std::memory_order_relaxed);
        const u64 head = m_stamps_head.load(std::memory_order_acquire);
        while (at < head && p_semaphore->IsFree(m_stamps[at % m_stamps.size()].tick)) {
            m_retired = m_stamps[at % m_stamps.size()].end;
            ++at;
        }
        m_stamps_tail.store(at, std::memory_order_release);
    }

    struct StampEntry {
        u64 end;
        u64 tick;
    };
    Scheduler& m_scheduler;
    VideoCore::Buffer m_buffer;
    u64 m_position = 0;     ///< stage A
    u64 m_retired = 0;      ///< stage A: the GPU is done with everything before this
    u64 m_pending_end = 0, m_pending_tick = 0, m_last_published = 0; ///< stage B
    std::array<StampEntry, 1024> m_stamps{};
    alignas(64) std::atomic<u64> m_stamps_head{0}; ///< stage B writes
    alignas(64) std::atomic<u64> m_stamps_tail{0}; ///< stage A reads
};

} // namespace Vulkan
