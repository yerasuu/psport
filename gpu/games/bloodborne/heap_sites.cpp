// SPDX-License-Identifier: GPL-2.0-or-later
// psport PS_HEAP_SITES=1 (diagnostics): the game's malloc replacement (the pointers libc.prx calls
// through, libc+0xba190: malloc, free, calloc, realloc, memalign, posix_memalign) is wrapped, and
// the live allocations are counted per call site in the game (the first two return addresses into
// its code on the stack). Report() prints the sites whose live bytes grew since the last report:
// what leaks when the guest heap runs out.
#include "heap_sites.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" std::uint64_t runtime_heap_growths(void);

namespace PsHeapSites {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

constexpr u64 ImageBase = 0x800000000ull;
constexpr u64 TextEnd = ImageBase + 0x50d96dcull;
constexpr u64 LibcBase = ImageBase + 0x56e0000ull;
constexpr u64 SlotMalloc = 0xba190, SlotFree = 0xba198, SlotCalloc = 0xba1a0,
              SlotRealloc = 0xba1a8, SlotMemalign = 0xba1b0, SlotPosixMemalign = 0xba1b8;

using MallocFn = void* (*)(std::size_t);
using FreeFn = void (*)(void*);
using CallocFn = void* (*)(std::size_t, std::size_t);
using ReallocFn = void* (*)(void*, std::size_t);
using MemalignFn = void* (*)(std::size_t, std::size_t);
using PosixMemalignFn = int (*)(void**, std::size_t, std::size_t);

MallocFn s_pfn_orig_malloc;
FreeFn s_pfn_orig_free;
CallocFn s_pfn_orig_calloc;
ReallocFn s_pfn_orig_realloc;
MemalignFn s_pfn_orig_memalign;
PosixMemalignFn s_pfn_orig_posix_memalign;

struct Site {
    u64 a = 0, b = 0; ///< return addresses into the game (image offsets); a == 0: empty slot
    s64 live = 0, cb_live = 0;
    u64 allocs = 0;
    s64 cb_reported = 0, reported_live = 0;
    u64 free_chain[5] = {}; ///< where an allocation of this site was freed last
};
constexpr u64 SiteCount = 1u << 14;
std::vector<Site> s_sites;

struct Alloc {
    u64 ptr; ///< 0 empty, 1 removed
    u32 site;
    u32 cb_allocation;
};
constexpr u64 AllocBits = 22; // 4M live allocations
std::vector<Alloc> s_allocs;
u64 s_untracked_frees = 0, s_table_full = 0;

std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
bool s_is_tracking = false; ///< PS_HEAP_SITES=1: per call site; else only the counters below
std::atomic<u64> s_c_count_allocs{0}, s_c_count_frees{0};
std::atomic<u64> s_release_ok{0}, s_release_busy{0}, s_release_missing{0}, s_release_other{0};
struct Guard {
    Guard() {
        while (s_lock.test_and_set(std::memory_order_acquire)) {
            __builtin_ia32_pause();
        }
    }
    ~Guard() {
        s_lock.clear(std::memory_order_release);
    }
};

u64 Hash(u64 v, u64 bits) {
    return (v * 0x9E3779B97F4A7C15ull) >> (64 - bits);
}

/// The first return addresses into the game's code above `frame` (up to `count`).
void CallChain(const u64* p_frame, u64* p_out, int c_frames) {
    int c_frames_found = 0;
    for (int i = 0; i < 160 && c_frames_found < c_frames; ++i) {
        const u64 v = p_frame[i];
        if (v > ImageBase && v < TextEnd) {
            p_out[c_frames_found++] = v - ImageBase;
        }
    }
    for (; c_frames_found < c_frames; ++c_frames_found) {
        p_out[c_frames_found] = 0;
    }
}
void CallSites(const u64* p_frame, u64& a, u64& b) {
    u64 chain[2];
    CallChain(p_frame, chain, 2);
    a = chain[0];
    b = chain[1];
}

u32 SiteIndex(u64 a, u64 b) {
    const u64 key = a * 31 + b + 1;
    for (u64 i = Hash(key, 14), c_probes = 0; c_probes < SiteCount; ++c_probes, i = (i + 1) & (SiteCount - 1)) {
        Site& site = s_sites[i];
        if (site.a == a + 1 && site.b == b) {
            return u32(i);
        }
        if (site.a == 0) {
            site.a = a + 1;
            site.b = b;
            return u32(i);
        }
    }
    return 0;
}

void Add(void* p_ptr, std::size_t cb_allocation, const u64* p_frame) {
    u64 a, b;
    CallSites(p_frame, a, b);
    Guard guard;
    const u32 i_site = SiteIndex(a, b);
    Site& site = s_sites[i_site];
    ++site.live;
    site.cb_live += s64(cb_allocation);
    ++site.allocs;
    const u64 p = reinterpret_cast<u64>(p_ptr);
    const u64 mask = (1ull << AllocBits) - 1;
    for (u64 i = Hash(p, AllocBits), c_probes = 0; c_probes < 64; ++c_probes, i = (i + 1) & mask) {
        if (s_allocs[i].ptr <= 1) {
            s_allocs[i] = {p, i_site, u32(std::min<std::size_t>(cb_allocation, 0xffffffffu))};
            return;
        }
    }
    ++s_table_full;
}

void Remove(void* p_ptr, const u64* p_frame) {
    u64 chain[5];
    CallChain(p_frame, chain, 5);
    const u64 p = reinterpret_cast<u64>(p_ptr);
    const u64 mask = (1ull << AllocBits) - 1;
    Guard guard;
    for (u64 i = Hash(p, AllocBits), c_probes = 0; c_probes < 64; ++c_probes, i = (i + 1) & mask) {
        if (s_allocs[i].ptr == p) {
            Site& site = s_sites[s_allocs[i].site];
            --site.live;
            std::copy_n(chain, 5, site.free_chain);
            site.cb_live -= s64(s_allocs[i].cb_allocation);
            s_allocs[i].ptr = 1;
            return;
        }
        if (s_allocs[i].ptr == 0) {
            break;
        }
    }
    ++s_untracked_frees;
}

void* WrapMalloc(std::size_t cb_allocation) {
    void* p_allocation = s_pfn_orig_malloc(cb_allocation);
    if (p_allocation && (s_c_count_allocs.fetch_add(1, std::memory_order_relaxed), s_is_tracking)) {
        Add(p_allocation, cb_allocation, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return p_allocation;
}
void WrapFree(void* p_allocation) {
    if (p_allocation && (s_c_count_frees.fetch_add(1, std::memory_order_relaxed), s_is_tracking)) {
        Remove(p_allocation, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    s_pfn_orig_free(p_allocation);
}
void* WrapCalloc(std::size_t c_elements, std::size_t cb_element) {
    void* p_allocation = s_pfn_orig_calloc(c_elements, cb_element);
    if (p_allocation && (s_c_count_allocs.fetch_add(1, std::memory_order_relaxed), s_is_tracking)) {
        Add(p_allocation, c_elements * cb_element, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return p_allocation;
}
void* WrapRealloc(void* p_old, std::size_t cb_allocation) {
    void* p_allocation = s_pfn_orig_realloc(p_old, cb_allocation);
    if (!p_old && p_allocation) {
        s_c_count_allocs.fetch_add(1, std::memory_order_relaxed);
    } else if (p_old && !p_allocation && cb_allocation == 0) {
        s_c_count_frees.fetch_add(1, std::memory_order_relaxed);
    }
    if (s_is_tracking && (p_allocation || cb_allocation == 0)) {
        if (p_old) {
            Remove(p_old, static_cast<const u64*>(__builtin_frame_address(0)));
        }
        if (p_allocation) {
            Add(p_allocation, cb_allocation, static_cast<const u64*>(__builtin_frame_address(0)));
        }
    }
    return p_allocation;
}
void* WrapMemalign(std::size_t alignment, std::size_t cb_allocation) {
    void* p_allocation = s_pfn_orig_memalign(alignment, cb_allocation);
    if (p_allocation && (s_c_count_allocs.fetch_add(1, std::memory_order_relaxed), s_is_tracking)) {
        Add(p_allocation, cb_allocation, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return p_allocation;
}
int WrapPosixMemalign(void** pp_out, std::size_t alignment, std::size_t cb_allocation) {
    const int result = s_pfn_orig_posix_memalign(pp_out, alignment, cb_allocation);
    if (result == 0 && pp_out && *pp_out &&
        (s_c_count_allocs.fetch_add(1, std::memory_order_relaxed), s_is_tracking)) {
        Add(*pp_out, cb_allocation, static_cast<const u64*>(__builtin_frame_address(0)));
    }
    return result;
}

bool InGame(u64 v) {
    return v >= ImageBase && v < TextEnd;
}
} // namespace

namespace {
void InstallWrappers(bool should_count) {
    static const bool s_is_wanted = [] {
        const char* sz_env = std::getenv("PS_HEAP_SITES");
        return sz_env && sz_env[0] == '1';
    }();
    static bool s_is_done = false;
    if ((!s_is_wanted && !should_count) || s_is_done) {
        return;
    }
    auto* p_slots = reinterpret_cast<u64*>(LibcBase);
    if (p_slots[SlotMalloc / 8] == 0) {
        return; // libc.prx has not set up the game's malloc yet: tried again later
    }
    s_is_done = true;
    const u64 targets[6] = {p_slots[SlotMalloc / 8], p_slots[SlotFree / 8], p_slots[SlotCalloc / 8],
                            p_slots[SlotRealloc / 8], p_slots[SlotMemalign / 8],
                            p_slots[SlotPosixMemalign / 8]};
    for (const u64 target : targets) {
        if (!InGame(target)) {
            std::printf("Heap sites: libc's malloc table not as expected (%#llx); not installed\n",
                        (unsigned long long)target);
            return;
        }
    }
    if (s_is_wanted) {
        s_sites.resize(SiteCount);
        s_allocs.resize(1ull << AllocBits);
        s_is_tracking = true;
    }
    s_pfn_orig_malloc = reinterpret_cast<MallocFn>(targets[0]);
    s_pfn_orig_free = reinterpret_cast<FreeFn>(targets[1]);
    s_pfn_orig_calloc = reinterpret_cast<CallocFn>(targets[2]);
    s_pfn_orig_realloc = reinterpret_cast<ReallocFn>(targets[3]);
    s_pfn_orig_memalign = reinterpret_cast<MemalignFn>(targets[4]);
    s_pfn_orig_posix_memalign = reinterpret_cast<PosixMemalignFn>(targets[5]);
    __atomic_store_n(&p_slots[SlotMalloc / 8], reinterpret_cast<u64>(&WrapMalloc), __ATOMIC_SEQ_CST);
    __atomic_store_n(&p_slots[SlotFree / 8], reinterpret_cast<u64>(&WrapFree), __ATOMIC_SEQ_CST);
    __atomic_store_n(&p_slots[SlotCalloc / 8], reinterpret_cast<u64>(&WrapCalloc), __ATOMIC_SEQ_CST);
    __atomic_store_n(&p_slots[SlotRealloc / 8], reinterpret_cast<u64>(&WrapRealloc), __ATOMIC_SEQ_CST);
    __atomic_store_n(&p_slots[SlotMemalign / 8], reinterpret_cast<u64>(&WrapMemalign),
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&p_slots[SlotPosixMemalign / 8], reinterpret_cast<u64>(&WrapPosixMemalign),
                     __ATOMIC_SEQ_CST);
    std::printf("Heap sites: the game's malloc (+%#llx) and free (+%#llx) are counted%s\n",
                (unsigned long long)(targets[0] - ImageBase),
                (unsigned long long)(targets[1] - ImageBase), s_is_tracking ? " per call site" : "");
}
} // namespace

void Install() {
    InstallWrappers(false);
}

// The game's live heap allocations (malloc minus free, counted by the wrappers) are watched; the
// leak added ~1800 a second, so 100000 over the lowest count seen is taken as one (or the heap
// asking for more memory, which the game never does otherwise).
bool OverlapLeaks(char* sz_reason, std::size_t cb_reason) {
    if (s_pfn_orig_malloc == nullptr) {
        InstallWrappers(true);
        return false;
    }
    static u64 s_frames = 0;
    static long long s_lowest = 0;
    const long long live = s64(s_c_count_allocs.load(std::memory_order_relaxed)) -
                           s64(s_c_count_frees.load(std::memory_order_relaxed));
    if (++s_frames < 600 || live < s_lowest) {
        s_lowest = live; // warming up: area loads
        return false;
    }
    if (live - s_lowest > 100000 || runtime_heap_growths() != 0) {
        std::snprintf(sz_reason, cb_reason, "the game's heap grows (%lld allocations over %lld)",
                      live - s_lowest, s_lowest);
        return true;
    }
    return false;
}

void NoteReleaseCheck(unsigned result) {
    if (s64(std::int32_t(result)) >= 0) {
        ++s_release_ok;
    } else if (result == 0x80004005u) {
        ++s_release_busy;
    } else if (result == 0x80000003u) {
        ++s_release_missing;
    } else {
        ++s_release_other;
    }
}

void Report() {
    Install();
    if (s_sites.empty()) {
        return;
    }
    std::printf("Heap sites: release checks %llu ok, %llu manager busy, %llu not registered, %llu "
                "other\n",
                (unsigned long long)s_release_ok.exchange(0), (unsigned long long)s_release_busy.exchange(0),
                (unsigned long long)s_release_missing.exchange(0),
                (unsigned long long)s_release_other.exchange(0));
    struct Row {
        u64 a, b;
        s64 grew, live_grew, live, cb_live;
        u64 allocs;
        u64 free_chain[5];
    };
    std::vector<Row> rows;
    s64 cb_total = 0, total_live = 0;
    u64 frees_unknown, full;
    {
        Guard guard;
        for (auto& site : s_sites) {
            if (!site.a) {
                continue;
            }
            cb_total += site.cb_live;
            total_live += site.live;
            if (site.cb_live != site.cb_reported || site.live != site.reported_live) {
                rows.push_back({site.a - 1, site.b, site.cb_live - site.cb_reported,
                                site.live - site.reported_live, site.live, site.cb_live, site.allocs,
                                {site.free_chain[0], site.free_chain[1], site.free_chain[2],
                                 site.free_chain[3], site.free_chain[4]}});
            }
            site.cb_reported = site.cb_live;
            site.reported_live = site.live;
        }
        frees_unknown = s_untracked_frees;
        full = s_table_full;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.grew > y.grew; });
    std::printf("Heap sites: %.1f MB live in %lld allocations (%llu frees of untracked, %llu not "
                "stored); grew most:",
                cb_total / 1e6, (long long)total_live, (unsigned long long)frees_unknown,
                (unsigned long long)full);
    for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 8); ++i) {
        const auto& r = rows[i];
        if (r.grew <= 0) {
            break;
        }
        std::printf("%s +%#llx<+%#llx %+.1f KB %+lld (live %.1f KB in %lld, %llu allocs; freed at "
                    "+%#llx<+%#llx<+%#llx<+%#llx<+%#llx)",
                    i ? ";" : "", (unsigned long long)r.a, (unsigned long long)r.b, r.grew / 1e3,
                    (long long)r.live_grew, r.cb_live / 1e3, (long long)r.live,
                    (unsigned long long)r.allocs, (unsigned long long)r.free_chain[0],
                    (unsigned long long)r.free_chain[1], (unsigned long long)r.free_chain[2],
                    (unsigned long long)r.free_chain[3], (unsigned long long)r.free_chain[4]);
    }
    std::printf("\n");
}
} // namespace PsHeapSites
