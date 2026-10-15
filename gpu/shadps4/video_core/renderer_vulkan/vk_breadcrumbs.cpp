// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" int runtime_memory_vma_info(uintptr_t va_guest, int* p_prot, int* p_type, uintptr_t* p_end);

#include <vk_mem_alloc.h>

namespace Vulkan::Breadcrumbs {

namespace {

constexpr u32 MaxStreams = 16;
constexpr u32 StreamStride = 64; // bytes of the marker buffer per stream (a cache line)
constexpr u64 RingSize = 1u << 15;
constexpr u32 ArgSlots = 4096;                     // indirect dispatch arguments, 16 bytes each
constexpr u64 ArgsOffset = MaxStreams * StreamStride; // where they start in the buffer

struct Entry {
    u64 id;
    Crumb crumb;
};

struct Stream {
    const char* sz_name = nullptr;
    std::atomic<u64> next{1};
    std::unique_ptr<Entry[]> ring;
};

std::array<Stream, MaxStreams> s_streams;
std::atomic<u32> s_c_streams{0};
bool s_is_enabled = false;
std::once_flag s_init_once;
vk::Buffer s_marker_buffer{};
VmaAllocation s_p_marker_allocation{};
volatile u32* s_p_markers = nullptr;
volatile u32* s_p_args = nullptr;
std::atomic<u32> s_next_args_slot{0};
/// What each argument slot was last used for.
struct ArgsNote {
    u64 id;
    u32 stream;
    u64 hash;
};
std::array<ArgsNote, ArgSlots> s_args_notes{};

void Init(const Instance& instance) {
    const char* sz_env = std::getenv("PS_BREADCRUMBS");
    if (sz_env && sz_env[0] == '0') {
        return;
    }
    if (!instance.IsBufferMarkerSupported()) {
        std::printf("GPU breadcrumbs: VK_AMD_buffer_marker unavailable, GPU hangs will not be "
                    "located\n");
        return;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ArgsOffset + ArgSlots * 16,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBuffer h_buffer{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &h_buffer,
                        &s_p_marker_allocation, &info) != VK_SUCCESS ||
        !info.pMappedData) {
        std::printf("GPU breadcrumbs: no host-visible buffer, GPU hangs will not be located\n");
        return;
    }
    s_marker_buffer = vk::Buffer{h_buffer};
    s_p_markers = static_cast<volatile u32*>(info.pMappedData);
    for (u32 i = 0; i < (ArgsOffset + ArgSlots * 16) / sizeof(u32); ++i) {
        s_p_markers[i] = 0;
    }
    s_p_args = s_p_markers + ArgsOffset / sizeof(u32);
    s_is_enabled = true;
}

/// Guest memory that may be gone by now: read only where the runtime has it mapped (its direct
/// memory is a GPU driver mapping, which process_vm_readv cannot read).
size_t ReadGuest(u64 va_guest, void* p_out, size_t cb_out) {
    int prot = 0, type = 0;
    uintptr_t end = 0;
    if (!va_guest || !runtime_memory_vma_info(va_guest, &prot, &type, &end) || end <= va_guest) {
        return 0;
    }
    cb_out = std::min<size_t>(cb_out, end - va_guest);
    std::memcpy(p_out, reinterpret_cast<const void*>(va_guest), cb_out);
    return cb_out;
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Pass:
        return "pass";
    case Kind::Draw:
        return "draw";
    case Kind::DrawIndexed:
        return "indexed draw";
    case Kind::DrawIndirect:
        return "indirect draw";
    case Kind::Dispatch:
        return "dispatch";
    case Kind::DispatchIndirect:
        return "indirect dispatch";
    }
    return "?";
}

void PrintEntry(const Stream& stream, u64 id, const char* sz_label) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        std::printf("    #%llu %s: no longer kept\n", (unsigned long long)id, sz_label);
        return;
    }
    const Crumb& c = entry.crumb;
    std::printf("    #%llu %s: %s", (unsigned long long)id, sz_label, KindName(c.kind));
    switch (c.kind) {
    case Kind::Pass:
        std::printf(" %s", c.sz_name ? c.sz_name : "?");
        break;
    case Kind::Draw:
    case Kind::DrawIndexed:
        std::printf(" vs %016llx ps %016llx, %u %s x %u instances", (unsigned long long)c.hash[0],
                    (unsigned long long)c.hash[1], c.count[0],
                    c.kind == Kind::Draw ? "vertices" : "indices", c.count[1]);
        break;
    case Kind::DrawIndirect: {
        std::printf(" vs %016llx ps %016llx, up to %u draws, stride %u, arguments at %#llx",
                    (unsigned long long)c.hash[0], (unsigned long long)c.hash[1], c.count[0],
                    c.count[1], (unsigned long long)c.va_arguments);
        u32 now[5]{};
        if (ReadGuest(c.va_arguments, now, sizeof(now)) == sizeof(now)) {
            std::printf(" (guest memory now %u %u %u %u %u)", now[0], now[1], now[2], now[3], now[4]);
        }
        break;
    }
    case Kind::Dispatch:
        std::printf(" cs %016llx, %ux%ux%u groups", (unsigned long long)c.hash[0], c.count[0],
                    c.count[1], c.count[2]);
        break;
    case Kind::DispatchIndirect: {
        std::printf(" cs %016llx, arguments at %#llx", (unsigned long long)c.hash[0],
                    (unsigned long long)c.va_arguments);
        if (c.args_slot < ArgSlots) {
            const volatile u32* p_read = s_p_args + c.args_slot * 4;
            std::printf(" (the GPU read %ux%ux%u groups)", p_read[0], p_read[1], p_read[2]);
        }
        std::printf(c.count[0] ? ", in place" : ", a copy");
        u32 now[3]{};
        if (ReadGuest(c.va_arguments, now, sizeof(now)) == sizeof(now)) {
            std::printf(" (guest memory now %ux%ux%u)", now[0], now[1], now[2]);
        }
        break;
    }
    }
    for (u32 i = 0; i < 2; ++i) {
        if (c.program[i]) {
            std::printf(", code %#llx", (unsigned long long)c.program[i]);
        }
    }
    std::printf("\n");
}

