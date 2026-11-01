// SPDX-License-Identifier: GPL-2.0-or-later
/* The titles psport has been seen to run, the one source of their state: psport --list-compat prints
 * it (cli.c) and tools/games_table.py writes the title tables of README*.md and docs/<lang>/GAMES.md
 * from it (--check fails while they differ). The includer defines the macro:
 *
 *   TITLE(title_id, sz_name, sz_version, state, sz_note_en, sz_note_es, sz_note_ru)
 *       title_id the CUSA id; sz_version the version tested, "" when any; state one of playable,
 *       experimental, broken, unsupported (docs/en/GAMES.md, States); the notes GAMES.md's Detail
 *       column per language, Markdown, with string literals only (adjacent ones joined). */

TITLE(CUSA03173, "Bloodborne", "1.09", playable,
      "Saves, several areas, sound, pad, keyboard and mouse (the key prompts are drawn over the pad's), frame-rate presets 30, 60, 90 and uncapped, 3840x2160 output (about 80 FPS in the test scene on an RX 7900 XT). Online play through the title's community servers (`PS_ONLINE=1`) works, with occasional stutters when interacting with messages. Per-title fixes are declared in [QUIRKS.md](QUIRKS.md).",
      "Partidas guardadas, varias zonas, sonido, mando, teclado y ratón (las indicaciones de teclas se dibujan sobre las del mando), ajustes de fotogramas 30, 60, 90 y sin límite, salida a 3840x2160 (unos 80 FPS en la escena de prueba con una RX 7900 XT). El juego en línea mediante los servidores comunitarios del título (`PS_ONLINE=1`) funciona, con tirones ocasionales al interactuar con mensajes. Las correcciones por título se declaran en [QUIRKS.md](../en/QUIRKS.md).",
      "Сохранения, несколько локаций, звук, геймпад, клавиатура и мышь (подсказки клавиш рисуются поверх подсказок геймпада), пресеты частоты кадров 30, 60, 90 и без ограничения, вывод 3840x2160 (около 80 FPS в тестовой сцене на RX 7900 XT). Сетевая игра через серверы сообщества (`PS_ONLINE=1`) работает, с редкими подтормаживаниями при работе с сообщениями. Исправления для конкретной игры объявлены в [QUIRKS.md](../en/QUIRKS.md).")
TITLE(CUSA16458, "Atelier Ryza", "", playable,
      "Title menu, prologue, the field with the player in control, sound; the river and the forest draw correctly. Saving and loading work.",
      "Menú principal, prólogo, el mapa con el control del jugador, sonido; el río y el bosque se dibujan bien. Guardar y cargar funcionan.",
      "Главное меню, пролог, локация с управлением у игрока, звук; река и лес рисуются правильно. Сохранение и загрузка работают.")
TITLE(CUSA16921, "Atelier Ayesha DX", "", playable,
      "Intro movie, title menu, opening scenes, the field, the atelier, sound; the key prompts are drawn for keyboard play.",
      "Vídeo de introducción, menú principal, escenas iniciales, el mapa, el taller, sonido; las indicaciones de teclas se dibujan para jugar con teclado.",
      "Вступительный ролик, главное меню, начальные сцены, локация, ателье, звук; подсказки клавиш рисуются для игры с клавиатуры.")
TITLE(CUSA50989, "Symphonia", "", experimental,
      "Title screen, the first room, sound.",
      "Pantalla de título, la primera sala, sonido.",
      "Титульный экран, первая комната, звук.")
TITLE(CUSA49515, "Astrea", "", experimental,
      "The intro's story panels, sound.",
      "Las viñetas de la historia de la introducción, sonido.",
      "Сюжетные панели вступления, звук.")
TITLE(CUSA43027, "SaGa Emerald Beyond", "", experimental,
      "Title screen, first story scenes with dialogue, sound.",
      "Pantalla de título, primeras escenas de la historia con diálogos, sonido.",
      "Титульный экран, первые сюжетные сцены с диалогами, звук.")
TITLE(CUSA26563, "ALTDEUS: Beyond Chronos", "", unsupported,
      "Black screen: it requires a PS VR headset and has no flat-screen mode.",
      "Pantalla negra: requiere un casco PS VR y no tiene modo de pantalla plana.",
      "Чёрный экран: требует шлем PS VR, плоского режима нет.")
TITLE(CUSA41240, "Gal Guardians: Demon Purge", "", experimental,
      "Regress: frames drawn, sound.",
      "Regress: fotogramas dibujados, sonido.",
      "Regress: нарисованные кадры, звук.")
TITLE(CUSA52187, "Gal Guardians: Servants of the Dark (JP)", "", experimental,
      "Regress: frames drawn, sound.",
      "Regress: fotogramas dibujados, sonido.",
      "Regress: нарисованные кадры, звук.")
TITLE(CUSA57547, "Call of Duty: Black Ops", "", experimental,
      "Regress: frames drawn, sound. Keyboard and mouse with the game's own camera, a shooter layout and a crosshair.",
      "Regress: fotogramas dibujados, sonido. Teclado y ratón con la cámara del propio juego, una distribución de shooter y una mira.",
      "Regress: нарисованные кадры, звук. Клавиатура и мышь с собственной камерой игры, раскладка для шутера и прицел.")
TITLE(CUSA57548, "Call of Duty: Black Ops II", "", playable,
      "First mission (Pyrrhic Victory) through chained checkpoints to the river barge, Save and Quit, Resume Story, videos, sound, pad.",
      "Primera misión (Pyrrhic Victory) por puntos de control encadenados hasta la barcaza del río, Guardar y salir, Reanudar historia, vídeos, sonido, mando.",
      "Первая миссия (Pyrrhic Victory) по цепочке контрольных точек до речной баржи, Save and Quit, Resume Story, ролики, звук, геймпад.")
TITLE(CUSA49769, "Blood Omen 2", "", experimental,
      "Regress: frames drawn, no sound.",
      "Regress: fotogramas dibujados, sin sonido.",
      "Regress: нарисованные кадры, без звука.")
TITLE(CUSA01458, "Onigiri", "", experimental,
      "Regress: frames drawn, sound.",
      "Regress: fotogramas dibujados, sonido.",
      "Regress: нарисованные кадры, звук.")
TITLE(CUSA53477, "OCTOPATH TRAVELER 0 (JP)", "", broken,
      "Loads a saved game and draws frames and sound, but the 3D scene stays black (only the minimap shows). Left out of routine runs.",
      "Carga una partida guardada y dibuja fotogramas y sonido, pero la escena 3D sigue negra (solo se ve el minimapa). Fuera de las ejecuciones habituales.",
      "Загружает сохранение, рисует кадры и звук, но 3D-сцена остаётся чёрной (видна только миникарта). Исключена из регулярных прогонов.")
