// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "game_profile.h"
#include "psport_overlay.h"
#include "psport_settings.h"
#include "psport_renderdoc.h"
#include "psport_timeline.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// psport: screenshot capture and ImGui overlays removed; the port presents directly.

namespace {
// psport: decided before the swapchain is made, whose image count follows frame generation.
void ConfigureFrameGen(const Instance& instance) {
    static std::string s_result;
    PsSettings::ConfigureFrameGenSupport(FrameGenerator::IsFsrSupported(instance, s_result), s_result.c_str());
    auto& settings = PsSettings::Get();
    // Frame generation reads the scene's depth and motion where the game profile places the
    // upscaler; without that placement it has nothing to interpolate from.
    if (settings.frame_gen != PsSettings::FrameGenOff && !IsUpscalerAvailable()) {
        settings.sz_frame_gen_problem = "no upscaler placement for this game (scene depth and motion)";
        settings.frame_gen = PsSettings::FrameGenOff;
        std::printf("Frame generation: off, the game has no upscaler placement (its profile's scene "
                    "combine shader), so no scene depth or motion vectors\n");
    }
    settings.is_frame_gen_live = settings.frame_gen != PsSettings::FrameGenOff;
}
} // namespace

std::unique_ptr<Presenter> Presenter::Create(Frontend::WindowSDL& window,
                                             AmdGpu::Liverpool* liverpool) {
    auto p_instance = std::make_unique<Instance>(window, EmulatorSettings.GetGpuId(),
                                                 EmulatorSettings.IsVkValidationEnabled(),
                                                 EmulatorSettings.IsVkCrashDiagnosticEnabled());
    if (!p_instance->IsCreated()) {
        return nullptr;
    }
    ConfigureFrameGen(*p_instance);
    auto p_presenter = std::make_unique<Presenter>(window, liverpool, std::move(p_instance));
    if (HasInitFailed()) {
        // Not destroyed: its teardown would use the objects that failed, and the process exits.
        static_cast<void>(p_presenter.release());
        return nullptr;
    }
    return p_presenter;
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_,
                     std::unique_ptr<Instance> p_instance)
    : window{window_}, m_p_instance{std::move(p_instance)}, instance{*m_p_instance},
      liverpool{liverpool_}, draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    if (PsSettings::Get().frame_gen != PsSettings::FrameGenOff) {
        m_p_frame_generator = std::make_unique<FrameGenerator>(instance, draw_scheduler);
        rasterizer->GetUpscaler().SetFrameGenerator(m_p_frame_generator.get());
    }
    // Composed in RGBA8 for frame generation, which writes storage images (B8G8R8A8 storage is
    // optional), and blitted into the swapchain's format.
    m_frame_format = m_p_frame_generator ? FrameGenerator::ColorFormat : swapchain.GetSurfaceFormat().format;
    // Named at start because a generated frame decoded apart from the game's, through its own sRGB
    // view, lifts its blacks on some drivers and strobes at the refresh rate; here generation reads
    // frames the same pass already decoded and composed.
    if (m_p_frame_generator) {
        std::printf("Frame generation: sampling: display buffer and picture before the UI decoded by their "
                    "sRGB views in the presenter's pass; the composed %s frames interpolated and blitted "
                    "unsampled into the %s swapchain\n",
                    vk::to_string(m_frame_format).c_str(),
                    vk::to_string(swapchain.GetSurfaceFormat().format).c_str());
    }
    // The overlay draws over every presented image and FSR's pass runs again for the picture without
    // UI: their per-image buffers must not be reused while one of them is still in use.
    const u32 c_pass_images = num_images * (m_p_frame_generator ? 2 : 1);

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
        // psport: frame images are made here rather than at the first frame, so that a failure
        // fails the start instead of leaving a frame without an image.
        if (!fence || (!HasInitFailed() &&
                       !RecreateFrame(&frame, expected_frame_width, expected_frame_height))) {
            ReportInitFailure();
        }
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    if (!fsr_pass.Create(device, instance.GetAllocator(), c_pass_images) ||
        !pp_pass.Create(device, m_frame_format)) {
        ReportInitFailure();
    }
    PsOverlay::Init(instance, swapchain.GetSurfaceFormat().format, c_pass_images);

}

