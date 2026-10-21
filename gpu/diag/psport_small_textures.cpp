// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_small_textures.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <fmt/format.h>
#include <miniz.h>
#include <xxhash.h>
#include "psport_toggles.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_format_traits.hpp>

namespace PsSmallTextures {
namespace {

constexpr auto IndexPeriod = std::chrono::seconds(5);
constexpr std::uint64_t BindsPerIndexCheck = 4096;
constexpr std::uint32_t MaxLimit = 65536;
constexpr std::uint64_t MiB = 1024 * 1024;
/// Atlas contents read back per guest address: past it the address is a texture the CPU keeps
/// rewriting (a font cache), listed without pictures.
constexpr std::uint32_t MaxAtlasReadbacksPerAddress = 8;

constexpr const char* SzPending = "pending";
constexpr const char* SzNoPendingRoom = "no:pending_cap";

struct Entry {
    std::uint64_t hash = 0;
    std::uint64_t level0_hash = 0;
    std::uint64_t va_guest = 0;
    std::uint32_t cb_guest = 0, cb_level0 = 0;
    std::uint32_t width = 0, height = 0, depth = 0, c_levels = 0, c_layers = 0, first_frame = 0;
    vk::Format format = vk::Format::eUndefined;
    AmdGpu::TileMode tile_mode{};
    AmdGpu::ImageType type{};
    bool is_atlas = false;
    /// "png", "pending", "failed" or why there is none ("no:<reason>").
    std::atomic<const char*> sz_png{SzPending};
    std::atomic<std::uint64_t> c_uploads{0}, c_binds{0};
};

/// A readback the GPU finished, for the PNG thread: it owns the buffer from then on.
struct PngJob {
    VmaAllocator allocator{};
    VkBuffer h_buffer{};
    VmaAllocation p_allocation{};
    const void* p_pixels = nullptr;
    std::uint32_t width = 0, height = 0;
    std::uint64_t cb_buffer = 0;
    Entry* p_entry = nullptr;
};

struct State {
    std::timed_mutex mutex; ///< the entries' creation, by_hash, atlas_readbacks and the index file
    std::string dir;
    std::uint32_t max_side = 256, atlas_side = 2048, c_limit = 4096;
    std::uint64_t cb_pending_cap = 512 * MiB, cb_disk_cap = 2048 * MiB;
    std::unique_ptr<Entry[]> p_entries;
    std::atomic<std::uint32_t> c_entries{0};
    /// Entry index + 1 per hash.
    std::unordered_map<std::uint64_t, std::uint32_t> by_hash;
    std::unordered_map<std::uint64_t, std::uint32_t> atlas_readbacks;
    std::atomic<std::uint32_t> c_small{0}, c_atlas{0}, c_no_png{0};
    std::atomic<std::uint64_t> c_uploads{0}, c_binds{0}, c_over_limit{0}, c_png{0}, c_png_failed{0};
    std::atomic<std::uint64_t> cb_pending{0}, cb_disk{0};
    std::atomic<bool> is_dirty{false}, is_disk_full{false};
    std::chrono::steady_clock::time_point written_at{};

