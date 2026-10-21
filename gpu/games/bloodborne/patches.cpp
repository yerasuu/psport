// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne's built-in patches, from the community's patches/Bloodborne.xml: the PS_FPS presets,
// the effect switches and model detail of psport.ini (the in-game menu, the launcher), PS_PATCHES
// names, the render resolution for PS_RENDER_RES, and two fixes on by default. The game reads all
// of them at start; a change applies after a restart.
#include "launch.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>
#include "../../../src/platform/ps_host.h"

extern "C" {
// runtime_config.c
const char* runtime_config_get(const char* sz_key);
const char* runtime_env(const char* sz_name, const char* sz_default);
}

namespace PsLaunch {
namespace {
constexpr const char* AppVersion = "01.09";
constexpr std::uint64_t EbootBase = 0x400000;

/// Above 60 FPS the sprint fix always goes with the frame rate patch: without it sprinting drops
/// to half speed (the game's wall detector measured distance per frame, a 30 FPS rule).
struct FpsPreset {
    const char* sz_name;
    std::vector<const char*> patches;
    const char* sz_vblank_hz;
    /// The most frames a second the profile's hooks let the game's pacer run (frame_rate.cpp).
    int rate;
};
const FpsPreset FpsPresets[] = {
    {"30", {}, "60", 30},
    {"60", {"60 FPS++"}, "60", 60},
    {"90", {"90 FPS++", "Sprint Fix (High FPS)"}, "90", 90},
    // Delta-time patch: vblank at 480 Hz, frames shown as soon as they are done.
    {"uncap", {"Uncap FPS++", "Sprint Fix (High FPS)"}, "480", 480},
};
int s_frame_hooks_rate = 0;

/// psport.ini key -> patch when it is 0, patch when it is 1.
struct Effect {
    const char* sz_key;
    const char* sz_off;
    const char* sz_on;
};
const Effect Effects[] = {
    {"effect_chromatic_aberration", "Disable Chromatic Aberration", nullptr},
    {"effect_dof", "Disable DoF", nullptr},
    {"effect_motion_blur", "Disable Motion Blur (perf increase)", nullptr},
    {"effect_ssao", "Disable SSAO", nullptr},
    {"effect_game_aa", "Disable AA", nullptr},
    {"effect_dynamic_shadows", "Disable Dynamic Light Shadows (perf increase)", nullptr},
    {"effect_ssr", nullptr, "Enable Screen Space Reflections (READ NOTE)"},
    {"skip_intro", nullptr, "Skip Intro"},
    {"debug_camera", nullptr, "Restore Debug Camera"},
    {"debug_menu", nullptr, "Restore Debug Menu (READ NOTES)"},
};
/// model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
const std::map<std::string, const char*> ModelLod = {
    {"-2", "Model LOD -2 (Highest)"}, {"1", "Model LOD 1 (Lower)"}, {"2", "Model LOD 2 (Lowest)"}};

/// Intel CPUs: the game's tone mapping turns black (DLC areas most; reported as darker than on the
/// PS4): the game code runs natively, and Intel's approximate float instructions differ from the
/// PS4's AMD CPU, so a block the game gates on a computed flag never runs. The community fix runs
/// it always; on by default on Intel. PS_INTEL_TONEMAP_FIX=0/1 forces it off or on.
constexpr const char* IntelTonemap = "Intel Black Tonemap Fix";
/// The title's PLAY ONLINE / PLAY OFFLINE dialog: the port has no PSN, the game goes straight to
/// the main menu offline. On by default (the launcher's switch); PS_SKIP_NETWORK_CHOICE=0 shows it.
constexpr const char* SkipNetworkChoice = "Skip Online/Offline Choice";

constexpr const char* DebugMenu = "Restore Debug Menu (READ NOTES)";
/// What the patched game reads for the debug menu (PS_FILE_TRACE=1), under dvdroot_ps4. The game
/// asks for lower-case names; the PS4's file system and the port's runtime ignore case.
constexpr const char* DebugMenuFonts[] = {"adhoc/font/DbgFont14h.ccm", "adhoc/font/DbgFont14h.tpf"};
constexpr const char* DebugMenuShaders[] = {"adhoc/FontShader/debugFont_vs.vpo", "adhoc/FontShader/debugFont_ps.ppo"};

/// The community patch changes two independent consumers: the game's render/window setup and the
/// UI movie viewport. The UI keeps the game's 1920x1080 coordinates even for a larger output (the
/// port draws it into the output-size image with a viewport scaled by output / 1920,
/// UiComposition::NativeViewport), so glyphs and vector shapes are rasterized at the output size
/// rather than the scene's.
constexpr const char* ResolutionTemplate = "Resolution Patch 1280x720 (16:9)";
constexpr const char* GraphicsHeap = "Increased Graphics Heap Sizes";
constexpr int UiWidth = 1920, UiHeight = 1080;
/// The template's mov instructions (opcode, its 1280x720 operand) and the size each one takes.
struct Operand {
    std::uint64_t off_image;
    unsigned char opcode;
    std::uint32_t template_value;
    bool is_width, is_scene;
};
const Operand ResolutionOperands[] = {
    {0x02196A6B - EbootBase, 0xB8, 0x500, true, true},
    {0x02196A7A - EbootBase, 0xB8, 0x2D0, false, true},
    {0x02358554 - EbootBase, 0xB8, 0x500, true, false},
    {0x0235855D - EbootBase, 0xB9, 0x2D0, false, false},
};

/// Why the frame rate stays with the preset's patches instead of the profile's hooks, or null for
/// the hooks: PS_FRAME_HOOKS=0, the profile or the guest hooks off, or another executable than the
/// one the hooks are written for (they check their bytes too, but by then the patches can no
/// longer be chosen).
const char* FrameHooksRefusal(const PsGameFiles& game) {
    if (std::strcmp(runtime_env("PS_FRAME_HOOKS", "1"), "0") == 0) {
        return "PS_FRAME_HOOKS=0";
    }
    if (std::strcmp(runtime_env("PS_GAME_PROFILE", ""), "none") == 0) {
        return "PS_GAME_PROFILE=none";
    }
    if (std::strcmp(runtime_env("PS_GUEST_HOOKS", "1"), "0") == 0) {
        return "PS_GUEST_HOOKS=0";
    }
    return IsSupportedImage(game) ? nullptr : "not the 1.09 executable";
}

/// PS_FPS, uncap when unset or empty.
std::string FpsPresetName() {
    const std::string name = runtime_env("PS_FPS", "");
    return name.empty() ? "uncap" : name;
}

const FpsPreset* FindPreset(const std::string& name) {
    for (const FpsPreset& preset : FpsPresets) {
        if (name == preset.sz_name) {
            return &preset;
        }
    }
    return nullptr;
}

std::string Strip(const std::string& text) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto it_begin = std::find_if_not(text.begin(), text.end(), is_space);
    const auto it_end = std::find_if_not(text.rbegin(), text.rend(), is_space).base();
    return it_begin < it_end ? std::string(it_begin, it_end) : std::string();
}

bool IsIntelCpu() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.starts_with("vendor_id") && line.find("GenuineIntel") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void AddOnce(std::vector<std::string>& names, const std::string& name) {
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
    }
}

