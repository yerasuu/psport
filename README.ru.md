# psport — игры PS4 нативно на Linux

> Справочным является README на английском ([README.md](README.md)); этот перевод может отставать. Техническая документация: [docs/en/](docs/en/ARCHITECTURE.md).

[English](README.md) · **Русский** · [Español](README.es.md)

psport — слой совместимости PS4 для Linux. Оригинальный исполняемый файл игры PS4 запускается на ПК
с Linux x86-64 так же, как программа для Windows под Wine: psport загружает его, предоставляет
импортируемые им системные библиотеки и переводит его работу с видеочипом в Vulkan. Это не эмулятор.
psport — независимый проект, выросший из [bbport](https://github.com/deadinside28/bloodborne_pc),
нативного порта Bloodborne для Linux, и из [shadPS4](https://github.com/shadps4-emu/shadPS4); он
обобщается в слой для любой игры, а Bloodborne остаётся поддерживаемой игрой со своим профилем.
bbport и shadPS4 время от времени просматриваются; полезные для psport изменения переносятся
вручную и записываются в [FORK.md](FORK.md).

- **Процессор.** Процессор PS4 — x86-64, поэтому код игры выполняется на процессоре ПК как есть,
  без эмуляции и без трансляции инструкций. Загрузчик на C (`src/main/main.c`,
  `src/loader/loader_*.c`) читает eboot и слинкованные с ним модули (SELF или ELF) прямо из папки
  игры, релоцирует и связывает их в один образ и передаёт ему управление.
- **Системные библиотеки.** Их предоставляет сам psport: рантайм на C в `src/*/runtime_*.c`
  (память, потоки, синхронизация, файлы, звук, геймпад, сохранения, исключения, сервисы, потоки
  пользовательского уровня, модули, которые игра загружает во время работы) и библиотеки в `gpu/`
  (GnmDriver, VideoOut, AvPlayer, Videodec2, Ngs2, Audiodec, Fiber, Font, PngEnc, PngDec). Ни одной
  из проверенных игр не нужен дамп системных модулей консоли.
- **Графика.** Поток команд видеочипа и шейдеры GCN переводятся в Vulkan рендером на основе
  видеоядра и рекомпилятора шейдеров [shadPS4](https://github.com/shadps4-emu/shadPS4)
  (`gpu/shadps4/`) в том виде, как его доработал bbport.
- **Профили игр.** То, что относится к одной игре (хуки и оптимизации в её коде), лежит в её
  профиле в `gpu/games/`; без профиля любая игра работает по общим путям.

Как всё устроено: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md). Что psport берёт из других
проектов (коммит источника, лицензия) и что меняет в унаследованном коде: [FORK.md](FORK.md).

> **Файлов игр в репозитории нет.** Нужны собственные дампы своих игр.
> Проект не связан с Sony Interactive Entertainment, FromSoftware, AMD и издателями перечисленных
> ниже игр и не имеет отношения к shadPS4.

## Игры

Каждая игра из `tools/regress/titles.txt` запускается без окна через `tools/regress.sh` (см. ниже).
Состояние — докуда игра доходила при проверке; строки «regress» показывают, что даёт её прогон по
сценарию (первый кадр видеочипа, нарисованные кадры, звук), а не прохождение.

| Игра | Title ID | Состояние |
|---|---|---|
| Bloodborne (1.09) | CUSA03173 | играбельна: сохранения, несколько локаций, звук, геймпад |
| Atelier Ryza | CUSA16458 | титульное меню, пролог, поле под управлением игрока, звук |
| Atelier Ayesha DX | CUSA16921 | вступительный ролик, титульное меню, первые сцены, поле, ателье, звук |
| Tales of Symphonia Remastered | CUSA50989 | титульный экран, первая комната, звук |
| Astrea | CUSA49515 | сюжетные кадры вступления, звук |
| SaGa Emerald Beyond | CUSA43027 | титульный экран, первые сюжетные сцены с диалогами, звук |
| ALTDEUS: Beyond Chronos | CUSA26563 | чёрный экран: нужен шлем PS VR, режима для обычного экрана нет |
| Gal Guardians: Demon Purge | CUSA41240 | regress: нарисованные кадры, звук |
| Gal Guardians: Servants of the Dark (JP) | CUSA52187 | regress: нарисованные кадры, звук |
| Call of Duty: Black Ops | CUSA57547 | regress: нарисованные кадры, звук |
| Call of Duty: Black Ops II | CUSA57548 | первая миссия (Pyrrhic Victory) через цепочку контрольных точек до речной баржи, Save and Quit, Resume Story, ролики, звук, геймпад |
| Blood Omen 2 | CUSA49769 | regress: нарисованные кадры, без звука |
| Onigiri | CUSA01458 | regress: первый кадр видеочипа и звук, освещённого кадра в его сценарии нет |
| OCTOPATH TRAVELER 0 (JP) | CUSA53477 | загружает сохранение, затем видеочип падает; исключена из обычных прогонов |

Игры, которым обязателен PS VR (`require_ps_vr` в `param.sfo`), не поддерживаются. Основательно
проверена одна машина (AMD Radeon RX 7800 XT, Mesa/RADV).

## Использование

Обычно через decanter: его раздел PS клонирует psport, собирает его через `build.sh`, пересобирает,
когда checkout сдвинулся, и запускает каждый дамп через `run.sh` (все игры, кроме Bloodborne, — с
`PS_PERMISSIVE=1`).

Отдельно:

```bash
git clone --recursive https://github.com/yerasuu/psport.git && cd psport
bash build.sh                                   # out/psport и out/gpu/libpsport-gpu.so
PS_GAME_DIR=/путь/к/CUSA03173 bash run.sh       # Bloodborne
PS_GAME_DIR=/путь/к/дампу PS_PERMISSIVE=1 bash run.sh
```

По умолчанию `PS_GAME_DIR` — единственный дамп рядом с checkout (например,
`../CUSA03173`). С `PS_PERMISSIVE=1` импорты, которых нет в psport, возвращают 0, а не
останавливают игру. `PS_SYS_MODULES=<папка>` по желанию подключает собственный дамп системных
модулей (`*.sprx`) там, где у порта ещё нет своей библиотеки; из него ничего не распространяется.
Сохранения и кэш шейдеров лежат в `user/`, настройки — в `psport.ini`. Лаунчер на GTK4
(`bash launcher/psport-launcher.sh`; `launcher/install-desktop.sh` добавляет его в меню
приложений) взят из bbport и рассчитан на Bloodborne.

## Требования и сборка

- Linux x86-64 и видеокарта с Vulkan 1.3.
- GCC (или другой компилятор через `CC`), CMake, Ninja, pkg-config, Python 3 (для игры по сети,
  встроенного лаунчера и инструментов; для самого запуска он не нужен) и пакеты из
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
игры запускаются без дампа системных модулей; `SYS_MODULES=<папка>` при желании передаёт его играм,
кроме Bloodborne, чтобы сравнить с собственными библиотеками psport. Запуск и вехи:
[FORK.md](FORK.md#regression-runs); как запустить новую игру:
[docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md).

## Диагностика

Перед запуском игры перечисляются все импорты без реализации или с заглушкой. Диагностические
переменные (`PS_PERMISSIVE_OUT`, `PS_AUDIO_STATS`, `PS_NGS2_TRACE`, `PS_FIBER_TRACE`) и
переключатели системных модулей (`PS_SYS_MODULES`, `PS_HLE`, `PS_LLE`, `PS_SYS_FONTS`) описаны в
[FORK.md](FORK.md). `tools/gcn_dis.sh` дизассемблирует шейдеры GCN.

## Правила

Из [CLAUDE.md](CLAUDE.md): никаких обходов под конкретные игры или шейдеры, исправления
устраняют саму причину; оптимизации для одной игры идут в её профиль (`gpu/games/`), и любая игра
должна правильно работать без него (`PS_GAME_PROFILE=none`); тестов в репозитории нет (изменения
проверяются через `tools/regress.sh`, аппаратные тесты, если нужны, — homebrew на OpenOrbis); код
следует соглашениям окружающего кода.

## Что читать дальше

- [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md): компоненты и порядок запуска.
- [docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md): как запустить новый дамп.
- [FORK.md](FORK.md): перенесённые компоненты, изменения в унаследованном коде, переменные, отчёт о
  вызовах, регрессионные прогоны, CI.
- Описания функций: [апскейлер](docs/en/upscaler.md), [параллельная работа GPU](docs/en/parallel_gpu.md),
  [векторы движения](docs/ru/motion_vectors.md), [моды и патчи](docs/ru/MODS.md).
- [docs/ru/history/](docs/ru/history/): проектные заметки, планы и журналы изменений bbport.

## Структура репозитория

| Путь | Содержимое |
|---|---|
| `src/` | Точка входа (`src/main/`), нативный загрузчик (`loader_*.c`) и рантайм на C (`runtime_*.c`) |
| `scripts/` | `online.py` — адреса для игры по сети, которые `run.sh` записывает при `PS_ONLINE=1` (единственный шаг запуска на Python), и `sce_file.py` для инструментов на Python |
| `gpu/` | Библиотека GPU: видеоядро shadPS4 с изменениями bbport (`gpu/VENDOR.txt`), собственный декодер PM4 (`cp/`) и модуль памяти (`layer/`), перенесённые библиотеки (`ngs2/`, `audiodec/`, `fiber/`, `font/`, `pngenc/`, `pngdec/`), профили игр (`games/`), оверлей и меню в игре (`port/`), мост DLSS, сетевой модуль |
| `launcher/`, `packaging/` | Лаунчер на GTK4 из bbport; пакет Nix и скрипт AppImage |
| `patches/` | Патчи сообщества для Bloodborne |
| `tools/` | Регрессионные прогоны, дизассемблер GCN, инструменты FSR 4 и `fsr4cap`, помощники для скриптовых запусков |
| `docs/` | По папке на язык: `docs/en/` (архитектура, запуск новой игры, описания функций, история bbport на английском) и `docs/ru/` (русские оригиналы bbport с теми же именами файлов) |

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
