// SPDX-License-Identifier: GPL-2.0-or-later
#include "job_step_probe.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "psport_guest_code.h"

namespace PsJobStepProbe {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using PsGuestCode::Emitter;
using PsGuestCode::Error;

/// The step's stack guard pointer, which both sites load into rbx RIP-relative.
constexpr u64 OffStackGuard = 0x53e3be0;
constexpr u64 OffStepExit = 0x1068092;

/// Each site from the `test eax, eax` after the call to its branch and the rbx load, replaced
/// whole because the branch and the load are relative to the instruction pointer.
struct Site {
    const char* sz_check;
    u64 off_site;
    std::array<u8, 11> code;
    u64 off_failed, off_passed;
    bool is_guard_loaded_when_passed;
};
constexpr std::array<Site, 2> Sites = {{
    {"usage check 0x2165150",
     0x1067fff,
     {0x85, 0xc0,                                // test eax, eax
      0x74, 0x15,                                // je 0x1068018
      0x48, 0x8b, 0x1d, 0xd6, 0xbb, 0x37, 0x04}, // mov rbx, [0x53e3be0]
     0x106800a,
     0x1068018,
     false},
    {"scratch block request 0x2169c30",
     0x1068036,
     {0x85, 0xc0,                               // test eax, eax
      0x48, 0x8b, 0x1d, 0xa1, 0xbb, 0x37, 0x04, // mov rbx, [0x53e3be0]
      0x75, 0x51},                              // jne 0x1068092
     OffStepExit,
     0x1068041,
     true},
}};

constexpr u64 LoggedFailures = 16;
std::array<std::atomic<u64>, Sites.size()> s_c_failures{};
std::array<u64, Sites.size()> s_c_reported{};

void PS4_SYSV_ABI OnFailure(u32 i_site, u32 result) {
    const u64 c_failures = s_c_failures[i_site].fetch_add(1, std::memory_order_relaxed) + 1;
    if (c_failures <= LoggedFailures || (c_failures & (c_failures - 1)) == 0) {
        std::printf("Job step probe: %s returned %u at %#llx (%llu so far; #683)\n", Sites[i_site].sz_check, result,
                    static_cast<unsigned long long>(Sites[i_site].off_site),
                    static_cast<unsigned long long>(c_failures));
    }
}

void EmitGuardLoad(Emitter& e, u64 va_image) {
    e.Bytes({0x48, 0xbb}); // movabs rbx, guard pointer's address
    e.U64(va_image + OffStackGuard);
    e.Bytes({0x48, 0x8b, 0x1b}); // mov rbx, [rbx]
}

Error InstallSite(u64 va_image, u32 i_site) {
    const Site& site = Sites[i_site];
    Emitter e;
    e.Bytes({0x85, 0xc0, 0x0f, 0x84}); // test eax, eax; jz passed
    const std::size_t off_rel = e.m_cb_code;
    e.U32(0);
    // Flags are dead past the site; the red zone is skipped and the caller-saved registers kept.
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
    e.SaveVolatile();
    e.SaveXmm();
    e.Bytes({0xbf}); // mov edi, i_site
    e.U32(i_site);
    e.Bytes({0x89, 0xc6}); // mov esi, eax
    e.CallAbs(reinterpret_cast<const void*>(&OnFailure));
    e.RestoreXmm();
    e.RestoreVolatile();
    e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
    EmitGuardLoad(e, va_image);
    e.JmpAbs(va_image + site.off_failed);
    const u32 rel = u32(e.m_cb_code - (off_rel + 4));
    std::memcpy(&e.m_code[off_rel], &rel, sizeof(rel));
    if (site.is_guard_loaded_when_passed) {
        EmitGuardLoad(e, va_image);
    }
    e.JmpAbs(va_image + site.off_passed);
    return PsGuestCode::InstallDetour("Bloodborne job step probe", va_image + site.off_site, site.code, e);
}
} // namespace

void PatchImage(unsigned char* p_image, std::uint64_t) {
    if (const char* sz_env = std::getenv("PS_JOB_STEP_PROBE"); !sz_env || sz_env[0] != '1') {
        return;
    }
    const u64 va_image = reinterpret_cast<u64>(p_image);
    int c_installed = 0;
    for (u32 i = 0; i < Sites.size(); ++i) {
        c_installed += InstallSite(va_image, i) == Error::None;
    }
    std::printf("Job step probe: %d of %zu failure exits of the render job step watched (PS_JOB_STEP_PROBE=1)\n",
                c_installed, Sites.size());
}

void Report() {
    for (std::size_t i = 0; i < Sites.size(); ++i) {
        const u64 c_failures = s_c_failures[i].load(std::memory_order_relaxed);
        if (c_failures != s_c_reported[i]) {
            std::printf("Job step probe: %llu failures of the %s\n", static_cast<unsigned long long>(c_failures),
                        Sites[i].sz_check);
            s_c_reported[i] = c_failures;
        }
    }
}
} // namespace PsJobStepProbe
