// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops and Black Ops II: their keyboard and mouse layout (Game::Title); their
// profile (profile.cpp) turns the camera with the mouse through the games' own mouse input.
#include "game_profile.h"

namespace Game {

extern const Profile BlackOpsProfile;
extern const Profile BlackOps2Profile;

namespace {
constexpr const char* BlackOpsSerials[] = {"CUSA57547"};
constexpr const char* BlackOps2Serials[] = {"CUSA57548"};

/// The PC games' layout over the actions of both games' buttons_default.cfg: R2 +attack, L2
/// +speed_throw, R1 +frag, L1 +smoke, R3 +melee, L3 +breath_sprint, cross +gostand, circle +stance,
/// square +usereload, triangle weapnext (Black Ops II: +weapnext_inventory, held for the
/// inventory), the d-pad +actionslot 1-4, options the menu, the touchpad the scores. The
/// mouse_look_ values tune the stick the mouse drives without the native camera (PS_NATIVE_MOUSE=0,
/// another build, menus); provisional until #457 measures the games' stick.
constexpr const char* DefaultControls = "key.cross=Space\n"
                                        "key.circle=Left Ctrl, C\n"
                                        "key.square=R, F\n"
                                        "key.triangle=1, 2, Wheel Up, Wheel Down\n"
                                        "key.l1=Q, Mouse X2\n"
                                        "key.r1=G, Mouse X1\n"
                                        "key.l2=Mouse Right\n"
                                        "key.r2=Mouse Left\n"
                                        "key.l3=Left Shift\n"
                                        "key.r3=V, Mouse Middle\n"
                                        "key.options=Escape\n"
                                        "key.touchpad=Tab\n"
                                        "key.up=3\n"
                                        "key.down=4\n"
                                        "key.left=5\n"
                                        "key.right=6\n"
                                        "mouse_look_gain=1\n"
                                        "mouse_look_smoothing_ms=40\n"
                                        "mouse_look_deadzone=24\n";
} // namespace

extern const Title BlackOpsTitle = {
    .sz_name = "Call of Duty: Black Ops",
    .serials = BlackOpsSerials,
    .sz_default_controls = DefaultControls,
    .p_profile = &BlackOpsProfile,
};

extern const Title BlackOps2Title = {
    .sz_name = "Call of Duty: Black Ops II",
    .serials = BlackOps2Serials,
    .sz_default_controls = DefaultControls,
    .p_profile = &BlackOps2Profile,
};

} // namespace Game
