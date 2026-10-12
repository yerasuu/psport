# psport — PS4 titles natively on Linux

**English** · [Русский](README.ru.md)

psport is decanter's fork of [bbport](https://github.com/deadinside28/bloodborne_pc), the native
Linux port of Bloodborne, taken past one game. A PS4 title's original executable runs on an x86-64
Linux PC the way a Windows program runs under Wine: a compatibility layer, not an emulator.

- **CPU.** The PS4 CPU is x86-64, so the game's code runs on the PC's CPU as it is, with no
  emulation and no instruction translation. The eboot is converted offline (`scripts/`) into a flat
  memory image with the game's libc linked in; a C loader (`src/probe.c`) maps it and jumps into it.
- **System libraries.** The port provides them itself: the runtime in `src/runtime_*.c` (memory,
  threads, synchronization, files, audio, pad, saves, exceptions, user-level threads, modules a
  game loads at run time) and the libraries in `gpu/` (libSceNgs2, libSceAudiodec, libSceFiber,
  libSceFont). No tested title needs a dump of the console's system modules.
- **Graphics.** The GPU command stream and GCN shaders are translated to Vulkan by a renderer
  derived from [shadPS4](https://github.com/shadps4-emu/shadPS4)'s video core and shader
  recompiler (`gpu/shadps4/`), as bbport extended it.

Bloodborne keeps working as upstream ships it, with its patches, upscalers and settings. Other
titles start with `BB_SKIP_GAME_CHECK=1`. What the fork adds to bbport, what it ports from other
projects (source commit, license) and what it changes in inherited shadPS4 code:
[FORK.md](FORK.md). Upstream releases are merged in as they come.

> **No game files are included.** You need your own dumps of the games you own.
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware, AMD or the
> publishers of the titles below, and it is not related to shadPS4.

## Titles

Tested headless with `tools/regress.sh` (see below): six of seven reach gameplay with sound.

| Title | Title ID | State |
|---|---|---|
| Bloodborne (1.09) | CUSA03173 | playable: saves, several areas, sound, pad |
| Atelier Ryza | CUSA16458 | title menu, prologue, field with the player in control, sound |
| Atelier Ayesha DX | CUSA16921 | intro movie, title menu, opening scenes, field, atelier, sound |
| Tales of Symphonia Remastered | CUSA50989 | title screen, the first room, sound |
| Astrea | CUSA49515 | the intro's story panels, sound |
| SaGa Emerald Beyond | CUSA43027 | title screen, first story scenes with dialogue, sound |
| ALTDEUS: Beyond Chronos | CUSA26563 | black screen: requires a PS VR headset, no flat-screen mode |

Titles that require PS VR (`require_ps_vr` in `param.sfo`) are unsupported. Only one machine
(AMD Radeon RX 7800 XT, Mesa/RADV) has been tested thoroughly.

## Using it

Normally through decanter: its PS section clones psport, builds it with `build.sh`, rebuilds it
when the checkout moves and runs each dump through `run.sh` (titles other than Bloodborne with
`BB_SKIP_GAME_CHECK=1 BB_PERMISSIVE=1`).

Standalone:

```bash
git clone --recursive https://github.com/yerasuu/psport.git && cd psport
bash build.sh                                   # out/bb-probe and out/gpu/libbbgpu.so
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh      # Bloodborne
BB_GAME_DIR=/path/to/dump BB_SKIP_GAME_CHECK=1 BB_PERMISSIVE=1 bash run.sh
```

`BB_GAME_DIR` defaults to `../CUSA03173`. `BB_PERMISSIVE=1` makes imports the port lacks return 0
instead of stopping. `BB_SYS_MODULES=<folder>` optionally links the user's own dump of system
modules (`*.sprx`) where the port has no library yet; nothing from it is shipped. Saves and the
shader cache go to `user/`, settings to `bbport.ini`. The GTK4 launcher
(`bash launcher/bb-launcher.sh`; `launcher/install-desktop.sh` adds it to the app menu) is
upstream's and built around Bloodborne.

## Requirements and building

- Linux x86-64 and a Vulkan 1.3 GPU.
- GCC (or another compiler through `CC`), CMake, Ninja, pkg-config, Python 3 and the packages in
  `shell.nix`: Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang and the rest of shadPS4's video core
  dependencies, plus protobuf, nlohmann_json, OpenSSL, zlib and miniupnpc for the optional online
  module. When these are missing and [Nix](https://nixos.org) is installed, `build.sh` and
  `run.sh` re-enter `shell.nix` themselves.
- Submodules (FSR-Vulkan, Dear ImGui, LibAtrac9): clone with `--recursive`, or `build.sh` runs
  `git submodule update --init --recursive`.
- `BB_LTO=OFF` disables link-time optimization; `BB_PGO=generate|use|off` sets profile-guided
  optimization (`use` by default when `pgo/` holds a profile).

## Measuring progress

`tools/regress.sh` runs every title in `tools/regress/titles.txt` inside a headless gamescope
(`BB_AUDIO=none`, `BB_PAD_HOST=0`: no window, no sound, none of the user's controllers), drives
each with a pad schedule (`tools/regress/*.sched`) and reports how far it got (loaded, entered,
initialized, first GPU frame, drawn frames, audio), its crash signature and its count of imports
without implementation; with `-c <earlier results.json>` it flags regressions. All seven run
without a dump of system modules; `SYS_MODULES=<folder>` optionally passes one to the titles other
than Bloodborne, to compare with the port's own libraries. Usage and milestones: [FORK.md](FORK.md#regression-runs).

## Diagnostics

Before a game starts, every import without implementation or answered by a stub is listed. The
diagnostic variables (`BB_PERMISSIVE_OUT`, `BB_AUDIO_STATS`, `BB_NGS2_TRACE`, `BB_FIBER_TRACE`)
and the system module switches (`BB_SYS_MODULES`, `BB_HLE`, `BB_LLE`, `BB_SYS_FONTS`) are
described in [FORK.md](FORK.md). `tools/gcn_dis.sh` disassembles GCN shaders.

## Rules

From [CLAUDE.md](CLAUDE.md): no game-specific or shader-specific hacks, fixes address the
underlying behavior; no tests in the repository (changes are checked with `tools/regress.sh`;
hardware tests, when needed, are OpenOrbis homebrew); match the surrounding code.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and runtime (`runtime_*.c`) |
| `scripts/` | Game image preparation, module linking, game check, patch compiler, mods |
| `gpu/` | Renderer library: shadPS4 video core with bbport's changes (`gpu/VENDOR.txt`), ported libraries (`ngs2/`, `audiodec/`, `fiber/`, `font/`), overlay and in-game menu (`shim/`), DLSS bridge, online module |
| `launcher/`, `packaging/` | Upstream's GTK4 launcher; Nix package and AppImage script |
| `patches/` | Community patches for Bloodborne |
| `tools/` | Regression runs, GCN disassembler, FSR 4 tools and `fsr4cap`, scripted-run helpers |
| `docs/` | Upstream's design notes, measurements and change logs |

## Credits and licenses

psport is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)).

### Projects it builds on

| Project | What it provides | License |
|---|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside | the upstream: native loader, HLE runtime, Vulkan renderer, everything for Bloodborne | GPL-2.0-or-later |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | video core, shader recompiler, AvPlayer and other libraries carried by bbport; source of the libSceFiber and libSceFont ports | GPL-2.0-or-later |
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | source of the libSceNgs2 and libSceAudiodec ports | GPL-2.0 |

psport itself is maintained by Yerasuu for [decanter](#using-it). What the fork changes in inherited
code and what it ports (with source commits) is recorded in [FORK.md](FORK.md).

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
