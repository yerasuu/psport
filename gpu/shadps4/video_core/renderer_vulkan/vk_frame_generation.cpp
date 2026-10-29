// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>

#include "common/logging/log.h"
#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "ffx_vk_portable.h"
#include "psport_settings.h"
#include "video_core/host_shaders/frame_gen_inputs_comp.h"
#include "video_core/host_shaders/frame_gen_ui_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));

namespace Vulkan {

namespace {

constexpr vk::ImageUsageFlags InputUsage =
    vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

/// The hudless copy viewed like the presenter views the display buffer (`requested`): the copy's
/// channel order, the requested sRGB decoding.
vk::Format HudlessViewFormat(vk::Format storage, vk::Format requested) {
    const bool is_srgb = requested == vk::Format::eR8G8B8A8Srgb || requested == vk::Format::eB8G8R8A8Srgb;
    switch (storage) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        return is_srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        return is_srgb ? vk::Format::eB8G8R8A8Srgb : vk::Format::eB8G8R8A8Unorm;
    default:
        return requested;
    }
}

/// Every format HudlessViewFormat views the copy in, listed at its creation so that a mutable
/// image keeps the driver's compression; empty for a copy outside the RGBA8 and BGRA8 classes.
std::span<const vk::Format> HudlessViewFormats(vk::Format storage) {
    static constexpr std::array RGBA8{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb};
    static constexpr std::array BGRA8{vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb};
    switch (storage) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        return RGBA8;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        return BGRA8;
    default:
        return {};
    }
}

void Summarize(char* sz_out, std::size_t cb_out) {
    using namespace FrameGenStats;
    const int flip_limit = PsSettings::Get().frame_gen_flip_limit.load(std::memory_order_relaxed);
    std::snprintf(sz_out, cb_out,
                  "Frame generation: summary: FSR 3.1; %llu game frames presented, %llu generated, %llu "
                  "presented, %llu dropped (the game waited), %llu without the scene's depth and motion, "
                  "%llu held (paused or no room on the display), %llu pauses, %llu failures; flip limit %d\n",
                  (unsigned long long)g_c_game_frames.load(), (unsigned long long)g_c_generated.load(),
                  (unsigned long long)g_c_presented.load(), (unsigned long long)g_c_dropped.load(),
                  (unsigned long long)g_c_without_capture.load(), (unsigned long long)g_c_held.load(),
                  (unsigned long long)g_c_pauses.load(), (unsigned long long)g_c_failures.load(), flip_limit);
    const auto skipped = [](FrameGenSkip reason) {
        return (unsigned long long)g_c_skipped[std::size_t(reason)].load(std::memory_order_relaxed);
    };
    const std::size_t cb_used = std::strlen(sz_out);
    std::snprintf(sz_out + cb_used, cb_out - cb_used,
                  "Frame generation: game frames without a generated frame: %llu HDR output, %llu without "
                  "capture, %llu paused, %llu no room on the display, %llu no memory, %llu after a failure, "
                  "%llu dropped (the game waited), %llu no swapchain image, %llu switched off in the menu\n",
                  skipped(FrameGenSkip::HdrOutput), skipped(FrameGenSkip::WithoutCapture),
                  skipped(FrameGenSkip::Paused), skipped(FrameGenSkip::NoRoom), skipped(FrameGenSkip::NoMemory),
                  skipped(FrameGenSkip::Failed), skipped(FrameGenSkip::GameWaited),
                  skipped(FrameGenSkip::NoSwapchainImage), skipped(FrameGenSkip::SwitchedOff));
}

// A line of its own: the report gives each summary 512 bytes, which the two lines above nearly fill.
void SummarizeCauses(char* sz_out, std::size_t cb_out) {
    using namespace FrameGenStats;
    // Frame counts alone mislead: a title menu flipping at the display's rate outnumbers the
    // play that follows it, so the game time of each side is given too.
    std::snprintf(sz_out, cb_out,
                  "Frame generation: without capture: %llu without the scene pass (the profile's upscaler "
                  "trigger did not run: menus, movies, loading), %llu with the scene pass but no scene depth; "
                  "game time %.1f s without capture, %.1f s with; generated: %llu with the UI interpolated "
                  "(no picture before the UI), %llu restarted (camera cut or after a frame without one)\n",
                  (unsigned long long)g_c_without_scene_pass.load(), (unsigned long long)g_c_without_scene_depth.load(),
                  double(g_us_without_capture.load()) / 1e6, double(g_us_captured.load()) / 1e6,
                  (unsigned long long)g_c_ui_interpolated.load(), (unsigned long long)g_c_restarted.load());
}

