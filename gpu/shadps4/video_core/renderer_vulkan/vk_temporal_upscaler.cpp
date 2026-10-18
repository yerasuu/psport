// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/ui_composition.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vk_mem_alloc.h>

#include "psport_settings.h"
#include "psport_toggles.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/upscale_merge_comp.h"
#include "video_core/host_shaders/upscale_reactive_comp.h"
#include "video_core/host_shaders/taa_comp.h"
#include "video_core/host_shaders/taa_sharpen_comp.h"
#include "video_core/host_shaders/taa_sharpen_ldr_comp.h"
#include "video_core/host_shaders/fsr4_decode_comp.h"
#include "video_core/host_shaders/fsr4_encode_comp.h"
#include "video_core/host_shaders/fsr4_reactive_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

/// psport: PS_DUMP_TRIGGER=<file> PS_DUMP_DIR=<dir> (default out/dump): creating the file dumps
/// the upscaler's images of the next PS_DUMP_FRAMES (8) frames as raw files
/// <dir>/fNNN_<name>_<w>x<h>_<format>.raw, for checking temporal stability offline.
/// Returns the frame number to dump, or -1.
int DumpFrame() {
    static const char* s_sz_trigger = std::getenv("PS_DUMP_TRIGGER");
    static int s_c_remaining = 0, s_i_dump = 0, s_c_polls = 0;
    if (!s_sz_trigger) {
        return -1;
    }
    if (s_c_remaining == 0) {
        if (++s_c_polls % 30 != 0 || std::remove(s_sz_trigger) != 0) {
            return -1;
        }
        const char* sz_frames = std::getenv("PS_DUMP_FRAMES");
        s_c_remaining = sz_frames ? std::max(1, std::atoi(sz_frames)) : 8;
        s_i_dump = 0;
    }
    --s_c_remaining;
    return s_i_dump++;
}

struct DumpImage {
    vk::Image image; ///< in layout General
    u32 width, height, cb_pixel;
    const char* sz_name;
    const char* sz_format;
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
};

/// Copies `images` into host buffers after the commands recorded so far and writes them to
/// files once the GPU is done.
void DumpImages(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                int frame, std::initializer_list<DumpImage> images) {
    static const std::string s_dir = [] {
        const char* sz_env = std::getenv("PS_DUMP_DIR");
        return std::string{sz_env && sz_env[0] ? sz_env : "out/dump"};
    }();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& image : images) {
        const VkDeviceSize cb_image = VkDeviceSize(image.width) * image.height * image.cb_pixel;
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = cb_image,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer h_buffer{};
        VmaAllocation p_allocation{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &h_buffer, &p_allocation,
                            &info) != VK_SUCCESS) {
            std::printf("Dump: no host memory for %s\n", image.sz_name);
            continue;
        }
        const vk::BufferImageCopy region{
            .imageSubresource = {image.aspect, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1},
        };
        cmdbuf.copyImageToBuffer(image.image, vk::ImageLayout::eGeneral, h_buffer, region);
        char sz_path[512];
        std::snprintf(sz_path, sizeof(sz_path), "%s/f%03d_%s_%ux%u_%s.raw", s_dir.c_str(), frame,
                      image.sz_name, image.width, image.height, image.sz_format);
        scheduler.DeferPriorityOperation(
            [allocator = instance.GetAllocator(), h_buffer, p_allocation, info, cb_image,
             file = std::string{sz_path}] {
                vmaInvalidateAllocation(allocator, p_allocation, 0, VK_WHOLE_SIZE);
                if (FILE* p_file = std::fopen(file.c_str(), "wb")) {
                    std::fwrite(info.pMappedData, 1, cb_image, p_file);
                    std::fclose(p_file);
                }
                vmaDestroyBuffer(allocator, h_buffer, p_allocation);
            });
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    std::printf("Dump: frame %d -> %s\n", frame, s_dir.c_str());
}

/// The pixel layout a dump's file name carries (tools/dump_view.py, tools/regress/report.py).
const char* ColorDumpLayout(vk::Format format) {
    switch (format) {
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        return "bgra";
    case vk::Format::eA2R10G10B10UnormPack32:
        return "a2r10g10b10";
    case vk::Format::eA2B10G10R10UnormPack32:
        return "a2b10g10r10";
    default:
        return "rgba";
    }
}

