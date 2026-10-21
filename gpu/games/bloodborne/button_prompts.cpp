// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: button prompts for the device in use (button_prompts.h).
#include "button_prompts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <miniz.h>
#include <xxhash.h>

#include "../common/button_prompt_art.h"

extern "C" {
// src/kernel/runtime_file.c, src/input/runtime_pad.c
int runtime_file_translate(const char* sz_guest, char* sz_out, std::size_t cb_out);
unsigned runtime_pad_prompt_generation(void);
int runtime_pad_prompt_device(void);
}

namespace PsButtonPrompts {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using PsButtonPromptArt::Layout;

/// Close to the game's own menus: dark plates, parchment frames and labels, amber for what is lit.
constexpr PsButtonPromptArt::Palette Palette{.fill = {0.075f, 0.07f, 0.065f, 0.93f},
                                             .frame = {0.78f, 0.74f, 0.64f},
                                             .ink = {0.95f, 0.92f, 0.85f},
                                             .accent = {0.82f, 0.64f, 0.36f}};

struct IconDef {
    const char* sz_name;
    Layout layout;
    std::array<const char*, 4> inputs; ///< runtime_pad.c input names
};
// The icons are known by what they show, not by their names: KG_OK is the circle and KG_Cancel
// the cross (the Japanese layout; a release whose confirm button is the cross shows KG_Cancel for
// OK). KG_R_U and KG_R_L are the triangle and the square (the right cluster's up and left).
constexpr IconDef Icons[] = {
    {"KG_OK", Layout::Single, {"circle"}},
    {"KG_Cancel", Layout::Single, {"cross"}},
    {"KG_R_U", Layout::Single, {"triangle"}},
    {"KG_R_L", Layout::Single, {"square"}},
    {"KG_L1", Layout::Single, {"l1"}},
    {"KG_L2", Layout::Single, {"l2"}},
    {"KG_R1", Layout::Single, {"r1"}},
    {"KG_R2", Layout::Single, {"r2"}},
    {"KG_L3", Layout::Single, {"l3"}},
    {"KG_R3", Layout::Single, {"r3"}},
    {"KG_Start", Layout::Single, {"options"}},
    {"KG_TP_L", Layout::Single, {"touchpad"}},
    {"KG_TP_R", Layout::Single, {"touchpad_right"}},
    {"KG_L_U", Layout::Single, {"up"}},
    {"KG_L_D", Layout::Single, {"down"}},
    {"KG_L_L", Layout::Single, {"left"}},
    {"KG_L_R", Layout::Single, {"right"}},
    {"KG_L_UD", Layout::Column, {"up", "down"}},
    {"KG_L_LR", Layout::Row, {"left", "right"}},
    {"KG_L_UDLR", Layout::Cross, {"up", "left", "down", "right"}},
    {"KG_LS", Layout::Stick, {"move_up", "move_left", "move_down", "move_right"}},
    {"KG_RS", Layout::Stick, {"look_up", "look_left", "look_down", "look_right"}},
};
constexpr std::size_t IconCount = std::size(Icons);
constexpr u32 IconTexels = 32;
constexpr std::size_t CbIcon = 1024; // 32x32 BC7, one level

// Atlases of the HUD (ingametop.gfx) with icons in them, drawn as the icons' own textures (3
// texels in from the cell's edges): MENU_Common_00091 has the circle, the cross and L3 in a row,
// for the "Examine" and "Pick up" prompts at lamps, gravestones and items.
struct AtlasDef {
    const char* sz_name;
    u32 width, height;
    std::size_t cb_atlas; ///< BC7, one level
    std::array<std::pair<u32, u32>, 3> cells;
    std::array<const char*, 3> icons;
};
constexpr AtlasDef Atlases[] = {
    {"MENU_Common_00091", 1024, 512, 1024 * 512, {{{658, 120}, {691, 120}, {723, 120}}},
     {"KG_OK", "KG_Cancel", "KG_L3"}},
};
constexpr std::size_t AtlasCount = std::size(Atlases);

constexpr auto MakeIconSubstitutes() {
    std::array<Game::TextureSubstitute, IconCount> substitutes{};
    for (u32 i = 0; i < IconCount; ++i) {
        substitutes[i].i_whole_picture = i;
    }
    return substitutes;
}
constexpr std::array<Game::TextureSubstitute, IconCount> IconSubstitutes = MakeIconSubstitutes();

// Written by the loader only, read after WaitTable.
std::array<u64, IconCount> s_icon_hashes{};   // 0: not in the game's file
std::array<u64, AtlasCount> s_atlas_hashes{}; // 0: not in the game's file
std::array<std::vector<Game::SubstituteCell>, AtlasCount> s_atlas_cells{};
std::array<Game::TextureSubstitute, AtlasCount> s_atlas_substitutes{};
u32 s_c_icons_found = 0;

std::once_flag s_started;
std::future<void> s_loader;
std::once_flag s_joined;

bool IsOn() {
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_BUTTON_PROMPTS");
        return !(sz_env && sz_env[0] == '0');
    }();
    return s_is_on;
}

