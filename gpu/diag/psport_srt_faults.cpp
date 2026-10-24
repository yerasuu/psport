// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_srt_faults.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <fmt/format.h>
#include "common/decoder.h"
#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/regs.h"

extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));
extern "C" std::int64_t runtime_memory_read_guest_signal(void* p_out, uintptr_t va_range, uint64_t cb_range);

namespace PsSrtFaults {

constinit thread_local Origin g_origin{};

namespace {

constexpr std::uint32_t MAX_DEPTH = 8;
constexpr std::size_t C_SLOTS = 64;
constexpr std::uint32_t C_LINES_PER_SHADER = 4;
constexpr std::uint32_t COMPUTED_OFFSET = ~0u;
// Bounds the search for the SET_SH_REG to command buffers of 4 MiB before the packet.
constexpr std::size_t MAX_SCAN_DWORDS = std::size_t{1} << 20;
constexpr std::size_t SCAN_CHUNK_DWORDS = 512;

// off_dw_levels[0] is the user-data SGPR holding the root table's pointer; each next entry is the
// dword offset, in the table before it, of the pointer to the next table.
struct Table {
    std::uint64_t pgm_hash;
    std::uint32_t hw_stage;
    std::uint32_t c_levels;
    std::array<std::uint32_t, MAX_DEPTH> off_dw_levels;
    bool operator==(const Table&) const = default;
};

struct Load {
    Table table;
    std::uint32_t off_dw;
    bool is_pointer;
};

struct Slot {
    Table table;
    std::uint64_t c_faults;
    bool is_logged;
    bool has_packet;
};

std::mutex s_mutex;
std::array<Slot, C_SLOTS> s_slots{};
std::size_t s_c_slots = 0;
std::uint64_t s_c_untracked = 0;
std::uint64_t s_c_unattributed = 0;
std::atomic<std::uint64_t> s_c_abandoned{0};
std::atomic<bool> s_is_summary_added{false};

const char* NameStage(std::uint32_t hw_stage) {
    static constexpr const char* s_sz_stages[] = {"ps", "vs", "gs", "es", "hs", "ls", "cs"};
    return hw_stage < std::size(s_sz_stages) ? s_sz_stages[hw_stage] : "?";
}

std::uint32_t FindUserDataShOffset(std::uint32_t hw_stage) {
    using AmdGpu::Regs;
    static constexpr std::array<std::size_t, 7> s_off_user_data = {
        offsetof(Regs, ps_program.user_data), offsetof(Regs, vs_program.user_data),
        offsetof(Regs, gs_program.user_data), offsetof(Regs, es_program.user_data),
        offsetof(Regs, hs_program.user_data), offsetof(Regs, ls_program.user_data),
        offsetof(Regs, cs_program.user_data)};
    return static_cast<std::uint32_t>(s_off_user_data[hw_stage] / sizeof(std::uint32_t)) - Regs::ShRegWordOffset;
}

std::string FormatTable(const Table& table) {
    std::string text = table.off_dw_levels[0] == COMPUTED_OFFSET ? std::string("s[computed]")
                                                                 : fmt::format("s{}", table.off_dw_levels[0]);
    for (std::uint32_t i = 1; i < table.c_levels; ++i) {
        text += table.off_dw_levels[i] == COMPUTED_OFFSET ? std::string("[computed]")
                                                          : fmt::format("[{}]", table.off_dw_levels[i]);
    }
    return text;
}

// Walkers are straight-line code (GenerateSrtProgram): a table level opens with `push rdi` and
// `mov rdi, [rdi + offset]` and closes with `pop rdi`, values are `mov r10d, [rdi + offset]`, and
// rdi starts at the user data. Replaying those up to the faulting load gives the table it read.
bool DecodeLoad(const Shader::Info& info, const std::uint8_t* p_code, Load& out_load) {
    const auto* p_begin = reinterpret_cast<const std::uint8_t*>(info.srt_info.walker_func);
    const auto* p_end = p_begin + info.srt_info.walker_func_size;
    if (p_code < p_begin || p_code >= p_end) {
        return false;
    }
    std::array<std::uint32_t, MAX_DEPTH> off_dw_levels{};
    std::uint32_t c_levels = 0;
    for (const auto* p = p_begin; p <= p_code;) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                instruction, operands, const_cast<std::uint8_t*>(p), static_cast<u64>(p_end - p)))) {
            return false;
        }
        const bool is_rdi =
            operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RDI;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_PUSH && is_rdi) {
            if (c_levels == MAX_DEPTH) {
                return false;
            }
            off_dw_levels[c_levels++] = COMPUTED_OFFSET;
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_POP && is_rdi) {
            if (c_levels == 0) {
                return false;
            }
            --c_levels;
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   operands[1].mem.base == ZYDIS_REGISTER_RDI) {
            const std::uint32_t off_dw = operands[1].mem.index == ZYDIS_REGISTER_NONE
                                             ? static_cast<std::uint32_t>(operands[1].mem.disp.value / 4)
                                             : COMPUTED_OFFSET;
            if (is_rdi && c_levels > 0) {
                off_dw_levels[c_levels - 1] = off_dw;
            }
            if (p == p_code) {
                // A pointer load reads the table one level up from the one it opens.
                const std::uint32_t c_table = is_rdi && c_levels > 0 ? c_levels - 1 : c_levels;
                if (c_table == 0) {
                    return false;
                }
                out_load.table = {info.pgm_hash, static_cast<std::uint32_t>(info.hw_stage), c_table, {}};
                std::copy_n(off_dw_levels.begin(), c_table, out_load.table.off_dw_levels.begin());
                out_load.off_dw = off_dw;
                out_load.is_pointer = is_rdi;
                return true;
            }
        }
        p += instruction.length;
    }
    return false;
}

