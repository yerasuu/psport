// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: the port's key guide icons (button_prompts.h), drawn for the device in use: a key cap
// with the bound key's name, a mouse with the bound button lit, or another gamepad's button. From
// bbport's bbport_button_prompts.cpp (72e5c8c).
#pragma once

#include <array>
#include <cstdint>

namespace PsButtonPromptArt {

/// The icons are square, this many texels a side.
constexpr std::uint32_t Side = 128;

/// What an icon of the game shows.
enum class Layout {
    Single, ///< one input
    Column, ///< two inputs, one over the other (the d-pad's up and down)
    Row,    ///< two inputs side by side (left and right)
    Cross,  ///< up, left, down, right: a key cluster (W over A S D)
    Stick,  ///< a stick: its four keys, the mouse (look) or the pad's stick
};

/// Draws the icon for `inputs` (runtime_pad.c input names, as many as `layout` takes) on the device
/// in use into p_out, Side x Side straight-alpha RGBA8; false when the game's own icon stays (a
/// PlayStation pad, the d-pad on any pad, an unbound input).
bool Draw(Layout layout, const std::array<const char*, 4>& inputs, std::uint32_t* p_out);

} // namespace PsButtonPromptArt
