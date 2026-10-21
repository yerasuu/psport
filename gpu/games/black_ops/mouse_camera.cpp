// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops and Black Ops II: native mouse camera (mouse_camera.h).
//
// Both executables keep the PC games' CL_MouseMove: each frame it takes the counts of slot idx of
// two int accumulators (x, y), flips idx and zeroes the new slot, and turns the view by
// counts * sensitivity * m_yaw (m_pitch), 5 * 0.022 degrees a count by default (Black Ops: dvar
// sensitivity; Black Ops II: the profile's mouseSensitivity), before the aim assist runs. Nothing
// writes them on the console (no sceMouse): each pad sample adds the mouse's counts to slot idx
// with an atomic add from the host (kb #723, decision #724).
// - Black Ops: local client 0 at *(image + 0x266d7d8), x at +0x1be0, y at +0x1be8, idx at
//   +0x1bf0; the reader at 0x3f5401.
// - Black Ops II: in the image, x at 0xcab64c, y at 0xcab654, idx at 0xcab65c; the reader at
//   0x130330.
// +x turns right and +y looks down, as the turn the caller hands over (runtime_pad.c applies
// mouse_invert_y before). Where CL_MouseMove reads and zeroes the counts without turning by them
// (frame msec 0, a vehicle or killcam flag of the client, Black Ops' top-down camera) the sample
// goes to the stick; its +strafe key and the guided-missile spin guard are not read.
#include "mouse_camera.h"
#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

// src/runtime/runtime_calls.c: a line of the exit summary.
extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));

