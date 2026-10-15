// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "psport_toggles.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "ffx_vk_fsr4_v07.h"
#include "ffx_vk_fsr4_v07_assets.h"
#include "psport_settings.h"
#include "video_core/renderer_vulkan/fsr411/fsr411.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

std::string AssetDir() {
    const char* sz_dir = std::getenv("PS_FSR4_DIR");
    return sz_dir && sz_dir[0] ? sz_dir : "fsr4_shaders";
}

bool ReadFile(const std::string& path, std::vector<u8>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    data.resize(size_t(file.tellg()));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())));
}

FfxFsr4ModelPreset ModelPreset(int preset) {
    switch (preset) {
    case 0:
        return FFX_FSR4_MODEL_PRESET_NATIVE_AA;
    case 1:
        return FFX_FSR4_MODEL_PRESET_QUALITY;
    case 2:
        return FFX_FSR4_MODEL_PRESET_BALANCED;
    case 3:
        return FFX_FSR4_MODEL_PRESET_PERFORMANCE;
    default:
        return FFX_FSR4_MODEL_PRESET_ULTRA_PERFORMANCE;
    }
}

} // namespace

struct Fsr4Upscaler::Impl {
    const Instance& m_instance;
    Scheduler& m_scheduler;
    std::vector<u8> m_scratch;
    FfxInterface m_backend{};
    bool m_is_backend_ok = false;
    ffxContext m_p_context{};
    bool m_is_context_ok = false;
    int m_model = -1;
    u32 m_out_width = 0, m_out_height = 0;
    std::string m_problem;
    bool m_is_fatal = false;
    u64 m_next_frame_id = 1;
    std::deque<std::pair<u64, u64>> m_in_flight; ///< provider frame id, scheduler tick
    // psport: FSR 4.1.1 (upscaler=fsr411): the replay of AMD's 4.1.1 DLL (fsr411/).
    std::unique_ptr<Fsr411::Upscaler> m_fsr411;
    std::deque<u64> m_fsr411_ticks; ///< scheduler ticks of its recent frames (constant ring)
    std::string m_fsr411_described;

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : m_instance{instance_}, m_scheduler{scheduler_} {}

    ~Impl() {
        Destroy();
    }

    /// `is_recording`: called from Record(), inside the upscaler's pass on the command buffer it
    /// records into: the earlier frames' dispatches are in earlier submissions (each frame ends
    /// with one), so only those are waited for. Finish() would send the command buffer.
    void Destroy(bool is_recording = false) {
        if (!m_is_backend_ok && !m_is_context_ok) {
            return;
        }
        if (is_recording) {
            m_scheduler.WaitSubmittedWork();
        } else {
            m_scheduler.Finish();
        }
        if (m_is_context_ok) {
            ffxFsr4V07DestroyContext(&m_p_context, nullptr);
            m_is_context_ok = false;
        }
        if (m_is_backend_ok) {
            ffxFsr4VkDestroyContext(reinterpret_cast<FfxFsr4VkContext*>(m_scratch.data()));
            m_is_backend_ok = false;
        }
        m_backend = {};
        m_in_flight.clear();
        m_model = -1;
    }

    void Fail(std::string reason, bool is_permanent) {
        if (m_problem != reason) {
            std::printf("Upscaler: FSR 4 unavailable: %s\n", reason.c_str());
        }
        m_problem = std::move(reason);
        m_is_fatal |= is_permanent;
    }

