// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: shadPS4's memory manager as BBSC asks it (is this address mapped, with
// which protection), answered from the runtime's own map (runtime_memory_vma_info). Each thread
// remembers the last few mappings for a moment: BBSC checks every read it makes, and the map is
// behind a lock the game's own mappings take. From bbport (gpu/bbnet/include/core/memory.h).
// The host-owned memory the guest sees (the image, stacks, trampolines: runtime_low_map) is not in
// the runtime's map, and BBSC reads the game's globals in the image: those are answered from the
// host's own list of mappings, read again whenever the cache has no answer, gaps cached too so that
// a stale pointer does not read the list on every check. Reads there go through ReadHostOwned,
// since such a mapping may go away while the cache still holds it.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

#include "../../../../src/platform/ps_host.h"
#include "common/singleton.h"
#include "common/types.h"

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

namespace Core {

enum class MemoryProt : u32 {
    NoAccess = 0,
    CpuRead = 1,
    CpuWrite = 2,
    CpuReadWrite = 3,
    CpuExec = 4,
    GpuRead = 16,
    GpuWrite = 32,
    GpuReadWrite = 48,
};

class MemoryManager {
    static constexpr u64 LowHostMin = 0x0800000000ULL;
    static constexpr u64 GuestMin = 0x1000000000ULL;

    struct HostRange {
        u64 start = 0, end = 0;
        u32 prot = 0; ///< 0: a gap between the host's mappings
    };

    /// The host mapping or the gap holding address; false when the list cannot be read.
    static bool FindHostRange(u64 address, HostRange* p_out_range) {
        struct Search {
            u64 address;
            HostRange range;
        } search{address, {LowHostMin, GuestMin, 0}};
        const auto check = [](const PsHostMapping* p_mapping, void* p_context) {
            auto* p_search = static_cast<Search*>(p_context);
            if (p_mapping->va_begin > p_search->address) {
                p_search->range.end = std::min<u64>(p_search->range.end, p_mapping->va_begin);
                return 1;
            }
            if (p_search->address >= p_mapping->va_end) {
                p_search->range.start = std::max<u64>(p_search->range.start, p_mapping->va_end);
                return 0;
            }
            p_search->range = {p_mapping->va_begin, p_mapping->va_end,
                               (p_mapping->is_readable ? u32(MemoryProt::CpuRead) : 0u) |
                                   (p_mapping->is_writable ? u32(MemoryProt::CpuWrite) : 0u) |
                                   (p_mapping->is_executable ? u32(MemoryProt::CpuExec) : 0u)};
            return 1;
        };
        if (ps_host_list_mappings(check, &search) != 0) {
            return false;
        }
        *p_out_range = search.range;
        return true;
    }

public:
    static bool IsHostOwned(u64 address) {
        return address >= LowHostMin && address < GuestMin;
    }
    /// Copies host-owned memory without faulting: false when part of it is no longer mapped.
    static bool ReadHostOwned(void* p_out, u64 address, u64 size) {
        return ps_host_read_memory(p_out, reinterpret_cast<const void*>(address), size) ==
               std::int64_t(size);
    }
    bool IsValidMapping(u64 address, u64 size) {
        u32 prot = 0;
        void* start = nullptr;
        void* end = nullptr;
        return QueryProtection(address, &start, &end, &prot) == 0 &&
               address + size <= reinterpret_cast<u64>(end);
    }
    /// 0 and the mapping's range and protection (MemoryProt bits), or -1 when unmapped.
    int QueryProtection(u64 address, void** start, void** end, u32* prot) {
        struct Cached {
            u64 start = 0, end = 0;
            u32 prot = 0;
            std::chrono::steady_clock::time_point at{};
        };
        thread_local std::array<Cached, 8> cache{};
        thread_local unsigned next = 0;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& c : cache) {
            if (address >= c.start && address < c.end &&
                now - c.at < std::chrono::milliseconds(250)) {
                if (c.prot == 0) {
                    return -1;
                }
                *start = reinterpret_cast<void*>(c.start);
                *end = reinterpret_cast<void*>(c.end);
                *prot = c.prot;
                return 0;
            }
        }
        int p = 0, type = -1;
        uintptr_t mapping_end = 0;
        Cached found{};
        HostRange host{};
        if (runtime_memory_vma_info(address, &p, &type, &mapping_end)) {
            // The runtime's map gives the end of the mapping, not its start: the address itself
            // stands for it (callers check address >= start).
            found = {address & ~u64(0xfff), mapping_end, u32(p), now};
        } else if (IsHostOwned(address) && FindHostRange(address, &host)) {
            found = {host.start, host.end, host.prot, now};
        } else {
            return -1;
        }
        auto& slot = cache[next++ % cache.size()];
        slot = found;
        if (slot.prot == 0) {
            return -1;
        }
        *start = reinterpret_cast<void*>(slot.start);
        *end = reinterpret_cast<void*>(slot.end);
        *prot = slot.prot;
        return 0;
    }
};

using Memory = Common::Singleton<MemoryManager>;

} // namespace Core
