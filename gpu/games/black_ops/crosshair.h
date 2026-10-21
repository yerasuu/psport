// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops (CUSA57547) and Black Ops II (CUSA57548): the crosshair with keyboard
// and mouse. Both games draw it by default (dvar cg_drawCrosshair), with no gamepad or platform
// gate; level scripts and the game's state hide it. While the keyboard and mouse are in use and the
// game shows its HUD, psport keeps cg_drawCrosshair on; with a pad the game's own value is back
// (PS_FORCE_CROSSHAIR=0: never forced). The dvars that hide it are logged as the game changes them.
#pragma once

#include <cstdint>

namespace PsBlackOpsCrosshair {
/// Checks the executable is the build whose dvars the crosshair knows (the profile's patch_image,
/// before the game runs) and turns it on when it is.
void PatchBlackOps(unsigned char* p_image, std::uint64_t cb_image);
void PatchBlackOps2(unsigned char* p_image, std::uint64_t cb_image);
/// Game::MouseHooks pfn_filter_pad: each pad sample, on the game's thread; the pad state is left
/// as it is.
void Sample(std::uint32_t* p_buttons, const std::uint8_t* p_sticks);
} // namespace PsBlackOpsCrosshair
