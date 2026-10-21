// SPDX-License-Identifier: GPL-2.0-or-later
#include "scene_skip.h"

#include <atomic>
#include <chrono>
#include <cstdio>

#include "menu_pointer.h"

namespace PsSceneSkip {
namespace {
constexpr std::uint32_t Options = 0x8;
constexpr std::int64_t PressMs = 100;

std::atomic<std::int64_t> s_until_ms{0};
std::atomic<std::uint32_t> s_c_presses{0};

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

bool Press(bool has_stopped_movie) {
    if (!has_stopped_movie || !PsMenuPointer::IsInputBlocked()) {
        return false;
    }
    s_until_ms.store(NowMs() + PressMs, std::memory_order_relaxed);
    std::printf("Scene skip: Options pressed after stopping the game's movie (skip %u)\n",
                s_c_presses.fetch_add(1, std::memory_order_relaxed) + 1);
    return true;
}

void FilterPad(std::uint32_t* p_buttons) {
    if (NowMs() < s_until_ms.load(std::memory_order_relaxed)) {
        *p_buttons |= Options;
    }
}
} // namespace PsSceneSkip
