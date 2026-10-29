// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (1.09): the WORKAROUND quirks of workarounds.h, from bbport 0.6 (8185b88,
// gpu/shim/bbport_guest_hooks.cpp), their sites confirmed in the executable (kb note #682).
#include "workarounds.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include "psport_guest_code.h"
#include "psport_quirks.h"

extern "C" {
int runtime_memory_is_mapped(uintptr_t va_range, uint64_t cb_range);
void runtime_memory_note_write(uintptr_t va_range, uint64_t cb_range);
}

namespace PsWorkarounds {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using PsGuestCode::Emitter;
using PsGuestCode::Error;

u64 s_va_image = 0;

/// Whether every site still holds the code expected, checked before any is written so that a
/// workaround goes in whole or not at all.
bool HasCode(u64 off_site, std::span<const u8> code) {
    return std::memcmp(reinterpret_cast<const void*>(s_va_image + off_site), code.data(), code.size()) == 0;
}

struct Counter {
    std::atomic<u64> c_events{0};
    u64 c_reported = 0;
};

/// The first few events and then every power of two, so a workaround that acts is seen at once
/// without a line per event.
bool ShouldLog(u64 c_events) {
    return c_events <= 3 || (c_events & (c_events - 1)) == 0;
}

/// A stub that calls a host function mid-function: the red zone skipped, the flags, the
/// caller-saved general registers and xmm0..15 kept around the call.
void EmitGuardedCall(Emitter& e, std::initializer_list<u8> arguments, const void* p_function) {
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
    e.Bytes({0x9c});                                           // pushfq
    e.SaveVolatile();
    e.SaveXmm();
    e.Bytes(arguments);
    e.CallAbs(p_function);
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0x9d});                                           // popfq
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
}

/// The render job pool (bloodborne_render_job_slots, #683): six slots, taken by the job runner's
/// step 0x1067f60 and given back by 0x1056990 (pool, slot). The step's two failure branches
/// (0x1068001, 0x106803f) leave through its common exit 0x1068092 returning 1 with the slot still
/// held at [runner + 0x120], and the runner's destructor (0x106798a) and reset (0x1067a5c) clear
/// that field without giving it back: after six such jobs every thread that asks for a slot sleeps
/// for good (Laurence's cinematic above 30 FPS). The slot now goes back first.
constexpr u64 OffGiveSlotBack = 0x1056990;
/// The pool's global (what 0x1057350 returns).
constexpr u64 OffJobPool = 0x54b8788;
constexpr u64 OffStepExit = 0x1068092;
constexpr u8 StepExitCode[7] = {0x48, 0x8b, 0x03,        // mov rax, [rbx]
                                0x48, 0x3b, 0x45, 0xd0}; // cmp rax, [rbp - 0x30]
