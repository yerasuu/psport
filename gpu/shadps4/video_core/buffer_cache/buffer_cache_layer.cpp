// SPDX-License-Identifier: GPL-2.0-or-later
// psport PS_LAYER_MEMORY: the BufferCache side of the layer's memory module
// (gpu/layer/pslayer_gpu_memory.h, docs/en/history/MEMORY_MODULE_PLAN.md). The game's memory is bound in
// place through its chunk buffers; blocks of known data (what loaders wrote) get VRAM copies in
// mirrors: one buffer per run of such blocks, runs that meet merged into a new mirror. No sparse
// binding: a move is a copy plus the module's bookkeeping, and bindings pick the source.
//
// Step 3a: only loaded data is copied to VRAM, and a range the GPU writes goes back in place
// first, so the game's memory always holds the current data and readers in place (the huge
// bindings over all memory) stay right.

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <sys/mman.h>
#include <map>
#include <tuple>
#include <string>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <fmt/format.h>
#include <xxhash.h>
#include "psport_guest_memory.h"
#include "../../src/platform/ps_host.h"
#include "psport_sections.h"
#include "psport_toggles.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "common/signal_context.h"
#include "core/signals.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
// After the Vulkan headers of the video core (their configuration).
#include "pslayer_gpu_memory.h"
#include "pslayer_write_traps.h"

extern "C" int runtime_memory_vma_info(uintptr_t va_guest, int* p_prot, int* p_type, uintptr_t* p_end);

namespace VideoCore {

namespace {
PsLayer::GpuMemory& Layer() {
    return PsLayer::GpuMemory::Get();
}

bool LayerTrapsOn(); // below, with the traps

/// Guest code (the game's image with its linked modules): the writer behind a trap hit. A hit
/// from our own code (the runtime touching pages before a file read into them) must not set the
/// trap again until that write is done.
bool IsGuestCode(u64 rip) {
    constexpr u64 Image = 0x800000000ull, ImageSize = 128ull << 20;
    return rip >= Image && rip < Image + ImageSize;
}

// Volatile blocks (PS_LAYER_VOLATILE=1; off by default): a range with a few unannounced blocks
// goes into one mirror anyway, those blocks refreshed for every binding that reads them. Off, the
// range stays mixed (in place). Their refreshes were ~300 MB/s of copies and, in render passes,
// up to ~100 pass breaks a frame; on a recorded route off was 4.29-4.31 ms of GPU time a frame,
// on 4.31-4.53 even with the in-pass reads in place, 5.4-7.3 before. They were ~0.1 % of the
// bytes bound.
bool LayerVolatileAllowed() {
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_LAYER_VOLATILE");
        return sz_env && sz_env[0] == '1';
    }();
    return s_is_on;
}
} // namespace

