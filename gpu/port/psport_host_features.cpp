// psport: psport --list-features' answers for the host table's rows the GPU library knows
// (src/platform/host_features.inc): the upscalers and frame generation on the device the game
// takes, from the game's own Vulkan::Instance and Dlss, and the titles' rows.
#include <initializer_list>
#include <string>
#include "../../src/platform/ps_host.h"
#include "../psport_gpu.h"
#include "core/emulator_settings.h"
#include "game_profile.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace {
// ps_host_feature_probed keeps the pointers for the exit summary.
std::string s_fsr4, s_fsr411, s_dlss, s_frame_gen_fsr;

void ProbeUpscalers() {
    const Vulkan::Instance instance{nullptr, EmulatorSettings.GetGpuId(),
                                    EmulatorSettings.IsVkValidationEnabled(),
                                    EmulatorSettings.IsVkCrashDiagnosticEnabled()};
    if (!instance.IsCreated()) {
        s_fsr4 = "no Vulkan device: " + instance.DescribeFailure();
        for (const PsHostFeatureId id :
             {PS_HOST_FEATURE_upscaler_fsr4, PS_HOST_FEATURE_upscaler_fsr411, PS_HOST_FEATURE_upscaler_dlss,
              PS_HOST_FEATURE_frame_gen_fsr}) {
            ps_host_feature_probed(id, 0, s_fsr4.c_str());
        }
        return;
    }
    const std::string device{instance.GetModelName()};
    const bool is_fsr4 = instance.IsFsr4Int8Supported();
    s_fsr4 = device + (is_fsr4 ? ": Float16, Int8, Int16, integer dot products and linear compute derivatives"
                               : " lacks one of Float16, Int8, Int16, integer dot products, linear compute "
                                 "derivatives and extended storage image formats");
    ps_host_feature_probed(PS_HOST_FEATURE_upscaler_fsr4, is_fsr4, s_fsr4.c_str());

    const bool is_fsr411 = instance.IsFsr411Supported();
    s_fsr411 = device + (!is_fsr4                          ? " lacks the FSR 4 INT8 shader features FSR 4.1.1 builds on"
                         : !is_fsr411                      ? " lacks VK_VALVE_shader_mixed_float_dot_product"
                         : instance.IsFsr411Fp8Supported() ? ": INT8 passes and FP8 cooperative matrices"
                                                           : ": INT8 passes, without FP8 cooperative matrices");
    ps_host_feature_probed(PS_HOST_FEATURE_upscaler_fsr411, is_fsr411, s_fsr411.c_str());

    const bool is_frame_gen_fsr = Vulkan::FrameGenerator::IsFsrSupported(instance, s_frame_gen_fsr);
    ps_host_feature_probed(PS_HOST_FEATURE_frame_gen_fsr, is_frame_gen_fsr, s_frame_gen_fsr.c_str());

    Vulkan::Dlss* p_dlss = Vulkan::Dlss::Get();
    const bool is_dlss = p_dlss && p_dlss->Available();
    s_dlss = !p_dlss                     ? "the DLSS bridge and NVIDIA's DLSS library are not installed, or PS_DLSS=0"
             : is_dlss                   ? device + ": NVIDIA NGX started on the device"
             : p_dlss->Problem().empty() ? device + ": NVIDIA NGX did not start on the device"
                                         : p_dlss->Problem();
    ps_host_feature_probed(PS_HOST_FEATURE_upscaler_dlss, is_dlss, s_dlss.c_str());
    // NGX holds the device, which goes with the instance.
    if (p_dlss) {
        p_dlss->Shutdown();
    }
}
} // namespace

extern "C" void psport_gpu_probe_host_features(void) {
    ProbeUpscalers();
    Game::ProbeHostFeatures();
}