void PrintIssues(const char* sz_what, u64 issues) {
    std::printf("Upscaler: %s invalid:", sz_what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

} // namespace

void DumpFinalFrameIfDue(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::Image image, u32 width, u32 height, vk::Format format) {
    static const char* s_sz_trigger = std::getenv("PS_FINAL_DUMP_TRIGGER");
    static int s_i_dump = 0;
    if (!s_sz_trigger || std::remove(s_sz_trigger) != 0) {
        return;
    }
    DumpImages(instance, scheduler, cmdbuf, s_i_dump++,
               {{image, width, height, 4, "final", ColorDumpLayout(format)}});
}

const Game::UpscalerPlacement& GetUpscalerPlacement() {
    static const Game::UpscalerPlacement s_placement = [] {
        Game::UpscalerPlacement found = Game::Upscaler();
        if (const char* sz_hash = std::getenv("PS_UPSCALE_BEFORE_CS")) {
            found.scene_combine_shader = std::strtoull(sz_hash, nullptr, 16);
        }
        if (const char* sz_hash = std::getenv("PS_UI_TRIGGER_VS")) {
            found.ui_shader = std::strtoull(sz_hash, nullptr, 16);
        }
        return found;
    }();
    return s_placement;
}

bool IsUpscalerAvailable() {
    static const bool s_is_available = [] {
        const char* sz_env = std::getenv("PS_UPSCALER");
        if (sz_env && std::strcmp(sz_env, "none") == 0) {
            return false;
        }
        if (GetUpscalerPlacement().scene_combine_shader == 0) {
            std::printf("Upscaler: off: no game profile places it in this game's frame "
                        "(PS_UPSCALE_BEFORE_CS)\n");
            return false;
        }
        return true;
    }();
    return s_is_available;
}

TemporalUpscaler::TemporalUpscaler(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                                   CameraMotion& camera_motion_, SceneTargets& scene_targets_)
    : m_instance{instance_}, m_scheduler{scheduler_}, m_texture_cache{texture_cache_},
      m_runtime{runtime_}, m_camera_motion{camera_motion_}, m_scene_targets{scene_targets_} {
    // Reject unsupported shaders before allocating resources or recording a frame.
    PsSettings::ConfigureUpscalerSupport(m_instance.IsFsr4Int8Supported(),
                                         m_instance.IsFsr411Supported());
    m_fsr4 = std::make_unique<Fsr4Upscaler>(m_instance, m_scheduler);
    {
        const Dlss* p_dlss = Dlss::Get();
        static std::string s_problem;
        s_problem = !p_dlss ? "the DLSS bridge and NVIDIA's DLSS library are not installed"
                        : p_dlss->Problem();
        PsSettings::ConfigureDlssSupport(p_dlss && p_dlss->Available(), s_problem.c_str());
    }
    // On/off and the parameters are the menu's settings.
    m_is_enabled = IsUpscalerAvailable();
    m_trigger_hash = GetUpscalerPlacement().scene_combine_shader;
    m_ui_trigger_vs = GetUpscalerPlacement().ui_shader;
    if (const char* sz_res = std::getenv("PS_OUTPUT_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(sz_res, "%ux%u", &w, &h) == 2 && w && h) {
            m_target_width = w;
            m_target_height = h;
        }
    }
    // Explicit resolution overrides retain the old compatibility path. Normal presets
    // keep guest allocations/UI native and change only host raster targets at frame boundaries.
    m_is_scaled_session = std::getenv("PS_RENDER_RES") && std::getenv("PS_RENDER_RES")[0];
    m_render_width = m_scene_targets.NativeSize().width;
    m_render_height = m_scene_targets.NativeSize().height;
    if (const char* sz_res = std::getenv("PS_RENDER_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(sz_res, "%ux%u", &w, &h) == 2 && w && h) {
            m_render_width = w;
            m_render_height = h;
        }
    }
    // Scene depth copied into the output-size UI depth by a blit (depth aspect).
    const auto features = m_instance.GetPhysicalDevice()
                              .getFormatProperties(vk::Format::eD32SfloatS8Uint)
                              .optimalTilingFeatures;
    m_is_depth_blit = (features & vk::FormatFeatureFlagBits::eBlitSrc) &&
                 (features & vk::FormatFeatureFlagBits::eBlitDst);
    if (m_is_enabled && !m_instance.IsStorageImageWriteWithoutFormatEnabled()) {
        std::printf("Upscaler: shaderStorageImageWriteWithoutFormat unsupported, FSR 3 off\n");
        m_is_enabled = false;
    }
    if (m_is_enabled) {
        std::printf("Upscaler: FSR 3.1 available (%s) on scene color before compute shader "
                    "%016llx\n",
                    PsSettings::Get().upscaler != PsSettings::UpscalerOff ? "on" : "off",
                    static_cast<unsigned long long>(m_trigger_hash));
    }
}

TemporalUpscaler::~TemporalUpscaler() {
    if (m_is_resources_ready) m_scheduler.Finish();
    if (m_p_context) {
        m_scheduler.Finish();
        ffxVkPortableUpscaleContextDestroy(m_p_context);
    }
}

bool TemporalUpscaler::Active() const {
    // Toggle 1 << 24 switches it off at run time (A/B); history restarts after.
    return m_is_enabled && !m_has_failed &&
           (PsSettings::Get().upscaler == PsSettings::UpscalerFsr3 ||
            PsSettings::IsFsr4(PsSettings::Get().upscaler) ||
            PsSettings::Get().upscaler == PsSettings::UpscalerTaa ||
            PsSettings::Get().upscaler == PsSettings::UpscalerDlss) &&
           !PsToggle::Disabled(1u << 24);
}

bool TemporalUpscaler::ReactiveOn() const {
    // FSR 4 takes no reactive mask itself: the mask blends its output afterwards
    // (RecordFsr4Reactive).
    // DLSS takes no reactive mask.
    return PsSettings::Get().should_use_reactive && !PsToggle::Disabled(1u << 27) && !UseDlss() &&
           PsSettings::Get().upscaler != PsSettings::UpscalerTaa;
}

void TemporalUpscaler::RecordFsr4Reactive(vk::CommandBuffer cmdbuf, vk::ImageView color, u32 w,
                                          u32 h, u32 ow, u32 oh) {
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const vk::MemoryBarrier2 before{
        .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    const vk::DescriptorImageInfo current{.sampler = *m_fsr4_linear_sampler, .imageView = color,
                                          .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo mask{.sampler = *m_fsr4_linear_sampler, .imageView = *m_reactive_view,
                                       .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{.imageView = *m_output_view,
                                      .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &current},
        {.dstBinding = 1, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &mask},
        {.dstBinding = 2, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
    }};
    const std::array<float, 5> push{float(w), float(h), m_jitter[0], m_jitter[1], 1.0f};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_fsr4_reactive_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_fsr4_reactive_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*m_fsr4_reactive_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), push.data());
    cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

void TemporalUpscaler::OnSceneColor(VideoCore::ImageId color) {
    m_scene_color = color;
}

bool TemporalUpscaler::KeepsLive(VideoCore::ImageId& id, const char* sz_role) {
    if (!id) {
        return false;
    }
    if (m_texture_cache.HasImage(id)) {
        return true;
    }
    LOG_ERROR_COUNTED(Render_Vulkan, "Upscaler: {} image {} freed during the frame, forgotten",
                      sz_role, id.index);
    id = {};
    return false;
}

namespace {
float Halton(u32 i_sample, u32 base) {
    float f = 1.0f, result = 0.0f;
    for (u32 i = i_sample; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}
} // namespace

bool TemporalUpscaler::RenderTarget(u32 w, u32 h) const {
    if (!m_is_scaled_session) return SceneResolution::Size{w, h} == m_scene_targets.NativeSize();
    return w >= m_render_width && h >= m_render_height && w < m_render_width + 8 &&
           h < m_render_height + 8;
}

bool TemporalUpscaler::ReducedScene(const VideoCore::Image& color) const {
    return !m_is_scaled_session && m_scene_targets.Reduced() && m_scene_targets.EligibleScene(color);
}

bool TemporalUpscaler::RasterScaling() const {
    return !m_is_scaled_session && m_scene_targets.Reduced() && !m_is_done_this_frame;
}

bool TemporalUpscaler::OnFrameStart() {
    // psport: PS_PRESET_FILE=<file> holding a preset number, read about once a second: switches
    // the preset like the menu does (scripted tests of live preset changes).
    static const char* s_sz_preset_file = std::getenv("PS_PRESET_FILE");
    if (s_sz_preset_file && ++m_c_preset_file_frames % 64 == 0) {
        if (FILE* p_file = std::fopen(s_sz_preset_file, "r")) {
            int value = 0;
            int output = -1;
            int provider = -1;
            const int fields = std::fscanf(p_file, "%d %d %d", &value, &output, &provider);
            if (fields >= 1 && value >= 0 &&
                value < PsSettings::PresetCount) {
                PsSettings::Get().preset.store(value);
            }
            const PsSettings::OutputList outputs = PsSettings::Outputs(PsSettings::Get().aspect);
            if (fields >= 2 && output >= 0 && output < outputs.c_sizes) {
                PsSettings::Get().output_res.store(PsSettings::PackSize(outputs.sizes[output]));
            }
            if (fields == 3 && provider >= 0 && provider < PsSettings::UpscalerCount) {
                PsSettings::Get().upscaler.store(provider);
            }
            std::fclose(p_file);
        }
    }
    const auto& settings = PsSettings::Get();
    // The guest's startup resolution patch remains in effect until restart. Keep the FSR
    // model at the applied preset while the menu saves the requested one for run.sh.
    const int preset = PsSettings::RenderPreset();
    if (m_applied_preset != preset || settings.upscaler == PsSettings::UpscalerOff) m_has_failed = false;
    if (m_has_dispatch_failed.exchange(false, std::memory_order_relaxed)) {
        m_has_failed = true;
    }
    const bool is_active = Active();
    const bool is_jitter_on = is_active && settings.should_jitter && !PsToggle::Disabled(1u << 25);
    const int upscaler = settings.upscaler.load();
    PsSettings::OutputSize output_size = PsSettings::CurrentOutput();
    // psport: the profile's camera, HUD markers and UI placement follow the output's aspect ratio
    // when its hooks are in.
    if (const float aspect = m_is_enabled ? float(output_size.width) / float(output_size.height) : 16.0f / 9.0f;
        aspect != m_applied_aspect) {
        m_applied_aspect = aspect;
        const Game::Profile* p_profile = Game::Active();
        m_is_aspect_followed = p_profile && p_profile->pfn_display_aspect && p_profile->pfn_display_aspect(aspect);
        PsSettings::Get().is_aspect_followed = m_is_aspect_followed;
    }
    if (!m_is_aspect_followed && output_size.width * 9 != output_size.height * 16) {
        // psport: a game whose camera keeps 16:9: its picture at that shape, as high (wide) as the
        // output allows; the presenter puts it in the middle.
        if (output_size.width * 9 > output_size.height * 16) {
            output_size.width = int(std::lround(output_size.height * 16 / 9.0 / 2)) * 2;
        } else {
            output_size.height = int(std::lround(output_size.width * 9 / 16.0 / 2)) * 2;
        }
    }
    const std::uint32_t output = PsSettings::PackSize(output_size);
    const bool is_output_changed = !m_is_scaled_session && m_applied_output != output;
    if (is_output_changed) {
        m_target_width = u32(output_size.width);
        m_target_height = u32(output_size.height);
        std::printf("Output resolution: %ux%u (live)\n", m_target_width, m_target_height);
        m_has_failed = false;
        m_has_fsr4_failed = false;
    }
    const bool is_changed = is_output_changed || m_applied_preset != preset || is_active != m_is_last_active ||
                         is_jitter_on != m_is_last_jitter || m_applied_upscaler != upscaler;
    if (m_applied_upscaler != upscaler || (is_output_changed && PsSettings::IsFsr4(upscaler))) {
        // A failed provider keeps a fatal flag internally; a user retry gets a fresh context, and so
        // does another output (one it could not do, 7680x2160, must not keep it off at 2560x1080).
        m_scheduler.Finish();
        m_fsr4 = std::make_unique<Fsr4Upscaler>(m_instance, m_scheduler);
        if (PsSettings::IsFsr4(upscaler)) PsSettings::Get().sz_fsr4_problem = nullptr;
    }
    if (m_applied_upscaler != upscaler) m_has_fsr4_failed = false; // retry after a menu change
    // Dynamic scene resolution scaling (live preset switching) works on all GPUs.
    // On GPUs without D32S8 blit support, UI depth is cleared instead of copied from scene.
    if (!m_is_scaled_session) {
        m_scene_targets.SetSize(SceneResolution::ForPreset(is_active ? preset : 0,
                                                        {m_target_width, m_target_height}));
        m_render_width = m_scene_targets.Size().width;
        m_render_height = m_scene_targets.Size().height;
    }
    PsSettings::Get().active_render_width = Scaled() ? m_render_width : m_scene_targets.Size().width;
    PsSettings::Get().active_render_height = Scaled() ? m_render_height : m_scene_targets.Size().height;
    if (is_changed || !m_is_dispatched_last_frame) m_should_reset = true;
    if (is_changed) m_i_jitter = 0;
    m_applied_preset = preset;
    m_applied_output = output;
    m_applied_upscaler = upscaler;
    m_is_last_active = is_active;
    m_is_last_jitter = is_jitter_on;
    m_is_dispatched_last_frame = false;

    // Diagnostics PS_UI_TRACE=1: the draws of frames that ended without the output-size UI
    // (their display buffer is shown as the game drew it, at its own size), and of one that had it.
    if (m_is_ui_trace_on) {
        static int s_c_bad = 0, s_c_good = 0;
        if ((!m_is_ui_phase && Scaled() && s_c_good > 0 && s_c_bad < 12) || (m_is_ui_phase && s_c_good < 1)) {
            ++(m_is_ui_phase ? s_c_good : s_c_bad);
            std::printf("UI trace: frame %llu %s:%s\n",
                        (unsigned long long)PsStats::g_frame_number.load(std::memory_order_relaxed),
                        m_is_ui_phase ? "with the output-size UI" : "WITHOUT it", m_ui_trace.c_str());
        }
        m_ui_trace.clear();
    }
    // The display pass of an upscaled frame reads the upscaled UI image.
    m_is_display_redirect = m_is_ui_phase;
    m_is_ui_phase = false;
    m_is_ui_read_barrier = false;
    m_is_done_this_frame = false;
    m_ldr_target = {};
    m_scene_color = {};
    m_is_snapshot_taken = false;
    m_is_opaque_valid = false;
    m_is_mask_ready = false;
    // Halton(2, 3); the menu or toggle 1 << 25 disables it.
    if (!is_jitter_on) {
        m_jitter = {};
        return is_changed;
    }
    // CameraMotion::OnDisplayPass has already cleared Depth(). Use the context's render
    // size, which survives the frame boundary (Performance 960 -> 1920 needs 32 phases).
    const u32 phases = Scaled() ? Motion::JitterPhases(m_render_width, m_target_width)
        : Motion::JitterPhases(m_scene_targets.Size().width, 1920);
    m_i_jitter = m_i_jitter % phases + 1;
    m_jitter = {Halton(m_i_jitter, 2) - 0.5f, Halton(m_i_jitter, 3) - 0.5f};
    return is_changed;
}

void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash != m_trigger_hash || m_is_done_this_frame || m_has_failed || Scaled()) {
        return;
    }
    m_is_done_this_frame = true;
    if (!Active() || !m_camera_motion.Ready() || !KeepsLive(m_scene_color, "scene color") ||
        !m_camera_motion.KeepsLiveDepth()) {
        m_should_reset = true;
        return;
    }
    Run();
}

bool TemporalUpscaler::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool is_hdr) {
    // DLSS, like FSR 4, has its own context: no portable FSR 3 context is made.
    const bool should_use_fsr4 = UseFsr4() || UseDlss();
    const bool should_use_taa = PsSettings::Get().upscaler == PsSettings::UpscalerTaa;
    if (should_use_taa && (w != ow || h != oh)) {
        std::printf("TAA: remove PS_RENDER_RES to use native-resolution TAA\n");
        return false;
    }
    if (m_is_resources_ready && w == m_width && h == m_height && ow == m_out_width && oh == m_out_height &&
        is_hdr == m_is_context_hdr && should_use_fsr4 == m_has_fsr4_resources && should_use_taa == m_has_taa_resources) {
        return true;
    }
    const auto device = m_instance.GetDevice();
    // FSR 4 owns no portable FSR 3 context, but its images can still be in flight.
    m_scheduler.Finish();
    m_is_resources_ready = false;
    if (m_p_context) {
        ffxVkPortableUpscaleContextDestroy(m_p_context);
        m_p_context = nullptr;
    }
    m_width = w;
    m_height = h;
    m_out_width = ow;
    m_out_height = oh;
    m_is_context_hdr = is_hdr;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = m_instance.GetInstance();
    device_info.physicalDevice = m_instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = m_instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = m_instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = m_instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = m_instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    // Native AA runs on the HDR scene color; scaled presets on the game's tonemapped frame.
    create_info.flags = is_hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
                                  FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE
                            : 0;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    // FSR 4 has its own model context (vk_fsr4); the images below are shared.
    if (!should_use_fsr4 && !should_use_taa) {
        if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
            PrintIssues("create info", issues);
            return false;
        }
        if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &m_p_context) !=
            FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 context creation failed\n");
            m_p_context = nullptr;
            return false;
        }
    }

    const auto p_allocator = m_instance.GetAllocator();
    m_motion_view.reset();
    m_output_view.reset();
    m_motion_image = VideoCore::UniqueImage(device, p_allocator);
    if (m_motion_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        SetObjectName(device, vk::Image(m_motion_image), "psport: upscaler motion {}x{}", w, h);
    }
    m_output_image = VideoCore::UniqueImage(device, p_allocator);
    if (m_output_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {ow, oh, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        SetObjectName(device, vk::Image(m_output_image), "psport: upscaler output {}x{}", ow, oh);
    }
    // psport: an image that could not be allocated (UniqueImage::Create logged it) gets no view;
    // the upscaler is then off (m_has_failed) instead of running on null images.
    bool is_allocated = m_motion_image && m_output_image;
    if (is_allocated) {
        m_output_view = Check(device.createImageViewUnique({
            .image = vk::Image(m_output_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        m_motion_view = Check(device.createImageViewUnique({
            .image = vk::Image(m_motion_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    const auto make_image = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                                const char* sz_name, vk::Format format,
                                vk::ImageUsageFlags usage, u32 iw = 0, u32 ih = 0,
                                vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor,
                                vk::ImageCreateFlags flags = {}) {
        view.reset();
        image = VideoCore::UniqueImage(device, p_allocator);
        if (image.Create(vk::ImageCreateInfo{
                .flags = flags,
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {iw ? iw : w, ih ? ih : h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = usage,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            SetObjectName(device, vk::Image(image), "psport: upscaler {} {}x{}", sz_name,
                          iw ? iw : w, ih ? ih : h);
        }
        if (!image) {
            is_allocated = false;
            return;
        }
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    make_image(m_opaque_image, m_opaque_view, "opaque", vk::Format::eR16G16B16A16Sfloat,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst |
                   vk::ImageUsageFlagBits::eTransferSrc);
    make_image(m_reactive_image, m_reactive_view, "reactive", vk::Format::eR8Unorm,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    m_fsr4_linear_view.reset();
    m_fsr4_linear_image = VideoCore::UniqueImage{};
    if (should_use_fsr4) {
        make_image(m_fsr4_linear_image, m_fsr4_linear_view, "FSR 4 linear", vk::Format::eR16G16B16A16Sfloat,
                   vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    }
    for (u32 i = 0; i < m_taa_history.size(); ++i) {
        m_taa_history_views[i].reset();
        m_taa_history[i] = VideoCore::UniqueImage{};
        if (should_use_taa) {
            make_image(m_taa_history[i], m_taa_history_views[i], i == 0 ? "TAA history 0" : "TAA history 1",
                       vk::Format::eR32G32B32A32Sfloat,
                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                           vk::ImageUsageFlagBits::eTransferSrc,
                       ow, oh);
        }
    }
    m_taa_next = 0;
    if (!is_allocated) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Upscaler images for {}x{} -> {}x{} not allocated, upscaler off",
                          w, h, ow, oh);
        return false;
    }
    CreatePipelines();
    m_is_opaque_valid = false;
    m_should_reset = true;
    m_is_resources_ready = true;
    m_has_fsr4_resources = should_use_fsr4;
    m_has_taa_resources = should_use_taa;
    if (should_use_taa) {
        std::printf("TAA: context %ux%u -> %ux%u (%s), no FSR model\n", w, h, ow, oh,
                    is_hdr ? "HDR scene color" : "tonemapped frame");
        return true;
    }
    if (should_use_fsr4) {
        std::printf("Upscaler: FSR 4 inputs %ux%u -> %ux%u\n", w, h, ow, oh);
        return true;
    }
    FfxVkPortableMemoryUsage usage{};
    usage.structSize = sizeof(usage);
    ffxVkPortableUpscaleContextGetMemoryUsage(m_p_context, &usage);
    std::printf("Upscaler: FSR 3 context %ux%u -> %ux%u (%s), %.1f MB\n", w, h, ow, oh,
                is_hdr ? "HDR scene color" : "tonemapped frame", usage.totalUsageInBytes / 1e6);
    return true;
}

void TemporalUpscaler::CreatePipelines() {
    if (m_merge_pipeline) {
        return;
    }
    const auto device = m_instance.GetDevice();
    const auto storage_layout = [&](u32 c_bindings, vk::UniqueDescriptorSetLayout& layout,
                                    vk::UniquePipelineLayout& pipeline_layout, u32 cb_push) {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < c_bindings; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = c_bindings,
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = cb_push};
        pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    };
    const auto compute = [&](const auto& code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    {
        // Merge: three storage images, then the motion and object motion images (debug).
        std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i < 3 ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        m_merge_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = sizeof(u32)};
        m_merge_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*m_merge_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    }
    m_merge_pipeline = compute(UPSCALE_MERGE_COMP, *m_merge_pipeline_layout);
    storage_layout(3, m_reactive_desc_layout, m_reactive_pipeline_layout, 3 * sizeof(float));
    m_reactive_pipeline = compute(UPSCALE_REACTIVE_COMP, *m_reactive_pipeline_layout);
    storage_layout(2, m_taa_sharpen_desc_layout, m_taa_sharpen_pipeline_layout, sizeof(float));
    m_taa_sharpen_pipeline = compute(TAA_SHARPEN_COMP, *m_taa_sharpen_pipeline_layout);
    m_taa_sharpen_ldr_pipeline = compute(TAA_SHARPEN_LDR_COMP, *m_taa_sharpen_pipeline_layout);
    storage_layout(1, m_fsr4_encode_desc_layout, m_fsr4_encode_pipeline_layout, sizeof(u32));
    m_fsr4_encode_pipeline = compute(FSR4_ENCODE_COMP, *m_fsr4_encode_pipeline_layout);
    {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
            {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1, .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        m_fsr4_decode_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(u32)};
        m_fsr4_decode_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*m_fsr4_decode_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        m_fsr4_decode_pipeline = compute(FSR4_DECODE_COMP, *m_fsr4_decode_pipeline_layout);
    }
    {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                                   : vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        m_fsr4_reactive_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 5 * sizeof(float)};
        m_fsr4_reactive_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*m_fsr4_reactive_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        m_fsr4_linear_sampler = Check(device.createSamplerUnique({
            .magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        m_fsr4_reactive_pipeline = compute(FSR4_REACTIVE_COMP, *m_fsr4_reactive_pipeline_layout);
    }
    {
        std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                        : vk::DescriptorType::eStorageImage,
                .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        m_taa_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 64};
        m_taa_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*m_taa_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        m_taa_sampler = Check(device.createSamplerUnique({
            .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        m_taa_pipeline = compute(TAA_COMP, *m_taa_pipeline_layout);
    }
}

void TemporalUpscaler::RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color,
                                vk::ImageView depth) {
    // Make scene writes and the last storage-image history writes visible to sampled reads.
    const vk::MemoryBarrier2 inputs{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,.pMemoryBarriers = &inputs});
    // Histories alternate between reads and writes. Queue ordering plus this dependency
    // protects both the previous write and a reused destination's previous read.
    std::array<vk::ImageMemoryBarrier2, 2> barriers{};
    for (u32 i = 0; i < barriers.size(); ++i) {
        barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = m_should_reset ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(m_taa_history[i]),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                             .pImageMemoryBarriers = barriers.data()});
    const std::array<vk::ImageView, 7> views{
        color, depth, *m_motion_view, *m_taa_history_views[1 - m_taa_next],
        *m_output_view, *m_taa_history_views[m_taa_next], *m_opaque_view};
    std::array<vk::DescriptorImageInfo, 7> infos{};
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (u32 i = 0; i < infos.size(); ++i) {
        infos[i] = {.sampler = i < 4 ? *m_taa_sampler : vk::Sampler{},
                    .imageView = views[i], .imageLayout = vk::ImageLayout::eGeneral};
        writes[i] = {.dstBinding = i, .descriptorCount = 1,
            .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &infos[i]};
    }
    struct Params {
        std::array<float, 2> jitter;
        u32 reset, pad;
        std::array<std::array<float, 4>, 3> depth;
    } params{m_jitter, m_should_reset ? 1u : 0u, std::getenv("PS_TAA_DIAGNOSTICS") ?
                 u32(std::clamp(std::atoi(std::getenv("PS_TAA_DIAGNOSTICS")),1,3)) : 0u,
             m_camera_motion.TaaDepthParameters()};
    // Optional techniques for A/B in one run (PS_TOGGLE_FILE bits 51-54): tonemapped blending,
    // YCoCg clipping, variance clipping, 3x3 reconstruction. Measured in game on 2026-10-02
    // (static and panning camera, interleaved captures): none improved stability beyond run
    // noise; YCoCg clipping and the reconstruction made it worse. Default: all off.
    params.pad |= (PsToggle::Disabled(PsToggle::TaaTonemapBlend) ? 1u << 8 : 0u) |
                  (PsToggle::Disabled(PsToggle::TaaClip) ? 2u << 8 : 0u) |
                  (PsToggle::Disabled(PsToggle::TaaVariance) ? 4u << 8 : 0u) |
                  (PsToggle::Disabled(PsToggle::TaaFilter) ? 8u << 8 : 0u) |
                  (PsToggle::Disabled(PsToggle::TaaKeepNearerHistory) ? 16u << 8 : 0u);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_taa_pipeline);
    if (params.pad & 0xffu) {
        static u32 s_c_diagnostic_frames = 0;
        if (s_c_diagnostic_frames++ < 16)
            std::printf("TAA diagnostic: reset=%u jitter=%.6f,%.6f mode=%u\n",
                        params.reset, m_jitter[0], m_jitter[1], params.pad);
    }
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_taa_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*m_taa_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(params), &params);
    cmdbuf.dispatch((m_out_width + 7) / 8, (m_out_height + 7) / 8, 1);
    const auto& settings = PsSettings::Get();
    const float strength = std::clamp(settings.sharpness.load(), 0.0f, 2.0f);
    if (settings.should_sharpen && strength > 0.0f && !(params.pad & 0xffu)) {
        const vk::MemoryBarrier2 resolved{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &resolved});
        const std::array<vk::DescriptorImageInfo, 2> sharpen_infos{{
            {.imageView = *m_taa_history_views[m_taa_next], .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *m_output_view, .imageLayout = vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 2> sharpen_writes{};
        for (u32 i = 0; i < sharpen_writes.size(); ++i)
            sharpen_writes[i] = {.dstBinding = i, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &sharpen_infos[i]};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_taa_sharpen_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                    *m_taa_sharpen_pipeline_layout, 0, sharpen_writes);
        cmdbuf.pushConstants(*m_taa_sharpen_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                             0, sizeof(strength), &strength);
        cmdbuf.dispatch((m_out_width + 7) / 8, (m_out_height + 7) / 8, 1);
    }
    m_taa_next = 1 - m_taa_next;
}

void TemporalUpscaler::ExtraSharpen(vk::Image target, bool is_ldr, u32 w, u32 h) {
    const auto& settings = PsSettings::Get();
    const float extra = std::clamp(settings.sharpness.load(), 0.0f, 2.0f) - 1.0f;
    if (!settings.should_sharpen || extra <= 0.0f) {
        return;
    }
    const auto device = m_instance.GetDevice();
    if (m_extra_sharpen_width != w || m_extra_sharpen_height != h) {
        // Submitted work may still read the old copy: destroyed once the GPU is past it. (A
        // Finish() here submitted the command buffer Run() goes on recording into.)
        m_scheduler.DeferOperation([image = std::move(m_extra_sharpen_image),
                                  view = std::move(m_extra_sharpen_view)]() mutable {
            view.reset();
            image.Destroy();
        });
        m_extra_sharpen_image = VideoCore::UniqueImage(device, m_instance.GetAllocator());
        if (m_extra_sharpen_image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR32G32B32A32Sfloat,
                .extent = {w, h, 1},
                .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            SetObjectName(device, vk::Image(m_extra_sharpen_image), "psport: extra sharpen copy {}x{}", w, h);
        }
        if (!m_extra_sharpen_image) {
            LOG_ERROR_COUNTED(Render_Vulkan, "Extra sharpening skipped: its {}x{} copy not allocated",
                              w, h);
            return;
        }
        m_extra_sharpen_view = Check(device.createImageViewUnique({
            .image = vk::Image(m_extra_sharpen_image), .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        m_extra_sharpen_width = w;
        m_extra_sharpen_height = h;
    }
    vk::ImageView target_view = *m_output_view;
    if (is_ldr) {
        if (!m_ui_storage_view) {
            // The UI image's own format may be sRGB; FSR 3 writes it through a UNORM view too.
            m_ui_storage_view = Check(device.createImageViewUnique({
                .image = target, .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        target_view = *m_ui_storage_view;
    }
    // psport: the commands are recorded through the scheduler (a recording thread for FSR 3).
    m_scheduler.RecordCrumb({.sz_name = "TAA extra sharpen"}, [target, copy = vk::Image(m_extra_sharpen_image),
                      copy_view = *m_extra_sharpen_view, target_view, extra, w, h,
                      pipeline = is_ldr ? *m_taa_sharpen_ldr_pipeline : *m_taa_sharpen_pipeline,
                      layout = *m_taa_sharpen_pipeline_layout](vk::CommandBuffer cmdbuf) {
        const auto image_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                       vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                       vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 b{
                .srcStageMask = src, .srcAccessMask = src_access,
                .dstStageMask = dst, .dstAccessMask = dst_access,
                .oldLayout = old_layout, .newLayout = vk::ImageLayout::eGeneral,
                .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        };
        constexpr auto all = vk::PipelineStageFlagBits2::eAllCommands;
        constexpr auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        image_barrier(target, vk::ImageLayout::eGeneral, all, rw,
                      vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
        image_barrier(copy, vk::ImageLayout::eUndefined, all, rw,
                      vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
        const vk::ImageBlit region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
        };
        cmdbuf.blitImage(target, vk::ImageLayout::eGeneral, copy, vk::ImageLayout::eGeneral,
                         region, vk::Filter::eNearest);
        image_barrier(copy, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                      vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageRead);
        image_barrier(target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                      vk::AccessFlagBits2::eTransferRead,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageWrite);
        const std::array<vk::DescriptorImageInfo, 2> infos{{
            {.imageView = copy_view, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = target_view, .imageLayout = vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        for (u32 i = 0; i < writes.size(); ++i)
            writes[i] = {.dstBinding = i, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[i]};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(extra), &extra);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        image_barrier(target, vk::ImageLayout::eGeneral,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageWrite, all, rw);
    });
}

void TemporalUpscaler::OnBlendedSceneDraw() {
    // The mask is opt-in (menu, PS_REACTIVE=1): on thin mist it trades trails for
    // jitter shimmer, which looked worse. Toggle 1 << 27 switches it off.
    if (m_is_snapshot_taken || !Active() || !ReactiveOn() ||
        !KeepsLive(m_scene_color, "scene color")) {
        return;
    }
    m_is_snapshot_taken = true;
    auto& color = m_texture_cache.GetImage(m_scene_color);
    // With a live preset the scene is in the reduced proxy: the mask and the FSR context
    // must use that size, or each frame would recreate the context (native <-> reduced).
    const bool is_reduced = ReducedScene(color);
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    // Scaled presets: the scene fills the top-left of its aligned targets (see RunScaled).
    const auto scene = Scaled() ? SceneSize(ow, oh) : std::array<u32, 2>{ow, oh};
    const u32 w = is_reduced ? m_scene_targets.Size().width : scene[0];
    const u32 h = is_reduced ? m_scene_targets.Size().height : scene[1];
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) {
        return;
    }
    if (!EnsureResources(w, h, Scaled() ? m_target_width : ow,
                         Scaled() ? m_target_height : oh, !Scaled())) {
        m_has_failed = true;
        return;
    }
    m_scheduler.EndRendering();
    vk::Image source = color.GetImage();
    vk::ImageLayout source_layout = vk::ImageLayout::eTransferSrcOptimal;
    if (is_reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        source = m_scene_targets.Read(m_scene_color, ci, vk::PipelineStageFlagBits2::eTransfer,
                                    vk::AccessFlagBits2::eTransferRead).image;
        if (!source) {
            return; // SceneTargets logged the missing proxy
        }
        source_layout = vk::ImageLayout::eGeneral;
    } else {
        m_runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        m_runtime.FlushBarriers();
    }
    const vk::Image opaque = vk::Image(m_opaque_image);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {w, h, 1},
    };
    // On the recording thread: direct recording here would move the rest of the scene there.
    m_scheduler.Record([opaque, source, source_layout, region](vk::CommandBuffer cmdbuf) {
        const auto to_general = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                    vk::ImageLayout old_layout, vk::PipelineStageFlags2 dst_stage,
                                    vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = old_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = opaque,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        // The previous frame's mask pass read it: wait for that before overwriting.
        to_general(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone,
                   vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eTransfer,
                   vk::AccessFlagBits2::eTransferWrite);
        cmdbuf.copyImage(source, source_layout, opaque, vk::ImageLayout::eGeneral, region);
        to_general(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                   vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderRead);
    });
    m_is_opaque_valid = true;
}

void TemporalUpscaler::OnSceneComposite() {
    if (!m_is_opaque_valid || m_is_mask_ready || m_has_failed || !ReactiveOn() ||
        !KeepsLive(m_scene_color, "scene color")) {
        return;
    }
    auto& color = m_texture_cache.GetImage(m_scene_color);
    const bool is_reduced = ReducedScene(color);
    const auto scene = Scaled() ? SceneSize(color.info.size.width, color.info.size.height)
                                : std::array<u32, 2>{color.info.size.width, color.info.size.height};
    const u32 w = is_reduced ? m_scene_targets.Size().width : scene[0];
    const u32 h = is_reduced ? m_scene_targets.Size().height : scene[1];
    if (w != m_width || h != m_height || !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    m_scheduler.EndRendering();
    if (is_reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto proxy = m_scene_targets.Read(m_scene_color, ci);
        m_is_mask_ready = proxy.image && RecordReactive(proxy.view);
        return;
    }
    const auto device = m_instance.GetDevice();
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    m_runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    m_runtime.FlushBarriers();
    m_is_mask_ready = RecordReactive(color_view);
    m_scheduler.DeferOperation([device, color_view] { device.destroyImageView(color_view); });
}

bool TemporalUpscaler::RecordReactive(vk::ImageView color_view) {
    if (!m_is_opaque_valid || !ReactiveOn()) {
        return false;
    }
    const vk::Image reactive = vk::Image(m_reactive_image);
    const vk::ImageView opaque = *m_opaque_view, mask = *m_reactive_view;
    const vk::Pipeline pipeline = *m_reactive_pipeline;
    const vk::PipelineLayout layout = *m_reactive_pipeline_layout;
    const u32 w = m_width, h = m_height;
    // Relative color change times the scale (default 1), zero below the threshold (0.2), at
    // most the maximum (0.9, never fully reactive) — the defaults of AMD's mask generator.
    const auto& settings = PsSettings::Get();
    const std::array<float, 3> params{settings.reactive_scale, settings.reactive_max,
                                      settings.reactive_threshold};
    m_scheduler.RecordCrumb({.sz_name = "TAA reactive mask"}, [=](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier2 to_write{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
        const vk::DescriptorImageInfo opaque_info{.imageView = opaque,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = mask,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &opaque_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
                                    writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(params), params.data());
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    });
    return true;
}

void TemporalUpscaler::Run() {
    if (auto* p_profiler = GpuProfiler::Get()) {
        const char* sz_label = PsSettings::Get().upscaler == PsSettings::UpscalerTaa
            ? "upscaler Run (TAA)" : "upscaler Run (FSR)";
        p_profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(sz_label),
                       [sz_label] { return std::string{sz_label}; });
    }
    auto& color = m_texture_cache.GetImage(m_scene_color);
    auto& depth = m_texture_cache.GetImage(m_camera_motion.Depth());
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const bool is_reduced = ReducedScene(color) && m_scene_targets.EligibleScene(depth);
    const u32 w = is_reduced ? m_scene_targets.Size().width : ow;
    const u32 h = is_reduced ? m_scene_targets.Size().height : oh;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        depth.info.size.width != ow || depth.info.size.height != oh ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, true)) {
        m_has_failed = true;
        return;
    }
    const auto depth_format = depth.info.pixel_format;
    const auto depth_view =
        CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    const auto color_view =
        CachedView(color, vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor);

    m_scheduler.EndRendering();
    m_runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    m_runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    m_runtime.FlushBarriers();
    vk::Image input_color = color.GetImage(), input_depth = depth.GetImage();
    vk::ImageView input_color_view = color_view, input_depth_view = depth_view;
    if (is_reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto c = m_scene_targets.Read(m_scene_color, ci);
        ci.format = depth.info.pixel_format;
        const auto d = m_scene_targets.Read(m_camera_motion.Depth(), ci);
        if (!c.image || !d.image) {
            return; // SceneTargets logged the missing proxy
        }
        input_color = c.image;
        input_depth = d.image;
        input_color_view = c.view;
        input_depth_view = d.view;
    }
    const auto cmdbuf = m_scheduler.CommandBuffer();
    const Breadcrumbs::Scope crumb{cmdbuf, m_scheduler.CrumbStream(), "TAA"};

    const auto own_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // Previous contents are not needed: the layouts start from undefined every frame.
    own_barrier(vk::Image(m_motion_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    own_barrier(vk::Image(m_output_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);

    m_camera_motion.RecordMotion(input_depth_view, *m_motion_view, w, h);
    if (m_camera_motion.TakeCut()) m_should_reset = true;
    own_barrier(vk::Image(m_motion_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const bool has_reactive = m_is_mask_ready || RecordReactive(input_color_view);
    if (!has_reactive) {
        // The merge pass binds the mask in GENERAL even when it is off, so it must leave UNDEFINED.
        own_barrier(vk::Image(m_reactive_image), vk::ImageLayout::eUndefined, all,
                    vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    }

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - m_last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    m_last_frame = now;

    bool is_dispatched = false;
    if (PsSettings::Get().upscaler == PsSettings::UpscalerTaa) {
        RecordTaa(cmdbuf, input_color_view, input_depth_view);
        is_dispatched = true;
    } else if (UseFsr4()) {
        is_dispatched = RecordFsr4(cmdbuf, {input_color, input_color_view, w, h},
                                {input_depth, input_depth_view, w, h}, w, h, ow, oh, frame_ms);
        if (is_dispatched && has_reactive) {
            RecordFsr4Reactive(cmdbuf, input_color_view, w, h, ow, oh);
        }
    } else if (UseDlss()) {
        is_dispatched = RecordDlss(
            cmdbuf,
            {input_color, input_color_view, color.info.pixel_format, vk::ImageAspectFlagBits::eColor, w, h},
            {input_depth, input_depth_view, depth_format, vk::ImageAspectFlagBits::eDepth, w, h}, w, h,
            ow, oh, frame_ms, true);
    } else {
        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmdbuf;
        info.color = Describe(input_color, color.info.pixel_format, w, h,
                              color.usage_flags, vk::ImageAspectFlagBits::eColor,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(input_depth, depth_format, w, h, depth.usage_flags,
                              vk::ImageAspectFlagBits::eDepth,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors = Describe(vk::Image(m_motion_image), vk::Format::eR16G16Sfloat, w, h,
                                      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                      vk::ImageAspectFlagBits::eColor,
                                      FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(vk::Image(m_output_image), vk::Format::eR16G16B16A16Sfloat, ow, oh,
                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        // Optional inputs, absent: a described but null image.
        info.exposure.structSize = sizeof(info.exposure);
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        if (has_reactive) {
            info.reactiveMask = Describe(vk::Image(m_reactive_image), vk::Format::eR8Unorm, w, h,
                                         vk::ImageUsageFlagBits::eStorage |
                                             vk::ImageUsageFlagBits::eSampled,
                                         vk::ImageAspectFlagBits::eColor,
                                         FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        }
        info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
        // Toggle 1 << 26 (tests): the opposite sign convention for FSR.
        const float sign = PsToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        info.jitterOffset = {sign * m_jitter[0], sign * m_jitter[1]};
        info.motionVectorScale = {1.0f, 1.0f};
        info.renderSize = {w, h};
        info.outputSize = {ow, oh};
        info.frameTimeMilliseconds = frame_ms;
        info.preExposure = 1.0f;
        info.cameraNear = m_camera_motion.Near();
        info.cameraFar = 3000.0f;
        info.cameraVerticalFovRadians = m_camera_motion.VerticalFov();
        info.viewSpaceToMeters = 1.0f;
        // RCAS strength 0..1 (menu, PS_FSR_SHARPNESS); jitter at 1:1 softens the image slightly.
        // AMD's RCAS ends at 1; ExtraSharpen adds the rest of the menu's 0..2.
        info.sharpness = std::min(PsSettings::Get().sharpness.load(), 1.0f);
        info.enableSharpening = PsSettings::Get().should_sharpen ? VK_TRUE : VK_FALSE;
        info.reset = m_should_reset ? VK_TRUE : VK_FALSE;
        info.frameId = m_frame_id++;
    
        FfxVkPortableUpscaleCreateInfo create_info{};
        create_info.structSize = sizeof(create_info);
        create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
        create_info.maxRenderSize = {w, h};
        create_info.maxOutputSize = {ow, oh};
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
            static bool s_has_printed = false;
            if (!s_has_printed) {
                s_has_printed = true;
                PrintIssues("dispatch", issues);
            }
            m_has_failed = true;
        } else if (ffxVkPortableUpscaleContextRecordDispatch(m_p_context, &info) !=
                   FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 dispatch failed\n");
            m_has_failed = true;
        } else {
            is_dispatched = true;
        }
    }
    if (is_dispatched) {
        m_should_reset = false;
        m_is_dispatched_last_frame = true;
        if (PsSettings::Get().upscaler != PsSettings::UpscalerTaa) {
            ExtraSharpen(vk::Image(m_output_image), false, ow, oh);
        }
        // psport: PS_DUMP_TRIGGER on this (HDR scene color) path too: the upscaler's inputs and
        // its result, for ghosting and history checks (tools/dump_view.py).
        if (const int dump = DumpFrame(); dump >= 0) {
            m_camera_motion.PrintState(dump);
            const vk::Format format = color.info.pixel_format;
            const bool is_rgba16f = format == vk::Format::eR16G16B16A16Sfloat;
            const bool is_r11g11b10 = format == vk::Format::eB10G11R11UfloatPack32;
            if (is_rgba16f || is_r11g11b10) {
                DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                           {{input_color, w, h, is_rgba16f ? 8u : 4u, "input",
                             is_rgba16f ? "rgba16f" : "r11g11b10f"}});
            }
            DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                       {{vk::Image(m_motion_image), w, h, 4, "motion", "rg16f"},
                        {vk::Image(m_output_image), ow, oh, 8, "output", "rgba16f"}});
            DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                       {{input_depth, w, h, 4, "depth", "f32", vk::ImageAspectFlagBits::eDepth}});
            if (const auto object = m_camera_motion.ObjectMotionImage(w, h); object) {
                DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                           {{object, w, h, 16, "objects", "rgba32f"}});
            }
        }
        // The result replaces the scene color's RGB (its alpha carries data for the post).
        own_barrier(vk::Image(m_output_image), vk::ImageLayout::eGeneral, all, rw,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
        m_runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        m_runtime.FlushBarriers();
        const vk::DescriptorImageInfo out_info{.imageView = *m_output_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = *m_reactive_view,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        // Sampled for the debug view only; the object image falls back to the motion image.
        const vk::ImageView objects = m_camera_motion.ObjectMotionView();
        const vk::DescriptorImageInfo motion_info{.imageView = *m_motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo objects_info{
            .imageView = objects ? objects : *m_motion_view,
            .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 5> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &out_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
            {.dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &motion_info},
            {.dstBinding = 4,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &objects_info},
        }};
        // Debug (menu or toggle 1 << 28): the reactive mask in red over a darkened frame;
        // or the motion vectors (menu).
        const int debug_view = PsSettings::Get().debug_view;
        const u32 mode = has_reactive && (debug_view == PsSettings::DebugReactive ||
                                          PsToggle::Disabled(1u << 28))
                             ? 1
                         : debug_view == PsSettings::DebugMotion ? 2
                                                                 : 0;
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_merge_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_merge_pipeline_layout, 0,
                                    writes);
        cmdbuf.pushConstants(*m_merge_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(mode), &mode);
        cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    }
}

vk::ImageView TemporalUpscaler::CachedView(const VideoCore::Image& image, vk::Format format,
                                           vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* p_oldest = &m_view_cache[0];
    for (auto& entry : m_view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++m_c_view_uses;
            return entry.view;
        }
        if (entry.last_use < p_oldest->last_use) {
            p_oldest = &entry;
        }
    }
    const auto device = m_instance.GetDevice();
    if (p_oldest->view) {
        m_scheduler.DeferOperation([device, view = p_oldest->view] { device.destroyImageView(view); });
    }
    *p_oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++m_c_view_uses,
    };
    return p_oldest->view;
}

} // namespace Vulkan

namespace Vulkan {

std::array<u32, 2> TemporalUpscaler::SceneSize(u32 w, u32 h) const {
    if (!m_is_scaled_session) return {m_render_width, m_render_height};
    // The game's own render size (scene constants); PS_RENDER_RES until the first camera.
    auto size = m_camera_motion.RenderSize();
    if (size[0] == 0 || size[1] == 0) {
        size = {m_render_width, m_render_height};
    }
    return {std::min(size[0], w), std::min(size[1], h)};
}

float TemporalUpscaler::SceneMipBias() const {
    if (!Active() || PsSettings::Get().upscaler == PsSettings::UpscalerTaa) return 0.0f;
    const float render = float(PsSettings::Get().active_render_width.load());
    const float output = float(Scaled() ? m_target_width : 1920u);
    return render > 0.0f && render < output ? std::log2(render / output) : 0.0f;
}

bool TemporalUpscaler::Scaled() const {
    return m_is_scaled_session || m_target_width != 1920 || m_target_height != 1080;
}

void TemporalUpscaler::OnColorTarget(VideoCore::ImageId color) {
    if (m_is_ui_phase || !m_camera_motion.KeepsLiveDepth()) {
        return;
    }
    const auto& image = m_texture_cache.GetImage(color);
    const auto& depth = m_texture_cache.GetImage(m_camera_motion.Depth());
    if (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
        image.info.size.width == depth.info.size.width &&
        image.info.size.height == depth.info.size.height) {
        m_ldr_target = color;
    }
}

void TemporalUpscaler::OnDraw(u64 vs_hash, VideoCore::ImageId color,
                              VideoCore::ImageId depth, bool is_native_viewport) {
    if (!Scaled() && vs_hash == m_ui_trigger_vs) m_is_done_this_frame = true;
    // psport (issue #67, patch by bmy): a UI draw without a color target (a Scaleform mask, color
    // writes off) writes the stencil the UI's next draws test, so it must go into the UI's
    // output-size depth like them. Left in the guest depth, the mask was lost when the next UI
    // draw switched to the UI depth (PrepareUiDepth clears its stencil): masked images (the
    // loading screen's item picture, cut out of a sheet) vanished.
    if (m_is_ui_phase && !color && depth && depth != m_ui_depth &&
        KeepsLive(m_ui_color, "UI color")) {
        const auto& depth_image = m_texture_cache.GetImage(depth);
        const auto& ui_target = m_texture_cache.GetImage(m_ui_color);
        if (depth_image.info.size.width == ui_target.info.size.width &&
            depth_image.info.size.height == ui_target.info.size.height) {
            if (!EnsureUiResources(m_ui_width, m_ui_height, m_ui_format,
                                   depth_image.info.pixel_format)) {
                m_is_ui_phase = false;
                return;
            }
            PrepareUiDepth(depth);
            m_ui_depth = depth;
        }
        return;
    }
    if (!Scaled() || !color) {
        return;
    }
    if (m_is_ui_phase) {
        // A UI movie can start without stencil and enable it for later text/masks.
        if (color == m_ui_color && depth && depth != m_ui_depth) {
            if (!EnsureUiResources(m_ui_width, m_ui_height, m_ui_format,
                                   m_texture_cache.GetImage(depth).info.pixel_format)) {
                m_is_ui_phase = false;
                return;
            }
            PrepareUiDepth(depth);
            m_ui_depth = depth;
        }
        return;
    }
    const auto& image = m_texture_cache.GetImage(color);
    const auto& movie_shaders = GetUpscalerPlacement().ui_movie_shaders;
    const bool is_movie = vs_hash == m_ui_trigger_vs ||
                       std::ranges::find(movie_shaders, vs_hash) != movie_shaders.end() ||
                       (m_is_scaled_session && is_native_viewport);
    if (m_is_ui_trace_on && m_ui_trace.size() < 1500) {
        m_ui_trace += fmt::format(" [vs {:x} {}x{} fmt {} movie {} display {}]", vs_hash,
                                image.info.size.width, image.info.size.height,
                                u32(image.info.pixel_format), is_movie ? 1 : 0,
                                FrameCapture::IsDisplayBuffer(image.info.guest_address) ? 1 : 0);
    }
    const bool is_ui_draw = is_movie &&
        (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm ||
         image.info.pixel_format == vk::Format::eR8G8B8A8Srgb) &&
        RenderTarget(image.info.size.width, image.info.size.height) &&
        !FrameCapture::IsDisplayBuffer(image.info.guest_address);
    switch (UiComposition::Choose(Scaled(), is_ui_draw,
                                  m_camera_motion.Ready() && m_ldr_target == color &&
                                      m_camera_motion.KeepsLiveDepth(),
                                  Active() && !m_has_failed)) {
    case UiComposition::Background::None:
        return;
    case UiComposition::Background::Temporal:
        RunScaled();
        if (m_is_ui_phase) {
            return;
        }
        // A failed FSR dispatch must not force UI text back to the scene resolution.
        [[fallthrough]];
    case UiComposition::Background::Copy:
        RunUiOnly(color, depth);
        return;
    }
}

bool TemporalUpscaler::EnsureUiResources(u32 w, u32 h, vk::Format color, vk::Format depth) {
    const bool is_resized = w != m_ui_width || h != m_ui_height;
    const bool is_new_color = !m_ui_image || is_resized || color != m_ui_format;
    const bool is_new_depth = !m_ui_depth_image || is_resized || depth != m_ui_depth_format;
    if (!is_new_color && !is_new_depth) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < m_ui_retry_time) {
        LOG_ERROR_COUNTED(Render_Vulkan,
                          "UI images {}x{} skipped until the retry after a failed allocation", w, h);
        return false;
    }
    m_scheduler.Finish();
    const auto device = m_instance.GetDevice();
    const auto p_allocator = m_instance.GetAllocator();
    m_ui_width = w;
    m_ui_height = h;
    if (is_new_color) {
        m_ui_views.clear();
        m_ui_view.reset();
        m_ui_storage_view.reset();
        m_ui_image = VideoCore::UniqueImage(device, p_allocator);
        if (m_ui_image.Create(vk::ImageCreateInfo{
                .flags = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
                .imageType = vk::ImageType::e2D,
                .format = color,
                .extent = {w, h, 1},
                .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                         vk::ImageUsageFlagBits::eTransferSrc,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            SetObjectName(device, vk::Image(m_ui_image), "psport: UI color {}x{} {}", w, h, vk::to_string(color));
        }
        m_ui_format = color;
    }
    if (is_new_depth) {
        m_ui_depth_view.reset();
        m_ui_depth_image = VideoCore::UniqueImage(device, p_allocator);
        if (m_ui_depth_image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = depth,
                .extent = {w, h, 1},
                .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                         vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            SetObjectName(device, vk::Image(m_ui_depth_image), "psport: UI depth {}x{} {}", w, h,
                          vk::to_string(depth));
        }
        const auto aspect = vk::ImageAspectFlagBits::eDepth |
            (depth == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                : vk::ImageAspectFlags{});
        if (m_ui_depth_image) {
            m_ui_depth_view = Check(device.createImageViewUnique({
                .image = vk::Image(m_ui_depth_image), .viewType = vk::ImageViewType::e2D,
                .format = depth, .subresourceRange = {aspect, 0, 1, 0, 1},
            }));
        }
        m_ui_depth_format = depth;
    }
    if (!m_ui_image || !m_ui_depth_image) {
        LOG_ERROR_COUNTED(Render_Vulkan,
                          "UI images {}x{} not allocated, the UI is drawn at the scene resolution",
                          w, h);
        // psport: retried after a delay because every attempt waits for the GPU (Finish), which
        // under memory pressure stalled every UI phase.
        m_ui_retry_time = now + std::chrono::seconds(2);
        return false;
    }
    std::printf("UI: native composition %ux%u (scene %ux%u), independent of FSR history\n",
                w, h, m_render_width, m_render_height);
    return true;
}

void TemporalUpscaler::PrepareUiDepth(VideoCore::ImageId depth_id) {
    m_scheduler.EndRendering();
    const auto aspect = vk::ImageAspectFlagBits::eDepth |
        (m_ui_depth_format == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                      : vk::ImageAspectFlags{});
    // Copy depth if blit is supported; otherwise just clear it (UI depth test still works).
    const bool should_copy = depth_id && m_is_depth_blit &&
        m_texture_cache.GetImage(depth_id).info.pixel_format == m_ui_depth_format;
    if (should_copy) {
        m_runtime.Transit(&m_texture_cache.GetImage(depth_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        m_runtime.FlushBarriers();
    }
    const vk::ImageSubresourceRange range{aspect, 0, 1, 0, 1};
    // Without blit support, UI depth is cleared but not copied from the scene.
    // UI elements will still depth-test correctly against the cleared buffer.
    vk::Image source{};
    vk::ImageBlit region{};
    if (should_copy) {
        const auto& image = m_texture_cache.GetImage(depth_id);
        source = vk::Image(image.backing->image);
        region = {
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(image.info.size.width), s32(image.info.size.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(m_ui_width), s32(m_ui_height), 1}},
        };
    }
    // psport: recorded on the recording thread (direct recording here waited for it to finish
    // everything queued, every frame).
    m_scheduler.Record([depth = vk::Image(m_ui_depth_image), range, source,
                      region](vk::CommandBuffer cmd) {
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = depth, .subresourceRange = range,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.clearDepthStencilImage(depth, vk::ImageLayout::eTransferDstOptimal,
                                   {.depth = 1.0f, .stencil = 0}, range);
        if (source) {
            barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
            barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            cmd.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, depth,
                          vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        }
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests;
        barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
}

void TemporalUpscaler::RunUiOnly(VideoCore::ImageId color_id, VideoCore::ImageId depth_id) {
    if (auto* p_profiler = GpuProfiler::Get()) {
        p_profiler->Mark(0xF5A1'0000ull, [] { return std::string{"upscaler RunUiOnly"}; });
    }
    const auto& color = m_texture_cache.GetImage(color_id);
    const auto depth_format = depth_id ? m_texture_cache.GetImage(depth_id).info.pixel_format
                                      : vk::Format::eD32SfloatS8Uint;
    if (!EnsureUiResources(m_target_width, m_target_height, color.info.pixel_format, depth_format)) {
        return;
    }
    PrepareUiDepth(depth_id);
    // Copy only the background already drawn. The subsequent menu/HUD/text draws execute
    // directly at output resolution, including frames with no scene or camera at all.
    vk::Image source = color.GetImage();
    auto source_layout = vk::ImageLayout::eTransferSrcOptimal;
    u32 source_width = color.info.size.width, source_height = color.info.size.height;
    if (!m_is_scaled_session && m_camera_motion.Depth() && m_scene_targets.Reduced() &&
        m_scene_targets.EligibleScene(color)) {
        VideoCore::ImageViewInfo view;
        view.format = color.info.pixel_format;
        const auto proxy = m_scene_targets.Read(color_id, view,
                                              vk::PipelineStageFlagBits2::eTransfer,
                                              vk::AccessFlagBits2::eTransferRead);
        if (!proxy.image) {
            return; // SceneTargets logged the missing proxy
        }
        source = proxy.image;
        source_layout = proxy.layout;
        source_width = m_render_width;
        source_height = m_render_height;
    } else {
        m_runtime.Transit(&m_texture_cache.GetImage(color_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        m_runtime.FlushBarriers();
    }
    // psport: without a scene (a title screen, loading screens) on an output of another aspect ratio,
    // the frame past the game's 16:9 screen is black: a game whose UI follows the output fills its
    // movies' whole (wider) view with their background colour there, as it fills the screen at 16:9.
    const auto edges = UiComposition::ScreenEdges(m_target_width, m_target_height);
    const bool is_screen_only = !m_camera_motion.Depth() && (edges[0] < 1.0f || edges[1] < 1.0f);
    const auto part = [](u32 size, float edge, bool is_high) {
        return s32(std::lround(float(size) * (is_high ? 1.0f + edge : 1.0f - edge) * 0.5f));
    };
    const float ex = is_screen_only ? edges[0] : 1.0f, ey = is_screen_only ? edges[1] : 1.0f;
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{part(source_width, ex, false), part(source_height, ey, false), 0},
                                 vk::Offset3D{part(source_width, ex, true), part(source_height, ey, true), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{part(m_ui_width, ex, false), part(m_ui_height, ey, false), 0},
                                 vk::Offset3D{part(m_ui_width, ex, true), part(m_ui_height, ey, true), 1}},
    };
    m_scheduler.Record([source, source_layout, ui = vk::Image(m_ui_image), region,
                        is_screen_only](vk::CommandBuffer cmd) {
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = ui,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        if (is_screen_only) {
            const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
            const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            cmd.clearColorImage(ui, vk::ImageLayout::eTransferDstOptimal, black, all);
            const vk::MemoryBarrier2 clear_then_blit{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            };
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &clear_then_blit});
        }
        cmd.blitImage(source, source_layout, ui, vk::ImageLayout::eTransferDstOptimal, region,
                      vk::Filter::eLinear);
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                                vk::AccessFlagBits2::eColorAttachmentWrite;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    m_should_reset = true; // returning to 3D must not reuse history from before a menu/loading screen
    m_is_done_this_frame = m_is_ui_phase = true;
    m_ui_color = color_id;
    m_ui_depth = depth_id;
}

void TemporalUpscaler::RunScaled() {
    if (auto* p_profiler = GpuProfiler::Get()) {
        const char* sz_label = PsSettings::Get().upscaler == PsSettings::UpscalerTaa
            ? "upscaler RunScaled (TAA)" : "upscaler RunScaled (FSR)";
        p_profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(sz_label),
                       [sz_label] { return std::string{sz_label}; });
    }
    auto& color = m_texture_cache.GetImage(m_ldr_target);
    auto& depth = m_texture_cache.GetImage(m_camera_motion.Depth());
    // The scene fills the top-left w x h of its targets (iw x ih, aligned by the game).
    const u32 iw = color.info.size.width, ih = color.info.size.height;
    const auto [w, h] = SceneSize(iw, ih);
    const u32 ow = m_target_width, oh = m_target_height;
    if (depth.info.size.width != iw || depth.info.size.height != ih || w > ow || h > oh) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, false)) {
        m_has_failed = true;
        return;
    }
    if (!EnsureUiResources(ow, oh, color.info.pixel_format, depth.info.pixel_format)) {
        m_has_failed = true;
        return;
    }
    PrepareUiDepth(m_camera_motion.Depth());
    const auto depth_format = depth.info.pixel_format;
    // Views kept across frames: FSR 4 registers images by view in a registry of eight.
    vk::Image depth_image = depth.GetImage(), color_image = color.GetImage();
    vk::ImageView depth_view{}, color_view{};
    u32 source_width = iw, source_height = ih;
    if (!m_is_scaled_session) {
        VideoCore::ImageViewInfo ci, di;
        ci.format = color.info.pixel_format;
        di.format = depth_format;
        const auto cp = m_scene_targets.Read(m_ldr_target, ci);
        const auto dp = m_scene_targets.Read(m_camera_motion.Depth(), di);
        if (!cp.image || !dp.image) {
            return; // SceneTargets logged the missing proxy
        }
        color_image = cp.image;
        color_view = cp.view;
        depth_image = dp.image;
        depth_view = dp.view;
        source_width = w;
        source_height = h;
    } else {
        depth_view = CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
        color_view = CachedView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        m_runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        m_runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        m_runtime.FlushBarriers();
    }

    m_scheduler.EndRendering();
    // psport: the motion pass and FSR 3 are recorded through the scheduler, on a recording
    // thread (recording them here cost the draw recording thread ~5% of its time, plus a wait
    // for the recording thread every frame). FSR 4 and TAA keep their frame bookkeeping on
    // this thread and record directly below.
    const auto barrier = [&](vk::Image image, vk::ImageAspectFlags aspect,
                             vk::ImageLayout old_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::ImageLayout new_layout,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };
        m_scheduler.Record([b](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto color_access = vk::AccessFlagBits2::eColorAttachmentRead |
                              vk::AccessFlagBits2::eColorAttachmentWrite;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // The previous frame's display pass read it: all commands before.
    barrier(vk::Image(m_ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);
    barrier(vk::Image(m_motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    m_camera_motion.RecordMotion(depth_view, *m_motion_view, w, h);
    if (m_camera_motion.TakeCut()) m_should_reset = true;
    barrier(vk::Image(m_motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - m_last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    m_last_frame = now;

    // psport: FSR 4 writes its HDR-format output, copied into the output-size UI image.
    if (UseFsr4() || UseDlss() || PsSettings::Get().upscaler == PsSettings::UpscalerTaa) {
        const auto cmdbuf = m_scheduler.CommandBuffer(); // after the commands recorded above
        barrier(vk::Image(m_output_image), vk::ImageAspectFlagBits::eColor,
                vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
                vk::ImageLayout::eGeneral, all, rw);
        bool is_ok4 = true;
        if (PsSettings::Get().upscaler == PsSettings::UpscalerTaa) {
            RecordTaa(cmdbuf, color_view, depth_view);
        } else if (UseDlss()) {
            m_is_fsr4_linear_frame = false;
            is_ok4 = RecordDlss(cmdbuf,
                             {color_image, color_view, color.info.pixel_format,
                              vk::ImageAspectFlagBits::eColor, source_width, source_height},
                             {depth_image, depth_view, depth_format, vk::ImageAspectFlagBits::eDepth,
                              source_width, source_height},
                             w, h, ow, oh, frame_ms, false);
        } else {
            // psport: FSR 4 treats its colour as linear light; this frame is the game's
            // tonemapped, sRGB-encoded one. Decoded first, the output encoded again below.
            // PS_FSR4_LINEAR=0 or toggle bit 63: the encoded frame as before (A/B).
            static const bool s_is_linear_requested = [] {
                const char* sz_env = std::getenv("PS_FSR4_LINEAR");
                return !sz_env || sz_env[0] != '0';
            }();
            // An sRGB-format frame is decoded by its view already (and encoded by the blit).
            const bool is_srgb_format =
                vk::to_string(color.info.pixel_format).find("Srgb") != std::string::npos;
            m_is_fsr4_linear_frame = s_is_linear_requested && m_fsr4_linear_image && !is_srgb_format &&
                                !PsToggle::Disabled(PsToggle::Fsr4EncodedInput);
            Fsr4Upscaler::Image input{color_image, color_view, source_width, source_height};
            {
                static int s_reported = -1;
                if (int(m_is_fsr4_linear_frame) != s_reported) {
                    s_reported = int(m_is_fsr4_linear_frame);
                    std::printf("Upscaler: FSR 4 input %s (%s)\n",
                                m_is_fsr4_linear_frame ? "decoded to linear light" : "as it is",
                                vk::to_string(color.info.pixel_format).c_str());
                }
            }
            if (m_is_fsr4_linear_frame) {
                const vk::ImageMemoryBarrier2 to_write{
                    .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eNone,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                    .image = vk::Image(m_fsr4_linear_image),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                const vk::MemoryBarrier2 source_ready{
                    .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &source_ready,
                                         .imageMemoryBarrierCount = 1,
                                         .pImageMemoryBarriers = &to_write});
                const vk::DescriptorImageInfo src{.imageView = color_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
                const vk::DescriptorImageInfo dst{.imageView = *m_fsr4_linear_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
                const std::array<vk::WriteDescriptorSet, 2> writes{{
                    {.dstBinding = 0, .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &src},
                    {.dstBinding = 1, .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &dst},
                }};
                const u32 unused = 0;
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_fsr4_decode_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                            *m_fsr4_decode_pipeline_layout, 0, writes);
                cmdbuf.pushConstants(*m_fsr4_decode_pipeline_layout,
                                     vk::ShaderStageFlagBits::eCompute, 0, sizeof(unused), &unused);
                cmdbuf.dispatch((source_width + 7) / 8, (source_height + 7) / 8, 1);
                const vk::MemoryBarrier2 decoded{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .dstStageMask = all,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &decoded});
                input = {vk::Image(m_fsr4_linear_image), *m_fsr4_linear_view, source_width,
                         source_height};
            }
            is_ok4 = RecordFsr4(cmdbuf, input,
                            {depth_image, depth_view, source_width, source_height}, w, h, ow,
                            oh, frame_ms);
            if (is_ok4 && m_is_fsr4_linear_frame) {
                const vk::MemoryBarrier2 upscaled{
                    .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                                     vk::AccessFlagBits2::eShaderStorageWrite};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &upscaled});
                const vk::DescriptorImageInfo out{.imageView = *m_output_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
                const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out};
                const u32 unused = 0;
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_fsr4_encode_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                            *m_fsr4_encode_pipeline_layout, 0, write);
                cmdbuf.pushConstants(*m_fsr4_encode_pipeline_layout,
                                     vk::ShaderStageFlagBits::eCompute, 0, sizeof(unused), &unused);
                cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
                const vk::MemoryBarrier2 encoded{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .dstStageMask = all,
                    .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                     vk::AccessFlagBits2::eMemoryWrite};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &encoded});
            }
            // The game's (encoded) frame, as the output is encoded again by now.
            if (is_ok4 && m_is_mask_ready) {
                RecordFsr4Reactive(cmdbuf, color_view, w, h, ow, oh);
            }
        }
        if (is_ok4) {
            if (PsSettings::Get().upscaler != PsSettings::UpscalerTaa) {
                ExtraSharpen(vk::Image(m_output_image), false, ow, oh);
            }
            barrier(vk::Image(m_output_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
            barrier(vk::Image(m_ui_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
            const vk::ImageBlit region{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ow), s32(oh), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ow), s32(oh), 1}},
            };
            cmdbuf.blitImage(vk::Image(m_output_image), vk::ImageLayout::eGeneral,
                             vk::Image(m_ui_image), vk::ImageLayout::eGeneral, region,
                             vk::Filter::eNearest);
            barrier(vk::Image(m_ui_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                    vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, color_access);
            m_should_reset = false;
            m_is_dispatched_last_frame = true;
            if (const int dump = DumpFrame(); dump >= 0) {
                m_camera_motion.PrintState(dump);
                DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                           {{color_image, source_width, source_height, 4, "input", "rgba"},
                            {vk::Image(m_motion_image), w, h, 4, "motion", "rg16f"},
                            {vk::Image(m_output_image), ow, oh, 8, "output", "rgba16f"}});
                DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                           {{depth_image, source_width, source_height, 4, "depth", "f32",
                             vk::ImageAspectFlagBits::eDepth}});
                if (PsSettings::Get().upscaler == PsSettings::UpscalerTaa &&
                    std::getenv("PS_TAA_DIAGNOSTICS")) {
                    DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                               {{vk::Image(m_taa_history[m_taa_next]),ow,oh,16,
                                 "previous_history","rgba32f"},
                                {vk::Image(m_opaque_image),ow,oh,8,"history_sample","rgba16f"}});
                }
                // ObjectMotion's attachment can be in a different resolution from the FSR
                // inputs: only dump it when it matches, to avoid an out-of-bounds GPU copy.
                if (const auto object = m_camera_motion.ObjectMotionImage(w, h); object) {
                    DumpImages(m_instance, m_scheduler, cmdbuf, dump,
                               {{object, w, h, 16, "objects", "rgba32f"}});
                }
            }
        }
        m_is_done_this_frame = true;
        if (is_ok4) {
            m_is_ui_phase = true;
            m_ui_color = m_ldr_target;
            m_ui_depth = m_camera_motion.Depth();
        }
        return;
    }

    const auto& settings = PsSettings::Get();
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.color = Describe(color_image, color.info.pixel_format, source_width, source_height,
                          color.usage_flags, vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(depth_image, depth_format, source_width, source_height, depth.usage_flags,
                          vk::ImageAspectFlagBits::eDepth,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(vk::Image(m_motion_image), vk::Format::eR16G16Sfloat, w, h,
                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(vk::Image(m_ui_image), vk::Format::eR8G8B8A8Unorm, ow, oh,
                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eColorAttachment,
                           vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    if (m_is_mask_ready && ReactiveOn()) {
        info.reactiveMask = Describe(vk::Image(m_reactive_image), vk::Format::eR8Unorm, w, h,
            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    }

    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    const float sign = PsToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    info.jitterOffset = {sign * m_jitter[0], sign * m_jitter[1]};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = {w, h};
    info.outputSize = {ow, oh};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = m_camera_motion.Near();
    info.cameraFar = 3000.0f;
    info.cameraVerticalFovRadians = m_camera_motion.VerticalFov();
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = std::min(settings.sharpness.load(), 1.0f);
    info.enableSharpening = settings.should_sharpen ? VK_TRUE : VK_FALSE;
    info.reset = m_should_reset ? VK_TRUE : VK_FALSE;
    info.frameId = m_frame_id++;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    // The command buffer is the recording thread's, known when the dispatch is recorded: the
    // check sees a placeholder.
    auto checked = info;
    checked.commandBuffer = reinterpret_cast<VkCommandBuffer>(uintptr_t{1});
    bool is_ok = false;
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &checked)) {
        PrintIssues("scaled dispatch", issues);
        m_has_failed = true;
    } else {
        // The context is only used here and destroyed after scheduler.Finish(); a dispatch
        // that fails sets `m_has_failed` at the next frame (the UI then draws over this one).
        m_scheduler.Record([this, ctx = m_p_context, info](vk::CommandBuffer cmdbuf) mutable {
            info.commandBuffer = cmdbuf;
            if (ffxVkPortableUpscaleContextRecordDispatch(ctx, &info) != FFX_VK_PORTABLE_OK) {
                std::printf("Upscaler: FSR 3 scaled dispatch failed\n");
                m_has_dispatch_failed.store(true, std::memory_order_relaxed);
            }
        });
        is_ok = true;
        m_should_reset = false;
        m_is_dispatched_last_frame = true;
        ExtraSharpen(vk::Image(m_ui_image), true, ow, oh);
    }
    barrier(vk::Image(m_ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral, all,
            rw, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            color_access);

    m_is_done_this_frame = true; // the UI is not jittered
    if (is_ok) {
        m_is_ui_phase = true;
        m_ui_color = m_ldr_target;
        m_ui_depth = m_camera_motion.Depth();
    }
}

vk::ImageView TemporalUpscaler::Mirror(vk::Image image, std::vector<MirrorView>& views,
                                       vk::Format format, vk::ComponentMapping mapping) {
    for (const auto& entry : views) {
        if (entry.format == format && entry.mapping == mapping) {
            return *entry.view;
        }
    }
    const vk::ImageViewUsageCreateInfo usage{
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment,
    };
    auto view = Check(m_instance.GetDevice().createImageViewUnique({
        .pNext = &usage,
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView handle = *view;
    views.push_back({format, mapping, std::move(view)});
    return handle;
}

bool TemporalUpscaler::RedirectColor(VideoCore::ImageId color,
                                     const VideoCore::ImageViewInfo& view_info, Target& target) {
    if (m_is_ui_phase && color == m_ui_color) {
        target = {Mirror(vk::Image(m_ui_image), m_ui_views, view_info.format, {}),
                  vk::ImageLayout::eGeneral, m_ui_width, m_ui_height, true};
        return true;
    }
    const auto& image = m_texture_cache.GetImage(color);
    const VAddr address = image.info.guest_address;
    if (!FrameCapture::IsDisplayBuffer(address)) {
        m_is_display_redirect = false;
        return false;
    }
    std::scoped_lock lock{m_display_mutex};
    auto& display = m_displays[address];
    if (!m_is_display_redirect) {
        display.is_valid = false; // not upscaled: the presenter shows the guest buffer
        return false;
    }
    if (!display.image || display.format != image.info.pixel_format ||
        display.width != m_ui_width || display.height != m_ui_height) {
        const auto device = m_instance.GetDevice();
        m_scheduler.Finish();
        display.views.clear();
        std::printf("Display: host buffer %ux%u (live)\n", m_ui_width, m_ui_height);
        display.format = image.info.pixel_format;
        display.width = m_ui_width;
        display.height = m_ui_height;
        display.image = VideoCore::UniqueImage(device, m_instance.GetAllocator());
        if (display.image.Create(vk::ImageCreateInfo{
                .flags = vk::ImageCreateFlagBits::eMutableFormat,
                .imageType = vk::ImageType::e2D,
                .format = display.format,
                .extent = {m_ui_width, m_ui_height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferSrc,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            SetObjectName(device, vk::Image(display.image), "psport: display {:#x} {}x{}", address, m_ui_width,
                          m_ui_height);
        }
        display.views.clear();
        if (!display.image) {
            LOG_ERROR_COUNTED(Render_Vulkan,
                              "Display buffer {:#x} not allocated at {}x{}, the game's buffer shown",
                              address, m_ui_width, m_ui_height);
            display.is_valid = false;
            return false;
        }
    }
    m_scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 b{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(display.image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    m_scheduler.Record([b](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    });
    display.is_valid = true;
    target = {Mirror(vk::Image(display.image), display.views, view_info.format, {}),
              vk::ImageLayout::eGeneral, m_ui_width, m_ui_height};
    return true;
}

bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId depth, Target& target) {
    if (!m_is_ui_phase || depth != m_ui_depth) {
        return false;
    }
    target = {*m_ui_depth_view, vk::ImageLayout::eGeneral, m_ui_width, m_ui_height, true};
    return true;
}

bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId image,
                                       const VideoCore::ImageViewInfo& info, vk::ImageView& view,
                                       vk::ImageLayout& layout) {
    if (!m_is_display_redirect || image != m_ui_color) {
        return false;
    }
    if (!m_is_ui_read_barrier) {
        m_is_ui_read_barrier = true;
        m_scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(m_ui_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        m_scheduler.Record([b](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    }
    view = Mirror(vk::Image(m_ui_image), m_ui_views, info.format, info.mapping);
    layout = vk::ImageLayout::eGeneral;
    return true;
}

bool TemporalUpscaler::DisplayOverride(VAddr va_display, Display& display) {
    std::scoped_lock lock{m_display_mutex};
    const auto it = m_displays.find(va_display);
    if (it == m_displays.end() || !it->second.is_valid) {
        return false;
    }
    display = {vk::Image(it->second.image), it->second.format, it->second.width, it->second.height};
    if (PresentDumpDue()) {
        DumpPresented(display.image, display.width, display.height, display.format);
    }
    return true;
}

bool TemporalUpscaler::PresentDumpDue() {
    // Diagnostic capture of the actual completed display buffer, including native UI.
    // PS_PRESENT_DUMP_COUNT=N: the trigger dumps N consecutive frames (temporal stability).
    static const char* s_sz_dump = std::getenv("PS_PRESENT_DUMP_TRIGGER");
    static const int s_c_dumps = [] {
        const char* sz_env = std::getenv("PS_PRESENT_DUMP_COUNT");
        return sz_env ? std::max(1, std::atoi(sz_env)) : 1;
    }();
    if (s_sz_dump && m_c_present_dumps_remaining == 0 && std::remove(s_sz_dump) == 0) {
        m_c_present_dumps_remaining = s_c_dumps;
    }
    if (m_c_present_dumps_remaining == 0) {
        return false;
    }
    --m_c_present_dumps_remaining;
    return true;
}

void TemporalUpscaler::DumpPresented(vk::Image image, u32 width, u32 height, vk::Format format) {
    m_scheduler.EndRendering();
    DumpImages(m_instance, m_scheduler, m_scheduler.CommandBuffer(), m_i_present_dump++,
               {{image, width, height, 4, "present", ColorDumpLayout(format)}});
}

} // namespace Vulkan

namespace Vulkan {

bool TemporalUpscaler::UseFsr4() const {
    const int selected = PsSettings::Get().upscaler;
    const bool is_supported = selected == PsSettings::UpscalerFsr411
                               ? m_instance.IsFsr411Supported()
                               : m_instance.IsFsr4Int8Supported();
    return PsSettings::IsFsr4(selected) && is_supported && !m_has_fsr4_failed;
}

bool TemporalUpscaler::RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color,
                                  Fsr4Upscaler::Image depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms) {
    const auto& settings = PsSettings::Get();
    // Same jitter convention as FSR 3; the menu (or toggle 1 << 26) flips it for tests.
    const float sign =
        PsToggle::Disabled(1u << 26) != settings.should_invert_fsr4_jitter.load() ? -1.0f : 1.0f;
    const bool is_ok = m_fsr4->Record({
        .cmdbuf = cmdbuf,
        .color = color,
        .depth = depth,
        .motion = {vk::Image(m_motion_image), *m_motion_view, w, h},
        .output = {vk::Image(m_output_image), *m_output_view, ow, oh},
        .render_width = w,
        .render_height = h,
        .preset = m_applied_preset,
        .jitter = {sign * m_jitter[0], sign * m_jitter[1]},
        .frame_ms = frame_ms,
        .near_plane = m_camera_motion.Near(),
        .far_plane = 3000.0f,
        .vertical_fov = m_camera_motion.VerticalFov(),
        .sharpness = std::min(settings.sharpness.load(), 1.0f),
        .should_sharpen = settings.should_sharpen,
        .should_reset = m_should_reset,
        .has_auto_exposure = settings.should_use_fsr4_auto_exposure,
    });
    // The menu shows the reason; it outlives this frame (FSR 4 keeps its last message).
    static std::string s_shown;
    const char* sz_problem = m_fsr4->Problem();
    if (!sz_problem) {
        PsSettings::Get().sz_fsr4_problem = nullptr;
    } else if (s_shown != sz_problem) {
        s_shown = sz_problem;
        static std::array<std::string, 8> s_kept;
        static u32 s_next = 0;
        s_kept[s_next] = s_shown;
        PsSettings::Get().sz_fsr4_problem = s_kept[s_next].c_str();
        s_next = (s_next + 1) % s_kept.size();
    }
    if (!is_ok && m_fsr4->Fatal()) {
        std::printf("Upscaler: falling back to FSR 3.1\n");
        PsSettings::Get().upscaler = PsSettings::UpscalerFsr3;
        m_has_fsr4_failed = true; // EnsureResources creates the FSR 3 context next frame
        m_should_reset = true;
    }
    return is_ok;
}

bool TemporalUpscaler::UseDlss() const {
    const Dlss* p_dlss = Dlss::Get();
    return PsSettings::Get().upscaler == PsSettings::UpscalerDlss && p_dlss && p_dlss->Available() &&
           !m_has_dlss_failed;
}

bool TemporalUpscaler::RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                                  const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms, bool is_hdr) {
    Dlss* p_dlss = Dlss::Get();
    const Dlss::FeatureDesc desc{w, h, ow, oh, Dlss::QualityForScale(float(ow) / float(w)), is_hdr};
    bool is_ok = true;
    if (!p_dlss->HasFeature(desc)) {
        // The previous feature may still be in use by submitted work; `cmdbuf` stays open.
        m_scheduler.WaitSubmittedWork();
        p_dlss->ReleaseFeature();
        is_ok = p_dlss->CreateFeature(cmdbuf, desc);
        m_should_reset = true;
    }
    if (is_ok) {
        // The jitter and motion vectors FSR 3 gets: render pixels, current to previous.
        const float sign = PsToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        const auto& settings = PsSettings::Get();
        is_ok = p_dlss->Evaluate(cmdbuf, {
            .color = color,
            .depth = depth,
            .motion = {vk::Image(m_motion_image), *m_motion_view, vk::Format::eR16G16Sfloat,
                       vk::ImageAspectFlagBits::eColor, w, h},
            .output = {vk::Image(m_output_image), *m_output_view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, ow, oh},
            .jitter_x = sign * m_jitter[0],
            .jitter_y = sign * m_jitter[1],
            .should_reset = m_should_reset,
            .frame_ms = frame_ms,
            .sharpness = settings.should_sharpen ? std::min(settings.sharpness.load(), 1.0f) : 0.0f,
        });
    }
    if (!is_ok) {
        std::printf("Upscaler: DLSS failed; falling back to FSR 3.1\n");
        PsSettings::Get().upscaler = PsSettings::UpscalerFsr3;
        m_has_dlss_failed = true; // EnsureResources creates the FSR 3 context next frame
        m_should_reset = true;
    }
    return is_ok;
}

} // namespace Vulkan