std::optional<std::pair<const Buffer*, u64>> BufferCache::LayerBind(VAddr va_range, u64 cb_range,
                                                                    bool is_written,
                                                                    bool is_texel_buffer,
                                                                    bool can_promote) {
    BB_SECTION(LayerBind);
    // Residency changes queued since the last packet: loads, demotions, unmaps, idle blocks.
    if (maintained_epoch != packet_epoch) {
        Maintain();
    }
    LayerProcessTraps(); // blocks the CPU wrote go back in place before anything binds them
    const u64 first = va_range >> block_shift;
    const u64 end = ((va_range + cb_range - 1) >> block_shift) + 1;
    NoteUse(va_range, cb_range); // idle VRAM blocks go back in place (LayerProcessIdle)
    using Kind = PsLayer::Resolution::Kind;
    // The module's answer for this range while nothing moved (layer_generation: promotions,
    // demotions, unmaps); ranges it does not know yet are asked again.
    struct ResolveMemo {
        VAddr va_range = 0;
        u64 cb_range = 0;
        u64 generation = 0;
        PsLayer::Resolution resolution;
    };
    // On the heap: a static TLS array is reserved on every thread's stack, and the game's threads
    // with small stacks failed to start (pthread_create: EINVAL).
    thread_local auto p_resolve_memo = std::make_unique<std::array<ResolveMemo, 2048>>();
    auto& memo = (*p_resolve_memo)[((va_range * 0x9E3779B97F4A7C15ull) ^ (cb_range * 0xC2B2AE3D27D4EB4Full)) >> 53];
    PsLayer::Resolution resolution;
    if (memo.va_range == va_range && memo.cb_range == cb_range && memo.generation == layer_generation + 1) {
        resolution = memo.resolution;
    } else {
        resolution = Layer().Resolve(va_range, cb_range);
        if (resolution.kind != Kind::None) {
            memo = {va_range, cb_range, layer_generation + 1, resolution};
        }
    }
    // PS_RESIDENCY_MIX=1: bindings per kind (none, in place, mirror, mixed) and written, per 2 s.
    static const bool s_should_count_mix = [] {
        const char* sz_env = std::getenv("PS_RESIDENCY_MIX");
        return sz_env && (sz_env[0] == '1' || sz_env[0] == '2');
    }();
    if (s_should_count_mix) {
        static std::array<std::atomic<u64>, 8> s_c_bindings{}, s_cb_bindings{};
        static std::atomic<u32> s_c_printed{0};
        const size_t k = size_t(resolution.kind) * 2 + (is_written ? 1 : 0);
        // In place: whether all its blocks may go to VRAM (else: why not, by block).
        static std::array<std::atomic<u64>, 4> s_cb_in_place_why{}; // eligible, asset gap, dynamic, chunk
        // Unknown bytes, by why not watched (PS_LAYER_WATCH): watchable, the GPU's dynamic,
        // volatile, failed before, mapping smaller than a block, written binding.
        static std::array<std::atomic<u64>, 6> s_cb_unwatched_why{};
        if (resolution.kind == Kind::InPlace) {
            int why = 0;
            for (u64 block = first; block < end && why == 0; ++block) {
                const VAddr a = block << block_shift;
                bool is_known = true;
                asset_bytes.ForEachGap(a, a + block_size, [&](u64 s, u64 e) {
                    is_known = is_known && gpu_written_bytes.Contains(s, e);
                });
                why = !is_known ? 1 : dynamic_blocks.Contains(block) ? 2
                      : !Layer().ResolveInPlace(a, block_size) ? 3 : 0;
                if (why == 1) {
                    int prot = 0, type = -1;
                    uintptr_t vma_end = 0;
                    const int w = is_written ? 5
                                  : dynamic_blocks.Contains(block) &&
                                            !layer_cpu_hot.Contains(block, block + 1) ? 1
                                  : layer_volatile.Contains(block, block + 1) ? 2
                                  : layer_watch_failed.Contains(block, block + 1) ? 3
                                  : (runtime_memory_vma_info(a, &prot, &type, &vma_end) &&
                                     (prot & 0x2) && vma_end < a + block_size) ? 4 : 0;
                    s_cb_unwatched_why[w].fetch_add(cb_range, std::memory_order_relaxed);
                }
            }
            s_cb_in_place_why[why].fetch_add(cb_range, std::memory_order_relaxed);
        }
        static std::atomic<u32> s_c_why_printed{0};
        if (PsStats::g_coarse_second.load() - s_c_why_printed.load() >= 2) {
            s_c_why_printed.store(PsStats::g_coarse_second.load());
            std::printf("Layer in place (MiB/2 s): eligible %llu, unknown bytes %llu, dynamic %llu, "
                        "not a whole chunk block %llu; unknown bytes not watched: watchable %llu, "
                        "GPU-dynamic %llu, volatile %llu, failed %llu, mapping %llu, written %llu\n",
                        (unsigned long long)(s_cb_in_place_why[0].exchange(0) >> 20),
                        (unsigned long long)(s_cb_in_place_why[1].exchange(0) >> 20),
                        (unsigned long long)(s_cb_in_place_why[2].exchange(0) >> 20),
                        (unsigned long long)(s_cb_in_place_why[3].exchange(0) >> 20),
                        (unsigned long long)(s_cb_unwatched_why[0].exchange(0) >> 20),
                        (unsigned long long)(s_cb_unwatched_why[1].exchange(0) >> 20),
                        (unsigned long long)(s_cb_unwatched_why[2].exchange(0) >> 20),
                        (unsigned long long)(s_cb_unwatched_why[3].exchange(0) >> 20),
                        (unsigned long long)(s_cb_unwatched_why[4].exchange(0) >> 20),
                        (unsigned long long)(s_cb_unwatched_why[5].exchange(0) >> 20));
        }
        // PS_RESIDENCY_MIX=2: also the bindings in place or mixed with the most bytes, and per
        // range its blocks in VRAM, eligible, dynamic and with unannounced bytes.
        static const bool s_should_list_mix_top = std::getenv("PS_RESIDENCY_MIX")[0] == '2';
        if (s_should_list_mix_top && (resolution.kind == Kind::InPlace || resolution.kind == Kind::Mixed)) {
            static std::map<std::tuple<VAddr, u64, bool, int>, u64> s_top;
            static u32 s_c_top_printed = 0;
            s_top[{va_range, cb_range, is_written, int(resolution.kind)}] += cb_range;
            const u32 second = PsStats::g_coarse_second.load(std::memory_order_relaxed);
            if (second - s_c_top_printed >= 4) {
                s_c_top_printed = second;
                std::vector<std::pair<u64, std::tuple<VAddr, u64, bool, int>>> sorted;
                for (const auto& [key, total] : s_top) {
                    sorted.emplace_back(total, key);
                }
                std::ranges::sort(sorted, std::greater{});
                for (size_t i = 0; i < std::min<size_t>(sorted.size(), 12); ++i) {
                    const auto& [total, key] = sorted[i];
                    const auto& [a, s, w, kind] = key;
                    u64 vram = 0, eligible = 0, dynamic = 0, unknown = 0;
                    for (u64 b = a >> block_shift; b <= (a + s - 1) >> block_shift; ++b) {
                        if (Layer().AnyValid(b, b + 1)) {
                            ++vram;
                        } else if (dynamic_blocks.Contains(b)) {
                            ++dynamic;
                        } else if (LayerEligible(b)) {
                            ++eligible;
                        } else {
                            ++unknown;
                        }
                    }
                    std::printf("Layer top: %#llx+%#llx %s %s, %llu MiB in 4 s; blocks: VRAM %llu, "
                                "eligible %llu, dynamic %llu, unannounced %llu\n",
                                (unsigned long long)a, (unsigned long long)s,
                                kind == int(Kind::Mixed) ? "mixed" : "in place",
                                w ? "written" : "read", (unsigned long long)(total >> 20),
                                (unsigned long long)vram, (unsigned long long)eligible,
                                (unsigned long long)dynamic, (unsigned long long)unknown);
                }
                s_top.clear();
            }
        }
        s_c_bindings[k].fetch_add(1, std::memory_order_relaxed);
        s_cb_bindings[k].fetch_add(cb_range, std::memory_order_relaxed);
        const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
        if (now - s_c_printed.load(std::memory_order_relaxed) >= 2) {
            s_c_printed.store(now, std::memory_order_relaxed);
            std::string text;
            constexpr std::array<const char*, 4> names = {"none", "in place", "mirror", "mixed"};
            for (size_t i = 0; i < s_c_bindings.size(); ++i) {
                text += fmt::format(" {}{} {}/{}MiB", names[i / 2], (i & 1) ? " w" : "",
                                    s_c_bindings[i].exchange(0), s_cb_bindings[i].exchange(0) >> 20);
            }
            std::printf("Layer bindings (2 s):%s\n", text.c_str());
        }
    }
    // As the arena gave VRAM to blocks of known data when first bound: a range all of known data
    // (or in VRAM already) is made one mirror now (the missing blocks copied from the game's
    // memory first, in stream order), then bound there. A few blocks with unannounced bytes
    // among many known ones (where the arena mixed VRAM and the game's memory) go into the
    // mirror as volatile blocks, refreshed for every binding that reads them. Not for a range the
    // GPU writes: copying its bytes back after the draw would also put back stale copies of what
    // the CPU (a frame ahead) wrote there meanwhile (exploding vertices).
    // Experiment bit 8 of PS_TOGGLE_FILE (diagnostics, while the game runs): reads bound in place
    // even where a VRAM copy is (a stale copy then shows by going away; nothing is moved).
    if (PsToggle::Experiment(8) && !is_written && resolution.kind == Kind::Mirror) {
        if (const auto in_place = LayerInPlace(va_range, cb_range, true)) {
            return in_place;
        }
    }
    const bool is_vram_allowed =
        !(is_written && (force_writes_in_place || !LayerGpuWriteToVram(va_range, cb_range))) &&
                              !PsToggle::Experiment(8);
    // Not for image uploads (can_promote false): the image is the GPU's copy of that data, read
    // once per upload, and the arena did not move it either. (Loaded data mostly gets its mirror
    // when loaded, ProcessPendingAssets: this alone did not shrink the mirrors.)
    if (resolution.kind != Kind::Mirror && is_vram_allowed && can_promote && GarlicInVramForLayer() &&
        !VramPromotionsPaused()) {
        constexpr u64 VolatileMinBlocks = 4, VolatileMaxBlocks = 2;
        // A range found not movable is looked at again next frame or when something moved
        // (what makes blocks known data changes between frames: loads, GPU writes).
        struct TriedMemo {
            u64 first = 0, end = 0, generation = 0;
            u32 frame = 0;
            bool is_written = false;
        };
        thread_local std::array<TriedMemo, 256> tried{};
        auto& t = tried[(first ^ (end << 7)) & (tried.size() - 1)];
        const u32 frame = static_cast<u32>(PsStats::g_frame_number.load(std::memory_order_relaxed));
        const bool should_skip = t.first == first && t.end == end && t.generation == layer_generation &&
                          t.frame == frame && t.is_written == is_written;
        // Blocks with unannounced bytes are watched copies for a reading binding (PS_LAYER_WATCH);
        // for a writing one only its first and last block when it covers them in part
        // (layer_edge_blocks).
        std::vector<u64> blocks, unknown, watch, edges;
        for (u64 block = first; !should_skip && block < end && unknown.size() <= VolatileMaxBlocks;
             ++block) {
            if (!Layer().AnyValid(block, block + 1)) {
                const bool is_partial = (block << block_shift) < va_range ||
                                        ((block + 1) << block_shift) > va_range + cb_range;
                if (LayerEligible(block)) {
                    blocks.push_back(block);
                } else if (!is_written && LayerWatchable(block)) {
                    blocks.push_back(block);
                    watch.push_back(block);
                } else if (is_written && is_partial && LayerWatchable(block)) {
                    blocks.push_back(block);
                    edges.push_back(block);
                } else {
                    unknown.push_back(block);
                }
            }
        }
        if (!should_skip) {
            t = {first, end, layer_generation, frame, is_written};
        }
        if (!should_skip && (unknown.empty() || (!is_written && end - first >= VolatileMinBlocks &&
                                unknown.size() <= VolatileMaxBlocks && LayerVolatileAllowed() &&
                                !PsToggle::Experiment(16)))) {
            for (const u64 block : watch) {
                layer_watched.Add({block, block + 1});
            }
            for (const u64 block : edges) {
                layer_edge_blocks.Add({block, block + 1});
            }
            LayerPromote(blocks);
            for (const u64 block : edges) {
                if (!Layer().AnyValid(block, block + 1)) {
                    layer_edge_blocks.Subtract(block, block + 1); // not moved (VRAM short)
                }
            }
            for (const u64 block : watch) {
                if (Layer().AnyValid(block, block + 1)) {
                    layer_cpu_blocks.Add({block, block + 1}); // the GPU writes it in place
                } else {
                    layer_watched.Subtract(block, block + 1); // not moved (VRAM short)
                }
            }
            LayerMakeVolatile(unknown);
            resolution = Layer().Resolve(va_range, cb_range);
        }
    }
    if (is_written && resolution.kind == Kind::Mirror && layer_volatile.Overlaps(first, end)) {
        // Volatile blocks go back in place (nothing to copy back: the GPU never writes them in
        // a mirror); the range is mixed now.
        std::vector<std::pair<u64, u64>> runs;
        layer_volatile.ForEachInRange(first, end, [&](const Interval& iv) {
            runs.emplace_back(std::max(first, iv.start), std::min(end, iv.end));
        });
        for (const auto& [a, b] : runs) {
            LayerDemote(a, b, false, 1);
        }
        resolution = Layer().Resolve(va_range, cb_range);
    }
    // A read over volatile blocks while a render pass is open: in place. Their refresh is a copy,
    // and a copy ends the pass: the same pass was stored and loaded again up to ~100 times a frame
    // (light volumes, one draw each), the GPU 1.3-1.5x slower for the whole launch in the launches
    // that made such blocks. Outside a pass (dispatches, the first draw) the mirror is refreshed.
    // PS_LAYER_VOLATILE_IN_PASS=0: refreshed in passes too (as before).
    static const bool s_is_volatile_in_place = [] {
        const char* sz_env = std::getenv("PS_LAYER_VOLATILE_IN_PASS");
        return !(sz_env && sz_env[0] == '0');
    }();
    if (resolution.kind == Kind::Mirror && is_vram_allowed && !is_written && s_is_volatile_in_place &&
        scheduler.IsRendering() && layer_volatile.Overlaps(first, end)) {
        if (const auto in_place = LayerInPlace(va_range, cb_range, true)) {
            PsStats::g_cb_bound_in_place.fetch_add(cb_range, std::memory_order_relaxed);
            layer_volatile_in_pass_bytes += cb_range;
            if (is_texel_buffer) {
                SynchronizeMemoryFromImage(in_place->first, in_place->second, va_range,
                                           static_cast<u32>(cb_range));
            }
            TraceBinding(va_range, cb_range, false, 2);
            return in_place;
        }
    }
    if (resolution.kind == Kind::Mirror && is_vram_allowed) {
        auto* p_mirror = static_cast<Buffer*>(resolution.span.p_source->p_owner);
        PsStats::g_cb_bound_vram.fetch_add(cb_range, std::memory_order_relaxed);
        NoteDirectUse(va_range, cb_range); // stale copies yield first (LayerYieldToImages)
        if (layer_volatile.Overlaps(first, end)) {
            // Experiment bit 16 (diagnostics, while the game runs): no refreshes, the range read
            // in place instead.
            if (PsToggle::Experiment(16) && !is_written) {
                if (const auto in_place = LayerInPlace(va_range, cb_range, true)) {
                    return in_place;
                }
            }
            LayerRefreshVolatile(va_range, cb_range);
        }
        if (!is_written && LayerVerifyOn()) {
            LayerVerifyMirror(va_range, cb_range, is_texel_buffer);
        }
        // Read before the upload check, so that a CPU change during it makes the memo stale at once.
        const u64 epoch = g_cpu_change_epoch.load(std::memory_order_acquire);
        // Uploads what the CPU changed there since (announced writes), images aliasing it.
        SynchronizeMemory(p_mirror, va_range, static_cast<u32>(cb_range), is_written, is_texel_buffer);
        if (!is_written && !is_texel_buffer && can_promote && !layer_volatile.Overlaps(first, end)) {
            StoreBindMemo(va_range, cb_range, epoch, p_mirror, p_mirror->Offset(va_range), true);
        }
        if (is_written) {
            // The GPU's data is in VRAM only: copied back before the blocks go in place.
            gpu_modified_ranges.Add(va_range, cb_range);
            NoteWriteTick(va_range, cb_range);
            LayerArmReadTraps(va_range, cb_range); // and before the CPU reads it
        }
        TraceBinding(va_range, cb_range, is_written, is_written ? 1 : 4);
        return std::pair<const Buffer*, u64>{p_mirror, p_mirror->Offset(va_range)};
    }
    if (resolution.kind == Kind::Mirror || resolution.kind == Kind::Mixed) {
        // One buffer range must hold it: in place. Ranges over nearly all memory in shaders are
        // paged instead (LayerPagedRecord). Written: its blocks in VRAM go back in place (the
        // GPU's data there copied back first). Read: only the GPU's data is copied back; the
        // blocks stay in VRAM for the bindings that lie whole in their mirror.
        if (is_written) {
            LayerDemote(first, end, true, resolution.kind == Kind::Mirror ? 0 : 1);
            // The GPU writes this range in place: its blocks stay in place (moving them back
            // would only bring them here again; the arena mixed them). All in one mirror: from
            // the second time (a reading binding brought it back every frame meanwhile: ~200 MiB/s
            // each way and traps set and cleared, the recording thread saturated at 42 FPS).
            if (resolution.kind == Kind::Mixed || layer_write_bounces.Overlaps(first, end)) {
                dynamic_blocks.Add({first, end});
            } else {
                layer_write_bounces.Add({first, end});
            }
        } else {
            LayerCopyBack(first, end);
        }
    }
    const bool is_pure_in_place = resolution.kind == Kind::InPlace;
    const u64 epoch = g_cpu_change_epoch.load(std::memory_order_acquire);
    // In place (huge bindings over several chunks are cut at the end of the first).
    const auto in_place = LayerInPlace(va_range, cb_range, true);
    if (!in_place) {
        return std::nullopt;
    }
    // Its blocks of known data go to VRAM at the next submission (as the arena gave VRAM to such
    // blocks when first bound): once per range while the residency is unchanged.
    if (can_promote) {
        LayerQueuePromotion(first, end);
    }
    const auto [buffer, offset] = *in_place;
    if (is_written) {
        NoteWriteTick(va_range, cb_range);
        PsStats::g_cb_bound_in_place_written.fetch_add(cb_range, std::memory_order_relaxed);
        // What the GPU writes often moves to VRAM once its blocks are all known data
        // (NoteGpuWrite: written in 3 of 60 frames). Some writers stay in place (the copy
        // shader's destinations: force_writes_in_place).
        if (!force_writes_in_place) {
            NoteGpuWrite(va_range, cb_range);
        }
    }
    PsStats::g_cb_bound_in_place.fetch_add(cb_range, std::memory_order_relaxed);
    if (is_texel_buffer && !is_written) {
        SynchronizeMemoryFromImage(buffer, offset, va_range, static_cast<u32>(cb_range));
    }
    // Small constants stay out, since ObtainBuffer copies them when not GPU-written (decode order).
    if (!is_written && !is_texel_buffer && can_promote && is_pure_in_place &&
        (cb_range > STREAM_THRESHOLD || ConstantsInPlace())) {
        StoreBindMemo(va_range, cb_range, epoch, buffer, offset, false);
    }
    TraceBinding(va_range, cb_range, is_written, is_written ? 0 : 2);
    return in_place;
}

