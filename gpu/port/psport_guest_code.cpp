// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// psport: changes to a game's own code (psport_guest_code.h). InstallHook's trampoline follows
// shadPS4's InstallGuestCodeHook (core/cpu_patches.cpp of the shadp2p and BBSC forks) as bbport
// carries it; the memory in reach, the checks and the writes are the port's.
#include "psport_guest_code.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include <xbyak/xbyak.h>
#include <Zydis/Zydis.h>

#include "common/decoder.h"

namespace PsGuestCode {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;
using namespace Xbyak::util;

constexpr std::size_t NearJumpBytes = 5;
constexpr std::size_t GuestRedZoneBytes = 128;
/// Trampolines are ~400 bytes each.
constexpr std::size_t CbArea = 4u << 20;
/// Inside a rel32 jump's 2 GiB, with room for the code's own extent.
constexpr std::int64_t JumpReach = 0x7000'0000;
constexpr std::int64_t SearchStep = 64ll << 20;

/// Installs come from the loader's thread (patch_image) and the GPU side's start (install_hooks);
/// both are rare, so one lock covers the sites and the areas.
std::mutex s_mutex;
/// [begin, end) of every site changed, so that two never overlap.
std::map<u64, u64> s_sites;
struct Area {
    u64 va_start = 0;
    std::unique_ptr<Xbyak::CodeGenerator> p_generator;
};
std::vector<Area> s_areas;
/// KeepWritable was called: code written under a short write permission goes back to read, write
/// and execute instead of read and execute.
std::atomic<bool> s_is_code_writable{false};

bool IsInReach(u64 va_a, u64 va_b) {
    const std::int64_t distance = std::int64_t(va_a) - std::int64_t(va_b);
    return distance > -JumpReach && distance < JumpReach;
}

/// cb_range bytes mapped with prot at a page within reach of va_near: below it first (a game's
/// image has room under it), then above, in SearchStep steps.
void* MapInReach(u64 va_near, std::size_t cb_range, int prot) {
    const u64 cb_page = u64(sysconf(_SC_PAGESIZE));
    for (std::int64_t step = 1; step * SearchStep < JumpReach; ++step) {
        for (const std::int64_t sign : {-1ll, 1ll}) {
            const std::int64_t va_try = std::int64_t(va_near) + sign * step * SearchStep;
            if (va_try <= 0) {
                continue;
            }
            void* p_hint = reinterpret_cast<void*>(u64(va_try) & ~(cb_page - 1));
            void* p_mapped = mmap(p_hint, cb_range, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p_mapped == MAP_FAILED) {
                continue;
            }
            // A kernel older than MAP_FIXED_NOREPLACE takes the address as a hint only.
            if (p_mapped != p_hint) {
                munmap(p_mapped, cb_range);
                continue;
            }
            return p_mapped;
        }
    }
    return nullptr;
}

Xbyak::CodeGenerator* AreaFor(u64 va_code) {
    for (Area& area : s_areas) {
        if (IsInReach(area.va_start, va_code) && area.p_generator->getSize() + 1024 < CbArea) {
            return area.p_generator.get();
        }
    }
    void* p_area = MapInReach(va_code, CbArea, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!p_area) {
        return nullptr;
    }
    s_areas.push_back({reinterpret_cast<u64>(p_area), std::make_unique<Xbyak::CodeGenerator>(CbArea, p_area)});
    return s_areas.back().p_generator.get();
}

bool Overlaps(u64 va_begin, u64 va_end) {
    auto it = s_sites.lower_bound(va_end);
    return it != s_sites.begin() && (--it)->second > va_begin;
}

/// The code at p_code is whole instructions for cb_code bytes; with is_absolute_only, none of
/// them addresses relative to the instruction pointer.
Error CheckInstructions(const u8* p_code, std::size_t cb_code, bool is_absolute_only) {
    for (std::size_t off_code = 0; off_code < cb_code;) {
        ZydisDecodedInstruction instruction{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        const ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(
            instruction, operands.data(), const_cast<u8*>(p_code) + off_code, cb_code - off_code);
        if (!ZYAN_SUCCESS(status) || instruction.length == 0 || instruction.length > cb_code - off_code) {
            return Error::NotWholeInstructions;
        }
        for (u8 i = 0; is_absolute_only && i < instruction.operand_count; ++i) {
            const ZydisDecodedOperand& operand = operands[i];
            if ((operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) ||
                (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP))) {
                return Error::RelativeAddressing;
            }
        }
        off_code += instruction.length;
    }
    return Error::None;
}

/// bytes over the game's code. Its pages are still writable while the loader patches the image
/// (written as they are, their protection left to the loader); once mapped read and execute they
/// are written under a short write permission. Bytes 1.. go first and byte 0 last: a thread
/// running there meanwhile meets either the old first instruction or the whole jump.
bool WriteCode(u64 va_code, std::span<const u8> bytes) {
    const auto write_direct = [](u64 va_to, const u8* p_from, std::size_t cb_copy) {
        iovec local{const_cast<u8*>(p_from), cb_copy}, remote{reinterpret_cast<void*>(va_to), cb_copy};
        return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == ssize_t(cb_copy);
    };
    if (bytes.size() > 1 ? write_direct(va_code + 1, bytes.data() + 1, bytes.size() - 1) : true) {
        return write_direct(va_code, bytes.data(), 1);
    }
    const u64 cb_page = u64(sysconf(_SC_PAGESIZE));
    void* p_first = reinterpret_cast<void*>(va_code & ~(cb_page - 1));
    const std::size_t cb_span = va_code + bytes.size() - reinterpret_cast<u64>(p_first);
    if (mprotect(p_first, cb_span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    auto* p_code = reinterpret_cast<u8*>(va_code);
    std::memcpy(p_code + 1, bytes.data() + 1, bytes.size() - 1);
    __atomic_store_n(p_code, bytes[0], __ATOMIC_SEQ_CST);
    const int prot_after = s_is_code_writable.load(std::memory_order_acquire) ? PROT_READ | PROT_WRITE | PROT_EXEC
                                                                               : PROT_READ | PROT_EXEC;
    return mprotect(p_first, cb_span, prot_after) == 0;
}

/// A jump to va_target at va_site, the rest of cb_site int3.
bool WriteJump(u64 va_site, std::size_t cb_site, u64 va_target) {
    std::array<u8, 64> patch{};
    if (cb_site > patch.size()) {
        return false;
    }
    patch.fill(0xcc);
    const std::int32_t rel = std::int32_t(std::int64_t(va_target) - std::int64_t(va_site + NearJumpBytes));
    patch[0] = 0xe9;
    std::memcpy(&patch[1], &rel, 4);
    return WriteCode(va_site, std::span(patch.data(), cb_site));
}

/// The checks common to a hook and a detour, under s_mutex.
Error CheckSite(u64 va_site, std::span<const u8> expected, bool is_absolute_only) {
    if (expected.size() < NearJumpBytes) {
        return Error::TooShort;
    }
    if (std::memcmp(reinterpret_cast<const void*>(va_site), expected.data(), expected.size()) != 0) {
        return Error::CodeDiffers;
    }
    if (const Error error = CheckInstructions(expected.data(), expected.size(), is_absolute_only); error != Error::None) {
        return error;
    }
    return Overlaps(va_site, va_site + expected.size()) ? Error::Overlaps : Error::None;
}

Error Logged(const char* sz_name, u64 va_site, std::size_t cb_site, const char* sz_kind, Error error) {
    if (error == Error::None) {
        std::printf("Guest code: %s %s at %#llx (%zu bytes)\n", sz_name, sz_kind, static_cast<unsigned long long>(va_site),
                    cb_site);
    } else {
        std::printf("Guest code: %s at %#llx not %s: %s\n", sz_name, static_cast<unsigned long long>(va_site), sz_kind,
                    Describe(error));
    }
    return error;
}

Error AssembleAndInstallHook(u64 va_site, std::span<const u8> expected, u64 tag, PfnHook pfn_hook,
                             std::uint8_t* p_skip_flag) {
    constexpr std::size_t OffSnapshot = 0;
    constexpr std::size_t OffScratchR11 = sizeof(Registers);
    constexpr std::size_t OffScratchRflags = OffScratchR11 + sizeof(u64);
    constexpr std::size_t CbFrame = (OffScratchRflags + sizeof(u64) + GuestRedZoneBytes + 15) & ~std::size_t(15);

    Xbyak::CodeGenerator* p_gen = AreaFor(va_site);
    if (!p_gen) {
        return Error::OutOfReach;
    }
    const std::size_t off_start = p_gen->getSize();
    const u64 va_trampoline = reinterpret_cast<u64>(p_gen->getCurr());
    const auto snapshot = [](std::size_t off_field) { return qword[rsp + OffSnapshot + off_field]; };
    try {
        p_gen->lea(rsp, ptr[rsp - CbFrame]); // past the game's red zone
        p_gen->mov(qword[rsp + OffScratchR11], r11);
        p_gen->pushfq();
        p_gen->pop(r11);
        p_gen->mov(qword[rsp + OffScratchRflags], r11);
        p_gen->mov(r11, rsp);
        p_gen->and_(rsp, -16);
        p_gen->mov(snapshot(offsetof(Registers, rax)), rax);
        p_gen->mov(snapshot(offsetof(Registers, rbx)), rbx);
        p_gen->mov(snapshot(offsetof(Registers, rcx)), rcx);
        p_gen->mov(snapshot(offsetof(Registers, rdx)), rdx);
        p_gen->mov(snapshot(offsetof(Registers, rsi)), rsi);
        p_gen->mov(snapshot(offsetof(Registers, rdi)), rdi);
        p_gen->mov(snapshot(offsetof(Registers, rbp)), rbp);
        p_gen->mov(snapshot(offsetof(Registers, r8)), r8);
        p_gen->mov(snapshot(offsetof(Registers, r9)), r9);
        p_gen->mov(snapshot(offsetof(Registers, r10)), r10);
        p_gen->mov(snapshot(offsetof(Registers, r12)), r12);
        p_gen->mov(snapshot(offsetof(Registers, r13)), r13);
        p_gen->mov(snapshot(offsetof(Registers, r14)), r14);
        p_gen->mov(snapshot(offsetof(Registers, r15)), r15);
        p_gen->mov(rax, qword[r11 + OffScratchR11]);
        p_gen->mov(snapshot(offsetof(Registers, r11)), rax);
        p_gen->mov(rax, qword[r11 + OffScratchRflags]);
        p_gen->mov(snapshot(offsetof(Registers, rflags)), rax);
        p_gen->lea(rax, ptr[r11 + CbFrame]);
        p_gen->mov(snapshot(offsetof(Registers, rsp)), rax);
        p_gen->stmxcsr(dword[rsp + OffSnapshot + offsetof(Registers, mxcsr)]);
        for (int i = 0; i < 16; ++i) {
            p_gen->vmovdqu(yword[rsp + OffSnapshot + offsetof(Registers, ymm) + i * 32], Xbyak::Ymm(i));
        }
        // The aligned snapshot in a callee-saved register across the call.
        p_gen->mov(r12, rsp);
        p_gen->mov(rdi, tag);
        p_gen->lea(rsi, ptr[r12 + OffSnapshot]);
        p_gen->mov(rax, reinterpret_cast<u64>(pfn_hook));
        p_gen->call(rax);
        p_gen->mov(rsp, r12);
        for (int i = 0; i < 16; ++i) {
            p_gen->vmovdqu(Xbyak::Ymm(i), yword[rsp + OffSnapshot + offsetof(Registers, ymm) + i * 32]);
        }
        p_gen->ldmxcsr(dword[rsp + OffSnapshot + offsetof(Registers, mxcsr)]);
        p_gen->mov(rbx, snapshot(offsetof(Registers, rbx)));
        p_gen->mov(rcx, snapshot(offsetof(Registers, rcx)));
        p_gen->mov(rdx, snapshot(offsetof(Registers, rdx)));
        p_gen->mov(rsi, snapshot(offsetof(Registers, rsi)));
        p_gen->mov(rdi, snapshot(offsetof(Registers, rdi)));
        p_gen->mov(rbp, snapshot(offsetof(Registers, rbp)));
        p_gen->mov(r8, snapshot(offsetof(Registers, r8)));
        p_gen->mov(r9, snapshot(offsetof(Registers, r9)));
        p_gen->mov(r10, snapshot(offsetof(Registers, r10)));
        p_gen->mov(r11, snapshot(offsetof(Registers, r11)));
        p_gen->mov(r12, snapshot(offsetof(Registers, r12)));
        p_gen->mov(r13, snapshot(offsetof(Registers, r13)));
        p_gen->mov(r14, snapshot(offsetof(Registers, r14)));
        p_gen->mov(r15, snapshot(offsetof(Registers, r15)));
        p_gen->mov(rax, snapshot(offsetof(Registers, rflags)));
        p_gen->push(rax);
        p_gen->popfq();
        p_gen->mov(rax, snapshot(offsetof(Registers, rax)));
        p_gen->mov(rsp, snapshot(offsetof(Registers, rsp)));
        Xbyak::Label run_original;
        if (p_skip_flag) {
            // Function entries only: [rsp] is the return address and the flags are not live across
            // a call; a set flag (cleared here) returns from the game's function at once.
            p_gen->push(rax);
            p_gen->mov(rax, reinterpret_cast<u64>(p_skip_flag));
            p_gen->cmp(byte[rax], 0);
            p_gen->mov(byte[rax], 0);
            p_gen->pop(rax);
            p_gen->je(run_original, Xbyak::CodeGenerator::LabelType::T_NEAR);
            p_gen->ret();
        }
        p_gen->L(run_original);
        p_gen->db(expected.data(), expected.size());
        p_gen->jmp(reinterpret_cast<const void*>(va_site + expected.size()), Xbyak::CodeGenerator::LabelType::T_NEAR);
        p_gen->align(16);
    } catch (const Xbyak::Error&) {
        // Xbyak reports by exception only; nothing of the trampoline is kept.
        p_gen->setSize(off_start);
        return Error::TrampolineFailed;
    }
    if (!WriteJump(va_site, expected.size(), va_trampoline)) {
        p_gen->setSize(off_start);
        return Error::ProtectFailed;
    }
    return Error::None;
}
} // namespace

const char* Describe(Error error) {
    switch (error) {
    case Error::None:
        return "done";
    case Error::CodeDiffers:
        return "the code differs (another build of the game, or a patch there)";
    case Error::TooShort:
        return "shorter than a jump";
    case Error::NotWholeInstructions:
        return "not whole instructions";
    case Error::RelativeAddressing:
        return "an instruction addresses relative to the instruction pointer";
    case Error::OperandDiffers:
        return "no RIP-relative operand reading the expected address";
    case Error::Overlaps:
        return "overlaps a site changed before";
    case Error::OutOfReach:
        return "no memory within a jump's reach";
    case Error::StubTooLarge:
        return "the stub is too large";
    case Error::TrampolineFailed:
        return "the trampoline could not be assembled";
    case Error::ProtectFailed:
        return "the code could not be made writable";
    }
    return "unknown error";
}

Error InstallHook(const char* sz_name, std::uint64_t va_site, std::span<const std::uint8_t> expected,
                  std::uint64_t tag, PfnHook pfn_hook, std::uint8_t* p_skip_flag) {
    std::scoped_lock lock{s_mutex};
    Error error = CheckSite(va_site, expected, true);
    if (error == Error::None) {
        error = AssembleAndInstallHook(va_site, expected, tag, pfn_hook, p_skip_flag);
    }
    if (error == Error::None) {
        s_sites.emplace(va_site, va_site + expected.size());
    }
    return Logged(sz_name, va_site, expected.size(), "hooked", error);
}

Error InstallDetour(const char* sz_name, std::uint64_t va_site, std::span<const std::uint8_t> expected,
                    const Emitter& stub) {
    std::scoped_lock lock{s_mutex};
    Error error = stub.m_is_overflow ? Error::StubTooLarge : CheckSite(va_site, expected, false);
    Xbyak::CodeGenerator* p_gen = error == Error::None ? AreaFor(va_site) : nullptr;
    if (error == Error::None && !p_gen) {
        error = Error::OutOfReach;
    }
    if (error == Error::None) {
        const std::size_t off_start = p_gen->getSize();
        const u64 va_stub = reinterpret_cast<u64>(p_gen->getCurr());
        // Within the area's free room (AreaFor keeps 1 KiB spare, a stub is at most 512 bytes).
        p_gen->db(stub.m_code.data(), stub.m_cb_code);
        p_gen->align(16);
        if (!WriteJump(va_site, expected.size(), va_stub)) {
            p_gen->setSize(off_start);
            error = Error::ProtectFailed;
        }
    }
    if (error == Error::None) {
        s_sites.emplace(va_site, va_site + expected.size());
    }
    return Logged(sz_name, va_site, expected.size(), "detoured", error);
}

Error RedirectOperand(const char* sz_name, std::uint64_t va_instruction, std::uint64_t va_expected,
                      const void* p_target) {
    std::scoped_lock lock{s_mutex};
    ZydisDecodedInstruction instruction{};
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
    const auto* p_code = reinterpret_cast<const u8*>(va_instruction);
    Error error = Error::None;
    if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(instruction, operands.data(),
                                                                      const_cast<u8*>(p_code), 15))) {
        error = Error::NotWholeInstructions;
    }
    bool has_operand = false;
    for (u8 i = 0; error == Error::None && i < instruction.operand_count; ++i) {
        const ZydisDecodedOperand& operand = operands[i];
        has_operand = has_operand ||
                      (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP &&
                       va_instruction + instruction.length + u64(operand.mem.disp.value) == va_expected);
    }
    if (error == Error::None && (!has_operand || instruction.raw.disp.size != 32)) {
        error = Error::OperandDiffers;
    }
    const std::int64_t disp =
        std::int64_t(reinterpret_cast<u64>(p_target)) - std::int64_t(va_instruction + instruction.length);
    if (error == Error::None && disp != std::int32_t(disp)) {
        error = Error::OutOfReach;
    }
    if (error == Error::None && Overlaps(va_instruction, va_instruction + instruction.length)) {
        error = Error::Overlaps;
    }
    if (error == Error::None) {
        const std::int32_t disp32 = std::int32_t(disp);
        u8 bytes[4];
        std::memcpy(bytes, &disp32, 4);
        if (WriteCode(va_instruction + instruction.raw.disp.offset, bytes)) {
            s_sites.emplace(va_instruction, va_instruction + instruction.length);
        } else {
            error = Error::ProtectFailed;
        }
    }
    if (error != Error::None) {
        Logged(sz_name, va_instruction, instruction.length, "redirected", error);
    }
    return error;
}

