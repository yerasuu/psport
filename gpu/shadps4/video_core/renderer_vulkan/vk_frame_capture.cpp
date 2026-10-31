// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <xxhash.h>

#include "video_core/amdgpu/regs.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_frame_capture_draw.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct Target {
    VAddr va_target{};
    vk::Format format{};
    u32 width{}, height{};
    bool operator==(const Target&) const = default;
};

enum DepthUseBits : u32 { DepthTest = 1, DepthWrite = 2, StencilTest = 4 };

struct ImageRef {
    VideoCore::ImageId id{};
    u64 uid{};
    u32 i_level{};
    u32 i_layer{};
};

/// How a texel read back from a summary grid is turned into up to four channel values.
enum class Decode : u32 {
    None,
    Unorm8,
    Snorm8,
    Uint8,
    Bgra8,
    Unorm16,
    Snorm16,
    Uint16,
    Float16,
    Float32,
    Uint32,
    B10G11R11,
    A2B10G10R10,
    A2R10G10B10,
    Depth16,
    Depth24,
    Depth32,
    Stencil8,
};

struct TexelLayout {
    Decode decode{Decode::None};
    u32 c_channels{};
    u32 cb_texel{};
};

struct Readback {
    std::string label;
    std::string identity;
    Target target;
    TexelLayout layout;
    u32 c_columns{};
    u32 c_rows{};
    u32 cb_stride{};
    u64 off_data{};
    std::string skipped; ///< why nothing was read back; empty when it was
};

struct Sample {
    Target target;
    bool is_storage{};
    ImageRef ref;
    vk::ImageView view{};
    bool is_render_target{};
    std::string identity;
};

struct StateRun {
    u32 i_first{};
    u32 i_last{};
    std::string text;
};

constexpr u32 NO_QUERY = ~0u;

struct DrawDetail {
    u32 i_draw{};
    u32 i_query{NO_QUERY};
    bool has_occlusion{};
    std::string geometry; ///< empty past the first draws of a pass
    bool has_gpu{};
    CaptureDraw::GpuGeometry gpu;
};

/// A guest buffer a dispatch binds, with copies of what the GPU had there before it ran (read
/// bindings) and after (written ones).
struct BindingCopy {
    u32 i_slot{};
    VAddr va_buffer{};
    u64 cb_buffer{};
    bool is_written{};
    CaptureDraw::GpuRange start;
    CaptureDraw::GpuRange end;
};

/// A whole depth/stencil subresource copied into a readback buffer: depth texels from offset 0,
/// stencil bytes from off_stencil.
struct FullPlanes {
    bool is_valid{};
    std::string skipped;
    u64 uid{};
    u32 width{};
    u32 height{};
    TexelLayout depth_layout; ///< Decode::None without a depth plane
    bool has_stencil{};
    u64 off_stencil{};
};

struct Entry {
    bool is_compute{};
    bool is_copy{};
    std::vector<Target> colors;
    Target depth;
    u32 depth_use{}; ///< DepthUseBits over the pass's draws; for a copy, depth and stencil moved
    Target copy_dst;
    u32 draws{};
    u64 indices{};
    std::vector<u64> shaders; ///< vs/ps pairs (graphics) or cs (compute), first few
    std::vector<Sample> sampled; ///< textures and storage images
    std::string note;
    std::vector<std::string> buffers; ///< dumped constants, per draw
    // PS_CAPTURE_SUMMARIZE only.
    u64 tick{};
    std::string route;
    std::vector<ImageRef> color_refs;
    ImageRef depth_ref;
    std::vector<std::string> identities; ///< colors then depth, or a copy's source and destination
    bool is_summarized{};
    std::vector<StateRun> states;
    u32 c_states_omitted{};
    std::vector<Readback> readbacks;
    std::vector<DrawDetail> draw_details;
    std::vector<BindingCopy> bindings; ///< compute: of the entry's first dispatch
    bool is_dispatch_copied{};
    FullPlanes full_start;
    std::string full_text;
};

constexpr u32 SUMMARY_GRID = 64;
constexpr u32 SUMMARY_CELLS = 16;
constexpr u64 CB_READBACK = 48ull << 20;
constexpr size_t C_STATE_RUNS_MAX = 24;
constexpr size_t C_SAMPLED_MAX = 32;
constexpr u32 C_DRAWS_DETAILED = 16;
constexpr u32 C_DRAWS_VERTEX_DUMPED = 4;
constexpr u32 C_QUERIES_MAX = 8192;
constexpr u64 CB_FULL_PLANES = 12ull << 20;
constexpr u32 C_STATISTICS = 6;
constexpr u64 CB_GPU_COPIES = 128ull << 20;
constexpr u64 CB_GPU_RANGE_MAX = 1ull << 20;
constexpr size_t C_BINDINGS_KEPT = 32;
constexpr u32 C_BINDINGS_COPIED = 4;
constexpr u32 C_STORAGE_IMAGES_READ = 2;
constexpr u32 C_WORDS_SHOWN = 32;

u32 SummarizeLevel() {
    static const u32 s_level = [] {
        const char* sz_value = std::getenv("PS_CAPTURE_SUMMARIZE");
        return sz_value ? u32(std::strtoul(sz_value, nullptr, 10)) : 0u;
    }();
    return s_level;
}

// GPU thread only.
std::vector<Entry> s_entries;
std::vector<Sample> s_pending_sampled;
std::vector<std::string> s_pending_buffers;
VideoCore::TextureCache* s_p_texture_cache = nullptr;
Runtime* s_p_runtime = nullptr;
std::unique_ptr<VideoCore::Buffer> s_p_readback;
u64 s_off_readback = 0;
u32 s_c_readbacks = 0;
u32 s_c_skipped = 0;
std::unique_ptr<VideoCore::Buffer> s_p_full_start;
std::unique_ptr<VideoCore::Buffer> s_p_full_end;
u32 s_c_full_waits = 0;
vk::UniqueQueryPool s_h_occlusion_pool;
vk::UniqueQueryPool s_h_statistics_pool;
bool s_is_queries_reset = false;
u32 s_c_queries = 0;
u32 s_i_pending_query = NO_QUERY;
bool s_is_pending_occlusion = false;
std::unique_ptr<VideoCore::Buffer> s_p_gpu_copies;
u64 s_off_gpu_copies = 0;
u32 s_c_gpu_copies = 0;
u32 s_c_gpu_copies_skipped = 0;
std::vector<BindingCopy> s_pending_bindings;

void AddBuffers(Entry& entry) {
    // Constants of full-screen passes (post-processing: camera matrices) and of the first draw
    // of each geometry pass.
    if (entry.draws <= 4) {
        for (auto& b : s_pending_buffers) {
            entry.buffers.push_back(std::move(b));
        }
    }
    s_pending_buffers.clear();
}
bool s_is_pass_open = false;
std::mutex s_display_mutex;
std::vector<VAddr> s_display_buffers;
// psport: read on every draw; the game registers a handful of display buffers once.
std::array<std::atomic<VAddr>, 16> s_display_list{};
std::atomic<u32> s_c_displays{0};

Target ToTarget(const VideoCore::ImageInfo& info) {
    return {info.guest_address, info.pixel_format, info.size.width, info.size.height};
}

std::string Describe(const Target& t) {
    if (!t.va_target) {
        return "-";
    }
    return std::format("{:#x} {} {}x{}", t.va_target, vk::to_string(t.format), t.width, t.height);
}

template <typename Handle>
u64 HandleBits(Handle handle) {
    return std::bit_cast<u64>(static_cast<typename Handle::CType>(handle));
}

/// Which texture-cache entry and Vulkan image stand behind an address: two images at one address
/// (a stale copy) differ here although Describe prints the same line for both.
std::string Identify(const VideoCore::Image& image, VideoCore::ImageId id) {
    if (!image.backing) {
        return std::format("img {} uid {} no backing", id.index, image.image_uid);
    }
    size_t i_backing = 0;
    for (const auto& backing : image.backing_images) {
        if (&backing == image.backing) {
            break;
        }
        ++i_backing;
    }
    return std::format("img {} uid {} vk {:#x} backing {}/{} written tick {} flags {:#x}",
                       id.index, image.image_uid, HandleBits(image.backing->image.image), i_backing,
                       image.backing_images.size(), image.gpu_write_tick, u32(image.flags));
}

VideoCore::Image* Resolve(const ImageRef& ref) {
    return s_p_texture_cache && ref.id ? s_p_texture_cache->TryGetImage(ref.id, ref.uid) : nullptr;
}