    std::mutex png_mutex;
    std::condition_variable png_ready;
    std::deque<PngJob> png_jobs;
};

/// Set once the inventory is ready; never destroyed, since the PNG thread waits on it until the
/// process ends and the exit report reads it from a signal handler.
std::atomic<State*> s_p_state{nullptr};

std::uint64_t EnvCount(const char* sz_name, std::uint64_t fallback, std::uint64_t max) {
    const char* sz_value = std::getenv(sz_name);
    if (!sz_value || !sz_value[0]) {
        return fallback;
    }
    char* p_end = nullptr;
    const unsigned long long value = std::strtoull(sz_value, &p_end, 10);
    if (*p_end) {
        std::printf("Small textures: %s=%s is no count, %llu kept\n", sz_name, sz_value,
                    (unsigned long long)fallback);
        return fallback;
    }
    if (value > max) {
        std::printf("Small textures: %s=%s above %llu, %llu used\n", sz_name, sz_value, (unsigned long long)max,
                    (unsigned long long)max);
        return max;
    }
    return value;
}

bool WriteFile(const std::string& path, const void* p_data, std::size_t cb_data) {
    FILE* p_file = std::fopen(path.c_str(), "wb");
    if (!p_file) {
        return false;
    }
    const bool is_written = std::fwrite(p_data, 1, cb_data, p_file) == cb_data;
    return std::fclose(p_file) == 0 && is_written;
}

void NoteNoPng(State& s, Entry& entry, const char* sz_reason) {
    entry.sz_png.store(sz_reason, std::memory_order_release);
    s.c_no_png.fetch_add(1, std::memory_order_relaxed);
    s.is_dirty.store(true, std::memory_order_relaxed);
}

/// The PNG thread: encodes and writes the readbacks, so that neither the recording thread nor the
/// scheduler's priority thread (which also writes image downloads back to guest memory) waits.
void WritePngs(State& s) {
    for (;;) {
        PngJob job;
        {
            std::unique_lock lock{s.png_mutex};
            s.png_ready.wait(lock, [&] { return !s.png_jobs.empty(); });
            job = s.png_jobs.front();
            s.png_jobs.pop_front();
        }
        Entry& entry = *job.p_entry;
        vmaInvalidateAllocation(job.allocator, job.p_allocation, 0, VK_WHOLE_SIZE);
        std::size_t cb_png = 0;
        void* p_png = s.is_disk_full.load(std::memory_order_relaxed)
                          ? nullptr
                          : tdefl_write_image_to_png_file_in_memory_ex(job.p_pixels, int(job.width),
                                                                       int(job.height), 4, &cb_png, MZ_BEST_SPEED,
                                                                       MZ_FALSE);
        vmaDestroyBuffer(job.allocator, job.h_buffer, job.p_allocation);
        s.cb_pending.fetch_sub(job.cb_buffer, std::memory_order_relaxed);
        if (p_png && s.cb_disk.fetch_add(cb_png, std::memory_order_relaxed) + cb_png > s.cb_disk_cap) {
            s.cb_disk.fetch_sub(cb_png, std::memory_order_relaxed);
            s.is_disk_full.store(true, std::memory_order_relaxed);
            mz_free(p_png);
            p_png = nullptr;
        }
        if (!p_png && s.is_disk_full.load(std::memory_order_relaxed)) {
            NoteNoPng(s, entry, "no:disk_cap");
            continue;
        }
        const bool is_written =
            p_png && WriteFile(fmt::format("{}/{:016x}.png", s.dir, entry.hash), p_png, cb_png);
        mz_free(p_png);
        entry.sz_png.store(is_written ? "png" : "failed", std::memory_order_release);
        (is_written ? s.c_png : s.c_png_failed).fetch_add(1, std::memory_order_relaxed);
        s.is_dirty.store(true, std::memory_order_relaxed);
    }
}

State* Setup() {
    auto* p_state = new State;
    State& s = *p_state;
    const std::string_view value = std::getenv("PS_DUMP_SMALL_TEXTURES");
    if (value == "1") {
        const char* sz_data = std::getenv("PS_DATA_DIR");
        s.dir = sz_data && sz_data[0] ? std::string{sz_data} + "/small_textures" : "small_textures";
    } else {
        s.dir = value;
    }
    s.max_side = std::uint32_t(EnvCount("PS_DUMP_SMALL_TEXTURES_MAX", s.max_side, 16384));
    s.atlas_side = std::uint32_t(EnvCount("PS_DUMP_SMALL_TEXTURES_ATLAS", s.atlas_side, 16384));
    s.c_limit = std::uint32_t(EnvCount("PS_DUMP_SMALL_TEXTURES_LIMIT", s.c_limit, MaxLimit));
    s.cb_pending_cap = EnvCount("PS_DUMP_SMALL_TEXTURES_PENDING_MB", s.cb_pending_cap / MiB, 1 << 20) * MiB;
    s.cb_disk_cap = EnvCount("PS_DUMP_SMALL_TEXTURES_DISK_MB", s.cb_disk_cap / MiB, 1 << 30) * MiB;
    std::error_code error;
    std::filesystem::create_directories(s.dir, error);
    if (error) {
        std::printf("ERROR: Small textures: no folder %s (%s): nothing dumped\n", s.dir.c_str(),
                    error.message().c_str());
        delete p_state;
        return nullptr;
    }
    s.p_entries = std::make_unique<Entry[]>(s.c_limit);
    std::thread{WritePngs, std::ref(s)}.detach();
    std::printf("Small textures: dumping to %s (max %u, atlas %u, limit %u, pending %llu MiB, disk %llu MiB)\n",
                s.dir.c_str(), s.max_side, s.atlas_side, s.c_limit, (unsigned long long)(s.cb_pending_cap / MiB),
                (unsigned long long)(s.cb_disk_cap / MiB));
    s_p_state.store(p_state, std::memory_order_release);
    return p_state;
}

/// The inventory, set up on first use; null when its folder could not be made.
State* Get() {
    static State* const s_p_ready = Setup();
    return s_p_ready;
}

bool HasAlpha(vk::Format format) {
    const std::uint8_t c_components = vk::componentCount(format);
    return c_components > 0 && std::string_view{vk::componentName(format, c_components - 1)} == "A";
}

bool IsInteger(vk::Format format) {
    const std::string_view numeric = vk::componentCount(format) ? vk::componentNumericFormat(format, 0) : "";
    return numeric == "UINT" || numeric == "SINT" || numeric == "USCALED" || numeric == "SSCALED";
}

/// Whether `image` is inventoried, and as an atlas: small on both sides in any shape, or larger
/// as UI atlases are, a flat 2D colour texture with alpha and no mips that nothing renders to.
bool Classify(const State& s, const VideoCore::Image& image, bool& is_out_atlas) {
    const auto& info = image.info;
    const std::uint32_t width = info.size.width, height = info.size.height;
    if (width <= s.max_side && height <= s.max_side) {
        is_out_atlas = false;
        return true;
    }
    is_out_atlas = true;
    return width <= s.atlas_side && height <= s.atlas_side && info.resources.levels == 1 &&
           info.resources.layers == 1 && info.type == AmdGpu::ImageType::Color2D && !info.props.is_depth &&
           !image.usage.render_target && !image.usage.depth_target && HasAlpha(info.pixel_format);
}

void WriteIndex(State& s) {
    std::string text = "hash\tguest_address\tguest_size\tlevel0_size\tlevel0_hash\twidth\theight\tdepth\tlevels\t"
                       "layers\ttype\tformat\ttiling\tkind\tfirst_frame\tuploads\tbinds\tpng\n";
    const std::uint32_t c_entries = s.c_entries.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < c_entries; ++i) {
        const Entry& entry = s.p_entries[i];
        fmt::format_to(std::back_inserter(text),
                       "{:016x}\t{:#x}\t{}\t{}\t{:016x}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                       entry.hash, entry.va_guest, entry.cb_guest, entry.cb_level0, entry.level0_hash,
                       entry.width, entry.height, entry.depth, entry.c_levels, entry.c_layers,
                       AmdGpu::NameOf(entry.type), vk::to_string(entry.format), AmdGpu::NameOf(entry.tile_mode),
                       entry.is_atlas ? "atlas" : "small", entry.first_frame,
                       entry.c_uploads.load(std::memory_order_relaxed),
                       entry.c_binds.load(std::memory_order_relaxed), entry.sz_png.load(std::memory_order_acquire));
    }
    const std::string path = s.dir + "/index.tsv";
    const std::string path_new = path + ".new";
    // Replaced by a rename, so that a run stopped while it is written keeps the previous index.
    if (!WriteFile(path_new, text.data(), text.size()) || std::rename(path_new.c_str(), path.c_str()) != 0) {
        std::printf("ERROR: Small textures: %s not written\n", path.c_str());
    }
    s.is_dirty.store(false, std::memory_order_relaxed);
    s.written_at = std::chrono::steady_clock::now();
}

void WriteIndexIfDue(State& s) {
    if (s.is_dirty.load(std::memory_order_relaxed) &&
        std::chrono::steady_clock::now() - s.written_at >= IndexPeriod) {
        WriteIndex(s);
    }
}

/// Level 0, layer 0 of `image` as RGBA8 into <hash>.png once the GPU is past it: blitted after the
/// upload in stream order, in its own encoding (sRGB stays sRGB), read back to host memory and
/// handed to the PNG thread. Returns "pending", or why there is no picture.
const char* ReadBack(State& s, const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                     Vulkan::Runtime& runtime, VideoCore::Image& image, Entry& entry) {
    const auto& info = image.info;
    if (info.props.is_depth) {
        return "no:depth";
    }
    if (info.props.is_volume || info.type == AmdGpu::ImageType::Color3D) {
        return "no:volume";
    }
    if (info.num_samples > 1) {
        return "no:msaa";
    }
    if (!image.IsAllocated()) {
        return "no:unallocated";
    }
    if (s.is_disk_full.load(std::memory_order_relaxed)) {
        return "no:disk_cap";
    }
    const vk::Format format = image.backing->image.image_ci.format;
    if (IsInteger(format)) {
        return "no:integer";
    }
    if (!instance.IsFormatSupported(format, vk::FormatFeatureFlagBits2::eBlitSrc)) {
        return "no:blit";
    }
    const std::uint32_t width = image.backing->image.image_ci.extent.width;
    const std::uint32_t height = image.backing->image.image_ci.extent.height;
    const std::uint64_t cb_buffer = std::uint64_t(width) * height * 4;
    if (s.cb_pending.load(std::memory_order_relaxed) + cb_buffer > s.cb_pending_cap) {
        return SzNoPendingRoom;
    }
    const bool is_srgb = std::string_view{vk::componentNumericFormat(format, 0)} == "SRGB";
    VideoCore::UniqueImage target{instance.GetDevice(), instance.GetAllocator()};
    if (!target.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = is_srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        return "no:memory";
    }
    Vulkan::SetObjectName(instance.GetDevice(), target.image, "psport: small texture {:016x} {}x{}", entry.hash,
                          width, height);
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = cb_buffer,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    PngJob job{.allocator = instance.GetAllocator(), .width = width, .height = height, .cb_buffer = cb_buffer,
               .p_entry = &entry};
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(job.allocator, &buffer_ci, &alloc_ci, &job.h_buffer, &job.p_allocation,
                        &allocation_info) != VK_SUCCESS) {
        return "no:memory";
    }
    job.p_pixels = allocation_info.pMappedData;
    s.cb_pending.fetch_add(cb_buffer, std::memory_order_relaxed);
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eBlit,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    scheduler.Record([source = image.GetImage(), target = target.image, buffer = vk::Buffer{job.h_buffer}, width,
                      height](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 to_blit{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = target,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_blit});
        const std::array corners{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(width), s32(height), 1}};
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = corners,
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = corners,
        };
        cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, target, vk::ImageLayout::eTransferDstOptimal,
                         blit, vk::Filter::eNearest);
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = target,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
        const vk::BufferImageCopy region{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyImageToBuffer(target, vk::ImageLayout::eTransferSrcOptimal, buffer, region);
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    });
    scheduler.DeferPriorityOperation([target = std::move(target), job, p_state = &s] {
        {
            std::scoped_lock lock{p_state->png_mutex};
            p_state->png_jobs.push_back(job);
        }
        p_state->png_ready.notify_one();
    });
    return SzPending;
}

