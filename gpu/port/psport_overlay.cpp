// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../../src/platform/ps_host_window.h"
#include "psport_keyboard.h"
#include "psport_settings.h"
#include "common/elf_info.h"
#include "game_profile.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"

// DejaVu Sans (Cyrillic), embedded by gpu/platform/<os>/embedded_font.cpp.
extern "C" const unsigned char psport_font_ttf[];
extern "C" const unsigned char psport_font_ttf_end[];

extern "C" void runtime_restart(void); // psport (src/runtime/runtime_restart.c)
extern "C" const char* runtime_toypad_file(void);              // src/system/runtime_toypad.c
extern "C" void runtime_toypad_lights(uint32_t p_out_colours[3]); // src/system/runtime_toypad.c
extern "C" int runtime_pad_menu_pressed(void);                    // src/input/runtime_pad.c

namespace PsOverlay {

namespace {

std::mutex s_imgui_mutex; // the ImGui context: window thread (input) and present thread
std::atomic<bool> s_is_initialized{false};
std::atomic<bool> s_is_menu_open{false};
bool s_is_l3_down = false, s_is_r3_down = false;
bool s_is_dirty = false; // settings changed while open: saved on close
float s_base_scale = 1.0f;
// The game's text dialog (ImeDialog, the character name), typed on the keyboard or on the
// on-screen one with a controller: drawn while it is open. In fullscreen the window title that
// showed it is not visible (issues #17, #19).
std::mutex s_prompt_mutex;
std::atomic<bool> s_is_prompt_active{false};
std::string s_prompt_title, s_prompt_text;
// Guide (or Home) asks whether to close the game, as the console's PS button leads there. The game
// reads the pad's state rather than its events, so the key or button that cancelled stays away
// from it until released.
std::atomic<bool> s_is_close_prompt_open{false};
std::atomic<bool> s_is_release_pending{false};
u32 s_release_key = PS_HOST_WINDOW_KEYCODE_UNKNOWN;
int s_release_button = -1;

// The game's message dialog (libSceMsgDialog): up while it runs, so the game sees it running until
// the player answers, as on the console.
struct MessageButton {
    int kind;
    std::string label;
};
std::mutex s_message_mutex;
std::atomic<bool> s_is_message_open{false};
std::atomic<int> s_message_answer{0};
std::string s_message_text;
int s_message_system_id = -1;
std::vector<MessageButton> s_message_buttons;
int s_i_message_focus = 0;
bool s_can_message_cancel = false;

// A game module's line over the game (psport_gpu_overlay_banner), shown where the game's own
// picture is black.
std::mutex s_banner_mutex;
std::string s_banner_text;
std::atomic<bool> s_is_banner_shown{false};

std::mutex s_cursor_mutex;
std::array<GameCursor, MaxGameCursors> s_game_cursors{};
std::atomic<bool> s_is_game_cursor_shown{false};

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point s_last_present{};
float s_frame_ms_avg = 0.0f;

float PixelDensity(PsHostWindowId h_window);

void SetOpen(bool is_open) {
    if (s_is_menu_open.exchange(is_open) == is_open) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (is_open) {
        PsHostWindowId h_window = 0;
        float x = 0.0f, y = 0.0f;
        if (ps_host_window_mouse_focus(&h_window, &x, &y)) {
            const float density = PixelDensity(h_window);
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!is_open && s_is_dirty) {
        s_is_dirty = false;
        PsSettings::Save();
    }
}

void OpenClosePrompt() {
    SetOpen(false);
    s_is_close_prompt_open = true;
    std::printf("Overlay: close prompt open\n");
}

// The key or button that answered stays away from the game until released, as the close prompt's.
// The answer is checked and stored under the dialog's lock, so that one the game closed meanwhile
// gets none and the next one it opens is not answered with its button.
void AnswerMessage(bool is_cancel, u32 key, int button) {
    int answer = 0;
    {
        std::scoped_lock lock{s_message_mutex};
        if (!s_is_message_open || (is_cancel && !s_can_message_cancel) || (!is_cancel && s_message_buttons.empty())) {
            return;
        }
        answer = is_cancel ? -1 : 1 + s_i_message_focus;
        s_is_message_open = false;
        s_message_answer = answer;
    }
    s_release_key = key;
    s_release_button = button;
    s_is_release_pending = true;
    std::printf("Overlay: message dialog %s\n", answer < 0 ? "backed out of" : "answered");
}

void MoveMessageFocus(int step) {
    std::scoped_lock lock{s_message_mutex};
    const int c_buttons = int(s_message_buttons.size());
    if (c_buttons > 1) {
        s_i_message_focus = std::clamp(s_i_message_focus + step, 0, c_buttons - 1);
    }
}

void AnswerClosePrompt(bool should_quit) {
    s_is_close_prompt_open = false;
    std::printf("Overlay: close prompt %s\n", should_quit ? "confirmed, quitting" : "cancelled");
    if (should_quit) {
        ps_host_window_request_quit();
    }
}

ImGuiKey KeyFromKeycode(u32 key) {
    switch (key) {
    case PS_HOST_WINDOW_KEYCODE_TAB: return ImGuiKey_Tab;
    case PS_HOST_WINDOW_KEYCODE_LEFT: return ImGuiKey_LeftArrow;
    case PS_HOST_WINDOW_KEYCODE_RIGHT: return ImGuiKey_RightArrow;
    case PS_HOST_WINDOW_KEYCODE_UP: return ImGuiKey_UpArrow;
    case PS_HOST_WINDOW_KEYCODE_DOWN: return ImGuiKey_DownArrow;
    case PS_HOST_WINDOW_KEYCODE_PAGEUP: return ImGuiKey_PageUp;
    case PS_HOST_WINDOW_KEYCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case PS_HOST_WINDOW_KEYCODE_HOME: return ImGuiKey_Home;
    case PS_HOST_WINDOW_KEYCODE_END: return ImGuiKey_End;
    case PS_HOST_WINDOW_KEYCODE_DELETE: return ImGuiKey_Delete;
    case PS_HOST_WINDOW_KEYCODE_BACKSPACE: return ImGuiKey_Backspace;
    case PS_HOST_WINDOW_KEYCODE_SPACE: return ImGuiKey_Space;
    case PS_HOST_WINDOW_KEYCODE_RETURN: return ImGuiKey_Enter;
    case PS_HOST_WINDOW_KEYCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case PS_HOST_WINDOW_KEYCODE_ESCAPE: return ImGuiKey_Escape;
    case PS_HOST_WINDOW_KEYCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case PS_HOST_WINDOW_KEYCODE_RCTRL: return ImGuiKey_RightCtrl;
    case PS_HOST_WINDOW_KEYCODE_LSHIFT: return ImGuiKey_LeftShift;
    case PS_HOST_WINDOW_KEYCODE_RSHIFT: return ImGuiKey_RightShift;
    case PS_HOST_WINDOW_KEYCODE_LALT: return ImGuiKey_LeftAlt;
    case PS_HOST_WINDOW_KEYCODE_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case PS_HOST_PAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case PS_HOST_PAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case PS_HOST_PAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case PS_HOST_PAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case PS_HOST_PAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case PS_HOST_PAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case PS_HOST_PAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case PS_HOST_PAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case PS_HOST_PAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case PS_HOST_PAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case PS_HOST_PAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case PS_HOST_PAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(PsHostWindowId h_window) {
    const float density = ps_host_window_pixel_density(h_window);
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool is_changed) {
    if (is_changed) {
        target = value;
        s_is_dirty = true;
    }
}

void Checkbox(const char* sz_label, std::atomic<bool>& value) {
    bool is_checked = value;
    // The widget runs before Store reads is_checked: argument evaluation order is unspecified,
    // and with Clang the copy taken before the click lost the change.
    const bool is_changed = ImGui::Checkbox(sz_label, &is_checked);
    Store(value, is_checked, is_changed);
}

void Slider(const char* sz_label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool is_changed = ImGui::SliderFloat(sz_label, &v, lo, hi, "%.2f");
    Store(value, v, is_changed);
}

void Hint(const char* sz_text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(sz_text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The emulated USB toy pad: its figures, put on and lifted off from the menu with the gamepad. The
// runtime names the figures file once the game has initialized libSceUsbd with the pad connected,
// and reads it again when it changes; this section edits that file.
struct ToyPadFigure {
    int zone;
    unsigned id;
};
struct ToyPadName {
    unsigned id;
    std::string name;
};

std::vector<ToyPadFigure> ReadToyPad(const char* sz_path) {
    std::vector<ToyPadFigure> figures;
    FILE* p_file = std::fopen(sz_path, "r");
    if (!p_file) {
        return figures;
    }
    char sz_line[256];
    while (std::fgets(sz_line, sizeof(sz_line), p_file)) {
        if (char* sz_hash = std::strchr(sz_line, '#')) {
            *sz_hash = 0;
        }
        int zone;
        unsigned id;
        if (std::sscanf(sz_line, "%d %u", &zone, &id) == 2 && zone >= 1 && zone <= 3 && id) {
            figures.push_back({zone, id});
        }
    }
    std::fclose(p_file);
    return figures;
}

// Figure names: "<id> <name>" lines in figure_ids.txt next to the figures file (PS_TOYPAD_NAMES
// names another one); without it the ids are shown.
const std::vector<ToyPadName>& ToyPadNames(const char* sz_figures_path) {
    static std::vector<ToyPadName> s_names;
    static bool s_is_loaded;
    if (s_is_loaded) {
        return s_names;
    }
    s_is_loaded = true;
    std::string path;
    if (const char* sz_env = std::getenv("PS_TOYPAD_NAMES"); sz_env && *sz_env) {
        path = sz_env;
    } else {
        path = sz_figures_path;
        const size_t off_slash = path.rfind('/');
        path = (off_slash == std::string::npos ? std::string{} : path.substr(0, off_slash + 1)) + "figure_ids.txt";
    }
    FILE* p_file = std::fopen(path.c_str(), "r");
    if (!p_file) {
        std::printf("Overlay: toy pad: no figure names (%s), ids are shown\n", path.c_str());
        return s_names;
    }
    char sz_line[256];
    while (std::fgets(sz_line, sizeof(sz_line), p_file)) {
        unsigned id;
        int cb_used = 0;
        if (std::sscanf(sz_line, "%u %n", &id, &cb_used) == 1 && id) {
            std::string name = sz_line + cb_used;
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
                name.pop_back();
            }
            s_names.push_back({id, std::move(name)});
        }
    }
    std::fclose(p_file);
    return s_names;
}

std::string ToyPadLabel(const std::vector<ToyPadName>& names, unsigned id) {
    for (const auto& n : names) {
        if (n.id == id) {
            return n.name + " (" + std::to_string(id) + ")";
        }
    }
    return "#" + std::to_string(id);
}

// Written to a temporary file and renamed, so the runtime never reads half a list.
void WriteToyPad(const char* sz_path, const std::vector<ToyPadFigure>& figures,
                 const std::vector<ToyPadName>& names) {
    const std::string temp = std::string{sz_path} + ".tmp";
    FILE* p_file = std::fopen(temp.c_str(), "w");
    if (!p_file) {
        std::printf("Overlay: toy pad: cannot write %s\n", temp.c_str());
        return;
    }
    std::fputs("# Figures on the emulated toy pad: \"<zone> <figure id>\", one per line.\n"
               "# Zone 1: centre (1 figure), 2: left (3), 3: right (3). Also edited from the menu "
               "(Insert / L3+R3).\n",
               p_file);
    for (const auto& f : figures) {
        std::fprintf(p_file, "%d %u  # %s\n", f.zone, f.id, ToyPadLabel(names, f.id).c_str());
    }
    const bool is_written = std::fclose(p_file) == 0;
    if (!is_written || std::rename(temp.c_str(), sz_path) != 0) {
        std::printf("Overlay: toy pad: cannot replace %s\n", sz_path);
    }
}

void ToyPadSection() {
    const char* sz_path = runtime_toypad_file();
    if (!sz_path) {
        return;
    }
    const auto& names = ToyPadNames(sz_path);
    auto figures = ReadToyPad(sz_path);
    bool is_changed = false;
    u32 lights[3] = {};
    runtime_toypad_lights(lights);
    static unsigned s_pick_id = 1;
    ImGui::SeparatorText(PsSettings::MenuText("Toy pad", "Toy pad (фигурки)", "Toy pad (figuras)"));
    // Characters first, then vehicles and gadgets (ids from 1000).
    const std::string current = ToyPadLabel(names, s_pick_id);
    if (ImGui::BeginCombo(PsSettings::MenuText("Figure", "Фигурка", "Figura"), current.c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        for (int is_vehicles = 0; is_vehicles < 2; ++is_vehicles) {
            ImGui::SeparatorText(is_vehicles
                                     ? PsSettings::MenuText("Vehicles and gadgets", "Техника", "Vehículos y objetos")
                                     : PsSettings::MenuText("Characters", "Персонажи", "Personajes"));
            for (const auto& n : names) {
                if ((n.id >= 1000) != bool(is_vehicles)) {
                    continue;
                }
                const std::string label = ToyPadLabel(names, n.id);
                if (ImGui::Selectable(label.c_str(), n.id == s_pick_id)) {
                    s_pick_id = n.id;
                }
                if (n.id == s_pick_id && ImGui::IsWindowAppearing()) {
                    ImGui::SetScrollHereY();
                }
            }
        }
        ImGui::EndCombo();
    }
    // The zones in the pad's order (left, centre, right), each lit as the game lights it: the zone
    // it wants a figure on glows.
    static const int s_order[] = {2, 1, 3};
    static const char* s_zone_english[] = {"Centre", "Left", "Right"};
    static const char* s_zone_russian[] = {"Центр", "Левая", "Правая"};
    static const char* s_zone_spanish[] = {"Centro", "Izquierda", "Derecha"};
    for (const int zone : s_order) {
        ImGui::PushID(zone);
        const u32 c = lights[zone - 1];
        const ImVec4 colour = c ? ImVec4(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1.0f)
                                : ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(colour, "%s", c ? "●" : "○");
        ImGui::SameLine();
        ImGui::TextUnformatted(
            PsSettings::MenuText(s_zone_english[zone - 1], s_zone_russian[zone - 1], s_zone_spanish[zone - 1]));
        int c_in_zone = 0;
        for (size_t i = 0; i < figures.size(); ++i) {
            if (figures[i].zone != zone) {
                continue;
            }
            ++c_in_zone;
            ImGui::PushID(int(i));
            ImGui::SameLine();
            const std::string label = ToyPadLabel(names, figures[i].id) + "  ×";
            const bool is_lifted = ImGui::SmallButton(label.c_str());
            ImGui::PopID();
            if (is_lifted) {
                figures.erase(figures.begin() + long(i));
                is_changed = true;
                break;
            }
        }
        const bool is_full = c_in_zone >= (zone == 1 ? 1 : 3) || figures.size() >= 7;
        if (!is_full) {
            ImGui::SameLine();
            if (ImGui::SmallButton(PsSettings::MenuText("+ put here", "+ поставить сюда", "+ poner aquí"))) {
                figures.push_back({zone, s_pick_id});
                is_changed = true;
            }
        }
        ImGui::PopID();
    }
    Hint(PsSettings::MenuText(
        "Choose a figure, then \"+ put here\" on a zone; press a figure to lift it. The dot is the "
        "zone's light: when the game asks for a figure it lights the zone to put it on. If the "
        "game misses a figure, lift it and put it on again.",
        "Выберите фигурку и нажмите «+ поставить сюда» у зоны; нажатие на фигурку снимает её. "
        "Кружок — подсветка зоны: когда игра просит фигурку, она подсвечивает зону, куда её "
        "ставить. Если игра не заметила фигурку — снимите и поставьте снова.",
        "Elige una figura y pulsa «+ poner aquí» en una zona; pulsa una figura para quitarla. El "
        "punto es la luz de la zona: cuando el juego pide una figura, ilumina la zona donde ponerla. "
        "Si el juego no detecta una figura, quítala y vuelve a ponerla."));
    if (is_changed) {
        WriteToyPad(sz_path, figures, names);
    }
}

// The settings menu as in 0.3 and 0.4: one window (moved with the mouse, its place kept in
// psport.ini), sections one under the other, ImGui's own widgets and keyboard/gamepad navigation.
void Menu() {
    auto& s = PsSettings::Get();
    const ImGuiViewport* p_viewport = ImGui::GetMainViewport();
    // Where it was moved last (psport.ini menu_pos, a fraction of the screen), kept on screen.
    ImVec2 pos(p_viewport->WorkPos.x + 40.0f * s_base_scale, p_viewport->WorkPos.y + 40.0f * s_base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * s_base_scale;
        pos.x = p_viewport->WorkPos.x +
                std::clamp(s.menu_x * p_viewport->WorkSize.x, 0.0f, std::max(p_viewport->WorkSize.x - margin, 0.0f));
        pos.y = p_viewport->WorkPos.y +
                std::clamp(s.menu_y * p_viewport->WorkSize.y, 0.0f, std::max(p_viewport->WorkSize.y - margin, 0.0f));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * s_base_scale, 0.0f), ImGuiCond_Appearing);
    // Sized to its contents, the menu outgrew a 720p or 800p screen and its last row (Close, Quit)
    // was out of reach: it ends above the screen's bottom and scrolls instead.
    const float bottom_margin = 20.0f * s_base_scale;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0f, 0.0f),
        ImVec2(FLT_MAX, std::max(p_viewport->WorkPos.y + p_viewport->WorkSize.y - pos.y - bottom_margin, 200.0f * s_base_scale)));
    bool should_keep_open = true;
    static const std::string s_game_title = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        return title.empty() ? std::string("psport") : std::string(title);
    }();
    const std::string window_title = s_game_title +
                                     PsSettings::MenuText(" - Graphics", " — настройки", ": gráficos") +
                                     "  (Insert / L3+R3)###psport_settings";
    if (!ImGui::Begin(window_title.c_str(), &should_keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // Moved: remembered (saved with the settings when the menu closes).
    if (!ImGui::IsWindowAppearing() && p_viewport->WorkSize.x > 0.0f && p_viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - p_viewport->WorkPos.x) / p_viewport->WorkSize.x;
        const float fy = (at.y - p_viewport->WorkPos.y) / p_viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            s_is_dirty = true;
        }
    }
    // Issues #74, #116: leaving the game from the pad (this menu opens with L3+R3). A second
    // press confirms; the window thread then closes like the window's own close button.
    static bool s_is_quit_armed;
    if (ImGui::Button(s_is_quit_armed ? PsSettings::MenuText("Press again to quit###quit_game",
                                                        "Нажмите ещё раз для выхода###quit_game",
                                                        "Pulsa otra vez para salir###quit_game")
                                 : PsSettings::MenuText("Quit game###quit_game", "Выйти из игры###quit_game",
                                                        "Salir del juego###quit_game"))) {
        if (s_is_quit_armed) {
            ps_host_window_request_quit();
        }
        s_is_quit_armed = !s_is_quit_armed;
    }
    if (s_is_quit_armed && !ImGui::IsItemFocused() && !ImGui::IsItemHovered()) {
        s_is_quit_armed = false;
    }
    // Always readable, whatever language the rest of the menu is in.
    static const char* s_languages[] = {"English", "Русский", "Español"};
    int language = int(s.menu_language.load());
    if (ImGui::Combo("Language", &language, s_languages, 3)) {
        s.menu_language = PsSettings::MenuLanguage(language);
        s.is_menu_language_chosen = true;
        PsSettings::Save();
    }
    ImGui::Text(PsSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)", "%.0f FPS  (%.1f ms)"),
                s_frame_ms_avg > 0.0f ? 1000.0f / s_frame_ms_avg : 0.0f, s_frame_ms_avg);
    ToyPadSection();

    ImGui::SeparatorText(PsSettings::MenuText("Temporal upscaler", "Временной апскейлер",
                                              "Escalado temporal"));
    if (Vulkan::GetUpscalerPlacement().scene_combine_shader == 0) {
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(PsSettings::MenuText(
            "No game profile places the upscaler in this title's frame, so it stays off.",
            "Ни один игровой профиль не размещает апскейлер в кадре этой игры, "
            "поэтому он выключен.",
            "Ningún perfil de juego coloca el escalador en el fotograma de este título, "
            "así que está desactivado."));
        ImGui::PopTextWrapPos();
    } else {
        const char* upscalers[] = {
            PsSettings::MenuText("Off", "Выкл", "Desactivado"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1",
            PsSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)",
                                 "TAA (antialiasing nativo)"),
            "DLSS (NVIDIA RTX)"};
        static_assert(sizeof(upscalers) / sizeof(upscalers[0]) == PsSettings::UpscalerCount);
        static const char* s_later[] = {"XeSS"};
        int upscaler = s.upscaler;
        if (ImGui::BeginCombo(PsSettings::MenuText("Upscaler", "Апскейлер", "Escalador"), upscalers[upscaler])) {
            for (int i = 0; i < PsSettings::UpscalerCount; ++i) {
                const bool is_supported = i == PsSettings::UpscalerFsr4     ? s.is_fsr4_supported.load()
                                       : i == PsSettings::UpscalerFsr411 ? s.is_fsr411_supported.load()
                                       : i == PsSettings::UpscalerDlss   ? s.is_dlss_supported.load()
                                                                         : true;
                ImGui::BeginDisabled(!is_supported);
                if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                    Store(s.upscaler, i, true);
                }
                ImGui::EndDisabled();
                if (!is_supported) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", PsSettings::MenuText("— not supported by this GPU",
                                                             "— не поддерживается видеокартой",
                                                             "— no compatible con esta GPU"));
                }
            }
            for (const char* sz_name : s_later) {
                ImGui::BeginDisabled();
                ImGui::Selectable(sz_name, false);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", PsSettings::MenuText("— in development", "— в работе",
                                                               "— en desarrollo"));
            }
            ImGui::EndCombo();
        }
        if (s.upscaler == PsSettings::UpscalerDlss) {
            Hint(PsSettings::MenuText(
                "NVIDIA DLSS Super Resolution (RTX GPUs; Native AA is DLAA). Needs the DLSS bridge "
                "and NVIDIA's library next to the game; otherwise FSR 3.1 is used.",
                "NVIDIA DLSS Super Resolution (видеокарты RTX; Native AA — это DLAA). Нужны мост DLSS "
                "и библиотека NVIDIA рядом с игрой; без них используется FSR 3.1.",
                "NVIDIA DLSS Super Resolution (GPU RTX; Native AA es DLAA). Necesita el puente DLSS y la "
                "biblioteca de NVIDIA junto al juego; si no, se usa FSR 3.1."));
        }
        const auto problem_text = [](const char* sz_text) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", sz_text);
            ImGui::PopTextWrapPos();
        };
        if (const char* sz_problem = s.sz_dlss_problem.load(); sz_problem && s.upscaler == PsSettings::UpscalerDlss) {
            char sz_line[192];
            std::snprintf(sz_line, sizeof(sz_line), "DLSS: %s", sz_problem);
            problem_text(sz_line);
        }
        if (const char* sz_problem = s.sz_fsr4_problem.load()) {
            char sz_line[192];
            std::snprintf(sz_line, sizeof(sz_line),
                          PsSettings::MenuText("FSR 4 unavailable: %s", "FSR 4 недоступен: %s",
                                               "FSR 4 no disponible: %s"), sz_problem);
            problem_text(sz_line);
            if (!PsSettings::IsFsr4(s.upscaler)) {
                ImGui::PushTextWrapPos();
                ImGui::TextUnformatted(PsSettings::MenuText(
                    "The mode selected above is active. You can select FSR 4 again.",
                    "Активен режим, выбранный выше. FSR 4 можно выбрать снова.",
                    "Está activo el modo elegido arriba. Puedes volver a elegir FSR 4."));
                ImGui::PopTextWrapPos();
            }
        }
        if (PsSettings::IsFsr4(s.upscaler)) {
            if (s.upscaler == PsSettings::UpscalerFsr411) {
                Hint(PsSettings::MenuText(
                    "FSR 4.1.1: the model from AMD's 4.1.1 DLL, reproduced in Vulkan (output matches "
                    "the DLL). Assets are built from your DLL in the launcher.",
                    "FSR 4.1.1: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат совпадает "
                    "с DLL). Ассеты собираются из вашей DLL в лаунчере.",
                    "FSR 4.1.1: el modelo de la DLL 4.1.1 de AMD, reproducido en Vulkan (el resultado coincide "
                    "con la DLL). Los recursos se generan a partir de tu DLL en el lanzador."));
            } else {
                Hint(PsSettings::MenuText(
                    "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                    "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                    "model (a brief pause).",
                    "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                    "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                    "пауза).",
                    "FSR 4 en modo INT8 (modelo v07 de las fuentes del AMD FidelityFX SDK). Más calidad que FSR "
                    "3.1, pero la pasada es más exigente. Cambiar el preajuste reconstruye el modelo (una pausa "
                    "breve)."));
            }
            Checkbox(PsSettings::MenuText("FSR 4: auto exposure", "FSR 4: авто-экспозиция",
                                          "FSR 4: exposición automática"),
                     s.should_use_fsr4_auto_exposure);
            Checkbox(PsSettings::MenuText("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter",
                                          "FSR 4: invertir el signo del jitter"),
                     s.should_invert_fsr4_jitter);
            Hint(PsSettings::MenuText(
                "For diagnosing ghosting: the FSR 4 network normalizes color by exposure and uses "
                "it to decide when to discard previous frames. Changes apply immediately, without a "
                "restart.",
                "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
                "когда отбросить прошлые кадры. Меняются сразу, без перезапуска.",
                "Para diagnosticar el ghosting: la red de FSR 4 normaliza el color según la exposición y la usa "
                "para decidir cuándo descartar los fotogramas anteriores. Se aplica al momento, sin reiniciar."));
        }
        const bool is_upscaler_on = s.upscaler != PsSettings::UpscalerOff;
        const bool is_taa = s.upscaler == PsSettings::UpscalerTaa;
        ImGui::BeginDisabled(!is_upscaler_on);
        ImGui::BeginDisabled(is_taa);
        int preset = is_taa ? PsSettings::NativeAA : s.preset.load();
        char sz_preset_label[64];
        std::snprintf(sz_preset_label, sizeof(sz_preset_label), "%s (x%.1f)", PsSettings::PresetName(preset),
                      PsSettings::PresetScale(preset));
        if (ImGui::BeginCombo(PsSettings::MenuText("Preset", "Пресет", "Preajuste"), sz_preset_label)) {
            for (int i = 0; i < PsSettings::PresetCount; ++i) {
                char sz_label[64];
                const float scale = PsSettings::PresetScale(i);
                const PsSettings::OutputSize output = PsSettings::CurrentOutput();
                std::snprintf(
                    sz_label, sizeof(sz_label),
                    PsSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)",
                                         "%s (x%.1f, render %dx%d)"),
                    PsSettings::PresetName(i), scale, int(std::lround(output.width / scale / 2) * 2),
                    int(std::lround(output.height / scale / 2) * 2));
                if (ImGui::Selectable(sz_label, i == preset)) {
                    Store(s.preset, i, true);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (is_taa) {
            ImGui::TextWrapped("%s", PsSettings::MenuText(
                "TAA anti-aliases the scene at output resolution, without an FSR model or upscaling. "
                "Your saved FSR preset is restored when you select FSR.",
                "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. "
                "Сохранённый пресет FSR восстановится при выборе FSR.",
                "TAA suaviza la escena a la resolución de salida, sin modelo FSR ni escalado. Tu preajuste de FSR "
                "guardado vuelve cuando eliges FSR."));
        }
        ImGui::Text(
            PsSettings::MenuText("Active scene render: %d x %d", "Активный рендер сцены: %d x %d",
                                 "Render de la escena actual: %d x %d"),
            s.active_render_width.load(), s.active_render_height.load());
        if (PsSettings::FixedRenderSession()) {
            ImGui::Text(PsSettings::MenuText("Startup preset: %s", "Пресет при запуске: %s",
                                             "Preajuste al iniciar: %s"),
                        PsSettings::PresetName(s.startup_preset));
            if (const char* sz_automatic = std::getenv("PS_AUTO_RENDER_RES");
                sz_automatic && sz_automatic[0] == '1') {
                Hint(PsSettings::MenuText(
                    "When output is not 1080p, the entire game renders at the preset resolution "
                    "(startup patch): this is fastest on Steam Deck and weaker GPUs. Preset or output "
                    "resolution changes require a restart. Enable Live resolution changes below to "
                    "change them without restarting (post-processing then stays at 1080p, which is "
                    "slower).",
                    "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): "
                    "это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения "
                    "вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает "
                    "смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее).",
                    "Si la salida no es 1080p, todo el juego se dibuja a la resolución del preajuste (parche al "
                    "iniciar): es lo más rápido en Steam Deck y GPU modestas. Cambiar el preajuste o la "
                    "resolución de salida requiere reiniciar. Activa abajo «Cambio de resolución en vivo» para "
                    "cambiarlos sin reiniciar (el posprocesado se queda entonces en 1080p, más lento)."));
            } else {
                Hint(PsSettings::MenuText(
                    "PS_RENDER_RES fixes the scene size at startup. Remove this explicit environment "
                    "variable to change resolution and presets without restarting the game.",
                    "PS_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную "
                    "для смены разрешения и пресетов без перезапуска игры.",
                    "PS_RENDER_RES fija el tamaño de la escena al iniciar. Quita esta variable de entorno para "
                    "cambiar la resolución y los preajustes sin reiniciar el juego."));
            }
        } else {
            Hint(PsSettings::MenuText(
                "Native AA: the upscaler acts as anti-aliasing. Other presets reduce the scene render "
                "resolution relative to the output. The UI renders at output resolution. "
                "The preset applies from the next frame without restarting the game.",
                "Native AA: апскейлер работает как сглаживание. Остальные пресеты уменьшают разрешение "
                "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
                "Пресет применяется со следующего кадра без перезапуска игры.",
                "Native AA: el escalador hace de antialiasing. Los demás preajustes reducen la resolución de la "
                "escena respecto a la salida. La interfaz se dibuja a la resolución de salida. El preajuste se "
                "aplica desde el siguiente fotograma sin reiniciar el juego."));
        }
        Checkbox(PsSettings::MenuText("Sharpening (RCAS)", "Резкость (RCAS)", "Nitidez (RCAS)"), s.should_sharpen);
        ImGui::BeginDisabled(!s.should_sharpen);
        Slider(PsSettings::MenuText("Sharpness", "Сила резкости", "Intensidad de nitidez"), s.sharpness, 0.0f, 2.0f);
        Hint(PsSettings::MenuText(
            "Up to 1: the upscaler's own sharpening (RCAS). Above 1 adds another RCAS pass. "
            "Ctrl+click the slider to enter an exact value.",
            "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
            "Ctrl+клик по ползунку — ввести точное значение.",
            "Hasta 1: la nitidez del propio escalador (RCAS). Por encima de 1 se añade otra pasada RCAS. "
            "Ctrl+clic en el control para escribir un valor exacto."));
        ImGui::EndDisabled();
        Checkbox(PsSettings::MenuText("Subpixel jitter", "Субпиксельный сдвиг (jitter)",
                                      "Desplazamiento subpíxel (jitter)"), s.should_jitter);
        Hint(PsSettings::MenuText(
            "Each frame shifts the scene by a fraction of a pixel, letting the upscaler reconstruct "
            "more detail from multiple frames. Without it, only history-based anti-aliasing remains.",
            "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
            "кадров больше деталей. Без него получается только сглаживание по истории.",
            "Cada fotograma desplaza la escena una fracción de píxel, y el escalador reconstruye más detalle a "
            "partir de varios fotogramas. Sin él solo queda el antialiasing por historial."));

        ImGui::SeparatorText(PsSettings::MenuText("Reactive mask", "Маска реактивности", "Máscara reactiva"));
        ImGui::BeginDisabled(is_taa);
        Checkbox(PsSettings::MenuText("Enable mask", "Включить маску", "Activar máscara"), s.should_use_reactive);
        Hint(PsSettings::IsFsr4(s.upscaler)
                 ? PsSettings::MenuText(
                       "Marks transparent effects (haze, light, water, particles). FSR 4 takes no "
                       "mask itself: its output is blended with the current frame there, so the "
                       "effects do not drag previous frames along.",
                       "Помечает прозрачные эффекты (дымку, свет, воду, частицы). FSR 4 сам маску не "
                       "принимает: там его результат смешивается с текущим кадром, и эффекты не "
                       "тянут за собой прошлые кадры.",
                       "Marca los efectos transparentes (bruma, luz, agua, partículas). FSR 4 no acepta máscara: "
                       "ahí su resultado se mezcla con el fotograma actual, y los efectos no arrastran fotogramas "
                       "anteriores.")
                 : PsSettings::MenuText(
                       "Marks transparent effects (particles, haze) so the upscaler relies less on "
                       "previous frames. Reduces trails behind effects, but shimmering returns "
                       "underneath them.",
                       "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше "
                       "опирался на прошлые кадры. Меньше шлейфов за эффектами, но под ними "
                       "возвращается дрожание.",
                       "Marca los efectos transparentes (partículas, bruma) para que el escalador dependa menos "
                       "de los fotogramas anteriores. Menos estelas tras los efectos, pero bajo ellos vuelve el "
                       "parpadeo."));
        ImGui::BeginDisabled(!s.should_use_reactive);
        Slider(PsSettings::MenuText("Scale", "Масштаб", "Escala"), s.reactive_scale, 0.0f, 4.0f);
        Slider(PsSettings::MenuText("Threshold", "Порог", "Umbral"), s.reactive_threshold, 0.0f, 1.0f);
        Slider(PsSettings::MenuText("Maximum", "Максимум", "Máximo"), s.reactive_max, 0.0f, 1.0f);
        bool should_show_mask = s.debug_view == PsSettings::DebugReactive;
        if (ImGui::Checkbox(PsSettings::MenuText("Show mask (debug)", "Показать маску (отладка)",
                                                 "Mostrar máscara (depuración)"),
                            &should_show_mask)) {
            s.debug_view = should_show_mask ? PsSettings::DebugReactive : PsSettings::DebugNone;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        Checkbox(PsSettings::MenuText("Character motion vectors", "Векторы движения персонажей",
                                      "Vectores de movimiento de personajes"),
                 s.should_use_object_motion);
        Hint(PsSettings::MenuText(
            "Accurate vectors for animated objects: clothing and weapons break up less "
            "during movement. The static scene does not get an additional pass. "
            "Changes apply after restarting the game.",
            "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
            "при движении. Статичная сцена не получает дополнительный проход. "
            "Изменение применяется после перезапуска игры.",
            "Vectores exactos para los objetos animados: la ropa y las armas se deshacen menos en movimiento. La "
            "escena estática no recibe una pasada adicional. Se aplica tras reiniciar el juego."));
        bool should_show_motion = s.debug_view == PsSettings::DebugMotion;
        if (ImGui::Checkbox(PsSettings::MenuText("Show motion vectors (debug)",
                                                 "Показать векторы движения (отладка)",
                                                 "Mostrar vectores de movimiento (depuración)"),
                            &should_show_motion)) {
            s.debug_view = should_show_motion ? PsSettings::DebugMotion : PsSettings::DebugNone;
        }
        Hint(PsSettings::MenuText(
            "Red/green: horizontal/vertical motion (8 pixels = full brightness). "
            "Blue: the pixel received an accurate object vector, not just camera motion. "
            "The upscaler treats a moving object with no blue or red/green as "
            "stationary, causing trails.",
            "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
            "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
            "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
            "неподвижным, отсюда шлейф.",
            "Rojo/verde: movimiento horizontal/vertical (8 píxeles = brillo máximo). Azul: el píxel recibió un "
            "vector exacto del objeto, no solo el movimiento de la cámara. El escalador trata como inmóvil un "
            "objeto que se mueve sin azul ni rojo/verde, y de ahí las estelas."));
        ImGui::EndDisabled(); // upscaler off
    }

    // Offered only where the device has FSR 3.1 frame generation's prerequisites (decision #840).
    if (s.is_frame_gen_fsr_supported) {
        ImGui::SeparatorText(PsSettings::MenuText("Frame generation", "Генерация кадров", "Generación de fotogramas"));
        if (Vulkan::GetUpscalerPlacement().scene_combine_shader == 0) {
            ImGui::PushTextWrapPos();
            ImGui::TextUnformatted(PsSettings::MenuText(
                "It reads the scene's depth and motion where a game profile places the upscaler; no profile "
                "places it in this title's frame, so it stays off.",
                "Она читает глубину и движение сцены там, где игровой профиль размещает апскейлер; ни один "
                "профиль не размещает его в кадре этой игры, поэтому она выключена.",
                "Lee la profundidad y el movimiento de la escena donde un perfil de juego coloca el escalador; "
                "ningún perfil lo coloca en el fotograma de este título, así que está desactivada."));
            ImGui::PopTextWrapPos();
        } else {
            const char* frame_gens[] = {PsSettings::MenuText("Off", "Выкл", "Desactivado"), "FSR 3.1 (2x)"};
            static_assert(sizeof(frame_gens) / sizeof(frame_gens[0]) == PsSettings::FrameGenCount);
            const int chosen = s.frame_gen_chosen;
            if (ImGui::BeginCombo(PsSettings::MenuText("Frame generation##frame_gen", "Генерация кадров##frame_gen",
                                                       "Generación de fotogramas##frame_gen"),
                                  frame_gens[chosen])) {
                for (int i = 0; i < PsSettings::FrameGenCount; ++i) {
                    if (ImGui::Selectable(frame_gens[i], i == chosen) && i != chosen) {
                        PsSettings::ChooseFrameGen(i);
                        s_is_dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            Hint(PsSettings::MenuText(
                "One generated frame between two of the game's: smoother motion on a display with room for "
                "twice the game's frame rate. It adds input latency (about half a game frame) and works best "
                "from about 60 FPS. Applies after restarting the game.",
                "Один сгенерированный кадр между двумя кадрами игры: плавнее движение на дисплее, у которого "
                "есть запас на удвоенную частоту кадров игры. Добавляет задержку ввода (около половины кадра "
                "игры), лучше всего работает примерно от 60 FPS. Применяется после перезапуска игры.",
                "Un fotograma generado entre dos del juego: movimiento más fluido en una pantalla con margen "
                "para el doble de fotogramas del juego. Añade latencia de entrada (alrededor de medio fotograma "
                "del juego) y funciona mejor a partir de unos 60 FPS. Se aplica tras reiniciar el juego."));
            if (s.is_frame_gen_from_env) {
                ImGui::PushTextWrapPos();
                ImGui::TextDisabled("%s", PsSettings::MenuText(
                                              "Set by the launcher for this game (PS_FRAME_GEN): a change here is "
                                              "kept by the launcher for this game, not in psport.ini.",
                                              "Задано лаунчером для этой игры (PS_FRAME_GEN): изменение здесь "
                                              "лаунчер сохранит для этой игры, а не в psport.ini.",
                                              "Lo fija el lanzador para este juego (PS_FRAME_GEN): un cambio aquí "
                                              "lo guarda el lanzador para este juego, no psport.ini."));
                ImGui::PopTextWrapPos();
            }
            if (s.frame_gen != PsSettings::FrameGenOff) {
                ImGui::TextDisabled("%s", s.c_frame_gen_now > 0
                                              ? PsSettings::MenuText("Now: 2x", "Сейчас: 2x", "Ahora: 2x")
                                              : PsSettings::MenuText("Now: paused", "Сейчас: на паузе",
                                                                     "Ahora: en pausa"));
            } else if (const char* sz_problem = s.sz_frame_gen_problem.load()) {
                ImGui::PushTextWrapPos();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", sz_problem);
                ImGui::PopTextWrapPos();
            }
        }
    }

    ImGui::SeparatorText(PsSettings::MenuText("Output", "Вывод", "Salida"));
    // The aspect ratio first; its resolutions follow (another one keeps the height).
    const auto aspect_label = [](int aspect) {
        if (aspect != PsSettings::AspectAuto) {
            return std::string(PsSettings::AspectKey(aspect));
        }
        return std::string(PsSettings::MenuText("Auto", "Авто", "Automática")) + " (" +
               PsSettings::AspectKey(PsSettings::ResolvedAspect(aspect)) + ")";
    };
    const int aspect = s.aspect;
    if (ImGui::BeginCombo(PsSettings::MenuText("Aspect ratio", "Соотношение сторон", "Relación de aspecto"),
                          aspect_label(aspect).c_str())) {
        for (int a = 0; a < PsSettings::AspectCount; ++a) {
            if (ImGui::Selectable(aspect_label(a).c_str(), a == aspect)) {
                Store(s.output_res, PsSettings::PackSize(PsSettings::NearestOutput(a, PsSettings::CurrentOutput())),
                      true);
                Store(s.aspect, a, true);
            }
        }
        ImGui::EndCombo();
    }
    const auto size_label = [](PsSettings::OutputSize size) {
        return std::to_string(size.width) + " x " + std::to_string(size.height);
    };
    const PsSettings::OutputSize current = PsSettings::CurrentOutput();
    if (ImGui::BeginCombo(PsSettings::MenuText("Output resolution", "Разрешение вывода", "Resolución de salida"),
                          size_label(current).c_str())) {
        for (const PsSettings::OutputSize& size : PsSettings::Outputs(s.aspect)) {
            if (ImGui::Selectable(size_label(size).c_str(), size == current)) {
                Store(s.output_res, PsSettings::PackSize(size), true);
            }
        }
        ImGui::EndCombo();
    }
    if (const PsSettings::OutputSize chosen = PsSettings::CurrentOutput();
        chosen.width * 9 != chosen.height * 16 && !PsSettings::Get().is_aspect_followed) {
        Hint(PsSettings::MenuText(
            "This game keeps its 16:9 picture: another aspect ratio shows it in the middle of the output.",
            "Эта игра сохраняет картинку 16:9: при другом соотношении она показывается в середине кадра.",
            "Este juego mantiene su imagen 16:9: con otra relación se muestra en el centro de la salida."));
    }
    if (PsSettings::FixedRenderSession()) {
        Hint(PsSettings::MenuText(
            "Size of the final frame and UI. The preset sets the scene size relative to "
            "the output: 4K Performance = 1920x1080. Another aspect ratio widens the view (the UI keeps "
            "to the screen's edges). Applies after restarting the game.",
            "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
            "вывода: 4K Performance = 1920x1080. Другое соотношение сторон расширяет обзор (интерфейс "
            "у краёв экрана). Применяется после перезапуска игры.",
            "Tamaño del fotograma final y de la interfaz. El preajuste fija el tamaño de la escena respecto a "
            "la salida: 4K Performance = 1920x1080. Otra relación de aspecto amplía la vista (la interfaz "
            "queda en los bordes de la pantalla). Se aplica tras reiniciar el juego."));
    } else {
        Hint(PsSettings::MenuText(
            "The final frame and UI size changes at the next frame boundary. "
            "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
            "Another aspect ratio widens the view (the UI keeps to the screen's edges). "
            "Changing the size resets the upscaler's history and may cause a brief pause.",
            "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
            "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
            "Другое соотношение сторон расширяет обзор (интерфейс у краёв экрана). "
            "Смена размера сбрасывает историю апскейлера и может вызвать короткую паузу.",
            "El tamaño del fotograma final y de la interfaz cambia en el siguiente fotograma. El preajuste "
            "fija el tamaño de la escena respecto a la salida: 4K Performance = 1920x1080. Otra relación de "
            "aspecto amplía la vista (la interfaz queda en los bordes de la pantalla). Cambiar el tamaño "
            "reinicia el historial del escalador y puede causar una pausa breve."));
    }
    if (const Game::Title* p_launched = Game::Launched(); p_launched && p_launched->pfn_add_patches) {
        const char* live_modes[] = {PsSettings::MenuText("Auto (based on GPU)", "Авто (по видеокарте)",
                                                         "Automático (según la GPU)"),
                                    PsSettings::MenuText("Off (faster)", "Выключена (быстрее)",
                                                         "Desactivado (más rápido)"),
                                    PsSettings::MenuText("On", "Включена", "Activado")};
        int live = s.live_resolution + 1;
        if (ImGui::BeginCombo(
                PsSettings::MenuText("Live resolution changes", "Смена разрешения на лету",
                                     "Cambio de resolución en vivo"),
                live_modes[live])) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::Selectable(live_modes[i], i == live)) {
                    Store(s.live_resolution, i - 1, true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(PsSettings::MenuText(
            "On: output resolution and preset change without restarting, but game post-processing "
            "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
            "Off: everything renders at the preset resolution, and changes require a restart. Auto "
            "enables this on powerful discrete GPUs. Applies after restarting the game.",
            "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
            "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
            "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
            "её на мощных дискретных видеокартах. Применяется после перезапуска игры.",
            "Activado: la resolución de salida y el preajuste cambian sin reiniciar, pero el posprocesado del "
            "juego se queda en 1080p, bastante más lento en Steam Deck y GPU antiguas. Desactivado: todo se "
            "dibuja a la resolución del preajuste y los cambios requieren reiniciar. Automático lo activa en GPU "
            "dedicadas potentes. Se aplica tras reiniciar el juego."));
    }
    const auto localized = [](const Game::LocalizedText& text) {
        return PsSettings::MenuText(text.sz_english, text.sz_russian, text.sz_spanish);
    };
    const Game::GameEffects effects = Game::Effects();
    if (s.c_effects > 0 || effects.has_model_detail) {
        ImGui::SeparatorText(PsSettings::MenuText("Game effects (restart required)",
                                                  "Эффекты игры (после перезапуска)",
                                                  "Efectos del juego (requiere reiniciar)"));
    }
    if (effects.has_model_detail) {
        const char* lods[] = {PsSettings::MenuText("Highest (-2)", "Максимальная (-2)", "Máximo (-2)"),
                              PsSettings::MenuText("Game default", "Как в игре", "El del juego"),
                              PsSettings::MenuText("Lower (1)", "Ниже (1)", "Menor (1)"),
                              PsSettings::MenuText("Lowest (2)", "Минимальная (2)", "Mínimo (2)")};
        static constexpr int s_lod_values[] = {-2, 0, 1, 2};
        int i_lod = 1;
        for (int i = 0; i < 4; ++i) {
            if (s_lod_values[i] == s.model_lod) i_lod = i;
        }
        if (ImGui::BeginCombo(PsSettings::MenuText("Model detail", "Детализация моделей",
                                                   "Detalle de los modelos"),
                              lods[i_lod])) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(lods[i], i == i_lod)) {
                    Store(s.model_lod, s_lod_values[i], true);
                }
            }
            ImGui::EndCombo();
        }
    }
    for (int e = 0; e < s.c_effects; ++e) {
        const Game::EffectSwitch& effect = effects.switches[e];
        Checkbox(localized(effect.label), s.effects[e]);
        if (effect.hint.sz_english) {
            Hint(localized(effect.hint));
        }
    }
    if (s.c_effects > 0) {
        const Game::Title* p_launched = Game::Launched();
        std::string note = PsSettings::MenuText("Effects are enabled and disabled by game patches at startup",
                                                "Эффекты включаются и выключаются патчами игры при запуске",
                                                "Los efectos se activan y desactivan con parches del juego al iniciar");
        if (p_launched->sz_patch_file) {
            note = note + " (" + p_launched->sz_patch_file + ")";
        }
        note += ".";
        if (effects.hint.sz_english) {
            note = note + " " + localized(effects.hint);
        }
        Hint(note.c_str());
    }

    bool should_restart =
        s.should_use_object_motion != s.is_object_motion_at_startup || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || s.frame_gen_chosen != s.startup_frame_gen ||
        PsSettings::ResolutionNeedsRestart();
    for (int e = 0; e < s.c_effects; ++e) {
        should_restart |= s.effects[e] != s.startup_effects[e];
    }
    if (should_restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           PsSettings::MenuText("Changes apply after restarting the game",
                                                "Изменения применятся после перезапуска игры",
                                                "Los cambios se aplicarán tras reiniciar el juego"));
        if (ImGui::Button(
                PsSettings::MenuText("Apply and restart game", "Применить и перезапустить игру",
                                     "Aplicar y reiniciar el juego"))) {
            PsSettings::Save();
            PsSettings::PrepareRestart();
            runtime_restart();
        }
    }

    ImGui::SeparatorText(PsSettings::MenuText("Other", "Прочее", "Otros"));
    Checkbox(PsSettings::MenuText("FPS counter in corner", "Счётчик FPS в углу",
                                  "Contador de FPS en la esquina"), s.should_show_fps);

    ImGui::Spacing();
    if (ImGui::Button(PsSettings::MenuText("Close", "Закрыть", "Cerrar"))) {
        should_keep_open = false;
    }
    ImGui::TextDisabled("%s", PsSettings::MenuText("Settings are saved to psport.ini",
                                             "Настройки сохраняются в psport.ini",
                                             "Los ajustes se guardan en psport.ini"));
    ImGui::End();
    if (!should_keep_open) {
        SetOpen(false);
    }
}

