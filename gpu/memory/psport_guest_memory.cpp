// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_guest_memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "pslayer_gpu_memory.h"
#include "psport_quirks.h"
#include "../../src/platform/ps_host.h"

extern "C" void runtime_memory_set_guest_chunk_allocator(int (*pfn_alloc)(uint64_t phys,
                                                                      uint64_t cb_range));
extern "C" void runtime_memory_set_guest_chunk_whole(int whole);
extern "C" void* runtime_memory_backing_pointer(uint64_t phys);

namespace PsGuestMemory {
namespace {
using u64 = std::uint64_t;
constexpr std::size_t MaxChunks = 256;

vk::Device s_device;
vk::PhysicalDeviceMemoryProperties s_memory_properties;
std::mutex s_mutex;
std::array<Chunk*, MaxChunks> s_chunks{}; // sorted by phys
std::size_t s_c_chunks = 0;
bool s_is_whole_only = false; // the driver's dma-buf maps at offset 0 only: a chunk per allocation
enum class Mode { DmaBuf, HostImport };
Mode s_mode = Mode::DmaBuf;
std::atomic<u64> s_cb_chunk{0};
bool s_is_usable_checked = false;
vk::BufferUsageFlags s_xfb_dmabuf;
vk::BufferUsageFlags s_xfb_host;

bool GuestInVram() {
    // psport PS_GUEST_VRAM=1 (experiment): the game's direct memory in VRAM the CPU maps through
    // the PCI BAR (Resizable BAR), as GDDR is the PS4's one memory: no VRAM copies.
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_GUEST_VRAM");
        return sz_env && sz_env[0] == '1';
    }();
    return s_is_on;
}

std::uint32_t FindType(std::uint32_t bits) {
    if (GuestInVram()) {
        const auto want = vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eDeviceLocal;
        for (std::uint32_t i = 0; i < s_memory_properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (s_memory_properties.memoryTypes[i].propertyFlags & want) == want) {
                return i;
            }
        }
    }
    const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
    for (std::uint32_t i = 0; i < s_memory_properties.memoryTypeCount; ++i) {
        const auto flags = s_memory_properties.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & want) == want &&
            !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            return i;
        }
    }
    return ~0u;
}

/// Uses of a chunk's buffer. With the layer's memory module the chunk buffers are what the GPU
/// binds (no sparse arena): every buffer use, texel buffers and a device address too.
vk::BufferUsageFlags ChunkBufferUsage(vk::ExternalMemoryHandleTypeFlagBits type) {
    vk::BufferUsageFlags usage =
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
        vk::BufferUsageFlagBits::eIndirectBuffer;
    if (LayerMemory()) {
        usage |= vk::BufferUsageFlagBits::eUniformTexelBuffer |
                 vk::BufferUsageFlagBits::eStorageTexelBuffer |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress |
                 (type == vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT ? s_xfb_dmabuf : s_xfb_host);
    }
    return usage;
}

/// A new chunk becomes a source of the layer's memory module.
void RegisterChunk(Chunk* p_chunk) {
    if (!LayerMemory()) {
        return;
    }
    const vk::DeviceAddress address = s_device.getBufferAddress({.buffer = p_chunk->buffer});
    PsLayer::GpuMemory::Get().AddGuestSource(p_chunk->phys, p_chunk->cb_chunk, p_chunk->buffer, address,
                                             p_chunk);
}

