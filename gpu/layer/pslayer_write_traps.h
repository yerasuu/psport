// SPDX-License-Identifier: GPL-2.0-or-later
// The runtime's write traps (src/kernel/runtime_memory.c, runtime_memory_trap): a guest page is
// read-only for the game while any reason holds it, and each owner clears only its own. The
// layer's VRAM copies and the texture cache trap the same pages; with one owner of the page
// protections, one's unprotect cannot drop the other's trap. Same on every GPU: the traps are the
// CPU's page protections, not a driver feature. Reasons from 16 up trap reads too (no access).
#pragma once
#include <cstdint>

extern "C" void runtime_memory_trap(std::uintptr_t va_range, std::uint64_t cb_range, unsigned reason,
                                    int is_on);
extern "C" unsigned runtime_memory_trap_reasons(std::uintptr_t va_guest);

namespace PsLayer::WriteTraps {

enum Reason : unsigned {
    Mirror = 1, ///< a VRAM copy of the page (the layer's memory module)
    Image = 2,  ///< an image made from the page (the texture cache)
    Buffer = 4, ///< a VRAM copy of the page in the buffer cache's arena
    /// A command buffer of a frame the GPU has not retired (PsSubmitGuard, PS_SUBMIT_AHEAD).
    FrameCommands = 8,
    /// Reads too: occlusion counters, until the game's first access after an event there
    /// (PS_OCCLUSION_READ_TRACE, diagnostics).
    QueryReads = 16,
    /// Reads too: the GPU wrote the page's newest data into a VRAM copy only; copied back into
    /// the game's memory on the first CPU access (PS_LAYER_READ_TRAPS).
    VramData = 32,
};
/// Reasons that make a page no access, not only read-only.
inline constexpr unsigned NoAccess = 0xf0;

/// [address, address + size) trapped for `reason` or no longer (page granular).
inline void Set(std::uintptr_t va_range, std::uint64_t cb_range, Reason reason, bool is_on) {
    runtime_memory_trap(va_range, cb_range, reason, is_on ? 1 : 0);
}

/// The reasons the page holding `va_guest` is trapped for.
inline unsigned Reasons(std::uintptr_t va_guest) {
    return runtime_memory_trap_reasons(va_guest);
}

} // namespace PsLayer::WriteTraps
