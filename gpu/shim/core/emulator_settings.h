// SPDX-License-Identifier: GPL-2.0-or-later
// psport: shadPS4 settings used by the video core, read once from PS_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "common/types.h"

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? (v[0] == '1' || v[0] == 'y' || v[0] == 't') : fallback;
    }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    /// The console's system fonts (SST and the rest), dumped by the user: PS_SYS_FONTS.
    std::filesystem::path GetFontsDir() { const char* v = std::getenv("PS_SYS_FONTS"); return v ? v : ""; }
    s32 GetGpuId() { static const auto value = s32(Number("PS_GPU_ID", -1)); return value; }
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("PS_INTERNAL_WIDTH", 1920)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("PS_INTERNAL_HEIGHT", 1080)); return value; }
    std::string GetPresentMode() { const char* v = std::getenv("PS_PRESENT_MODE"); return v ? v : "Mailbox"; }
    int GetRcasAttenuation() { static const auto value = int(Number("PS_RCAS_ATTENUATION", 250)); return value; }
    // psport: Relaxed by default: without readbacks FaceGen reads stale GPU-written vertices
    // (vertex explosions); in a dense area it costs no measurable frame rate.
    u32 GetReadbacksMode() { static const auto value = u32(Number("PS_READBACKS", GpuReadbacksMode::Relaxed)); return value; }
    // psport: uncapped presets (run.sh: PS_VBLANK_HZ=480; 0 means the same): vblank runs at
    // 480 Hz, and a finished frame is presented as soon as it arrives (IsUncappedVblank), not on
    // the next vblank tick (at the display rate frames alternated 10/20 ms at 100 Hz: judder; at
    // 480 Hz they snap to ~2.1 ms steps). The frame rate is not limited unless PS_FPS_LIMIT
    // (launcher: "FPS limit") asks for it. Other values are used as given (60/90: the
    // fixed-timestep presets, flips on vblank ticks).
    u32 GetVblankFrequency() {
        static const u32 value = [] {
            const long hz = Number("PS_VBLANK_HZ", 60);
            return hz > 0 ? u32(hz) : 480u;
        }();
        return value;
    }
    /// Vblank faster than any fixed-timestep preset (above 120 Hz, or 0): flips at once.
    bool IsUncappedVblank() {
        static const bool value = [] {
            const long hz = Number("PS_VBLANK_HZ", 60);
            return hz <= 0 || hz > 120;
        }();
        return value;
    }
    /// Frames per second the present thread lets through; 0 = no limit (PS_FPS_LIMIT).
    u32 GetFrameLimit() {
        static const u32 value = u32(std::max(0L, Number("PS_FPS_LIMIT", 0)));
        return value;
    }
    /// PS_VRR=1: the display follows the game's frames (G-Sync, FreeSync, HDMI VRR), which Vulkan
    /// does not report. Presentation is FIFO, whose images such a display shows as they come
    /// (with Mailbox it does not follow the game on every driver), and the frame rate is held a
    /// little under the display's refresh rate (VideoOut), where FIFO would queue frames.
    bool IsVrr() { static const auto value = Flag("PS_VRR", false); return value; }
    /// PS_PACE_AT_START: the frame limit is kept before the game starts a frame, and VideoOut
    /// shows a finished frame at once, instead of holding it for its slot while the game makes the
    /// next one (a frame of latency). On with PS_VRR, off otherwise; 0 or 1 overrides.
    bool IsPacedAtStart() { static const auto value = Flag("PS_PACE_AT_START", IsVrr()); return value; }
    /// PS_DRIVER_PIPELINE_CACHE=0: the driver's pipeline cache is not saved beside the shader
    /// cache (vk_driver_pipeline_cache.h).
    bool IsDriverPipelineCacheEnabled() { static const auto value = Flag("PS_DRIVER_PIPELINE_CACHE", true); return value; }
    /// PS_COPY_GPU_BUFFERS=0 decodes guest memory instead of a copy. On by default on every host:
    /// the GPU thread decodes a submission after the game's call returns, so a game that refills
    /// that memory meanwhile would change what the GPU gets.
    bool IsCopyGpuBuffers() {
        static const auto value = [] {
            const bool is_set = std::getenv("PS_COPY_GPU_BUFFERS") != nullptr;
            const bool is_on = Flag("PS_COPY_GPU_BUFFERS", true);
            std::fprintf(stderr, "GPU buffers: submission copy %s (%s)\n", is_on ? "on" : "off",
                         is_set ? "PS_COPY_GPU_BUFFERS" : "default: the GPU thread decodes after the game's call returns");
            return is_on;
        }();
        return value;
    }
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("PS_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("PS_DUMP_SHADERS", false); return value; }
    bool IsFsrEnabled() { static const auto value = Flag("PS_FSR1", false); return value; }
    bool IsHdrAllowed() { static const auto value = Flag("PS_HDR", false); return value; }
    bool IsNullGPU() { static const auto value = Flag("PS_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { static const auto value = Flag("PS_PIPELINE_CACHE", true); return value; }
    bool IsRcasEnabled() { static const auto value = Flag("PS_RCAS", true); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("PS_READBACK_LINEAR", true); return value; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsUserfaultfdTracking() { return false; }
    bool IsVkCrashDiagnosticEnabled() { return false; }
    bool IsVkGuestMarkersEnabled() { static const auto value = Flag("PS_VK_MARKERS", false); return value; }
    bool IsVkHostMarkersEnabled() { static const auto value = Flag("PS_VK_MARKERS", false); return value; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { static const auto value = Flag("PS_VK_VALIDATION", false); return value; }
    bool IsVkValidationGpuEnabled() { return false; }
    bool IsVkValidationSyncEnabled() { static const auto value = Flag("PS_VK_VALIDATION_SYNC", false); return value; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
