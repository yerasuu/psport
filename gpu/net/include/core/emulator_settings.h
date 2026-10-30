// psport: shadPS4 settings used by the video core, read once from PS_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
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
    s32 GetGpuId() { static const auto value = s32(Number("PS_GPU_ID", -1)); return value; }
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("PS_INTERNAL_WIDTH", 1920)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("PS_INTERNAL_HEIGHT", 1080)); return value; }
    std::string GetPresentMode() { const char* v = std::getenv("PS_PRESENT_MODE"); return v ? v : "Mailbox"; }
    int GetRcasAttenuation() { static const auto value = int(Number("PS_RCAS_ATTENUATION", 250)); return value; }
    // psport: Relaxed by default: without readbacks FaceGen reads stale GPU-written vertices
    // (vertex explosions); in Hunter's Nightmare it costs no measurable frame rate.
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
    bool IsCopyGpuBuffers() { static const auto value = Flag("PS_COPY_GPU_BUFFERS", false); return value; }
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("PS_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("PS_DUMP_SHADERS", false); return value; }
    bool IsFsrEnabled() { static const auto value = Flag("PS_FSR1", false); return value; }
    bool IsHdrAllowed() { static const auto value = Flag("PS_HDR", false); return value; }
    bool IsNullGPU() { static const auto value = Flag("PS_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { static const auto value = Flag("PS_PIPELINE_CACHE", true); return value; }
    bool IsRcasEnabled() { static const auto value = Flag("PS_RCAS", true); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("PS_READBACK_LINEAR", false); return value; }
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
    // psport co-op: online play through a shadNet server (PS_ONLINE=1, set by the launcher).
    static std::string Text(const char* name, const char* fallback) {
        const char* v = std::getenv(name);
        return v && v[0] ? v : fallback;
    }
    bool IsConnectedToNetwork() { static const auto value = Flag("PS_ONLINE", false); return value; }
    bool IsShadNetEnabled() { return IsConnectedToNetwork() && !shadnet_session_disabled.load(); }
    void SetShadNetSessionDisabled(bool v) { shadnet_session_disabled.store(v); }
    std::string GetShadNetServer() { return Text("PS_SHADNET_SERVER", "127.0.0.1:31313"); }
    std::string GetShadNetWebApiServer() { return Text("PS_SHADNET_WEBAPI", "http://127.0.0.1:31315"); }
    bool IsShadNetTlsRequired() { static const auto value = Flag("PS_SHADNET_TLS", false); return value; }
    bool IsPlaintextLoginAllowed() { static const auto value = Flag("PS_ONLINE_ALLOW_PLAINTEXT_LOGIN", false); return value; }
    bool IsUPnPEnabled() { static const auto value = Flag("PS_UPNP", true); return value; }

private:
    std::atomic<bool> shadnet_session_disabled{false};
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
