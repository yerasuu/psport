// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (update 1.09): what the translator knows about its code. Addresses are those of the
// 1.09 executable (game_check.py makes sure that is the one running); every retail release runs
// the same one.
#include "game_profile.h"
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include "game_menu.h"
#include "gnm_hooks.h"
#include "guest_hooks.h"
#include "heap_sites.h"

namespace Game {
namespace {
constexpr std::uint64_t ImageBase = 0x800000000ull;

/// The retail releases (scripts/game_check.py SUPPORTED_TITLES): Bloodborne in the US, EU, UK, JP
/// and Asia, then the editions with The Old Hunters (US, EU, JP, Asia). Without its serial here a
/// region ran on the generic paths alone, and the new memory model crashed on entering the world
/// (issue #119, the US release: a null read in the allocator, 0x2085xxx). Its bookkeeping is in
/// GPU-mapped pages; DynamicWriters below keeps them in place from the first write (the hooks
/// alone do not matter for it: PS_GUEST_HOOKS=0 runs; PS_GAME_PROFILE=none crashes).
constexpr const char* Serials[] = {"CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
                                   "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"};

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
void PrepareSaves(const char* saves, const char* save_title) {
    if (const char* env = std::getenv("PS_SOUND_HACK"); env && env[0] == '0') {
        return;
    }
    DIR* users = opendir(saves);
    if (!users) {
        return;
    }
    while (const dirent* user = readdir(users)) {
        if (user->d_name[0] == '.') {
            continue;
        }
        const std::string path =
            std::string(saves) + "/" + user->d_name + "/" + save_title + "/SPRJ0005/userdata0010";
        std::FILE* file = std::fopen(path.c_str(), "r+b");
        if (!file) {
            continue;
        }
        const int old = std::fseek(file, 0x204E, SEEK_SET) ? EOF : std::fgetc(file);
        if (old != EOF && old != 1 && !std::fseek(file, 0x204E, SEEK_SET)) {
            std::fputc(1, file);
            std::printf("Game profile: Bloodborne sound flag set in %s (was %d)\n", path.c_str(), old);
        }
        std::fclose(file);
    }
    closedir(users);
}

void InstallHooks(const CoreServices& services) {
    // The resource loaders' copies, the GPU memory range allocator and the heap (guest_hooks).
    BbGuestHooks::Install(services.fresh_range);
}

void PatchImage(unsigned char* image, std::uint64_t size) {
    BbGnmHooks::PatchImage(image, size);
    BbGameMenu::PatchImage(image, size);
}

void ReportStats() {
    if (static int heap_reports = 0; ++heap_reports % 6 == 1) {
        BbHeapSites::Report();
    }
}

/// PS_LABEL_TRAP=2: the small-object pool's release, and the branch of the GPU range collector
/// (0x26aa860) taken for a block whose label reads 4 (done).
constexpr LabelReleaseCode LabelRelease = {
    .release = ImageBase + 0x263b9b0,
    .collector_done = ImageBase + 0x26aa986,
};
} // namespace

extern const Profile BloodborneProfile = {
    .name = "Bloodborne",
    .serials = Serials,
    .install_hooks = InstallHooks,
    .patch_image = PatchImage,
    .dynamic_writers = DynamicWriters,
    .prepare_saves = PrepareSaves,
    // The game's buffer-copy compute shader (not the export-stage copy shader).
    .buffer_copy_shader = 0xfefebf9f,
    .driver_commands_written = BbGnmHooks::NoteDriverWrite,
    .commands_submitted = BbGnmHooks::CheckSubmission,
    .frame_overlap_unsafe = BbHeapSites::OverlapLeaks,
    // The game keeps two frame contexts and builds the next frame in the one of the frame before
    // the previous: its first two submissions are translated while the GPU finishes the previous
    // frame (1080p, Yahar'gul: 101 -> 154 FPS). With 8 or more, or no wait at all, its heap leaked
    // (~2 resource objects a frame, out of memory after minutes).
    .c_submits_ahead = 2,
    // Its scene textures have little or no anisotropic filtering: behind the upscaler, surfaces
    // seen at a grazing angle (stairs, floors, walls along the view) stayed blurred.
    .anisotropy = 16,
    .report_stats = ReportStats,
    .before_present = BbGameMenu::Poll,
    .label_release = LabelRelease,
    .small_object_pool = {.header_offset = 0xfc0, .block_size = 0x30},
};

} // namespace Game
