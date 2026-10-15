// SPDX-License-Identifier: GPL-2.0-or-later
// psport: frame analyzer. Creating the file named by PS_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in a game's frame (Game::UpscalerPlacement).

#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr va_presented);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr va_buffer);
    static bool IsDisplayBuffer(VAddr va_buffer);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return m_state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* pp_colors, u32 c_colors,
                          const VideoCore::ImageInfo* p_depth);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 c_indices, u32 c_instances);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    static void Sampled(const VideoCore::ImageInfo& info, bool is_storage);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr va_buffer, const void* p_data, u64 cb_data);
    static void Note(const char* sz_text);

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> m_state{Idle};
    static inline std::atomic<u64> m_c_flips{0};
    static inline std::atomic<VAddr> m_last_presented{0};
};

} // namespace Vulkan