TexelLayout LayoutOf(vk::Format format) {
    using F = vk::Format;
    switch (format) {
    case F::eR8Unorm:
    case F::eR8Srgb:
        return {Decode::Unorm8, 1, 1};
    case F::eR8Snorm:
        return {Decode::Snorm8, 1, 1};
    case F::eR8Uint:
        return {Decode::Uint8, 1, 1};
    case F::eR8G8Unorm:
    case F::eR8G8Srgb:
        return {Decode::Unorm8, 2, 2};
    case F::eR8G8Snorm:
        return {Decode::Snorm8, 2, 2};
    case F::eR8G8Uint:
        return {Decode::Uint8, 2, 2};
    case F::eR8G8B8A8Unorm:
    case F::eR8G8B8A8Srgb:
        return {Decode::Unorm8, 4, 4};
    case F::eR8G8B8A8Snorm:
        return {Decode::Snorm8, 4, 4};
    case F::eR8G8B8A8Uint:
        return {Decode::Uint8, 4, 4};
    case F::eB8G8R8A8Unorm:
    case F::eB8G8R8A8Srgb:
        return {Decode::Bgra8, 4, 4};
    case F::eR16Unorm:
        return {Decode::Unorm16, 1, 2};
    case F::eR16Snorm:
        return {Decode::Snorm16, 1, 2};
    case F::eR16Uint:
        return {Decode::Uint16, 1, 2};
    case F::eR16Sfloat:
        return {Decode::Float16, 1, 2};
    case F::eR16G16Unorm:
        return {Decode::Unorm16, 2, 4};
    case F::eR16G16Snorm:
        return {Decode::Snorm16, 2, 4};
    case F::eR16G16Uint:
        return {Decode::Uint16, 2, 4};
    case F::eR16G16Sfloat:
        return {Decode::Float16, 2, 4};
    case F::eR16G16B16A16Unorm:
        return {Decode::Unorm16, 4, 8};
    case F::eR16G16B16A16Snorm:
        return {Decode::Snorm16, 4, 8};
    case F::eR16G16B16A16Uint:
        return {Decode::Uint16, 4, 8};
    case F::eR16G16B16A16Sfloat:
        return {Decode::Float16, 4, 8};
    case F::eR32Sfloat:
        return {Decode::Float32, 1, 4};
    case F::eR32Uint:
        return {Decode::Uint32, 1, 4};
    case F::eR32G32Sfloat:
        return {Decode::Float32, 2, 8};
    case F::eR32G32Uint:
        return {Decode::Uint32, 2, 8};
    case F::eR32G32B32A32Sfloat:
        return {Decode::Float32, 4, 16};
    case F::eR32G32B32A32Uint:
        return {Decode::Uint32, 4, 16};
    case F::eB10G11R11UfloatPack32:
        return {Decode::B10G11R11, 3, 4};
    case F::eA2B10G10R10UnormPack32:
        return {Decode::A2B10G10R10, 4, 4};
    case F::eA2R10G10B10UnormPack32:
        return {Decode::A2R10G10B10, 4, 4};
    case F::eD16Unorm:
    case F::eD16UnormS8Uint:
        return {Decode::Depth16, 1, 2};
    case F::eX8D24UnormPack32:
    case F::eD24UnormS8Uint:
        return {Decode::Depth24, 1, 4};
    case F::eD32Sfloat:
    case F::eD32SfloatS8Uint:
        return {Decode::Depth32, 1, 4};
    default:
        return {};
    }
}

/// The unsigned 11- and 10-bit floats of B10G11R11: a 5-bit exponent, no sign.
float SmallFloatToFloat(u32 bits, u32 c_mantissa_bits) {
    const u32 exponent = bits >> c_mantissa_bits;
    const u32 mantissa = bits & ((1u << c_mantissa_bits) - 1);
    if (exponent == 0) {
        return std::ldexp(float(mantissa), -14 - int(c_mantissa_bits));
    }
    if (exponent == 31) {
        return mantissa ? NAN : INFINITY;
    }
    return std::ldexp(float(mantissa | (1u << c_mantissa_bits)),
                      int(exponent) - 15 - int(c_mantissa_bits));
}

template <typename T>
T LoadChannel(const u8* p_texel, u32 i_channel) {
    T value;
    std::memcpy(&value, p_texel + i_channel * sizeof(T), sizeof(T));
    return value;
}

std::array<float, 4> DecodeTexel(const TexelLayout& layout, const u8* p_texel) {
    std::array<float, 4> values{};
    const u32 word = layout.cb_texel == 4 ? LoadChannel<u32>(p_texel, 0) : 0;
    for (u32 i = 0; i < layout.c_channels; ++i) {
        switch (layout.decode) {
        case Decode::Unorm8:
            values[i] = float(p_texel[i]) / 255.0f;
            break;
        case Decode::Snorm8:
            values[i] = std::max(float(s8(p_texel[i])) / 127.0f, -1.0f);
            break;
        case Decode::Uint8:
        case Decode::Stencil8:
            values[i] = float(p_texel[i]);
            break;
        case Decode::Bgra8:
            values[i] = float(p_texel[i == 3 ? 3 : 2 - i]) / 255.0f;
            break;
        case Decode::Unorm16:
        case Decode::Depth16:
            values[i] = float(LoadChannel<u16>(p_texel, i)) / 65535.0f;
            break;
        case Decode::Snorm16:
            values[i] = std::max(float(LoadChannel<s16>(p_texel, i)) / 32767.0f, -1.0f);
            break;
        case Decode::Uint16:
            values[i] = float(LoadChannel<u16>(p_texel, i));
            break;
        case Decode::Float16:
            values[i] = CaptureDraw::HalfToFloat(LoadChannel<u16>(p_texel, i));
            break;
        case Decode::Float32:
        case Decode::Depth32:
            values[i] = LoadChannel<float>(p_texel, i);
            break;
        case Decode::Uint32:
            values[i] = float(LoadChannel<u32>(p_texel, i));
            break;
        case Decode::B10G11R11:
            values[i] = i < 2 ? SmallFloatToFloat((word >> (11 * i)) & 0x7ff, 6)
                              : SmallFloatToFloat(word >> 22, 5);
            break;
        case Decode::A2B10G10R10:
            values[i] =
                i < 3 ? float((word >> (10 * i)) & 0x3ff) / 1023.0f : float(word >> 30) / 3.0f;
            break;
        case Decode::A2R10G10B10:
            values[i] = i < 3 ? float((word >> (10 * (2 - i))) & 0x3ff) / 1023.0f
                              : float(word >> 30) / 3.0f;
            break;
        case Decode::Depth24:
            values[i] = float(word & 0xffffff) / 16777215.0f;
            break;
        case Decode::None:
            break;
        }
    }
    return values;
}

/// The state the image tracker holds for one subresource.
VideoCore::Image::State TrackedState(const VideoCore::Image& image, u32 i_level, u32 i_layer) {
    const auto& backing = *image.backing;
    return backing.subresource_states.empty()
               ? backing.state
               : backing.subresource_states[i_level * image.info.resources.layers + i_layer];
}

/// Records `regions` copied from one subresource into `buffer`, in stream order. The image goes
/// to transfer layout and back to the state it was tracked in, so the layout an already
/// prepared attachment or descriptor names stays true.
void RecordCopy(VideoCore::Image& image, u32 i_level, u32 i_layer,
                std::span<const vk::BufferImageCopy> regions, const VideoCore::Buffer& buffer) {
    const VideoCore::Image::State saved = TrackedState(image, i_level, i_layer);
    auto& scheduler = s_p_runtime->GetScheduler();
    const VideoCore::SubresourceRange range{.base = {i_level, i_layer}, .extent = {1, 1}};
    // Barriers already pending may transition this very image; one pipeline barrier must not
    // hold two transitions of a subresource, so they go first.
    s_p_runtime->FlushBarriers();
    scheduler.EndRendering();
    s_p_runtime->Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                         vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead,
                         range);
    s_p_runtime->FlushBarriers();
    scheduler.Record([vk_image = image.GetImage(), vk_buffer = buffer.Handle(),
                      copies = scheduler.RecordData(regions)](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(vk_image, vk::ImageLayout::eTransferSrcOptimal, vk_buffer,
                                 u32(copies.size()), copies.data());
    });
    s_p_runtime->Transit(&image, saved.layout, saved.pl_stage, saved.access_mask, range);
    s_p_runtime->FlushBarriers();
}