bool BufferCache::BindMemoOn() {
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_BIND_MEMO");
        return !(sz_env && sz_env[0] == '0');
    }();
    return s_is_on;
}

BufferCache::BindMemo& BufferCache::BindMemoSlot(VAddr va_range, u64 cb_range) {
    thread_local auto p_memo = std::make_unique<std::array<BindMemo, 4096>>(); // heap: see LayerBind
    return (*p_memo)[((va_range * 0x9E3779B97F4A7C15ull) ^ (cb_range * 0xC2B2AE3D27D4EB4Full)) >> 52];
}

void BufferCache::StoreBindMemo(VAddr va_range, u64 cb_range, u64 epoch, const Buffer* p_buffer,
                                u64 off_buffer, bool is_vram) {
    if (!BindMemoOn()) {
        return;
    }
    BindMemoSlot(va_range, cb_range) = {
        .va_range = va_range,
        .cb_range = cb_range,
        .generation = layer_generation,
        .epoch = epoch,
        .off_buffer = off_buffer,
        .p_buffer = p_buffer,
        .second = PsStats::g_coarse_second.load(std::memory_order_relaxed),
        .is_vram = is_vram,
    };
}

bool BufferCache::LayerEligible(u64 block) {
    // Every byte last written by a loader or by the GPU (writes we hear of), in a guest chunk.
    const VAddr address = block << block_shift;
    if (dynamic_blocks.Contains(block)) {
        return false;
    }
    bool is_known = true;
    asset_bytes.ForEachGap(address, address + block_size, [&](u64 start, u64 end) {
        is_known = is_known && gpu_written_bytes.Contains(start, end) &&
                LayerGpuWriteToVram(start, end - start);
    });
    return is_known && Layer().ResolveInPlace(address, block_size).has_value();
}

bool BufferCache::LayerWatchEnabled() {
    // PS_LAYER_WATCH=1/0 (default 1). The reads over the bus cost far more on NVIDIA's imported
    // host memory than on AMD's dma-buf (GTX 1660 Ti: 30-35 FPS with the module, 60-70 with the
    // model of 0.3; RX 7800 XT: on par). It also replaces most volatile blocks, refreshed for
    // every binding (RX 7800 XT: 1.3 GB/s of copies -> 36 MB/s). Needs the write traps
    // (PS_LAYER_WRITE_TRAPS).
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_LAYER_WATCH");
        const bool is_enabled = !(sz_env && sz_env[0] == '0');
        if (is_enabled && LayerTrapsOn()) {
            std::printf("Guest memory: blocks the CPU writes unannounced get VRAM copies too, "
                        "uploaded again after its next write (PS_LAYER_WATCH=1)\n");
        }
        return is_enabled;
    }();
    return s_is_on && LayerTrapsOn();
}

bool BufferCache::LayerWatchHot() {
    // PS_LAYER_WATCH_HOT=1 (experiment): blocks the CPU writes often (NoteCpuWrite: in 3 of 60
    // frames) are watched too and uploaded again after every write, as the model of 0.3 uploads
    // them. Off: they go back in place. Uploading them before bindings ended render passes
    // (RX 7800 XT: the GPU's frame 4.8 -> 7.2 ms).
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_LAYER_WATCH_HOT");
        return sz_env && sz_env[0] == '1';
    }();
    return s_is_on;
}

bool BufferCache::LayerWatchable(u64 block) {
    // Dynamic for the GPU's writes (in place in a mixed range): never; for the CPU's (often
    // written) only with PS_LAYER_WATCH_HOT=1.
    const bool is_dynamic = dynamic_blocks.Contains(block) &&
                         !(LayerWatchHot() && layer_cpu_hot.Contains(block, block + 1));
    if (!LayerWatchEnabled() || is_dynamic || layer_volatile.Contains(block, block + 1) ||
        layer_watch_failed.Contains(block, block + 1)) {
        return false;
    }
    const VAddr address = block << block_shift;
    if (!Layer().ResolveInPlace(address, block_size)) {
        return false; // no source to upload from
    }
    // A trap must be able to catch every CPU write: the CPU cannot write it, or one mapping
    // holds the whole block (LayerArmTraps).
    int prot = 0, type = -1;
    uintptr_t vma_end = 0;
    if (!runtime_memory_vma_info(address, &prot, &type, &vma_end)) {
        return false;
    }
    return !(prot & 0x2) || vma_end >= address + block_size;
}

void BufferCache::LayerRefreshWatched(u64 block) {
    // The CPU wrote it (a trap hit): uploaded again before the next binding over it (the bound
    // path's SynchronizeMemory; paged bindings: LayerFlushUploads), the trap set again for its
    // next write. The GPU never writes it in VRAM: nothing of the GPU's to keep.
    const VAddr from = block << block_shift;
    memory_tracker->UnmarkRegionAsGpuModified(from, block_size);
    gpu_modified_ranges.Subtract(from, block_size);
    memory_tracker->MarkRegionAsCpuModified(from, block_size);
    {
        std::scoped_lock lk{layer_uploads_mutex};
        layer_pending_uploads.emplace_back(from, block_size);
        layer_uploads_pending.store(true, std::memory_order_release);
    }
    LayerArmTraps(block, block + 1);
    ++layer_watch_refreshes;
}

void BufferCache::LayerPromote(const std::vector<u64>& blocks) {
    PsStats::Timer timer{PsStats::g_t_layer[1]};
    u64 moved = 0;
    for (size_t i = 0; i < blocks.size();) {
        // A run of consecutive candidate blocks.
        u64 a = blocks[i], b = a + 1;
        for (++i; i < blocks.size() && blocks[i] == b; ++i) {
            ++b;
        }
        // Only blocks still eligible and not in VRAM yet; then their run.
        u64 run_first = ~0ULL, run_end = 0;
        for (u64 block = a; block < b; ++block) {
            if (Layer().AnyValid(block, block + 1) ||
                !(LayerEligible(block) || layer_watched.Contains(block, block + 1) ||
                  layer_edge_blocks.Contains(block, block + 1))) {
                if (run_first != ~0ULL) {
                    moved += LayerPromoteRun(run_first, run_end);
                    run_first = ~0ULL;
                }
                continue;
            }
            if (run_first == ~0ULL) {
                run_first = block;
            }
            run_end = block + 1;
        }
        if (run_first != ~0ULL) {
            moved += LayerPromoteRun(run_first, run_end);
        }
    }
    if (moved != 0) {
        static u64 s_c_promoted = 0;
        if (((s_c_promoted + moved) >> 26) != (s_c_promoted >> 26)) {
            std::printf("Layer memory: %llu MiB of loaded blocks copied to VRAM, %llu MiB valid "
                        "there in %zu mirrors\n",
                        (unsigned long long)((s_c_promoted + moved) >> 20),
                        (unsigned long long)(Layer().ValidBytes() >> 20), layer_mirrors.size());
        }
        s_c_promoted += moved;
    }
}

u64 BufferCache::LayerPromoteRun(u64 first, u64 end) {
    // The mirror range: the run and every mirror touching it (or touching what that adds).
    u64 lo = first, hi = end;
    std::vector<PsLayer::MemorySource> touching;
    for (bool has_grown = true; has_grown;) {
        has_grown = false;
        touching = Layer().MirrorsIn((lo - 1) << block_shift, (hi + 1) << block_shift);
        for (const auto& mirror : touching) {
            const u64 mirror_lo = mirror.base >> block_shift;
            const u64 mirror_hi = (mirror.base + mirror.cb_source) >> block_shift;
            if (mirror_lo < lo || mirror_hi > hi) {
                lo = std::min(lo, mirror_lo);
                hi = std::max(hi, mirror_hi);
                has_grown = true;
            }
        }
    }
    // Page table entries that change: the run, and the blocks of mirrors replaced by a new one.
    std::vector<std::pair<u64, u64>> moved;
    Buffer* p_target = nullptr;
    if (touching.size() == 1 && touching[0].base == (lo << block_shift) &&
        touching[0].cb_source == ((hi - lo) << block_shift)) {
        p_target = static_cast<Buffer*>(touching[0].p_owner);
    } else {
        // A new mirror over all of it; the old ones' valid blocks are copied into it and they
        // go once the GPU is past the commands recorded so far. One that grows (it takes in a
        // mirror it meets) gets room above it for as much again, up to LayerGrowthBlocks: a
        // loader writing a file in pieces made a new mirror, and a copy of the old one, for each
        // piece (15-27 GiB allocated for 1.3 GiB of data on loading a level, 7.4 GiB of VRAM
        // at once; streaming areas did the same while playing).
        // Growth at the top only: a run that fills a gap between mirrors is not a file coming in.
        u64 top = 0;
        for (const auto& mirror : touching) {
            top = std::max<u64>(top, (mirror.base + mirror.cb_source) >> block_shift);
        }
        if (!touching.empty() && end >= top) {
            hi = LayerGrowthEnd(lo, hi);
        }
        const VAddr start = lo << block_shift;
        const u64 cb_range = (hi - lo) << block_shift;
        auto mirror = std::make_unique<Buffer>(instance, start, cb_range, MemoryType::DeviceLocal,
                                               fmt::format("psport mirror {:#x}+{:#x}", start, cb_range));
        p_target = mirror.get();
        for (const auto& old : touching) {
            auto* p_old_buffer = static_cast<Buffer*>(old.p_owner);
            moved.emplace_back(old.base >> block_shift, (old.base + old.cb_source) >> block_shift);
            boost::container::small_vector<vk::BufferCopy, 8> copies;
            Layer().ForEachValid(old.base >> block_shift, (old.base + old.cb_source) >> block_shift,
                                 [&](u64 v_first, u64 v_end) {
                                     const VAddr from = v_first << block_shift;
                                     copies.push_back({p_old_buffer->Offset(from),
                                                       p_target->Offset(from),
                                                       (v_end - v_first) << block_shift});
                                 });
            if (!copies.empty()) {
                runtime.CopyBuffer(p_old_buffer, p_target, copies);
            }
            Layer().RemoveMirror(old.base);
            auto node = layer_mirrors.extract(old.base);
            if (!node.empty()) {
                // Otherwise the statistic only grew with every merge.
                PsStats::g_cb_residency_alloc.fetch_sub(old.cb_source, std::memory_order_relaxed);
                layer_retired.push_back({std::move(node.mapped()), scheduler.CurrentTick()});
            }
        }
        Layer().AddMirror(start, cb_range, p_target->Handle(), p_target->BufferDeviceAddress(), p_target);
        layer_mirrors.emplace(start, std::move(mirror));
        PsStats::g_cb_residency_alloc.fetch_add(cb_range, std::memory_order_relaxed);
    }
    // The run's data: copied from the game's memory now (SynchronizeMemory, which also arms the
    // tracking of later CPU writes there).
    Layer().MarkValid(first, end);
    NoteUse(first << block_shift, (end - first) << block_shift);
    NoteDirectUse(first << block_shift, (end - first) << block_shift); // not stale at once
    const VAddr from = first << block_shift;
    const u64 cb_range = (end - first) << block_shift;
    memory_tracker->UnmarkRegionAsGpuModified(from, cb_range);
    gpu_modified_ranges.Subtract(from, cb_range);
    memory_tracker->MarkRegionAsCpuModified(from, cb_range);
    SynchronizeMemory(p_target, from, static_cast<u32>(cb_range), false, false);
    if (LayerVerifyOn()) {
        LayerVerifyForget(first, end); // a fresh copy: compared from its next binding
    }
    LayerArmTraps(first, end); // writes nobody announces send it back in place
    // Paged bindings find the mirror (a merge moved its other blocks too). Not all of it: a
    // mirror of a GiB rewrote 16k entries for every run moved into it (160k in a frame).
    LayerRepublish(first, end);
    for (const auto& [a, b] : moved) {
        LayerRepublish(a, b);
    }
    ++layer_generation;
    return cb_range;
}

