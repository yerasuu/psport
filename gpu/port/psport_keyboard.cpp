// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_keyboard.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>
#include <span>

#include "../psport_gpu.h"
#include "../../src/platform/ps_host_input.h"
#include "imgui.h"
#include "psport_settings.h"

namespace PsOverlay {

namespace {

constexpr int COLUMNS = 10;
constexpr int ROWS = 4;
constexpr std::array<std::string_view, ROWS> LETTER_ROWS{"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,.'"};
constexpr std::array<std::string_view, ROWS> SYMBOL_ROWS{"1234567890", "!@#$%^&*()", "-_=+[]{};:", "'\",./?\\|~`"};
constexpr std::array<std::string_view, 1> DIGIT_ROWS{"1234567890"};

enum class Special { Shift, Symbols, Space, Erase, Done };
struct WideKey {
    Special special;
    int c_columns;
};
constexpr std::array<WideKey, 5> TEXT_BOTTOM{{{Special::Shift, 2},
                                              {Special::Symbols, 1},
                                              {Special::Space, 3},
                                              {Special::Erase, 2},
                                              {Special::Done, 2}}};
constexpr std::array<WideKey, 2> DIGIT_BOTTOM{{{Special::Erase, 5}, {Special::Done, 5}}};

enum class Shift { Off, Once, Locked };
enum class Direction { None, Up, Down, Left, Right };

// Stick past half its travel moves the cursor, back under a third lets it go, so that a stick
// resting near the threshold does not flicker between held and released.
constexpr s16 STICK_PRESS = 16384, STICK_RELEASE = 10923;
constexpr s16 TRIGGER_PRESS = 16384, TRIGGER_RELEASE = 8192;
constexpr auto REPEAT_DELAY = std::chrono::milliseconds(400);
constexpr auto REPEAT_INTERVAL = std::chrono::milliseconds(90);

std::mutex s_keyboard_mutex; // the window thread presses, the present thread draws and repeats
int s_charset = PS_TEXT_ANY;
u32 s_max_length = 0;
bool s_is_symbols = false;
Shift s_shift = Shift::Off;
int s_i_row = 0, s_i_column = 0;
Direction s_dpad = Direction::None, s_stick_x = Direction::None, s_stick_y = Direction::None;
Direction s_held = Direction::None;
std::chrono::steady_clock::time_point s_next_repeat{};
bool s_is_l2_down = false, s_is_r2_down = false;

bool IsDigits() {
    return s_charset == PS_TEXT_DIGITS;
}

int RowCount() {
    return IsDigits() ? int(DIGIT_ROWS.size()) : ROWS;
}

std::string_view Row(int i_row) {
    return IsDigits() ? DIGIT_ROWS[i_row] : s_is_symbols ? SYMBOL_ROWS[i_row] : LETTER_ROWS[i_row];
}

std::span<const WideKey> Bottom() {
    if (IsDigits()) {
        return DIGIT_BOTTOM;
    }
    return TEXT_BOTTOM;
}

bool IsBottom(int i_row) {
    return i_row == RowCount();
}

int BottomKeyAt(int i_column) {
    const std::span<const WideKey> keys = Bottom();
    int i_end = 0;
    for (int i_key = 0; i_key < int(keys.size()); ++i_key) {
        i_end += keys[i_key].c_columns;
        if (i_column < i_end) {
            return i_key;
        }
    }
    return int(keys.size()) - 1;
}

int BottomStart(int i_key) {
    int i_start = 0;
    for (int i = 0; i < i_key; ++i) {
        i_start += Bottom()[i].c_columns;
    }
    return i_start;
}

void PutCursorOnDone() {
    s_i_row = RowCount();
    s_i_column = BottomStart(int(Bottom().size()) - 1);
}

void Move(Direction direction) {
    const int c_rows = RowCount() + 1;
    switch (direction) {
    case Direction::Up:
        s_i_row = (s_i_row + c_rows - 1) % c_rows;
        break;
    case Direction::Down:
        s_i_row = (s_i_row + 1) % c_rows;
        break;
    case Direction::Left:
    case Direction::Right: {
        const int step = direction == Direction::Left ? -1 : 1;
        if (!IsBottom(s_i_row)) {
            s_i_column = (s_i_column + COLUMNS + step) % COLUMNS;
            break;
        }
        const int c_keys = int(Bottom().size());
        s_i_column = BottomStart((BottomKeyAt(s_i_column) + c_keys + step) % c_keys);
        break;
    }
    case Direction::None:
        break;
    }
}

// The d-pad wins over the stick; a new direction moves at once, then repeats while held (DrawKeyboard).
void UpdateHeld() {
    const Direction stick = s_stick_y != Direction::None ? s_stick_y : s_stick_x;
    const Direction direction = s_dpad != Direction::None ? s_dpad : stick;
    if (direction == s_held) {
        return;
    }
    s_held = direction;
    if (direction != Direction::None) {
        Move(direction);
        s_next_repeat = std::chrono::steady_clock::now() + REPEAT_DELAY;
    }
}

void CycleShift() {
    s_shift = s_shift == Shift::Off ? Shift::Once : s_shift == Shift::Once ? Shift::Locked : Shift::Off;
}

void ToggleSymbols() {
    if (!IsDigits()) {
        s_is_symbols = !s_is_symbols;
    }
}

KeyPress PressCursorKey() {
    if (!IsBottom(s_i_row)) {
        char c = Row(s_i_row)[s_i_column];
        if (!s_is_symbols && s_shift != Shift::Off && std::isalpha(static_cast<unsigned char>(c))) {
            c = char(std::toupper(static_cast<unsigned char>(c)));
            if (s_shift == Shift::Once) {
                s_shift = Shift::Off;
            }
        }
        return {KeyAction::Type, std::string(1, c)};
    }
    switch (Bottom()[BottomKeyAt(s_i_column)].special) {
    case Special::Shift:
        CycleShift();
        return {};
    case Special::Symbols:
        ToggleSymbols();
        return {};
    case Special::Space:
        return {KeyAction::Type, " "};
    case Special::Erase:
        return {KeyAction::Erase, {}};
    case Special::Done:
        return {KeyAction::Accept, {}};
    }
    return {};
}

Direction DpadDirection(u8 button) {
    switch (button) {
    case PS_HOST_PAD_BUTTON_DPAD_UP: return Direction::Up;
    case PS_HOST_PAD_BUTTON_DPAD_DOWN: return Direction::Down;
    case PS_HOST_PAD_BUTTON_DPAD_LEFT: return Direction::Left;
    case PS_HOST_PAD_BUTTON_DPAD_RIGHT: return Direction::Right;
    default: return Direction::None;
    }
}

Direction StickDirection(Direction held, s16 value, Direction negative, Direction positive) {
    const int magnitude = value < 0 ? -int(value) : int(value);
    if (held != Direction::None && magnitude >= STICK_RELEASE && (value < 0) == (held == negative)) {
        return held;
    }
    return magnitude < STICK_PRESS ? Direction::None : value < 0 ? negative : positive;
}

// A trigger counts once per pull: true on the pull that crosses the press threshold.
bool IsTriggerPulled(bool& is_down, s16 value) {
    if (!is_down && value >= TRIGGER_PRESS) {
        is_down = true;
        return true;
    }
    if (is_down && value < TRIGGER_RELEASE) {
        is_down = false;
    }
    return false;
}

const char* SpecialLabel(Special special) {
    switch (special) {
    case Special::Shift:
        return s_shift == Shift::Locked ? PsSettings::MenuText("Caps", "Caps", "Bloq Mayús")
                                        : PsSettings::MenuText("Shift", "Shift", "Mayús");
    case Special::Symbols: return s_is_symbols ? "abc" : "#+=";
    case Special::Space: return PsSettings::MenuText("Space", "Пробел", "Espacio");
    case Special::Erase: return PsSettings::MenuText("Delete", "Удалить", "Borrar");
    case Special::Done: return PsSettings::MenuText("OK", "Готово", "Aceptar");
    }
    return "";
}

void DrawKey(const char* sz_label, int id, float width, float height, bool is_selected, bool is_lit) {
    ImGui::PushID(id);
    int c_colors = 0;
    if (is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(2.0f * ImGui::GetStyle().FontScaleMain, 1.0f));
        c_colors = 2;
    } else if (is_lit) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        c_colors = 1;
    }
    ImGui::Button(sz_label, ImVec2(width, height));
    if (is_selected) {
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleColor(c_colors);
    ImGui::PopID();
}

size_t Utf8Length(unsigned char lead) {
    return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
}

size_t Utf16Length(std::string_view text) {
    size_t c_units = 0;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) != 0x80) {
            c_units += byte >= 0xF0 ? 2 : 1;
        }
    }
    return c_units;
}