/// Why a subresource cannot be copied out, or empty.
std::string CopyObstacle(const VideoCore::Image& image, u32 i_level, u32 i_layer) {
    if (!image.IsAllocated()) {
        return "image has no memory";
    }
    if (image.info.num_samples > 1 || image.backing->num_samples > 1) {
        return "multisampled";
    }
    if (!(image.backing->image.image_ci.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
        return "image not created as a transfer source";
    }
    if (i_level >= image.info.resources.levels || i_layer >= image.info.resources.layers) {
        return std::format("level {} layer {} outside the image", i_level, i_layer);
    }
    if (TrackedState(image, i_level, i_layer).layout == vk::ImageLayout::eUndefined) {
        return "never written (layout undefined)";
    }
    return {};
}

/// Records a copy of a grid of texels of one subresource into the readback buffer.
void ReadBack(VideoCore::Image& image, VideoCore::ImageId id, u32 i_level, u32 i_layer,
              std::string label, std::vector<Readback>& out_readbacks) {
    Readback base{.label = std::move(label),
                  .identity = Identify(image, id),
                  .target = ToTarget(image.info)};
    const auto skip = [&](std::string reason) {
        base.skipped = std::move(reason);
        ++s_c_skipped;
        out_readbacks.push_back(std::move(base));
    };
    if (!s_p_runtime || !s_p_readback) {
        return skip("no readback buffer");
    }
    if (auto obstacle = CopyObstacle(image, i_level, i_layer); !obstacle.empty()) {
        return skip(std::move(obstacle));
    }
    const auto& backing = *image.backing;

    struct Plane {
        vk::ImageAspectFlagBits aspect;
        TexelLayout layout;
        const char* sz_suffix;
    };
    const vk::Format format = backing.image.image_ci.format;
    std::vector<Plane> planes;
    if (image.aspect_mask & vk::ImageAspectFlagBits::eDepth) {
        planes.push_back({vk::ImageAspectFlagBits::eDepth, LayoutOf(format), ""});
    }
    if (image.aspect_mask & vk::ImageAspectFlagBits::eStencil) {
        planes.push_back({vk::ImageAspectFlagBits::eStencil, {Decode::Stencil8, 1, 1}, " stencil"});
    }
    if (planes.empty()) {
        planes.push_back({vk::ImageAspectFlagBits::eColor, LayoutOf(format), ""});
    }
    if (planes[0].layout.decode == Decode::None) {
        return skip(std::format("format {} not decoded", vk::to_string(format)));
    }

    const auto& extent = backing.image.image_ci.extent;
    const u32 width = std::max(extent.width >> i_level, 1u);
    const u32 height = std::max(extent.height >> i_level, 1u);
    const u32 c_columns = std::min(width, SUMMARY_GRID);
    const u32 c_rows = std::min(height, SUMMARY_GRID);
    const bool is_3d = backing.image.image_ci.imageType == vk::ImageType::e3D;
    u64 cb_needed = 0;
    for (const auto& plane : planes) {
        cb_needed += u64(c_columns) * c_rows * ((plane.layout.cb_texel + 3) & ~3u);
    }
    if (s_off_readback + cb_needed > CB_READBACK) {
        return skip("readback buffer full");
    }

    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(planes.size() * c_columns * c_rows);
    for (const auto& plane : planes) {
        Readback readback = base;
        readback.label += plane.sz_suffix;
        readback.layout = plane.layout;
        readback.c_columns = c_columns;
        readback.c_rows = c_rows;
        readback.cb_stride = (plane.layout.cb_texel + 3) & ~3u;
        readback.off_data = s_off_readback;
        for (u32 row = 0; row < c_rows; ++row) {
            for (u32 column = 0; column < c_columns; ++column) {
                regions.push_back({
                    .bufferOffset = s_off_readback,
                    .imageSubresource = {plane.aspect, i_level, is_3d ? 0 : i_layer, 1},
                    .imageOffset = {s32((2 * column + 1) * width / (2 * c_columns)),
                                    s32((2 * row + 1) * height / (2 * c_rows)),
                                    s32(is_3d ? i_layer : 0)},
                    .imageExtent = {1, 1, 1},
                });
                s_off_readback += readback.cb_stride;
            }
        }
        out_readbacks.push_back(std::move(readback));
        ++s_c_readbacks;
    }

    RecordCopy(image, i_level, i_layer, regions, *s_p_readback);
}

void ReadBackRef(const ImageRef& ref, std::string label, std::vector<Readback>& out_readbacks) {
    if (VideoCore::Image* p_image = Resolve(ref)) {
        ReadBack(*p_image, ref.id, ref.i_level, ref.i_layer, std::move(label), out_readbacks);
        return;
    }
    ++s_c_skipped;
    out_readbacks.push_back(
        {.label = std::move(label), .skipped = "image gone before its summary"});
}

/// Submits what was recorded and waits for it, with the copies made visible to the host.
void WaitForCopies() {
    auto& scheduler = s_p_runtime->GetScheduler();
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
}

/// Records copies of GPU buffer ranges into the capture's buffer, in stream order, between
/// barriers: the writes before them (shaders, transfers) are made visible to the copies, and the
/// writes after them wait until they have read.
void RecordBufferCopies(std::span<CaptureDraw::GpuRange* const> ranges) {
    std::vector<vk::Buffer> sources;
    std::vector<vk::BufferCopy> regions;
    for (auto* p_range : ranges) {
        if (!p_range->skipped.empty() || !p_range->h_buffer) {
            continue;
        }
        if (!s_p_gpu_copies || s_off_gpu_copies + p_range->cb_copy > CB_GPU_COPIES) {
            p_range->skipped = s_p_gpu_copies ? "budget full" : "no readback buffer";
            ++s_c_gpu_copies_skipped;
            continue;
        }
        p_range->off_data = s_off_gpu_copies;
        sources.push_back(p_range->h_buffer);
        regions.push_back({.srcOffset = p_range->off_buffer,
                           .dstOffset = s_off_gpu_copies,
                           .size = p_range->cb_copy});
        s_off_gpu_copies += (p_range->cb_copy + 15) & ~u64(15);
        ++s_c_gpu_copies;
    }
    if (regions.empty()) {
        return;
    }
    auto& scheduler = s_p_runtime->GetScheduler();
    s_p_runtime->FlushBarriers();
    scheduler.EndRendering();
    scheduler.Record([source_buffers = scheduler.RecordData(std::span<const vk::Buffer>(sources)),
                      copies = scheduler.RecordData(std::span<const vk::BufferCopy>(regions)),
                      destination = s_p_gpu_copies->Handle()](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        for (size_t i = 0; i < copies.size(); ++i) {
            cmdbuf.copyBuffer(source_buffers[i], destination, 1, &copies[i]);
        }
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
}

/// The vertex and index buffers the pass's first draws read, as the GPU holds them. Copied at
/// the pass's end because a copy ahead of each draw would split its render pass, replaying the
/// attachments' clears; a pass rarely writes the buffers it draws from.
void CopyPassGeometry(Entry& entry) {
    std::vector<CaptureDraw::GpuRange*> ranges;
    for (auto& detail : entry.draw_details) {
        if (!detail.has_gpu) {
            continue;
        }
        if (detail.gpu.cb_index) {
            ranges.push_back(&detail.gpu.indices);
        }
        for (auto& stream : detail.gpu.attributes) {
            ranges.push_back(&stream.range);
        }
    }
    RecordBufferCopies(ranges);
}

/// Records a copy of the whole depth and stencil planes of a pass's depth subresource.
FullPlanes CopyFull(const ImageRef& ref, const VideoCore::Buffer& buffer) {
    FullPlanes planes{.uid = ref.uid};
    VideoCore::Image* p_image = Resolve(ref);
    if (!p_image) {
        planes.skipped = "image gone";
        return planes;
    }
    auto& image = *p_image;
    planes.skipped = CopyObstacle(image, ref.i_level, ref.i_layer);
    if (!planes.skipped.empty()) {
        return planes;
    }
    const auto& ci = image.backing->image.image_ci;
    const bool has_depth = bool(image.aspect_mask & vk::ImageAspectFlagBits::eDepth);
    planes.has_stencil = bool(image.aspect_mask & vk::ImageAspectFlagBits::eStencil);
    planes.depth_layout = has_depth ? LayoutOf(ci.format) : TexelLayout{};
    if (has_depth && planes.depth_layout.decode == Decode::None) {
        planes.skipped = std::format("format {} not decoded", vk::to_string(ci.format));
        return planes;
    }
    if (!has_depth && !planes.has_stencil) {
        planes.skipped = "no depth or stencil plane";
        return planes;
    }
    planes.width = std::max(ci.extent.width >> ref.i_level, 1u);
    planes.height = std::max(ci.extent.height >> ref.i_level, 1u);
    const u64 c_texels = u64(planes.width) * planes.height;
    planes.off_stencil = (c_texels * planes.depth_layout.cb_texel + 3) & ~u64(3);
    const u64 cb_total = planes.off_stencil + (planes.has_stencil ? c_texels : 0);
    if (cb_total > CB_FULL_PLANES) {
        planes.skipped = std::format("{} KiB over the {} MiB budget", cb_total >> 10,
                                     CB_FULL_PLANES >> 20);
        return planes;
    }
    const bool is_3d = ci.imageType == vk::ImageType::e3D;
    const auto region = [&](vk::ImageAspectFlagBits aspect, u64 off_buffer) {
        return vk::BufferImageCopy{
            .bufferOffset = off_buffer,
            .imageSubresource = {aspect, ref.i_level, is_3d ? 0 : ref.i_layer, 1},
            .imageOffset = {0, 0, s32(is_3d ? ref.i_layer : 0)},
            .imageExtent = {planes.width, planes.height, 1},
        };
    };
    std::vector<vk::BufferImageCopy> regions;
    if (has_depth) {
        regions.push_back(region(vk::ImageAspectFlagBits::eDepth, 0));
    }
    if (planes.has_stencil) {
        regions.push_back(region(vk::ImageAspectFlagBits::eStencil, planes.off_stencil));
    }
    // The buffer is rewritten at every pass, possibly within one submission: the copy before
    // must be done writing it.
    auto& scheduler = s_p_runtime->GetScheduler();
    scheduler.EndRendering();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
    RecordCopy(image, ref.i_level, ref.i_layer, regions, buffer);
    planes.is_valid = true;
    return planes;
}

std::string RankValues(const std::array<u64, 256>& c_values, u64 c_total, size_t c_shown) {
    std::vector<std::pair<u64, u32>> ranked;
    for (u32 value = 0; value < 256; ++value) {
        if (c_values[value]) {
            ranked.emplace_back(c_values[value], value);
        }
    }
    std::ranges::sort(ranked, std::greater{});
    std::string text;
    for (size_t i = 0; i < std::min(ranked.size(), c_shown); ++i) {
        text += std::format(" {:#04x}:{} ({:.2f}%)", ranked[i].second, ranked[i].first,
                            100.0 * double(ranked[i].first) / double(std::max<u64>(c_total, 1)));
    }
    if (ranked.size() > c_shown) {
        text += std::format(" +{} more", ranked.size() - c_shown);
    }
    return text.empty() ? std::string{" none"} : text;
}

/// The pass's depth and stencil over the whole image, its end against its start.
std::string CompareFull(const FullPlanes& start, const FullPlanes& end) {
    s_p_full_start->Invalidate(0, CB_FULL_PLANES);
    s_p_full_end->Invalidate(0, CB_FULL_PLANES);
    const u8* p_start = s_p_full_start->mapped_data.data();
    const u8* p_end = s_p_full_end->mapped_data.data();
    const u64 c_texels = u64(end.width) * end.height;
    std::string text =
        std::format("  whole image {}x{} ({} texels), pass end against pass start:", end.width,
                    end.height, c_texels);
    const bool is_comparable = start.is_valid && start.width == end.width &&
                               start.height == end.height &&
                               start.depth_layout.decode == end.depth_layout.decode &&
                               start.has_stencil == end.has_stencil;
    if (!is_comparable) {
        text += std::format(" no start copy ({})", start.is_valid ? "another size or format"
                                                                   : start.skipped);
    }
    if (const auto& layout = end.depth_layout; layout.decode != Decode::None) {
        u64 c_changed = 0, c_far = 0, c_near = 0;
        for (u64 i = 0; i < c_texels; ++i) {
            const u8* p_texel = p_end + i * layout.cb_texel;
            if (is_comparable &&
                std::memcmp(p_texel, p_start + i * layout.cb_texel, layout.cb_texel) != 0) {
                ++c_changed;
            }
            const float depth = DecodeTexel(layout, p_texel)[0];
            c_far += depth == 1.0f ? 1 : 0;
            c_near += depth == 0.0f ? 1 : 0;
        }
        text += is_comparable ? std::format(" depth changed {}", c_changed) : std::string{};
        text += std::format(" | depth at 1.0 {} at 0.0 {}", c_far, c_near);
    }
    if (end.has_stencil) {
        std::array<u64, 256> c_now{}, c_written{};
        u64 c_changed = 0;
        for (u64 i = 0; i < c_texels; ++i) {
            const u8 value = p_end[end.off_stencil + i];
            ++c_now[value];
            if (is_comparable && value != p_start[start.off_stencil + i]) {
                ++c_changed;
                ++c_written[value];
            }
        }
        text += std::format("\n    stencil now{}", RankValues(c_now, c_texels, 8));
        if (is_comparable) {
            text += std::format("\n    stencil changed {}, to{}", c_changed,
                                RankValues(c_written, c_changed, 4));
        }
    }
    return text + "\n";
}

/// `is_wait_allowed`: the GPU may be waited for here (no binding recorded yet that a new
/// command buffer would lose).
void SummarizePassEnd(Entry& entry, bool is_wait_allowed) {
    if (!SummarizeLevel() || entry.is_compute || entry.is_copy || entry.is_summarized) {
        return;
    }
    entry.is_summarized = true;
    CopyPassGeometry(entry);
    for (size_t i = 0; i < entry.color_refs.size(); ++i) {
        if (entry.color_refs[i].id) {
            ReadBackRef(entry.color_refs[i], std::format("end color{}", i), entry.readbacks);
        }
    }
    if (!entry.depth_ref.id) {
        return;
    }
    ReadBackRef(entry.depth_ref, "end depth", entry.readbacks);
    if (!s_p_full_end) {
        return;
    }
    if (!(entry.depth_use & (DepthWrite | StencilTest))) {
        entry.full_text = "  whole image: the pass neither writes depth nor tests stencil\n";
        return;
    }
    if (!is_wait_allowed) {
        entry.full_text = "  whole image: not read (the pass ended where no wait is allowed)\n";
        return;
    }
    const FullPlanes end = CopyFull(entry.depth_ref, *s_p_full_end);
    if (!end.is_valid) {
        entry.full_text = std::format("  whole image: not read ({})\n", end.skipped);
        return;
    }
    // The pass's start copy shares its buffer with the next pass's, so the comparison cannot
    // wait for the frame's end.
    WaitForCopies();
    ++s_c_full_waits;
    entry.full_text = CompareFull(entry.full_start, end);
}

void SummarizeOpenPass(bool is_wait_allowed = true) {
    if (!s_entries.empty()) {
        SummarizePassEnd(s_entries.back(), is_wait_allowed);
    }
}

/// The render targets a pass's first draw samples, as that draw will read them.
void SummarizePassStart(Entry& entry) {
    std::vector<VideoCore::ImageId> done;
    for (const auto& sample : s_pending_sampled) {
        if (sample.is_storage || !sample.is_render_target ||
            std::ranges::find(done, sample.ref.id) != done.end()) {
            continue;
        }
        done.push_back(sample.ref.id);
        ReadBackRef({.id = sample.ref.id, .uid = sample.ref.uid}, "start sampled",
                    entry.readbacks);
    }
}

void AddSampled(Entry& entry) {
    for (auto& s : s_pending_sampled) {
        const bool is_known = std::ranges::any_of(entry.sampled, [&](const Sample& known) {
            return known.target == s.target && known.is_storage == s.is_storage &&
                   known.ref.uid == s.ref.uid && known.view == s.view;
        });
        if (entry.sampled.size() < C_SAMPLED_MAX && !is_known) {
            entry.sampled.push_back(std::move(s));
        }
    }
    s_pending_sampled.clear();
}

void AddShader(Entry& entry, u64 hash) {
    if (entry.shaders.size() < 64 && std::ranges::find(entry.shaders, hash) == entry.shaders.end()) {
        entry.shaders.push_back(hash);
    }
}

const char* CompareName(AmdGpu::CompareFunc func) {
    static constexpr std::array<const char*, 8> s_names{
        "Never", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always"};
    return s_names[u32(func) & 7];
}

const char* StencilOpName(AmdGpu::StencilFunc func) {
    static constexpr std::array<const char*, 16> s_names{
        "Keep",    "Zero",    "Ones", "ReplaceTest", "ReplaceOp", "AddClamp", "SubClamp", "Invert",
        "AddWrap", "SubWrap", "And",  "Or",          "Xor",       "Nand",     "Nor",      "Xnor"};
    return s_names[u32(func) & 15];
}

std::string DescribeStencilOps(const StencilOps& ops) {
    return std::format("{} fail {} pass {} zfail {}", vk::to_string(ops.compare_op),
                       vk::to_string(ops.fail_op), vk::to_string(ops.pass_op),
                       vk::to_string(ops.depth_fail_op));
}

std::string DescribeDrawState(const AmdGpu::Regs& regs, const DynamicState& dynamic,
                              const GraphicsPipeline& pipeline) {
    const auto& dc = regs.depth_control;
    std::string text = std::format(
        "      depth   vk test {} write {} {} | guest enable {} write {} func {} valid {}\n",
        u32(dynamic.depth_test_enabled), u32(dynamic.depth_write_enabled),
        vk::to_string(dynamic.depth_compare_op), u32(dc.depth_enable), u32(dc.depth_write_enable),
        CompareName(dc.depth_func), u32(regs.depth_buffer.DepthValid()));
    if (dynamic.stencil_test_enabled) {
        text += std::format("      stencil vk front {} ref {:#x} compare mask {:#x} "
                            "write mask {:#x}",
                            DescribeStencilOps(dynamic.stencil_front_ops),
                            dynamic.stencil_front_reference, dynamic.stencil_front_compare_mask,
                            dynamic.stencil_front_write_mask);
        if (dynamic.stencil_back_ops == dynamic.stencil_front_ops &&
            dynamic.stencil_back_reference == dynamic.stencil_front_reference &&
            dynamic.stencil_back_compare_mask == dynamic.stencil_front_compare_mask &&
            dynamic.stencil_back_write_mask == dynamic.stencil_front_write_mask) {
            text += ", back same\n";
        } else {
            text += std::format(", back {} ref {:#x} compare mask {:#x} write mask {:#x}\n",
                                DescribeStencilOps(dynamic.stencil_back_ops),
                                dynamic.stencil_back_reference, dynamic.stencil_back_compare_mask,
                                dynamic.stencil_back_write_mask);
        }
    } else {
        text += "      stencil vk off\n";
    }
    const auto& sc = regs.stencil_control;
    const auto& front = regs.stencil_ref_front;
    const auto& back = regs.stencil_ref_back;
    text += std::format(
        "      stencil guest enable {} valid {} clear {} func {} test {:#x} mask {:#x} write {:#x} "
        "op {:#x} ops fail {} zpass {} zfail {}; backface {} func {} test {:#x} mask {:#x} write "
        "{:#x} op {:#x} ops fail {} zpass {} zfail {}\n",
        u32(dc.stencil_enable), u32(regs.depth_buffer.StencilValid()),
        u32(regs.depth_render_control.stencil_clear_enable), CompareName(dc.stencil_ref_func),
        front.stencil_test_val, front.stencil_mask, front.stencil_write_mask, front.stencil_op_val,
        StencilOpName(sc.stencil_fail_front), StencilOpName(sc.stencil_zpass_front),
        StencilOpName(sc.stencil_zfail_front), u32(dc.backface_enable),
        CompareName(dc.stencil_bf_func), back.stencil_test_val, back.stencil_mask,
        back.stencil_write_mask, back.stencil_op_val, StencilOpName(sc.stencil_fail_back),
        StencilOpName(sc.stencil_zpass_back), StencilOpName(sc.stencil_zfail_back));
    const auto& key = pipeline.GetGraphicsKey();
    const auto& attachments = pipeline.BlendAttachments();
    for (u32 i = 0; i < key.num_color_attachments && i < attachments.size(); ++i) {
        const auto& a = attachments[i];
        const auto& bc = regs.blend_control[i];
        text += std::format(
            "      cb{} vk blend {} color {} {} {} alpha {} {} {} mask {} dynamic mask {} | guest "
            "enable {} color src {} dst {} func {} alpha src {} dst {} func {} separate {}\n",
            i, u32(a.blendEnable), vk::to_string(a.srcColorBlendFactor),
            vk::to_string(a.colorBlendOp), vk::to_string(a.dstColorBlendFactor),
            vk::to_string(a.srcAlphaBlendFactor), vk::to_string(a.alphaBlendOp),
            vk::to_string(a.dstAlphaBlendFactor), vk::to_string(key.write_masks[i]),
            vk::to_string(dynamic.color_write_masks[i]), u32(bc.enable), u32(bc.color_src_factor),
            u32(bc.color_dst_factor), u32(bc.color_func), u32(bc.alpha_src_factor),
            u32(bc.alpha_dst_factor), u32(bc.alpha_func), u32(bc.separate_alpha_blend));
    }
    return text;
}

std::string SummarizeReadback(const Readback& r, const u8* p_base) {
    std::string text = std::format("    {} {} | {}\n", r.label, Describe(r.target), r.identity);
    if (!r.skipped.empty()) {
        text += std::format("      not read: {}\n", r.skipped);
        return text;
    }
    const u32 c_texels = r.c_columns * r.c_rows;
    const u8* p_data = p_base + r.off_data;
    const u64 hash = XXH3_64bits(p_data, u64(c_texels) * r.cb_stride);
    if (r.layout.decode == Decode::Stencil8) {
        std::array<u32, 256> c_values{};
        for (u32 i = 0; i < c_texels; ++i) {
            ++c_values[p_data[i * r.cb_stride]];
        }
        std::vector<std::pair<u32, u32>> ranked;
        for (u32 value = 0; value < 256; ++value) {
            if (c_values[value]) {
                ranked.emplace_back(c_values[value], value);
            }
        }
        std::ranges::sort(ranked, std::greater{});
        text += std::format("      {}x{} hash {:016x} values", r.c_columns, r.c_rows, hash);
        for (size_t i = 0; i < std::min<size_t>(ranked.size(), 8); ++i) {
            text += std::format(" {:#04x}:{} ({:.1f}%)", ranked[i].second, ranked[i].first,
                                100.0 * ranked[i].first / c_texels);
        }
        if (ranked.size() > 8) {
            text += std::format(" +{} more", ranked.size() - 8);
        }
        text += "\n";
    } else {
        std::array<float, 4> min_values, max_values;
        min_values.fill(INFINITY);
        max_values.fill(-INFINITY);
        std::array<double, 4> sums{};
        u32 c_nonzero = 0, c_nonfinite = 0, c_zero = 0, c_one = 0;
        for (u32 i = 0; i < c_texels; ++i) {
            const auto values = DecodeTexel(r.layout, p_data + u64(i) * r.cb_stride);
            bool is_nonzero = false;
            for (u32 c = 0; c < r.layout.c_channels; ++c) {
                if (!std::isfinite(values[c])) {
                    ++c_nonfinite;
                    continue;
                }
                min_values[c] = std::min(min_values[c], values[c]);
                max_values[c] = std::max(max_values[c], values[c]);
                sums[c] += values[c];
                is_nonzero |= values[c] != 0.0f;
            }
            c_nonzero += is_nonzero ? 1 : 0;
            c_zero += values[0] == 0.0f ? 1 : 0;
            c_one += values[0] == 1.0f ? 1 : 0;
        }
        text += std::format("      {}x{} hash {:016x} nonzero {} nonfinite {}", r.c_columns,
                            r.c_rows, hash, c_nonzero, c_nonfinite);
        const bool is_depth = r.layout.decode == Decode::Depth16 ||
                              r.layout.decode == Decode::Depth24 ||
                              r.layout.decode == Decode::Depth32;
        if (is_depth) {
            text += std::format(" at 0.0 {} at 1.0 {}", c_zero, c_one);
        }
        static constexpr std::array<char, 4> s_channel_names{'r', 'g', 'b', 'a'};
        for (u32 c = 0; c < r.layout.c_channels; ++c) {
            text += std::format(" | {} min {:.6g} mean {:.6g} max {:.6g}",
                                is_depth ? 'z' : s_channel_names[c], min_values[c],
                                sums[c] / c_texels, max_values[c]);
        }
        text += "\n";
    }
    if (SummarizeLevel() >= 2) {
        const u32 c_cell_columns = std::min(r.c_columns, SUMMARY_CELLS);
        const u32 c_cell_rows = std::min(r.c_rows, SUMMARY_CELLS);
        text += std::format("      grid {}x{} of the first channel (cell mean):\n", c_cell_columns,
                            c_cell_rows);
        for (u32 cell_row = 0; cell_row < c_cell_rows; ++cell_row) {
            text += "       ";
            for (u32 cell_column = 0; cell_column < c_cell_columns; ++cell_column) {
                double sum = 0.0;
                u32 c_summed = 0;
                for (u32 row = cell_row * r.c_rows / c_cell_rows;
                     row < (cell_row + 1) * r.c_rows / c_cell_rows; ++row) {
                    for (u32 column = cell_column * r.c_columns / c_cell_columns;
                         column < (cell_column + 1) * r.c_columns / c_cell_columns; ++column) {
                        const float value = DecodeTexel(
                            r.layout, p_data + u64(row * r.c_columns + column) * r.cb_stride)[0];
                        if (std::isfinite(value)) {
                            sum += value;
                            ++c_summed;
                        }
                    }
                }
                text += std::format(" {:>9.4g}", c_summed ? sum / c_summed : NAN);
            }
            text += "\n";
        }
    }
    return text;
}

/// Waits once for the recorded frame's readbacks; the recorded frame alone stalls.
const u8* FinishReadbacks() {
    if (!s_p_runtime || (s_off_readback == 0 && s_c_queries == 0 && s_off_gpu_copies == 0)) {
        return nullptr;
    }
    WaitForCopies();
    if (s_p_gpu_copies && s_off_gpu_copies) {
        s_p_gpu_copies->Invalidate(0, s_off_gpu_copies);
    }
    if (!s_p_readback || s_off_readback == 0) {
        return nullptr;
    }
    s_p_readback->Invalidate(0, s_off_readback);
    return s_p_readback->mapped_data.data();
}

u64 CurrentTick() {
    return s_p_runtime ? s_p_runtime->GetScheduler().CurrentTick() : 0;
}

std::unique_ptr<VideoCore::Buffer> CreateReadbackBuffer(u64 cb_buffer, const char* sz_name) {
    auto p_buffer = std::make_unique<VideoCore::Buffer>(s_p_runtime->GetInstance(), 0, cb_buffer,
                                                        VideoCore::MemoryType::HostCached,
                                                        sz_name, false);
    if (!p_buffer->IsAllocated() || p_buffer->mapped_data.size() < cb_buffer) {
        std::printf("Frame capture: no host-visible %s of %llu MiB, its summaries are off\n",
                    sz_name, static_cast<unsigned long long>(cb_buffer >> 20));
        return nullptr;
    }
    return p_buffer;
}

vk::UniqueQueryPool CreateQueryPool(vk::QueryType type, vk::QueryPipelineStatisticFlags flags) {
    auto h_pool = Check(s_p_runtime->GetInstance().GetDevice().createQueryPoolUnique({
        .queryType = type,
        .queryCount = C_QUERIES_MAX,
        .pipelineStatistics = flags,
    }));
    if (!h_pool) {
        std::printf("Frame capture: no %s query pool, per-draw %s counts are off\n",
                    vk::to_string(type).c_str(), vk::to_string(type).c_str());
    }
    return h_pool;
}

constexpr vk::QueryPipelineStatisticFlags STATISTICS =
    vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices |
    vk::QueryPipelineStatisticFlagBits::eInputAssemblyPrimitives |
    vk::QueryPipelineStatisticFlagBits::eVertexShaderInvocations |
    vk::QueryPipelineStatisticFlagBits::eClippingInvocations |
    vk::QueryPipelineStatisticFlagBits::eClippingPrimitives |
    vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations;

void StartRecording() {
    s_off_readback = 0;
    s_c_readbacks = 0;
    s_c_skipped = 0;
    s_c_full_waits = 0;
    s_c_queries = 0;
    s_i_pending_query = NO_QUERY;
    s_is_queries_reset = false;
    s_off_gpu_copies = 0;
    s_c_gpu_copies = 0;
    s_c_gpu_copies_skipped = 0;
    s_pending_bindings.clear();
    if (!SummarizeLevel() || !s_p_runtime) {
        return;
    }
    if (!s_p_readback) {
        s_p_readback = CreateReadbackBuffer(CB_READBACK, "frame capture readback");
    }
    if (!s_p_gpu_copies) {
        s_p_gpu_copies = CreateReadbackBuffer(CB_GPU_COPIES, "frame capture buffer copies");
    }
    if (!s_p_full_start || !s_p_full_end) {
        s_p_full_start = CreateReadbackBuffer(CB_FULL_PLANES, "frame capture pass start planes");
        s_p_full_end = CreateReadbackBuffer(CB_FULL_PLANES, "frame capture pass end planes");
        if (!s_p_full_start || !s_p_full_end) {
            s_p_full_start.reset();
            s_p_full_end.reset();
        }
    }
    if (!s_h_occlusion_pool) {
        s_h_occlusion_pool = CreateQueryPool(vk::QueryType::eOcclusion, {});
    }
    if (!s_h_statistics_pool && s_p_runtime->GetInstance().IsPipelineStatisticsQuerySupported()) {
        s_h_statistics_pool = CreateQueryPool(vk::QueryType::ePipelineStatistics, STATISTICS);
    }
    auto& scheduler = s_p_runtime->GetScheduler();
    scheduler.EndRendering();
    scheduler.Record([occlusion = *s_h_occlusion_pool,
                      statistics = *s_h_statistics_pool](vk::CommandBuffer cmdbuf) {
        if (occlusion) {
            cmdbuf.resetQueryPool(occlusion, 0, C_QUERIES_MAX);
        }
        if (statistics) {
            cmdbuf.resetQueryPool(statistics, 0, C_QUERIES_MAX);
        }
    });
    s_is_queries_reset = true;
}

/// Per draw: samples passed, then the pipeline statistics; empty when there was no query.
std::string DescribeQueries(const DrawDetail& detail, std::span<const u64> occlusion_results,
                            std::span<const u64> statistics_results) {
    if (detail.i_query == NO_QUERY) {
        return "no query";
    }
    std::string text;
    if (!detail.has_occlusion) {
        text = "samples passed n/a (the game's occlusion query is active)";
    } else if (occlusion_results.size() > 2 * detail.i_query + 1 &&
               occlusion_results[2 * detail.i_query + 1]) {
        text = std::format("samples passed {}", occlusion_results[2 * detail.i_query]);
    } else {
        text = "samples passed unavailable";
    }
    const size_t i_first = size_t(C_STATISTICS + 1) * detail.i_query;
    if (statistics_results.size() > i_first + C_STATISTICS &&
        statistics_results[i_first + C_STATISTICS]) {
        const auto* p = statistics_results.data() + i_first;
        text += std::format(" | ia vertices {} primitives {} vs {} clip in {} out {} fs {}", p[0],
                            p[1], p[2], p[3], p[4], p[5]);
    } else if (s_h_statistics_pool) {
        text += " | statistics unavailable";
    }
    return text;
}

std::vector<u64> QueryResults(const vk::UniqueQueryPool& h_pool, u32 c_values) {
    if (!h_pool || s_c_queries == 0) {
        return {};
    }
    std::vector<u64> results(size_t(s_c_queries) * (c_values + 1));
    const auto result = s_p_runtime->GetInstance().GetDevice().getQueryPoolResults(
        *h_pool, 0, s_c_queries, results.size() * sizeof(u64), results.data(),
        (c_values + 1) * sizeof(u64),
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (result != vk::Result::eSuccess && result != vk::Result::eNotReady) {
        std::printf("Frame capture: query results not read (%s)\n",
                    vk::to_string(result).c_str());
        return {};
    }
    return results;
}

/// Statistics of a copied buffer read as 32-bit floats, and its first words.
std::string DescribeWords(const CaptureDraw::GpuRange& range, const u8* p_data) {
    if (range.off_data == CaptureDraw::NOT_COPIED || !p_data) {
        return std::format(" not copied ({})\n",
                           range.skipped.empty() ? "not reached" : range.skipped);
    }
    const u8* p_bytes = p_data + range.off_data;
    const u64 c_words = range.cb_copy / 4;
    float min_value = INFINITY, max_value = -INFINITY;
    double sum = 0.0;
    u64 c_nonfinite = 0;
    for (u64 i = 0; i < c_words; ++i) {
        float value;
        std::memcpy(&value, p_bytes + i * 4, 4);
        if (!std::isfinite(value)) {
            ++c_nonfinite;
            continue;
        }
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
        sum += value;
    }
    const u64 c_finite = c_words - c_nonfinite;
    std::string text = std::format(
        " {} of {} B{} | f32 min {:.6g} mean {:.6g} max {:.6g} nonfinite {} hash {:016x}",
        range.cb_copy, range.cb_full,
        range.cb_copy < range.cb_full
            ? std::format(" ({} KiB truncated)", (range.cb_full - range.cb_copy) >> 10)
            : std::string{},
        min_value, c_finite ? sum / double(c_finite) : 0.0, max_value, c_nonfinite,
        XXH3_64bits(p_bytes, range.cb_copy));
    for (u64 i = 0; i < std::min<u64>(c_words, C_WORDS_SHOWN); ++i) {
        float value;
        std::memcpy(&value, p_bytes + i * 4, 4);
        text += std::format("{}{:12.6g}", i % 8 == 0 ? std::format("\n        [{:3}]", i) : "",
                            value);
    }
    return text + "\n";
}

/// The latest dispatch before entry `i_entry` whose written bindings overlap the range.
std::string FindProducer(size_t i_entry, VAddr va_range, u64 cb_range) {
    for (size_t j = std::min(i_entry, s_entries.size()); j-- > 0;) {
        for (const auto& binding : s_entries[j].bindings) {
            if (binding.is_written && va_range < binding.va_buffer + binding.cb_buffer &&
                binding.va_buffer < va_range + std::max<u64>(cb_range, 1)) {
                return std::format(", matches output of #{} binding {}", j, binding.i_slot);
            }
        }
    }
    return {};
}

void Write(VAddr presented) {
    const u8* p_readback = FinishReadbacks();
    const u8* p_gpu_copies =
        s_p_gpu_copies && s_off_gpu_copies ? s_p_gpu_copies->mapped_data.data() : nullptr;
    const std::vector<u64> occlusion_results = QueryResults(s_h_occlusion_pool, 1);
    const std::vector<u64> statistics_results = QueryResults(s_h_statistics_pool, C_STATISTICS);
    const char* sz_dir = std::getenv("PS_CAPTURE_DIR");
    const std::string path =
        std::format("{}/frame_{}.txt", sz_dir ? sz_dir : ".", static_cast<long long>(std::time(nullptr)));
    FILE* p_file = std::fopen(path.c_str(), "w");
    if (!p_file) {
        std::printf("Frame capture: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(p_file, "presented buffer %#llx\n", static_cast<unsigned long long>(presented));
    if (SummarizeLevel()) {
        std::fprintf(p_file,
                     "summaries: %u read back (%llu KiB), %u not read; grid %ux%u texels, "
                     "level 0 layer 0 of sampled images, the attachment's own of targets\n",
                     s_c_readbacks, static_cast<unsigned long long>(s_off_readback >> 10),
                     s_c_skipped, SUMMARY_GRID, SUMMARY_GRID);
        std::fprintf(p_file,
                     "draw queries: %u (occlusion %s, %s; pipeline statistics %s); whole-image "
                     "depth/stencil comparisons waited for the GPU %u times\n",
                     s_c_queries, s_h_occlusion_pool ? "on" : "off",
                     s_p_runtime && s_p_runtime->GetInstance().IsOcclusionQueryPrecise()
                         ? "precise"
                         : "zero or not",
                     s_h_statistics_pool ? "on" : "off (not supported)", s_c_full_waits);
        std::fprintf(p_file,
                     "gpu buffer copies: %u (%llu KiB), %u not copied; up to %llu KiB per "
                     "range\n",
                     s_c_gpu_copies, static_cast<unsigned long long>(s_off_gpu_copies >> 10),
                     s_c_gpu_copies_skipped,
                     static_cast<unsigned long long>(CB_GPU_RANGE_MAX >> 10));
    }
    const auto write_summaries = [&](const Entry& e) {
        if (!e.readbacks.empty() && p_readback) {
            std::fprintf(p_file, "  summaries:\n");
            for (const auto& r : e.readbacks) {
                std::fputs(SummarizeReadback(r, p_readback).c_str(), p_file);
            }
        }
    };
    u32 i_file = 0;
    for (size_t i_entry = 0; i_entry < s_entries.size(); ++i_entry) {
        const auto& e = s_entries[i_entry];
        if (e.is_copy) {
            std::fprintf(p_file, "\n#%u DEPTH COPY%s%s\n  from %s\n  to   %s\n", i_file++,
                         (e.depth_use & DepthTest) ? " depth" : "",
                         (e.depth_use & StencilTest) ? " stencil" : "", Describe(e.depth).c_str(),
                         Describe(e.copy_dst).c_str());
            if (e.identities.size() == 2) {
                std::fprintf(p_file, "  tick %llu\n  from %s\n  to   %s\n",
                             static_cast<unsigned long long>(e.tick), e.identities[0].c_str(),
                             e.identities[1].c_str());
            }
            write_summaries(e);
            continue;
        }
        if (e.is_compute) {
            std::fprintf(p_file, "\n#%u COMPUTE dispatches %u, cs", i_file++, e.draws);
        } else {
            std::fprintf(p_file, "\n#%u PASS draws %u (indices %llu), vs/ps", i_file++, e.draws,
                         static_cast<unsigned long long>(e.indices));
        }
        for (const u64 h : e.shaders) {
            std::fprintf(p_file, " %016llx", static_cast<unsigned long long>(h));
        }
        std::fprintf(p_file, "\n");
        for (size_t i = 0; i < e.colors.size(); ++i) {
            std::fprintf(p_file, "  color%zu %s\n", i, Describe(e.colors[i]).c_str());
        }
        if (!e.is_compute) {
            std::fprintf(p_file, "  depth  %s%s%s%s\n", Describe(e.depth).c_str(),
                         (e.depth_use & DepthTest) ? " test" : "",
                         (e.depth_use & DepthWrite) ? " write" : "",
                         (e.depth_use & StencilTest) ? " stencil" : "");
        }
        if (SummarizeLevel() && !e.is_compute) {
            std::fprintf(p_file, "  tick %llu%s%s\n", static_cast<unsigned long long>(e.tick),
                         e.route.empty() ? "" : ", attachments redirected to the ",
                         e.route.c_str());
            for (size_t i = 0; i < e.identities.size(); ++i) {
                if (!e.identities[i].empty()) {
                    std::fprintf(p_file, "  %s %s\n",
                                 i < e.colors.size() ? std::format("color{}", i).c_str() : "depth ",
                                 e.identities[i].c_str());
                }
            }
        }
        for (const auto& s : e.sampled) {
            std::fprintf(p_file, "  %s %s | %s\n", s.is_storage ? "writes " : "samples",
                         Describe(s.target).c_str(), s.identity.c_str());
        }
        if (!e.note.empty()) {
            std::fprintf(p_file, "  note: %s\n", e.note.c_str());
        }
        if (!e.bindings.empty()) {
            std::fprintf(p_file,
                         "  buffer bindings of the first dispatch (gpu copies: up to %u read "
                         "before it, %u written after it):\n",
                         C_BINDINGS_COPIED, C_BINDINGS_COPIED);
            for (const auto& binding : e.bindings) {
                std::fprintf(p_file, "    %s binding %u at %#llx size %llu%s\n",
                             binding.is_written ? "written" : "read   ", binding.i_slot,
                             static_cast<unsigned long long>(binding.va_buffer),
                             static_cast<unsigned long long>(binding.cb_buffer),
                             FindProducer(i_entry, binding.va_buffer, binding.cb_buffer).c_str());
                const auto& range = binding.is_written ? binding.end : binding.start;
                std::fprintf(p_file, "      gpu %s:%s", binding.is_written ? "after" : "before",
                             DescribeWords(range, p_gpu_copies).c_str());
            }
        }
        if (!e.states.empty()) {
            std::fprintf(p_file, "  draw state:\n");
            for (const auto& run : e.states) {
                std::fprintf(p_file, "    draws %u-%u\n%s", run.i_first, run.i_last,
                             run.text.c_str());
            }
            if (e.c_states_omitted) {
                std::fprintf(p_file, "    %u more draws with other states not listed\n",
                             e.c_states_omitted);
            }
        }
        if (!e.draw_details.empty()) {
            std::fprintf(p_file, "  draws (geometry of the first %u, queries of all):\n",
                         C_DRAWS_DETAILED);
            u64 c_samples = 0;
            u32 c_measured = 0;
            for (const auto& detail : e.draw_details) {
                const std::string queries =
                    DescribeQueries(detail, occlusion_results, statistics_results);
                if (detail.has_occlusion && occlusion_results.size() > 2 * detail.i_query + 1 &&
                    occlusion_results[2 * detail.i_query + 1]) {
                    c_samples += occlusion_results[2 * detail.i_query];
                    ++c_measured;
                }
                if (!detail.geometry.empty()) {
                    std::fprintf(p_file, "    draw %u: %s\n%s", detail.i_draw, queries.c_str(),
                                 detail.geometry.c_str());
                }
                if (detail.has_gpu) {
                    std::fputs(CaptureDraw::DescribeGpuGeometry(detail.gpu, p_gpu_copies).c_str(),
                               p_file);
                    std::string producers;
                    if (detail.gpu.cb_index) {
                        const auto found = FindProducer(i_entry, detail.gpu.va_indices,
                                                        detail.gpu.indices.cb_full);
                        producers += found.empty() ? "" : " indices" + found.substr(1);
                    }
                    for (const auto& stream : detail.gpu.attributes) {
                        const auto found = FindProducer(
                            i_entry, stream.va_base, u64(stream.c_records) * stream.cb_stride);
                        producers += found.empty() ? ""
                                                   : std::format(" semantic {}{}", stream.semantic,
                                                                 found.substr(1));
                    }
                    if (!producers.empty()) {
                        std::fprintf(p_file, "      inputs written by dispatches:%s\n",
                                     producers.c_str());
                    }
                }
            }
            std::fprintf(p_file, "  samples passed: %llu over %u measured draws of %zu\n",
                         static_cast<unsigned long long>(c_samples), c_measured,
                         e.draw_details.size());
        }
        write_summaries(e);
        if (!e.full_text.empty()) {
            std::fputs(e.full_text.c_str(), p_file);
        }
        for (const auto& b : e.buffers) {
            std::fprintf(p_file, "%s\n", b.c_str());
        }
    }
    std::fclose(p_file);
    std::printf("Frame capture: %zu passes written to %s\n", s_entries.size(), path.c_str());
    if (SummarizeLevel()) {
        std::printf("Frame capture: %u summaries read back, %u not read, %u draw queries, %u "
                    "whole-image waits, %u gpu buffer copies (%u not copied)\n",
                    s_c_readbacks, s_c_skipped, s_c_queries, s_c_full_waits, s_c_gpu_copies,
                    s_c_gpu_copies_skipped);
    }
}

} // namespace

void FrameCapture::Attach(VideoCore::TextureCache& texture_cache, Runtime& runtime) {
    s_p_texture_cache = &texture_cache;
    s_p_runtime = &runtime;
}

void FrameCapture::OnFlip(VAddr va_presented) {
    m_last_presented.store(va_presented, std::memory_order_relaxed);
    m_c_flips.fetch_add(1, std::memory_order_release);
    static const char* s_sz_trigger = std::getenv("PS_CAPTURE_TRIGGER");
    if (s_sz_trigger && m_state.load(std::memory_order_relaxed) == Idle &&
        std::filesystem::exists(s_sz_trigger)) {
        std::error_code ec;
        std::filesystem::remove(s_sz_trigger, ec);
        m_state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed\n");
    }
}

void FrameCapture::Poll() {}

bool FrameCapture::IsDisplayBuffer(VAddr va_buffer) {
    const u32 c_displays = s_c_displays.load(std::memory_order_acquire);
    for (u32 i = 0; i < c_displays; ++i) {
        if (s_display_list[i].load(std::memory_order_relaxed) == va_buffer) {
            return true;
        }
    }
    if (c_displays < s_display_list.size()) {
        return false;
    }
    std::scoped_lock lk{s_display_mutex};
    return std::ranges::find(s_display_buffers, va_buffer) != s_display_buffers.end();
}

void FrameCapture::AddDisplayBuffer(VAddr va_buffer) {
    std::scoped_lock lk{s_display_mutex};
    if (std::ranges::find(s_display_buffers, va_buffer) == s_display_buffers.end()) {
        s_display_buffers.push_back(va_buffer);
        const u32 c_displays = s_c_displays.load(std::memory_order_relaxed);
        if (c_displays < s_display_list.size()) {
            s_display_list[c_displays].store(va_buffer, std::memory_order_relaxed);
            s_c_displays.store(c_displays + 1, std::memory_order_release);
        }
    }
}

void FrameCapture::BeginPass(std::span<const PassTarget> colors, PassTarget depth,
                             const char* sz_route) {
    if (!s_p_texture_cache) {
        return;
    }
    const auto resolve = [](const PassTarget& t) -> VideoCore::Image* {
        return t.id ? s_p_texture_cache->TryGetImage(t.id, 0) : nullptr;
    };
    std::vector<Target> targets;
    std::vector<ImageRef> refs;
    bool is_display = false;
    {
        std::scoped_lock lk{s_display_mutex};
        for (const auto& color : colors) {
            const VideoCore::Image* p_image = resolve(color);
            targets.push_back(p_image ? ToTarget(p_image->info) : Target{});
            refs.push_back(p_image ? ImageRef{color.id, p_image->image_uid, color.i_level,
                                              color.i_layer}
                                   : ImageRef{});
            is_display |= p_image && std::ranges::find(s_display_buffers,
                                                       p_image->info.guest_address) !=
                                         s_display_buffers.end();
        }
    }
    // Frame boundaries in the command stream: the pass that writes a display buffer.
    const bool is_new_pass = !(s_is_pass_open && !s_entries.empty() && !s_entries.back().is_compute &&
                            s_entries.back().colors == targets);
    if (is_display && is_new_pass) {
        if (m_state.load(std::memory_order_relaxed) == Recording) {
            SummarizeOpenPass();
            Write(m_last_presented.load(std::memory_order_relaxed));
            s_entries.clear();
            s_pending_sampled.clear();
            s_pending_buffers.clear();
            m_state.store(Idle, std::memory_order_release);
            return;
        }
        s_entries.clear();
        s_pending_sampled.clear();
        s_pending_buffers.clear();
        s_is_pass_open = false;
        StartRecording();
        m_state.store(Recording, std::memory_order_release);
    }
    if (m_state.load(std::memory_order_relaxed) != Recording) {
        s_pending_sampled.clear();
        s_pending_buffers.clear();
        return;
    }
    const VideoCore::Image* p_depth = resolve(depth);
    const Target d = p_depth ? ToTarget(p_depth->info) : Target{};
    if (s_is_pass_open && !s_entries.empty() && !s_entries.back().is_compute &&
        s_entries.back().colors == targets && s_entries.back().depth == d) {
        return;
    }
    SummarizeOpenPass();
    Entry entry{.is_compute = false, .colors = std::move(targets), .depth = d};
    if (SummarizeLevel()) {
        entry.tick = CurrentTick();
        entry.route = sz_route ? sz_route : "";
        for (size_t i = 0; i < colors.size(); ++i) {
            const VideoCore::Image* p_image = resolve(colors[i]);
            entry.identities.push_back(p_image ? Identify(*p_image, colors[i].id) : std::string{});
        }
        if (p_depth) {
            entry.identities.push_back(Identify(*p_depth, depth.id));
            entry.depth_ref = {depth.id, p_depth->image_uid, depth.i_level, depth.i_layer};
        }
        entry.color_refs = std::move(refs);
        SummarizePassStart(entry);
        if (entry.depth_ref.id && s_p_full_start) {
            entry.full_start = CopyFull(entry.depth_ref, *s_p_full_start);
        }
    }
    s_entries.push_back(std::move(entry));
    s_is_pass_open = true;
}

void FrameCapture::Draw(u64 vs_hash, u64 ps_hash, u32 c_indices, u32 c_instances) {
    s_pending_bindings.clear();
    if (m_state.load(std::memory_order_relaxed) != Recording || s_entries.empty() ||
        s_entries.back().is_compute || s_entries.back().is_copy) {
        return;
    }
    auto& e = s_entries.back();
    ++e.draws;
    e.indices += u64(c_indices) * std::max(c_instances, 1u);
    AddShader(e, vs_hash);
    AddShader(e, ps_hash);
    AddSampled(e);
    AddBuffers(e);
}

void FrameCapture::DepthUse(bool is_test, bool is_write, bool is_stencil) {
    if (m_state.load(std::memory_order_relaxed) != Recording || s_entries.empty() ||
        s_entries.back().is_compute || s_entries.back().is_copy) {
        return;
    }
    s_entries.back().depth_use |=
        (is_test ? DepthTest : 0u) | (is_write ? DepthWrite : 0u) | (is_stencil ? StencilTest : 0u);
}

FrameCapture::DrawQueries FrameCapture::TakeDrawQueries(bool is_occlusion_free) {
    s_i_pending_query = NO_QUERY;
    if (!SummarizeLevel() || m_state.load(std::memory_order_relaxed) != Recording ||
        !s_is_queries_reset || s_c_queries >= C_QUERIES_MAX ||
        (!s_h_occlusion_pool && !s_h_statistics_pool)) {
        return {};
    }
    s_i_pending_query = s_c_queries++;
    s_is_pending_occlusion = is_occlusion_free && s_h_occlusion_pool;
    return {
        .occlusion = s_is_pending_occlusion ? *s_h_occlusion_pool : vk::QueryPool{},
        .statistics = *s_h_statistics_pool,
        .i_query = s_i_pending_query,
        .is_precise = s_p_runtime->GetInstance().IsOcclusionQueryPrecise(),
    };
}

void FrameCapture::DrawState(const AmdGpu::Regs& regs, const DynamicState& dynamic,
                             const GraphicsPipeline& pipeline, const DrawInputs& inputs) {
    const u32 i_query = std::exchange(s_i_pending_query, NO_QUERY);
    if (!SummarizeLevel() || m_state.load(std::memory_order_relaxed) != Recording ||
        s_entries.empty() || s_entries.back().is_compute || s_entries.back().is_copy ||
        s_entries.back().draws == 0) {
        return;
    }
    auto& e = s_entries.back();
    const u32 i_draw = e.draws - 1;
    e.draw_details.push_back({
        .i_draw = i_draw,
        .i_query = i_query,
        .has_occlusion = i_query != NO_QUERY && s_is_pending_occlusion,
        .geometry = i_draw < C_DRAWS_DETAILED
                        ? CaptureDraw::DescribeGeometry(regs, pipeline, inputs,
                                                        i_draw < C_DRAWS_VERTEX_DUMPED,
                                                        i_draw == 0)
                        : std::string{},
        .has_gpu = i_draw < C_DRAWS_VERTEX_DUMPED,
    });
    if (e.draw_details.back().has_gpu) {
        e.draw_details.back().gpu =
            CaptureDraw::GpuSources(regs, pipeline, inputs, CB_GPU_RANGE_MAX);
    }
    std::string text = DescribeDrawState(regs, dynamic, pipeline) +
                       CaptureDraw::DescribeRasterState(regs, dynamic, pipeline);
    if (!e.states.empty() && e.states.back().text == text && e.states.back().i_last + 1 == i_draw) {
        e.states.back().i_last = i_draw;
    } else if (e.states.size() < C_STATE_RUNS_MAX) {
        e.states.push_back({i_draw, i_draw, std::move(text)});
    } else {
        ++e.c_states_omitted;
    }
}

void FrameCapture::EndPass() {
    if (m_state.load(std::memory_order_relaxed) == Recording) {
        SummarizeOpenPass();
    }
}

void FrameCapture::DepthCopy(const VideoCore::Image& src, VideoCore::ImageId src_id,
                             const VideoCore::Image& dst, VideoCore::ImageId dst_id, bool is_depth,
                             bool is_stencil) {
    if (m_state.load(std::memory_order_relaxed) != Recording) {
        return;
    }
    SummarizeOpenPass();
    Entry entry{.is_copy = true,
                .depth = ToTarget(src.info),
                .depth_use = (is_depth ? DepthTest : 0u) | (is_stencil ? StencilTest : 0u),
                .copy_dst = ToTarget(dst.info)};
    if (SummarizeLevel()) {
        entry.tick = CurrentTick();
        entry.identities = {Identify(src, src_id), Identify(dst, dst_id)};
        ReadBackRef({.id = dst_id, .uid = dst.image_uid}, "after copy dst", entry.readbacks);
    }
    s_entries.push_back(std::move(entry));
    s_is_pass_open = false;
}

void FrameCapture::Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (m_state.load(std::memory_order_relaxed) != Recording) {
        s_pending_sampled.clear();
        s_pending_buffers.clear();
        s_pending_bindings.clear();
        return;
    }
    // The rasterizer ended the pass (EndPass) before binding the dispatch; a wait here would
    // drop the bindings with the command buffer.
    SummarizeOpenPass(false);
    if (!s_entries.empty() && s_entries.back().is_compute && !s_entries.back().shaders.empty() &&
        s_entries.back().shaders[0] == cs_hash) {
        ++s_entries.back().draws;
    } else {
        s_entries.push_back({.is_compute = true, .draws = 1, .shaders = {cs_hash}});
        s_entries.back().note = std::format("groups {}x{}x{}", x, y, z);
        if (SummarizeLevel()) {
            auto& e = s_entries.back();
            e.bindings = std::move(s_pending_bindings);
            std::vector<CaptureDraw::GpuRange*> ranges;
            for (auto& binding : e.bindings) {
                if (binding.is_written) {
                    continue;
                }
                if (ranges.size() < C_BINDINGS_COPIED) {
                    ranges.push_back(&binding.start);
                } else if (binding.start.skipped.empty()) {
                    binding.start.skipped = "over the bindings copied per dispatch";
                }
            }
            RecordBufferCopies(ranges);
        }
    }
    s_pending_bindings.clear();
    AddSampled(s_entries.back());
    AddBuffers(s_entries.back());
    s_is_pass_open = false;
}

void FrameCapture::AfterDispatch() {
    if (!SummarizeLevel() || m_state.load(std::memory_order_relaxed) != Recording ||
        s_entries.empty() || !s_entries.back().is_compute || s_entries.back().is_dispatch_copied) {
        return;
    }
    auto& e = s_entries.back();
    e.is_dispatch_copied = true;
    std::vector<CaptureDraw::GpuRange*> ranges;
    for (auto& binding : e.bindings) {
        if (!binding.is_written) {
            continue;
        }
        if (ranges.size() < C_BINDINGS_COPIED) {
            ranges.push_back(&binding.end);
        } else if (binding.end.skipped.empty()) {
            binding.end.skipped = "over the bindings copied per dispatch";
        }
    }
    RecordBufferCopies(ranges);
    u32 c_images = 0;
    for (const auto& sample : e.sampled) {
        if (sample.is_storage && c_images < C_STORAGE_IMAGES_READ) {
            ++c_images;
            ReadBackRef({.id = sample.ref.id, .uid = sample.ref.uid}, "after dispatch storage",
                        e.readbacks);
        }
    }
}

void FrameCapture::BoundBuffer(u32 i_slot, VAddr va_buffer, u64 cb_buffer, bool is_written,
                               vk::Buffer h_buffer, u64 off_buffer) {
    if (!SummarizeLevel() || m_state.load(std::memory_order_relaxed) != Recording ||
        s_pending_bindings.size() >= C_BINDINGS_KEPT) {
        return;
    }
    CaptureDraw::GpuRange range{.h_buffer = h_buffer,
                                .off_buffer = off_buffer,
                                .cb_copy = std::min(cb_buffer, CB_GPU_RANGE_MAX) & ~u64(3),
                                .cb_full = cb_buffer};
    if (!h_buffer) {
        range.skipped = "reached through the page table (PS_LAYER_MEMORY), not copied";
    } else if (!range.cb_copy) {
        range.skipped = "empty binding";
    }
    s_pending_bindings.push_back({.i_slot = i_slot,
                                  .va_buffer = va_buffer,
                                  .cb_buffer = cb_buffer,
                                  .is_written = is_written,
                                  .start = range,
                                  .end = range});
}

void FrameCapture::Buffer(u64 stage_hash, u32 slot, VAddr va_buffer, const void* p_data, u64 cb_data) {
    const u64 cb_dumped = std::min<u64>(cb_data, 1024) & ~u64(3);
    std::string out = std::format("  buffer stage {:016x} slot {} at {:#x} size {}:", stage_hash,
                                  slot, va_buffer, cb_data);
    const auto* p_words = static_cast<const float*>(p_data);
    for (u64 i = 0; i < cb_dumped / 4; ++i) {
        if (i % 8 == 0) {
            out += std::format("\n    [{:3}]", i);
        }
        out += std::format(" {:12.6g}", p_words[i]);
    }
    s_pending_buffers.push_back(std::move(out));
}

void FrameCapture::Sampled(const VideoCore::Image& image, VideoCore::ImageId id, VAddr va_requested,
                           vk::ImageView view, bool is_storage, const char* sz_route) {
    if (s_pending_sampled.size() >= 64) {
        return;
    }
    std::string identity = std::format("{} view {:#x}", Identify(image, id), HandleBits(view));
    if (va_requested != image.info.guest_address) {
        identity += std::format(" requested {:#x}", va_requested);
    }
    if (sz_route) {
        identity += std::format(" via {}", sz_route);
    }
    s_pending_sampled.push_back({.target = ToTarget(image.info),
                                 .is_storage = is_storage,
                                 .ref = {id, image.image_uid},
                                 .view = view,
                                 .is_render_target =
                                     image.usage.render_target || image.usage.depth_target,
                                 .identity = std::move(identity)});
}

void FrameCapture::Note(const char* sz_text) {
    if (!s_entries.empty()) {
        s_entries.back().note += sz_text;
    }
}

} // namespace Vulkan
