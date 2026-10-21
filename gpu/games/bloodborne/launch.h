// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne's part of the launch (Game::Title): its frame rate presets and the community
// patches the player's settings choose (patches/Bloodborne.xml, app version 01.09), and the check
// that the game folder holds the one executable they and the profile's hooks are written for.
#pragma once

#include <string>
#include "../../psport_gpu.h"

namespace PsLaunch {
/// The XML the presets come from, relative to psport's folder.
inline constexpr const char* PatchFile = "patches/Bloodborne.xml";
/// PS_VBLANK_HZ for the PS_FPS preset, unless it is set.
void PrepareEnvironment();
/// The 1.09 executable, its shaders intact.
bool CheckFiles(const PsGameFiles& game, std::string& kind, std::string& advice);
/// The PS_FPS preset, PS_PATCHES, the effect switches of psport.ini, PS_RENDER_RES and the
/// Intel and online-choice fixes.
bool AddPatches(const PsGameFiles& game, const PsPatchServices& services);
/// The 1.09 executable the presets' patches and the profile's hooks are written for.
bool IsSupportedImage(const PsGameFiles& game);
/// The preset's frame rate (60, 90 or 480 for uncap) when AddPatches left the frame rate to the
/// profile's hooks (frame_rate.cpp) instead of the preset's patches; 0 otherwise.
int FrameHooksChosen();
/// External patches (a copy of the community XML) that would rewrite the code the frame rate hooks
/// replace: the presets' frame rate patches, while the hooks drive it.
const char* ExternalPatchRefusal(const char* sz_name);
} // namespace PsLaunch