const char* SkipName(FrameGenSkip reason) {
    switch (reason) {
    case FrameGenSkip::HdrOutput:
        return "HDR output presents without generation";
    case FrameGenSkip::WithoutCapture:
        return "no scene depth and motion (menu, movie, loading)";
    case FrameGenSkip::Paused:
        return "paused, presenting fell behind";
    case FrameGenSkip::NoRoom:
        return "no room on the display (frame_gen_fit_display)";
    case FrameGenSkip::NoMemory:
        return "no memory for the generated frame";
    case FrameGenSkip::Failed:
        return "FSR 3.1 failed, frame generation is off";
    case FrameGenSkip::GameWaited:
        return "dropped, the game waited for a presentation frame";
    case FrameGenSkip::NoSwapchainImage:
        return "dropped, the swapchain gave no image";
    case FrameGenSkip::SwitchedOff:
        return "switched off in the menu";
    case FrameGenSkip::None:
    case FrameGenSkip::Count:
        break;
    }
    return "unknown";
}

} // namespace

void CountFrameGenSkip(FrameGenSkip reason) {
    if (reason == FrameGenSkip::None || reason >= FrameGenSkip::Count) {
        return;
    }
    if (FrameGenStats::g_c_skipped[std::size_t(reason)].fetch_add(1, std::memory_order_relaxed) == 0) {
        std::printf("Frame generation: first game frame without a generated frame: %s (counted per reason in the "
                    "exit summary)\n",
                    SkipName(reason));
    }
}

bool FrameGenerator::IsFsrSupported(const Instance& instance, std::string& out_result) {
    const std::string device{instance.GetModelName()};
    FfxVkPortableDeviceCapabilities capabilities{.structSize = sizeof(FfxVkPortableDeviceCapabilities)};
    if (ffxVkPortableQueryDeviceCapabilities(instance.GetPhysicalDevice(), &capabilities) != FFX_VK_PORTABLE_OK) {
        out_result = device + ": FSR-Vulkan could not query the device";
        return false;
    }
    if (!capabilities.fsr3FrameGenerationPrerequisites) {
        out_result = device + " lacks one of a compute queue, the subgroup operations, the FSR 3 storage formats, "
                              "shaderStorageImageWriteWithoutFormat and timeline semaphores";
        return false;
    }
    if (!instance.IsStorageImageWriteWithoutFormatEnabled()) {
        out_result = device + ": shaderStorageImageWriteWithoutFormat was not enabled on the device";
        return false;
    }
    out_result = device + ": FSR 3.1 frame interpolation and optical flow, 2x";
    return true;
}

FrameGenerator::FrameGenerator(const Instance& instance, Scheduler& scheduler)
    : m_instance{instance}, m_scheduler{scheduler} {
    if (IsWanted()) {
        runtime_calls_add_summary(&Summarize);
        runtime_calls_add_summary(&SummarizeCauses);
        std::printf("Frame generation: FSR 3.1, 2x\n");
    }
}

FrameGenerator::~FrameGenerator() {
    DestroyFsr3();
}

bool FrameGenerator::IsWanted() const {
    return PsSettings::Get().frame_gen.load(std::memory_order_relaxed) != PsSettings::FrameGenOff;
}

void FrameGenerator::Skip() {
    m_is_captured = false;
    m_is_hudless_captured = false;
    m_should_reset = true;
}

void FrameGenerator::Release() {
    Skip();
    if (!m_p_fsr3 && !m_depth_view && !m_motion_view && !m_hudless_image) {
        return;
    }
    double mb_fsr3 = 0.0;
    if (m_p_fsr3) {
        FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
        ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(m_p_fsr3, &usage);
        mb_fsr3 = double(usage.totalUsageInBytes) / 1e6;
        DestroyFsr3();
    }
    const double mb_inputs =
        double(u64(m_input_width) * m_input_height * (4 + 4) + u64(m_hudless_width) * m_hudless_height * 4) / 1e6;
    m_scheduler.DeferOperation([depth_view = std::move(m_depth_view), motion_view = std::move(m_motion_view),
                                hudless_views = std::move(m_hudless_views), depth = std::move(m_depth_image),
                                motion = std::move(m_motion_image),
                                hudless = std::move(m_hudless_image)]() mutable {
        depth_view.reset();
        motion_view.reset();
        hudless_views.clear();
        depth.Destroy();
        motion.Destroy();
        hudless.Destroy();
    });
    m_input_width = m_input_height = 0;
    m_hudless_width = m_hudless_height = 0;
    m_hudless_format = {};
    m_is_released = true;
    std::printf("Frame generation: off; FSR 3.1 context (%.1f MB) and its inputs (%.1f MB) freed\n", mb_fsr3,
                mb_inputs);
}

