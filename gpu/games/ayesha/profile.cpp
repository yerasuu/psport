// SPDX-License-Identifier: GPL-2.0-or-later
// Atelier Ayesha DX: what the translator knows about it (Game::Profile; its title is title.cpp):
// its button glyphs as data (games/common/glyphs.h). The game draws its prompts ("Message Log",
// menus, dialogs) from one 512x512 BC3 UI atlas holding every PlayStation button, a bright and a
// dimmed one of each face and shoulder button and the d-pad with each direction lit.
#include "../common/glyphs.h"
#include "game_profile.h"

namespace Game {
namespace {
using PsButtonPromptArt::Layout;

/// Close to the game's own glyphs: black plates in a white ring, gold for what is lit.
constexpr PsButtonPromptArt::Palette Palette{.fill = {0.05f, 0.05f, 0.05f, 0.92f},
                                             .frame = {0.95f, 0.95f, 0.95f},
                                             .ink = {1.0f, 1.0f, 1.0f},
                                             .accent = {0.92f, 0.74f, 0.36f}};

constexpr PsGlyphs::Glyph Glyphs[] = {
    {"cross", {}, Layout::Single, {"cross"}},
    {"circle", {}, Layout::Single, {"circle"}},
    {"square", {}, Layout::Single, {"square"}},
    {"triangle", {}, Layout::Single, {"triangle"}},
    {"l1", {}, Layout::Single, {"l1"}},
    {"r1", {}, Layout::Single, {"r1"}},
    {"l2", {}, Layout::Single, {"l2"}},
    {"r2", {}, Layout::Single, {"r2"}},
    {"l3", {}, Layout::Single, {"l3"}},
    {"options", {}, Layout::Single, {"options"}},
    {"touchpad", {}, Layout::Single, {"touchpad"}},
    {"up", {}, Layout::Single, {"up"}},
    {"down", {}, Layout::Single, {"down"}},
    {"left", {}, Layout::Single, {"left"}},
    {"right", {}, Layout::Single, {"right"}},
    {"up_down", {}, Layout::Column, {"up", "down"}},
    {"left_right", {}, Layout::Row, {"left", "right"}},
    {"d_pad", {}, Layout::Cross, {"up", "left", "down", "right"}},
    {"left_stick", {}, Layout::Stick, {"move_up", "move_left", "move_down", "move_right"}},
    {"right_stick", {}, Layout::Stick, {"look_up", "look_left", "look_down", "look_right"}},
};

// Texels as the game uploads them, its picture upside down (the circle row, last here, shows on
// top). The dimmed copies (the second of each pair) take the same picture, so that a prompt the
// game greys out still names the key; the d-pad with nothing lit stays the game's. L1 and R1 are
// 48x32: their square cells reach past the plates' top and bottom, where the atlas is empty.
constexpr PsGlyphs::AtlasCell UiCells[] = {
    {.x = 1, .y = 330, .side = 44, .sz_glyph = "square"},
    {.x = 47, .y = 330, .side = 44, .sz_glyph = "square"},
    {.x = 93, .y = 330, .side = 44, .sz_glyph = "up_down"},
    {.x = 139, .y = 330, .side = 44, .sz_glyph = "right"},
    {.x = 185, .y = 330, .side = 44, .sz_glyph = "left"},
    {.x = 231, .y = 330, .side = 44, .sz_glyph = "up"},
    {.x = 277, .y = 330, .side = 44, .sz_glyph = "down"},
    {.x = 323, .y = 330, .side = 44, .sz_glyph = "d_pad"},
    {.x = 1, .y = 375, .side = 44, .sz_glyph = "triangle"},
    {.x = 47, .y = 375, .side = 44, .sz_glyph = "triangle"},
    {.x = 93, .y = 375, .side = 44, .sz_glyph = "left_right"},
    {.x = 140, .y = 374, .side = 46, .sz_glyph = "options"},
    {.x = 193, .y = 374, .side = 45, .sz_glyph = "touchpad"},
    {.x = 240, .y = 375, .side = 44, .sz_glyph = "left_stick"},
    {.x = 286, .y = 375, .side = 44, .sz_glyph = "right_stick"},
    {.x = 139, .y = 418, .side = 48, .sz_glyph = "l1"},
    {.x = 190, .y = 418, .side = 48, .sz_glyph = "l1"},
    {.x = 241, .y = 418, .side = 48, .sz_glyph = "r1"},
    {.x = 292, .y = 418, .side = 48, .sz_glyph = "r1"},
    {.x = 0, .y = 422, .side = 44, .sz_glyph = "cross"},
    {.x = 46, .y = 422, .side = 44, .sz_glyph = "cross"},
    {.x = 1, .y = 467, .side = 44, .sz_glyph = "circle"},
    {.x = 47, .y = 467, .side = 44, .sz_glyph = "circle"},
    {.x = 139, .y = 467, .side = 44, .sz_glyph = "l2"},
    {.x = 185, .y = 467, .side = 44, .sz_glyph = "l2"},
    {.x = 231, .y = 467, .side = 44, .sz_glyph = "r2"},
    {.x = 277, .y = 467, .side = 44, .sz_glyph = "r2"},
    {.x = 322, .y = 467, .side = 44, .sz_glyph = "l3"},
};

// The hash is the US release's (CUSA16921) level0_hash from PS_DUMP_SMALL_TEXTURES until an
// extractor reads Gust's G1T archives from the game's own files.
constexpr PsGlyphs::Atlas Atlases[] = {
    {.sz_name = "ui_buttons",
     .texture = {.width = 512, .height = 512, .cb_texels = 512 * 512, .hash = 0x673fd53c2e88ed8cull},
     .cells = UiCells,
     .is_upside_down = true},
};

constexpr PsGlyphs::Table GlyphTable = {.glyphs = Glyphs, .atlases = Atlases, .palette = Palette};
} // namespace

extern const Profile AyeshaProfile = {
    .p_glyphs = &GlyphTable,
};

} // namespace Game
