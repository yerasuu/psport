// SPDX-License-Identifier: GPL-2.0-or-later
// psport: object motion vectors for temporal upscaling (docs/en/history/ROADMAP.md, step 1).
//
// G-buffer shaders preserve clip positions for the exact indexed range of each draw.
// Matching includes vertex streams, index contents and base offsets. A missing match falls
// back to camera motion. See motion_history.h for CPU bookkeeping and its regression test.

#pragma once

#include <array>
#include "video_core/renderer_vulkan/motion_history.h"

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;
struct RenderState;

class ObjectMotion {
public:
    ObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~ObjectMotion();

    [[nodiscard]] bool Enabled() const noexcept {
        return m_is_enabled;
    }

    using DrawInfo = Motion::Draw;
    u32 PrepareDraw(const DrawInfo& draw);
    /// Small skeletons: whether the bone palette changed (Motion::History::Moving).
    bool Moving(const Motion::History::GateKey& key, u64 palette) {
        return m_history.Moving(key, palette);
    }
    /// Index range and topology hash of an indexed draw, reused across frames.
    template <class Scan>
    Motion::IndexRangeCache::Result IndexRange(const Motion::IndexRangeCache::Key& key,
                                               Scan&& scan) {
        return m_index_ranges.Get(key, m_frame, scan);
    }

    /// Rendering of a motion pipeline: the motion attachment (render-target size). False when
    /// it has no image: the attachment is then a null view the pipeline still declares.
    bool Attach(RenderState& state, u32 width, u32 height);

    /// Start of a frame (display pass).
    void OnFrameStart();
    void InvalidateHistory() { m_history.NextFrame(); m_history.NextFrame(); }

    /// The motion image for the compose pass (layout General after this call) and whether it
    /// holds this frame's vectors at `width` x `height`.
    vk::ImageView PrepareRead(u32 width, u32 height, bool& valid);
    /// The image of the last PrepareRead (layout General), or null.
    [[nodiscard]] vk::ImageView View() const noexcept {
        return m_p_read_target && m_p_read_target->view ? *m_p_read_target->view : vk::ImageView{};
    }
    [[nodiscard]] vk::Image Image(u32 width, u32 height) const noexcept {
        const Target* p_target = Find(width, height);
        return p_target && p_target->is_written ? vk::Image(p_target->image) : vk::Image{};
    }

private:
    /// psport: a motion image per render-target size. Scenes with G-buffer passes at two sizes
    /// (a reduced proxy and the full target) shared one image: every switch recreated it after
    /// a full GPU wait (Scheduler::Finish), twice a frame — ~70% of the draw recording thread's
    /// time and an idle GPU — and threw this frame's vectors away.
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
        bool is_written = false; ///< this frame's vectors are in the image
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        u64 used_frame = 0;
    };
    static constexpr u32 MaxTargets = 4;

    [[nodiscard]] const Target* Find(u32 width, u32 height) const noexcept {
        for (const auto& target : m_targets) {
            if (target.image && target.width == width && target.height == height) {
                return &target;
            }
        }
        return nullptr;
    }
    /// The image of this size, created when missing (replacing the one used longest ago; it is
    /// destroyed once the GPU is done with it, without waiting). Null when it cannot be
    /// allocated: the motion output is then dropped.
    Target* GetTarget(u32 width, u32 height);

    const Instance& m_instance;
    Scheduler& m_scheduler;
    bool m_is_enabled = false;

    // Parameter ring (host visible): FrameSlots frames of ParamsPerFrame entries (two u32x4),
    // element 0 all zero (motion off).
    static constexpr u32 FrameSlots = 4;
    static constexpr u32 ParamsPerFrame = 8192;
    // Positions (device local): two halves (current/previous frame) of vec4; element 0 is reserved.
    static constexpr u32 PositionsPerFrame = 4u << 20;
    Motion::History m_history{PositionsPerFrame};
    Motion::IndexRangeCache m_index_ranges;
    std::array<u64, FrameSlots> m_params_ticks{};
    vk::Buffer m_params_buffer{};
    VmaAllocation m_p_params_allocation{};
    u32* m_p_params_mapped{};
    vk::Buffer m_positions_buffer{};
    VmaAllocation m_p_positions_allocation{};

    u64 m_frame = 0;
    u32 m_c_params_used = 0;
    std::array<Target, MaxTargets> m_targets;
    const Target* m_p_read_target = nullptr; ///< of the last PrepareRead
};

} // namespace Vulkan
