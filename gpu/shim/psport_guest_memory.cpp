// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_guest_memory.h"

#include <array>
#include <chrono>
#include <string>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "pslayer_gpu_memory.h"

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

/// The runtime's chunk allocator: a dma-buf fd of `size` bytes of GPU-visible memory, or -1.
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
        s_chunks[i] = new Chunk{phys, cb_range, buffer, memory, s_i_next++};
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
    const int h_fd = memfd_create("psport-guest-memory-probe", MFD_CLOEXEC);
    if (h_fd < 0 || ftruncate(h_fd, Size) != 0) {
        why = "memfd";
        if (h_fd >= 0) {
            close(h_fd);
        }
        return false;
    }
    auto* p_view = static_cast<std::uint32_t*>(
        mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0));
    auto* p_guest = static_cast<std::uint32_t*>(
        mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0));
    close(h_fd);
    bool is_ok = false;
    vk::Buffer buffer, sparse;
    vk::DeviceMemory memory;
    vk::Result error{};
    if (p_view == MAP_FAILED || p_guest == MAP_FAILED) {
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
    if (p_view != MAP_FAILED) {
        munmap(p_view, Size);
    }
    if (p_guest != MAP_FAILED) {
        munmap(p_guest, Size);
    }
    return is_ok;
}

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
} // namespace

bool PcModelGpu(const Vulkan::Instance& instance) {
    static const bool s_is_ok = [&] {
        // The layer's memory module on every GPU; the startup checks in Usable() decide whether
        // the driver can do it. The sparse arena (PS_LAYER_MEMORY=0) is for AMD; not on NVIDIA
        // (only with PS_PC_MODEL_ANY_GPU=1):
        // its host memory import passes those checks, but each rebinding of the game's memory (vkQueueBindSparse, blocks moved to and from VRAM) held the GPU for 10 s
        // and more, the desktop frozen with it (GTX 1660 Ti, driver 615.71). The Linux driver's
        // sparse binding has slowed down since 555: the time grows with the pages already bound
        // (NVIDIA developer forum, "Sparse texture binding is painfully slow"), and the arena has
        // gigabytes of 64 KiB pages bound. Without rebinding (PS_FIXED_ARENA=1, all in place) the
        // GPU reads the game's data over the bus: 48 FPS against 210 on an RX 7800 XT.
        constexpr std::uint32_t AmdVendor = 0x1002, NvidiaVendor = 0x10de;
        const std::uint32_t vendor = instance.GetVendorID();
        const char* sz_any = std::getenv("PS_PC_MODEL_ANY_GPU");
        // AMD keeps the sparse arena, every other GPU gets the layer's memory module (no sparse
        // binding of the game's memory, see LayerMemory); PS_LAYER_MEMORY=1/0 by hand. On an
        // RX 7800 XT the module was on par with the arena on a route in Yharnam, but in the
        // Hunter's Dream it kept ~1 GB of the area's data out of VRAM (ranges with a few
        // unannounced blocks stay in place without volatile blocks): 100 FPS against ~200.
        s_layer_default.store(vendor == AmdVendor ? 0 : 1, std::memory_order_relaxed);
        if (LayerMemory()) {
            std::printf("Guest memory: the new memory model through the layer's memory module on "
                        "this GPU (vendor 0x%04x): no sparse binding of the game's memory\n",
                        vendor);
        }
        if (vendor == AmdVendor || LayerMemory() || (sz_any && sz_any[0] == '1')) {
            return true;
        }
        std::printf("Guest memory: the new memory model is for AMD GPUs%s; this GPU (vendor "
                    "0x%04x) uses the model of 0.3 (PS_PC_MODEL_ANY_GPU=1: try it)\n",
                    vendor == NvidiaVendor ? " (NVIDIA: rebinding memory holds the GPU for seconds)"
                                           : "",
                    vendor);
        return false;
    }();
    return s_is_ok;
}

bool HostImported() {
    return s_mode == Mode::HostImport;
}

