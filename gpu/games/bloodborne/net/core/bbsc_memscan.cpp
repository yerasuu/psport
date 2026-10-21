// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bbsc_memscan.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <sys/uio.h>
#include <unistd.h>

extern "C" int runtime_memory_region(std::uintptr_t va_guest, std::uintptr_t* p_start,
                                     std::uintptr_t* p_end, int* p_mapped);
extern "C" void runtime_memory_read_backing(std::uintptr_t va_range, void* p_data,
                                            std::uint64_t cb_range);
extern "C" int runtime_memory_vma_info(std::uintptr_t va_guest, int* p_prot, int* p_type,
                                       std::uintptr_t* p_end);
#endif

namespace Core::Bbsc {

namespace {

#ifndef _WIN32
// The runtime's guest range starts here (src/kernel/runtime_memory.h, USER_MIN); host-owned memory
// the guest sees lies below it.
constexpr std::uint64_t GuestMin = 0x1000000000ULL;
#endif

#ifdef _WIN32
// One region. The emulator changes page protection all the time (GPU tracking), so a page that was
// readable a moment ago may fault now: the search runs under SEH and a fault just ends the region.
void SearchRegion(const std::uint8_t* start, std::size_t size, const std::uint8_t* needle,
                  std::size_t length, std::vector<std::uint64_t>& hits, std::size_t max_hits) {
    __try {
        const std::uint8_t* end = start + size;
        const std::uint8_t* at = start;
        while (hits.size() < max_hits && end - at >= static_cast<std::ptrdiff_t>(length)) {
            at = static_cast<const std::uint8_t*>(
                std::memchr(at, needle[0], static_cast<std::size_t>(end - at) - length + 1));
            if (at == nullptr) {
                break;
            }
            if (std::memcmp(at, needle, length) == 0) {
                hits.push_back(reinterpret_cast<std::uint64_t>(at));
            }
            ++at;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#endif

} // namespace

std::vector<std::uint64_t> FindInMemory(std::span<const std::uint8_t> needle, std::uint64_t lo,
                                        std::uint64_t hi, std::size_t max_hits) {
    std::vector<std::uint64_t> hits;
#ifdef _WIN32
    if (needle.empty()) {
        return hits;
    }
    constexpr DWORD Readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    std::uint64_t at = lo;
    while (at < hi && hits.size() < max_hits) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) {
            break;
        }
        const auto start = reinterpret_cast<std::uint64_t>(info.BaseAddress);
        const std::uint64_t end = start + info.RegionSize;
        if (info.State == MEM_COMMIT && (info.Protect & Readable) != 0 &&
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
            SearchRegion(reinterpret_cast<const std::uint8_t*>(start), info.RegionSize,
                         needle.data(), needle.size(), hits, max_hits);
        }
        if (end <= at) {
            break;
        }
        at = end;
    }
#else
    if (needle.empty() || lo >= hi) {
        return hits;
    }
    constexpr std::size_t Piece = 1u << 20;
    std::vector<std::uint8_t> buffer(Piece + needle.size());
    const auto search_piece = [&](std::uint64_t at, std::size_t cb_piece) {
        const std::uint8_t* data = buffer.data();
        const std::uint8_t* last = data + cb_piece - needle.size();
        for (const std::uint8_t* p = data; p <= last && hits.size() < max_hits; ++p) {
            p = static_cast<const std::uint8_t*>(
                std::memchr(p, needle[0], std::size_t(last - p) + 1));
            if (!p) {
                break;
            }
            // A match starting past this piece's own bytes is found by the next piece.
            if (std::size_t(p - data) < Piece &&
                std::memcmp(p, needle.data(), needle.size()) == 0) {
                hits.push_back(at + std::uint64_t(p - data));
            }
        }
    };
    // Host-owned memory below the guest's (the image, stacks: runtime_low_map), from
    // /proc/self/maps, copied with process_vm_readv on this process: a page the port protects
    // meanwhile only fails that copy instead of faulting.
    const std::uint64_t hi_low = std::min(hi, GuestMin);
    if (lo < hi_low) {
        if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
            char line[512];
            while (hits.size() < max_hits && std::fgets(line, sizeof(line), maps)) {
                unsigned long long start = 0, end = 0;
                char perms[5] = {};
                if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3 ||
                    perms[0] != 'r') {
                    continue;
                }
                start = std::max<unsigned long long>(start, lo);
                end = std::min<unsigned long long>(end, hi_low);
                for (std::uint64_t at = start; at < end && hits.size() < max_hits; at += Piece) {
                    const std::size_t want =
                        std::size_t(std::min<std::uint64_t>(end - at, Piece + needle.size() - 1));
                    iovec local{buffer.data(), want};
                    iovec remote{reinterpret_cast<void*>(at), want};
                    const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
                    if (got >= ssize_t(needle.size())) {
                        search_piece(at, std::size_t(got));
                    }
                }
            }
            std::fclose(maps);
        }
    }
    // The guest's memory region by region from the runtime's map, copied with process_vm_readv
    // (a protected or GPU-trapped page only fails its piece). That cannot read the dma-buf
    // mappings that hold the game's direct memory (gpu/memory): a direct memory region is read
    // through the runtime's backing view instead, which always has its pages and no traps. A file
    // mapping has no such view to fall back on and keeps the failed piece.
    for (std::uint64_t at = std::max(lo, GuestMin); at < hi && hits.size() < max_hits;) {
        std::uintptr_t start = 0, end = 0;
        int is_mapped = 0;
        if (!runtime_memory_region(at, &start, &end, &is_mapped) || end <= at) {
            break;
        }
        int prot = 0, type = -1;
        std::uintptr_t vma_end = 0;
        const bool is_direct = is_mapped && runtime_memory_vma_info(at, &prot, &type, &vma_end) &&
                               type >= 0 && vma_end >= end;
        const std::uint64_t region_end = std::min<std::uint64_t>(end, hi);
        for (std::uint64_t piece = at; is_mapped && piece < region_end && hits.size() < max_hits;
             piece += Piece) {
            const std::size_t want =
                std::size_t(std::min<std::uint64_t>(region_end - piece, Piece + needle.size() - 1));
            if (want < needle.size()) {
                break;
            }
            iovec local{buffer.data(), want};
            iovec remote{reinterpret_cast<void*>(piece), want};
            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(want)) {
                search_piece(piece, want);
            } else if (is_direct) {
                runtime_memory_read_backing(piece, buffer.data(), want);
                search_piece(piece, want);
            }
        }
        at = end;
    }
#endif
    return hits;
}

} // namespace Core::Bbsc
