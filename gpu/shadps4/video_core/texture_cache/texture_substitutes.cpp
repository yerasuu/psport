// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/texture_cache/texture_substitutes.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "psport_small_textures.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image_view.h"

namespace VideoCore {
namespace {

bool IsSrgb(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc7SrgbBlock:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eA8B8G8R8SrgbPack32:
        return true;
    default:
        return false;
    }
}

u32 PackMapping(const vk::ComponentMapping& mapping) {
    return u32(mapping.r) | u32(mapping.g) << 8 | u32(mapping.b) << 16 | u32(mapping.a) << 24;
}

vk::ImageCreateInfo PictureImageInfo(u32 width, u32 height, u32 c_layers, vk::Format format) {
    return vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = c_layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
}

vk::ImageMemoryBarrier2 LayoutBarrier(vk::Image image, u32 c_layers, vk::PipelineStageFlags2 src_stage,
                                      vk::AccessFlags2 src_access, vk::ImageLayout old_layout,
                                      vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                                      vk::ImageLayout new_layout) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, c_layers},
    };
}

void RecordBarrier(vk::CommandBuffer cmdbuf, const vk::ImageMemoryBarrier2& barrier) {
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

} // namespace

TextureSubstitutes::TextureSubstitutes(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                                       Vulkan::Runtime& runtime, BufferCache& buffer_cache)
    : m_instance{instance}, m_scheduler{scheduler}, m_runtime{runtime}, m_buffer_cache{buffer_cache},
      m_substitution{Game::Substitution()} {
    if (!m_substitution.pfn_identify || !m_substitution.pfn_generation || !m_substitution.pfn_draw) {
        m_substitution = {};
        return;
    }
    if (m_substitution.pfn_start) {
        m_substitution.pfn_start();
    }
}

TextureSubstitutes::~TextureSubstitutes() = default;

void TextureSubstitutes::Identify(Image& image) const {
    image.p_substitute = nullptr;
    image.is_substitute_rescanned = false;
    const auto& info = image.info;
    if (!m_substitution.pfn_identify || info.resources.levels != 1 || info.resources.layers != 1 ||
        !info.props.is_block) {
        return;
    }
    const auto* p_texels = reinterpret_cast<const void*>(info.guest_address);
    image.p_substitute = m_substitution.pfn_identify(p_texels, info.guest_size, info.size.width, info.size.height);
    image.is_substitute_rescanned =
        !image.p_substitute && m_substitution.pfn_is_rescanned &&
        m_substitution.pfn_is_rescanned(info.guest_size, info.size.width, info.size.height);
}

void TextureSubstitutes::Update() {
    if (!m_substitution.pfn_generation) {
        return;
    }
    const u32 generation = m_substitution.pfn_generation();
    if (m_has_generation && generation == m_generation) {
        return;
    }
    m_has_generation = true;
    m_generation = generation;
    Game::SubstitutePictures pictures;
    m_is_active = m_substitution.pfn_draw(pictures) && Upload(std::move(pictures));
}

