// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the generic red-zone guard pass (#803). On Windows the kernel writes the exception frame
// into the 128-byte System V red zone below the faulting rsp before any handler runs, over the live
// locals a guest leaf keeps there. For every function the .eh_frame bounds, this pass finds the
// leaves that use the red zone and sends each of their non-stack memory accesses (the ones that can
// fault on a GPU-tracked page) through a trampoline that lowers rsp past the red zone first
// (PsGuestCode::RelocateBelowRedZone). It is the shadPS4 #4802 model without liveness, Windows-only
// by host capability; it never special-cases a title. Everything it cannot prove safe is skipped
// and counted (nothing silent).
#include "../psport_gpu.h"
#include "psport_guest_code.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include <Zydis/Zydis.h>

#include "common/decoder.h"

extern "C" {
const char* runtime_env(const char* sz_name, const char* sz_default);
void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));
}

namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::size_t NearJumpBytes = 5;
constexpr std::int64_t RedZoneBytes = 128;
/// GCC and clang on this target emit 'zR' CIEs with DW_EH_PE_pcrel|sdata4 FDE pointers; the loader
/// assumes the same for .eh_frame_hdr (loader_self.c). Used when a CIE lacks an 'R' augmentation.
constexpr u8 DefaultFdeEncoding = 0x1b;

struct Counts {
    u64 functions = 0, red_zone_functions = 0, non_leaf = 0, protected_sites = 0;
    u64 skip_short = 0, skip_branch_inside = 0, skip_rsp = 0, skip_rip = 0;
    u64 skip_patched = 0, skip_overlap = 0, skip_reach = 0, skip_decode = 0, fde_errors = 0;
    void Add(const Counts& o) {
        functions += o.functions;
        red_zone_functions += o.red_zone_functions;
        non_leaf += o.non_leaf;
        protected_sites += o.protected_sites;
        skip_short += o.skip_short;
        skip_branch_inside += o.skip_branch_inside;
        skip_rsp += o.skip_rsp;
        skip_rip += o.skip_rip;
        skip_patched += o.skip_patched;
        skip_overlap += o.skip_overlap;
        skip_reach += o.skip_reach;
        skip_decode += o.skip_decode;
        fde_errors += o.fde_errors;
    }
};

/// Why the pass drops functions and where non-leaf red-zone uses sit; filled only when the diagnostics
/// are on (report mode or PS_RED_ZONE_GUARD_DIAG=1), per module, with fixed-size buffers.
struct Diag {
    static constexpr std::size_t MaxFailures = 8;
    static constexpr std::size_t MaxStatuses = 16;
    static constexpr std::size_t FailureBytes = 16;

    struct Failure {
        u64 va = 0, off = 0, cb_function = 0;
        ZyanStatus status = 0;
        ZydisMnemonic prev = ZYDIS_MNEMONIC_INVALID;
        bool is_after_terminator = false;
        u8 c_bytes = 0;
        std::array<u8, FailureBytes> bytes{};
    };
    struct StatusBin {
        ZyanStatus status = 0;
        u64 c = 0;
    };

    u64 va_code_end = 0; // bounds the raw bytes read at a failure
    u64 cb_fde = 0, cb_dropped = 0;
    u64 c_length_zero = 0, c_crosses_end = 0, c_status_other = 0;
    std::size_t c_bins = 0, c_failures = 0;
    std::array<StatusBin, MaxStatuses> bins{};
    std::array<Failure, MaxFailures> failures{};
    u64 c_failures_total = 0;
    u64 c_after_terminator = 0;
    u64 non_leaf = 0, non_leaf_rsp = 0, non_leaf_rbp = 0, non_leaf_before_call = 0, non_leaf_between = 0,
        non_leaf_after_call = 0, non_leaf_lea_only = 0;
};

Counts g_totals;
bool g_is_summary_registered = false;

