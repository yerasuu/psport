// SPDX-License-Identifier: GPL-2.0-or-later
// psport: temporal upscaling of the game's HDR scene color (docs/en/upscaler.md). PS_UPSCALER=fsr3
// runs FSR 3.1 (FireBurn/FSR-Vulkan, native Vulkan) on the scene color right before the
// post-processing combine pass, with the scene depth and camera motion vectors, and writes the
// result back so the game's own post, tonemap and UI continue unchanged (Native AA preset).
//
// Guest targets stay at 1920x1080. Host scene targets use output/preset dimensions,
// resized at frame boundaries. At 1080p HDR reconstruction precedes post; other outputs
// reconstruct tonemapped scene proxies before output-size UI/display composition.
// PS_RENDER_RES retains the older startup-patched guest-resolution path.

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "game_profile.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableUpscaleContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;

/// psport PS_FINAL_DUMP_TRIGGER: the frame as presented (after FSR and post processing) is saved
/// as final_<w>x<h> in PS_DUMP_DIR when the trigger file exists (consumed). `image` in General.
void DumpFinalFrameIfDue(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::Image image, u32 width, u32 height, vk::Format format);

/// The scene combine and UI shaders that place the upscaler in the frame: PS_UPSCALE_BEFORE_CS
/// and PS_UI_TRIGGER_VS, else the game profile's (Game::Profile::upscaler), with the profile's
/// UI shaders, camera constants and bone palettes.
const Game::UpscalerPlacement& GetUpscalerPlacement();
/// Unless PS_UPSCALER=none, the upscaler runs where its scene combine is known; the camera and
/// object motion vectors and the scene jitter exist only for it.
bool IsUpscalerAvailable();

class TemporalUpscaler {
public:
    TemporalUpscaler(const Instance& instance, Scheduler& scheduler,
                     VideoCore::TextureCache& texture_cache, Runtime& runtime,
                     CameraMotion& camera_motion, SceneTargets& scene_targets);
    ~TemporalUpscaler();

    [[nodiscard]] bool Enabled() const noexcept {
        return m_is_enabled;
    }

    /// A draw into a full-size RGBA16F target with the scene depth: the scene color.
    void OnSceneColor(VideoCore::ImageId color);

    /// A blended (transparent) draw into the scene color that is not a full-screen pass: the
    /// first one of a frame snapshots the opaque scene for the reactive mask.
    void OnBlendedSceneDraw();

    /// A full-screen pass into the scene color: after a snapshot, the reactive mask is taken
    /// before it (the fog composite rewrites every pixel).
    void OnSceneComposite();

    /// Before a compute dispatch: the post-processing combine shader triggers the upscale.
    void OnDispatch(u64 cs_hash);

    /// Start of a frame in the command stream (display pass).
    bool OnFrameStart();
    bool RasterScaling() const;
    /// psport: LOD bias for scene materials, log2(render / output) while a reduced scene is
    /// upscaled (0 at native size, for TAA and without an upscaler).
    [[nodiscard]] float SceneMipBias() const;
    /// psport: the state RedirectColor/RedirectDepth/RasterScaling depend on, for memoizing a
    /// draw's render state (Rasterizer::BeginRendering).
    [[nodiscard]] u64 RedirectState() const noexcept {
        return u64(m_is_ui_phase) | u64(m_is_display_redirect) << 1 | u64(m_is_done_this_frame) << 2 |
               u64(m_ui_color.index) << 8 | u64(m_ui_depth.index) << 36;
    }


