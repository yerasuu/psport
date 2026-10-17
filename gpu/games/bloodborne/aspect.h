// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): any output aspect ratio, live, through hooks in its code (aspect.cpp): its
// camera shows more to the sides of a wider output (above and below a taller one), the HUD's
// markers stay on their characters, the HUD keeps to the screen's edges and its Scaleform movies
// draw their wider (taller) view into the 1920x1080 frame, which the output stretches back
// (UiComposition::Scale). The game keeps its 1920x1080 frame.
#pragma once

#include <cstdint>

namespace PsAspect {
/// Installs the hooks (the profile's patch_image, before the game runs).
void PatchImage(unsigned char* p_image, std::uint64_t cb_image);
/// Game::Profile::pfn_display_aspect: the output's aspect ratio for the camera's next update and
/// the HUD; false while the camera's hooks are not in.
bool SetDisplayAspect(float aspect);
} // namespace PsAspect
