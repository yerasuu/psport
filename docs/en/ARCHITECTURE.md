# Architecture

psport runs a PS4 title's own x86-64 code on a Linux PC. It does not emulate the CPU: the game's
executable and the modules it ships are loaded into one process and run natively, the PS4 system
libraries they import are provided by psport, and the game's GPU work is translated to Vulkan.
The model is Wine's, not an emulator's.

## Components

| Component | Path | Built into | Role |
|---|---|---|---|
| Launch script | `run.sh` | - | environment and settings, then `psport` |
| Entry | `src/main/main.c`, `src/main/main.h` | `out/psport` | launch sequence: arguments, image mapping, entering the game |
| Launcher queries | `src/main/cli.c`, `src/main/game_info.c` | `out/psport` | `--check-game`, `--has-presets`, `--list-patches`, `--list-mods`, `--list-quirks`, `--list-traits`, `--list-effects`, `--online-overrides`, `BB_*` names; `param.sfo` and the game's identity |
| Import binding | `src/main/bind.c` | `out/psport` | traps, failure thunks, relocation walk, patch writes |
| Signals | `src/main/signals.c` | `out/psport` | fault, watchdog, thread dump and termination handlers |
| Native loader | `src/loader/loader.h`, `src/loader/loader_self.c`, `src/loader/loader_link.c`, `src/loader/loader_traits.c` | `out/psport` | SELF/ELF parsing, module linking, relocations, TLS, bindings; middleware traits (`Middleware:` line) |
| Launch data | `src/loader/loader_mods.c`, `src/loader/loader_patches.c`, `src/loader/loader_xml.c`, `src/loader/loader_hash.c`, `src/loader/loader_json.c` | `out/psport` | loose-file mod view, shadPS4/GoldHEN XML patches, the eboot's identity (SHA-256), `mods.json`/`patches.json` |
| Quirk registry | `src/runtime/runtime_quirks.inc`, `runtime_quirks.h`, `runtime_quirks.c`, `gpu/port/psport_quirks.h` | `out/psport` | fixes for one title, host or middleware, decided once and read at setup ([QUIRKS.md](QUIRKS.md)) |
| C runtime | `src/*/runtime*.c`, `src/runtime/runtime.h`, `src/runtime/imports/import_*.inc` | `out/psport` | libkernel and the other system libraries, import resolution, the call report |
| GPU library | `gpu/port/psport_gpu*.cpp`, `gpu/psport_gpu.h` | `out/gpu/libpsport-gpu.so` | C interface between `psport` and the C++ side |
| Translator | `gpu/shadps4/` (inherited from shadPS4, `gpu/VENDOR.txt`), `gpu/cp/`, `gpu/layer/` | `libpsport-gpu.so` | GnmDriver, VideoOut, command processor, Vulkan rasterizer, caches, shader recompiler |
| Ported libraries | `gpu/ngs2/`, `gpu/audiodec/`, `gpu/fiber/`, `gpu/font/`, `gpu/pngenc/`, `gpu/pngdec/` | `libpsport-gpu.so` | libSceNgs2, libSceAudiodec, libSceFiber, libSceFont/FontFt, libScePngEnc, libScePngDec |
| Port services | `gpu/port/`, `gpu/memory/`, `gpu/diag/` | `libpsport-gpu.so` | settings (`psport.ini`), overlay, submit guard, guest memory, diagnostics |
| Game profiles | `gpu/games/` | `libpsport-gpu.so` | per-title launch and game data (`Game::Title`) and translator optimisations (`Game::Profile`) |
| Online module | `gpu/net/` | `out/gpu/` (optional) | network and PSN imports with `PS_ONLINE=1` |
| Online addresses | `scripts/online.py` | - | Python run by `run.sh` with `PS_ONLINE=1` only; see [Launch scripts](#launch-scripts) |
| Regression runs | `tools/regress.sh`, `tools/regress/` | - | headless runs of every title |

`build.sh` builds `libpsport-gpu.so` with CMake/Ninja, then compiles `src/main/*.c`, `src/loader/loader*.c` and
`src/*/runtime*.c` with `-Werror` into `out/psport`, linked against `libpsport-gpu.so`.

## Launch flow

```
run.sh
  environment: memory model, scene size for the output, driver caches, ...
  bash build.sh (unless PS_PREBUILT=1)
  scripts/online.py (PS_ONLINE=1 only)
  psport --game <dir> [--system-modules <dir> --hle <libs> --lle <libs>]
           [--mods <dir> --mods-config <file> --out <dir>]
           --patches-dir <dir> --patches-config <file> --app0 <dir> --user <dir>
    loader_mods_view()                the game folder, or a view of it with the mods linked in
    AppContent profile                param.sfo's user parameters, PS_CONTENT_SKU
    psport_gpu_register_kernel()           event queues and the ported libraries register their exports
    loader_load_game()                eboot + modules -> one LoaderImage
    psport_gpu_title()                     the title's launch data (PsTitleInfo), whatever PS_GAME_PROFILE says
    runtime_quirks_evaluate()              PS_QUIRKS, the title's quirks, published (QUIRKS.md)
    pfn_prepare_environment                the title's environment defaults (PS_VBLANK_HZ)
    pfn_check_files                        the title's check of the game's files (exit 2)
    pfn_add_patches                        the title's own patches, then the external XML patches
    runtime_start()
    psport_gpu_init()                      game title and profile, settings, saves, window thread, GnmDriver, VideoOut
    bind every import                 native binding or runtime_resolve() or trap
    apply the patches, PsTitleInfo's pfn_patch_image
    protect segments, set TLS, start linked modules in link order
    enter the eboot's entry on an 8 MiB stack below 1 TiB
```

### run.sh

`run.sh` takes the game folder from `PS_GAME_DIR` (default `../CUSA03173`) and a writable data
directory from `PS_DATA_DIR` (default the checkout: `out/`, `user/`, `psport.ini`). It re-enters
`shell.nix` when the build dependencies are missing and Nix is installed, and builds with
`build.sh` unless `PS_PREBUILT=1`. Before starting `psport` it:

- passes the mods folder and `mods.json` (`PS_MODS_DIR`, `PS_MODS_CONFIG`; none with
  `PS_MODS_ENABLED=0`) and the external patch folder and `patches.json` (`PS_PATCHES_DIR`,
  `PS_PATCHES_CONFIG`), by default in the data directory;
- for a `psport.ini` `output_res` other than 1080p, chooses between live resolution changes and a
  scene size patched in at start (`PS_RENDER_RES`, `PS_OUTPUT_RES`, `PS_DMEM_MB`): the output
  divided by the upscaler preset's ratio, rounded half to even to an even size;
- chooses driver workarounds from the GPU's `HOST` quirks
  (`out/psport-gpu-capabilities --host-env`, [QUIRKS.md](QUIRKS.md)), see [Memory model](#memory-model);
- passes `PS_SYS_MODULES`, `PS_HLE` and `PS_LLE` as `--system-modules`, `--hle` and `--lle`.

### psport

`src/main/main.c` checks the CPU, builds the mods' view of the game folder when `--mods` is given,
reads `param.sfo` (title, title ID, save directory, SDK version, attributes, the AppContent user
parameters; `PS_CONTENT_SKU=full` or `trial` sets the SKU), configures the file mounts (`/app0` is
the game folder, saves under `user/`), loads the game, lets its title check the game's files and
choose its patches, reads the external patches, binds its imports, installs the fault (`SIGSEGV`, `SIGILL`, `SIGBUS`, `SIGFPE`), dump
(`SIGUSR2`) and watchdog (`SIGALRM`, `--timeout`) handlers and jumps to the eboot's entry. Host
heap objects handed to the guest stay below 1 TiB (`mallopt` keeps glibc in the non-PIE brk heap):
PS4 code packs pointers into 40 bits. Guest memory is placed there too.

`sceSystemServiceLoadExec` replaces the application the way the console does: `psport` starts
again on its own command line (`src/runtime/runtime_restart.c`) with `--exec <guest path>` and one
`--arg <value>` per argument, the loader takes that executable of the package instead of
`eboot.bin`, the game's entry, `getargv` and `__progname` see the new command line, and the
patches (the title's and the external XML ones) are applied only to `/app0/eboot.bin`. The
command line keeps `--mods`, so the new process builds its own mods' view; the old one's goes
with the exec (its cleaner's pipe is close-on-exec).

### Native loader

`loader_load_game` (`src/loader/loader_link.c`) builds one image, the eboot at offset 0 and every module
at the next 64 KiB boundary after the previous one, as one table of imports:

- `src/loader/loader_self.c` reads SELF and plain ELF files: program headers, dynamic section, the
  module and library tables, symbols (NID, library, module) and relocations. Only a segment's
  permission bits become its protection (some system modules set high bits in `p_flags`).
- Modules linked: the game's `sce_module/libc.prx` and `libSceFios2.prx` always; the other
  `sce_module/*.prx` once something linked imports their library; the user's system modules
  (`--system-modules`, `*.sprx`) the same way; and every `*.prx`/`*.sprx` outside `sce_module`
  and `sce_sys`, which the game loads itself with `sceKernelLoadStartModule`. Those are linked,
  relocated and bound at start but left unstarted; loading the path later runs their initializer
  once (`src/kernel/runtime_module.c`).
- Exports are matched by (NID, library, module). Imports of `libSceLibcInternal` are served by
  `libc.prx`'s exports with the same NID.
- Initial-exec TLS loads `mov rax, fs:[0]` are rewritten to GS, because glibc owns FS on Linux.
  The eboot's `PT_TLS` and each module's TLS block get module IDs for `__tls_get_addr`.
- The result (`LoaderImage`, `src/loader/loader.h`): segments, relocations (to an image offset, or to an
  import index for functions and data), import names `NID#library#module`, native bindings with
  a kind, the module table (`RuntimeModule`: init, TLS, `.eh_frame`, exports) and procparam.

System module rules, from `src/loader/loader_link.c`:

| Library | Rule |
|---|---|
| `libSceLibcInternal` | never taken from the system dump: the game's `libc.prx` serves it, so the runtime's replacements (`memcpy`, `memset`) keep tracking writes to GPU memory |
| `libkernel`, `libkernel_unity`, `libkernel_sys`, `libSceGnmDriver`, `libSceVideoOut`, `libScePad`, `libSceAudioOut`, `libSceFiber`, `libSceFont`, `libSceFontFt`, `libSceAudiodec`, `libSceNgs2`, `libSceUlt`, `libSceRtc` | implemented by psport (`port_implements`): the system module is skipped unless `--lle` (`PS_LLE`) names it |
| a library named in `--hle` (`PS_HLE`) | system module skipped, psport's implementation used |
| any other library a linked module imports | the bundled `sce_module` copy, else the system dump's; its exports win over psport's (binding kinds 3 and 4) |

System modules are the user's own dump; nothing from them is in the repository or in a build. No
title in `tools/regress/titles.txt` needs them; they are a stopgap until psport implements the
library.

### Launch data

- Mods (`src/loader/loader_mods.c`, [MODS.md](MODS.md)): with `--mods`, the enabled mods' files are linked
  over links to the game's entries in a private folder under `--out` (`mod-game-*`), which becomes
  `/app0` and the folder the loader reads. A mod's wrapper folder may be named after any title ID of
  the game's executable (`PsTitleInfo` `pp_serials`), and a mod without `dvdroot_ps4` may consist of the
  folders of the game's own `dvdroot_ps4` and its title's `mod_folders`. A process of its own
  (`mod-view-clean`) removes the view once `psport` ends, by any route.
- Patches (`src/loader/loader_patches.c`, read by `src/loader/loader_xml.c`): shadPS4/GoldHEN XML, `bytes`, `bytes16/32/64`, `float32/64`,
  `utf8` and `utf16` lines at the eboot's addresses (base `0x400000`). The title's own patches
  come first (`PsTitleInfo` `pfn_add_patches`, through `PsPatchServices`); then the files in `--patches-dir`
  whose `<ID>`s name one of the game's title IDs (or that have none), entries for its `APP_VER` and
  `eboot.bin`, selected by `patches.json` or their `isEnabled`. A patch with an unsupported line or
  an address outside the eboot's `PT_LOAD` segments is skipped whole, and said; the title's own
  file is left out of the folder.
- The eboot's identity (`src/loader/loader_hash.c`): the SHA-256 of its loadable segments at their
  addresses, the same whatever tool dumped the game; computed when a title's check asks for it.
- For launchers, without starting the game: `psport --list-mods <dir> [--game <dir>]` and
  `psport --list-patches <dir> --game <dir>` print JSON (instead of `--game`, `--title <TITLE_ID>
  [--version <APP_VER>]` names a title whose folder the launcher does not have at hand) (mod folder names; key, name, author, note,
  default of each external patch); `psport --check-game <dir>` runs the title's file check and
  prints `kind<TAB>title<TAB>version` with exit status 2 when the files cannot work;
  `psport --list-traits --game <dir>` prints the game's middleware traits as JSON (`id`, `name`);
  `psport --list-effects (--game <dir> | --title <TITLE_ID>)` prints the overlay's sections of the
  title's own effects as JSON, `[]` for a title without any (`psport_gpu_title_effects`): each
  section (`"section": "game_effects"`) has `model_detail` (the `model_lod` choice comes first),
  an optional `hint`, and `effects` in the overlay's order, each with its `psport.ini` `key`, its
  `default` (the game's unpatched behaviour), a `name` and an optional `description`; texts are
  objects by language (`en`, `ru`, `es`, those the title has).
- Middleware traits (`src/loader/loader_traits.c`): read once at load from the eboot's module and
  library names (`PS4Player*`, `Il2CppUserAssemblies`, `libkernel_unity`, `<Project>-PS4-Shipping`,
  `libfmod*`), Havok's class names in the eboot, and the game's file names (`PS4Util.prx`,
  `*eeaot*.prx`, `Media/Managed/mscorlib.dll`, `uecommandline.txt`, `Paks/*.pak`, CRI
  `.acb/.awb/.hca/.adx`, `.usm`, `.cpk`, FMOD `.fev/.fsb/*.strings.bank`, Wwise `Init.bnk`,
  `.hkx`), logged as `Middleware: <names>` (or `none detected`) and kept for `loader_game_traits()`.
  Informational: nothing changes behaviour by them yet.

### Import binding

`psport` walks the relocations. For each import (`src/main/bind.c`):

1. a binding of kind 3-6 (a system module, or a module the game loads) wins;
2. otherwise `runtime_resolve(name)` (C runtime, then the GPU library);
3. otherwise a native binding of kind 1-2 (a module the game ships);
4. otherwise a trap: calling it stops the game with the import's name, or with `PS_PERMISSIVE=1`
   returns 0 (`PS_PERMISSIVE_OUT=error`: `0x80020016` for imports with output arguments).

Data imports nothing provides point at a page that faults on access. Imports resolved to
`runtime_stub` go through a trap that logs them as stubs; imports whose result can report a
failure (`src/runtime/imports/import_returns.inc`) go through a thunk that logs failures. Before entering the
game, `psport` prints every stub and every import without implementation, then one summary line:
`Runtime: imports N: host N, game modules N, system modules N, modules the game loads N, stubs N,
without implementation N`.

After binding: the patches are applied (pointer writes into relocated slots are rebased), the
profile's and the title's `patch_image` run, segments get their protections, TLS is set up, linked modules start
in link order (libc first) and the eboot's entry runs with `argv[0] = /app0/eboot.bin`.

## C runtime

`src/*/runtime*.c` implements the system libraries in C, one file per area:

| Area | Files |
|---|---|
| process, libc hooks, TLS, procparam, `runtime_resolve` | `runtime.c`, `runtime.h` |
| the libraries' resolver table, NID names, stubs | `runtime_resolve.c` |
| restarts: the settings menu's, `sceSystemServiceLoadExec` | `runtime_restart.c` |
| settings: `psport.ini` read once for every reader (controls, mouse, the menu's settings, a title's patches; `PS_CONFIG`, else `psport.ini` in the working directory), `PS_*` variables (`runtime_env`) | `runtime_config.c` |
| kernel misc, process, time | `runtime_kernel.c`, `runtime_rtc.c` |
| memory: direct, flexible, mappings, pools, write traps, GPU interface | `runtime_memory.c`, `runtime_memory_kernel.c`, `runtime_memory_gpu.c`, `runtime_mempool.c` |
| threads and synchronisation | `runtime_thread.c`, `runtime_sched.c`, `runtime_mutex.c`, `runtime_rwlock.c`, `runtime_sema.c`, `runtime_eventflag.c`, `runtime_barrier.c`, `runtime_ult.c`, `runtime_ulobjmgr.c` |
| exceptions | `runtime_exception.c` |
| modules loaded at run time | `runtime_module.c` |
| files, async I/O, AppContent, PlayGo, saves | `runtime_file.c`, `runtime_aio.c`, `runtime_content.c`, `runtime_playgo.c`, `runtime_savedata.c` |
| system and user services, dialogs, NP offline, trophies, sockets, HTTP | `runtime_services.c`, `runtime_trophy.c` (with `runtime_trophy_pack.c` and `runtime_trophy_sfm.c`, the game's trophy pack), `runtime_socket.c`, `runtime_http.c`, `runtime_net.c`, `runtime_voice.c` |
| audio | `runtime_audio.c` (AudioOut), `runtime_audio3d.c`, `runtime_ajm.c` |
| absent peripherals: USB devices (libSceUsbd), PlayStation Camera | `runtime_devices.c` |
| Remote Play, Share Play, video recording, core dumps (none running) | `runtime_capture.c` |
| PlayStation VR with no headset connected: Hmd, VrTracker, SocialScreen | `runtime_vr.c` |
| input | `runtime_pad.c`, `runtime_pad_script.c` (scripted input for automated runs), `runtime_gesture.c` |
| call report | `runtime_calls.c` |
| quirk registry: evaluation, `PS_QUIRKS`, `--list-quirks` | `runtime_quirks.c`, `runtime_quirks.inc` |

Each file keeps a table of `RuntimeExport {symbol name, function}` and a `runtime_<area>_resolve`
that looks the import up with `RUNTIME_LOOKUP`. Imports arrive as `NID#library#module`;
`runtime_lookup` turns the NID into a symbol name (`src/runtime/imports/import_names_all.inc`, generated from
shadPS4's aerolib list, plus a few extra NIDs in `runtime_symbol`) and compares names, so the same
function serves the NID whichever library the executable names (`libkernel` or `libScePosix`).
`runtime_resolve` (`src/runtime/runtime.c`) answers the runtime's own libc, TLS and procparam entries,
then `runtime_library_resolve` (`src/runtime/runtime_resolve.c`) asks the tables in a fixed order:
mutex, thread, sched, content, sema, eventflag, ulobjmgr, ult, rwlock, barrier, memory, mempool,
exception, kernel, module, playgo, voice, services, ajm, audio, audio3d, pad, gesture, devices,
capture, vr, mouse, keyboard, rtc, savedata, file, aio, and last `psport_gpu_resolve`. The first
table that has the symbol wins. services asks the online module first (`runtime_net_resolve`, with
`PS_ONLINE=1`), then its own table, http, trophy, json, np (and np's community table) and socket;
keyboard asks ime after its own table.

Guest-visible errno values are FreeBSD's (`runtime_guest_errno`). Sockets, libSceNet's and POSIX's,
take their descriptors from the file table (`runtime_file.c`), as kernel descriptors do on the console,
so `close`, `read`, `write`, `fcntl` and `select` serve both. Guest threads are host pthreads
with PS4 stacks below 1 TiB.

### Call report

`src/runtime/runtime_calls.c` logs, once per import (and error code), every call that gets no real work
done: imports without implementation, stubs, GPU library functions marked `(STUBBED)`, failures
(SCE error codes, `-1` with errno) and polled states (`src/runtime/imports/import_statuses.inc`). Counts are summed
when the process ends, by any route (guest exit, window close, signal, fault, watchdog, `STOP:`).
Line formats: [FORK.md](../../FORK.md#call-report). `PS_CALL_REPORT=<file>` also writes the summary to
a file.

## GPU library

`libpsport-gpu.so` holds the C++ side. `gpu/port/psport_gpu.cpp` is its C interface (`gpu/psport_gpu.h`),
with the window's input in `psport_gpu_input.cpp` and the game title's C view (`psport_gpu_title`)
in `psport_gpu_games.cpp`; the shadPS4 parts the port replaces (memory manager, kernel services) are
stood in for in `gpu/shim/psport_stand_ins.cpp`:

- `psport_gpu_register_kernel` registers the kernel event queues and the libraries that do not need a
  window: AvPlayer, Ngs2, Fiber, Font, FontFt, Audiodec, Videodec2, PngEnc, PngDec.
- `psport_gpu_init` selects the game's title and profile, loads `psport.ini`, prepares saves through
  the title, starts the SDL window on its own thread and registers GnmDriver and VideoOut. The
  Vulkan instance hands the chosen device's facts to `runtime_quirks_note_gpu` before creating the
  device (`HOST` quirks, [QUIRKS.md](QUIRKS.md)).
- `psport_gpu_resolve` returns the function registered for an import's NID. Libraries register with
  shadPS4's `LIB_FUNCTION(nid, library, version, module, function)` (`gpu/shim/core/libraries/libs.h`).
- `psport_gpu_handle_fault` is the first stop of `psport`'s `SIGSEGV` handler: GPU page-tracking
  faults (write traps) are handled there.
- `psport_gpu_title` (`psport_gpu_games.cpp`) returns what the launch takes from the title
  (`PsTitleInfo`): its serials, mod folders, patch file and presets, and its environment, file check,
  patch and image patch calls; the last hands the writable image to the active profile, then to the
  title.

The translator, from the game's submission to the screen:

| Stage | Where |
|---|---|
| libSceGnmDriver (`sceGnmSubmitCommandBuffers`, `sceGnmSubmitDone`, packet builders) | `gpu/shadps4/core/libraries/gnmdriver/` |
| libSceVideoOut (flips, vblank, `PS_VBLANK_HZ`, cursor state) | `gpu/shadps4/core/libraries/videoout/` |
| Liverpool command processor: PM4 decoding, register state, draws and dispatches; a malformed or unknown packet is skipped by its declared size, one whose size cannot be trusted (type 0 or 1, past its buffer's end) ends that buffer, an unsupported field takes the nearest safe behaviour, each counted by opcode and logged (`pm4_faults.h`), never an assert | `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, `pm4_faults.*`, `gpu/cp/` (psport's own PM4 decoder, and the command stream stage A records with it: `cp_stream.h`) |
| Vulkan rasterizer, presenter, upscalers | `gpu/shadps4/video_core/renderer_vulkan/` |
| buffer, texture and pipeline caches; page tracking | `gpu/shadps4/video_core/buffer_cache/`, `texture_cache/`, `page_manager.cpp`, `renderer_vulkan/vk_pipeline_cache.cpp` |
| GCN shader recompiler to SPIR-V | `gpu/shadps4/shader_recompiler/` |
| GPU memory without sparse binding (memory module) | `gpu/layer/` |

`PS_SUBMIT_AHEAD` (else the game profile's `c_submits_ahead`, else 0) lets a frame's first
submissions run without waiting for the previous frame; the submit guard (`gpu/memory/psport_submit_guard.*`) stops the overlap for the
session when a game rewrites command buffers in flight. Multithreaded draw preparation and the
two-stage draw pipeline: [parallel_gpu.md](parallel_gpu.md). Temporal upscaling (FSR 3.1, FSR 4,
TAA, DLSS): [upscaler.md](upscaler.md). Changes to inherited shadPS4 files:
[FORK.md](../../FORK.md#changes-to-inherited-shadps4-code).

### Memory model

One model on every GPU: the GPU uses the game's memory where it is (GPU-visible system memory),
keeps often-read data in VRAM, and the command processor's work is translated. AMD keeps the game's
memory in a sparse arena (`HOST` quirk `amd_guest_sparse_arena`, [QUIRKS.md](QUIRKS.md)); other GPUs
(and `PS_LAYER_MEMORY=1`) go through the memory module in `gpu/layer/`, which binds the game's
memory in place without sparse binding, from imported host memory or dma-buf chunks
(`gpu/memory/psport_guest_memory.cpp`). A driver that can do neither stops the launch with a `Guest
memory:` line. bbport 0.3's model (VRAM copies kept current by write tracking) is gone;
`PS_PC_MODEL`, `PS_AS_0_3` and the other switches of the old launcher are ignored with one log line.
Write traps on guest pages are shared by the runtime and the GPU library
(`gpu/layer/pslayer_write_traps.h`, `runtime_memory_trap`).

## Game profiles

`gpu/games/game_profile.h` defines what psport knows about one title, kept out of the core, in two
parts. `Game::Find` finds a title's `Game::Title` by `param.sfo` `TITLE_ID`, and `Game::Select`
records the running one in `psport_gpu_init`. The title is part of running the game and always
applies, whatever `PS_GAME_PROFILE` says; its `Game::Profile` is what the translator knows about the
game's code, and `PS_GAME_PROFILE=none` turns it off (`Game::Active` returns none). The log says
which: `Game profile: Bloodborne (CUSA03173): title data applied, translator profile active` (or
`off (PS_GAME_PROFILE=none)`). The C side reads the title through `psport_gpu_title` before
`psport_gpu_init`.

A title may provide:

| Field | Use |
|---|---|
| `serials` | the title IDs it applies to (one executable: patch files and mod folders named after any apply) |
| `mod_folders` | folders a mod may consist of without `dvdroot_ps4` around them |
| `patch_file` | its own shadPS4 XML, relative to psport's folder; left out of external patch folders |
| `prepare_environment` | environment defaults before `psport_gpu_init` (Bloodborne: `PS_VBLANK_HZ` for its `PS_FPS` preset) |
| `check_files` | the check of the game's files before its code runs (`PS_SKIP_GAME_CHECK=1` skips it) |
| `add_patches` | its own patches of the executable, chosen by the player's settings; a title with them has presets (`--has-presets`) |
| `effects` | the settings of `add_patches` the overlay menu's "Game effects" section offers: its `psport.ini` switches (key, label and hint in the menu's languages, default), the `model_lod` choice, a note; without them the section is not shown |
| `prepare_saves` | changes to the user's saves the game needs before it runs (Bloodborne: the sound flag at `0x204E`, gated by its `WORKAROUND` quirk `bloodborne_save_sound_flag`) |
| `patch_image`, `before_present` | the port's own pages in the game's menus: patched into its code after the profile's patches, and applied each presented frame |
| `online` | the servers of its online play: its own game server, the community server that answers for it, the community API port and its summon route ([Online play](#online-play)) |
| `quirks` | its entries of the quirk registry ([QUIRKS.md](QUIRKS.md)) |
| `profile` | its `Game::Profile`, or none |

A profile may provide:

| Field | Use |
|---|---|
| `install_hooks`, `patch_image` | hooks and patches in the game's code, each checking the bytes it replaces |
| `dynamic_writers` | guest code ranges known to write per-frame data into GPU memory |
| `buffer_copy_shader` | hash of a compute shader that only copies buffers |
| `driver_commands_written`, `commands_submitted` | observe the game's command buffers |
| `frame_overlap_unsafe` | tells the core to stop `PS_SUBMIT_AHEAD` overlap for the session |
| `c_submits_ahead` | the frame overlap the game takes when `PS_SUBMIT_AHEAD` is unset |
| `anisotropy` | anisotropic filtering forced on its textures when `PS_ANISO` is unset |
| `upscaler` | where the temporal upscaler goes in its frame (scene combine and UI shaders), its scene constants (camera) and bone palettes (object motion); without it the upscaler stays off |
| `report_stats` | profile diagnostics with each frame statistics report |
| `label_release` | the game's label release code, for the free check's `PS_LABEL_TRAP=2` |
| `small_object_pool` | the layout of the pool holding its GPU labels, for the free check (`PS_FREE_CHECK`) |

A profile makes a title faster or easier to diagnose; it never makes it work. The core must run
every title correctly on its generic paths, which `PS_GAME_PROFILE=none` checks. The only title
today is Bloodborne (`gpu/games/bloodborne/`: `title.cpp` and, for the launch, `patches.cpp`: the
`PS_FPS` presets 30/60/90/uncap, `PS_PATCHES`, the effect switches and `model_lod` of `psport.ini`,
the `PS_RENDER_RES` scene size, the Intel tone mapping and online-choice fixes, all from
`patches/Bloodborne.xml`; `game_check.cpp`: the 1.09 executable and intact shaders; the in-game menu
pages, `game_menu.cpp`; its profile in `profile.cpp`, with Gnm entry hooks and `gnm_table.inc`,
guest hooks and heap sites), listed in `gpu/games/game_profile.cpp`. Another title runs unpatched,
on the console's 60 Hz vblank.

## Launch scripts

A launch runs no Python, except for online play:

| Script | Use |
|---|---|
| `scripts/online.py` | addresses for `PS_ONLINE=1` from the title's servers (`psport --online-overrides`; it probes the shadNet WebAPI for the summon service); the bundled launcher imports it too |
| `scripts/sce_file.py` | `param.sfo` and SELF/ELF readers for the Python tools (`tools/got_import.py`, `tools/re/`) |
| `tools/dcx_check.py` | every `.dcx` file of a Bloodborne folder checked (the launch checks the shaders only) |

## Online play

`PS_ONLINE=1` (launcher: *Online*) routes the game's network and PSN imports to the online module
(`gpu/net/`, `libpsport-net.so`, loaded by `src/net/runtime_net.c` on the first such import). The
servers a game talks to are its title's data (`Game::Title` `online`), never the core's:

| Field | Bloodborne |
|---|---|
| `sz_game_server` (the game's own server, as it addresses it) | `https://ss4.scej-network.jp:20443` |
| `sz_community_server` (answers for it on the same port; `PS_COMMUNITY_SERVER` names another) | `https://thehuntersdream.com` |
| `community_api_port` (where the community server sends the game for its API) | `18671` |
| `sz_summon_route` (the path of its summon calls on that API) | `/summon_messenger` |

`psport --online-overrides (--game <dir> | --title <TITLE_ID>)` prints them as `key=value` lines
(`game_server`, `community_server`, `community_api_port`, `summon_route`) and exits before any game
is loaded; a title without online servers prints nothing. `run.sh` passes that to
`scripts/online.py overrides --title-online`, which writes `user/host_overrides.json` for the launch
(`SHADPS4_HTTP_HOST_OVERRIDES_JSON`): the game server to the community server, and the summon route
on the community API port to the shadNet WebAPI (`PS_SHADNET_WEBAPI`) when that answers the summon
probe. A title without servers gets an empty file, so a file another game's launch left is not
used; `user/host_overrides.custom.json`, when present, is used as it is for any game.

The module learns the summon route through `PsNetHost` `sz_summon_route` (`PSPORT_NET_ABI_VERSION`
2; a runtime and a module of different versions refuse each other and the game stays offline). The
summon helpers in `http.cpp` (the host placement header, the seamless route of
`SHADPS4_BLOODBORNE_SEAMLESS_COOP=1`, the exchange capture of `SHADPS4_CAPTURE_BLOODBORNE_SUMMON=1`)
act on that route only; for a title that declares none, either `SHADPS4_*` switch still forces
shadp2p's `/summon_messenger`, with a warning. The module logs `Co-op: summon route ... (the
title's)` or that the title declares none.

## Other parts

- `launcher/`: bbport's GTK4 launcher, built around Bloodborne.
- `packaging/`: `port.nix` (build in Nix's sandbox) and the AppImage script.
- `patches/Bloodborne.xml`: community patches for Bloodborne.
- `tools/`: regression runs, GCN disassembler (`gcn_dis.sh`), FSR 4 asset tools, scripted-run
  helpers, `import_outparams.py` and `import_returns.py` (generate `src/runtime/imports/import_*.inc`).
- [history/](history/): bbport's design notes, plans, measurements and change logs in English; the
  Russian originals, under the same names, are in [docs/ru/](../ru/).
