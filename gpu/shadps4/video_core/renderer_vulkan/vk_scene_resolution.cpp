// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/host_shaders/depth_resample_frag.h"
#include "video_core/host_shaders/depth_stencil_resample_frag.h"
#include "video_core/host_shaders/depth_stencil_bits_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "psport_toggles.h"
#include <cstdio>
#include <cstdlib>

namespace Vulkan {
SceneTargets::SceneTargets(const Instance& i, Scheduler& s, Runtime& r,
                           VideoCore::TextureCache& t)
    : SceneTargets(i, s, r, [&t](VideoCore::ImageId id, u64 uid) {
        return t.TryGetImage(id, uid);
    }) {}
SceneTargets::SceneTargets(const Instance& i, Scheduler& s, Runtime& r, Lookup get)
    : m_instance{i}, m_scheduler{s}, m_runtime{r}, m_lookup{std::move(get)} {
    m_runtime.scene_targets = this;
    const char* sz_force = std::getenv("PS_SCENE_STENCIL_BITS");
    m_should_force_stencil_bits = sz_force && sz_force[0] == '1';
    const auto& placement = GetUpscalerPlacement();
    m_native_size = {placement.scene_width, placement.scene_height};
    m_post_divisor = std::max(1u, placement.post_divisor);
    m_size = m_native_size;
    CreateResampleResources();
}
SceneTargets::~SceneTargets() {
    m_scheduler.Finish();
    m_runtime.scene_targets = nullptr;
}
vk::FormatFeatureFlags SceneTargets::Features(vk::Format format) const {
    // Called for every attachment of every draw: the driver query was ~5% of the GPU thread.
    // Core formats index a flat table; extension formats (large enum values) use the map.
    const auto i_format = u32(format);
    if (i_format < m_format_table.size() && m_format_known[i_format]) return m_format_table[i_format];
    if (i_format >= m_format_table.size()) {
        if (const auto it = m_format_features.find(format); it != m_format_features.end()) {
            return it->second;
        }
    }
    const auto features =
        m_instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    if (i_format < m_format_table.size()) {
        m_format_table[i_format] = features;
        m_format_known[i_format] = true;
    } else {
        m_format_features.emplace(format, features);
    }
    return features;
}
bool SceneTargets::Blittable(vk::Format format) const {
    const auto features = Features(format);
    const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
    return (features & required) == required;
}
bool SceneTargets::ShaderResampled(const VideoCore::Image& image) const {
    const auto format = image.backing->image.image_ci.format;
    if (!image.info.props.is_depth || (Blittable(format) && !m_should_force_stencil_bits)) return false;
    const auto features = Features(format);
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eDepthStencilAttachment;
    // Without shader stencil export (e.g. Pascal) depth/stencil stays native: the eight-draw
    // stencil resampler costs too much on such GPUs. PS_SCENE_STENCIL_BITS=1 forces it (tests).
    return (features & required) == required &&
           (!(image.aspect_mask & vk::ImageAspectFlagBits::eStencil) ||
            m_instance.IsShaderStencilExportSupported() || m_should_force_stencil_bits);
}
// 1 for the native scene size, the post divisor for the game's post-processing chain, else 0.
// Post targets need a scene size that divides exactly: the proxy keeps the scene's factor, so
// the shaders' FragCoord conversion (native / scene width) stays correct for it.
u32 SceneTargets::SizeDivisor(const VideoCore::ImageInfo& i) const {
    if (i.size.width == m_native_size.width && i.size.height == m_native_size.height) return 1;
    if (m_post_divisor > 1 && i.size.width == m_native_size.width / m_post_divisor &&
        i.size.height == m_native_size.height / m_post_divisor &&
        m_size.width % m_post_divisor == 0 && m_size.height % m_post_divisor == 0 &&
        !PsToggle::Disabled(PsToggle::SceneHalfRes)) {
        return m_post_divisor;
    }
    return 0;
}
bool SceneTargets::Eligible(const VideoCore::Image& image) const {
    const auto& i = image.info;
    const u32 div = SizeDivisor(i);
    if (!div || i.size.depth != 1 || i.resources.layers != 1 || i.num_samples != 1 ||
        i.props.is_block || !image.backing || image.backing->num_samples != 1) {
        return false;
    }
    if (i.resources.levels != 1) {
        // Mip chains (the bloom pyramid at half resolution): one blitted proxy per level.
        return div > 1 && i.resources.levels <= 16 && !i.props.is_depth &&
               Blittable(image.backing->image.image_ci.format);
    }
    return Blittable(image.backing->image.image_ci.format) || ShaderResampled(image);
}
bool SceneTargets::EligibleScene(const VideoCore::Image& image) const {
    return image.info.size.width == m_native_size.width &&
           image.info.size.height == m_native_size.height && Eligible(image);
}
SceneResolution::Size SceneTargets::ProxySize(const VideoCore::Image& image, u32 level) const {
    const u32 div = std::max(1u, SizeDivisor(image.info));
    return {std::max(1u, m_size.width / div >> level), std::max(1u, m_size.height / div >> level)};
}
bool SceneTargets::SetSize(SceneResolution::Size next) {
    if (next == m_size) return false;
    ResolveAll();
    m_scheduler.Finish();
    m_entries.clear(); // no command buffer can still reference these images/views
    m_tracked.clear();
    ++m_generation;
    m_recent = {};
    m_size = next;
    std::printf("Scene resolution: raster %ux%u, half-resolution targets %ux%u, UI %ux%u (live)\n",
                m_size.width, m_size.height, m_size.width / m_post_divisor,
                m_size.height / m_post_divisor, m_native_size.width, m_native_size.height);
    return true;
}
void SceneTargets::ResolveAll() {
    for (auto& [key, entry] : m_entries) {
        if (entry->state.m_is_dirty) {
            if (auto* p_image = m_lookup(entry->source, entry->uid)) Copy(*entry, *p_image, true);
        }
    }
}
void SceneTargets::NativeAccess(VideoCore::Image& image, vk::AccessFlags2 access) {
    if (m_is_copying || !image.scene_proxy) return; // most images never had a proxy
    constexpr auto writes = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
        vk::AccessFlagBits2::eMemoryWrite;
    const u32 levels = std::min(16u, image.info.resources.levels);
    for (u32 level = 0; level < levels; ++level) {
        const auto it = m_entries.find(Key(image.image_uid, level));
        if (it == m_entries.end()) continue;
        auto& entry = *it->second;
        if (entry.state.m_is_dirty) Copy(entry, image, true);
        if (access & writes) entry.state.NativeWrite();
    }
}
void SceneTargets::Transition(Entry& e, vk::ImageAspectFlags aspect, vk::ImageLayout layout,
                               vk::PipelineStageFlags2 stages, vk::AccessFlags2 access) {
    m_scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = stages, .dstAccessMask = access,
        .oldLayout = e.layout, .newLayout = layout,
        .image = e.image, .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    m_scheduler.Record([barrier](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    e.layout = layout;
}
void SceneTargets::Copy(Entry& e, VideoCore::Image& original, bool is_to_native) {
    m_is_copying = true;
    m_scheduler.EndRendering();
    const Runtime::TransferMark mark{m_runtime, is_to_native ? "scene proxy resolve" : "scene proxy fill",
                                     original};
    if (m_is_debug) {
        std::printf("Scene %s: %s %ux%u level %u\n", is_to_native ? "resolve" : "fill",
                    vk::to_string(original.info.pixel_format).c_str(), original.info.size.width,
                    original.info.size.height, e.level);
    }
    const vk::Image src = is_to_native ? vk::Image(e.image) : original.GetImage();
    const vk::Image dst = is_to_native ? original.GetImage() : vk::Image(e.image);
    if (ShaderResampled(original)) {
        constexpr auto read_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        constexpr auto write_layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        constexpr auto read_stage = vk::PipelineStageFlagBits2::eFragmentShader;
        constexpr auto write_stage = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                     vk::PipelineStageFlagBits2::eLateFragmentTests;
        constexpr auto read_access = vk::AccessFlagBits2::eShaderSampledRead;
        constexpr auto write_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        m_runtime.Transit(&original, is_to_native ? write_layout : read_layout,
                        is_to_native ? write_stage : read_stage,
                        is_to_native ? write_access : read_access);
        m_runtime.FlushBarriers();
        Transition(e, original.aspect_mask, is_to_native ? read_layout : write_layout,
                   is_to_native ? read_stage : write_stage, is_to_native ? read_access : write_access);
        const auto& proxy = e.image.image_ci.extent;
        Resample(src, dst, original,
                 is_to_native ? vk::Extent2D{original.info.size.width, original.info.size.height}
                           : vk::Extent2D{proxy.width, proxy.height});
        // (Depth proxies are single-level: Eligible.)
        if (is_to_native) e.state.Resolved(); else e.state.CopiedToProxy();
        m_is_copying = false;
        return;
    }
    const VideoCore::SubresourceRange level_range{.base = {e.level, 0}, .extent = {1, 1}};
    m_runtime.Transit(&original, is_to_native ? vk::ImageLayout::eTransferDstOptimal
                                       : vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer,
                    is_to_native ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead,
                    level_range);
    m_runtime.FlushBarriers();
    Transition(e, original.aspect_mask,
               is_to_native ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eTransferDstOptimal,
               vk::PipelineStageFlagBits2::eTransfer,
               is_to_native ? vk::AccessFlagBits2::eTransferRead : vk::AccessFlagBits2::eTransferWrite);
    const vk::Offset3D native{s32(std::max(1u, original.info.size.width >> e.level)),
                              s32(std::max(1u, original.info.size.height >> e.level)), 1};
    const vk::Offset3D reduced{s32(e.image.image_ci.extent.width),
                               s32(e.image.image_ci.extent.height), 1};
    for (auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
                        vk::ImageAspectFlagBits::eStencil}) {
        if (!(original.aspect_mask & aspect)) continue;
        const vk::ImageBlit region{
            .srcSubresource = {aspect, is_to_native ? 0u : e.level, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{}, is_to_native ? reduced : native},
            .dstSubresource = {aspect, is_to_native ? e.level : 0u, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{}, is_to_native ? native : reduced},
        };
        // Nearest preserves depth/stencil and avoids assuming linear blit support for
        // integer G-buffer formats. FSR reconstructs color at the HDR scene boundary.
        m_scheduler.Record([src,dst,region](vk::CommandBuffer cmd) {
            cmd.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                          vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        });
    }
    if (is_to_native) e.state.Resolved(); else e.state.CopiedToProxy();
    m_is_copying = false;
}
void SceneTargets::Resample(vk::Image src, vk::Image dst, const VideoCore::Image& original,
                            vk::Extent2D dst_size) {
    const auto device = m_instance.GetDevice();
    const auto format = original.backing->image.image_ci.format;
    const bool is_stencil = bool(original.aspect_mask & vk::ImageAspectFlagBits::eStencil);
    const bool has_stencil_bits = is_stencil && !m_depth_stencil_frag;
    const auto make_view = [&](vk::Image image, vk::ImageAspectFlags aspect,
                               vk::ImageUsageFlags usage) {
        const vk::ImageViewUsageCreateInfo usage_ci{.usage = usage};
        return Check(device.createImageView({
            .pNext = &usage_ci, .image = image, .viewType = vk::ImageViewType::e2D,
            .format = format, .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    const auto depth_view = make_view(src, vk::ImageAspectFlagBits::eDepth,
                                      vk::ImageUsageFlagBits::eSampled);
    const auto stencil_view = is_stencil ? make_view(src, vk::ImageAspectFlagBits::eStencil,
                                                  vk::ImageUsageFlagBits::eSampled)
                                      : vk::ImageView{};
    const auto target_view = make_view(dst, original.aspect_mask,
                                       vk::ImageUsageFlagBits::eDepthStencilAttachment);
    m_scheduler.DeferOperation([device, depth_view, stencil_view, target_view] {
        device.destroyImageView(depth_view);
        if (stencil_view) device.destroyImageView(stencil_view);
        device.destroyImageView(target_view);
    });
    const auto pipeline = ResamplePipeline(format, is_stencil);
    const auto layout = *m_resample_layout;
    m_scheduler.RecordCrumb({.sz_name = "scene resample"}, [=](vk::CommandBuffer cmd) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = target_view,
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        auto stencil_attachment = attachment;
        if (has_stencil_bits) {
            stencil_attachment.loadOp = vk::AttachmentLoadOp::eClear;
            stencil_attachment.clearValue.depthStencil = vk::ClearDepthStencilValue{0.f, 0};
        }
        cmd.beginRendering({
            .renderArea = {{0, 0}, dst_size}, .layerCount = 1,
            .pDepthAttachment = &attachment,
            .pStencilAttachment = is_stencil ? &stencil_attachment : nullptr,
        });
        const std::array infos{
            vk::DescriptorImageInfo{{}, depth_view, vk::ImageLayout::eShaderReadOnlyOptimal},
            vk::DescriptorImageInfo{{}, stencil_view, vk::ImageLayout::eShaderReadOnlyOptimal},
        };
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[0]},
            vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[1]},
        };
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout, 0,
                                 vk::ArrayProxy<const vk::WriteDescriptorSet>(
                                     is_stencil ? 2u : 1u, writes.data()));
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmd.setViewportWithCount(vk::Viewport{0.f, 0.f, float(dst_size.width),
                                              float(dst_size.height), 0.f, 1.f});
        cmd.setScissorWithCount(vk::Rect2D{{0, 0}, dst_size});
        if (has_stencil_bits) {
            // Copy depth first, then reconstruct all eight stencil bits without needing
            // VK_EXT_shader_stencil_export (e.g. Pascal). Discard keeps unset bits clear.
            for (u32 bit : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u}) {
                cmd.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(bit), &bit);
                cmd.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, bit);
                cmd.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, bit);
                cmd.draw(3, 1, 0, 0);
            }
        } else {
            cmd.draw(3, 1, 0, 0);
        }
        cmd.endRendering();
    });
    // The guest pipelines' dynamic state must be emitted again after this pipeline.
    m_scheduler.GetDynamicState().Invalidate();
}
void SceneTargets::CreateResampleResources() {
    const auto device = m_instance.GetDevice();
    m_fs_tri_vert = vk::UniqueShaderModule(CompileSPV(FS_TRI_VERT, device), device);
    m_depth_frag = vk::UniqueShaderModule(CompileSPV(DEPTH_RESAMPLE_FRAG, device), device);
    if (m_instance.IsShaderStencilExportSupported() && !m_should_force_stencil_bits) {
        m_depth_stencil_frag =
            vk::UniqueShaderModule(CompileSPV(DEPTH_STENCIL_RESAMPLE_FRAG, device), device);
    }
    if (!m_depth_stencil_frag) {
        m_stencil_bits_frag =
            vk::UniqueShaderModule(CompileSPV(DEPTH_STENCIL_BITS_FRAG, device), device);
    }
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eFragment},
    };
    m_resample_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data(),
    }));
    const auto set_layout = *m_resample_set_layout;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(u32)};
    m_resample_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    }));
}
vk::Pipeline SceneTargets::ResamplePipeline(vk::Format format, bool is_stencil) {
    const bool has_stencil_bits = is_stencil && !m_depth_stencil_frag;
    const std::pair key{format, is_stencil};
    for (const auto& [k, pipeline] : m_resample_pipelines) if (k == key) return *pipeline;
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    // Reference, compare and write masks are static; the exported value replaces the reference.
    const vk::StencilOpState stencil_op{
        .failOp = vk::StencilOp::eReplace, .passOp = vk::StencilOp::eReplace,
        .depthFailOp = vk::StencilOp::eReplace, .compareOp = vk::CompareOp::eAlways,
        .compareMask = 0xff, .writeMask = 0xff, .reference = 0,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state{
        .depthTestEnable = true, .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
        .stencilTestEnable = is_stencil, .front = stencil_op, .back = stencil_op,
    };
    const std::array dynamic_states{vk::DynamicState::eViewportWithCount,
                                    vk::DynamicState::eScissorWithCount,
                                    vk::DynamicState::eStencilWriteMask,
                                    vk::DynamicState::eStencilReference};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = has_stencil_bits ? 4u : 2u, .pDynamicStates = dynamic_states.data(),
    };
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                                          .module = *m_fs_tri_vert, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                          .module = is_stencil ? (has_stencil_bits ? *m_stencil_bits_frag :
                                                             *m_depth_stencil_frag) : *m_depth_frag,
                                          .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .depthAttachmentFormat = format,
        .stencilAttachmentFormat = is_stencil ? format : vk::Format::eUndefined,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1.f};
    auto pipeline = Check(m_instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, {
        .pNext = &rendering, .stageCount = u32(stages.size()), .pStages = stages.data(),
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info, .pRasterizationState = &raster,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending, .pDynamicState = &dynamic_info,
        .layout = *m_resample_layout,
    }));
    const auto result = *pipeline;
    m_resample_pipelines.emplace_back(key, std::move(pipeline));
    return result;
}
SceneTargets::Entry& SceneTargets::Get(VideoCore::ImageId id, u32 level) {
    auto& original = *m_lookup(id, 0);
    const u64 key = Key(original.image_uid, level);
    // A draw has up to six targets, mostly the same as the previous draw's.
    for (const auto& [recent_key, recent_entry] : m_recent) {
        if (recent_entry && recent_key == key) {
            if (!recent_entry->state.m_is_valid) Copy(*recent_entry, original, false);
            return *recent_entry;
        }
    }
    auto& entry = m_entries[key];
    original.scene_proxy = true;
    if (!entry) {
        entry = std::make_unique<Entry>();
        entry->source = id;
        entry->uid = original.image_uid;
        entry->level = level;
        entry->image = VideoCore::UniqueImage(m_instance.GetDevice(), m_instance.GetAllocator());
        auto ci = original.backing->image.image_ci;
        ci.pNext = nullptr;
        const auto proxy = ProxySize(original, level);
        ci.extent = vk::Extent3D{proxy.width, proxy.height, 1};
        ci.mipLevels = 1;
        entry->image.Create(ci);
        m_tracked.insert(original.image_uid);
    }
    m_recent[m_recent_next++ % m_recent.size()] = {key, entry.get()};
    if (!entry->state.m_is_valid) Copy(*entry, original, false);
    return *entry;
}
vk::ImageView SceneTargets::View(Entry& e, const VideoCore::Image& original,
                                 const VideoCore::ImageViewInfo& info) {
    for (const auto& [key,view] : e.views) if (key == info) return *view;
    const auto aspect = original.info.props.is_depth
        ? (info.is_storage ? original.aspect_mask : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth))
        : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
    const auto format = original.info.props.is_depth ? original.backing->image.image_ci.format : info.format;
    auto view = Check(m_instance.GetDevice().createImageViewUnique({
        .image = e.image, .viewType = vk::ImageViewType::e2D, .format = format,
        .components = info.mapping, .subresourceRange = {aspect,0,1,0,1},
    }));
    const auto result = *view;
    e.views.emplace_back(info, std::move(view));
    return result;
}
SceneTargets::Target SceneTargets::Attachment(VideoCore::ImageId id,
                                               const VideoCore::ImageViewInfo& info) {
    auto& original = *m_lookup(id, 0);
    auto& e = Get(id, info.range.base.level);
    const bool is_depth = original.info.props.is_depth;
    const auto layout = is_depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                              : vk::ImageLayout::eColorAttachmentOptimal;
    // Consecutive draws can stay in the same render pass.
    if (e.layout != layout) {
        Transition(e, original.aspect_mask, layout,
            is_depth ? vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests
                  : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eColorAttachmentOutput),
            is_depth ? vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                  : vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    }
    e.state.ProxyWrite();
    auto attachment_info = info;
    attachment_info.is_storage = true; // include stencil in attachment views
    return {e.image, View(e, original, attachment_info), e.layout, e.image.image_ci.usage};
}
bool SceneTargets::ProxyCurrent(const VideoCore::Image& image, u32 level, u32 levels) const {
    if (m_is_copying || !image.scene_proxy || image.info.props.is_depth || levels != 1) {
        return false;
    }
    const auto it = m_entries.find(Key(image.image_uid, level));
    return it != m_entries.end() && it->second->state.m_is_valid;
}
vk::ImageLayout SceneTargets::PrepareSample(const VideoCore::Image& image, u32 level) {
    auto& e = *m_entries.find(Key(image.image_uid, level))->second;
    constexpr auto layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    if (e.layout != layout) {
        Transition(e, image.aspect_mask, layout,
                   vk::PipelineStageFlagBits2::eVertexShader |
                       vk::PipelineStageFlagBits2::eFragmentShader |
                       vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderSampledRead);
    }
    return layout;
}
std::optional<SceneTargets::Target> SceneTargets::SampleProxy(
    const VideoCore::Image& image, const VideoCore::ImageViewInfo& info) {
    const u32 level = info.range.base.level;
    if (!ProxyCurrent(image, level, info.range.extent.levels)) {
        return std::nullopt;
    }
    auto& e = *m_entries.find(Key(image.image_uid, level))->second;
    const auto layout = PrepareSample(image, level);
    return Target{e.image, View(e, image, info), layout, e.image.image_ci.usage};
}
SceneTargets::Target SceneTargets::Read(VideoCore::ImageId id,
                                       const VideoCore::ImageViewInfo& info,
                                       vk::PipelineStageFlags2 stages, vk::AccessFlags2 access) {
    auto& original = *m_lookup(id, 0);
    auto& e = Get(id);
    Transition(e, original.aspect_mask, vk::ImageLayout::eGeneral, stages, access);
    return {e.image, View(e, original, info), e.layout, e.image.image_ci.usage};
}
} // namespace Vulkan
