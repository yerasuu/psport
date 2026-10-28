// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "psport_copy.h"
#include "psport_toggles.h"
#include "psport_sections.h"
#include "psport_free_check.h"
#include "psport_native_probe.h"
#include "psport_guest_memory.h"
#include "game_profile.h"
#include "pslayer_gpu_memory.h"
#include "pslayer_write_traps.h"
#include "../../src/platform/ps_host.h"
#include "common/alignment.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <mutex>
#include <pthread.h>
#include <x86intrin.h>
#include <cstring>
#include <string>
#include <fmt/format.h>
#include "common/signal_context.h"
#include "core/signals.h"
#include <vk_mem_alloc.h>

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);
extern "C" int runtime_memory_direct_phys(uintptr_t address, uint64_t* phys, uintptr_t* end);

/// The layer's memory module asks the runtime where direct memory is mapped.
static bool LayerTranslate(uint64_t va, uint64_t* phys, uint64_t* end) {
    uintptr_t map_end = 0;
    if (!runtime_memory_direct_phys(va, phys, &map_end)) {
        return false;
    }
    *end = map_end;
    return true;
}

namespace VideoCore {

namespace {
// psport: the tick of the newest GPU write per 64 KiB of guest memory, per 1 MiB for writes over
// 16 MiB (ObtainBuffer with is_written: the only place ranges become GPU-modified). A readback
// waits for that submission only and copies on the readback queue, not behind the queued
// frames; a coarser grain let a buffer the GPU writes every frame hold up its neighbours.
// Read for every small constant binding of every draw (the GPU command thread, thousands of times
// a frame) while the draw recording thread notes writes: lock-free sparse page tables (a mutex and
// hash maps here took a third of that thread). Guest addresses below 2^40.
struct GpuWriteTicks {
    static constexpr u64 FineShift = 16, CoarseShift = 20;
    static constexpr u64 FineLimit = 16_MB;
    static constexpr u64 AddressBits = 40, LeafBits = 12, LeafMask = (1ull << LeafBits) - 1;
    struct Table {
        explicit Table(u64 shift_)
            : shift{shift_}, leaves((1ull << (AddressBits - shift_)) >> LeafBits) {}
        u64 shift;
        /// Page -> tick of the newest write; a leaf of 4096 pages is made at its first write.
        std::vector<std::atomic<std::atomic<u64>*>> leaves;
        std::atomic<u64>* Leaf(u64 page) const {
            return leaves[page >> LeafBits].load(std::memory_order_acquire);
        }
    };
    Table fine{FineShift};   ///< 64 KiB pages
    Table coarse{CoarseShift}; ///< 1 MiB pages (writes over FineLimit)
    std::mutex mutex;          ///< leaf creation
    void Note(u64 address, u64 size, u64 tick) {
        if (size == 0 || (address + size - 1) >> AddressBits) {
            return;
        }
        auto& table = size <= FineLimit ? fine : coarse;
        for (u64 page = address >> table.shift; page <= (address + size - 1) >> table.shift;
             ++page) {
            std::atomic<u64>* leaf = table.Leaf(page);
            if (!leaf) {
                std::scoped_lock lk{mutex};
                leaf = table.Leaf(page);
                if (!leaf) {
                    leaf = new std::atomic<u64>[1ull << LeafBits]{};
                    table.leaves[page >> LeafBits].store(leaf, std::memory_order_release);
                }
            }
            leaf[page & LeafMask].store(tick, std::memory_order_release);
        }
    }
    /// The newest write tick in the range, 0 when none is known.
    u64 Newest(u64 address, u64 size) const {
        if (size == 0 || (address + size - 1) >> AddressBits) {
            return 0;
        }
        u64 newest = 0;
        for (const Table* table : {&fine, &coarse}) {
            const u64 last = (address + size - 1) >> table->shift;
            for (u64 page = address >> table->shift; page <= last; ++page) {
                if (const std::atomic<u64>* leaf = table->Leaf(page)) {
                    newest = std::max(newest, leaf[page & LeafMask].load(std::memory_order_acquire));
                } else {
                    page |= LeafMask; // nothing written in this leaf
                }
            }
        }
        return newest;
    }
};
GpuWriteTicks& WriteTicks() {
    static GpuWriteTicks write_ticks;
    return write_ticks;
}

// psport (PS_READBACK_LOG): the newest GPU-written bindings, to show which writes a readback that
// waited for the GPU overlapped (a binding wider than what the shader writes would show here).
struct WrittenBinding {
    u64 address, size, tick;
};
std::array<WrittenBinding, 4096> written_bindings;
std::atomic<u64> written_head{0};

// psport: PS_READBACK_LOG=path — every download of GPU-written buffer memory back to guest
// memory (a CPU write to a range the GPU wrote): who asked, where, how much, how long it took,
// how old the GPU's newest write there was, and whether the readback queue served it.
struct ReadbackLog {
    FILE* file = nullptr;
    std::mutex mutex;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
ReadbackLog* Readbacks() {
    static ReadbackLog* const log = [] () -> ReadbackLog* {
        const char* path = std::getenv("PS_READBACK_LOG");
        if (!path || !*path) {
            return nullptr;
        }
        auto* readbacks = new ReadbackLog;
        readbacks->file = std::fopen(path, "w");
        if (!readbacks->file) {
            delete readbacks;
            return nullptr;
        }
        std::fprintf(readbacks->file, "t_s,requester,address,window_kb,downloaded_kb,copies,is_write,"
                                      "wait_ms,ticks_since_write,write_done,readback_queue\n");
        return readbacks;
    }();
    return log;
}
thread_local const char* readback_requester = "";
thread_local bool readback_is_write = false;

/// psport: copies finished GPU data out of an arena on the readback queue (Instance), waiting
/// only for those writes (the timeline semaphore at their tick), not for the queued frames.
class FastReadback {
public:
    explicit FastReadback(const Vulkan::Instance& instance)
        : device{instance.GetDevice()}, queue{instance.GetReadbackQueue()},
          allocator{instance.GetAllocator()} {
        if (!queue) {
            return;
        }
        pool = Vulkan::Check(device.createCommandPoolUnique({
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetReadbackQueueFamilyIndex(),
        }));
        cmdbuf = Vulkan::Check(device.allocateCommandBuffers({
            .commandPool = *pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        }))[0];
        fence = Vulkan::Check(device.createFenceUnique({}));
    }
    ~FastReadback() {
        if (buffer) {
            vmaDestroyBuffer(allocator, buffer, allocation);
        }
    }

    [[nodiscard]] bool Available() const noexcept {
        return bool(queue);
    }

    /// Copies `copies` (destination offsets from 0) of `arena` once `semaphore` reaches `tick`;
    /// returns the host copy, or null on failure (the caller takes the regular path).
    const u8* Copy(const Buffer* arena, std::span<const vk::BufferCopy> copies, u64 total,
                   vk::Semaphore semaphore, u64 tick) {
        if (!Ensure(total)) {
            return nullptr;
        }
        Vulkan::Check(cmdbuf.reset());
        Vulkan::Check(cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
        cmdbuf.copyBuffer(arena->Handle(), vk::Buffer{buffer}, copies);
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        Vulkan::Check(cmdbuf.end());
        const vk::SemaphoreSubmitInfo wait{
            .semaphore = semaphore,
            .value = tick,
            .stageMask = vk::PipelineStageFlagBits2::eTransfer,
        };
        const vk::CommandBufferSubmitInfo command{.commandBuffer = cmdbuf};
        const vk::SubmitInfo2 submit{
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &wait,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &command,
        };
        if (queue.submit2(submit, *fence) != vk::Result::eSuccess) {
            return nullptr;
        }
        const auto result = device.waitForFences(*fence, vk::True, UINT64_MAX);
        Vulkan::Check(device.resetFences(*fence));
        if (result != vk::Result::eSuccess) {
            return nullptr;
        }
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        return mapped;
    }

private:
    bool Ensure(u64 size) {
        if (size <= capacity) {
            return true;
        }
        if (buffer) {
            vmaDestroyBuffer(allocator, buffer, allocation);
            buffer = VK_NULL_HANDLE;
            capacity = 0;
        }
        const u64 new_capacity = std::max<u64>(std::bit_ceil(size), 1_MB);
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = new_capacity,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer, &allocation, &info) !=
            VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            return false;
        }
        mapped = static_cast<u8*>(info.pMappedData);
        capacity = new_capacity;
        return true;
    }

    vk::Device device;
    vk::Queue queue;
    VmaAllocator allocator;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer cmdbuf;
    vk::UniqueFence fence;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    u8* mapped = nullptr;
    u64 capacity = 0;
};
std::mutex fast_readback_mutex;
/// One readback queue user for the buffer cache's downloads and the module's read traps.
FastReadback& SharedReadback(const Vulkan::Instance& instance) {
    static FastReadback readback{instance};
    return readback;
}

// psport: PS_GPU_WRITE_TWINS=1 — a guest write into a page that holds bytes the GPU is still
// writing (counters, small compute outputs rewritten every frame) does not wait for the GPU: the
// guest's copy of those bytes is kept as a "twin" and the page becomes CPU-modified at once.
// Uploads leave the GPU's bytes in the arena while the guest bytes still match their twin (the
// guest did not write them); downloads do not overwrite guest bytes that no longer match it (the
// guest wrote them since). Only pages with at most PS_GPU_WRITE_TWINS_MAX (1024) GPU-written
// bytes: larger outputs (vertices the CPU reads back, FaceGen) keep waiting for the GPU.
// Not with PS_READBACKS=2: a twinned page stays GPU-modified (read-protected) while the guest may
// write it, a write-only page the page manager cannot make (it stopped at the first cutscene).
bool TwinsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("PS_GPU_WRITE_TWINS");
        return env && env[0] == '1' &&
               EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Precise;
    }();
    return enabled;
}
u64 TwinsMaxBytes() {
    static const u64 max_bytes = [] {
        const char* env = std::getenv("PS_GPU_WRITE_TWINS_MAX");
        return env ? std::strtoull(env, nullptr, 10) : 1024ull;
    }();
    return max_bytes;
}
std::mutex twins_mutex;
std::map<VAddr, std::vector<u8>> twins; ///< start -> guest bytes when the twin was made
std::atomic<u64> twin_faults{0}, twin_kept_bytes{0}, twin_guest_bytes{0};

enum class TwinRun { None, Unchanged, Changed };

/// Calls func(start, end, TwinRun) over [begin, end) in order (twins_mutex held).
template <typename Func>
void ForEachTwinRun(VAddr begin, VAddr end, Func&& func) {
    VAddr cursor = begin;
    auto it = twins.upper_bound(begin);
    if (it != twins.begin()) {
        --it;
    }
    for (; it != twins.end() && it->first < end; ++it) {
        const VAddr start = it->first, stop = it->first + it->second.size();
        if (stop <= begin) {
            continue;
        }
        const VAddr from = std::max(start, begin), to = std::min(stop, end);
        if (cursor < from) {
            func(cursor, from, TwinRun::None);
        }
        // psport: through the backing view: with PS_READBACKS=2 the page is read-protected, and a
        // fault here (GPU thread, twins_mutex held) waited for a readback needing twins_mutex:
        // the game froze at the first cutscene (a movie frame written into GPU-written memory).
        thread_local std::vector<u8> guest_bytes;
        guest_bytes.resize(to - from);
        Core::Memory::Instance()->ReadBacking(from, guest_bytes.data(), to - from);
        const u8* guest = guest_bytes.data();
        const u8* twin = it->second.data() + (from - start);
        VAddr run = from;
        bool same = guest[0] == twin[0];
        for (VAddr at = from + 1; at <= to; ++at) {
            const bool now_same = at < to && guest[at - from] == twin[at - from];
            if (at == to || now_same != same) {
                func(run, at, same ? TwinRun::Unchanged : TwinRun::Changed);
                run = at;
                same = now_same;
            }
        }
        cursor = to;
    }
    if (cursor < end) {
        func(cursor, end, TwinRun::None);
    }
}

/// Drops the twins' parts in [begin, end), keeping the parts outside it (twins_mutex held).
void EraseTwins(VAddr begin, VAddr end) {
    auto it = twins.upper_bound(begin);
    if (it != twins.begin()) {
        --it;
    }
    while (it != twins.end() && it->first < end) {
        const VAddr start = it->first, stop = it->first + it->second.size();
        if (stop <= begin) {
            ++it;
            continue;
        }
        std::vector<u8> bytes = std::move(it->second);
        it = twins.erase(it);
        if (start < begin) {
            twins[start] = std::vector<u8>(bytes.begin(), bytes.begin() + (begin - start));
        }
        if (stop > end) {
            twins[end] = std::vector<u8>(bytes.begin() + (end - start), bytes.end());
        }
    }
}

} // namespace

/// On an integrated GPU (Steam Deck) VRAM is the same memory: everything stays in place there.
bool integrated_gpu = false;
/// psport: Garlic memory (type 3) is cached in VRAM, Onion in place.
/// psport optimizations, for A/B: VRAM copies uploaded by GPU copies from the
/// guest chunks (PS_CHUNK_UPLOADS=0: CPU staging), texture data bound in place whatever its type
/// (PS_INPLACE_TEXTURES=1; off: by type).
bool ChunkUploads() {
    static const bool on = [] {
        const char* env = std::getenv("PS_CHUNK_UPLOADS");
        return !(env && env[0] == '0');
    }();
    return on;
}
bool ChunkTextures() {
    static const bool on = [] {
        const char* env = std::getenv("PS_CHUNK_TEXTURES");
        return !(env && env[0] == '0');
    }();
    return on;
}
bool InPlaceTextures() {
    static const bool on = [] {
        // Off by default: sampled data looked smeared and ghosted with it (2026-10-04).
        const char* env = std::getenv("PS_INPLACE_TEXTURES");
        return env && env[0] == '1';
    }();
    return on;
}

/// psport: the arena keeps the first binding of every block while the game runs (imported host
/// memory, NVIDIA): game data is read in place over the bus, never moved to VRAM and back, and
/// the few blocks outside the guest chunks get system memory, not VRAM. On NVIDIA (GTX 1660 Ti,
/// 615.71) the first blocks moved to VRAM held the GPU for 10 s and more, the desktop with it.
/// PS_FIXED_ARENA=1/0 forces it on/off (tests). Valid once the BufferCache checked the chunks.
bool FixedArena() {
    // Only sparse rebinding needs this NVIDIA workaround. The layer has ordinary chunk
    // buffers and VRAM mirrors, so fixing an arena that it does not use would disable all
    // its promotions on host-import GPUs (GTX 1660 Ti: zero mirrors and 10-15 FPS).
    if (PsGuestMemory::LayerMemory()) {
        return false;
    }
    static const bool on = [] {
        const char* env = std::getenv("PS_FIXED_ARENA");
        const bool fixed = env && *env ? env[0] != '0' : PsGuestMemory::HostImported();
        if (fixed) {
            std::printf("Guest memory: the arena keeps its first bindings (game data read in place, "
                        "no copies in VRAM)\n");
        }
        return fixed;
    }();
    return on;
}

bool MixStats() {
    static const bool on = [] {
        const char* env = std::getenv("PS_RESIDENCY_MIX");
        return env && env[0] == '1';
    }();
    return on;
}

/// psport PS_LAYER_MEMORY=1: the layer's memory module (gpu/layer) binds the game's memory: a
/// range in place is a range of its chunk's buffer, with separate VRAM mirrors and no sparse
/// arena. FixedArena must not prohibit those mirrors.
bool LayerMode() {
    return PsGuestMemory::LayerMemory();
}

bool GarlicInVram() {
    static const bool on = [] {
        const char* env = std::getenv("PS_GARLIC_VRAM");
        return env ? env[0] != '0' : !integrated_gpu;
    }();
    return on && !FixedArena();
}

bool WriteVerify() {
    static const bool on = [] {
        const char* env = std::getenv("PS_WRITE_VERIFY");
        return env && env[0] == '1';
    }();
    return on;
}

bool ConstantsInPlace() {
    static const bool on = [] {
        const char* env = std::getenv("PS_CONSTANTS_IN_PLACE");
        return env && env[0] == '1';
    }();
    return on != PsToggle::Experiment(1);
}

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;
constexpr std::optional<u32> CmaskColorExpandedValue(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return 0xFFFFFFFF;
    case 2:
        return 0xDDDDDDDD;
    case 4:
        return 0xEEEEEEEE;
    case 8:
        return 0xFFFFFFFF;
    default:
        return std::nullopt;
    }
}

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    integrated_gpu = instance.IsIntegrated();
    // psport: the GPU uses the game's direct memory in place, in chunks the runtime maps
    // (PsGuestMemory::Usable). Asked first: it also settles LayerMode for this GPU.
    if (!PsGuestMemory::Usable(instance)) {
        PsGuestMemory::Unsupported("direct memory cannot be shared with this driver");
    }
    // Hooks in the game's own code, from its profile (games/). PS_GUEST_HOOKS=0: none, as for a
    // game the port knows nothing about.
    const char* hooks_env = std::getenv("PS_GUEST_HOOKS");
    const Game::Profile* game = Game::Active();
    if (game && game->pfn_install_hooks && !(hooks_env && hooks_env[0] == '0')) {
        static BufferCache* hooked_cache = this;
        game->pfn_install_hooks(Game::CoreServices{
            .pfn_fresh_range = [](u64 address, u64 size) { hooked_cache->NoteFreshRange(address, size); },
        });
    } else if (game) {
        std::printf("Guest hooks: off (PS_GUEST_HOOKS=0): nothing in the game's own code is hooked\n");
    }
    if (LayerMode()) {
        PsLayer::GpuMemory::Get().SetTranslate(&LayerTranslate);
        std::printf("Guest memory: the layer's memory module binds the game's memory in place "
                    "(chunk buffers, no sparse arena for it; PS_LAYER_MEMORY=1)\n");
    }
    // psport: one bit under maxBufferSize, so that the two pages a request crossing a page
    // boundary needs always fit in one arena (RADV: 4 GiB - 4, 1 GiB pages); 4 GiB pages at most,
    // as before. Vulkan 1.3 guarantees at least 2^30.
    m_cb_max_arena = std::max<u64>(instance.MaxBufferSize(), u64{1} << 30);
    m_arena_page_bits = std::min<u64>(32, std::bit_width(m_cb_max_arena) - 2);
    m_c_max_arena_pages = m_cb_max_arena >> m_arena_page_bits;
    address_space.assign(u64{1} << (ADDRESS_SPACE_BITS - m_arena_page_bits), nullptr);
    std::printf("GPU memory: maxBufferSize %#llx, arena pages of %llu MiB, at most %llu pages an "
                "arena\n",
                static_cast<unsigned long long>(instance.MaxBufferSize()),
                static_cast<unsigned long long>((u64{1} << m_arena_page_bits) >> 20),
                static_cast<unsigned long long>(m_c_max_arena_pages));
    const std::array<u32, 2> families = {instance.GetGraphicsQueueFamilyIndex(),
                                         instance.GetReadbackQueueFamilyIndex()};
    const bool shared = bool(instance.GetReadbackQueue()); // as Buffer creates the arenas
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = u64{1} << m_arena_page_bits,
        .usage = ARENA_USAGE | PsGuestMemory::TransformFeedbackUsage(instance),
        .sharingMode = shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = shared ? 2u : 0u,
        .pQueueFamilyIndices = shared ? families.data() : nullptr,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    // psport: Vulkan reports a power of two alignment, which bit_ceil keeps as it is.
    block_size = std::bit_ceil(
        Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment));
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = (u64{1} << m_arena_page_bits) / block_size;
    blocks_per_arena_page_shift = m_arena_page_bits - block_shift;
    PsLayer::GpuMemory::Get().SetBlockShift(static_cast<u32>(block_shift));
    group_use.assign(u64{1} << (ADDRESS_SPACE_BITS - USE_GROUP_BITS), 0);
    group_demoted.assign(group_use.size(), 0);
    layer_group_use.assign(group_use.size(), 0);
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    arena_memory_type_bits = reqs.memoryTypeBits;
    {
        const auto& properties = instance.GetMemoryProperties();
        for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
            const auto flags = properties.memoryTypes[i].propertyFlags;
            if (((reqs.memoryTypeBits >> i) & 1) && !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal) &&
                (flags & vk::MemoryPropertyFlagBits::eHostVisible)) {
                arena_fallback_type_index = i;
                break;
            }
        }
    }

    const u64 c_blocks = u64{blocks_per_arena_page} * address_space.size();
    const u64 bda_pagetable_size = c_blocks * sizeof(vk::DeviceAddress) * (LayerActive() ? 2 : 1);
    fault_manager =
        std::make_unique<FaultManager>(instance, scheduler, *this, block_shift, c_blocks);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                 MemoryType::DeviceLocal, "Null Index Buffer");
    runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    if (LayerMode()) {
        LayerNoteCpuWrite(device_addr, size);
    }
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// psport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in a dense area, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const PsCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.p_context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.cb_copy);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.cb_copy);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
// Each source is noted once the copy is queued (a WaitHostCopies started after the note covers it).
void BufferCache::SmallGuestCopy(const PsCopy::Item& item) {
    if (!scheduler.IsRecordingDeferred()) {
        // psport: commands recorded on this thread (direct mode: the draw or dispatch after the
        // upscaler's passes; or PS_VK_RECORD_THREAD=0): the copy runs now, before the commands
        // that read it. In this thread's copy batch it ran at the next submission at the
        // earliest, possibly after a WRITE_DATA had changed its source (that wait runs only the
        // GpuComm thread's own batch). Suspected in the vertex explosions with
        // PS_VK_RECORD_THREAD=0 (issue #39).
        item.pfn_run(item);
    } else if (!PsToggle::Disabled(PsToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.pfn_run(item); });
    } else {
        PsCopy::QueueCopy(item);
    }
    scheduler.NoteHostCopySource(item.source, item.cb_copy);
}

