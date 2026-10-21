// psport: the C side's view of the game titles (games/): launch files, mods, patches, checks and effects.
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

PsLocalizedText Localized(const Game::LocalizedText& text) {
    return {text.sz_english, text.sz_russian, text.sz_spanish};
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
    const Game::OnlineExtension& extension = p_title->online.extension;
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
        .pfn_external_patch_refusal = p_title->pfn_external_patch_refusal,
        .pfn_replaced_patch_consequence = p_title->pfn_replaced_patch_consequence,
        .pfn_patch_image = has_patch_image ? PatchImage : nullptr,
        .p_quirks = p_title->quirks.data(),
        .c_quirks = p_title->quirks.size(),
        .online = {p_title->online.sz_game_server, p_title->online.sz_community_server,
                   p_title->online.community_api_port, p_title->online.sz_summon_route,
                   {extension.sz_name, extension.sz_switch, extension.sz_local_server,
                    extension.sz_local_server_args, extension.sz_local_switch, extension.sz_status_path}},
        .sz_default_controls = p_title->sz_default_controls,
    };
}

extern "C" const char* psport_gpu_title_controls(void) {
    const Game::Title* p_title = Game::Launched();
    return p_title ? p_title->sz_default_controls : nullptr;
}

extern "C" PsTitleEffects psport_gpu_title_effects(const char* sz_serial) {
    const Game::Title* p_title = Game::Find(sz_serial);
    if (!p_title) {
        return {};
    }
    const Game::GameEffects& effects = p_title->effects;
    return {effects.switches.size(), effects.has_model_detail ? 1 : 0, Localized(effects.hint)};
}

extern "C" int psport_gpu_title_effect_switch(const char* sz_serial, size_t i_switch, PsEffectSwitch* p_out_switch) {
    const Game::Title* p_title = Game::Find(sz_serial);
    if (!p_title || i_switch >= p_title->effects.switches.size()) {
        return 0;
    }
    const Game::EffectSwitch& effect = p_title->effects.switches[i_switch];
    *p_out_switch = {effect.sz_key, effect.is_default_on ? 1 : 0, Localized(effect.label), Localized(effect.hint)};
    return 1;
}