// Drawn behind the menu's windows, so the menu stays readable over the cursor.
void GameCursors(const vk::Rect2D& frame) {
    std::scoped_lock lock{s_cursor_mutex};
    ImDrawList* p_draw = ImGui::GetBackgroundDrawList();
    for (const GameCursor& cursor : s_game_cursors) {
        if (!cursor.is_enabled || !cursor.width || !cursor.height) {
            continue;
        }
        const float scale_x = float(frame.extent.width) / float(cursor.width);
        const float scale_y = float(frame.extent.height) / float(cursor.height);
        const float sx = cursor.is_magnified ? scale_x * 2.0f : scale_x;
        const float sy = cursor.is_magnified ? scale_y * 2.0f : scale_y;
        const float left = float(frame.offset.x) + float(cursor.x) * scale_x - float(cursor.hot_x) * sx;
        const float top = float(frame.offset.y) + float(cursor.y) * scale_y - float(cursor.hot_y) * sy;
        for (u32 row = 0; row < CursorImageSize; ++row) {
            const u32* p_pixels = cursor.p_image + row * CursorImageSize;
            for (u32 column = 0; column < CursorImageSize;) {
                const u32 argb = p_pixels[column];
                u32 end = column + 1;
                while (end < CursorImageSize && p_pixels[end] == argb) {
                    ++end;
                }
                if (argb >> 24) {
                    p_draw->AddRectFilled(ImVec2(left + float(column) * sx, top + float(row) * sy),
                                        ImVec2(left + float(end) * sx, top + float(row + 1) * sy),
                                        IM_COL32((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, argb >> 24));
                }
                column = end;
            }
        }
    }
}

void FpsCounter() {
    const ImGuiViewport* p_viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * s_base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(p_viewport->WorkPos.x + p_viewport->WorkSize.x - pad, p_viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = PsSettings::Get();
    ImGui::Text(PsSettings::MenuText("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s",
                                     "%.0f FPS  %.1f ms  %s"),
                s_frame_ms_avg > 0.0f ? 1000.0f / s_frame_ms_avg : 0.0f, s_frame_ms_avg,
                s.upscaler == PsSettings::UpscalerFsr3     ? "FSR 3.1"
                : s.upscaler == PsSettings::UpscalerFsr4   ? "FSR 4"
                : s.upscaler == PsSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == PsSettings::UpscalerTaa    ? "TAA"
                : s.upscaler == PsSettings::UpscalerDlss   ? "DLSS"
                                                           : "");
    // With frame generation the presented rate is the game's times two while the GPU keeps up:
    // the game's own rate is what resolution and upscaler change.
    if (s.frame_gen != PsSettings::FrameGenOff) {
        ImGui::Text(s.c_frame_gen_now > 0
                        ? PsSettings::MenuText("game %.0f, frame gen 2x", "игра %.0f, генерация кадров 2x",
                                               "juego %.0f, generación de fotogramas 2x")
                        : PsSettings::MenuText("game %.0f, frame gen paused", "игра %.0f, генерация кадров на паузе",
                                               "juego %.0f, generación de fotogramas en pausa"),
                    double(s.game_fps.load(std::memory_order_relaxed)));
    }
    ImGui::End();
}

void ClosePrompt() {
    const ImGuiViewport* p_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(p_viewport->WorkPos.x + p_viewport->WorkSize.x * 0.5f,
                                   p_viewport->WorkPos.y + p_viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##closeprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(PsSettings::MenuText("Close the game?", "Закрыть игру?", "¿Cerrar el juego?"));
    ImGui::Separator();
    ImGui::TextUnformatted(PsSettings::MenuText("Cross / Enter: close     Circle / Esc: cancel",
                                                "Крест / Enter — закрыть     Круг / Esc — отмена",
                                                "X / Intro: cerrar     Círculo / Esc: cancelar"));
    ImGui::End();
}

void Banner() {
    std::string text;
    {
        std::scoped_lock lock{s_banner_mutex};
        text = s_banner_text;
    }
    const ImGuiViewport* p_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(p_viewport->WorkPos.x + p_viewport->WorkSize.x * 0.5f,
                                   p_viewport->WorkPos.y + p_viewport->WorkSize.y * 0.82f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("##banner", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{s_prompt_mutex};
        title = s_prompt_title;
        text = s_prompt_text;
    }
    const ImGuiViewport* p_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(p_viewport->WorkPos.x + p_viewport->WorkSize.x * 0.5f,
                                   p_viewport->WorkPos.y + p_viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    DrawKeyboard(text);
    ImGui::Separator();
    ImGui::TextUnformatted(PsSettings::MenuText("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel",
                                                "Клавиатура: ввод, Backspace — удалить, Enter — OK, Esc — отмена",
                                                "Teclado: escribe, Retroceso = borrar, Intro = aceptar, Esc = cancelar"));
    ImGui::TextUnformatted(PsSettings::MenuText(
        "Controller: d-pad or left stick = move, Cross = press, Options = OK, Square = delete",
        "Геймпад: крестовина или левый стик — выбор, крест — нажать, Options — OK, квадрат — удалить",
        "Mando: cruceta o stick izquierdo = mover, Cruz = pulsar, Options = aceptar, Cuadrado = borrar"));
    ImGui::TextUnformatted(PsSettings::MenuText(
        "Circle = delete, or cancel when empty, Triangle = space, L2 = shift, R2 = symbols",
        "круг — удалить или отмена, если пусто, треугольник — пробел, L2 — регистр, R2 — символы",
        "Círculo = borrar, o cancelar si está vacío, Triángulo = espacio, L2 = mayúsculas, R2 = símbolos"));
    ImGui::End();
}

const char* MessageButtonLabel(const MessageButton& button) {
    switch (button.kind) {
    case PS_MESSAGE_BUTTON_OK: return PsSettings::MenuText("OK", "OK", "Aceptar");
    case PS_MESSAGE_BUTTON_YES: return PsSettings::MenuText("Yes", "Да", "Sí");
    case PS_MESSAGE_BUTTON_NO: return PsSettings::MenuText("No", "Нет", "No");
    case PS_MESSAGE_BUTTON_CANCEL: return PsSettings::MenuText("Cancel", "Отмена", "Cancelar");
    default: return button.label.c_str();
    }
}

void MessagePrompt() {
    std::scoped_lock lock{s_message_mutex};
    const ImGuiViewport* p_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(p_viewport->WorkPos.x + p_viewport->WorkSize.x * 0.5f,
                                   p_viewport->WorkPos.y + p_viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##messageprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushTextWrapPos(p_viewport->WorkSize.x * 0.5f);
    if (s_message_system_id >= 0) {
        ImGui::Text(PsSettings::MenuText("System message %d", "Системное сообщение %d", "Mensaje del sistema %d"),
                    s_message_system_id);
    } else {
        ImGui::TextUnformatted(s_message_text.c_str());
    }
    ImGui::PopTextWrapPos();
    if (!s_message_buttons.empty()) {
        ImGui::Separator();
        for (std::size_t i = 0; i < s_message_buttons.size(); ++i) {
            if (i) {
                ImGui::SameLine();
            }
            ImGui::Text(int(i) == s_i_message_focus ? "[ %s ]" : "  %s  ", MessageButtonLabel(s_message_buttons[i]));
        }
        ImGui::Separator();
        std::string hint =
            PsSettings::MenuText("Cross / Enter: choose", "Крест / Enter — выбрать", "X / Intro: elegir");
        if (s_message_buttons.size() > 1) {
            hint = std::string(PsSettings::MenuText("Left / Right: select", "Влево / Вправо — выбор",
                                                    "Izquierda / Derecha: seleccionar")) +
                   "     " + hint;
        }
        if (s_can_message_cancel) {
            hint += std::string("     ") +
                    PsSettings::MenuText("Circle / Esc: cancel", "Круг / Esc — отмена", "Círculo / Esc: cancelar");
        }
        ImGui::TextUnformatted(hint.c_str());
    }
    ImGui::End();
}

/// PS_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1,
/// consumed when the file appears (it is removed), one key press per frame.
void ScriptedKeys() {
    static const char* s_sz_path = std::getenv("PS_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point s_last_check{};
    static std::vector<std::string> s_queue;
    static bool s_should_release = false;
    static ImGuiKey s_held = ImGuiKey_None;
    if (!s_sz_path) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (s_should_release) {
        io.AddKeyEvent(s_held, false);
        s_should_release = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (s_queue.empty() && now - s_last_check > std::chrono::milliseconds(100)) {
        s_last_check = now;
        if (FILE* p_file = std::fopen(s_sz_path, "r")) {
            char sz_token[32];
            while (std::fscanf(p_file, "%31s", sz_token) == 1) {
                s_queue.emplace_back(sz_token);
            }
            std::fclose(p_file);
            std::remove(s_sz_path);
        }
    }
    if (s_queue.empty()) {
        return;
    }
    const std::string token = s_queue.front();
    s_queue.erase(s_queue.begin());
    if (token == "toggle") {
        SetOpen(!s_is_menu_open);
        return;
    }
    s_held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (s_held != ImGuiKey_None) {
        io.AddKeyEvent(s_held, true);
        s_should_release = true;
    }
}

} // namespace

bool OpenMessage(const std::string& text, int system_id, std::span<const PsMessageButton> buttons, int i_focus,
                 bool can_cancel) {
    if (!s_is_initialized) {
        return false;
    }
    std::scoped_lock lock{s_message_mutex};
    s_message_text = text;
    s_message_system_id = system_id;
    s_message_buttons.clear();
    s_message_buttons.reserve(buttons.size());
    for (const PsMessageButton& button : buttons) {
        s_message_buttons.push_back({button.kind, button.sz_label ? button.sz_label : ""});
    }
    s_i_message_focus = buttons.empty() ? 0 : std::clamp(i_focus, 0, int(buttons.size()) - 1);
    s_can_message_cancel = can_cancel;
    s_message_answer = 0;
    s_is_message_open = true;
    return true;
}

int PollMessage() {
    return s_message_answer;
}

void CloseMessage() {
    std::scoped_lock lock{s_message_mutex};
    s_is_message_open = false;
}

void SetTextPrompt(bool is_active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{s_prompt_mutex};
        s_prompt_title = prompt;
        s_prompt_text = text;
    }
    s_is_prompt_active = is_active;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 c_images) {
    std::scoped_lock lock{s_imgui_mutex};
    if (s_is_initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "psport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(psport_font_ttf),
                                   int(psport_font_ttf_end - psport_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* sz_name, void* p_user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(p_user), sz_name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(c_images, 2u);
    info.ImageCount = std::max(c_images, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    s_is_initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(PsHostWindow* p_window) {
    bool is_wanted = false;
    {
        std::scoped_lock lock{s_imgui_mutex};
        is_wanted = s_is_initialized && s_is_menu_open && ImGui::GetIO().WantTextInput;
    }
    if (is_wanted != ps_host_window_is_text_input(p_window)) {
        ps_host_window_text_input(p_window, is_wanted);
    }
}

bool HandleEvent(const PsHostWindowEvent& event) {
    const bool is_input_event =
        event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN || event.kind == PS_HOST_WINDOW_EVENT_KEY_UP ||
        event.kind == PS_HOST_WINDOW_EVENT_PAD_BUTTON_DOWN || event.kind == PS_HOST_WINDOW_EVENT_PAD_BUTTON_UP;
    // Asked before taking the ImGui lock because the runtime takes its pad lock before this one.
    const bool is_menu_pressed = is_input_event && runtime_pad_menu_pressed() != 0;
    std::scoped_lock lock{s_imgui_mutex};
    if (!s_is_initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = s_is_menu_open;
    switch (event.kind) {
    case PS_HOST_WINDOW_EVENT_KEY_DOWN:
    case PS_HOST_WINDOW_EVENT_KEY_UP: {
        const bool is_down = event.kind == PS_HOST_WINDOW_EVENT_KEY_DOWN;
        const u32 keycode = event.key.keycode;
        if (!is_down && s_is_release_pending && keycode == s_release_key) {
            s_is_release_pending = false;
            return true;
        }
        if (s_is_close_prompt_open) {
            if (is_down && !event.key.is_repeat) {
                if (keycode == PS_HOST_WINDOW_KEYCODE_RETURN || keycode == PS_HOST_WINDOW_KEYCODE_KP_ENTER) {
                    AnswerClosePrompt(true);
                } else if (keycode == PS_HOST_WINDOW_KEYCODE_ESCAPE || keycode == PS_HOST_WINDOW_KEYCODE_HOME) {
                    AnswerClosePrompt(false);
                    s_release_key = keycode;
                    s_is_release_pending = true;
                }
            }
            return true;
        }
        if (is_down && !event.key.is_repeat && !is_open && keycode == PS_HOST_WINDOW_KEYCODE_HOME) {
            OpenClosePrompt();
            return true;
        }
        if (is_menu_pressed) {
            SetOpen(!is_open);
            return true;
        }
        if (is_down && !event.key.is_repeat &&
            (keycode == PS_HOST_WINDOW_KEYCODE_INSERT || (is_open && keycode == PS_HOST_WINDOW_KEYCODE_ESCAPE))) {
            SetOpen(keycode == PS_HOST_WINDOW_KEYCODE_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open && s_is_message_open) {
            if (is_down && !event.key.is_repeat) {
                if (keycode == PS_HOST_WINDOW_KEYCODE_RETURN || keycode == PS_HOST_WINDOW_KEYCODE_KP_ENTER) {
                    AnswerMessage(false, keycode, -1);
                } else if (keycode == PS_HOST_WINDOW_KEYCODE_ESCAPE) {
                    AnswerMessage(true, keycode, -1);
                } else if (keycode == PS_HOST_WINDOW_KEYCODE_LEFT || keycode == PS_HOST_WINDOW_KEYCODE_RIGHT) {
                    MoveMessageFocus(keycode == PS_HOST_WINDOW_KEYCODE_LEFT ? -1 : 1);
                }
            }
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & PS_HOST_WINDOW_MOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & PS_HOST_WINDOW_MOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & PS_HOST_WINDOW_MOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromKeycode(keycode); key != ImGuiKey_None) {
            io.AddKeyEvent(key, is_down);
        }
        return true;
    }
    case PS_HOST_WINDOW_EVENT_PAD_BUTTON_DOWN:
    case PS_HOST_WINDOW_EVENT_PAD_BUTTON_UP: {
        const bool is_down = event.kind == PS_HOST_WINDOW_EVENT_PAD_BUTTON_DOWN;
        const u8 button = u8(event.pad_button.button);
        if (button == PS_HOST_PAD_BUTTON_LEFT_STICK) {
            s_is_l3_down = is_down;
        } else if (button == PS_HOST_PAD_BUTTON_RIGHT_STICK) {
            s_is_r3_down = is_down;
        }
        if (!is_down && s_is_release_pending && button == s_release_button) {
            s_is_release_pending = false;
            return true;
        }
        if (s_is_close_prompt_open) {
            if (is_down && button == PS_HOST_PAD_BUTTON_SOUTH) {
                AnswerClosePrompt(true);
            } else if (is_down && (button == PS_HOST_PAD_BUTTON_EAST || button == PS_HOST_PAD_BUTTON_GUIDE)) {
                AnswerClosePrompt(false);
                s_release_button = button;
                s_is_release_pending = true;
            }
            return true;
        }
        if (is_down && button == PS_HOST_PAD_BUTTON_GUIDE) {
            OpenClosePrompt();
            return true;
        }
        if (is_down && ((s_is_l3_down && s_is_r3_down) || is_menu_pressed)) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open && s_is_message_open) {
            if (is_down && button == PS_HOST_PAD_BUTTON_SOUTH) {
                AnswerMessage(false, PS_HOST_WINDOW_KEYCODE_UNKNOWN, button);
            } else if (is_down && button == PS_HOST_PAD_BUTTON_EAST) {
                AnswerMessage(true, PS_HOST_WINDOW_KEYCODE_UNKNOWN, button);
            } else if (is_down && (button == PS_HOST_PAD_BUTTON_DPAD_LEFT || button == PS_HOST_PAD_BUTTON_DPAD_RIGHT)) {
                MoveMessageFocus(button == PS_HOST_PAD_BUTTON_DPAD_LEFT ? -1 : 1);
            }
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, is_down);
        }
        return true;
    }
    case PS_HOST_WINDOW_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. The host sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.sz_utf8);
        return true;
    }
    case PS_HOST_WINDOW_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.h_window);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN:
    case PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == PS_HOST_INPUT_MOUSE_LEFT    ? 0
                           : event.button.button == PS_HOST_INPUT_MOUSE_RIGHT  ? 1
                           : event.button.button == PS_HOST_INPUT_MOUSE_MIDDLE ? 2
                                                                               : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.kind == PS_HOST_WINDOW_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case PS_HOST_WINDOW_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return s_is_initialized &&
           (s_is_menu_open || s_is_prompt_active || s_is_close_prompt_open || s_is_message_open ||
            s_is_game_cursor_shown || s_is_banner_shown || PsSettings::Get().should_show_fps);
}

void SetBanner(const char* sz_text) {
    {
        std::scoped_lock lock{s_banner_mutex};
        s_banner_text = sz_text ? sz_text : "";
    }
    s_is_banner_shown = sz_text && sz_text[0];
}

void SetGameCursor(u32 i_cursor, const GameCursor& cursor) {
    std::scoped_lock lock{s_cursor_mutex};
    s_game_cursors[i_cursor] = cursor;
    s_is_game_cursor_shown = std::any_of(s_game_cursors.begin(), s_game_cursors.end(),
                                    [](const GameCursor& c) { return c.is_enabled; });
}

bool GameCursorShown() {
    return s_is_game_cursor_shown;
}

bool MenuOpen() {
    return s_is_menu_open;
}

bool ClosePromptOpen() {
    return s_is_close_prompt_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return s_is_menu_open || s_is_prompt_active || s_is_close_prompt_open || s_is_message_open ||
           s_is_release_pending;
}

bool PadFilePress(bool is_guide_pressed, bool is_cross_pressed, bool is_circle_pressed) {
    std::scoped_lock lock{s_imgui_mutex};
    if (!s_is_initialized) {
        return false;
    }
    if (!s_is_close_prompt_open) {
        if (is_guide_pressed) {
            OpenClosePrompt();
        }
        return is_guide_pressed;
    }
    if (is_guide_pressed || is_cross_pressed || is_circle_pressed) {
        AnswerClosePrompt(is_cross_pressed);
    }
    return true;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent, vk::Rect2D frame) {

    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - s_last_present).count();
    s_last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        s_frame_ms_avg = s_frame_ms_avg == 0.0f ? ms : s_frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (std::getenv("PS_MENU_KEYS_FILE") && s_is_initialized) {
        std::scoped_lock lock{s_imgui_mutex};
        ScriptedKeys();
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{s_imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - s_base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / s_base_scale);
        style.FontScaleMain = scale;
        s_base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    GameCursors(frame);
    if (s_is_menu_open) {
        Menu();
    }
    if (PsSettings::Get().should_show_fps && !s_is_menu_open) {
        FpsCounter();
    }
    if (s_is_prompt_active && !s_is_menu_open) {
        TextPrompt();
    }
    if (s_is_message_open && !s_is_menu_open) {
        MessagePrompt();
    }
    if (s_is_close_prompt_open) {
        ClosePrompt();
    }
    if (s_is_banner_shown) {
        Banner();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace PsOverlay
