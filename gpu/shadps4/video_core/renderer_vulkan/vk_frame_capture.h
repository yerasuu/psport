// SPDX-License-Identifier: GPL-2.0-or-later
// psport: frame analyzer. Creating the file named by PS_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in a game's frame (Game::UpscalerPlacement).
// PS_CAPTURE_SUMMARIZE=1 adds, for the recorded frame only, each draw's depth, stencil and blend
// state as the guest set it and as translated for Vulkan, and a value summary of every target at
// the end of its pass, of the render targets a pass samples at its first draw and of depth copy
// destinations, read back from a 64x64 grid of texels; 2 adds a 16x16 grid of the first channel.
// It also gives every draw its viewport, scissor, rasterizer and index/vertex inputs, Vulkan
// occlusion and pipeline statistics queries around it, and every pass that writes depth or stencil
// whole-image counts of the depth and stencil texels it changed.

#pragma once

#include <atomic>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace AmdGpu {
union Regs;
}

namespace VideoCore {
struct Image;
struct ImageInfo;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

class Runtime;
class GraphicsPipeline;
struct DynamicState;

class FrameCapture {
public:
    /// A pass attachment: the texture-cache image and the subresource the pass renders into.
    struct PassTarget {
        VideoCore::ImageId id{};
        u32 i_level{};
        u32 i_layer{};
    };

    /// The draw's index and vertex range as the rasterizer resolved it.
    struct DrawInputs {
        VAddr va_indices{}; ///< 0 for a draw without indices
        u32 cb_index{};     ///< 2 or 4; 0 without indices
        u32 c_indices{};
        u32 c_instances{};
        u32 vertex_offset{};
        u32 first_instance{};
        /// The Vulkan buffers the draw is bound to, one per fetched attribute, and its index
        /// buffer: the GPU's view of the inputs, which the CPU's lags behind for meshes a
        /// shader writes.
        std::span<const vk::Buffer> vertex_buffers;
        std::span<const vk::DeviceSize> vertex_offsets;
        std::span<const vk::DeviceSize> vertex_sizes;
        vk::Buffer index_buffer{};
        u64 off_index_buffer{};
        u64 cb_index_buffer{};
    };

    /// Queries a draw's own command closure begins and ends around the draw, so that they stay
    /// inside its render pass instance; null pools when there are none.
    struct DrawQueries {
        vk::QueryPool occlusion{};
        vk::QueryPool statistics{};
        u32 i_query{};
        bool is_precise{};
    };

    /// Rasterizer construction: where summaries resolve images and record their readbacks.
    static void Attach(VideoCore::TextureCache& texture_cache, Runtime& runtime);

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

    /// Called after the draw's textures were bound and its attachments transitioned, before
    /// the scheduler begins rendering: summaries may end the current rendering here.
    /// `sz_route` names an attachment redirection (scene proxy, upscaler output), or null.
    static void BeginPass(std::span<const PassTarget> colors, PassTarget depth,
                          const char* sz_route);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 c_indices, u32 c_instances);
    /// After Draw: the draw's depth and stencil state, accumulated over the pass.
    static void DepthUse(bool is_test, bool is_write, bool is_stencil);
    /// GPU thread, before recording a draw, with PS_CAPTURE_SUMMARIZE while recording: query
    /// slots for it. `is_occlusion_free`: no other occlusion query can be active around the draw.
    static DrawQueries TakeDrawQueries(bool is_occlusion_free);
    /// After Draw, with PS_CAPTURE_SUMMARIZE: the draw's depth, stencil, blend and rasterizer
    /// state, guest registers next to the Vulkan state recorded for it, and its geometry.
    static void DrawState(const AmdGpu::Regs& regs, const DynamicState& dynamic,
                          const GraphicsPipeline& pipeline, const DrawInputs& inputs);
    /// Before a transfer or dispatch that is not a pass: the open pass's targets are summarized
    /// before it can change them. It may submit and wait for the GPU, so a dispatch calls it
    /// before binding anything a new command buffer would lose.
    static void EndPass();
    /// A copy between depth surfaces outside any pass (DB override copy), kept in order; called
    /// after the copy was recorded, so the destination's summary holds its result.
    static void DepthCopy(const VideoCore::Image& src, VideoCore::ImageId src_id,
                          const VideoCore::Image& dst, VideoCore::ImageId dst_id, bool is_depth,
                          bool is_stencil);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    /// After the dispatch was recorded: what it wrote is copied back (first dispatch of an
    /// entry, with PS_CAPTURE_SUMMARIZE).
    static void AfterDispatch();
    /// A guest buffer binding of the stage being bound (the dispatch's, for compute). A null
    /// `h_buffer` is a binding the shader reaches through the page table (PS_LAYER_MEMORY).
    static void BoundBuffer(u32 i_slot, VAddr va_buffer, u64 cb_buffer, bool is_written,
                            vk::Buffer h_buffer, u64 off_buffer);
    /// A bound texture or storage image. `va_requested` is the T#'s address (a stencil plane
    /// read through its depth image differs from the image's); `view` the view the descriptor
    /// got; `sz_route` a replacement of the image's own view (scene proxy, substitute), or null.
    static void Sampled(const VideoCore::Image& image, VideoCore::ImageId id, VAddr va_requested,
                        vk::ImageView view, bool is_storage, const char* sz_route);
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
