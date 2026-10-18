// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <numeric>

#include "common/alignment.h"
#include "video_core/buffer_cache/buffer.h"
#include "psport_guest_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryType type) {
    return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
    case MemoryType::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryType::HostCached:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryType::DeviceLocal:
    default:
        return {};
    }
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryType type) {
    switch (type) {
    case MemoryType::DeviceLocal:
    case MemoryType::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryType::HostUncached:
    case MemoryType::HostCached:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    default:
        return VMA_MEMORY_USAGE_UNKNOWN;
    }
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (allocation) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    } else if (buffer) {
        device.destroyBuffer(buffer);
    }
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bda_addr = 0;
}

bool UniqueBuffer::Create(vk::BufferCreateInfo& buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo* out_alloc_info, bool can_exceed_budget) {
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    if (mem_type != MemoryType::Sparse) {
        const VmaAllocationCreateFlags bda_flag =
            with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
        VmaAllocationCreateInfo alloc_ci = {
            .flags =
                VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(mem_type),
            .usage = MemoryUsageVma(mem_type),
            .requiredFlags = 0,
            .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
        };

        const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
        VkBuffer unsafe_buffer{};
        VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                          &allocation, out_alloc_info);
        if (result != VK_SUCCESS) {
            // psport: over the budget, as images do (TextureCache::CreateImage), while the texture
            // collector evicts by submissions to give the memory back.
            if (mem_type == MemoryType::DeviceLocal || mem_type == MemoryType::Stream) {
                TextureCache::NoteVramShort();
            }
            if (!can_exceed_budget) {
                allocation = {};
                return false;
            }
            alloc_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                     &allocation, out_alloc_info);
        }
        if (result != VK_SUCCESS) {
            allocation = {};
            LOG_ERROR_COUNTED(Render_Vulkan, "Failed allocating a {} buffer of {:#x} bytes: {}",
                              BufferTypeName(mem_type), buffer_ci.size,
                              vk::to_string(vk::Result{result}));
            return false;
        }
        buffer = vk::Buffer{unsafe_buffer};
    } else {
        buffer_ci.flags |=
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
        // Bound to exported guest memory chunks as well as VRAM blocks.
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = PsGuestMemory::HandleType(),
        };
        buffer_ci.pNext = &external;
        // psport: a failure leaves the buffer null (Buffer::IsAllocated); GetArena returns null
        // and its callers go on without the arena.
        const auto [result, created] = device.createBuffer(buffer_ci);
        if (result != vk::Result::eSuccess) {
            LOG_ERROR_COUNTED(Render_Vulkan,
                              "Sparse arena of {:#x} bytes not created: it cannot take exported "
                              "memory ({})",
                              buffer_ci.size, vk::to_string(result));
            return false;
        }
        // psport: VRAM blocks are bound into it too: the external memory type must not keep
        // device-local memory out (a driver may restrict it).
        const auto reqs = device.getBufferMemoryRequirements(created);
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(allocator, &props);
        bool vram = false;
        for (u32 i = 0; i < props->memoryTypeCount; ++i) {
            vram |= (reqs.memoryTypeBits & (1u << i)) &&
                    (props->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (!vram) {
            device.destroyBuffer(created);
            LOG_ERROR_COUNTED(Render_Vulkan,
                              "Sparse arena of {:#x} bytes not created: it cannot take VRAM",
                              buffer_ci.size);
            return false;
        }
        buffer = created;
    }

    if (with_bda) {
        const vk::BufferDeviceAddressInfo bda_info = {
            .buffer = buffer,
        };
        bda_addr = device.getBufferAddress(bda_info);
        if (bda_addr == 0) {
            LOG_ERROR_COUNTED(Render_Vulkan, "A {} buffer of {:#x} bytes has no device address",
                              BufferTypeName(mem_type), buffer_ci.size);
            Destroy();
            return false;
        }
    }
    return true;
}

Buffer::Buffer(const Vulkan::Instance& instance, u64 size_bytes_, vk::DeviceMemory memory,
               std::string_view debug_name)
    : size_bytes{size_bytes_}, mem_type{MemoryType::HostCached},
      buffer{instance.GetDevice(), instance.GetAllocator()} {
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = PsGuestMemory::HandleType(),
    };
    // psport PS_LAYER_MEMORY: what the GPU binds for the game's memory in place (no arena):
    // vertex, index and indirect data and device addresses too (the memory has them).
    const bool layer = PsGuestMemory::LayerMemory();
    vk::BufferUsageFlags usage =
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer;
    if (layer) {
        usage |= vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eIndirectBuffer |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress |
                 PsGuestMemory::TransformFeedbackUsage(instance);
    }
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external,
        .size = size_bytes,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto device = instance.GetDevice();
    buffer.buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    if (!buffer.buffer) {
        return;
    }
    if (const auto result = device.bindBufferMemory(buffer.buffer, memory, 0);
        result != vk::Result::eSuccess) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Binding {} failed: {}", debug_name, vk::to_string(result));
        buffer.Destroy();
        return;
    }
    if (layer) {
        buffer.bda_addr = device.getBufferAddress({.buffer = buffer.buffer});
    }
    Vulkan::SetObjectName(device, Handle(), debug_name);
    is_coherent = true;
}

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name, bool can_exceed_budget)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {

    // An in-place arena is bound to external memory, which may not take stream-out usage.
    const vk::BufferUsageFlags xfb_usage = mem_type == MemoryType::Sparse
                                               ? PsGuestMemory::TransformFeedbackUsage(instance)
                                               : instance.GetTransformFeedbackBufferUsage();
    vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = AllFlags | xfb_usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    // psport: arenas are also read by the readback queue (BufferCache::DownloadMemory).
    const std::array<u32, 2> families = {instance.GetGraphicsQueueFamilyIndex(),
                                         instance.GetReadbackQueueFamilyIndex()};
    if (mem_type == MemoryType::Sparse && instance.GetReadbackQueue()) {
        buffer_ci.sharingMode = vk::SharingMode::eConcurrent;
        buffer_ci.queueFamilyIndexCount = static_cast<u32>(families.size());
        buffer_ci.pQueueFamilyIndices = families.data();
    }
    VmaAllocationInfo alloc_info{};
    if (!buffer.Create(buffer_ci, mem_type, &alloc_info, can_exceed_budget)) {
        return;
    }

    const auto device = instance.GetDevice();
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }

    if (mem_type != MemoryType::Sparse) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes, bool can_exceed_budget)
    : Buffer{instance, 0, size_bytes, mem_type, "", can_exceed_budget}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    if (!IsAllocated()) {
        return;
    }
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    if (!IsAllocated()) {
        return false;
    }
    if (!mapped_data.empty() && !is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    u8* const data = mapped_data.empty() ? nullptr : mapped_data.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit() {
    if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler.Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