    /// This frame's sub-pixel jitter in pixels (screen x right, y down); zero when off and after
    /// the upscale (post and UI are not jittered).
    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        return m_is_done_this_frame ? std::array<float, 2>{} : m_jitter;
    }

    // Scaled presets.

    /// After identifying this draw's targets, before drawing any UI pixels. Menus use
    /// spatial background copy and native UI, without needing scene depth or FSR history.
    void OnDraw(u64 vs_hash, VideoCore::ImageId color, VideoCore::ImageId depth,
                bool is_native_viewport);

    /// A pass's first color target: the last render-size RGBA8 target before the UI is the
    /// game's finished frame.
    void OnColorTarget(VideoCore::ImageId color);

    struct Target {
        vk::ImageView view;
        vk::ImageLayout layout;
        u32 width, height;
        bool is_native_ui = false;
    };
    /// Render target redirection (UI passes, display pass): false when not redirected.
    /// `view` is the game's view of the image: redirected views mirror its format (sRGB) and,
    /// for sampling, its channel swizzle.
    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view,
                       Target& target);
    bool RedirectDepth(VideoCore::ImageId depth, Target& target);
    /// A sampled image replaced by the upscaled frame (the display pass).
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         vk::ImageView& view, vk::ImageLayout& layout);
    /// Whether RedirectSampled would replace `image` (it may record a barrier then).
    [[nodiscard]] bool RedirectsSampled(VideoCore::ImageId image) const {
        return m_is_display_redirect && image == m_ui_color;
    }

    /// Presenter: the output-size display buffer standing in for the guest one at `address`.
    struct Display {
        vk::Image image;
        vk::Format format;
        u32 width, height;
    };
    bool DisplayOverride(VAddr va_display, Display& display);
    /// PS_PRESENT_DUMP_TRIGGER: whether this presented frame is to be saved (consumes the trigger).
    bool PresentDumpDue();
    /// Saves `image` (General layout, 4 bytes a pixel) as present_<w>x<h> in PS_DUMP_DIR.
    void DumpPresented(vk::Image image, u32 width, u32 height, vk::Format format);