const char* DecodeStatusName(ZyanStatus status) {
    switch (status) {
    case ZYDIS_STATUS_NO_MORE_DATA: return "NO_MORE_DATA";
    case ZYDIS_STATUS_DECODING_ERROR: return "DECODING_ERROR";
    case ZYDIS_STATUS_INSTRUCTION_TOO_LONG: return "INSTRUCTION_TOO_LONG";
    case ZYDIS_STATUS_BAD_REGISTER: return "BAD_REGISTER";
    case ZYDIS_STATUS_ILLEGAL_LOCK: return "ILLEGAL_LOCK";
    case ZYDIS_STATUS_ILLEGAL_LEGACY_PFX: return "ILLEGAL_LEGACY_PFX";
    case ZYDIS_STATUS_ILLEGAL_REX: return "ILLEGAL_REX";
    case ZYDIS_STATUS_INVALID_MAP: return "INVALID_MAP";
    case ZYDIS_STATUS_MALFORMED_EVEX: return "MALFORMED_EVEX";
    case ZYDIS_STATUS_MALFORMED_MVEX: return "MALFORMED_MVEX";
    case ZYDIS_STATUS_INVALID_MASK: return "INVALID_MASK";
    case ZYDIS_STATUS_IMPOSSIBLE_INSTRUCTION: return "IMPOSSIBLE_INSTRUCTION";
    default: return "UNKNOWN";
    }
}

void NoteDecodeFailure(Diag& diag, u64 va_begin, u64 va_end, u64 va, ZyanStatus status, bool is_status_failure,
                       const ZydisDecodedInstruction& instruction, ZydisMnemonic prev, bool is_after_terminator) {
    diag.cb_dropped += va_end - va_begin;
    ++diag.c_failures_total;
    diag.c_after_terminator += is_after_terminator;
    if (!is_status_failure) {
        ++(instruction.length == 0 ? diag.c_length_zero : diag.c_crosses_end);
    } else {
        std::size_t i = 0;
        while (i < diag.c_bins && diag.bins[i].status != status) {
            ++i;
        }
        if (i < diag.c_bins) {
            ++diag.bins[i].c;
        } else if (diag.c_bins < Diag::MaxStatuses) {
            diag.bins[diag.c_bins++] = {status, 1};
        } else {
            ++diag.c_status_other;
        }
    }
    if (diag.c_failures >= Diag::MaxFailures) {
        return;
    }
    Diag::Failure& failure = diag.failures[diag.c_failures++];
    failure.va = va;
    failure.off = va - va_begin;
    failure.cb_function = va_end - va_begin;
    failure.status = is_status_failure ? status : 0;
    failure.prev = prev;
    failure.is_after_terminator = is_after_terminator;
    const u64 va_limit = diag.va_code_end > va_end ? diag.va_code_end : va_end;
    failure.c_bytes = u8(va_limit - va < Diag::FailureBytes ? va_limit - va : Diag::FailureBytes);
    std::memcpy(failure.bytes.data(), reinterpret_cast<const void*>(va), failure.c_bytes);
}

u64 ReadUleb(const u8*& p, const u8* p_end) {
    u64 value = 0;
    for (int shift = 0; p < p_end && shift < 64; shift += 7) {
        const u8 byte = *p++;
        value |= u64(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            break;
        }
    }
    return value;
}

