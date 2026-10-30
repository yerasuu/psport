# psport

Игры PS4 нативно на Linux.

[English](README.md) · [Español](README.es.md) · **Русский**

> Справочным является README на английском ([README.md](README.md)); этот перевод может отставать.
> Техническая документация на английском: [docs/en/](docs/en/ARCHITECTURE.md).

## Обзор

psport — слой совместимости PS4 для Linux x86-64. Это не эмулятор: он работает так же, как Wine с
программами для Windows. Процессор PS4 — x86-64, поэтому оригинальный исполняемый файл игры
выполняется прямо на процессоре ПК, без трансляции инструкций. psport загружает исполняемый файл и
связанные с ним модули, предоставляет системные библиотеки, которые импортирует игра, и переводит
её работу с видеочипом в Vulkan.

psport — независимый проект. Он вырос из [bbport](https://github.com/deadinside28/bloodborne_pc),
нативного порта Bloodborne для Linux, и опирается на видеоядро и рекомпилятор шейдеров
[shadPS4](https://github.com/shadps4-emu/shadPS4). Из порта одной игры он обобщается в слой для
любой игры; Bloodborne по-прежнему поддерживается, со своим профилем игры.

> [!IMPORTANT]
> **Файлы игр не входят в проект.** Пользователю нужны собственные дампы игр, которыми он владеет.
> Проект не связан с Sony Interactive Entertainment, FromSoftware, AMD и издателями перечисленных
> ниже игр и не имеет отношения к shadPS4.

## Состояние

psport активно разрабатывается. Игра может быть **играбельной**, **экспериментальной** (запускается,
но видна лишь ограниченная часть), **неработающей** (запускается, но ломается основная часть) или
**неподдерживаемой**. Состояние — докуда игра доходила на глазах, а не полное прохождение; что
делает каждая игра, написано в [docs/ru/GAMES.md](docs/ru/GAMES.md). Каждая игра из
таблицы есть в [tools/regress/titles.txt](tools/regress/titles.txt) и запускается без окна через
`tools/regress.sh` (см. [Тестирование](#тестирование)).

| Игра | ID | Состояние |
|---|---|---|
| Bloodborne (1.09) | CUSA03173 | играбельна |
| Atelier Ryza | CUSA16458 | играбельна |
| Atelier Ayesha DX | CUSA16921 | играбельна |
| Tales of Symphonia Remastered | CUSA50989 | экспериментально |
| Astrea | CUSA49515 | экспериментально |
| SaGa Emerald Beyond | CUSA43027 | экспериментально |
| ALTDEUS: Beyond Chronos | CUSA26563 | не поддерживается |
| Gal Guardians: Demon Purge | CUSA41240 | экспериментально |
| Gal Guardians: Servants of the Dark (JP) | CUSA52187 | экспериментально |
| Call of Duty: Black Ops | CUSA57547 | экспериментально |
| Call of Duty: Black Ops II | CUSA57548 | играбельна |
| Blood Omen 2 | CUSA49769 | экспериментально |
| Onigiri | CUSA01458 | экспериментально |
| OCTOPATH TRAVELER 0 (JP) | CUSA53477 | не работает |

- Игры, требующие PS VR (`require_ps_vr` в `param.sfo`), не поддерживаются.
- Ни одной проверенной игре не нужен дамп системных модулей консоли.
- Тщательно проверена только одна машина: AMD Radeon RX 7900 XT с Mesa (RADV).

## Требования

- **ОС:** Linux на x86-64. Windows пока только основа и игр не запускает; его сборка в CI — тестовый
  пакет с зондом, который сообщает, что предлагает драйвер видеочипа
  ([руководство по проверке на Windows](docs/en/WINDOWS_TEST.md)).
- **Видеочип:** Vulkan 1.3 с descriptor indexing и buffer device address. Путь видеочипа один для
  всех производителей и выбирает возможности по тому, что сообщает устройство, а не по
  производителю. Проверен на AMD с RADV; NVIDIA и Intel пока не проверены
  ([руководство по проверке на NVIDIA](docs/en/NVIDIA_TEST.md)).
- **Игры:** собственные дампы игр, которыми владеет пользователь (папка игры с `eboot.bin`, `sce_module/` и
  `sce_sys/param.sfo`).
- **Инструменты сборки**, на выбор:
  - [Nix](https://nixos.org): если зависимостей не хватает, `build.sh` и `run.sh` сами входят в
    [shell.nix](shell.nix).
  - Пакеты системы: GCC (или другой компилятор через `CC`), CMake, Ninja, pkg-config и библиотеки
    из `shell.nix` (Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang и остальные зависимости
    видеоядра shadPS4; nlohmann_json, mbedTLS, zlib и miniupnpc для необязательного
    сетевого модуля).
- **Python 3** нужен только для сетевой игры, сборки сетевого модуля (он генерирует сообщения
  shadNet), встроенного лаунчера и инструментов разработчика; обычному запуску он не нужен.

## Начало работы

### Через decanter (рекомендуется)

[decanter](https://github.com/yerasuu/decanter) — рекомендуемая оболочка. Его раздел PS клонирует
psport, собирает его через `build.sh`, пересобирает при изменении рабочей копии и запускает каждый
дамп через `run.sh` (игры, кроме Bloodborne, с `PS_PERMISSIVE=1`).

### Сборка из исходников

```bash
git clone --recursive https://github.com/yerasuu/psport.git
cd psport
bash build.sh        # собирает out/psport и out/gpu/libpsport-gpu.so
```

Без `--recursive` `build.sh` сам загрузит подмодуль (Dear ImGui).

| Параметр сборки | Действие |
|---|---|
| `PS_LTO=OFF` | отключает оптимизацию при компоновке (по умолчанию включена) |
| `PS_NET=OFF` | без сетевого модуля (`libpsport-net.so`), как собираются публичный бинарный кэш и архив |
| `PS_PGO=generate\|use\|off` | оптимизация по профилю; по умолчанию `use`, когда в `pgo/` есть профиль |
| установлен ccache | библиотека видеочипа компилируется через него, один кэш на все рабочие копии (`~/.cache/psport/ccache` или `CCACHE_DIR`) |

`nix-build packaging/port.nix` собирает порт из исходников в песочнице Nix, с сетевым модулем;
`--arg withNet false` собирает без него.

### Запуск игры

```bash
PS_GAME_DIR=/путь/к/CUSA03173 bash run.sh                     # Bloodborne
PS_GAME_DIR=/путь/к/дампу PS_PERMISSIVE=1 bash run.sh         # любая другая игра
```

`run.sh` при необходимости собирает порт, затем запускает игру.

| Переменная | Действие |
|---|---|
| `PS_GAME_DIR` | папка игры; по умолчанию единственный дамп рядом с рабочей копией |
| `PS_PERMISSIVE=1` | импорты, которые psport ещё не реализует, возвращают 0 вместо остановки игры |
| `PS_DATA_DIR` | папка с правом записи для результатов сборки (`out/`), сохранений и кэша шейдеров (`user/`) и `psport.ini`; по умолчанию рабочая копия |
| `PS_SYS_MODULES=<папка>` | по желанию подключает собственный дамп системных модулей пользователя (`*.sprx`) там, где у psport ещё нет библиотеки; ничего из него не распространяется |

`bash run.sh --software` рисует через Lavapipe (программный драйвер Vulkan из Mesa) вместо
видеочипа.

В игре Insert или L3+R3 открывает меню оверлея. Сохранения и кэш шейдеров хранятся в `user/`,
настройки — в `psport.ini`.

### Готовые сборки

Каждый тег версии `vX.Y.Z` собирается в CI и публикуется как GitHub Release с архивом, который
работает без Nix: `psport-<tag>-x86_64.tar.zst` и его `.sha256`, созданные
[tools/bundle.sh](tools/bundle.sh). Распакуйте его в любое место с правом записи и запускайте с
драйверами Vulkan самой системы:

```bash
PS_DATA_DIR=<папка с правом записи> PS_GAME_DIR=/путь/к/дампу bash <распакованная папка>/run.sh
```

В архиве пока нет сетевого модуля (`libpsport-net.so`): он больше не связан с OpenSSL, abseil и
protobuf (Apache-2.0, их нельзя распространять вместе с psport под GPL-2.0), а использует mbedTLS
по варианту GPL-2.0-or-later, и остаётся вне архива, пока сетевая игра с ним не проверена; до тех
пор для игры по сети (`PS_ONLINE=1`) нужна сборка из исходников. Загрузчика
Vulkan в архиве тоже нет: используется системный `libvulkan.so.1`, поэтому в системе без него в
папках библиотек архив не запускается (в NixOS его нет в `/run/opengl-driver/lib`).

Та же сборка, тоже без сетевого модуля, публикуется как бинарный кэш Nix, из которого decanter
скачивает готовые сборки; см. [FORK.md](FORK.md#ci-and-binary-cache). Локальная сборка (`build.sh`,
`nix-build packaging/port.nix`) сетевой модуль содержит.

### Лаунчер

В [launcher/](launcher/) лежит лаунчер на GTK4, унаследованный от bbport и рассчитанный на
Bloodborne: `bash launcher/psport-launcher.sh` запускает его, а `bash launcher/install-desktop.sh`
добавляет в меню приложений.

## Настройка

Настройки хранятся в `psport.ini` в папке данных (`PS_CONFIG` указывает другой файл). Большинство
из них меняется в меню оверлея, которое записывает файл. Самые используемые ключи и переменные:

| Настройка | Действие |
|---|---|
| `output_res` | разрешение вывода; для игры с пресетами размеры, отличные от 1080p, масштабируют сцену |
| `upscaler` (`PS_UPSCALER`) | временной апскейлер там, где его размещает профиль игры ([заметки об апскейлере](docs/en/upscaler.md)) |
| `frame_gen` (`PS_FRAME_GEN`) | `off` (по умолчанию) или `fsr`: генерация кадров FSR 3.1, один сгенерированный кадр на кадр игры, если видеокарта отвечает её требованиям (строка `frame_gen_fsr` в `--list-features`); `auto` и `dlss` пока недоступны и оставляют её выключенной; читается при запуске ([генерация кадров](docs/en/upscaler.md#frame-generation)) |
| `menu_language` (`PS_MENU_LANGUAGE`) | язык оверлея: `en`, `es` или `ru` |
| `game_mouse_sensitivity` | масштаб движения мыши системы, которое получает игра (от 0,05 до 20, по умолчанию 1) |
| `mouse_pad_filter` (`PS_MOUSE_PAD_FILTER`) | включено по умолчанию: движение мыши, пока стик контроллера отклонён, отбрасывается, так как его создаёт Desktop Layout Steam или reWASD из стика и камера повернулась бы дважды; `0` сохраняет его |
| `PS_LANGUAGE` | системный язык, который читает игра (1 английский, 3 испанский, 8 русский) |
| `PS_FPS` | пресет частоты кадров для игр, у которых они есть (Bloodborne: 30, 60, 90, `uncap`) |
| `PS_ONLINE=1` | сетевая игра через серверы сообщества для этой игры ([сетевая игра](docs/en/ARCHITECTURE.md#online-play)) |
| `PS_MODS_ENABLED=0` | отключает файловые моды ([моды и патчи](docs/en/MODS.md)) |
| `PS_GAME_PROFILE=none` | запуск без профиля игры, только по общим путям |
| `PS_QUIRKS=none,-<id>,+<id>` | выключает или включает исправления для отдельной игры или системы ([quirks](docs/en/QUIRKS.md)) |
| `toypad` (`PS_TOYPAD=1`) | подключает эмулируемый toy pad LEGO Dimensions, по умолчанию выключен ([toy pad](docs/en/TOYPAD.md)) |
| `skip_movies` (`PS_SKIP_MOVIES=1`) | `key.skip_movie` (Escape) или `pad.skip_movie` (`misc1`) останавливает идущие видео, по умолчанию выключен ([FORK.md, Keyboard](FORK.md#keyboard)) |

Значения по умолчанию и почему они подходят любой игре: [FORK.md, Defaults](FORK.md#defaults).
Диагностические переменные и отчёт о вызовах: [FORK.md, Diagnostics](FORK.md#diagnostics).
Системные модули и шрифты (`PS_SYS_MODULES`, `PS_HLE`, `PS_LLE`, `PS_SYS_FONTS`):
[FORK.md, System modules](FORK.md#system-modules).

## Как это работает

- **Загрузчик.** Загрузчик на C читает eboot и связанные с ним модули (SELF или ELF) прямо из папки
  игры, перемещает и связывает их в один образ и передаёт управление коду игры.
- **Системные библиотеки.** psport реализует их нативно: среда выполнения на C (память, потоки,
  синхронизация, файлы, звук, геймпад, сохранения, исключения, сервисы, пользовательские потоки,
  модули, загружаемые во время работы) и библиотеки в `gpu/` (GnmDriver, VideoOut, AvPlayer,
  Videodec2, Ngs2, Audiodec, Fiber, Font, PngEnc, PngDec, JpegDec).
- **Графика.** Поток команд видеочипа и шейдеры GCN переводятся в Vulkan рендерером, выросшим из
  видеоядра и рекомпилятора шейдеров shadPS4 (`gpu/shadps4/`) в том виде, как его расширил bbport.
- **Профили игр.** То, что относится к одной игре (хуки и оптимизации), живёт в её профиле в
  `gpu/games/`; без профиля каждая игра работает по общим путям.
- **Ничего молча.** До старта игры перечисляются все импорты без реализации или с заглушкой;
  вызовы, которые завершаются ошибкой или не выполняют настоящей работы, записываются в журнал и
  подсчитываются в отчёте о вызовах.

Подробности: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md) (компоненты и порядок запуска) и
[FORK.md](FORK.md) (что psport берёт из других проектов и что меняет).

## Разработка

### Тестирование

`tools/regress.sh <папка с играми>` запускает каждую игру из `tools/regress/titles.txt` в
gamescope без окна, без звука и без контроллеров пользователя. Каждой игрой управляет сценарий геймпада
(`tools/regress/*.sched`), а отчёт показывает, докуда она дошла (загружена, вход в код,
инициализирована, первый кадр видеочипа, нарисованные кадры, звук), её сигнатуру сбоя и сколько
нереализованных импортов она вызвала. С `-c <прежний results.json>` он отмечает регрессии.
Использование и вехи: [FORK.md, Regression runs](FORK.md#regression-runs).

### Запуск новой игры

[docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md) описывает, как запустить новый дамп,
прочитать его журнал и отчёт о вызовах, реализовать недостающее и проверить результат.

### Правила для изменений

- Исправления устраняют исходное поведение; никаких хаков под отдельную игру или шейдер в ядре.
- Оптимизации для одной игры идут в её профиль (`gpu/games/<игра>/`), и каждая игра должна
  правильно работать без него (`PS_GAME_PROFILE=none`).
- Временное исправление, нужное одной игре, — это объявленный quirk, который пишется в журнал и
  отключается ([docs/en/QUIRKS.md](docs/en/QUIRKS.md)).
- Тестов в репозитории нет: изменения проверяются через `tools/regress.sh`; тесты на железе, когда
  нужны, — это homebrew на OpenOrbis.
- Изменения следуют стилю окружающего кода; именование и остальные соглашения описаны в [CLAUDE.md](CLAUDE.md).
- Изменения из bbport и shadPS4 переносятся вручную, каждое записывается в [FORK.md](FORK.md) с
  исходным коммитом.

`tools/gcn_dis.sh` дизассемблирует шейдеры GCN.

### Структура репозитория

| Путь | Содержимое |
|---|---|
| `src/` | точка входа (`src/main/`), нативный загрузчик и среда выполнения на C |
| `gpu/` | библиотека видеочипа: видеоядро shadPS4 с изменениями bbport ([gpu/VENDOR.txt](gpu/VENDOR.txt)), декодер PM4 (`cp/`), модуль памяти (`layer/`), перенесённые библиотеки (`fiber/`, `font/`, `pngenc/`, `pngdec/`, `jpegdec/`), libSceNgs2 (`ngs2/`), libSceAudiodec (`audiodec/`), профили игр (`games/`), оверлей и внутриигровое меню (`port/`), мост DLSS, сетевой модуль (`net/`) |
| `scripts/` | `online.py` (сетевые адреса для `PS_ONLINE=1`) и `sce_file.py` для инструментов на Python |
| `launcher/`, `packaging/` | лаунчер bbport на GTK4; пакет Nix и скрипт AppImage |
| `patches/` | отмечает папку psport; собственный файл патчей игры передаёт лаунчер (`PS_BUILTIN_PATCHES_DIR`) |
| `tools/` | регрессионные прогоны, дизассемблер GCN, инструменты FSR 4, помощники сценарных прогонов, архив релиза |
| `docs/` | `docs/en/` (архитектура, запуск новой игры, заметки о возможностях, история bbport на английском), `docs/ru/` (русские оригиналы bbport) |

### Что почитать дальше

- [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md): компоненты и порядок запуска.
- [docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md): запуск нового дампа.
- [docs/en/QUIRKS.md](docs/en/QUIRKS.md): реестр исправлений для одной игры, системы или
  middleware.
- [FORK.md](FORK.md): перенесённые компоненты, изменения унаследованного кода, переменные, отчёт о
  вызовах, регрессионные прогоны, CI.
- Заметки о возможностях: [апскейлер](docs/en/upscaler.md),
  [параллельная работа видеочипа](docs/en/parallel_gpu.md),
  [векторы движения](docs/en/motion_vectors.md), [моды и патчи](docs/en/MODS.md),
  [профилирование](docs/en/PROFILING.md), [toy pad](docs/en/TOYPAD.md).
- [docs/en/history/](docs/en/history/): проектные заметки, планы и журналы изменений bbport
  (русские оригиналы — в [docs/ru/](docs/ru/)).

## Лицензия и благодарности

psport распространяется под **GNU General Public License версии 2** ([LICENSE](LICENSE)). Файлы,
помеченные в заголовке как GPL-2.0-only, доступны только под версией 2; остальные — под
GPL-2.0-or-later и используются как версия 2. Проект поддерживает Yerasuu.

### На чём основан проект

| Проект | Что даёт | Лицензия |
|---|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc) от deadinside | исток psport: нативный загрузчик, среда HLE, рендерер Vulkan, всё для Bloodborne | GPL-2.0-or-later |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | видеоядро, рекомпилятор шейдеров, AvPlayer и другие библиотеки, пришедшие с bbport; источник портов libSceFiber и libSceFont и заголовков libSceNgs2 | GPL-2.0-or-later |
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | поведение, прочитанное для нативных libSceNgs2 и libSceAudiodec, без кода | GPL-2.0 |

Исходные коммиты и изменения унаследованного кода записаны в [FORK.md](FORK.md) и
[gpu/VENDOR.txt](gpu/VENDOR.txt).

Лицензия каждого файла и компонента и то, что берётся из каждого форка (bbport, SrShadowy, Teefpyre,
Supermedo, Mrsuss60, willl03 и AnyPS5), подробно описаны в [LICENSES.md](LICENSES.md) (в машиночитаемом виде:
[LICENSES.json](LICENSES.json)).

### Встроенный сторонний код

| Компонент | Для чего | Лицензия |
|---|---|---|
| [sirit](https://github.com/shadps4-emu/sirit) | генерация SPIR-V | BSD-3-Clause |
| Заголовки GCN от AMD (`gpu/third_party/gcn`) | декодирование инструкций GCN | MIT |
| [half](https://half.sourceforge.net/) | числа половинной точности | MIT |
| [xbyak](https://github.com/herumi/xbyak) 7.43 (`gpu/third_party/xbyak`, заголовки) | генерация кода x86-64 во время выполнения | BSD-3-Clause |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) от FireBurn, AMD FidelityFX SDK | FSR 3.1 | MIT |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | декодирование звука ATRAC9 | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | оверлей и внутриигровые меню | MIT |
| [Tracy](https://github.com/wolfpld/tracy) (подмодуль, загружается только при `PS_TRACY=ON`) | сборки для профилирования | BSD-3-Clause |
| Заголовок in-application API из RenderDoc (`gpu/third_party/renderdoc`) | захват одного кадра | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.45.0 (`gpu/third_party/httplib`), с бэкендом mbedTLS | сетевой модуль | MIT |
| Мост DLSS (`gpu/dlss_bridge`) из [порта для Windows от Supermedo](https://github.com/Supermedo/bloodborne_pc), адаптированный из [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) | DLSS | MIT |
| [Шрифты DejaVu](https://dejavu-fonts.github.io/) | текст оверлея, запасной вариант для libSceFont | Bitstream Vera (изменения DejaVu — общественное достояние) |
| [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) | сборка ресурсов FSR 4.1.1 на машине пользователя | MIT |

### Библиотеки в архиве релиза

Архив релиза содержит в `lib/` библиотеки, которые psport подключает из Nix (AppImage — в своём
хранилище Nix), каждую под её собственной лицензией: FFmpeg (собранный как LGPL-2.1-or-later, без
кодировщиков под GPL и сетевых протоколов), SDL3, FreeType, fmt, xxHash, glibc, libstdc++ и их
зависимости. Файл `THIRD-PARTY-NOTICES` в архиве перечисляет каждую из них, встроенные при сборке
библиотеки (Boost, xbyak, Zydis, miniz, Vulkan Memory Allocator и др.) и код из таблицы выше, с
лицензией и файлами лицензий из их исходников; `SOURCES.txt` указывает, где лежат исходники каждого
компонента под GPL, LGPL и MPL, и содержит письменное предложение предоставить исходники,
действующее три года. Оба файла создаёт `packaging/licences.nix`. Сборка из исходников подключает те
же библиотеки из системы или окружения Nix (`shell.nix`).

### Сообщество

Патчи Bloodborne от Kyo, Lance McDonald, auser1337, illusion, emoose и других участников сообщества.
Теперь они поставляются с decanter (`crates/ps/data/bloodborne/Bloodborne.xml`), а не с psport,
который читает их из папки, переданной лаунчером (`--builtin-patches-dir`, `PS_BUILTIN_PATCHES_DIR`).

### Что не распространяется

Никаких файлов игр и системных модулей консоли. DLL и данные модели FSR 4 от AMD, а также SDK и
библиотеки DLSS от NVIDIA тоже не входят в проект; их предоставляет пользователь. Ресурсов FSR 4 v07
нет ни в архиве, ни в AppImage: их скачивает на машину пользователя `tools/fetch_fsr4_assets.sh`,
который есть в клоне, в архиве и в AppImage, — в `fsr4_shaders/` рядом с `run.sh`
(`bash <распакованная папка>/tools/fetch_fsr4_assets.sh` для архива) или, с
`PS_FSR4_DIR=<папка данных>/fsr4_shaders`, в папку данных, где их находят `run.sh` и лаунчер;
лаунчер AppImage показывает эту команду для своей копии.
