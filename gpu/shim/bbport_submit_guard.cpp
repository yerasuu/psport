// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_submit_guard.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <csetjmp>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <xxhash.h>
#include "bblayer_write_traps.h"
#include "bbport_toggles.h"
#include "common/signal_context.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace BbSubmitGuard {
namespace {
constexpr std::uintptr_t PageSize = 4096;
constexpr auto Reason = BbLayer::WriteTraps::FrameCommands;
// A GPU that waits for a label the CPU writes into these pages would never retire the frame while
// the writer waits here: the write goes through after this.
constexpr auto WaitLimit = std::chrono::seconds{5};

struct Hole {
    std::uint64_t offset, size;
};

struct Range {
    std::uintptr_t begin, end;           // the submission's bytes
    std::uintptr_t page_begin, page_end; // its whole pages, read-only while trapped
    std::uint64_t frame;
    std::vector<Hole> holes; // what the GPU itself writes inside it (labels), left out of the hash
    std::uint64_t hash;
    bool hashed, trapped;
};

std::mutex mutex;
std::condition_variable retired_cv;
std::vector<Range> armed;
std::uint64_t closed = 0, retired = 0;
std::atomic<bool> tripped{false};

enum Mode : unsigned { Off = 0, Traps = 1, Hashes = 2 };
unsigned Modes() {
    static const unsigned modes = [] {
        const char* env = std::getenv("BB_SUBMIT_GUARD");
        const std::string_view value = env ? env : "1";
        if (value == "0") {
            return unsigned(Off);
        }
        if (value == "trap") {
            return unsigned(Traps);
        }
        if (value == "hash") {
            return unsigned(Hashes);
        }
        return unsigned(Traps | Hashes);
    }();
    return modes;
}

bool GpuSideThread() {
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    return rasterizer && rasterizer->IsGpuSideThread();
}

std::uint32_t PacketDwords(std::uint32_t header) {
    switch (header >> 30) {
    case 3:
        return ((header >> 16) & 0x3fff) + 2;
    case 2:
        return 1;
    default:
        return 0;
    }
}

// The fence and WRITE_DATA targets inside the buffer: the GPU writes them while the frame runs,
// so they are not the game's doing. Other GPU writes into its own commands (DMA_DATA, COPY_DATA)
// are not expected and would read as the game's.
std::vector<Hole> GpuWrittenHoles(const Range& range) {
    using namespace AmdGpu;
    std::vector<Hole> holes;
    const auto add = [&](std::uintptr_t address, std::uint64_t size) {
        if (address >= range.begin && address < range.end) {
            holes.push_back({address - range.begin, std::min<std::uint64_t>(size, range.end - address)});
        }
    };
    const auto* commands = reinterpret_cast<const std::uint32_t*>(range.begin);
    const std::uint64_t dwords = (range.end - range.begin) / 4;
    for (std::uint64_t at = 0; at < dwords;) {
        const auto* header = reinterpret_cast<const PM4Type3Header*>(commands + at);
        std::uint32_t n = PacketDwords(header->raw);
        if (n == 0 || at + n > dwords) {
            n = 1;
        } else if (header->type == 3) {
            switch (header->opcode) {
            case PM4ItOpcode::WriteData: {
                const auto* packet = reinterpret_cast<const PM4CmdWriteData*>(header);
                add(packet->Address<std::uintptr_t>(), packet->Size());
                break;
            }
            case PM4ItOpcode::EventWriteEop:
                add(reinterpret_cast<std::uintptr_t>(
                        reinterpret_cast<const PM4CmdEventWriteEop*>(header)->Address<unsigned char>()),
                    8);
                break;
            case PM4ItOpcode::EventWriteEos:
                add(reinterpret_cast<const PM4CmdEventWriteEos*>(header)->Address<std::uintptr_t>(), 4);
                break;
            case PM4ItOpcode::ReleaseMem:
                add(reinterpret_cast<const PM4CmdReleaseMem*>(header)->Address<std::uintptr_t>(), 8);
                break;
            default:
                break;
            }
        }
        at += n;
    }
    std::sort(holes.begin(), holes.end(), [](const Hole& a, const Hole& b) { return a.offset < b.offset; });
    return holes;
}

// Empty when the buffer cannot be read any more (the game unmapped it): nothing to compare.
std::optional<std::uint64_t> Hash(const Range& range) {
    const auto start = std::chrono::steady_clock::now();
    const auto* bytes = reinterpret_cast<const unsigned char*>(range.begin);
    const std::uint64_t size = range.end - range.begin;
    std::uint64_t hash = 0, at = 0;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        return std::nullopt;
    }
    runtime_fault_recover = &recover;
    for (const Hole& hole : range.holes) {
        if (hole.offset > at) {
            hash = XXH3_64bits_withSeed(bytes + at, hole.offset - at, hash);
        }
        at = std::max(at, hole.offset + hole.size);
    }
    if (at < size) {
        hash = XXH3_64bits_withSeed(bytes + at, size - at, hash);
    }
    runtime_fault_recover = nullptr;
    BbStats::submit_guard_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(),
        std::memory_order_relaxed);
    BbStats::submit_guard_bytes.fetch_add(size, std::memory_order_relaxed);
    return hash;
}