u32 Be32(const u8* p_bytes) {
    return u32(p_bytes[0]) << 24 | u32(p_bytes[1]) << 16 | u32(p_bytes[2]) << 8 | p_bytes[3];
}
u32 Le32(const u8* p_bytes) {
    return u32(p_bytes[0]) | u32(p_bytes[1]) << 8 | u32(p_bytes[2]) << 16 | u32(p_bytes[3]) << 24;
}

bool ReadFile(const char* sz_path, std::vector<u8>& out) {
    FILE* p_file = std::fopen(sz_path, "rb");
    if (!p_file) {
        return false;
    }
    bool is_read = std::fseek(p_file, 0, SEEK_END) == 0;
    const long cb_file = is_read ? std::ftell(p_file) : -1;
    is_read = cb_file > 0 && std::fseek(p_file, 0, SEEK_SET) == 0;
    if (is_read) {
        out.resize(std::size_t(cb_file));
        is_read = std::fread(out.data(), 1, out.size(), p_file) == out.size();
    }
    std::fclose(p_file);
    return is_read;
}

/// The TPF inside a DCX (zlib "DFLT"): "DCX\0", "DCS\0" at 0x18 with the sizes (big endian),
/// "DCP\0" "DFLT", "DCA\0" and its size at 0x44, then the zlib stream.
bool Unpack(const std::vector<u8>& dcx, std::vector<u8>& out) {
    if (dcx.size() < 0x50 || std::memcmp(dcx.data(), "DCX\0", 4) != 0 ||
        std::memcmp(dcx.data() + 0x18, "DCS\0", 4) != 0 || std::memcmp(dcx.data() + 0x28, "DFLT", 4) != 0 ||
        std::memcmp(dcx.data() + 0x44, "DCA\0", 4) != 0) {
        return false;
    }
    const u32 cb_unpacked = Be32(dcx.data() + 0x1c);
    const std::size_t off_stream = 0x44 + std::size_t(Be32(dcx.data() + 0x48));
    if (cb_unpacked == 0 || cb_unpacked > (256u << 20) || off_stream >= dcx.size()) {
        return false;
    }
    out.resize(cb_unpacked);
    mz_ulong cb_out = cb_unpacked;
    return mz_uncompress(out.data(), &cb_out, dcx.data() + off_stream, mz_ulong(dcx.size() - off_stream)) == MZ_OK &&
           cb_out == cb_unpacked;
}

std::string EntryName(const std::vector<u8>& tpf, std::size_t off_name, bool is_utf16) {
    std::string name;
    const std::size_t cb_char = is_utf16 ? 2 : 1;
    for (std::size_t off = off_name; off + cb_char <= tpf.size() && name.size() <= 64; off += cb_char) {
        const u32 code = is_utf16 ? u32(tpf[off]) | u32(tpf[off + 1]) << 8 : tpf[off];
        if (code == 0) {
            break;
        }
        name.push_back(code < 0x80 ? char(code) : '?');
    }
    return name;
}