private:
    /// psport: views of guest images the upscaler reads, kept across frames: FSR 4 registers
    /// images by view in a registry of eight (a new view per frame filled it at Native AA).
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);
    struct ViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view;
        u64 last_use = 0;
    };
    std::array<ViewEntry, 6> m_view_cache{};
    u64 m_c_view_uses = 0;

    void Run();
    void RunScaled();
    void RunUiOnly(VideoCore::ImageId color, VideoCore::ImageId depth);
    void EnsureUiResources(u32 width, u32 height, vk::Format color, vk::Format depth);
    void PrepareUiDepth(VideoCore::ImageId depth);
    /// Render size below the scaled-preset output size (the resolution patch is on).
    [[nodiscard]] bool Scaled() const;
    /// A target of the patched render size: the game allocates it with aligned dimensions
    /// (a 1916x1078 scene in 1916x1080 targets).
    [[nodiscard]] bool RenderTarget(u32 w, u32 h) const {
        if (!m_is_scaled_session) return w == 1920 && h == 1080;
        return w >= m_render_width && h >= m_render_height && w < m_render_width + 8 &&
               h < m_render_height + 8;
    }
    /// The scene's size inside a `w` x `h` render target: the game's viewport.
    [[nodiscard]] std::array<u32, 2> SceneSize(u32 w, u32 h) const;
    // The scene color is drawn into a reduced SceneTargets proxy (live presets).
    [[nodiscard]] bool ReducedScene(const VideoCore::Image& color) const;
    /// Available and switched on (menu setting, toggle 1 << 24).
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool ReactiveOn() const;
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool is_hdr);
    void CreatePipelines();
    /// Records the reactive mask pass; false when there is no snapshot this frame.
    bool RecordReactive(vk::ImageView color_view);
    /// FSR 4 is selected, possible in this session (not PS_RENDER_RES) and has not failed.
    [[nodiscard]] bool UseFsr4() const;
    /// psport: DLSS selected and the bridge is ready (NVIDIA RTX, gpu/dlss_bridge).
    [[nodiscard]] bool UseDlss() const;
    /// Records DLSS into `cmdbuf` (output in General). `is_hdr`: linear scene color input.
    bool RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                    const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh, float frame_ms,
                    bool is_hdr);
    /// Records FSR 4 into m_output_image; on a permanent failure FSR 3 takes over.
    bool RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color, Fsr4Upscaler::Image depth,
                    u32 w, u32 h, u32 ow, u32 oh, float frame_ms);
    void RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView depth);
    /// Sharpness above 1 for FSR 3/4 (their RCAS stops at 1): one more RCAS pass over the target
    /// (m_output_image, or the 8-bit UI image with ldr) in General layout after the upscaler.
    void ExtraSharpen(vk::Image target, bool is_ldr, u32 w, u32 h);

    const Instance& m_instance;
    Scheduler& m_scheduler;
    VideoCore::TextureCache& m_texture_cache;
    Runtime& m_runtime;
    CameraMotion& m_camera_motion;
    SceneTargets& m_scene_targets;
    int m_applied_preset = -1;
    int m_applied_upscaler = -1;
    bool m_is_dispatched_last_frame = false;
    bool m_is_last_active = false, m_is_last_jitter = false;


    bool m_is_enabled = false;
    bool m_has_failed = false;
    /// An FSR 3 dispatch recorded on a recording thread failed; `m_has_failed` at the next frame.
    std::atomic<bool> m_has_dispatch_failed{false};
    u64 m_trigger_hash = 0;
    VideoCore::ImageId m_scene_color{};
    bool m_is_done_this_frame = false;
    u32 m_c_preset_file_frames = 0; ///< PS_PRESET_FILE polling
    int m_applied_output = -1;
    bool m_is_snapshot_taken = false;
    bool m_is_opaque_valid = false;
    bool m_is_mask_ready = false;
    std::array<float, 2> m_jitter{};
    u32 m_i_jitter = 0;
    bool m_should_reset = true;
    u64 m_frame_id = 0;
    std::chrono::steady_clock::time_point m_last_frame{};

    u32 m_width = 0, m_height = 0;             ///< render size
    u32 m_out_width = 0, m_out_height = 0;     ///< output size of the context
    bool m_is_context_hdr = true;
    u32 m_target_width = 1920, m_target_height = 1080; ///< output size of scaled presets
    u64 m_ui_trigger_vs = 0;
    VideoCore::ImageId m_ldr_target{};
    VideoCore::ImageId m_ui_color{}, m_ui_depth{};
    bool m_is_ui_phase = false;         ///< from the upscale to the next display pass
    const bool m_is_ui_trace_on = std::getenv("PS_UI_TRACE") != nullptr; ///< diagnostics
    std::string m_ui_trace;
    bool m_is_display_redirect = false; ///< the display pass of an upscaled frame
    bool m_is_ui_read_barrier = false;
    VideoCore::UniqueImage m_ui_image;
    VideoCore::UniqueImage m_ui_depth_image;
    vk::UniqueImageView m_ui_view;
    vk::UniqueImageView m_ui_depth_view;
    bool m_is_depth_blit = false;
    bool m_is_scaled_session = false;
    u32 m_ui_width = 0, m_ui_height = 0;
    u32 m_render_width = 0, m_render_height = 0;
    vk::Format m_ui_format = vk::Format::eUndefined;
    vk::Format m_ui_depth_format = vk::Format::eUndefined;
    /// Views of the port's images in the formats/swizzles the game's views use.
    struct MirrorView {
        vk::Format format;
        vk::ComponentMapping mapping;
        vk::UniqueImageView view;
    };
    vk::ImageView Mirror(vk::Image image, std::vector<MirrorView>& views, vk::Format format,
                         vk::ComponentMapping mapping);
    std::vector<MirrorView> m_ui_views;
    struct DisplayImage {
        VideoCore::UniqueImage image;
        std::vector<MirrorView> views;
        vk::Format format{};
        u32 width = 0, height = 0;
        bool is_valid = false;
    };
    std::mutex m_display_mutex;
    int m_c_present_dumps_remaining = 0, m_i_present_dump = 0;
    std::unordered_map<VAddr, DisplayImage> m_displays;
    FfxVkPortableUpscaleContext* m_p_context = nullptr;
    bool m_is_resources_ready = false; ///< images below match width/height/out size
    bool m_has_fsr4_resources = false;  ///< made for FSR 4 (no FSR 3 context)
    bool m_has_taa_resources = false;
    std::unique_ptr<Fsr4Upscaler> m_fsr4;
    bool m_has_fsr4_failed = false;
    bool m_has_dlss_failed = false;
    VideoCore::UniqueImage m_motion_image;
    VideoCore::UniqueImage m_output_image;
    vk::UniqueImageView m_motion_view;
    vk::UniqueImageView m_output_view;
    VideoCore::UniqueImage m_opaque_image;   ///< scene color before the blended draws
    VideoCore::UniqueImage m_reactive_image; ///< R8 reactive mask
    vk::UniqueImageView m_opaque_view;
    vk::UniqueImageView m_reactive_view;
    vk::UniqueDescriptorSetLayout m_reactive_desc_layout;
    vk::UniquePipelineLayout m_reactive_pipeline_layout;
    vk::UniquePipeline m_reactive_pipeline;
    vk::UniqueDescriptorSetLayout m_merge_desc_layout;
    vk::UniquePipelineLayout m_merge_pipeline_layout;
    vk::UniquePipeline m_merge_pipeline;
    std::array<VideoCore::UniqueImage, 2> m_taa_history;
    std::array<vk::UniqueImageView, 2> m_taa_history_views;
    u32 m_taa_next = 0;
    vk::UniqueSampler m_taa_sampler;
    vk::UniqueDescriptorSetLayout m_taa_desc_layout;
    vk::UniquePipelineLayout m_taa_pipeline_layout;
    vk::UniquePipeline m_taa_pipeline;
    vk::UniqueDescriptorSetLayout m_taa_sharpen_desc_layout;
    vk::UniquePipelineLayout m_taa_sharpen_pipeline_layout;
    vk::UniquePipeline m_taa_sharpen_pipeline;
    vk::UniquePipeline m_taa_sharpen_ldr_pipeline;
    // psport: FSR 4 in linear light on the scaled presets (fsr4_color.comp): the decoded input
    // and the passes that decode it and encode the output again.
    VideoCore::UniqueImage m_fsr4_linear_image;
    bool m_is_fsr4_linear_frame = false; ///< this frame's FSR 4 input was decoded (and the output encoded)
    vk::UniqueImageView m_fsr4_linear_view;
    vk::UniqueDescriptorSetLayout m_fsr4_decode_desc_layout;
    vk::UniquePipelineLayout m_fsr4_decode_pipeline_layout;
    vk::UniquePipeline m_fsr4_decode_pipeline;
    vk::UniqueDescriptorSetLayout m_fsr4_encode_desc_layout;
    vk::UniquePipelineLayout m_fsr4_encode_pipeline_layout;
    vk::UniquePipeline m_fsr4_encode_pipeline;
    vk::UniqueSampler m_fsr4_linear_sampler;
    vk::UniqueDescriptorSetLayout m_fsr4_reactive_desc_layout;
    vk::UniquePipelineLayout m_fsr4_reactive_pipeline_layout;
    vk::UniquePipeline m_fsr4_reactive_pipeline;
    /// psport: FSR 4 takes no reactive mask: its output is blended with `color` (the frame it
    /// upscaled, render size, General layout, same colour space as the output) where the mask
    /// marks blended effects (fsr4_reactive.comp). Recorded after FSR 4 into `cmdbuf`.
    void RecordFsr4Reactive(vk::CommandBuffer cmdbuf, vk::ImageView color, u32 w, u32 h, u32 ow,
                            u32 oh);
    // ExtraSharpen: a copy of the upscaled frame (RCAS reads neighbours) and the target views.
    VideoCore::UniqueImage m_extra_sharpen_image;
    vk::UniqueImageView m_extra_sharpen_view;
    u32 m_extra_sharpen_width = 0, m_extra_sharpen_height = 0;
    vk::UniqueImageView m_ui_storage_view;
};

} // namespace Vulkan
