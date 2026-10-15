// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_menu.h"

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "psport_settings.h"
#include "effects.h"

namespace PsGameMenu {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The game's menu code (eboot 1.09 vaddrs; found from the System menu builder 0x1bb3ad0).
constexpr u64 MsgLookup = 0xf6f340;      ///< (repository, table, category, id) -> UTF-16 text
constexpr u64 AddPageItem = 0x1b4e2a0;   ///< (menu, name+help texts, page factory, out)
constexpr u64 MakeText = 0x1ae8cc0;      ///< (out 0x40 bytes, category, id): a text entry
constexpr u64 FactoryToFunction = 0x1b6ed90; ///< ({vtable, fn}, out std::function) -> its __f_
constexpr u64 CreatePage = 0x1bb4c70;    ///< (out, arg, content std::function): an options page
constexpr u64 OpenPage = 0x1b20900;      ///< (a, b, layout name, rows fn, 0, 0): the page's content
constexpr u64 ListPush = 0x1b1bbd0;      ///< (value list, {u32 value, text}): up to 32 values
constexpr u64 ListRow = 0x1b78ab0;   ///< (page, texts, int* value, value list, int* default): pop-up list
constexpr u64 PairPush = 0x1b2c200;  ///< (two-value list, {u8 value, text})
constexpr u64 PairRow = 0x1b2a100;   ///< (page, texts, u8* value, two-value list, u8* default): left/right
constexpr u64 SliderRow = 0x1b2ac00; ///< (page, texts, u8* value 0..10, u8* default)
constexpr u64 FactoryVtable = 0x533d120; ///< a System item's {vtable, page factory fn}
constexpr u64 ContentVtable = 0x5343880; ///< an options page's {vtable, content fn}
constexpr u64 ControlsLayout = 0x4934065; ///< "ControllSetting": six plain rows, the layout used
constexpr u32 TextName = 0xc8, TextHelp = 0xc9; // SP_menu text / SP_one-line help
constexpr u32 ScreenSoundItem = 110001;         // "Screen/Sound" in the System menu: ours follow it
constexpr u32 ControlsTitle = 113020;           // the ControllSetting layout's title ("Controls")
constexpr std::size_t RowsPerPage = 6;          // ControllSetting's rows

const u8 MsgLookupPrologue[] = {0x89, 0xf0, 0x48, 0x8b, 0x77, 0x08, 0x48, 0x8b, 0x34, 0xc6};
const u8 AddPageItemPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

u64 s_image_base = 0;
template <typename F>
F Game(u64 va_guest) {
    return reinterpret_cast<F>(s_image_base + va_guest);
}

// ---- Texts: ids from IdBase up, looked up by the game like its own messages. ----
constexpr u32 IdBase = 0x7f0000;
struct Text {
    std::u16string english, russian;
};
std::vector<Text> s_texts;

std::u16string Utf16(const char* sz_utf8) {
    std::u16string out;
    for (const unsigned char* p_cursor = reinterpret_cast<const unsigned char*>(sz_utf8); *p_cursor;) {
        u32 c = *p_cursor++;
        if (c >= 0xf0) {
            c = (c & 7) << 18 | (p_cursor[0] & 0x3f) << 12 | (p_cursor[1] & 0x3f) << 6 | (p_cursor[2] & 0x3f);
            p_cursor += 3;
        } else if (c >= 0xe0) {
            c = (c & 15) << 12 | (p_cursor[0] & 0x3f) << 6 | (p_cursor[1] & 0x3f);
            p_cursor += 2;
        } else if (c >= 0xc0) {
            c = (c & 31) << 6 | (p_cursor[0] & 0x3f);
            p_cursor += 1;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xd800 + (c >> 10)));
            out.push_back(char16_t(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}
u32 AddText(const char* sz_english, const char* sz_russian) {
    s_texts.push_back({Utf16(sz_english), Utf16(sz_russian)});
    return IdBase + u32(s_texts.size() - 1);
}
/// The game's own language decides (its "Screen/Sound" text in Cyrillic: Russian, else English);
/// the port's menu language until the game has looked that text up.
enum class GameLanguage { Unknown, English, Russian };
std::atomic<GameLanguage> s_game_language{GameLanguage::Unknown};

const char16_t* TextOf(u32 id) {
    const Text& t = s_texts[id - IdBase];
    const GameLanguage game = s_game_language.load();
    const bool is_russian = game == GameLanguage::Unknown
                             ? PsSettings::Get().menu_language == PsSettings::MenuLanguage::Russian
                             : game == GameLanguage::Russian;
    return is_russian ? t.russian.c_str() : t.english.c_str();
}

// ---- The port's values the game's rows edit (ints: the list rows bind ints). ----
enum Field : int {
    OutputRes, Upscaler, Preset, Sharpness, ShowFps,
    ModelLod, FirstEffect, FieldCount = FirstEffect + PsEffects::SwitchCount
};
static_assert(PsEffects::SwitchCount <= PsSettings::MaxEffects);
int s_values[FieldCount];  // what the game's rows show and change (list rows bind ints)
u8 s_bytes[FieldCount];    // the same for toggle and slider rows (they bind bytes)
bool s_byte_field[FieldCount]; // fields shown by toggle or slider rows
int s_default_values[FieldCount]; // the port's defaults: what the page's "Default" sets
u8 s_default_bytes[FieldCount];
int s_applied[FieldCount]; // what Poll applied last
std::atomic<bool> s_has_values{false};
constexpr float SharpnessStep = 0.2f; // the game's slider: 0..10 -> 0.0 .. 2.0
constexpr int SharpnessSteps = 11;
constexpr int LodValues[] = {-2, 0, 1, 2};
/// Rows with a few ordered choices on the game's 0..10 slider (the quality preset, the output
/// resolution): a pop-up list let the rows below it show through, so only the last row of a page
/// is a list. The game keeps its own copy of a slider's value while the page is open, so every
/// position stands for a choice (ranges, as the row's help says), and a choice is shown at
/// ChoicePosition.
int s_steps_count[FieldCount]; ///< choices of a slider-with-choices row, else 0

int ChoiceFromPosition(u8 position, int c_choices) {
    return std::clamp((int(position) * (c_choices - 1) + 5) / 10, 0, c_choices - 1);
}

u8 ChoicePosition(int choice, int c_choices) {
    return u8((std::clamp(choice, 0, c_choices - 1) * 10 * 2 + (c_choices - 1)) / (2 * (c_choices - 1)));
}

/// The rows' values of settings `s`.
void Fill(const PsSettings::Values& s, int* p_values) {
    p_values[OutputRes] = s.output_res;
    p_values[Upscaler] = s.upscaler;
    p_values[Preset] = s.preset;
    // One slider for both: 0 is sharpening off.
    p_values[Sharpness] =
        s.should_sharpen ? std::clamp(int(std::lround(s.sharpness / SharpnessStep)), 1, SharpnessSteps - 1) : 0;
    p_values[ShowFps] = s.should_show_fps;
    p_values[ModelLod] = 1;
    for (int i = 0; i < 4; ++i) {
        if (LodValues[i] == s.model_lod) p_values[ModelLod] = i;
    }
    for (int e = 0; e < PsEffects::SwitchCount; ++e) {
        p_values[FirstEffect + e] = s.effects[e];
    }
}

void LoadValues() {
    Fill(PsSettings::Get(), s_values);
    for (int f = 0; f < FieldCount; ++f) {
        s_bytes[f] = s_steps_count[f] ? ChoicePosition(s_values[f], s_steps_count[f]) : u8(s_values[f]);
    }
    std::memcpy(s_applied, s_values, sizeof(s_values));
    s_has_values = true;
}

// ---- Pages and rows ----
enum class Kind { List, Toggle, Slider, Steps };
struct Row {
    Field field;
    u32 name, help;
    std::vector<u32> choices; ///< value texts, value = index (a toggle: off, on)
    Kind kind = Kind::List;
};
struct Page {
    u32 name, help;
    std::vector<Row> rows; ///< at most RowsPerPage (the layout's)
};
std::vector<Page> s_pages;

void DefineTexts() {
    const u32 i_text_off = AddText("Off", "Выкл"), i_text_on = AddText("On", "Вкл");
    const std::vector<u32> toggle = {i_text_off, i_text_on};
    const auto page = [](const char* sz_en, const char* sz_ru, const char* sz_help_en, const char* sz_help_ru) {
        const u32 name = AddText(sz_en, sz_ru);
        s_pages.push_back({name, AddText(sz_help_en, sz_help_ru), {}});
    };
    const auto row = [](Field f, const char* sz_en, const char* sz_ru, const char* sz_help_en,
                        const char* sz_help_ru, std::vector<u32> choices, Kind kind = Kind::List) {
        if (kind == Kind::List && choices.size() == 2) {
            kind = Kind::Toggle; // left/right in place, as the game's own on/off rows
        }
        s_pages.back().rows.push_back(
            {f, AddText(sz_en, sz_ru), AddText(sz_help_en, sz_help_ru), std::move(choices), kind});
    };

    // Two pages of the ControllSetting layout's six rows (the game's other layouts are fixed
    // forms): the picture, then the game's effects. The other patches (intros, the free camera,
    // the debug menu) and the game's own AA are in the overlay menu and the launcher.
    page("Display", "Изображение", "Resolution, upscaler, sharpness and model detail (psport)",
         "Разрешение, апскейлер, резкость и детализация (psport)");
    // Sliders and toggles first, the pop-up list last: a list opens downwards over the rows
    // below it, and their values showed through it.
    row(Preset, "Quality preset", "Пресет",
        "Scene resolution: 0-1 Native AA, 2-3 Quality, 4-6 Balanced, 7-8 Performance, 9-10 Ultra",
        "Разрешение сцены: 0-1 Native AA, 2-3 Quality, 4-6 Balanced, 7-8 Performance, 9-10 Ultra",
        {AddText("Native AA", "Native AA"), AddText("Quality", "Quality"), AddText("Balanced", "Balanced"),
         AddText("Performance", "Performance"), AddText("Ultra Performance", "Ultra Performance")},
        Kind::Steps);
    row(Sharpness, "Sharpness", "Резкость", "RCAS after the upscaler: 0 off, 10 strongest",
        "RCAS после апскейлера: 0 выкл, 10 сильнее всего", {}, Kind::Slider);
    row(OutputRes, "Output resolution", "Разрешение вывода",
        "Final frame: 0-1 1280x720, 2-4 1920x1080, 5-7 2560x1440, 8-10 3840x2160",
        "Готовый кадр: 0-1 1280x720, 2-4 1920x1080, 5-7 2560x1440, 8-10 3840x2160",
        {AddText("1280 x 720", "1280 x 720"), AddText("1920 x 1080", "1920 x 1080"),
         AddText("2560 x 1440", "2560 x 1440"), AddText("3840 x 2160", "3840 x 2160")},
        Kind::Steps);
    row(ShowFps, "FPS counter", "Счётчик FPS", "Frame rate in the top right corner",
        "Частота кадров в правом верхнем углу", toggle);
    row(ModelLod, "Model detail", "Детализация",
        "After a restart: 0-1 highest, 2-4 game default, 5-7 lower, 8-10 lowest",
        "После перезапуска: 0-1 максимальная, 2-4 как в игре, 5-7 ниже, 8-10 минимальная",
        {AddText("Highest", "Максимальная"), AddText("Game default", "Как в игре"),
         AddText("Lower", "Ниже"), AddText("Lowest", "Минимальная")},
        Kind::Steps);
    // The page's only pop-up list, last: it opens below itself.
    row(Upscaler, "Upscaler", "Апскейлер", "Temporal upscaler and anti-aliasing",
        "Временной апскейлер и сглаживание",
        {i_text_off, AddText("FSR 3.1", "FSR 3.1"), AddText("FSR 4", "FSR 4"), AddText("FSR 4.1.1", "FSR 4.1.1"),
         AddText("TAA", "TAA"), AddText("DLSS", "DLSS")});

    page("Effects", "Эффекты", "The game's effects (psport; after a restart)",
         "Эффекты игры (psport; после перезапуска)");

    // Short names for the game's narrow name column (PsEffects::Switches order); the help line
    // has the details.
    struct Short {
        const char *sz_key, *sz_en, *sz_ru, *sz_help_en, *sz_help_ru;
        bool is_graphics = true;
    };
    static const Short s_shorts[] = {
        {"effect_chromatic_aberration", "Chromatic aberration", "Хром. аберрация",
         "Colour fringes at the frame edges", "Цветные каймы по краям кадра"},
        {"effect_dof", "Depth of field", "Глубина резкости", "Blur of distant and near objects (DoF)",
         "Размытие дальних и близких объектов (DoF)"},
        {"effect_motion_blur", "Motion blur", "Размытие в движении", "Blur when the camera turns",
         "Размытие при повороте камеры"},
        {"effect_ssao", "Ambient occlusion", "Затенение SSAO", "Shading in corners and contacts (SSAO)",
         "Затенение в углах и местах касания (SSAO)"},
        {"effect_game_aa", "Game's own AA", "Сглаживание игры", "The game's own anti-aliasing",
         "Собственное сглаживание игры", false},
        {"effect_dynamic_shadows", "Dynamic shadows", "Динамические тени", "Shadows from dynamic lights",
         "Тени от динамических источников света"},
        {"effect_ssr", "SSR reflections", "Отражения SSR", "Screen-space reflections (not in the original)",
         "Экранные отражения (не было в оригинале)"},
        {"skip_intro", "Skip intros", "Пропуск заставок", "Skip the startup logos and intro",
         "Пропуск логотипов и заставки при запуске", false},
        {"debug_camera", "Free camera", "Свободная камера", "Toggled with Cross + L3",
         "Включается Cross + L3", false},
        {"debug_menu", "Debug menu", "Debug menu", "The game's debug menu (requires font files)",
         "Отладочное меню игры (нужны файлы шрифтов)", false},
    };
    for (int e = 0; e < PsEffects::SwitchCount; ++e) {
        const auto& effect = PsEffects::Switches[e];
        const Short* p_name = nullptr;
        for (const Short& s : s_shorts) {
            if (std::strcmp(s.sz_key, effect.sz_key) == 0) p_name = &s;
        }
        if ((p_name && !p_name->is_graphics) || s_pages.back().rows.size() == RowsPerPage) {
            continue; // the overlay menu and the launcher have it
        }
        // A new effect without a short name: its full label.
        const std::string help_en =
            std::string(p_name ? p_name->sz_help_en : effect.label.sz_english) + " (after a restart)";
        const std::string help_ru =
            std::string(p_name ? p_name->sz_help_ru : effect.label.sz_russian) + " (после перезапуска)";
        row(Field(FirstEffect + e), p_name ? p_name->sz_en : effect.label.sz_english,
            p_name ? p_name->sz_ru : effect.label.sz_russian, help_en.c_str(), help_ru.c_str(), toggle);
    }
}

// ---- The game's structures, as its own code builds them. ----
constexpr std::size_t TextSize = 0x40;     // text entry (MakeText): text pointer, string, flag
constexpr std::size_t ValueSize = 0x48;    // {u32 value; text}
constexpr std::size_t ListSize = 0x920;    // 32 values + count at +0x908 (+ alignment slack)
constexpr std::size_t PairListSize = 0xb0; // 2 values + count at +0x98 (+ alignment slack)
constexpr std::size_t ListCountOffset = 0x908, PairCountOffset = 0x98;
constexpr std::size_t FunctionSize = 0x28; // std::function: 32-byte buffer, __f_ at +0x20

/// Destroys a std::function the way the game's code does (its __f_ at +0x20).
void DestroyFunction(u8* p_function) {
    void* p_impl = *reinterpret_cast<void**>(p_function + 0x20);
    if (!p_impl) {
        return;
    }
    using Destroy = void (*)(void*, int);
    Destroy pfn_destroy = (*reinterpret_cast<Destroy**>(p_impl))[4];
    pfn_destroy(p_impl, p_impl != p_function);
    *reinterpret_cast<void**>(p_function + 0x20) = nullptr;
}

/// Frees a text entry's string the way the game's code does (heap buffer once capacity >= 8).
void DestroyText(u8* p_text) {
    if (*reinterpret_cast<u64*>(p_text + 0x28) >= 8) {
        void* p_allocator = *reinterpret_cast<void**>(p_text + 0x30);
        using Free = void (*)(void*, void*);
        (*reinterpret_cast<Free**>(p_allocator))[0x70 / 8](p_allocator, *reinterpret_cast<void**>(p_text + 0x10));
    }
    *reinterpret_cast<u64*>(p_text + 0x28) = 7;
    *reinterpret_cast<u64*>(p_text + 0x20) = 0;
    *reinterpret_cast<char16_t*>(p_text + 0x10) = 0;
}
void DestroyTexts(u8* p_pair) {
    DestroyText(p_pair);
    DestroyText(p_pair + TextSize);
}
/// A value list's entries (count at `count_offset`), after the row has copied them.
void DestroyValues(u8* p_list, std::size_t c_offsets) {
    const u64 c_values = *reinterpret_cast<u64*>(p_list + c_offsets);
    for (u64 i = 0; i < c_values; ++i) {
        DestroyText(p_list + i * ValueSize + 8);
    }
}

void MakeTexts(u8* p_pair, u32 name, u32 help) {
    using Make = void* (*)(void*, u32, u32);
    Game<Make>(MakeText)(p_pair, TextName, name);
    Game<Make>(MakeText)(p_pair + TextSize, TextHelp, help);
}

/// Upscalers this GPU and build can run (known once the device exists, before any menu).
bool UpscalerAvailable(int i) {
    const auto& s = PsSettings::Get();
    return i == PsSettings::UpscalerFsr4     ? s.is_fsr4_supported.load()
           : i == PsSettings::UpscalerFsr411 ? s.is_fsr411_supported.load()
           : i == PsSettings::UpscalerDlss   ? s.is_dlss_supported.load()
                                             : true;
}

void AddRows(void* p_page, const std::vector<Row>& rows) {
    using Push = void (*)(void*, void*);
    using AddRow = void (*)(void*, void*, int*, void*, const int*);
    using Make = void* (*)(void*, u32, u32);
    for (const Row& row : rows) {
        alignas(16) u8 pair[2 * TextSize] = {};
        MakeTexts(pair, row.name, row.help);
        if (row.kind == Kind::Slider || row.kind == Kind::Steps) {
            using Slider = void (*)(void*, void*, u8*, const u8*);
            Game<Slider>(SliderRow)(p_page, pair, &s_bytes[row.field], &s_default_bytes[row.field]);
            DestroyTexts(pair);
            continue;
        }
        if (row.kind == Kind::Toggle) {
            using PairRowFn = void (*)(void*, void*, u8*, void*, const u8*);
            u8* p_two = static_cast<u8*>(std::aligned_alloc(16, PairListSize));
            std::memset(p_two, 0, PairListSize);
            for (std::size_t i = 0; i < 2; ++i) {
                alignas(16) u8 value[ValueSize] = {};
                value[0] = u8(i);
                Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
                Game<Push>(PairPush)(p_two, value);
                DestroyText(value + 8);
            }
            Game<PairRowFn>(PairRow)(p_page, pair, &s_bytes[row.field], p_two, &s_default_bytes[row.field]);
            DestroyValues(p_two, PairCountOffset);
            std::free(p_two);
            DestroyTexts(pair);
            continue;
        }
        u8* p_list = static_cast<u8*>(std::aligned_alloc(16, ListSize));
        std::memset(p_list, 0, ListSize);
        for (std::size_t i = 0; i < row.choices.size(); ++i) {
            if (row.field == Upscaler && !UpscalerAvailable(int(i))) {
                continue; // the list's values are the upscaler numbers, not positions
            }
            alignas(16) u8 value[ValueSize] = {};
            *reinterpret_cast<u32*>(value) = u32(i);
            Game<Make>(MakeText)(value + 8, TextName, row.choices[i]);
            Game<Push>(ListPush)(p_list, value);
            DestroyText(value + 8);
        }
        Game<AddRow>(ListRow)(p_page, pair, &s_values[row.field], p_list, &s_default_values[row.field]);
        DestroyValues(p_list, ListCountOffset);
        std::free(p_list);
        DestroyTexts(pair);
    }
}

std::atomic<const char16_t*> s_p_title_override{nullptr}; // while the game builds one of our pages (its menu thread)
// Called by the game's menu code (guest threads, System V ABI like the game), one set per page.
template <int P>
void Rows(void* p_page, void*) {
    LoadValues();
    AddRows(p_page, s_pages[P].rows);
}
template <int P>
void* Content(void* p_a, void* p_b) {
    using Open = void* (*)(void*, void*, const char*, void*, u64, u64);
    s_p_title_override = nullptr; // the title was looked up just before
    return Game<Open>(OpenPage)(p_a, p_b, Game<const char*>(ControlsLayout),
                                reinterpret_cast<void*>(&Rows<P>), 0, 0);
}
void* Factory(void* p_out, void* p_arg, void* p_content, const char16_t* p_title) {
    using Create = void* (*)(void*, void*, void*);
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<u64*>(function) = s_image_base + ContentVtable;
    *reinterpret_cast<void**>(function + 8) = p_content;
    *reinterpret_cast<void**>(function + 0x20) = function;
    // The page is built later (the game's menu task): its layout title is looked up right before
    // the content function runs, which takes the override back.
    s_p_title_override = p_title;
    Game<Create>(CreatePage)(p_out, p_arg, function);
    DestroyFunction(function);
    return p_out;
}
template <int P>
void* PageFactory(void* p_out, void* p_arg) {
    return Factory(p_out, p_arg, reinterpret_cast<void*>(&Content<P>), TextOf(s_pages[P].name));
}
constexpr std::size_t MaxPages = 6;
void* const s_factories[MaxPages] = {
    reinterpret_cast<void*>(&PageFactory<0>), reinterpret_cast<void*>(&PageFactory<1>),
    reinterpret_cast<void*>(&PageFactory<2>), reinterpret_cast<void*>(&PageFactory<3>),
    reinterpret_cast<void*>(&PageFactory<4>), reinterpret_cast<void*>(&PageFactory<5>)};

// ---- Hooks ----
using LookupFn = const char16_t* (*)(void*, u32, u32, u32);
using AddFn = void* (*)(void*, void*, void*, void*);
LookupFn s_pfn_lookup_original = nullptr;
AddFn s_pfn_add_original = nullptr;
std::atomic<const void*> s_p_screen_sound_text{nullptr};

const char16_t* LookupHook(void* p_repository, u32 table, u32 category, u32 id) {
    if (id >= IdBase && id < IdBase + s_texts.size()) {
        // PS_GAME_MENU_TRACE=1: how often the game asks for our texts (per 2 s).
        static const bool s_should_count = [] {
            const char* sz_env = std::getenv("PS_GAME_MENU_TRACE");
            return sz_env && sz_env[0] == '1';
        }();
        if (s_should_count) {
            static std::atomic<u64> s_c_lookups{0};
            static std::atomic<long long> s_printed{0};
            ++s_c_lookups;
            const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            if (now - s_printed.load() >= 2) {
                s_printed = now;
                std::printf("Game menu: %llu lookups of the port's texts in 2 s (last id %u)\n",
                            (unsigned long long)s_c_lookups.exchange(0), id - IdBase);
            }
        }
        return TextOf(id);
    }
    if (s_p_title_override && category == TextName && id == ControlsTitle) {
        return s_p_title_override;
    }
    const char16_t* p_text = s_pfn_lookup_original(p_repository, table, category, id);
    static const bool s_should_trace = [] {
        const char* sz_env = std::getenv("PS_GAME_MENU_TRACE");
        return sz_env && sz_env[0] == '1';
    }();
    if (s_should_trace && id >= 110000 && id < 120000) {
        std::printf("Game menu: text %#x/%u%s from %#llx\n", category, id, s_p_title_override ? " (our page)" : "",
                    (unsigned long long)(reinterpret_cast<u64>(__builtin_return_address(0)) - s_image_base));
    }
    if (category == TextName && id == ScreenSoundItem && p_text) {
        s_p_screen_sound_text = p_text;
        bool is_cyrillic = false;
        for (const char16_t* p_char = p_text; *p_char; ++p_char) {
            is_cyrillic = is_cyrillic || (*p_char >= 0x400 && *p_char < 0x500);
        }
        s_game_language = is_cyrillic ? GameLanguage::Russian : GameLanguage::English;
    }
    return p_text;
}

void AddItem(void* p_menu, u32 name, u32 help, void* p_factory) {
    using ToFunction = void* (*)(void*, void*);
    alignas(16) u8 pair[2 * TextSize] = {};
    MakeTexts(pair, name, help);
    alignas(16) u8 source[FunctionSize] = {};
    *reinterpret_cast<u64*>(source) = s_image_base + FactoryVtable;
    *reinterpret_cast<void**>(source + 8) = p_factory;
    *reinterpret_cast<void**>(source + 0x20) = source;
    alignas(16) u8 function[FunctionSize] = {};
    *reinterpret_cast<void**>(function + 0x20) = Game<ToFunction>(FactoryToFunction)(source, function);
    alignas(16) u8 out[16] = {};
    s_pfn_add_original(p_menu, pair, function, out);
    DestroyFunction(function);
    DestroyTexts(pair);
}

void* AddHook(void* p_menu, void* p_pair, void* p_function, void* p_out) {
    void* p_result = s_pfn_add_original(p_menu, p_pair, p_function, p_out);
    const void* p_screen_sound = s_p_screen_sound_text.load();
    if (p_screen_sound && *reinterpret_cast<const void* const*>(p_pair) == p_screen_sound) {
        for (std::size_t i = 0; i < s_pages.size() && i < MaxPages; ++i) {
            AddItem(p_menu, s_pages[i].name, s_pages[i].help, s_factories[i]);
        }
    }
    return p_result;
}

/// `jmp hook` at the function (via a near thunk); its moved prologue + `jmp` back is the original.
bool Detour(unsigned char* p_image, u64 va_target, const u8* p_prologue, std::size_t length, void* p_hook,
            void** pp_original, u8*& cursor) {
    u8* p_site = p_image + va_target;
    if (std::memcmp(p_site, p_prologue, length) != 0) {
        std::printf("Game menu: unexpected code at %#llx, not hooked\n", (unsigned long long)va_target);
        return false;
    }
    u8* p_trampoline = cursor;
    std::memcpy(p_trampoline, p_prologue, length);
    p_trampoline[length] = 0xe9;
    const std::int64_t back = std::int64_t(p_site + length) - std::int64_t(p_trampoline + length + 5);
    const std::int32_t back32 = std::int32_t(back);
    std::memcpy(p_trampoline + length + 1, &back32, 4);
    u8* p_thunk = p_trampoline + 32;
    p_thunk[0] = 0x48;
    p_thunk[1] = 0xb8; // movabs rax, hook
    std::memcpy(p_thunk + 2, &p_hook, 8);
    p_thunk[10] = 0xff;
    p_thunk[11] = 0xe0; // jmp rax
    const std::int64_t to = std::int64_t(p_thunk) - std::int64_t(p_site + 5);
    if (back != back32 || to != std::int32_t(to)) {
        std::printf("Game menu: stubs out of jump range, not hooked\n");
        return false;
    }
    const std::int32_t to32 = std::int32_t(to);
    p_site[0] = 0xe9;
    std::memcpy(p_site + 1, &to32, 4);
    for (std::size_t i = 5; i < length; ++i) {
        p_site[i] = 0xcc;
    }
    *pp_original = p_trampoline;
    cursor += 64;
    return true;
}

/// A mapping within +-2 GiB of the image, for rel32 jumps both ways.
u8* MapNear(unsigned char* p_image, u64 cb_image, std::size_t cb_range) {
    const u64 base = reinterpret_cast<u64>(p_image);
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (24ull << 20), base + cb_image + k * (24ull << 20)}) {
            void* p_mapping = mmap(reinterpret_cast<void*>(hint), cb_range, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p_mapping != MAP_FAILED) {
                return static_cast<u8*>(p_mapping);
            }
        }
    }
    return nullptr;
}

} // namespace

