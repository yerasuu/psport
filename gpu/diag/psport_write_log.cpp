// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_write_log.h"
#include "psport_free_check.h"
#include "../../src/platform/ps_host.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <x86intrin.h>

namespace PsWriteLog {
namespace {
struct Entry {
    std::uint64_t va_range, cb_range, first, tsc;
    std::uint32_t source, tid;
};
constexpr std::size_t Size = 1 << 20;
std::array<Entry, Size> s_ring;
std::atomic<std::uint64_t> s_head{0};
// Writes that contained the suspicious qword (see Note), with the exact address.
std::array<Entry, 256> s_hits;
std::atomic<std::uint64_t> s_hits_head{0};
constexpr std::uint64_t Pattern = 0x0000005300000000ull;

void Push(std::array<Entry, Size>& r, std::atomic<std::uint64_t>& h, const Entry& e) {
    r[h.fetch_add(1, std::memory_order_relaxed) % r.size()] = e;
}
} // namespace

void Record(std::uint64_t va_range, const void* p_data, std::uint64_t cb_range, Source source);

int Mode() {
    static const int s_mode = [] {
        const char* sz_env = std::getenv("PS_WRITE_LOG");
        return sz_env ? std::atoi(sz_env) : 0;
    }();
    return s_mode;
}

bool Enabled() {
    return Mode() == 1;
}

void NoteIntent(std::uint64_t va_range, const void* p_data, std::uint64_t cb_range, Source source) {
    if (Mode() == 2) {
        Record(va_range, p_data, cb_range, source);
    }
}

void Note(std::uint64_t va_range, const void* p_data, std::uint64_t cb_range, Source source) {
    if (Enabled()) {
        Record(va_range, p_data, cb_range, source);
    }
}

void Record(std::uint64_t va_range, const void* p_data, std::uint64_t cb_range, Source source) {
    static thread_local const std::uint32_t s_tid = static_cast<std::uint32_t>(ps_host_thread_id());
    Entry e{va_range, cb_range, 0, __rdtsc(), source, s_tid};
    std::memcpy(&e.first, p_data, cb_range < 8 ? cb_range : 8);
    Push(s_ring, s_head, e);
    // Only small writes are scanned: scanning downloads of megabytes delays them enough to hide
    // the race this log is for.
    if (cb_range > 64) {
        return;
    }
    const auto* p_bytes = static_cast<const unsigned char*>(p_data);
    for (std::uint64_t at = (8 - (va_range & 7)) & 7; at + 8 <= cb_range; at += 8) {
        std::uint64_t v;
        std::memcpy(&v, p_bytes + at, 8);
        if (v == Pattern) {
            Entry hit = e;
            hit.va_range = va_range + at;
            hit.first = v;
            s_hits[s_hits_head.fetch_add(1, std::memory_order_relaxed) % s_hits.size()] = hit;
        }
    }
}
void DumpRange(std::uint64_t va_range, std::uint64_t cb_range) {
    if (Mode() == 0) {
        std::fprintf(stderr, "Write log: off (PS_WRITE_LOG=1 lists the writes into this range)\n");
        return;
    }
    const char* sources[] = {"backing",      "WriteData",           "fence",
                             "EOP (decoded)", "WriteData (decoded)", "EOS (decoded)", "DmaData"};
    const std::uint64_t now = __rdtsc(), c_entries = s_head.load();
    int c_shown = 0;
    for (std::uint64_t i = c_entries; i-- > (c_entries > Size ? c_entries - Size : 0) && c_shown < 64;) {
        const Entry& e = s_ring[i % Size];
        if (e.va_range < va_range + cb_range && va_range < e.va_range + e.cb_range) {
            std::fprintf(stderr,
                         "Write log: into range %s %#llx +%llu first %#llx tid %u, %.3f s before\n",
                         e.source < 7 ? sources[e.source] : "?", (unsigned long long)e.va_range,
                         (unsigned long long)e.cb_range, (unsigned long long)e.first, e.tid,
                         double(now - e.tsc) / 3.0e9);
            ++c_shown;
        }
    }
    std::fprintf(stderr, "Write log: %d of %llu logged writes in %#llx +%llu\n", c_shown,
                 (unsigned long long)c_entries, (unsigned long long)va_range, (unsigned long long)cb_range);
}
} // namespace PsWriteLog