std::int64_t ReadSleb(const u8*& p, const u8* p_end) {
    std::int64_t value = 0;
    int shift = 0;
    u8 byte = 0;
    do {
        if (p >= p_end || shift >= 64) {
            break;
        }
        byte = *p++;
        value |= std::int64_t(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) {
        value |= -(std::int64_t(1) << shift);
    }
    return value;
}

/// A DWARF-encoded value at p (advanced), with va_field its own address for pcrel application.
/// false when the encoding or format is one the pass does not read (counted by the caller).
bool ReadEncoded(const u8*& p, const u8* p_end, u8 enc, u64 va_field, u64& out) {
    if (enc == 0xff) {
        return false;
    }
    std::int64_t raw = 0;
    switch (enc & 0x0f) {
    case 0x00: // absptr (8 bytes on this target)
    case 0x04: // udata8
    case 0x0c: // sdata8
        if (p_end - p < 8) {
            return false;
        }
        std::memcpy(&raw, p, 8);
        p += 8;
        break;
    case 0x01:
        raw = std::int64_t(ReadUleb(p, p_end));
        break;
    case 0x09:
        raw = ReadSleb(p, p_end);
        break;
    case 0x02: {
        if (p_end - p < 2) {
            return false;
        }
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        p += 2;
        raw = v;
        break;
    }
    case 0x0a: {
        if (p_end - p < 2) {
            return false;
        }
        std::int16_t v;
        std::memcpy(&v, p, 2);
        p += 2;
        raw = v;
        break;
    }
    case 0x03: {
        if (p_end - p < 4) {
            return false;
        }
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        raw = v;
        break;
    }
    case 0x0b: {
        if (p_end - p < 4) {
            return false;
        }
        std::int32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        raw = v;
        break;
    }
    default:
        return false;
    }
    switch (enc & 0x70) {
    case 0x00: // absolute, and (with the application bits masked off) a plain length
        out = u64(raw);
        return true;
    case 0x10: // pcrel
        out = va_field + u64(raw);
        return true;
    default:
        return false;
    }
}

/// The FDE pointer encoding a CIE declares in its 'zR' augmentation, else the target's default.
u8 CieFdeEncoding(const u8* p_record, std::size_t cb_record) {
    const u8* p = p_record + 8; // past length and the zero CIE id
    const u8* p_end = p_record + cb_record;
    if (p >= p_end) {
        return DefaultFdeEncoding;
    }
    const u8 version = *p++;
    const char* sz_aug = reinterpret_cast<const char*>(p);
    const std::size_t c_aug = strnlen(sz_aug, std::size_t(p_end - p));
    p += c_aug + 1;
    ReadUleb(p, p_end); // code alignment factor
    ReadSleb(p, p_end); // data alignment factor
    if (version == 1) {
        if (p < p_end) {
            ++p; // return address register (ubyte before version 3)
        }
    } else {
        ReadUleb(p, p_end);
    }
    if (c_aug == 0 || sz_aug[0] != 'z') {
        return DefaultFdeEncoding;
    }
    const u64 cb_aug_data = ReadUleb(p, p_end);
    const u8* p_aug_end = p + cb_aug_data <= p_end ? p + cb_aug_data : p_end;
    for (std::size_t i = 1; i < c_aug && p < p_aug_end; ++i) {
        switch (sz_aug[i]) {
        case 'R':
            return *p;
        case 'L':
            ++p;
            break;
        case 'P': {
            const u8 personality_enc = *p++;
            u64 ignored;
            ReadEncoded(p, p_aug_end, personality_enc, 0, ignored);
            break;
        }
        default: // 'S', 'B', 'G': no data
            break;
        }
    }
    return DefaultFdeEncoding;
}

struct Insn {
    u64 va = 0;
    u32 len = 0;
    bool is_site = false;
    bool uses_rsp = false;
    bool uses_rip = false;
    bool is_control = false;
};

bool IsStackPointer(ZydisRegister reg) { return reg == ZYDIS_REGISTER_RSP; }

/// Red-zone-form operands of one function, positioned by the number of calls seen before each.
struct RedZoneUses {
    bool is_rsp = false, is_rbp = false, is_before_call = false, is_non_lea = false;
    u64 k_min_after_call = ~u64(0), k_max = 0;
};

bool OverlapsRange(u64 va_begin, u64 va_end, const PsRedZoneRange* p_ranges, std::size_t c_ranges) {
    for (std::size_t i = 0; i < c_ranges; ++i) {
        if (va_begin < p_ranges[i].va_end && p_ranges[i].va_begin < va_end) {
            return true;
        }
    }
    return false;
}

void AnalyzeFunction(u64 va_begin, u64 va_end, const PsRedZoneRange* p_patched, std::size_t c_patched,
                     bool is_dry_run, Counts& counts, Diag* p_diag = nullptr) {
    ++counts.functions;
    if (p_diag) {
        p_diag->cb_fde += va_end - va_begin;
    }
    RedZoneUses uses;
    u64 c_calls = 0;
    ZydisMnemonic prev_mnemonic = ZYDIS_MNEMONIC_INVALID;
    bool is_prev_terminator = false;
    std::vector<Insn> instructions;
    std::map<u64, bool> branch_targets;
    bool has_call = false;
    bool has_indirect_jump = false;
    bool uses_red_zone = false;
    std::int64_t rsp_off = 0; // rsp relative to the function's entry rsp (0; negative going down)
    bool is_rsp_known = true;
    bool is_fp_set = false;
    std::int64_t fp_off = 0; // rbp relative to entry rsp, once it is a frame pointer

    for (u64 va = va_begin; va < va_end;) {
        ZydisDecodedInstruction instruction{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        const ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(
            instruction, operands.data(), reinterpret_cast<void*>(va), va_end - va);
        const bool is_status_failure = !ZYAN_SUCCESS(status);
        if (is_status_failure || instruction.length == 0 || va + instruction.length > va_end) {
            ++counts.skip_decode;
            if (p_diag) {
                NoteDecodeFailure(*p_diag, va_begin, va_end, va, status, is_status_failure, instruction,
                                  prev_mnemonic, is_prev_terminator);
            }
            return; // a mid-function decode failure makes the rest unsafe to reason about
        }
        Insn insn;
        insn.va = va;
        insn.len = instruction.length;

        const ZydisMnemonic mnemonic = instruction.mnemonic;
        const ZydisInstructionCategory category = instruction.meta.category;
        has_call = has_call || category == ZYDIS_CATEGORY_CALL;
        const bool is_branch = category == ZYDIS_CATEGORY_COND_BR || category == ZYDIS_CATEGORY_UNCOND_BR;
        insn.is_control = is_branch || category == ZYDIS_CATEGORY_CALL || category == ZYDIS_CATEGORY_RET ||
                          category == ZYDIS_CATEGORY_INTERRUPT || category == ZYDIS_CATEGORY_SYSCALL ||
                          category == ZYDIS_CATEGORY_SYSRET;

        bool is_site = false;
        for (u8 i = 0; i < instruction.operand_count; ++i) {
            const ZydisDecodedOperand& operand = operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER && IsStackPointer(operand.reg.value)) {
                insn.uses_rsp = true;
            }
            if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
                insn.uses_rip = true;
                if (is_branch || category == ZYDIS_CATEGORY_CALL) {
                    branch_targets[va + instruction.length + u64(operand.imm.value.s)] = true;
                }
            }
            if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) {
                continue;
            }
            const ZydisRegister base = operand.mem.base;
            if (base == ZYDIS_REGISTER_RSP || operand.mem.index == ZYDIS_REGISTER_RSP) {
                insn.uses_rsp = true;
            }
            if (base == ZYDIS_REGISTER_RIP || base == ZYDIS_REGISTER_EIP) {
                insn.uses_rip = true;
                continue;
            }
            const bool is_stack_base = base == ZYDIS_REGISTER_RSP || (is_fp_set && base == ZYDIS_REGISTER_RBP);
            if (is_stack_base && is_rsp_known) {
                const std::int64_t anchor = base == ZYDIS_REGISTER_RSP ? rsp_off : fp_off;
                const std::int64_t addr_off = anchor + operand.mem.disp.value;
                if (addr_off < rsp_off && addr_off >= rsp_off - RedZoneBytes) {
                    uses_red_zone = true;
                    uses.is_rsp = uses.is_rsp || base == ZYDIS_REGISTER_RSP;
                    uses.is_rbp = uses.is_rbp || base == ZYDIS_REGISTER_RBP;
                    uses.is_non_lea = uses.is_non_lea || mnemonic != ZYDIS_MNEMONIC_LEA;
                    if (c_calls == 0) {
                        uses.is_before_call = true;
                    } else if (c_calls < uses.k_min_after_call) {
                        uses.k_min_after_call = c_calls;
                    }
                    uses.k_max = c_calls > uses.k_max ? c_calls : uses.k_max;
                }
            }
            if (!is_stack_base && base != ZYDIS_REGISTER_NONE) {
                is_site = true; // a memory access the GPU trap can fault; not the (untrapped) stack
            }
        }

        if (is_branch && instruction.operand_count > 0 && operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            has_indirect_jump = true;
        }

        // Track rsp so [rsp-x]/[rbp-x] below the current top is recognised as the red zone.
        if (mnemonic == ZYDIS_MNEMONIC_PUSH) {
            rsp_off -= 8;
        } else if (mnemonic == ZYDIS_MNEMONIC_POP) {
            rsp_off += 8;
        } else if ((mnemonic == ZYDIS_MNEMONIC_SUB || mnemonic == ZYDIS_MNEMONIC_ADD) &&
                   operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RSP &&
                   operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            rsp_off += (mnemonic == ZYDIS_MNEMONIC_SUB ? -1 : 1) * std::int64_t(operands[1].imm.value.s);
        } else if (mnemonic == ZYDIS_MNEMONIC_LEA && operands[0].reg.value == ZYDIS_REGISTER_RSP &&
                   operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[1].mem.base == ZYDIS_REGISTER_RSP &&
                   operands[1].mem.index == ZYDIS_REGISTER_NONE) {
            rsp_off += operands[1].mem.disp.value;
        } else if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RSP) {
            is_rsp_known = false; // and rsp,-16 and the like: stop reasoning about the red zone
        } else if (!is_fp_set && (mnemonic == ZYDIS_MNEMONIC_MOV || mnemonic == ZYDIS_MNEMONIC_LEA) &&
                   operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RBP &&
                   is_rsp_known &&
                   ((mnemonic == ZYDIS_MNEMONIC_MOV && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                     operands[1].reg.value == ZYDIS_REGISTER_RSP) ||
                    (mnemonic == ZYDIS_MNEMONIC_LEA && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                     operands[1].mem.base == ZYDIS_REGISTER_RSP && operands[1].mem.index == ZYDIS_REGISTER_NONE))) {
            is_fp_set = true;
            fp_off = rsp_off + (mnemonic == ZYDIS_MNEMONIC_LEA ? operands[1].mem.disp.value : 0);
        }

        c_calls += category == ZYDIS_CATEGORY_CALL;
        prev_mnemonic = mnemonic;
        is_prev_terminator = category == ZYDIS_CATEGORY_RET || category == ZYDIS_CATEGORY_UNCOND_BR ||
                             mnemonic == ZYDIS_MNEMONIC_UD2 || mnemonic == ZYDIS_MNEMONIC_INT3;
        insn.is_site = is_site;
        instructions.push_back(insn);
        va += instruction.length;
    }

    if (!uses_red_zone) {
        return;
    }
    if (has_call) {
        ++counts.non_leaf; // a red-zone access in a non-leaf: the leaf assumption does not hold here
        if (p_diag) {
            ++p_diag->non_leaf;
            p_diag->non_leaf_rsp += uses.is_rsp;
            p_diag->non_leaf_rbp += uses.is_rbp;
            p_diag->non_leaf_before_call += uses.is_before_call;
            p_diag->non_leaf_between += uses.k_min_after_call < c_calls;
            p_diag->non_leaf_after_call += uses.k_max == c_calls;
            p_diag->non_leaf_lea_only += !uses.is_non_lea;
        }
        return;
    }
    ++counts.red_zone_functions;

    const auto has_target_inside = [&](u64 va_begin_span, u64 va_end_span) {
        auto it = branch_targets.upper_bound(va_begin_span);
        return it != branch_targets.end() && it->first < va_end_span;
    };

    for (std::size_t i = 0; i < instructions.size();) {
        if (!instructions[i].is_site) {
            ++i;
            continue;
        }
        if (instructions[i].uses_rip) {
            ++counts.skip_rip;
            ++i;
            continue;
        }
        if (instructions[i].uses_rsp) {
            ++counts.skip_rsp;
            ++i;
            continue;
        }
        std::size_t j = i;
        u64 cb_span = instructions[i].len;
        bool is_extendable = true;
        if (cb_span < NearJumpBytes) {
            if (has_indirect_jump) {
                ++counts.skip_short; // a span cannot cross an unknown jump-table target
                ++i;
                continue;
            }
            while (cb_span < NearJumpBytes && j + 1 < instructions.size()) {
                const Insn& next = instructions[++j];
                if (next.uses_rsp || next.uses_rip || next.is_control) {
                    is_extendable = false; // the span must stay straight-line and rsp/rip-free
                    break;
                }
                cb_span += next.len;
            }
            if (!is_extendable) {
                ++counts.skip_short;
                ++i;
                continue;
            }
            if (cb_span < NearJumpBytes) {
                ++counts.skip_short; // the function ends before a jump fits
                ++i;
                continue;
            }
        }
        const u64 va_span = instructions[i].va;
        const u64 va_span_end = va_span + cb_span;
        if (has_target_inside(va_span, va_span_end)) {
            ++counts.skip_branch_inside; // a jump into the middle of the rewritten bytes
            ++i;
            continue;
        }
        if (OverlapsRange(va_span, va_span_end, p_patched, c_patched)) {
            ++counts.skip_patched;
            ++i;
            continue;
        }
        const PsGuestCode::Error error =
            PsGuestCode::RelocateBelowRedZone("red zone", va_span, std::size_t(cb_span), is_dry_run);
        switch (error) {
        case PsGuestCode::Error::None:
            ++counts.protected_sites;
            break;
        case PsGuestCode::Error::Overlaps:
            ++counts.skip_overlap;
            break;
        case PsGuestCode::Error::UsesStackPointer:
            ++counts.skip_rsp;
            break;
        case PsGuestCode::Error::TooShort:
        case PsGuestCode::Error::NotWholeInstructions:
            ++counts.skip_short;
            break;
        case PsGuestCode::Error::RelativeAddressing:
            ++counts.skip_rip;
            break;
        default: // OutOfReach, TrampolineFailed, ProtectFailed
            ++counts.skip_reach;
            break;
        }
        i = j + 1; // the span (one instruction or several) is handled whole
    }
}