void BufferCache::ExtendWriteFault(VAddr device_addr, u64 guest_rip) {
    NoteCpuWrite(device_addr, guest_rip);
    static const u64 window = [] {
        const char* env = std::getenv("PS_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || PsToggle::Disabled(PsToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    PsStats::g_c_readbacks.fetch_add(1, std::memory_order_relaxed);
    std::array<char, 16> requester{};
    if (Readbacks()) {
        pthread_getname_np(pthread_self(), requester.data(), requester.size());
    }
    const auto flush_request = [this, device_addr, size, is_write, requester] {
        readback_requester = requester.data();
        readback_is_write = is_write;
        const u64 first_block = device_addr >> block_shift;
        u64 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArenaClamped(first_block, last_block);
        if (!arena) {
            // Nothing downloaded (counted in GetArena); a write goes on as with a twin.
            if (is_write) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            }
            return;
        }

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        // psport (PS_GPU_WRITE_TWINS): bytes the GPU is still writing on the faulting page are
        // twinned instead of waited for (see TwinsEnabled).
        if (is_write && TwinsEnabled()) {
            const VAddr page_begin = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
            const VAddr page_end = Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE);
            u64 tick = 0, gpu_bytes = 0;
            gpu_modified_ranges.ForEachInRange(page_begin, page_end - page_begin,
                                               [&](VAddr start, VAddr end) {
                                                   gpu_bytes += end - start;
                                                   tick = std::max(tick, WriteTicks().Newest(
                                                                             start, end - start));
                                               });
            if (gpu_bytes != 0 && gpu_bytes <= TwinsMaxBytes() && tick != 0 &&
                !scheduler.IsFree(tick)) {
                {
                    std::scoped_lock lk{twins_mutex};
                    gpu_modified_ranges.ForEachInRange(
                        page_begin, page_end - page_begin, [&](VAddr start, VAddr end) {
                            // Parts with a twin keep their older snapshot: the guest bytes may
                            // have been written since it was taken.
                            ForEachTwinRun(start, end, [&](VAddr from, VAddr to, TwinRun run) {
                                if (run == TwinRun::None) {
                                    // The backing view: the page may be read-protected.
                                    std::vector<u8> bytes(to - from);
                                    Core::Memory::Instance()->ReadBacking(from, bytes.data(),
                                                                          to - from);
                                    twins[from] = std::move(bytes);
                                }
                            });
                        });
                }
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                twin_faults.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        // psport (PS_GPU_WRITE_TWINS): the window around the fault is downloaded only when the GPU has
        // done its writes there; else just the faulting page (the other pages stay GPU-modified
        // and protected), instead of waiting for GPU writes that have nothing to do with it.
        VAddr download_start = window_start, download_end = window_end;
        if (is_write && TwinsEnabled()) {
            u64 window_tick = 0;
            gpu_modified_ranges.ForEachInRange(
                window_start, window_end - window_start, [&](VAddr start, VAddr end) {
                    window_tick = std::max(window_tick, WriteTicks().Newest(start, end - start));
                });
            if (window_tick != 0 && !scheduler.IsFree(window_tick)) {
                download_start = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
                download_end = Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE);
            }
        }
        DownloadMemory(arena, download_start, download_end - download_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DropOverwrittenGpuData(VAddr addr, u64 size) {
    // The layer's mirrors hold GPU data the tracker does not describe alone (LayerNoteCpuWrite):
    // its writes keep the readback path.
    if (LayerMode() || EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Disabled) {
        return;
    }
    const VAddr pages_start = Common::AlignUp(addr, TRACKER_BYTES_PER_PAGE);
    const VAddr pages_end = Common::AlignDown(addr + size, TRACKER_BYTES_PER_PAGE);
    if (pages_start >= pages_end ||
        !memory_tracker->IsRegionGpuModified(pages_start, pages_end - pages_start)) {
        return;
    }
    liverpool->SendCommand<true>([this, pages_start, pages_end] {
        boost::container::small_vector<std::pair<VAddr, VAddr>, 8> ranges;
        memory_tracker->ForEachDownloadRange<false>(
            pages_start, pages_end - pages_start,
            [&ranges](VAddr start, u64 cb_range) { ranges.emplace_back(start, start + cb_range); });
        u64 cb_dropped = 0;
        for (const auto& [start, end] : ranges) {
            // A GPU write not done yet could land in the game's memory after the CPU's (a block in
            // place) or be read back over it later: such pages wait for it at the write's fault.
            const u64 tick = WriteTicks().Newest(start, end - start);
            if (tick == 0 || !scheduler.IsFree(tick)) {
                PsStats::g_c_overwrite_drops_kept.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (TwinsEnabled()) {
                std::scoped_lock lk{twins_mutex};
                EraseTwins(start, end);
            }
            gpu_modified_ranges.Subtract(start, end - start);
            memory_tracker->UnmarkRegionAsGpuModified(start, end - start);
            memory_tracker->MarkRegionAsCpuModified(start, end - start);
            cb_dropped += end - start;
        }
        if (cb_dropped != 0) {
            PsStats::g_c_overwrite_drops.fetch_add(1, std::memory_order_relaxed);
            PsStats::g_cb_overwrite_drop.fetch_add(cb_dropped, std::memory_order_relaxed);
        }
    });
}

bool BufferCache::LayerReadTrapHit(VAddr address, TrapCaller caller) {
    if (!(PsLayer::WriteTraps::Reasons(address) & PsLayer::WriteTraps::VramData)) {
        return false;
    }
    layer_read_trap_hits.fetch_add(1, std::memory_order_relaxed);
    switch (caller) {
    case TrapCaller::Guest:
        liverpool->SendCommand<true>([this, address] { LayerCopyBackForCpu(address, true); });
        break;
    case TrapCaller::CommandThread:
        LayerCopyBackForCpu(address, true); // the caller drained the draw pipe
        break;
    case TrapCaller::RecordingThread:
        LayerCopyBackForCpu(address, false);
        break;
    }
    return true;
}

void BufferCache::LayerCopyBackForCpu(VAddr address, bool may_record) {
    if (!(PsLayer::WriteTraps::Reasons(address) & PsLayer::WriteTraps::VramData)) {
        return; // copied back meanwhile
    }
    // The GPU's data in the block's VRAM copy (one readback for the block, not for each page),
    // once the GPU has written it: on the readback queue, without waiting for the queued frames.
    const VAddr from = Common::AlignDown(address, block_size);
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    boost::container::small_vector<VAddr, 8> guests;
    const Buffer* mirror_buffer = nullptr;
    u64 total = 0, tick = 0;
    if (const auto mirror = PsLayer::GpuMemory::Get().MirrorAt(from)) {
        mirror_buffer = static_cast<const Buffer*>(mirror->p_owner);
        gpu_modified_ranges.ForEachInRange(from, block_size, [&](VAddr start, VAddr end) {
            copies.push_back({mirror_buffer->Offset(start), total, end - start});
            guests.push_back(start);
            total += Common::AlignUp(end - start, 64);
            tick = std::max(tick, WriteTicks().Newest(start, end - start));
        });
    }
    bool copied = copies.empty();
    if (!copies.empty() && !may_record && tick != 0 && !scheduler.IsSubmitted(tick)) {
        // The recording thread itself, before its own GPU writes are submitted: they cannot be
        // waited for here. The page is read as it is in the game's memory (as without read
        // traps); the GPU's data stays marked for later copies back.
        layer_read_trap_stale.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lk{layer_read_trap_mutex};
        PsLayer::WriteTraps::Set(from, block_size, PsLayer::WriteTraps::VramData, false);
        layer_read_trapped.Subtract(from, block_size);
        return;
    }
    if (!copies.empty()) {
        if (tick != 0 && !scheduler.IsFree(tick)) {
            if (may_record) {
                scheduler.Wait(tick); // submitted, and done
            } else {
                scheduler.GetWorkSemaphore()->Wait(tick); // submitted already: done
            }
        }
        auto& readback = SharedReadback(instance);
        if (readback.Available() && !PsToggle::Disabled(PsToggle::ReadbackQueue)) {
            std::scoped_lock lk{fast_readback_mutex};
            if (const u8* data = readback.Copy(mirror_buffer, copies, total,
                                               scheduler.GetWorkSemaphore()->Handle(), tick)) {
                for (size_t i = 0; i < copies.size(); ++i) {
                    memory->TryWriteBacking(std::bit_cast<u8*>(guests[i]),
                                            data + copies[i].dstOffset, copies[i].size);
                }
                copied = true;
            }
        }
        if (copied) {
            gpu_modified_ranges.Subtract(from, block_size);
        } else if (may_record) {
            // No readback queue: a copy in stream order, waited for.
            LayerCopyBack(from >> block_shift, (from >> block_shift) + 1);
            scheduler.Finish();
        } else {
            layer_read_trap_stale.fetch_add(1, std::memory_order_relaxed);
        }
        layer_read_trap_bytes.fetch_add(total, std::memory_order_relaxed);
    } else if (layer_copy_back_tick != 0 && !scheduler.IsFree(layer_copy_back_tick)) {
        // A copy back recorded is not done yet.
        if (may_record) {
            scheduler.Wait(layer_copy_back_tick);
        } else if (scheduler.IsSubmitted(layer_copy_back_tick)) {
            scheduler.GetWorkSemaphore()->Wait(layer_copy_back_tick);
        } else {
            layer_read_trap_stale.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::scoped_lock lk{layer_read_trap_mutex};
    PsLayer::WriteTraps::Set(from, block_size, PsLayer::WriteTraps::VramData, false);
    layer_read_trapped.Subtract(from, block_size);
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto write_range = [&](VAddr guest, const u8* src, u64 bytes) {
        PsFreeCheck::Check(guest, bytes, src, PsFreeCheck::BufferDownload);
        memory->TryWriteBacking(std::bit_cast<u8*>(guest), src, bytes);
    };
    const auto write_back = [&](const u8* data, u64 data_offset) {
        std::unique_lock twins_lock{twins_mutex, std::defer_lock};
        if (TwinsEnabled()) {
            twins_lock.lock();
        }
        for (const auto& copy : copies) {
            const VAddr guest = arena_base + copy.srcOffset;
            const u8* src = data + (copy.dstOffset - data_offset);
            if (!TwinsEnabled() || twins.empty()) {
                write_range(guest, src, copy.size);
                continue;
            }
            // psport (PS_GPU_WRITE_TWINS): guest bytes written since their twin was made stay;
            // the page is CPU-modified, so its next upload carries them to the arena.
            ForEachTwinRun(guest, guest + copy.size, [&](VAddr from, VAddr to, TwinRun run) {
                if (run != TwinRun::Changed) {
                    write_range(from, src + (from - guest), to - from);
                }
            });
            EraseTwins(guest, guest + copy.size);
        }
    };
    const auto wait_start = std::chrono::steady_clock::now();
    const u64 tick_before = scheduler.CurrentTick();
    // psport: the readback queue copies the data once the GPU has done the newest write into
    // it (waiting for that submission only when it is still queued); the graphics queue would
    // first run all queued work (~a frame: 20-30 ms stutters whenever the game wrote into
    // memory a GPU pass had written).
    u64 write_tick = 0;
    for (const auto& copy : copies) {
        write_tick = std::max(write_tick, WriteTicks().Newest(arena_base + copy.srcOffset, copy.size));
    }
    const bool write_done = write_tick != 0 && scheduler.IsFree(write_tick);
    bool fast = false;
    auto& readback = SharedReadback(instance);
    if (write_tick != 0 && readback.Available() && !PsToggle::Disabled(PsToggle::ReadbackQueue)) {
        if (!write_done) {
            scheduler.Wait(write_tick);
        }
        {
            std::scoped_lock lk{fast_readback_mutex};
            if (const u8* data = readback.Copy(arena, copies, total_size_bytes,
                                               scheduler.GetWorkSemaphore()->Handle(),
                                               write_tick)) {
                write_back(data, 0);
                fast = true;
            }
        }
    }
    if (!fast) {
        const auto download =
            staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
        if (download.buffer) {
            for (auto& copy : copies) {
                copy.dstOffset += download.offset;
            }
            runtime.CopyBuffer(arena, download.buffer, copies);
            scheduler.Finish();
            download.buffer->Invalidate(download.offset, download.size);
            write_back(download.mapped, download.offset);
        } else {
            LOG_ERROR_COUNTED(Render_Vulkan, "Readback of {:#x} bytes at {:#x} skipped",
                              total_size_bytes, device_addr);
            // psport: the data stays GPU-modified so that the next access reads it back again.
            for (const auto& copy : copies) {
                gpu_modified_ranges.Add(arena_base + copy.srcOffset, copy.size);
            }
            return;
        }
    }
    if (auto* log = Readbacks()) {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lk{log->mutex};
        std::fprintf(log->file, "%.4f,%s,%#llx,%llu,%llu,%zu,%d,%.3f,%lld,%d,%d\n",
                     std::chrono::duration<double>(now - log->start).count(), readback_requester,
                     (unsigned long long)device_addr, (unsigned long long)(size >> 10),
                     (unsigned long long)(total_size_bytes >> 10), copies.size(),
                     readback_is_write ? 1 : 0,
                     std::chrono::duration<double, std::milli>(now - wait_start).count(),
                     write_tick ? (long long)(tick_before - write_tick) : -1LL, write_done ? 1 : 0,
                     fast ? 1 : 0);
        if (!write_done && write_tick != 0) {
            // The GPU-written bindings this readback waited for (newest first).
            const u64 n = written_head.load();
            u32 shown = 0;
            for (u64 i = n; i-- > (n > written_bindings.size() ? n - written_bindings.size() : 0) &&
                            shown < 4;) {
                const WrittenBinding b = written_bindings[i % written_bindings.size()];
                bool overlaps = false;
                u64 cpu_lo = ~0ull, cpu_hi = 0;
                for (const auto& copy : copies) {
                    const u64 a = arena_base + copy.srcOffset;
                    overlaps |= b.address < a + copy.size && a < b.address + b.size;
                    cpu_lo = std::min(cpu_lo, a);
                    cpu_hi = std::max(cpu_hi, a + copy.size);
                }
                if (overlaps) {
                    std::fprintf(log->file,
                                 "# waited for GPU write %#llx+%llu (%lld ticks before), downloaded "
                                 "%#llx..%#llx\n",
                                 (unsigned long long)b.address, (unsigned long long)b.size,
                                 (long long)(tick_before - b.tick), (unsigned long long)cpu_lo,
                                 (unsigned long long)cpu_hi);
                    ++shown;
                }
            }
        }
        std::fflush(log->file);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// psport: PS_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = PsStats::g_c_gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("PS_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    PS_SECTION(ObtainBuffer);
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // psport: buffers in the game's direct memory are used where they are. Small
    // read-only ones (constants) are still copied when bound (the stream path below): commands
    // decoded later (WriteData, constant RAM dumps) change them on the CPU in decode order, and
    // a draw decoded before must keep its values (buffer renaming). GPU writes are noted for
    // that: a range the GPU has yet to write is not copied.
    if (is_written && !shadows.empty()) {
        DropShadows(device_addr, size);
    }
    // psport: a read-only binding of blocks that were resident and in place the last time, with
    // the residency unchanged since (most bindings in place, constants with
    // PS_CONSTANTS_IN_PLACE): the arena where it is, without the residency walk.
    struct InPlaceMemo {
        u64 first = ~0ULL, last = 0, generation = 0;
        const Buffer* arena = nullptr;
    };
    if (LayerMode() && !is_written && !is_texel_buffer && !stats && BindMemoOn()) {
        // psport: a reading binding resolved before, with nothing changed since (BindMemo).
        if (maintained_epoch != packet_epoch) {
            Maintain();
        }
        LayerProcessTraps();
        const BindMemo& memo = BindMemoSlot(device_addr, size);
        if (memo.va_range == device_addr && memo.cb_range == size && memo.generation == layer_generation &&
            memo.second == PsStats::g_coarse_second.load(std::memory_order_relaxed) &&
            memo.epoch == g_cpu_change_epoch.load(std::memory_order_acquire)) {
            (memo.is_vram ? PsStats::g_cb_bound_vram : PsStats::g_cb_bound_in_place)
                .fetch_add(size, std::memory_order_relaxed);
            return {memo.p_buffer, memo.off_buffer};
        }
    }
    // psport PS_LAYER_MEMORY: the game's memory in place through its chunk buffer (the module
    // says which); what is not whole in one chunk takes the paths below (VRAM copies).
    if (LayerMode() && (is_written || size > STREAM_THRESHOLD || ConstantsInPlace() ||
                        IsRegionGpuModified(device_addr, size) || LayerMirrored(device_addr, size))) {
        if (const auto bound = LayerBind(device_addr, size, is_written, is_texel_buffer)) {
            return *bound;
        }
        NoteLayerMiss(device_addr, size, is_written);
    }
    thread_local std::array<InPlaceMemo, 256> in_place_reads{};
    const u64 memo_first = device_addr >> block_shift;
    const u64 memo_last = (device_addr + size - 1) >> block_shift;
    auto& read_memo = in_place_reads[memo_first & (in_place_reads.size() - 1)];
    const bool memo_candidate = !LayerMode() && !is_written && !is_texel_buffer && !stats &&
                                (size > STREAM_THRESHOLD || ConstantsInPlace());
    if (memo_candidate && maintained_epoch != packet_epoch) {
        Maintain(); // what EnsureResident does first: residency changes queued since
    }
    if (memo_candidate && read_memo.first == memo_first && read_memo.last == memo_last &&
        read_memo.generation == ResidencyGeneration()) {
        NoteUse(memo_first << block_shift, (memo_last - memo_first + 1) << block_shift);
        PsStats::g_cb_bound_in_place.fetch_add(size, std::memory_order_relaxed);
        return {read_memo.arena, read_memo.arena->Offset(device_addr)};
    }
    if (is_written || size > STREAM_THRESHOLD || ConstantsInPlace() ||
        IsRegionGpuModified(device_addr, size)) {
        const u64 first_block = device_addr >> block_shift;
        u64 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArenaClamped(first_block, last_block);
        if (!arena) {
            return NullBinding();
        }
        size = static_cast<u32>(
            std::min<u64>(size, ((last_block + 1) << block_shift) - device_addr));
        // GPU-written buffers: the GPU is a writer we hear of, like the loaders: VRAM wherever
        // whole blocks are known data, as a default heap. The CPU does not read what the GPU wrote
        // there (no readbacks: PS_READBACKS was relaxed before too); PS_GPU_WRITES_IN_PLACE=1 keeps
        // them in place (PCIe, ~9 ms a frame on an RX 7800 XT).
        static const bool writes_in_place_forced = [] {
            const char* env = std::getenv("PS_GPU_WRITES_IN_PLACE");
            return env && env[0] == '1';
        }();
        const bool writes_in_place = writes_in_place_forced || force_writes_in_place;
        if (is_written && !writes_in_place) {
            NoteGpuWrite(device_addr, size);
        }
        EnsureResident(arena, first_block, last_block, is_written && writes_in_place);
        if (MixStats()) {
            NoteResidencyMix(first_block, last_block, size, is_written);
        }
        if (is_written && size > 64_KB && PsStats::g_is_enabled) {
            NoteLargeGpuWrite(device_addr, size);
        }
        if (IsInPlace(device_addr, size)) {
            if (is_written) {
                WriteTicks().Note(device_addr, size, scheduler.CurrentTick());
            }
            PsStats::g_cb_bound_in_place.fetch_add(size, std::memory_order_relaxed);
            if (is_written) {
                PsStats::g_cb_bound_in_place_written.fetch_add(size, std::memory_order_relaxed);
            }
            if (is_texel_buffer && !is_written) {
                SynchronizeMemoryFromImage(arena, arena->Offset(device_addr), device_addr, size);
            }
            TraceBinding(device_addr, size, is_written, is_written ? 0 : 2);
            if (!is_written && !is_texel_buffer) {
                if (const auto shadow = ShadowCopy(device_addr, size)) {
                    return *shadow;
                }
            }
            if (memo_candidate) {
                read_memo = {memo_first, memo_last, ResidencyGeneration(), arena};
            }
            return {arena, arena->Offset(device_addr)};
        }
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        PS_SECTION(ObtainStream);
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // psport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !PsToggle::Disabled(PsToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .pfn_run = &RunGuestCopy,
                    .p_context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .cb_copy = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                TraceBinding(device_addr, size, false, 3);
                return {&stream_buffer, *offset};
            }
        }
        // psport: a stream buffer without memory leaves the binding to the arena below.
        if (const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
            data) {
            memory->CopySparseMemory(device_addr, data, size);
            stream_buffer.Commit();
            TraceBinding(device_addr, size, false, 3);
            return {&stream_buffer, offset};
        }
    }
    const u64 first_block = device_addr >> block_shift;
    u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArenaClamped(first_block, last_block);
    if (!arena) {
        return NullBinding();
    }
    size =
        static_cast<u32>(std::min<u64>(size, ((last_block + 1) << block_shift) - device_addr));
    EnsureResident(arena, first_block, last_block);
    PS_SECTION(ObtainVram);
    if (!is_written) {
    }
    const u64 uploaded_before = PsStats::g_cb_buffer_upload.load(std::memory_order_relaxed);
    PsStats::g_cb_bound_vram.fetch_add(size, std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            PsStats::g_cb_buffer_upload.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        WriteTicks().Note(device_addr, size, scheduler.CurrentTick());
        if (Readbacks()) {
            written_bindings[written_head.fetch_add(1) % written_bindings.size()] =
                WrittenBinding{device_addr, size, scheduler.CurrentTick()};
        }
    }
    TraceBinding(device_addr, size, is_written, is_written ? 1 : 4);
    return {arena, arena->Offset(device_addr)};
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::CommandWriteMirror(VAddr device_addr,
                                                                           u32 size) {
    if (!LayerMode() || size == 0) {
        return std::nullopt;
    }
    const auto resolution = PsLayer::GpuMemory::Get().Resolve(device_addr, size);
    if (resolution.kind != PsLayer::Resolution::Kind::Mirror) {
        return std::nullopt;
    }
    // Not over volatile blocks (refreshed from the game's memory anyway) or data only the GPU has
    // there (copied back before a demotion; a write beside it keeps the rules simple).
    const u64 first = device_addr >> block_shift, end = ((device_addr + size - 1) >> block_shift) + 1;
    if (layer_volatile.Overlaps(first, end) || gpu_modified_ranges.Intersects(device_addr, size)) {
        return std::nullopt;
    }
    auto* mirror = static_cast<Buffer*>(resolution.span.p_source->p_owner);
    return std::pair<const Buffer*, u64>{mirror, mirror->Offset(device_addr)};
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::CommandWriteTarget(VAddr device_addr,
                                                                           u32 size) {
    if (size == 0) {
        return std::nullopt;
    }
    if (LayerMode()) {
        // Written by the GPU in place: a VRAM copy of it would be stale (step 3a), unless the
        // range is whole in one mirror: then the caller writes that copy too (CommandWriteMirror).
        // Demoting it made watched blocks (PS_LAYER_WATCH) go back and forth between VRAM and in
        // place (WRITE_DATA beside the data shaders read: 2 GiB a minute).
        if (!CommandWriteMirror(device_addr, size)) {
            LayerDemote(device_addr >> block_shift, ((device_addr + size - 1) >> block_shift) + 1,
                        true, 2);
        }
        const auto target = LayerInPlace(device_addr, size);
        if (target) {
            WriteTicks().Note(device_addr, size, scheduler.CurrentTick());
        }
        return target;
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    if (!arena) {
        return std::nullopt;
    }
    EnsureResident(arena, first_block, last_block);
    if (!IsInPlace(device_addr, size)) {
        return std::nullopt;
    }
    WriteTicks().Note(device_addr, size, scheduler.CurrentTick());
    return std::pair<const Buffer*, u64>{arena, arena->Offset(device_addr)};
}

void BufferCache::PrepareDynamicRange(VAddr address, u64 size, bool is_written) {
    if (LayerMode()) {
        LayerPagedPublish(address, size, is_written);
        return;
    }
    const u32 cb_range = static_cast<u32>(std::min<u64>(size, 0xFFFFFFFFull));
    if (is_written || cb_range > STREAM_THRESHOLD || IsRegionGpuModified(address, cb_range)) {
        (void)ObtainBuffer(address, cb_range, is_written);
        return;
    }
    // A small read-only range would be copied into the stream buffer, out of the page table's
    // reach: its blocks are made resident in the arena like a larger binding's.
    const u64 first_block = address >> block_shift;
    const u64 last_block = (address + cb_range - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    if (!arena) {
        return;
    }
    EnsureResident(arena, first_block, last_block);
    if (!IsInPlace(address, cb_range)) {
        SynchronizeMemory(arena, address, cb_range, false, false);
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    // psport: texture data is read by the GPU from the game's memory itself:
    // through the arena where it is bound in place, else (PS_CHUNK_TEXTURES) from its guest
    // memory chunk.
    if (LayerMode()) {
        // From its mirror where the data is in VRAM (the GPU may have written it there), else
        // in place (a mixed range: the GPU's data in mirrors copied back first, LayerBind). No
        // mirror is made for an upload: the image holds the data in VRAM.
        if (const auto source = LayerBind(device_addr, size, false, false, false)) {
            TraceBinding(device_addr, size, false, 6);
            return *source;
        }
        NoteLayerMiss(device_addr, size, false);
    } else {
        const u64 first_block = device_addr >> block_shift;
        const u64 last_block = (device_addr + size - 1) >> block_shift;
        const Buffer* arena = InPlaceTextures() ? GetArena(first_block, last_block) : nullptr;
        if (arena) {
            EnsureResident(arena, first_block, last_block, true);
        }
        if (IsInPlace(device_addr, size)) {
            if (!arena) {
                arena = GetArena(first_block, last_block);
            }
            if (arena) {
                TraceBinding(device_addr, size, false, 5);
                return {arena, arena->Offset(device_addr)};
            }
        }
        if (ChunkTextures() && !IsRegionGpuModified(device_addr, size)) {
            if (const auto source = GuestChunkSource(device_addr, size)) {
                TraceBinding(device_addr, size, false, 6);
                    return *source;
            }
        }
    }
    if (IsRegionGpuModified(device_addr, size)) {
        TraceBinding(device_addr, size, false, 7);
        return ObtainBuffer(device_addr, size, false);
    }
    TraceBinding(device_addr, size, false, 9);
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!staging.buffer) {
        return ObtainBuffer(device_addr, size, false);
    }
    if (!PsToggle::Disabled(PsToggle::DeferredUploads)) {
        // psport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const PsCopy::Item item{
                .pfn_run = &RunGuestCopy,
                .p_context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .cb_copy = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                PsCopy::Async([item] { item.pfn_run(item); });
                scheduler.NoteHostCopySource(item.source, item.cb_copy);
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    if (memory_tracker->IsRegionGpuModified(addr, size)) {
        return true;
    }
    // psport: GPU writes are not tracked per page; one not done yet counts.
    // So does what the GPU wrote into a VRAM copy (NoteGpuWrite: its data, as a default heap):
    // the game's memory under it is older (issue #122). PS_GPU_DATA_CHECK=0: not counted.
    static const bool gpu_data_check = [] {
        const char* env = std::getenv("PS_GPU_DATA_CHECK");
        return !(env && env[0] == '0');
    }();
    if (gpu_data_check && !LayerMode() && gpu_written_bytes.Overlaps(addr, addr + size) &&
        !IsInPlace(addr, size)) {
        return true;
    }
    const u64 tick = WriteTicks().Newest(addr, size);
    if (tick == 0 || scheduler.GetWorkSemaphore()->IsFree(tick)) {
        return false;
    }
    // Asking the driver is a system call, and the GPU command thread asks for thousands of
    // constant bindings a frame: at most every ~250 us per thread (until then the write
    // counts as not done, which only takes the slower, always correct path; the GPU signal
    // thread refreshes the known tick on every fence anyway).
    thread_local u64 last_refresh = 0;
    if (const u64 now = __rdtsc(); now - last_refresh > 800000) {
        last_refresh = now;
        scheduler.GetWorkSemaphore()->Refresh();
    }
    return !scheduler.GetWorkSemaphore()->IsFree(tick);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        // psport: page by page, since a resident range may cross arenas.
        for (u64 block = range.start; block < range.end;) {
            const u64 page = block >> blocks_per_arena_page_shift;
            const u64 end = std::min<u64>(range.end, (page + 1) << blocks_per_arena_page_shift);
            if (const Buffer* arena = address_space[page]) {
                SynchronizeMemory(arena, block << block_shift, (end - block) << block_shift, false,
                                  false);
            }
            block = end;
        }
    }
}

namespace {
enum class ArenaSpan { Split, Clamped, Failed };

void NoteArenaSpan(ArenaSpan kind, VAddr va_first, VAddr va_end) {
    static std::array<std::atomic<u64>, 3> s_c_spans{};
    static constexpr std::array<std::string_view, 3> s_what = {
        "got an arena of its own pages: merging its arenas would exceed maxBufferSize",
        "clamped to what one arena covers (maxBufferSize)",
        "has no arena: bound to the null binding or skipped",
    };
    static constexpr std::array<std::atomic<u64>*, 3> s_p_stats = {
        &PsStats::g_c_arena_spans_split,
        &PsStats::g_c_arena_spans_clamped,
        &PsStats::g_c_arena_spans_failed,
    };
    const auto i_kind = static_cast<std::size_t>(kind);
    s_p_stats[i_kind]->fetch_add(1, std::memory_order_relaxed);
    const u64 c_spans = s_c_spans[i_kind].fetch_add(1, std::memory_order_relaxed) + 1;
    if ((c_spans & (c_spans - 1)) == 0) {
        LOG_WARNING(Render, "Buffer range {:#x}..{:#x} {} ({} so far)", va_first, va_end,
                    s_what[i_kind], c_spans);
    }
}
} // namespace

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    const Buffer* owner = address_space[first_page];
    bool is_owned = owner != nullptr;
    for (u64 page = first_page + 1; is_owned && page <= last_page; ++page) {
        is_owned = address_space[page] == owner;
    }
    if (is_owned) {
        return owner;
    }
    const auto fail = [&] {
        NoteArenaSpan(ArenaSpan::Failed, first_block << block_shift,
                      (last_block + 1) << block_shift);
        return nullptr;
    };
    if (last_page - first_page + 1 > m_c_max_arena_pages) {
        return fail();
    }
    const auto create = [&](u64 begin_page, u64 end_page) -> const Buffer* {
        PsNativeProbe::Note(PsNativeProbe::BufferCreate);
        auto& arena = arenas.emplace_back(instance, begin_page << m_arena_page_bits,
                                          (end_page - begin_page) << m_arena_page_bits,
                                          MemoryType::Sparse);
        if (!arena.IsAllocated()) {
            arenas.pop_back();
            return nullptr;
        }
        return &arena;
    };

    // The arenas the range touches are replaced whole by one covering them all.
    u64 begin_page = first_page, end_page = last_page + 1;
    bool has_arenas = false;
    for (bool is_grown = true; is_grown;) {
        is_grown = false;
        for (u64 page = begin_page; page < end_page; ++page) {
            const Buffer* arena = address_space[page];
            if (!arena) {
                continue;
            }
            has_arenas = true;
            const u64 arena_begin = arena->cpu_addr >> m_arena_page_bits;
            const u64 arena_end = (arena->cpu_addr + arena->size_bytes) >> m_arena_page_bits;
            if (arena_begin < begin_page || arena_end > end_page) {
                begin_page = std::min(begin_page, arena_begin);
                end_page = std::max(end_page, arena_end);
                is_grown = true;
            }
        }
    }
    const Buffer* target = nullptr;
    if (end_page - begin_page <= m_c_max_arena_pages) {
        target = create(begin_page, end_page);
    } else {
        // Too large merged: the range's own pages move to an arena covering just them, an earlier
        // one where it exists, so that ranges alternating across a boundary between arenas do not
        // create one each time. The other pages keep their arenas, which still cover them.
        NoteArenaSpan(ArenaSpan::Split, first_block << block_shift,
                      (last_block + 1) << block_shift);
        begin_page = first_page;
        end_page = last_page + 1;
        const VAddr va_begin = begin_page << m_arena_page_bits;
        const VAddr va_end = end_page << m_arena_page_bits;
        for (auto it = arenas.rbegin(); it != arenas.rend() && !target; ++it) {
            if (it->cpu_addr <= va_begin && it->cpu_addr + it->size_bytes >= va_end) {
                target = &*it;
            }
        }
        if (!target) {
            target = create(begin_page, end_page);
        }
    }
    if (!target) {
        return fail();
    }
    if (has_arenas) {
        LOG_WARNING(Render, "Migrating arena");
    }

    // The pages changing arena get the blocks resident there bound into the new one (the BDA
    // page table keeps the old arena's addresses, whose binds stay).
    const auto rebind = [&](u64 page_begin, u64 page_end) {
        const u64 base_block = page_begin << blocks_per_arena_page_shift;
        const u64 end_block = page_end << blocks_per_arena_page_shift;
        resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
            const u64 start = std::max(base_block, backing.start);
            const u64 end = std::min(end_block, backing.end);
            BindsForArena(target)->binds.push_back(vk::SparseMemoryBind{
                .resourceOffset = (start << block_shift) - target->cpu_addr,
                .size = (end - start) << block_shift,
                .memory = backing.memory,
                .memoryOffset = (backing.offset + (start - backing.start)) << block_shift,
            });
        });
    };
    u64 run_begin = end_page;
    for (u64 page = begin_page; page < end_page; ++page) {
        if (address_space[page] == target) {
            if (run_begin != end_page) {
                rebind(run_begin, page);
                run_begin = end_page;
            }
            continue;
        }
        if (run_begin == end_page) {
            run_begin = page;
        }
        address_space[page] = target;
    }
    if (run_begin != end_page) {
        rebind(run_begin, end_page);
    }
    return target;
}

const Buffer* BufferCache::GetArenaClamped(u64 first_block, u64& last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 max_last_block =
        ((first_page + m_c_max_arena_pages) << blocks_per_arena_page_shift) - 1;
    if (last_block > max_last_block) {
        NoteArenaSpan(ArenaSpan::Clamped, first_block << block_shift,
                      (last_block + 1) << block_shift);
        last_block = max_last_block;
    }
    return GetArena(first_block, last_block);
}

std::pair<const Buffer*, u64> BufferCache::NullBinding() {
    if (!m_null_binding) {
        m_null_binding = std::make_unique<Buffer>(instance, 0, 64_KB, MemoryType::DeviceLocal,
                                                  "Null Binding Buffer");
    }
    return {m_null_binding.get(), 0};
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block,
                                 bool in_place) {
    PS_SECTION(EnsureResident);
    NoteUse(first_block << block_shift, (last_block - first_block + 1) << block_shift);
    if (maintained_epoch != packet_epoch) {
        Maintain();
    }
    // psport: the blocks were all resident the last time and nothing changed since: no interval
    // lookup (it ran for every buffer binding).
    thread_local std::array<ResidencyMemo, 256> resident_memo{};
    auto& memo = resident_memo[first_block & (resident_memo.size() - 1)];
    if (memo.first == first_block && memo.last == last_block &&
        memo.generation == ResidencyGeneration()) {
        return;
    }
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1,
                               [&](u64 start, u64 end) { bind_ranges.Add({start, end}); });
    if (bind_ranges.Empty()) {
        PsNativeProbe::Note(PsNativeProbe::MemoMiss);
        memo = {first_block, last_block, ResidencyGeneration()};
        return;
    }
    PsNativeProbe::Note(PsNativeProbe::BufferCreate);
    PsStats::Timer timer{PsStats::g_t_resident};

    // psport: blocks of the game's direct memory are bound to the memory it lives in; the rest
    // gets VRAM.
    std::vector<ResidentBind> binds;
    {
        IntervalList rest;
        for (const auto& range : bind_ranges) {
            BindInPlace(range.start, range.end, binds, rest, in_place);
        }
        bind_ranges = std::move(rest);
    }
    u64 vram_blocks = 0;
    for (const auto& range : bind_ranges) {
        vram_blocks += range.end - range.start;
    }

    if (vram_blocks != 0) {
        // psport: the slots of blocks that left VRAM first, block by block. A request of several
        // blocks only ever took new space, and the free slots piled up while new 64 MiB chunks
        // were allocated.
        IntervalList rest_ranges;
        for (const auto& range : bind_ranges) {
            u64 block = range.start;
            for (; block < range.end && free_slot_count != 0; ++block) {
                const auto [memory, memory_offset] = AllocateResidency(block_size);
                if (!memory) {
                    break;
                }
                auto* last = binds.empty() ? nullptr : &binds.back();
                if (last && last->end == block && last->memory == memory &&
                    last->offset + ((block - last->start) << block_shift) == memory_offset) {
                    last->end = block + 1;
                } else {
                    binds.push_back({block, block + 1, memory, memory_offset});
                }
                --vram_blocks;
            }
            if (block < range.end) {
                rest_ranges.Add({block, range.end});
            }
        }
        if (vram_blocks != 0) {
            auto [memory, memory_offset] = AllocateResidency(vram_blocks << block_shift);
            if (!memory && WaitForFreedResidency()) {
                IntervalList still_unbound;
                for (const auto& range : rest_ranges) {
                    resident_ranges.ForEachGap(range.start, range.end,
                                               [&](u64 start, u64 end) { still_unbound.Add({start, end}); });
                }
                rest_ranges = std::move(still_unbound);
                vram_blocks = 0;
                for (const auto& range : rest_ranges) {
                    vram_blocks += range.end - range.start;
                }
                if (vram_blocks != 0) {
                    std::tie(memory, memory_offset) = AllocateResidency(vram_blocks << block_shift);
                }
                static std::atomic<u32> waited{0};
                if (memory && waited.fetch_add(1, std::memory_order_relaxed) < 8) {
                    std::printf("Guest memory: VRAM is full: waited for the GPU to free memory (%llu MiB)\n",
                                (unsigned long long)((vram_blocks << block_shift) >> 20));
                }
            }
            if (!memory && vram_blocks != 0) {
                NoteResidencyShort(vram_blocks << block_shift);
            }
            for (const auto& range : rest_ranges) {
                if (!memory) {
                    unbacked_blocks.Add({range.start, range.end});
                    continue;
                }
                binds.push_back({range.start, range.end, memory, memory_offset});
                memory_offset += (range.end - range.start) << block_shift;
            }
        }
    }

    if (binds.empty()) {
        return;
    }
    u64 total_blocks = 0;
    for (const auto& bind : binds) {
        total_blocks += bind.end - bind.start;
    }
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(total_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);
    ArenaBinds* arena_binds = BindsForArena(arena);
    std::vector<vk::DeviceAddress> unstaged;
    if (!staging.mapped) {
        unstaged.resize(total_blocks);
    }
    auto* bda_addrs =
        staging.mapped ? reinterpret_cast<vk::DeviceAddress*>(staging.mapped) : unstaged.data();
    u64 offset = staging.offset;
    for (const auto& bind : binds) {
        Backing backing;
        backing.start = bind.start;
        backing.end = bind.end;
        backing.memory = bind.memory;
        backing.offset = bind.offset >> block_shift;
        resident_ranges.Add(backing);
        // psport: what was uploaded while these blocks had no memory went nowhere: the VRAM copy
        // is filled from the game's memory again.
        if (!unbacked_blocks.Empty() && unbacked_blocks.Overlaps(bind.start, bind.end)) {
            if (!in_place_blocks.Contains(bind.start, bind.end)) {
                unbacked_blocks.ForEachInRange(bind.start, bind.end, [&](const auto& iv) {
                    const u64 start = std::max(iv.start, bind.start);
                    const u64 end = std::min(iv.end, bind.end);
                    memory_tracker->MarkRegionAsCpuModified(start << block_shift, (end - start) << block_shift);
                });
            }
            unbacked_blocks.Subtract(bind.start, bind.end);
        }

        const auto& sparse = arena_binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (bind.start << block_shift) - arena->cpu_addr,
            .size = (bind.end - bind.start) << block_shift,
            .memory = bind.memory,
            .memoryOffset = bind.offset,
        });
        for (u64 block = 0; block < sparse.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + sparse.resourceOffset + block;
        }
        const u64 copy_size = (bind.end - bind.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, bind.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    if (staging.mapped) {
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
        return;
    }
    // psport: without staging memory the entries travel in the command buffer, since a binding
    // whose entries are missing would read through stale device addresses.
    const auto entries = std::as_bytes(std::span{unstaged});
    for (const auto& copy : copies) {
        runtime.UpdateBuffer(bda_pagetable_buffer.get(), copy.dstOffset,
                             {reinterpret_cast<const u8*>(entries.data()) + copy.srcOffset,
                              copy.size});
    }
}

std::pair<vk::DeviceMemory, u64> BufferCache::AllocateResidency(u64 bytes) {
    if (bytes == block_size && free_slot_count != 0) {
        // psport: a slot of the fullest chunk: the emptier ones drain and go back to the driver.
        auto best = residency_chunks.end();
        for (auto it = residency_chunks.begin(); it != residency_chunks.end(); ++it) {
            if (!it->second.free_offsets.empty() &&
                (best == residency_chunks.end() || it->second.used > best->second.used)) {
                best = it;
            }
        }
        if (best != residency_chunks.end()) {
            const u64 offset = best->second.free_offsets.back();
            best->second.free_offsets.pop_back();
            best->second.used += block_size;
            --free_slot_count;
            NoteResidencyUnused();
            return {vk::DeviceMemory{best->first}, offset};
        }
    }
    // psport: residency comes out of 64 MiB blocks (a larger request gets its own): a streaming
    // burst bound hundreds of small ranges, each its own vkAllocateMemory (~8 ms a burst). At most
    // one block is partly unused. The next block is allocated in the background once half of the
    // current one is used: the kernel clears new memory, on a Steam Deck with the CPU (16-22 ms a
    // burst). At most one spare block.
    constexpr u64 ResidencyBlock = 64_MB;
    // psport: VRAM full (a 4 GB card: "ErrorOutOfDeviceMemory", the game stopped): the block
    // comes from system memory the GPU reads over the bus, as a PC game's data spills there.
    // Slower, not fatal. PS_RESIDENCY_VRAM_LIMIT_MB=N (tests): VRAM "full" after N MiB of blocks.
    const auto allocate = [this](u64 size) {
        static const u64 test_limit = [] {
            const char* env = std::getenv("PS_RESIDENCY_VRAM_LIMIT_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        static std::atomic<u64> vram_bytes{0};
        const auto device = instance.GetDevice();
        vk::Result result = vk::Result::eErrorOutOfDeviceMemory;
        vk::DeviceMemory memory{};
        // FixedArena: no VRAM in the arena (system memory, as the guest chunks it is bound with).
        // psport: out of memory here returns none too, and AllocateResidency waits and tries
        // again; Vulkan::Check stopped the game (bbport 8185b88, PR #206).
        if (FixedArena() && arena_fallback_type_index) {
            auto host = device.allocateMemory(
                {.allocationSize = size, .memoryTypeIndex = *arena_fallback_type_index});
            if (host.result != vk::Result::eSuccess) {
                return vk::DeviceMemory{};
            }
            memory = host.value;
            PsStats::g_cb_device_alloc.fetch_add(size, std::memory_order_relaxed);
            PsStats::g_cb_residency_alloc.fetch_add(size, std::memory_order_relaxed);
            return memory;
        }
        if (!test_limit || vram_bytes.load(std::memory_order_relaxed) + size <= test_limit) {
            auto vram = device.allocateMemory({.allocationSize = size,
                                               .memoryTypeIndex = arena_memory_type_index});
            result = vram.result;
            memory = vram.value;
        }
        if (result == vk::Result::eSuccess) {
            vram_bytes.fetch_add(size, std::memory_order_relaxed);
        } else if ((result == vk::Result::eErrorOutOfDeviceMemory ||
                    result == vk::Result::eErrorOutOfHostMemory) &&
                   arena_fallback_type_index) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                std::printf("Guest memory: VRAM is full (%s): further copies of the game's memory "
                            "go to system memory (slower)\n",
                            vk::to_string(result).c_str());
            }
            auto host = device.allocateMemory(
                {.allocationSize = size, .memoryTypeIndex = *arena_fallback_type_index});
            if (host.result != vk::Result::eSuccess) {
                return vk::DeviceMemory{};
            }
            memory = host.value;
        } else {
            // No system-memory type for the arena (NVIDIA's sparse buffers take VRAM only):
            // AllocateResidency tries a smaller block and the other memory types.
            return vk::DeviceMemory{};
        }
        PsStats::g_cb_device_alloc.fetch_add(size, std::memory_order_relaxed);
        PsStats::g_cb_residency_alloc.fetch_add(size, std::memory_order_relaxed);
        return memory;
    };
    if (residency_used + bytes > residency_size) {
        u64 size = std::max(bytes, ResidencyBlock);
        vk::DeviceMemory memory =
            size == ResidencyBlock && spare_residency.valid() ? spare_residency.get() : vk::DeviceMemory{};
        // psport: VRAM full and no system memory to spill to (NVIDIA's sparse buffers take VRAM
        // only; RTX 3050 4 GB: "Failed to allocate 64 MiB for guest memory copies", the game
        // stopped): the texture collector evicts by submissions again for a while, and this
        // request gets just its size, then any other memory type the arena can be bound to.
        if (!memory) {
            memory = allocate(size);
        }
        if (!memory) {
            TextureCache::NoteVramShort();
            static std::atomic<u32> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::printf("Guest memory: VRAM is full (%llu MiB wanted): textures go sooner\n",
                            (unsigned long long)(size >> 20));
            }
            size = bytes;
            memory = allocate(size);
        }
        const auto allocate_other_type = [&] {
            const auto& types = instance.GetMemoryProperties();
            vk::DeviceMemory other_memory{};
            for (u32 i = 0; !other_memory && i < types.memoryTypeCount; ++i) {
                if (((arena_memory_type_bits >> i) & 1) && i != arena_memory_type_index) {
                    auto other = instance.GetDevice().allocateMemory({.allocationSize = size, .memoryTypeIndex = i});
                    if (other.result == vk::Result::eSuccess) {
                        other_memory = other.value;
                        PsStats::g_cb_device_alloc.fetch_add(size, std::memory_order_relaxed);
                        PsStats::g_cb_residency_alloc.fetch_add(size, std::memory_order_relaxed);
                    }
                }
            }
            return other_memory;
        };
        if (!memory) {
            memory = allocate_other_type();
        }
        if (!memory) {
            LOG_ERROR_COUNTED(Render_Vulkan, "No memory for {} MiB of guest memory copies (VRAM and system memory full)",
                              size >> 20);
            return {vk::DeviceMemory{}, 0};
        }
        residency_memory = memory;
        residency_size = size;
        residency_used = 0;
        residency_chunks[static_cast<VkDeviceMemory>(residency_memory)] = {size, 0, 0, 0};
    }
    const u64 offset = residency_used;
    residency_used += bytes;
    residency_chunks[static_cast<VkDeviceMemory>(residency_memory)].used += bytes;
    if (!spare_residency.valid() && residency_used * 2 >= residency_size) {
        spare_residency = std::async(std::launch::async, allocate, ResidencyBlock);
    }
    NoteResidencyUnused();
    return {residency_memory, offset};
}

void BufferCache::NoteResidencyUnused() {
    const u64 unused = free_slot_count * block_size + (residency_size - residency_used) +
                       (spare_residency.valid() ? 64_MB : 0);
    PsStats::g_cb_residency_unused.store(unused, std::memory_order_relaxed);
    PsStats::g_c_residency_chunks.store(residency_chunks.size(), std::memory_order_relaxed);
}

u32 BufferCache::VramIdleSeconds() {
    static const u32 seconds = [] {
        const char* env = std::getenv("PS_VRAM_IDLE_SECONDS");
        return env ? u32(std::strtoul(env, nullptr, 10)) : 60u;
    }();
    return seconds;
}

void BufferCache::NoteResidencyMix(u64 first_block, u64 last_block, u64 size, bool is_written) {
    // Per class (0 in place, 1 VRAM, 2 mixed): bindings and bytes; mixed by read/write too.
    static std::array<std::atomic<u64>, 3> count{}, bytes{};
    static std::atomic<u64> mixed_written{0}, mixed_gpu_data{0};
    static std::atomic<u32> printed_second{0};
    u64 in_place = 0;
    in_place_blocks.ForEachInRange(first_block, last_block + 1, [&](const Interval& iv) {
        in_place += std::min(iv.end, last_block + 1) - std::max(iv.start, first_block);
    });
    const u64 blocks = last_block - first_block + 1;
    const int cls = in_place == blocks ? 0 : in_place == 0 ? 1 : 2;
    count[cls].fetch_add(1, std::memory_order_relaxed);
    bytes[cls].fetch_add(size, std::memory_order_relaxed);
    if (cls == 2) {
        if (is_written) {
            mixed_written.fetch_add(1, std::memory_order_relaxed);
        }
        if (gpu_modified_ranges.Intersects(first_block << block_shift, blocks << block_shift)) {
            mixed_gpu_data.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Segment sizes for VRAM mirrors aligned to guest addresses (MEMORY_MODULE_PLAN): bindings
    // all in VRAM (and under 64 MiB) that cross a segment of 2^k MiB.
    constexpr std::array<u32, 6> SegmentMiB = {2, 4, 8, 16, 32, 64};
    static std::array<std::atomic<u64>, 6> crossing{};
    static std::atomic<u64> vram_small{0};
    const VAddr address = first_block << block_shift;
    if (cls == 1 && size < 64_MB) {
        vram_small.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < SegmentMiB.size(); ++i) {
            const u64 seg = u64(SegmentMiB[i]) << 20;
            if (address / seg != (address + size - 1) / seg) {
                crossing[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (now - printed_second.load(std::memory_order_relaxed) >= 2) {
        printed_second.store(now, std::memory_order_relaxed);
        const auto take = [](std::atomic<u64>& v) { return (unsigned long long)v.exchange(0); };
        std::string cross_text;
        for (size_t i = 0; i < SegmentMiB.size(); ++i) {
            cross_text += fmt::format(" {}M:{}", SegmentMiB[i], take(crossing[i]));
        }
        // Distinct segments holding VRAM blocks now (VRAM a mirror of segments would take).
        std::string seg_text;
        for (const u32 mib : SegmentMiB) {
            const u64 seg_blocks = (u64(mib) << 20) >> block_shift;
            u64 segments = 0, last = ~0ULL;
            for (const auto& range : resident_ranges) {
                if (!residency_chunks.contains(static_cast<VkDeviceMemory>(range.memory))) {
                    continue;
                }
                for (u64 s = range.start / seg_blocks; s <= (range.end - 1) / seg_blocks; ++s) {
                    segments += s != last;
                    last = s;
                }
            }
            seg_text += fmt::format(" {}M:{}MiB", mib, segments * mib);
        }
        std::printf("Residency mix: VRAM bindings under 64 MiB %llu, crossing a segment of%s; "
                    "VRAM as segments of%s\n",
                    take(vram_small), cross_text.c_str(), seg_text.c_str());
        const auto c0 = take(count[0]), c1 = take(count[1]), c2 = take(count[2]);
        const auto b0 = take(bytes[0]) >> 20, b1 = take(bytes[1]) >> 20, b2 = take(bytes[2]) >> 20;
        std::printf("Residency mix (2 s): in place %llu bindings %llu MiB, VRAM %llu / %llu MiB, "
                    "mixed %llu / %llu MiB (%llu written, %llu over GPU-written data)\n",
                    c0, b0, c1, b1, c2, b2, take(mixed_written), take(mixed_gpu_data));
    }
}

void BufferCache::NoteUse(VAddr address, u64 size) {
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    const u64 last = std::min<u64>((address + size - 1) >> USE_GROUP_BITS, group_use.size() - 1);
    for (u64 group = address >> USE_GROUP_BITS; group <= last; ++group) {
        if (group_use[group] != now) {
            group_use[group] = now;
        }
        if (group_demoted[group]) [[unlikely]] {
            group_demoted[group] = 0;
            const u64 first = (group << USE_GROUP_BITS) >> block_shift;
            const u64 end = ((group + 1) << USE_GROUP_BITS) >> block_shift;
            idle_demoted.ForEachInRange(first, end, [&](const Interval& range) {
                promote_candidates.Add({std::max(first, range.start), std::min(end, range.end)});
            });
            idle_demoted.Subtract(first, end);
        }
    }
}

void BufferCache::ReleaseResidencySlot(vk::DeviceMemory memory, u64 offset) {
    if (const auto it = residency_chunks.find(static_cast<VkDeviceMemory>(memory));
        it != residency_chunks.end()) {
        it->second.free_offsets.push_back(offset);
        ++free_slot_count;
        it->second.used -= block_size;
        if (it->second.used == 0) {
            it->second.empty_second = PsStats::g_coarse_second.load(std::memory_order_relaxed);
            it->second.empty_tick = scheduler.CurrentTick();
        }
    }
    NoteResidencyUnused();
}

void BufferCache::EvictVramBlock(u64 block) {
    const auto old = resident_ranges.Find(block);
    if (old == resident_ranges.end() || in_place_blocks.Contains(block) ||
        !residency_chunks.contains(static_cast<VkDeviceMemory>(old->memory))) {
        return;
    }
    const vk::DeviceMemory memory = old->memory;
    const u64 offset = (old->offset + (block - old->start)) << block_shift;
    const VAddr address = block << block_shift;
    for (const auto& arena : arenas) {
        if (arena.cpu_addr <= address && address - arena.cpu_addr < arena.size_bytes) {
            BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                .resourceOffset = address - arena.cpu_addr,
                .size = block_size,
                .memory = {},
                .memoryOffset = 0,
            });
        }
    }
    resident_ranges.Subtract(block, block + 1);
    memory_tracker->UnmarkRegionAsGpuModified(address, block_size);
    gpu_modified_ranges.Subtract(address, block_size);
    memory_tracker->MarkRegionAsCpuModified(address, block_size);
    ReleaseResidencySlot(memory, offset);
    // After the work submitted so far: it may still read the block.
    bind_wait_tick = std::max(bind_wait_tick, scheduler.CurrentTick() - 1);
    PsStats::g_cb_vram_unmapped.fetch_add(block_size, std::memory_order_relaxed);
}

void BufferCache::ProcessIdleBlocks() {
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (now == idle_scan_second || VramIdleSeconds() == 0) {
        return;
    }
    idle_scan_second = now;
    if (LayerMode()) {
        LayerMaintain();
        LayerProcessIdle();
        return;
    }
    // Blocks in VRAM whose 2 MiB were not bound for the idle time go back in place
    // (ProcessDemotions: only blocks of the game's direct memory without GPU-written data).
    const u64 tick = scheduler.CurrentTick();
    for (const auto& range : resident_ranges) {
        if (!residency_chunks.contains(static_cast<VkDeviceMemory>(range.memory))) {
            continue; // in place
        }
        for (u64 block = range.start; block < range.end; ++block) {
            if (now - group_use[(block << block_shift) >> USE_GROUP_BITS] >= VramIdleSeconds()) {
                demotions_waiting.push_back({block, tick, false, true});
            }
        }
    }
    // Over 128 MiB of free slots: the blocks of the emptiest chunk go back in place too, idle or
    // not, when the free slots of the others can take them; the ones bound again return to VRAM
    // into fuller chunks (AllocateResidency), and the chunk empties. Free slots spread evenly
    // (half of 1.3 GiB) left no chunk empty. One chunk every 10 s, each once a minute at most
    // (blocks with GPU-written data stay).
    if (free_slot_count * block_size >= 128_MB && now - last_evacuation >= 10) {
        auto sparsest = residency_chunks.end();
        for (auto it = residency_chunks.begin(); it != residency_chunks.end(); ++it) {
            const ResidencyChunk& chunk = it->second;
            if (it->first == static_cast<VkDeviceMemory>(residency_memory) || chunk.used == 0 ||
                now - chunk.evacuated_second < 60) {
                continue;
            }
            if (sparsest == residency_chunks.end() || chunk.used < sparsest->second.used) {
                sparsest = it;
            }
        }
        const u64 free_elsewhere =
            sparsest == residency_chunks.end()
                ? 0
                : (free_slot_count - sparsest->second.free_offsets.size()) * block_size;
        if (sparsest != residency_chunks.end() && sparsest->second.used <= free_elsewhere) {
            sparsest->second.evacuated_second = now;
            last_evacuation = now;
            PsStats::g_c_evacuations.fetch_add(1, std::memory_order_relaxed);
            for (const auto& range : resident_ranges) {
                if (static_cast<VkDeviceMemory>(range.memory) != sparsest->first) {
                    continue;
                }
                for (u64 block = range.start; block < range.end; ++block) {
                    demotions_waiting.push_back({block, tick, false, true, true});
                }
            }
        }
    }
    // Chunks without blocks for 5 s, the rebinds away from them long done: back to the driver.
    FreeEmptyResidencyChunks(5);
}

void BufferCache::FreeEmptyResidencyChunks(u32 min_empty_seconds) {
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    for (auto it = residency_chunks.begin(); it != residency_chunks.end();) {
        const ResidencyChunk& chunk = it->second;
        const VkDeviceMemory memory = it->first;
        if (chunk.used != 0 || memory == static_cast<VkDeviceMemory>(residency_memory) ||
            now - chunk.empty_second < min_empty_seconds || !pending_binds.empty() ||
            !scheduler.GetWorkSemaphore()->IsFree(chunk.empty_tick + 1)) {
            ++it;
            continue;
        }
        free_slot_count -= chunk.free_offsets.size();
        instance.GetDevice().freeMemory(vk::DeviceMemory{memory});
        PsStats::g_cb_residency_alloc.fetch_sub(chunk.size, std::memory_order_relaxed);
        PsStats::g_cb_device_free.fetch_add(chunk.size, std::memory_order_relaxed);
        PsStats::g_cb_vram_chunks_freed.fetch_add(chunk.size, std::memory_order_relaxed);
        it = residency_chunks.erase(it);
        NoteResidencyUnused();
    }
}

bool BufferCache::WaitForFreedResidency() {
    // psport: memory still full (bbport 8185b88, PR #206; 4 GB cards on area loads): what was
    // freed is given back only once the GPU is past it. Pending operations are not run here: one
    // of them (a fault callback) binds buffers again, over the binding being made, and the texture
    // binding helper may be reading what another erases.
    const u32 frame = PsStats::g_frame_number.load(std::memory_order_relaxed);
    if (residency_failed_frame == frame || Vulkan::Scheduler::IsInsideSchedulerWork()) {
        return false;
    }
    residency_failed_frame = frame;
    scheduler.WaitSubmittedWork();
    FreeEmptyResidencyChunks(0);
    return true;
}

void BufferCache::NoteResidencyShort(u64 bytes) {
    // psport: the blocks stay unbound (the arena is sparse resident: reads undefined, writes
    // dropped); a later binding over them tries again and uploads their data.
    LOG_ERROR_COUNTED(Render_Vulkan, "No memory for {} MiB of guest memory copies (VRAM and system memory full)",
                      bytes >> 20);
}

bool BufferCache::VramEligible(u64 block) {
    if (!GarlicInVram() || dynamic_blocks.Contains(block)) {
        return false;
    }
    // Diagnostics: PS_GARLIC_VRAM_RANGE=begin,end keeps VRAM copies to guest addresses in
    // [begin, end) (finding the block whose copy goes stale by halving the range).
    static const std::pair<u64, u64> vram_range = [] {
        const char* env = std::getenv("PS_GARLIC_VRAM_RANGE");
        char* end = nullptr;
        const u64 begin = env ? std::strtoull(env, &end, 0) : 0;
        return std::pair<u64, u64>{begin, end && *end == ',' ? std::strtoull(end + 1, nullptr, 0) : ~0ull};
    }();
    if ((block << block_shift) < vram_range.first || (block << block_shift) >= vram_range.second) {
        return false;
    }
    if (LayerMode()) {
        return LayerEligible(block);
    }
    // Every byte last written by a loader or by the GPU.
    bool known = true;
    asset_bytes.ForEachGap(block << block_shift, (block + 1) << block_shift,
                           [&](u64 start, u64 end) {
                               known = known && gpu_written_bytes.Contains(start, end);
                           });
    // Any memory type: what decides is that every write there is one we hear of (Onion data
    // the CPU keeps writing is written by code we do not hear of, so it is not known data).
    return known;
}

void BufferCache::BindInPlace(u64 start, u64 end, std::vector<ResidentBind>& out,
                              IntervalList<>& rest, bool any_type) {
    static std::atomic<u64> in_place_bytes{0}, vram_bytes{0}, reports{0};
    for (u64 block = start; block < end;) {
        const VAddr address = block << block_shift;
        u64 phys = 0;
        uintptr_t mapping_end = 0;
        const PsGuestMemory::Chunk* chunk = nullptr;
        u64 run = 0;
        // PS4 memory types as PC heaps: Onion (CPU-cached; what the CPU writes often: constants,
        // dynamic buffers) stays in place, like an upload heap the GPU reads over PCIe; Garlic
        // (what the GPU reads a lot and the CPU rarely writes: geometry, textures) gets a VRAM
        // copy kept current like a default heap (PS_GARLIC_VRAM=0: in place too).
        // Without write tracking only data whose writes we hear of may keep a VRAM copy.
        const bool garlic = !any_type && VramEligible(block);
        // LayerMode: what reaches the arena is not bound in place (no chunk memory in sparse).
        if (!garlic && !LayerMode() && runtime_memory_direct_phys(address, &phys, &mapping_end) &&
            (phys & (block_size - 1)) == 0 && (chunk = PsGuestMemory::Find(phys)) != nullptr) {
            // Whole blocks within the mapping, the chunk and the range.
            const u64 bytes = std::min<u64>({mapping_end - address, chunk->phys + chunk->cb_chunk - phys,
                                             (end - block) << block_shift});
            run = bytes >> block_shift;
            // The run ends where data may keep a VRAM copy.
            if (!any_type && GarlicInVram()) {
                for (u64 i = 1; i < run; ++i) {
                    if (VramEligible(block + i)) {
                        run = i;
                        break;
                    }
                }
            }
        }
        if (run == 0) {
            rest.Add({block, block + 1});
            vram_bytes.fetch_add(block_size, std::memory_order_relaxed);
            ++block;
            continue;
        }
        out.push_back({block, block + run, chunk->memory, phys - chunk->phys});
        in_place_blocks.Add({block, block + run});
        in_place_bytes.fetch_add(run << block_shift, std::memory_order_relaxed);
        block += run;
    }
    if (const u64 n = reports.fetch_add(1, std::memory_order_relaxed); (n & (n + 1)) == 0) {
        std::printf("Guest memory: %llu MiB of the arena bound in place, %llu MiB in VRAM "
                    "(Garlic, not direct memory in a guest chunk, or not block-aligned)\n",
                    (unsigned long long)(in_place_bytes.load() >> 20),
                    (unsigned long long)(vram_bytes.load() >> 20));
    }
}

void BufferCache::NoteCpuWrite(VAddr address, u64 guest_rip) {
    constexpr u32 Frames = 3, Window = 60;
    // Guest code the game's profile knows to write per-frame data: what it writes is an upload
    // heap at its first write (others after Frames writes in Window frames).
    const bool dynamic_writer = Game::IsDynamicWriter(guest_rip);
    const u64 block = address >> block_shift;
    const u32 frame = static_cast<u32>(PsStats::g_frame_number.load(std::memory_order_relaxed));
    std::scoped_lock lk{dynamic_mutex};
    auto& [last, frames] = cpu_write_frames[block];
    if (dynamic_writer && frames < Frames) {
        frames = Frames;
        last = frame;
        demote_requests.push_back(block);
        demotes_requested.store(true, std::memory_order_release);
        return;
    }
    if (last == frame && frames != 0) {
        return;
    }
    frames = frame - last > Window ? 1 : frames + 1;
    last = frame;
    if (frames == Frames) {
        demote_requests.push_back(block);
        demotes_requested.store(true, std::memory_order_release);
    }
}

void BufferCache::ProcessDemotions() {
    // psport: the waiting requests become ready only once their tick is submitted: rescanned
    // when a new tick began or a request came (it ran for every draw and dispatch).
    const bool requested = demotes_requested.load(std::memory_order_acquire);
    if (!requested && demotions_scanned_tick == scheduler.CurrentTick()) {
        return;
    }
    demotions_scanned_tick = scheduler.CurrentTick();
    if (requested) {
        std::scoped_lock lk{dynamic_mutex};
        for (const u64 block : demote_requests) {
            demotions_waiting.push_back({block, scheduler.CurrentTick(), true});
        }
        demote_requests.clear();
        demotes_requested.store(false, std::memory_order_release);
    }
    if (demotions_waiting.empty()) {
        return;
    }
    static std::atomic<u64> moved_bytes{0}, kept{0};
    const u64 submitted_below = scheduler.CurrentTick(); // ticks under it are submitted
    std::erase_if(demotions_waiting, [&](const Demotion& request) {
        const auto [block, tick, cpu_written, idle, force] = request;
        if (tick >= submitted_below) {
            return false; // its uploads are still being recorded
        }
        const VAddr address = block << block_shift;
        if (cpu_written) {
            dynamic_blocks.Add({block, block + 1}); // bound in place when it is next used
            if (LayerMode()) {
                layer_cpu_hot.Add({block, block + 1}); // the CPU's, not the GPU's: may be watched
            }
        }
        if (LayerMode()) {
            // Its mirror's copy goes (written by code we do not hear of, or handed out again
            // and not reloaded): in place from now on.
            if (!cpu_written && !idle && VramEligible(block)) {
                return true; // loaded again since
            }
            LayerDemote(block, block + 1, true, 3);
            return true;
        }
        if (in_place_blocks.Contains(block) || !resident_ranges.Contains(block)) {
            return true;
        }
        if (idle) {
            if (!force &&
                PsStats::g_coarse_second.load(std::memory_order_relaxed) -
                        group_use[address >> USE_GROUP_BITS] <
                    VramIdleSeconds()) {
                return true; // bound again since
            }
        } else if (!cpu_written && VramEligible(block)) {
            return true; // written by a loader since it was handed out again
        }
        // Data only the GPU has (in the VRAM copy) would be lost: such blocks stay.
        if (IsRegionGpuModified(address, block_size) ||
            gpu_modified_ranges.Intersects(address, block_size)) {
            kept.fetch_add(1, std::memory_order_relaxed);
            if (force) {
                PsStats::g_evac_kept_gpu.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        u64 phys = 0;
        uintptr_t mapping_end = 0;
        const PsGuestMemory::Chunk* chunk = nullptr;
        if (!runtime_memory_direct_phys(address, &phys, &mapping_end) ||
            (phys & (block_size - 1)) != 0 || mapping_end - address < block_size ||
            (chunk = PsGuestMemory::Find(phys)) == nullptr) {
            if (force) {
                PsStats::g_evac_kept_other.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        if (!address_space[block >> blocks_per_arena_page_shift]) {
            return true;
        }
        Backing backing;
        backing.start = block;
        backing.end = block + 1;
        backing.memory = chunk->memory;
        backing.offset = (phys - chunk->phys) >> block_shift;
        // Its VRAM slot is reused by a later move to VRAM (bound once this rebind is done).
        if (const auto old = resident_ranges.Find(block); old != resident_ranges.end()) {
            ReleaseResidencySlot(old->memory, (old->offset + (block - old->start)) << block_shift);
        }
        resident_ranges.Subtract(block, block + 1);
        resident_ranges.Add(backing);
        in_place_blocks.Add({block, block + 1});
        // Every arena over it: one replaced by a wider one (GetArena) still serves the shaders
        // whose device addresses (the BDA page table) were taken from it.
        for (const auto& arena : arenas) {
            if (arena.cpu_addr <= address && address - arena.cpu_addr < arena.size_bytes) {
                BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                    .resourceOffset = address - arena.cpu_addr,
                    .size = block_size,
                    .memory = chunk->memory,
                    .memoryOffset = backing.offset << block_shift,
                });
            }
        }
        // Its uploads up to `tick` land in the VRAM copy first; the rebind waits for them.
        bind_wait_tick = std::max(bind_wait_tick, tick);
        // Never uploaded or protected again: writes go to the memory the GPU reads.
        memory_tracker->MarkRegionAsCpuModified(address, block_size);
        if (idle) {
            PsStats::g_cb_vram_idle.fetch_add(block_size, std::memory_order_relaxed);
            idle_demoted.Add({block, block + 1});
            group_demoted[address >> USE_GROUP_BITS] = 1;
        }
        const u64 total = moved_bytes.fetch_add(block_size, std::memory_order_relaxed) + block_size;
        if (!idle && (total >> 20) != ((total - block_size) >> 20)) {
            std::printf("Guest memory: %llu MiB of CPU-written blocks moved in place from VRAM, "
                        "%llu blocks kept (GPU-written)\n",
                        (unsigned long long)(total >> 20), (unsigned long long)kept.load());
        }
        return true;
    });
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::GuestChunkSource(VAddr address, u64 size) {
    u64 phys = 0;
    uintptr_t mapping_end = 0;
    const PsGuestMemory::Chunk* chunk = nullptr;
    if (size == 0 || !runtime_memory_direct_phys(address, &phys, &mapping_end) ||
        address + size > mapping_end || (chunk = PsGuestMemory::Find(phys)) == nullptr ||
        phys + size > chunk->phys + chunk->cb_chunk) {
        return std::nullopt;
    }
    const Buffer* p_buffer = ChunkBuffer(*chunk);
    if (!p_buffer) {
        return std::nullopt; // the arena serves the range
    }
    return std::pair<const Buffer*, u64>{p_buffer, phys - chunk->phys};
}

const Buffer* BufferCache::ChunkBuffer(const PsGuestMemory::Chunk& chunk) {
    const size_t i_slot = chunk.i_creation % chunk_buffers.size();
    auto& buffer = chunk_buffers[i_slot];
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (!buffer || (!buffer->IsAllocated() && chunk_buffer_failed_at[i_slot] != now)) {
        buffer = std::make_unique<Buffer>(instance, chunk.cb_chunk, chunk.memory,
                                          fmt::format("psport guest memory {:#x}", chunk.phys));
        chunk_buffer_failed_at[i_slot] = now;
    }
    if (!buffer->IsAllocated()) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Guest memory chunk {:#x} has no buffer: not bound in place",
                          chunk.phys);
        return nullptr;
    }
    return buffer.get();
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::LayerInPlace(VAddr address, u64 size,
                                                                      bool clamp_huge) {
    auto span = PsLayer::GpuMemory::Get().ResolvePrefix(address, size);
    if (!span) {
        return std::nullopt;
    }
    if (span->cb_span != size) {
        // Shaders given (nearly) all memory as one buffer: the part the module has whole. Until
        // the paged path (MEMORY_MODULE_PLAN step 3) a range across chunks is not one buffer.
        if (!clamp_huge || size < 64_MB) {
            return std::nullopt;
        }
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("Layer memory: binding %#llx+%#llx cut to %#llx (the rest is in another "
                        "chunk)\n",
                        (unsigned long long)address, (unsigned long long)size,
                        (unsigned long long)span->cb_span);
        }
    }
    const auto* chunk = static_cast<const PsGuestMemory::Chunk*>(span->p_source->p_owner);
    const Buffer* p_buffer = ChunkBuffer(*chunk);
    if (!p_buffer) {
        return std::nullopt;
    }
    return std::pair<const Buffer*, u64>{p_buffer, span->off_source};
}

void BufferCache::NoteLayerMiss(VAddr address, u64 size, bool is_written) {
    // Per 2 s: bindings not whole in one chunk (served by the arena: VRAM copies).
    static std::atomic<u64> misses{0}, bytes{0}, written{0};
    static std::atomic<u32> printed_second{0};
    misses.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
    if (is_written) {
        written.fetch_add(1, std::memory_order_relaxed);
    }
    static std::atomic<u32> examples{0};
    if (examples.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::printf("Layer memory: %#llx+%#llx%s not whole in one guest chunk\n",
                    (unsigned long long)address, (unsigned long long)size,
                    is_written ? " (written)" : "");
    }
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (now - printed_second.load(std::memory_order_relaxed) >= 2) {
        printed_second.store(now, std::memory_order_relaxed);
        std::printf("Layer memory (2 s): %llu bindings, %llu MiB not whole in one guest chunk "
                    "(%llu written)\n",
                    (unsigned long long)misses.exchange(0), (unsigned long long)(bytes.exchange(0) >> 20),
                    (unsigned long long)written.exchange(0));
    }
}

void BufferCache::NoteAssetWrite(VAddr addr, u64 size) {
    if (size == 0) {
        return;
    }
    std::scoped_lock lk{pending_assets_mutex};
    pending_assets.push_back({addr, size, false});
    assets_pending.store(true, std::memory_order_release);
}

void BufferCache::NoteFreshRange(VAddr addr, u64 size) {
    PsStats::g_c_gpu_range_allocs.fetch_add(1, std::memory_order_relaxed);
    PsStats::g_cb_gpu_range_alloc.fetch_add(size, std::memory_order_relaxed);
    // The GPU's old data there is dead: whole pages inside stop being GPU-modified now, so the
    // writes that follow (announced after the fact) read none of it back over themselves.
    const VAddr pages_start = Common::AlignUp(addr, u64(4096));
    const VAddr pages_end = Common::AlignDown(addr + size, u64(4096));
    if (pages_start < pages_end) {
        memory_tracker->UnmarkRegionAsGpuModified(pages_start, pages_end - pages_start);
    }
    std::scoped_lock lk{pending_assets_mutex};
    pending_assets.push_back({addr, size, true});
    assets_pending.store(true, std::memory_order_release);
}

void BufferCache::NoteCpuWriteRange(VAddr addr, u64 size) {
    if (size == 0) {
        return;
    }
    for (VAddr block = addr >> block_shift; block <= (addr + size - 1) >> block_shift; ++block) {
        NoteCpuWrite(block << block_shift, 0);
    }
}

void BufferCache::ProcessPendingAssets() {
    if (!assets_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<PendingRange> ranges;
    {
        std::scoped_lock lk{pending_assets_mutex};
        ranges.swap(pending_assets);
        assets_pending.store(false, std::memory_order_release);
    }
    for (const auto& [addr, size, fresh] : ranges) {
        const u64 first = addr >> block_shift, end = (addr + size + block_size - 1) >> block_shift;
        if (fresh) {
            // Its old contents are dead and the next writer may be code we do not hear of: in
            // place until a loader writes it. Until the move, uploads keep the VRAM copy current.
            for (auto& [key, write] : large_writes) {
                if (addr < write.addr + write.size && write.addr < addr + size) {
                    ++write.fresh;
                }
            }
            asset_bytes.Subtract(addr, addr + size);
            gpu_written_bytes.Subtract(addr, addr + size);
            promote_candidates.Subtract(first, end);
            // What the GPU wrote there before is dead too: never copied back over what the new
            // owner writes (only the GPU's data beside it is, QueueCopyBacks/LayerDemote).
            gpu_modified_ranges.Subtract(addr, size);
            memory_tracker->UnmarkRegionAsGpuModified(addr, size);
            if (LayerMode()) {
                // Blocks whole in it start over: the GPU may keep its data there in VRAM again (CPU
                // writes still queued mark them again, LayerForgetGpuData).
                const u64 whole_first = (addr + block_size - 1) >> block_shift;
                const u64 whole_end = (addr + size) >> block_shift;
                if (whole_first < whole_end) {
                    layer_cpu_blocks.Subtract(whole_first, whole_end);
                }
            }
            for (u64 block = first; block < end; ++block) {
                const VAddr block_addr = block << block_shift;
                const VAddr from = std::max<VAddr>(addr, block_addr);
                const VAddr to = std::min<VAddr>(addr + size, block_addr + block_size);
                const bool in_vram =
                    LayerMode() ? PsLayer::GpuMemory::Get().AnyValid(block, block + 1)
                                : resident_ranges.Contains(block) && !in_place_blocks.Contains(block);
                if (in_vram) {
                    if (memory_tracker->IsRegionGpuModified(block_addr, block_size) ||
                        gpu_modified_ranges.Intersects(block_addr, block_size)) {
                        // The GPU's data beside it is only in VRAM: it goes back into the game's
                        // memory as the submission ends, then the block moves (QueueCopyBacks).
                        // An upload now would put stale bytes over that data (whole pages).
                        copy_back_blocks.Add({block, block + 1});
                        continue;
                    }
                    demotions_waiting.push_back({block, scheduler.CurrentTick(), false});
                }
                memory_tracker->MarkRegionAsCpuModified(from, to - from);
            }
            continue;
        }
        asset_bytes.Add({addr, addr + size});
        // psport: a block the GPU once wrote in place (dynamic for good) that the loaders have now
        // written whole holds only the CPU's data, so it may have a VRAM copy again. Kept, such
        // marks piled up as areas loaded over memory the previous area's GPU buffers had used
        // (164 -> 1117 MiB after a few lamp travels), and one such block kept a whole range in
        // place (FPS -28 %). GPU writes there stay in place (layer_cpu_blocks) so that the CPU
        // never reads behind a VRAM copy; blocks the CPU writes often stay dynamic.
        // PS_LAYER_RELOAD_RESET=0: as before.
        static const bool s_should_reset_reloaded = [] {
            const char* sz_env = std::getenv("PS_LAYER_RELOAD_RESET");
            return !(sz_env && sz_env[0] == '0');
        }();
        if (LayerMode() && s_should_reset_reloaded) {
            for (u64 block = first; block < end; ++block) {
                if (!dynamic_blocks.Contains(block) || layer_cpu_hot.Contains(block, block + 1)) {
                    continue;
                }
                bool is_whole = true;
                asset_bytes.ForEachGap(block << block_shift, (block + 1) << block_shift,
                                       [&](u64, u64) { is_whole = false; });
                if (!is_whole) {
                    continue;
                }
                layer_cpu_blocks.Add({block, block + 1});
                dynamic_blocks.Subtract(block, block + 1);
                layer_write_bounces.Subtract(block, block + 1);
                PsStats::g_cb_dynamic_reloaded.fetch_add(block_size, std::memory_order_relaxed);
            }
        }
        for (u64 block = first; block < end; ++block) {
            const bool in_place =
                LayerMode() ? !PsLayer::GpuMemory::Get().AnyValid(block, block + 1)
                            : in_place_blocks.Contains(block) && resident_ranges.Contains(block);
            if (in_place && VramEligible(block)) {
                promote_candidates.Add({block, block + 1});
            }
        }
    }
}

void BufferCache::NoteLateCommandWrite(VAddr addr, const void* data, u64 size) {
    if (size == 0) {
        return;
    }
    const auto* bytes = static_cast<const u8*>(data);
    std::scoped_lock lk{late_writes_mutex};
    late_writes.emplace_back(addr, std::vector<u8>(bytes, bytes + size));
    late_writes_pending.store(true, std::memory_order_release);
}

void BufferCache::TraceBinding(VAddr addr, u64 size, bool is_written, int kind) {
    static const u64 trace_size = [] {
        const char* env = std::getenv("PS_TRACE_SIZE");
        return env ? std::strtoull(env, nullptr, 0) : u64(0);
    }();
    // PS_TRACE_ADDR=addr,size: that range is traced from the start (with PS_TRACE_SIZE set).
    static const bool fixed_range = [this] {
        const char* env = std::getenv("PS_TRACE_ADDR");
        if (!env) {
            return false;
        }
        char* end = nullptr;
        const u64 start = std::strtoull(env, &end, 0);
        traced_ranges.emplace_back(start, end && *end == ',' ? std::strtoull(end + 1, nullptr, 0) : 4096);
        return true;
    }();
    if (trace_size == 0) {
        return;
    }
    if (is_written && size == trace_size &&
        std::ranges::find(traced_ranges, std::pair<VAddr, u64>{addr, size}) == traced_ranges.end()) {
        if (traced_ranges.size() >= 16) {
            traced_ranges.erase(traced_ranges.begin() + (fixed_range ? 1 : 0));
        }
        traced_ranges.emplace_back(addr, size);
    }
    const bool traced = std::ranges::any_of(traced_ranges, [&](const auto& range) {
        return addr < range.first + range.second && range.first < addr + size;
    });
    if (traced) {
        ++trace_counts[kind];
        if (!is_written) {
            trace_min = std::min(trace_min, size);
            trace_max = std::max(trace_max, size);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - trace_report < std::chrono::seconds(5)) {
        return;
    }
    trace_report = now;
    std::printf("Trace %#llx-byte GPU buffers (%zu, 5 s): write in place %llu, write VRAM %llu; read "
                "in place %llu, snapshot %llu, VRAM %llu (sizes %llu-%llu); image source: in place "
                "%llu, chunk %llu, GPU data %llu, staging %llu\n",
                (unsigned long long)trace_size, traced_ranges.size(),
                (unsigned long long)trace_counts[0], (unsigned long long)trace_counts[1],
                (unsigned long long)trace_counts[2], (unsigned long long)trace_counts[3],
                (unsigned long long)trace_counts[4], (unsigned long long)(trace_max ? trace_min : 0),
                (unsigned long long)trace_max, (unsigned long long)trace_counts[5],
                (unsigned long long)trace_counts[6], (unsigned long long)trace_counts[7],
                (unsigned long long)trace_counts[9]);
    trace_counts = {};
    trace_min = ~0ull;
    trace_max = 0;
}

void BufferCache::NoteLargeGpuWrite(VAddr addr, u64 size) {
    auto& write = large_writes[addr ^ (size << 40)];
    if (write.count++ == 0) {
        write.addr = addr;
        write.size = size;
        write.images = texture_cache.DescribeImagesIn(addr, size);
    }
    write.in_place = IsInPlace(addr, size);
    const auto now = std::chrono::steady_clock::now();
    if (now - large_writes_report < std::chrono::seconds(5)) {
        return;
    }
    large_writes_report = now;
    std::vector<const LargeWrite*> rows;
    for (const auto& [key, entry] : large_writes) {
        rows.push_back(&entry);
    }
    std::ranges::sort(rows, [](const auto* a, const auto* b) { return a->count > b->count; });
    std::printf("GPU-written over 64 KiB (5 s):");
    for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 12); ++i) {
        std::printf("%s %#llx+%#llx x%llu %s fresh %llu%s", i ? ";" : "",
                    (unsigned long long)rows[i]->addr, (unsigned long long)rows[i]->size,
                    (unsigned long long)rows[i]->count, rows[i]->in_place ? "in-place" : "VRAM",
                    (unsigned long long)rows[i]->fresh, rows[i]->images.c_str());
    }
    std::printf("\n");
    large_writes.clear();
}

void BufferCache::NotePreciseUpload(VAddr addr, u64 size) {
    if (size == 0) {
        return;
    }
    if (LayerMode()) {
        LayerNoteCpuWrite(addr, size);
    }
    std::scoped_lock lk{late_writes_mutex};
    precise_uploads.emplace_back(addr, size);
    late_writes_pending.store(true, std::memory_order_release);
}

void BufferCache::ProcessLateWrites() {
    if (!late_writes_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> uploads;
    std::vector<std::pair<VAddr, std::vector<u8>>> writes;
    {
        std::scoped_lock lk{late_writes_mutex};
        writes.swap(late_writes);
        uploads.swap(precise_uploads);
        late_writes_pending.store(false, std::memory_order_release);
    }
    for (const auto& [addr, bytes] : writes) {
        const u64 size = bytes.size();
        const u64 first = addr >> block_shift, last = (addr + size - 1) >> block_shift;
        if (((addr | size) & 3) != 0 || !resident_ranges.Contains(first, last + 1) ||
            in_place_blocks.Overlaps(first, last + 1)) {
            continue; // the GPU reads the game's memory there (or nothing yet)
        }
        const auto* arena = GetArena(first, last);
        if (!arena) {
            continue;
        }
        runtime.UpdateBuffer(arena, arena->Offset(addr), bytes);
        PsStats::g_c_late_vram_writes.fetch_add(1, std::memory_order_relaxed);
    }
    // CPU bytes next to GPU data in VRAM: copied by the GPU from the game's memory, block by
    // block (a VRAM block's guest memory is one contiguous run of a chunk).
    for (const auto& [addr, size] : uploads) {
        const VAddr end = addr + size;
        for (VAddr at = addr; at < end;) {
            const u64 block = at >> block_shift;
            const VAddr to = std::min<VAddr>(end, (block + 1) << block_shift);
            u64 phys = 0;
            uintptr_t mapping_end = 0;
            const PsGuestMemory::Chunk* chunk = nullptr;
            if (resident_ranges.Contains(block) && !in_place_blocks.Contains(block) &&
                runtime_memory_direct_phys(at, &phys, &mapping_end) && to <= mapping_end &&
                (chunk = PsGuestMemory::Find(phys)) != nullptr &&
                phys + (to - at) <= chunk->phys + chunk->cb_chunk) {
                const auto* arena = GetArena(block, block);
                if (!arena) {
                    at = to;
                    continue;
                }
                const vk::BufferCopy copy{phys - chunk->phys, at - arena->cpu_addr, to - at};
                runtime.CopyFromGuestChunk(chunk->buffer, arena, std::span{&copy, 1});
                PsStats::g_c_late_vram_writes.fetch_add(1, std::memory_order_relaxed);
            }
            at = to;
        }
    }
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::ShadowCopy(VAddr addr, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("PS_SHADOWS");
        return env && env[0] == '1';
    }();
    constexpr u64 MaxShadow = 4_MB, ShadowRing = 128_MB;
    // A GPU write there not done yet: the copy (through the chunk's own buffer) would not wait
    // for it; the arena is read instead.
    if (!enabled || !GarlicInVram() || size > MaxShadow || IsRegionGpuModified(addr, size)) {
        return std::nullopt;
    }
    if (const u64 tick = scheduler.CurrentTick(); tick != shadow_tick) {
        shadows.clear();
        shadow_tick = tick;
    }
    for (const auto& shadow : shadows) {
        if (shadow.addr == addr && shadow.size == size) {
            return std::pair<const Buffer*, u64>{shadow_buffer.get(), shadow.offset};
        }
    }
    const auto source = GuestChunkSource(addr, size);
    if (!source) {
        return std::nullopt;
    }
    if (!shadow_buffer) {
        shadow_buffer = std::make_unique<StreamBuffer>(instance, scheduler, MemoryType::DeviceLocal,
                                                       ShadowRing);
    }
    const auto offset = shadow_buffer->Reserve(size, instance.StorageMinAlignment());
    if (!offset) {
        return std::nullopt;
    }
    const vk::BufferCopy copy{source->second, *offset, size};
    runtime.CopyBuffer(source->first, shadow_buffer.get(), std::span{&copy, 1});
    shadows.push_back({addr, size, *offset});
    PsStats::g_c_shadow_copies.fetch_add(1, std::memory_order_relaxed);
    PsStats::g_cb_shadow.fetch_add(size, std::memory_order_relaxed);
    return std::pair<const Buffer*, u64>{shadow_buffer.get(), *offset};
}

void BufferCache::DropShadows(VAddr addr, u64 size) {
    std::erase_if(shadows, [&](const Shadow& shadow) {
        return addr < shadow.addr + shadow.size && shadow.addr < addr + size;
    });
}

bool BufferCache::UpdateGpuWritten(VAddr addr, std::span<const u8> data) {
    const u64 size = data.size();
    if (size == 0 || ((addr | size) & 3) != 0 || IsAnyInPlace(addr, size) ||
        !memory_tracker->IsRegionGpuModified(addr, size)) {
        return false;
    }
    const u64 first = addr >> block_shift, last = (addr + size - 1) >> block_shift;
    if (!resident_ranges.Contains(first, last + 1)) {
        return false;
    }
    const auto* arena = GetArena(first, last);
    if (!arena) {
        return false;
    }
    runtime.UpdateBuffer(arena, arena->Offset(addr), data);
    PsStats::g_c_gpu_data_updates.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BufferCache::NoteGpuWrite(VAddr addr, u64 size) {
    if (size == 0 || gpu_written_bytes.Contains(addr, addr + size)) {
        return;
    }
    // Only what the GPU writes regularly (in 3 frames of 60) is its data, like a default heap: a
    // buffer the GPU writes only now and then stays in place, where writes by the CPU are seen
    // (0x104ff30000, 192 KiB, written by the GPU every few seconds: in VRAM the picture smeared
    // while moving).
    constexpr u32 Frames = 3, Window = 60;
    const u32 frame = static_cast<u32>(PsStats::g_frame_number.load(std::memory_order_relaxed));
    auto& [last, frames] = gpu_write_frames[addr ^ (size << 40)];
    if (last != frame || frames == 0) {
        frames = frame - last > Window ? 1 : frames + 1;
        last = frame;
    }
    if (frames < Frames) {
        if (gpu_write_frames.size() > 65536) {
            std::erase_if(gpu_write_frames,
                          [frame](const auto& entry) { return frame - entry.second.first > Window; });
        }
        return;
    }
    gpu_written_bytes.Add({addr, addr + size});
    // Blocks the GPU already writes in place move to VRAM once they are all known data.
    for (u64 block = addr >> block_shift; block <= (addr + size - 1) >> block_shift; ++block) {
        const bool in_place =
            LayerMode() ? !PsLayer::GpuMemory::Get().AnyValid(block, block + 1)
                        : in_place_blocks.Contains(block) && resident_ranges.Contains(block);
        if (in_place && VramEligible(block)) {
            promote_candidates.Add({block, block + 1});
        }
    }
}

void BufferCache::QueueCopyBacks(u64 submitted) {
    if (copy_back_blocks.Empty()) {
        return;
    }
    if (LayerMode()) {
        // GPU data in mirrors back into the game's memory, then in place. A run whose copy back
        // cannot be made is queued again by LayerDemote.
        const auto pending = std::exchange(copy_back_blocks, {});
        for (const auto& range : pending) {
            LayerDemote(range.start, range.end, true, 4);
        }
        return;
    }
    std::vector<u64> blocks;
    for (const auto& range : copy_back_blocks) {
        for (u64 block = range.start; block < range.end; ++block) {
            blocks.push_back(block);
        }
    }
    copy_back_blocks.Clear();
    bool moved = false;
    for (const u64 block : blocks) {
        const VAddr address = block << block_shift;
        u64 phys = 0;
        uintptr_t mapping_end = 0;
        const PsGuestMemory::Chunk* chunk = nullptr;
        if (!resident_ranges.Contains(block) || in_place_blocks.Contains(block) ||
            !runtime_memory_direct_phys(address, &phys, &mapping_end) ||
            (phys & (block_size - 1)) != 0 || mapping_end - address < block_size ||
            (chunk = PsGuestMemory::Find(phys)) == nullptr ||
            !address_space[block >> blocks_per_arena_page_shift]) {
            continue;
        }
        const auto source = GuestChunkSource(address, block_size);
        if (!source) {
            continue;
        }
        // What the GPU wrote there since the block was handed out (not the bytes handed out
        // again: the CPU may be writing them now), copied at the end of this submission.
        const auto* arena = GetArena(block, block);
        if (!arena) {
            continue;
        }
        boost::container::small_vector<vk::BufferCopy, 8> copies;
        gpu_written_bytes.ForEachInRange(address, address + block_size, [&](const Interval& iv) {
            const VAddr from = std::max<VAddr>(iv.start, address);
            const VAddr to = std::min<VAddr>(iv.end, address + block_size);
            if (from < to) {
                copies.push_back({arena->Offset(from), source->second + (from - address), to - from});
            }
        });
        if (!copies.empty()) {
            runtime.CopyBuffer(arena, source->first, copies);
        }
        // Then in place from the next submission on (as ProcessDemotions).
        if (const auto old = resident_ranges.Find(block); old != resident_ranges.end()) {
            ReleaseResidencySlot(old->memory, (old->offset + (block - old->start)) << block_shift);
        }
        Backing backing;
        backing.start = block;
        backing.end = block + 1;
        backing.memory = chunk->memory;
        backing.offset = (phys - chunk->phys) >> block_shift;
        resident_ranges.Subtract(block, block + 1);
        resident_ranges.Add(backing);
        in_place_blocks.Add({block, block + 1});
        for (const auto& covering : arenas) {
            if (covering.cpu_addr <= address && address - covering.cpu_addr < covering.size_bytes) {
                BindsForArena(&covering)->binds.emplace_back(vk::SparseMemoryBind{
                    .resourceOffset = address - covering.cpu_addr,
                    .size = block_size,
                    .memory = chunk->memory,
                    .memoryOffset = backing.offset << block_shift,
                });
            }
        }
        memory_tracker->UnmarkRegionAsGpuModified(address, block_size);
        gpu_modified_ranges.Subtract(address, block_size);
        memory_tracker->MarkRegionAsCpuModified(address, block_size);
        PsStats::g_c_copied_back_blocks.fetch_add(1, std::memory_order_relaxed);
        moved = true;
    }
    if (moved) {
        bind_wait_tick = std::max(bind_wait_tick, submitted);
    }
}

namespace {
// psport PS_VRAM_ACCESS_TRAP=1 (diagnostics): blocks moved to VRAM are closed to the CPU
// (PS_HOST_VM_NONE); the first CPU access to each is recorded (code site, read or write) and the block
// opened again. Finds CPU readers of data the GPU writes in VRAM, where the CPU sees a stale copy.
struct VramTrap {
    std::atomic<u64> address{0}, rip{0}, caller{0};
    std::atomic<bool> write{false};
};
std::array<VramTrap, 256> vram_traps;
std::atomic<u32> vram_trap_count{0}, vram_trap_reported{0};
std::mutex vram_trapped_mutex;
std::unordered_map<u64, u64> vram_trapped; // block address -> block size
u64 vram_trap_block_size = 0;
// PS_VRAM_TRAP_RANGE=addr,size: only blocks over that range, re-armed for every packet, every
// access counted by site.
std::pair<u64, u64> VramTrapRange() {
    static const std::pair<u64, u64> range = [] {
        const char* env = std::getenv("PS_VRAM_TRAP_RANGE");
        if (!env) {
            return std::pair<u64, u64>{0, 0};
        }
        char* end = nullptr;
        const u64 addr = std::strtoull(env, &end, 0);
        return std::pair<u64, u64>{addr, end && *end == ',' ? std::strtoull(end + 1, nullptr, 0) : 0};
    }();
    return range;
}
struct SiteCount {
    std::atomic<u64> key{0}, count{0}, caller{0};
};
std::array<SiteCount, 128> vram_trap_sites;
std::vector<u64> vram_rearm;

bool VramTrapsEnabled() {
    static const bool enabled = std::getenv("PS_VRAM_ACCESS_TRAP") != nullptr;
    return enabled;
}

bool VramTrapHandler(void* context, void* fault_address) {
    const u64 address = reinterpret_cast<u64>(fault_address);
    const u64 block = vram_trap_block_size ? address & ~(vram_trap_block_size - 1) : 0;
    {
        std::scoped_lock lk{vram_trapped_mutex};
        if (!vram_trapped.erase(block)) {
            return false;
        }
    }
    const auto* p_context = static_cast<const PsHostFaultContext*>(context);
    const u64 rip = ps_host_fault_register(p_context, PS_HOST_REG_RIP);
    constexpr u64 Image = 0x800000000ull, ImageEnd = 0x810000000ull;
    u64 caller = 0;
    const auto* stack = reinterpret_cast<const u64*>(ps_host_fault_register(p_context, PS_HOST_REG_RSP));
    for (u32 i = 0; i < 48; ++i) {
        if (stack[i] >= Image && stack[i] < ImageEnd) {
            caller = stack[i] - Image;
            break;
        }
    }
    const bool write = Common::IsWriteError(context);
    const u64 site = (rip >= Image && rip < ImageEnd ? rip - Image : rip | (1ull << 63)) ^
                     (write ? 1ull << 62 : 0);
    if (VramTrapRange().second) {
        // Counted by site; the block is closed again at the next packet.
        for (u32 i = 0, slot = u32((site * 0x9E3779B97F4A7C15ull) >> 57); i < vram_trap_sites.size();
             ++i) {
            auto& entry = vram_trap_sites[(slot + i) % vram_trap_sites.size()];
            u64 expected = 0;
            if (entry.key.load() == site || entry.key.compare_exchange_strong(expected, site)) {
                entry.count.fetch_add(1);
                entry.caller = caller;
                break;
            }
        }
        std::scoped_lock lk{vram_trapped_mutex};
        vram_rearm.push_back(block);
    } else {
        const u32 slot = vram_trap_count.fetch_add(1);
        if (slot < vram_traps.size()) {
            auto& trap = vram_traps[slot];
            trap.rip = site & ~(1ull << 62);
            trap.caller = caller;
            trap.write = write;
            trap.address = address;
        }
    }
    ps_host_vm_protect(reinterpret_cast<void*>(block), vram_trap_block_size, PS_HOST_VM_READ | PS_HOST_VM_WRITE);
    return true;
}

std::string VramTrapSiteName(u64 rip) {
    if (!(rip >> 63)) {
        return fmt::format("+{:#x}", rip);
    }
    PsHostCodeAddress where;
    const u64 address = rip & ~(1ull << 63);
    ps_host_dl_address(reinterpret_cast<void*>(address), &where);
    const char* name = where.sz_file ? std::strrchr(where.sz_file, '/') : nullptr;
    return fmt::format("host {}+{:#x}", name ? name + 1 : "?", address - where.va_base);
}

void ReportVramTraps() {
    if (VramTrapRange().second) {
        // Re-arm the blocks touched since the last packet.
        std::vector<u64> blocks;
        {
            std::scoped_lock lk{vram_trapped_mutex};
            blocks.swap(vram_rearm);
            for (const u64 block : blocks) {
                vram_trapped[block] = vram_trap_block_size;
            }
        }
        for (const u64 block : blocks) {
            ps_host_vm_protect(reinterpret_cast<void*>(block), vram_trap_block_size, PS_HOST_VM_NONE);
        }
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last < std::chrono::seconds(3)) {
            return;
        }
        last = std::chrono::steady_clock::now();
        std::string line;
        for (auto& entry : vram_trap_sites) {
            const u64 key = entry.key.load(), count = entry.count.exchange(0);
            if (key && count) {
                line += fmt::format(" {} {} x{} (guest caller +{:#x});",
                                    key & (1ull << 62) ? "write" : "read",
                                    VramTrapSiteName(key & ~(1ull << 62)), count,
                                    entry.caller.load());
            }
        }
        std::printf("VRAM trap sites (3 s):%s\n", line.c_str());
        return;
    }
    const u32 count = std::min<u32>(vram_trap_count.load(), vram_traps.size());
    for (u32 i = vram_trap_reported.load(); i < count; ++i) {
        const auto& trap = vram_traps[i];
        std::printf("VRAM trap: CPU %s at %#llx by %s (guest caller +%#llx)\n",
                    trap.write ? "write" : "read", (unsigned long long)trap.address.load(),
                    VramTrapSiteName(trap.rip.load()).c_str(),
                    (unsigned long long)trap.caller.load());
    }
    vram_trap_reported = count;
}
} // namespace

void BufferCache::ReportInPlaceBlocks() {
    // PS_RESIDENCY_REPORT=<file>: when the file appears (checked once a second), the blocks bound
    // in place that were used in the last seconds, and why they have no VRAM copy.
    static const char* const trigger = std::getenv("PS_RESIDENCY_REPORT");
    static s64 checked = -1;
    const s64 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (!trigger || now == checked) {
        return;
    }
    checked = now;
    if (access(trigger, F_OK) != 0) {
        return;
    }
    unlink(trigger);
    u64 used = 0, idle = 0, dynamic = 0, unknown = 0, eligible = 0, candidates = 0, whole = 0;
    std::map<int, u64> unknown_by_type;
    std::vector<std::pair<u64, u64>> samples; // block address, unknown bytes
    for (const auto& range : in_place_blocks) {
        for (u64 block = range.start; block < range.end; ++block) {
            const VAddr address = block << block_shift;
            const u64 group = address >> USE_GROUP_BITS;
            if (group >= group_use.size() || now - s64(group_use[group]) > 2) {
                ++idle;
                continue;
            }
            ++used;
            if (dynamic_blocks.Contains(block)) {
                ++dynamic;
                continue;
            }
            u64 missing = 0;
            asset_bytes.ForEachGap(address, address + block_size, [&](u64 start, u64 end) {
                gpu_written_bytes.ForEachGap(start, end, [&](u64 s, u64 e) { missing += e - s; });
            });
            if (missing == 0) {
                ++eligible;
                candidates += promote_candidates.Contains(block);
                continue;
            }
            ++unknown;
            whole += missing == block_size;
            int prot = 0, type = -1;
            uintptr_t vma_end = 0;
            runtime_memory_vma_info(address, &prot, &type, &vma_end);
            ++unknown_by_type[type];
            if (samples.size() < 16) {
                samples.emplace_back(address, missing);
            }
        }
    }
    const auto mib = [&](u64 blocks) { return (unsigned long long)((blocks * block_size) >> 20); };
    std::printf("Residency report: blocks of %u KiB bound in place: %llu MiB used in the last 2 s, "
                "%llu MiB idle; of the used: %llu MiB dynamic (CPU-written), %llu MiB with bytes "
                "nobody announced (%llu MiB entirely), %llu MiB eligible (%llu MiB queued)\n",
                block_size >> 10, mib(used), mib(idle), mib(dynamic), mib(unknown), mib(whole),
                mib(eligible), mib(candidates));
    for (const auto& [type, count] : unknown_by_type) {
        std::printf("Residency report: unannounced bytes, memory type %d: %llu MiB\n", type,
                    mib(count));
    }
    for (const auto& [address, missing] : samples) {
        std::printf("Residency report:   block 0x%llx: %llu KiB unannounced\n",
                    (unsigned long long)address, (unsigned long long)(missing >> 10));
    }
}

void BufferCache::QueuePromotions(u64 submitted) {
    if (VramTrapsEnabled()) {
        ReportVramTraps();
    }
    // psport PS_LAYER_MEMORY: mirrors replaced by bigger ones go as soon as the GPU is past them,
    // not once a second (LayerMaintain): loading a level replaced hundreds within a second.
    if (LayerMode() && !layer_retired.empty()) {
        LayerReleaseRetired();
    }
    ReportInPlaceBlocks();
    if (promote_candidates.Empty()) {
        return;
    }
    if (VramPromotionsPaused()) {
        return;
    }
    std::vector<u64> blocks;
    for (const auto& range : promote_candidates) {
        for (u64 block = range.start; block < range.end; ++block) {
            blocks.push_back(block);
        }
    }
    promote_candidates.Clear();
    if (LayerMode()) {
        LayerPromote(blocks); // mirrors in VRAM (buffer_cache_layer.cpp)
        return;
    }
    u64 moved = 0;
    for (const u64 block : blocks) {
        const VAddr address = block << block_shift;
        // What the GPU wrote in place is in the game's memory: the copy is filled from there.
        if (!in_place_blocks.Contains(block) || !resident_ranges.Contains(block) ||
            !VramEligible(block)) {
            continue;
        }
        // Inside the submit callback (SubmitPendingArenaBinds): no waiting for memory.
        const auto [memory, offset] = AllocateResidency(block_size);
        if (!memory) {
            NoteResidencyShort(block_size);
            break;
        }
        memory_tracker->UnmarkRegionAsGpuModified(address, block_size);
        gpu_modified_ranges.Subtract(address, block_size);
        Backing backing;
        backing.start = block;
        backing.end = block + 1;
        backing.memory = memory;
        backing.offset = offset >> block_shift;
        resident_ranges.Subtract(block, block + 1);
        resident_ranges.Add(backing);
        in_place_blocks.Subtract(block, block + 1);
        // Every arena over it (as ProcessDemotions). The commands recorded so far read it in place;
        // the bind waits for them, and the next submission fills the VRAM copy before reading it.
        for (const auto& arena : arenas) {
            if (arena.cpu_addr <= address && address - arena.cpu_addr < arena.size_bytes) {
                BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                    .resourceOffset = address - arena.cpu_addr,
                    .size = block_size,
                    .memory = memory,
                    .memoryOffset = offset,
                });
            }
        }
        memory_tracker->MarkRegionAsCpuModified(address, block_size);
        moved += block_size;
        const auto trap_range = VramTrapRange();
        if (VramTrapsEnabled() &&
            (trap_range.second ? address < trap_range.first + trap_range.second &&
                                     trap_range.first < address + block_size
                               : gpu_written_bytes.Overlaps(address, address + block_size))) {
            static const bool registered = [] {
                Core::Signals::Instance()->RegisterAccessViolationHandler(VramTrapHandler, 0);
                return true;
            }();
            vram_trap_block_size = block_size;
            std::scoped_lock lk{vram_trapped_mutex};
            vram_trapped[address] = block_size;
            ps_host_vm_protect(reinterpret_cast<void*>(address), block_size, PS_HOST_VM_NONE);
        }
    }
    if (moved == 0) {
        return;
    }
    bind_wait_tick = std::max(bind_wait_tick, submitted);
    if (((promoted_bytes + moved) >> 20) != (promoted_bytes >> 20)) {
        std::printf("Guest memory: %llu MiB of loaded blocks moved from in place to VRAM\n",
                    (unsigned long long)((promoted_bytes + moved) >> 20));
    }
    promoted_bytes += moved;
}

void BufferCache::UnmapInPlace(VAddr addr, u64 size) {
    if (size == 0) {
        return;
    }
    std::scoped_lock lk{pending_unmaps_mutex};
    pending_unmaps.emplace_back(addr, size);
    unmaps_pending.store(true, std::memory_order_release);
}

void BufferCache::Maintain() {
    maintained_epoch = packet_epoch;
    if (VramTrapsEnabled()) {
        ReportVramTraps();
    }
    ProcessPendingUnmaps();
    ProcessIdleBlocks();
    ProcessDemotions();
    ProcessPendingAssets();
    ProcessLateWrites();
}

void BufferCache::ProcessPendingUnmaps() {
    if (!unmaps_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> unmaps;
    {
        std::scoped_lock lk{pending_unmaps_mutex};
        unmaps.swap(pending_unmaps);
        unmaps_pending.store(false, std::memory_order_release);
    }
    // The blocks bound to memory the game no longer maps there: unbound, and bound again (to
    // whatever is mapped then) when next used. Partly unmapped blocks go too.
    for (const auto& [addr, size] : unmaps) {
        const u64 first = addr >> block_shift, end = (addr + size + block_size - 1) >> block_shift;
        IntervalList<> unbind;
        if (LayerMode()) {
            LayerDemote(first, end, false, 5); // the memory there is gone: no copy back
            LayerRepublish(first, end);        // paged bindings read zero there now
            layer_published.Subtract(first, end);
            ++layer_generation; // the mapping changed: memoized resolutions (LayerBind) go
        }
        asset_bytes.Subtract(first << block_shift, end << block_shift);
        gpu_written_bytes.Subtract(first << block_shift, end << block_shift);
        // psport: blocks wholly unmapped hold nobody's data: whatever kept them in place (CPU
        // writes, GPU writes in place, a failed watch) is over, and memory mapped there again
        // starts as new. PS_LAYER_UNMAP_RESET=0: as before.
        static const bool s_should_reset_unmapped = [] {
            const char* sz_env = std::getenv("PS_LAYER_UNMAP_RESET");
            return !(sz_env && sz_env[0] == '0');
        }();
        if (const u64 whole_first = (addr + block_size - 1) >> block_shift, whole_end = (addr + size) >> block_shift;
            s_should_reset_unmapped && whole_first < whole_end) {
            static u64 s_cb_unmap_reset = 0;
            const u64 cb_reset = (whole_end - whole_first) << block_shift;
            if (((s_cb_unmap_reset + cb_reset) >> 26) != (s_cb_unmap_reset >> 26)) {
                std::printf("Guest memory: %llu MiB of unmapped blocks started over\n",
                            (unsigned long long)((s_cb_unmap_reset + cb_reset) >> 20));
            }
            s_cb_unmap_reset += cb_reset;
            dynamic_blocks.Subtract(whole_first, whole_end);
            if (LayerMode()) {
                layer_cpu_blocks.Subtract(whole_first, whole_end);
                layer_cpu_hot.Subtract(whole_first, whole_end);
                layer_watch_failed.Subtract(whole_first, whole_end);
                layer_write_bounces.Subtract(whole_first, whole_end);
                layer_watched.Subtract(whole_first, whole_end);
                layer_edge_blocks.Subtract(whole_first, whole_end);
                layer_volatile.Subtract(whole_first, whole_end);
                for (u64 block = whole_first; block < whole_end; ++block) {
                    layer_watch_strikes.erase(block);
                }
            }
            std::scoped_lock dynamic_lock{dynamic_mutex};
            for (u64 block = whole_first; block < whole_end; ++block) {
                cpu_write_frames.erase(block);
            }
        }
        copy_back_blocks.Subtract(first, end);
        promote_candidates.Subtract(first, end);
        unbacked_blocks.Subtract(first, end);
        idle_demoted.Subtract(first, end);
        in_place_blocks.ForEachInRange(first, end, [&](const Interval& iv) {
            unbind.Add({std::max(first, iv.start), std::min(end, iv.end)});
        });
        for (const auto& range : unbind) {
            const VAddr from = range.start << block_shift, to = range.end << block_shift;
            for (const auto& arena : arenas) {
                const VAddr a = std::max<VAddr>(from, arena.cpu_addr);
                const VAddr b = std::min<VAddr>(to, arena.cpu_addr + arena.size_bytes);
                if (a < b) {
                    BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                        .resourceOffset = a - arena.cpu_addr,
                        .size = b - a,
                        .memory = {},
                        .memoryOffset = 0,
                    });
                }
            }
            resident_ranges.Subtract(range.start, range.end);
            in_place_blocks.Subtract(range.start, range.end);
            asset_bytes.Subtract(range.start << block_shift, range.end << block_shift);
            gpu_written_bytes.Subtract(range.start << block_shift, range.end << block_shift);
            promote_candidates.Subtract(range.start, range.end);
        }
        // psport: blocks wholly unmapped that have a VRAM copy leave VRAM too (their slots go
        // to the next blocks moved there; uploaded again if mapped and used).
        for (u64 block = (addr + block_size - 1) >> block_shift; block < (addr + size) >> block_shift;
             ++block) {
            EvictVramBlock(block);
        }
    }
}

bool BufferCache::IsHostMemory(const Buffer* buffer, VAddr addr, u64 size) const {
    if (buffer->mem_type == MemoryType::HostCached || buffer->mem_type == MemoryType::HostUncached) {
        return true;
    }
    if (!IsInPlace(addr, size)) {
        return false;
    }
    u64 phys = 0;
    uintptr_t mapping_end = 0;
    const PsGuestMemory::Chunk* chunk =
        runtime_memory_direct_phys(addr, &phys, &mapping_end) ? PsGuestMemory::Find(phys) : nullptr;
    return chunk != nullptr && !chunk->is_device_local;
}

bool BufferCache::IsInPlace(VAddr addr, u64 size) const {
    if (size == 0) {
        return false;
    }
    if (LayerMode()) {
        return PsLayer::GpuMemory::Get().Resolve(addr, size).kind ==
               PsLayer::Resolution::Kind::InPlace;
    }
    const u64 first = addr >> block_shift, last = (addr + size - 1) >> block_shift;
    // psport: asked for every binding; the answer holds while the residency is unchanged.
    thread_local std::array<ResidencyMemo, 256> in_place_memo{};
    auto& memo = in_place_memo[first & (in_place_memo.size() - 1)];
    const u64 generation = ResidencyGeneration();
    if (memo.first != first || memo.last != last || memo.generation != generation) {
        memo = {first, last, generation, false, in_place_blocks.Contains(first, last + 1)};
    }
    return memo.in_place;
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<std::pair<vk::Buffer, vk::BufferCopy>, 4> chunk_copies;
    // psport: the arena is the game's memory there: nothing to upload, no
    // write tracking, and GPU writes need no readback. Images aliasing it still sync.
    if (!LayerMode() && IsInPlace(device_addr, size)) {
        return is_texel_buffer && !is_written &&
               SynchronizeMemoryFromImage(arena, arena->Offset(device_addr), device_addr, size);
    }
    // Partly in place: only the parts in VRAM are synced. An upload over a block in place would
    // write an older copy of the game's memory back into it after the game wrote it again.
    if (in_place_blocks.Overlaps(device_addr >> block_shift,
                                 ((device_addr + size - 1) >> block_shift) + 1)) {
        const VAddr end = device_addr + size;
        bool image = false;
        in_place_blocks.ForEachGap(
            device_addr >> block_shift, ((end - 1) >> block_shift) + 1, [&](u64 first, u64 last) {
                const VAddr from = std::max<VAddr>(device_addr, first << block_shift);
                const VAddr to = std::min<VAddr>(end, last << block_shift);
                if (from < to) {
                    image |= SynchronizeMemory(arena, from, static_cast<u32>(to - from),
                                               is_written, is_texel_buffer);
                }
            });
        return image;
    }
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] {
            // psport (PS_GPU_WRITE_TWINS): bytes whose guest copy still matches its twin keep the
            // GPU's data in the arena; twin bytes the guest wrote since are uploaded and no
            // longer GPU-modified.
            if (TwinsEnabled() && !copies.empty()) {
                std::scoped_lock lk{twins_mutex};
                if (!twins.empty()) {
                    boost::container::small_vector<vk::BufferCopy, 4> kept;
                    boost::container::small_vector<std::pair<VAddr, VAddr>, 4> changed;
                    u64 offset = 0;
                    for (const auto& copy : copies) {
                        ForEachTwinRun(copy.dstOffset, copy.dstOffset + copy.size,
                                       [&](VAddr from, VAddr to, TwinRun run) {
                                           if (run == TwinRun::Unchanged) {
                                               twin_kept_bytes.fetch_add(to - from);
                                               return;
                                           }
                                           if (run == TwinRun::Changed) {
                                               twin_guest_bytes.fetch_add(to - from);
                                               gpu_modified_ranges.Subtract(from, to - from);
                                               changed.emplace_back(from, to);
                                           }
                                           kept.push_back({offset, from, to - from});
                                           offset += to - from;
                                       });
                    }
                    // Changed parts are uploaded now: their twins are done.
                    for (const auto& [from, to] : changed) {
                        EraseTwins(from, to);
                    }
                    copies.assign(kept.begin(), kept.end());
                    total_size_bytes = offset;
                }
            }
            // psport: what lies in a guest memory chunk is copied by the GPU
            // straight from it (no CPU copy into staging); the rest goes through staging.
            if (ChunkUploads() && !copies.empty()) {
                boost::container::small_vector<vk::BufferCopy, 4> staged;
                u64 staged_bytes = 0;
                for (const auto& copy : copies) {
                    const VAddr address = copy.dstOffset;
                    u64 phys = 0;
                    uintptr_t mapping_end = 0;
                    const PsGuestMemory::Chunk* chunk = nullptr;
                    if (runtime_memory_direct_phys(address, &phys, &mapping_end) &&
                        address + copy.size <= mapping_end &&
                        (chunk = PsGuestMemory::Find(phys)) != nullptr &&
                        phys + copy.size <= chunk->phys + chunk->cb_chunk) {
                        chunk_copies.push_back({chunk->buffer,
                                                {phys - chunk->phys, address - arena->cpu_addr,
                                                 copy.size}});
                        continue;
                    }
                    staged.push_back({staged_bytes, copy.dstOffset, copy.size});
                    staged_bytes += copy.size;
                }
                copies.assign(staged.begin(), staged.end());
                total_size_bytes = staged_bytes;
            }
            src_buffer = UploadCopies(arena, copies, total_size_bytes);
        });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    } else {
        // psport: the upload failed after its pages were marked clean: they are marked again
        // (and a written binding's GPU mark dropped) so that a later bind uploads them.
        for (const auto& copy : copies) {
            if (is_written) {
                memory_tracker->UnmarkRegionAsGpuModified(copy.dstOffset, copy.size);
            }
            memory_tracker->MarkRegionAsCpuModified(copy.dstOffset, copy.size);
        }
    }
    for (const auto& [chunk_buffer, copy] : chunk_copies) {
        PsNativeProbe::Note(PsNativeProbe::Upload);
        PsStats::g_cb_buffer_upload.fetch_add(copy.size, std::memory_order_relaxed);
        // psport PS_LAYER_MEMORY: the GPU writes the game's memory too (through the chunk's
        // buffer wrapper): a copy from it waits for those writes like any buffer copy.
        if (LayerMode()) {
            const VAddr address = arena->cpu_addr + copy.dstOffset;
            if (const auto source = LayerInPlace(address, copy.size)) {
                const vk::BufferCopy wrapped{source->second, copy.dstOffset, copy.size};
                runtime.CopyBuffer(source->first, arena, std::span{&wrapped, 1});
                continue;
            }
        }
        runtime.CopyFromGuestChunk(chunk_buffer, arena, std::span{&copy, 1});
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, arena->Offset(device_addr), device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    PsNativeProbe::Note(PsNativeProbe::Upload);
    PsStats::g_cb_buffer_upload.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    if (!staging.buffer) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Upload of {:#x} bytes into {:#x} skipped",
                          total_size_bytes, arena->cpu_addr);
        return nullptr;
    }
    // psport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!PsToggle::Disabled(PsToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .pfn_run = &RunGuestCopy,
                .p_context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .cb_copy = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!PsToggle::Disabled(PsToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            PsCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
            for (const auto& copy : *group) {
                scheduler.NoteHostCopySource(copy.source, copy.size);
            }
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        PsCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::WriteBackImage(VAddr addr, u32 size) {
    const auto [arena, offset] = ObtainBuffer(addr, size, true);
    return arena && SynchronizeMemoryFromImage(arena, offset, addr, size);
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, u64 arena_offset,
                                             VAddr device_addr, u32 size) {
    if (const auto meta = texture_cache.GetMetaInfo(device_addr)) {
        if (meta->type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena_offset, size, ZmaskUncompressed);
            return true;
        } else if (meta->type == TextureCache::MetaType::CMask) {
            const auto expanded_value = CmaskColorExpandedValue(meta->num_samples);
            if (!expanded_value) {
                LOG_ERROR_COUNTED(Render_Vulkan,
                                  "Unsupported CMask sample count: address={:#x}, size={:#x}, "
                                  "samples={}, owner={}:{}",
                                  device_addr, size, meta->num_samples, meta->owner_id.index,
                                  meta->owner_uid);
                return false;
            }
            runtime.FillBuffer(arena, arena_offset, size, *expanded_value);
            return true;
        } else {
            static std::atomic<u32> c_unhandled{0};
            if (c_unhandled.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render_Vulkan, "Unhandled metadata type {} at {:#x}+{:#x}",
                            magic_enum::enum_name(meta->type), device_addr, size);
            }
        }
    }
    // psport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !PsToggle::Disabled(PsToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // psport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    if (device_addr != image.info.guest_address || !image.IsAllocated()) {
        LOG_ERROR_COUNTED(Render_Vulkan,
                          "Texel buffer at {:#x} over an image at {:#x} it does not start or "
                          "without memory: not copied from it",
                          device_addr, image.info.guest_address);
        return false;
    }
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    // psport PS_TILE_BACK_LOG=1 (diagnostics): each image written back into the game's memory
    // because a texel buffer reads it, with the age of its last GPU write in submissions.
    static const bool tile_back_log = std::getenv("PS_TILE_BACK_LOG") != nullptr;
    if (tile_back_log) {
        static std::atomic<u32> printed{0};
        if (printed.fetch_add(1) < 400) {
            std::printf("Tile back: image %#llx %ux%u fmt %u tile %u size %llu, read %#llx+%u; last "
                        "GPU write %lld submissions ago, flags %#x\n",
                        (unsigned long long)image.info.guest_address, image.info.size.width,
                        image.info.size.height, u32(image.info.pixel_format),
                        u32(image.info.tile_mode), (unsigned long long)image.info.guest_size,
                        (unsigned long long)device_addr, size,
                        image.gpu_write_tick ? (long long)(scheduler.CurrentTick() -
                                                           image.gpu_write_tick)
                                             : -1ll,
                        u32(image.flags));
        }
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset, IsHostMemory(arena, device_addr, size));
    DropShadows(device_addr, size);
    // A GPU write like any other: what the GPU copies there each frame (a render target read as a
    // texel buffer) goes to VRAM instead of crossing PCIe twice (written there, read back).
    NoteGpuWrite(device_addr, size);
    return true;
}

void BufferCache::DropTwins(VAddr addr, u64 size) {
    if (TwinsEnabled()) {
        std::scoped_lock lk{twins_mutex};
        EraseTwins(addr, addr + size);
    }
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    // The submission going out has the tick before the current one.
    const u64 submitted = scheduler.CurrentTick() - 1;
    // A VkSparseBufferMemoryBindInfo must have bindCount > 0 (an arena created by a readback, or
    // whose blocks got no memory, had none): on Windows the first such call, with no ranges at
    // all, failed with ErrorOutOfHostMemory (#1002).
    std::erase_if(pending_binds, [](const ArenaBinds& arena_binds) { return arena_binds.binds.empty(); });
    if (pending_binds.empty()) {
        QueueCopyBacks(submitted);
        QueuePromotions(submitted);
        return;
    }

    // psport: blocks moved in place wait for the submission that may still
    // upload into their VRAM copy (ProcessDemotions).
    const u64 wait_tick = bind_wait_tick;
    const auto wait_sema = scheduler.GetWorkSemaphore()->Handle();
    bind_wait_tick = 0;
    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();
    info.AddWait(signal_sema, signal_tick);

    // The binds go into the queue right before the submission (PS_ASYNC_SUBMIT: later, from a
    // recording thread), after the submissions still on their way there.
    auto binds = std::make_shared<std::vector<ArenaBinds>>(std::move(pending_binds));
    pending_binds.clear();
    info.before_submit.push_back([this, binds, wait_tick, wait_sema, signal_tick, signal_sema] {
        std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
        buffer_binds.reserve(binds->size());
        u64 ranges = 0;
        for (const auto& arena_binds : *binds) {
            ranges += arena_binds.binds.size();
            buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
                .buffer = arena_binds.arena->Handle(),
                .bindCount = static_cast<u32>(arena_binds.binds.size()),
                .pBinds = arena_binds.binds.data(),
            });
        }
        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .waitSemaphoreValueCount = wait_tick ? 1u : 0u,
            .pWaitSemaphoreValues = wait_tick ? &wait_tick : nullptr,
            .signalSemaphoreValueCount = 1u,
            .pSignalSemaphoreValues = &signal_tick,
        };
        const vk::BindSparseInfo sparse_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = wait_tick ? 1u : 0u,
            .pWaitSemaphores = wait_tick ? &wait_sema : nullptr,
            .bufferBindCount = static_cast<u32>(buffer_binds.size()),
            .pBufferBinds = buffer_binds.data(),
            .signalSemaphoreCount = 1u,
            .pSignalSemaphores = &signal_sema,
        };
        const auto bind_start = std::chrono::steady_clock::now();
        auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
        const double bind_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - bind_start)
                                   .count();
        arena_bind_calls.fetch_add(1, std::memory_order_relaxed);
        arena_bind_ranges.fetch_add(ranges, std::memory_order_relaxed);
        arena_bind_signaled.store(signal_tick, std::memory_order_relaxed);
        if (bind_ms >= 50.0) {
            std::printf("Guest memory: vkQueueBindSparse of %llu ranges took %.0f ms in the driver\n",
                        (unsigned long long)ranges, bind_ms);
        }
        if (submit_result == vk::Result::eErrorDeviceLost) {
            Vulkan::StopOnDeviceLost("binding guest memory");
        }
        if (submit_result == vk::Result::eSuccess) {
            return;
        }
        LOG_ERROR_COUNTED(Render_Vulkan, "vkQueueBindSparse failed: {}", vk::to_string(submit_result));
        LogFailedArenaBinds(*binds, submit_result);
        submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
        if (submit_result == vk::Result::eErrorDeviceLost) {
            Vulkan::StopOnDeviceLost("binding guest memory");
        }
        if (submit_result == vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan, "vkQueueBindSparse of {} ranges succeeded when retried", ranges);
            return;
        }
        // A failed vkQueueBindSparse leaves its semaphores untouched, so the submissions waiting
        // for signal_tick would wait forever: an empty submission signals it in queue order.
        m_c_failed_arena_binds.fetch_add(1, std::memory_order_relaxed);
        m_c_unbound_arena_ranges.fetch_add(ranges, std::memory_order_relaxed);
        const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eAllCommands;
        const vk::SubmitInfo release_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = wait_tick ? 1u : 0u,
            .pWaitSemaphores = wait_tick ? &wait_sema : nullptr,
            .pWaitDstStageMask = &wait_stage,
            .signalSemaphoreCount = 1u,
            .pSignalSemaphores = &signal_sema,
        };
        const auto release_result = instance.GetGraphicsQueue().submit(release_info);
        if (release_result == vk::Result::eErrorDeviceLost) {
            Vulkan::StopOnDeviceLost("releasing a failed guest memory bind");
        }
        LOG_ERROR_COUNTED(Render_Vulkan,
                          "vkQueueBindSparse failed again ({}): {} ranges stay unbound (GPU reads "
                          "there undefined); memory semaphore {} signaled by an empty submission: {}",
                          vk::to_string(submit_result), ranges, signal_tick, vk::to_string(release_result));
    });
    QueueCopyBacks(submitted); // bound with the next submission
    QueuePromotions(submitted);
}

