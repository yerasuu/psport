# psport — игры PS4 нативно на Linux

> Справочным является README на английском ([README.md](README.md)); этот перевод может отставать. Техническая документация: [docs/en/](docs/en/ARCHITECTURE.md).

[English](README.md) · **Русский** · [Español](README.es.md)

psport — независимый проект, выросший из [bbport](https://github.com/deadinside28/bloodborne_pc),
нативного порта Bloodborne для Linux, и из [shadPS4](https://github.com/shadps4-emu/shadPS4);
он сделан для decanter и рассчитан не на одну игру. Оригинальный исполняемый файл
игры PS4 запускается на ПК с Linux x86-64 так же, как программа для Windows под Wine: это слой
совместимости, а не эмулятор.

- **Процессор.** Процессор PS4 — x86-64, поэтому код игры выполняется на процессоре ПК как есть,
  без эмуляции и без трансляции инструкций. eboot заранее (`scripts/`) превращается в плоский образ
  памяти, в который слинкована libc игры; загрузчик на C (`src/main/main.c`) отображает его и передаёт
  ему управление.
- **Системные библиотеки.** Их предоставляет сам порт: рантайм в `src/*/runtime_*.c` (память,
  потоки, синхронизация, файлы, звук, геймпад, сохранения, исключения, потоки пользовательского
  уровня, модули, которые игра загружает во время работы) и библиотеки в `gpu/` (libSceNgs2,
  libSceAudiodec, libSceFiber, libSceFont). Ни одной из проверенных игр не нужен дамп системных
  модулей консоли.
- **Графика.** Поток команд видеочипа и шейдеры GCN переводятся в Vulkan рендером на основе
  видеоядра и рекомпилятора шейдеров [shadPS4](https://github.com/shadps4-emu/shadPS4)
  (`gpu/shadps4/`) в том виде, как его доработал bbport.

Bloodborne работает так же, как в bbport, с его патчами, апскейлерами и настройками. Остальные
игры запускаются с `PS_SKIP_GAME_CHECK=1`. Что psport берёт из других
проектов (коммит источника, лицензия) и что меняет в унаследованном коде: [FORK.md](FORK.md).
bbport и shadPS4 время от времени просматриваются; полезные для psport изменения переносятся
вручную и записываются в [FORK.md](FORK.md), релизы целиком не вливаются.

> **Файлов игр в репозитории нет.** Нужны собственные дампы своих игр.
> Проект не связан с Sony Interactive Entertainment, FromSoftware, AMD и издателями перечисленных
> ниже игр и не имеет отношения к shadPS4.

## Игры

Проверены без окна через `tools/regress.sh` (см. ниже): шесть из семи доходят до геймплея со
звуком.

| Игра | Title ID | Состояние |
|---|---|---|
| Bloodborne (1.09) | CUSA03173 | играбельна: сохранения, несколько локаций, звук, геймпад |
| Atelier Ryza | CUSA16458 | титульное меню, пролог, поле под управлением игрока, звук |
| Atelier Ayesha DX | CUSA16921 | вступительный ролик, титульное меню, первые сцены, поле, ателье, звук |
| Tales of Symphonia Remastered | CUSA50989 | титульный экран, первая комната, звук |
| Astrea | CUSA49515 | сюжетные кадры вступления, звук |
| SaGa Emerald Beyond | CUSA43027 | титульный экран, первые сюжетные сцены с диалогами, звук |
| ALTDEUS: Beyond Chronos | CUSA26563 | чёрный экран: нужен шлем PS VR, режима для обычного экрана нет |

Игры, которым обязателен PS VR (`require_ps_vr` в `param.sfo`), не поддерживаются. Основательно
проверена одна машина (AMD Radeon RX 7800 XT, Mesa/RADV).

## Использование

Обычно через decanter: его раздел PS клонирует psport, собирает его через `build.sh`, пересобирает,
когда checkout сдвинулся, и запускает каждый дамп через `run.sh` (все игры, кроме Bloodborne, — с
`PS_SKIP_GAME_CHECK=1 PS_PERMISSIVE=1`).

Отдельно:

```bash
git clone --recursive https://github.com/yerasuu/psport.git && cd psport
bash build.sh                                   # out/psport и out/gpu/libpsport-gpu.so
PS_GAME_DIR=/путь/к/CUSA03173 bash run.sh       # Bloodborne
PS_GAME_DIR=/путь/к/дампу PS_SKIP_GAME_CHECK=1 PS_PERMISSIVE=1 bash run.sh
```

По умолчанию `PS_GAME_DIR` — единственный дамп рядом с checkout (например,
`../CUSA03173`). С `PS_PERMISSIVE=1` импорты, которых нет в порте, возвращают 0, а не останавливают игру. `PS_SYS_MODULES=<папка>` по желанию подключает
собственный дамп системных модулей (`*.sprx`) там, где у порта ещё нет своей библиотеки; из него
ничего не распространяется. Сохранения и кэш шейдеров лежат в `user/`, настройки — в `psport.ini`.
Лаунчер на GTK4 (`bash launcher/psport-launcher.sh`; `launcher/install-desktop.sh` добавляет его в меню
приложений) взят из bbport и рассчитан на Bloodborne.

## Требования и сборка

- Linux x86-64 и видеокарта с Vulkan 1.3.
- GCC (или другой компилятор через `CC`), CMake, Ninja, pkg-config, Python 3 и пакеты из
  `shell.nix`: Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang и остальные зависимости видеоядра
  shadPS4, а также protobuf, nlohmann_json, OpenSSL, zlib и miniupnpc для необязательного сетевого
  модуля. Если их нет, а [Nix](https://nixos.org) установлен, `build.sh` и `run.sh` сами
  перезапускаются в `shell.nix`.
- Подмодуль (Dear ImGui): клонируйте с `--recursive`, иначе `build.sh`
  выполнит `git submodule update --init --recursive`.
- `PS_LTO=OFF` отключает оптимизацию при линковке; `PS_PGO=generate|use|off` задаёт оптимизацию по
  профилю (по умолчанию `use`, если в `pgo/` есть профиль).
- Если установлен [ccache](https://ccache.dev) (он есть в `shell.nix`), библиотека GPU собирается
  через него, с одним кэшем для всех копий и worktree (`~/.cache/psport/ccache`, `CCACHE_DIR`).
- `nix-build packaging/port.nix` собирает порт из исходников в песочнице Nix; CI собирает каждый
  коммит в `master` и публикует сборку каждого тега `vX.Y.Z` как бинарный кэш Nix и GitHub Release
  с пакетом, который работает без Nix (`psport-<tag>-x86_64.tar.zst`, `tools/bundle.sh`;
  FORK.md, «CI and binary cache»).

## Измерение прогресса

`tools/regress.sh` запускает каждую игру из `tools/regress/titles.txt` в gamescope без окна
(`PS_AUDIO=none`, `PS_PAD_HOST=0`: ни окна, ни звука, ни геймпадов пользователя), ведёт её по
расписанию нажатий (`tools/regress/*.sched`) и сообщает, докуда она дошла (загружена, вход в код
игры, инициализирована, первый кадр видеочипа, нарисованные кадры, звук), её сигнатуру падения и
сколько импортов без реализации она вызвала; с `-c <прежний results.json>` отмечает регрессии. Все
семь запускаются без дампа системных модулей; `SYS_MODULES=<папка>` при желании передаёт его играм,
кроме Bloodborne, чтобы сравнить с собственными библиотеками порта. Запуск и вехи: [FORK.md](FORK.md#regression-runs).

## Диагностика

Перед запуском игры перечисляются все импорты без реализации или с заглушкой. Диагностические
переменные (`PS_PERMISSIVE_OUT`, `PS_AUDIO_STATS`, `PS_NGS2_TRACE`, `PS_FIBER_TRACE`) и
переключатели системных модулей (`PS_SYS_MODULES`, `PS_HLE`, `PS_LLE`, `PS_SYS_FONTS`) описаны в
[FORK.md](FORK.md). `tools/gcn_dis.sh` дизассемблирует шейдеры GCN.

## Правила

Из [CLAUDE.md](CLAUDE.md): никаких обходов под конкретные игры или шейдеры, исправления
устраняют саму причину; тестов в репозитории нет (изменения проверяются через `tools/regress.sh`,
аппаратные тесты, если нужны, — homebrew на OpenOrbis); код следует соглашениям окружающего кода.

## Структура репозитория

| Путь | Содержимое |
|---|---|
| `src/` | Загрузчик (`src/main/`) и рантайм (`runtime_*.c`) |
| `scripts/` | `online.py` — адреса для игры по сети, которые `run.sh` записывает при `PS_ONLINE=1` (единственный шаг запуска на Python), и `sce_file.py` для инструментов на Python |
| `gpu/` | Библиотека рендера: видеоядро shadPS4 с изменениями bbport (`gpu/VENDOR.txt`), перенесённые библиотеки (`ngs2/`, `audiodec/`, `fiber/`, `font/`), оверлей и меню в игре (`port/`), мост DLSS, сетевой модуль |
| `launcher/`, `packaging/` | Лаунчер на GTK4 из bbport; пакет Nix и скрипт AppImage |
| `patches/` | Патчи сообщества для Bloodborne |
| `tools/` | Регрессионные прогоны, дизассемблер GCN, инструменты FSR 4 и `fsr4cap`, помощники для скриптовых запусков |
| `docs/` | По папке на язык: `docs/en/` (архитектура, [добавление игры](docs/en/ADDING_A_TITLE.md), описания функций, история bbport на английском) и `docs/ru/` (русские оригиналы bbport с теми же именами) |

## Благодарности и лицензии

psport распространяется под лицензией **GNU GPL v2 или новее** ([LICENSE](LICENSE)).

### На чём построен

| Проект | Что даёт | Лицензия |
|---|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc) от deadinside | основа psport: нативный загрузчик, HLE-рантайм, рендерер на Vulkan, всё для Bloodborne | GPL-2.0-or-later |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | видеоядро, рекомпилятор шейдеров, AvPlayer и другие библиотеки, которые несёт bbport; источник переноса libSceFiber и libSceFont | GPL-2.0-or-later |
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | источник переноса libSceNgs2 и libSceAudiodec | GPL-2.0 |

Сам psport поддерживает Yerasuu для [decanter](#использование). Что psport меняет в унаследованном
коде и что переносит (с коммитами источников), а также последние просмотренные точки bbport и
shadPS4 записаны в [FORK.md](FORK.md).

### Встроенный сторонний код

| Компонент | Для чего | Лицензия |
|---|---|---|
| [sirit](https://github.com/shadps4-emu/sirit) | генерация SPIR-V | BSD-3-Clause |
| заголовки AMD GCN (`gpu/third_party/gcn`) | декодирование инструкций GCN | MIT |
| [half](https://half.sourceforge.net/) | числа половинной точности | MIT |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) от FireBurn, AMD FidelityFX SDK | FSR 3.1 | MIT |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | декодирование звука ATRAC9 | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | оверлей и игровые меню | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | сетевой модуль | MIT |
| мост DLSS (`gpu/dlss_bridge`) из [Windows-порта Supermedo](https://github.com/Supermedo/bloodborne_pc), адаптированного из [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) | DLSS | MIT |
| [шрифты DejaVu](https://dejavu-fonts.github.io/) | текст оверлея, запасной шрифт для libSceFont | Bitstream Vera (изменения DejaVu — общественное достояние) |
| [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) | сборка файлов FSR 4.1.1 на машине пользователя | MIT |

Подключаются из системы или оболочки Nix, а не встраиваются, под своими лицензиями: FFmpeg,
FreeType, Vulkan, SDL3, Boost, fmt и остальное из `shell.nix`.

### Сообщество

Патчи Bloodborne — Kyo, Lance McDonald, auser1337, illusion, emoose и другие участники сообщества
(`patches/Bloodborne.xml`).

### Не распространяется

Никаких файлов игры и системных модулей консоли. DLL и данные моделей FSR 4 от AMD, а также SDK и
библиотеки DLSS от NVIDIA тоже не включены — их предоставляет пользователь.