/// Reads `entry` back unless its atlas address has been read back too often; a texture without a
/// picture says why and is counted.
void StartReadBack(State& s, const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                   Vulkan::Runtime& runtime, VideoCore::Image& image, Entry& entry) {
    if (entry.is_atlas && ++s.atlas_readbacks[entry.va_guest] > MaxAtlasReadbacksPerAddress) {
        NoteNoPng(s, entry, "no:rewritten");
        return;
    }
    const char* sz_png = ReadBack(s, instance, scheduler, runtime, image, entry);
    if (sz_png != SzPending) {
        NoteNoPng(s, entry, sz_png);
    }
}

void Append(char* sz_out, std::size_t cb_out, std::size_t& off_out, std::string_view text) {
    for (const char c : text) {
        if (off_out + 1 < cb_out) {
            sz_out[off_out++] = c;
        }
    }
}

void AppendCount(char* sz_out, std::size_t cb_out, std::size_t& off_out, std::string_view key,
                 std::uint64_t value) {
    std::array<char, 20> digits;
    std::size_t c_digits = 0;
    do {
        digits[c_digits++] = char('0' + value % 10);
        value /= 10;
    } while (value);
    Append(sz_out, cb_out, off_out, key);
    while (c_digits) {
        Append(sz_out, cb_out, off_out, std::string_view{&digits[--c_digits], 1});
    }
}

} // namespace

