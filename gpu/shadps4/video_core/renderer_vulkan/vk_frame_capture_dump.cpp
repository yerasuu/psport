// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture_dump.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
#include <miniz.h>
#include <vulkan/vulkan_format_traits.hpp>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_frame_capture_bc.h"
#include "video_core/renderer_vulkan/vk_frame_capture_draw.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::CaptureDump {

namespace {

constexpr u32 C_IMAGES_MAX = 160;
constexpr u64 CB_STAGING = 64ull << 20;
constexpr u32 WIDTH_MAX = 480;
constexpr u32 HEIGHT_MAX = 270;
constexpr u64 NOT_STAGED = ~0ull;
// A float destination keeps what lies above 1.0 for the tonemap; blits into it are required of
// every Vulkan device.
constexpr vk::Format COLOR_FORMAT = vk::Format::eR16G16B16A16Sfloat;

enum class Conversion : u32 { Unorm, Srgb, Snorm, Tonemap, Stretch };

struct Kind {
    Conversion conversion{};
    u32 c_channels{};
    bool is_integer{};
};

struct Item {
    Source source;
    std::string method;
    Conversion conversion{};
    u32 c_channels{};
    vk::Format staged_format{};
    u32 width{};
    u32 height{};
    u64 off_staging{NOT_STAGED};
    bool is_compressed{}; ///< whole blocks of staged_format in the staging buffer
    u32 compressed_width{};
    u32 compressed_height{};
    std::vector<u8> rgb;
    std::vector<u8> alpha; ///< textures whose alpha is not constant
    std::string values;   ///< min/max and non-finite count, for the index and the log
    std::string fallback; ///< why the blit was not used
    std::string skipped;  ///< why nothing was dumped; empty when it was
};

struct Scratch {
    vk::Format format{};
    std::unique_ptr<VideoCore::UniqueImage> p_image;
};

// GPU thread only.
std::vector<Item> s_items;
std::unique_ptr<VideoCore::Buffer> s_p_staging;
bool s_is_staging_failed = false;
std::vector<Scratch> s_scratches;
std::vector<std::string> s_notes;
u64 s_off_staging = 0;
u32 s_c_images = 0;
u32 s_c_budget_skipped = 0;
u32 s_c_waits = 0;

Kind Classify(vk::Format format) {
    const std::string_view component = vk::componentName(format, 0);
    const u32 c_channels = std::min<u32>(vk::componentCount(format), 4);
    if (component == "D" || component == "S") {
        return {Conversion::Stretch, 1, false};
    }
    const std::string_view numeric = vk::componentNumericFormat(format, 0);
    if (numeric == "SRGB") {
        return {Conversion::Srgb, c_channels};
    }
    if (numeric == "SNORM") {
        return {Conversion::Snorm, c_channels};
    }
    if (numeric == "SFLOAT" || numeric == "UFLOAT") {
        return {Conversion::Tonemap, c_channels};
    }
    if (numeric == "UINT" || numeric == "SINT" || numeric == "USCALED" || numeric == "SSCALED") {
        return {Conversion::Stretch, c_channels, numeric == "UINT" || numeric == "SINT"};
    }
    return {Conversion::Unorm, c_channels};
}

bool IsDepthFormat(vk::Format format) {
    const std::string_view component = vk::componentName(format, 0);
    return component == "D" || component == "S";
}

/// A texture is shown as its shader reads it, every channel; a target in its own channels.
u32 ShownChannels(const Source& source, const Kind& kind) {
    return source.is_texture && !IsDepthFormat(source.format) ? 4 : kind.c_channels;
}

std::string MappingName(const vk::ComponentMapping& mapping) {
    std::string name;
    const std::array<vk::ComponentSwizzle, 4> swizzles{mapping.r, mapping.g, mapping.b, mapping.a};
    for (u32 c = 0; c < 4; ++c) {
        switch (swizzles[c]) {
        case vk::ComponentSwizzle::eIdentity:
            name += "rgba"[c];
            break;
        case vk::ComponentSwizzle::eZero:
            name += '0';
            break;
        case vk::ComponentSwizzle::eOne:
            name += '1';
            break;
        case vk::ComponentSwizzle::eR:
            name += 'r';
            break;
        case vk::ComponentSwizzle::eG:
            name += 'g';
            break;
        case vk::ComponentSwizzle::eB:
            name += 'b';
            break;
        case vk::ComponentSwizzle::eA:
            name += 'a';
            break;
        }
    }
    return name;
}

/// Puts the decoded texels (4 floats each, in the stored format's channel order) in the order the
/// source's mapping names. A single-channel target is shown grey as stored, whatever its swizzle.
void Remap(const Item& item, std::span<float> rgba) {
    const Source& source = item.source;
    if (IsDepthFormat(source.format) || (!source.is_texture && item.c_channels <= 1) ||
        MappingName(source.mapping) == "rgba") {
        return;
    }
    const std::array<vk::ComponentSwizzle, 4> swizzles{source.mapping.r, source.mapping.g,
                                                       source.mapping.b, source.mapping.a};
    for (size_t i = 0; i + 4 <= rgba.size(); i += 4) {
        const std::array<float, 4> stored{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
        for (u32 c = 0; c < 4; ++c) {
            switch (swizzles[c]) {
            case vk::ComponentSwizzle::eIdentity:
                rgba[i + c] = stored[c];
                break;
            case vk::ComponentSwizzle::eZero:
                rgba[i + c] = 0.0f;
                break;
            case vk::ComponentSwizzle::eOne:
                rgba[i + c] = 1.0f;
                break;
            default:
                rgba[i + c] = stored[u32(swizzles[c]) - u32(vk::ComponentSwizzle::eR)];
                break;
            }
        }
    }
}

const char* ConversionName(Conversion conversion) {
    switch (conversion) {
    case Conversion::Unorm:
        return "unorm as is";
    case Conversion::Srgb:
        return "srgb (linear after the blit, encoded back)";
    case Conversion::Snorm:
        return "snorm (x+1)/2";
    case Conversion::Tonemap:
        return "float x/(1+x) gamma 2.2";
    case Conversion::Stretch:
        return "contrast stretch to min/max";
    }
    return "?";
}

/// The output size: at most WIDTH_MAX x HEIGHT_MAX with the aspect ratio kept, never larger
/// than the source.
std::pair<u32, u32> FitSize(u32 width, u32 height) {
    const double scale = std::min({1.0, double(WIDTH_MAX) / width, double(HEIGHT_MAX) / height});
    return {std::max(1u, u32(std::lround(width * scale))),
            std::max(1u, u32(std::lround(height * scale)))};
}

std::string AttachmentName(const Source& source) {
    return source.is_depth ? std::string{"d"} : std::format("c{}", source.i_color);
}

std::string FileName(const Source& source) {
    return source.is_texture ? std::format("tex_p{}_d{}_b{}", source.i_pass, source.i_draw,
                                           source.i_binding)
                             : std::format("p{}_{}", source.i_pass, AttachmentName(source));
}

u8 ToByte(float value) {
    return u8(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

float EncodeSrgb(float linear) {
    return linear <= 0.0031308f ? linear * 12.92f
                                : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

/// A texture's alpha as a grey image beside its colour one, unless it holds one value.
void ConvertAlpha(Item& item, std::span<const float> rgba, u32 c_texels) {
    float min_alpha = INFINITY, max_alpha = -INFINITY;
    for (u32 i = 0; i < c_texels; ++i) {
        const float alpha = rgba[size_t(i) * 4 + 3];
        if (std::isfinite(alpha)) {
            min_alpha = std::min(min_alpha, alpha);
            max_alpha = std::max(max_alpha, alpha);
        }
    }
    if (min_alpha > max_alpha) {
        item.values += " alpha not finite";
        return;
    }
    if (min_alpha == max_alpha) {
        item.values += std::format(" alpha constant {:.6g}", min_alpha);
        return;
    }
    item.values += std::format(" alpha min {:.6g} max {:.6g}", min_alpha, max_alpha);
    item.alpha.resize(c_texels);
    for (u32 i = 0; i < c_texels; ++i) {
        const float alpha = rgba[size_t(i) * 4 + 3];
        item.alpha[i] = !std::isfinite(alpha)                  ? u8(0)
                        : item.conversion == Conversion::Snorm ? ToByte((alpha + 1.0f) * 0.5f)
                                                               : ToByte(alpha);
    }
}

/// Converts decoded texels (4 floats each) into 8-bit RGB; non-finite texels become magenta, so
/// that a NaN or infinity a pass writes stands out.
void Convert(Item& item, std::span<const float> rgba, u32 c_texels) {
    item.rgb.assign(size_t(c_texels) * 3, 0);
    const u32 c_used = std::max(item.c_channels, 1u);
    u64 c_nonfinite = 0;
    float min_value = INFINITY, max_value = -INFINITY;
    for (u32 i = 0; i < c_texels; ++i) {
        for (u32 c = 0; c < std::min(c_used, 3u); ++c) {
            const float value = rgba[size_t(i) * 4 + c];
            if (std::isfinite(value)) {
                min_value = std::min(min_value, value);
                max_value = std::max(max_value, value);
            }
        }
    }
    const float range = max_value > min_value ? max_value - min_value : 1.0f;
    for (u32 i = 0; i < c_texels; ++i) {
        const float* p_texel = rgba.data() + size_t(i) * 4;
        u8* p_out = item.rgb.data() + size_t(i) * 3;
        bool is_finite = true;
        for (u32 c = 0; c < std::min(c_used, 3u); ++c) {
            is_finite &= std::isfinite(p_texel[c]);
        }
        if (!is_finite) {
            ++c_nonfinite;
            p_out[0] = 255;
            p_out[2] = 255;
            continue;
        }
        for (u32 c = 0; c < 3; ++c) {
            const u32 i_channel = c_used == 1 ? 0 : c;
            if (i_channel >= c_used) {
                continue;
            }
            const float value = p_texel[i_channel];
            switch (item.conversion) {
            case Conversion::Unorm:
                p_out[c] = ToByte(value);
                break;
            case Conversion::Srgb:
                p_out[c] = ToByte(EncodeSrgb(std::max(value, 0.0f)));
                break;
            case Conversion::Snorm:
                p_out[c] = ToByte((value + 1.0f) * 0.5f);
                break;
            case Conversion::Tonemap: {
                const float positive = std::max(value, 0.0f);
                p_out[c] = ToByte(std::pow(positive / (1.0f + positive), 1.0f / 2.2f));
                break;
            }
            case Conversion::Stretch:
                p_out[c] = ToByte((value - min_value) / range);
                break;
            }
        }
    }
    item.values = min_value <= max_value
                      ? std::format("min {:.6g} max {:.6g}", min_value, max_value)
                      : std::string{"no finite value"};
    if (c_nonfinite) {
        item.values += std::format(" nonfinite {} (magenta)", c_nonfinite);
    }
    if (item.source.is_texture && item.c_channels == 4) {
        ConvertAlpha(item, rgba, c_texels);
    }
}

/// Texels of the staging buffer as the copy left them: COLOR_FORMAT, or the depth plane of a
/// depth format.
std::vector<float> DecodeStaged(const Item& item, const u8* p_data) {
    const u32 c_texels = item.width * item.height;
    std::vector<float> rgba(size_t(c_texels) * 4, 0.0f);
    for (u32 i = 0; i < c_texels; ++i) {
        float* p_out = rgba.data() + size_t(i) * 4;
        if (item.staged_format == COLOR_FORMAT) {
            for (u32 c = 0; c < 4; ++c) {
                u16 bits;
                std::memcpy(&bits, p_data + size_t(i) * 8 + c * 2, 2);
                p_out[c] = CaptureDraw::HalfToFloat(bits);
            }
            continue;
        }
        switch (item.staged_format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eD16UnormS8Uint: {
            u16 bits;
            std::memcpy(&bits, p_data + size_t(i) * 2, 2);
            p_out[0] = float(bits) / 65535.0f;
            break;
        }
        case vk::Format::eX8D24UnormPack32:
        case vk::Format::eD24UnormS8Uint: {
            u32 bits;
            std::memcpy(&bits, p_data + size_t(i) * 4, 4);
            p_out[0] = float(bits & 0xffffff) / 16777215.0f;
            break;
        }
        default:
            std::memcpy(p_out, p_data + size_t(i) * 4, 4);
            break;
        }
    }
    return rgba;
}

/// Whole blocks of a compressed image decoded and box-filtered down to the item's size.
std::vector<float> DecodeCompressed(const Item& item, const u8* p_data) {
    const u32 cb_block = CaptureBc::BlockBytes(item.staged_format);
    const u32 width = item.compressed_width;
    const u32 height = item.compressed_height;
    const u32 c_block_columns = (width + 3) / 4;
    const u32 c_block_rows = (height + 3) / 4;
    std::vector<float> rgba(size_t(item.width) * item.height * 4, 0.0f);
    std::vector<u32> c_summed(size_t(item.width) * item.height, 0);
    std::vector<u32> out_columns(width);
    for (u32 x = 0; x < width; ++x) {
        out_columns[x] = u32(u64(x) * item.width / width);
    }
    std::array<float, 64> block;
    for (u32 block_row = 0; block_row < c_block_rows; ++block_row) {
        for (u32 block_column = 0; block_column < c_block_columns; ++block_column) {
            CaptureBc::DecodeBlock(
                item.staged_format,
                p_data + (size_t(block_row) * c_block_columns + block_column) * cb_block, block);
            for (u32 j = 0; j < 4 && block_row * 4 + j < height; ++j) {
                const size_t off_row =
                    size_t(u64(block_row * 4 + j) * item.height / height) * item.width;
                for (u32 k = 0; k < 4 && block_column * 4 + k < width; ++k) {
                    const size_t i_out = off_row + out_columns[block_column * 4 + k];
                    for (u32 c = 0; c < 4; ++c) {
                        rgba[i_out * 4 + c] += block[(j * 4 + k) * 4 + c];
                    }
                    ++c_summed[i_out];
                }
            }
        }
    }
    for (size_t i = 0; i < c_summed.size(); ++i) {
        for (u32 c = 0; c < 4; ++c) {
            rgba[i * 4 + c] /= float(std::max(c_summed[i], 1u));
        }
    }
    return rgba;
}

u32 StagedTexelBytes(vk::Format format) {
    if (format == COLOR_FORMAT) {
        return 8;
    }
    return format == vk::Format::eD16Unorm || format == vk::Format::eD16UnormS8Uint ? 2 : 4;
}

/// Waits for the blits recorded so far and converts them, so that the staging buffer starts
/// over. The GPU stalls here, in captured frames only.
void Drain(Runtime& runtime) {
    const bool has_pending = std::ranges::any_of(
        s_items, [](const Item& item) { return item.off_staging != NOT_STAGED; });
    if (!has_pending || !s_p_staging) {
        s_off_staging = 0;
        return;
    }
    auto& scheduler = runtime.GetScheduler();
    scheduler.EndRendering();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
    scheduler.Finish();
    ++s_c_waits;
    s_p_staging->Invalidate(0, s_off_staging);
    const u8* p_data = s_p_staging->mapped_data.data();
    for (auto& item : s_items) {
        if (item.off_staging == NOT_STAGED) {
            continue;
        }
        std::vector<float> rgba = item.is_compressed
                                      ? DecodeCompressed(item, p_data + item.off_staging)
                                      : DecodeStaged(item, p_data + item.off_staging);
        Remap(item, rgba);
        Convert(item, rgba, item.width * item.height);
        item.off_staging = NOT_STAGED;
    }
    s_off_staging = 0;
}

VideoCore::UniqueImage* ScratchFor(Runtime& runtime, vk::Format format) {
    for (const auto& scratch : s_scratches) {
        if (scratch.format == format) {
            return scratch.p_image.get();
        }
    }
    const auto& instance = runtime.GetInstance();
    auto p_image =
        std::make_unique<VideoCore::UniqueImage>(instance.GetDevice(), instance.GetAllocator());
    const vk::ImageCreateInfo image_ci{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {WIDTH_MAX, HEIGHT_MAX, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    if (!p_image->Create(image_ci, false)) {
        std::printf("Frame capture: no %ux%u %s image for the dump's blits\n", WIDTH_MAX,
                    HEIGHT_MAX, vk::to_string(format).c_str());
        return nullptr;
    }
    s_scratches.push_back({format, std::move(p_image)});
    return s_scratches.back().p_image.get();
}

VideoCore::Image::State TrackedState(const VideoCore::Image& image, u32 i_level, u32 i_layer) {
    const auto& backing = *image.backing;
    return backing.subresource_states.empty()
               ? backing.state
               : backing.subresource_states[i_level * image.info.resources.layers + i_layer];
}

/// Records `record` with the subresource in transfer source layout, then puts the image back in
/// the state it was tracked in, so that the layout an already prepared attachment or descriptor
/// names stays true.
template <typename Record>
void RecordFromSource(Runtime& runtime, VideoCore::Image& image, const Source& source,
                      vk::PipelineStageFlags2 stage, Record&& record) {
    const VideoCore::Image::State saved = TrackedState(image, source.i_level, source.i_layer);
    auto& scheduler = runtime.GetScheduler();
    const VideoCore::SubresourceRange range{.base = {source.i_level, source.i_layer},
                                            .extent = {1, 1}};
    // Barriers already pending may transition this very image; one pipeline barrier must not
    // hold two transitions of a subresource, so they go first.
    runtime.FlushBarriers();
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, stage,
                    vk::AccessFlagBits2::eTransferRead, range);
    runtime.FlushBarriers();
    scheduler.Record(std::forward<Record>(record));
    runtime.Transit(&image, saved.layout, saved.pl_stage, saved.access_mask, range);
    runtime.FlushBarriers();
}

/// Room for `cb_image` more bytes of staging, draining it when allowed; false when the image was
/// listed as skipped instead.
bool ReserveStaging(Runtime& runtime, const Source& source, u64 cb_image, bool is_wait_allowed) {
    if (s_off_staging + cb_image <= CB_STAGING) {
        return true;
    }
    if (!is_wait_allowed) {
        ++s_c_budget_skipped;
        Skip(source, std::format("budget ({} MiB staging full where no wait is allowed)",
                                 CB_STAGING >> 20));
        return false;
    }
    Drain(runtime);
    return true;
}

/// BC1 to BC5: the whole subresource copied into the staging buffer, for DecodeCompressed.
BlitResult RecordCompressed(Runtime& runtime, VideoCore::Image& image, const Source& source,
                            bool is_wait_allowed) {
    const auto& ci = image.backing->image.image_ci;
    if (!s_p_staging) {
        Skip(source, "no staging buffer");
        return BlitResult::Skipped;
    }
    if (ci.tiling != vk::ImageTiling::eOptimal) {
        Skip(source, "linear tiling");
        return BlitResult::Skipped;
    }
    const u32 width = std::max(ci.extent.width >> source.i_level, 1u);
    const u32 height = std::max(ci.extent.height >> source.i_level, 1u);
    const u64 cb_blocks =
        u64((width + 3) / 4) * ((height + 3) / 4) * CaptureBc::BlockBytes(ci.format);
    const u64 cb_image = (cb_blocks + 15) & ~u64(15);
    if (cb_image > CB_STAGING) {
        Skip(source, std::format("too large: {} KiB of blocks, the staging buffer holds {} MiB",
                                 cb_blocks >> 10, CB_STAGING >> 20));
        return BlitResult::Skipped;
    }
    if (!ReserveStaging(runtime, source, cb_image, is_wait_allowed)) {
        return BlitResult::Skipped;
    }
    const bool is_3d = ci.imageType == vk::ImageType::e3D;
    const vk::BufferImageCopy copy{
        .bufferOffset = s_off_staging,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, source.i_level,
                             is_3d ? 0 : source.i_layer, 1},
        .imageOffset = {0, 0, is_3d ? s32(source.i_layer) : 0},
        .imageExtent = {width, height, 1},
    };
    RecordFromSource(runtime, image, source, vk::PipelineStageFlagBits2::eCopy,
                     [vk_image = image.GetImage(), vk_staging = s_p_staging->Handle(),
                      copy](vk::CommandBuffer cmdbuf) {
                         cmdbuf.copyImageToBuffer(vk_image, vk::ImageLayout::eTransferSrcOptimal,
                                                  vk_staging, 1, &copy);
                     });
    const Kind kind = Classify(ci.format);
    const auto [out_width, out_height] = FitSize(width, height);
    s_items.push_back({
        .source = source,
        .method = "whole copy, decoded on the cpu, box filter",
        // Decoded as stored: an sRGB texture stays encoded.
        .conversion = kind.conversion == Conversion::Srgb ? Conversion::Unorm : kind.conversion,
        .c_channels = ShownChannels(source, kind),
        .staged_format = ci.format,
        .width = out_width,
        .height = out_height,
        .off_staging = s_off_staging,
        .is_compressed = true,
        .compressed_width = width,
        .compressed_height = height,
    });
    s_off_staging += cb_image;
    ++s_c_images;
    return BlitResult::Recorded;
}

/// Why the PNG could not be written, or empty. `pixels`: `c_channels` bytes per pixel.
std::string WritePng(std::span<const u8> pixels, u32 c_channels, u32 width, u32 height,
                     const std::string& path) {
    size_t cb_png = 0;
    void* p_png = tdefl_write_image_to_png_file_in_memory_ex(pixels.data(), int(width),
                                                             int(height), int(c_channels), &cb_png,
                                                             MZ_DEFAULT_LEVEL, MZ_FALSE);
    if (!p_png) {
        return "png encoding failed";
    }
    FILE* p_file = std::fopen(path.c_str(), "wb");
    bool is_written = p_file && std::fwrite(p_png, 1, cb_png, p_file) == cb_png;
    if (p_file) {
        is_written &= std::fclose(p_file) == 0;
    }
    mz_free(p_png);
    return is_written ? std::string{} : std::format("cannot write {}", path);
}

} // namespace

u32 Level() {
    static const u32 s_level = [] {
        const char* sz_value = std::getenv("PS_CAPTURE_DUMP");
        return sz_value ? std::min(u32(std::strtoul(sz_value, nullptr, 10)), 2u) : 0u;
    }();
    return s_level;
}

void StartFrame(Runtime& runtime) {
    s_items.clear();
    s_notes.clear();
    s_off_staging = 0;
    s_c_images = 0;
    s_c_budget_skipped = 0;
    s_c_waits = 0;
    if (s_p_staging || s_is_staging_failed) {
        return;
    }
    s_p_staging = std::make_unique<VideoCore::Buffer>(runtime.GetInstance(), 0, CB_STAGING,
                                                      VideoCore::MemoryType::HostCached,
                                                      "frame capture image dump", false);
    if (!s_p_staging->IsAllocated() || s_p_staging->mapped_data.size() < CB_STAGING) {
        std::printf("Frame capture: no host-visible image dump buffer of %llu MiB, only the "
                    "grid fallback dumps images\n",
                    static_cast<unsigned long long>(CB_STAGING >> 20));
        s_p_staging.reset();
        s_is_staging_failed = true;
    }
}

bool HasRoom(const Source& source) {
    if (s_c_images < C_IMAGES_MAX) {
        return true;
    }
    ++s_c_budget_skipped;
    Skip(source, std::format("budget ({} images per frame)", C_IMAGES_MAX));
    return false;
}

void Note(std::string line) {
    s_notes.push_back(std::move(line));
}

void Skip(const Source& source, std::string reason) {
    s_items.push_back({.source = source, .skipped = std::move(reason)});
}

BlitResult RecordBlit(Runtime& runtime, VideoCore::Image& image, const Source& source,
                      bool is_wait_allowed, std::string& out_reason) {
    const auto& ci = image.backing->image.image_ci;
    const vk::Format format = ci.format;
    if (CaptureBc::BlockBytes(format)) {
        return RecordCompressed(runtime, image, source, is_wait_allowed);
    }
    const Kind kind = Classify(format);
    const bool has_depth = bool(image.aspect_mask & vk::ImageAspectFlagBits::eDepth);
    const bool is_depth_image =
        has_depth || bool(image.aspect_mask & vk::ImageAspectFlagBits::eStencil);
    const auto features =
        runtime.GetInstance().GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    const vk::Format staged_format = is_depth_image ? format : COLOR_FORMAT;
    if (!s_p_staging) {
        out_reason = "no staging buffer";
    } else if (ci.tiling != vk::ImageTiling::eOptimal) {
        out_reason = "linear tiling";
    } else if (!(features & vk::FormatFeatureFlagBits::eBlitSrc)) {
        out_reason = "format not a blit source";
    } else if (is_depth_image && !has_depth) {
        out_reason = "stencil only";
    } else if (kind.is_integer) {
        out_reason = "integer format";
    } else if (is_depth_image && !(features & vk::FormatFeatureFlagBits::eBlitDst)) {
        out_reason = "depth format not a blit destination";
    }
    if (!out_reason.empty()) {
        return BlitResult::Fallback;
    }

    const u32 width = std::max(ci.extent.width >> source.i_level, 1u);
    const u32 height = std::max(ci.extent.height >> source.i_level, 1u);
    const auto [out_width, out_height] = FitSize(width, height);
    const u64 cb_image = (u64(out_width) * out_height * StagedTexelBytes(staged_format) + 15) &
                         ~u64(15);
    if (!ReserveStaging(runtime, source, cb_image, is_wait_allowed)) {
        return BlitResult::Skipped;
    }
    VideoCore::UniqueImage* p_scratch = ScratchFor(runtime, staged_format);
    if (!p_scratch) {
        out_reason = "no scratch image";
        return BlitResult::Fallback;
    }

    const bool is_linear =
        !is_depth_image && bool(features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
    const vk::ImageAspectFlags aspect =
        is_depth_image ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    const bool is_3d = ci.imageType == vk::ImageType::e3D;
    const s32 z = is_3d ? s32(source.i_layer) : 0;
    const vk::ImageBlit blit{
        .srcSubresource = {aspect, source.i_level, is_3d ? 0 : source.i_layer, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, z},
                                 vk::Offset3D{s32(width), s32(height), z + 1}},
        .dstSubresource = {aspect, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{s32(out_width), s32(out_height), 1}},
    };
    const vk::BufferImageCopy copy{
        .bufferOffset = s_off_staging,
        .imageSubresource = {aspect, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {out_width, out_height, 1},
    };

    RecordFromSource(runtime, image, source, vk::PipelineStageFlagBits2::eBlit,
                     [vk_image = image.GetImage(), vk_scratch = p_scratch->image,
                      vk_staging = s_p_staging->Handle(), blit, copy, aspect,
                      filter = is_linear ? vk::Filter::eLinear
                                         : vk::Filter::eNearest](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange scratch_range{aspect, 0, 1, 0, 1};
        // The previous dump's copy out of the scratch image must be done reading it.
        const vk::ImageMemoryBarrier2 to_destination{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = vk_scratch,
            .subresourceRange = scratch_range,
        };
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_destination});
        cmdbuf.blitImage(vk_image, vk::ImageLayout::eTransferSrcOptimal, vk_scratch,
                         vk::ImageLayout::eTransferDstOptimal, 1, &blit, filter);
        const vk::ImageMemoryBarrier2 to_source{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = vk_scratch,
            .subresourceRange = scratch_range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_source});
        cmdbuf.copyImageToBuffer(vk_scratch, vk::ImageLayout::eTransferSrcOptimal, vk_staging, 1,
                                 &copy);
    });

    s_items.push_back({
        .source = source,
        .method = is_linear ? "blit linear" : "blit nearest",
        .conversion = kind.conversion,
        .c_channels = ShownChannels(source, kind),
        .staged_format = staged_format,
        .width = out_width,
        .height = out_height,
        .off_staging = s_off_staging,
    });
    s_off_staging += cb_image;
    ++s_c_images;
    return BlitResult::Recorded;
}

u32 BeginGrid(const Source& source, std::string reason) {
    const Kind kind = Classify(source.format);
    const auto [width, height] = FitSize(source.width, source.height);
    // The grid holds the stored values: an sRGB target reads back still encoded.
    s_items.push_back({
        .source = source,
        .conversion = kind.conversion == Conversion::Srgb ? Conversion::Unorm : kind.conversion,
        .c_channels = ShownChannels(source, kind),
        .width = width,
        .height = height,
        .fallback = std::move(reason),
    });
    ++s_c_images;
    return u32(s_items.size() - 1);
}

void FillGrid(u32 i_item, std::span<const float> rgba, u32 c_columns, u32 c_rows) {
    auto& item = s_items[i_item];
    item.method = std::format("grid {}x{} nearest", c_columns, c_rows);
    // Enlarged to the blit's output size with the aspect ratio of the target, which the square
    // grid does not keep.
    std::vector<float> enlarged(size_t(item.width) * item.height * 4);
    for (u32 y = 0; y < item.height; ++y) {
        const u32 row = std::min(y * c_rows / item.height, c_rows - 1);
        for (u32 x = 0; x < item.width; ++x) {
            const u32 column = std::min(x * c_columns / item.width, c_columns - 1);
            std::memcpy(enlarged.data() + (size_t(y) * item.width + x) * 4,
                        rgba.data() + (size_t(row) * c_columns + column) * 4, 4 * sizeof(float));
        }
    }
    Remap(item, enlarged);
    Convert(item, enlarged, item.width * item.height);
}

void FailGrid(u32 i_item, std::string reason) {
    auto& item = s_items[i_item];
    item.skipped = std::move(reason);
    --s_c_images;
}

std::string WriteFrame(Runtime& runtime, const std::string& stem) {
    Drain(runtime);
    const std::string index_path = stem + "_dump.txt";
    FILE* p_index = std::fopen(index_path.c_str(), "w");
    if (!p_index) {
        std::printf("Frame capture: cannot write %s\n", index_path.c_str());
    }
    const std::string name = stem.substr(stem.find_last_of('/') + 1);
    if (p_index) {
        std::fprintf(p_index,
                     "image dump of %s.txt, PS_CAPTURE_DUMP=%u; one line per attachment: "
                     "'<file> pass <n> <c<k>|d> target <address> <format> <W>x<H> level <l> "
                     "layer <m> [swizzle <s>] -> <w>x<h> <method> conversion <conversion> "
                     "<values>', or '- pass ... not dumped: <reason>'; one per texture of a "
                     "PS_CAPTURE_DUMP_TEX draw: '<file> [alpha <file>] tex pass <n> draw <d> "
                     "binding <b> image <address> <format> <W>x<H> level <l> layer <m> swizzle "
                     "<s> view <format> stage <hash> image binding <i> <identity> -> ...'; "
                     "blits fit in %ux%u keeping the aspect ratio; channels as the shader wrote "
                     "(targets: inverse of the export swizzle) or reads them (textures: the view's "
                     "swizzle, from the stored r g b a, 0 and 1); non-finite texels are magenta\n",
                     name.c_str(), Level(), WIDTH_MAX, HEIGHT_MAX);
    }
    for (const auto& note : s_notes) {
        std::printf("Frame capture: %s\n", note.c_str());
        if (p_index) {
            std::fprintf(p_index, "%s\n", note.c_str());
        }
    }
    u32 c_written = 0, c_not_dumped = 0, c_textures_written = 0, c_textures_not_dumped = 0;
    std::vector<std::string> fallbacks;
    for (auto& item : s_items) {
        const Source& s = item.source;
        std::string target =
            s.is_texture
                ? std::format("tex pass {} draw {} binding {} image {:#x} {} {}x{} level {} layer "
                              "{} swizzle {} {}",
                              s.i_pass, s.i_draw, s.i_binding, s.va_target,
                              vk::to_string(s.format), s.width, s.height, s.i_level, s.i_layer,
                              MappingName(s.mapping), s.detail)
                : std::format("pass {} {} target {:#x} {} {}x{} level {} layer {}", s.i_pass,
                              AttachmentName(s), s.va_target, vk::to_string(s.format), s.width,
                              s.height, s.i_level, s.i_layer);
        if (!s.is_texture && MappingName(s.mapping) != "rgba") {
            target += std::format(" swizzle {}", MappingName(s.mapping));
        }
        if (!item.fallback.empty()) {
            std::string fallback =
                std::format("{} ({})", vk::to_string(s.format), item.fallback);
            if (std::ranges::find(fallbacks, fallback) == fallbacks.end()) {
                fallbacks.push_back(std::move(fallback));
            }
        }
        if (item.skipped.empty() && item.rgb.empty()) {
            item.skipped = "not read back";
        }
        const std::string file_stem = FileName(s);
        const std::string file = std::format("{}_{}.png", name, file_stem);
        if (item.skipped.empty()) {
            item.skipped = WritePng(item.rgb, 3, item.width, item.height,
                                    std::format("{}_{}.png", stem, file_stem));
        }
        std::string alpha_file;
        if (item.skipped.empty() && !item.alpha.empty()) {
            alpha_file = std::format("{}_{}_a.png", name, file_stem);
            if (const std::string why = WritePng(item.alpha, 1, item.width, item.height,
                                                 std::format("{}_{}_a.png", stem, file_stem));
                !why.empty()) {
                alpha_file = std::format("(alpha not written: {})", why);
                std::printf("Frame capture: %s: %s\n", file.c_str(), alpha_file.c_str());
            }
        }
        if (!item.skipped.empty()) {
            ++(s.is_texture ? c_textures_not_dumped : c_not_dumped);
            if (s.is_texture) {
                std::printf("Frame capture: texture of pass %u draw %u binding %u not dumped: "
                            "%s\n",
                            s.i_pass, s.i_draw, s.i_binding, item.skipped.c_str());
            }
            if (p_index) {
                std::fprintf(p_index, "- %s not dumped: %s\n", target.c_str(),
                             item.skipped.c_str());
            }
            continue;
        }
        ++(s.is_texture ? c_textures_written : c_written);
        if (s.is_depth) {
            std::printf("Frame capture: dump pass %u depth %s %s\n", s.i_pass,
                        vk::to_string(s.format).c_str(), item.values.c_str());
        }
        if (p_index) {
            const std::string fallback =
                item.fallback.empty() ? std::string{}
                                      : std::format(" (fallback: {})", item.fallback);
            const std::string alpha =
                alpha_file.empty() ? std::string{} : std::format(" alpha {}", alpha_file);
            std::fprintf(p_index, "%s%s %s -> %ux%u %s%s conversion %s %s\n", file.c_str(),
                         alpha.c_str(), target.c_str(), item.width, item.height,
                         item.method.c_str(), fallback.c_str(), ConversionName(item.conversion),
                         item.values.c_str());
        }
    }
    if (p_index) {
        std::fclose(p_index);
    }
    std::string fallback_list;
    for (const auto& fallback : fallbacks) {
        fallback_list += (fallback_list.empty() ? "" : ", ") + fallback;
    }
    std::string line = std::format(
        "image dump: {} written ({}_p<pass>_<c<k>|d>.png), {} not dumped ({} over the budget of "
        "{} images and {} MiB staging), {} staging waits; grid fallback: {}; index {}_dump.txt",
        c_written, name, c_not_dumped, s_c_budget_skipped, C_IMAGES_MAX, CB_STAGING >> 20,
        s_c_waits, fallback_list.empty() ? std::string{"none"} : fallback_list, name);
    if (c_textures_written || c_textures_not_dumped) {
        line += std::format("; textures of PS_CAPTURE_DUMP_TEX draws: {} written "
                            "({}_tex_p<pass>_d<draw>_b<binding>.png), {} not dumped",
                            c_textures_written, name, c_textures_not_dumped);
    }
    std::printf("Frame capture: %s\n", line.c_str());
    s_items.clear();
    s_notes.clear();
    s_off_staging = 0;
    return line;
}

} // namespace Vulkan::CaptureDump
