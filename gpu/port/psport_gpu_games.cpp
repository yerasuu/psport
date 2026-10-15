// psport: the C side's view of the game profiles (games/): launch files, mods, patches and checks.
#include <cstdio>
#include <string>
#include "../psport_gpu.h"
#include "game_profile.h"

extern "C" void psport_gpu_patch_image(unsigned char* p_image, uint64_t cb_image) {
    if (const Game::Profile* p_game = Game::Active(); p_game && p_game->pfn_patch_image) {
        p_game->pfn_patch_image(p_image, cb_image);
    }
}

extern "C" const char* psport_gpu_game_serial(const char* sz_serial, size_t i_serial) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game && i_serial < p_game->serials.size() ? p_game->serials[i_serial] : nullptr;
}

extern "C" const char* psport_gpu_game_mod_folder(const char* sz_serial, size_t i_folder) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game && i_folder < p_game->mod_folders.size() ? p_game->mod_folders[i_folder] : nullptr;
}

extern "C" const char* psport_gpu_game_patch_file(const char* sz_serial) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game ? p_game->sz_patch_file : nullptr;
}

extern "C" void psport_gpu_prepare_game(const PsGameFiles* p_files) {
    if (const Game::Profile* p_game = Game::Find(p_files->sz_serial); p_game && p_game->pfn_prepare_environment) {
        p_game->pfn_prepare_environment();
    }
}

extern "C" int psport_gpu_check_game(const PsGameFiles* p_files, char* sz_out_kind, size_t cb_kind, char* sz_out_advice,
                                size_t cb_advice) {
    const Game::Profile* p_game = Game::Find(p_files->sz_serial);
    std::string kind, advice;
    if (!p_game || !p_game->pfn_check_files || p_game->pfn_check_files(*p_files, kind, advice)) {
        return 0;
    }
    std::snprintf(sz_out_kind, cb_kind, "%s", kind.c_str());
    std::snprintf(sz_out_advice, cb_advice, "%s", advice.c_str());
    return 1;
}

extern "C" int psport_gpu_game_has_presets(const char* sz_serial) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game && p_game->pfn_add_patches ? 1 : 0;
}

extern "C" int psport_gpu_game_patches(const PsGameFiles* p_files, const PsPatchServices* p_services) {
    const Game::Profile* p_game = Game::Find(p_files->sz_serial);
    if (!p_game || !p_game->pfn_add_patches) {
        return -1;
    }
    return p_game->pfn_add_patches(*p_files, *p_services) ? 1 : 0;
}
