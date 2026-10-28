// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// psport: changes to a game's own code for its profile (games/<game>/), the same for any game.
// Every site is compared byte for byte with the code the caller expects before anything is
// written: another build of the game, or a patch already there, is left as it is and the call
// returns why. Each hook and detour is logged when it goes in or is refused ("Guest code: ...").
//
// - InstallHook: an observer. A run of whole, position-independent instructions (5 bytes or more)
//   becomes a jump to a trampoline that saves every general register, the flags, MXCSR and the
//   YMM registers into a Registers snapshot, calls the handler with it, puts the (possibly
//   changed) registers back, runs the replaced instructions and jumps back. With a skip flag
//   (function entries only) the handler may return from the game's function at once. The
//   trampoline follows shadPS4's InstallGuestCodeHook (core/cpu_patches.cpp of the shadp2p and
//   BBSC forks), by way of bbport.
// - InstallDetour: the site becomes a jump to a stub the caller writes with Emitter, for sites
//   where a snapshot would cost too much or the stub changes what the replaced code does.
// - RedirectOperand: an instruction that reads a constant RIP-relative reads another address.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include "common/types.h"

namespace PsGuestCode {

enum class Error {
    None,
    /// The site does not hold the expected bytes (another build of the game, or a patch there).
    CodeDiffers,
    /// Fewer bytes than a near jump takes.
    TooShort,
    /// The expected bytes do not end on an instruction boundary.
    NotWholeInstructions,
    /// InstallHook: an instruction addresses relative to the instruction pointer.
    RelativeAddressing,
    /// RedirectOperand: no RIP-relative operand reading the expected address.
    OperandDiffers,
    /// Overlaps a site changed before.
    Overlaps,
    /// No memory within a rel32 reach of the site for the stub or the new operand.
    OutOfReach,
    /// The stub is larger than an Emitter holds.
    StubTooLarge,
    /// The trampoline could not be assembled.
    TrampolineFailed,
    /// The code's pages could not be made writable.
    ProtectFailed,
    /// RelocateBelowRedZone: the span reads or writes rsp, so shifting rsp around it would move it.
    UsesStackPointer,
};
const char* Describe(Error error);

struct alignas(16) Registers {
    std::uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    std::uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    std::uint64_t rflags;
    std::uint32_t mxcsr;
    alignas(16) std::array<std::array<std::uint8_t, 32>, 16> ymm;
};

/// Called on the game's thread and stack (keep it small); it may change the registers. SysV, since
/// the trampoline passes tag and p_registers in rdi and rsi on every host.
using PfnHook = void(PS4_SYSV_ABI*)(std::uint64_t tag, Registers* p_registers);

/// An observer at va_site over expected (whole instructions, none RIP-relative). p_skip_flag, for
/// a function's entry only: when the handler leaves it non-zero the trampoline clears it and
/// returns from the game's function without running it.
Error InstallHook(const char* sz_name, std::uint64_t va_site, std::span<const std::uint8_t> expected,
                  std::uint64_t tag, PfnHook pfn_hook, std::uint8_t* p_skip_flag = nullptr);

/// A stub's machine code, built in place. A stub runs at an address chosen at install time: only
/// absolute addresses in it (CallAbs, JmpAbs, movabs).
struct Emitter {
    std::array<std::uint8_t, 512> m_code{};
    std::size_t m_cb_code = 0;
    /// More than m_code holds: InstallDetour refuses it.
    bool m_is_overflow = false;