Presenter::~Presenter() {
    rasterizer->GetUpscaler().SetFrameGenerator(nullptr);

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        DestroyGenerated(&frame);
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    if (m_hudless_frame.image) {
        vmaDestroyImage(instance.GetAllocator(), m_hudless_frame.image, m_hudless_frame.allocation);
        device.destroyImageView(m_hudless_frame.image_view);
    }
}

void Presenter::DestroyGenerated(Frame* frame) {
    if (frame->generated_view) {
        instance.GetDevice().destroyImageView(frame->generated_view);
    }
    if (frame->generated_image) {
        vmaDestroyImage(instance.GetAllocator(), frame->generated_image, frame->generated_allocation);
    }
    frame->generated_view = vk::ImageView{};
    frame->generated_image = vk::Image{};
    frame->generated_allocation = {};
    frame->has_generated = false;
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

bool Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    const vk::Format format = m_frame_format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VmaAllocation allocation{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &allocation, nullptr);
    if (result != VK_SUCCESS) {
        // psport: over the budget rather than without a frame to present.
        alloc_info.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                &unsafe_image, &allocation, nullptr);
    }
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_ERROR_COUNTED(Render_Vulkan, "Failed allocating a {}x{} frame image, the frame keeps "
                                         "its previous one: {}",
                          width, height, vk::to_string(vk::Result{result}));
        return false;
    }
    const vk::Image image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    const vk::ImageView view = Check<"create frame image view">(device.createImageView(view_info));
    if (!view) {
        vmaDestroyImage(instance.GetAllocator(), image, allocation);
        return false;
    }

    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }
    DestroyGenerated(frame); // made again at the new size when needed
    frame->image = image;
    frame->allocation = allocation;
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
    return true;
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            StopOnDeviceLost("waiting for a frame");
        }
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    LOG_ERROR_COUNTED(Render_Vulkan, "Unknown display buffer format {:#x}, shown as B8G8R8A8 sRGB",
                      static_cast<u32>(format));
    return vk::Format::eB8G8R8A8Srgb;
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    PsTimeline::Note(PsTimeline::PipeTask, 4, draw_scheduler.CurrentTick());
    // psport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        if (!image_id) {
            return PrepareBlankFrame(false);
        }
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    // psport: recorded like draws, on the recording threads. CommandBuffer() would first wait
    // for them to record everything handed over before (~90 us a frame of the draw recording
    // thread) and then submit from this thread.
    draw_scheduler.EndRendering();

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        draw_scheduler.Record([image = display.image](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
        });
    } else {
        auto& image = texture_cache.GetImage(image_id);
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        // psport: PS_PRESENT_DUMP_TRIGGER for the game's own display buffer too.
        if (auto& upscaler = rasterizer->GetUpscaler(); upscaler.PresentDumpDue()) {
            runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            upscaler.DumpPresented(image.GetImage(), image_size.width, image_size.height,
                                   view_info.format);
        }
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;

    // Numbered here, in command order; marked where the commands are recorded.
    const u32 stream = draw_scheduler.CrumbStream();
    const bool crumbs = Breadcrumbs::Enabled();
    const u32 fsr_crumb = crumbs ? Breadcrumbs::Note(stream, {.sz_name = "presenter FSR"}) : 0;
    const u32 pp_crumb = crumbs ? Breadcrumbs::Note(stream, {.sz_name = "presenter post process"}) : 0;
    draw_scheduler.Record([this, frame, image_view, image_size, frame_subresources, stream,
                           fsr_crumb, pp_crumb, fsr = fsr_settings,
                           pp = pp_settings](vk::CommandBuffer cmdbuf) {
        // Frames of different submissions may be recorded on two threads at once.
        std::scoped_lock lock{passes_mutex};
        const auto pre_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = frame->image,
            .subresourceRange{frame_subresources},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, false);
        }
        const vk::ImageView input = fsr_pass.Render(cmdbuf, image_view, image_size,
                                                    {frame->width, frame->height}, fsr,
                                                    frame->is_hdr);
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, true);
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, false);
        }
        pp_pass.Render(cmdbuf, input, image_size, *frame, pp);
        if (pp_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, true);
        }
    });

    frame->has_generated = false;
    frame->frame_gen_skip = FrameGenSkip::None;
    if (m_p_frame_generator) {
        GenerateFrame(frame, view_info.format, image_size);
    }

    // Flush frame creation commands.
    PsStats::g_frame_number.fetch_add(1, std::memory_order_relaxed); // psport: game frames shown
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    PsTimeline::Note(PsTimeline::PipeTask, 5, frame->ready_tick);

    // psport: the GPU command thread runs at most PS_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("PS_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                PsStats::WaitTimer timer{PsStats::g_present_wait_ns};
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->has_generated = false;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::WaitRendered(const Frame* frame) {
    if (!frame || !frame->ready_semaphore) {
        return;
    }
    const vk::SemaphoreWaitInfo info{
        .semaphoreCount = 1,
        .pSemaphores = &frame->ready_semaphore,
        .pValues = &frame->ready_tick,
    };
    // A lost device ends the wait (the presenter reports it); a timeout waits again.
    while (instance.GetDevice().waitSemaphores(&info, 1'000'000'000) == vk::Result::eTimeout) {
    }
}

void Presenter::GenerateFrame(Frame* frame, vk::Format view_format, vk::Extent2D image_size) {
    FrameGenerator& generator = *m_p_frame_generator;
    if (!generator.IsWanted()) {
        frame->frame_gen_skip = PsSettings::Get().sz_frame_gen_problem.load() ? FrameGenSkip::Failed
                                                                               : FrameGenSkip::SwitchedOff;
        generator.Release();
        // GetRenderFrame waited for this frame's presentation, the generated frame's blit included.
        DestroyGenerated(frame);
        if (m_hudless_frame.image) {
            // Earlier frames' commands may still read it.
            draw_scheduler.DeferOperation([device = instance.GetDevice(), allocator = instance.GetAllocator(),
                                           image = m_hudless_frame.image, allocation = m_hudless_frame.allocation,
                                           view = m_hudless_frame.image_view] {
                device.destroyImageView(view);
                vmaDestroyImage(allocator, image, allocation);
            });
            m_hudless_frame.image = vk::Image{};
            m_hudless_frame.image_view = vk::ImageView{};
            m_hudless_frame.allocation = {};
            m_hudless_frame.width = m_hudless_frame.height = 0;
        }
        return;
    }
    if (frame->is_hdr) {
        frame->frame_gen_skip = FrameGenSkip::HdrOutput;
        generator.Skip();
        return;
    }
    const bool should_generate = m_c_frame_gen_limit.load(std::memory_order_relaxed) > 0;
    if (should_generate && !frame->generated_image) {
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = FrameGenerator::ColorFormat,
            .extent = {frame->width, frame->height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
        });
        const VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &frame->generated_allocation, nullptr) != VK_SUCCESS) {
            LOG_ERROR_COUNTED(Render_Vulkan, "Frame generation: no memory for a {}x{} generated frame, the "
                                             "frame is shown without one",
                              frame->width, frame->height);
            frame->generated_allocation = {};
            frame->frame_gen_skip = FrameGenSkip::NoMemory;
            generator.Skip();
            return;
        }
        frame->generated_image = vk::Image{image};
        SetObjectName(instance.GetDevice(), frame->generated_image, "Generated frame #{}", frame->id);
        frame->generated_view = Check<"create a generated frame view">(instance.GetDevice().createImageView({
            .image = frame->generated_image,
            .viewType = vk::ImageViewType::e2D,
            .format = FrameGenerator::ColorFormat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        if (!frame->generated_view) {
            DestroyGenerated(frame);
            frame->frame_gen_skip = FrameGenSkip::NoMemory;
            generator.Skip();
            return;
        }
    }
    // Recorded on this thread, in order after the passes handed to the recording threads: the
    // generator's state is the GPU thread's.
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    std::scoped_lock lock{passes_mutex};
    // The picture before the UI, composed like the frame (equal to it outside the UI).
    vk::Image hudless{};
    vk::ImageView hudless_view{};
    vk::Extent2D hudless_size{};
    if (const auto source = generator.Hudless(view_format, hudless_size);
        should_generate && source && hudless_size == image_size) {
        if (m_hudless_frame.width != frame->width || m_hudless_frame.height != frame->height ||
            !m_hudless_frame.image) {
            // A single image: the previous frames' commands may still read the old one.
            draw_scheduler.WaitSubmittedWork();
            RecreateFrame(&m_hudless_frame, frame->width, frame->height);
        }
        if (m_hudless_frame.image && m_hudless_frame.width == frame->width &&
            m_hudless_frame.height == frame->height) {
            const vk::ImageMemoryBarrier2 to_target{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = m_hudless_frame.image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
            const vk::ImageView input = fsr_pass.Render(cmdbuf, source, hudless_size,
                                                        {frame->width, frame->height}, fsr_settings, frame->is_hdr);
            pp_pass.Render(cmdbuf, input, hudless_size, m_hudless_frame, pp_settings);
            hudless = m_hudless_frame.image;
            hudless_view = m_hudless_frame.image_view;
        }
    }
    frame->frame_gen_skip = generator.Generate(
        cmdbuf, frame->image, frame->image_view,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
            vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
        hudless, hudless_view, frame->width, frame->height, {frame->generated_image, frame->generated_view},
        should_generate && frame->generated_image, m_frame_gen_held.load(std::memory_order_relaxed));
    frame->has_generated = frame->frame_gen_skip == FrameGenSkip::None;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    ZoneScopedNC("Present", RendererMarkerColor);
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // psport: nothing to present to while the window is minimised (a 0x0 surface, issue #31).
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }
    // Recreate the swapchain if the window was resized (or was minimised at the last try).
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        !swapchain.IsPresentable()) {
        // psport: recreating waits for the device idle, so no other thread may submit meanwhile.
        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
        if (!swapchain.IsPresentable()) {
            free_frame();
            return;
        }
    }

    // psport: frame generation: the generated frame first, then the game's own half a game frame
    // later (the generated one belongs to the time before this frame). A frame shown again has none.
    const bool is_new_game_frame = !is_reusing_frame && is_game_frame;
    bool has_generated = !is_reusing_frame && frame->has_generated;
    FrameGenSkip skip = frame->frame_gen_skip;
    const auto start = std::chrono::steady_clock::now();
    if (m_p_frame_generator && is_new_game_frame) {
        const double ms = std::chrono::duration<double, std::milli>(start - m_last_game_present).count();
        if (ms > 1.0 && ms < 100.0) {
            m_game_frame_ms += (ms - m_game_frame_ms) * 0.1;
        }
        m_last_game_present = start;
    }
    // The game must never wait for a generated frame: when every presentation frame is in use (the
    // GPU thread would block on the next one, and the game on its GPU work) it is dropped and the
    // game's frame goes out at once.
    const auto is_game_waiting = [this] {
        std::scoped_lock lock{free_mutex};
        return free_queue.empty();
    };
    if (has_generated && is_game_waiting()) {
        has_generated = false;
        skip = FrameGenSkip::GameWaited;
        ++FrameGenStats::g_c_dropped;
    }
    if (has_generated && PresentImage(frame, frame->generated_image, false)) {
        ++FrameGenStats::g_c_presented;
        // The system sleep is coarse: most of the way asleep, then spinning.
        const auto at = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>(m_game_frame_ms / 2.0));
        constexpr auto spin = std::chrono::microseconds(1500);
        if (const auto now = std::chrono::steady_clock::now(); at - now > spin && !is_game_waiting()) {
            std::this_thread::sleep_for(at - now - spin);
        }
        while (std::chrono::steady_clock::now() < at && !is_game_waiting()) {
            std::this_thread::yield();
        }
    } else if (has_generated) {
        has_generated = false;
        skip = FrameGenSkip::NoSwapchainImage;
    }
    if (!PresentImage(frame, frame->image, true)) {
        if (has_generated) {
            // The frame's fence was not reset and so does not cover the generated frame's blit: it
            // must be done before the frame is drawn again.
            present_scheduler.Finish();
        }
        free_frame();
        return;
    }
    if (m_p_frame_generator && is_new_game_frame) {
        ++FrameGenStats::g_c_game_frames;
        CountFrameGenSkip(skip);
        UpdateFrameGenLimit(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }

    const u32 width = frame->width, height = frame->height;
    free_frame();
    if (is_new_game_frame) {
        DebugState.IncFlipFrameNum();
        // psport: once, so that a log says whether the game ever got a frame on screen
        // (tools/regress.sh reads it as the gpu milestone).
        static std::atomic<bool> first_shown{false};
        if (!first_shown.exchange(true, std::memory_order_relaxed)) {
            std::printf("Presenter: first game frame presented (%ux%u)\n", width, height);
        }
        PsRenderDoc::OnPresent();
    }
}

