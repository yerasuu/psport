// psport: the C side's view of the game titles (games/): launch files, mods, patches and checks.
#include <cstdio>
#include <string>
#include "../psport_gpu.h"
#include "game_profile.h"

namespace {
int CheckFiles(const PsGameFiles* p_files, char* sz_out_kind, size_t cb_kind, char* sz_out_advice, size_t cb_advice) {
    const Game::Title* p_title = Game::Find(p_files->sz_serial);
    std::string kind, advice;
    if (!p_title || p_title->pfn_check_files(*p_files, kind, advice)) {
        return 0;
    }
    std::snprintf(sz_out_kind, cb_kind, "%s", kind.c_str());
    std::snprintf(sz_out_advice, cb_advice, "%s", advice.c_str());
    return 1;
}

int AddPatches(const PsGameFiles* p_files, const PsPatchServices* p_services) {
    const Game::Title* p_title = Game::Find(p_files->sz_serial);
    return p_title && p_title->pfn_add_patches(*p_files, *p_services) ? 1 : 0;
}

void PatchImage(unsigned char* p_image, uint64_t cb_image) {
    if (const Game::Profile* p_profile = Game::Active(); p_profile && p_profile->pfn_patch_image) {
        p_profile->pfn_patch_image(p_image, cb_image);
    }
    if (const Game::Title* p_title = Game::Launched(); p_title && p_title->pfn_patch_image) {
        p_title->pfn_patch_image(p_image, cb_image);
    }
}
} // namespace

extern "C" PsTitleInfo psport_gpu_title(const char* sz_serial) {
    const Game::Title* p_title = Game::Find(sz_serial);
    if (!p_title) {
        return {};
    }
    const bool has_patch_image = p_title->pfn_patch_image || (p_title->p_profile && p_title->p_profile->pfn_patch_image);
    return {
        .sz_name = p_title->sz_name,
        .pp_serials = p_title->serials.data(),
        .c_serials = p_title->serials.size(),
        .pp_mod_folders = p_title->mod_folders.data(),
        .c_mod_folders = p_title->mod_folders.size(),
        .sz_patch_file = p_title->sz_patch_file,
        .has_presets = p_title->pfn_add_patches ? 1 : 0,
        .pfn_prepare_environment = p_title->pfn_prepare_environment,
        .pfn_check_files = p_title->pfn_check_files ? CheckFiles : nullptr,
        .pfn_add_patches = p_title->pfn_add_patches ? AddPatches : nullptr,
        .pfn_patch_image = has_patch_image ? PatchImage : nullptr,
        .p_quirks = p_title->quirks.data(),
        .c_quirks = p_title->quirks.size(),
        .online = {p_title->online.sz_game_server, p_title->online.sz_community_server,
                   p_title->online.community_api_port, p_title->online.sz_summon_route},
    };
}
