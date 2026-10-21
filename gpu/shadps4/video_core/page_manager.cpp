// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "psport_toggles.h"
#include "pslayer_write_traps.h"
#include "psport_guest_memory.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "../../src/platform/ps_host.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <string>
#include <fmt/format.h>
#include <unistd.h>
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

extern "C" void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch);
extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

namespace {
/// psport: write fault sites (guest code), an open-addressing table keyed by the instruction.
struct FaultSite {
    std::atomic<u64> rip{0};
    std::atomic<u64> caller{0};
    std::atomic<u64> count{0};
    std::atomic<u64> last_address{0};
};
std::array<FaultSite, 512> fault_sites;
std::atomic<u64> fault_sites_dropped{0};
constexpr u64 GuestImage = 0x800000000ull, GuestImageEnd = 0x810000000ull;
constexpr u64 HostSite = 1ull << 63;
constexpr u64 ReadSite = 1ull << 62; ///< a read fault (PS_READBACKS=2 protects GPU data from reads)

/// "+offset" for guest code, the symbol for host code.
std::string SiteName(u64 key) {
    const char* access = key & ReadSite ? "read:" : "";
    key &= ~ReadSite;
    if (!(key & HostSite)) {
        return fmt::format("{}+{:#x}", access, key);
    }
    const u64 address = key & ~HostSite;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_sname) {
        return fmt::format("{}host:{}+{:#x}", access, info.dli_sname, address - u64(info.dli_saddr));
    }
    if (info.dli_fname) {
        return fmt::format("{}host:{}+{:#x}", access, info.dli_fname, address - u64(info.dli_fbase));
    }
    return fmt::format("{}host:{:#x}", access, address);
}

/// The write fault this thread is handling (guest offsets; 0: none).
thread_local u64 current_fault_rip = 0, current_fault_caller = 0;

/// Guest code that wrote image (texture) memory: keyed by instruction and image.
struct ImageFaultSite {
    std::atomic<u64> key{0}; // a mix of the site and the image address
    std::atomic<u64> rip{0}, caller{0}, count{0}, image{0}, size{0};
    std::atomic<u32> width{0}, height{0}, format{0}, tiling{0};
};
std::array<ImageFaultSite, 256> image_fault_sites;