void AnalyzeModule(const PsRedZoneModule& module, const PsRedZoneRange* p_patched, std::size_t c_patched,
                   bool is_dry_run, Counts& counts, Diag* p_diag = nullptr) {
    if (p_diag) {
        p_diag->va_code_end = module.va_code_end;
    }
    if (!module.va_eh_frame || !module.cb_eh_frame) {
        ++counts.fde_errors; // no .eh_frame: its functions cannot be bounded, so none is touched
        return;
    }
    const u8* p_base = reinterpret_cast<const u8*>(module.va_eh_frame);
    const std::size_t cb = module.cb_eh_frame;
    std::map<std::size_t, u8> cie_encodings;
    for (std::size_t off = 0; off + 4 <= cb;) {
        u32 len;
        std::memcpy(&len, p_base + off, 4);
        if (len == 0) {
            break; // terminator
        }
        if (len == 0xffffffff) {
            ++counts.fde_errors; // 64-bit DWARF: this target does not emit it
            break;
        }
        const std::size_t off_id = off + 4;
        if (off_id + 4 > cb) {
            ++counts.fde_errors;
            break;
        }
        const std::size_t off_next = off_id + len;
        if (off_next > cb || off_next <= off) {
            ++counts.fde_errors;
            break;
        }
        u32 id;
        std::memcpy(&id, p_base + off_id, 4);
        if (id == 0) {
            cie_encodings[off] = CieFdeEncoding(p_base + off, off_next - off);
        } else if (id <= off_id) {
            const std::size_t off_cie = off_id - id;
            const auto it = cie_encodings.find(off_cie);
            const u8 enc = it != cie_encodings.end() ? it->second : DefaultFdeEncoding;
            const std::size_t off_pc = off_id + 4;
            const u8* p = p_base + off_pc;
            const u8* p_end = p_base + off_next;
            u64 va_begin = 0;
            u64 range = 0;
            const u8* p_range = nullptr;
            if (ReadEncoded(p, p_end, enc, module.va_eh_frame + off_pc, va_begin) &&
                (p_range = p, ReadEncoded(p_range, p_end, enc & 0x0f, 0, range)) && range) {
                const u64 va_end = va_begin + range;
                if (va_begin >= module.va_code_begin && va_end <= module.va_code_end && va_end > va_begin) {
                    AnalyzeFunction(va_begin, va_end, p_patched, c_patched, is_dry_run, counts, p_diag);
                } else {
                    ++counts.fde_errors; // an FDE outside the module's code (AnyPS5 saw bad bounds)
                }
            } else {
                ++counts.fde_errors;
            }
        } else {
            ++counts.fde_errors; // an FDE pointing forward: malformed
        }
        off = off_next;
    }
}