void PatchImage(unsigned char* p_image, std::uint64_t cb_image) {
    if (const char* sz_env = std::getenv("PS_GAME_MENU"); sz_env && sz_env[0] == '0') {
        return;
    }
    s_image_base = reinterpret_cast<u64>(p_image);
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    u8* p_stubs = MapNear(p_image, cb_image, page);
    if (!p_stubs) {
        std::printf("Game menu: no memory near the image, the port's pages are off\n");
        return;
    }
    DefineTexts();
    for (const Page& p : s_pages) {
        for (const Row& r : p.rows) {
            s_byte_field[r.field] = r.kind != Kind::List;
            s_steps_count[r.field] = r.kind == Kind::Steps ? int(r.choices.size()) : 0;
        }
    }
    static const PsSettings::Values s_defaults;
    Fill(s_defaults, s_default_values);
    for (int e = 0; e < PsEffects::SwitchCount; ++e) {
        s_default_values[FirstEffect + e] = PsEffects::Switches[e].is_default_on;
    }
    for (int f = 0; f < FieldCount; ++f) {
        s_default_bytes[f] =
            s_steps_count[f] ? ChoicePosition(s_default_values[f], s_steps_count[f]) : u8(s_default_values[f]);
    }
    u8* p_cursor = p_stubs;
    // The lookup first: the menu hook only acts once it has seen the Screen/Sound text.
    const bool is_ok =
        Detour(p_image, MsgLookup, MsgLookupPrologue, sizeof(MsgLookupPrologue),
               reinterpret_cast<void*>(&LookupHook), reinterpret_cast<void**>(&s_pfn_lookup_original), p_cursor) &&
        Detour(p_image, AddPageItem, AddPageItemPrologue, sizeof(AddPageItemPrologue),
               reinterpret_cast<void*>(&AddHook), reinterpret_cast<void**>(&s_pfn_add_original), p_cursor);
    mprotect(p_stubs, page, PROT_READ | PROT_EXEC);
    std::printf("Game menu: %s (%zu pages in System)\n", is_ok ? "the port's pages are in" : "hooks failed, off",
                s_pages.size());
}


