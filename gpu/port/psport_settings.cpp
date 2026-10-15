// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_profile.h"

extern "C" {
// runtime_config.c
const char* runtime_config_path(void);
int runtime_config_is_read(void);
int runtime_config_entry(std::size_t i_entry, const char** pp_out_key, const char** pp_out_value);
const char* runtime_env(const char* sz_name, const char* sz_default);
}

namespace PsSettings {

namespace {

const char* Path() {
    return runtime_config_path();
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "menu_language") {
        if (value == "en") v.menu_language = MenuLanguage::English;
        else if (value == "ru") v.menu_language = MenuLanguage::Russian;
        else if (value == "es") v.menu_language = MenuLanguage::Spanish;
        else std::printf("Settings: unknown menu_language '%s' (expected en, ru or es)\n", value.c_str());
    } else if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.should_sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.should_jitter = i != 0;
    } else if (key == "reactive") {
        v.should_use_reactive = i != 0;
    } else if (key == "object_motion") {
        v.should_use_object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.should_show_fps = i != 0;
    } else if (key == "menu_pos") {
        float x = -1.0f, y = -1.0f;
        if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 1.0f && y >= 0.0f &&
            y <= 1.0f) {
            v.menu_x = x;
            v.menu_y = y;
        }
    } else if (key == "fsr4_auto_exposure") {
        v.should_use_fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.should_invert_fsr4_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        const auto switches = Game::Effects().switches;
        for (int e = 0; e < v.c_effects; ++e) {
            if (key == switches[e].sz_key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values s_values;
    return s_values;
}

const char* MenuText(const char* sz_english, const char* sz_russian, const char* sz_spanish) {
    const MenuLanguage language = Get().menu_language;
    return language == MenuLanguage::Russian ? sz_russian : language == MenuLanguage::Spanish ? sz_spanish : sz_english;
}

void Load() {
    auto& v = Get();
    const auto switches = Game::Effects().switches;
    if (switches.size() > MaxEffects) {
        std::printf("Settings: the game profile offers %zu effect switches; the first %d are kept\n",
                    switches.size(), MaxEffects);
    }
    v.c_effects = int(std::min(switches.size(), std::size_t(MaxEffects)));
    for (int e = 0; e < v.c_effects; ++e) {
        v.effects[e] = switches[e].is_default_on;
    }
    // psport: the menu's language until one is chosen in it (menu_language in psport.ini): the
    // launcher's (PS_MENU_LANGUAGE), else the system's, as the launcher picks its own.
    {
        const char* sz_lang = runtime_env("PS_MENU_LANGUAGE", nullptr);
        if (!sz_lang || !*sz_lang) {
            for (const char* sz_key : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
                if (const char* sz_value = runtime_env(sz_key, nullptr); sz_value && *sz_value) {
                    sz_lang = sz_value;
                    break;
                }
            }
        }
        const auto is = [sz_lang](char first, char second) {
            return sz_lang && std::tolower(static_cast<unsigned char>(sz_lang[0])) == first && std::tolower(static_cast<unsigned char>(sz_lang[1])) == second;
        };
        v.menu_language = is('r', 'u')   ? MenuLanguage::Russian
                          : is('e', 's') ? MenuLanguage::Spanish
                                         : MenuLanguage::English;
    }
    const char *sz_key, *sz_value;
    for (std::size_t i_entry = 0; runtime_config_entry(i_entry, &sz_key, &sz_value); ++i_entry) {
        Set(v, sz_key, sz_value);
    }
    if (runtime_config_is_read()) {
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* sz_env = runtime_env("PS_UPSCALER", nullptr)) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(sz_env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"PS_FSR_SHARPNESS", "sharpness"},        {"PS_JITTER", "jitter"},
        {"PS_REACTIVE", "reactive"},              {"PS_REACTIVE_SCALE", "reactive_scale"},
        {"PS_REACTIVE_THRESHOLD", "reactive_threshold"}, {"PS_REACTIVE_MAX", "reactive_max"},
        {"PS_UPSCALE_PRESET", "preset"},            {"PS_OBJECT_MOTION", "object_motion"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* sz_value = runtime_env(env, nullptr)) {
            Set(v, key, sz_value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.is_object_motion_at_startup = v.should_use_object_motion;
    for (int e = 0; e < v.c_effects; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool is_fsr4_supported, bool is_fsr411_supported) {
    auto& v = Get();
    v.is_fsr4_supported = is_fsr4_supported;
    v.is_fsr411_supported = is_fsr4_supported && is_fsr411_supported;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.is_fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.is_fsr411_supported)) {
        v.sz_fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool is_available, const char* sz_problem) {
    auto& v = Get();
    v.is_dlss_supported = is_available;
    static std::string s_kept;
    s_kept = sz_problem ? sz_problem : "";
    v.sz_dlss_problem = is_available || s_kept.empty() ? nullptr : s_kept.c_str();
    if (v.upscaler == UpscalerDlss && !is_available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    s_kept.empty() ? "the DLSS bridge or NVIDIA's DLSS library is missing" : s_kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    return runtime_env("PS_RENDER_RES", "")[0] != '\0';
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    // The keys the menu writes, in this order; the rest of the file stays as it is: the
    // launcher's controls (key.* / pad.*) and its other keys, comments.
    std::vector<std::pair<std::string, std::string>> keys;
    const auto put = [&](const char* sz_key, std::string value) { keys.emplace_back(sz_key, std::move(value)); };
    const auto fixed = [](float value, int digits) {
        char sz_text[32];
        std::snprintf(sz_text, sizeof(sz_text), "%.*f", digits, value);
        return std::string(sz_text);
    };
    const auto flag = [](bool is_on) { return std::string(is_on ? "1" : "0"); };
    put("menu_language", v.menu_language == MenuLanguage::Russian   ? "ru"
                         : v.menu_language == MenuLanguage::Spanish ? "es"
                                                                    : "en");
    put("upscaler", UpscalerName(v.upscaler));
    put("preset", std::to_string(v.preset.load()));
    put("sharpen", flag(v.should_sharpen));
    put("sharpness", fixed(v.sharpness, 2));
    put("jitter", flag(v.should_jitter));
    put("reactive", flag(v.should_use_reactive));
    put("object_motion", flag(v.should_use_object_motion));
    put("reactive_scale", fixed(v.reactive_scale, 2));
    put("reactive_threshold", fixed(v.reactive_threshold, 2));
    put("reactive_max", fixed(v.reactive_max, 2));
    put("debug_view", std::to_string(v.debug_view.load()));
    put("show_fps", flag(v.should_show_fps));
    put("fsr4_auto_exposure", flag(v.should_use_fsr4_auto_exposure));
    put("fsr4_invert_jitter", flag(v.should_invert_fsr4_jitter));
    // Read by the game profile at start (its pfn_add_patches).
    const Game::GameEffects effects = Game::Effects();
    for (int e = 0; e < v.c_effects; ++e) {
        put(effects.switches[e].sz_key, flag(v.effects[e]));
    }
    if (effects.has_model_detail) {
        put("model_lod", std::to_string(v.model_lod.load()));
    }
    put("output_res", std::to_string(OutputWidths[v.output_res]) + "x" +
                          std::to_string(OutputHeights[v.output_res]));
    // Read by run.sh at start.
    put("live_resolution", v.live_resolution < 0 ? "auto" : flag(v.live_resolution != 0));
    if (v.menu_x >= 0.0f && v.menu_y >= 0.0f) {
        put("menu_pos", fixed(v.menu_x, 4) + "," + fixed(v.menu_y, 4));
    }

    std::string out;
    bool has_lines = false;
    std::vector<bool> written(keys.size());
    if (FILE* p_file = std::fopen(Path(), "r")) {
        char sz_line[512];
        while (std::fgets(sz_line, sizeof(sz_line), p_file)) {
            has_lines = true;
            std::string text{sz_line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
                std::string key = text.substr(0, eq);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                const auto it = std::find_if(keys.begin(), keys.end(),
                                             [&](const auto& k) { return k.first == key; });
                if (it != keys.end()) {
                    out += it->first + "=" + it->second + "\n";
                    written[size_t(it - keys.begin())] = true;
                    continue;
                }
            }
            out += text + "\n";
        }
        std::fclose(p_file);
    }
    if (!has_lines) {
        out = "# psport settings (in-game menu: Insert / L3+R3)\n";
    }
    for (size_t k = 0; k < keys.size(); ++k) {
        if (!written[k]) {
            out += keys[k].first + "=" + keys[k].second + "\n";
        }
    }
    // Replaced whole: a crash while writing leaves the old file.
    const std::string temporary = std::string(Path()) + ".tmp";
    FILE* p_file = std::fopen(temporary.c_str(), "w");
    bool is_ok = p_file && std::fwrite(out.data(), 1, out.size(), p_file) == out.size();
    is_ok = p_file && std::fclose(p_file) == 0 && is_ok;
    if (!is_ok || std::rename(temporary.c_str(), Path()) != 0) {
        std::printf("Settings: cannot write %s\n", Path());
    }
}

float PresetScale(int preset) {
    static constexpr float s_scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return s_scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* s_names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return s_names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* s_names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return s_names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace PsSettings
