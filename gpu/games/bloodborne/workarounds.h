// SPDX-License-Identifier: GPL-2.0-or-later
// psport: Bloodborne's WORKAROUND quirks that change the game's code (docs/en/QUIRKS.md), each
// written only while its quirk is active and removed with it once the general fix of its task
// lands. Sites are checked byte for byte against the 1.09 executable (another build: refused and
// logged).
#pragma once

#include <cstdint>

namespace PsWorkarounds {
/// Before any of the game's code runs, while the image is still writable.
void PatchImage(unsigned char* p_image, std::uint64_t cb_image);
/// Prints the workarounds' counters that grew since the last call (PS_FRAME_STATS).
void Report();
} // namespace PsWorkarounds
