// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_submit_guard.h"
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
#include <unistd.h>
#include <xxhash.h>
#include "pslayer_write_traps.h"
#include "psport_toggles.h"
#include "common/signal_context.h"
#include "../../src/platform/ps_host.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace PsSubmitGuard {
namespace {
constexpr std::uintptr_t PageSize = 4096;
constexpr auto Reason = PsLayer::WriteTraps::FrameCommands;
// A GPU that waits for a label the CPU writes into these pages would never retire the frame while
// the writer waits here: the write goes through after this.
constexpr auto WaitLimit = std::chrono::seconds{5};

struct Hole {
    std::uint64_t off_hole, cb_hole;
};

struct Range {
    std::uintptr_t begin, end;           // the submission's bytes
    std::uintptr_t page_begin, page_end; // its whole pages, read-only while trapped
    std::uint64_t frame;
    std::vector<Hole> holes; // what the GPU itself writes inside it (labels), left out of the hash
    std::uint64_t hash;
    bool is_hashed, is_trapped;
};

std::mutex s_mutex;
std::condition_variable s_retired_cv;
std::vector<Range> s_armed;
std::uint64_t s_closed = 0, s_retired = 0;
std::atomic<bool> s_is_tripped{false};

enum Mode : unsigned { Off = 0, Traps = 1, Hashes = 2 };
unsigned Modes() {
    static const unsigned s_modes = [] {
        const char* sz_env = std::getenv("PS_SUBMIT_GUARD");
        const std::string_view value = sz_env ? sz_env : "1";
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
    return s_modes;
}

bool GpuSideThread() {
    auto* p_rasterizer = Core::Memory::Instance()->GetRasterizer();
    return p_rasterizer && p_rasterizer->IsGpuSideThread();
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
    const auto add = [&](std::uintptr_t va_range, std::uint64_t cb_range) {
        if (va_range >= range.begin && va_range < range.end) {
            holes.push_back({va_range - range.begin, std::min<std::uint64_t>(cb_range, range.end - va_range)});
        }
    };
    const auto* p_commands = reinterpret_cast<const std::uint32_t*>(range.begin);
    const std::uint64_t dwords = (range.end - range.begin) / 4;
    for (std::uint64_t at = 0; at < dwords;) {
        const auto* p_header = reinterpret_cast<const PM4Type3Header*>(p_commands + at);
        std::uint32_t c_dwords = PacketDwords(p_header->raw);
        if (c_dwords == 0 || at + c_dwords > dwords) {
            c_dwords = 1;
        } else if (p_header->type == 3) {
            switch (p_header->opcode) {
            case PM4ItOpcode::WriteData: {
                const auto* p_packet = reinterpret_cast<const PM4CmdWriteData*>(p_header);
                add(p_packet->Address<std::uintptr_t>(), p_packet->Size());
                break;
            }
            case PM4ItOpcode::EventWriteEop:
                add(reinterpret_cast<std::uintptr_t>(
                        reinterpret_cast<const PM4CmdEventWriteEop*>(p_header)->Address<unsigned char>()),
                    8);
                break;
            case PM4ItOpcode::EventWriteEos:
                add(reinterpret_cast<const PM4CmdEventWriteEos*>(p_header)->Address<std::uintptr_t>(), 4);
                break;
            case PM4ItOpcode::ReleaseMem:
                add(reinterpret_cast<const PM4CmdReleaseMem*>(p_header)->Address<std::uintptr_t>(), 8);
                break;
            default:
                break;
            }
        }
        at += c_dwords;
    }
    std::sort(holes.begin(), holes.end(), [](const Hole& a, const Hole& b) { return a.off_hole < b.off_hole; });
    return holes;
}

// Empty when the buffer cannot be read any more (the game unmapped it): nothing to compare.
std::optional<std::uint64_t> Hash(const Range& range) {
    const auto start = std::chrono::steady_clock::now();
    const auto* p_bytes = reinterpret_cast<const unsigned char*>(range.begin);
    const std::uint64_t cb_range = range.end - range.begin;
    std::uint64_t hash = 0, at = 0;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        g_p_runtime_fault_recover = nullptr;
        return std::nullopt;
    }
    g_p_runtime_fault_recover = &recover;
    for (const Hole& hole : range.holes) {
        if (hole.off_hole > at) {
            hash = XXH3_64bits_withSeed(p_bytes + at, hole.off_hole - at, hash);
        }
        at = std::max(at, hole.off_hole + hole.cb_hole);
    }
    if (at < cb_range) {
        hash = XXH3_64bits_withSeed(p_bytes + at, cb_range - at, hash);
    }
    g_p_runtime_fault_recover = nullptr;
    PsStats::g_submit_guard_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(),
        std::memory_order_relaxed);
    PsStats::g_cb_submit_guard.fetch_add(cb_range, std::memory_order_relaxed);
    return hash;
}