void Poll() {
    if (!s_has_values) {
        return;
    }
    for (int f = 0; f < FieldCount; ++f) {
        if (s_byte_field[f]) {
            // A slider with choices: its position, not the choice's number.
            s_values[f] = s_steps_count[f] ? ChoiceFromPosition(s_bytes[f], s_steps_count[f]) : s_bytes[f];
        }
    }
    // The output resolution and the preset re-create the scene targets and the upscaler (a few
    // grey frames): a slider walking through its steps did that at each one. Applied once they
    // rest for 0.4 s; the other rows at once.
    using Clock = std::chrono::steady_clock;
    static int s_resting[2] = {-1, -1};
    static Clock::time_point s_rested_since{};
    const auto now = Clock::now();
    if (s_resting[0] != s_values[OutputRes] || s_resting[1] != s_values[Preset]) {
        s_resting[0] = s_values[OutputRes];
        s_resting[1] = s_values[Preset];
        s_rested_since = now;
    }
    if (now - s_rested_since < std::chrono::milliseconds(400)) {
        s_values[OutputRes] = s_applied[OutputRes];
        s_values[Preset] = s_applied[Preset];
    }
    if (std::memcmp(s_values, s_applied, sizeof(s_values)) == 0) {
        return;
    }
    auto& s = PsSettings::Get();
    s.output_res = std::clamp(s_values[OutputRes], 0, PsSettings::OutputCount - 1);
    const int upscaler = std::clamp(s_values[Upscaler], 0, PsSettings::UpscalerCount - 1);
    s.upscaler = UpscalerAvailable(upscaler) ? upscaler : int(PsSettings::UpscalerFsr3);
    s.preset = std::clamp(s_values[Preset], 0, PsSettings::PresetCount - 1);
    s.should_sharpen = s_values[Sharpness] != 0;
    if (s_values[Sharpness] != 0) {
        s.sharpness = std::clamp(s_values[Sharpness], 0, SharpnessSteps - 1) * SharpnessStep;
    }
    s.should_show_fps = s_values[ShowFps] != 0;
    s.model_lod = LodValues[std::clamp(s_values[ModelLod], 0, 3)];
    for (int e = 0; e < PsEffects::SwitchCount; ++e) {
        s.effects[e] = s_values[FirstEffect + e] != 0;
    }
    std::memcpy(s_applied, s_values, sizeof(s_values));
    PsSettings::Save();
}

} // namespace PsGameMenu
