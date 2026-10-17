// SPDX-License-Identifier: GPL-2.0-or-later
#include "gnm_hooks.h"

#include <algorithm>
#include <initializer_list>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "../../../src/platform/ps_host.h"

namespace PsGnmHooks {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

struct Entry {
    u64 off_image;
    u8 cb_prologue;
    u8 prologue[15];
    unsigned sites;
    bool cb_method; // calls the command buffer's reserve callback: rdi is the command buffer
    const char* sz_pm4;
};
constexpr Entry Table[] = {
#include "gnm_table.inc"
};
constexpr std::size_t Count = sizeof(Table) / sizeof(Table[0]);

/// Hook stub per function: `lock inc qword [rip+counter]`, the moved prologue, `jmp` back past it.
constexpr std::size_t SlotSize = 256; // observing stubs are ~190 bytes
constexpr std::size_t CounterStride = 64; // one cache line per counter

u8* s_p_stubs = nullptr;
u64* CounterAt(std::size_t i) {
    const std::size_t page = ps_host_vm_page_size();
    const std::size_t cb_code = ((Count + 1) * SlotSize + page - 1) / page * page;
    return reinterpret_cast<u64*>(s_p_stubs + cb_code + i * CounterStride);
}

/// A mapping within +-2 GiB of the image text, for rel32 jumps both ways.
u8* MapNear(unsigned char* p_image, std::uint64_t cb_image, std::size_t cb_range) {
    const u64 base = reinterpret_cast<u64>(p_image);
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (16ull << 20), base + cb_image + k * (16ull << 20)}) {
            void* p_mapping = ps_host_vm_map(reinterpret_cast<void*>(hint), cb_range, PS_HOST_VM_READ | PS_HOST_VM_WRITE,
                                             PS_HOST_VM_EXACT);
            if (p_mapping) {
                return static_cast<u8*>(p_mapping);
            }
        }
    }
    return nullptr;
}