std::string DescribeSource(const Origin& origin, std::uint32_t hw_stage, std::uint32_t sgpr) {
    if (!origin.p_dcb || !origin.p_packet) {
        return "no command buffer packet on this thread (draw preparation or replay)";
    }
    const auto va_dcb = reinterpret_cast<std::uintptr_t>(origin.p_dcb);
    const auto va_packet = reinterpret_cast<std::uintptr_t>(origin.p_packet);
    AmdGpu::PM4Header packet{};
    if (runtime_memory_read_guest_signal(&packet.raw, va_packet, sizeof(packet.raw)) != sizeof(packet.raw)) {
        return fmt::format("packet at {:#x} unreadable", va_packet);
    }
    const std::string where = fmt::format("packet {:#x} at {:#x}", static_cast<std::uint32_t>(packet.type3.opcode.Value()),
                                          va_packet);
    if (va_packet < va_dcb || (va_packet - va_dcb) / sizeof(std::uint32_t) > MAX_SCAN_DWORDS) {
        return fmt::format("{}, command buffer from {:#x} not searched", where, va_dcb);
    }
    if (sgpr == COMPUTED_OFFSET) {
        return fmt::format("{}, user data register computed by the walker, not searched", where);
    }
    const std::size_t c_dwords = (va_packet - va_dcb) / sizeof(std::uint32_t);
    // Read in stack-sized chunks rather than a heap copy of up to 4 MiB, since this runs in the
    // fault handler.
    std::array<std::uint32_t, SCAN_CHUNK_DWORDS> chunk;
    std::size_t i_chunk = 0;
    std::size_t c_chunk = 0;
    const auto read_dword = [&](std::size_t i, std::uint32_t& out_value) {
        if (i < i_chunk || i >= i_chunk + c_chunk) {
            i_chunk = i;
            c_chunk = std::min(chunk.size(), c_dwords - i);
            const std::size_t cb_chunk = c_chunk * sizeof(chunk[0]);
            if (runtime_memory_read_guest_signal(chunk.data(), va_dcb + i * sizeof(chunk[0]), cb_chunk) !=
                static_cast<std::int64_t>(cb_chunk)) {
                c_chunk = 0;
                return false;
            }
        }
        out_value = chunk[i - i_chunk];
        return true;
    };
    const std::uint32_t off_sh = FindUserDataShOffset(hw_stage) + sgpr;
    std::uintptr_t va_set = 0;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < c_dwords;) {
        AmdGpu::PM4Header header{};
        if (!read_dword(i, header.raw)) {
            return fmt::format("{}, command buffer from {:#x} unreadable at {:#x}", where, va_dcb,
                               va_dcb + i * sizeof(std::uint32_t));
        }
        if (header.type == 2) {
            ++i;
            continue;
        }
        if (header.type != 3) {
            break;
        }
        const std::uint32_t c_body = header.type3.NumWords();
        std::uint32_t off_first = 0;
        if (header.type3.opcode == AmdGpu::PM4ItOpcode::SetShReg && c_body >= 2 && i + 1 + c_body <= c_dwords &&
            read_dword(i + 1, off_first)) {
            off_first &= 0xFFFF;
            if (off_sh >= off_first && off_sh - off_first < c_body - 1 &&
                read_dword(i + 2 + off_sh - off_first, value)) {
                va_set = va_dcb + i * sizeof(std::uint32_t);
            }
        }
        i += 1 + c_body;
    }
    if (!va_set) {
        return fmt::format("{}, s{} not written by a SET_SH_REG in its command buffer from {:#x}", where, sgpr,
                           va_dcb);
    }
    return fmt::format("{}, s{} = {:#x} written by the SET_SH_REG at {:#x}", where, sgpr, value, va_set);
}