/// root/relative, each component matched ignoring case; empty when absent.
std::string FindIgnoringCase(const std::string& root, const std::string& relative) {
    std::string path = root;
    for (std::size_t off_part = 0; off_part <= relative.size();) {
        std::size_t off_end = relative.find('/', off_part);
        if (off_end == std::string::npos) {
            off_end = relative.size();
        }
        const std::string part = relative.substr(off_part, off_end - off_part);
        off_part = off_end + 1;
        struct stat st;
        if (::stat((path + "/" + part).c_str(), &st) == 0) {
            path += "/" + part;
            continue;
        }
        DIR* p_dir = opendir(path.c_str());
        if (!p_dir) {
            return {};
        }
        std::string match;
        while (const dirent* p_entry = readdir(p_dir)) {
            if (std::strcmp(p_entry->d_name, ".") && std::strcmp(p_entry->d_name, "..") &&
                !strcasecmp(p_entry->d_name, part.c_str())) {
                match = p_entry->d_name;
                break;
            }
        }
        closedir(p_dir);
        if (match.empty()) {
            return {};
        }
        path += "/" + match;
    }
    return path;
}

/// Without its font files the game crashes opening the debug menu: the patch is left out, and
/// said.
void CheckDebugMenu(std::vector<std::string>& names, const std::string& game_dir) {
    if (std::find(names.begin(), names.end(), DebugMenu) == names.end()) {
        return;
    }
    const std::string dvdroot = game_dir + "/dvdroot_ps4";
    std::vector<std::string> missing;
    for (const char* sz_font : DebugMenuFonts) {
        const std::string found = FindIgnoringCase(dvdroot, sz_font);
        struct stat st;
        if (found.empty() || ::stat(found.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
            missing.push_back(sz_font);
        }
    }
    if (missing.empty()) {
        std::string absent;
        for (const char* sz_shader : DebugMenuShaders) {
            if (FindIgnoringCase(dvdroot, sz_shader).empty()) {
                absent += (absent.empty() ? "" : ", ") + std::string(sz_shader);
            }
        }
        if (!absent.empty()) {
            std::fprintf(stderr, "Patches: debug menu: %s not found (the mod's font shaders); the menu may not draw\n",
                         absent.c_str());
        }
        return;
    }
    bool is_in_font = true;
    std::string list;
    for (const std::string& font : missing) {
        list += (list.empty() ? "" : ", ") + font;
        is_in_font = is_in_font && !FindIgnoringCase(dvdroot, "font/" + font.substr(font.rfind('/') + 1)).empty();
    }
    std::fprintf(stderr,
                 "Patches: debug menu off: the debug menu needs the font files from "
                 "https://www.nexusmods.com/bloodborne/mods/253 in dvdroot_ps4 of the game folder or of a mod: "
                 "missing %s%s\n",
                 list.c_str(),
                 is_in_font ? " (they are in dvdroot_ps4/font: the game reads them from dvdroot_ps4/adhoc/font)" : "");
    names.erase(std::remove(names.begin(), names.end(), DebugMenu), names.end());
}

bool Add(const PsPatchServices& services, const std::string& file, const std::vector<std::string>& names) {
    std::vector<const char*> pointers;
    pointers.reserve(names.size());
    for (const std::string& name : names) {
        pointers.push_back(name.c_str());
    }
    char sz_error[1024];
    if (!services.pfn_add_named(services.p_list, file.c_str(), pointers.data(), pointers.size(), AppVersion, sz_error,
                                sizeof(sz_error))) {
        std::fprintf(stderr, "ERROR: Patches: %s\n", sz_error);
        return false;
    }
    return true;
}

/// The template's writes, its four size operands set to the scene's and the UI's sizes.
bool AddResolution(const PsPatchServices& services, const std::string& file, int width, int height) {
    const std::size_t i_first = services.pfn_count(services.p_list);
    if (!Add(services, file, {ResolutionTemplate})) {
        return false;
    }
    bool seen[std::size(ResolutionOperands)] = {};
    for (std::size_t i = i_first; i < services.pfn_count(services.p_list); ++i) {
        std::uint64_t off_image, cb_data;
        unsigned char* p_data = services.pfn_write(services.p_list, i, &off_image, &cb_data);
        for (std::size_t o = 0; o < std::size(ResolutionOperands); ++o) {
            const Operand& operand = ResolutionOperands[o];
            if (operand.off_image != off_image) {
                continue;
            }
            const unsigned char expected[4] = {operand.opcode, std::uint8_t(operand.template_value),
                                               std::uint8_t(operand.template_value >> 8),
                                               std::uint8_t(operand.template_value >> 16)};
            if (cb_data != 4 || std::memcmp(p_data, expected, 4) || seen[o]) {
                std::fprintf(stderr, "ERROR: Patches: unexpected resolution patch at %#llx\n",
                             static_cast<unsigned long long>(off_image + EbootBase));
                return false;
            }
            const std::uint32_t value = operand.is_scene ? (operand.is_width ? width : height)
                                                         : (operand.is_width ? UiWidth : UiHeight);
            p_data[1] = std::uint8_t(value);
            p_data[2] = std::uint8_t(value >> 8);
            p_data[3] = std::uint8_t(value >> 16);
            seen[o] = true;
        }
    }
    if (!std::all_of(std::begin(seen), std::end(seen), [](bool is_seen) { return is_seen; })) {
        std::fprintf(stderr, "ERROR: Patches: resolution patch is missing scene/UI viewport instructions\n");
        return false;
    }
    return true;
}

bool ParseSize(const std::string& text, int& out_width, int& out_height) {
    const std::size_t off_x = text.find_first_of("xX");
    if (off_x == std::string::npos || text.find_first_of("xX", off_x + 1) != std::string::npos) {
        return false;
    }
    char* p_end;
    const long width = std::strtol(text.c_str(), &p_end, 10);
    if (p_end != text.c_str() + off_x) {
        return false;
    }
    const long height = std::strtol(text.c_str() + off_x + 1, &p_end, 10);
    if (*p_end || off_x == 0 || off_x + 1 == text.size() || width <= 0 || height <= 0 || width > 0xffffff ||
        height > 0xffffff) {
        return false;
    }
    out_width = int(width);
    out_height = int(height);
    return true;
}
} // namespace

