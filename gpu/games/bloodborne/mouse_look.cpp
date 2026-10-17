// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): native mouse look (mouse_look.h).
//
// The follow camera's update (0x143ac60, rdi the camera) derives its angles from where the camera
// is around the player each frame, pitch at +0x140 and yaw at +0x144; in its normal mode
// 0x143cde0..0x143cdf0 makes xmm1 = camera stick * turn speed * frame time, the increment
// 0x143cdf0 adds to (pitch, yaw) before the game wraps the yaw and clamps the pitch, and the
// camera is then placed from them. The detour adds the mouse's turn since the last update
// (radians, taken and cleared atomically) to that increment.
#include "mouse_look.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "psport_guest_code.h"

namespace PsMouseLook {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;

constexpr u64 OffLookSite = 0x143cde0;
constexpr u8 LookSiteCode[16] = {0xc5, 0xa8, 0x59, 0xc9,                         // vmulps xmm1, xmm10, xmm1
                                 0xc5, 0xf8, 0x28, 0x95, 0x10, 0xfc, 0xff, 0xff, // vmovaps xmm2, [rbp - 0x3f0]
                                 0xc5, 0xe8, 0x59, 0xc9};                        // vmulps xmm1, xmm2, xmm1

/// (pitch, yaw) as two floats, taken by the detour.
alignas(8) std::atomic<u64> s_look_delta{0};
std::atomic<bool> s_is_installed{false};
/// When s_look_delta last went from empty to not.
std::atomic<long long> s_look_since_ms{0};
std::atomic<bool> s_is_taking{true}, s_has_stored{false};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void PatchImage(unsigned char* p_image, std::uint64_t) {
    if (const char* sz_env = std::getenv("PS_NATIVE_MOUSE"); sz_env && sz_env[0] == '0') {
        std::printf("Mouse look: the mouse drives the right stick (PS_NATIVE_MOUSE=0)\n");
        return;
    }
    const u64 va_site = reinterpret_cast<u64>(p_image) + OffLookSite;
    PsGuestCode::Emitter e;
    e.Copy(LookSiteCode);   // the three instructions the jump covers (none RIP-relative)
    e.Bytes({0x50, 0x51});  // push rax; push rcx
    e.Bytes({0x48, 0xb8});  // movabs rax, &s_look_delta
    e.U64(reinterpret_cast<u64>(&s_look_delta));
    e.Bytes({0x31, 0xc9});       // xor ecx, ecx
    e.Bytes({0x48, 0x87, 0x08}); // xchg [rax], rcx: taken and cleared
    // A turn counts as camera input, as the stick does (0x143b8cc): else the update pulls the pitch
    // back to its rest angle (+0x150) at once, and the rest angle follows the new pitch.
    e.Bytes({0x48, 0x85, 0xc9});                                                 // test rcx, rcx
    e.Bytes({0x74, 0x1d});                                                       // je past the three stores
    e.Bytes({0xc7, 0x85, 0x3c, 0xfc, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00});       // mov dword [rbp - 0x3c4], 1
    e.Bytes({0x41, 0xc6, 0x85, 0x5c, 0x01, 0x00, 0x00, 0x01});                   // mov byte [r13 + 0x15c], 1
    e.Bytes({0x41, 0xc7, 0x85, 0x54, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov dword [r13 + 0x154], 0
    e.Bytes({0xc4, 0xe1, 0xf9, 0x6e, 0xd1}); // vmovq xmm2, rcx (xmm2 is dead here)
    e.Bytes({0xc5, 0xf0, 0x58, 0xca});       // vaddps xmm1, xmm1, xmm2
    e.Bytes({0x59, 0x58});                   // pop rcx; pop rax
    e.JmpAbs(va_site + sizeof(LookSiteCode));
    const bool is_installed =
        PsGuestCode::InstallDetour("Bloodborne camera turn", va_site, LookSiteCode, e) == PsGuestCode::Error::None;
    s_is_installed.store(is_installed, std::memory_order_release);
    std::printf("Mouse look: %s\n", is_installed ? "native (the mouse turns the game's camera itself)"
                                                 : "not available (the mouse drives the right stick)");
}

bool Turn(float pitch, float yaw) {
    if (!s_is_installed.load(std::memory_order_acquire)) {
        return false;
    }
    // Whether the camera takes the turns: a turn it has not taken for 100 ms (lock-on, menus,
    // cutscenes: its normal update does not run) means not, until it takes one again. Meanwhile the
    // caller drives the stick (lock-on switches targets with the stick) and the turn waiting here is
    // replaced rather than applied all at once later.
    const long long now_ms = NowMs();
    u64 old = s_look_delta.load(std::memory_order_acquire);
    if (s_has_stored.load(std::memory_order_relaxed)) {
        if (old == 0) {
            s_is_taking.store(true, std::memory_order_relaxed);
        } else if (now_ms - s_look_since_ms.load(std::memory_order_relaxed) > 100) {
            s_is_taking.store(false, std::memory_order_relaxed);
        }
    }
    const bool is_taking = s_is_taking.load(std::memory_order_relaxed);
    if (pitch == 0.0f && yaw == 0.0f) {
        return is_taking;
    }
    for (;;) {
        float pending_pitch = 0.0f, pending_yaw = 0.0f;
        if (old != 0 && is_taking) {
            const std::uint32_t lo = std::uint32_t(old), hi = std::uint32_t(old >> 32);
            std::memcpy(&pending_pitch, &lo, 4);
            std::memcpy(&pending_yaw, &hi, 4);
        }
        pending_pitch += pitch;
        pending_yaw += yaw;
        std::uint32_t lo, hi;
        std::memcpy(&lo, &pending_pitch, 4);
        std::memcpy(&hi, &pending_yaw, 4);
        const u64 next = u64(lo) | (u64(hi) << 32);
        if (s_look_delta.compare_exchange_weak(old, next, std::memory_order_acq_rel)) {
            if (old == 0 || !is_taking) {
                s_look_since_ms.store(now_ms, std::memory_order_relaxed);
            }
            s_has_stored.store(true, std::memory_order_relaxed);
            return is_taking;
        }
    }
}
} // namespace PsMouseLook