void Trip(const char* what, const Range& range, std::uintptr_t address, const char* detail) {
    if (tripped.exchange(true)) {
        return;
    }
    std::printf("Gnm: %s the commands of frame %llu at %#llx%s while the GPU ran them: frames no "
                "longer overlap (BB_SUBMIT_AHEAD 0)\n",
                what, (unsigned long long)range.frame, (unsigned long long)address, detail);
}

void Trap(std::uintptr_t begin, std::uintptr_t end, bool on) {
    BbLayer::WriteTraps::Set(begin, end - begin, Reason, on);
}

// The reason bit is one per page: lifting a range re-arms what later frames still hold there.
void Lift(std::vector<Range>::iterator it) {
    const Range range = std::move(*it);
    armed.erase(it);
    if (!range.trapped) {
        return;
    }
    Trap(range.page_begin, range.page_end, false);
    for (const auto& other : armed) {
        if (other.trapped && other.page_begin < range.page_end && range.page_begin < other.page_end) {
            Trap(std::max(other.page_begin, range.page_begin), std::min(other.page_end, range.page_end), true);
        }
    }
}

std::vector<Range>::iterator FindTrapped(std::uintptr_t address) {
    return std::find_if(armed.begin(), armed.end(), [address](const Range& range) {
        return range.trapped && range.page_begin <= address && address < range.page_end;
    });
}

bool OnFault(void* context, void* fault_address) {
    const auto address = reinterpret_cast<std::uintptr_t>(fault_address);
    if (!Common::IsWriteError(context) || !(BbLayer::WriteTraps::Reasons(address) & Reason)) {
        return false;
    }
    std::unique_lock lock{mutex};
    auto it = FindTrapped(address);
    if (it == armed.end()) {
        return true; // lifted meanwhile: the write runs again
    }
    if (GpuSideThread()) {
        Lift(it); // the GPU side's own writes into the game's commands (labels it writes by CPU)
        return true;
    }
    const std::uint64_t frame = it->frame;
    char detail[64];
    std::snprintf(detail, sizeof(detail), " (tid %ld, rip %#llx)", (long)syscall(SYS_gettid),
                  (unsigned long long)static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
    Trip("the game writes into", *it, address, detail);
    if (frame <= closed) {
        if (!retired_cv.wait_for(lock, WaitLimit, [&] { return retired >= frame; })) {
            std::printf("Gnm: frame %llu not retired after %lld s: its commands are the game's again\n",
                        (unsigned long long)frame, (long long)WaitLimit.count());
        }
    }
    it = FindTrapped(address);
    if (it != armed.end()) {
        Lift(it);
    }
    return true;
}
} // namespace

void Arm(const void* address, std::uint64_t size, std::uint64_t frame) {
    const unsigned modes = Modes();
    if (modes == Off || size == 0 || tripped.load(std::memory_order_relaxed)) {
        return;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    Range range{start,
                start + size,
                (start + PageSize - 1) & ~(PageSize - 1),
                (start + size) & ~(PageSize - 1),
                frame,
                {},
                0,
                false,
                false};
    range.trapped = (modes & Traps) && range.page_end > range.page_begin;
    if (modes & Hashes) {
        range.holes = GpuWrittenHoles(range);
        if (const auto hash = Hash(range)) {
            range.hash = *hash;
            range.hashed = true;
        }
    }
    if (!range.trapped && !range.hashed) {
        return;
    }
    if (range.trapped) {
        static const bool registered = [] {
            Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 0);
            return true;
        }();
        (void)registered;
    }
    std::scoped_lock lock{mutex};
    if (range.trapped) {
        Trap(range.page_begin, range.page_end, true);
    }
    armed.push_back(std::move(range));
}

void Close(std::uint64_t frame) {
    std::scoped_lock lock{mutex};
    closed = std::max(closed, frame);
}

void Retire(std::uint64_t frame) {
    std::scoped_lock lock{mutex};
    retired = std::max(retired, frame);
    for (auto it = armed.begin(); it != armed.end();) {
        if (it->frame > retired) {
            ++it;
            continue;
        }
        if (it->hashed) {
            if (const auto hash = Hash(*it); hash && *hash != it->hash) {
                Trip("something rewrote", *it, it->begin, " (checksum)");
            }
        }
        Lift(it);
        it = armed.begin();
    }
    retired_cv.notify_all();
}

bool Tripped() {
    return tripped.load(std::memory_order_relaxed);
}
} // namespace BbSubmitGuard
