// SPDX-License-Identifier: GPL-2.0-or-later
// psport (diagnostics, #683): why the render job runner's step 0x1067f60 fails. Its two failure
// exits are probed for the result they leave with: the usage check 0x2165150 (8) and the scratch
// block request 0x2169c30 (9: no block free, the ring's blocks free only once the GPU has written
// their end-of-pipe label). Installed only with PS_JOB_STEP_PROBE=1.
#pragma once

#include <cstdint>

namespace PsJobStepProbe {
/// Before any of the game's code runs, while the image is still writable.
void PatchImage(unsigned char* p_image, std::uint64_t cb_image);
/// Prints the failures counted since the last call (PS_FRAME_STATS).
void Report();
} // namespace PsJobStepProbe
