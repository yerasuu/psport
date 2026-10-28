// SPDX-License-Identifier: GPL-2.0-or-later
// psport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        std::jthread previous;
        std::scoped_lock lock{mutex};
        previous = std::move(thread);
        thread = std::jthread([func = std::move(func)](std::stop_token stop) {
            runtime_thread_attach_host("psport:hle");
            func(stop);
        });
        id = thread.get_id();
        stop_source = thread.get_stop_source();
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // Another thread may join it at the same time (AvPlayer's demuxer and Stop()): one of them
    // takes the handle and the other waits until the thread has ended, so Stop() never returns
    // while the thread still runs.
    void Join() {
        std::unique_lock lock{mutex};
        if (std::this_thread::get_id() == id) {
            if (thread.joinable()) thread.detach();
            return;
        }
        if (joining) {
            joined.wait(lock, [this] { return !joining; });
            return;
        }
        if (!thread.joinable()) return;
        std::jthread taken = std::move(thread);
        joining = true;
        lock.unlock();
        taken.join();
        lock.lock();
        joining = false;
        joined.notify_all();
    }
    bool Joinable() const {
        std::scoped_lock lock{mutex};
        return thread.joinable() || joining;
    }
    // The stop request goes through the thread's own stop source because a thread joining it may
    // already have taken the handle: a decoder waiting for a free buffer would then never see it,
    // and the demuxer joining that decoder and Stop() joining the demuxer would wait forever.
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            if (!thread.joinable() && !joining) return;
            stop_source.request_stop();
        }
        Join();
    }

private:
    mutable std::mutex mutex;
    std::condition_variable joined;
    std::jthread thread;
    std::thread::id id;
    std::stop_source stop_source{std::nostopstate};
    bool joining = false;
};
} // namespace Libraries::Kernel
