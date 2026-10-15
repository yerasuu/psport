// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (update 1.09): what the translator knows about its code. Addresses are those of the
// 1.09 executable (CheckFiles, game_check.cpp, makes sure that is the one running); every retail
// release runs the same one.
#include "game_profile.h"
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include "effects.h"
#include "game_menu.h"
#include "gnm_hooks.h"
#include "guest_hooks.h"
#include "heap_sites.h"
#include "launch.h"

namespace Game {
namespace {
constexpr std::uint64_t ImageBase = 0x800000000ull;

/// The retail releases (the content IDs from PR #117): Bloodborne in the US, EU, UK, JP
/// and Asia, then the editions with The Old Hunters (US, EU, JP, Asia). Without its serial here a
/// region ran on the generic paths alone, and the new memory model crashed on entering the world
/// (issue #119, the US release: a null read in the allocator, 0x2085xxx). Its bookkeeping is in
/// GPU-mapped pages; DynamicWriters below keeps them in place from the first write (the hooks
/// alone do not matter for it: PS_GUEST_HOOKS=0 runs; PS_GAME_PROFILE=none crashes).
constexpr const char* Serials[] = {"CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
                                   "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"};

/// The top-level folders of the game's dvdroot_ps4, with adhoc and font, which mods add: the debug
/// menu's fonts, which the retail game does not ship.
constexpr const char* ModFolders[] = {"action", "adhoc",    "chr",   "event", "facegen", "font",   "map",
                                      "menu",   "movie",    "msg",   "mtd",   "obj",     "other",  "param",
                                      "paramdef", "parts",  "remo",  "script", "sfx",    "shader", "sound"};

/// Reverse engineering: 0x2ab7350 computes vertices on the CPU (cloth) into a ~128 MiB ring, each
/// block written now and then; 0x20858a0 is the game's allocator, its bookkeeping in GPU-mapped
/// pages.
constexpr CodeRange DynamicWriters[] = {
    {ImageBase + 0x2ab7350, ImageBase + 0x2aba310},
    {ImageBase + 0x20858a0, ImageBase + 0x2085e20},
};

/// The "sound hack" (rainvmaker, from the Diegolix29 shadPS4 fork): the game data save carries a
/// flag at 0x204E of SPRJ0005/userdata0010; with it clear, parts of the game's audio (e.g. the
/// player's weapon sounds) never play. PS_SOUND_HACK=0 leaves saves untouched.
void PrepareSaves(const char* sz_saves, const char* sz_save_title) {
    if (const char* sz_env = std::getenv("PS_SOUND_HACK"); sz_env && sz_env[0] == '0') {
        return;
    }
    DIR* p_users = opendir(sz_saves);
    if (!p_users) {
        return;
    }
    while (const dirent* p_user = readdir(p_users)) {
        if (p_user->d_name[0] == '.') {
            continue;
        }
        const std::string path =
            std::string(sz_saves) + "/" + p_user->d_name + "/" + sz_save_title + "/SPRJ0005/userdata0010";
        std::FILE* p_file = std::fopen(path.c_str(), "r+b");
        if (!p_file) {
            continue;
        }
        const int old = std::fseek(p_file, 0x204E, SEEK_SET) ? EOF : std::fgetc(p_file);
        if (old != EOF && old != 1 && !std::fseek(p_file, 0x204E, SEEK_SET)) {
            std::fputc(1, p_file);
            std::printf("Game profile: Bloodborne sound flag set in %s (was %d)\n", path.c_str(), old);
        }
        std::fclose(p_file);
    }
    closedir(p_users);
}

void InstallHooks(const CoreServices& services) {
    // The resource loaders' copies, the GPU memory range allocator and the heap (guest_hooks).
    PsGuestHooks::Install(services.pfn_fresh_range);
}

void PatchImage(unsigned char* p_image, std::uint64_t cb_image) {
    PsGnmHooks::PatchImage(p_image, cb_image);
    PsGameMenu::PatchImage(p_image, cb_image);
}

void ReportStats() {
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
    .sz_name = "Bloodborne",
    .serials = Serials,
    .mod_folders = ModFolders,
    .sz_patch_file = PsLaunch::PatchFile,
    .pfn_prepare_environment = PsLaunch::PrepareEnvironment,
    .pfn_check_files = PsLaunch::CheckFiles,
    .pfn_add_patches = PsLaunch::AddPatches,
    .effects = {.switches = PsEffects::Switches,
                .has_model_detail = true,
                .hint = {"Motion blur and shadows from dynamic lights place a significant load on the GPU.",
                         "Размытие в движении и тени от динамических источников заметно нагружают GPU.",
                         "El desenfoque de movimiento y las sombras de luces dinámicas cargan bastante la GPU."}},
    .pfn_install_hooks = InstallHooks,
    .pfn_patch_image = PatchImage,
    .dynamic_writers = DynamicWriters,
    .pfn_prepare_saves = PrepareSaves,
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
    .pfn_report_stats = ReportStats,
    .pfn_before_present = PsGameMenu::Poll,
    .label_release = LabelRelease,
    .small_object_pool = {.off_header = 0xfc0, .cb_block = 0x30},
};

} // namespace Game
