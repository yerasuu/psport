// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_copy.h"
#include "psport_threads.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "psport_toggles.h"
#include "common/thread.h"

namespace PsCopy {
namespace {

thread_local bool s_is_in_copy_thread = false;

class Pool {
public:
    Pool() {
        // Copies are on the critical path (fences wait for them): normal priority, a quarter
        // of the hardware threads available to the process (Steam Deck 2).
        unsigned c_threads = std::clamp(PsThreads::Available() / 4, 1u, 4u);
        if (const char* sz_env = std::getenv("PS_COPY_THREADS")) {
            c_threads = static_cast<unsigned>(std::clamp(std::atoi(sz_env), 0, 16));
        }
        m_epochs.emplace_back();
        for (unsigned i = 0; i < c_threads; ++i) {
            m_threads.emplace_back([this, i] { Loop(i); });
        }
    }
    ~Pool() {
        {
            std::scoped_lock lk{m_mutex};
            m_should_stop = true;
        }
        m_cv.notify_all();
        for (auto& thread : m_threads) {
            thread.join();
        }
    }

    bool Enabled() const {
        return !m_threads.empty();
    }

    void Async(std::function<void()> task) {
        {
            std::scoped_lock lk{m_mutex};
            const u64 epoch = m_epoch_base + m_epochs.size() - 1;
            ++m_epochs.back().pending;
            ++m_pending;
            m_async_tasks.push_back({std::move(task), epoch});
        }
        m_cv.notify_one();
    }

    void AfterCopies(std::function<void()> callback) {
        {
            std::unique_lock lk{m_mutex};
            if (m_epochs.size() > 1 || m_epochs.back().pending != 0 || m_is_draining) {
                // Runs once every copy issued so far is done, in order with earlier ones.
                m_epochs.back().callbacks.push_back(std::move(callback));
                m_epochs.emplace_back();
                return;
            }
        }
        callback();
    }

    void WaitAll() {
        // The waiting thread helps instead of only spinning.
        while (true) {
            Task task;
            {
                std::scoped_lock lk{m_mutex};
                if (m_pending == 0 && m_epochs.size() == 1 && !m_is_draining) {
                    return;
                }
                if (!m_async_tasks.empty()) {
                    task = std::move(m_async_tasks.front());
                    m_async_tasks.pop_front();
                }
            }
            if (task.run) {
                RunAsync(task);
            } else {
                std::this_thread::yield();
            }
        }
    }

    void Run(std::size_t c_tasks, const std::function<void(std::size_t)>& task) {
        auto job = std::make_shared<Job>();
        job->p_task = &task;
        job->c_tasks = c_tasks;
        {
            std::scoped_lock lk{m_mutex};
            m_current = job;
            ++m_generation;
        }
        m_cv.notify_all();
        Work(*job);
        // Workers only call the task for indices below count, all done before this returns.
        while (job->done.load(std::memory_order_acquire) < c_tasks) {
            std::this_thread::yield();
        }
        std::scoped_lock lk{m_mutex};
        if (m_current == job) {
            m_current.reset();
        }
    }

private:
    using u64 = unsigned long long;
    struct Task {
        std::function<void()> run;
        u64 epoch{};
    };
    /// Copies issued between two AfterCopies() calls, and the callbacks waiting for them.
    struct Epoch {
        std::size_t pending = 0;
        std::vector<std::function<void()>> callbacks;
    };
    struct Job {
        const std::function<void(std::size_t)>* p_task{};
        std::size_t c_tasks{};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
    };

