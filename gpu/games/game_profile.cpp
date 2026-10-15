// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Game {

// One per game (games/<game>/title.cpp).
extern const Title BloodborneTitle;

namespace {
constexpr std::array<const Title*, 1> Titles = {&BloodborneTitle};
std::atomic<const Title*> s_p_launched{nullptr};
std::atomic<const Profile*> s_p_active{nullptr};
} // namespace

const Title* Find(const char* sz_serial) {
    for (const Title* p_title : Titles) {
        if (sz_serial && std::ranges::any_of(p_title->serials, [&](const char* sz_candidate) { return std::strcmp(sz_candidate, sz_serial) == 0; })) {
            return p_title;
        }
    }
    return nullptr;
}

void Select(const char* sz_serial) {
    const char* sz_env = std::getenv("PS_GAME_PROFILE");
    const bool is_profile_off = sz_env && std::strcmp(sz_env, "none") == 0;
    const Title* p_title = Find(sz_serial);
    s_p_launched.store(p_title, std::memory_order_release);
    if (!p_title) {
        std::printf("Game profile: none for %s: the generic paths only%s\n", sz_serial ? sz_serial : "?",
                    is_profile_off ? " (PS_GAME_PROFILE=none)" : "");
        return;
    }
    if (!is_profile_off) {
        s_p_active.store(p_title->p_profile, std::memory_order_release);
    }
    std::printf("Game profile: %s (%s): title data applied, %s\n", p_title->sz_name, sz_serial,
                !p_title->p_profile ? "no translator profile"
                : is_profile_off    ? "translator profile off (PS_GAME_PROFILE=none)"
                                    : "translator profile active");
}

const Title* Launched() {
    return s_p_launched.load(std::memory_order_acquire);
}

const Profile* Active() {
    return s_p_active.load(std::memory_order_acquire);
}

bool IsDynamicWriter(std::uint64_t guest_rip) {
    const Profile* p_profile = Active();
    return p_profile && std::ranges::any_of(p_profile->dynamic_writers, [&](const CodeRange& r) {
               return guest_rip >= r.begin && guest_rip < r.end;
           });
}

} // namespace Game
