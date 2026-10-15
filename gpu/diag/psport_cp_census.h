// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_CP_CENSUS=1 — what a title's command streams use, counted over the whole run and
// printed once at exit after the call report ("CP census:" lines, `section: key=value ...`, read
// by tools/regress/report.py): packet mix per engine, compute queue submissions, constant engine
// dumps, nested indirect buffers, predication, WAIT_REG_MEM on memory the GPU or the CPU writes,
// MEM_SEMAPHORE, buffers per frame, and the Gnm driver's draw and dispatch helpers the game calls.
// Input for the native submission translator (decision #361); off, every hook is one load of a
// constant flag.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <span>

namespace PsCpCensus {

inline const bool g_is_enabled = [] {
    const char* sz_env = std::getenv("PS_CP_CENSUS");
    return sz_env && sz_env[0] == '1';
}();

/// GPU command thread: a graphics buffer about to be processed (any depth) and its constant
/// engine buffer.
void ScanGraphics(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb, int depth);
/// GPU command thread: a compute queue buffer about to be processed (`is_indirect`: an indirect
/// buffer of one).
void ScanCompute(std::span<const std::uint32_t> acb, bool is_indirect);
/// Guest threads: a Gnm driver helper was called. `sz_function` must outlive the process
/// (`__func__`): it is kept by address.
void NoteDriverCallSlow(const char* sz_function);
inline void NoteDriverCall(const char* sz_function) {
    if (g_is_enabled) {
        NoteDriverCallSlow(sz_function);
    }
}

} // namespace PsCpCensus
