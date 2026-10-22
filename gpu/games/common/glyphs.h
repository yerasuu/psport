// SPDX-License-Identifier: GPL-2.0-or-later
// A title's button glyphs as data (Profile::p_glyphs): which of the game's textures show a
// PlayStation button and what the port draws there for the device in use, through texture
// substitution (Game::TextureSubstitution). A texture is known by its first level's shape and the
// XXH3-64 of its bytes as the game uploads them, the hash given or computed at start from the
// user's own copy of the game (an entry of one of its files, read by an extractor of that file's
// format), so no art of the game ships with the port. Either the whole texture is a glyph, or an
// atlas has glyphs in some of its cells. PS_BUTTON_PROMPTS=0: the game's glyphs always.
// docs/en/ADDING_A_TITLE.md has the steps for a new title.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "button_prompt_art.h"

namespace PsGlyphs {

/// Lists the textures a file of the game holds: visit for each with its name and its first
/// level's bytes. False, with why in error, for a file not in its format.
using EntryVisitor = std::function<void(std::string_view name, std::span<const std::uint8_t> texels)>;
struct Extractor {
    /// What a Source names it by; glyphs.cpp lists every extractor of the tree.
    const char* sz_id = nullptr;
    bool (*pfn_extract)(std::span<const std::uint8_t> file, const EntryVisitor& visit, std::string& error) = nullptr;
};

/// A file of the game the reference hashes come from: its guest path (/app0/...), so a mod that
/// replaces it is followed, and the id of the extractor that reads it.
struct Source {
    const char* sz_asset = nullptr;
    const char* sz_extractor = nullptr;
};

/// How one texture of the game is known: its first level's width and height, the first cb_texels
/// bytes of it hashed, and its VkFormat as a number (0: any). The hash is either given (an
/// identifier only: level0_hash of PS_DUMP_SMALL_TEXTURES, the diagnostic that comes with
/// feat/706) or, with p_source, that of entry
/// sz_entry of the source, which must be cb_texels long. A Texture without width is none.
struct Texture {
    std::uint32_t width = 0, height = 0;
    std::size_t cb_texels = 0;
    std::uint32_t format = 0;
    std::uint64_t hash = 0;
    const Source* p_source = nullptr;
    const char* sz_entry = nullptr;
};

/// What a glyph shows: runtime_pad.c input names, as many as layout takes. Its texture is replaced
/// whole by the port's picture; a glyph without one is a picture only atlas cells use.
struct Glyph {
    const char* sz_name = nullptr;
    Texture texture;
    PsButtonPromptArt::Layout layout = PsButtonPromptArt::Layout::Single;
    std::array<const char*, 4> inputs{};
};

/// The side x side texels at (x, y) of an atlas's first level show glyph sz_glyph (a name of the
/// table), drawn while that glyph's own texture was found in the game's files or it has none.
struct AtlasCell {
    std::uint32_t x = 0, y = 0, side = 0;
    const char* sz_glyph = nullptr;
};

/// is_upside_down: its first level holds its last row first (the game samples it with v
/// reversed), so the pictures go into its cells upside down too.
struct Atlas {
    const char* sz_name = nullptr;
    Texture texture;
    std::span<const AtlasCell> cells;
    bool is_upside_down = false;
};

/// palette: the colours the port draws the title's glyphs in.
struct Table {
    std::span<const Glyph> glyphs;
    std::span<const Atlas> atlases;
    PsButtonPromptArt::Palette palette{};
};

} // namespace PsGlyphs