void BufferCache::LogFailedArenaBinds(std::span<const ArenaBinds> binds, vk::Result result) const {
    static std::atomic<u32> s_c_reports{0};
    if (s_c_reports.fetch_add(1, std::memory_order_relaxed) >= 4) {
        return;
    }
    std::printf("Arena binds: vkQueueBindSparse returned %s for %zu arenas (block size %#llx)\n",
                vk::to_string(result).c_str(), binds.size(), (unsigned long long)block_size);
    for (const auto& arena_binds : binds) {
        u64 cb_bound = 0, cb_min = ~0ull, cb_max = 0, c_unbinds = 0, c_misaligned = 0, c_past_end = 0;
        for (const auto& bind : arena_binds.binds) {
            cb_bound += bind.size;
            cb_min = std::min<u64>(cb_min, bind.size);
            cb_max = std::max<u64>(cb_max, bind.size);
            c_unbinds += !bind.memory;
            c_misaligned += ((bind.resourceOffset | bind.size | bind.memoryOffset) & (block_size - 1)) != 0;
            c_past_end += bind.resourceOffset + bind.size > arena_binds.arena->size_bytes;
        }
        std::printf("  arena %#llx+%#llx: %zu ranges, %llu MiB, sizes %#llx..%#llx, %llu unbinds, "
                    "%llu misaligned, %llu past the end\n",
                    (unsigned long long)arena_binds.arena->cpu_addr,
                    (unsigned long long)arena_binds.arena->size_bytes, arena_binds.binds.size(),
                    (unsigned long long)(cb_bound >> 20), (unsigned long long)cb_min,
                    (unsigned long long)cb_max, (unsigned long long)c_unbinds,
                    (unsigned long long)c_misaligned, (unsigned long long)c_past_end);
        for (std::size_t i = 0; i < std::min<std::size_t>(arena_binds.binds.size(), 4); ++i) {
            const auto& bind = arena_binds.binds[i];
            std::printf("    offset %#llx size %#llx memory %p memory offset %#llx\n",
                        (unsigned long long)bind.resourceOffset, (unsigned long long)bind.size,
                        static_cast<void*>(static_cast<VkDeviceMemory>(bind.memory)),
                        (unsigned long long)bind.memoryOffset);
        }
    }
}

