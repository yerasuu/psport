// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the on-screen keyboard of the game's text entry (ImeDialog, libSceIme), so that a player
// with only a controller can type. The window keeps the text (window.cpp) and asks the keyboard
// what each press does; the overlay draws it under the text (psport_overlay.cpp TextPrompt).

#pragma once

#include <string>
#include <string_view>
#include "common/types.h"

namespace PsOverlay {

enum class KeyAction { None, Type, Erase, Back, Accept };

struct KeyPress {
    KeyAction action = KeyAction::None;
    std::string text; ///< UTF-8, for Type
};

/// Any thread, when an entry opens: what it accepts (psport_gpu.h PS_TEXT_*) and its length in
/// UTF-16 units, as the game's dialog parameters give them. Starts on the letters with the cursor
/// on Done, so that Cross accepts the text the entry opened with.
void OpenKeyboard(int charset, u32 max_length);

/// Appends what the open entry accepts of `typed` to `text`, up to its length.
void TypeInto(std::string& text, std::string_view typed);
/// Drops the last character (code point) of UTF-8 `text`.
void EraseLast(std::string& text);

/// Window thread, a controller button while the entry is open: false when the keyboard does not
/// use it (L3, R3, Guide stay the overlay's).
bool KeyboardButton(u8 button, bool is_down, KeyPress& out_press);
/// Window thread: the left stick moves the cursor, L2 shifts, R2 switches to the symbols.
void KeyboardAxis(u8 axis, s16 value);

/// Present thread, inside the ImGui frame: the keys, and how much of the entry's length `text` uses.
void DrawKeyboard(std::string_view text);

} // namespace PsOverlay