constexpr u64 OffRunnerDestroy = 0x106798a, OffRunnerReset = 0x1067a5c;
constexpr u8 RunnerClearCode[11] = {0x48, 0xc7, 0x83, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr u64 OffHeldSlot = 0x120;
using PfnGiveSlotBack = void(PS4_SYSV_ABI*)(u64 va_pool, u64 va_slot);
Counter s_slots_after_failure, s_slots_after_clear;

void GiveSlotBack(u64 va_runner, u64 va_pool, Counter& counter, const char* sz_when) {
    auto* p_va_slot = reinterpret_cast<u64*>(va_runner + OffHeldSlot);
    if (*p_va_slot == 0 || va_pool == 0) {
        return;
    }
    reinterpret_cast<PfnGiveSlotBack>(s_va_image + OffGiveSlotBack)(va_pool, *p_va_slot);
    *p_va_slot = 0;
    if (const u64 c_events = counter.c_events.fetch_add(1, std::memory_order_relaxed) + 1; ShouldLog(c_events)) {
        std::printf("Workarounds: a render job slot given back %s (%llu so far; #683)\n", sz_when,
                    static_cast<unsigned long long>(c_events));
    }
}

/// r14 the runner, the pool in the step's r12 (saved at [r12] by SaveVolatile), r15d the result:
/// 0 only for a full success, and the step's early exits come before a slot is taken (kb note
/// #696), so any other result with a slot held is a failed step's.
void PS4_SYSV_ABI OnStepExit(u64 va_runner, u64 va_pool, u32 result) {
    if (result != 0) {
        GiveSlotBack(va_runner, va_pool, s_slots_after_failure, "after a failed job step");
    }
}

void PS4_SYSV_ABI OnRunnerClear(u64 va_runner) {
    GiveSlotBack(va_runner, *reinterpret_cast<const u64*>(s_va_image + OffJobPool), s_slots_after_clear,
                 "by a runner destroyed or reset while holding it");
}

bool InstallJobSlots() {
    if (!HasCode(OffStepExit, StepExitCode) || !HasCode(OffRunnerDestroy, RunnerClearCode) ||
        !HasCode(OffRunnerReset, RunnerClearCode)) {
        return false;
    }
    Emitter exit;
    EmitGuardedCall(exit,
                    {0x4c, 0x89, 0xf7,        // mov rdi, r14
                     0x49, 0x8b, 0x34, 0x24,  // mov rsi, [r12]
                     0x44, 0x89, 0xfa},       // mov edx, r15d
                    reinterpret_cast<const void*>(&OnStepExit));
    exit.Copy(StepExitCode);
    exit.JmpAbs(s_va_image + OffStepExit + sizeof(StepExitCode));
    Emitter clear;
    EmitGuardedCall(clear, {0x48, 0x89, 0xdf}, reinterpret_cast<const void*>(&OnRunnerClear)); // mov rdi, rbx
    clear.Copy(RunnerClearCode);
    // The clear detours first: with the step's exit in and a clear refused, a slot dropped there
    // would still be lost; with a clear in and the exit refused, nothing changes for failed steps.
    int c_installed = 0;
    for (const u64 off_site : {OffRunnerDestroy, OffRunnerReset}) {
        Emitter site = clear;
        site.JmpAbs(s_va_image + off_site + sizeof(RunnerClearCode));
        c_installed += PsGuestCode::InstallDetour("Bloodborne render job runner clear", s_va_image + off_site,
                                                  RunnerClearCode, site) == Error::None;
    }
    c_installed += c_installed == 2 && PsGuestCode::InstallDetour("Bloodborne render job step exit",
                                                                   s_va_image + OffStepExit, StepExitCode, exit) ==
                                           Error::None;
    if (c_installed != 3) {
        std::printf("Workarounds: render job slots partially installed (%d of 3 sites)\n", c_installed);
    }
    return c_installed == 3;
}

/// Yebis's GPUTexUtil_GetRecursiveSampleParameters (bloodborne_blur_sample_clamp, #684): its
/// assertion nBaseSamples >= 3 (GPUMath.cpp:3199) is compiled into the retail build (0xbbc40b),
/// and game data under psport now and then passes 2: _Assert raises 0xa0020007 and the game stops.
/// The count is raised to 3 at the entry; a console would abort on a 2 as well, so the 2 itself is
/// what #684 looks for.
constexpr u64 OffSampleParameters = 0xbbc3e0;
constexpr u8 SampleParametersEntry[6] = {0x55,             // push rbp
                                         0x48, 0x89, 0xe5, // mov rbp, rsp
                                         0x41, 0x57};      // push r15
Counter s_blur_clamps;

void PS4_SYSV_ABI OnBlurClamp() {
    if (const u64 c_events = s_blur_clamps.c_events.fetch_add(1, std::memory_order_relaxed) + 1; ShouldLog(c_events)) {
        std::printf("Workarounds: Yebis blur kernel asked for 2 base samples: 3 given (%llu so far; #684)\n",
                    static_cast<unsigned long long>(c_events));
    }
}

bool InstallBlurClamp() {
    if (!HasCode(OffSampleParameters, SampleParametersEntry)) {
        return false;
    }
    const u64 va_site = s_va_image + OffSampleParameters;
    Emitter e;
    e.Bytes({0x83, 0xff, 0x03}); // cmp edi, 3
    e.Bytes({0x0f, 0x8d}); // jge past the clamp
    const std::size_t off_rel = e.m_cb_code;
    e.U32(0);
    // At the entry the arguments are live in the general registers and xmm0..15, the flags dead
    // and nothing of the function below the return address yet.
    e.SaveVolatile();
    e.SaveXmm();
    e.CallAbs(reinterpret_cast<const void*>(&OnBlurClamp));
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0xbf, 0x03, 0x00, 0x00, 0x00}); // mov edi, 3
    const u32 rel = u32(e.m_cb_code - (off_rel + 4));
    std::memcpy(&e.m_code[off_rel], &rel, sizeof(rel));
    e.Copy(SampleParametersEntry);
    e.JmpAbs(va_site + sizeof(SampleParametersEntry));
    return PsGuestCode::InstallDetour("Bloodborne blur kernel sample count", va_site, SampleParametersEntry, e) ==
           Error::None;
}