extern "C" void psport_gpu_dump_guest_writes(const PsHostFaultContext* p_context) {
    using namespace PsWriteLog;
    PsFreeCheck::DumpAtFault(ps_host_fault_register(p_context, PS_HOST_REG_RAX),
                             ps_host_fault_register(p_context, PS_HOST_REG_R14));
    if (Mode() == 0) {
        return;
    }
    const std::uint64_t regs[] = {ps_host_fault_register(p_context, PS_HOST_REG_RAX), ps_host_fault_register(p_context, PS_HOST_REG_RBX),
                                  ps_host_fault_register(p_context, PS_HOST_REG_RCX), ps_host_fault_register(p_context, PS_HOST_REG_RDX),
                                  ps_host_fault_register(p_context, PS_HOST_REG_RSI), ps_host_fault_register(p_context, PS_HOST_REG_RDI),
                                  ps_host_fault_register(p_context, PS_HOST_REG_R14), ps_host_fault_register(p_context, PS_HOST_REG_R15)};
    const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r14", "r15"};
    for (int i = 0; i < 8; ++i) {
        std::fprintf(stderr, "Write log: %s=%#llx\n", names[i], (unsigned long long)regs[i]);
    }
    const char* sources[] = {"backing",      "WriteData",           "fence",
                             "EOP (decoded)", "WriteData (decoded)", "EOS (decoded)", "DmaData"};
    const std::uint64_t now = __rdtsc();
    const auto print = [&](const Entry& e, const char* sz_what) {
        std::fprintf(stderr,
                     "Write log: %s %s %#llx +%llu first %#llx tid %u, %.3f s before the fault\n",
                     sz_what, e.source < 7 ? sources[e.source] : "?", (unsigned long long)e.va_range,
                     (unsigned long long)e.cb_range, (unsigned long long)e.first, e.tid,
                     double(now - e.tsc) / 3.0e9);
    };
    const std::uint64_t nh = s_hits_head.load();
    for (std::uint64_t i = nh > s_hits.size() ? nh - s_hits.size() : 0; i < nh; ++i) {
        print(s_hits[i % s_hits.size()], "pattern");
    }
    // The block the guest read (rax) and all logged writes into it or its neighbours.
    const std::uint64_t block = regs[0];
    for (std::uint64_t at = block - 0x30; at < block + 0x60; at += 8) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(at), 8);
        std::fprintf(stderr, "Write log: [%#llx] = %#llx\n", (unsigned long long)at,
                     (unsigned long long)value);
    }
    {
        const std::uint64_t c_entries = s_head.load();
        int c_shown = 0;
        for (std::uint64_t i = c_entries; i-- > (c_entries > Size ? c_entries - Size : 0) && c_shown < 200;) {
            const Entry& e = s_ring[i % Size];
            if (e.va_range + e.cb_range > block - 0x30 && e.va_range < block + 0x60) {
                print(e, "in block");
                ++c_shown;
            }
        }
        c_shown = 0;
        for (std::uint64_t i = c_entries; i-- > (c_entries > Size ? c_entries - Size : 0) && c_shown < 40;) {
            const Entry& e = s_ring[i % Size];
            if ((e.first & 0xffffffffull) == 0x53 || (e.first >> 32) == 0x53) {
                print(e, "value 0x53");
                ++c_shown;
            }
        }
    }
    // Writes that cover the chunk header the guest read (rax..rax+0x40) or the registers.
    const std::uint64_t c_entries = s_head.load();
    int c_shown = 0;
    for (std::uint64_t i = c_entries; i-- > (c_entries > Size ? c_entries - Size : 0) && c_shown < 64;) {
        const Entry& e = s_ring[i % Size];
        bool is_near = false;
        for (const auto r : regs) {
            is_near |= r + 0x1000 > e.va_range && r < e.va_range + e.cb_range + 0x1000;
        }
        if (is_near) {
            print(e, "near");
            ++c_shown;
        }
    }
    std::fprintf(stderr, "Write log: %llu writes logged\n", (unsigned long long)c_entries);
}
