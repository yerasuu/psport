// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: its frame rate set free by hooks in its code (PS_FRAME_HOOKS, on by default for the
// 1.09 executable), not by the community's patches: its frame pacer no longer waits for a 30 FPS
// frame, its step takes the frame's real time, and what it ties to a frame (a 30 FPS frame's time,
// a per-frame step, smoothing and damping) reads values the hooks recompute from that time every
// frame (frame_rate.cpp). The patches stay the fallback (patches.cpp, PS_FRAME_HOOKS=0).
#pragma once

#include <cstdint>

namespace PsFrameRate {
/// Installs the hooks for the PS_FPS preset (the profile's patch_image, before the game runs) when
/// the launch chose them (PsLaunch::FrameHooksChosen).
void PatchImage(unsigned char* p_image, std::uint64_t cb_image);
/// Whether the hooks run the game's frame rate: the video out then shows a frame as soon as the
/// game submits it (the game's own flip rate asks for 30 FPS).
bool IsUnlocked();
} // namespace PsFrameRate
