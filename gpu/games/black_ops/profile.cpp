// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops and Black Ops II: what the translator knows about their code
// (Game::Profile; their titles are title.cpp): the mouse in their own mouse input.
#include "game_profile.h"
#include "mouse_camera.h"

namespace Game {

extern const Profile BlackOpsProfile = {
    .pfn_patch_image = PsBlackOpsMouse::PatchBlackOps,
    .mouse = {.pfn_camera_look = PsBlackOpsMouse::Turn},
};

extern const Profile BlackOps2Profile = {
    .pfn_patch_image = PsBlackOpsMouse::PatchBlackOps2,
    .mouse = {.pfn_camera_look = PsBlackOpsMouse::Turn},
};

} // namespace Game
