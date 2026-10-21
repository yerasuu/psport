// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops and Black Ops II: the crosshair with keyboard and mouse (crosshair.h).
//
// CG_ShouldDrawCrosshair reads the current value of cg_drawCrosshair (byte +0x28 of its dvar_t)
// each frame, without its modified or latched copies, so writing that byte is enough (kb #729). Its
// draw also needs CG_Draw2D's cg_draw2D, cg_drawHUD and hud_drawHUD on and hud_missionFailed off
// (cutscenes and scripted moments turn them off), which psport only reads: it holds the crosshair
// on only while those show the HUD, so the game's own hides of the whole HUD stay. Each dvar_t is
// found through the image's global holding its pointer, its type (+0x1c) checked before any read.
// A game's write of 1 over psport's 1 is not seen (the byte does not change), so psport never
// writes back a 0 it saw earlier: it stops holding and leaves the byte, and the game's next write
// decides.
#include "crosshair.h"
#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

// src/runtime/runtime_calls.c: a line of the exit summary.
extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));
// src/input/runtime_pad.c
extern "C" int runtime_pad_prompt_device(void);

namespace PsBlackOpsCrosshair {
namespace {
using PsBlackOps::Get;
using PsBlackOps::IsGuestAddress;
using PsBlackOps::Put;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

constexpr int PromptKeyboard = 2; // RUNTIME_PAD_PROMPT_KEYBOARD
constexpr u64 OffDvarType = 0x1c;
constexpr u64 OffDvarCurrent = 0x28;
/// The game's changes of cg_drawCrosshair are logged this many times, of the other dvars as many
/// (each its own budget, so a fading alpha does not use up the crosshair's), psport's own actions
/// as many, then only counted; each kind of failure is logged LoggedFailures times.
constexpr u64 LoggedChanges = 32;
constexpr u64 LoggedFailures = 8;

enum Dvar : std::size_t { DrawCrosshair, Draw2D, DrawHud, HudDrawHud, MissionFailed, CrosshairAlpha, DvarCount };
constexpr const char* DvarNames[DvarCount] = {"cg_drawCrosshair", "cg_draw2D",         "cg_drawHUD",
                                              "hud_drawHUD",      "hud_missionFailed", "cg_crosshairAlpha"};

/// off_check: cg_drawCrosshair's reader in CG_ShouldDrawCrosshair, whose lea gives the global of
/// off_globals[DrawCrosshair]; the dvars' type codes differ between the games.
struct Layout {
    const char* sz_game;
    u64 off_check;
    std::span<const u8> check;
    u32 type_bool, type_float;
    std::array<u64, DvarCount> off_globals;
};
constexpr u8 BlackOpsReaderCode[] = {
    0x48, 0x8d, 0x05, 0x07, 0x8d, 0x6d, 0x02, // lea rax, [cg_drawCrosshair] (0x27702f0)
    0x48, 0x8b, 0x00,                         // mov rax, [rax]
    0x80, 0x78, 0x28, 0x00,                   // cmp byte [rax + 0x28], 0
};
constexpr u8 BlackOps2ReaderCode[] = {
    0x48, 0x8d, 0x05, 0x09, 0xcb, 0x3c, 0x02, // lea rax, [cg_drawCrosshair] (0x2470d98)
    0x48, 0x8b, 0x38,                         // mov rdi, [rax]
    0xe8, 0x59, 0xb4, 0x35, 0x00,             // call Dvar_GetBool (0x3ff6f0)
    0x84, 0xc0,                               // test al, al
};
constexpr Layout BlackOps = {"Black Ops",
                             0x975e2,
                             BlackOpsReaderCode,
                             0,
                             1,
                             {0x27702f0, 0x27704f0, 0x27704f8, 0x27708e8, 0x27708f0, 0x27704c8}};
constexpr Layout BlackOps2 = {"Black Ops II",
                              0xa4288,
                              BlackOps2ReaderCode,
                              1,
                              2,
                              {0x2470d98, 0x2470e68, 0x2470e78, 0x15b4760, 0x24710b8, 0x2470e40}};

std::atomic<const Layout*> s_p_layout{nullptr};
u64 s_va_image = 0;
bool s_is_forcing_allowed = true;

/// Only the pad's thread touches this state, as the mouse camera's.
bool s_has_seen = false;
std::array<float, DvarCount> s_seen{};
u8 s_game_crosshair = 1;
bool s_is_held = false;

std::atomic<u64> s_c_unregistered{0}, s_c_unreadable{0}, s_c_refused{0}, s_c_write_failed{0};
std::atomic<u64> s_c_changes{0}, s_c_crosshair_changes{0}, s_c_actions{0}, s_c_held{0}, s_c_shown{0},
    s_c_released{0};

enum class DvarRead { Read, Unregistered, Unreadable, WrongType };

/// The dvar's current value (a bool as 0 or 1, the raw byte otherwise) and where it lies.
DvarRead ReadDvar(const Layout& layout, Dvar dvar, u64* p_out_va_value, float* p_out_value) {
    u64 va_dvar = 0;
    if (!Get(s_va_image + layout.off_globals[dvar], &va_dvar)) {
        return DvarRead::Unreadable;
    }
    if (va_dvar == 0) {
        return DvarRead::Unregistered;
    }
    u32 type = 0;
    if (!IsGuestAddress(va_dvar, 8) || !Get(va_dvar + OffDvarType, &type)) {
        return DvarRead::Unreadable;
    }
    const bool is_float = dvar == CrosshairAlpha;
    if (type != (is_float ? layout.type_float : layout.type_bool)) {
        return DvarRead::WrongType;
    }
    *p_out_va_value = va_dvar + OffDvarCurrent;
    if (is_float) {
        return Get(*p_out_va_value, p_out_value) ? DvarRead::Read : DvarRead::Unreadable;
    }
    u8 value = 0;
    if (!Get(*p_out_va_value, &value)) {
        return DvarRead::Unreadable;
    }
    *p_out_value = float(value);
    return DvarRead::Read;
}

void NoteFailure(std::atomic<u64>& c_failures, const Layout& layout, Dvar dvar, const char* sz_what) {
    if (c_failures.fetch_add(1, std::memory_order_relaxed) < LoggedFailures) {
        std::printf("Crosshair: %s: %s %s (logged %llu times, then counted)\n", layout.sz_game, DvarNames[dvar],
                    sz_what, (unsigned long long)LoggedFailures);
    }
}

void NoteAction(const Layout& layout, const char* sz_what) {
    if (s_c_actions.fetch_add(1, std::memory_order_relaxed) < LoggedChanges) {
        std::printf("Crosshair: %s: %s (logged %llu times, then counted)\n", layout.sz_game, sz_what,
                    (unsigned long long)LoggedChanges);
    }
}

void NoteChanges(const Layout& layout, const std::array<float, DvarCount>& values) {
    if (!s_has_seen) {
        s_has_seen = true;
        s_seen = values;
        s_game_crosshair = u8(values[DrawCrosshair]);
        std::printf("Crosshair: %s: the game's dvars: cg_drawCrosshair %g, cg_draw2D %g, cg_drawHUD %g, "
                    "hud_drawHUD %g, hud_missionFailed %g, cg_crosshairAlpha %g\n",
                    layout.sz_game, values[DrawCrosshair], values[Draw2D], values[DrawHud], values[HudDrawHud],
                    values[MissionFailed], values[CrosshairAlpha]);
        return;
    }
    for (std::size_t i = 0; i < DvarCount; ++i) {
        if (values[i] == s_seen[i]) {
            continue;
        }
        const bool was_held = i == DrawCrosshair && s_is_held;
        if (i == DrawCrosshair) {
            s_game_crosshair = u8(values[i]);
            s_is_held = false;
        }
        std::atomic<u64>& c_changes = i == DrawCrosshair ? s_c_crosshair_changes : s_c_changes;
        if (c_changes.fetch_add(1, std::memory_order_relaxed) < LoggedChanges) {
            std::printf("Crosshair: %s: the game set %s %g -> %g%s (logged %llu times, then counted)\n",
                        layout.sz_game, DvarNames[i], s_seen[i], values[i], was_held ? ", over psport's" : "",
                        (unsigned long long)LoggedChanges);
        }
        s_seen[i] = values[i];
    }
}

void Summary(char* sz_out, std::size_t cb_out) {
    const Layout* p_layout = s_p_layout.load(std::memory_order_acquire);
    std::snprintf(sz_out, cb_out,
                  "Crosshair: %s: held on by psport for %llu pad samples (%llu times set over the game's 0, %llu "
                  "times released to the game's next write), %llu changes of cg_drawCrosshair by the game, %llu of "
                  "the other dvars, %llu samples before the game registered them, %llu refused (a dvar's type "
                  "not the build's), %llu unreadable, %llu writes failed%s\n",
                  p_layout ? p_layout->sz_game : "?", (unsigned long long)s_c_held.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_shown.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_released.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_crosshair_changes.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_changes.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_unregistered.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_refused.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_unreadable.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_write_failed.load(std::memory_order_relaxed),
                  s_is_forcing_allowed ? "" : " (PS_FORCE_CROSSHAIR=0: never held)");
}

void Install(unsigned char* p_image, std::uint64_t cb_image, const Layout& layout) {
    const u64 off_data_end = *std::max_element(layout.off_globals.begin(), layout.off_globals.end()) + 8;
    if (off_data_end > cb_image || layout.off_check + layout.check.size() > cb_image ||
        std::memcmp(p_image + layout.off_check, layout.check.data(), layout.check.size()) != 0) {
        std::printf("Crosshair: %s: not the known build (cg_drawCrosshair's reader at 0x%llx differs): its dvars are "
                    "neither read nor held\n",
                    layout.sz_game, (unsigned long long)layout.off_check);
        return;
    }
    if (const char* sz_env = std::getenv("PS_FORCE_CROSSHAIR"); sz_env && sz_env[0] == '0') {
        s_is_forcing_allowed = false;
    }
    s_va_image = reinterpret_cast<u64>(p_image);
    s_p_layout.store(&layout, std::memory_order_release);
    runtime_calls_add_summary(Summary);
    std::printf("Crosshair: %s: build checked (cg_drawCrosshair's reader at 0x%llx): its dvars are logged as the "
                "game changes them; %s\n",
                layout.sz_game, (unsigned long long)layout.off_check,
                s_is_forcing_allowed ? "psport holds it on with keyboard and mouse while the game shows its HUD"
                                     : "never held on (PS_FORCE_CROSSHAIR=0)");
}
} // namespace

void PatchBlackOps(unsigned char* p_image, std::uint64_t cb_image) {
    Install(p_image, cb_image, BlackOps);
}

void PatchBlackOps2(unsigned char* p_image, std::uint64_t cb_image) {
    Install(p_image, cb_image, BlackOps2);
}

void Sample(std::uint32_t*, const std::uint8_t*) {
    const Layout* p_layout = s_p_layout.load(std::memory_order_acquire);
    if (!p_layout) {
        return;
    }
    std::array<float, DvarCount> values{};
    u64 va_crosshair = 0;
    for (std::size_t i = 0; i < DvarCount; ++i) {
        u64 va_value = 0;
        switch (ReadDvar(*p_layout, Dvar(i), &va_value, &values[i])) {
        case DvarRead::Read:
            break;
        case DvarRead::Unregistered:
            s_c_unregistered.fetch_add(1, std::memory_order_relaxed);
            return;
        case DvarRead::Unreadable:
            NoteFailure(s_c_unreadable, *p_layout, Dvar(i), "is not readable (its global or its dvar_t)");
            return;
        case DvarRead::WrongType:
            NoteFailure(s_c_refused, *p_layout, Dvar(i), "has a type other than the build's: not used");
            return;
        }
        if (i == DrawCrosshair) {
            va_crosshair = va_value;
        }
    }
    NoteChanges(*p_layout, values);

    const bool is_keyboard = runtime_pad_prompt_device() == PromptKeyboard;
    const bool is_hud_shown =
        values[Draw2D] != 0 && values[DrawHud] != 0 && values[HudDrawHud] != 0 && values[MissionFailed] == 0;
    const bool should_hold = s_is_forcing_allowed && is_keyboard && is_hud_shown;
    if (should_hold && !s_is_held && s_game_crosshair == 0) {
        if (!Put<u8>(va_crosshair, 1)) {
            NoteFailure(s_c_write_failed, *p_layout, DrawCrosshair, "could not be set to 1");
            return;
        }
        s_is_held = true;
        s_seen[DrawCrosshair] = 1;
        s_c_shown.fetch_add(1, std::memory_order_relaxed);
        NoteAction(*p_layout, "psport set cg_drawCrosshair 0 -> 1 (keyboard and mouse in use, HUD shown)");
    } else if (!should_hold && s_is_held) {
        // The byte stays 1: it is taken as the game's until the game writes it again.
        s_is_held = false;
        s_game_crosshair = 1;
        s_c_released.fetch_add(1, std::memory_order_relaxed);
        NoteAction(*p_layout, is_keyboard
                                  ? "stopped holding cg_drawCrosshair (HUD hidden); the game's next write decides"
                                  : "stopped holding cg_drawCrosshair (a pad in use); the game's next write decides");
    }
    if (s_is_held) {
        s_c_held.fetch_add(1, std::memory_order_relaxed);
    }
}
} // namespace PsBlackOpsCrosshair
