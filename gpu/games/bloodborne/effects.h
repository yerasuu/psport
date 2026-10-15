// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne's effect switches (bbport.ini keys; the patches each one adds are in patches.cpp), for
// the overlay menu, its own System menu pages (game_menu.cpp) and the launcher.
#pragma once

#include <iterator>
#include "game_profile.h"

namespace BbEffects {
inline constexpr Game::EffectSwitch Switches[] = {
    {"effect_chromatic_aberration", {"Chromatic aberration", "Хроматическая аберрация", "Aberración cromática"}, true},
    {"effect_dof", {"Depth of field (DoF)", "Глубина резкости (DoF)", "Profundidad de campo (DoF)"}, true},
    {"effect_motion_blur", {"Motion blur", "Размытие в движении", "Desenfoque de movimiento"}, true},
    {"effect_ssao", {"Ambient occlusion (SSAO)", "Затенение SSAO", "Oclusión ambiental (SSAO)"}, true},
    {"effect_game_aa", {"Game's own anti-aliasing", "Собственное сглаживание игры", "Antialiasing propio del juego"}, true},
    {"effect_dynamic_shadows",
     {"Shadows from dynamic lights", "Тени от динамических источников", "Sombras de luces dinámicas"},
     true},
    {"effect_ssr",
     {"Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)",
      "Reflejos en espacio de pantalla (no estaban en el juego)"},
     false},
    {"skip_intro", {"Skip startup intros", "Пропуск заставок при запуске", "Saltar las intros al iniciar"}, false},
    {"debug_camera",
     {"Free camera (Cross + L3)", "Свободная камера (Cross + L3)", "Cámara libre (X + L3)"},
     false,
     {"Hold Cross and press L3 (keyboard: Space + Z).", "Удерживайте Cross и нажмите L3 (клавиатура: Space + Z).",
      "Mantén X y pulsa L3 (teclado: Espacio + Z)."}},
    {"debug_menu",
     {"Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)",
      "Menú de depuración (necesita archivos de fuentes)"},
     false,
     {"Left side of the touchpad (Tab); right side: Backspace. Needs the adhoc folder "
      "from Nexus mod #253 (the fonts in adhoc/font).",
      "Левая сторона тачпада (Tab), правая — Backspace. Нужна папка adhoc из мода "
      "Nexus #253 (шрифты в adhoc/font).",
      "Lado izquierdo del panel táctil (Tab); lado derecho: Retroceso. Necesita la carpeta adhoc "
      "del mod #253 de Nexus (las fuentes en adhoc/font)."}},
};
inline constexpr int SwitchCount = int(std::size(Switches));
} // namespace BbEffects
