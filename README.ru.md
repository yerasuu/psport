# psport — игры PS4 нативно на Linux

[English](README.md) · **Русский**

psport — форк [bbport](https://github.com/deadinside28/bloodborne_pc), нативного порта Bloodborne
для Linux, сделанный для decanter и рассчитанный не на одну игру. Оригинальный исполняемый файл
игры PS4 запускается на ПК с Linux x86-64 так же, как программа для Windows под Wine: это слой
совместимости, а не эмулятор.

- **Процессор.** Процессор PS4 — x86-64, поэтому код игры выполняется на процессоре ПК как есть,
  без эмуляции и без трансляции инструкций. eboot заранее (`scripts/`) превращается в плоский образ
  памяти, в который слинкована libc игры; загрузчик на C (`src/probe.c`) отображает его и передаёт
  ему управление.
- **Системные библиотеки.** Их предоставляет сам порт: рантайм в `src/runtime_*.c` (память,
  потоки, синхронизация, файлы, звук, геймпад, сохранения, исключения, потоки пользовательского
  уровня, модули, которые игра загружает во время работы) и библиотеки в `gpu/` (libSceNgs2,
  libSceAudiodec, libSceFiber, libSceFont). Ни одной из проверенных игр не нужен дамп системных
  модулей консоли.
- **Графика.** Поток команд видеочипа и шейдеры GCN переводятся в Vulkan рендером на основе
  видеоядра и рекомпилятора шейдеров [shadPS4](https://github.com/shadps4-emu/shadPS4)
  (`gpu/shadps4/`) в том виде, как его доработал bbport.

Bloodborne работает так же, как в апстриме, с его патчами, апскейлерами и настройками. Остальные
игры запускаются с `BB_SKIP_GAME_CHECK=1`. Что форк добавляет к bbport, что переносит из других
проектов (коммит источника, лицензия) и что меняет в унаследованном коде shadPS4:
[FORK.md](FORK.md). Релизы апстрима вливаются по мере выхода.

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
| ALTDEUS: Beyond Chronos | CUSA26563 | не запускается: нужен шлем PS VR, режима для обычного экрана нет |

Игры, которым обязателен PS VR (`require_ps_vr` в `param.sfo`), не поддерживаются. Основательно
проверена одна машина (AMD Radeon RX 7800 XT, Mesa/RADV).

## Использование

Обычно через decanter: его раздел PS клонирует psport, собирает его через `build.sh`, пересобирает,
когда checkout сдвинулся, и запускает каждый дамп через `run.sh` (все игры, кроме Bloodborne, — с
`BB_SKIP_GAME_CHECK=1 BB_PERMISSIVE=1`).

Отдельно:

```bash
git clone --recursive https://github.com/yerasuu/psport.git && cd psport
bash build.sh                                   # out/bb-probe и out/gpu/libbbgpu.so
BB_GAME_DIR=/путь/к/CUSA03173 bash run.sh       # Bloodborne
BB_GAME_DIR=/путь/к/дампу BB_SKIP_GAME_CHECK=1 BB_PERMISSIVE=1 bash run.sh
```

По умолчанию `BB_GAME_DIR` — `../CUSA03173`. С `BB_PERMISSIVE=1` импорты, которых нет в порте,
возвращают 0, а не останавливают игру. `BB_SYS_MODULES=<папка>` по желанию подключает
собственный дамп системных модулей (`*.sprx`) там, где у порта ещё нет своей библиотеки; из него
ничего не распространяется. Сохранения и кэш шейдеров лежат в `user/`, настройки — в `bbport.ini`.
Лаунчер на GTK4 (`bash launcher/bb-launcher.sh`; `launcher/install-desktop.sh` добавляет его в меню
приложений) взят из апстрима и рассчитан на Bloodborne.

## Требования и сборка

- Linux x86-64 и видеокарта с Vulkan 1.3.
- GCC (или другой компилятор через `CC`), CMake, Ninja, pkg-config, Python 3 и пакеты из
  `shell.nix`: Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang и остальные зависимости видеоядра
  shadPS4, а также protobuf, nlohmann_json, OpenSSL, zlib и miniupnpc для необязательного сетевого
  модуля. Если их нет, а [Nix](https://nixos.org) установлен, `build.sh` и `run.sh` сами
  перезапускаются в `shell.nix`.
- Подмодули (FSR-Vulkan, Dear ImGui, LibAtrac9): клонируйте с `--recursive`, иначе `build.sh`
  выполнит `git submodule update --init --recursive`.
- `BB_LTO=OFF` отключает оптимизацию при линковке; `BB_PGO=generate|use|off` задаёт оптимизацию по
  профилю (по умолчанию `use`, если в `pgo/` есть профиль).

## Bloodborne

Нужен свой расшифрованный дамп с обновлением 1.09 (папка обновления копируется поверх базовой
игры): у всех изданий из магазина (CUSA03173, CUSA00900, CUSA03023, ...) один и тот же
исполняемый файл. `run.sh` его проверяет (`scripts/game_check.py`): базовая игра без обновления
(1.00) падает при запуске, а eboot со встроенным патчем 60 fps падает в меню жестов.

- **Модели памяти.** `BB_PC_MODEL=1/0`, без него — по видеокарте (*Авто* в лаунчере). Новая
  модель (по умолчанию на AMD): память игры остаётся в системной RAM, видеочип читает её на месте
  и держит часто читаемые данные в VRAM, а записи в память, DMA и fence командного процессора
  транслируются в команды Vulkan. Старая модель, как в bbport 0.3 (по умолчанию на остальных):
  копии в VRAM, согласованные через отслеживание записи защитой страниц. `BB_LAYER_MEMORY=1/0`
  выбирает в новой модели между разреженной ареной и модулем памяти слоя.
- **Частота кадров.** Патчи сообщества (`patches/Bloodborne.xml`, компилируются при запуске
  `scripts/patches.py`): `BB_FPS=uncap` (по умолчанию, патч на реальное время кадра), `90`, `60`
  или `30` (без патча).
- **Апскейлинг.** В Bloodborne нет буфера скоростей, поэтому порт сам вычисляет векторы движения
  (камера — по глубине и матрицам сцены, объекты — по вершинам прошлого кадра) и сдвигает сцену
  джиттером. `BB_UPSCALER=taa|fsr3|fsr4|fsr411|off|none`:
  - FSR 3.1 (FSR-Vulkan), без дополнительных файлов;
  - FSR 4 (INT8, модель v07): `bash tools/fetch_fsr4_assets.sh`;
  - FSR 4.1.1: DLL AMD один раз записывается под vkd3d-proton и воспроизводится на Vulkan,
    побитово совпадая с DLL; файлы собираются из своей `amd_fidelityfx_upscaler_dx12.dll` 4.1.x
    командой `bash tools/fsr4cap/build_assets.sh <dll>` (GE-Proton 10 или новее) или кнопкой
    лаунчера *Выбрать DLL…*; на видеокартах с `VK_EXT_shader_float8` используется вариант FP8
    (`BB_FSR411_VARIANT=int8` включает INT8);
  - DLSS на NVIDIA RTX: сборка с `DLSS_SDK_ROOT=<checkout NVIDIA/DLSS> bash build.sh`
    (`gpu/dlss_bridge`); SDK не входит в репозиторий.

  FSR 4 и 4.1.1 требуют, среди прочего, шейдерных Float16, Int8/Int16 и целочисленных скалярных
  произведений; недоступный выбор откатывается на FSR 3.1. Подробнее:
  [docs/upscaler.md](docs/upscaler.md), [docs/motion_vectors.md](docs/motion_vectors.md).
- **Разрешение.** При выводе не в 1080p (720p, 1440p, 2160p) вся игра рендерится в размере
  пресета через патч при запуске, поэтому их смена требует перезапуска. Смена разрешения на лету
  (`bbport.ini` `live_resolution=0|1|auto`, `BB_LIVE_RES=1`) обходится без перезапуска, но
  дороже.
- **Настройки.** В меню самой игры (*System → Display / Effects*, в русской версии *Изображение / Эффекты*), в оверлее (Insert или L3+R3) и
  в лаунчере.
- **Моды, патчи, DLC.** Папки модов из отдельных файлов и XML-патчи в формате shadPS4, и то и
  другое — из лаунчера ([docs/MODS.md](docs/MODS.md)); дополнения — в
  `user/addcont/<title id>/<метка>/`.
- **Сеть.** Необязательный `out/gpu/libbbnet.so` (`gpu/bbnet`, клиент shadNet) собирается, если
  есть его библиотеки, и загружается только с `BB_ONLINE=1`.

Остальные переменные (`BB_FRAME_STATS`, `BB_GPU_PROFILE`, `BB_ANISO`, `BB_VRAM_LIMIT_MB`, ...)
описаны там, где их читают `run.sh` и `gpu/`. Многопоточность видеочасти:
[docs/parallel_gpu.md](docs/parallel_gpu.md).

## Измерение прогресса

`tools/regress.sh` запускает каждую игру из `tools/regress/titles.txt` в gamescope без окна
(`BB_AUDIO=none`, `BB_PAD_HOST=0`: ни окна, ни звука, ни геймпадов пользователя), ведёт её по
расписанию нажатий (`tools/regress/*.sched`) и сообщает, докуда она дошла (загружена, вход в код
игры, инициализирована, первый кадр видеочипа, нарисованные кадры, звук), её сигнатуру падения и
число импортов без реализации; с `-c <прежний results.json>` отмечает регрессии. Запуск и
вехи: [FORK.md](FORK.md#regression-runs).

## Диагностика

Перед запуском игры перечисляются все импорты без реализации или с заглушкой. Диагностические
переменные (`BB_PERMISSIVE_OUT`, `BB_AUDIO_STATS`, `BB_NGS2_TRACE`, `BB_FIBER_TRACE`) и
переключатели системных модулей (`BB_SYS_MODULES`, `BB_HLE`, `BB_LLE`, `BB_SYS_FONTS`) описаны в
[FORK.md](FORK.md). `tools/gcn_dis.sh` дизассемблирует шейдеры GCN.

## Правила

Из [CLAUDE.md](CLAUDE.md): никаких обходов под конкретные игры или шейдеры, исправления
устраняют саму причину; тестов в репозитории нет (изменения проверяются через `tools/regress.sh`,
аппаратные тесты, если нужны, — homebrew на OpenOrbis); код следует соглашениям окружающего кода.

## Структура репозитория

| Путь | Содержимое |
|---|---|
| `src/` | Загрузчик (`probe.c`) и рантайм (`runtime_*.c`) |
| `scripts/` | Подготовка образа игры, линковка модулей, проверка игры, компилятор патчей, моды |
| `gpu/` | Библиотека рендера: видеоядро shadPS4 с изменениями bbport (`gpu/VENDOR.txt`), перенесённые библиотеки (`ngs2/`, `audiodec/`, `fiber/`, `font/`), оверлей и меню в игре (`shim/`), мост DLSS, сетевой модуль |
| `launcher/`, `packaging/` | Лаунчер на GTK4 из апстрима; пакет Nix и скрипт AppImage |
| `patches/` | Патчи сообщества для Bloodborne |
| `tools/` | Регрессионные прогоны, дизассемблер GCN, инструменты FSR 4 и `fsr4cap`, помощники для скриптовых запусков |
| `docs/` | Заметки апстрима о дизайне, замеры и списки изменений |

## Благодарности и лицензии

Лицензия — **GNU GPL v2 или новее** ([LICENSE](LICENSE)).
[bbport](https://github.com/deadinside28/bloodborne_pc) — апстрим, на котором построен этот форк.
Сторонние компоненты сохраняют свои лицензии: видеоядро и рекомпилятор шейдеров
[shadPS4](https://github.com/shadps4-emu/shadPS4) и перенесённые из него libSceFiber и libSceFont
(GPL-2.0-or-later); libSceNgs2 и libSceAudiodec, перенесённые из
[AnyPS5](https://github.com/boykopovar/AnyPS5) (GPL-2.0); [sirit](https://github.com/shadps4-emu/sirit),
[half](https://half.sourceforge.net/), [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) от
FireBurn (MIT), AMD FidelityFX SDK (MIT), [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)
(MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), FreeType, шрифты DejaVu,
[dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT, для сборки файлов FSR 4.1.1).
Патчи Bloodborne — Kyo, Lance McDonald, auser1337, illusion, emoose и другие участники сообщества
(`patches/Bloodborne.xml`). Мост DLSS (`gpu/dlss_bridge`, MIT) взят из
[Windows-порта Supermedo](https://github.com/Supermedo/bloodborne_pc), адаптированного из
[IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR).
DLL и данные моделей FSR 4 от AMD, а также SDK и библиотеки DLSS от NVIDIA здесь не
распространяются.