void BufferCache::ReportArenaBinds() {
    const u64 signaled = arena_bind_signaled.load(std::memory_order_relaxed);
    const auto [result, done] =
        instance.GetDevice().getSemaphoreCounterValue(memory_semaphore.Handle());
    std::printf("Arena binds: %llu vkQueueBindSparse calls (%llu ranges), %llu failed twice (%llu ranges "
                "left unbound); the last one signals %llu, the GPU is at %llu%s\n",
                (unsigned long long)arena_bind_calls.load(), (unsigned long long)arena_bind_ranges.load(),
                (unsigned long long)m_c_failed_arena_binds.load(),
                (unsigned long long)m_c_unbound_arena_ranges.load(), (unsigned long long)signaled,
                result == vk::Result::eSuccess ? (unsigned long long)done : 0ull,
                result == vk::Result::eSuccess && done < signaled
                    ? ": the submissions after it wait for a bind not done yet"
                    : "");
}

} // namespace VideoCore

namespace VideoCore {
void BufferCache::NoteWriteTick(VAddr address, u64 size) {
    WriteTicks().Note(address, size, scheduler.CurrentTick());
}
} // namespace VideoCore

namespace VideoCore {
bool BufferCache::GarlicInVramForLayer() {
    return GarlicInVram();
}
} // namespace VideoCore