/// The runtime's chunk allocator: a dma-buf fd of `cb_range` bytes of GPU-visible memory, or -1.
int AllocChunk(u64 phys, u64 cb_range) {
    {
        std::scoped_lock lk{s_mutex};
        if (s_c_chunks == MaxChunks) {
            return -1;
        }
    }
    static std::atomic<int> s_c_failures{0};
    const auto fail = [&](const char* sz_what, vk::Result result) {
        if (s_c_failures.fetch_add(1) < 4) {
            std::fprintf(stderr, "Guest memory: %s failed (%s); chunk %#llx stays in the memfd\n",
                         sz_what, vk::to_string(result).c_str(), (unsigned long long)phys);
        }
        return -1;
    };
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::BufferCreateInfo buffer_ci{
        .pNext = &external,
        .size = cb_range,
        .usage = ChunkBufferUsage(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT),
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [buffer_result, buffer] = s_device.createBuffer(buffer_ci);
    if (buffer_result != vk::Result::eSuccess) {
        return fail("buffer creation", buffer_result);
    }
    const auto requirements = s_device.getBufferMemoryRequirements(buffer);
    const std::uint32_t type = FindType(requirements.memoryTypeBits);
    if (type == ~0u) {
        s_device.destroyBuffer(buffer);
        return fail("finding a cached system memory type", vk::Result::eErrorFeatureNotPresent);
    }
    const vk::MemoryAllocateFlagsInfo address_flags{
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::ExportMemoryAllocateInfo export_info{
        .pNext = LayerMemory() ? &address_flags : nullptr,
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const auto [memory_result, memory] = s_device.allocateMemory({
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) {
        s_device.destroyBuffer(buffer);
        return fail("allocation", memory_result);
    }
    if (const auto result = s_device.bindBufferMemory(buffer, memory, 0);
        result != vk::Result::eSuccess) {
        s_device.freeMemory(memory);
        s_device.destroyBuffer(buffer);
        return fail("binding", result);
    }
    const auto [fd_result, fd] = s_device.getMemoryFdKHR({
        .memory = memory,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    });
    if (fd_result != vk::Result::eSuccess) {
        s_device.freeMemory(memory);
        s_device.destroyBuffer(buffer);
        return fail("dma-buf export", fd_result);
    }
    {
        std::scoped_lock lk{s_mutex};
        std::size_t i = s_c_chunks++;
        for (; i > 0 && s_chunks[i - 1]->phys > phys; --i) {
            s_chunks[i] = s_chunks[i - 1];
        }
        static std::uint32_t s_i_next = 0;
        const bool is_device_local = static_cast<bool>(s_memory_properties.memoryTypes[type].propertyFlags &
                                                       vk::MemoryPropertyFlagBits::eDeviceLocal);
        s_chunks[i] = new Chunk{phys, cb_range, buffer, memory, s_i_next++, is_device_local};
        RegisterChunk(s_chunks[i]);
    }
    const u64 cb_total = s_cb_chunk.fetch_add(cb_range) + cb_range;
    std::printf("Guest memory: direct memory %#llx+%llu MiB in GPU-visible memory (dma-buf), "
                "%llu MiB so far\n",
                (unsigned long long)phys, (unsigned long long)(cb_range >> 20),
                (unsigned long long)(cb_total >> 20));
    return fd;
}

/// Host memory import (VK_EXT_external_memory_host): the chunk stays in the runtime's memfd, the
/// GPU imports its backing view. CPU access is ordinary cached memory; the GPU reads and writes it
/// over the bus, as a PC game's upload heap. For drivers whose dma-buf does not fit (NVIDIA: maps
/// at offset 0 only, CPU access through it may be uncached).
vk::DeviceSize s_import_alignment = 4096;

std::uint32_t FindHostType(std::uint32_t bits) {
    // Cached host memory first, then any host-visible type the import allows.
    for (const bool is_cached : {true, false}) {
        for (std::uint32_t i = 0; i < s_memory_properties.memoryTypeCount; ++i) {
            const auto flags = s_memory_properties.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || !(flags & vk::MemoryPropertyFlagBits::eHostVisible) ||
                (flags & vk::MemoryPropertyFlagBits::eDeviceLocal) ||
                (is_cached && !(flags & vk::MemoryPropertyFlagBits::eHostCached))) {
                continue;
            }
            return i;
        }
    }
    return ~0u;
}

constexpr vk::BufferUsageFlags ChunkUsage =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

/// Imports [pointer, pointer + size) as device memory with a buffer over it; false on failure.
bool ImportHost(void* p_pointer, u64 cb_range, vk::Buffer& buffer, vk::DeviceMemory& memory,
                vk::Result& error) {
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const auto [buffer_result, created] = s_device.createBuffer({
        .pNext = &external,
        .size = cb_range,
        .usage = ChunkBufferUsage(vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT),
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer_result != vk::Result::eSuccess) {
        error = buffer_result;
        return false;
    }
    const auto [props_result, props] = s_device.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, p_pointer);
    const auto requirements = s_device.getBufferMemoryRequirements(created);
    const std::uint32_t type = props_result == vk::Result::eSuccess
                                   ? FindHostType(props.memoryTypeBits & requirements.memoryTypeBits)
                                   : ~0u;
    if (type == ~0u) {
        s_device.destroyBuffer(created);
        error = props_result != vk::Result::eSuccess ? props_result
                                                     : vk::Result::eErrorFeatureNotPresent;
        return false;
    }
    const vk::MemoryAllocateFlagsInfo address_flags{
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .pNext = LayerMemory() ? &address_flags : nullptr,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = p_pointer,
    };
    const auto [memory_result, allocated] = s_device.allocateMemory({
        .pNext = &import_info,
        .allocationSize = cb_range,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) {
        s_device.destroyBuffer(created);
        error = memory_result;
        return false;
    }
    if (const auto result = s_device.bindBufferMemory(created, allocated, 0);
        result != vk::Result::eSuccess) {
        s_device.freeMemory(allocated);
        s_device.destroyBuffer(created);
        error = result;
        return false;
    }
    buffer = created;
    memory = allocated;
    return true;
}

/// The runtime's chunk allocator in host import mode: CHUNK_IMPORTED (-3, the chunk stays in the
/// memfd) once its backing view is imported, or -1.
int AllocImported(u64 phys, u64 cb_range) {
    {
        std::scoped_lock lk{s_mutex};
        if (s_c_chunks == MaxChunks) {
            return -1;
        }
    }
    void* p_pointer = runtime_memory_backing_pointer(phys);
    if (!p_pointer || reinterpret_cast<std::uintptr_t>(p_pointer) % s_import_alignment != 0 ||
        cb_range % s_import_alignment != 0) {
        return -1;
    }
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    vk::Result error{};
    if (!ImportHost(p_pointer, cb_range, buffer, memory, error)) {
        static std::atomic<int> s_c_failures{0};
        if (s_c_failures.fetch_add(1) < 4) {
            std::fprintf(stderr, "Guest memory: importing chunk %#llx failed (%s); it stays in the "
                                 "memfd\n",
                         (unsigned long long)phys, vk::to_string(error).c_str());
        }
        return -1;
    }
    {
        std::scoped_lock lk{s_mutex};
        std::size_t i = s_c_chunks++;
        for (; i > 0 && s_chunks[i - 1]->phys > phys; --i) {
            s_chunks[i] = s_chunks[i - 1];
        }
        static std::uint32_t s_i_next = 0;
        s_chunks[i] = new Chunk{phys, cb_range, buffer, memory, s_i_next++};
        RegisterChunk(s_chunks[i]);
    }
    const u64 cb_total = s_cb_chunk.fetch_add(cb_range) + cb_range;
    std::printf("Guest memory: direct memory %#llx+%llu MiB imported by the GPU (host memory), "
                "%llu MiB so far\n",
                (unsigned long long)phys, (unsigned long long)(cb_range >> 20),
                (unsigned long long)(cb_total >> 20));
    return -3;
}

/// Runs `record` on the graphics queue and waits; false on any failure.
template <typename Record>
bool RunOnce(const Vulkan::Instance& instance, Record&& record) {
    const vk::Device dev = instance.GetDevice();
    const auto [pool_result, pool] = dev.createCommandPool(
        {.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()});
    if (pool_result != vk::Result::eSuccess) {
        return false;
    }
    bool is_ok = false;
    const auto [alloc_result, cmdbufs] = dev.allocateCommandBuffers(
        {.commandPool = pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
    const auto [fence_result, fence] = dev.createFence({});
    if (alloc_result == vk::Result::eSuccess && fence_result == vk::Result::eSuccess) {
        const vk::CommandBuffer cmd = cmdbufs[0];
        if (cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
            vk::Result::eSuccess) {
            record(cmd);
            const vk::MemoryBarrier to_host{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                            .dstAccessMask = vk::AccessFlagBits::eHostRead};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {}, to_host, {}, {});
            if (cmd.end() == vk::Result::eSuccess) {
                const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
                is_ok = instance.GetGraphicsQueue().submit(submit, fence) == vk::Result::eSuccess &&
                     dev.waitForFences(fence, true, 5'000'000'000ull) == vk::Result::eSuccess;
            }
        }
    }
    if (fence_result == vk::Result::eSuccess) {
        dev.destroyFence(fence);
    }
    dev.destroyCommandPool(pool);
    return is_ok;
}

/// Host import as the PC model uses it, on a small memfd mapped twice (the guest's view and the
/// backing view): imported through one view, bound into a sparse buffer like the arena, written by
/// the GPU and read by the CPU through the other view, and the other way round.
bool HostImportWorks(const Vulkan::Instance& instance, const char*& why) {
    why = "VK_EXT_external_memory_host unavailable";
    if (!instance.IsHostMemoryImportSupported()) {
        return false;
    }
    const vk::Device dev = instance.GetDevice();
    vk::PhysicalDeviceExternalMemoryHostPropertiesEXT host_props{};
    vk::PhysicalDeviceProperties2 props2{.pNext = &host_props};
    instance.GetPhysicalDevice().getProperties2(&props2);
    s_import_alignment = std::max<vk::DeviceSize>(host_props.minImportedHostPointerAlignment, 4096);
    constexpr u64 Size = 4 << 20;
    if (Size % s_import_alignment != 0) {
        why = "import alignment above 4 MiB";
        return false;
    }
    int h_fd = -1;
    if (ps_host_vm_section_create("psport-guest-memory-probe", Size, &h_fd) != 0) {
        why = "memfd";
        return false;
    }
    auto* p_view = static_cast<std::uint32_t*>(
        ps_host_vm_map_shared(nullptr, Size, PS_HOST_VM_READ | PS_HOST_VM_WRITE, 0, h_fd, 0));
    auto* p_guest = static_cast<std::uint32_t*>(
        ps_host_vm_map_shared(nullptr, Size, PS_HOST_VM_READ | PS_HOST_VM_WRITE, 0, h_fd, 0));
    ps_host_fs_close(h_fd);
    bool is_ok = false;
    vk::Buffer buffer, sparse;
    vk::DeviceMemory memory;
    vk::Result error{};
    if (!p_view || !p_guest) {
        why = "mmap of the memfd";
    } else if (!ImportHost(p_view, Size, buffer, memory, error)) {
        static std::string s_text;
        s_text = "the import of a memfd mapping failed (" + vk::to_string(error) + ")";
        why = s_text.c_str();
    } else if (LayerMemory()) {
        // The layer's memory module binds the imported buffers themselves (no sparse arena): the
        // same GPU and CPU writes through the imported buffer.
        p_guest[1024] = 0x5ca1ab1e;
        const bool has_run = RunOnce(instance, [&](vk::CommandBuffer cmd) {
            cmd.fillBuffer(buffer, 0, 4096, 0x600dcafe);
            cmd.copyBuffer(buffer, buffer, vk::BufferCopy{4096, 8192, 4});
        });
        why = "GPU work on imported memory";
        if (has_run) {
            const bool is_gpu_to_cpu_ok = p_guest[0] == 0x600dcafe && p_guest[1023] == 0x600dcafe;
            const bool is_cpu_to_gpu_ok = p_guest[2048] == 0x5ca1ab1e;
            why = !is_gpu_to_cpu_ok ? "GPU writes not seen by the CPU" : "CPU writes not seen by the GPU";
            is_ok = is_gpu_to_cpu_ok && is_cpu_to_gpu_ok;
        }
    } else {
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        };
        const auto [sparse_result, created] = dev.createBuffer({
            .pNext = &external,
            .flags = vk::BufferCreateFlagBits::eSparseBinding |
                     vk::BufferCreateFlagBits::eSparseResidency,
            .size = Size,
            .usage = ChunkUsage | s_xfb_host,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        why = "a sparse buffer for imported memory";
        // The arena also binds VRAM blocks: the buffer must take device-local memory too.
        bool is_vram_ok = false;
        if (sparse_result == vk::Result::eSuccess) {
            const auto reqs = dev.getBufferMemoryRequirements(created);
            for (std::uint32_t i = 0; i < s_memory_properties.memoryTypeCount; ++i) {
                is_vram_ok |= (reqs.memoryTypeBits & (1u << i)) &&
                           (s_memory_properties.memoryTypes[i].propertyFlags &
                            vk::MemoryPropertyFlagBits::eDeviceLocal);
            }
            if (!is_vram_ok) {
                why = "a sparse buffer for imported memory cannot take VRAM";
                dev.destroyBuffer(created);
            }
        }
        if (sparse_result == vk::Result::eSuccess && is_vram_ok) {
            sparse = created;
            const vk::SparseMemoryBind bind{.resourceOffset = 0, .size = Size, .memory = memory,
                                            .memoryOffset = 0};
            const vk::SparseBufferMemoryBindInfo buffer_bind{.buffer = sparse, .bindCount = 1,
                                                             .pBinds = &bind};
            const auto [fence_result, fence] = dev.createFence({});
            why = "binding imported memory into a sparse buffer";
            if (fence_result == vk::Result::eSuccess &&
                instance.GetGraphicsQueue().bindSparse(
                    vk::BindSparseInfo{.bufferBindCount = 1, .pBufferBinds = &buffer_bind},
                    fence) == vk::Result::eSuccess &&
                dev.waitForFences(fence, true, 5'000'000'000ull) == vk::Result::eSuccess) {
                // GPU writes through the sparse buffer, CPU reads through the guest view; CPU
                // writes through the guest view, the GPU copies them through the sparse buffer.
                p_guest[1024] = 0x5ca1ab1e;
                const bool has_run = RunOnce(instance, [&](vk::CommandBuffer cmd) {
                    cmd.fillBuffer(sparse, 0, 4096, 0x600dcafe);
                    cmd.copyBuffer(sparse, buffer, vk::BufferCopy{4096, 8192, 4});
                });
                why = "GPU work on imported memory";
                if (has_run) {
                    const bool is_gpu_to_cpu_ok = p_guest[0] == 0x600dcafe && p_guest[1023] == 0x600dcafe;
                    const bool is_cpu_to_gpu_ok = p_guest[2048] == 0x5ca1ab1e;
                    why = !is_gpu_to_cpu_ok ? "GPU writes not seen by the CPU"
                                      : "CPU writes not seen by the GPU";
                    is_ok = is_gpu_to_cpu_ok && is_cpu_to_gpu_ok;
                }
            }
            if (fence_result == vk::Result::eSuccess) {
                dev.destroyFence(fence);
            }
        }
    }
    if (sparse) {
        dev.destroyBuffer(sparse);
    }
    if (buffer) {
        dev.destroyBuffer(buffer);
    }
    if (memory) {
        dev.freeMemory(memory);
    }
    if (p_view) {
        ps_host_vm_unmap(p_view, Size);
    }
    if (p_guest) {
        ps_host_vm_unmap(p_guest, Size);
    }
    return is_ok;
}

/// Below this the mapping is uncached (write-combined reads run at a few hundred MB/s).
constexpr double MinCachedReadMBps = 1000.0;

/// CPU read speed through a dma-buf mapping, MB/s (the game's code runs on this memory: an
/// uncached mapping would slow it to a crawl).
double MappedReadSpeed(const volatile std::uint64_t* p_words, u64 cb_range) {
    std::uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 4; ++pass) {
        for (u64 i = 0; i < cb_range / 8; i += 8) {
            sum += p_words[i];
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    static volatile std::uint64_t s_sink;
    s_sink = sum;
    return s > 0 ? 4.0 * cb_range / s / 1e6 : 1e9;
}
} // namespace

namespace {
/// The GPU's choice of LayerMemory without PS_LAYER_MEMORY: -1 not known yet, 0 or 1.
std::atomic<int> s_layer_default{-1};

/// What Usable found and why it chose as it did, for its one summary line.
struct Detected {
    bool has_sparse_binding = false;
    bool has_sparse_residency_buffer = false;
    bool has_sparse_queue = false;
    double sparse_bind_ms = -1.0; ///< negative: not measured or failed
    const char* sz_backing_why = "";
    const char* sz_dmabuf = "untested";
    double dmabuf_read_mbps = 0.0;
    const char* sz_host_import = "untested";
    const char* sz_chunks_why = "";
};
Detected s_detected;

/// Well above what a driver that binds in the page tables takes (a few ms), well below the
/// seconds of a stalled rebinding.
constexpr double MaxSparseBindMs = 250.0;

/// Binds 64 MiB of VRAM into a sparse buffer in the arena's blocks, rebinds every block to
/// another part of the memory and unbinds them, each pass waited for: ms for the three, or a
/// negative value when the driver failed one.
double MeasureSparseBind(const Vulkan::Instance& instance) {
    constexpr u64 Size = 64 << 20;
    constexpr u64 MinBlock = 16 << 10;
    const vk::Device dev = instance.GetDevice();
    const auto [buffer_result, buffer] = dev.createBuffer({
        .flags = vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = Size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer_result != vk::Result::eSuccess) {
        return -1.0;
    }
    const auto reqs = dev.getBufferMemoryRequirements(buffer);
    const u64 cb_block = (std::max<u64>(reqs.alignment, MinBlock) + reqs.alignment - 1) / reqs.alignment *
                         reqs.alignment;
    std::uint32_t type = ~0u;
    for (std::uint32_t i = 0; i < s_memory_properties.memoryTypeCount && type == ~0u; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (s_memory_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            type = i;
        }
    }
    vk::DeviceMemory memory;
    if (type != ~0u && Size % cb_block == 0) {
        const auto [memory_result, allocated] = dev.allocateMemory({.allocationSize = Size, .memoryTypeIndex = type});
        if (memory_result == vk::Result::eSuccess) {
            memory = allocated;
        }
    }
    const auto [fence_result, fence] = dev.createFence({});
    double ms = -1.0;
    if (memory && fence_result == vk::Result::eSuccess) {
        const u64 c_blocks = Size / cb_block;
        std::vector<vk::SparseMemoryBind> binds(c_blocks);
        const vk::Queue queue = instance.GetGraphicsQueue();
        const auto run_pass = [&](int i_pass) {
            for (u64 i = 0; i < c_blocks; ++i) {
                const u64 i_memory_block = i_pass == 1 ? c_blocks - 1 - i : i;
                binds[i] = vk::SparseMemoryBind{
                    .resourceOffset = i * cb_block,
                    .size = cb_block,
                    .memory = i_pass == 2 ? vk::DeviceMemory{} : memory,
                    .memoryOffset = i_pass == 2 ? 0 : i_memory_block * cb_block,
                };
            }
            const vk::SparseBufferMemoryBindInfo buffer_bind{
                .buffer = buffer, .bindCount = static_cast<std::uint32_t>(c_blocks), .pBinds = binds.data()};
            return dev.resetFences(fence) == vk::Result::eSuccess &&
                   queue.bindSparse(vk::BindSparseInfo{.bufferBindCount = 1, .pBufferBinds = &buffer_bind},
                                    fence) == vk::Result::eSuccess &&
                   dev.waitForFences(fence, true, 5'000'000'000ull) == vk::Result::eSuccess;
        };
        const auto start = std::chrono::steady_clock::now();
        if (run_pass(0) && run_pass(1) && run_pass(2)) {
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        } else {
            // so that nothing still uses the buffer when it goes, since a pass may have timed out
            (void)queue.waitIdle();
        }
    }
    if (fence_result == vk::Result::eSuccess) {
        dev.destroyFence(fence);
    }
    dev.destroyBuffer(buffer);
    if (memory) {
        dev.freeMemory(memory);
    }
    return ms;
}

/// The sparse arena where the device can bind sparse buffers and does it fast, the layer's memory
/// module (no sparse binding) elsewhere; PS_LAYER_MEMORY=1/0 chooses by hand. On an RX 7800 XT the
/// module was on par with the arena on a route in Yharnam, but in the Hunter's Dream it kept ~1 GB
/// of the area's data out of VRAM (ranges with a few unannounced blocks stay in place without
/// volatile blocks): 100 FPS against ~200. NVIDIA's proprietary driver held the GPU for 10 s and
/// more on each rebinding of the game's memory, the time growing with the pages already bound,
/// which the test bind does not reach (nvidia_slow_sparse_rebind; NVIDIA developer forum, "Sparse
/// texture binding is painfully slow").
void ChooseLayerDefault(const Vulkan::Instance& instance) {
    const vk::PhysicalDevice physical = instance.GetPhysicalDevice();
    const vk::PhysicalDeviceFeatures features = physical.getFeatures();
    const auto families = physical.getQueueFamilyProperties();
    const std::uint32_t i_family = instance.GetGraphicsQueueFamilyIndex();
    s_detected.has_sparse_binding = features.sparseBinding;
    s_detected.has_sparse_residency_buffer = features.sparseResidencyBuffer;
    s_detected.has_sparse_queue = i_family < families.size() &&
                                  (families[i_family].queueFlags & vk::QueueFlagBits::eSparseBinding);
    const char* sz_env = std::getenv("PS_LAYER_MEMORY");
    const bool is_forced = sz_env && *sz_env;
    const bool can_bind_sparse = s_detected.has_sparse_binding && s_detected.has_sparse_residency_buffer &&
                                 s_detected.has_sparse_queue;
    if (can_bind_sparse && !(is_forced && sz_env[0] == '1')) {
        s_detected.sparse_bind_ms = MeasureSparseBind(instance);
    }
    const char*& sz_why = s_detected.sz_backing_why;
    bool is_arena = false;
    if (is_forced) {
        sz_why = "PS_LAYER_MEMORY";
    } else if (!s_detected.has_sparse_binding) {
        sz_why = "no sparseBinding";
    } else if (!s_detected.has_sparse_residency_buffer) {
        sz_why = "no sparseResidencyBuffer";
    } else if (!s_detected.has_sparse_queue) {
        sz_why = "the graphics queue cannot bind sparse memory";
    } else if (s_detected.sparse_bind_ms < 0) {
        sz_why = "the test bind failed";
    } else if (s_detected.sparse_bind_ms > MaxSparseBindMs) {
        sz_why = "the test bind was slow";
    } else if (PsQuirk::Active(RUNTIME_QUIRK_nvidia_slow_sparse_rebind)) {
        sz_why = "HOST quirk nvidia_slow_sparse_rebind";
    } else {
        is_arena = true;
        sz_why = "sparse buffers bound fast";
    }
    s_layer_default.store(is_arena ? 0 : 1, std::memory_order_relaxed);
}
} // namespace

void Unsupported(const std::string& why) {
    std::printf("Guest memory: %s. The port needs the GPU to use the game's memory in place: a "
                "driver that imports host memory (VK_EXT_external_memory_host) or exports "
                "mappable dma-buf memory\n",
                why.c_str());
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(1);
}

bool HostImported() {
    return s_mode == Mode::HostImport;
}

bool LayerMemory() {
    // PS_LAYER_MEMORY=1/0 chooses; else Usable (which asks first) sets it by GPU.
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_LAYER_MEMORY");
        if (sz_env && *sz_env) {
            return sz_env[0] == '1';
        }
        return s_layer_default.load(std::memory_order_relaxed) == 1;
    }();
    return s_is_on;
}

namespace {
/// dma-buf chunks: exported cached system memory the runtime maps at the game's addresses.
bool DmaBufWorks(const Vulkan::Instance& instance) {
    {
        if (!instance.IsGuestMemoryExportSupported()) {
            std::printf("Guest memory: the driver cannot export system memory as a dma-buf\n");
            s_detected.sz_dmabuf = "unsupported";
            return false;
        }
        const vk::Device dev = instance.GetDevice();
        const vk::PhysicalDeviceMemoryProperties props = instance.GetPhysicalDevice().getMemoryProperties();
        constexpr u64 Size = 2 << 20, Offset = 1 << 20;
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        };
        const auto [buffer_result, buffer] = dev.createBuffer({
            .pNext = &external,
            .size = Size,
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        if (buffer_result != vk::Result::eSuccess) {
            s_detected.sz_dmabuf = "unsupported";
            return false;
        }
        const auto requirements = dev.getBufferMemoryRequirements(buffer);
        std::uint32_t type = ~0u;
        const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
        for (std::uint32_t i = 0; i < props.memoryTypeCount && type == ~0u; ++i) {
            const auto flags = props.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) && (flags & want) == want &&
                !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                type = i;
            }
        }
        bool is_ok = false;
        vk::DeviceMemory memory{};
        int h_fd = -1;
        const vk::ExportMemoryAllocateInfo export_info{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        };
        if (type != ~0u) {
            const auto [memory_result, allocated] = dev.allocateMemory(
                {.pNext = &export_info, .allocationSize = requirements.size, .memoryTypeIndex = type});
            if (memory_result == vk::Result::eSuccess) {
                memory = allocated;
                const auto [fd_result, exported] = dev.getMemoryFdKHR(
                    {.memory = memory, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT});
                h_fd = fd_result == vk::Result::eSuccess ? exported : -1;
            }
        }
        if (h_fd >= 0) {
            // The whole buffer and one page inside it, writing through one, reading through the other.
            void* p_whole = ps_host_vm_map_shared(nullptr, Size, PS_HOST_VM_READ | PS_HOST_VM_WRITE, 0, h_fd, 0);
            void* p_page = ps_host_vm_map_shared(nullptr, 4096, PS_HOST_VM_READ | PS_HOST_VM_WRITE, 0, h_fd, Offset);
            // PS_GUEST_WHOLE_CHUNKS=1: as if the offset mapping failed (tests the NVIDIA path).
            const char* sz_force_whole = std::getenv("PS_GUEST_WHOLE_CHUNKS");
            if (sz_force_whole && sz_force_whole[0] == '1' && p_page) {
                ps_host_vm_unmap(p_page, 4096);
                p_page = nullptr;
            }
            if (p_whole && p_page) {
                *static_cast<volatile std::uint32_t*>(p_page) = 0x5ca1ab1e;
                is_ok = static_cast<volatile std::uint32_t*>(p_whole)[Offset / 4] == 0x5ca1ab1e;
            } else if (p_whole) {
                // Offset 0 only (NVIDIA): a second mapping of the start must alias the first.
                void* p_again = ps_host_vm_map_shared(nullptr, 4096, PS_HOST_VM_READ | PS_HOST_VM_WRITE, 0, h_fd, 0);
                if (p_again) {
                    *static_cast<volatile std::uint32_t*>(p_again) = 0x5ca1ab1e;
                    is_ok = s_is_whole_only = static_cast<volatile std::uint32_t*>(p_whole)[0] == 0x5ca1ab1e;
                    ps_host_vm_unmap(p_again, 4096);
                }
            }
            // The game's code works on this memory: an uncached CPU mapping would crawl.
            if (is_ok) {
                const double speed =
                    MappedReadSpeed(static_cast<const volatile std::uint64_t*>(p_whole), Size);
                std::printf("Guest memory: CPU reads through the driver's dma-buf: %.0f MB/s\n",
                            speed);
                s_detected.dmabuf_read_mbps = speed;
                if (speed < MinCachedReadMBps) {
                    std::printf("Guest memory: CPU reads of the driver's dma-buf run at %.0f MB/s "
                                "(uncached?): not used\n",
                                speed);
                    is_ok = false;
                    s_is_whole_only = false;
                    s_detected.sz_dmabuf = "slow";
                }
            }
            if (p_whole) {
                ps_host_vm_unmap(p_whole, Size);
            }
            if (p_page) {
                ps_host_vm_unmap(p_page, 4096);
            }
            ps_host_fs_close(h_fd);
        }
        if (memory) {
            dev.freeMemory(memory);
        }
        dev.destroyBuffer(buffer);
        if (is_ok) {
            s_detected.sz_dmabuf = s_is_whole_only ? "whole" : "offsets";
        } else if (s_detected.dmabuf_read_mbps == 0.0) {
            std::printf("Guest memory: the driver's dma-buf cannot be mapped (%s)\n",
                        h_fd >= 0 ? "mmap failed" : "no exportable cached system memory");
            s_detected.sz_dmabuf = "unmappable";
        }
        return is_ok;
    }
}
} // namespace

namespace {
/// The instance's transform feedback usage if buffers over `type` memory lose no external memory
/// feature with it, plain or sparse (the arena), else none: the probes and the chunks rely on
/// creating those buffers.
vk::BufferUsageFlags ExternalXfbUsage(const Vulkan::Instance& instance,
                                      vk::ExternalMemoryHandleTypeFlagBits type) {
    const vk::BufferUsageFlags xfb = instance.GetTransformFeedbackBufferUsage();
    if (!xfb) {
        return {};
    }
    const vk::PhysicalDevice physical = instance.GetPhysicalDevice();
    for (const vk::BufferCreateFlags flags :
         {vk::BufferCreateFlags{},
          vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency}) {
        const auto features = [&](vk::BufferUsageFlags usage) {
            return physical
                .getExternalBufferProperties({.flags = flags, .usage = usage, .handleType = type})
                .externalMemoryProperties.externalMemoryFeatures;
        };
        const auto without = features(ChunkUsage);
        if ((features(ChunkUsage | xfb) & without) != without) {
            std::printf("Guest memory: %s memory does not take transform feedback buffers\n",
                        vk::to_string(type).c_str());
            return {};
        }
    }
    return xfb;
}
} // namespace

namespace {
/// The one line that says what the device offers the game's memory and what Usable chose.
void PrintDetected(const Vulkan::Instance& instance, bool is_usable) {
    u64 cb_device_local = 0, cb_rebar = 0, cb_host_visible = 0;
    for (std::uint32_t i = 0; i < s_memory_properties.memoryTypeCount; ++i) {
        const auto flags = s_memory_properties.memoryTypes[i].propertyFlags;
        const vk::MemoryHeap& heap = s_memory_properties.memoryHeaps[s_memory_properties.memoryTypes[i].heapIndex];
        const bool is_local_heap = static_cast<bool>(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
        const bool is_host_visible = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eHostVisible);
        if (is_local_heap) {
            cb_device_local = std::max<u64>(cb_device_local, heap.size);
        }
        if (is_host_visible && (flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            cb_rebar = std::max<u64>(cb_rebar, heap.size);
        }
        if (is_host_visible && !is_local_heap) {
            cb_host_visible = std::max<u64>(cb_host_visible, heap.size);
        }
    }
    const vk::PhysicalDeviceType device_type = instance.GetPhysicalDevice().getProperties().deviceType;
    const bool is_uma =
        device_type == vk::PhysicalDeviceType::eIntegratedGpu || device_type == vk::PhysicalDeviceType::eCpu;
    const char* sz_host_import = s_detected.sz_host_import;
    if (std::string_view{sz_host_import} == "untested" && !instance.IsHostMemoryImportSupported()) {
        sz_host_import = "unsupported";
    }
    const char* sz_chunks = !is_usable                ? "none"
                            : s_mode == Mode::HostImport ? "host-import"
                            : s_is_whole_only            ? "dmabuf-whole"
                                                         : "dmabuf";
    const std::string model{instance.GetModelName()};
    std::printf("Guest memory: detected device=\"%s\" driver=%s vram_mib=%llu rebar_mib=%llu host_visible_mib=%llu "
                "uma=%d sparse_binding=%d sparse_residency_buffer=%d sparse_queue=%d sparse_bind_ms=%.1f "
                "dmabuf=%s dmabuf_read_mbps=%.0f host_import=%s backing=%s backing_why=\"%s\" chunks=%s "
                "chunks_why=\"%s\"\n",
                model.c_str(), vk::to_string(instance.GetDriverID()).c_str(),
                (unsigned long long)(cb_device_local >> 20), (unsigned long long)(cb_rebar >> 20),
                (unsigned long long)(cb_host_visible >> 20), is_uma ? 1 : 0, s_detected.has_sparse_binding ? 1 : 0,
                s_detected.has_sparse_residency_buffer ? 1 : 0, s_detected.has_sparse_queue ? 1 : 0,
                s_detected.sparse_bind_ms, s_detected.sz_dmabuf, s_detected.dmabuf_read_mbps, sz_host_import,
                LayerMemory() ? "layer-module" : "sparse-arena", s_detected.sz_backing_why, sz_chunks,
                s_detected.sz_chunks_why);
    const std::string_view dmabuf{s_detected.sz_dmabuf};
    if (dmabuf != "untested") {
        ps_host_feature_probed(PS_HOST_FEATURE_gpu_memory_dmabuf, dmabuf == "offsets" || dmabuf == "whole",
                               s_detected.sz_dmabuf);
    }
    const std::string_view host_import{sz_host_import};
    if (host_import != "untested") {
        ps_host_feature_probed(PS_HOST_FEATURE_gpu_memory_host_import, host_import == "works", sz_host_import);
    }
}
} // namespace

bool Usable(const Vulkan::Instance& instance) {
    static const bool s_is_usable = [&] {
        s_device = instance.GetDevice();
        s_memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
        ChooseLayerDefault(instance);
        s_xfb_dmabuf = ExternalXfbUsage(instance, vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT);
        s_xfb_host =
            ExternalXfbUsage(instance, vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT);
        s_is_usable_checked = true;
        // PS_GUEST_MEMORY=dmabuf|host chooses. Else dma-buf chunks when the driver's dma-buf maps at
        // page offsets at cached speed, host memory import first when it maps at offset 0 only
        // (NVIDIA: a chunk per allocation) or not at all. Host import is not preferred for being
        // reported: RADV reports it and passes the probe, yet the game stalls after 2 s on imported
        // memfd memory.
        const char* sz_forced = std::getenv("PS_GUEST_MEMORY");
        const bool should_force_host = sz_forced && std::string_view{sz_forced} == "host";
        const bool should_force_dmabuf = sz_forced && std::string_view{sz_forced} == "dmabuf";
        const bool is_dmabuf_ok = !should_force_host && DmaBufWorks(instance);
        const char*& sz_why = s_detected.sz_chunks_why;
        bool is_usable = false;
        if (should_force_dmabuf) {
            sz_why = "PS_GUEST_MEMORY=dmabuf";
            is_usable = is_dmabuf_ok;
        } else if (is_dmabuf_ok && !s_is_whole_only) {
            sz_why = "dma-buf maps at page offsets at cached speed";
            is_usable = true;
        } else {
            const char* sz_host_why = "";
            if (HostImportWorks(instance, sz_host_why)) {
                s_mode = Mode::HostImport;
                s_detected.sz_host_import = "works";
                std::printf("Guest memory: host memory imported by the GPU (VK_EXT_external_memory_host, "
                            "alignment %llu)\n",
                            (unsigned long long)s_import_alignment);
                sz_why = should_force_host ? "PS_GUEST_MEMORY=host"
                         : is_dmabuf_ok    ? "dma-buf maps at offset 0 only"
                                           : "dma-buf unusable";
                is_usable = true;
            } else {
                std::printf("Guest memory: host memory import does not work here: %s\n", sz_host_why);
                s_detected.sz_host_import = "fails";
                sz_why = should_force_host ? "PS_GUEST_MEMORY=host and host import fails"
                         : is_dmabuf_ok    ? "host import fails, dma-buf maps at offset 0 only"
                                           : "neither dma-buf nor host import works";
                is_usable = !should_force_host && is_dmabuf_ok;
            }
        }
        if (is_usable && s_mode == Mode::DmaBuf && s_is_whole_only) {
            std::printf("Guest memory: the driver's dma-buf maps at offset 0 only: one chunk per "
                        "direct memory allocation\n");
        }
        PrintDetected(instance, is_usable);
        return is_usable;
    }();
    return s_is_usable;
}

vk::ExternalMemoryHandleTypeFlagBits HandleType() {
    return s_mode == Mode::HostImport ? vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT
                                    : vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
}

vk::BufferUsageFlags TransformFeedbackUsage(const Vulkan::Instance& instance) {
    if (!s_is_usable_checked) {
        return instance.GetTransformFeedbackBufferUsage();
    }
    return s_mode == Mode::HostImport ? s_xfb_host : s_xfb_dmabuf;
}

void Install(const Vulkan::Instance& instance) {
    if (!Usable(instance)) {
        Unsupported("direct memory cannot be shared with this driver");
    }
    const bool is_host = s_mode == Mode::HostImport;
    // One chunk per direct allocation where the dma-buf maps at offset 0 only, and with the
    // layer's memory module (a mapping then lies whole in one chunk buffer).
    runtime_memory_set_guest_chunk_whole(LayerMemory() || (!is_host && s_is_whole_only) ? 1 : 0);
    runtime_memory_set_guest_chunk_allocator(is_host ? &AllocImported : &AllocChunk);
    std::printf("Guest memory: direct memory chunks come from Vulkan\n");
}

const Chunk* Find(std::uint64_t phys) {
    std::scoped_lock lk{s_mutex};
    // The last chunk starting at or below phys.
    std::size_t lo = 0, hi = s_c_chunks;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (s_chunks[mid]->phys <= phys) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return nullptr;
    }
    const Chunk* p_chunk = s_chunks[lo - 1];
    return phys < p_chunk->phys + p_chunk->cb_chunk ? p_chunk : nullptr;
}
} // namespace PsGuestMemory
