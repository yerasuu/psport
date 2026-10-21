// SPDX-License-Identifier: GPL-2.0-or-later
// psport: camera motion vectors for temporal upscaling (docs/en/upscaler.md). The scene constants
// give the camera of each frame; the previous one is kept here. PS_DEBUG_MOTION=1 blends the
// motion vectors as colors into the frame before it is copied to the display.

#pragma once

#include <array>
#include <utility>
#include <vector>

#include "common/types.h"
#include "game_profile.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class ObjectMotion;

class CameraMotion {
public:
    CameraMotion(const Instance& instance, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~CameraMotion();
    void InvalidateHistory() { m_current.is_valid = m_previous.is_valid = false; }
    /// psport: whether this frame's camera is a cut from the previous one (CommitFrameCamera), once:
    /// temporal history from another shot must not be blended in.
    [[nodiscard]] bool TakeCut() noexcept {
        return std::exchange(m_is_cut, false);
    }
    void SetObjectMotion(ObjectMotion* p_motion) noexcept {
        m_p_object_motion = p_motion;
    }
    /// This frame's object motion image (after RecordMotion), for the debug view; or null.
    vk::ImageView ObjectMotionView() const noexcept;
    vk::Image ObjectMotionImage(u32 width, u32 height) const noexcept;

    void SetJitter(std::array<float, 2> value) noexcept {
        m_previous_jitter = m_jitter;
        m_jitter = value;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return m_has_debug_overlay || m_is_for_upscaler;
    }

    /// Both cameras and the scene depth of the current frame are known.
    [[nodiscard]] bool Ready() const noexcept {
        return m_current.is_valid && m_previous.is_valid && m_depth_id;
    }
    [[nodiscard]] VideoCore::ImageId Depth() const noexcept {
        return m_depth_id;
    }
    /// The render size in the scene constants (the game's viewport; its targets may be
    /// allocated larger, e.g. 1916x1080 for a 1916x1078 scene), or zero before the first camera.
    [[nodiscard]] std::array<u32, 2> RenderSize() const noexcept {
        return m_render_size;
    }
    /// Vertical field of view and near/far planes of the current camera.
    [[nodiscard]] float VerticalFov() const noexcept;
    [[nodiscard]] float Near() const noexcept;
    // Current projection, previous projection, and row Z of previous-view * inverse-view.
    [[nodiscard]] std::array<std::array<float, 4>, 3> TaaDepthParameters() const noexcept;
    /// psport: the matrices and jitter of the motion of this frame, printed with an upscaler dump
    /// (PS_DUMP_TRIGGER) for offline checks of the vectors.
    void PrintState(int frame) const;

    /// Records the motion vector pass (Scheduler::Record): `depth_view` (depth aspect, General
    /// layout) into `motion_view` (RG16F storage, General), pixels, previous minus current.
    void RecordMotion(vk::ImageView depth_view, vk::ImageView motion_view, u32 width, u32 height);

    /// Whether a constant buffer of this size may be the game profile's scene constants.
    [[nodiscard]] bool IsSceneConstants(u32 cb_buffer) const noexcept {
        return m_constants.cb_constants && cb_buffer == m_constants.cb_constants;
    }
    /// A bound constant buffer that IsSceneConstants: checks the scene constant signature.
    void OnConstants(const float* p_data);

    /// The G-buffer pass (5+ color targets): its depth is the scene depth.
    /// `x_sign`/`y_sign`: signs of the G-buffer pass's viewport x/y scale (window = ndc * scale +
    /// offset). The motion shaders take ndc as +y down the screen, so the projection's scales are
    /// multiplied by them: the game's viewport decides which way view +y goes, per frame.
    void OnGBufferPass(VideoCore::ImageId depth, float x_sign = 1.0f, float y_sign = 1.0f);

    /// The pass copying the finished frame (`frame`, the last target drawn) to the display: the
    /// frame's depth and camera are complete. Records the debug overlay, starts a new frame.
    void OnDisplayPass(VideoCore::ImageId frame);

private:
    struct Camera {
        std::array<float, 12> view{};     ///< world to view, 3x4 rows
        std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
        std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset
        bool is_valid = false;
    };

    void Overlay(VideoCore::ImageId frame);

    const Instance& m_instance;
    Scheduler& m_scheduler;
    VideoCore::TextureCache& m_texture_cache;
    Runtime& m_runtime;
    bool m_has_debug_overlay = false;
    bool m_is_for_upscaler = false;
    vk::UniqueDescriptorSetLayout m_motion_desc_layout;
    vk::UniquePipelineLayout m_motion_pipeline_layout;
    vk::UniquePipeline m_motion_pipeline;

    ObjectMotion* m_p_object_motion = nullptr;
    const Game::CameraConstants m_constants;
    Camera m_current, m_previous;
    bool m_has_frame_camera = false;
    /// psport: the camera of the G-buffer pass in progress (OnConstants), committed as the frame's
    /// camera once per frame (CommitFrameCamera: the motion pass or the display pass).
    Camera m_pass_camera;
    bool m_has_pass_camera = false;
    bool m_has_committed = false;
    bool m_is_cut = false;
    u64 m_c_cuts = 0;
    u32 m_c_frame_gbuffer_passes = 0;
    std::array<std::array<float, 3>, 4> m_pass_positions{};
    void CommitFrameCamera();
    std::array<float, 2> m_jitter{}, m_previous_jitter{};
    std::array<u32, 2> m_render_size{};
    VideoCore::ImageId m_depth_id{};
    float m_gbuffer_x_sign = 1.0f; ///< psport: signs of the G-buffer viewport scales (OnGBufferPass)
    float m_gbuffer_y_sign = 1.0f;

    vk::UniqueDescriptorSetLayout m_desc_layout;
    vk::UniquePipelineLayout m_pipeline_layout;
    vk::UniquePipeline m_overlay_pipeline;
    /// Debug: the last two finished frames (RGBA8 packed), for the reprojection check.
    std::unique_ptr<VideoCore::Buffer> m_frames[2];
    u32 m_i_frame = 0;
};

} // namespace Vulkan