bool TextureSubstitutes::Upload(Game::SubstitutePictures&& pictures) {
    const u32 c_pictures = u32(pictures.is_drawn.size());
    const u64 cb_pictures = u64(pictures.pixels.size()) * sizeof(u32);
    if (c_pictures == 0 || pictures.side == 0 || cb_pictures != u64(c_pictures) * pictures.side * pictures.side * 4) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: {} pictures of side {} in {} bytes, not used",
                          c_pictures, pictures.side, cb_pictures);
        return false;
    }
    if (!m_picture_image) {
        m_c_layers = c_pictures;
        m_picture_image = UniqueImage{m_instance.GetDevice(), m_instance.GetAllocator()};
        if (!m_picture_image.Create(
                PictureImageInfo(pictures.side, pictures.side, m_c_layers, vk::Format::eR8G8B8A8Unorm))) {
            LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: no memory for the pictures");
            return false;
        }
        Vulkan::SetObjectName(m_instance.GetDevice(), m_picture_image.image, "psport: substitute pictures {}x{}x{}",
                              pictures.side, pictures.side, m_c_layers);
    }
    if (c_pictures != m_c_layers || pictures.side != m_picture_image.image_ci.extent.width) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: {} pictures of side {} after {} of side {}",
                          c_pictures, pictures.side, m_c_layers, m_picture_image.image_ci.extent.width);
        return false;
    }
    auto& stream = m_buffer_cache.GetStreamBuffer();
    const auto [p_staging, off_staging] = stream.Map(cb_pictures, 16);
    if (!p_staging) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: no staging space for {} bytes", cb_pictures);
        return false;
    }
    std::memcpy(p_staging, pictures.pixels.data(), cb_pictures);
    stream.Commit();
    m_pictures = std::move(pictures);
    // In stream order, ahead of the draws that sample it; the draws before it are done reading it
    // by the barrier's source stages.
    m_scheduler.EndRendering();
    m_scheduler.Record([image = m_picture_image.image, buffer = stream.Handle(), off_staging,
                        side = m_pictures.side, c_layers = m_c_layers](vk::CommandBuffer cmdbuf) {
        RecordBarrier(cmdbuf, LayoutBarrier(image, c_layers, vk::PipelineStageFlagBits2::eAllCommands, {},
                                            vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eCopy,
                                            vk::AccessFlagBits2::eTransferWrite,
                                            vk::ImageLayout::eTransferDstOptimal));
        const vk::BufferImageCopy copy{
            .bufferOffset = off_staging,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, c_layers},
            .imageExtent = {side, side, 1},
        };
        cmdbuf.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, copy);
        RecordBarrier(cmdbuf, LayoutBarrier(image, c_layers, vk::PipelineStageFlagBits2::eCopy,
                                            vk::AccessFlagBits2::eTransferWrite,
                                            vk::ImageLayout::eTransferDstOptimal,
                                            vk::PipelineStageFlagBits2::eAllCommands,
                                            vk::AccessFlagBits2::eShaderRead,
                                            vk::ImageLayout::eShaderReadOnlyOptimal));
    });
    return std::ranges::any_of(m_pictures.is_drawn, [](u8 is_drawn) { return is_drawn != 0; });
}

bool TextureSubstitutes::IsDrawn(const Game::TextureSubstitute& substitute) const {
    const auto is_drawn = [&](u32 i_picture) {
        return i_picture < m_pictures.is_drawn.size() && m_pictures.is_drawn[i_picture] != 0;
    };
    if (substitute.i_whole_picture != Game::TextureSubstitute::NoPicture) {
        return is_drawn(substitute.i_whole_picture);
    }
    return std::ranges::any_of(substitute.cells,
                               [&](const Game::SubstituteCell& cell) { return is_drawn(cell.i_picture); });
}

bool TextureSubstitutes::Replaces(Image& image) {
    PsSmallTextures::NoteBind(image);
    if (!m_is_active) {
        return false;
    }
    if (!image.p_substitute && image.is_substitute_rescanned) {
        const auto& info = image.info;
        image.p_substitute = m_substitution.pfn_identify(reinterpret_cast<const void*>(info.guest_address),
                                                         info.guest_size, info.size.width, info.size.height);
        image.is_substitute_rescanned = !image.p_substitute;
    }
    return image.p_substitute && IsDrawn(*image.p_substitute);
}

vk::ImageView TextureSubstitutes::View(Image& image, const ImageViewInfo& info) {
    if (!m_is_active || !image.p_substitute) {
        return {};
    }
    const auto& substitute = *image.p_substitute;
    if (substitute.i_whole_picture != Game::TextureSubstitute::NoPicture) {
        return PictureView(substitute.i_whole_picture, info);
    }
    return CompositeView(image, substitute, info);
}