u64 BufferCache::LayerGrowthEnd(u64 lo, u64 hi) {
    // PS_LAYER_MIRROR_GROWTH_MB (64; 0: none): the most room a growing mirror gets above it.
    static const u64 s_cb_growth = [] {
        const char* sz_env = std::getenv("PS_LAYER_MIRROR_GROWTH_MB");
        return (sz_env && *sz_env ? std::strtoull(sz_env, nullptr, 10) : 64ull) << 20;
    }();
    const u64 extra = std::min(hi - lo, s_cb_growth >> block_shift);
    if (extra == 0) {
        return hi;
    }
    u64 end = hi + extra;
    // Not over the next mirror, and only blocks that can be valid here: the game's memory whole
    // in a chunk, contiguous with the blocks below.
    for (const auto& mirror : Layer().MirrorsIn(hi << block_shift, end << block_shift)) {
        end = std::min(end, mirror.base >> block_shift);
    }
    if (end <= hi) {
        return hi;
    }
    const auto prefix = Layer().ResolvePrefix(hi << block_shift, (end - hi) << block_shift);
    if (!prefix) {
        return hi;
    }
    return std::min(end, hi + (prefix->cb_span >> block_shift));
}

static std::array<std::atomic<u64>, 7> s_cb_layer_demoted{};

void BufferCache::LayerDemote(u64 first, u64 end, bool should_copy_back, int reason) {
    PsStats::Timer timer{PsStats::g_t_layer[2]};
    LayerForgetGpuData();
    std::vector<std::pair<u64, u64>> runs;
    Layer().ForEachValid(first, end, [&](u64 a, u64 b) { runs.emplace_back(a, b); });
    if (runs.empty()) {
        return;
    }
    for (const auto& [a, b] : runs) {
        const VAddr from = a << block_shift;
        const u64 cb_range = (b - a) << block_shift;
        if (should_copy_back) {
            // What the GPU wrote in VRAM goes back into the game's memory first.
            const auto mirror = Layer().MirrorAt(from);
            if (mirror) {
                auto* p_buffer = static_cast<Buffer*>(mirror->p_owner);
                gpu_modified_ranges.ForEachInRange(from, cb_range, [&](VAddr w_from, VAddr w_to) {
                    if (const auto target = LayerInPlace(w_from, w_to - w_from)) {
                        const vk::BufferCopy copy{p_buffer->Offset(w_from), target->second,
                                                  w_to - w_from};
                        runtime.CopyBuffer(p_buffer, target->first, std::span{&copy, 1});
                    }
                });
            }
        }
        layer_volatile.Subtract(a, b);
        layer_edge_blocks.Subtract(a, b);
        // A watched block sent back for a GPU write (binding or command) is not watched again (it
        // came back for every binding, ~40 MiB/s while playing; memory handed out anew does not
        // clear it either: the game's allocator recycles some every frame); one sent back for an
        // announced write or a request, after the third time (loading a level sends many back
        // once). Idle or unmapped ones may be again.
        if (reason <= 3) {
            layer_watched.ForEachInRange(a, b, [&](const Interval& iv) {
                for (u64 block = std::max(a, iv.start); block < std::min(b, iv.end); ++block) {
                    if (reason <= 2 || ++layer_watch_strikes[block] >= 3) {
                        layer_watch_failed.Add({block, block + 1});
                    }
                }
            });
        }
        layer_watched.Subtract(a, b);
        LayerDisarmTraps(a, b, false);
        Layer().MarkInvalid(a, b);
        // In place from now on: never uploaded or watched again (writes go where the GPU reads).
        memory_tracker->MarkRegionAsCpuModified(from, cb_range);
        s_cb_layer_demoted[reason].fetch_add(cb_range, std::memory_order_relaxed);
        LayerRepublish(a, b); // paged bindings find them in place
        memory_tracker->UnmarkRegionAsGpuModified(from, cb_range);
        gpu_modified_ranges.Subtract(from, cb_range);
    }
    // Mirrors left without valid blocks go once the GPU is past the commands recorded so far.
    for (const auto& mirror : Layer().MirrorsIn(first << block_shift, end << block_shift)) {
        if (Layer().AnyValid(mirror.base >> block_shift, (mirror.base + mirror.cb_source) >> block_shift)) {
            continue;
        }
        Layer().RemoveMirror(mirror.base);
        auto node = layer_mirrors.extract(mirror.base);
        if (!node.empty()) {
            PsStats::g_cb_residency_alloc.fetch_sub(mirror.cb_source, std::memory_order_relaxed);
            layer_retired.push_back({std::move(node.mapped()), scheduler.CurrentTick()});
        }
    }
    ++layer_generation;
}

void BufferCache::LayerMaintain() {
    LayerProcessTraps();
    // Retired mirrors the GPU is done with.
    std::erase_if(layer_retired, [&](const auto& retired) {
        return scheduler.GetWorkSemaphore()->IsFree(retired.second);
    });
    static u32 s_c_printed = 0;
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (now - s_c_printed >= 10) {
        s_c_printed = now;
        u64 cb_mirror = 0, c_asset = 0;
        for (const auto& range : asset_bytes) {
            c_asset += range.end - range.start;
        }
        for (const auto& [start, mirror] : layer_mirrors) {
            cb_mirror += mirror->SizeBytes();
        }
        std::printf("Layer memory: %llu MiB valid in VRAM, %zu mirrors of %llu MiB (%zu retired); "
                    "loaded data %llu MiB; vkQueueBindSparse: %llu calls, %llu ranges; write traps hit %llu\n",
                    (unsigned long long)(Layer().ValidBytes() >> 20), layer_mirrors.size(),
                    (unsigned long long)(cb_mirror >> 20), layer_retired.size(),
                    (unsigned long long)(c_asset >> 20),
                    (unsigned long long)arena_bind_calls.load(),
                    (unsigned long long)arena_bind_ranges.load(), (unsigned long long)LayerTrapFaults());
        u64 volatile_blocks = 0;
        for (const auto& range : layer_volatile) {
            volatile_blocks += range.end - range.start;
        }
        u64 watched_blocks = 0;
        for (const auto& range : layer_watched) {
            watched_blocks += range.end - range.start;
        }
        std::printf("Layer memory: %llu volatile blocks, %llu MiB refreshed in 10 s (%llu MiB read in "
                    "place in render passes); %llu watched blocks, %llu uploaded again after CPU writes; GPU writes in VRAM barred in "
                    "%llu MiB the CPU wrote\n",
                    (unsigned long long)volatile_blocks,
                    (unsigned long long)(layer_volatile_refresh_bytes >> 20),
                    (unsigned long long)(layer_volatile_in_pass_bytes >> 20),
                    (unsigned long long)watched_blocks, (unsigned long long)layer_watch_refreshes,
                    (unsigned long long)([&] {
                        u64 blocks = 0;
                        for (const auto& range : layer_cpu_blocks) {
                            blocks += range.end - range.start;
                        }
                        return blocks;
                    }() << block_shift >> 20));
        layer_volatile_refresh_bytes = 0;
        layer_volatile_in_pass_bytes = 0;
        layer_watch_refreshes = 0;
        std::printf("Layer memory: write traps in 10 s: announced %llu; the game's code on watched "
                    "%llu, on other copies %llu; other code on watched %llu, on other copies %llu\n",
                    (unsigned long long)layer_trap_kinds[0], (unsigned long long)layer_trap_kinds[1],
                    (unsigned long long)layer_trap_kinds[2], (unsigned long long)layer_trap_kinds[3],
                    (unsigned long long)layer_trap_kinds[4]);
        layer_trap_kinds = {};
        if (LayerReadTrapsOn()) {
            u64 trapped = 0;
            {
                std::scoped_lock lk{layer_read_trap_mutex};
                layer_read_trapped.ForEach([&](VAddr a, VAddr b) { trapped += b - a; });
            }
            std::printf("Layer memory: read traps (GPU data in VRAM only): %llu KiB trapped; %llu CPU "
                        "accesses in 10 s, %llu KiB copied back for them, %llu by the recording "
                        "thread before the GPU's writes went out (read as they were)\n",
                        (unsigned long long)(trapped >> 10),
                        (unsigned long long)layer_read_trap_hits.exchange(0),
                        (unsigned long long)(layer_read_trap_bytes.exchange(0) >> 10),
                        (unsigned long long)layer_read_trap_stale.exchange(0));
        }
        std::printf("Layer memory: back in place (MiB): written %llu, mixed %llu, command %llu, "
                    "request %llu, copy back %llu, unmap %llu, idle %llu; GPU data copied back %llu MiB\n",
                    (unsigned long long)(s_cb_layer_demoted[0].load() >> 20),
                    (unsigned long long)(s_cb_layer_demoted[1].load() >> 20),
                    (unsigned long long)(s_cb_layer_demoted[2].load() >> 20),
                    (unsigned long long)(s_cb_layer_demoted[3].load() >> 20),
                    (unsigned long long)(s_cb_layer_demoted[4].load() >> 20),
                    (unsigned long long)(s_cb_layer_demoted[5].load() >> 20),
                    (unsigned long long)(s_cb_layer_demoted[6].load() >> 20),
                    (unsigned long long)(layer_copied_back_bytes >> 20));
        // What keeps blocks in place for good, and loaded data without a copy: over a long session
        // the copies' share fell (84 % -> 50 %) while these marks only grew.
        const auto mib = [&](const auto& list) {
            u64 c_blocks = 0;
            for (const auto& iv : list) {
                c_blocks += iv.end - iv.start;
            }
            return (unsigned long long)((c_blocks << block_shift) >> 20);
        };
        u64 c_loaded_out = 0;
        asset_bytes.ForEachInRange(0, ~0ULL, [&](const Interval& iv) {
            for (u64 b = iv.start >> block_shift; b <= (iv.end - 1) >> block_shift; ++b) {
                c_loaded_out += Layer().AnyValid(b, b + 1) ? 0 : 1;
            }
        });
        std::printf("Layer memory: held in place (MiB): dynamic %llu (CPU-hot %llu), watch failed %llu, "
                    "CPU-written %llu (GPU writes kept in place by them: %llu MiB in 10 s), write bounces "
                    "%llu; dynamic blocks reloaded whole %llu; blocks with loaded data and no copy %llu MiB\n",
                    mib(dynamic_blocks), mib(layer_cpu_hot), mib(layer_watch_failed), mib(layer_cpu_blocks),
                    (unsigned long long)(PsStats::g_cb_gpu_writes_kept_by_cpu.exchange(0) >> 20),
                    mib(layer_write_bounces),
                    (unsigned long long)(PsStats::g_cb_dynamic_reloaded.load() >> 20),
                    (unsigned long long)((c_loaded_out << block_shift) >> 20));
    }
}

