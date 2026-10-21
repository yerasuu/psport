// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: button prompts for the device in use. The game draws its key guide ("Examine",
// item pick-ups, tutorials) with 32x32 icons of PlayStation buttons (KG_OK, KG_Cancel, KG_L1, ...
// in menu/common.tpf.dcx), and its HUD packs some of them into an atlas. Both are known by their
// bytes, hashed from the game's own file (so any release and any mod of it), and drawn by the port
// for the keyboard and mouse or another gamepad (games/common/glyphs.h). From bbport's
// bbport_button_prompts (72e5c8c).
#pragma once

#include "../common/glyphs.h"

namespace PsButtonPrompts {

extern const PsGlyphs::Table Glyphs;

} // namespace PsButtonPrompts
