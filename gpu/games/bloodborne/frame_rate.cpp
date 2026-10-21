// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): its frame rate set free by hooks in its code (frame_rate.h).
//
// The game's main loop (0x2018d20) runs its frame pacer (0x2034770: it waits for the frame time
// its mode asks, 1/30 s, and measures the frame at [pacer + 0x264]) and then steps the game by a
// fixed 1/30 s (0x2018e36). The hooks keep the pacer's mode at one vblank and no frame time to
// wait for beyond the shortest frame, and step the game by the measured frame time (1/480 to
// 1/30 s: below 30 FPS the game slows down, as it always did).
//
// What the game ties to a 30 FPS frame besides (found by the frame time each function is called
// with and by what its code does with the constant): subsystems stepped by a fixed 1/30 s,
// per-frame steps and approaches, per-frame damping. Their instructions read the constant from a
// page next to the image instead (the instruction's own address operand moved; the game's code is
// otherwise as it was), where FrameStep writes the value for this frame's time: a step of 1/30 s
// becomes the frame's time, a per-frame factor f becomes f^(frames) for the frame's length in
// 30 FPS frames. Durations the game converts from 30 FPS frames into seconds stay as they are:
// what counts them down uses the real time.
#include "frame_rate.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <span>
#include "launch.h"
#include "psport_guest_code.h"