/// The FX init 0xebb960 (bloodborne_fx_init_clear, #685) clears three 2x2 RG16F history targets
/// through the graphics context's clear 0x2168630(context, context + 0xb730, target, color) to
/// (0, 0, 0, 1); at a save's load those clears' packets came out as bad PM4 (opcode 23). Without
/// FAST_CLEAR that clear has a compute shader write the colour into the surface (kb note #696), so
/// the same colour is written here in the target's format, a uniform texel that reads the same in
/// every tiling, and no packet is written. A target whose format is not known for certain, a
/// fast-cleared one (its CMASK cleared instead) or a colour RG16F cannot hold exactly goes to the
/// game's clear.
constexpr u64 OffGraphicsClear = 0x2168630;
struct CallSite {
    u64 off_code;
    u8 code[5];
};
constexpr std::array<CallSite, 3> FxInitClearSites = {{
    {0xebbc86, {0xe8, 0xa5, 0xc9, 0x2a, 0x01}},
    {0xebbca4, {0xe8, 0x87, 0xc9, 0x2a, 0x01}},
    {0xebbcc2, {0xe8, 0x69, 0xc9, 0x2a, 0x01}},
}};
using PfnGraphicsClear = void(PS4_SYSV_ABI*)(void* p_context, void* p_state, const u8* p_target, const float* p_color);
Counter s_fx_written, s_fx_native;

struct ColorClear {
    u64 va_target;
    u32 cb_target;
    u32 texel;
    /// Why the game's clear runs instead, or null.
    const char* sz_refusal;
};

/// A half float holding value exactly (zero or a normal half), else nothing.
std::optional<std::uint16_t> ExactHalf(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = bits >> 31, exponent = (bits >> 23) & 0xff, mantissa = bits & 0x7fffff;
    if (exponent == 0 && mantissa == 0) {
        return std::uint16_t(sign << 15);
    }
    const int half_exponent = int(exponent) - 127 + 15;
    if (exponent == 0xff || half_exponent < 1 || half_exponent > 30 || (mantissa & 0x1fff) != 0) {
        return std::nullopt;
    }
    return std::uint16_t(sign << 15 | u32(half_exponent) << 10 | mantissa >> 13);
}

/// The target's descriptor from +0x10 (CB_COLOR*: base, pitch, slice, view, info, attrib, ...).
ColorClear PlanColorClear(const u8* p_target, const float* p_color) {
    ColorClear clear{};
    if (!p_target || !p_color) {
        clear.sz_refusal = "no target or colour";
        return clear;
    }
    std::array<u32, 11> target;
    std::memcpy(target.data(), p_target + 0x10, sizeof(target));
    if (target[0] == 0 || target[3] != 0 || target[6] != 0x00020002 || target[7] || target[8] || target[9] ||
        target[10]) {
        clear.sz_refusal = "not a 2x2 single-slice target";
        return clear;
    }
    const u32 info = target[4], attrib = target[5];
    if (info & (1u << 13)) {
        clear.sz_refusal = "fast-cleared target (CMASK)";
        return clear;
    }
    // COLOR_16_16 FLOAT, the standard component order (red, green: no alpha stored), no
    // compression, CMASK or Neo mode.
    if (((info >> 2) & 31) != 5 || ((info >> 8) & 7) != 7 ||
        (info & (3u | (1u << 7) | (3u << 11) | (1u << 14) | (1u << 28) | (1u << 31))) || (attrib & 31) != 14 ||
        (attrib & (31u << 12))) {
        clear.sz_refusal = "not an uncompressed RG16F target";
        return clear;
    }
    const auto red = ExactHalf(p_color[0]), green = ExactHalf(p_color[1]);
    if (!red || !green) {
        clear.sz_refusal = "a colour RG16F does not hold exactly";
        return clear;
    }
    const u64 c_pitch = ((target[1] & 0x7ff) + 1) * 8;
    const u64 c_texels = ((target[2] & 0x3fffff) + 1) * 64;
    const u64 cb_target = c_texels * 4;
    const u64 va_target = u64(target[0]) << 8;
    if (c_pitch < 2 || c_texels / c_pitch < 2 || c_texels % c_pitch || cb_target > 32768 ||
        va_target + cb_target > (1ull << 40) || !runtime_memory_is_mapped(va_target, cb_target)) {
        clear.sz_refusal = "surface size or address out of range";
        return clear;
    }
    return ColorClear{va_target, u32(cb_target), u32(*red) | u32(*green) << 16, nullptr};
}

