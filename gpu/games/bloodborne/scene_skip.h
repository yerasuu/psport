// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): the port's skip binding (skip_movies, src/input/runtime_pad.c) when it stopped
// one of the game's movies while the game holds its input back. The game leaves its scenes on
// Options, and SrShadowy's fork (b1d3b8b) presses it after stopping a movie too, so the game does
// not wait on a scene whose movie is gone: one Options press per skip, never one per frame. No
// reliable signal tells an in-engine cutscene from other blocked states (loads, fades), so those
// are left to the game's own Options.
#pragma once

#include <cstdint>

namespace PsSceneSkip {
/// Game::Profile pfn_skip_scene: true when the press stopped a movie, the game's input is blocked
/// and an Options press is queued.
bool Press(bool has_stopped_movie);

/// The queued Options press in the pad state the game reads (pfn_filter_pad), held long enough
/// for the game to see it.
void FilterPad(std::uint32_t* p_buttons);
} // namespace PsSceneSkip
