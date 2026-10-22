// SPDX-License-Identifier: GPL-2.0-or-later
// Atelier Ayesha DX: its profile (profile.cpp) draws its button glyphs for the device in use; the
// neutral keyboard layout serves it.
#include "game_profile.h"

namespace Game {

extern const Profile AyeshaProfile;

namespace {
constexpr const char* Serials[] = {"CUSA16921"};
} // namespace

extern const Title AyeshaTitle = {
    .sz_name = "Atelier Ayesha DX",
    .serials = Serials,
    .p_profile = &AyeshaProfile,
};

} // namespace Game