void NoteFaultSite(void* context, VAddr address) {
    const auto* p_context = static_cast<const PsHostFaultContext*>(context);
    current_fault_rip = 0;
    const u64 rip = ps_host_fault_register(p_context, PS_HOST_REG_RIP);
    const bool guest_code = rip >= GuestImage && rip < GuestImageEnd;
    u64 caller = 0;
    if (guest_code) {
        // The caller: [rbp + 8] when the guest code keeps frames (its memcpy-like leaves do not).
        u64 saved[2] = {};
        if (ps_host_read_memory(saved, reinterpret_cast<const void*>(ps_host_fault_register(p_context, PS_HOST_REG_RBP)),
                                sizeof(saved)) == std::int64_t(sizeof(saved)) &&
            saved[1] >= GuestImage && saved[1] < GuestImageEnd) {
            caller = saved[1];
        }
    } else {
        // Host code (a libc import the port runs natively, the runtime): the first guest return
        // address on the stack is its guest caller.
        std::array<u64, 64> stack{};
        const std::int64_t got = ps_host_read_memory(
            stack.data(), reinterpret_cast<const void*>(ps_host_fault_register(p_context, PS_HOST_REG_RSP)), sizeof(stack));
        for (std::int64_t i = 0; i < got / 8; ++i) {
            if (stack[i] >= GuestImage && stack[i] < GuestImageEnd) {
                caller = stack[i];
                break;
            }
        }
    }
    // Guest code: its offset in the image; host code: its address with HostSite set.
    const u64 key = (guest_code ? rip - GuestImage : rip | HostSite) |
                    (Common::IsWriteError(context) ? 0 : ReadSite);
    current_fault_rip = key;
    current_fault_caller = caller ? caller - GuestImage : 0;
    for (u64 i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 55; i < fault_sites.size(); ++i) {
        auto& site = fault_sites[(slot + i) % fault_sites.size()];
        u64 expected = 0;
        if (site.rip.load(std::memory_order_relaxed) == key ||
            site.rip.compare_exchange_strong(expected, key)) {
            site.caller.store(caller ? caller - GuestImage : 0, std::memory_order_relaxed);
            site.last_address.store(address, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    fault_sites_dropped.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

static void ReportImageFaultSites() {
    bool any = false;
    for (auto& site : image_fault_sites) {
        const u64 count = site.count.exchange(0, std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        std::printf("%s %s (from +%#llx) %llu into image %#llx size %llu %ux%u fmt %u tile %u",
                    any ? ";" : "Image write sites (guest code -> texture, this window):",
                    SiteName(site.rip.load()).c_str(), (unsigned long long)site.caller.load(),
                    (unsigned long long)count, (unsigned long long)site.image.load(),
                    (unsigned long long)site.size.load(), site.width.load(), site.height.load(),
                    site.format.load(), site.tiling.load());
        any = true;
    }
    if (any) {
        std::printf("\n");
    }
}

void PageManager::NoteImageFault(VAddr image_address, u64 image_size, u32 width, u32 height,
                                 u32 format, u32 tiling) {
    if (current_fault_rip == 0) {
        return;
    }
    const u64 key = (current_fault_rip * 0x9E3779B97F4A7C15ull) ^ image_address;
    for (u64 i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 56; i < image_fault_sites.size(); ++i) {
        auto& site = image_fault_sites[(slot + i) % image_fault_sites.size()];
        u64 expected = 0;
        if (site.key.load(std::memory_order_relaxed) == key ||
            site.key.compare_exchange_strong(expected, key)) {
            site.rip.store(current_fault_rip, std::memory_order_relaxed);
            site.caller.store(current_fault_caller, std::memory_order_relaxed);
            site.image.store(image_address, std::memory_order_relaxed);
            site.size.store(image_size, std::memory_order_relaxed);
            site.width.store(width, std::memory_order_relaxed);
            site.height.store(height, std::memory_order_relaxed);
            site.format.store(format, std::memory_order_relaxed);
            site.tiling.store(tiling, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void PageManager::ReportFaultSites() {
    struct Row {
        u64 rip, caller, count, address;
    };
    std::array<Row, 512> rows{};
    std::size_t n = 0;
    u64 total = 0;
    for (auto& site : fault_sites) {
        const u64 count = site.count.exchange(0, std::memory_order_relaxed);
        if (count != 0) {
            rows[n++] = {site.rip.load(), site.caller.load(), count, site.last_address.load()};
            total += count;
        }
    }
    if (total == 0) {
        return;
    }
    ReportImageFaultSites();
    std::sort(rows.begin(), rows.begin() + n, [](const Row& a, const Row& b) { return a.count > b.count; });
    std::printf("Write fault sites (guest code, this window): %llu faults", (unsigned long long)total);
    for (std::size_t i = 0; i < std::min<std::size_t>(n, 12); ++i) {
        std::printf("%s %s (from +%#llx) %llu at %#llx", i ? ";" : ":",
                    SiteName(rows[i].rip).c_str(), (unsigned long long)rows[i].caller,
                    (unsigned long long)rows[i].count, (unsigned long long)rows[i].address);
    }
    std::printf("\n");
}

// psport: with the game's memory in place, the pages under images are
// trapped for writes (PsLayer::WriteTraps::Image): a write nobody announced (the game's own copy
// loops, its heap over an old texture) still reaches the texture cache, on any game and GPU,
// without hooks in the game's code; the hooks and the libc wrappers only make it sooner.
// PS_IMAGE_TRAPS=0: announced writes only, as before. Latched at the first use: the counts must
// match their traps for the whole run.
static bool ImageTraps() {
    static const bool on = [] {
        const char* env = std::getenv("PS_IMAGE_TRAPS");
        const bool enabled = !(env && env[0] == '0') && !VideoCore::WriteVerify();
        if (enabled) {
            std::printf("Guest memory: writes under images are trapped (PS_IMAGE_TRAPS=0: "
                        "announced writes only)\n");
        }
        return enabled;
    }();
    return on;
}

// psport: as ImageTraps for the pages the buffer cache watches (VRAM copies in its arena): a write
// nobody announced reaches the copy instead of the GPU reading the older data uploaded there.
static bool BufferTraps() {
    static const bool on = ImageTraps() && !PsGuestMemory::LayerMemory();
    return on;
}

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        NoteFaultSite(context, addr);
        // psport: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            PsStats::g_c_gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        // Traps on reads too (no access, PsLayer::WriteTraps::NoAccess): their owner first.
        if (PsLayer::WriteTraps::Reasons(addr) & PsLayer::WriteTraps::VramData) {
            return rasterizer->OnVramDataAccess(addr, is_gpu_thread);
        }
        if (PsLayer::WriteTraps::Reasons(addr) & PsLayer::WriteTraps::QueryReads) {
            return rasterizer->OnOcclusionPageAccess(addr, u64(Common::GetRip(context)),
                                                     Common::IsWriteError(context), is_gpu_thread);
        }
        if (Common::IsWriteError(context)) {
            if (ImageTraps() && !(PsLayer::WriteTraps::Reasons(addr) &
                                  (PsLayer::WriteTraps::Image | PsLayer::WriteTraps::Buffer))) {
                // Not an image's trap: another owner's being lifted by another thread, or one
                // lifted meanwhile (the write runs again), or a write the game may not do.
                int prot = 0, type = -1;
                uintptr_t end = 0;
                return runtime_memory_vma_info(addr, &prot, &type, &end) && (prot & 0x2);
            }
            PsStats::Timer timer{PsStats::g_t_write_faults};
            const bool handled = rasterizer->OnWriteFault(addr, is_gpu_thread, u64(Common::GetRip(context)));
            current_fault_rip = 0;
            return handled;
        } else {
            PsStats::g_c_read_faults.fetch_add(1, std::memory_order_relaxed);
            PsStats::Timer timer{PsStats::g_t_read_faults};
            const auto size = std::min<u64>(8, PageManager::GetNextPageAddr(addr) - addr);
            return rasterizer->ReadMemory(addr, size, is_gpu_thread);
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    /// psport: the owners over each page for one trap reason; a page is trapped while one is there.
    /// Not the page states: they count images and buffer watches together, so one's last watch
    /// going would lift or leave the other's trap (left, the write faults again forever).
    struct TrapOwners {
        u16* counts = nullptr;
        std::once_flag once;
    };

    template <bool track, typename ForEachPage>
    static void UpdateTraps(TrapOwners& owners, PsLayer::WriteTraps::Reason reason,
                            ForEachPage&& for_each_page) {
        std::call_once(owners.once, [&owners] {
            void* mem = ps_host_vm_map(nullptr, NUM_ADDRESS_PAGES * sizeof(u16), PS_HOST_VM_READ | PS_HOST_VM_WRITE,
                                       PS_HOST_VM_SPARSE);
            ASSERT_MSG(mem != nullptr, "trap owner table");
            owners.counts = static_cast<u16*>(mem);
        });
        // Runs of pages whose first owner came or last one went: one trap change each.
        u64 run_begin = 0, run_pages = 0;
        const auto flush = [&] {
            if (run_pages != 0) {
                PsLayer::WriteTraps::Set(run_begin << PM_PAGE_BITS, run_pages << PM_PAGE_BITS,
                                         reason, track);
                run_pages = 0;
            }
        };
        for_each_page([&](u64 page) {
            u16& count = owners.counts[page];
            bool change;
            if constexpr (track) {
                change = count++ == 0;
            } else {
                ASSERT_MSG(count > 0, "trap owners below zero");
                change = --count == 0;
            }
            if (!change) {
                return;
            }
            if (run_pages != 0 && run_begin + run_pages == page) {
                ++run_pages;
            } else {
                flush();
                run_begin = page;
                run_pages = 1;
            }
        });
        flush();
    }

    /// psport ImageTraps: the images over each page.
    template <bool track>
    void UpdateImageTraps(VAddr addr, u64 size) {
        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        UpdateTraps<track>(image_owners, PsLayer::WriteTraps::Image, [&](auto&& visit) {
            for (; page != page_end; ++page) {
                visit(page);
            }
        });
    }

    /// psport BufferTraps: the buffer cache's watches (writes and readbacks) over each page.
    template <bool track>
    void UpdateBufferTraps(VAddr base_addr, RegionBits& mask) {
        const size_t base_page = base_addr >> PM_PAGE_BITS;
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        UpdateTraps<track>(buffer_owners, PsLayer::WriteTraps::Buffer, [&](auto&& visit) {
            for (size_t page = mask.FirstRange().first; page < mask.LastRange().second; ++page) {
                if (mask.Get(page)) {
                    visit(base_page + page);
                }
            }
        });
    }
    TrapOwners image_owners;
    TrapOwners buffer_owners;

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first (psport: after PS_VRAM_ACCESS_TRAP, priority 0, diagnostics only).
        constexpr u32 priority = 1;
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        // psport: no page protection; the pages are marked in the runtime's write watch instead,
        // and the writers that check it tell the GPU side.
        runtime_memory_set_write_watch(address, size, !True(perms & Core::MemoryPermission::Write));
        if (!VideoCore::WriteVerify()) {
            return;
        }
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, false>(addr, size);
    // psport: only the texture cache comes here (the buffer cache: UpdatePageWatchersForRegion).
    if (ImageTraps()) {
        impl->UpdateImageTraps<track>(addr, size);
    }
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
    if (BufferTraps()) {
        impl->UpdateBufferTraps<track>(base_addr, mask);
    }
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
