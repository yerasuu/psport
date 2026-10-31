// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture_dump.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <utility>
#include <vector>
#include <miniz.h>
#include <vulkan/vulkan_format_traits.hpp>

#include "video_core/buffer_cache/buffer.h"
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
    std::vector<u8> rgb;
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

u8 ToByte(float value) {
    return u8(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

float EncodeSrgb(float linear) {
    return linear <= 0.0031308f ? linear * 12.92f
                                : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
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
        const std::vector<float> rgba = DecodeStaged(item, p_data + item.off_staging);
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

/// Why the PNG could not be written, or empty.
std::string WritePng(const Item& item, const std::string& path) {
    size_t cb_png = 0;
    void* p_png = tdefl_write_image_to_png_file_in_memory_ex(item.rgb.data(), int(item.width),
                                                             int(item.height), 3, &cb_png,
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

void Skip(const Source& source, std::string reason) {
    s_items.push_back({.source = source, .skipped = std::move(reason)});
}

BlitResult RecordBlit(Runtime& runtime, VideoCore::Image& image, const Source& source,
                      bool is_wait_allowed, std::string& out_reason) {
    const auto& ci = image.backing->image.image_ci;
    const vk::Format format = ci.format;
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
    if (s_off_staging + cb_image > CB_STAGING) {
        if (!is_wait_allowed) {
            ++s_c_budget_skipped;
            Skip(source, std::format("budget ({} MiB staging full where no wait is allowed)",
                                     CB_STAGING >> 20));
            return BlitResult::Skipped;
        }
        Drain(runtime);
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

    const VideoCore::Image::State saved = TrackedState(image, source.i_level, source.i_layer);
    auto& scheduler = runtime.GetScheduler();
    const VideoCore::SubresourceRange range{.base = {source.i_level, source.i_layer},
                                            .extent = {1, 1}};
    // Barriers already pending may transition this very image; one pipeline barrier must not
    // hold two transitions of a subresource, so they go first.
    runtime.FlushBarriers();
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead, range);
    runtime.FlushBarriers();
    scheduler.Record([vk_image = image.GetImage(), vk_scratch = p_scratch->image,
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
    runtime.Transit(&image, saved.layout, saved.pl_stage, saved.access_mask, range);
    runtime.FlushBarriers();

    s_items.push_back({
        .source = source,
        .method = is_linear ? "blit linear" : "blit nearest",
        .conversion = kind.conversion,
        .c_channels = kind.c_channels,
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
        .c_channels = kind.c_channels,
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
    const std::string_view name = stem.substr(stem.find_last_of('/') + 1);
    if (p_index) {
        std::fprintf(p_index,
                     "image dump of %.*s.txt, PS_CAPTURE_DUMP=%u; one line per attachment: "
                     "'<file> pass <n> <c<k>|d> target <address> <format> <W>x<H> level <l> "
                     "layer <m> -> <w>x<h> <method> conversion <conversion> <values>', or "
                     "'- pass ... not dumped: <reason>'; blits fit in %ux%u keeping the aspect "
                     "ratio; non-finite texels are magenta\n",
                     int(name.size()), name.data(), Level(), WIDTH_MAX, HEIGHT_MAX);
    }
    u32 c_written = 0, c_not_dumped = 0;
    std::vector<std::string> fallbacks;
    for (auto& item : s_items) {
        const Source& s = item.source;
        const std::string attachment = AttachmentName(s);
        const std::string target =
            std::format("pass {} {} target {:#x} {} {}x{} level {} layer {}", s.i_pass, attachment,
                        s.va_target, vk::to_string(s.format), s.width, s.height, s.i_level,
                        s.i_layer);
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
        const std::string file = std::format("{}_p{}_{}.png", name, s.i_pass, attachment);
        if (item.skipped.empty()) {
            item.skipped =
                WritePng(item, std::format("{}_p{}_{}.png", stem, s.i_pass, attachment));
        }
        if (!item.skipped.empty()) {
            ++c_not_dumped;
            if (p_index) {
                std::fprintf(p_index, "- %s not dumped: %s\n", target.c_str(),
                             item.skipped.c_str());
            }
            continue;
        }
        ++c_written;
        if (s.is_depth) {
            std::printf("Frame capture: dump pass %u depth %s %s\n", s.i_pass,
                        vk::to_string(s.format).c_str(), item.values.c_str());
        }
        if (p_index) {
            const std::string fallback =
                item.fallback.empty() ? std::string{}
                                      : std::format(" (fallback: {})", item.fallback);
            std::fprintf(p_index, "%s %s -> %ux%u %s%s conversion %s %s\n", file.c_str(),
                         target.c_str(), item.width, item.height, item.method.c_str(),
                         fallback.c_str(), ConversionName(item.conversion), item.values.c_str());
        }
    }
    if (p_index) {
        std::fclose(p_index);
    }
    std::string fallback_list;
    for (const auto& fallback : fallbacks) {
        fallback_list += (fallback_list.empty() ? "" : ", ") + fallback;
    }
    const std::string line = std::format(
        "image dump: {} written ({}_p<pass>_<c<k>|d>.png), {} not dumped ({} over the budget of "
        "{} images and {} MiB staging), {} staging waits; grid fallback: {}; index {}_dump.txt",
        c_written, name, c_not_dumped, s_c_budget_skipped, C_IMAGES_MAX, CB_STAGING >> 20,
        s_c_waits, fallback_list.empty() ? std::string{"none"} : fallback_list, name);
    std::printf("Frame capture: %s\n", line.c_str());
    s_items.clear();
    s_off_staging = 0;
    return line;
}

} // namespace Vulkan::CaptureDump