void BufferCache::LayerProcessIdle() {
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (VramIdleSeconds() == 0) {
        return;
    }
    LayerYieldToImages(now);
    std::vector<std::pair<u64, u64>> idle;
    Layer().ForEachValid(0, ~0ULL, [&](u64 a, u64 b) {
        for (u64 block = a; block < b;) {
            const u64 group = (block << block_shift) >> USE_GROUP_BITS;
            const u64 group_end = std::min(b, ((group + 1) << USE_GROUP_BITS) >> block_shift);
            if (now - group_use[group] >= VramIdleSeconds()) {
                idle.emplace_back(block, group_end);
            }
            block = group_end;
        }
    });
    for (const auto& [a, b] : idle) {
        LayerDemote(a, b, true, 6);
        PsStats::g_cb_vram_idle.fetch_add((b - a) << block_shift, std::memory_order_relaxed);
    }
}

void BufferCache::NoteDirectUse(VAddr va_range, u64 cb_range) {
    const u32 now = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    const u64 last = std::min<u64>((va_range + cb_range - 1) >> USE_GROUP_BITS, layer_group_use.size() - 1);
    for (u64 group = va_range >> USE_GROUP_BITS; group <= last; ++group) {
        if (layer_group_use[group] != now) {
            layer_group_use[group] = now;
        }
    }
}

u64 BufferCache::LayerCompactMirrors(u64 wanted) {
    // Mirror memory without valid blocks (sent back in place, unmapped, handed out anew, the room
    // to grow) stays allocated as long as its mirror lives. Mirrors with at least MinWaste of it,
    // the most first, are made again over their valid blocks only: one new mirror per run of
    // valid blocks (runs closer than Bridge stay in one), their data copied over, the old mirror
    // retired as in a merge. No copy is lost.
    constexpr u64 MinWaste = 8_MB, Bridge = 4_MB;
    std::vector<std::pair<u64, VAddr>> wasteful; // bytes without valid blocks, start
    for (const auto& [start, mirror] : layer_mirrors) {
        const u64 lo = start >> block_shift, hi = (start + mirror->SizeBytes()) >> block_shift;
        u64 valid = 0;
        Layer().ForEachValid(lo, hi, [&](u64 a, u64 b) { valid += b - a; });
        const u64 waste = mirror->SizeBytes() - (valid << block_shift);
        if (waste >= MinWaste) {
            wasteful.emplace_back(waste, start);
        }
    }
    std::ranges::sort(wasteful, std::greater{});
    u64 freed = 0;
    size_t compacted = 0;
    for (const auto& [waste, start] : wasteful) {
        if (freed >= wanted) {
            break;
        }
        auto node = layer_mirrors.extract(start);
        Buffer* p_old_buffer = node.mapped().get();
        const u64 cb_old = p_old_buffer->SizeBytes();
        const u64 lo = start >> block_shift, hi = (start + cb_old) >> block_shift;
        std::vector<std::pair<u64, u64>> segments;
        Layer().ForEachValid(lo, hi, [&](u64 a, u64 b) {
            if (!segments.empty() && ((a - segments.back().second) << block_shift) < Bridge) {
                segments.back().second = b;
            } else {
                segments.emplace_back(a, b);
            }
        });
        Layer().RemoveMirror(start);
        PsStats::g_cb_residency_alloc.fetch_sub(cb_old, std::memory_order_relaxed); // replaced below
        u64 kept = 0;
        for (const auto& [a, b] : segments) {
            const VAddr base = a << block_shift;
            const u64 cb_range = (b - a) << block_shift;
            auto mirror = std::make_unique<Buffer>(instance, base, cb_range, MemoryType::DeviceLocal,
                                                   fmt::format("psport mirror {:#x}+{:#x}", base, cb_range));
            boost::container::small_vector<vk::BufferCopy, 8> copies;
            Layer().ForEachValid(a, b, [&](u64 v_first, u64 v_end) {
                const VAddr from = v_first << block_shift;
                copies.push_back({p_old_buffer->Offset(from), mirror->Offset(from),
                                  (v_end - v_first) << block_shift});
            });
            if (!copies.empty()) {
                runtime.CopyBuffer(p_old_buffer, mirror.get(), copies);
            }
            Layer().AddMirror(base, cb_range, mirror->Handle(), mirror->BufferDeviceAddress(), mirror.get());
            layer_mirrors.emplace(base, std::move(mirror));
            PsStats::g_cb_residency_alloc.fetch_add(cb_range, std::memory_order_relaxed);
            kept += cb_range;
        }
        layer_retired.push_back({std::move(node.mapped()), scheduler.CurrentTick()});
        LayerRepublish(lo, hi); // paged bindings find the new buffers
        ++layer_generation;
        freed += cb_old - kept;
        ++compacted;
    }
    if (freed != 0) {
        std::printf("Layer memory: VRAM short: %zu mirrors made again over their valid blocks, "
                    "%llu MiB freed\n",
                    compacted, (unsigned long long)(freed >> 20));
    }
    return freed;
}

void BufferCache::LayerYieldToImages(u32 now) {
    PsStats::Timer timer{PsStats::g_t_layer[4]};
    // VRAM short: the usage where promotions wait (PromoteCriticalMargin short of the texture
    // collector's critical mark). Past that mark the collector evicts images used a moment ago,
    // re-uploaded at once (stutters), and mirrors are what it cannot free. Meanwhile the copies of
    // where the player is now wait for their room, read over the bus (10 FPS less after a lamp
    // travel on a 6 GB card), so room is made for them.
    if (!instance.CanReportMemoryUsage() || layer_mirrors.empty()) {
        return;
    }
    const u64 critical = PsStats::g_cb_gc_critical.load(std::memory_order_relaxed);
    const u64 usage = PsStats::g_cb_gc_used.load(std::memory_order_relaxed);
    if (critical == 0 || usage + PromoteCriticalMargin < critical) {
        return;
    }
    const u64 wanted = usage + PromoteCriticalMargin + StaleHeadroom - critical;
    static const u32 s_idle_seconds = [] {
        const char* sz_env = std::getenv("PS_VRAM_PRESSURE_IDLE_SECONDS");
        return sz_env && *sz_env ? u32(std::strtoul(sz_env, nullptr, 10)) : 3u;
    }();
    // 1. Valid blocks in 2 MiB groups no binding took directly for PS_VRAM_PRESSURE_IDLE_SECONDS
    // (3): an area left behind, a lamp travelled from. They go back in place, the longest unused
    // first, whatever the mirrors' floor, as far as the mirrors' memory without valid blocks does
    // not already cover the need. Only direct bindings count (layer_group_use): paged bindings
    // over nearly all memory mark every group as used (group_use), and nothing was ever idle
    // while playing.
    u64 waste = 0;
    for (const auto& [start, mirror] : layer_mirrors) {
        u64 valid = 0;
        Layer().ForEachValid(start >> block_shift, (start + mirror->SizeBytes()) >> block_shift,
                             [&](u64 a, u64 b) { valid += b - a; });
        waste += mirror->SizeBytes() - (valid << block_shift);
    }
    u64 cb_stale = 0;
    if (s_idle_seconds != 0 && waste < wanted) {
        std::vector<std::tuple<u32, u64, u64>> stale; // last direct use, first block, end block
        Layer().ForEachValid(0, ~0ULL, [&](u64 a, u64 b) {
            for (u64 block = a; block < b;) {
                const u64 group = (block << block_shift) >> USE_GROUP_BITS;
                const u64 group_end = std::min(b, ((group + 1) << USE_GROUP_BITS) >> block_shift);
                const u32 last = layer_group_use[group];
                if (now - last >= s_idle_seconds) {
                    stale.emplace_back(last, block, group_end);
                }
                block = group_end;
            }
        });
        std::ranges::sort(stale);
        for (const auto& [last, a, b] : stale) {
            if (waste + cb_stale >= wanted) {
                break;
            }
            LayerDemote(a, b, true, 6);
            cb_stale += (b - a) << block_shift;
        }
        if (cb_stale != 0) {
            PsStats::g_cb_vram_idle.fetch_add(cb_stale, std::memory_order_relaxed);
            static u32 s_c_printed = 0;
            if (now - s_c_printed >= 5) {
                s_c_printed = now;
                std::printf("Layer memory: VRAM %llu MiB, texture collector critical at %llu MiB: "
                            "%llu MiB of VRAM copies not bound for %u s back in place\n",
                            (unsigned long long)(usage >> 20), (unsigned long long)(critical >> 20),
                            (unsigned long long)(cb_stale >> 20), s_idle_seconds);
            }
        }
    }
    // 2. The memory without valid blocks is given back (mirrors made again, smaller).
    LayerCompactMirrors(wanted);
}

} // namespace VideoCore

