# Architecture

psport runs a PS4 title's own x86-64 code on a Linux PC. It does not emulate the CPU: the game's
executable and the modules it ships are loaded into one process and run natively, the PS4 system
libraries they import are provided by psport, and the game's GPU work is translated to Vulkan.
The model is Wine's, not an emulator's.

## Components

| Component | Path | Built into | Role |
|---|---|---|---|
| Launch script | `run.sh` | - | environment, settings, mods and patches, then `bb-probe` |
| Entry | `src/probe.c` | `out/bb-probe` | arguments, image mapping, import binding, signals, entering the game |
| Native loader | `src/loader.h`, `src/loader_self.c`, `src/loader_link.c` | `out/bb-probe` | SELF/ELF parsing, module linking, relocations, TLS, bindings |
| C runtime | `src/runtime*.c`, `src/runtime.h`, `src/import_*.inc` | `out/bb-probe` | libkernel and the other system libraries, import resolution, the call report |
| GPU library | `gpu/shim/bbgpu.cpp`, `gpu/bbgpu.h` | `out/gpu/libbbgpu.so` | C interface between `bb-probe` and the C++ side |
| Translator | `gpu/shadps4/` (inherited from shadPS4, `gpu/VENDOR.txt`), `gpu/cp/`, `gpu/layer/` | `libbbgpu.so` | GnmDriver, VideoOut, command processor, Vulkan rasterizer, caches, shader recompiler |
| Ported libraries | `gpu/ngs2/`, `gpu/audiodec/`, `gpu/fiber/`, `gpu/font/`, `gpu/pngenc/` | `libbbgpu.so` | libSceNgs2, libSceAudiodec, libSceFiber, libSceFont/FontFt, libScePngEnc |
| Port services | `gpu/shim/bbport_*` | `libbbgpu.so` | settings (`bbport.ini`), overlay, submit guard, guest memory, diagnostics |
| Game profiles | `gpu/games/` | `libbbgpu.so` | per-title optimisations (`Game::Profile`) |
| Online module | `gpu/bbnet/` | `out/gpu/` (optional) | network and PSN imports with `BB_ONLINE=1` |
| Launch helpers | `scripts/` | - | Python run by `run.sh`; see [Launch scripts](#launch-scripts) |
| Regression runs | `tools/regress.sh`, `tools/regress/` | - | headless runs of every title |

`build.sh` builds `libbbgpu.so` with CMake/Ninja, then compiles `src/probe.c`, `src/loader*.c` and
`src/runtime*.c` with `-Werror` into `out/bb-probe`, linked against `libbbgpu.so`.

## Launch flow

```
run.sh
  scripts/mods.py, scripts/game_check.py, scripts/content_profile.py, scripts/patches.py
  environment: memory model, BB_VBLANK_HZ, driver caches, ...
  bash build.sh (unless BB_PREBUILT=1)
  bb-probe --game <dir> [--system-modules <dir> --hle <libs> --lle <libs>]
           --content-profile out/content.bin --patches out/patches.bin --app0 <dir> --user <dir>
    bbgpu_register_kernel()           event queues and the ported libraries register their exports
    loader_load_game()                eboot + modules -> one LoaderImage
    runtime_start()
    bbgpu_init()                      settings, game profile, window thread, GnmDriver, VideoOut
    bind every import                 native binding or runtime_resolve() or trap
    apply patches.bin, bbgpu_patch_image()
    protect segments, set TLS, start linked modules in link order
    enter the eboot's entry on an 8 MiB stack below 1 TiB
```

### run.sh

`run.sh` takes the game folder from `BB_GAME_DIR` (default `../CUSA03173`) and a writable data
directory from `BB_DATA_DIR` (default the checkout: `out/`, `user/`, `bbport.ini`). It re-enters
`shell.nix` when the build dependencies are missing and Nix is installed, and builds with
`build.sh` unless `BB_PREBUILT=1`. Before starting `bb-probe` it:

- merges loose-file mods into a link tree (`scripts/mods.py`, [MODS.md](MODS.md));
- runs Bloodborne's file check (`scripts/game_check.py --launch`; other titles pass unchecked);
- writes `out/content.bin`, the AppContent parameters from `param.sfo`
  (`scripts/content_profile.py`);
- writes `out/patches.bin` (`scripts/patches.py`): Bloodborne's built-in and third-party patches
  for its 1.09 executable, an empty file for every other title;
- chooses the memory model and other defaults from the GPU (`out/bb-gpu-capabilities --device`),
  see [Memory model](#memory-model);
- passes `BB_SYS_MODULES`, `BB_HLE` and `BB_LLE` as `--system-modules`, `--hle` and `--lle`.

### bb-probe

`src/probe.c` checks the CPU, reads `param.sfo` (title, title ID, save directory, SDK version,
attributes), configures the file mounts (`/app0` is the game folder, saves under `user/`), loads
the game, binds its imports, installs the fault (`SIGSEGV`, `SIGILL`, `SIGBUS`), dump (`SIGUSR2`)
and watchdog (`SIGALRM`, `--timeout`) handlers and jumps to the eboot's entry. Host heap objects
handed to the guest stay below 1 TiB (`mallopt` keeps glibc in the non-PIE brk heap): PS4 code
packs pointers into 40 bits. Guest memory is placed there too.

### Native loader

`loader_load_game` (`src/loader_link.c`) builds one image, the eboot at offset 0 and every module
at the next 64 KiB boundary after the previous one, as one table of imports:

- `src/loader_self.c` reads SELF and plain ELF files: program headers, dynamic section, the
  module and library tables, symbols (NID, library, module) and relocations. Only a segment's
  permission bits become its protection (some system modules set high bits in `p_flags`).
- Modules linked: the game's `sce_module/libc.prx` and `libSceFios2.prx` always; the other
  `sce_module/*.prx` once something linked imports their library; the user's system modules
  (`--system-modules`, `*.sprx`) the same way; and every `*.prx`/`*.sprx` outside `sce_module`
  and `sce_sys`, which the game loads itself with `sceKernelLoadStartModule`. Those are linked,
  relocated and bound at start but left unstarted; loading the path later runs their initializer
  once (`src/runtime_module.c`).
- Exports are matched by (NID, library, module). Imports of `libSceLibcInternal` are served by
  `libc.prx`'s exports with the same NID.
- Initial-exec TLS loads `mov rax, fs:[0]` are rewritten to GS, because glibc owns FS on Linux.
  The eboot's `PT_TLS` and each module's TLS block get module IDs for `__tls_get_addr`.
- The result (`LoaderImage`, `src/loader.h`): segments, relocations (to an image offset, or to an
  import index for functions and data), import names `NID#library#module`, native bindings with
  a kind, the module table (`RuntimeModule`: init, TLS, `.eh_frame`, exports) and procparam.

System module rules, from `src/loader_link.c`:

| Library | Rule |
|---|---|
| `libSceLibcInternal` | never taken from the system dump: the game's `libc.prx` serves it, so the runtime's replacements (`memcpy`, `memset`) keep tracking writes to GPU memory |
| `libkernel`, `libkernel_unity`, `libkernel_sys`, `libSceGnmDriver`, `libSceVideoOut`, `libScePad`, `libSceAudioOut`, `libSceFiber`, `libSceFont`, `libSceFontFt`, `libSceAudiodec`, `libSceNgs2`, `libSceUlt`, `libSceRtc` | implemented by psport (`port_implements`): the system module is skipped unless `--lle` (`BB_LLE`) names it |
| a library named in `--hle` (`BB_HLE`) | system module skipped, psport's implementation used |
| any other library a linked module imports | the bundled `sce_module` copy, else the system dump's; its exports win over psport's (binding kinds 3 and 4) |

System modules are the user's own dump; nothing from them is in the repository or in a build. No
title in `tools/regress/titles.txt` needs them; they are a stopgap until psport implements the
library.

### Import binding

`bb-probe` walks the relocations. For each import (`src/probe.c`):

1. a binding of kind 3-6 (a system module, or a module the game loads) wins;
2. otherwise `runtime_resolve(name)` (C runtime, then the GPU library);
3. otherwise a native binding of kind 1-2 (a module the game ships);
4. otherwise a trap: calling it stops the game with the import's name, or with `BB_PERMISSIVE=1`
   returns 0 (`BB_PERMISSIVE_OUT=error`: `0x80020016` for imports with output arguments).

Data imports nothing provides point at a page that faults on access. Imports resolved to
`runtime_stub` go through a trap that logs them as stubs; imports whose result can report a
failure (`src/import_returns.inc`) go through a thunk that logs failures. Before entering the
game, `bb-probe` prints every stub and every import without implementation, then one summary line:
`Runtime: imports N: host N, game modules N, system modules N, modules the game loads N, stubs N,
without implementation N`.

After binding: `patches.bin` is applied (pointer writes into relocated slots are rebased), the
profile's `patch_image` runs, segments get their protections, TLS is set up, linked modules start
in link order (libc first) and the eboot's entry runs with `argv[0] = /app0/eboot.bin`.

## C runtime

`src/runtime*.c` implements the system libraries in C, one file per area:

| Area | Files |
|---|---|
| process, libc hooks, TLS, procparam, `runtime_resolve` | `runtime.c`, `runtime.h` |
| kernel misc, process, time | `runtime_kernel.c`, `runtime_time.c`, `runtime_rtc.c` |
| memory: direct, flexible, mappings, pools, write traps, GPU interface | `runtime_memory.c`, `runtime_memory_kernel.c`, `runtime_memory_gpu.c`, `runtime_mempool.c` |
| threads and synchronisation | `runtime_thread.c`, `runtime_sched.c`, `runtime_mutex.c`, `runtime_rwlock.c`, `runtime_sema.c`, `runtime_eventflag.c`, `runtime_ult.c`, `runtime_ulobjmgr.c` |
| exceptions | `runtime_exception.c` |
| modules loaded at run time | `runtime_module.c` |
| files, async I/O, AppContent, PlayGo, saves | `runtime_file.c`, `runtime_aio.c`, `runtime_content.c`, `runtime_playgo.c`, `runtime_savedata.c` |
| system and user services, dialogs, NP offline, sockets | `runtime_services.c`, `runtime_socket.c`, `runtime_net.c`, `runtime_voice.c` |
| audio | `runtime_audio.c` (AudioOut), `runtime_audio3d.c`, `runtime_ajm.c` |
| input | `runtime_pad.c`, `runtime_gesture.c` |
| call report | `runtime_calls.c` |

Each file keeps a table of `RuntimeExport {symbol name, function}` and a `runtime_<area>_resolve`
that looks the import up with `RUNTIME_LOOKUP`. Imports arrive as `NID#library#module`;
`runtime_lookup` turns the NID into a symbol name (`src/import_names_all.inc`, generated from
shadPS4's aerolib list, plus a few extra NIDs in `runtime_symbol`) and compares names, so the same
function serves the NID whichever library the executable names (`libkernel` or `libScePosix`).
`runtime_resolve` (`src/runtime.c`) asks the tables in a fixed chain: the runtime's own libc,
TLS and procparam entries, then mutex, thread, sched, content, time, sema, eventflag, ulobjmgr,
ult, rwlock, memory, mempool, exception, kernel, module, playgo, voice, services, ajm, audio,
audio3d, pad, gesture, rtc, savedata, file, aio, and last `bbgpu_resolve`. The first table that
has the symbol wins.

Guest-visible errno values are FreeBSD's (`runtime_guest_errno`). Guest threads are host pthreads
with PS4 stacks below 1 TiB.

### Call report

`src/runtime_calls.c` logs, once per import (and error code), every call that gets no real work
done: imports without implementation, stubs, GPU library functions marked `(STUBBED)`, failures
(SCE error codes, `-1` with errno) and polled states (`src/import_statuses.inc`). Counts are summed
when the process ends, by any route (guest exit, window close, signal, fault, watchdog, `STOP:`).
Line formats: [FORK.md](../../FORK.md#call-report). `BB_CALL_REPORT=<file>` also writes the summary to
a file.

## GPU library

`libbbgpu.so` holds the C++ side. `gpu/shim/bbgpu.cpp` is its C interface (`gpu/bbgpu.h`):

- `bbgpu_register_kernel` registers the kernel event queues and the libraries that do not need a
  window: AvPlayer, Ngs2, Fiber, Font, FontFt, Audiodec, Videodec2, PngEnc.
- `bbgpu_init` loads `bbport.ini`, selects the game profile, prepares saves through it, starts the
  SDL window on its own thread and registers GnmDriver and VideoOut.
- `bbgpu_resolve` returns the function registered for an import's NID. Libraries register with
  shadPS4's `LIB_FUNCTION(nid, library, version, module, function)` (`gpu/shim/core/libraries/libs.h`).
- `bbgpu_handle_fault` is the first stop of `bb-probe`'s `SIGSEGV` handler: GPU page-tracking
  faults (write traps) are handled there.
- `bbgpu_patch_image` hands the writable image to the active profile.

The translator, from the game's submission to the screen:

| Stage | Where |
|---|---|
| libSceGnmDriver (`sceGnmSubmitCommandBuffers`, `sceGnmSubmitDone`, packet builders) | `gpu/shadps4/core/libraries/gnmdriver/` |
| libSceVideoOut (flips, vblank, `BB_VBLANK_HZ`, cursor state) | `gpu/shadps4/core/libraries/videoout/` |
| Liverpool command processor: PM4 decoding, register state, draws and dispatches | `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, `gpu/cp/` (psport's own PM4 decoder) |
| Vulkan rasterizer, presenter, upscalers | `gpu/shadps4/video_core/renderer_vulkan/` |
| buffer, texture and pipeline caches; page tracking | `gpu/shadps4/video_core/buffer_cache/`, `texture_cache/`, `page_manager.cpp`, `renderer_vulkan/vk_pipeline_cache.cpp` |
| GCN shader recompiler to SPIR-V | `gpu/shadps4/shader_recompiler/` |
| GPU memory without sparse binding (memory module) | `gpu/layer/` |

`BB_SUBMIT_AHEAD` (default 2) lets a frame's first submissions run without waiting for the
previous frame; the submit guard (`gpu/shim/bbport_submit_guard.*`) stops the overlap for the
session when a game rewrites command buffers in flight. Multithreaded draw preparation and the
two-stage draw pipeline: [parallel_gpu.md](parallel_gpu.md). Temporal upscaling (FSR 3.1, FSR 4,
TAA, DLSS): [upscaler.md](upscaler.md). Changes to inherited shadPS4 files:
[FORK.md](../../FORK.md#changes-to-inherited-shadps4-code).

### Memory model

Two models coexist on master; `run.sh` picks one when `BB_PC_MODEL` is unset:

| GPU | Default | What it does |
|---|---|---|
| AMD (vendor `0x1002`) | `BB_PC_MODEL=1` | the GPU uses the game's memory where it is (GPU-visible system memory, `BB_GUEST_IN_PLACE=1`), keeps often-read data in VRAM, and the command processor's work is translated; AMD keeps the game's memory in a sparse arena |
| others | `BB_PC_MODEL=0` | bbport 0.3's model: VRAM copies of the game's memory kept current by write tracking |

With `BB_PC_MODEL=1` off AMD, or `BB_LAYER_MEMORY=1`, the memory module in `gpu/layer/` binds the
game's memory in place and keeps VRAM copies without sparse binding. `BB_GUEST_IN_PLACE` set by
hand overrides the choice; `BB_AS_0_3=1` reverts to the 0.3 behaviour for comparisons. The 0.3
model and the emulation paths around it are being retired; the in-place model is the one new work
targets. Write traps on guest pages are shared by the runtime and the GPU library
(`gpu/layer/bblayer_write_traps.h`, `runtime_memory_trap`).

## Game profiles

`gpu/games/game_profile.h` defines `Game::Profile`: what the translator knows about one title,
kept out of the core. `Game::Select` picks a profile by `param.sfo` `TITLE_ID` in `bbgpu_init`;
`BB_GAME_PROFILE=none` runs any title without one. A profile may provide:

| Field | Use |
|---|---|
| `serials` | the title IDs it applies to |
| `install_hooks`, `patch_image` | hooks and patches in the game's code, each checking the bytes it replaces |
| `dynamic_writers` | guest code ranges known to write per-frame data into GPU memory |
| `prepare_saves` | changes to the user's saves before the game runs |
| `buffer_copy_shader` | hash of a compute shader that only copies buffers |
| `driver_commands_written`, `commands_submitted` | observe the game's command buffers |
| `frame_overlap_unsafe` | tells the core to stop `BB_SUBMIT_AHEAD` overlap for the session |
| `report_stats`, `before_present` | profile diagnostics; per-frame work on the presenter thread |
| `label_release` | the game's label release code, for the free check's `BB_LABEL_TRAP=2` |

A profile makes a title faster or easier to diagnose; it never makes it work. The core must run
every title correctly on its generic paths, which `BB_GAME_PROFILE=none` checks. The only profile
today is Bloodborne's (`gpu/games/bloodborne/`: `profile.cpp`, Gnm entry hooks with
`gnm_table.inc`, guest hooks, heap sites, the in-game menu patch), listed in
`gpu/games/game_profile.cpp`.

## Launch scripts

What Python remains is run by `run.sh`, not by the loader:

| Script | Use |
|---|---|
| `scripts/mods.py` | loose-file mod overlay ([MODS.md](MODS.md)) |
| `scripts/content_profile.py` | `out/content.bin` (AppContent SKU and user parameters) |
| `scripts/patches.py` | `out/patches.bin`: Bloodborne's built-in, preset and third-party patches; upscaler preset sizes for `run.sh` |
| `scripts/game_check.py` | Bloodborne's check of its files (1.09 executable, intact extraction) |
| `scripts/online.py` | addresses for `BB_ONLINE=1` |
| `scripts/sce_file.py` | `param.sfo` and SELF/ELF readers shared by the scripts |

## Other parts

- `launcher/`: bbport's GTK4 launcher, built around Bloodborne.
- `packaging/`: `port.nix` (build in Nix's sandbox) and the AppImage script.
- `patches/Bloodborne.xml`: community patches for Bloodborne.
- `tools/`: regression runs, GCN disassembler (`gcn_dis.sh`), FSR 4 asset tools, scripted-run
  helpers, `import_outparams.py` and `import_returns.py` (generate `src/import_*.inc`).
- [history/](history/): bbport's design notes, plans, measurements and change logs in English; the
  Russian originals, under the same names, are in [docs/ru/](../ru/).
