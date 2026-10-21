// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops (CUSA57547) and Black Ops II (CUSA57548): native mouse camera. The
// mouse's counts go into the PC games' own mouse input, which both still carry: the game turns the
// view with its own sensitivity, before its aim assist, with none of the stick's dead zone, curve
// or slowdown (PS_NATIVE_MOUSE=0: the mouse drives the right stick instead, src/input/runtime_pad.c).
#pragma once

#include <cstdint>

namespace PsBlackOpsMouse {
/// Checks the executable is the build the camera knows (the profile's patch_image, before the game
/// runs) and turns the camera on when it is.
void PatchBlackOps(unsigned char* p_image, std::uint64_t cb_image);
void PatchBlackOps2(unsigned char* p_image, std::uint64_t cb_image);
/// Game::MouseHooks pfn_camera_look: the turn (radians) as mouse counts for the game's next
/// frame; false while the camera is off, the game does not read them (menus, loading) or reads
/// them without turning (a vehicle, a killcam, the top-down camera): the mouse drives the right
/// stick then.
bool Turn(float pitch, float yaw);
} // namespace PsBlackOpsMouse
