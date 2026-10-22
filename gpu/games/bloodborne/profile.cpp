// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (update 1.09): what the translator knows about its code (Game::Profile; its title is
// title.cpp). Addresses are those of the 1.09 executable (CheckFiles, game_check.cpp, makes sure
// that is the one running); every retail release runs the same one.
#include "game_profile.h"
#include <cstdlib>
#include "aspect.h"
#include "button_prompts.h"
#include "frame_rate.h"
#include "gnm_hooks.h"
#include "guest_hooks.h"
#include "heap_sites.h"
#include "job_step_probe.h"
#include "menu_pointer.h"
#include "mouse_look.h"
#include "workarounds.h"
#include "scene_skip.h"

namespace Game {
namespace {
constexpr std::uint64_t ImageBase = 0x800000000ull;

/// Reverse engineering: 0x2ab7350 computes vertices on the CPU (cloth) into a ~128 MiB ring, each
/// block written now and then; 0x20858a0 is the game's allocator, its bookkeeping in GPU-mapped
/// pages.
constexpr CodeRange DynamicWriters[] = {
    {ImageBase + 0x2ab7350, ImageBase + 0x2aba310},
    {ImageBase + 0x20858a0, ImageBase + 0x2085e20},
};

void InstallHooks(const CoreServices& services) {
    // The resource loaders' copies, the GPU memory range allocator and the heap (guest_hooks).
    PsGuestHooks::Install(services.pfn_fresh_range);
}

/// After the launch's patches, before any of the game's code runs: its own hooks are installed
/// here rather than with install_hooks, so that no patch is written over one of them.
void PatchImage(unsigned char* p_image, std::uint64_t cb_image) {
    PsGnmHooks::PatchImage(p_image, cb_image);
    PsFrameRate::PatchImage(p_image, cb_image);
    // PS_GUEST_HOOKS=0: nothing more of the game's code hooked (the frame rate stays with the
    // patches then: patches.cpp).
    if (const char* sz_env = std::getenv("PS_GUEST_HOOKS"); sz_env && sz_env[0] == '0') {
        return;
    }
    PsMouseLook::PatchImage(p_image, cb_image);
    PsMenuPointer::PatchImage(p_image, cb_image);
    PsAspect::PatchImage(p_image, cb_image);
    PsJobStepProbe::PatchImage(p_image, cb_image);
}

void FilterPad(std::uint32_t* p_buttons, const std::uint8_t* p_sticks) {
    PsMenuPointer::FilterPad(p_buttons, p_sticks);
    PsSceneSkip::FilterPad(p_buttons);
}

void ReportStats() {
    PsGuestHooks::Report();
    PsWorkarounds::Report();
    PsJobStepProbe::Report();
    if (static int s_heap_reports = 0; ++s_heap_reports % 6 == 1) {
        PsHeapSites::Report();
    }
}

/// Scaleform's draws, the first stencil and movie pass among them, and the title menu's while it
/// animates its highlight: without those its frames were shown at the game's 1080p between
/// output-size ones, a flicker (PS_UI_TRACE=1). A native-size viewport alone also matches every
/// fullscreen post pass while the game's targets stay at 1080p.
constexpr std::uint64_t UiMovieShaders[] = {0x34e8a281, 0x81d336ce, 0x09957251, 0x24042a9b, 0xa400228b,
                                            0xbb4d5f8c, 0xe94b5b06, 0x9951d199, 0x44072322, 0xa334a845};

/// PS_LABEL_TRAP=2: the small-object pool's release, and the branch of the GPU range collector
/// (0x26aa860) taken for a block whose label reads 4 (done).
constexpr LabelReleaseCode LabelRelease = {
    .release = ImageBase + 0x263b9b0,
    .collector_done = ImageBase + 0x26aa986,
};
} // namespace

extern const Profile BloodborneProfile = {
    .pfn_install_hooks = InstallHooks,
    .pfn_patch_image = PatchImage,
    .pfn_drives_frame_rate = PsFrameRate::IsUnlocked,
    .dynamic_writers = DynamicWriters,
    // The game's buffer-copy compute shader (not the export-stage copy shader).
    .buffer_copy_shader = 0xfefebf9f,
    .pfn_driver_commands_written = PsGnmHooks::NoteDriverWrite,
    .pfn_commands_submitted = PsGnmHooks::CheckSubmission,
    .pfn_frame_overlap_unsafe = PsHeapSites::OverlapLeaks,
    // The game keeps two frame contexts and builds the next frame in the one of the frame before
    // the previous: its first two submissions are translated while the GPU finishes the previous
    // frame (1080p, Yahar'gul: 101 -> 154 FPS). With 8 or more, or no wait at all, its heap leaked
    // (~2 resource objects a frame, out of memory after minutes).
    .c_submits_ahead = 2,
    // Its scene textures have little or no anisotropic filtering: behind the upscaler, surfaces
    // seen at a grazing angle (stairs, floors, walls along the view) stayed blurred.
    .anisotropy = 16,
    // The post-processing combine, and the UI's first vertex shader. Characters' bone palettes
    // are 656 bytes and up, weapons' and props' 2 to 8 bones.
    .upscaler = {.scene_combine_shader = 0x9a9cf8a9,
                 .ui_shader = 0x34e8a281,
                 .ui_movie_shaders = UiMovieShaders,
                 .scene_width = 1920,
                 .scene_height = 1080,
                 .post_divisor = 2,
                 .camera = {.cb_constants = 864,
                            .far_plane = 3000.0f,
                            .off_dw_far = 0,
                            .off_dw_render_size = 4,
                            .off_dw_view = 8,
                            .off_dw_inverse_view = 180,
                            .off_dw_projection = 52},
                 .bone_palettes = {.cb_bone = 48,
                                   .cb_min_small_skeleton = 96,
                                   .cb_min_skeleton = 640,
                                   .cb_max_skeleton = 16384}},
    .pfn_display_aspect = PsAspect::SetDisplayAspect,
    .pfn_report_stats = ReportStats,
    .label_release = LabelRelease,
    .small_object_pool = {.off_header = 0xfc0, .cb_block = 0x30},
    .mouse = {.pfn_camera_look = PsMouseLook::Turn,
              .pfn_window_event = PsMenuPointer::WindowEvent,
              .pfn_wants_pointer = PsMenuPointer::WantsPointer,
              .pfn_filter_pad = FilterPad},
    .pfn_skip_scene = PsSceneSkip::Press,
    .p_glyphs = &PsButtonPrompts::Glyphs,
};

} // namespace Game