vk::ImageView TextureSubstitutes::PictureView(u32 i_picture, const ImageViewInfo& info) {
    const bool is_array = info.type == AmdGpu::ImageType::Color2DArray;
    if (i_picture >= m_c_layers || (!is_array && info.type != AmdGpu::ImageType::Color2D)) {
        return {};
    }
    const bool is_srgb = IsSrgb(info.format);
    auto& view = m_picture_views[{i_picture, is_srgb, is_array, PackMapping(info.mapping)}];
    if (!view) {
        view = Vulkan::Check<"create a substitute picture view">(
            m_instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
                .image = m_picture_image.image,
                .viewType = is_array ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
                .format = is_srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
                .components = info.mapping,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, i_picture, 1},
            }));
    }
    return *view;
}

vk::ImageView TextureSubstitutes::CompositeView(Image& image, const Game::TextureSubstitute& substitute,
                                                const ImageViewInfo& info) {
    if (info.type != AmdGpu::ImageType::Color2D || !image.IsAllocated()) {
        return {};
    }
    // A blit decodes the game's texture; block-compressed formats are blit sources wherever the
    // device samples them, but that is the device's to say.
    const vk::Format source_format = image.backing->image.image_ci.format;
    if (!m_instance.IsFormatSupported(source_format, vk::FormatFeatureFlagBits2::eBlitSrc)) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: {} is no blit source, the game's texture stays",
                          vk::to_string(source_format));
        return {};
    }
    const u32 width = image.info.size.width, height = image.info.size.height;
    const bool is_srgb = IsSrgb(source_format);
    Composite& composite = m_composites[&substitute];
    if (!composite.image) {
        composite.width = width;
        composite.height = height;
        composite.is_srgb = is_srgb;
        composite.image = UniqueImage{m_instance.GetDevice(), m_instance.GetAllocator()};
        // Its own colour space, so that the blit copies the game's texels as they are.
        const auto format = is_srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        if (!composite.image.Create(PictureImageInfo(width, height, 1, format))) {
            LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: no memory for a {}x{} copy", width, height);
            return {};
        }
        Vulkan::SetObjectName(m_instance.GetDevice(), composite.image.image, "psport: substitute copy {}x{} of {:#x}",
                              width, height, image.info.guest_address);
    }
    if (composite.width != width || composite.height != height || composite.is_srgb != is_srgb) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: a {}x{} texture for a {}x{} copy", width, height,
                          composite.width, composite.height);
        return {};
    }
    if (!composite.is_current || composite.generation != m_generation) {
        composite.is_current = Compose(composite, image, substitute);
        composite.generation = m_generation;
        if (!composite.is_current) {
            return {};
        }
    }
    const bool is_view_srgb = IsSrgb(info.format);
    auto& view = composite.views[PackMapping(info.mapping) | (is_view_srgb ? 1u << 31 : 0u)];
    if (!view) {
        view = Vulkan::Check<"create a substitute copy view">(
            m_instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
                .image = composite.image.image,
                .viewType = vk::ImageViewType::e2D,
                .format = is_view_srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
                .components = info.mapping,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
    }
    return *view;
}