bool LayerMemory() {
    // PS_LAYER_MEMORY=1/0 chooses; else PcModelGpu (which asks first) sets the module for every GPU
    // (PS_LAYER_MEMORY=0: the sparse arena; NVIDIA's sparse binding stalls for seconds).
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
            void* p_whole = mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0);
            void* p_page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, Offset);
            // PS_GUEST_WHOLE_CHUNKS=1: as if the offset mapping failed (tests the NVIDIA path).
            const char* sz_force_whole = std::getenv("PS_GUEST_WHOLE_CHUNKS");
            if (sz_force_whole && sz_force_whole[0] == '1' && p_page != MAP_FAILED) {
                munmap(p_page, 4096);
                p_page = MAP_FAILED;
            }
            if (p_whole != MAP_FAILED && p_page != MAP_FAILED) {
                *static_cast<volatile std::uint32_t*>(p_page) = 0x5ca1ab1e;
                is_ok = static_cast<volatile std::uint32_t*>(p_whole)[Offset / 4] == 0x5ca1ab1e;
            } else if (p_whole != MAP_FAILED) {
                // Offset 0 only (NVIDIA): a second mapping of the start must alias the first.
                void* p_again = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0);
                if (p_again != MAP_FAILED) {
                    *static_cast<volatile std::uint32_t*>(p_again) = 0x5ca1ab1e;
                    is_ok = s_is_whole_only = static_cast<volatile std::uint32_t*>(p_whole)[0] == 0x5ca1ab1e;
                    munmap(p_again, 4096);
                }
            }
            // The game's code works on this memory: an uncached CPU mapping would crawl.
            if (is_ok) {
                const double speed =
                    MappedReadSpeed(static_cast<const volatile std::uint64_t*>(p_whole), Size);
                std::printf("Guest memory: CPU reads through the driver's dma-buf: %.0f MB/s\n",
                            speed);
                if (speed < 1000.0) {
                    std::printf("Guest memory: CPU reads of the driver's dma-buf run at %.0f MB/s "
                                "(uncached?): not used\n",
                                speed);
                    is_ok = false;
                    s_is_whole_only = false;
                }
            }
            if (p_whole != MAP_FAILED) {
                munmap(p_whole, Size);
            }
            if (p_page != MAP_FAILED) {
                munmap(p_page, 4096);
            }
            close(h_fd);
        }
        if (memory) {
            dev.freeMemory(memory);
        }
        dev.destroyBuffer(buffer);
        if (!is_ok) {
            std::printf("Guest memory: the driver's dma-buf cannot be mapped (%s)\n",
                        h_fd >= 0 ? "mmap failed" : "no exportable cached system memory");
        } else if (s_is_whole_only) {
            std::printf("Guest memory: the driver's dma-buf maps at offset 0 only: one chunk per "
                        "direct memory allocation\n");
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

bool Usable(const Vulkan::Instance& instance) {
    static const bool s_is_usable = [&] {
        s_device = instance.GetDevice();
        s_memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
        s_xfb_dmabuf = ExternalXfbUsage(instance, vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT);
        s_xfb_host =
            ExternalXfbUsage(instance, vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT);
        s_is_usable_checked = true;
        // PS_GUEST_MEMORY=dmabuf|host chooses; else AMD uses dma-buf chunks (tested the most) and
        // other GPUs host memory import first (NVIDIA: its dma-buf maps at offset 0 only, and
        // CPU access through it may be slow).
        const char* sz_forced = std::getenv("PS_GUEST_MEMORY");
        const bool should_force_host = sz_forced && std::string_view{sz_forced} == "host";
        const bool should_force_dmabuf = sz_forced && std::string_view{sz_forced} == "dmabuf";
        const bool is_host_first =
            should_force_host || (!should_force_dmabuf && instance.GetVendorID() != 0x1002);
        const char* sz_why = "";
        if (is_host_first && HostImportWorks(instance, sz_why)) {
            s_mode = Mode::HostImport;
            std::printf("Guest memory: host memory imported by the GPU (VK_EXT_external_memory_host, "
                        "alignment %llu)\n",
                        (unsigned long long)s_import_alignment);
            return true;
        }
        if (is_host_first) {
            std::printf("Guest memory: host memory import does not work here: %s\n", sz_why);
            if (should_force_host) {
                return false;
            }
        }
        s_mode = Mode::DmaBuf;
        return DmaBufWorks(instance);
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
    // PS_GUEST_IN_PLACE (the GPU uses this memory in place) needs it too.
    const char* sz_env = std::getenv("PS_GUEST_GPU_MEMORY");
    const char* sz_in_place = std::getenv("PS_GUEST_IN_PLACE");
    if (!(sz_env && sz_env[0] == '1') && !(sz_in_place && sz_in_place[0] == '1' && PcModelGpu(instance))) {
        return;
    }
    if (!Usable(instance)) {
        std::printf("Guest memory: direct memory stays in the memfd\n");
        return;
    }
    const bool is_host = s_mode == Mode::HostImport;
    // One chunk per direct allocation where the dma-buf maps at offset 0 only, and with the
    // layer's memory module (a mapping then lies whole in one chunk buffer).
    runtime_memory_set_guest_chunk_whole(LayerMemory() || (!is_host && s_is_whole_only) ? 1 : 0);
    runtime_memory_set_guest_chunk_allocator(is_host ? &AllocImported : &AllocChunk);
    std::printf("Guest memory: direct memory chunks come from Vulkan (PS_GUEST_GPU_MEMORY=1)\n");
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