bool FrameGenerator::CreatePipelines() {
    if (m_inputs_pipeline && m_ui_pipeline) {
        return true;
    }
    const auto device = m_instance.GetDevice();
    const auto compute = [&](std::span<const u32> code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check<"create a frame generation pipeline">(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    const auto make_layout = [&](std::span<const vk::DescriptorSetLayoutBinding> bindings,
                                 vk::UniqueDescriptorSetLayout& desc_layout,
                                 vk::UniquePipelineLayout& pipeline_layout) {
        desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 2 * sizeof(s32)};
        pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    };
    const std::array<vk::DescriptorSetLayoutBinding, 2> input_bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    make_layout(input_bindings, m_inputs_desc_layout, m_inputs_pipeline_layout);
    // The UI over generated frames: the generated frame (storage), the frame and the frame without
    // UI (sampled).
    const std::array<vk::DescriptorSetLayoutBinding, 3> ui_bindings{{
        {0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    make_layout(ui_bindings, m_ui_desc_layout, m_ui_pipeline_layout);
    if (!m_inputs_pipeline_layout || !m_ui_pipeline_layout) {
        return false;
    }
    m_inputs_pipeline = compute(FRAME_GEN_INPUTS_COMP, *m_inputs_pipeline_layout);
    m_ui_pipeline = compute(FRAME_GEN_UI_COMP, *m_ui_pipeline_layout);
    return m_inputs_pipeline && m_ui_pipeline;
}

bool FrameGenerator::EnsureInputs(u32 w, u32 h) {
    if (m_depth_view && m_motion_view && m_input_width == w && m_input_height == h) {
        return true;
    }
    // The previous frame's generation may still read the old images.
    m_scheduler.WaitSubmittedWork();
    const auto device = m_instance.GetDevice();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view, vk::Format format,
                          const char* sz_name) {
        view.reset();
        image = VideoCore::UniqueImage(device, m_instance.GetAllocator());
        if (!image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = InputUsage,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            return false;
        }
        SetObjectName(device, vk::Image(image), "psport: frame generation {} {}x{}", sz_name, w, h);
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = ColorRange,
        }));
        return bool(view);
    };
    m_input_width = m_input_height = 0;
    if (!make(m_depth_image, m_depth_view, vk::Format::eR32Sfloat, "depth") ||
        !make(m_motion_image, m_motion_view, vk::Format::eR16G16Sfloat, "motion")) {
        Fail("no memory for its depth and motion inputs");
        return false;
    }
    m_input_width = w;
    m_input_height = h;
    m_should_reset = true;
    return true;
}

vk::ImageView FrameGenerator::BeginCapture(vk::CommandBuffer cmdbuf, vk::ImageView depth, u32 w, u32 h) {
    if (!IsWanted()) {
        return {};
    }
    if (!depth || !w || !h) {
        NoteSceneWithoutDepth();
        return {};
    }
    if (!CreatePipelines()) {
        Fail("its pipelines could not be created");
        return {};
    }
    if (!EnsureInputs(w, h)) {
        return {};
    }
    // Last frame's contents are not needed.
    std::array<vk::ImageMemoryBarrier2, 2> to_write{};
    for (u32 i = 0; i < to_write.size(); ++i) {
        to_write[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = i == 0 ? vk::Image(m_depth_image) : vk::Image(m_motion_image),
            .subresourceRange = ColorRange,
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_write.size()), .pImageMemoryBarriers = to_write.data()});
    const vk::DescriptorImageInfo depth_info{.imageView = depth, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo depth_out{.imageView = *m_depth_view, .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &depth_out},
    }};
    const std::array<s32, 2> size{s32(w), s32(h)};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_inputs_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_inputs_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*m_inputs_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size), size.data());
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    return *m_motion_view;
}

