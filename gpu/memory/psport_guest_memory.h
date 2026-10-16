// SPDX-License-Identifier: GPL-2.0-or-later
// psport: guest direct memory in GPU-visible system memory.
//
// The runtime asks for direct memory in chunks (runtime_memory.c, CHUNK); each chunk is a
// Vulkan allocation of cached system memory, exported as a dma-buf that the runtime maps at the
// game's addresses and in its backing view, or the runtime's own memfd imported by the GPU. The
// GPU reads the game's data in place: no CPU copy into staging buffers before an upload. A chunk
// that cannot be made stays in the runtime's memfd.
#pragma once

#include <cstdint>
#include <string>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace PsGuestMemory {
struct Chunk {
    std::uint64_t phys = 0; ///< first byte of direct memory it holds
    std::uint64_t cb_chunk = 0;
    vk::Buffer buffer;      ///< the whole chunk, for transfers
    vk::DeviceMemory memory;
    std::uint32_t i_creation = 0; ///< creation order (stable key)
};

/// Whether the runtime can use such chunks: host memory the GPU imports, or dma-buf export and mmap
/// of the dma-buf, checked once with a small chunk. Asked first: it also settles LayerMemory() for
/// this GPU. A driver that maps at page offsets gets 256 MiB chunks (the game's mappings
/// start inside them); NVIDIA's dma-buf maps at offset 0 only (the grid made every mapping after
/// the first fail and the game panic at startup), so there each direct allocation is its own chunk:
/// the game maps its allocations whole, at offset 0.
bool Usable(const Vulkan::Instance& instance);
/// Ends the process with `why`: the GPU cannot use the game's memory in place, and the port has no
/// other memory model.
[[noreturn]] void Unsupported(const std::string& why);
/// Whether the chunks are imported host memory (the dma-buf maps at offset 0 only or not at all,
/// or PS_GUEST_MEMORY=host). Valid after Usable(). The sparse arena then keeps the first binding of every block (BufferCache FixedArena).
bool HostImported();
/// psport PS_LAYER_MEMORY=1: the layer's memory module (gpu/layer/pslayer_gpu_memory.h) gives the
/// GPU the game's memory through the chunk buffers instead of a sparse arena. Chunks are then one
/// per direct memory allocation (a mapping lies whole in one chunk), their buffers take every use
/// (vertex, index, indirect, texel, device address) and are registered with the module.
bool LayerMemory();
/// The external memory handle type of the chunks (dma-buf or imported host memory): buffers bound
/// to them (the arena, chunk buffers) are created with it. Valid after Usable().
vk::ExternalMemoryHandleTypeFlagBits HandleType();
/// Transform feedback usage for buffers the GPU binds over guest memory: the instance's, unless
/// the driver reports that the chunks' external memory type loses a feature with it (the arena and
/// chunk buffers then go without, as before). Stream-out must check it before writing to them.
vk::BufferUsageFlags TransformFeedbackUsage(const Vulkan::Instance& instance);
/// Hands the runtime direct memory chunks from now on.
void Install(const Vulkan::Instance& instance);
/// The chunk holding direct memory address `phys`, or null.
const Chunk* Find(std::uint64_t phys);
} // namespace PsGuestMemory