void PrepareEnvironment() {
    if (*runtime_env("PS_VBLANK_HZ", "")) {
        return;
    }
    const FpsPreset* p_preset = FindPreset(FpsPresetName());
    setenv("PS_VBLANK_HZ", p_preset ? p_preset->sz_vblank_hz : "60", 1);
}

void ProbeHostFeatures() {
    if (IsIntelCpu()) {
        ps_host_feature_probed(PS_HOST_FEATURE_intel_tonemap_fix, 1,
                               "GenuineIntel CPU: the fix is on unless PS_INTEL_TONEMAP_FIX=0");
    } else {
        ps_host_feature_probed(PS_HOST_FEATURE_intel_tonemap_fix, 0,
                               "not needed: this CPU's float instructions answer as the PS4's");
    }
}

bool AddPatches(const PsGameFiles& game, const PsPatchServices& services) {
    const std::string fps = FpsPresetName();
    const FpsPreset* p_preset = FindPreset(fps);
    if (!p_preset) {
        std::fprintf(stderr, "ERROR: Patches: PS_FPS=%s: expected 30, 60, 90 or uncap\n", fps.c_str());
        return false;
    }
    const char* sz_refusal = p_preset->rate > 30 ? FrameHooksRefusal(game) : "";
    s_frame_hooks_rate = sz_refusal ? 0 : p_preset->rate;
    std::vector<std::string> names;
    if (sz_refusal) {
        names.assign(p_preset->patches.begin(), p_preset->patches.end());
    }
    const std::string extra = runtime_env("PS_PATCHES", "");
    for (std::size_t off_name = 0; off_name <= extra.size();) {
        std::size_t off_end = extra.find(';', off_name);
        if (off_end == std::string::npos) {
            off_end = extra.size();
        }
        const std::string name = Strip(extra.substr(off_name, off_end - off_name));
        if (!name.empty()) {
            names.push_back(name);
        }
        off_name = off_end + 1;
    }
    for (const Effect& effect : Effects) {
        const char* sz_value = runtime_config_get(effect.sz_key);
        if (!sz_value) {
            continue;
        }
        if (const char* sz_name = std::strcmp(sz_value, "1") == 0 ? effect.sz_on : effect.sz_off) {
            AddOnce(names, sz_name);
        }
    }
    const char* sz_lod = runtime_config_get("model_lod");
    if (const auto it = ModelLod.find(sz_lod ? sz_lod : "0"); it != ModelLod.end()) {
        AddOnce(names, it->second);
    }
    const std::string intel = runtime_env("PS_INTEL_TONEMAP_FIX", "");
    if (intel == "1" || (intel != "0" && IsIntelCpu())) {
        AddOnce(names, IntelTonemap);
    }
    if (std::strcmp(runtime_env("PS_SKIP_NETWORK_CHOICE", "1"), "0") != 0) {
        AddOnce(names, SkipNetworkChoice);
    }
    if (std::find(names.begin(), names.end(), "Restore Debug Camera") != names.end() &&
        std::find(names.begin(), names.end(), "Enemy Control") != names.end()) {
        std::fprintf(stderr, "ERROR: Patches: Restore Debug Camera conflicts with Enemy Control; enable only one\n");
        return false;
    }
    CheckDebugMenu(names, game.sz_game_dir);
    const std::string file = game.sz_patch_file ? game.sz_patch_file : PatchFile;
    if (!Add(services, file, names)) {
        return false;
    }
    if (const std::string size = runtime_env("PS_RENDER_RES", ""); !size.empty()) {
        int width, height;
        if (!ParseSize(size, width, height)) {
            std::fprintf(stderr, "ERROR: Patches: PS_RENDER_RES=%s: expected WIDTHxHEIGHT\n", size.c_str());
            return false;
        }
        if (!AddResolution(services, file, width, height)) {
            return false;
        }
        if (static_cast<long long>(width) * height > UiWidth * UiHeight) {
            if (!Add(services, file, {GraphicsHeap})) {
                return false;
            }
            names.push_back(GraphicsHeap);
        }
        std::printf("Patches: scene %dx%d; UI %dx%d\n", width, height, UiWidth, UiHeight);
    }
    std::string list;
    for (const std::string& name : names) {
        list += (list.empty() ? "'" : ", '") + name + "'";
    }
    std::printf("Patches: FPS preset %s%s%s%s; %zu writes from %s\n", fps.c_str(),
                !sz_refusal ? " by hooks" : *sz_refusal ? " by patches (" : "", sz_refusal ? sz_refusal : "",
                sz_refusal && *sz_refusal ? ")" : "", services.pfn_count(services.p_list),
                names.empty() ? "none" : ("[" + list + "]").c_str());
    return true;
}

int FrameHooksChosen() {
    return s_frame_hooks_rate;
}

const char* ExternalPatchRefusal(const char* sz_name) {
    if (!s_frame_hooks_rate) {
        return nullptr;
    }
    for (const FpsPreset& preset : FpsPresets) {
        for (const char* sz_patch : preset.patches) {
            if (std::strcmp(sz_patch, sz_name) == 0) {
                return "the profile's hooks drive the frame rate (PS_FRAME_HOOKS=0: the patches)";
            }
        }
    }
    return nullptr;
}
} // namespace PsLaunch