    void Bytes(std::initializer_list<std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes) {
            if (m_cb_code < m_code.size()) {
                m_code[m_cb_code] = b;
            }
            ++m_cb_code;
        }
        m_is_overflow = m_is_overflow || m_cb_code > m_code.size();
    }
    void Copy(std::span<const std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes) {
            Bytes({b});
        }
    }
    void U32(std::uint32_t value) {
        std::uint8_t b[4];
        std::memcpy(b, &value, 4);
        Bytes({b[0], b[1], b[2], b[3]});
    }
    void U64(std::uint64_t value) {
        U32(std::uint32_t(value));
        U32(std::uint32_t(value >> 32));
    }
    /// Saves the caller-saved general registers (a host function may change them) and aligns the
    /// stack; r12 keeps the unaligned stack pointer.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
    /// xmm0..15 below the aligned stack (after SaveVolatile): a function's float arguments.
    void SaveXmm() {
        Bytes({0x48, 0x81, 0xec}); // sub rsp, 256
        U32(256);
        for (std::uint8_t i = 0; i < 16; ++i) { // movdqu [rsp + 16 i], xmm_i
            Bytes({0xf3});
            if (i >= 8) {
                Bytes({0x44});
            }
            Bytes({0x0f, 0x7f, std::uint8_t(0x84 | (i & 7) << 3), 0x24});
            U32(16u * i);
        }
    }
    void RestoreXmm() {
        for (std::uint8_t i = 0; i < 16; ++i) { // movdqu xmm_i, [rsp + 16 i]
            Bytes({0xf3});
            if (i >= 8) {
                Bytes({0x44});
            }
            Bytes({0x0f, 0x6f, std::uint8_t(0x84 | (i & 7) << 3), 0x24});
            U32(16u * i);
        }
        Bytes({0x48, 0x81, 0xc4}); // add rsp, 256
        U32(256);
    }
    /// p_function must be PS4_SYSV_ABI: the stubs pass arguments in rdi, rsi, rdx, rcx and leave no
    /// shadow space, so a Win64 function would read its arguments from the wrong registers.
    void CallAbs(const void* p_function) {
        Bytes({0x48, 0xb8}); // movabs rax, p_function
        U64(reinterpret_cast<std::uint64_t>(p_function));
        Bytes({0xff, 0xd0}); // call rax
    }
    /// jmp [rip]: to va_target anywhere.
    void JmpAbs(std::uint64_t va_target) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(va_target);
    }
    /// The 32-bit value at p_value into the stack frame's [rbp + off_frame] (rax kept).
    void StoreToFrame(const void* p_value, std::int8_t off_frame) {
        Bytes({0x50, 0x48, 0xb8}); // push rax; movabs rax, p_value
        U64(reinterpret_cast<std::uint64_t>(p_value));
        Bytes({0x8b, 0x00});                                // mov eax, [rax]
        Bytes({0x89, 0x45, std::uint8_t(off_frame), 0x58}); // mov [rbp + off_frame], eax; pop rax
    }
};

/// [va_site, va_site + expected.size()) becomes a jump to a copy of stub (the rest int3). The stub
/// runs instead of the replaced instructions and must jump back itself.
Error InstallDetour(const char* sz_name, std::uint64_t va_site, std::span<const std::uint8_t> expected,
                    const Emitter& stub);

/// The instruction at va_instruction reads va_expected RIP-relative: it reads p_target instead,
/// which must lie within a rel32 reach (MapNear). Failures are logged; successes are the
/// caller's to count.
Error RedirectOperand(const char* sz_name, std::uint64_t va_instruction, std::uint64_t va_expected,
                      const void* p_target);

/// For a game module that writes the game's code itself and switches some of it while the game
/// runs (a title's online extension: BBSC's byte patches): the executable mappings within
/// [va_image, va_image + cb_image) become read, write and execute, and every later write of this
/// layer leaves its pages so. Once called it holds for the process. Returns the mappings made
/// writable; each refusal is logged.
std::uint32_t KeepWritable(std::uint64_t va_image, std::uint64_t cb_image);

/// cb_range bytes of zeroed, read-write memory within a rel32 reach of va_near, for values the
/// game's code reads RIP-relative (RedirectOperand); nullptr (logged) when none is free. Never
/// unmapped.
void* MapNear(std::uint64_t va_near, std::size_t cb_range);

/// [va_span, va_span + cb_span) (whole, position-independent instructions, none RIP- or
/// rsp-relative, cb_span >= a near jump) becomes a jump to a trampoline that lowers rsp by 128,
/// runs the span and raises rsp back before jumping on. An exception the span takes then writes its
/// frame below the guest's red zone instead of over the leaf's live locals there (#803). With
/// is_dry_run the checks run and the error is returned without changing any code (report mode).
/// SiteOf maps a fault inside the trampoline back to va_span.
Error RelocateBelowRedZone(const char* sz_name, std::uint64_t va_span, std::size_t cb_span, bool is_dry_run);

/// va_span of the trampoline va_rip lies in (a fault inside a RelocateBelowRedZone trampoline), or
/// va_rip unchanged. For fault reports, so the original guest instruction is named, not the copy.
std::uint64_t SiteOf(std::uint64_t va_rip);

} // namespace PsGuestCode