void PS4_SYSV_ABI FxInitClear(void* p_context, void* p_state, const u8* p_target, const float* p_color) {
    const ColorClear clear = PlanColorClear(p_target, p_color);
    if (!clear.sz_refusal) {
        // Told before (its write traps lift) and after (the colour now there), as a resource copy.
        runtime_memory_note_write(clear.va_target, clear.cb_target);
        auto* p_texels = reinterpret_cast<u32*>(clear.va_target);
        for (u32 i = 0; i < clear.cb_target / 4; ++i) {
            p_texels[i] = clear.texel;
        }
        runtime_memory_note_write(clear.va_target, clear.cb_target);
        if (const u64 c_events = s_fx_written.c_events.fetch_add(1, std::memory_order_relaxed) + 1;
            ShouldLog(c_events)) {
            std::printf("Workarounds: FX init target %#llx (%u bytes) cleared in memory to texel %#010x without the "
                        "game's clear (%llu so far; #685)\n",
                        static_cast<unsigned long long>(clear.va_target), clear.cb_target, clear.texel,
                        static_cast<unsigned long long>(c_events));
        }
        return;
    }
    if (const u64 c_events = s_fx_native.c_events.fetch_add(1, std::memory_order_relaxed) + 1; ShouldLog(c_events)) {
        std::printf("Workarounds: FX init clear left to the game: %s (%llu so far; #685)\n", clear.sz_refusal,
                    static_cast<unsigned long long>(c_events));
    }
    reinterpret_cast<PfnGraphicsClear>(s_va_image + OffGraphicsClear)(p_context, p_state, p_target, p_color);
}

bool InstallFxInitClear() {
    for (const CallSite& site : FxInitClearSites) {
        if (!HasCode(site.off_code, site.code)) {
            return false;
        }
    }
    int c_installed = 0;
    for (const CallSite& site : FxInitClearSites) {
        const u64 va_site = s_va_image + site.off_code;
        Emitter e;
        e.CallAbs(reinterpret_cast<const void*>(&FxInitClear)); // rax is free at a call
        e.JmpAbs(va_site + sizeof(site.code));
        c_installed += PsGuestCode::InstallDetour("Bloodborne FX init clear", va_site, site.code, e) == Error::None;
    }
    // Each site stands alone (a site left as it is keeps the game's clear), so a refusal after the
    // check is only said.
    if (c_installed != int(FxInitClearSites.size())) {
        std::printf("Workarounds: FX init clears partially installed (%d of %zu sites)\n", c_installed,
                    FxInitClearSites.size());
    }
    return c_installed == int(FxInitClearSites.size());
}

/// The online login's character registration step 0x1c43ee0 (bloodborne_server_chara_id, #817)
/// asks the game server for an id (sync_chara_id) only while the character's ServerChrId
/// ([PlayerGameData + 0x690]) is 0, and its reply callback 0x1c440a0 stores the reply's id
/// unchecked. A server answering an empty PublishCharacterIdList leaves the reply parser's
/// 0x8000000000000000 there, which the game then takes for a valid id. That value alone is cleared
/// where the step reads it, so the character registers again; 0 already means no id and any other
/// value is one a server gave. Kept with the character in its save (inferred: the sentinel outlives
/// a session, kb #815), which is why the guard rather than the server fix recovers those saves, and
/// a server that still answers an empty list (The Hunter's Dream's reply is not known) only gets the
/// sentinel cleared again at the next login.
constexpr u64 OffServerChrIdRead = 0x1c43f0e;
constexpr u8 ServerChrIdReadCode[10] = {0x48, 0x8b, 0x80, 0x90, 0x06, 0x00, 0x00, // mov rax, [rax + 0x690]
                                        0x48, 0x85, 0xc0};                         // test rax, rax
constexpr u64 ServerChrIdSentinel = 0x8000000000000000;
Counter s_server_chr_ids_cleared;

void PS4_SYSV_ABI OnServerChrIdCleared() {
    if (const u64 c_events = s_server_chr_ids_cleared.c_events.fetch_add(1, std::memory_order_relaxed) + 1;
        ShouldLog(c_events)) {
        std::printf("Workarounds: ServerChrId 0x8000000000000000 cleared, the character registers again (%llu so "
                    "far; #817)\n",
                    static_cast<unsigned long long>(c_events));
    }
}

