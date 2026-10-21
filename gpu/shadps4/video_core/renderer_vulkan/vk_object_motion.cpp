// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_object_motion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vk_mem_alloc.h>
#include "psport_settings.h"
#include "psport_toggles.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"

namespace Vulkan {

namespace {

bool CreateBuffer(const Instance& instance, vk::DeviceSize cb_buffer, bool is_host, vk::Buffer& buffer,
                  VmaAllocation& allocation, void** pp_out_mapped, u64& address) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = cb_buffer,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (is_host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer h_raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &h_raw, &allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    buffer = h_raw;
    if (pp_out_mapped) {
        *pp_out_mapped = info.pMappedData;
    }
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return address != 0;
}

} // namespace

ObjectMotion::ObjectMotion(const Instance& instance_, Scheduler& scheduler_)
    : m_instance{instance_}, m_scheduler{scheduler_} {
    // Pipeline selection limits the extra attachment and vertex stores to likely
    // animated draws. A setting or environment override can still disable the path.
    if (!IsUpscalerAvailable()) {
        std::puts("Object motion: off (no upscaler)");
        return;
    }
    if (!PsSettings::Get().should_use_object_motion ||
        PsSettings::Get().upscaler == PsSettings::UpscalerOff) {
        std::puts("Object motion: off (enable in menu or PS_OBJECT_MOTION=1)");
        return;
    }
    const auto& features = m_instance.GetPhysicalDevice().getFeatures();
    if (!features.vertexPipelineStoresAndAtomics) {
        std::printf("Object motion: vertexPipelineStoresAndAtomics unsupported, off\n");
        return;
    }
    void* p_mapped = nullptr;
    u64 va_params = 0, va_positions = 0;
    if (!CreateBuffer(m_instance, vk::DeviceSize(1 + FrameSlots * ParamsPerFrame) * 32, true,
                      m_params_buffer, m_p_params_allocation, &p_mapped, va_params) ||
        !CreateBuffer(m_instance, vk::DeviceSize(1 + 2 * PositionsPerFrame) * 16, false,
                      m_positions_buffer, m_p_positions_allocation, nullptr, va_positions)) {
        std::printf("Object motion: buffer creation failed, off\n");
        return;
    }
    m_p_params_mapped = static_cast<u32*>(p_mapped);
    std::memset(m_p_params_mapped, 0, 32);
    Shader::MotionVectors::params_address = va_params;
    Shader::MotionVectors::positions_address = va_positions;
    m_is_enabled = true;
    std::printf("Object motion: on (%u vertices per frame)\n", PositionsPerFrame);
}

ObjectMotion::~ObjectMotion() {
    m_scheduler.Finish();
    const auto p_allocator = m_instance.GetAllocator();
    if (m_params_buffer) {
        vmaDestroyBuffer(p_allocator, m_params_buffer, m_p_params_allocation);
    }
    if (m_positions_buffer) {
        vmaDestroyBuffer(p_allocator, m_positions_buffer, m_p_positions_allocation);
    }
}

void ObjectMotion::OnFrameStart() {
    if (!m_is_enabled) {
        return;
    }
    if (PsStats::g_is_enabled && (m_frame % 600) == 0 && m_history.m_stats.draws) {
        const auto& s = m_history.m_stats;
        std::printf("Object motion: %llu draws, %llu stored, %llu with history, %llu unmatched, "
                    "%llu capacity skips, %llu invalid, %llu still (600 frames); last frame %u/%u vertices; "
                    "index ranges %llu reused, %llu scanned (%llu found changed), %zu cached\n",
                    (unsigned long long)s.draws, (unsigned long long)s.stored,
                    (unsigned long long)s.loaded, (unsigned long long)s.unmatched,
                    (unsigned long long)s.exhausted, (unsigned long long)s.invalid,
                    (unsigned long long)s.still,
                    m_history.Used(), PositionsPerFrame, (unsigned long long)m_index_ranges.m_stats.hits,
                    (unsigned long long)m_index_ranges.m_stats.scans,
                    (unsigned long long)m_index_ranges.m_stats.stale, m_index_ranges.Size());
        m_index_ranges.m_stats = {};
        m_history.m_stats = {};
    }
    m_history.NextFrame();
    ++m_frame;
    if (m_frame % Motion::IndexRangeCache::Unused == 0) {
        m_index_ranges.Trim(m_frame);
    }
    const u32 slot = u32(m_frame % FrameSlots);
    // Do not overwrite host parameters still referenced by queued GPU work.
    if (m_params_ticks[slot]) {
        m_scheduler.Wait(m_params_ticks[slot]);
        m_params_ticks[slot] = 0;
    }
    m_c_params_used = 0;
    for (auto& target : m_targets) {
        target.is_written = false;
    }
    m_scheduler.EndRendering();
    // Last frame's stores must be visible to this frame's loads. Also finish old reads
    // before the ping-pong half they reference is reused for writes.
    m_scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
}

u32 ObjectMotion::PrepareDraw(const DrawInfo& draw) {
    if (!m_is_enabled || PsToggle::Disabled(1u << 29) || m_c_params_used >= ParamsPerFrame) {
        return 0;
    }
    const auto allocation = m_history.Prepare(draw);
    u32 flags = (allocation.store ? Shader::MotionVectors::FlagStore : 0) |
                (allocation.load ? Shader::MotionVectors::FlagLoad : 0);
    if (!flags) {
        return 0;
    }
    const u32 slot = u32(m_frame % FrameSlots);
    const u32 i_param = 1 + slot * ParamsPerFrame + m_c_params_used++;
    u32* p_param = m_p_params_mapped + i_param * 8;
    p_param[0] = allocation.store;
    p_param[1] = allocation.load;
    p_param[2] = allocation.vertices;
    p_param[3] = flags;
    p_param[4] = allocation.first_vertex;
    p_param[5] = allocation.first_instance;
    p_param[6] = allocation.instances;
    p_param[7] = 0;
    m_params_ticks[slot] = m_scheduler.CurrentTick();
    return i_param;
}

ObjectMotion::Target* ObjectMotion::GetTarget(u32 width, u32 height) {
    for (auto& target : m_targets) {
        if (target.image && target.width == width && target.height == height) {
            target.used_frame = m_frame;
            return &target;
        }
    }
    // A free slot, else the image used longest ago (not the one being read).
    Target* p_slot = nullptr;
    for (auto& target : m_targets) {
        if (!target.image) {
            p_slot = &target;
            break;
        }
        if (&target != m_p_read_target && (!p_slot || target.used_frame < p_slot->used_frame)) {
            p_slot = &target;
        }
    }
    if (p_slot->image) {
        // Destroyed once the GPU is past the work recorded so far (it may still use it).
        m_scheduler.DeferOperation([image = std::move(p_slot->image),
                                  view = std::move(p_slot->view)]() mutable {
            view.reset();
            image.Destroy();
        });
    }
    static u32 s_c_reports = 0;
    if (PsStats::g_is_enabled && s_c_reports < 16) {
        ++s_c_reports;
        std::printf("Object motion: motion image %ux%u%s\n", width, height,
                    p_slot->width ? " (replacing one)" : "");
    }
    const auto device = m_instance.GetDevice();
    p_slot->image = VideoCore::UniqueImage(device, m_instance.GetAllocator());
    if (p_slot->image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        SetObjectName(device, vk::Image(p_slot->image), "psport: object motion {}x{}", width, height);
    }
    if (!p_slot->image) {
        LOG_ERROR_COUNTED(Render_Vulkan,
                          "Object motion image {}x{} not allocated, its vectors dropped", width,
                          height);
        p_slot->width = 0;
        p_slot->height = 0;
        p_slot->is_written = false;
        return nullptr;
    }
    p_slot->view = Check(device.createImageViewUnique({
        .image = vk::Image(p_slot->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    p_slot->width = width;
    p_slot->height = height;
    p_slot->is_written = false;
    p_slot->layout = vk::ImageLayout::eUndefined;
    p_slot->used_frame = m_frame;
    return p_slot;
}

bool ObjectMotion::Attach(RenderState& state, u32 width, u32 height) {
    constexpr u32 slot = Shader::MotionVectors::Output;
    Target* p_target = GetTarget(width, height);
    auto& attachment = state.color_attachments[slot];
    attachment = {};
    state.num_color_attachments = std::max<u16>(state.num_color_attachments, slot + 1);
    if (!p_target) {
        return false;
    }
    Target& target = *p_target;
    attachment.image_view = *target.view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    if (!target.is_written || target.layout != vk::ImageLayout::eColorAttachmentOptimal) {
        // Clear once per frame; later reads must not erase the validity of this history.
        m_scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = target.layout,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = vk::Image(target.image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        // Recorded in order on the recording thread: CommandBuffer() here switched the rest of
        // the submission (most of the G-buffer) to direct recording on the GPU thread.
        m_scheduler.Record([barrier](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        });
        target.layout = vk::ImageLayout::eColorAttachmentOptimal;
        attachment.is_clear = !target.is_written;
        target.is_written = true;
    }
    return true;
}

vk::ImageView ObjectMotion::PrepareRead(u32 width, u32 height, bool& valid) {
    // The image of this size; otherwise any image (the pass binds one, its vectors unused).
    Target* p_target = const_cast<Target*>(Find(width, height));
    if (!p_target) {
        for (auto& candidate : m_targets) {
            if (candidate.image && (!p_target || candidate.used_frame > p_target->used_frame)) {
                p_target = &candidate;
            }
        }
    }
    if (!p_target) {
        p_target = GetTarget(1, 1);
    }
    if (!p_target) {
        valid = false;
        return {}; // a null view (nullDescriptor): GetTarget logged it
    }
    valid = p_target->is_written && width == p_target->width && height == p_target->height;
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = p_target->layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(p_target->image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    m_scheduler.Record([barrier](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    p_target->layout = vk::ImageLayout::eGeneral;
    m_p_read_target = p_target;
    return *p_target->view;
}

} // namespace Vulkan