    bool CreateBackend(int preset, u32 ow, u32 oh) {
        FfxFsr4V07AssetSet assets{};
        if (!ffxFsr4V07BuildAssetSet(ModelPreset(preset), ow, oh, &assets)) {
            Fail("output size is not supported by the v07 model", true);
            return false;
        }
        const std::string dir = AssetDir() + "/";
        std::array<std::vector<u8>, FFX_FSR4_VK_PASS_COUNT> code;
        std::vector<u8> initializer, weights;
        // psport: tools/fsr4_optimize.sh puts fixed or faster passes into opt/ (post through
        // shared memory, pass 11 without out-of-bounds writes); PS_FSR4_OPT=0 keeps the originals.
        const char* sz_opt_env = std::getenv("PS_FSR4_OPT");
        const bool should_use_opt = !(sz_opt_env && sz_opt_env[0] == '0');
        u32 optimized = 0;
        const auto load = [&](const char* sz_name, std::vector<u8>& data) {
            if (should_use_opt && ReadFile(dir + "opt/" + sz_name, data)) {
                ++optimized;
                return true;
            }
            if (ReadFile(dir + sz_name, data)) {
                return true;
            }
            Fail("missing " + dir + sz_name + " (run tools/fetch_fsr4_assets.sh)", true);
            return false;
        };
        if (!load(assets.pre, code[0])) return false;
        for (u32 pass = 0; pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
            if (!load(assets.model[pass], code[1 + pass])) return false;
        }
        if (!load(assets.post, code[13]) || !load(assets.rcas, code[14]) ||
            !load(assets.spdAutoExposure, code[15]) || !load(assets.initializer, initializer) ||
            !load(assets.prePassWeights, weights)) {
            return false;
        }
        if (initializer.size() != FFX_FSR4_V07_INITIALIZER_BYTES ||
            weights.size() != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES) {
            Fail("model weights have an unexpected size", true);
            return false;
        }
        if (optimized) {
            std::printf("Upscaler: FSR 4, %u passes from %sopt/\n", optimized, dir.c_str());
        }
        static const std::array<std::string, FFX_FSR4_VK_PASS_COUNT> s_entries = [] {
            std::array<std::string, FFX_FSR4_VK_PASS_COUNT> names;
            names.fill("main");
            for (u32 pass = 1; pass <= FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
                names[pass] = "fsr4_model_v07_i8_pass" + std::to_string(pass);
            }
            return names;
        }();
        FfxFsr4VkCreateInfo ci{};
        ci.device = m_instance.GetDevice();
        ci.physicalDevice = m_instance.GetPhysicalDevice();
        for (u32 i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
            if (code[i].size() % 4) {
                Fail("invalid SPIR-V asset", true);
                return false;
            }
            ci.shaders[i] = {reinterpret_cast<const uint32_t*>(code[i].data()), code[i].size(),
                             s_entries[i].c_str()};
        }
        ci.modelInitializer = initializer.data();
        ci.modelInitializerSize = initializer.size();
        ci.prePassWeights = weights.data();
        ci.prePassWeightsSize = weights.size();
        m_scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
        ci.scratchBuffer = m_scratch.data();
        ci.scratchBufferSize = m_scratch.size();
        if (const VkResult result = ffxFsr4VkCreateContext(&ci, &m_backend);
            result != VK_SUCCESS) {
            Fail("Vulkan backend creation failed (" + std::to_string(int(result)) + ")", true);
            m_backend = {};
            return false;
        }
        m_is_backend_ok = true;

        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        // Any preset's render size fits: the context is not recreated for a size change.
        desc.maxRenderSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
        desc.maxUpscaleSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
        ffxFsr4V07SetBackendInterface(&m_backend);
        const auto created = ffxFsr4V07CreateContext(&m_p_context, &desc.header, nullptr);
        ffxFsr4V07SetBackendInterface(nullptr);
        if (created != FFX_API_RETURN_OK) {
            Fail("provider context creation failed (" + std::to_string(created) + ")", true);
            return false;
        }
        m_is_context_ok = true;
        m_model = preset;
        m_out_width = ow;
        m_out_height = oh;
        std::printf("Upscaler: FSR 4 v07 INT8 %s model, %s tier, output %ux%u\n",
                    ffxFsr4ModelPresetName(ModelPreset(preset)), assets.tier, ow, oh);
        return true;
    }

    void Retire() {
        while (!m_in_flight.empty() && m_scheduler.IsFree(m_in_flight.front().second)) {
            ffxFsr4VkRetireFrame(&m_backend, m_in_flight.front().first);
            m_in_flight.pop_front();
        }
    }

