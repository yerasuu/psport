// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: what running it takes, whatever PS_GAME_PROFILE says (Game::Title): its releases,
// its launch (launch.h), the changes its saves need and the port's pages in its System menu.
#include "game_profile.h"
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include "effects.h"
#include "game_menu.h"
#include "launch.h"

namespace Game {
extern const Profile BloodborneProfile;

namespace {
/// The retail releases (the content IDs from PR #117): Bloodborne in the US, EU, UK, JP
/// and Asia, then the editions with The Old Hunters (US, EU, JP, Asia). Without its serial here a
/// region ran on the generic paths alone, and the new memory model crashed on entering the world
/// (issue #119, the US release: a null read in the allocator, 0x2085xxx). Its bookkeeping is in
/// GPU-mapped pages; the profile's DynamicWriters (profile.cpp) keep them in place from the first
/// write (the hooks alone do not matter for it: PS_GUEST_HOOKS=0 runs; PS_GAME_PROFILE=none
/// crashes).
constexpr const char* Serials[] = {"CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
                                   "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"};

/// The top-level folders of the game's dvdroot_ps4, with adhoc and font, which mods add: the debug
/// menu's fonts, which the retail game does not ship.
constexpr const char* ModFolders[] = {"action", "adhoc",    "chr",   "event", "facegen", "font",   "map",
                                      "menu",   "movie",    "msg",   "mtd",   "obj",     "other",  "param",
                                      "paramdef", "parts",  "remo",  "script", "sfx",    "shader", "sound"};

constexpr RuntimeQuirkId Quirks[] = {RUNTIME_QUIRK_bloodborne_save_sound_flag};

/// The "sound hack" (rainvmaker, from the Diegolix29 shadPS4 fork): the game data save carries a
/// flag at 0x204E of SPRJ0005/userdata0010; with it clear, parts of the game's audio (e.g. the
/// player's weapon sounds) never play. A WORKAROUND quirk until #348 finds the general fix:
/// PS_QUIRKS=-bloodborne_save_sound_flag, or the older PS_SOUND_HACK=0, leaves saves untouched.
void PrepareSaves(const char* sz_saves, const char* sz_save_title) {
    if (!PsQuirk::Active(RUNTIME_QUIRK_bloodborne_save_sound_flag)) {
        return;
    }
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

} // namespace

extern const Title BloodborneTitle = {
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
    .pfn_prepare_saves = PrepareSaves,
    .pfn_patch_image = PsGameMenu::PatchImage,
    .pfn_before_present = PsGameMenu::Poll,
    .online = {.sz_game_server = "https://ss4.scej-network.jp:20443",
               .sz_community_server = "https://thehuntersdream.com",
               .community_api_port = 18671,
               .sz_summon_route = "/summon_messenger"},
    .quirks = Quirks,
    .p_profile = &BloodborneProfile,
};

} // namespace Game
