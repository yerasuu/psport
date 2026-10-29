// SPDX-License-Identifier: GPL-2.0-or-later
// psport: frame generation (psport.ini frame_gen, PS_FRAME_GEN; docs/en/upscaler.md). From two
// finished frames of the game, FSR 3.1 frame interpolation (FireBurn/FSR-Vulkan, native Vulkan,
// any GPU) makes the frame between them; the Presenter shows it before the game's frame, half a
// game frame earlier. Depth and motion vectors are taken at the upscaler's trigger (the game
// profile's scene combine, before post-processing): the scene depth and CameraMotion's vectors.
// The picture before the UI is taken at the profile's first UI draw, so the UI is drawn over the
// generated frame instead of being interpolated.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;

namespace Vulkan {

class Instance;
class Scheduler;

/// Why a game frame was presented without a generated frame before it, counted per reason
/// (FrameGenStats::g_c_skipped) so that a run can say where the generated frames went.
enum class FrameGenSkip : u8 {
    None,             ///< generated and presented
    HdrOutput,        ///< HDR output presents without generation
    WithoutCapture,   ///< no scene depth and motion this frame (menu, movie, loading)
    Paused,           ///< paused because presenting fell behind (Presenter::UpdateFrameGenLimit)
    NoRoom,           ///< the display's refresh rate has no room for it (frame_gen_fit_display)
    NoMemory,         ///< the generated frame's image could not be made
    Failed,           ///< FSR 3.1 failed, frame generation is off since
    GameWaited,       ///< generated but dropped: the game waited for a presentation frame
    NoSwapchainImage, ///< generated but the swapchain gave no image for it
    SwitchedOff,      ///< switched off in the menu (PsSettings::is_frame_gen_live)
    Count,
};

/// psport: frame generation's counts for the exit report (registered once it is on).
namespace FrameGenStats {
inline std::atomic<u64> g_c_game_frames{0};     ///< game frames presented while it was on
inline std::atomic<u64> g_c_generated{0};       ///< frames generated
inline std::atomic<u64> g_c_presented{0};       ///< generated frames presented
inline std::atomic<u64> g_c_dropped{0};         ///< generated but not presented: the game waited for a frame
inline std::atomic<u64> g_c_without_capture{0}; ///< game frames without the scene's depth and motion (menus, movies)
inline std::atomic<u64> g_c_without_scene_pass{0}; ///< of those, the profile's scene pass (trigger) did not run
inline std::atomic<u64> g_c_without_scene_depth{0}; ///< of those, it ran but the scene had no depth to take
inline std::atomic<u64> g_us_without_capture{0};  ///< game time of the frames without capture, microseconds
inline std::atomic<u64> g_us_captured{0};         ///< game time of the frames with capture, microseconds
inline std::atomic<u64> g_c_ui_interpolated{0};   ///< generated without a picture before the UI
inline std::atomic<u64> g_c_restarted{0};         ///< generated with FSR 3.1's history reset (cut, gap)
inline std::atomic<u64> g_c_held{0};            ///< game frames not generated: paused, or no room on the display
inline std::atomic<u64> g_c_pauses{0};          ///< pauses because presenting fell behind
inline std::atomic<u64> g_c_failures{0};        ///< FSR 3.1 calls that failed (it is then off)
inline std::array<std::atomic<u64>, std::size_t(FrameGenSkip::Count)> g_c_skipped{}; ///< by FrameGenSkip
} // namespace FrameGenStats

/// Counts a game frame presented without a generated frame; the first of each reason is logged.
void CountFrameGenSkip(FrameGenSkip reason);

/// The game's camera of a frame: near and far planes, vertical field of view (radians).
struct FrameGenCamera {
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float vertical_fov = 1.0f;
};

class FrameGenerator {
public:
    /// The format of the frames read and written (the Presenter's frame images while it is on):
    /// storage-capable on every device, unlike B8G8R8A8.
    static constexpr vk::Format ColorFormat = vk::Format::eR8G8B8A8Unorm;
    struct Output {
        vk::Image image;
        vk::ImageView view;
    };

    /// FSR-Vulkan's fsr3FrameGenerationPrerequisites on `instance`'s device, which must also have
    /// been created with shaderStorageImageWriteWithoutFormat. `out_result`: the device and what was
    /// found, for the log and --list-features.
    static bool IsFsrSupported(const Instance& instance, std::string& out_result);

    FrameGenerator(const Instance& instance, Scheduler& scheduler);
    ~FrameGenerator();
    FrameGenerator(const FrameGenerator&) = delete;
    FrameGenerator& operator=(const FrameGenerator&) = delete;

    /// On in the settings and not failed.
    [[nodiscard]] bool IsWanted() const;
    /// A presented frame without generation (HDR output): its captured inputs are dropped and the
    /// next generation starts over.
    void Skip();
    /// Off (switched in the menu, or after a failure), at a frame boundary on the GPU thread: the
    /// FSR 3.1 context is destroyed and the depth, motion and UI-less copies are freed once the GPU
    /// has passed the commands recorded so far, which may still capture into them. All of them are
    /// made again at the first frame after it is switched on.
    void Release();

