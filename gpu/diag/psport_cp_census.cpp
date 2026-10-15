// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_cp_census.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unistd.h>
#include <magic_enum/magic_enum.hpp>

#include "psport_toggles.h"
#include "video_core/amdgpu/pm4_cmds.h"

namespace PsCpCensus {
namespace {
using AmdGpu::PM4CmdWaitRegMem;
using AmdGpu::PM4Header;
using AmdGpu::PM4ItOpcode;

enum Engine : unsigned { Graphics, Compute, Constant, NumEngines };

/// Counters with a single writer (the GPU command thread) read by the report from any thread.
using Counter = std::atomic<std::uint64_t>;
void Bump(Counter& counter, std::uint64_t by = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

/// Addresses in a fixed open-addressed table, so that the report can walk it while the GPU
/// command thread inserts (no rehashing); full, an address is counted as overflow.
template <bool has_counts>
struct AddressTable {
    static constexpr std::size_t Slots = 4096;
    std::array<std::atomic<std::uint64_t>, Slots> va_slots{};
    std::array<Counter, has_counts ? Slots : 1> c_hits{};
    Counter c_overflow{0};

    static std::size_t Hash(std::uint64_t va) {
        return std::size_t((va >> 2) * 0x9E3779B97F4A7C15ull >> 52) % Slots;
    }
    void Note(std::uint64_t va) {
        for (std::size_t i = 0, slot = Hash(va); i < Slots; ++i, slot = (slot + 1) % Slots) {
            const std::uint64_t va_seen = va_slots[slot].load(std::memory_order_relaxed);
            if (va_seen == 0) {
                va_slots[slot].store(va, std::memory_order_release);
            } else if (va_seen != va) {
                continue;
            }
            if constexpr (has_counts) {
                Bump(c_hits[slot]);
            }
            return;
        }
        Bump(c_overflow);
    }
    [[nodiscard]] bool Contains(std::uint64_t va) const {
        for (std::size_t i = 0, slot = Hash(va); i < Slots; ++i, slot = (slot + 1) % Slots) {
            const std::uint64_t va_seen = va_slots[slot].load(std::memory_order_acquire);
            if (va_seen == va) {
                return true;
            }
            if (va_seen == 0) {
                return false;
            }
        }
        return false;
    }
};

struct State {
    std::array<std::array<Counter, 256>, NumEngines> c_packets{};
    Counter c_predicated{0};
    Counter c_dcbs{0}, c_ib_top{0}, c_ib_nested{0}, c_max_depth{0};
    Counter c_asc_submissions{0}, c_compute_ibs{0};
    Counter c_ce_dumps{0}, cb_ce_dumps{0};
    Counter c_wait_register{0};
    /// WAIT_REG_MEM on memory, by address; addresses a GPU packet writes (EOP, EOS, RELEASE_MEM,
    /// WRITE_DATA to memory): a wait on one of them waits for the GPU, on any other for the CPU.
    AddressTable<true> wait_addresses;
    AddressTable<false> gpu_written;
    std::atomic<bool> has_started{false};
    std::chrono::steady_clock::time_point start;
    std::uint32_t first_frame = 0;
    struct DriverSlot {
        std::atomic<const char*> sz_function{nullptr};
        Counter c_calls{0};
    };
    std::array<DriverSlot, 64> driver{};
    Counter c_driver_overflow{0};
};

State& Get() {
    static State s_state;
    return s_state;
}

void Start(State& state) {
    if (!state.has_started.load(std::memory_order_relaxed)) {
        state.start = std::chrono::steady_clock::now();
        state.first_frame = PsStats::g_frame_number.load(std::memory_order_relaxed);
        state.has_started.store(true, std::memory_order_release);
    }
}

void NoteGpuWrite(State& state, std::uint64_t va) {
    if (va != 0) {
        state.gpu_written.Note(va & ~3ull);
    }
}

void ScanPacket(State& state, Engine engine, const PM4Header* p_header, int depth) {
    const PM4ItOpcode op = p_header->type3.opcode;
    Bump(state.c_packets[engine][static_cast<std::uint8_t>(op)]);
    if (p_header->type3.predicate.Value() != AmdGpu::PM4Predicate::PredDisable) {
        Bump(state.c_predicated);
    }
    switch (op) {
    case PM4ItOpcode::IndirectBuffer:
        if (engine == Compute) {
            Bump(state.c_compute_ibs);
        } else {
            Bump(depth == 0 ? state.c_ib_top : state.c_ib_nested);
        }
        break;
    case PM4ItOpcode::DumpConstRam: {
        const auto* p_dump = reinterpret_cast<const AmdGpu::PM4DumpConstRam*>(p_header);
        Bump(state.c_ce_dumps);
        Bump(state.cb_ce_dumps, p_dump->Size());
        break;
    }
    case PM4ItOpcode::WaitRegMem: {
        const auto* p_wait = reinterpret_cast<const PM4CmdWaitRegMem*>(p_header);
        if (p_wait->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory) {
            state.wait_addresses.Note(p_wait->Address<std::uint64_t>());
        } else {
            Bump(state.c_wait_register);
        }
        break;
    }
    case PM4ItOpcode::EventWriteEop: {
        const auto* p_eop = reinterpret_cast<const AmdGpu::PM4CmdEventWriteEop*>(p_header);
        if (p_eop->data_sel.Value() != AmdGpu::DataSelect::None) {
            NoteGpuWrite(state, reinterpret_cast<std::uint64_t>(p_eop->Address<std::uint32_t>()));
        }
        break;
    }
    case PM4ItOpcode::EventWriteEos: {
        const auto* p_eos = reinterpret_cast<const AmdGpu::PM4CmdEventWriteEos*>(p_header);
        if (p_eos->command.Value() == AmdGpu::PM4CmdEventWriteEos::Command::SignalFence) {
            NoteGpuWrite(state, p_eos->Address<std::uint64_t>());
        }
        break;
    }
    case PM4ItOpcode::ReleaseMem: {
        const auto* p_release = reinterpret_cast<const AmdGpu::PM4CmdReleaseMem*>(p_header);
        if (p_release->data_sel.Value() != AmdGpu::DataSelect::None) {
            NoteGpuWrite(state, p_release->Address<std::uint64_t>());
        }
        break;
    }
    case PM4ItOpcode::WriteData: {
        const auto* p_write = reinterpret_cast<const AmdGpu::PM4CmdWriteData*>(p_header);
        if (p_write->dst_sel.Value() != 0) {
            NoteGpuWrite(state, p_write->Address<std::uint64_t>());
        }
        break;
    }
    default:
        break;
    }
}

void Scan(State& state, Engine engine, std::span<const std::uint32_t> words, int depth) {
    for (std::size_t at = 0; at < words.size();) {
        const auto* p_header = reinterpret_cast<const PM4Header*>(words.data() + at);
        if (p_header->type == 2) {
            ++at;
            continue;
        }
        if (p_header->type != 3) {
            break;
        }
        const std::size_t c_words = p_header->type3.NumWords() + 1;
        if (at + c_words > words.size()) {
            break;
        }
        ScanPacket(state, engine, p_header, depth);
        at += c_words;
    }
}

/// Appends to a line buffer; the report writes with write(2) because it also runs from the
/// fault handler (runtime_calls_report).
struct Line {
    char sz_text[8192];
    std::size_t cb_used = 0;
    template <typename... Args>
    void Add(const char* sz_format, Args... args) {
        if (cb_used < sizeof(sz_text)) {
            int written = 0;
            if constexpr (sizeof...(Args) == 0) {
                written = std::snprintf(sz_text + cb_used, sizeof(sz_text) - cb_used, "%s", sz_format);
            } else {
                written = std::snprintf(sz_text + cb_used, sizeof(sz_text) - cb_used, sz_format, args...);
            }
            if (written > 0) {
                cb_used = std::min(sizeof(sz_text) - 1, cb_used + std::size_t(written));
            }
        }
    }
    void Flush() {
        Add("\n");
        for (std::size_t off = 0; off < cb_used;) {
            const ssize_t written = write(1, sz_text + off, cb_used - off);
            if (written <= 0) {
                break;
            }
            off += std::size_t(written);
        }
        cb_used = 0;
    }
};

unsigned long long Load(const Counter& counter) {
    return counter.load(std::memory_order_relaxed);
}

void AddPackets(Line& line, const State& state, Engine engine) {
    static constexpr const char* s_names[NumEngines] = {"graphics", "compute", "ce"};
    line.Add("CP census: %s packets:", s_names[engine]);
    bool is_empty = true;
    for (unsigned op = 0; op < 256; ++op) {
        if (const auto c_packets = Load(state.c_packets[engine][op])) {
            const std::string_view name = magic_enum::enum_name(PM4ItOpcode(op));
            if (name.empty()) {
                line.Add(" op_0x%02x=%llu", op, c_packets);
            } else {
                line.Add(" %.*s=%llu", int(name.size()), name.data(), c_packets);
            }
            is_empty = false;
        }
    }
    if (is_empty) {
        line.Add(" total=0");
    }
    line.Flush();
}
} // namespace

void ScanGraphics(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb, int depth) {
    auto& state = Get();
    Start(state);
    if (depth == 0) {
        Bump(state.c_dcbs);
    }
    if (std::uint64_t(depth) > Load(state.c_max_depth)) {
        state.c_max_depth.store(std::uint64_t(depth), std::memory_order_relaxed);
    }
    Scan(state, Graphics, dcb, depth);
    Scan(state, Constant, ccb, depth);
}

void ScanCompute(std::span<const std::uint32_t> acb, bool is_indirect) {
    auto& state = Get();
    Start(state);
    if (!is_indirect) {
        Bump(state.c_asc_submissions);
    }
    Scan(state, Compute, acb, is_indirect ? 1 : 0);
}

void NoteDriverCallSlow(const char* sz_function) {
    auto& state = Get();
    for (auto& slot : state.driver) {
        const char* sz_seen = slot.sz_function.load(std::memory_order_acquire);
        if (sz_seen == nullptr &&
            slot.sz_function.compare_exchange_strong(sz_seen, sz_function, std::memory_order_acq_rel)) {
            sz_seen = sz_function;
        }
        if (sz_seen == sz_function) {
            slot.c_calls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    state.c_driver_overflow.fetch_add(1, std::memory_order_relaxed);
}

} // namespace PsCpCensus

extern "C" void psport_gpu_cp_census_report(void) {
    using namespace PsCpCensus;
    if (!g_is_enabled) {
        return;
    }
    static std::atomic<bool> s_has_reported{false};
    if (s_has_reported.exchange(true)) {
        return;
    }
    auto& state = Get();
    static Line s_line;
    Line& line = s_line;
    if (!state.has_started.load(std::memory_order_acquire)) {
        line.Add("CP census: run: seconds=0 frames=0 dcbs=0 (no command buffer reached the GPU thread)");
        line.Flush();
        return;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
    const std::uint32_t frames = PsStats::g_frame_number.load(std::memory_order_relaxed) - state.first_frame;
    const double per_frame = frames ? 1.0 / frames : 0.0;
    const double per_second = seconds > 0 ? 1.0 / seconds : 0.0;
    std::uint64_t c_totals[NumEngines]{};
    for (unsigned engine = 0; engine < NumEngines; ++engine) {
        for (const auto& counter : state.c_packets[engine]) {
            c_totals[engine] += Load(counter);
        }
    }
    line.Add("CP census: run: seconds=%.1f frames=%u dcbs=%llu dcbs_per_frame=%.2f", seconds, frames,
             Load(state.c_dcbs), Load(state.c_dcbs) * per_frame);
    line.Flush();
    line.Add("CP census: packets: graphics=%llu compute=%llu ce=%llu predicated=%llu graphics_per_frame=%.0f",
             (unsigned long long)c_totals[Graphics], (unsigned long long)c_totals[Compute],
             (unsigned long long)c_totals[Constant], Load(state.c_predicated), c_totals[Graphics] * per_frame);
    line.Flush();
    std::uint64_t c_wait_gpu = 0, c_wait_cpu = 0, c_wait_gpu_addresses = 0, c_wait_cpu_addresses = 0;
    for (std::size_t i = 0; i < state.wait_addresses.Slots; ++i) {
        const std::uint64_t va = state.wait_addresses.va_slots[i].load(std::memory_order_acquire);
        if (va == 0) {
            continue;
        }
        const std::uint64_t c_hits = Load(state.wait_addresses.c_hits[i]);
        if (state.gpu_written.Contains(va & ~3ull)) {
            c_wait_gpu += c_hits;
            ++c_wait_gpu_addresses;
        } else {
            c_wait_cpu += c_hits;
            ++c_wait_cpu_addresses;
        }
    }
    const auto c_set_predication = Load(state.c_packets[Graphics][std::uint8_t(PM4ItOpcode::SetPredication)]);
    const auto c_cond_exec = Load(state.c_packets[Graphics][std::uint8_t(PM4ItOpcode::CondExec)]) +
                             Load(state.c_packets[Compute][std::uint8_t(PM4ItOpcode::CondExec)]);
    const auto c_mem_semaphore = Load(state.c_packets[Graphics][std::uint8_t(PM4ItOpcode::MemSemaphore)]) +
                                 Load(state.c_packets[Compute][std::uint8_t(PM4ItOpcode::MemSemaphore)]);
    line.Add("CP census: sync: set_predication=%llu cond_exec=%llu mem_semaphore=%llu wait_reg_mem_register=%llu "
             "wait_reg_mem_gpu_memory=%llu wait_reg_mem_cpu_memory=%llu gpu_wait_addresses=%llu "
             "cpu_wait_addresses=%llu address_overflow=%llu",
             c_set_predication, c_cond_exec, c_mem_semaphore, Load(state.c_wait_register),
             (unsigned long long)c_wait_gpu, (unsigned long long)c_wait_cpu,
             (unsigned long long)c_wait_gpu_addresses, (unsigned long long)c_wait_cpu_addresses,
             Load(state.wait_addresses.c_overflow) + Load(state.gpu_written.c_overflow));
    line.Flush();
    line.Add("CP census: buffers: indirect_top=%llu indirect_nested=%llu max_depth=%llu asc_submissions=%llu "
             "asc_per_frame=%.2f compute_indirect=%llu ce_dumps=%llu ce_dumps_per_s=%.1f ce_dump_kb_per_frame=%.1f",
             Load(state.c_ib_top), Load(state.c_ib_nested), Load(state.c_max_depth), Load(state.c_asc_submissions),
             Load(state.c_asc_submissions) * per_frame, Load(state.c_compute_ibs), Load(state.c_ce_dumps),
             Load(state.c_ce_dumps) * per_second, Load(state.cb_ce_dumps) * per_frame / 1024.0);
    line.Flush();
    AddPackets(line, state, Graphics);
    AddPackets(line, state, Compute);
    AddPackets(line, state, Constant);
    line.Add("CP census: driver calls:");
    std::uint64_t c_driver = 0;
    for (const auto& slot : state.driver) {
        if (const char* sz_function = slot.sz_function.load(std::memory_order_acquire)) {
            line.Add(" %s=%llu", sz_function, Load(slot.c_calls));
            c_driver += Load(slot.c_calls);
        }
    }
    line.Add(" total=%llu overflow=%llu", (unsigned long long)c_driver, Load(state.c_driver_overflow));
    line.Flush();
}
