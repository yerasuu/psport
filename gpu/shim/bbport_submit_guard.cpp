// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_submit_guard.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include "bblayer_write_traps.h"
#include "common/signal_context.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace BbSubmitGuard {
namespace {
constexpr std::uintptr_t PageSize = 4096;
constexpr auto Reason = BbLayer::WriteTraps::FrameCommands;
// A GPU that waits for a label the CPU writes into these pages would never retire the frame while
// the writer waits here: the write goes through after this.
constexpr auto WaitLimit = std::chrono::seconds{5};

struct Range {
    std::uintptr_t begin, end;
    std::uint64_t frame;
};

std::mutex mutex;
std::condition_variable retired_cv;
std::vector<Range> armed;
std::uint64_t closed = 0, retired = 0;
std::atomic<bool> tripped{false};

bool On() {
    static const bool on = [] {
        const char* env = std::getenv("BB_SUBMIT_GUARD");
        return !env || env[0] != '0';
    }();
    return on;
}

bool GpuSideThread() {
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    return rasterizer && rasterizer->IsGpuSideThread();
}

void Trap(const Range& range, bool on) {
    BbLayer::WriteTraps::Set(range.begin, range.end - range.begin, Reason, on);
}

// The reason bit is one per page: lifting a range re-arms what later frames still hold there.
void Lift(std::vector<Range>::iterator it) {
    const Range range = *it;
    armed.erase(it);
    Trap(range, false);
    for (const auto& other : armed) {
        if (other.begin < range.end && range.begin < other.end) {
            Trap({std::max(other.begin, range.begin), std::min(other.end, range.end), other.frame},
                 true);
        }
    }
}

std::vector<Range>::iterator Find(std::uintptr_t address) {
    return std::find_if(armed.begin(), armed.end(), [address](const Range& range) {
        return range.begin <= address && address < range.end;
    });
}

bool OnFault(void* context, void* fault_address) {
    const auto address = reinterpret_cast<std::uintptr_t>(fault_address);
    if (!Common::IsWriteError(context) || !(BbLayer::WriteTraps::Reasons(address) & Reason)) {
        return false;
    }
    std::unique_lock lock{mutex};
    auto it = Find(address);
    if (it == armed.end()) {
        return true; // lifted meanwhile: the write runs again
    }
    if (GpuSideThread()) {
        Lift(it); // the GPU side's own writes into the game's commands (labels it writes by CPU)
        return true;
    }
    const Range range = *it;
    const auto* g = static_cast<const ucontext_t*>(context)->uc_mcontext.gregs;
    if (!tripped.exchange(true)) {
        std::printf("Gnm: the game writes into the commands of frame %llu at %#llx (tid %ld, rip "
                    "%#llx) while the GPU runs it: frames no longer overlap (BB_SUBMIT_AHEAD 0)\n",
                    (unsigned long long)range.frame, (unsigned long long)address,
                    (long)syscall(SYS_gettid), (unsigned long long)g[REG_RIP]);
    }
    if (range.frame <= closed) {
        if (!retired_cv.wait_for(lock, WaitLimit, [&] { return retired >= range.frame; })) {
            std::printf("Gnm: frame %llu not retired after %lld s: its commands are the game's "
                        "again\n",
                        (unsigned long long)range.frame, (long long)WaitLimit.count());
        }
    }
    it = Find(address);
    if (it != armed.end()) {
        Lift(it);
    }
    return true;
}
} // namespace

void Arm(const void* address, std::uint64_t size, std::uint64_t frame) {
    if (!On() || tripped.load(std::memory_order_relaxed)) {
        return;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const Range range{(start + PageSize - 1) & ~(PageSize - 1), (start + size) & ~(PageSize - 1),
                      frame};
    if (range.end <= range.begin) {
        return;
    }
    static const bool registered = [] {
        Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 0);
        return true;
    }();
    (void)registered;
    std::scoped_lock lock{mutex};
    armed.push_back(range);
    Trap(range, true);
}

void Close(std::uint64_t frame) {
    std::scoped_lock lock{mutex};
    closed = std::max(closed, frame);
}

void Retire(std::uint64_t frame) {
    std::scoped_lock lock{mutex};
    retired = std::max(retired, frame);
    for (auto it = armed.begin(); it != armed.end();) {
        if (it->frame <= retired) {
            Lift(it);
            it = armed.begin();
        } else {
            ++it;
        }
    }
    retired_cv.notify_all();
}

bool Tripped() {
    return tripped.load(std::memory_order_relaxed);
}
} // namespace BbSubmitGuard
