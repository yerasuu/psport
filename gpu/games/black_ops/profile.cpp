// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops and Black Ops II: what the translator knows about their code
// (Game::Profile; their titles are title.cpp): the mouse in their own mouse input and the
// crosshair with keyboard and mouse.
#include "crosshair.h"
#include "game_profile.h"
#include "mouse_camera.h"

namespace Game {

namespace {
void PatchBlackOps(unsigned char* p_image, std::uint64_t cb_image) {
    PsBlackOpsMouse::PatchBlackOps(p_image, cb_image);
    PsBlackOpsCrosshair::PatchBlackOps(p_image, cb_image);
}

void PatchBlackOps2(unsigned char* p_image, std::uint64_t cb_image) {
    PsBlackOpsMouse::PatchBlackOps2(p_image, cb_image);
    PsBlackOpsCrosshair::PatchBlackOps2(p_image, cb_image);
}
} // namespace

extern const Profile BlackOpsProfile = {
    .pfn_patch_image = PatchBlackOps,
    .mouse = {.pfn_camera_look = PsBlackOpsMouse::Turn, .pfn_filter_pad = PsBlackOpsCrosshair::Sample},
};

extern const Profile BlackOps2Profile = {
    .pfn_patch_image = PatchBlackOps2,
    .mouse = {.pfn_camera_look = PsBlackOpsMouse::Turn, .pfn_filter_pad = PsBlackOpsCrosshair::Sample},
};

} // namespace Game