/// The guest code of the shaders of the stuck command, for disassembly (up to 64 KiB each:
/// the program and what follows, its OrbShdr header among it).
void DumpPrograms(const Stream& stream, u64 id) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        return;
    }
    const char* sz_dir = access("out", W_OK) == 0 ? "out/" : "";
    for (u32 i = 0; i < 2; ++i) {
        const u64 program = entry.crumb.program[i];
        if (!program) {
            continue;
        }
        std::array<u8, 64 * 1024> code{};
        size_t cb_code = 0;
        // Page by page: the read stops at the first page that is not mapped.
        while (cb_code < code.size()) {
            const size_t chunk = std::min<size_t>(4096 - ((program + cb_code) & 4095), code.size() - cb_code);
            const size_t read = ReadGuest(program + cb_code, code.data() + cb_code, chunk);
            cb_code += read;
            if (read != chunk) {
                break;
            }
        }
        char sz_path[256];
        std::snprintf(sz_path, sizeof(sz_path), "%shang_shader_%016llx.bin", sz_dir,
                      (unsigned long long)entry.crumb.hash[i]);
        if (FILE* p_file = std::fopen(sz_path, "wb")) {
            std::fwrite(code.data(), 1, cb_code, p_file);
            std::fclose(p_file);
            std::printf("    shader %016llx code (%zu bytes from %#llx) saved to %s\n",
                        (unsigned long long)entry.crumb.hash[i], cb_code,
                        (unsigned long long)program, sz_path);
        }
    }
}

} // Anonymous namespace

u32 NewStream(const Instance& instance, const char* sz_name) {
    std::call_once(s_init_once, [&] { Init(instance); });
    const u32 i_stream = s_c_streams.fetch_add(1, std::memory_order_relaxed);
    if (i_stream >= MaxStreams) {
        return MaxStreams;
    }
    s_streams[i_stream].sz_name = sz_name;
    s_streams[i_stream].ring = std::make_unique<Entry[]>(RingSize);
    return i_stream;
}

bool Enabled() noexcept {
    return s_is_enabled;
}

u32 Note(u32 i_stream, const Crumb& crumb) {
    if (i_stream >= MaxStreams) {
        return 0;
    }
    Stream& stream = s_streams[i_stream];
    const u64 id = stream.next.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = stream.ring[id & (RingSize - 1)];
    entry.crumb = crumb;
    entry.id = id;
    if (crumb.args_slot < ArgSlots) {
        s_args_notes[crumb.args_slot] = {id, i_stream, crumb.hash[0]};
    }
    return u32(id);
}

void Mark(vk::CommandBuffer cmdbuf, u32 stream, u32 id, bool is_after) {
    if (!s_is_enabled || stream >= MaxStreams) {
        return;
    }
    cmdbuf.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, s_marker_buffer,
                                stream * StreamStride, id * 2 + (is_after ? 1 : 0));
}

u32 ArgsSlot() {
    return s_is_enabled ? s_next_args_slot.fetch_add(1, std::memory_order_relaxed) % ArgSlots : ~0u;
}

void CopyArgs(vk::CommandBuffer cmdbuf, vk::Buffer source, u64 off_source, u32 slot) {
    if (!s_is_enabled || slot >= ArgSlots) {
        return;
    }
    cmdbuf.copyBuffer(source, s_marker_buffer,
                      vk::BufferCopy{off_source, ArgsOffset + u64(slot) * 16, 3 * sizeof(u32)});
}

namespace {
void Report(const char* sz_title, const char* sz_where);
}

void ReportDeviceLost(const char* sz_where) {
    static std::atomic_flag s_reported = ATOMIC_FLAG_INIT;
    if (s_is_enabled && !s_reported.test_and_set()) {
        Report("at device lost", sz_where);
    }
}

