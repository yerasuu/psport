// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): native mouse look. The mouse turns the game's follow camera itself, as far as
// it moved, with none of the stick's dead zone, response curve, acceleration or top speed
// (PS_NATIVE_MOUSE=0: the mouse drives the right stick instead, src/input/runtime_pad.c).
#pragma once

#include <cstdint>

namespace PsMouseLook {
/// Installs the camera hook (the profile's patch_image, before the game runs).
void PatchImage(unsigned char* p_image, std::uint64_t cb_image);
/// Game::MouseHooks pfn_camera_look: a turn (radians) for the camera's next update; false while
/// the hook is not in or the camera does not take turns (lock-on, menus, cutscenes).
bool Turn(float pitch, float yaw);
} // namespace PsMouseLook