bool IsAccepted(std::string_view character) {
    const auto lead = static_cast<unsigned char>(character.front());
    switch (s_charset) {
    case PS_TEXT_DIGITS: return lead >= '0' && lead <= '9';
    case PS_TEXT_LATIN: return lead >= 0x20 && lead < 0x7F;
    default: return lead >= 0x20 && lead != 0x7F;
    }
}

} // namespace

void OpenKeyboard(int charset, u32 max_length) {
    std::scoped_lock lock{s_keyboard_mutex};
    s_charset = charset;
    s_max_length = max_length;
    s_is_symbols = false;
    s_shift = Shift::Off;
    s_dpad = s_stick_x = s_stick_y = s_held = Direction::None;
    PutCursorOnDone();
}

void TypeInto(std::string& text, std::string_view typed) {
    std::scoped_lock lock{s_keyboard_mutex};
    size_t c_units = Utf16Length(text);
    while (!typed.empty()) {
        const size_t cb_character = std::min(Utf8Length(static_cast<unsigned char>(typed.front())), typed.size());
        const std::string_view character = typed.substr(0, cb_character);
        typed.remove_prefix(cb_character);
        if (!IsAccepted(character)) {
            continue;
        }
        const size_t c_character_units = cb_character == 4 ? 2 : 1;
        if (c_units + c_character_units > s_max_length) {
            break;
        }
        text += character;
        c_units += c_character_units;
    }
}

