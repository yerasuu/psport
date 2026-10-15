// SPDX-License-Identifier: GPL-2.0-or-later
// bbport PS_HEAP_SITES=1: live allocations of the game's heap per call site (diagnostics).
#pragma once

#include <cstddef>

namespace PsHeapSites {
/// Wraps the game's malloc replacement once libc.prx has set it up (PS_HEAP_SITES=1).
void Install();
/// Prints the call sites whose live bytes grew since the last report.
void Report();
/// The result of the game's release check (0xbf0790) seen at 0xbb5fb0.
void NoteReleaseCheck(unsigned result);
/// Once a frame while frames overlap (Game::Profile::frame_overlap_unsafe): with deep overlap the
/// game's heap leaked (~2 resource objects a frame, out of memory after minutes).
bool OverlapLeaks(char* reason, std::size_t size);
} // namespace PsHeapSites
