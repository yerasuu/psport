// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): the mouse as a pointer in the game's own menus (Scaleform lists: the title
// menu, the inventory, the shops, the options). The item under the pointer gets the game's cursor,
// a click presses the confirm button on it (or the slider's arrow it is on), the right button goes
// back, the wheel moves up and down. PS_MENU_POINTER=0 leaves the menus to the keys and the pad.
// menu_pointer.cpp has the reverse engineering.
#pragma once

#include <cstdint>
#include "../../../src/platform/ps_host_window.h"

namespace PsMenuPointer {
/// Hooks the game's list update (the profile's patch_image, before the game runs). Nothing happens
/// when the game's bytes differ (another build).
void PatchImage(unsigned char* p_image, std::uint64_t cb_image);

/// Game::MouseHooks: the window's events (window thread), whether a menu wants the pointer, and
/// the pointer's presses in the pad state the game reads (game thread).
bool WindowEvent(const PsHostWindowEvent* p_event, PsHostWindow* p_window);
bool WantsPointer();
void FilterPad(std::uint32_t* p_buttons, const std::uint8_t* p_sticks);

/// Whether the game holds its input back (either input block of menu_pointer.cpp's notes); false
/// until PatchImage has seen the 1.09 executable's code.
bool IsInputBlocked();
} // namespace PsMenuPointer