void EraseLast(std::string& text) {
    if (text.empty()) {
        return;
    }
    size_t cut = text.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    text.erase(cut);
}

bool KeyboardButton(u8 button, bool is_down, KeyPress& out_press) {
    std::scoped_lock lock{s_keyboard_mutex};
    out_press = {};
    if (const Direction direction = DpadDirection(button); direction != Direction::None) {
        if (is_down) {
            s_dpad = direction;
        } else if (s_dpad == direction) {
            s_dpad = Direction::None;
        }
        UpdateHeld();
        return true;
    }
    switch (button) {
    case PS_HOST_PAD_BUTTON_SOUTH:
        if (is_down) {
            out_press = PressCursorKey();
        }
        return true;
    case PS_HOST_PAD_BUTTON_EAST:
        if (is_down) {
            out_press.action = KeyAction::Back;
        }
        return true;
    case PS_HOST_PAD_BUTTON_WEST:
        if (is_down) {
            out_press.action = KeyAction::Erase;
        }
        return true;
    case PS_HOST_PAD_BUTTON_NORTH:
        if (is_down) {
            out_press = {KeyAction::Type, " "};
        }
        return true;
    case PS_HOST_PAD_BUTTON_START:
        if (is_down) {
            out_press.action = KeyAction::Accept;
        }
        return true;
    default:
        return false;
    }
}

void KeyboardAxis(u8 axis, s16 value) {
    std::scoped_lock lock{s_keyboard_mutex};
    switch (axis) {
    case PS_HOST_PAD_AXIS_LEFTX:
        s_stick_x = StickDirection(s_stick_x, value, Direction::Left, Direction::Right);
        UpdateHeld();
        break;
    case PS_HOST_PAD_AXIS_LEFTY:
        s_stick_y = StickDirection(s_stick_y, value, Direction::Up, Direction::Down);
        UpdateHeld();
        break;
    case PS_HOST_PAD_AXIS_LEFT_TRIGGER:
        if (IsTriggerPulled(s_is_l2_down, value) && !IsDigits()) {
            CycleShift();
        }
        break;
    case PS_HOST_PAD_AXIS_RIGHT_TRIGGER:
        if (IsTriggerPulled(s_is_r2_down, value)) {
            ToggleSymbols();
        }
        break;
    default:
        break;
    }
}

void DrawKeyboard(std::string_view text) {
    std::scoped_lock lock{s_keyboard_mutex};
    if (s_held != Direction::None) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= s_next_repeat) {
            Move(s_held);
            // A frame late or a hitch moves once, not by every interval it missed.
            s_next_repeat = std::max(s_next_repeat + REPEAT_INTERVAL, now);
        }
    }
    ImGui::TextDisabled("%zu / %u", Utf16Length(text), s_max_length);
    const float height = ImGui::GetFrameHeight() * 1.6f;
    const float width = height * 1.25f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    for (int i_row = 0; i_row < RowCount(); ++i_row) {
        const std::string_view row = Row(i_row);
        for (int i_column = 0; i_column < COLUMNS; ++i_column) {
            char c = row[i_column];
            if (!s_is_symbols && s_shift != Shift::Off && std::isalpha(static_cast<unsigned char>(c))) {
                c = char(std::toupper(static_cast<unsigned char>(c)));
            }
            const char sz_label[2] = {c, 0};
            if (i_column) {
                ImGui::SameLine();
            }
            DrawKey(sz_label, i_row * COLUMNS + i_column, width, height, i_row == s_i_row && i_column == s_i_column,
                    false);
        }
    }
    const int i_cursor_key = IsBottom(s_i_row) ? BottomKeyAt(s_i_column) : -1;
    const std::span<const WideKey> keys = Bottom();
    for (int i_key = 0; i_key < int(keys.size()); ++i_key) {
        if (i_key) {
            ImGui::SameLine();
        }
        const int c_columns = keys[i_key].c_columns;
        const bool is_lit = (keys[i_key].special == Special::Shift && s_shift != Shift::Off) ||
                            (keys[i_key].special == Special::Symbols && s_is_symbols);
        DrawKey(SpecialLabel(keys[i_key].special), ROWS * COLUMNS + i_key, width * c_columns + gap * (c_columns - 1),
                height, i_key == i_cursor_key, is_lit);
    }
}

} // namespace PsOverlay