void Trip(const char* sz_what, const Range& range, std::uintptr_t va_guest, const char* sz_detail) {
    if (s_is_tripped.exchange(true)) {
        return;
    }
    std::printf("Gnm: %s the commands of frame %llu at %#llx%s while the GPU ran them: frames no "
                "longer overlap (PS_SUBMIT_AHEAD 0)\n",
                sz_what, (unsigned long long)range.frame, (unsigned long long)va_guest, sz_detail);
}

void Trap(std::uintptr_t begin, std::uintptr_t end, bool is_on) {
    PsLayer::WriteTraps::Set(begin, end - begin, Reason, is_on);
}

// The reason bit is one per page: lifting a range re-arms what later frames still hold there.
void Lift(std::vector<Range>::iterator it) {
    const Range range = std::move(*it);
    s_armed.erase(it);
    if (!range.is_trapped) {
        return;
    }
    Trap(range.page_begin, range.page_end, false);
    for (const auto& other : s_armed) {
        if (other.is_trapped && other.page_begin < range.page_end && range.page_begin < other.page_end) {
            Trap(std::max(other.page_begin, range.page_begin), std::min(other.page_end, range.page_end), true);
        }
    }
}

std::vector<Range>::iterator FindTrapped(std::uintptr_t va_guest) {
    return std::find_if(s_armed.begin(), s_armed.end(), [va_guest](const Range& range) {
        return range.is_trapped && range.page_begin <= va_guest && va_guest < range.page_end;
    });
}

bool OnFault(void* p_context, void* p_fault_address) {
    const auto address = reinterpret_cast<std::uintptr_t>(p_fault_address);
    if (!Common::IsWriteError(p_context) || !(PsLayer::WriteTraps::Reasons(address) & Reason)) {
        return false;
    }
    std::unique_lock lock{s_mutex};
    auto it = FindTrapped(address);
    if (it == s_armed.end()) {
        return true; // lifted meanwhile: the write runs again
    }
    if (GpuSideThread()) {
        Lift(it); // the GPU side's own writes into the game's commands (labels it writes by CPU)
        return true;
    }
    const std::uint64_t frame = it->frame;
    char sz_detail[64];
    std::snprintf(sz_detail, sizeof(sz_detail), " (tid %ld, rip %#llx)", (long)ps_host_thread_id(),
                  (unsigned long long)ps_host_fault_register(static_cast<const PsHostFaultContext*>(p_context), PS_HOST_REG_RIP));
    Trip("the game writes into", *it, address, sz_detail);
    if (frame <= s_closed) {
        if (!s_retired_cv.wait_for(lock, WaitLimit, [&] { return s_retired >= frame; })) {
            std::printf("Gnm: frame %llu not retired after %lld s: its commands are the game's again\n",
                        (unsigned long long)frame, (long long)WaitLimit.count());
        }
    }
    it = FindTrapped(address);
    if (it != s_armed.end()) {
        Lift(it);
    }
    return true;
}
} // namespace

void Arm(const void* p_address, std::uint64_t cb_range, std::uint64_t frame) {
    const unsigned modes = Modes();
    if (modes == Off || cb_range == 0 || s_is_tripped.load(std::memory_order_relaxed)) {
        return;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(p_address);
    Range range{start,
                start + cb_range,
                (start + PageSize - 1) & ~(PageSize - 1),
                (start + cb_range) & ~(PageSize - 1),
                frame,
                {},
                0,
                false,
                false};
    range.is_trapped = (modes & Traps) && range.page_end > range.page_begin;
    if (modes & Hashes) {
        range.holes = GpuWrittenHoles(range);
        if (const auto hash = Hash(range)) {
            range.hash = *hash;
            range.is_hashed = true;
        }
    }
    if (!range.is_trapped && !range.is_hashed) {
        return;
    }
    if (range.is_trapped) {
        static const bool s_is_registered = [] {
            Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 0);
            return true;
        }();
        (void)s_is_registered;
    }
    std::scoped_lock lock{s_mutex};
    if (range.is_trapped) {
        Trap(range.page_begin, range.page_end, true);
    }
    s_armed.push_back(std::move(range));
}

void Close(std::uint64_t frame) {
    std::scoped_lock lock{s_mutex};
    s_closed = std::max(s_closed, frame);
}

void Retire(std::uint64_t frame) {
    std::scoped_lock lock{s_mutex};
    s_retired = std::max(s_retired, frame);
    for (auto it = s_armed.begin(); it != s_armed.end();) {
        if (it->frame > s_retired) {
            ++it;
            continue;
        }
        if (it->is_hashed) {
            if (const auto hash = Hash(*it); hash && *hash != it->hash) {
                Trip("something rewrote", *it, it->begin, " (checksum)");
            }
        }
        Lift(it);
        it = s_armed.begin();
    }
    s_retired_cv.notify_all();
}

bool Tripped() {
    return s_is_tripped.load(std::memory_order_relaxed);
}
} // namespace PsSubmitGuard
