// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bbsc_memscan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../../../../src/platform/ps_host.h"

extern "C" int runtime_memory_region(std::uintptr_t va_guest, std::uintptr_t* p_start,
                                     std::uintptr_t* p_end, int* p_mapped);
extern "C" void runtime_memory_read_backing(std::uintptr_t va_range, void* p_data,
                                            std::uint64_t cb_range);
extern "C" int runtime_memory_vma_info(std::uintptr_t va_guest, int* p_prot, int* p_type,
                                       std::uintptr_t* p_end);

namespace Core::Bbsc {

namespace {

// The runtime's guest range starts here (src/kernel/runtime_memory.h, USER_MIN); host-owned memory
// the guest sees lies below it.
constexpr std::uint64_t GuestMin = 0x1000000000ULL;

} // namespace

std::vector<std::uint64_t> FindInMemory(std::span<const std::uint8_t> needle, std::uint64_t lo,
                                        std::uint64_t hi, std::size_t max_hits) {
    std::vector<std::uint64_t> hits;
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
    // Host-owned memory below the guest's (the image, stacks: runtime_low_map), from the host's
    // mapping list, copied without faulting: a page the port protects meanwhile only fails that
    // copy.
    const std::uint64_t hi_low = std::min(hi, GuestMin);
    if (lo < hi_low) {
        auto scan_mapping = [&](const PsHostMapping& mapping) {
            if (!mapping.is_readable) {
                return 0;
            }
            const std::uint64_t start = std::max<std::uint64_t>(mapping.va_begin, lo);
            const std::uint64_t end = std::min<std::uint64_t>(mapping.va_end, hi_low);
            for (std::uint64_t at = start; at < end && hits.size() < max_hits; at += Piece) {
                const std::size_t want =
                    std::size_t(std::min<std::uint64_t>(end - at, Piece + needle.size() - 1));
                const std::int64_t got =
                    ps_host_read_memory(buffer.data(), reinterpret_cast<const void*>(at), want);
                if (got >= std::int64_t(needle.size())) {
                    search_piece(at, std::size_t(got));
                }
            }
            return hits.size() < max_hits ? 0 : 1;
        };
        ps_host_list_mappings(
            [](const PsHostMapping* p_mapping, void* p_context) {
                return (*static_cast<decltype(scan_mapping)*>(p_context))(*p_mapping);
            },
            &scan_mapping);
    }
    // The guest's memory region by region from the runtime's map, copied without faulting
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
            if (ps_host_read_memory(buffer.data(), reinterpret_cast<const void*>(piece), want) ==
                std::int64_t(want)) {
                search_piece(piece, want);
            } else if (is_direct) {
                runtime_memory_read_backing(piece, buffer.data(), want);
                search_piece(piece, want);
            }
        }
        at = end;
    }
    return hits;
}

} // namespace Core::Bbsc