    VkResult Register(const Image& image, VkAccessFlags access) {
        const FfxFsr4VkExternalImageState state{
            .structSize = sizeof(FfxFsr4VkExternalImageState),
            .image = image.image,
            .view = image.view,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
            .stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .accessMask = access,
            .restoreLayout = VK_IMAGE_LAYOUT_GENERAL,
            .restoreStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .restoreAccessMask = access,
        };
        return ffxFsr4VkSetExternalImageState(&m_backend, &state);
    }

    bool Record411(const Frame& f) {
        if (!m_instance.IsFsr411Supported()) {
            Fail("FSR 4.1.1 needs INT8 dot products and VK_VALVE_shader_mixed_float_dot_product", true);
            return false;
        }
        if (!m_fsr411) {
            const char* sz_env = std::getenv("PS_FSR411_DIR");
            const Fsr411::Features features{.has_fp8_matrices = m_instance.IsFsr411Fp8Supported(),
                                            .has_fp16_matrices = m_instance.IsFsr411MatrixSupported()};
            m_fsr411 = std::make_unique<Fsr411::Upscaler>(m_instance.GetPhysicalDevice(), m_instance.GetDevice(),
                                                        sz_env && sz_env[0] ? sz_env : "fsr4_411", features);
        }
        // Its constant ring holds kFramesInFlight frames: the oldest must be done.
        while (m_fsr411_ticks.size() >= Fsr411::kFramesInFlight) {
            m_scheduler.Wait(m_fsr411_ticks.front());
            m_fsr411_ticks.pop_front();
        }
        Fsr411::Frame g;
        g.h_cmdbuf = f.cmdbuf;
        g.color = {f.color.image, f.color.view, f.color.width, f.color.height};
        g.depth = {f.depth.image, f.depth.view, f.depth.width, f.depth.height};
        g.motion = {f.motion.image, f.motion.view, f.motion.width, f.motion.height};
        g.output = {f.output.image, f.output.view, f.output.width, f.output.height};
        g.render_width = f.render_width;
        g.render_height = f.render_height;
        g.is_ultra_performance = f.preset >= 4;
        g.jitter[0] = f.jitter[0];
        g.jitter[1] = f.jitter[1];
        g.motion_scale[1] = PsToggle::Disabled(PsToggle::Fsr4MotionYFlip) ? -1.0f : 1.0f;
        g.sharpness = f.sharpness;
        g.should_sharpen = f.should_sharpen && f.sharpness > 0.0f;
        g.should_reset = f.should_reset;
        g.has_auto_exposure = f.has_auto_exposure;
        if (!m_fsr411->Record(g)) {
            // Missing assets are permanent for this session; the menu shows the reason.
            Fail("FSR 4.1.1: " + m_fsr411->Error(), m_fsr411->Error().starts_with("missing"));
            return false;
        }
        m_fsr411_ticks.push_back(m_scheduler.CurrentTick());
        if (const std::string d = m_fsr411->Describe(); d != m_fsr411_described) {
            m_fsr411_described = d;
            std::printf("Upscaler: FSR 4.1.1 replay, %s\n", d.c_str());
        }
        m_problem.clear();
        return true;
    }