namespace VideoCore {

u64 BufferCache::LayerEntry(u64 block) {
    // The device address of the block's current source: its mirror where valid, else the game's
    // memory (a block mapped whole, in a chunk), else 0 (reads give zero, writes are dropped).
    const VAddr va = block << block_shift;
    if (Layer().AnyValid(block, block + 1) && !layer_volatile.Contains(block, block + 1)) {
        if (const auto mirror = Layer().MirrorAt(va)) {
            return mirror->va_device + (va - mirror->base);
        }
    }
    const auto span = Layer().ResolveInPlace(va, block_size);
    if (!span || span->p_source->va_device == 0) {
        return 0;
    }
    return span->p_source->va_device + span->off_source;
}

void BufferCache::LayerWriteEntries(u64 first, u64 end) {
    if (first >= end) {
        return;
    }
    // Diagnostics (PS_FRAME_STATS): where the time goes (g_t_layer 8-11).
    auto mark = std::chrono::steady_clock::now();
    const auto lap = [&](size_t kind) {
        if (!PsStats::g_is_enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        PsStats::g_t_layer[kind].fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count(),
            std::memory_order_relaxed);
        mark = now;
    };
    PsStats::g_layer_entries_written.fetch_add(end - first, std::memory_order_relaxed);
    const auto staging =
        staging_pool.Request((end - first) * 2 * sizeof(vk::DeviceAddress), MemoryType::HostUncached);
    lap(8);
    // Read/current view, then CPU-visible guest view. A write-through store updates both: the CPU
    // sees the result without discarding all mirrors covered by a huge descriptor.
    auto* p_entries = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    auto* p_guest_entries = p_entries + (end - first);
    // The guest view (as LayerEntry in place: a block mapped whole, in a chunk), one translation
    // per mapping, not per block: each takes the runtime's memory lock, which the game's threads
    // hold while they map memory; a level streaming in kept the GPU thread waiting 30-70 ms per
    // frame on the thousands a large promotion made.
    for (u64 block = first; block < end;) {
        const auto span = Layer().ResolvePrefix(block << block_shift, (end - block) << block_shift);
        const u64 whole = span ? span->cb_span >> block_shift : 0;
        if (whole == 0) {
            p_guest_entries[block - first] = 0;
            ++block;
            continue;
        }
        const vk::DeviceAddress base =
            span->p_source->va_device ? span->p_source->va_device + span->off_source : 0;
        for (u64 i = 0; i < whole; ++i) {
            p_guest_entries[block - first + i] = base ? base + (i << block_shift) : 0;
        }
        block += whole;
    }
    lap(9);
    std::copy_n(p_guest_entries, end - first, p_entries);
    // Valid blocks (not volatile) are read from their mirror.
    std::vector<std::pair<u64, u64>> valid;
    Layer().ForEachValid(first, end, [&](u64 a, u64 b) { valid.emplace_back(a, b); });
    for (const auto& [a, b] : valid) {
        std::optional<PsLayer::MemorySource> mirror;
        const bool is_any_volatile = layer_volatile.Overlaps(a, b);
        for (u64 block = a; block < b; ++block) {
            const VAddr va = block << block_shift;
            if (is_any_volatile && layer_volatile.Contains(block, block + 1)) {
                continue;
            }
            if (!mirror || va < mirror->base || va >= mirror->base + mirror->cb_source) {
                mirror = Layer().MirrorAt(va);
            }
            if (mirror) {
                p_entries[block - first] = mirror->va_device + (va - mirror->base);
            }
        }
    }
    lap(10);
    staging.Flush();
    // Keep the original current-view table layout for DMA/ReadConst users too; append the
    // guest-view table in the second half rather than interleaving its entries.
    const u64 cb_table = (end - first) * sizeof(vk::DeviceAddress);
    const std::array<vk::BufferCopy, 2> copies{{
        {staging.offset, first * sizeof(vk::DeviceAddress), cb_table},
        {staging.offset + cb_table, bda_pagetable_buffer->SizeBytes() / 2 +
                                    first * sizeof(vk::DeviceAddress), cb_table},
    }};
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
    lap(11);
}

void BufferCache::LayerRepublish(u64 first, u64 end) {
    PsStats::Timer timer{PsStats::g_t_layer[6]};
    std::vector<std::pair<u64, u64>> runs;
    layer_published.ForEachInRange(first, end, [&](const Interval& iv) {
        runs.emplace_back(std::max(first, iv.start), std::min(end, iv.end));
    });
    for (const auto& [a, b] : runs) {
        LayerWriteEntries(a, b);
    }
}

void BufferCache::LayerNoteCpuWrite(VAddr va_range, u64 cb_range) {
    if (cb_range == 0) { // guest threads too: no layer_mirrors here
        return;
    }
    {
        // The CPU's bytes are newer than what the GPU wrote there: never copied back over them,
        // and the CPU uses these blocks (layer_cpu_blocks).
        std::scoped_lock lk{layer_uploads_mutex};
        layer_cpu_written.emplace_back(va_range, cb_range);
        layer_cpu_written_pending.store(true, std::memory_order_release);
    }
    if (Layer().ValidBytes() == 0) {
        return;
    }
    // Announced: the write must not fault (a file read into it would fail): unprotected now, and
    // the block goes back in place.
    LayerDisarmTraps(va_range >> block_shift, ((va_range + cb_range - 1) >> block_shift) + 1, true);
    std::scoped_lock lk{layer_uploads_mutex};
    layer_pending_uploads.emplace_back(va_range, cb_range);
    layer_uploads_pending.store(true, std::memory_order_release);
}

void BufferCache::LayerForgetGpuData() {
    if (!layer_cpu_written_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> ranges;
    {
        std::scoped_lock lk{layer_uploads_mutex};
        ranges.swap(layer_cpu_written);
        layer_cpu_written_pending.store(false, std::memory_order_release);
    }
    for (const auto& [address, size] : ranges) {
        gpu_modified_ranges.Subtract(address, size);
        memory_tracker->UnmarkRegionAsGpuModified(address, size);
        layer_cpu_blocks.Add({address >> block_shift, ((address + size - 1) >> block_shift) + 1});
    }
}

void BufferCache::LayerFlushUploads() {
    PsStats::Timer timer{PsStats::g_t_layer[7]};
    // What the CPU wrote into blocks with a VRAM copy: uploaded now, before a paged binding
    // (it reads mirrors through the page table, without the bound path's own upload).
    if (!layer_uploads_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> ranges;
    {
        std::scoped_lock lk{layer_uploads_mutex};
        ranges.swap(layer_pending_uploads);
        layer_uploads_pending.store(false, std::memory_order_release);
    }
    for (const auto& [address, size] : ranges) {
        const u64 first = address >> block_shift;
        const u64 end = ((address + size - 1) >> block_shift) + 1;
        std::vector<std::pair<u64, u64>> runs;
        Layer().ForEachValid(first, end, [&](u64 a, u64 b) { runs.emplace_back(a, b); });
        for (const auto& [a, b] : runs) {
            const auto mirror = Layer().MirrorAt(a << block_shift);
            if (!mirror) {
                continue;
            }
            const VAddr from = std::max<VAddr>(address, a << block_shift);
            const VAddr to = std::min<VAddr>(address + size, b << block_shift);
            SynchronizeMemory(static_cast<Buffer*>(mirror->p_owner), from, static_cast<u32>(to - from),
                              false, false);
        }
    }
}

std::pair<const Buffer*, u64> BufferCache::LayerPagedRecord(VAddr va_range, u64 cb_range,
                                                            bool is_written) {
    LayerPagedPublish(va_range, cb_range, is_written);
    PsStats::g_cb_bound_in_place.fetch_add(cb_range, std::memory_order_relaxed);
    return PageTableRecord(va_range, cb_range, is_written && force_writes_in_place);
}

void BufferCache::LayerPagedPublish(VAddr va_range, u64 cb_range, bool is_written) {
    if (maintained_epoch != packet_epoch) {
        Maintain();
    }
    LayerProcessTraps();
    LayerFlushUploads();
    const u64 first = va_range >> block_shift;
    const u64 end = ((va_range + cb_range - 1) >> block_shift) + 1;
    NoteUse(va_range, cb_range);
    // Page table entries for the blocks not published yet (those mapped now: an unmapped block
    // is tried again next time).
    std::vector<std::pair<u64, u64>> gaps;
    layer_published.ForEachGap(first, end, [&](u64 a, u64 b) { gaps.emplace_back(a, b); });
    for (const auto& [a, b] : gaps) {
        u64 run = a;
        for (u64 block = a; block <= b; ++block) {
            const bool is_mapped = block < b && Layer().ResolvePrefix(block << block_shift, 1);
            if (!is_mapped) {
                if (run < block) {
                    LayerWriteEntries(run, block);
                    layer_published.Add({run, block});
                }
                run = block + 1;
            }
        }
    }
    if (is_written) {
        NoteWriteTick(va_range, cb_range);
        // Where it writes is unknown: what is in VRAM there may now differ from the game's
        // memory; it is copied back before it goes in place (LayerDemote).
        if (!force_writes_in_place) {
            Layer().ForEachValid(first, end, [&](u64 a, u64 b) {
                gpu_modified_ranges.Add(a << block_shift, (b - a) << block_shift);
            });
        }
    }
}

std::pair<const Buffer*, u64> BufferCache::PageTableRecord(VAddr va_range, u64 cb_range,
                                                           bool is_write_through) {
    if (!layer_trash) {
        layer_trash = std::make_unique<Buffer>(instance, 0, 64_KB, MemoryType::DeviceLocal,
                                               "psport paged trash");
    }
    const vk::DeviceAddress trash = layer_trash->BufferDeviceAddress();
    const u32 record[8] = {static_cast<u32>(va_range),
                           static_cast<u32>(va_range >> 32),
                           static_cast<u32>(std::min<u64>(cb_range, 0xFFFFFFFFull)),
                           u32(is_write_through),
                           static_cast<u32>(trash),
                           static_cast<u32>(trash >> 32),
                           static_cast<u32>(bda_pagetable_buffer->SizeBytes() /
                                            (2 * sizeof(vk::DeviceAddress))),
                           0};
    const u64 off_record = stream_buffer.Copy(record, sizeof(record), instance.StorageMinAlignment());
    return {&stream_buffer, off_record};
}

bool BufferCache::LayerActive() {
    return PsGuestMemory::LayerMemory();
}

} // namespace VideoCore

namespace VideoCore {
bool BufferCache::LayerPagedActive() {
    // PS_LAYER_PAGED=0 (tests): huge bindings cut in place instead of the page table.
    static const bool s_is_paged = [] {
        const char* sz_env = std::getenv("PS_LAYER_PAGED");
        return !(sz_env && sz_env[0] == '0');
    }();
    return s_is_paged && LayerActive();
}

int BufferCache::LayerGpuWritesMode() {
    // Anywhere (2) gave exploding vertices and garbage textures: GPU data in a VRAM copy of memory
    // the CPU also uses (the game's buffers, its heap's headers, reused allocations) went stale or
    // was copied back over newer CPU data. Images (render targets) are the GPU's own.
    static const int s_mode = [] {
        const char* sz_env = std::getenv("PS_LAYER_GPU_WRITES");
        return sz_env && (sz_env[0] == '0' || sz_env[0] == '2') ? sz_env[0] - '0' : 1;
    }();
    return s_mode;
}

bool BufferCache::LayerInImage(VAddr va_range, u64 cb_range) {
    bool is_inside = false;
    texture_cache.ForEachImageInRegion(va_range, cb_range, [&](ImageId, Image& image) {
        is_inside = is_inside || (image.info.guest_address <= va_range &&
                            va_range + cb_range <= image.info.guest_address + image.info.guest_size);
    });
    return is_inside;
}

bool BufferCache::LayerGpuWriteToVram(VAddr va_range, u64 cb_range) {
    const int mode = LayerGpuWritesMode();
    const bool is_cpu_block =
        layer_cpu_blocks.Overlaps(va_range >> block_shift, ((va_range + cb_range - 1) >> block_shift) + 1);
    const bool is_vram = mode == 2 || (mode == 1 && !is_cpu_block);
    if (mode == 1 && is_cpu_block) {
        PsStats::g_cb_gpu_writes_kept_by_cpu.fetch_add(cb_range, std::memory_order_relaxed);
    }
    // PS_LAYER_WRITE_LOG=1 (diagnostics): large GPU writes kept in place, and the images near them.
    static const bool s_should_log = std::getenv("PS_LAYER_WRITE_LOG") != nullptr;
    if (s_should_log && !is_vram && cb_range >= (u64{1} << 20)) {
        static std::map<std::pair<VAddr, u64>, u32> s_seen;
        if (s_seen.size() < 64 && s_seen[{va_range, cb_range}]++ == 0) {
            std::string near;
            texture_cache.ForEachImageInRegion(va_range, cb_range, [&](ImageId, Image& image) {
                near += fmt::format(" [{:#x}+{:#x} {}x{} fmt {}]", image.info.guest_address,
                                    image.info.guest_size, image.info.size.width,
                                    image.info.size.height, u32(image.info.pixel_format));
            });
            std::printf("Layer write in place: %#llx+%#llx; images:%s\n",
                        (unsigned long long)va_range, (unsigned long long)cb_range,
                        near.empty() ? " none" : near.c_str());
        }
    }
    return is_vram;
}
} // namespace VideoCore

namespace VideoCore {
void BufferCache::LayerCopyBack(u64 first, u64 end) {
    LayerForgetGpuData();
    // The GPU's data in mirrors over [first, end) into the game's memory; the blocks stay valid
    // in VRAM (both copies equal now).
    std::vector<std::pair<u64, u64>> runs;
    Layer().ForEachValid(first, end, [&](u64 a, u64 b) { runs.emplace_back(a, b); });
    for (const auto& [a, b] : runs) {
        const VAddr from = a << block_shift;
        const u64 cb_range = (b - a) << block_shift;
        if (!gpu_modified_ranges.Intersects(from, cb_range)) {
            continue;
        }
        const auto mirror = Layer().MirrorAt(from);
        if (!mirror) {
            continue;
        }
        auto* p_buffer = static_cast<Buffer*>(mirror->p_owner);
        gpu_modified_ranges.ForEachInRange(from, cb_range, [&](VAddr w_from, VAddr w_to) {
            if (const auto target = LayerInPlace(w_from, w_to - w_from)) {
                const vk::BufferCopy copy{p_buffer->Offset(w_from), target->second, w_to - w_from};
                runtime.CopyBuffer(p_buffer, target->first, std::span{&copy, 1});
                layer_copied_back_bytes += w_to - w_from;
                layer_copy_back_tick = scheduler.CurrentTick();
            }
        });
        gpu_modified_ranges.Subtract(from, cb_range);
    }
}


// PS_LAYER_READ_TRAPS=1: what the GPU writes into a VRAM copy only is no access for the CPU until
// it is copied back (LayerReadTrapHit): the game or the translator reading it on the CPU would
// see stale data (on the PS4 the memory is one). Off by default for now.
bool BufferCache::LayerReadTrapsOn() {
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_LAYER_READ_TRAPS");
        return sz_env && sz_env[0] == '1';
    }();
    return s_is_on;
}

void BufferCache::LayerArmReadTraps(VAddr va_range, u64 cb_range) {
    if (!LayerReadTrapsOn() || cb_range == 0) {
        return;
    }
    constexpr u64 Page = 4096;
    const VAddr from = Common::AlignDown(va_range, Page);
    const VAddr to = Common::AlignUp(va_range + cb_range, Page);
    std::scoped_lock lk{layer_read_trap_mutex};
    if (layer_read_trapped.Contains(from, to - from)) {
        return;
    }
    layer_read_trapped.ForEachNotInRange(from, to - from, [&](VAddr gap, u64 cb_gap) {
        PsLayer::WriteTraps::Set(gap, cb_gap, PsLayer::WriteTraps::VramData, true);
    });
    layer_read_trapped.Add(from, to - from);
}

void BufferCache::LayerQueuePromotion(u64 first, u64 end) {
    // Hashed by the whole range: indexed by its first block alone, ranges starting in the same
    // block (vertex streams of one buffer) took each other's slot and 1 call in 8 walked its
    // blocks again under the module's lock. Looked at again each second too, since blocks become
    // known data without anything moving (loads, GPU writes).
    struct Memo {
        u64 first = ~0ULL, end = 0, generation = 0;
        u32 second = 0;
    };
    thread_local auto p_memo = std::make_unique<std::array<Memo, 4096>>(); // heap: see LayerBind
    auto& m = (*p_memo)[((first * 0x9E3779B97F4A7C15ull) ^ (end * 0xC2B2AE3D27D4EB4Full)) >> 52];
    const u32 second = PsStats::g_coarse_second.load(std::memory_order_relaxed);
    if (m.first == first && m.end == end && m.generation == layer_generation && m.second == second) {
        return;
    }
    m = {first, end, layer_generation, second};
    if (!GarlicInVramForLayer()) {
        return;
    }
    // And by block, since the rings the game writes each frame are new ranges every time but the
    // same blocks.
    struct BlockMemo {
        u64 block = ~0ULL, generation = 0;
        u32 second = 0;
    };
    thread_local auto p_block_memo = std::make_unique<std::array<BlockMemo, 4096>>();
    for (u64 block = first; block < end; ++block) {
        auto& b = (*p_block_memo)[(block * 0x9E3779B97F4A7C15ull) >> 52];
        if (b.block == block && b.generation == layer_generation && b.second == second) {
            continue;
        }
        b = {block, layer_generation, second};
        if (!Layer().AnyValid(block, block + 1) && VramEligible(block)) {
            promote_candidates.Add({block, block + 1});
        }
    }
}
} // namespace VideoCore

namespace VideoCore {
bool BufferCache::IsInVramLayer(VAddr va_range, u64 cb_range) const {
    return Layer().Resolve(va_range, cb_range).kind == PsLayer::Resolution::Kind::Mirror;
}
} // namespace VideoCore

namespace VideoCore {
bool BufferCache::LayerAllKnown(u64 first, u64 end) {
    for (u64 block = first; block < end; ++block) {
        if (!Layer().AnyValid(block, block + 1) && !LayerEligible(block)) {
            return false;
        }
    }
    return true;
}
} // namespace VideoCore

namespace VideoCore {
void BufferCache::LayerMakeVolatile(const std::vector<u64>& blocks) {
    for (const u64 block : blocks) {
        if (Layer().AnyValid(block, block + 1) || !Layer().ResolveInPlace(block << block_shift, block_size)) {
            continue; // in VRAM already, or not a whole block of a chunk (no source to refresh from)
        }
        // Volatile before the page table entries are rewritten (they keep it in place).
        layer_volatile.Add({block, block + 1});
        LayerPromoteRun(block, block + 1);
        if (!Layer().AnyValid(block, block + 1)) {
            layer_volatile.Subtract(block, block + 1);
        }
    }
}

void BufferCache::LayerRefreshVolatile(VAddr va_range, u64 cb_range) {
    PsStats::Timer timer{PsStats::g_t_layer[5]};
    // The binding's bytes in volatile blocks: uploaded from the game's memory by the
    // SynchronizeMemory that follows (as CPU-modified: a GPU copy from its chunk, in stream
    // order). Only reading bindings use them (LayerBind).
    const u64 first = va_range >> block_shift;
    const u64 end = ((va_range + cb_range - 1) >> block_shift) + 1;
    layer_volatile.ForEachInRange(first, end, [&](const Interval& iv) {
        const VAddr from = std::max<VAddr>(va_range, iv.start << block_shift);
        const VAddr to = std::min<VAddr>(va_range + cb_range, iv.end << block_shift);
        if (from >= to) {
            return;
        }
        memory_tracker->UnmarkRegionAsGpuModified(from, to - from);
        gpu_modified_ranges.Subtract(from, to - from);
        memory_tracker->MarkRegionAsCpuModified(from, to - from);
        layer_volatile_refresh_bytes += to - from;
    });
}
} // namespace VideoCore

// psport PS_LAYER_MEMORY: blocks with a VRAM copy are write-protected (PS_LAYER_WRITE_TRAPS=0:
// off). The game's own code writes some of them without telling us (its heap's headers, parts of
// its resource loaders), and a stale copy then gives exploding vertices and garbage textures. A
// write there is let through at once; the block goes back in place before the next binding (the
// writers seen often stay there: NoteCpuWrite).
namespace VideoCore {
namespace {
std::mutex s_layer_trap_mutex;
std::unordered_set<u64> s_layer_trap_armed;              // blocks protected now
std::vector<std::pair<u64, u64>> s_layer_trap_hits;      // block, guest instruction (0: announced)
std::atomic<bool> s_is_layer_trap_pending{false};
std::atomic<u64> s_c_layer_trap_faults{0};
u32 s_layer_trap_shift = 16;

bool LayerTrapsOn() {
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_LAYER_WRITE_TRAPS");
        return !(sz_env && sz_env[0] == '0');
    }();
    return s_is_on;
}

void LayerUnprotect(u64 block) {
    // Writable again unless another owner (an image there) still traps it (pslayer_write_traps.h).
    PsLayer::WriteTraps::Set(block << s_layer_trap_shift, u64{1} << s_layer_trap_shift,
                             PsLayer::WriteTraps::Mirror, false);
}

bool LayerTrapHandler(void* p_context, void* p_fault_address) {
    if (!Common::IsWriteError(p_context)) {
        return false;
    }
    const u64 block = reinterpret_cast<u64>(p_fault_address) >> s_layer_trap_shift;
    {
        std::scoped_lock lk{s_layer_trap_mutex};
        if (!s_layer_trap_armed.erase(block)) {
            return false;
        }
        // Under the lock, before the hit is visible: LayerProcessTraps re-arms a watched block
        // under this lock, and an unprotect after it took that new trap away while the block still
        // counted as armed, so the CPU's next writes went unseen and the VRAM copy stayed stale.
        LayerUnprotect(block); // the write runs again, now through
        s_layer_trap_hits.emplace_back(block, u64(Common::GetRip(p_context)));
    }
    s_c_layer_trap_faults.fetch_add(1, std::memory_order_relaxed);
    s_is_layer_trap_pending.store(true, std::memory_order_release);
    return true;
}
} // namespace

void BufferCache::LayerArmTraps(u64 first, u64 end) {
    if (!LayerTrapsOn()) {
        return;
    }
    static const bool s_has_registered = [this] {
        s_layer_trap_shift = static_cast<u32>(block_shift);
        Core::Signals::Instance()->RegisterAccessViolationHandler(LayerTrapHandler, 0);
        return true;
    }();
    (void)s_has_registered;
    // One mprotect per run of blocks, not per block: each call interrupts every core running the
    // game's threads (TLB shootdown), and a mirror of 100 MiB made 1600 of them (the GPU thread
    // 250 ms in the kernel while an area streamed in).
    std::scoped_lock lk{s_layer_trap_mutex};
    u64 run_first = 0, run_end = 0;
    const auto flush = [&] {
        if (run_end > run_first) {
            PsStats::Timer timer{PsStats::g_t_layer[0]};
            PsStats::g_c_layer_mprotects.fetch_add(1, std::memory_order_relaxed);
            PsLayer::WriteTraps::Set(run_first << block_shift, (run_end - run_first) << block_shift,
                                     PsLayer::WriteTraps::Mirror, true);
        }
        run_first = run_end = 0;
    };
    int prot = 0, type = -1;
    uintptr_t vma_start = 0, vma_end = 0;
    for (u64 block = first; block < end; ++block) {
        if (layer_volatile.Contains(block, block + 1)) {
            flush(); // refreshed for every binding anyway
            continue;
        }
        const VAddr address = block << block_shift;
        // Only what the game's CPU may write (SCE_KERNEL_PROT_CPU_WRITE), whole in one mapping.
        if (address < vma_start || address >= vma_end) {
            if (!runtime_memory_vma_info(address, &prot, &type, &vma_end)) {
                vma_start = vma_end = 0;
                flush();
                continue;
            }
            vma_start = address;
        }
        if (!(prot & 0x2) || vma_end < address + block_size || !s_layer_trap_armed.insert(block).second) {
            flush();
            continue;
        }
        if (run_end != block) {
            flush();
            run_first = block;
        }
        run_end = block + 1;
    }
    flush();
}

void BufferCache::LayerDisarmTraps(u64 first, u64 end, bool should_demote) {
    std::scoped_lock lk{s_layer_trap_mutex};
    if (s_layer_trap_armed.empty()) {
        return;
    }
    // As LayerArmTraps: one mprotect per run of blocks that were armed.
    u64 run_first = 0, run_end = 0;
    const auto flush = [&] {
        if (run_end > run_first) {
            PsStats::Timer timer{PsStats::g_t_layer[0]};
            PsStats::g_c_layer_mprotects.fetch_add(1, std::memory_order_relaxed);
            PsLayer::WriteTraps::Set(run_first << s_layer_trap_shift,
                                     (run_end - run_first) << s_layer_trap_shift,
                                     PsLayer::WriteTraps::Mirror, false);
        }
        run_first = run_end = 0;
    };
    for (u64 block = first; block < end; ++block) {
        if (!s_layer_trap_armed.erase(block)) {
            flush();
            continue;
        }
        if (run_end != block) {
            flush();
            run_first = block;
        }
        run_end = block + 1;
        if (should_demote) {
            s_layer_trap_hits.emplace_back(block, 0);
            s_is_layer_trap_pending.store(true, std::memory_order_release);
        }
    }
    flush();
}

void BufferCache::LayerProcessTraps() {
    LayerForgetGpuData(); // before any demotion copies the GPU's data back
    if (!s_is_layer_trap_pending.load(std::memory_order_acquire)) {
        return; // every binding asks: no clock read when there is nothing to do
    }
    PsStats::Timer timer{PsStats::g_t_layer[3]};
    std::vector<std::pair<u64, u64>> hits;
    {
        std::scoped_lock lk{s_layer_trap_mutex};
        hits.swap(s_layer_trap_hits);
        s_is_layer_trap_pending.store(false, std::memory_order_release);
    }
    for (const auto& [block, rip] : hits) {
        const bool is_watched = layer_watched.Contains(block, block + 1);
        // The CPU wrote beside a range the GPU writes in VRAM: the GPU's bytes there are copied
        // back below before the block goes in place.
        const bool is_edge = layer_edge_blocks.Contains(block, block + 1) && Layer().AnyValid(block, block + 1);
        ++layer_trap_kinds[rip == 0 ? 0 : (is_watched ? 1 : 2) + (IsGuestCode(rip) ? 0 : 2)];
        // The CPU uses this block: GPU writes there stay in place from now on (until it is handed
        // out again), so a VRAM copy never holds GPU data the CPU's data could meet.
        layer_cpu_blocks.Add({block, block + 1});
        // A watched block the CPU wrote (the game's code or ours: HLE command buffers): it stays in
        // VRAM, uploaded again; with PS_LAYER_WATCH_HOT=1 however often (as the model of 0.3
        // uploads what the CPU wrote), else until it counts as often written (NoteCpuWrite: then
        // a request sends it in place). An announced write may still be under way (a file read):
        // in place as below, no trap set meanwhile. The runtime's file reads retry once their
        // pages fault again (a trap set between its touch and the read).
        if (rip != 0 && !(is_watched && LayerWatchHot())) {
            NoteCpuWrite(block << block_shift, rip); // often written: in place for good
        }
        if (rip != 0 && is_watched && Layer().AnyValid(block, block + 1)) {
            LayerRefreshWatched(block);
            continue;
        }
        // Written by the CPU since its copy was made: in place from now on, until it is known
        // data again. An announced write: the GPU's data beside its bytes is copied back first
        // (LayerForgetGpuData took its bytes out). A write nobody announced: the CPU reuses the
        // memory, the GPU's data there is dead (copying it back would land over the CPU's).
        LayerDemote(block, block + 1, rip == 0 || is_edge, 3);
    }
}

u64 BufferCache::LayerTrapFaults() {
    return s_c_layer_trap_faults.load(std::memory_order_relaxed);
}
} // namespace VideoCore