namespace PsBlackOpsMouse {
namespace {
using PsBlackOps::Get;
using PsBlackOps::IsGuestAddress;
using u64 = std::uint64_t;
using u8 = std::uint8_t;
using i32 = std::int32_t;

struct CodeCheck {
    u64 off_code;
    std::span<const u8> code;
};

constexpr u8 BlackOpsClientCode[] = {
    0x48, 0x8d, 0x0d, 0xac, 0x91, 0x27, 0x02, // lea rcx, [clients] (0x266d7d8)
    0x4d, 0x69, 0xf7, 0x88, 0x0e, 0x20, 0x00, // imul r14, r15, 0x200e88
    0x48, 0x8d, 0x05, 0x96, 0x91, 0x27, 0x02, // lea rax, [client count] (0x266d7d0)
};
constexpr u8 BlackOpsReaderCode[] = {
    0x49, 0x63, 0x84, 0x24, 0xf0, 0x1b, 0x00, 0x00,                   // movsxd rax, [r12 + 0x1bf0]
    0xc4, 0xc1, 0x6a, 0x2a, 0x84, 0x84, 0xe0, 0x1b, 0x00, 0x00,       // vcvtsi2ss xmm0, [r12 + rax*4 + 0x1be0]
    0xc4, 0xc1, 0x6a, 0x2a, 0x94, 0x84, 0xe8, 0x1b, 0x00, 0x00,       // vcvtsi2ss xmm2, [r12 + rax*4 + 0x1be8]
    0x83, 0xf0, 0x01,                                                 // xor eax, 1
    0xc4, 0xc1, 0x7b, 0x10, 0xbc, 0x24, 0x9c, 0x1c, 0x00, 0x00,       // vmovsd xmm7, [r12 + 0x1c9c]
    0x48, 0x8d, 0x1d, 0xff, 0x52, 0x29, 0x02,                         // lea rbx, [frame msec]
    0x41, 0x89, 0x84, 0x24, 0xf0, 0x1b, 0x00, 0x00,                   // mov [r12 + 0x1bf0], eax
    0x48, 0x98,                                                       // cdqe
    0x41, 0xc7, 0x84, 0x84, 0xe0, 0x1b, 0x00, 0x00, 0, 0, 0, 0,       // mov dword [r12 + rax*4 + 0x1be0], 0
    0x49, 0x63, 0x84, 0x24, 0xf0, 0x1b, 0x00, 0x00,                   // movsxd rax, [r12 + 0x1bf0]
    0x41, 0xc7, 0x84, 0x84, 0xe8, 0x1b, 0x00, 0x00, 0, 0, 0, 0,       // mov dword [r12 + rax*4 + 0x1be8], 0
};
constexpr u8 BlackOps2ReaderCode[] = {
    0x48, 0x63, 0x05, 0x25, 0xb3, 0xb7, 0x00, // movsxd rax, [idx]
    0x48, 0x8d, 0x0d, 0x0e, 0xb3, 0xb7, 0x00, // lea rcx, [x]
    0xc5, 0xf2, 0x2a, 0x04, 0x81,             // vcvtsi2ss xmm0, [rcx + rax*4]
    0xc5, 0xfa, 0x11, 0x45, 0xb0,             // vmovss [rbp - 0x50], xmm0
    0xc5, 0xf2, 0x2a, 0x44, 0x81, 0x08,       // vcvtsi2ss xmm0, [rcx + rax*4 + 8]
    0xc5, 0xfa, 0x11, 0x45, 0x90,             // vmovss [rbp - 0x70], xmm0
    0x4c, 0x8d, 0x2d, 0xe6, 0xa4, 0xb9, 0x00, // lea r13, [frame msec]
    0x83, 0xf0, 0x01,                         // xor eax, 1
    0x48, 0x8d, 0x0d, 0xe8, 0xb2, 0xb7, 0x00, // lea rcx, [x]
    0x89, 0x05, 0xf2, 0xb2, 0xb7, 0x00,       // mov [idx], eax
    0x48, 0x98,                               // cdqe
    0xc7, 0x04, 0x81, 0, 0, 0, 0,             // mov dword [rcx + rax*4], 0
    0x45, 0x8b, 0x65, 0x00,                   // mov r12d, [r13]
    0x48, 0x63, 0x05, 0xde, 0xb2, 0xb7, 0x00, // movsxd rax, [idx]
    0x45, 0x85, 0xe4,                         // test r12d, r12d
    0xc7, 0x44, 0x81, 0x08, 0, 0, 0, 0,       // mov dword [rcx + rax*4 + 8], 0
};
constexpr u8 BlackOpsGateCode[] = {
    0x8b, 0x0b,                                     // mov ecx, [rbx] (frame msec)
    0x85, 0xc9,                                     // test ecx, ecx
    0x74, 0x1b,                                     // je past the turn
    0x41, 0x8b, 0x44, 0x24, 0x34,                   // mov eax, [r12 + 0x34]
    0xa9, 0x00, 0x08, 0x00, 0x00,                   // test eax, 0x800
    0x75, 0x0f,                                     // jne past the turn
    0x41, 0x83, 0xbc, 0x24, 0x80, 0x01, 0x00, 0x00, // cmp dword [r12 + 0x180], 0x24
    0x24, 0x0f, 0x85, 0xa6, 0x01, 0x00, 0x00,       // jne to the turn
};
constexpr u8 BlackOpsTopDownCode[] = {
    0x48, 0x8d, 0x0d, 0xe6, 0x2e, 0xaf, 0x00,             // lea rcx, [player_topDownCamMode] (0xee8548)
    0x48, 0x8b, 0x09,                                     // mov rcx, [rcx]
    0xc4, 0xc1, 0x62, 0x59, 0x9c, 0x24, 0x24, 0x1c, 0, 0, // vmulss xmm3, xmm3, [r12 + 0x1c24]
    0x83, 0x79, 0x28, 0x00,                               // cmp dword [rcx + 0x28], 0
    0x74, 0x08,                                           // je to the turn
    0x41, 0x83, 0x7c, 0x24, 0x2c, 0x02,                   // cmp dword [r12 + 0x2c], 2
};
constexpr u8 BlackOps2ClientCode[] = {
    0x48, 0x8d, 0x0d, 0x49, 0xff, 0xb7, 0x00, // lea rcx, [client count] (0xcb01f8)
    0x48, 0x89, 0xf3,                         // mov rbx, rsi
    0x41, 0x89, 0xfe,                         // mov r14d, edi
    0x48, 0x8b, 0x00,                         // mov rax, [rax]
    0x48, 0x89, 0x45, 0xd0,                   // mov [rbp - 0x30], rax
    0x39, 0x39,                               // cmp [rcx], edi
    0x7e, 0x0c,                               // jle no client
    0x48, 0x8d, 0x05, 0x39, 0xff, 0xb7, 0x00, // lea rax, [clients] (0xcb0200)
    0x4c, 0x8b, 0x38,                         // mov r15, [rax]
    0xeb, 0x03,                               // jmp
    0x45, 0x31, 0xff,                         // xor r15d, r15d
};
constexpr u8 BlackOps2GateCode[] = {
    0x41, 0xf6, 0x87, 0xc5, 0x00, 0x00, 0x00, 0x08, // test byte [r15 + 0xc5], 8
    0x0f, 0x85, 0xe1, 0x03, 0x00, 0x00,             // jne past the turn
    0x41, 0x83, 0xbf, 0x30, 0x02, 0x00, 0x00, 0x2b, // cmp dword [r15 + 0x230], 0x2b
    0x0f, 0x84, 0xd3, 0x03, 0x00, 0x00,             // je past the turn
    0x41, 0xf6, 0x87, 0xd1, 0x00, 0x00, 0x00, 0x04, // test byte [r15 + 0xd1], 4
};
constexpr CodeCheck BlackOpsChecks[] = {{0x3f4625, BlackOpsClientCode},
                                        {0x3f5401, BlackOpsReaderCode},
                                        {0x3f545b, BlackOpsGateCode},
                                        {0x3f565b, BlackOpsTopDownCode}};
constexpr CodeCheck BlackOps2Checks[] = {
    {0x1302a8, BlackOps2ClientCode}, {0x130330, BlackOps2ReaderCode}, {0x1303b7, BlackOps2GateCode}};

/// A field of local client 0 that makes CL_MouseMove read and zero the counts but not turn by them
/// (a vehicle, a killcam, a frozen player): the mouse is ignored when (field & mask) == value.
struct Gate {
    u64 off_field;
    std::uint32_t mask, value;
};
constexpr Gate BlackOpsGates[] = {{0x34, 0x800, 0x800}, {0x180, ~0u, 0x24}};
constexpr Gate BlackOps2Gates[] = {{0xc4, 0x800, 0x800}, {0x230, ~0u, 0x2b}, {0xd0, 0x400, 0x400}};

/// Where a game keeps its mouse input: local client 0 (off_clients: the image's pointer to the
/// clients, off_client_count their count) holds the accumulators when is_in_client, else the image
/// does. off_frame_msec: the frame's milliseconds, 0 skips the turn; off_top_down_dvar (Black Ops,
/// player_topDownCamMode, its int at +0x28): non-zero ignores the mouse unless the client's +0x2c
/// is 2.
struct Layout {
    const char* sz_game;
    std::span<const CodeCheck> checks;
    u64 off_clients, off_client_count;
    bool is_in_client;
    u64 off_x, off_y, off_index;
    u64 off_frame_msec;
    std::span<const Gate> gates;
    u64 off_top_down_dvar;
};
constexpr Layout BlackOps = {
    "Black Ops", BlackOpsChecks, 0x266d7d8, 0x266d7d0, true, 0x1be0, 0x1be8, 0x1bf0, 0x268a730, BlackOpsGates,
    0xee8548};
constexpr Layout BlackOps2 = {
    "Black Ops II", BlackOps2Checks, 0xcb0200, 0xcb01f8, false, 0xcab64c, 0xcab654, 0xcab65c, 0xcca840,
    BlackOps2Gates, 0};

/// runtime_pad.c's MOUSE_RAD_PER_PIXEL: a count per pixel at mouse_sensitivity 1, as the PC games
/// read the mouse.
constexpr float RadiansPerCount = 0.0015f;
/// Counts left unread this long after the game last showed it reads them (or after they were
/// added, when later): it does not (menus, loading).
constexpr long long TakeTimeoutMs = 100;
/// Each kind of failure is logged this many times, then only counted.
constexpr u64 LoggedFailures = 8;

std::atomic<const Layout*> s_p_layout{nullptr};
u64 s_va_image = 0;

/// What the last adds left in the game, until it reads them. Only the pad's thread touches this
/// state, as runtime_pad.c's mouse look state.
struct Pending {
    bool is_waiting = false;
    i32 i_slot = 0;
    i32 left_x = 0, left_y = 0;
    i32 added_x = 0, added_y = 0;
    long long since_ms = 0;
};
Pending s_pending;
float s_carry_x = 0.0f, s_carry_y = 0.0f;
bool s_is_taking = true;
i32 s_i_slot_seen = -1;
long long s_read_seen_ms = 0;

std::atomic<u64> s_c_fed{0}, s_c_raced{0}, s_c_invalid{0}, s_c_ignored{0}, s_c_handed_back{0};
std::atomic<u64> s_c_take_back_failed{0}, s_c_partial{0};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Counts a failure, logged its first LoggedFailures times.
void NoteFailure(std::atomic<u64>& c_failures, const char* sz_what, i32 counts_x, i32 counts_y) {
    if (c_failures.fetch_add(1, std::memory_order_relaxed) < LoggedFailures) {
        const Layout* p_layout = s_p_layout.load(std::memory_order_acquire);
        std::printf("Mouse camera: %s: %s (%d, %d counts; logged %llu times, then counted)\n",
                    p_layout ? p_layout->sz_game : "?", sz_what, counts_x, counts_y,
                    (unsigned long long)LoggedFailures);
    }
}

/// Adds value to the guest's int at va atomically (the game's thread may flip and zero it at the
/// same time); *p_out_sum is what it holds after. False where nothing is mapped.
bool Add(u64 va, i32 value, i32* p_out_sum) {
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        g_p_runtime_fault_recover = nullptr;
        return false;
    }
    g_p_runtime_fault_recover = &recover;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    *p_out_sum = __atomic_add_fetch(reinterpret_cast<i32*>(va), value, __ATOMIC_RELAXED);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    g_p_runtime_fault_recover = nullptr;
    return true;
}

struct Slots {
    u64 va_client = 0;
    u64 va_index = 0;
    i32 i_slot = 0;
    u64 va_x = 0, va_y = 0;
};

/// The accumulators' slot the game reads next; false when the pointers are not valid (no client
/// yet, or not in the guest's memory).
bool FindSlots(const Layout& layout, Slots* p_out_slots) {
    i32 c_clients = 0;
    u64 va_client = 0;
    if (!Get(s_va_image + layout.off_client_count, &c_clients) || c_clients <= 0 ||
        !Get(s_va_image + layout.off_clients, &va_client) || !IsGuestAddress(va_client, 8)) {
        return false;
    }
    const u64 va_base = layout.is_in_client ? va_client : s_va_image;
    i32 i_slot = -1;
    if (!Get(va_base + layout.off_index, &i_slot) || (i_slot != 0 && i_slot != 1)) {
        return false;
    }
    *p_out_slots = {.va_client = va_client,
                    .va_index = va_base + layout.off_index,
                    .i_slot = i_slot,
                    .va_x = va_base + layout.off_x + 4 * u64(i_slot),
                    .va_y = va_base + layout.off_y + 4 * u64(i_slot)};
    return true;
}

enum class MouseUse { Turns, Ignored, Unreadable };

/// Whether CL_MouseMove turns by the counts it reads now, from the fields it tests first.
MouseUse ReadMouseUse(const Layout& layout, u64 va_client) {
    i32 frame_msec = 0;
    if (!Get(s_va_image + layout.off_frame_msec, &frame_msec)) {
        return MouseUse::Unreadable;
    }
    if (frame_msec == 0) {
        return MouseUse::Ignored;
    }
    for (const Gate& gate : layout.gates) {
        std::uint32_t field = 0;
        if (!Get(va_client + gate.off_field, &field)) {
            return MouseUse::Unreadable;
        }
        if ((field & gate.mask) == gate.value) {
            return MouseUse::Ignored;
        }
    }
    if (layout.off_top_down_dvar) {
        u64 va_dvar = 0;
        i32 top_down = 0, camera_kind = 0;
        if (!Get(s_va_image + layout.off_top_down_dvar, &va_dvar) || !IsGuestAddress(va_dvar, 8) ||
            !Get(va_dvar + 0x28, &top_down) || !Get(va_client + 0x2c, &camera_kind)) {
            return MouseUse::Unreadable;
        }
        if (top_down != 0 && camera_kind != 2) {
            return MouseUse::Ignored;
        }
    }
    return MouseUse::Turns;
}

/// The whole counts of a carried amount, bounded so that a hitch cannot overflow the game's int.
i32 TakeWhole(float* p_carry) {
    const float whole = *p_carry < -1048576.0f ? -1048576.0f : *p_carry > 1048576.0f ? 1048576.0f : *p_carry;
    const i32 counts = i32(whole);
    *p_carry -= float(counts);
    return counts;
}

void ForgetMotion() {
    s_pending.is_waiting = false;
    s_carry_x = s_carry_y = 0.0f;
}

/// Takes the counts still unread back out of the game, so that they do not turn the view all at
/// once when it reads again.
void TakeBack(const Slots& slots) {
    i32 sum = 0;
    if (!Add(slots.va_x, -s_pending.added_x, &sum) || !Add(slots.va_y, -s_pending.added_y, &sum)) {
        NoteFailure(s_c_take_back_failed, "unread counts could not be taken back", s_pending.added_x,
                    s_pending.added_y);
    }
    s_pending.is_waiting = false;
}

void Summary(char* sz_out, std::size_t cb_out) {
    const Layout* p_layout = s_p_layout.load(std::memory_order_acquire);
    std::snprintf(sz_out, cb_out,
                  "Mouse camera: %s: %llu samples fed to the game's mouse input (%llu as it flipped its slot), "
                  "%llu to the stick while the game ignores the mouse, %llu skipped (its pointers not valid), "
                  "%llu times handed to the right stick (not read for %lld ms), %llu take-backs failed, %llu "
                  "adds half done\n",
                  p_layout ? p_layout->sz_game : "?", (unsigned long long)s_c_fed.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_raced.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_ignored.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_invalid.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_handed_back.load(std::memory_order_relaxed), TakeTimeoutMs,
                  (unsigned long long)s_c_take_back_failed.load(std::memory_order_relaxed),
                  (unsigned long long)s_c_partial.load(std::memory_order_relaxed));
}

void Install(unsigned char* p_image, std::uint64_t cb_image, const Layout& layout) {
    if (const char* sz_env = std::getenv("PS_NATIVE_MOUSE"); sz_env && sz_env[0] == '0') {
        std::printf("Mouse camera: %s: the mouse drives the right stick (PS_NATIVE_MOUSE=0)\n", layout.sz_game);
        return;
    }
    const u64 off_data_end = std::max({layout.off_clients + 8, layout.off_client_count + 4, layout.off_frame_msec + 4,
                                       layout.off_top_down_dvar + 8,
                                       layout.is_in_client ? 0 : layout.off_index + 4});
    if (off_data_end > cb_image) {
        std::printf("Mouse camera: %s: not the known build (image of 0x%llx bytes ends before its mouse input): "
                    "the mouse drives the right stick\n",
                    layout.sz_game, (unsigned long long)cb_image);
        return;
    }
    for (const CodeCheck& check : layout.checks) {
        if (check.off_code + check.code.size() > cb_image ||
            std::memcmp(p_image + check.off_code, check.code.data(), check.code.size()) != 0) {
            std::printf("Mouse camera: %s: not the known build (the mouse reader's code at 0x%llx differs): the "
                        "mouse drives the right stick\n",
                        layout.sz_game, (unsigned long long)check.off_code);
            return;
        }
    }
    s_va_image = reinterpret_cast<u64>(p_image);
    s_p_layout.store(&layout, std::memory_order_release);
    runtime_calls_add_summary(Summary);
    std::printf("Mouse camera: %s: native (build checked, %zu code sites match): the mouse's counts go to the "
                "game's own mouse input (mouse_sensitivity 1: a count a pixel)\n",
                layout.sz_game, layout.checks.size());
}
} // namespace