void Summarize(char* sz_out, std::size_t cb_out) {
    std::unique_lock lock{s_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        std::snprintf(sz_out, cb_out, "SRT walker faults: summary: busy at exit\n");
        return;
    }
    std::array<const Slot*, C_SLOTS> p_slots{};
    std::uint64_t c_faults = s_c_untracked + s_c_unattributed;
    for (std::size_t i = 0; i < s_c_slots; ++i) {
        p_slots[i] = &s_slots[i];
        c_faults += s_slots[i].c_faults;
    }
    std::sort(p_slots.begin(), p_slots.begin() + static_cast<std::ptrdiff_t>(s_c_slots),
              [](const Slot* p_left, const Slot* p_right) { return p_left->c_faults > p_right->c_faults; });
    std::string text = fmt::format(
        "SRT walker faults: summary: {} faults, {} tables, {} untracked, {} unattributed, {} speculative walks "
        "abandoned:",
        c_faults, s_c_slots, s_c_untracked, s_c_unattributed, s_c_abandoned.load(std::memory_order_relaxed));
    for (std::size_t i = 0; i < s_c_slots; ++i) {
        text += fmt::format(" {} {:#018x} {} {}", NameStage(p_slots[i]->table.hw_stage), p_slots[i]->table.pgm_hash,
                            FormatTable(p_slots[i]->table), p_slots[i]->c_faults);
    }
    if (text.size() + 2 > cb_out && cb_out > 4) {
        text.resize(cb_out - 5);
        text += "...";
    }
    std::snprintf(sz_out, cb_out, "%s\n", text.c_str());
}

} // namespace

// NOTE: runs inside the SIGSEGV handler, so the mutex, fmt's allocations and the logger are safe
// only because the fault is synchronous: it comes from walker code (straight-line loads), whose
// callers (Info::RefreshFlatBuf, the draw pipe's table check) hold neither s_mutex nor the
// logger's lock and are not inside the allocator. Guest memory is read only through
// runtime_memory_read_guest_signal and user data only within c_user_data, so Note itself
// cannot fault again while holding s_mutex.
void NoteAbandoned(const std::uint8_t* p_code, std::uintptr_t va_load) {
    if (!s_is_summary_added.exchange(true, std::memory_order_relaxed)) {
        runtime_calls_add_summary(&Summarize);
    }
    const std::uint64_t c_abandoned = s_c_abandoned.fetch_add(1, std::memory_order_relaxed) + 1;
    if (std::has_single_bit(c_abandoned)) {
        LOG_WARNING(Render_Recompiler,
                    "SRT walker load at {} ahead of the command processor faulted at {:#x}: walk abandoned, "
                    "the draw is left to the GPU thread ({} so far)",
                    static_cast<const void*>(p_code), va_load, c_abandoned);
    }
}