void FrameGenerator::EndCapture(vk::CommandBuffer cmdbuf, std::array<float, 2> jitter, bool is_motion_valid,
                                const FrameGenCamera& camera) {
    std::array<vk::ImageMemoryBarrier2, 2> to_read{};
    for (u32 i = 0; i < to_read.size(); ++i) {
        to_read[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = i == 0 ? vk::Image(m_depth_image) : vk::Image(m_motion_image),
            .subresourceRange = ColorRange,
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_read.size()), .pImageMemoryBarriers = to_read.data()});
    if (!is_motion_valid) {
        m_should_reset = true;
    }
    m_is_captured = true;
    m_capture_jitter = jitter;
    // FSR 3.1 rejects a camera that is not finite and positive: the last good one is kept.
    if (camera.near_plane > 0.0f && camera.far_plane > camera.near_plane && camera.vertical_fov > 0.0f &&
        camera.far_plane < 1e7f && camera.vertical_fov < 3.14f) {
        m_capture_camera = camera;
    }
}

void FrameGenerator::CaptureHudless(vk::CommandBuffer cmdbuf, vk::Image source, vk::Format format, u32 w, u32 h) {
    if (!IsWanted() || !m_is_captured || m_is_hudless_captured || !w || !h) {
        return;
    }
    if (!m_hudless_image || m_hudless_format != format || m_hudless_width != w || m_hudless_height != h) {
        // The presenter of the previous frame may still read the old copy.
        m_scheduler.WaitSubmittedWork();
        m_hudless_views.clear();
        m_hudless_width = m_hudless_height = 0;
        m_hudless_image = VideoCore::UniqueImage(m_instance.GetDevice(), m_instance.GetAllocator());
        const auto view_formats = HudlessViewFormats(format);
        const vk::ImageFormatListCreateInfo format_list{
            .viewFormatCount = u32(view_formats.size()),
            .pViewFormats = view_formats.data(),
        };
        if (!m_hudless_image.Create(vk::ImageCreateInfo{
                .pNext = &format_list,
                .flags = vk::ImageCreateFlagBits::eMutableFormat,
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            LOG_ERROR_COUNTED(Render_Vulkan,
                              "Frame generation: no memory for the {}x{} picture before the UI, the UI is "
                              "interpolated with the scene",
                              w, h);
            return;
        }
        SetObjectName(m_instance.GetDevice(), vk::Image(m_hudless_image), "psport: frame generation hudless {}x{}",
                      w, h);
        m_hudless_format = format;
        m_hudless_width = w;
        m_hudless_height = h;
    }
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                             vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 image_barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .image = vk::Image(m_hudless_image),
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &image_barrier});
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, vk::Image(m_hudless_image),
                     vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageCopy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .extent = {w, h, 1}});
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead);
    m_is_hudless_captured = true;
    static std::atomic<bool> s_is_reported{false};
    if (!s_is_reported.exchange(true, std::memory_order_relaxed)) {
        std::printf("Frame generation: the picture before the UI, %ux%u %s\n", w, h, vk::to_string(format).c_str());
    }
}

vk::ImageView FrameGenerator::Hudless(vk::Format format, vk::Extent2D& out_size) {
    if (!m_is_hudless_captured || !m_is_captured || !m_hudless_image) {
        return {};
    }
    out_size = vk::Extent2D{m_hudless_width, m_hudless_height};
    format = HudlessViewFormat(m_hudless_format, format);
    for (const auto& view : m_hudless_views) {
        if (view.format == format) {
            return *view.view;
        }
    }
    auto view = Check<"create a frame generation hudless view">(m_instance.GetDevice().createImageViewUnique({
        .image = vk::Image(m_hudless_image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
        .subresourceRange = ColorRange,
    }));
    if (!view) {
        return {};
    }
    m_hudless_views.push_back({format, std::move(view)});
    return *m_hudless_views.back().view;
}

void FrameGenerator::ComposeUi(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView hudless,
                               const Output& output, u32 width, u32 height) {
    const vk::ImageMemoryBarrier2 written{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &written});
    const vk::DescriptorImageInfo output_info{.imageView = output.view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo hudless_info{.imageView = hudless, .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &output_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &hudless_info},
    }};
    const std::array<s32, 2> size{s32(width), s32(height)};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_ui_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_ui_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*m_ui_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size), size.data());
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void FrameGenerator::RetireFinished() {
    while (m_p_fsr3 && !m_fsr3_pending.empty() && m_scheduler.IsFree(m_fsr3_pending.front().tick)) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(m_p_fsr3, m_fsr3_pending.front().frame_id);
        m_fsr3_pending.pop_front();
    }
}