void PatchBlackOps(unsigned char* p_image, std::uint64_t cb_image) {
    Install(p_image, cb_image, BlackOps);
}

void PatchBlackOps2(unsigned char* p_image, std::uint64_t cb_image) {
    Install(p_image, cb_image, BlackOps2);
}

bool Turn(float pitch, float yaw) {
    const Layout* p_layout = s_p_layout.load(std::memory_order_acquire);
    if (!p_layout) {
        return false;
    }
    Slots slots;
    if (!FindSlots(*p_layout, &slots)) {
        s_c_invalid.fetch_add(1, std::memory_order_relaxed);
        ForgetMotion();
        s_i_slot_seen = -1;
        return false;
    }
    // The game shows it reads the counts when idx moves or the slot no longer holds what the last
    // add left. Counts it leaves unread for TakeTimeoutMs after it last showed that (menus,
    // loading: CL_MouseMove does not run) are taken back and the stick takes the mouse, without
    // counts added meanwhile, until idx moves again.
    const long long now_ms = NowMs();
    bool is_reading = s_i_slot_seen >= 0 && slots.i_slot != s_i_slot_seen;
    s_i_slot_seen = slots.i_slot;
    if (s_pending.is_waiting) {
        i32 x = 0, y = 0;
        const bool is_unread = slots.i_slot == s_pending.i_slot && Get(slots.va_x, &x) && Get(slots.va_y, &y) &&
                               x == s_pending.left_x && y == s_pending.left_y;
        if (!is_unread) {
            is_reading = true;
            s_pending.is_waiting = false;
        }
    }
    if (is_reading) {
        s_read_seen_ms = now_ms;
        s_is_taking = true;
    } else if (s_pending.is_waiting && now_ms - std::max(s_read_seen_ms, s_pending.since_ms) > TakeTimeoutMs) {
        TakeBack(slots);
        s_is_taking = false;
        s_c_handed_back.fetch_add(1, std::memory_order_relaxed);
    }
    if (!s_is_taking) {
        s_carry_x = s_carry_y = 0.0f;
        return false;
    }
    switch (ReadMouseUse(*p_layout, slots.va_client)) {
    case MouseUse::Turns:
        break;
    case MouseUse::Ignored:
        s_c_ignored.fetch_add(1, std::memory_order_relaxed);
        s_carry_x = s_carry_y = 0.0f;
        return false;
    case MouseUse::Unreadable:
        s_c_invalid.fetch_add(1, std::memory_order_relaxed);
        s_carry_x = s_carry_y = 0.0f;
        return false;
    }
    s_carry_x += yaw / RadiansPerCount;
    s_carry_y += pitch / RadiansPerCount;
    const i32 counts_x = TakeWhole(&s_carry_x), counts_y = TakeWhole(&s_carry_y);
    if (counts_x == 0 && counts_y == 0) {
        return true;
    }
    i32 left_x = 0, left_y = 0;
    if (!Add(slots.va_x, counts_x, &left_x)) {
        s_c_invalid.fetch_add(1, std::memory_order_relaxed);
        ForgetMotion();
        return false;
    }
    if (!Add(slots.va_y, counts_y, &left_y)) {
        // The x counts are in the game: the camera turns by them, so the stick does not as well.
        NoteFailure(s_c_partial, "x counts added, y not", counts_x, counts_y);
        ForgetMotion();
        return true;
    }
    s_c_fed.fetch_add(1, std::memory_order_relaxed);
    if (i32 i_slot_after = slots.i_slot; Get(slots.va_index, &i_slot_after) && i_slot_after != slots.i_slot) {
        s_c_raced.fetch_add(1, std::memory_order_relaxed);
    }
    const bool was_waiting = s_pending.is_waiting;
    s_pending = {.is_waiting = left_x != 0 || left_y != 0,
                 .i_slot = slots.i_slot,
                 .left_x = left_x,
                 .left_y = left_y,
                 .added_x = (was_waiting ? s_pending.added_x : 0) + counts_x,
                 .added_y = (was_waiting ? s_pending.added_y : 0) + counts_y,
                 .since_ms = was_waiting ? s_pending.since_ms : now_ms};
    return true;
}
} // namespace PsBlackOpsMouse
