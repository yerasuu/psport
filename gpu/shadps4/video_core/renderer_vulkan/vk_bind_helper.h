// SPDX-License-Identifier: GPL-2.0-or-later
// psport: a second thread for the GPU command thread's per-draw work (docs/en/parallel_gpu.md,
// "Texture binding on a helper thread"). The GPU thread forks one task per draw and joins it
// before anything that depends on the task's results; the helper spins for the next task
// while draws flow and sleeps after a short idle period.

#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <x86intrin.h>

#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class BindHelper {
public:
    using Task = void (*)(void*);

    explicit BindHelper(bool is_enabled) {
        if (is_enabled) {
            m_thread = std::jthread([this](std::stop_token stop) { Run(stop); });
        }
    }

    ~BindHelper() {
        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_c_posted.fetch_add(1, std::memory_order_seq_cst);
            m_c_posted.notify_one();
        }
    }

    [[nodiscard]] bool Available() const noexcept {
        return m_thread.joinable();
    }

    /// True on the helper thread (its callees must not join it).
    [[nodiscard]] static bool OnHelper() noexcept {
        return m_is_on_helper;
    }

    /// True between Fork() and Join() (GPU thread only).
    [[nodiscard]] bool Active() const noexcept {
        return m_is_active;
    }

    /// Starts `task(context)` on the helper.
    void Fork(Task task_, void* context_) {
        m_pfn_task = task_;
        m_p_context = context_;
        m_is_active = true;
        m_c_posted.fetch_add(1, std::memory_order_seq_cst);
        if (m_is_sleeping.load(std::memory_order_seq_cst)) {
            m_c_posted.notify_one();
        }
    }

    /// Waits for the forked task; no-op when none is running.
    void Join() {
        if (!m_is_active) {
            return;
        }
        const u64 target = m_c_posted.load(std::memory_order_relaxed);
        if (m_c_done.load(std::memory_order_acquire) != target) {
            const u64 start = __rdtsc();
            while (m_c_done.load(std::memory_order_acquire) != target) {
                __builtin_ia32_pause();
            }
            m_wait_cycles += __rdtsc() - start;
        }
        m_is_active = false;
    }

    /// Cycles the GPU thread waited in Join() and the helper spent in tasks (PS_FRAME_STATS).
    u64 m_wait_cycles = 0;
    std::atomic<u64> m_task_cycles{0};

private:
    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("psport:TexBind");
        m_is_on_helper = true;
        u64 seen = 0;
        while (!stop.stop_requested()) {
            // Draws arrive every few microseconds while a frame is recorded: spin, and sleep
            // only after a pause longer than that (between frames).
            const auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
            u64 now = m_c_posted.load(std::memory_order_acquire);
            for (u32 spins = 1; now == seen; ++spins) {
                __builtin_ia32_pause();
                if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                    m_is_sleeping.store(true, std::memory_order_seq_cst);
                    m_c_posted.wait(seen, std::memory_order_seq_cst);
                    m_is_sleeping.store(false, std::memory_order_relaxed);
                }
                now = m_c_posted.load(std::memory_order_acquire);
            }
            if (stop.stop_requested()) {
                break;
            }
            seen = now;
            const u64 start = __rdtsc();
            m_pfn_task(m_p_context);
            m_task_cycles.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            m_c_done.store(seen, std::memory_order_release);
        }
    }

    static inline thread_local bool m_is_on_helper = false;
    Task m_pfn_task = nullptr;
    void* m_p_context = nullptr;
    bool m_is_active = false;
    alignas(64) std::atomic<u64> m_c_posted{0};
    alignas(64) std::atomic<u64> m_c_done{0};
    alignas(64) std::atomic<bool> m_is_sleeping{false};
    std::jthread m_thread;
};

} // namespace Vulkan
