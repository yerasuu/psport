// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: button prompts for the device in use (button_prompts.h).
#include "button_prompts.h"

namespace PsButtonPrompts {
namespace {
using PsButtonPromptArt::Layout;

/// Close to the game's own menus: dark plates, parchment frames and labels, amber for what is lit.
constexpr PsButtonPromptArt::Palette Palette{.fill = {0.075f, 0.07f, 0.065f, 0.93f},
                                             .frame = {0.78f, 0.74f, 0.64f},
                                             .ink = {0.95f, 0.92f, 0.85f},
                                             .accent = {0.82f, 0.64f, 0.36f}};

constexpr PsGlyphs::Source CommonTpf{.sz_asset = "/app0/dvdroot_ps4/menu/common.tpf.dcx",
                                     .sz_extractor = "fromsoftware.dcx_tpf"};

/// 32x32 BC7, one level.
constexpr PsGlyphs::Texture Icon(const char* sz_entry) {
    return {.width = 32, .height = 32, .cb_texels = 1024, .p_source = &CommonTpf, .sz_entry = sz_entry};
}

// The icons are known by what they show, not by their names: KG_OK is the circle and KG_Cancel
// the cross (the Japanese layout; a release whose confirm button is the cross shows KG_Cancel for
// OK). KG_R_U and KG_R_L are the triangle and the square (the right cluster's up and left).
constexpr PsGlyphs::Glyph Icons[] = {
    {"KG_OK", Icon("KG_OK"), Layout::Single, {"circle"}},
    {"KG_Cancel", Icon("KG_Cancel"), Layout::Single, {"cross"}},
    {"KG_R_U", Icon("KG_R_U"), Layout::Single, {"triangle"}},
    {"KG_R_L", Icon("KG_R_L"), Layout::Single, {"square"}},
    {"KG_L1", Icon("KG_L1"), Layout::Single, {"l1"}},
    {"KG_L2", Icon("KG_L2"), Layout::Single, {"l2"}},
    {"KG_R1", Icon("KG_R1"), Layout::Single, {"r1"}},
    {"KG_R2", Icon("KG_R2"), Layout::Single, {"r2"}},
    {"KG_L3", Icon("KG_L3"), Layout::Single, {"l3"}},
    {"KG_R3", Icon("KG_R3"), Layout::Single, {"r3"}},
    {"KG_Start", Icon("KG_Start"), Layout::Single, {"options"}},
    {"KG_TP_L", Icon("KG_TP_L"), Layout::Single, {"touchpad"}},
    {"KG_TP_R", Icon("KG_TP_R"), Layout::Single, {"touchpad_right"}},
    {"KG_L_U", Icon("KG_L_U"), Layout::Single, {"up"}},
    {"KG_L_D", Icon("KG_L_D"), Layout::Single, {"down"}},
    {"KG_L_L", Icon("KG_L_L"), Layout::Single, {"left"}},
    {"KG_L_R", Icon("KG_L_R"), Layout::Single, {"right"}},
    {"KG_L_UD", Icon("KG_L_UD"), Layout::Column, {"up", "down"}},
    {"KG_L_LR", Icon("KG_L_LR"), Layout::Row, {"left", "right"}},
    {"KG_L_UDLR", Icon("KG_L_UDLR"), Layout::Cross, {"up", "left", "down", "right"}},
    {"KG_LS", Icon("KG_LS"), Layout::Stick, {"move_up", "move_left", "move_down", "move_right"}},
    {"KG_RS", Icon("KG_RS"), Layout::Stick, {"look_up", "look_left", "look_down", "look_right"}},
};

// Atlases of the HUD (ingametop.gfx) with icons in them, drawn as the icons' own textures (3
// texels in from the cell's edges): MENU_Common_00091 has the circle, the cross and L3 in a row,
// for the "Examine" and "Pick up" prompts at lamps, gravestones and items.
constexpr PsGlyphs::AtlasCell Common00091Cells[] = {
    {.x = 658, .y = 120, .side = 32, .sz_glyph = "KG_OK"},
    {.x = 691, .y = 120, .side = 32, .sz_glyph = "KG_Cancel"},
    {.x = 723, .y = 120, .side = 32, .sz_glyph = "KG_L3"},
};
constexpr PsGlyphs::Atlas Atlases[] = {
    {.sz_name = "MENU_Common_00091",
     .texture = {.width = 1024,
                 .height = 512,
                 .cb_texels = 1024 * 512,
                 .p_source = &CommonTpf,
                 .sz_entry = "MENU_Common_00091"},
     .cells = Common00091Cells},
};
} // namespace

extern const PsGlyphs::Table Glyphs = {.glyphs = Icons, .atlases = Atlases, .palette = Palette};

} // namespace PsButtonPrompts