namespace VideoCore {
bool BufferCache::LayerMirrored(VAddr va_range, u64 cb_range) {
    if (cb_range == 0 || !LayerActive()) {
        return false;
    }
    // Asked for every small binding by two threads: the answer memoized per thread while no
    // block gained or lost its copy (the module's lock taken only then). By the blocks covered,
    // not by address, since the game's constants move through a ring: a new address every draw
    // within the same few blocks.
    struct Memo {
        u64 first = 0, end = 0, version = ~0ULL;
        bool is_mirrored = false;
    };
    thread_local auto p_memo = std::make_unique<std::array<Memo, 1024>>(); // heap: see LayerBind
    const u32 shift = Layer().BlockShift();
    const u64 first = va_range >> shift, end = ((va_range + cb_range - 1) >> shift) + 1;
    auto& m = (*p_memo)[(first ^ (end << 7)) & (p_memo->size() - 1)];
    const u64 version = Layer().ValidVersion();
    if (m.first == first && m.end == end && m.version == version) {
        return m.is_mirrored;
    }
    const bool is_mirrored = Layer().AnyValid(first, end);
    m = {first, end, version, is_mirrored};
    return is_mirrored;
}

// PS_LAYER_VERIFY=1 (diagnostics, slow): a VRAM copy bound for reading while the game's bytes
// changed with no trap hit or announced write is reported as stale, with what is known of its block.
bool BufferCache::LayerVerifyOn() {
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_LAYER_VERIFY");
        const bool is_enabled = sz_env && sz_env[0] == '1';
        if (is_enabled) {
            std::printf("Layer verify: VRAM copies checked against the game's memory at every "
                        "binding (PS_LAYER_VERIFY=1, slow)\n");
        }
        return is_enabled;
    }();
    return s_is_on;
}