std::uint32_t KeepWritable(std::uint64_t va_image, std::uint64_t cb_image) {
    s_is_code_writable.store(true, std::memory_order_release);
    std::FILE* p_maps = std::fopen("/proc/self/maps", "r");
    if (!p_maps) {
        std::printf("Guest code: /proc/self/maps unreadable: the game's code keeps its protection\n");
        return 0;
    }
    std::uint32_t c_writable = 0, c_refused = 0;
    char sz_line[512];
    while (std::fgets(sz_line, sizeof(sz_line), p_maps)) {
        unsigned long long va_begin = 0, va_end = 0;
        char sz_perms[5] = {};
        if (std::sscanf(sz_line, "%llx-%llx %4s", &va_begin, &va_end, sz_perms) != 3 || sz_perms[2] != 'x') {
            continue;
        }
        va_begin = std::max<unsigned long long>(va_begin, va_image);
        va_end = std::min<unsigned long long>(va_end, va_image + cb_image);
        if (va_begin >= va_end) {
            continue;
        }
        if (mprotect(reinterpret_cast<void*>(va_begin), va_end - va_begin, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            ++c_writable;
        } else {
            ++c_refused;
            std::printf("Guest code: %#llx-%#llx could not be made writable\n", va_begin, va_end);
        }
    }
    std::fclose(p_maps);
    std::printf("Guest code: the game's code at %#llx stays writable (%u mappings, %u refused)\n",
                static_cast<unsigned long long>(va_image), c_writable, c_refused);
    return c_writable;
}

void* MapNear(std::uint64_t va_near, std::size_t cb_range) {
    void* p_mapped = MapInReach(va_near, cb_range, PROT_READ | PROT_WRITE);
    if (!p_mapped) {
        std::printf("Guest code: no free memory within a jump's reach of %#llx (%zu bytes)\n",
                    static_cast<unsigned long long>(va_near), cb_range);
    }
    return p_mapped;
}

} // namespace PsGuestCode