void Presenter::UpdateFrameGenLimit(double present_ms) {
    auto& settings = PsSettings::Get();
    settings.game_fps.store(float(1000.0 / m_game_frame_ms), std::memory_order_relaxed);
    if (!m_p_frame_generator->IsWanted()) {
        m_c_frame_gen_limit = 0;
        settings.c_frame_gen_now = 0;
        settings.frame_gen_flip_limit = 0;
        m_present_busy = 0.0;
        return;
    }
    const double interval = m_game_frame_ms;
    const int known_hz = settings.display_refresh_hz.load(std::memory_order_relaxed);
    const u32 hz = known_hz > 0 ? u32(known_hz) : 60;
    const auto now = std::chrono::steady_clock::now();
    const u32 before = m_c_frame_gen_limit.load(std::memory_order_relaxed);
    u32 limit = 1;
    if (settings.should_fit_frame_gen_to_display) {
        // The images the display shows per game frame: 60 FPS on 144 Hz has room for a generated
        // frame, 90 FPS on 144 Hz has not. Decided once a second from that second's game frame rate
        // (single slow frames while an area loads do not count): off at once when 10% short, on
        // after two seconds with 5% to spare.
        limit = before;
        ++m_c_limit_frames;
        const double window_s = std::chrono::duration<double>(now - m_limit_window).count();
        if (window_s >= 1.0) {
            const double ratio = hz * window_s / m_c_limit_frames;
            m_limit_window = now;
            m_c_limit_frames = 0;
            if (window_s < 2.0) { // not the first, nor after a pause of the game
                if (limit > 0 && ratio < 2.0 * 0.9) {
                    limit = 0;
                    m_c_limit_up_seconds = 0;
                } else if (limit == 0 && ratio >= 2.0 * 0.95) {
                    if (++m_c_limit_up_seconds >= 2) {
                        limit = 1;
                        m_c_limit_up_seconds = 0;
                    }
                } else {
                    m_c_limit_up_seconds = 0;
                }
            }
        }
    }
    // The share of the game's frame interval spent presenting its images: paced, about half. Near
    // one the presentation holds the game back (FIFO on a display slower than measured, a GPU too
    // slow for generation): off for a while, longer each time. Each frame counts at most 1.2, so a
    // single long hitch (a load, a shader compile) is not taken for a display that cannot keep up;
    // about ten overloaded frames in a row are.
    m_present_busy += (std::min(present_ms / interval, 1.2) - m_present_busy) * 0.1;
    if (before && m_present_busy > 0.97) {
        m_frame_gen_hold_seconds = now - m_frame_gen_hold_until > std::chrono::seconds(60)
                                       ? 5
                                       : std::min(m_frame_gen_hold_seconds * 2, 120);
        m_frame_gen_hold_until = now + std::chrono::seconds(m_frame_gen_hold_seconds);
        ++FrameGenStats::g_c_pauses;
        std::printf("Frame generation: paused for %d s, presenting does not keep up (%.1f of %.1f ms per game "
                    "frame; FIFO present mode or a slower display?)\n",
                    m_frame_gen_hold_seconds, present_ms, interval);
        m_present_busy = 0.0;
    }
    if (now < m_frame_gen_hold_until) {
        limit = 0;
    }
    if (limit != before && settings.should_fit_frame_gen_to_display) {
        std::printf("Frame generation: %u generated per game frame (display %u Hz, game %.0f FPS)\n", limit, hz,
                    1000.0 / interval);
    }
    // FIFO (V-Sync) shows at most `hz` images a second and makes each one more wait for the
    // display: the game is capped so that its frames and the generated ones fit, e.g. 72 FPS on
    // 144 Hz (VideoOut applies it). Mailbox and Immediate keep the game's rate.
    const int flip_limit = swapchain.WaitsForDisplay() && limit ? int(hz / 2) : 0;
    if (flip_limit != settings.frame_gen_flip_limit.exchange(flip_limit) && flip_limit) {
        std::printf("Frame generation: V-Sync (FIFO) at %u Hz: the game is capped at %d FPS so its frames and "
                    "the generated ones fit\n",
                    hz, flip_limit);
    }
    m_frame_gen_held = now < m_frame_gen_hold_until ? FrameGenSkip::Paused : FrameGenSkip::NoRoom;
    m_c_frame_gen_limit = limit;
    settings.c_frame_gen_now = int(limit);
}