    /// At the upscaler's trigger, render size `w` x `h`: records the copy of the scene depth
    /// (`depth`: depth aspect view in General layout) and returns the motion vector image to
    /// write next (RG16F storage, General: pixels, previous minus current), or null when off.
    vk::ImageView BeginCapture(vk::CommandBuffer cmdbuf, vk::ImageView depth, u32 w, u32 h);
    /// After the motion vectors are written. `is_motion_valid` false: no previous camera or a
    /// cut, the next frame is not interpolated from this one.
    void EndCapture(vk::CommandBuffer cmdbuf, std::array<float, 2> jitter, bool is_motion_valid,
                    const FrameGenCamera& camera);
    /// The upscaler's trigger ran but the scene had no depth: counted apart from frames without
    /// the scene pass at all.
    void NoteSceneWithoutDepth() noexcept {
        m_is_scene_without_depth = true;
    }

    /// The game's picture before its UI (`source` in TransferSrcOptimal layout): copied, once a
    /// frame and only after BeginCapture.
    void CaptureHudless(vk::CommandBuffer cmdbuf, vk::Image source, vk::Format format, u32 w, u32 h);
    /// This frame's copy without UI as the presenter reads the display buffer in `format`: the
    /// copy's channel order with the sRGB decoding of `format` (the game's display pass draws the
    /// UI target into the display buffer), alpha one, ShaderReadOnlyOptimal. Null without one.
    vk::ImageView Hudless(vk::Format format, vk::Extent2D& out_size);

    /// The frame was composed into `color` (ColorFormat, General, `width` x `height`): records the
    /// frame between the previous one and it into `output` (storage, sampled and transfer source;
    /// left in General) and returns FrameGenSkip::None when it did, else why not (`held`: the
    /// reason `should_generate` is false). `hudless`: the same frame composed from
    /// Hudless() (General), or null: FSR 3.1 then interpolates the frame with its UI. Nothing for
    /// frames without a capture (menus, movies) or when `should_generate` is false (paused, no room
    /// on the display): the next generation starts over.
    FrameGenSkip Generate(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageView color_view,
                          vk::ImageUsageFlags color_usage, vk::Image hudless, vk::ImageView hudless_view,
                          u32 width, u32 height, const Output& output, bool should_generate, FrameGenSkip held);

private:
    bool EnsureInputs(u32 w, u32 h);
    bool CreatePipelines();
    bool GenerateFsr3(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageUsageFlags color_usage,
                      u32 width, u32 height, const Output& output, float frame_ms);
    /// Where `color` and `hudless` differ (the UI), `color` replaces the generated pixels.
    void ComposeUi(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView hudless,
                   const Output& output, u32 width, u32 height);
    void RetireFinished();
    void DestroyFsr3();
    /// FSR 3.1 failed for good: frame generation is switched off, with the reason in the settings.
    void Fail(const char* sz_reason);

    const Instance& m_instance;
    Scheduler& m_scheduler;

    // Inputs at render size, rewritten every captured frame.
    VideoCore::UniqueImage m_depth_image;  ///< R32F hardware depth
    VideoCore::UniqueImage m_motion_image; ///< RG16F pixels, previous - current
    vk::UniqueImageView m_depth_view, m_motion_view;
    u32 m_input_width = 0, m_input_height = 0;
    vk::UniqueDescriptorSetLayout m_inputs_desc_layout;
    vk::UniquePipelineLayout m_inputs_pipeline_layout;
    vk::UniquePipeline m_inputs_pipeline;
    vk::UniqueDescriptorSetLayout m_ui_desc_layout;
    vk::UniquePipelineLayout m_ui_pipeline_layout;
    vk::UniquePipeline m_ui_pipeline;

    // The picture before the UI, as the game drew it (its UI target's format).
    VideoCore::UniqueImage m_hudless_image;
    vk::Format m_hudless_format{};
    u32 m_hudless_width = 0, m_hudless_height = 0;
    struct HudlessView {
        vk::Format format;
        vk::UniqueImageView view;
    };
    std::vector<HudlessView> m_hudless_views;
    bool m_is_hudless_captured = false; ///< this frame's

    bool m_is_captured = false; ///< BeginCapture ran since the last Generate
    bool m_is_scene_without_depth = false; ///< NoteSceneWithoutDepth since the last Generate
    std::array<float, 2> m_capture_jitter{};
    FrameGenCamera m_capture_camera{};
    bool m_should_reset = true;
    bool m_is_released = false; ///< Release ran and nothing was generated since
    bool m_was_hudless = false;
    u32 m_c_hudless_changes = 0; ///< reported the first few times
    std::chrono::steady_clock::time_point m_last_generate{};

    // FSR 3.1: the context, and the frames it still holds imported views for until the GPU is done.
    FfxVkFsr3_3_1_6FrameGenerationContext* m_p_fsr3 = nullptr;
    u32 m_fsr3_width = 0, m_fsr3_height = 0, m_fsr3_render_width = 0, m_fsr3_render_height = 0;
    u64 m_fsr3_frame_id = 0;
    struct Pending {
        u64 frame_id;
        u64 tick;
    };
    std::deque<Pending> m_fsr3_pending;
};

} // namespace Vulkan