FrameGenSkip FrameGenerator::Generate(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageView color_view,
                                      vk::ImageUsageFlags color_usage, vk::Image hudless,
                                      vk::ImageView hudless_view, u32 width, u32 height, const Output& output,
                                      bool should_generate, FrameGenSkip held) {
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - m_last_generate).count();
    m_last_generate = now;
    if (!(frame_ms > 0.0f && frame_ms < 200.0f)) {
        frame_ms = 16.6f;
    }
    RetireFinished();
    if (m_is_released && IsWanted()) {
        m_is_released = false;
        std::printf("Frame generation: on again; its FSR 3.1 context and inputs are made at the next scene "
                    "frame\n");
    }
    const bool had_capture = m_is_captured;
    const bool was_scene_without_depth = m_is_scene_without_depth;
    m_is_captured = false;
    m_is_hudless_captured = false;
    m_is_scene_without_depth = false;
    if (IsWanted()) {
        const auto us = u64(frame_ms * 1000.0f);
        (had_capture ? FrameGenStats::g_us_captured : FrameGenStats::g_us_without_capture) += us;
    }
    if (!IsWanted() || !had_capture || !should_generate || !output.image || !width || !height) {
        // A frame without the game's scene (menu, movie, loading), or none wanted (paused, the
        // display has no room): the frames after it are not interpolated from the ones before.
        if (IsWanted()) {
            ++(!had_capture ? FrameGenStats::g_c_without_capture : FrameGenStats::g_c_held);
            if (!had_capture) {
                ++(was_scene_without_depth ? FrameGenStats::g_c_without_scene_depth
                                           : FrameGenStats::g_c_without_scene_pass);
            }
        }
        m_should_reset = true;
        return !IsWanted()        ? FrameGenSkip::Failed
               : !had_capture     ? FrameGenSkip::WithoutCapture
               : !should_generate ? held
                                  : FrameGenSkip::NoMemory;
    }
    // FSR 3.1 keeps the previous input: it cannot switch between frames with and without UI.
    const bool is_hudless = hudless && hudless_view;
    if (is_hudless != m_was_hudless) {
        m_was_hudless = is_hudless;
        m_should_reset = true;
        if (++m_c_hudless_changes <= 4) {
            std::printf("Frame generation: %s\n",
                        is_hudless ? "UI kept out of the generated frames" : "frames without a picture before the UI");
        }
    }
    // The composed frame is read by compute after the presenter's passes wrote it.
    const vk::ImageMemoryBarrier2 color_ready{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eShaderWrite |
                         vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = color,
        .subresourceRange = ColorRange,
    };
    auto hudless_ready = color_ready;
    hudless_ready.image = hudless;
    // The output's previous contents (presented already) are not needed.
    const vk::ImageMemoryBarrier2 to_write{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = ColorRange,
    };
    const std::array ready{color_ready, to_write, hudless_ready};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = is_hudless ? 3u : 2u, .pImageMemoryBarriers = ready.data()});
    // The picture without UI is interpolated, then this frame's UI goes on top.
    const bool is_generated =
        GenerateFsr3(cmdbuf, is_hudless ? hudless : color, color_usage, width, height, output, frame_ms);
    if (!is_generated) {
        m_should_reset = true;
        return FrameGenSkip::Failed;
    }
    if (is_hudless) {
        ComposeUi(cmdbuf, color_view, hudless_view, output, width, height);
    } else {
        ++FrameGenStats::g_c_ui_interpolated;
    }
    if (m_should_reset) {
        ++FrameGenStats::g_c_restarted;
    }
    m_should_reset = false;
    // Presented after this submission: the present queue's blit reads it.
    vk::ImageMemoryBarrier2 written = to_write;
    written.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
    written.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    written.oldLayout = vk::ImageLayout::eGeneral;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &written});
    ++FrameGenStats::g_c_generated;
    return FrameGenSkip::None;
}

void FrameGenerator::DestroyFsr3() {
    if (!m_p_fsr3) {
        return;
    }
    m_scheduler.WaitSubmittedWork();
    ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(m_p_fsr3, m_fsr3_frame_id);
    ffxVkFsr3_3_1_6FrameGenerationContextDestroy(m_p_fsr3);
    m_p_fsr3 = nullptr;
    m_fsr3_pending.clear();
}