bool TextureSubstitutes::Compose(Composite& composite, Image& image, const Game::TextureSubstitute& substitute) {
    std::vector<vk::BufferImageCopy> copies;
    copies.reserve(substitute.cells.size());
    std::vector<u32> pixels;
    std::vector<u32> scaled;
    for (const auto& cell : substitute.cells) {
        if (cell.i_picture >= m_pictures.is_drawn.size() || !m_pictures.is_drawn[cell.i_picture] ||
            cell.side == 0 || cell.x + cell.side > composite.width || cell.y + cell.side > composite.height) {
            continue;
        }
        ScalePicture(cell.i_picture, cell.side, scaled);
        copies.push_back(vk::BufferImageCopy{
            .bufferOffset = pixels.size() * sizeof(u32),
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = {s32(cell.x), s32(cell.y), 0},
            .imageExtent = {cell.side, cell.side, 1},
        });
        pixels.insert(pixels.end(), scaled.begin(), scaled.end());
    }
    if (copies.empty()) {
        return false;
    }
    const u64 cb_pixels = pixels.size() * sizeof(u32);
    auto& stream = m_buffer_cache.GetStreamBuffer();
    const auto [p_staging, off_staging] = stream.Map(cb_pixels, 16);
    if (!p_staging) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Texture substitution: no staging space for {} bytes", cb_pixels);
        return false;
    }
    std::memcpy(p_staging, pixels.data(), cb_pixels);
    stream.Commit();
    for (auto& copy : copies) {
        copy.bufferOffset += off_staging;
    }
    m_scheduler.EndRendering();
    m_runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eBlit,
                      vk::AccessFlagBits2::eTransferRead);
    m_runtime.FlushBarriers();
    m_scheduler.Record([source = image.GetImage(), target = composite.image.image, buffer = stream.Handle(),
                        copies = std::move(copies), width = composite.width,
                        height = composite.height](vk::CommandBuffer cmdbuf) {
        RecordBarrier(cmdbuf, LayoutBarrier(target, 1, vk::PipelineStageFlagBits2::eAllCommands, {},
                                            vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eBlit,
                                            vk::AccessFlagBits2::eTransferWrite,
                                            vk::ImageLayout::eTransferDstOptimal));
        const std::array corners{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(width), s32(height), 1}};
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = corners,
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = corners,
        };
        cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, target, vk::ImageLayout::eTransferDstOptimal,
                         blit, vk::Filter::eNearest);
        RecordBarrier(cmdbuf, LayoutBarrier(target, 1, vk::PipelineStageFlagBits2::eBlit,
                                            vk::AccessFlagBits2::eTransferWrite,
                                            vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                                            vk::AccessFlagBits2::eTransferWrite,
                                            vk::ImageLayout::eTransferDstOptimal));
        cmdbuf.copyBufferToImage(buffer, target, vk::ImageLayout::eTransferDstOptimal, copies);
        RecordBarrier(cmdbuf, LayoutBarrier(target, 1, vk::PipelineStageFlagBits2::eCopy,
                                            vk::AccessFlagBits2::eTransferWrite,
                                            vk::ImageLayout::eTransferDstOptimal,
                                            vk::PipelineStageFlagBits2::eAllCommands,
                                            vk::AccessFlagBits2::eShaderRead,
                                            vk::ImageLayout::eShaderReadOnlyOptimal));
    });
    return true;
}

void TextureSubstitutes::ScalePicture(u32 i_picture, u32 side, std::vector<u32>& out) const {
    const u32 source_side = m_pictures.side;
    const u32* p_picture = m_pictures.pixels.data() + std::size_t(i_picture) * source_side * source_side;
    out.resize(std::size_t(side) * side);
    for (u32 y = 0; y < side; ++y) {
        for (u32 x = 0; x < side; ++x) {
            // The source texels this one covers; colours weighted by their alpha, so that
            // transparent texels do not darken the edges.
            const u32 x0 = x * source_side / side, x1 = std::max(x0 + 1, (x + 1) * source_side / side);
            const u32 y0 = y * source_side / side, y1 = std::max(y0 + 1, (y + 1) * source_side / side);
            u64 sum_alpha = 0;
            std::array<u64, 3> sums{};
            for (u32 sy = y0; sy < y1; ++sy) {
                for (u32 sx = x0; sx < x1; ++sx) {
                    const u32 texel = p_picture[sy * source_side + sx];
                    const u32 alpha = texel >> 24;
                    sum_alpha += alpha;
                    for (u32 c = 0; c < 3; ++c) {
                        sums[c] += u64((texel >> (8 * c)) & 0xff) * alpha;
                    }
                }
            }
            const u64 c_texels = u64(x1 - x0) * (y1 - y0);
            u32 texel = u32((sum_alpha + c_texels / 2) / c_texels) << 24;
            if (sum_alpha != 0) {
                for (u32 c = 0; c < 3; ++c) {
                    texel |= u32((sums[c] + sum_alpha / 2) / sum_alpha) << (8 * c);
                }
            }
            out[std::size_t(y) * side + x] = texel;
        }
    }
}

} // namespace VideoCore