void Report(const char* sz_name, const Counts& counts, bool is_dry_run) {
    std::printf("Red zone guard: %s: %llu functions, %llu red-zone leaves, %llu sites %s; not covered: "
                "%llu short, %llu branch-target inside, %llu rsp, %llu rip, %llu patched, %llu overlapping, "
                "%llu out of reach, %llu decode; %llu non-leaf red-zone uses, %llu FDE skips\n",
                sz_name, (unsigned long long)counts.functions, (unsigned long long)counts.red_zone_functions,
                (unsigned long long)counts.protected_sites, is_dry_run ? "would protect" : "protected",
                (unsigned long long)counts.skip_short, (unsigned long long)counts.skip_branch_inside,
                (unsigned long long)counts.skip_rsp, (unsigned long long)counts.skip_rip,
                (unsigned long long)counts.skip_patched, (unsigned long long)counts.skip_overlap,
                (unsigned long long)counts.skip_reach, (unsigned long long)counts.skip_decode,
                (unsigned long long)counts.non_leaf, (unsigned long long)counts.fde_errors);
}

void ReportDiag(const char* sz_name, const Counts& counts, const Diag& diag) {
    std::printf("Red zone guard: %s: decode diagnostics: %llu bytes covered by FDEs, %llu functions dropped "
                "(%llu bytes), %llu after a terminator; length 0 x%llu, crosses FDE end x%llu",
                sz_name, (unsigned long long)diag.cb_fde, (unsigned long long)counts.skip_decode,
                (unsigned long long)diag.cb_dropped, (unsigned long long)diag.c_after_terminator,
                (unsigned long long)diag.c_length_zero, (unsigned long long)diag.c_crosses_end);
    for (std::size_t i = 0; i < diag.c_bins; ++i) {
        std::printf("; status 0x%08x %s x%llu", diag.bins[i].status, DecodeStatusName(diag.bins[i].status),
                    (unsigned long long)diag.bins[i].c);
    }
    if (diag.c_status_other) {
        std::printf("; other statuses x%llu", (unsigned long long)diag.c_status_other);
    }
    std::printf("\n");
    for (std::size_t i = 0; i < diag.c_failures; ++i) {
        const Diag::Failure& f = diag.failures[i];
        char sz_bytes[Diag::FailureBytes * 3 + 1] = "";
        for (std::size_t b = 0; b < f.c_bytes; ++b) {
            std::snprintf(sz_bytes + b * 3, 4, "%02x ", f.bytes[b]);
        }
        std::printf("Red zone guard: %s: decode failure at 0x%llx (+%llu/%llu) status=0x%08x %s after %s (%s) "
                    "bytes %s\n",
                    sz_name, (unsigned long long)f.va, (unsigned long long)f.off, (unsigned long long)f.cb_function,
                    f.status, f.status ? DecodeStatusName(f.status) : "LENGTH_OR_FDE_END",
                    f.prev == ZYDIS_MNEMONIC_INVALID ? "function start" : ZydisMnemonicGetString(f.prev),
                    f.is_after_terminator ? "after terminator" : "mid-function", sz_bytes);
    }
    std::printf("Red zone guard: %s: non-leaf red-zone uses by form: %llu functions; rsp-based %llu, rbp-based "
                "%llu; before first call %llu, between calls %llu, after last call %llu; lea only %llu\n",
                sz_name, (unsigned long long)diag.non_leaf, (unsigned long long)diag.non_leaf_rsp,
                (unsigned long long)diag.non_leaf_rbp, (unsigned long long)diag.non_leaf_before_call,
                (unsigned long long)diag.non_leaf_between, (unsigned long long)diag.non_leaf_after_call,
                (unsigned long long)diag.non_leaf_lea_only);
}

