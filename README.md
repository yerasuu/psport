# psport

Run PS4 games natively on Linux.

**English** · [Español](README.es.md) · [Русский](README.ru.md)

## Overview

psport is a PS4 compatibility layer for x86-64 Linux. It is not an emulator: it works the way Wine
does for Windows programs. The PS4 CPU is x86-64, so a game's original executable runs directly on
the PC's processor, with no instruction translation. psport loads the executable and the modules it
links, provides the system libraries the game imports, and translates the game's GPU work to Vulkan.

psport is an independent project. It started from
[bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of Bloodborne, and
builds on the video core and shader recompiler of [shadPS4](https://github.com/shadps4-emu/shadPS4).
It is being generalised from one game into a layer for any title; Bloodborne remains supported with
its own game profile.

> [!IMPORTANT]
> **No game files are included.** The user needs their own dumps of the games they own.
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware, AMD or the
> publishers of the titles listed below, and it is not related to shadPS4.

## Status

psport is under active development. Every title below is listed in
[tools/regress/titles.txt](tools/regress/titles.txt) and run headless with `tools/regress.sh`
(see [Testing](#testing)). The state is how far a title has been seen to get, not a full
play-through. Rows marked "regress" show only what the title's scripted run records (first GPU
frame, frames drawn, sound).

| Title | Title ID | State |
|---|---|---|
| Bloodborne (1.09) | CUSA03173 | playable: saves, several areas, sound, pad |
| Atelier Ryza | CUSA16458 | title menu, prologue, field with the player in control, sound |
| Atelier Ayesha DX | CUSA16921 | intro movie, title menu, opening scenes, field, atelier, sound |
| Tales of Symphonia Remastered | CUSA50989 | title screen, the first room, sound |
| Astrea | CUSA49515 | the intro's story panels, sound |
| SaGa Emerald Beyond | CUSA43027 | title screen, first story scenes with dialogue, sound |
| ALTDEUS: Beyond Chronos | CUSA26563 | black screen: requires a PS VR headset, no flat-screen mode |
| Gal Guardians: Demon Purge | CUSA41240 | regress: frames drawn, sound |
| Gal Guardians: Servants of the Dark (JP) | CUSA52187 | regress: frames drawn, sound |
| Call of Duty: Black Ops | CUSA57547 | regress: frames drawn, sound |
| Call of Duty: Black Ops II | CUSA57548 | first mission (Pyrrhic Victory) through chained checkpoints to the river barge, Save and Quit, Resume Story, videos, sound, pad |
| Blood Omen 2 | CUSA49769 | regress: frames drawn, no sound |
| Onigiri | CUSA01458 | regress: first GPU frame and sound, no lit frame in its schedule |
| OCTOPATH TRAVELER 0 (JP) | CUSA53477 | loads a saved game, then the GPU faults; left out of routine runs |

- Titles that require PS VR (`require_ps_vr` in `param.sfo`) are not supported.
- No tested title needs a dump of the console's system modules.
- Only one machine has been tested thoroughly: AMD Radeon RX 7800 XT with Mesa (RADV).

## Requirements

- **OS:** Linux on x86-64.
- **GPU:** Vulkan 1.3 with descriptor indexing and buffer device address. The GPU path is the same
  for every vendor and chooses features by what the device reports, not by vendor. It is tested on
  AMD with RADV; NVIDIA and Intel are not verified yet
  ([NVIDIA test guide](docs/en/NVIDIA_TEST.md)).
- **Games:** the user's own dumps of games they own (the game folder with `eboot.bin`, `sce_module/` and
  `sce_sys/param.sfo`).
- **Build tools**, one of:
  - [Nix](https://nixos.org): when dependencies are missing, `build.sh` and `run.sh` enter
    [shell.nix](shell.nix) themselves.
  - Host packages: GCC (or another compiler through `CC`), CMake, Ninja, pkg-config and the
    libraries in `shell.nix` (Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang and the rest of
    shadPS4's video core dependencies; protobuf, nlohmann_json, OpenSSL, zlib and miniupnpc for the
    optional online module).
- **Python 3** only for online play, the bundled launcher and the developer tools; a normal launch
  does not need it.

## Getting started

### With decanter (recommended)

[decanter](https://github.com/yerasuu/decanter) is the recommended front-end. Its PS section
clones psport, builds it with `build.sh`, rebuilds it when the checkout moves and launches each dump
through `run.sh` (titles other than Bloodborne with `PS_PERMISSIVE=1`).

### Build from source

```bash
git clone --recursive https://github.com/yerasuu/psport.git
cd psport
bash build.sh        # produces out/psport and out/gpu/libpsport-gpu.so
```

Without `--recursive`, `build.sh` fetches the submodule (Dear ImGui) itself.

| Build option | Effect |
|---|---|
| `PS_LTO=OFF` | disable link-time optimization (on by default) |
| `PS_PGO=generate\|use\|off` | profile-guided optimization; `use` by default when `pgo/` holds a profile |
| ccache installed | the GPU library compiles through it, one cache for every checkout (`~/.cache/psport/ccache`, or `CCACHE_DIR`) |

`nix-build packaging/port.nix` builds the port from source in Nix's sandbox.

### Run a game

```bash
PS_GAME_DIR=/path/to/CUSA03173 bash run.sh                    # Bloodborne
PS_GAME_DIR=/path/to/dump PS_PERMISSIVE=1 bash run.sh         # any other title
```

`run.sh` builds the port when needed, then starts the game.

| Variable | Effect |
|---|---|
| `PS_GAME_DIR` | the game folder; by default the only dump next to the checkout |
| `PS_PERMISSIVE=1` | imports psport does not implement yet return 0 instead of stopping the game |
| `PS_DATA_DIR` | writable folder for build output (`out/`), saves and shader cache (`user/`) and `psport.ini`; by default the checkout |
| `PS_SYS_MODULES=<folder>` | optionally links the user's own dump of system modules (`*.sprx`) where psport has no library yet; nothing from it is shipped |

`bash run.sh --software` renders with Lavapipe (Mesa's CPU Vulkan driver) instead of the GPU.

In game, Insert or L3+R3 opens the overlay menu. Saves and the shader cache go to `user/`,
settings to `psport.ini`.

### Prebuilt releases

Each version tag `vX.Y.Z` is built by CI and published as a GitHub Release with a bundle that runs
without Nix: `psport-<tag>-x86_64.tar.zst` and its `.sha256`, made by
[tools/bundle.sh](tools/bundle.sh). Extract it anywhere writable and run it with the host's own
Vulkan drivers:

```bash
PS_DATA_DIR=<writable folder> PS_GAME_DIR=/path/to/dump bash <extracted folder>/run.sh
```

The same build is published as a Nix binary cache; see [FORK.md](FORK.md#ci-and-binary-cache).

### Launcher

[launcher/](launcher/) holds the GTK4 launcher inherited from bbport, built around Bloodborne:
`bash launcher/psport-launcher.sh` starts it and `bash launcher/install-desktop.sh` adds it to the
application menu.

## Configuration

Settings live in `psport.ini` in the data folder (`PS_CONFIG` names another file). Most of them
are changed from the overlay menu, which writes the file. Commonly used keys and variables:

| Setting | Effect |
|---|---|
| `output_res` | output resolution; for a title with presets, sizes other than 1080p scale the scene |
| `upscaler` (`PS_UPSCALER`) | temporal upscaler, where the game's profile places it ([upscaler notes](docs/en/upscaler.md)) |
| `menu_language` (`PS_MENU_LANGUAGE`) | overlay language: `en`, `es` or `ru` |
| `game_mouse_sensitivity` | scale of the host mouse motion a game receives (0.05 to 20, default 1) |
| `PS_LANGUAGE` | the system language the game reads (1 English, 3 Spanish, 8 Russian) |
| `PS_FPS` | frame rate preset for titles that have presets (Bloodborne: 30, 60, 90, `uncap`) |
| `PS_ONLINE=1` | online play through the title's community servers ([online play](docs/en/ARCHITECTURE.md#online-play)) |
| `PS_MODS_ENABLED=0` | disables file mods ([mods and patches](docs/en/MODS.md)) |
| `PS_GAME_PROFILE=none` | runs without the game's profile, on the generic paths only |
| `PS_QUIRKS=none,-<id>,+<id>` | turns per-title or per-host fixes off or on ([quirks](docs/en/QUIRKS.md)) |
| `debug` (`PS_DEBUG=1`) | gathers each run's logs in one folder (`PS_DEBUG_DIR`, `PS_DEBUG_KEEP`; [FORK.md, Diagnostics](FORK.md#diagnostics)) |
| `toypad` (`PS_TOYPAD=1`) | plugs in the emulated LEGO Dimensions toy pad, off by default ([toy pad](docs/en/TOYPAD.md)) |
| `skip_movies` (`PS_SKIP_MOVIES=1`) | `key.skip_movie` (Escape) or `pad.skip_movie` (`misc1`) stops the movies playing, off by default ([FORK.md, Keyboard](FORK.md#keyboard)) |

Defaults and why they hold for any game: [FORK.md, Defaults](FORK.md#defaults). Diagnostic
variables and the call report: [FORK.md, Diagnostics](FORK.md#diagnostics). System modules and
fonts (`PS_SYS_MODULES`, `PS_HLE`, `PS_LLE`, `PS_SYS_FONTS`):
[FORK.md, System modules](FORK.md#system-modules).

## How it works

- **Loader.** A C loader reads the eboot and the modules it links (SELF or ELF) straight from the
  game folder, relocates and binds them into one image and jumps into the game's code.
- **System libraries.** psport implements them natively: a C runtime (memory, threads,
  synchronization, files, audio, pad, saves, exceptions, services, user-level threads, modules
  loaded at run time) and the libraries in `gpu/` (GnmDriver, VideoOut, AvPlayer, Videodec2, Ngs2,
  Audiodec, Fiber, Font, PngEnc, PngDec, JpegDec).
- **Graphics.** The GPU command stream and GCN shaders are translated to Vulkan by a renderer
  derived from shadPS4's video core and shader recompiler (`gpu/shadps4/`), as bbport extended it.
- **Game profiles.** What is specific to one title (hooks and optimisations) lives in its profile
  under `gpu/games/`; every title runs on the generic paths without one.
- **Nothing silent.** Before a game starts, every import without an implementation or answered by
  a stub is listed; calls that fail or do no real work are logged and counted in a call report.

Details: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md) (components and launch flow) and
[FORK.md](FORK.md) (what psport takes from other projects and what it changes).

## Development

### Testing

`tools/regress.sh <games root>` runs every title in `tools/regress/titles.txt` in a headless
gamescope, with no window, no sound and none of the user's controllers. A pad schedule
(`tools/regress/*.sched`) drives each title, and the report says how far it got (loaded, entered,
initialized, first GPU frame, frames drawn, audio), its crash signature and how many unimplemented
imports it called. With `-c <earlier results.json>` it flags regressions. Usage and milestones:
[FORK.md, Regression runs](FORK.md#regression-runs).

### Bringing up a title

[docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md) walks through running a new dump, reading
its log and call report, implementing what is missing and checking the result.

### Rules for changes

- Fixes address the underlying behaviour; no game- or shader-specific hacks in the core.
- Optimisations for one title go in its profile (`gpu/games/<game>/`), and every title must run
  correctly without one (`PS_GAME_PROFILE=none`).
- A temporary fix one title needs is a declared, logged and disableable quirk
  ([docs/en/QUIRKS.md](docs/en/QUIRKS.md)).
- No tests in the repository: changes are checked with `tools/regress.sh`; hardware tests, when
  needed, are OpenOrbis homebrew (`tools/hwtest/`).
- Changes match the surrounding code; naming and the other conventions are in [CLAUDE.md](CLAUDE.md).
- Changes from bbport and shadPS4 are ported by hand, each recorded in [FORK.md](FORK.md) with its
  source commit.

`tools/gcn_dis.sh` disassembles GCN shaders.

### Repository layout

| Path | Contents |
|---|---|
| `src/` | entry point (`src/main/`), native loader and C runtime |
| `gpu/` | GPU library: shadPS4 video core with bbport's changes ([gpu/VENDOR.txt](gpu/VENDOR.txt)), PM4 decoder (`cp/`), memory module (`layer/`), ported libraries (`ngs2/`, `audiodec/`, `fiber/`, `font/`, `pngenc/`, `pngdec/`, `jpegdec/`), game profiles (`games/`), overlay and in-game menu (`port/`), DLSS bridge, online module (`net/`) |
| `scripts/` | `online.py` (online addresses for `PS_ONLINE=1`) and `sce_file.py` for the Python tools |
| `launcher/`, `packaging/` | bbport's GTK4 launcher; Nix package and AppImage script |
| `patches/` | community patches for Bloodborne |
| `tools/` | regression runs, GCN disassembler, FSR 4 tools, scripted-run helpers, release bundle |
| `docs/` | `docs/en/` (architecture, bringing up a title, feature notes, bbport's history in English), `docs/ru/` (bbport's Russian originals) |

### Further reading

- [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md): components and the launch flow.
- [docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md): bringing up a new dump.
- [docs/en/QUIRKS.md](docs/en/QUIRKS.md): the registry of fixes for one title, host or middleware.
- [FORK.md](FORK.md): ported components, changes to inherited code, variables, call report,
  regression runs, CI.
- Feature notes: [upscaler](docs/en/upscaler.md), [parallel GPU work](docs/en/parallel_gpu.md),
  [motion vectors](docs/en/motion_vectors.md), [mods and patches](docs/en/MODS.md),
  [profiling](docs/en/PROFILING.md), [toy pad](docs/en/TOYPAD.md).
- [docs/en/history/](docs/en/history/): bbport's design notes, plans and change logs
  (Russian originals in [docs/ru/](docs/ru/)).

## License and credits

psport is licensed under the **GNU General Public License v2.0 or later** ([LICENSE](LICENSE)).
It is maintained by Yerasuu.

### Projects it builds on

| Project | What it provides | License |
|---|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside | psport's origin: native loader, HLE runtime, Vulkan renderer, everything for Bloodborne | GPL-2.0-or-later |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | video core, shader recompiler, AvPlayer and other libraries carried by bbport; source of the libSceFiber and libSceFont ports | GPL-2.0-or-later |
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | source of the libSceNgs2 and libSceAudiodec ports | GPL-2.0 |

Source commits and changes to inherited code are recorded in [FORK.md](FORK.md) and
[gpu/VENDOR.txt](gpu/VENDOR.txt).

### Bundled third-party code

| Component | Used for | License |
|---|---|---|
| [sirit](https://github.com/shadps4-emu/sirit) | SPIR-V emission | BSD-3-Clause |
| AMD GCN headers (`gpu/third_party/gcn`) | GCN instruction decoding | MIT |
| [half](https://half.sourceforge.net/) | half-precision floats | MIT |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn, AMD FidelityFX SDK | FSR 3.1 | MIT |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | ATRAC9 audio decoding | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | overlay and in-game menus | MIT |
| [Tracy](https://github.com/wolfpld/tracy) (submodule, fetched only for `PS_TRACY=ON`) | profiling builds | BSD-3-Clause |
| RenderDoc's in-application API header (`gpu/third_party/renderdoc`) | one-frame captures | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | online module | MIT |
| DLSS bridge (`gpu/dlss_bridge`) from [Supermedo's Windows port](https://github.com/Supermedo/bloodborne_pc), adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) | DLSS | MIT |
| [DejaVu fonts](https://dejavu-fonts.github.io/) | overlay text, fallback for libSceFont | Bitstream Vera (DejaVu changes public domain) |
| [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) | building the FSR 4.1.1 assets on the user's machine | MIT |

Linked from the system or the Nix shell, not bundled, under their own licenses: FFmpeg, FreeType,
Vulkan, SDL3, Boost, fmt and the rest listed in `shell.nix`.

### Community

Bloodborne patches by Kyo, Lance McDonald, auser1337, illusion, emoose and other community members
(`patches/Bloodborne.xml`).

### Not distributed

No game files and no console system modules. AMD's FSR 4 DLLs and model data and NVIDIA's DLSS SDK
and libraries are not included either; the user provides them.