void NoteUploadSlow(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime,
                    VideoCore::Image& image) {
    std::atomic_ref{image.i_small_texture}.store(0, std::memory_order_relaxed);
    State* p_state = Get();
    if (!p_state) {
        return;
    }
    State& s = *p_state;
    const auto& info = image.info;
    bool is_atlas = false;
    if (info.guest_size == 0 || !Classify(s, image, is_atlas)) {
        return;
    }
    s.c_uploads.fetch_add(1, std::memory_order_relaxed);
    // Outside the lock, so that a binding thread's index check never waits for a large atlas.
    const std::uint64_t hash = XXH3_64bits(reinterpret_cast<const void*>(info.guest_address), info.guest_size);
    std::unique_lock lock{s.mutex};
    std::uint32_t i_entry_plus_one = 0;
    if (const auto it = s.by_hash.find(hash); it != s.by_hash.end()) {
        i_entry_plus_one = it->second;
        Entry& entry = s.p_entries[i_entry_plus_one - 1];
        // The readback that found no room earlier tries again.
        if (entry.sz_png.load(std::memory_order_relaxed) == SzNoPendingRoom &&
            ReadBack(s, instance, scheduler, runtime, image, entry) == SzPending) {
            entry.sz_png.store(SzPending, std::memory_order_release);
            s.c_no_png.fetch_sub(1, std::memory_order_relaxed);
        }
    } else {
        const std::uint32_t i_entry = s.c_entries.load(std::memory_order_relaxed);
        if (i_entry >= s.c_limit) {
            s.c_over_limit.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Entry& entry = s.p_entries[i_entry];
        entry.hash = hash;
        entry.va_guest = info.guest_address;
        entry.cb_guest = info.guest_size;
        // A profile hashes the payload its game's file holds, which is level 0 alone for a texture
        // without mips when the guest size is padded.
        const auto& level0 = info.mips_layout[0];
        if (level0.offset < info.guest_size) {
            entry.cb_level0 = std::min(level0.size, info.guest_size - level0.offset);
            entry.level0_hash =
                XXH3_64bits(reinterpret_cast<const void*>(info.guest_address + level0.offset), entry.cb_level0);
        }
        entry.width = info.size.width;
        entry.height = info.size.height;
        entry.depth = info.size.depth;
        entry.c_levels = info.resources.levels;
        entry.c_layers = info.resources.layers;
        entry.first_frame = PsStats::g_frame_number.load(std::memory_order_relaxed);
        entry.format = info.pixel_format;
        entry.tile_mode = info.tile_mode;
        entry.type = info.type;
        entry.is_atlas = is_atlas;
        StartReadBack(s, instance, scheduler, runtime, image, entry);
        (is_atlas ? s.c_atlas : s.c_small).fetch_add(1, std::memory_order_relaxed);
        i_entry_plus_one = i_entry + 1;
        s.by_hash.emplace(hash, i_entry_plus_one);
        s.c_entries.store(i_entry_plus_one, std::memory_order_release);
        s.is_dirty.store(true, std::memory_order_relaxed);
    }
    s.p_entries[i_entry_plus_one - 1].c_uploads.fetch_add(1, std::memory_order_relaxed);
    std::atomic_ref{image.i_small_texture}.store(i_entry_plus_one, std::memory_order_relaxed);
    WriteIndexIfDue(s);
}

void NoteBindSlow(VideoCore::Image& image) {
    const std::uint32_t i_entry_plus_one = std::atomic_ref{image.i_small_texture}.load(std::memory_order_relaxed);
    State* p_state = s_p_state.load(std::memory_order_acquire);
    if (!i_entry_plus_one || !p_state) {
        return;
    }
    State& s = *p_state;
    s.p_entries[i_entry_plus_one - 1].c_binds.fetch_add(1, std::memory_order_relaxed);
    if (s.c_binds.fetch_add(1, std::memory_order_relaxed) % BindsPerIndexCheck != 0) {
        return;
    }
    s.is_dirty.store(true, std::memory_order_relaxed);
    // A binding thread never waits for the recording thread's upload to write the index.
    if (std::unique_lock lock{s.mutex, std::try_to_lock}) {
        WriteIndexIfDue(s);
    }
}

void WriteIndexAtExit() {
    State* p_state = g_is_enabled ? s_p_state.load(std::memory_order_acquire) : nullptr;
    if (!p_state) {
        return;
    }
    if (std::unique_lock lock{p_state->mutex, std::chrono::seconds(1)}) {
        WriteIndex(*p_state);
    } else {
        std::printf("Small textures: the recording thread holds the inventory: %s/index.tsv as last written\n",
                    p_state->dir.c_str());
    }
}

} // namespace PsSmallTextures