    bool Record(const Frame& f) {
        if (PsSettings::Get().upscaler == PsSettings::UpscalerFsr411) {
            return Record411(f);
        }
        if (m_is_fatal) {
            return false;
        }
        if (!m_instance.IsFsr4Int8Supported()) {
            Fail("the GPU lacks INT8 dot product / compute derivative support", true);
            return false;
        }
        const int preset = std::clamp(f.preset, 0, 4);
        if (!m_is_context_ok || preset != m_model || f.output.width != m_out_width ||
            f.output.height != m_out_height) {
            Destroy(true);
            if (!CreateBackend(preset, f.output.width, f.output.height)) {
                Destroy(true);
                return false;
            }
        }
        Retire();
        const u64 frame_id = m_next_frame_id++;
        VkResult begin = ffxFsr4VkBeginFrame(&m_backend, frame_id);
        while (begin == VK_NOT_READY && !m_in_flight.empty()) {
            m_scheduler.Wait(m_in_flight.front().second);
            Retire();
            begin = ffxFsr4VkBeginFrame(&m_backend, frame_id);
        }
        if (begin != VK_SUCCESS) {
            Fail("no free provider frame (" + std::to_string(int(begin)) + ")", false);
            return false;
        }
        // psport: a frame begun here must reach RetireFrame even when it records nothing,
        // or every later BeginFrame fails (VK_ERROR_VALIDATION_FAILED_EXT).
        const auto abandon = [&] { m_in_flight.emplace_back(frame_id, m_scheduler.CurrentTick()); };
        constexpr VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
        const std::array<std::pair<const Image*, VkAccessFlags>, 4> images{{
            {&f.color, read},
            {&f.depth, read},
            {&f.motion, read},
            {&f.output, read | VK_ACCESS_SHADER_WRITE_BIT},
        }};
        static constexpr const char* s_names[] = {"color", "depth", "motion", "output"};
        for (u32 i = 0; i < images.size(); ++i) {
            if (const VkResult result = Register(*images[i].first, images[i].second);
                result != VK_SUCCESS) {
                Fail(std::string{"external image registration failed ("} + s_names[i] + ", " +
                         std::to_string(int(result)) + ")",
                     false);
                abandon();
                return false;
            }
        }
        const auto resource = [](const Image& image, u32 format, u32 state) {
            FfxApiResource r{};
            r.resource = reinterpret_cast<void*>(VkImageView(image.view));
            r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
            r.description.format = format;
            r.description.width = image.width;
            r.description.height = image.height;
            r.description.depth = 1;
            r.description.mipCount = 1;
            r.state = state;
            return r;
        };
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = reinterpret_cast<void*>(VkCommandBuffer(f.cmdbuf));
        d.color = resource(f.color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = resource(f.depth, FFX_SURFACE_FORMAT_R32_FLOAT,
                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = resource(f.motion, FFX_SURFACE_FORMAT_R16G16_FLOAT,
                                   FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = resource(f.output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {f.jitter[0], f.jitter[1]};
        // Vectors are in render pixels; the provider divides by the render size.
        d.motionVectorScale = {1.0f,
                               PsToggle::Disabled(PsToggle::Fsr4MotionYFlip) ? -1.0f : 1.0f};
        d.renderSize = {f.render_width, f.render_height};
        d.upscaleSize = {f.output.width, f.output.height};
        d.enableSharpening = f.should_sharpen && f.sharpness > 0.0f;
        d.sharpness = f.sharpness;
        d.enableAutoExposure = f.has_auto_exposure;
        d.frameTimeDelta = f.frame_ms;
        d.preExposure = 1.0f;
        d.reset = f.should_reset;
        d.cameraNear = f.near_plane;
        d.cameraFar = f.far_plane;
        d.cameraFovAngleVertical = f.vertical_fov;
        d.viewSpaceToMetersFactor = 1.0f;
        if (const auto result = ffxFsr4V07Dispatch(&m_p_context, &d.header);
            result != FFX_API_RETURN_OK) {
            Fail("dispatch failed (" + std::to_string(result) + ")", false);
            abandon();
            return false;
        }
        m_in_flight.emplace_back(frame_id, m_scheduler.CurrentTick());
        m_problem.clear();
        return true;
    }
};

Fsr4Upscaler::Fsr4Upscaler(const Instance& instance, Scheduler& scheduler)
    : m_impl{std::make_unique<Impl>(instance, scheduler)} {}

Fsr4Upscaler::~Fsr4Upscaler() = default;

bool Fsr4Upscaler::Record(const Frame& frame) {
    return m_impl->Record(frame);
}

const char* Fsr4Upscaler::Problem() const noexcept {
    return m_impl->m_problem.empty() ? nullptr : m_impl->m_problem.c_str();
}

bool Fsr4Upscaler::Fatal() const noexcept {
    return m_impl->m_is_fatal;
}

} // namespace Vulkan