void ReportStuck(const char* sz_where) {
    static std::atomic_flag s_reported = ATOMIC_FLAG_INIT;
    if (s_is_enabled && !s_reported.test_and_set()) {
        Report("while the GPU makes no progress", sz_where);
    }
}

namespace {
void Report(const char* sz_title, const char* sz_where) {
    std::printf("GPU breadcrumbs %s (%s): what each command stream finished\n", sz_title, sz_where);
    const u32 c_streams = std::min(s_c_streams.load(), MaxStreams);
    for (u32 s = 0; s < c_streams; ++s) {
        const Stream& stream = s_streams[s];
        const u64 last = stream.next.load() - 1;
        const u32 marker = s_p_markers[s * StreamStride / sizeof(u32)];
        std::printf("  stream %u (%s): %llu commands noted", s, stream.sz_name,
                    (unsigned long long)last);
        if (last == 0) {
            std::printf("\n");
            continue;
        }
        if (marker == 0) {
            std::printf(", the GPU finished none\n");
            continue;
        }
        // The marker holds the low 32 bits of 2 * id + after.
        const u64 newest = last * 2 + 1;
        const u64 value = newest - u32(u32(newest) - marker);
        const u64 id = value / 2;
        if (value & 1) {
            if (id == last) {
                std::printf(", the GPU finished all\n");
                continue;
            }
            std::printf(", the GPU finished #%llu but did not start #%llu: stuck in the work "
                        "recorded between them (copies, clears, barriers) or before it\n",
                        (unsigned long long)id, (unsigned long long)(id + 1));
            PrintEntry(stream, id, "finished");
            PrintEntry(stream, id + 1, "next");
        } else {
            std::printf(", the GPU started #%llu and did not finish it (%llu noted after it)\n",
                        (unsigned long long)id, (unsigned long long)(last - id));
            if (id > 1) {
                PrintEntry(stream, id - 1, "finished");
            }
            PrintEntry(stream, id, "STUCK");
            std::fflush(stdout);
            DumpPrograms(stream, id);
        }
        for (u64 next = id + 2; next <= std::min(last, id + 4); ++next) {
            PrintEntry(stream, next, "later");
        }
    }
    std::fflush(stdout);
}
} // Anonymous namespace


void PrintProgress() {
    if (!s_is_enabled) {
        return;
    }
    std::printf("GPU breadcrumbs:");
    const u32 c_streams = std::min(s_c_streams.load(), MaxStreams);
    std::array<u64, MaxStreams> finished{};
    for (u32 s = 0; s < c_streams; ++s) {
        const u64 last = s_streams[s].next.load(std::memory_order_relaxed) - 1;
        if (last == 0) {
            continue;
        }
        const u32 marker = s_p_markers[s * StreamStride / sizeof(u32)];
        const u64 newest = last * 2 + 1;
        const u64 value = marker ? newest - u32(u32(newest) - marker) : 0;
        // 2n + 1: n finished; 2n: n started, n - 1 finished.
        finished[s] = value ? value / 2 - (value & 1 ? 0 : 1) : 0;
        std::printf(" stream %u (%s) %llu noted, %llu unfinished;", s, s_streams[s].sz_name,
                    (unsigned long long)last, (unsigned long long)(last - finished[s]));
    }
    // The largest indirect dispatches among the last ArgSlots the GPU finished, one per shader:
    // counts that only grow from frame to frame end in a dispatch that never finishes.
    struct Largest {
        u64 hash;
        u64 groups;
        u32 dims[3];
    };
    std::array<Largest, 3> largest{};
    for (u32 slot = 0; slot < ArgSlots; ++slot) {
        const ArgsNote note = s_args_notes[slot];
        if (note.id == 0 || note.stream >= c_streams || note.id > finished[note.stream]) {
            continue;
        }
        const u32 x = s_p_args[slot * 4], y = s_p_args[slot * 4 + 1], z = s_p_args[slot * 4 + 2];
        const u64 groups = u64(x) * y * z;
        auto it = std::find_if(largest.begin(), largest.end(),
                               [&](const Largest& l) { return l.hash == note.hash; });
        if (it == largest.end()) {
            it = std::min_element(largest.begin(), largest.end(),
                                  [](const Largest& a, const Largest& b) { return a.groups < b.groups; });
            if (it->groups >= groups) {
                continue;
            }
            *it = {note.hash, 0, {}};
        }
        if (groups >= it->groups) {
            *it = {note.hash, groups, {x, y, z}};
        }
    }
    std::sort(largest.begin(), largest.end(),
              [](const Largest& a, const Largest& b) { return a.groups > b.groups; });
    std::printf(" largest indirect dispatches:");
    for (const auto& l : largest) {
        if (l.groups != 0) {
            std::printf(" cs %016llx %ux%ux%u;", (unsigned long long)l.hash, l.dims[0], l.dims[1],
                        l.dims[2]);
        }
    }
    std::printf("\n");
}

} // namespace Vulkan::Breadcrumbs