bool InstallServerChrIdClear() {
    if (!HasCode(OffServerChrIdRead, ServerChrIdReadCode)) {
        return false;
    }
    const u64 va_site = s_va_image + OffServerChrIdRead;
    // rax holds PlayerGameData; rcx and the flags are dead at the site (both branches write rcx
    // before reading it, and the replaced test sets the flags the je at the return reads).
    Emitter e;
    e.Bytes({0x48, 0xb9}); // movabs rcx, ServerChrIdSentinel
    e.U64(ServerChrIdSentinel);
    e.Bytes({0x48, 0x39, 0x88, 0x90, 0x06, 0x00, 0x00}); // cmp [rax + 0x690], rcx
    e.Bytes({0x0f, 0x85});                               // jne to the replaced code
    const std::size_t off_rel = e.m_cb_code;
    e.U32(0);
    e.SaveVolatile();
    e.SaveXmm();
    e.CallAbs(reinterpret_cast<const void*>(&OnServerChrIdCleared));
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0x48, 0xc7, 0x80, 0x90, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov qword [rax + 0x690], 0
    const u32 rel = u32(e.m_cb_code - (off_rel + 4));
    std::memcpy(&e.m_code[off_rel], &rel, sizeof(rel));
    e.Copy(ServerChrIdReadCode);
    e.JmpAbs(va_site + sizeof(ServerChrIdReadCode));
    return PsGuestCode::InstallDetour("Bloodborne ServerChrId sentinel clear", va_site, ServerChrIdReadCode, e) ==
           Error::None;
}

/// The GX swap chain's two per-frame command heaps ([r12 + 0x370], [r12 + 0x378];
/// bloodborne_gx_command_heap, #504) are set up with `mov edx, 0x40000` (256 KiB): under psport
/// they fill, and the render command's node at 0x2199310 comes back null (the guard of
/// bloodborne_gx_node_guard). Each gets 1 MiB.
constexpr u64 OffGxHeapSizes[2] = {0x2195c0a, 0x2195c47};
constexpr u32 CbGxHeapGame = 0x40000, CbGxHeap = 0x100000;

bool PatchGxCommandHeap(unsigned char* p_image) {
    for (const u64 off_size : OffGxHeapSizes) {
        u32 cb_heap;
        std::memcpy(&cb_heap, p_image + off_size, sizeof(cb_heap));
        if (p_image[off_size - 1] != 0xba || cb_heap != CbGxHeapGame) {
            return false;
        }
    }
    for (const u64 off_size : OffGxHeapSizes) {
        std::memcpy(p_image + off_size, &CbGxHeap, sizeof(CbGxHeap));
    }
    return true;
}

void Announce(RuntimeQuirkId id, const char* sz_what, bool is_installed) {
    if (PsQuirk::Active(id)) {
        std::printf("Workarounds: %s %s\n", sz_what,
                    is_installed ? "in" : "not installed (other code at its sites, or the refusal above)");
    }
}

void ReportCounter(Counter& counter, const char* sz_what) {
    const u64 c_events = counter.c_events.load(std::memory_order_relaxed);
    if (c_events != counter.c_reported) {
        std::printf("Workarounds: %llu %s\n", static_cast<unsigned long long>(c_events), sz_what);
        counter.c_reported = c_events;
    }
}
} // namespace

void PatchImage(unsigned char* p_image, std::uint64_t) {
    s_va_image = reinterpret_cast<u64>(p_image);
    Announce(RUNTIME_QUIRK_bloodborne_gx_command_heap, "GX per-frame command heaps of 1 MiB",
             PsQuirk::Active(RUNTIME_QUIRK_bloodborne_gx_command_heap) && PatchGxCommandHeap(p_image));
    Announce(RUNTIME_QUIRK_bloodborne_render_job_slots, "render job slots given back",
             PsQuirk::Active(RUNTIME_QUIRK_bloodborne_render_job_slots) && InstallJobSlots());
    Announce(RUNTIME_QUIRK_bloodborne_blur_sample_clamp, "blur kernel sample count clamp",
             PsQuirk::Active(RUNTIME_QUIRK_bloodborne_blur_sample_clamp) && InstallBlurClamp());
    Announce(RUNTIME_QUIRK_bloodborne_fx_init_clear, "FX init targets cleared in memory",
             PsQuirk::Active(RUNTIME_QUIRK_bloodborne_fx_init_clear) && InstallFxInitClear());
    Announce(RUNTIME_QUIRK_bloodborne_server_chara_id, "ServerChrId sentinel cleared at the login's registration",
             PsQuirk::Active(RUNTIME_QUIRK_bloodborne_server_chara_id) && InstallServerChrIdClear());
}

void Report() {
    ReportCounter(s_slots_after_failure, "render job slots given back after failed steps");
    ReportCounter(s_slots_after_clear, "render job slots given back by runners destroyed or reset");
    ReportCounter(s_blur_clamps, "blur kernel sample counts raised from 2 to 3");
    ReportCounter(s_fx_written, "FX init targets cleared in memory");
    ReportCounter(s_fx_native, "FX init clears left to the game");
    ReportCounter(s_server_chr_ids_cleared, "ServerChrId sentinels cleared so the character registers again");
}
} // namespace PsWorkarounds