/// The icons' and atlases' hashes from the game's menu/common.tpf.dcx.
void LoadTable() {
    constexpr const char* SzGuest = "/app0/dvdroot_ps4/menu/common.tpf.dcx";
    char sz_path[1024];
    std::vector<u8> dcx, tpf;
    if (runtime_file_translate(SzGuest, sz_path, sizeof(sz_path)) != 0 || !ReadFile(sz_path, dcx)) {
        std::printf("Button prompts: %s not read: the game's icons stay\n", SzGuest);
        return;
    }
    if (!Unpack(dcx, tpf)) {
        std::printf("Button prompts: %s is not a DCX (DFLT) file that unpacks: the game's icons stay\n", sz_path);
        return;
    }
    // TPF (PS4): "TPF\0", count at 8, name encoding at 0xe (1 or 2: UTF-16), then 36-byte entries
    // from 0x10: data offset, size, format, type, levels, flags, width, height, two words, the
    // name's offset, a word, the DXGI format.
    if (tpf.size() < 0x10 || std::memcmp(tpf.data(), "TPF\0", 4) != 0) {
        std::printf("Button prompts: %s holds no TPF: the game's icons stay\n", sz_path);
        return;
    }
    const u32 c_entries = Le32(tpf.data() + 8);
    const bool is_utf16 = tpf[0xe] == 1 || tpf[0xe] == 2;
    constexpr std::size_t CbEntry = 36;
    for (u32 i = 0; i < c_entries && 0x10 + (std::size_t(i) + 1) * CbEntry <= tpf.size(); ++i) {
        const u8* p_entry = tpf.data() + 0x10 + std::size_t(i) * CbEntry;
        const u64 off_data = Le32(p_entry), cb_data = Le32(p_entry + 4), off_name = Le32(p_entry + 24);
        if (off_name >= tpf.size() || off_data + cb_data > tpf.size()) {
            continue;
        }
        const std::string name = EntryName(tpf, off_name, is_utf16);
        for (std::size_t k = 0; k < AtlasCount; ++k) {
            if (name == Atlases[k].sz_name && cb_data == Atlases[k].cb_atlas) {
                s_atlas_hashes[k] = XXH3_64bits(tpf.data() + off_data, cb_data);
            }
        }
        for (std::size_t k = 0; k < IconCount; ++k) {
            if (name == Icons[k].sz_name && cb_data == CbIcon) {
                s_icon_hashes[k] = XXH3_64bits(tpf.data() + off_data, cb_data);
                ++s_c_icons_found;
            }
        }
    }
    u32 c_atlases = 0;
    for (std::size_t k = 0; k < AtlasCount; ++k) {
        for (std::size_t c = 0; s_atlas_hashes[k] && c < Atlases[k].cells.size(); ++c) {
            for (u32 i = 0; i < IconCount; ++i) {
                if (s_icon_hashes[i] && std::string_view{Icons[i].sz_name} == Atlases[k].icons[c]) {
                    s_atlas_cells[k].push_back({.x = Atlases[k].cells[c].first,
                                                .y = Atlases[k].cells[c].second,
                                                .side = IconTexels,
                                                .i_picture = i});
                }
            }
        }
        s_atlas_substitutes[k].cells = s_atlas_cells[k];
        c_atlases += !s_atlas_cells[k].empty();
    }
    std::printf("Button prompts: %u of the game's %zu key guide icons and %u of %zu HUD atlases found (%s)\n",
                s_c_icons_found, IconCount, c_atlases, AtlasCount,
                s_c_icons_found != 0 ? "drawn for the keyboard and other gamepads; PS_BUTTON_PROMPTS=0: off" : "off");
}

void WaitTable() {
    std::call_once(s_joined, [] {
        Start();
        if (s_loader.valid()) {
            s_loader.get();
        }
    });
}

} // namespace

void Start() {
    std::call_once(s_started, [] {
        if (!IsOn()) {
            std::printf("Button prompts: the game's icons always (PS_BUTTON_PROMPTS=0)\n");
            return;
        }
        s_loader = std::async(std::launch::async, LoadTable);
    });
}

const Game::TextureSubstitute* Identify(const void* p_texels, std::size_t cb_texels, std::uint32_t width,
                                        std::uint32_t height) {
    if (!IsOn()) {
        return nullptr;
    }
    if (width == IconTexels && height == IconTexels && cb_texels >= CbIcon) {
        WaitTable();
        const u64 hash = XXH3_64bits(p_texels, CbIcon);
        for (std::size_t i = 0; i < IconCount; ++i) {
            if (s_icon_hashes[i] != 0 && s_icon_hashes[i] == hash) {
                return &IconSubstitutes[i];
            }
        }
        return nullptr;
    }
    for (std::size_t k = 0; k < AtlasCount; ++k) {
        if (Atlases[k].width != width || Atlases[k].height != height || cb_texels < Atlases[k].cb_atlas) {
            continue;
        }
        WaitTable();
        if (!s_atlas_cells[k].empty() && XXH3_64bits(p_texels, Atlases[k].cb_atlas) == s_atlas_hashes[k]) {
            return &s_atlas_substitutes[k];
        }
    }
    return nullptr;
}

bool IsRescanned(std::size_t cb_texels, std::uint32_t width, std::uint32_t height) {
    return IsOn() && width == IconTexels && height == IconTexels && cb_texels >= CbIcon;
}

std::uint32_t Generation() {
    return IsOn() ? runtime_pad_prompt_generation() : 0;
}

bool Draw(Game::SubstitutePictures& out) {
    if (!IsOn()) {
        return false;
    }
    WaitTable();
    if (s_c_icons_found == 0) {
        return false;
    }
    constexpr u32 Side = PsButtonPromptArt::Side;
    out.side = Side;
    out.is_drawn.assign(IconCount, 0);
    out.pixels.assign(IconCount * Side * Side, 0);
    u32 c_drawn = 0;
    for (std::size_t i = 0; i < IconCount; ++i) {
        if (s_icon_hashes[i] != 0 && PsButtonPromptArt::Draw(Icons[i].layout, Icons[i].inputs, Palette,
                                                            out.pixels.data() + i * Side * Side)) {
            out.is_drawn[i] = 1;
            ++c_drawn;
        }
    }
    constexpr const char* Devices[] = {"the game's own (PlayStation pad)", "the gamepad", "the keyboard and mouse"};
    const int device = runtime_pad_prompt_device();
    std::printf("Button prompts: icons from %s, %u of %u drawn by the port\n",
                device >= 0 && device < int(std::size(Devices)) ? Devices[device] : "no device yet", c_drawn,
                s_c_icons_found);
    return c_drawn != 0;
}

} // namespace PsButtonPrompts