namespace VideoCore {
u64 BufferCache::LayerMirrorFloor(u64 budget) {
    // PS_LAYER_MIRROR_FLOOR_PERCENT (25) of the driver's VRAM budget: the mirrors' share that the
    // texture collector's mark does not take from them (6 GB: ~1.4 GB, 4 GB: ~0.9 GB). Past it they
    // yield to images; under it images yield (the collector evicts more), up to the budget's
    // PS_VRAM_PROMOTE_PERCENT. Reading the game's memory over the bus costs far more on NVIDIA.
    static const u64 percent = [] {
        const char* v = std::getenv("PS_LAYER_MIRROR_FLOOR_PERCENT");
        return std::min<u64>(v && *v ? std::strtoull(v, nullptr, 10) : 25ull, 90ull);
    }();
    return budget / 100 * percent;
}

bool BufferCache::VramPromotionsPaused() {
    // VRAM nearly full (past PS_VRAM_PROMOTE_PERCENT of the driver's budget, default 90): no new
    // copies; the candidates wait, read in place meanwhile (#32: a card with 8 GB). Checked at
    // most once a second (the query goes to the driver).
    if (!instance.CanReportMemoryUsage()) {
        return false;
    }
    static const u64 percent = [] {
        const char* v = std::getenv("PS_VRAM_PROMOTE_PERCENT");
        return v && *v ? std::strtoull(v, nullptr, 10) : 90ull;
    }();
    static s64 checked_second = -1;
    static bool full = false;
    const s64 second = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (second != checked_second) {
        checked_second = second;
        const u64 usage = instance.GetDeviceMemoryUsage();
        const u64 budget = instance.GetDeviceMemoryBudgetNow();
        // Also short of the texture collector's critical mark: the collector compares all of our
        // VRAM and can only free images, so copies past it had it evict textures in use (6 GB
        // GTX 1660 Ti: mark 3927 MiB, 1.8 GiB of copies, 200-1250 images evicted per 5 s).
        // Not below the mirrors' own share (LayerMirrorFloor), though: on a 4 GB card images and
        // the rest already held the usage past that point, no copy was made again once the old
        // ones went idle, and everything was read over the bus (5 FPS after 4 minutes).
        const u64 critical = PsStats::g_cb_gc_critical.load(std::memory_order_relaxed);
        const bool under_floor = LayerMode() && PsLayer::GpuMemory::Get().ValidBytes() <
                                                    LayerMirrorFloor(budget);
        const bool near_critical =
            critical != 0 && usage + PromoteCriticalMargin >= critical && !under_floor;
        const bool now_full = (budget != 0 && usage * 100 > budget * percent) || near_critical;
        if (now_full != full) {
            std::printf("Guest memory: VRAM %llu of %llu MiB in use (texture collector critical at "
                        "%llu): copies of loaded blocks %s\n",
                        (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
                        (unsigned long long)(critical >> 20),
                        now_full ? "wait (read in place meanwhile)" : "resume");
        }
        full = now_full;
    }
    return full;
}
} // namespace VideoCore
