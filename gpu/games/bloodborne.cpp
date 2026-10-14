// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne (update 1.09): what the translator knows about its code. Addresses are those of the
// 1.09 executable (game_check.py makes sure that is the one running); every retail release runs
// the same one.
#include "game_profile.h"
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include "bbport_game_menu.h"
#include "bbport_guest_hooks.h"

namespace Game {
namespace {
constexpr std::uint64_t ImageBase = 0x800000000ull;

/// The retail releases (scripts/game_check.py SUPPORTED_TITLES): Bloodborne in the US, EU, UK, JP
/// and Asia, then the editions with The Old Hunters (US, EU, JP, Asia). Without its serial here a
/// region ran on the generic paths alone, and the new memory model crashed on entering the world
/// (issue #119, the US release: a null read in the allocator, 0x2085xxx). Its bookkeeping is in
/// GPU-mapped pages; DynamicWriters below keeps them in place from the first write (the hooks
/// alone do not matter for it: BB_GUEST_HOOKS=0 runs; BB_GAME_PROFILE=none crashes).
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
/// player's weapon sounds) never play. BB_SOUND_HACK=0 leaves saves untouched.
void PrepareSaves(const char* saves, const char* save_title) {
    if (const char* env = std::getenv("BB_SOUND_HACK"); env && env[0] == '0') {
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
    // The resource loaders' copies, the GPU memory range allocator and the heap (bbport_guest_hooks).
    BbGuestHooks::Install(services.fresh_range);
}
} // namespace

extern const Profile BloodborneProfile = {
    .name = "Bloodborne",
    .serials = Serials,
    .install_hooks = InstallHooks,
    .patch_image = BbGameMenu::PatchImage,
    .dynamic_writers = DynamicWriters,
    .prepare_saves = PrepareSaves,
    // The game's buffer-copy compute shader (not the export-stage copy shader).
    .buffer_copy_shader = 0xfefebf9f,
};

} // namespace Game