bool Rel32(const u8* p_from_next, const void* p_to, std::int32_t& rel) {
    const auto d = reinterpret_cast<std::intptr_t>(p_to) - reinterpret_cast<std::intptr_t>(p_from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = static_cast<std::int32_t>(d);
    return true;
}

/// PS_GNM_OBSERVE=1: command buffer methods run through an observing stub. It saves the argument
/// registers, calls Enter (which notes the buffer's write pointer and puts ExitThunk in place of the
/// return address), runs the function; ExitThunk calls Leave, which marks every packet the call
/// wrote in a shadow table (writer, checksum). Submissions are checked against it: which packets
/// came from known API calls, which from elsewhere, which were changed after the call wrote them.
bool s_should_observe = false;
u8* s_p_exit_thunk = nullptr;

struct Frame {
    std::uint32_t i_function;
    u64 va_command_buffer;
    u64 before;
    u64 ret;
    u64 args[6]; // rdi, rsi, rdx, rcx, r8, r9 (PS_GNM_TRACE)
};
/// PS_GNM_TRACE=offset[:count]: prints the arguments, the caller and the packets written by the
/// first `count` (default 40) calls of that function.
std::uint32_t s_i_trace = ~0u;
std::atomic<int> s_trace_left{0};
constexpr int MaxDepth = 64;
thread_local Frame s_frames[MaxDepth];
thread_local int s_depth = 0;

std::atomic<u64> s_c_anomalies{0};
std::atomic<u64> s_anomalies_by_fn[1024];
std::atomic<u64> s_anomaly_kind[3]; // 0 pointer went back or far, 1 not a packet, 2 packet past the end
std::atomic<u64> s_overflow{0};

/// Shadow of guest command memory: per dword at a packet start, (writer index + 1) << 22 | checksum.
constexpr int L1Bits = 14, L2Bits = 14;
std::atomic<std::atomic<std::uint32_t*>*> s_shadow_l1[1 << L1Bits];

std::uint32_t* ShadowPage(u64 va_guest, bool should_create) {
    const u64 l1 = (va_guest >> 26) & ((1 << L1Bits) - 1);
    const u64 l2 = (va_guest >> 12) & ((1 << L2Bits) - 1);
    auto* p_table = s_shadow_l1[l1].load(std::memory_order_acquire);
    if (!p_table) {
        if (!should_create) {
            return nullptr;
        }
        auto* p_fresh = new std::atomic<std::uint32_t*>[1 << L2Bits]();
        if (!s_shadow_l1[l1].compare_exchange_strong(p_table, p_fresh)) {
            delete[] p_fresh;
        } else {
            p_table = p_fresh;
        }
    }
    auto* p_page = p_table[l2].load(std::memory_order_acquire);
    if (!p_page && should_create) {
        auto* p_fresh = new std::uint32_t[1024]();
        if (!p_table[l2].compare_exchange_strong(p_page, p_fresh)) {
            delete[] p_fresh;
        } else {
            p_page = p_fresh;
        }
    }
    return p_page;
}

std::uint32_t Checksum(const std::uint32_t* p_words, std::uint32_t c_words) {
    std::uint32_t h = 2166136261u;
    for (std::uint32_t k = 0; k < c_words; ++k) {
        h = (h ^ p_words[k]) * 16777619u;
    }
    return h & 0x3fffff;
}

/// Packet size in dwords (type 3: header + count + 1; type 2: filler), 0 for anything else.
std::uint32_t PacketDwords(std::uint32_t h) {
    switch (h >> 30) {
    case 3:
        return ((h >> 16) & 0x3fff) + 2;
    case 2:
        return 1;
    default:
        return 0;
    }
}

std::atomic<u64> s_c_chunk_switches{0};
std::atomic<u64> s_uncovered_after[1024]; // uncovered packets by the writer of the packet before
std::atomic<u64> s_uncovered_ctx_reg[0x400];
std::atomic<u64> s_uncovered_sh_reg[0x400];
std::atomic<u64> s_callback_seen[8];
std::atomic<u64> s_callback_hits[8];
u64 s_image_base = 0;

void NoteCallback(u64 va_command_buffer) {
    const u64 fn = *reinterpret_cast<const u64*>(va_command_buffer + 0x18) - s_image_base;
    for (int k = 0; k < 8; ++k) {
        u64 seen = s_callback_seen[k].load(std::memory_order_relaxed);
        if (seen == fn || (seen == 0 && s_callback_seen[k].compare_exchange_strong(seen, fn)) || seen == fn) {
            s_callback_hits[k].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void Mark(std::uint32_t i_function, u64 before, u64 after, u64 va_command_buffer = 0) {
    if (after == before) {
        return;
    }
    if (va_command_buffer && (after < before || after - before > (1u << 20))) {
        // The reserve callback started a new chunk (the engine's 0x26af230 keeps a chunk list):
        // the call wrote from the new chunk's start.
        const u64 begin = *reinterpret_cast<const u64*>(va_command_buffer);
        if (begin <= after && after - begin <= (1u << 20)) {
            s_c_chunk_switches.fetch_add(1, std::memory_order_relaxed);
            before = begin;
        }
    }
    if (after < before || after - before > (1u << 20) || (before & 3)) {
        s_c_anomalies.fetch_add(1, std::memory_order_relaxed);
        s_anomalies_by_fn[i_function].fetch_add(1, std::memory_order_relaxed);
        s_anomaly_kind[0].fetch_add(1, std::memory_order_relaxed);
        if (va_command_buffer) {
            NoteCallback(va_command_buffer);
        }
        return;
    }
    for (u64 p = before; p < after;) {
        const auto* p_words = reinterpret_cast<const std::uint32_t*>(p);
        const std::uint32_t c_dwords = PacketDwords(*p_words);
        if (c_dwords == 0 || p + c_dwords * 4 > after) {
            s_c_anomalies.fetch_add(1, std::memory_order_relaxed);
            s_anomalies_by_fn[i_function].fetch_add(1, std::memory_order_relaxed);
            s_anomaly_kind[c_dwords == 0 ? 1 : 2].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::uint32_t* p_page = ShadowPage(p, true);
        p_page[(p >> 2) & 1023] = ((i_function + 1) << 22) | Checksum(p_words, c_dwords);
        p += c_dwords * 4;
    }
}

extern "C" void PsGnmEnter(std::uint32_t i_function, u64 va_command_buffer, u64* p_slot) {
    __atomic_fetch_add(CounterAt(i_function), 1, __ATOMIC_RELAXED);
    if (s_depth == MaxDepth) {
        s_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Frame& f = s_frames[s_depth++];
    // A traced function that is not a command buffer method: no write pointer to note.
    f = {i_function, va_command_buffer, Table[i_function].cb_method ? *reinterpret_cast<const u64*>(va_command_buffer + 0x10) : 0, *p_slot, {}};
    if (i_function == s_i_trace) {
        // The stub saved rdi..r9 just below the return address slot.
        f.args[0] = p_slot[-1]; f.args[1] = p_slot[-2]; f.args[2] = p_slot[-3];
        f.args[3] = p_slot[-4]; f.args[4] = p_slot[-5]; f.args[5] = p_slot[-6];
    }
    *p_slot = reinterpret_cast<u64>(s_p_exit_thunk);
}

extern "C" u64 PsGnmLeave() {
    const Frame f = s_frames[--s_depth];
    const bool cb_method = Table[f.i_function].cb_method;
    if (f.i_function == s_i_trace && s_trace_left.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const u64 after = cb_method ? *reinterpret_cast<const u64*>(f.va_command_buffer + 0x10) : f.before;
        char sz_line[4096];
        int cb_line = std::snprintf(sz_line, sizeof(sz_line),
                              "Gnm trace 0x%07llx from 0x%llx: args %llx %llx %llx %llx %llx %llx; wrote",
                              (unsigned long long)Table[f.i_function].off_image,
                              (unsigned long long)(f.ret - s_image_base), (unsigned long long)f.args[1],
                              (unsigned long long)f.args[2], (unsigned long long)f.args[3],
                              (unsigned long long)f.args[4], (unsigned long long)f.args[5], 0ull);
        if (!cb_method && f.args[0]) {
            cb_line += std::snprintf(sz_line + cb_line, sizeof(sz_line) - cb_line, " (rdi %llx:", (unsigned long long)f.args[0]);
            for (int k = 0; k < 8; ++k) {
                cb_line += std::snprintf(sz_line + cb_line, sizeof(sz_line) - cb_line, " %08x",
                                   reinterpret_cast<const std::uint32_t*>(f.args[0])[k]);
            }
            cb_line += std::snprintf(sz_line + cb_line, sizeof(sz_line) - cb_line, ")");
        }
        if (after > f.before && after - f.before <= 256) {
            for (u64 p = f.before; p < after && cb_line < int(sizeof(sz_line)) - 12; p += 4) {
                cb_line += std::snprintf(sz_line + cb_line, sizeof(sz_line) - cb_line, " %08x",
                                   *reinterpret_cast<const std::uint32_t*>(p));
            }
        }
        std::printf("%s\n", sz_line);
    }
    if (!cb_method) {
        return f.ret;
    }
    Mark(f.i_function, f.before, *reinterpret_cast<const u64*>(f.va_command_buffer + 0x10), f.va_command_buffer);
    return f.ret;
}

// Submission check statistics (written by the submitting thread, read by the report thread).
std::atomic<u64> s_c_sub_packets{0}, s_c_sub_dwords{0}, s_c_cov_packets{0}, s_c_cov_dwords{0}, s_c_changed_packets{0};
std::atomic<u64> s_uncovered_by_op[256];
std::atomic<u64> s_uncovered_dwords_by_op[256];
std::atomic<u64> s_changed_by_fn[1024];
std::atomic<u64> s_nop_tags[16]; // uncovered NOPs by their first payload dword's top byte (markers)

struct Writer {
    u8* m_p_cursor;
    void Put(std::initializer_list<int> bytes) {
        for (int b : bytes) {
            *m_p_cursor++ = u8(b);
        }
    }
    void Imm32(std::uint32_t v) {
        std::memcpy(m_p_cursor, &v, 4);
        m_p_cursor += 4;
    }
    void Imm64(u64 v) {
        std::memcpy(m_p_cursor, &v, 8);
        m_p_cursor += 8;
    }
};

// movdqu [rsp+8k], xmmk / movdqu xmmk, [rsp+8k] for k = 0..7 (16 bytes apart).
void SaveXmm(Writer& w, int c_registers, bool should_load) {
    for (int k = 0; k < c_registers; ++k) {
        const int op = should_load ? 0x6f : 0x7f;
        if (k == 0) {
            w.Put({0xf3, 0x0f, op, 0x04, 0x24});
        } else {
            w.Put({0xf3, 0x0f, op, 0x44 | (k << 3), 0x24, k * 16});
        }
    }
}

void WriteExitThunk(u8* p_at) {
    Writer w{p_at};
    w.Put({0x48, 0x83, 0xec, 0x08}); // sub rsp, 8 (the return address slot)
    w.Put({0x50, 0x52});             // push rax; push rdx
    w.Put({0x48, 0x83, 0xec, 0x28}); // sub rsp, 0x28
    SaveXmm(w, 2, false);
    w.Put({0x48, 0xb8});
    w.Imm64(reinterpret_cast<u64>(&PsGnmLeave)); // mov rax, PsGnmLeave
    w.Put({0xff, 0xd0});                          // call rax
    w.Put({0x48, 0x89, 0x44, 0x24, 0x38});        // mov [rsp+0x38], rax
    SaveXmm(w, 2, true);
    w.Put({0x48, 0x83, 0xc4, 0x28}); // add rsp, 0x28
    w.Put({0x5a, 0x58, 0xc3});       // pop rdx; pop rax; ret
}

/// Observing entry stub; returns its end (where the moved prologue goes).
u8* WriteObserveStub(u8* p_at, std::uint32_t i_function) {
    Writer w{p_at};
    w.Put({0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x50}); // push rdi..r9, rax
    w.Put({0x48, 0x81, 0xec});
    w.Imm32(0x80); // sub rsp, 0x80
    SaveXmm(w, 8, false);
    w.Put({0xbf});
    w.Imm32(i_function);                                            // mov edi, index
    w.Put({0x48, 0x8b, 0xb4, 0x24});
    w.Imm32(0xb0);                                             // mov rsi, [rsp+0xb0] (rdi)
    w.Put({0x48, 0x8d, 0x94, 0x24});
    w.Imm32(0xb8);                                             // lea rdx, [rsp+0xb8] (return slot)
    w.Put({0x48, 0xb8});
    w.Imm64(reinterpret_cast<u64>(&PsGnmEnter));               // mov rax, PsGnmEnter
    w.Put({0xff, 0xd0});                                       // call rax
    SaveXmm(w, 8, true);
    w.Put({0x48, 0x81, 0xc4});
    w.Imm32(0x80);                                             // add rsp, 0x80
    w.Put({0x58, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f}); // pop rax, r9, r8, rcx..rdi
    return w.m_p_cursor;
}

void ReportObserve() {
    const u64 sp = s_c_sub_packets.exchange(0), sd = s_c_sub_dwords.exchange(0);
    const u64 cp = s_c_cov_packets.exchange(0), cd = s_c_cov_dwords.exchange(0);
    const u64 ch = s_c_changed_packets.exchange(0);
    if (!sp) {
        return;
    }
    std::printf("Gnm observe: %llu packets submitted, %.1f%% from hooked API calls (%.1f%% of dwords), "
                "%llu changed after the call, %llu call anomalies, %llu depth overflows\n",
                (unsigned long long)sp, 100.0 * cp / sp, sd ? 100.0 * cd / sd : 0.0,
                (unsigned long long)ch, (unsigned long long)s_c_anomalies.exchange(0),
                (unsigned long long)s_overflow.exchange(0));
    std::vector<std::pair<u64, int>> ops;
    for (int k = 0; k < 256; ++k) {
        if (const u64 c_hits = s_uncovered_by_op[k].exchange(0)) {
            ops.emplace_back(c_hits, k);
        }
    }
    std::sort(ops.rbegin(), ops.rend());
    for (std::size_t k = 0; k < std::min<std::size_t>(ops.size(), 16); ++k) {
        const int op = ops[k].second;
        std::printf("  not from the API: opcode 0x%02x %llu packets, %llu dwords\n", op,
                    (unsigned long long)ops[k].first,
                    (unsigned long long)s_uncovered_dwords_by_op[op].exchange(0));
    }
    for (int k = 0; k < 16; ++k) {
        if (const u64 c_hits = s_nop_tags[k].exchange(0)) {
            std::printf("  not from the API: NOP payload top nibble 0x%x: %llu\n", k,
                        (unsigned long long)c_hits);
        }
    }
    for (std::size_t k = 0; k <= Count; ++k) {
        if (const u64 c_hits = s_changed_by_fn[k + 1].exchange(0)) {
            std::printf("  changed after %s 0x%07llx wrote them: %llu packets\n",
                        k == Count ? "the driver" : "", k == Count ? 0ull : (unsigned long long)Table[k].off_image,
                        (unsigned long long)c_hits);
        }
        if (const u64 c_hits = s_anomalies_by_fn[k].exchange(0)) {
            std::printf("  anomalies in %s0x%07llx: %llu\n", k == Count ? "the driver " : "",
                        k == Count ? 0ull : (unsigned long long)Table[k].off_image, (unsigned long long)c_hits);
        }
    }
    for (int k = 0; k < 8; ++k) {
        if (const u64 c_hits = s_callback_hits[k].exchange(0)) {
            std::printf("  reserve callback at guest 0x%llx moved the pointer back: %llu\n",
                        (unsigned long long)s_callback_seen[k].load(), (unsigned long long)c_hits);
        }
    }
    {
        std::vector<std::pair<u64, int>> top;
        for (int k = 0; k < 1024; ++k) {
            if (const u64 c_hits = s_uncovered_after[k].exchange(0)) {
                top.emplace_back(c_hits, k);
            }
        }
        std::sort(top.rbegin(), top.rend());
        for (std::size_t k = 0; k < std::min<std::size_t>(top.size(), 6); ++k) {
            const int id = top[k].second;
            std::printf("  not from the API, after a packet of %s0x%07llx: %llu\n",
                        id == 0 ? "(nothing) " : id == int(Count) + 1 ? "the driver " : "",
                        id >= 1 && id <= int(Count) ? (unsigned long long)Table[id - 1].off_image : 0ull,
                        (unsigned long long)top[k].first);
        }
    }
    std::printf("  chunk switches: %llu\n", (unsigned long long)s_c_chunk_switches.exchange(0));
    for (int t = 0; t < 2; ++t) {
        auto* p_regs = t ? s_uncovered_sh_reg : s_uncovered_ctx_reg;
        std::vector<std::pair<u64, int>> top;
        for (int r = 0; r < 0x400; ++r) {
            if (const u64 c_hits = p_regs[r].exchange(0)) {
                top.emplace_back(c_hits, r);
            }
        }
        std::sort(top.rbegin(), top.rend());
        for (std::size_t k = 0; k < std::min<std::size_t>(top.size(), 8); ++k) {
            std::printf("  not from the API: %s reg 0x%x: %llu\n", t ? "SH" : "context", top[k].second,
                        (unsigned long long)top[k].first);
        }
    }
    std::printf("  anomaly kinds: pointer back/far %llu, not a packet %llu, past the end %llu\n",
                (unsigned long long)s_anomaly_kind[0].exchange(0),
                (unsigned long long)s_anomaly_kind[1].exchange(0),
                (unsigned long long)s_anomaly_kind[2].exchange(0));
}

void ReportLoop() {
    std::vector<u64> last(Count, 0);
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const auto t1 = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
        std::vector<std::pair<u64, std::size_t>> rates;
        u64 c_total = 0;
        std::size_t used = 0;
        for (std::size_t i = 0; i < Count; ++i) {
            const u64 now = __atomic_load_n(CounterAt(i), __ATOMIC_RELAXED);
            const u64 d = now - last[i];
            last[i] = now;
            c_total += d;
            used += now != 0;
            if (d) {
                rates.emplace_back(d, i);
            }
        }
        std::sort(rates.rbegin(), rates.rend());
        // PS_GNM_CENSUS_FILE: every function's total calls so far (offset, calls, cb method, PM4).
        if (const char* sz_path = std::getenv("PS_GNM_CENSUS_FILE")) {
            if (FILE* p_file = std::fopen(sz_path, "w")) {
                for (std::size_t i = 0; i < Count; ++i) {
                    std::fprintf(p_file, "0x%07llx %llu %d %s\n", (unsigned long long)Table[i].off_image,
                                 (unsigned long long)last[i], int(Table[i].cb_method), Table[i].sz_pm4);
                }
                std::fclose(p_file);
            }
        }
        std::printf("Gnm census: %.0f calls/s, %zu of %zu functions called so far\n", c_total / s,
                    used, Count);
        for (std::size_t k = 0; k < std::min<std::size_t>(rates.size(), 40); ++k) {
            const auto& e = Table[rates[k].second];
            std::printf("  0x%07llx %9.0f/s  %s\n", static_cast<unsigned long long>(e.off_image),
                        rates[k].first / s, e.sz_pm4);
        }
        if (s_should_observe) {
            ReportObserve();
        }
        std::fflush(stdout);
    }
}
} // namespace

void CheckSubmission(const std::uint32_t* p_commands, std::uint64_t dwords) {
    if (!s_should_observe || !p_commands) {
        return;
    }
    u64 packets = 0, covered = 0, covered_dw = 0, changed = 0;
    std::uint32_t previous = 0;
    for (u64 at = 0; at < dwords;) {
        const auto* p_words = p_commands + at;
        std::uint32_t c_dwords = PacketDwords(*p_words);
        if (c_dwords == 0 || at + c_dwords > dwords) {
            c_dwords = 1; // type 0/1 or cut off: counted as one uncovered dword
        }
        const u64 va = reinterpret_cast<u64>(p_words);
        std::uint32_t* p_page = ShadowPage(va, false);
        const std::uint32_t e = p_page ? p_page[(va >> 2) & 1023] : 0;
        ++packets;
        const int op = (*p_words >> 30) == 3 ? int((*p_words >> 8) & 0xff) : 0;
        if (e == 0) {
            s_uncovered_after[std::min<std::uint32_t>(previous, 1023)].fetch_add(1, std::memory_order_relaxed);
            s_uncovered_by_op[op].fetch_add(1, std::memory_order_relaxed);
            s_uncovered_dwords_by_op[op].fetch_add(c_dwords, std::memory_order_relaxed);
            if ((op == 0x69 || op == 0x76) && c_dwords > 1) {
                (op == 0x69 ? s_uncovered_ctx_reg : s_uncovered_sh_reg)[p_words[1] & 0x3ff].fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (op == 0x10 && c_dwords > 1) {
                s_nop_tags[p_words[1] >> 28].fetch_add(1, std::memory_order_relaxed);
            }
        } else if ((e & 0x3fffff) != Checksum(p_words, c_dwords)) {
            ++changed;
            s_changed_by_fn[std::min<std::uint32_t>(e >> 22, 1023)].fetch_add(1, std::memory_order_relaxed);
        } else {
            ++covered;
            covered_dw += c_dwords;
        }
        if (e) {
            previous = e >> 22;
        }
        if (p_page) {
            p_page[(va >> 2) & 1023] = 0;
        }
        at += c_dwords;
    }
    s_c_sub_packets.fetch_add(packets, std::memory_order_relaxed);
    s_c_sub_dwords.fetch_add(dwords, std::memory_order_relaxed);
    s_c_cov_packets.fetch_add(covered, std::memory_order_relaxed);
    s_c_cov_dwords.fetch_add(covered_dw, std::memory_order_relaxed);
    s_c_changed_packets.fetch_add(changed, std::memory_order_relaxed);
}

void PatchImage(unsigned char* p_image, std::uint64_t cb_image) {
    const char* sz_env = std::getenv("PS_GNM_CENSUS");
    const char* sz_observe_env = std::getenv("PS_GNM_OBSERVE");
    s_should_observe = sz_observe_env && sz_observe_env[0] == '1';
    if (!s_should_observe && (!sz_env || sz_env[0] != '1')) {
        return;
    }
    const std::size_t cb_stub = (Count + 1) * SlotSize; // + the shared exit thunk
    const std::size_t page = ps_host_vm_page_size();
    const std::size_t cb_code = (cb_stub + page - 1) / page * page;
    const std::size_t cb_map = cb_code + Count * CounterStride;
    s_image_base = reinterpret_cast<u64>(p_image);
    if (const char* sz_trace = std::getenv("PS_GNM_TRACE")) {
        unsigned long long off_traced = 0;
        int c_traced = 40;
        std::sscanf(sz_trace, "%llx:%d", &off_traced, &c_traced);
        for (std::size_t i = 0; i < Count; ++i) {
            if (Table[i].off_image == off_traced) {
                s_i_trace = std::uint32_t(i);
            }
        }
        s_trace_left = c_traced;
    }
    s_p_stubs = MapNear(p_image, cb_image, cb_map);
    if (!s_p_stubs) {
        std::printf("Gnm census: no memory near the image, off\n");
        return;
    }
    std::size_t patched = 0, mismatched = 0;
    s_p_exit_thunk = s_p_stubs + Count * SlotSize;
    WriteExitThunk(s_p_exit_thunk);
    // PS_GNM_CENSUS_RANGE=first:last (table indices, bisecting).
    std::size_t first = 0, last = Count;
    if (const char* sz_range = std::getenv("PS_GNM_CENSUS_RANGE")) {
        std::sscanf(sz_range, "%zu:%zu", &first, &last);
    }
    u8* p_counters = s_p_stubs + cb_code;
    for (std::size_t i = first; i < std::min(last, Count); ++i) {
        const Entry& e = Table[i];
        u8* p_function = p_image + e.off_image;
        if (e.off_image + e.cb_prologue > cb_image || std::memcmp(p_function, e.prologue, e.cb_prologue) != 0) {
            ++mismatched;
            continue;
        }
        u8* p_stub = s_p_stubs + i * SlotSize;
        u8* p_counter = p_counters + i * CounterStride;
        std::int32_t rel;
        u8* p_body;
        if (s_should_observe && (e.cb_method || i == s_i_trace)) {
            p_body = WriteObserveStub(p_stub, std::uint32_t(i));
        } else {
            // lock inc qword [rip+rel]
            p_stub[0] = 0xf0; p_stub[1] = 0x48; p_stub[2] = 0xff; p_stub[3] = 0x05;
            if (!Rel32(p_stub + 8, p_counter, rel)) {
                ++mismatched;
                continue;
            }
            std::memcpy(p_stub + 4, &rel, 4);
            p_body = p_stub + 8;
        }
        std::memcpy(p_body, e.prologue, e.cb_prologue);
        u8* p_jump = p_body + e.cb_prologue;
        p_jump[0] = 0xe9;
        if (!Rel32(p_jump + 5, p_function + e.cb_prologue, rel)) {
            ++mismatched;
            continue;
        }
        std::memcpy(p_jump + 1, &rel, 4);
        // The entry: jmp stub, the rest of the moved prologue int3 (never reached).
        std::int32_t to_stub;
        if (!Rel32(p_function + 5, p_stub, to_stub)) {
            ++mismatched;
            continue;
        }
        p_function[0] = 0xe9;
        std::memcpy(p_function + 1, &to_stub, 4);
        std::memset(p_function + 5, 0xcc, e.cb_prologue - 5);
        ++patched;
    }
    ps_host_vm_protect(s_p_stubs, cb_code, PS_HOST_VM_READ | PS_HOST_VM_EXECUTE);
    std::printf("Gnm census: %zu of %zu libGnm entry points hooked (%zu do not match the 1.09 eboot)%s\n",
                patched, Count, mismatched, s_should_observe ? "; command buffer methods observed" : "");
    std::thread(ReportLoop).detach();
}

void NoteDriverWrite(std::uint32_t* p_commands, std::uint64_t dwords) {
    if (!s_should_observe || !p_commands) {
        return;
    }
    // The written packets end where the space does or at the first zero dword (unused space).
    const auto begin = reinterpret_cast<u64>(p_commands);
    u64 end = begin;
    const u64 limit = begin + dwords * 4;
    while (end < limit) {
        const std::uint32_t c_dwords = PacketDwords(*reinterpret_cast<const std::uint32_t*>(end));
        if (c_dwords == 0 || end + c_dwords * 4 > limit) {
            break;
        }
        end += c_dwords * 4;
    }
    Mark(std::uint32_t(Count), begin, end);
}
} // namespace PsGnmHooks