namespace PsFrameRate {
namespace {
using PsGuestCode::Emitter;
using PsGuestCode::Error;
using u64 = std::uint64_t;
using u8 = std::uint8_t;

std::atomic<bool> s_is_unlocked{false};
u64 s_va_image = 0;

constexpr float Frame30 = 1.0f / 30.0f;
/// The highest frame rate the pacer allows (uncap, and the most PS_FPS_LIMIT can ask for). The game
/// itself breaks above it: on loading screens at 880-1000 FPS its main loop outran its worker
/// threads and a pool allocator popped a freed node (guest fault at +0x263b8e7, 2 of 2 loads); at
/// 480 all loads went through (4 of 4, bbport's measurement).
constexpr int MaxRate = 480;
constexpr float ShortestFrame = 1.0f / MaxRate;

/// What the redirected instructions read, rewritten by FrameStep for each frame.
struct alignas(64) Live {
    float dt = Frame30;             ///< the frame's time: the game steps by it
    float shortest = ShortestFrame; ///< the pacer's frame time target
    float pad[2] = {};
    float decay95[4] = {0.95f, 0.95f, 0.95f, 0.95f}; ///< 0.95 per 30 FPS frame (read as four)
    float approach30 = Frame30;                      ///< a 1/30 approach per 30 FPS frame
    float approach10 = 0.1f;                         ///< a 0.1 approach per 30 FPS frame
    float step_down = -0.1f;                         ///< -0.1 per 30 FPS frame
    float step_up = 0.1f;                            ///< +0.1 per 30 FPS frame
    float decay08 = 0.8f;                            ///< 0.8 per 30 FPS frame
    float cloth = 1.0f;                              ///< cloth constraints' stiffness
};
Live* s_p_live = nullptr;

float EnvFloat(const char* sz_name, float fallback) {
    const char* sz_value = std::getenv(sz_name);
    return sz_value && *sz_value ? std::strtof(sz_value, nullptr) : fallback;
}

/// The pacer at the main loop's step (rdi: it): this frame's values.
void FrameStep(u64 va_pacer) {
    Live& live = *s_p_live;
    const float measured = *reinterpret_cast<const float*>(va_pacer + 0x264);
    float dt = std::clamp(std::isfinite(measured) ? measured : Frame30, live.shortest, Frame30);
    // PS_FRAME_DT_SMOOTH=a (diagnostics, 0 < a < 1): the step follows the measured frame time by
    // that fraction a frame, so frame-to-frame jitter of the measurement does not reach the
    // simulation (time stays right on average).
    static const float s_smooth = [] {
        const float a = EnvFloat("PS_FRAME_DT_SMOOTH", 0.0f);
        return a > 0.0f && a < 1.0f ? a : 0.0f;
    }();
    if (s_smooth > 0.0f) {
        static float s_smoothed = 0.0f;
        s_smoothed = s_smoothed > 0.0f ? s_smoothed + s_smooth * (dt - s_smoothed) : dt;
        dt = std::clamp(s_smoothed, live.shortest, Frame30);
    }
    const float frames = dt * 30.0f; // the frame's length in 30 FPS frames
    live.dt = dt;
    const float decay95 = std::pow(0.95f, frames);
    for (float& value : live.decay95) {
        value = decay95;
    }
    live.approach30 = 1.0f - std::pow(1.0f - Frame30, frames);
    live.approach10 = 1.0f - std::pow(0.9f, frames);
    live.step_down = -0.1f * frames;
    live.step_up = 0.1f * frames;
    live.decay08 = std::pow(0.8f, frames);
    // Cloth is stiffer the more often it is solved: the game's 1.0 at 30 FPS, 0.5 from 60 FPS up
    // (the value the community's 60 FPS patches settled on). It is the upper bound the cloth setups
    // clamp their Stiffness, Rigid Factor and Allowed Compression to when they load.
    // PS_CLOTH_STIFFNESS=value (diagnostics): that bound at any frame rate.
    static const float s_cloth_override = EnvFloat("PS_CLOTH_STIFFNESS", -1.0f);
    live.cloth = s_cloth_override > 0.0f ? s_cloth_override : std::clamp(frames, 0.5f, 1.0f);
}

/// An instruction that reads a constant RIP-relative, the constant, and the value it reads
/// instead (offsetof(Live, ...)).
struct Redirect {
    u64 off_site;
    u64 off_constant;
    std::size_t off_value;
};
#define LIVE(field) offsetof(Live, field)
constexpr Redirect Redirects[] = {
    // A fixed 1/30 s step or "one frame" (the patches' lists, checked one by one):
    {0xab9f0d, 0x47efdcc, LIVE(dt)}, // a time step, one frame when unset
    {0xdca3ed, 0x4888688, LIVE(dt)}, // a per-frame budget
    {0xee596a, 0x48ad65c, LIVE(dt)}, // effects: a fade of at least one frame (three releases)
    {0xee5a8a, 0x48ad660, LIVE(dt)},
    {0xee5baa, 0x48ad664, LIVE(dt)},
    {0xef06f1, 0x48adc08, LIVE(dt)}, // at least one frame
    {0xef6992, 0x48add28, LIVE(dt)},
    {0x1044943, 0x48cc3f4, LIVE(dt)}, // an update by one frame
    {0x12a4416, 0x492577c, LIVE(dt)}, // a timer advanced by one frame each frame
    {0x15dc1f7, 0x4926c9c, LIVE(dt)},
    {0x173456e, 0x49270e4, LIVE(dt)}, // an update by one frame
    {0x17f9d26, 0x4927554, LIVE(dt)},
    {0x180f647, 0x49275f4, LIVE(dt)}, // a timer advanced by one frame each frame
    {0x18bc4f7, 0x4927c4c, LIVE(dt)}, // an update by one frame (twice)
    {0x18bc50d, 0x4927c4c, LIVE(dt)},
    {0x1dbc308, 0x4928e98, LIVE(dt)}, // frames counted at one frame's time
    {0x1f77d4a, 0x4929170, LIVE(dt)}, // an update by one frame (0 after a reset)
    {0x227784b, 0x4929b80, LIVE(dt)}, // timers advanced by one frame (0xebbda0)
    {0x2277892, 0x4929b80, LIVE(dt)},
    {0x2313dad, 0x4929e70, LIVE(dt)}, // a simulation step: 1/60 in the game's 60 Hz mode,
    {0x2313db7, 0x4929e74, LIVE(dt)}, // 1/30 otherwise
    {0x2315d71, 0x4929e74, LIVE(dt)},
    // Body parts the physics moves (0x1515ec0, each part every frame): their timers advanced by the
    // step they are given (0x180df90 at +0xf0, 0x1811650 at +0xd0), 1/30 s; not in the patches.
    {0x1515fab, 0x4926364, LIVE(dt)},
    {0x1516030, 0x4926364, LIVE(dt)},
    // Steps and approaches per frame:
    {0x19084f5, 0x4927fd4, LIVE(step_down)}, // a value ramped by 0.1 a frame
    {0x1908522, 0x4927fd4, LIVE(step_down)},
    {0x1908518, 0x4927fe0, LIVE(step_up)},
    {0x190854c, 0x4927fe0, LIVE(step_up)},
    {0x143e2b2, 0x4925e94, LIVE(approach30)}, // the follow camera's approaches
    {0x143af8c, 0x4925e68, LIVE(approach10)},
    // Damping per frame:
    {0x1a4abca, 0x490b5b0, LIVE(decay95)}, // a character's motion (0x1a4a730, x0.95 a frame)
    {0x1514c4d, 0x4926334, LIVE(decay08)}, // the run's wall check (x0.8 a frame while blocked)
    // Cloth constraints' stiffness:
    {0x47c6dc, 0x47693f0, LIVE(cloth)},
    {0x7e45be, 0x47b63e4, LIVE(cloth)},
    {0x7e4642, 0x47b63e4, LIVE(cloth)},
    {0x7e6239, 0x47b63e4, LIVE(cloth)},
    {0x7f706d, 0x47b8474, LIVE(cloth)},
    {0x7f70ad, 0x47b8474, LIVE(cloth)},
    {0x7f79e5, 0x47b8474, LIVE(cloth)},
    {0x7f7be8, 0x47b8474, LIVE(cloth)},
    {0x7f7dbd, 0x47b8474, LIVE(cloth)},
    {0x805898, 0x47b91c0, LIVE(cloth)},
    {0x805d18, 0x47b91c0, LIVE(cloth)},
    {0x806269, 0x47b91c0, LIVE(cloth)},
    {0x8062f0, 0x47b91c0, LIVE(cloth)},
    {0x809d62, 0x47b98b8, LIVE(cloth)},
    {0x80a4c9, 0x47b98b8, LIVE(cloth)},
    {0x80a876, 0x47b98b8, LIVE(cloth)},
};
#undef LIVE

constexpr u64 OffPacerMode = 0x203493d;
constexpr u8 PacerModeCode[] = {0x4d, 0x8d, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00};
/// The pacer's mode, after the game chose it (0x203493d, `lea r15, [r12 + 0x268]`; r12 the
/// pacer): one vblank (+0x10), no flip skipping (+0x14), the shortest frame as the frame time
/// target (+0x18), no vblank counts to wait for (+0x268, +0x26c).
bool HookPacerMode() {
    Emitter e;
    e.Bytes({0x41, 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00}); // mov dword [r12 + 0x10], 1
    e.Bytes({0x41, 0xc6, 0x44, 0x24, 0x14, 0x00});                   // mov byte [r12 + 0x14], 0
    e.Bytes({0x50, 0x48, 0xb8});                                     // push rax; movabs rax, &shortest
    e.U64(reinterpret_cast<u64>(&s_p_live->shortest));
    e.Bytes({0x8b, 0x00});                         // mov eax, [rax]
    e.Bytes({0x41, 0x89, 0x44, 0x24, 0x18, 0x58}); // mov [r12 + 0x18], eax; pop rax
    e.Bytes({0x49, 0xc7, 0x84, 0x24, 0x68, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov qword [r12 + 0x268], 0
    e.Copy(PacerModeCode);
    e.JmpAbs(s_va_image + OffPacerMode + sizeof(PacerModeCode));
    return PsGuestCode::InstallDetour("Bloodborne frame pacer mode", s_va_image + OffPacerMode, PacerModeCode, e) ==
           Error::None;
}

constexpr u64 OffPacerLate = 0x2034d96;
constexpr u8 PacerLateCode[] = {0x84, 0xd2, 0x74, 0x05, 0x41, 0x8b, 0x44, 0x24, 0x10};
/// The pacer keeps its one vblank when a frame came late (0x2034d96: `test dl, dl; je; mov eax,
/// [r12 + 0x10]`, then eax is stored back: 0 when late).
bool HookPacerLate() {
    Emitter e;
    e.Bytes({0x84, 0xd2});                   // test dl, dl
    e.Bytes({0x41, 0x8b, 0x44, 0x24, 0x10}); // mov eax, [r12 + 0x10]
    e.JmpAbs(s_va_image + OffPacerLate + sizeof(PacerLateCode));
    return PsGuestCode::InstallDetour("Bloodborne frame pacer late frame", s_va_image + OffPacerLate, PacerLateCode,
                                      e) == Error::None;
}

constexpr u64 OffStep = 0x2018e36;
constexpr u8 StepCode[] = {0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d};
/// The main loop's step (0x2018e36: `mov dword [rbp - 0x38], 1/30`, its time; rbx the pacer):
/// FrameStep, then the frame's time.
bool HookStep() {
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx
    e.CallAbs(reinterpret_cast<const void*>(&FrameStep));
    e.RestoreVolatile();
    e.StoreToFrame(&s_p_live->dt, -0x38);
    e.JmpAbs(s_va_image + OffStep + sizeof(StepCode));
    return PsGuestCode::InstallDetour("Bloodborne main loop step", s_va_image + OffStep, StepCode, e) == Error::None;
}

/// The core's sites checked before any of them changes, so that a mismatch leaves the game's own
/// pacer whole instead of half hooked.
bool IsCoreIntact() {
    struct Site {
        u64 off_site;
        std::span<const u8> code;
    };
    const Site sites[] = {{OffPacerMode, PacerModeCode}, {OffPacerLate, PacerLateCode}, {OffStep, StepCode}};
    bool is_intact = true;
    for (const Site& site : sites) {
        if (std::memcmp(reinterpret_cast<const u8*>(s_va_image + site.off_site), site.code.data(), site.code.size())) {
            std::printf("Frame rate: the game's code at +0x%llx differs (another build of the game, or a patch "
                        "there)\n",
                        static_cast<unsigned long long>(site.off_site));
            is_intact = false;
        }
    }
    return is_intact;
}

/// A subsystem stepped by a fixed 1/30 s (`mov dword [rbp + off_frame], 1/30` at off_site): by
/// the frame's time.
bool HookFixedStep(const char* sz_name, u64 off_site, std::int8_t off_frame) {
    const u8 code[] = {0xc7, 0x45, u8(off_frame), 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.StoreToFrame(&s_p_live->dt, off_frame);
    e.JmpAbs(s_va_image + off_site + sizeof(code));
    return PsGuestCode::InstallDetour(sz_name, s_va_image + off_site, code, e) == Error::None;
}

enum class RunCheck { Unchanged, Hooked, SprintFixPatch };

/// The run's wall check (0x1514c24: the distance moved this frame x 30 = a speed at 30 FPS): the
/// distance divided by the frame's time ([rbp - 0x74], the character update's), a speed at any
/// frame rate. Above ~90 FPS the per-frame distance fell under the check's threshold and the run
/// dropped to half speed.
RunCheck HookRunSpeed() {
    constexpr u64 OffSite = 0x1514c24;
    const u8* p_code = reinterpret_cast<const u8*>(s_va_image + OffSite);
    constexpr u8 SprintFix[] = {0xc5, 0xfa, 0x5e, 0x45, 0x8c, 0x0f, 0x1f, 0x00}; // vdivss; nop
    if (std::memcmp(p_code, SprintFix, sizeof(SprintFix)) == 0) {
        return RunCheck::SprintFixPatch;
    }
    // vmulss xmm0, xmm0, [rip + disp] reading the game's 30.0 at 0x492632c.
    std::int32_t disp = 0;
    std::memcpy(&disp, p_code + 4, 4);
    const u8 code[] = {0xc5, 0xfa, 0x59, 0x05, p_code[4], p_code[5], p_code[6], p_code[7]};
    if (s_va_image + OffSite + sizeof(code) + std::int64_t(disp) != s_va_image + 0x492632c) {
        std::printf("Frame rate: the run's wall check is not the expected code, left as it is\n");
        return RunCheck::Unchanged;
    }
    Emitter e;
    e.Bytes({0xc5, 0xfa, 0x5e, 0x45, 0x8c}); // vdivss xmm0, xmm0, [rbp - 0x74]
    e.JmpAbs(s_va_image + OffSite + sizeof(code));
    return PsGuestCode::InstallDetour("Bloodborne run wall check", s_va_image + OffSite, code, e) == Error::None
               ? RunCheck::Hooked
               : RunCheck::Unchanged;
}
} // namespace

void PatchImage(unsigned char* p_image, std::uint64_t) {
    const int rate_preset = PsLaunch::FrameHooksChosen();
    if (rate_preset <= 30) {
        return;
    }
    s_va_image = reinterpret_cast<u64>(p_image);
    void* p_live_page = PsGuestCode::MapNear(s_va_image, sizeof(Live));
    if (!p_live_page) {
        std::printf("ERROR: Frame rate: no memory next to the game's code: 30 FPS (PS_FRAME_HOOKS=0: the patches)\n");
        return;
    }
    s_p_live = new (p_live_page) Live{};
    // The preset's rate and PS_FPS_LIMIT (the launcher's frame rate limit): the game's pacer waits
    // for it itself, before the frame's simulation (lower latency than holding finished frames back
    // at the present).
    int rate = std::clamp(rate_preset, 30, MaxRate);
    if (const char* sz_limit = std::getenv("PS_FPS_LIMIT"); sz_limit && std::atoi(sz_limit) > 0) {
        rate = std::min(rate, std::clamp(std::atoi(sz_limit), 30, MaxRate));
    }
    s_p_live->shortest = 1.0f / float(rate);
    // The core first: without all three the game keeps its own frame rate.
    if (!(IsCoreIntact() && HookPacerMode() && HookPacerLate() && HookStep())) {
        std::printf("ERROR: Frame rate: the game's frame pacer is not the expected code: 30 FPS "
                    "(PS_FRAME_HOOKS=0: the patches)\n");
        return;
    }
    s_is_unlocked.store(true, std::memory_order_release);
    const int c_steps = HookFixedStep("Bloodborne fixed step 0x17f9b71", 0x17f9b71, -0x48) +
                        HookFixedStep("Bloodborne fixed step 0x1572a7d", 0x1572a7d, -0x30);
    const RunCheck run = HookRunSpeed();
    int c_redirected = 0;
    for (const Redirect& redirect : Redirects) {
        c_redirected += PsGuestCode::RedirectOperand("Bloodborne per-frame value", s_va_image + redirect.off_site,
                                                     s_va_image + redirect.off_constant,
                                                     reinterpret_cast<const u8*>(s_p_live) + redirect.off_value) ==
                        Error::None;
    }
    std::printf("Frame rate: free (hooks, no patches; PS_FRAME_HOOKS=0: the patches): the game steps by each "
                "frame's time (1/%d..1/30 s); %d of 2 fixed steps, %d of %zu per-frame values follow the frame's "
                "time, the run's wall check %s\n",
                rate, c_steps, c_redirected, std::size(Redirects),
                run == RunCheck::Hooked           ? "by speed"
                : run == RunCheck::SprintFixPatch ? "by speed (a Sprint Fix patch did it already)"
                                                  : "unchanged (not found)");
}

bool IsUnlocked() {
    return s_is_unlocked.load(std::memory_order_acquire);
}
} // namespace PsFrameRate
