// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Game {

// One per game (games/<game>.cpp, games/<game>/profile.cpp).
extern const Profile BloodborneProfile;

namespace {
constexpr std::array<const Profile*, 1> Profiles = {&BloodborneProfile};
std::atomic<const Profile*> s_p_active{nullptr};
std::atomic<const Profile*> s_p_launched{nullptr};
} // namespace

const Profile* Find(const char* sz_serial) {
    for (const Profile* p_profile : Profiles) {
        if (sz_serial && std::ranges::any_of(p_profile->serials, [&](const char* sz_candidate) { return std::strcmp(sz_candidate, sz_serial) == 0; })) {
            return p_profile;
        }
    }
    return nullptr;
}

void Select(const char* sz_serial) {
    s_p_launched.store(Find(sz_serial), std::memory_order_release);
    const char* sz_env = std::getenv("PS_GAME_PROFILE");
    if (sz_env && std::strcmp(sz_env, "none") == 0) {
        std::printf("Game profile: none (PS_GAME_PROFILE=none): the game runs as an unknown one\n");
        return;
    }
    if (const Profile* p_profile = Find(sz_serial)) {
        s_p_active.store(p_profile, std::memory_order_release);
        std::printf("Game profile: %s (%s)\n", p_profile->sz_name, sz_serial);
        return;
    }
    std::printf("Game profile: none for %s: the generic paths only\n", sz_serial ? sz_serial : "?");
}

const Profile* Active() {
    return s_p_active.load(std::memory_order_acquire);
}

const Profile* Launched() {
    return s_p_launched.load(std::memory_order_acquire);
}

bool IsDynamicWriter(std::uint64_t guest_rip) {
    const Profile* p_profile = Active();
    return p_profile && std::ranges::any_of(p_profile->dynamic_writers, [&](const CodeRange& r) {
               return guest_rip >= r.begin && guest_rip < r.end;
           });
}

} // namespace Game