bool Presenter::PresentImage(Frame* frame, vk::Image image, bool is_last) {
    // psport: recreating waits for the device idle, so no other thread may submit meanwhile, as
    // around the recreation after a failed present below.
    const auto recreate_swapchain = [&] {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    };
    if (!swapchain.AcquireNextImage()) {
        recreate_swapchain();
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            return false;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    if (is_last && !Check<"reset a present done fence">(instance.GetDevice().resetFences(frame->present_done))) {
        // The acquired image is given back by recreating the swapchain; the frame is not shown.
        recreate_swapchain();
        return false;
    }

    // psport: the frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    if (is_last) {
        DumpFinalFrameIfDue(instance, scheduler, cmdbuf, image, frame->width, frame->height, m_frame_format);
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        PsSettings::Get().shown_frame_aspect.store(float(frame->width) / float(std::max(frame->height, 1u)),
                                                   std::memory_order_relaxed);
        // psport: the settings menu / FPS counter over the frame, at display resolution.
        if (const Game::Title* game = Game::Launched(); is_last && game && game->pfn_before_present) {
            game->pfn_before_present();
        }
        const bool overlay = PsOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            PsOverlay::Render(cmdbuf, swapchain.ImageView(), extent,
                              FitImage(frame->width, frame->height, extent.width, extent.height));
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    // psport: the frame's submission goes out from a recording thread (PS_ASYNC_SUBMIT); this
    // one, waiting for it on the same queue, must not get there first.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick);
    }
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    if (is_last) {
        info.AddSignal(frame->present_done);
    }
    scheduler.Flush(info);

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present()) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    return true;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            StopOnDeviceLost("waiting for a frame");
        }
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // minimised: keep the last size (a 0x0 frame image cannot be made, issue #31)
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
