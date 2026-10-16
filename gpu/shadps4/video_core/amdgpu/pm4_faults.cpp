// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/amdgpu/pm4_faults.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));

namespace AmdGpu::Pm4Faults {

namespace {

constexpr std::size_t KindCount = 3;
constexpr const char* s_sz_kinds[KindCount] = {"skipped", "buffer ended", "unsupported field"};

std::array<std::array<std::atomic<u64>, 256>, KindCount> s_c_by_opcode{};
std::atomic<bool> s_is_summary_added{false};

__attribute__((format(printf, 4, 5))) void Append(char* sz_out, std::size_t cb_out, std::size_t& off_out,
                                                  const char* sz_format, ...) {
    if (off_out >= cb_out) {
        return;
    }
    va_list args;
    va_start(args, sz_format);
    const int c_written = std::vsnprintf(sz_out + off_out, cb_out - off_out, sz_format, args);
    va_end(args);
    off_out += c_written > 0 ? static_cast<std::size_t>(c_written) : 0;
}

void Summarize(char* sz_out, std::size_t cb_out) {
    std::size_t off_out = 0;
    Append(sz_out, cb_out, off_out, "PM4 faults: summary:");
    for (std::size_t i_kind = 0; i_kind < KindCount; ++i_kind) {
        u64 c_total = 0;
        for (const auto& count : s_c_by_opcode[i_kind]) {
            c_total += count.load(std::memory_order_relaxed);
        }
        Append(sz_out, cb_out, off_out, "%s %s %llu", i_kind == 0 ? "" : ";", s_sz_kinds[i_kind],
               static_cast<unsigned long long>(c_total));
        for (std::size_t opcode = 0; opcode < 256; ++opcode) {
            if (const u64 c_faults = s_c_by_opcode[i_kind][opcode].load(std::memory_order_relaxed)) {
                Append(sz_out, cb_out, off_out, " %#zx:%llu", opcode, static_cast<unsigned long long>(c_faults));
            }
        }
    }
    Append(sz_out, cb_out, off_out, "\n");
    if (off_out >= cb_out && cb_out > 1) {
        sz_out[cb_out - 2] = '\n';
    }
}

} // namespace

bool Note(Kind kind, u32 opcode, const char* sz_format, ...) {
    const auto i_kind = static_cast<std::size_t>(kind);
    const u64 c_faults = s_c_by_opcode[i_kind][opcode & 0xFF].fetch_add(1, std::memory_order_relaxed) + 1;
    if (!s_is_summary_added.exchange(true, std::memory_order_relaxed)) {
        runtime_calls_add_summary(&Summarize);
    }
    if (!std::has_single_bit(c_faults)) {
        return false;
    }
    char sz_detail[256];
    va_list args;
    va_start(args, sz_format);
    std::vsnprintf(sz_detail, sizeof(sz_detail), sz_format, args);
    va_end(args);
    std::printf("PM4: %s, opcode %#x (%llu so far): %s\n", s_sz_kinds[i_kind], opcode & 0xFF,
                static_cast<unsigned long long>(c_faults), sz_detail);
    return true;
}

} // namespace AmdGpu::Pm4Faults