namespace {
/// The game's bytes of each mirrored block as last bound (its VRAM copy matched them then).
struct VerifiedBlock {
    u64 hash = 0;
    u64 frame = 0;
    std::unique_ptr<u8[]> p_bytes;
};
std::mutex s_layer_verify_mutex;
std::unordered_map<u64, VerifiedBlock> s_layer_verified;
u64 s_c_layer_verify_reports = 0;
} // namespace

void BufferCache::LayerVerifyForget(u64 first, u64 end) {
    std::scoped_lock lk{s_layer_verify_mutex};
    for (u64 block = first; block < end; ++block) {
        s_layer_verified.erase(block);
    }
}

void BufferCache::LayerVerifyMirror(VAddr va_range, u64 cb_range, bool is_texel_buffer) {
    constexpr u64 MaxReports = 300;
    const u64 first = va_range >> block_shift;
    const u64 end = ((va_range + cb_range - 1) >> block_shift) + 1;
    const u64 frame = PsStats::g_frame_number.load(std::memory_order_relaxed);
    std::scoped_lock lk{s_layer_verify_mutex};
    for (u64 block = first; block < end; ++block) {
        const VAddr va_block = block << block_shift;
        // The GPU's data is newer than the game's memory, and volatile blocks are refreshed at
        // every binding: neither can be compared.
        if (!Layer().AnyValid(block, block + 1) || layer_volatile.Contains(block, block + 1) ||
            gpu_modified_ranges.Intersects(va_block, block_size)) {
            s_layer_verified.erase(block);
            continue;
        }
        auto& entry = s_layer_verified[block];
        if (entry.p_bytes && entry.frame == frame) {
            continue;
        }
        const auto* p_now = reinterpret_cast<const u8*>(va_block);
        const u64 hash = XXH3_64bits(p_now, block_size);
        const bool is_announced = memory_tracker->IsRegionCpuModified(va_block, block_size);
        bool is_armed;
        {
            std::scoped_lock trap_lk{s_layer_trap_mutex};
            is_armed = s_layer_trap_armed.contains(block);
        }
        u32 c_open_pages = 0;
        for (VAddr va_page = va_block; va_page < va_block + block_size; va_page += 4096) {
            c_open_pages += (PsLayer::WriteTraps::Reasons(va_page) & PsLayer::WriteTraps::Mirror) ? 0 : 1;
        }
        // Only a trap that should have seen the write is reported: armed with pages open (a
        // refresh uploaded meanwhile also changes the bytes with nothing pending).
        if (entry.p_bytes && hash != entry.hash && !is_announced && is_armed && c_open_pages != 0 &&
            s_c_layer_verify_reports < MaxReports) {
            ++s_c_layer_verify_reports;
            u64 cb_changed = 0, off_first = ~0ull, off_last = 0;
            for (u64 i = 0; i < block_size; ++i) {
                if (entry.p_bytes[i] != p_now[i]) {
                    ++cb_changed;
                    off_first = std::min(off_first, i);
                    off_last = i;
                }
            }
            bool was_zero = true;
            for (u64 i = off_first; i <= off_last && was_zero; ++i) {
                was_zero = entry.p_bytes[i] == 0;
            }
            int prot = 0, type = -1;
            uintptr_t va_vma_end = 0;
            const bool has_vma = runtime_memory_vma_info(va_block, &prot, &type, &va_vma_end);
            const auto part = [&](const IntervalList<>& list) {
                return list.Contains(va_block, va_block + block_size)   ? "all"
                       : list.Overlaps(va_block, va_block + block_size) ? "part"
                                                                        : "none";
            };
            std::printf("Layer verify: STALE %#llx (binding %#llx+%#llx%s): %llu bytes changed at "
                        "+%#llx..+%#llx (before: %s); %u pages without their trap, page traps %#x, "
                        "vma %s prot %#x type %d; asset %s, GPU-written %s, watched %d, CPU block %d, "
                        "dynamic %d, frames since last check %llu\n",
                        (unsigned long long)va_block, (unsigned long long)va_range,
                        (unsigned long long)cb_range, is_texel_buffer ? " texel" : "",
                        (unsigned long long)cb_changed, (unsigned long long)off_first,
                        (unsigned long long)off_last, was_zero ? "zeros" : "data", c_open_pages,
                        PsLayer::WriteTraps::Reasons(va_block), has_vma ? "yes" : "no", prot, type,
                        part(asset_bytes), part(gpu_written_bytes),
                        layer_watched.Contains(block, block + 1) ? 1 : 0,
                        layer_cpu_blocks.Contains(block, block + 1) ? 1 : 0,
                        dynamic_blocks.Contains(block) ? 1 : 0, (unsigned long long)(frame - entry.frame));
        }
        if (!entry.p_bytes) {
            entry.p_bytes = std::make_unique<u8[]>(block_size);
        }
        std::memcpy(entry.p_bytes.get(), p_now, block_size);
        entry.hash = hash;
        entry.frame = frame;
    }
}
} // namespace VideoCore