std::uint64_t AbandonedWalks() {
    return s_c_abandoned.load(std::memory_order_relaxed);
}

void Note(const std::uint8_t* p_code, std::uintptr_t va_load) {
    if (!s_is_summary_added.exchange(true, std::memory_order_relaxed)) {
        runtime_calls_add_summary(&Summarize);
    }
    const Origin origin = g_origin;
    Load load{};
    const bool is_attributed = origin.p_info && DecodeLoad(*origin.p_info, p_code, load);
    std::uint64_t c_fault = 0;
    {
        std::scoped_lock lock{s_mutex};
        if (!is_attributed) {
            c_fault = ++s_c_unattributed;
        } else {
            Slot* const p_first = s_slots.data();
            Slot* const p_end = p_first + s_c_slots;
            Slot* const p_slot =
                std::find_if(p_first, p_end, [&](const Slot& slot) { return slot.table == load.table; });
            if (p_slot == p_end) {
                if (s_c_slots == C_SLOTS) {
                    ++s_c_untracked;
                    return;
                }
                *p_slot = {load.table, 0, false, false};
                ++s_c_slots;
            }
            c_fault = ++p_slot->c_faults;
            const bool has_packet = origin.p_packet != nullptr;
            // A fault seen first on a draw preparation worker is logged again once the command
            // processor thread meets it, since only that thread knows the packet.
            if (p_slot->is_logged && (p_slot->has_packet || !has_packet)) {
                return;
            }
            if (!p_slot->is_logged &&
                std::count_if(p_first, p_end, [&](const Slot& slot) {
                    return slot.is_logged && slot.table.pgm_hash == load.table.pgm_hash;
                }) >= C_LINES_PER_SHADER) {
                return;
            }
            p_slot->is_logged = true;
            p_slot->has_packet = has_packet;
        }
    }
    if (!is_attributed) {
        if (std::has_single_bit(c_fault)) {
            LOG_ERROR(Render_Recompiler,
                      "SRT walker load at {} faulted at {:#x} outside the walker this thread announced ({} so far)",
                      static_cast<const void*>(p_code), va_load, c_fault);
        }
        return;
    }
    const std::uint32_t sgpr = load.table.off_dw_levels[0];
    std::string root = "computed";
    if (sgpr != COMPUTED_OFFSET) {
        root = fmt::format("s{}:s{} = ", sgpr, sgpr + 1);
        if (!origin.p_user_data) {
            root += "unknown";
        } else if (origin.c_user_data >= 2 && sgpr <= origin.c_user_data - 2) {
            // Unmasked: the high dword tells a pointer from a sharp read as one.
            const std::uint64_t raw_root =
                origin.p_user_data[sgpr] | (std::uint64_t{origin.p_user_data[sgpr + 1]} << 32);
            root += fmt::format("{:#018x}", raw_root);
        } else {
            root += fmt::format("past the {} user data registers", origin.c_user_data);
        }
    }
    LOG_ERROR(Render_Recompiler,
              "SRT walker load faulted: {} shader {:#018x}, user data {}, table {}, {} at dword {} "
              "(address {:#x}, fault {} of this table); {}",
              NameStage(load.table.hw_stage), load.table.pgm_hash, root, FormatTable(load.table),
              load.is_pointer ? "pointer" : "value",
              load.off_dw == COMPUTED_OFFSET ? std::string("computed") : std::to_string(load.off_dw), va_load,
              c_fault, DescribeSource(origin, load.table.hw_stage, sgpr));
}

} // namespace PsSrtFaults
