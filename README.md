# psport — PS4 titles natively on Linux

**English** · [Русский](README.ru.md) · [Español](README.es.md)

psport is a PS4 compatibility layer for Linux. A PS4 title's original executable runs on an
x86-64 Linux PC the way a Windows program runs under Wine: psport loads it, provides the system
libraries it imports and translates its GPU work to Vulkan. It is not an emulator. psport is an
independent project derived from [bbport](https://github.com/deadinside28/bloodborne_pc), the
native Linux port of Bloodborne, and from [shadPS4](https://github.com/shadps4-emu/shadPS4), and
is being generalised into a layer for any title; Bloodborne remains a supported title with its own
profile. bbport and shadPS4 are reviewed from time to time, and the changes useful to psport are
ported by hand and recorded in [FORK.md](FORK.md).

- **CPU.** The PS4 CPU is x86-64, so the game's code runs on the PC's CPU as it is, with no
  emulation and no instruction translation. A C loader (`src/main/main.c`, `src/loader/loader_*.c`) reads the
  eboot and the modules it links (SELF or ELF) straight from the game's folder, relocates and binds
  them into one image and jumps into it.
- **System libraries.** psport provides them itself: the C runtime in `src/*/runtime_*.c` (memory,
  threads, synchronization, files, audio, pad, saves, exceptions, services, user-level threads,
  modules a game loads at run time) and the libraries in `gpu/` (GnmDriver, VideoOut, AvPlayer,
  Videodec2, Ngs2, Audiodec, Fiber, Font, PngEnc, PngDec). No tested title needs a dump of the console's
  system modules.
- **Graphics.** The GPU command stream and GCN shaders are translated to Vulkan by a renderer
  derived from [shadPS4](https://github.com/shadps4-emu/shadPS4)'s video core and shader
  recompiler (`gpu/shadps4/`), as bbport extended it.
- **Game profiles.** What is specific to one title (hooks and optimisations in its code) lives in
  its profile under `gpu/games/`; every title runs on the generic paths without one.

How it fits together: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md). What psport takes from
other projects (source commit, license) and what it changes in inherited code:
[FORK.md](FORK.md).

> **No game files are included.** You need your own dumps of the games you own.
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware, AMD or the
> publishers of the titles below, and it is not related to shadPS4.

## Titles

Every title in `tools/regress/titles.txt` is run headless with `tools/regress.sh` (see below).
The state is how far a title has been seen to get; "regress" rows are what its scripted run shows
(first GPU frame, frames drawn, sound), not a play-through.

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

Titles that require PS VR (`require_ps_vr` in `param.sfo`) are unsupported. Only one machine
(AMD Radeon RX 7800 XT, Mesa/RADV) has been tested thoroughly.

## Using it

Normally through decanter: its PS section clones psport, builds it with `build.sh`, rebuilds it
when the checkout moves and runs each dump through `run.sh` (titles other than Bloodborne with
`PS_PERMISSIVE=1`).

Standalone:

```bash
git clone --recursive https://github.com/yerasuu/psport.git && cd psport
bash build.sh                                   # out/psport and out/gpu/libpsport-gpu.so
PS_GAME_DIR=/path/to/CUSA03173 bash run.sh      # Bloodborne
PS_GAME_DIR=/path/to/dump PS_PERMISSIVE=1 bash run.sh
```

`PS_GAME_DIR` defaults to the only dump next to the checkout (`../CUSA03173`, say).
`PS_PERMISSIVE=1` makes imports psport lacks return 0 instead of stopping. `PS_SYS_MODULES=<folder>` optionally links the user's own dump of system
modules (`*.sprx`) where the port has no library yet; nothing from it is shipped. Saves and the
shader cache go to `user/`, settings to `psport.ini`. The GTK4 launcher
(`bash launcher/psport-launcher.sh`; `launcher/install-desktop.sh` adds it to the app menu) is
inherited from bbport and built around Bloodborne.

## Requirements and building

- Linux x86-64 and a Vulkan 1.3 GPU.
- GCC (or another compiler through `CC`), CMake, Ninja, pkg-config, Python 3 (for online play,
  the bundled launcher and tools; a launch does not need it otherwise) and the packages in
  `shell.nix`: Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang and the rest of shadPS4's video core
  dependencies, plus protobuf, nlohmann_json, OpenSSL, zlib and miniupnpc for the optional online
  module. When these are missing and [Nix](https://nixos.org) is installed, `build.sh` and
  `run.sh` re-enter `shell.nix` themselves.
- Submodule (Dear ImGui): clone with `--recursive`, or `build.sh` runs
  `git submodule update --init --recursive`.
- `PS_LTO=OFF` disables link-time optimization; `PS_PGO=generate|use|off` sets profile-guided
  optimization (`use` by default when `pgo/` holds a profile).
- With [ccache](https://ccache.dev) installed (`shell.nix` has it), the GPU library is compiled
  through it, one cache for every checkout and worktree (`~/.cache/psport/ccache`, `CCACHE_DIR`).
- `nix-build packaging/port.nix` builds the port from source in Nix's sandbox; CI builds every
  commit on `master` and publishes the build of each `vX.Y.Z` tag as a Nix binary cache and a GitHub
  Release, with a bundle that runs without Nix (`psport-<tag>-x86_64.tar.zst`, `tools/bundle.sh`;
  FORK.md, "CI and binary cache").

## Measuring progress

`tools/regress.sh` runs every title in `tools/regress/titles.txt` inside a headless gamescope
(`PS_AUDIO=none`, `PS_PAD_HOST=0`: no window, no sound, none of the user's controllers), drives each
with a pad schedule (`tools/regress/*.sched`) and reports how far it got (loaded, entered,
initialized, first GPU frame, drawn frames, audio), its crash signature and how many imports without
implementation it called; with `-c <earlier results.json>` it flags regressions. Every title runs
without a dump of system modules; `SYS_MODULES=<folder>` optionally passes one to the titles other
than Bloodborne, to compare with psport's own libraries. Usage and milestones:
[FORK.md](FORK.md#regression-runs); bringing up a new title:
[docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md).

## Diagnostics

Before a game starts, every import without implementation or answered by a stub is listed. The
diagnostic variables (`PS_PERMISSIVE_OUT`, `PS_AUDIO_STATS`, `PS_NGS2_TRACE`, `PS_FIBER_TRACE`)
and the system module switches (`PS_SYS_MODULES`, `PS_HLE`, `PS_LLE`, `PS_SYS_FONTS`) are
described in [FORK.md](FORK.md). `tools/gcn_dis.sh` disassembles GCN shaders.

## Rules

From [CLAUDE.md](CLAUDE.md): no game-specific or shader-specific hacks, fixes address the
underlying behavior; optimisations for one title go in its game profile (`gpu/games/`), and every
title must run correctly without one (`PS_GAME_PROFILE=none`); no tests in the repository (changes are checked with `tools/regress.sh`;
hardware tests, when needed, are OpenOrbis homebrew); match the surrounding code.

## Read next

- [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md): components and the launch flow.
- [docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md): bringing up a new dump.
- [FORK.md](FORK.md): ported components, changes to inherited code, variables, call report,
  regression runs, CI.
- Feature notes: [upscaler](docs/en/upscaler.md), [parallel GPU work](docs/en/parallel_gpu.md),
  [motion vectors](docs/en/motion_vectors.md), [mods and patches](docs/en/MODS.md).
- [docs/en/history/](docs/en/history/): bbport's design notes, plans and change logs.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Entry (`src/main/`), native loader (`loader_*.c`) and C runtime (`runtime_*.c`) |
| `scripts/` | `online.py`, the online addresses `run.sh` writes with `PS_ONLINE=1` (the one launch step in Python), and `sce_file.py` for the Python tools |
| `gpu/` | GPU library: shadPS4 video core with bbport's changes (`gpu/VENDOR.txt`), own PM4 decoder (`cp/`) and memory module (`layer/`), ported libraries (`ngs2/`, `audiodec/`, `fiber/`, `font/`, `pngenc/`, `pngdec/`), game profiles (`games/`), overlay and in-game menu (`port/`), DLSS bridge, online module |
| `launcher/`, `packaging/` | bbport's GTK4 launcher; Nix package and AppImage script |
| `patches/` | Community patches for Bloodborne |
| `tools/` | Regression runs, GCN disassembler, FSR 4 tools and `fsr4cap`, scripted-run helpers |
| `docs/` | One folder per language: `docs/en/` (architecture, bringing up a title, feature notes, bbport's history in English), `docs/ru/` (bbport's Russian originals, same file names) |

## Credits and licenses

psport is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)).

### Projects it builds on

| Project | What it provides | License |
|---|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside | psport's origin: native loader, HLE runtime, Vulkan renderer, everything for Bloodborne | GPL-2.0-or-later |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | video core, shader recompiler, AvPlayer and other libraries carried by bbport; source of the libSceFiber and libSceFont ports | GPL-2.0-or-later |
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | source of the libSceNgs2 and libSceAudiodec ports | GPL-2.0 |

psport itself is maintained by Yerasuu for [decanter](#using-it). What psport changes in inherited
code and what it ports (with source commits), and the last reviewed points of bbport and shadPS4,
are recorded in [FORK.md](FORK.md).

### Bundled third-party code

| Component | Used for | License |
|---|---|---|
| [sirit](https://github.com/shadps4-emu/sirit) | SPIR-V emission | BSD-3-Clause |
| AMD GCN headers (`gpu/third_party/gcn`) | GCN instruction decoding | MIT |
| [half](https://half.sourceforge.net/) | half-precision floats | MIT |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn, AMD FidelityFX SDK | FSR 3.1 | MIT |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | ATRAC9 audio decoding | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | overlay and in-game menus | MIT |
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
