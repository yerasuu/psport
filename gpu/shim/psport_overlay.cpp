// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "psport_settings.h"
#include "common/elf_info.h"
#include "game_profile.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden psport_font_ttf\n"
    ".global psport_font_ttf\n"
    "psport_font_ttf:\n"
    ".incbin \"" PS_FONT_PATH "\"\n"
    ".hidden psport_font_ttf_end\n"
    ".global psport_font_ttf_end\n"
    "psport_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char psport_font_ttf[];
extern "C" const unsigned char psport_font_ttf_end[];

extern "C" void runtime_restart(void); // psport (probe.c)

namespace PsOverlay {

namespace {

std::mutex s_imgui_mutex; // the ImGui context: window thread (input) and present thread
bool s_is_initialized = false;
std::atomic<bool> s_is_menu_open{false};
bool s_is_l3_down = false, s_is_r3_down = false;
bool s_is_dirty = false; // settings changed while open: saved on close
float s_base_scale = 1.0f;
// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex s_prompt_mutex;
std::atomic<bool> s_is_prompt_active{false};
std::string s_prompt_title, s_prompt_text;
// Guide (or Home) asks whether to close the game, as the console's PS button leads there. The game
// reads the pad's state rather than its events, so the key or button that cancelled stays away
// from it until released.
std::atomic<bool> s_is_close_prompt_open{false};
std::atomic<bool> s_is_release_pending{false};
SDL_Keycode s_release_key = SDLK_UNKNOWN;
int s_release_button = -1;

std::mutex s_cursor_mutex;
std::array<GameCursor, MaxGameCursors> s_game_cursors{};
std::atomic<bool> s_is_game_cursor_shown{false};

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point s_last_present{};
float s_frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool is_open) {
    if (s_is_menu_open.exchange(is_open) == is_open) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (is_open) {
        if (SDL_Window* p_window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(p_window));
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

void AnswerClosePrompt(bool should_quit) {
    s_is_close_prompt_open = false;
    std::printf("Overlay: close prompt %s\n", should_quit ? "confirmed, quitting" : "cancelled");
    if (should_quit) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* p_window = SDL_GetWindowFromID(id);
    const float density = p_window ? SDL_GetWindowPixelDensity(p_window) : 1.0f;
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
    Store(value, is_checked, ImGui::Checkbox(sz_label, &is_checked));
}

void Slider(const char* sz_label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(sz_label, &v, lo, hi, "%.2f"));
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
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
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
        PsSettings::Save();
    }
    ImGui::Text(PsSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)", "%.0f FPS  (%.1f ms)"),
                s_frame_ms_avg > 0.0f ? 1000.0f / s_frame_ms_avg : 0.0f, s_frame_ms_avg);

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
                const int output = s.output_res;
                std::snprintf(
                    sz_label, sizeof(sz_label),
                    PsSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)",
                                         "%s (x%.1f, render %dx%d)"),
                    PsSettings::PresetName(i), scale,
                    int(std::lround(PsSettings::OutputWidths[output] / scale / 2) * 2),
                    int(std::lround(PsSettings::OutputHeights[output] / scale / 2) * 2));
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

    ImGui::SeparatorText(PsSettings::MenuText("Output resolution", "Разрешение вывода",
                                              "Resolución de salida"));
    static const char* s_outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo(PsSettings::MenuText("Output resolution", "Разрешение вывода",
                                               "Resolución de salida"),
                          s_outputs[output])) {
        for (int i = 0; i < PsSettings::OutputCount; ++i) {
            if (ImGui::Selectable(s_outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (PsSettings::FixedRenderSession()) {
        Hint(PsSettings::MenuText(
            "Size of the final frame and UI. The preset sets the scene size relative to "
            "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
            "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
            "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры.",
            "Tamaño del fotograma final y de la interfaz. El preajuste fija el tamaño de la escena respecto a "
            "la salida: 4K Performance = 1920x1080. Se aplica tras reiniciar el juego."));
    } else {
        Hint(PsSettings::MenuText(
            "The final frame and UI size changes at the next frame boundary. "
            "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
            "Changing the size resets the upscaler's history and may cause a brief pause.",
            "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
            "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
            "Смена размера сбрасывает историю апскейлера и может вызвать короткую паузу.",
            "El tamaño del fotograma final y de la interfaz cambia en el siguiente fotograma. El preajuste "
            "fija el tamaño de la escena respecto a la salida: 4K Performance = 1920x1080. Cambiar el tamaño "
            "reinicia el historial del escalador y puede causar una pausa breve."));
    }
    if (const Game::Profile* p_launched = Game::Launched(); p_launched && p_launched->add_patches) {
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
        const Game::Profile* p_launched = Game::Launched();
        std::string note = PsSettings::MenuText("Effects are enabled and disabled by game patches at startup",
                                                "Эффекты включаются и выключаются патчами игры при запуске",
                                                "Los efectos se activan y desactivan con parches del juego al iniciar");
        if (p_launched->patch_file) {
            note = note + " (" + p_launched->patch_file + ")";
        }
        note += ".";
        if (effects.hint.sz_english) {
            note = note + " " + localized(effects.hint);
        }
        Hint(note.c_str());
    }

    bool should_restart =
        s.should_use_object_motion != s.is_object_motion_at_startup || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || PsSettings::ResolutionNeedsRestart();
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
    ImGui::Separator();
    ImGui::TextUnformatted(PsSettings::MenuText("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel",
                                                "Клавиатура: ввод, Backspace — удалить, Enter — OK, Esc — отмена",
                                                "Teclado: escribe, Retroceso = borrar, Intro = aceptar, Esc = cancelar"));
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

void UpdateTextInput(SDL_Window* p_window) {
    bool is_wanted = false;
    {
        std::scoped_lock lock{s_imgui_mutex};
        is_wanted = s_is_initialized && s_is_menu_open && ImGui::GetIO().WantTextInput;
    }
    if (is_wanted != SDL_TextInputActive(p_window)) {
        if (is_wanted) {
            SDL_StartTextInput(p_window);
        } else {
            SDL_StopTextInput(p_window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{s_imgui_mutex};
    if (!s_is_initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = s_is_menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool is_down = event.type == SDL_EVENT_KEY_DOWN;
        if (!is_down && s_is_release_pending && event.key.key == s_release_key) {
            s_is_release_pending = false;
            return true;
        }
        if (s_is_close_prompt_open) {
            if (is_down && !event.key.repeat) {
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                    AnswerClosePrompt(true);
                } else if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_HOME) {
                    AnswerClosePrompt(false);
                    s_release_key = event.key.key;
                    s_is_release_pending = true;
                }
            }
            return true;
        }
        if (is_down && !event.key.repeat && !is_open && event.key.key == SDLK_HOME) {
            OpenClosePrompt();
            return true;
        }
        if (is_down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, is_down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool is_down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            s_is_l3_down = is_down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            s_is_r3_down = is_down;
        }
        if (!is_down && s_is_release_pending && button == s_release_button) {
            s_is_release_pending = false;
            return true;
        }
        if (s_is_close_prompt_open) {
            if (is_down && button == SDL_GAMEPAD_BUTTON_SOUTH) {
                AnswerClosePrompt(true);
            } else if (is_down && (button == SDL_GAMEPAD_BUTTON_EAST || button == SDL_GAMEPAD_BUTTON_GUIDE)) {
                AnswerClosePrompt(false);
                s_release_button = button;
                s_is_release_pending = true;
            }
            return true;
        }
        if (is_down && button == SDL_GAMEPAD_BUTTON_GUIDE) {
            OpenClosePrompt();
            return true;
        }
        if (is_down && s_is_l3_down && s_is_r3_down) {
            SetOpen(!is_open);
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
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
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
           (s_is_menu_open || s_is_prompt_active || s_is_close_prompt_open || s_is_game_cursor_shown || PsSettings::Get().should_show_fps);
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

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return s_is_menu_open || s_is_prompt_active || s_is_close_prompt_open || s_is_release_pending;
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
    if (s_is_close_prompt_open) {
        ClosePrompt();
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