    void RunAsync(Task& task) {
        const bool is_previous_copy_thread = s_is_in_copy_thread;
        s_is_in_copy_thread = true;
        task.run();
        s_is_in_copy_thread = is_previous_copy_thread;
        Complete(task.epoch);
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(u64 epoch) {
        std::unique_lock lk{m_mutex};
        --m_epochs[epoch - m_epoch_base].pending;
        --m_pending;
        if (m_is_draining) {
            return;
        }
        m_is_draining = true;
        while (true) {
            std::vector<std::function<void()>> ready;
            while (m_epochs.size() > 1 && m_epochs.front().pending == 0) {
                for (auto& callback : m_epochs.front().callbacks) {
                    ready.push_back(std::move(callback));
                }
                m_epochs.pop_front();
                ++m_epoch_base;
            }
            if (ready.empty()) {
                break;
            }
            lk.unlock();
            for (auto& callback : ready) {
                callback();
            }
            lk.lock();
        }
        m_is_draining = false;
    }

    static void Work(Job& job) {
        const bool is_previous_copy_thread = s_is_in_copy_thread;
        s_is_in_copy_thread = true;
        for (std::size_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < job.c_tasks;) {
            (*job.p_task)(i);
            job.done.fetch_add(1, std::memory_order_release);
        }
        s_is_in_copy_thread = is_previous_copy_thread;
    }

    void Loop(unsigned i_thread) {
        Common::SetCurrentThreadName(("psport:Copy" + std::to_string(i_thread)).c_str());
        u64 seen = 0;
        std::unique_lock lk{m_mutex};
        while (true) {
            m_cv.wait(lk, [&] {
                return m_should_stop || !m_async_tasks.empty() || (m_generation != seen && m_current);
            });
            if (m_should_stop) {
                return;
            }
            if (!m_async_tasks.empty()) {
                Task task = std::move(m_async_tasks.front());
                m_async_tasks.pop_front();
                lk.unlock();
                RunAsync(task);
                lk.lock();
                continue;
            }
            seen = m_generation;
            const auto job = m_current;
            lk.unlock();
            Work(*job);
            lk.lock();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::shared_ptr<Job> m_current;
    u64 m_generation = 0;
    bool m_should_stop = false;
    std::deque<Task> m_async_tasks;
    std::deque<Epoch> m_epochs; ///< back() is open for new copies
    u64 m_epoch_base = 0;       ///< epoch number of epochs.front()
    std::size_t m_pending = 0;
    bool m_is_draining = false;
};

Pool& GetPool() {
    static Pool s_pool;
    return s_pool;
}

struct Batch {
    std::vector<Item> items;
    unsigned long long cb_batch = 0;
};
thread_local Batch s_batch;

} // namespace

bool Enabled() {
    return GetPool().Enabled() && !PsToggle::Disabled(PsToggle::ParallelCopies);
}

void Async(std::function<void()> task) {
    if (!Enabled()) {
        task();
        return;
    }
    GetPool().Async(std::move(task));
}

void FlushBatch() {
    if (s_batch.items.empty()) {
        return;
    }
    auto items = std::make_shared<std::vector<Item>>(std::move(s_batch.items));
    s_batch.items = {};
    s_batch.items.reserve(256);
    s_batch.cb_batch = 0;
    Async([items] {
        for (const auto& item : *items) {
            item.pfn_run(item);
        }
    });
}

void QueueCopy(const Item& item) {
    if (!Enabled()) {
        item.pfn_run(item);
        return;
    }
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    s_batch.items.push_back(item);
    s_batch.cb_batch += item.cb_copy;
    if (s_batch.cb_batch >= 512 * 1024 || s_batch.items.size() >= 256) {
        FlushBatch();
    }
}

void AfterCopies(std::function<void()> callback) {
    FlushBatch();
    GetPool().AfterCopies(std::move(callback));
}

void WaitAsync() {
    // The caller's own batch runs here: handing it over only to wait for it costs a wakeup.
    if (!s_batch.items.empty()) {
        for (const auto& item : s_batch.items) {
            item.pfn_run(item);
        }
        s_batch.items.clear();
        s_batch.cb_batch = 0;
    }
    GetPool().WaitAll();
}

void ParallelFor(std::size_t c_tasks, const std::function<void(std::size_t)>& task) {
    if (c_tasks <= 1 || s_is_in_copy_thread || !Enabled()) {
        for (std::size_t i = 0; i < c_tasks; ++i) {
            task(i);
        }
        return;
    }
    GetPool().Run(c_tasks, task);
}

} // namespace PsCopy