extern "C" void psport_gpu_small_textures_report(void) {
    using namespace PsSmallTextures;
    if (!g_is_enabled) {
        return;
    }
    static std::atomic<bool> s_has_reported{false};
    if (s_has_reported.exchange(true)) {
        return;
    }
    // A fault or the watchdog may print it from a signal handler: no locks, allocations or stdio.
    char sz_line[512];
    std::size_t cb_line = 0;
    Append(sz_line, sizeof(sz_line), cb_line, "Small textures: summary:");
    if (const State* p_state = s_p_state.load(std::memory_order_acquire); !p_state) {
        Append(sz_line, sizeof(sz_line), cb_line, " nothing dumped (no upload, or no folder)");
    } else {
        const State& s = *p_state;
        const std::uint32_t c_entries = s.c_entries.load(std::memory_order_acquire);
        const std::uint64_t c_png = s.c_png.load(std::memory_order_relaxed);
        const std::uint64_t c_png_failed = s.c_png_failed.load(std::memory_order_relaxed);
        const std::uint32_t c_no_png = s.c_no_png.load(std::memory_order_relaxed);
        Append(sz_line, sizeof(sz_line), cb_line, " dir=");
        Append(sz_line, sizeof(sz_line), cb_line, s.dir);
        AppendCount(sz_line, sizeof(sz_line), cb_line, " unique=", c_entries);
        AppendCount(sz_line, sizeof(sz_line), cb_line, " small=", s.c_small.load(std::memory_order_relaxed));
        AppendCount(sz_line, sizeof(sz_line), cb_line, " atlas=", s.c_atlas.load(std::memory_order_relaxed));
        AppendCount(sz_line, sizeof(sz_line), cb_line, " png=", c_png);
        AppendCount(sz_line, sizeof(sz_line), cb_line, " png_pending=",
                    c_entries - std::min<std::uint64_t>(c_entries, c_png + c_png_failed + c_no_png));
        AppendCount(sz_line, sizeof(sz_line), cb_line, " png_failed=", c_png_failed);
        AppendCount(sz_line, sizeof(sz_line), cb_line, " no_png=", c_no_png);
        AppendCount(sz_line, sizeof(sz_line), cb_line, " png_mib=", s.cb_disk.load(std::memory_order_relaxed) / MiB);
        AppendCount(sz_line, sizeof(sz_line), cb_line, " over_limit=",
                    s.c_over_limit.load(std::memory_order_relaxed));
        AppendCount(sz_line, sizeof(sz_line), cb_line, " uploads=", s.c_uploads.load(std::memory_order_relaxed));
        AppendCount(sz_line, sizeof(sz_line), cb_line, " binds=", s.c_binds.load(std::memory_order_relaxed));
    }
    Append(sz_line, sizeof(sz_line), cb_line, "\n");
    for (std::size_t off = 0; off < cb_line;) {
        const ssize_t written = write(1, sz_line + off, cb_line - off);
        if (written <= 0) {
            break;
        }
        off += std::size_t(written);
    }
}
