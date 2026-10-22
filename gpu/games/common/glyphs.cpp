// SPDX-License-Identifier: GPL-2.0-or-later
// The active profile's glyph table (glyphs.h) behind Game::TextureSubstitution. From bbport's
// bbport_button_prompts (72e5c8c), shared by every title's profile.
#include "glyphs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <vector>

#include <xxhash.h>

#include "game_profile.h"

extern "C" {
// src/kernel/runtime_file.c, src/input/runtime_pad.c
int runtime_file_translate(const char* sz_guest, char* sz_out, std::size_t cb_out);
unsigned runtime_pad_prompt_generation(void);
int runtime_pad_prompt_device(void);
}

namespace PsGlyphs {

// One per file format, in the folder of the game it came with, as game_profile.cpp lists the
// titles.
extern const Extractor FromSoftwareDcxTpf;

namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::array<const Extractor*, 1> Extractors = {&FromSoftwareDcxTpf};

// Sized at Start, written by the loader only, read after WaitTable.
const Table* s_p_table = nullptr;
std::vector<u64> s_glyph_hashes; // 0: not known
std::vector<u64> s_atlas_hashes; // 0: not known
std::vector<std::vector<Game::SubstituteCell>> s_atlas_cells;
std::vector<Game::TextureSubstitute> s_glyph_substitutes;
std::vector<Game::TextureSubstitute> s_atlas_substitutes;
std::vector<u8> s_is_glyph_available;
u32 s_c_glyphs_available = 0;
u32 s_c_found = 0;

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

const Table* ActiveTable() {
    const Game::Profile* p_profile = Game::Active();
    return p_profile ? p_profile->p_glyphs : nullptr;
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

bool IsSameSource(const Source* p_a, const Source* p_b) {
    return p_a == p_b || (p_a && p_b && std::strcmp(p_a->sz_asset, p_b->sz_asset) == 0 &&
                          std::strcmp(p_a->sz_extractor, p_b->sz_extractor) == 0);
}

/// Why a texture of the table can never be matched, or nullptr.
const char* Unusable(const Texture& texture) {
    if (texture.height == 0 || texture.cb_texels == 0) {
        return "no height or no bytes to hash";
    }
    if (texture.p_source) {
        return !texture.p_source->sz_asset || !texture.p_source->sz_extractor || !texture.sz_entry
                   ? "a source without its file, extractor or entry"
                   : nullptr;
    }
    return texture.hash == 0 ? "neither a hash nor a source" : nullptr;
}

/// Checks the table's entries once: what cannot be matched keeps no hash and is reported.
std::vector<u8> CheckTable(const Table& table) {
    std::vector<u8> is_usable(table.glyphs.size() + table.atlases.size(), 0);
    auto check = [&](std::size_t i, const char* sz_name, const Texture& texture) {
        if (texture.width == 0) {
            return;
        }
        if (const char* sz_why = Unusable(texture)) {
            std::printf("Button prompts: %s is never matched: %s\n", sz_name, sz_why);
            return;
        }
        is_usable[i] = 1;
    };
    for (std::size_t i = 0; i < table.glyphs.size(); ++i) {
        check(i, table.glyphs[i].sz_name, table.glyphs[i].texture);
    }
    for (std::size_t k = 0; k < table.atlases.size(); ++k) {
        check(table.glyphs.size() + k, table.atlases[k].sz_name, table.atlases[k].texture);
    }
    return is_usable;
}

/// Hashes the entries of one source file that textures of the table name.
void LoadSource(const Table& table, const Source& source, const std::vector<u8>& is_usable) {
    const auto p_extractor = std::ranges::find_if(
        Extractors, [&](const Extractor* p) { return std::strcmp(p->sz_id, source.sz_extractor) == 0; });
    if (p_extractor == Extractors.end()) {
        std::printf("Button prompts: no extractor %s for %s: the glyphs from it stay the game's\n",
                    source.sz_extractor, source.sz_asset);
        return;
    }
    char sz_path[1024];
    std::vector<u8> file;
    if (runtime_file_translate(source.sz_asset, sz_path, sizeof(sz_path)) != 0 || !ReadFile(sz_path, file)) {
        std::printf("Button prompts: %s not read: the glyphs from it stay the game's\n", source.sz_asset);
        return;
    }
    auto hash_entry = [&](const Texture& texture, bool is_entry_usable, std::string_view name,
                          std::span<const u8> texels, u64& out_hash) {
        if (is_entry_usable && IsSameSource(texture.p_source, &source) && name == texture.sz_entry &&
            texels.size() == texture.cb_texels) {
            out_hash = XXH3_64bits(texels.data(), texels.size());
        }
    };
    const std::size_t c_glyphs = table.glyphs.size();
    std::string error;
    const bool is_extracted = (*p_extractor)->pfn_extract(
        file,
        [&](std::string_view name, std::span<const u8> texels) {
            for (std::size_t i = 0; i < c_glyphs; ++i) {
                hash_entry(table.glyphs[i].texture, is_usable[i], name, texels, s_glyph_hashes[i]);
            }
            for (std::size_t k = 0; k < table.atlases.size(); ++k) {
                hash_entry(table.atlases[k].texture, is_usable[c_glyphs + k], name, texels, s_atlas_hashes[k]);
            }
        },
        error);
    if (!is_extracted) {
        std::printf("Button prompts: %s %s: the glyphs from it stay the game's\n", sz_path, error.c_str());
    }
}

void LoadTable(const Table& table) {
    const std::vector<u8> is_usable = CheckTable(table);
    const std::size_t c_glyphs = table.glyphs.size();
    std::vector<const Source*> sources;
    auto take = [&](const Texture& texture, bool is_entry_usable, u64& out_hash) {
        if (!is_entry_usable) {
            return;
        }
        if (!texture.p_source) {
            out_hash = texture.hash;
        } else if (std::ranges::none_of(sources, [&](const Source* p) { return IsSameSource(p, texture.p_source); })) {
            sources.push_back(texture.p_source);
        }
    };
    for (std::size_t i = 0; i < c_glyphs; ++i) {
        take(table.glyphs[i].texture, is_usable[i], s_glyph_hashes[i]);
    }
    for (std::size_t k = 0; k < table.atlases.size(); ++k) {
        take(table.atlases[k].texture, is_usable[c_glyphs + k], s_atlas_hashes[k]);
    }
    for (const Source* p_source : sources) {
        LoadSource(table, *p_source, is_usable);
    }

    std::string missing;
    u32 c_glyphs_found = 0;
    for (std::size_t i = 0; i < c_glyphs; ++i) {
        const bool has_texture = table.glyphs[i].texture.width != 0;
        const bool is_found = has_texture && s_glyph_hashes[i] != 0;
        s_is_glyph_available[i] = !has_texture || is_found;
        s_c_glyphs_available += s_is_glyph_available[i];
        c_glyphs_found += is_found;
        if (has_texture && !is_found) {
            missing.append(missing.empty() ? "" : ", ").append(table.glyphs[i].sz_name);
        }
    }
    u32 c_atlases = 0;
    for (std::size_t k = 0; k < table.atlases.size(); ++k) {
        const Atlas& atlas = table.atlases[k];
        for (const AtlasCell& cell : atlas.cells) {
            const auto p_glyph = std::ranges::find_if(table.glyphs, [&](const Glyph& glyph) {
                return cell.sz_glyph && std::strcmp(glyph.sz_name, cell.sz_glyph) == 0;
            });
            if (p_glyph == table.glyphs.end() || cell.side == 0 || cell.x + cell.side > atlas.texture.width ||
                cell.y + cell.side > atlas.texture.height) {
                std::printf("Button prompts: %s's cell at %u,%u (%s) is no glyph of the table inside it: left out\n",
                            atlas.sz_name, cell.x, cell.y, cell.sz_glyph ? cell.sz_glyph : "none");
                continue;
            }
            const u32 i_glyph = u32(p_glyph - table.glyphs.begin());
            if (s_atlas_hashes[k] != 0 && s_is_glyph_available[i_glyph]) {
                s_atlas_cells[k].push_back({.x = cell.x,
                                             .y = cell.y,
                                             .side = cell.side,
                                             .i_picture = i_glyph,
                                             .is_upside_down = atlas.is_upside_down});
            }
        }
        s_atlas_substitutes[k].cells = s_atlas_cells[k];
        c_atlases += !s_atlas_cells[k].empty();
        if (atlas.texture.width != 0 && s_atlas_hashes[k] == 0) {
            missing.append(missing.empty() ? "" : ", ").append(atlas.sz_name);
        }
    }
    s_c_found = c_glyphs_found + c_atlases;
    std::printf("Button prompts: %u of the game's %zu glyphs and %u of %zu atlases found (%s)\n", c_glyphs_found,
                c_glyphs, c_atlases, table.atlases.size(),
                s_c_found != 0 ? "drawn for the keyboard and other gamepads; PS_BUTTON_PROMPTS=0: off" : "off");
    if (!missing.empty()) {
        std::printf("Button prompts: not in the game's files, the game's own stay: %s\n", missing.c_str());
    }
}

void Start() {
    std::call_once(s_started, [] {
        s_p_table = ActiveTable();
        if (!s_p_table) {
            return;
        }
        if (!IsOn()) {
            std::printf("Button prompts: the game's glyphs always (PS_BUTTON_PROMPTS=0)\n");
            return;
        }
        const std::size_t c_glyphs = s_p_table->glyphs.size(), c_atlases = s_p_table->atlases.size();
        s_glyph_hashes.assign(c_glyphs, 0);
        s_atlas_hashes.assign(c_atlases, 0);
        s_atlas_cells.assign(c_atlases, {});
        s_atlas_substitutes.assign(c_atlases, {});
        s_is_glyph_available.assign(c_glyphs, 0);
        s_glyph_substitutes.assign(c_glyphs, {});
        for (u32 i = 0; i < c_glyphs; ++i) {
            s_glyph_substitutes[i].i_whole_picture = i;
        }
        s_loader = std::async(std::launch::async, LoadTable, std::cref(*s_p_table));
    });
}

void WaitTable() {
    std::call_once(s_joined, [] {
        Start();
        if (s_loader.valid()) {
            s_loader.get();
        }
    });
}

bool IsShape(const Texture& texture, std::size_t cb_texels, u32 width, u32 height) {
    return texture.width != 0 && texture.width == width && texture.height == height && cb_texels >= texture.cb_texels;
}

const Game::TextureSubstitute* Identify(const void* p_texels, std::size_t cb_texels, std::uint32_t width,
                                        std::uint32_t height, std::uint32_t format) {
    if (!IsOn()) {
        return nullptr;
    }
    // The table Start sized the hashes for, so a later change of the active profile cannot index
    // past them.
    Start();
    const Table* p_table = s_p_table;
    if (!p_table) {
        return nullptr;
    }
    std::size_t cb_hashed = 0;
    u64 hash = 0;
    auto is_match = [&](const Texture& texture, u64 reference) {
        if (reference == 0) {
            return false;
        }
        if (texture.cb_texels != cb_hashed) {
            hash = XXH3_64bits(p_texels, texture.cb_texels);
            cb_hashed = texture.cb_texels;
        }
        return hash == reference;
    };
    auto is_candidate = [&](const Texture& texture) {
        return IsShape(texture, cb_texels, width, height) && (texture.format == 0 || texture.format == format);
    };
    for (std::size_t i = 0; i < p_table->glyphs.size(); ++i) {
        const Texture& texture = p_table->glyphs[i].texture;
        if (!is_candidate(texture)) {
            continue;
        }
        WaitTable();
        if (is_match(texture, s_glyph_hashes[i])) {
            return &s_glyph_substitutes[i];
        }
    }
    for (std::size_t k = 0; k < p_table->atlases.size(); ++k) {
        const Texture& texture = p_table->atlases[k].texture;
        if (!is_candidate(texture)) {
            continue;
        }
        WaitTable();
        if (!s_atlas_cells[k].empty() && is_match(texture, s_atlas_hashes[k])) {
            return &s_atlas_substitutes[k];
        }
    }
    return nullptr;
}

bool IsRescanned(std::size_t cb_texels, std::uint32_t width, std::uint32_t height) {
    if (!IsOn()) {
        return false;
    }
    Start();
    const Table* p_table = s_p_table;
    return p_table && std::ranges::any_of(p_table->glyphs, [&](const Glyph& glyph) {
               return IsShape(glyph.texture, cb_texels, width, height);
           });
}

std::uint32_t Generation() {
    return IsOn() ? runtime_pad_prompt_generation() : 0;
}

bool Draw(Game::SubstitutePictures& out) {
    if (!IsOn()) {
        return false;
    }
    WaitTable();
    if (!s_p_table || s_c_found == 0) {
        return false;
    }
    constexpr u32 Side = PsButtonPromptArt::Side;
    const std::size_t c_glyphs = s_p_table->glyphs.size();
    out.side = Side;
    out.is_drawn.assign(c_glyphs, 0);
    out.pixels.assign(c_glyphs * Side * Side, 0);
    u32 c_drawn = 0;
    for (std::size_t i = 0; i < c_glyphs; ++i) {
        const Glyph& glyph = s_p_table->glyphs[i];
        if (s_is_glyph_available[i] &&
            PsButtonPromptArt::Draw(glyph.layout, glyph.inputs, s_p_table->palette,
                                    out.pixels.data() + i * Side * Side)) {
            out.is_drawn[i] = 1;
            ++c_drawn;
        }
    }
    constexpr const char* Devices[] = {"the game's own (PlayStation pad)", "the gamepad", "the keyboard and mouse"};
    const int device = runtime_pad_prompt_device();
    std::printf("Button prompts: glyphs from %s, %u of %u drawn by the port\n",
                device >= 0 && device < int(std::size(Devices)) ? Devices[device] : "no device yet", c_drawn,
                s_c_glyphs_available);
    return c_drawn != 0;
}

} // namespace
} // namespace PsGlyphs

namespace Game {

TextureSubstitution GlyphSubstitution() {
    return {.pfn_start = PsGlyphs::Start,
            .pfn_identify = PsGlyphs::Identify,
            .pfn_is_rescanned = PsGlyphs::IsRescanned,
            .pfn_generation = PsGlyphs::Generation,
            .pfn_draw = PsGlyphs::Draw};
}

} // namespace Game