void Summarize(char* sz_out, std::size_t cb_out) {
    std::snprintf(sz_out, cb_out,
                  "Red zone guard: summary: %llu sites protected in %llu red-zone leaves of %llu functions; "
                  "not covered: %llu short, %llu branch-target inside, %llu rsp, %llu rip, %llu patched, "
                  "%llu overlapping, %llu out of reach, %llu decode; %llu non-leaf, %llu FDE skips\n",
                  (unsigned long long)g_totals.protected_sites, (unsigned long long)g_totals.red_zone_functions,
                  (unsigned long long)g_totals.functions, (unsigned long long)g_totals.skip_short,
                  (unsigned long long)g_totals.skip_branch_inside, (unsigned long long)g_totals.skip_rsp,
                  (unsigned long long)g_totals.skip_rip, (unsigned long long)g_totals.skip_patched,
                  (unsigned long long)g_totals.skip_overlap, (unsigned long long)g_totals.skip_reach,
                  (unsigned long long)g_totals.skip_decode, (unsigned long long)g_totals.non_leaf,
                  (unsigned long long)g_totals.fde_errors);
}
} // namespace

extern "C" uint64_t psport_gpu_guard_red_zones(const PsRedZoneModule* p_modules, size_t c_modules,
                                               const PsRedZoneRange* p_patched, size_t c_patched,
                                               int is_clobbers) {
    bool is_on = is_clobbers != 0;
    bool is_dry_run = false;
    if (const char* sz_mode = runtime_env("PS_RED_ZONE_GUARD", nullptr)) {
        if (!std::strcmp(sz_mode, "0")) {
            is_on = false;
        } else if (!std::strcmp(sz_mode, "1")) {
            is_on = true;
        } else if (!std::strcmp(sz_mode, "report")) {
            is_on = true;
            is_dry_run = true;
        }
    }
    if (!is_on) {
        std::printf("Red zone guard: off (the host's fault frame keeps the red zone, or PS_RED_ZONE_GUARD=0)\n");
        return 0;
    }
    if (is_dry_run) {
        std::printf("Red zone guard: report mode (PS_RED_ZONE_GUARD=report): analysing without patching\n");
    }
    const char* sz_diag = runtime_env("PS_RED_ZONE_GUARD_DIAG", nullptr);
    const bool is_diag = is_dry_run || (sz_diag && !std::strcmp(sz_diag, "1"));
    if (!g_is_summary_registered) {
        runtime_calls_add_summary(&Summarize);
        g_is_summary_registered = true;
    }
    for (std::size_t i = 0; i < c_modules; ++i) {
        Counts counts;
        Diag diag;
        AnalyzeModule(p_modules[i], p_patched, c_patched, is_dry_run, counts, is_diag ? &diag : nullptr);
        const char* sz_name = p_modules[i].sz_name ? p_modules[i].sz_name : "?";
        Report(sz_name, counts, is_dry_run);
        if (is_diag) {
            ReportDiag(sz_name, counts, diag);
        }
        g_totals.Add(counts);
    }
    return g_totals.protected_sites;
}

extern "C" uint64_t psport_gpu_red_zone_site(uint64_t va_rip) { return PsGuestCode::SiteOf(va_rip); }
