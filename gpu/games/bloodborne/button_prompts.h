// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: button prompts for the device in use. The game draws its key guide ("Examine",
// item pick-ups, tutorials) with 32x32 icons of PlayStation buttons (KG_OK, KG_Cancel, KG_L1, ...
// in menu/common.tpf.dcx), and its HUD packs some of them into an atlas. Both are known by their
// bytes, hashed from the game's own file (so any release and any mod of it), and while the
// keyboard and mouse or another gamepad was used last the core samples icons drawn by the port
// instead (Game::TextureSubstitution): the bound key, mouse button or that pad's button.
// PS_BUTTON_PROMPTS=0: the game's icons always. From bbport's bbport_button_prompts (72e5c8c).
#pragma once

#include "game_profile.h"

namespace PsButtonPrompts {

/// Game::TextureSubstitution pfn_start: reads the icons' hashes from the game's file on a thread
/// of its own (Identify waits for it once).
void Start();
/// pfn_identify: one of the game's icons (32x32 BC7, one level) or its HUD atlas, by its bytes.
const Game::TextureSubstitute* Identify(const void* p_texels, std::size_t cb_texels, std::uint32_t width,
                                        std::uint32_t height);
/// pfn_is_rescanned: an icon's shape, which the game may have the GPU copy in.
bool IsRescanned(std::size_t cb_texels, std::uint32_t width, std::uint32_t height);
/// pfn_generation: runtime_pad_prompt_generation (the device in use, its type and the bindings).
std::uint32_t Generation();
/// pfn_draw: the icons for the device in use.
bool Draw(Game::SubstitutePictures& out);

} // namespace PsButtonPrompts