bool FrameGenerator::GenerateFsr3(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageUsageFlags color_usage,
                                  u32 width, u32 height, const Output& output, float frame_ms) {
    if (m_p_fsr3 && (m_fsr3_width != width || m_fsr3_height != height || m_fsr3_render_width != m_input_width ||
                     m_fsr3_render_height != m_input_height)) {
        DestroyFsr3();
    }
    if (!m_p_fsr3) {
        const FfxVkFsr3_3_1_6FrameGenerationCreateInfo create{
            .physicalDevice = m_instance.GetPhysicalDevice(),
            .device = m_instance.GetDevice(),
            .maxRenderWidth = m_input_width,
            .maxRenderHeight = m_input_height,
            .displayWidth = width,
            .displayHeight = height,
            .colorFormat = static_cast<VkFormat>(ColorFormat),
        };
        if (const auto result = ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create, &m_p_fsr3);
            result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            m_p_fsr3 = nullptr;
            std::printf("Frame generation: FSR 3.1 context creation failed (%d)\n", int(result));
            Fail("FSR 3.1 frame generation context creation failed");
            return false;
        }
        m_fsr3_width = width;
        m_fsr3_height = height;
        m_fsr3_render_width = m_input_width;
        m_fsr3_render_height = m_input_height;
        m_should_reset = true;
        FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
        ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(m_p_fsr3, &usage);
        std::printf("Frame generation: FSR 3.1 %ux%u (depth and motion %ux%u), %.1f MB\n", width, height,
                    m_input_width, m_input_height, double(usage.totalUsageInBytes) / 1e6);
    }
    const auto image = [](vk::Image handle, vk::Format format, u32 w, u32 h, vk::ImageUsageFlags usage) {
        return FfxVkFsr3_3_1_6FrameGenerationImage{
            .image = handle,
            .format = static_cast<VkFormat>(format),
            .width = w,
            .height = h,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
            .usage = static_cast<VkImageUsageFlags>(usage),
        };
    };
    const u64 frame_id = ++m_fsr3_frame_id;
    const FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{
        .commandBuffer = cmdbuf,
        .color = image(color, ColorFormat, width, height, color_usage),
        .depth = image(vk::Image(m_depth_image), vk::Format::eR32Sfloat, m_input_width, m_input_height, InputUsage),
        .motionVectors =
            image(vk::Image(m_motion_image), vk::Format::eR16G16Sfloat, m_input_width, m_input_height, InputUsage),
        .renderWidth = m_input_width,
        .renderHeight = m_input_height,
        .jitterOffsetX = m_capture_jitter[0],
        .jitterOffsetY = m_capture_jitter[1],
        .motionVectorScaleX = 1.0f,
        .motionVectorScaleY = 1.0f,
        .frameTimeMilliseconds = frame_ms,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .cameraNear = m_capture_camera.near_plane,
        .cameraFar = m_capture_camera.far_plane,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = m_capture_camera.vertical_fov,
        .cameraPosition = {0.0f, 0.0f, 0.0f},
        .cameraUp = {0.0f, 1.0f, 0.0f},
        .cameraRight = {1.0f, 0.0f, 0.0f},
        .cameraForward = {0.0f, 0.0f, 1.0f},
        .frameId = frame_id,
        .reset = m_should_reset ? VK_TRUE : VK_FALSE,
    };
    if (const auto result = ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(m_p_fsr3, &prepare);
        result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        m_fsr3_pending.push_back({frame_id, m_scheduler.CurrentTick()});
        std::printf("Frame generation: FSR 3.1 prepare failed (%d)\n", int(result));
        Fail("FSR 3.1 frame generation (prepare) failed");
        return false;
    }
    const FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{
        .commandBuffer = cmdbuf,
        .color = prepare.color,
        .output = image(output.image, ColorFormat, width, height, InputUsage | vk::ImageUsageFlagBits::eTransferSrc),
        .distortionField = {},
        .displayWidth = width,
        .displayHeight = height,
        .frameTimeMilliseconds = frame_ms,
        .cameraNear = prepare.cameraNear,
        .cameraFar = prepare.cameraFar,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = prepare.cameraVerticalFovRadians,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .frameId = frame_id,
        .reset = prepare.reset,
    };
    const auto result = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(m_p_fsr3, &dispatch);
    m_fsr3_pending.push_back({frame_id, m_scheduler.CurrentTick()});
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        std::printf("Frame generation: FSR 3.1 dispatch failed (%d)\n", int(result));
        Fail("FSR 3.1 frame generation (dispatch) failed");
        return false;
    }
    return true;
}

void FrameGenerator::Fail(const char* sz_reason) {
    auto& settings = PsSettings::Get();
    std::printf("Frame generation: %s; off\n", sz_reason);
    ++FrameGenStats::g_c_failures;
    settings.sz_frame_gen_problem = sz_reason;
    settings.frame_gen = PsSettings::FrameGenOff;
    m_should_reset = true;
}

} // namespace Vulkan
