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
| Launcher queries | `src/main/cli.c`, `src/main/game_info.c` | `out/psport` | `--check-game`, `--has-presets`, `--list-patches`, `--list-mods`, `--list-quirks`, `--list-features`, `--list-traits`, `--list-effects`, `--list-controls`, `--online-overrides`, `BB_*` names; `param.sfo` and the game's identity |
| Import binding | `src/main/bind.c` | `out/psport` | traps, failure thunks, relocation walk, patch writes |
| Signals | `src/main/signals.c` | `out/psport` | fault, watchdog, thread dump and termination handlers |
| Native loader | `src/loader/loader.h`, `src/loader/loader_self.c`, `src/loader/loader_link.c`, `src/loader/loader_traits.c` | `out/psport` | SELF/ELF parsing, module linking, relocations, TLS, bindings; middleware traits (`Middleware:` line) |
| Launch data | `src/loader/loader_mods.c`, `src/loader/loader_patches.c`, `src/loader/loader_xml.c`, `src/loader/loader_hash.c`, `src/loader/loader_json.c` | `out/psport` | loose-file mod view, shadPS4/GoldHEN XML patches, the eboot's identity (SHA-256), `mods.json`/`patches.json` |
| Quirk registry | `src/runtime/runtime_quirks.inc`, `runtime_quirks.h`, `runtime_quirks.c`, `gpu/port/psport_quirks.h` | `out/psport` | fixes for one title, host or middleware, decided once and read at setup ([QUIRKS.md](QUIRKS.md)) |
| Platform layer | `src/platform/ps_host.h`, `src/platform/host_features.inc`, `src/platform/host_features.c`, `src/platform/<os>/` | `out/psport` | what psport asks of the operating system (`ps_host_*`) and what each OS has ([Platform layer](#platform-layer)) |
| C runtime | `src/*/runtime*.c`, `src/runtime/runtime.h`, `src/runtime/imports/import_*.inc` | `out/psport` | libkernel and the other system libraries, import resolution, the call report |
| GPU library | `gpu/port/psport_gpu*.cpp`, `gpu/psport_gpu.h` | `out/gpu/libpsport-gpu.so` | C interface between `psport` and the C++ side |
| Translator | `gpu/shadps4/` (inherited from shadPS4, `gpu/VENDOR.txt`), `gpu/cp/`, `gpu/layer/` | `libpsport-gpu.so` | GnmDriver, VideoOut, command processor, Vulkan rasterizer, caches, shader recompiler |
| Ported libraries | `gpu/ngs2/` (native) and `gpu/ngs2_anyps5/`, `gpu/audiodec/`, `gpu/fiber/`, `gpu/font/`, `gpu/pngenc/`, `gpu/pngdec/`, `gpu/jpegdec/` | `libpsport-gpu.so` | libSceNgs2, libSceAudiodec, libSceFiber, libSceFont/FontFt, libScePngEnc, libScePngDec, libSceJpegDec |
| Port services | `gpu/port/`, `gpu/memory/`, `gpu/diag/` | `libpsport-gpu.so` | settings (`psport.ini`), overlay, submit guard, guest memory, diagnostics |
| Game profiles | `gpu/games/` | `libpsport-gpu.so` | per-title launch and game data (`Game::Title`) and translator optimisations (`Game::Profile`) |
| Online module | `gpu/net/` | `out/gpu/` (optional) | network and PSN imports with `PS_ONLINE=1` |
| Online addresses | `scripts/online.py` | - | Python run by `run.sh` with `PS_ONLINE=1` only; see [Launch scripts](#launch-scripts) |
| Regression runs | `tools/regress.sh`, `tools/regress/` | - | headless runs of every title |

`build.sh` builds `libpsport-gpu.so` with CMake/Ninja, then compiles `src/main/*.c`, `src/loader/loader*.c`,
`src/*/runtime*.c` and `src/platform/` with `-Werror` into `out/psport`, linked against `libpsport-gpu.so`.

## Launch flow

```
run.sh
  environment: memory model, scene size for the output, driver caches, ...
  bash build.sh (unless PS_PREBUILT=1)
  scripts/online.py (PS_ONLINE=1 only)
  psport --game <dir> [--system-modules <dir> --hle <libs> --lle <libs>]
           [--mods <dir> --mods-config <file> --out <dir>]
           --patches-dir <dir> --patches-config <file> --app0 <dir> --user <dir> [--user-id <N>]
    loader_mods_view()                the game folder, or a view of it with the mods linked in
    AppContent profile                param.sfo's user parameters, PS_CONTENT_SKU
    psport_gpu_register_kernel()           event queues and the ported libraries register their exports
    loader_load_game()                eboot + modules -> one LoaderImage
    psport_gpu_title()                     the title's launch data (PsTitleInfo), whatever PS_GAME_PROFILE says
    runtime_quirks_evaluate()              PS_QUIRKS, the title's quirks, published (QUIRKS.md)
    pfn_prepare_environment                the title's environment defaults (PS_VBLANK_HZ)
    pfn_check_files                        the title's check of the game's files (exit 2)
    pfn_add_patches                        the title's own patches (and the PS_DMEM_MB they need), then the external XML patches
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
(`SIGUSR2`) and watchdog (`SIGALRM`, `--timeout`) handlers and jumps to the eboot's entry. A fault report
shows, besides the guest offset and the call chains, the guest memory ranges `PS_FAULT_DUMP` names
([FORK.md](../../FORK.md#diagnostics)). Host
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
- Exports are matched by (NID, library, module), library and module versions included. Imports of
  `libSceLibcInternal` are served by `libc.prx`'s exports with the same NID. An import that only an
  export of other versions would serve is logged (`Loader: import ... only ignoring versions`) and
  counted after the call report, and bound to it only with `PS_LOOSE_IMPORT_VERSIONS=1`: the console's
  kernel does this binding, and whether it compares versions is not known.
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
  `psport --list-controls [--game <dir> | --title <TITLE_ID>]` prints `title=<name>` when psport
  knows the title, then its default bindings as `psport.ini` lines (`key.<input>=`, and
  `pad.<input>=` for the pad's inputs, then the `mouse_look_*` values the title sets): the neutral
  layout of `src/input/runtime_pad.c` with the title's `sz_default_controls` over it, the neutral
  layout alone without a game; the launcher's
  controls page shows these as defaults.
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
| input | `runtime_pad.c`, `runtime_pad_sensors.c` (the pad's motion sensors), `runtime_pad_script.c` (scripted input for automated runs), `runtime_gesture.c` |
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

### Debug mode

`PS_DEBUG=1` (or `debug=1` in `psport.ini`) makes `src/runtime/runtime_debug.c` gather a run's logs in
`<PS_DEBUG_DIR>/<host>/<YYYYmmdd-HHMMSS>-<serial>/` (`run.log`, `gpu.log`, `calls.txt`, `faults.txt`,
`validation.txt`, `frame-stats.txt`, `env.txt`, `config.txt`, `summary.txt`), for a folder synchronised
between machines: each file is written as `<name>.partial` and renamed when complete, `summary.txt`
last. A helper process (`ps_host_output_tee`) copies stdout and stderr into `run.log` and finishes the
folder once the process is gone, by any route; every exit path calls `runtime_debug_finish` after the
call report. `PS_DEBUG_KEEP` (20) run folders of the host are kept. Details:
[FORK.md](../../FORK.md#diagnostics).

### Platform layer

What psport asks of the operating system goes through one small C ABI, `ps_host_*`
(`src/platform/ps_host.h`, decision #540), with an implementation per OS in `src/platform/<os>/`
(and `gpu/platform/<os>/` for the C++ side in libpsport-gpu); what is the same on every OS that builds it
lives once in `src/platform/common/` or `gpu/platform/common/` (decision #1009). Linux is implemented; Windows
(`src/platform/win32/`, #614) has every group below, not run with a game yet. On Windows `build.sh` builds
`psport.exe`, the GPU library (`libpsport-gpu.dll`, which imports the runtime's symbols from `psport.exe`
through `gpu/port/psport_runtime.def`) and the capability check with MSYS2's CLANG64
(`.github/workflows/windows.yml`, decision #623); the C library calls UCRT lacks (memmem, strcasestr, getline,
timegm, the recursive mutex initializer, rwlock kinds, pthread_cond_clockwait, pthread_mutex_clocklock,
pthread_rwlock_clockrdlock and pthread_rwlock_clockwrlock) come from
`src/platform/win32/posix_compat.h`, which that build force-includes, so that the generic code keeps one
spelling. Its groups: vm, fault/interrupt, thread, tls, time, fs/path, proc, dl, rand and net,
moved behind the ABI area by area without changing what Linux does. So far:

| Group | Functions | Linux |
|---|---|---|
| time | `ps_host_time_read`, `ps_host_time_resolution` (`PsHostClock`), `ps_host_time_monotonic_ns`, `ps_host_time_realtime_ns`, `ps_host_time_sleep_ns`, `ps_host_time_sleep_until_ns`, `ps_host_time_zone_at` (the zone rule in force at an instant), `ps_host_time_local_fields`, `ps_host_time_utc_fields`, `ps_host_time_utc_seconds` (calendar fields and back) | `clock_gettime`, `clock_getres`, `nanosleep`, `clock_nanosleep` (`linux/host_time.c`), `tzset`, `localtime_r`, `gmtime_r`, `timegm` and `tm_gmtoff` |
| rand | `ps_host_random_bytes` | `getrandom` (`linux/host_system.c`) |
| path | `ps_host_exe_path`, `ps_host_command_line` (`/proc/self/cmdline`, joined by spaces; `GetCommandLineW` on Windows) | `/proc/self/exe` (`linux/host_system.c`) |
| thread | `ps_host_thread_id`, `ps_host_wait_on_address`, `ps_host_wake_address`, `ps_host_thread_set_name` (15 characters kept), `ps_host_thread_name`, `ps_host_thread_find` (by name prefix), `ps_host_thread_each` (no allocation: the watchdog walks threads with it), `ps_host_thread_stack`, `ps_host_thread_cpu_count`, `ps_host_thread_make_background`, `ps_host_thread_yield`, `ps_host_thread_cpu_times` (user and kernel time, process or thread) | `gettid`, `/proc/self/task`, `futex` (`FUTEX_WAIT_BITSET`, `FUTEX_WAKE`), `pthread_setname_np`, `pthread_getname_np`, `/proc/self/task/*/comm`, `pthread_getattr_np`, `sched_yield`, `sched_getaffinity`, `SCHED_IDLE` else nice 19 (`linux/host_thread.c`), `getrusage` |
| fs | `ps_host_list_dir` (no allocation: usable in a signal handler) | `getdents64` (`linux/host_process.c`) |
| fs | `ps_host_fs_open`, `_close`, `_read`, `_pread`, `_write` (safe in signal handlers), `_pwrite`, `_seek`, `_sync`, `_truncate`, `_truncate_path`, `_stat`, `_stat_link`, `_stat_file`, `_stat_stream`, `_sync_stream`, `_truncate_stream` (a stdio stream's file, whose descriptor is the C runtime's) (`PsHostStat`, the Unix mode word), `_access`, `_make_dir`, `_make_unique_dir`, `_remove_dir`, `_remove`, `_rename`, `_symlink`, `_free_space`, `_real_path`, `_set_times`, `_set_times_file`, `_set_mode`, `_set_mode_file`, `_status`, `_set_status`, `_close_on_exec`, `_set_close_on_exec`, `_sync_data`, `_pipe`, `_lock` (flock's whole-file locks), `_poll` (a stream's readiness without waiting) (`PS_HOST_OPEN_*` flags; 0 or the host's errno, counts or minus it); `ps_host_dir_open`, `_next`, `_stat`, `_close` (`PsHostDirEntry`); `ps_host_fs_open_stream` (a stdio stream on a host path, fopen's modes); `ps_host_fs_walk` (`PsHostWalkEntry`: the root and everything below it, links not followed, folders before or after their entries) and `ps_host_fs_remove_tree`, one definition for both OSes in `common/host_walk.c` over the listing and `_stat_link` | `open` (always `O_CLOEXEC`), `read`, `pread`, `write`, `lseek`, `fsync`, `ftruncate`, `stat`, `lstat`, `fstat`, `fstatat`, `mkdir`, `mkdtemp`, `rename`, `symlink`, `statvfs`, `realpath`, `utimes`, `fcntl`, `fdatasync`, `pipe2`, `flock`, `poll`, `opendir`/`readdir` (`linux/host_file.c`), `fileno`, `fopen` (`_wfopen` of the wide path on Windows) |
| vm | `ps_host_vm_reserve_arena` (the range fixed mappings go to, taken at launch), `ps_host_vm_map` (private zeroed memory), `ps_host_vm_map_shared` (a view of a section or file), `_unmap`, `_protect` (safe in signal handlers), `_flush`, `_page_size` (`PS_HOST_VM_*` access; `PS_HOST_VM_REPLACE`, `PS_HOST_VM_EXACT`, `PS_HOST_VM_SPARSE` placement); `ps_host_vm_section_create`, `_section_discard` (the guest's direct and flexible memory); `ps_host_vm_keep_heap_low` (malloc below 1 TiB); `ps_host_list_mappings` (`PsHostMapping`; no allocation: usable in a fault handler) | `mmap` (`MAP_FIXED`, `MAP_FIXED_NOREPLACE`, `MAP_NORESERVE`), `munmap`, `mprotect`, `msync`, `memfd_create` and `ftruncate`, `fallocate(FALLOC_FL_PUNCH_HOLE)`, `mallopt` over the `-no-pie` brk heap, `/proc/self/maps` read with `read`, no arena reservation (`linux/host_memory.c`) |
| gpu_memory | `ps_host_gpu_memory.h`: `ps_host_gpu_memory_kinds` (`PS_HOST_GPU_SHARE_EXPORT_FD`, `_IMPORT_RANGE`: what the OS can share, not what a driver does, which the probes of `gpu/memory/` answer), `_range_name` (the imported view, for logs), `_granularity`, `_adopt` (takes an exported descriptor), `_describe_range` (a range of the host's view the GPU imports in place), `_native`, `_pointer`, `_map` (an exported share at the guest's addresses), `_release`, `_stats` (`PsHostGpuMemoryStats`: shares, bytes and failures per kind, each failure logged at its 1st, 2nd, 4th... time) on the opaque `PsHostGpuShare`; the shares, ranges and counts once in `common/host_gpu_memory.c`, the exported descriptor's view and close per OS; memory types, placement and unified memory stay in `gpu/memory/` | dma-buf descriptors through `mmap` and `close`, memfd views at page granularity (`linux/host_gpu_memory.c`); Windows takes ranges of the section's views only, `_adopt` answers `EOPNOTSUPP` |
| net | `ps_host_net_init`, `ps_host_net_close`, `ps_host_net_error`, `ps_host_net_set_nonblocking`, `ps_host_net_queued`, `ps_host_net_tcp_state` (`PsHostTcpState`, FreeBSD's order); the rest are the BSD socket calls; `ps_host_net_socket`, `_accept`, `_connect` (EINPROGRESS while connecting), `_send`, `_receive` (`PsHostIoVector`, `PS_HOST_NET_*` flags), `_poll` (`PsHostPollEntry`), `_socketpair` (EOPNOTSUPP where the host has none), `_ready` (`ps_host_fs_poll` for a socket); the BSD calls through `src/platform/ps_host_sockets.h` | `close`, `errno`, `fcntl`, `ioctl(FIONREAD, TIOCOUTQ)`, `TCP_INFO` (`linux/host_net.c`); `socket`, `socketpair` and `accept4` with `SOCK_CLOEXEC`, `connect`, `sendmsg` and `recvmsg` with `MSG_NOSIGNAL`, `ppoll` |
| fault | `PsHostFaultContext` and its accessors: `ps_host_fault_kind` (`PsHostFaultKind`: access, bus, illegal, arithmetic, breakpoint), `_signal`, `_address`, `_access` (read, write, execute), `_register`, `_set_register` (`PsHostRegister`, x86-64), `_cpu` (trap number, error code, code selector, fxsave image), `_recover` (to a recovery point set with `PS_HOST_RECOVERY_SET`, left without a fault with `PS_HOST_RECOVERY_JUMP`), `_backtrace`; `ps_host_backtrace` (the calling thread's); `ps_host_fault_install` (handlers per kind, newest first, a 0 passes the fault on; `PS_HOST_FAULT_NESTED`, `PS_HOST_FAULT_HOLD_GUEST_EXCEPTION`); `ps_host_resume` (a whole register set and xsave image in one step, the guest's `sigreturn`); `ps_host_read_memory` and `ps_host_write_memory` (no fault: a page without access ends the copy) | `sigaction` of `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGTRAP` to one dispatcher, `ucontext_t` `gregs` and `REG_ERR` bits, `sigsetjmp`, `siglongjmp` with the context's mask, `backtrace` from the faulting RIP or the caller, `rt_sigreturn`, `process_vm_readv`, `process_vm_writev` (`linux/host_fault.c`) |
| interrupt | `ps_host_interrupt_install`, `ps_host_interrupt_thread`, `ps_host_interrupt_signal` (`PsHostInterrupt`: guest exception, thread dump, watchdog, sample; the handler gets a `PsHostFaultContext`), `ps_host_watchdog_arm`, `ps_host_on_termination`, `ps_host_interrupts_hold`, `ps_host_interrupts_restore` (`PsHostInterruptMask`: what a thread holds back, around a new thread's start) | `SIGRTMIN+3`, `SIGUSR2`, `SIGALRM`, `SIGPROF` through `tgkill`, `alarm`, `SIGTERM`/`SIGINT`/`SIGHUP` when SDL left them at their default (`linux/host_fault.c`), `pthread_sigmask` with the fault signals left open |
| tls | `ps_host_tls_set_base`, `ps_host_tls_base` (the guest's TCB, on GS), `ps_host_tls_init` (first thing in `main`), `ps_host_tls_tcb_offset` (the displacement the loader writes into the guest's thread pointer loads) | `arch_prctl(ARCH_SET_GS, ARCH_GET_GS)`, no reservation, displacement 0 (`linux/host_thread.c`) |
| proc | `ps_host_close_from`, `ps_host_exec`, `ps_host_exec_self`, `ps_host_env_set`, `ps_host_env_get` (`getenv`, one definition for both OSes in `common/host_env.c`), `ps_host_process_id` (`getpid`), `ps_host_exit_at_once` (`_exit`; `TerminateProcess` on Windows), `ps_host_remove_tree_at_exit` (the mod view's cleaner) | `close_range`, `execvp`, `execv("/proc/self/exe")` (`linux/host_process.c`), `setenv` (`linux/host_system.c`), `fork`, `setsid` and `nftw` |
| dl | `ps_host_dl_open`, `ps_host_dl_sym`, `ps_host_dl_find_loaded` (a library the process already holds, by base name), `ps_host_dl_address` (`PsHostCodeAddress`: file, symbol, their addresses) | `dlopen(RTLD_NOW \| RTLD_LOCAL)`, `dlopen(RTLD_NOW \| RTLD_NOLOAD)` of `lib<name>.so`, `dlsym`, `dladdr` (`linux/host_library.c`) |
| audio | `ps_host_audio.h`: `ps_host_audio_start`, `_driver`, `_device_channels`; `ps_host_audio_out_open` (`PsHostAudioFormat`, channels, rate), `_out_set_format`, `_out_resume`, `_out_submit`, `_out_queued`, `_out_close`; `ps_host_audio_error` | SDL3 audio streams, one shared backend for Linux and Windows (`common/host_audio.c`) |
| input | `ps_host_input.h`: `ps_host_input_pad_start`, `_pad_list`, `_free`, `_pad_guid`, `_pad_name_for_id`, `_pad_open`, `_pad_close`, `_pad_is_connected`, `_pad_id`, `_pad_name`, `_pad_type`, `_pad_button`, `_pad_axis`, `_pad_touchpads`, `_pad_touchpad_fingers`, `_pad_touchpad_finger`, `_pad_rumble`, `_pad_set_light_bar`, `_pad_has_sensor`, `_pad_enable_sensor`, `_pad_read_sensor`, `_pad_button_from_name`, `_pad_button_name`, `_pad_button_label`; `ps_host_input_is_window_ready`, `_keyboard_state`, `_key_from_name`, `_key_name` (keys as USB HID usages, `PS_HOST_INPUT_MOD_*`), `_mouse_buttons`; `ps_host_input_watch` (`PsHostInputEvent`, on the window thread), `ps_host_input_error` | SDL3, one shared backend for Linux and Windows (`common/host_input.c`) |
| window | `ps_host_window.h`: `ps_host_window_start`, `_driver`, `_app_id`, `_ticks_ms`, `_delay_ms`, `_has_mouse`, `_has_keyboard`, `_keycode` (`PS_HOST_WINDOW_KEYCODE_*`), `_free`, `_error`; `ps_host_window_primary_display`, `_displays`, `_display_name`, `_display_is_named`, `_display_mode` (`PsHostDisplayMode`); `ps_host_window_create`, `_destroy`, `_set_icon`, `_set_title`, `_size`, `_size_in_pixels`, `_display`, `_is_fullscreen`, `_has_input_focus`, `_surface` (the opaque `PsHostWindowSurface`), `_set_relative_mouse`, `_show_cursor`, `_mouse_focus`, `_pixel_density`, `_text_input`, `_is_text_input`; `ps_host_window_poll_event` (`PsHostWindowEvent`, which game modules' filters and `Game::MouseHooks` see), `_request_quit`; `ps_host_window_vulkan.h`: `ps_host_window_vulkan_extension`, `_vulkan_system`, `_vulkan_can_capture`, `_vulkan_surface` | SDL3, one shared backend for Linux and Windows in libpsport-gpu (`gpu/platform/common/host_window.cpp`); the surface of SDL's X11 or Wayland window through `VK_KHR_xlib_surface` or `VK_KHR_wayland_surface` (`gpu/platform/linux/host_window_surface.cpp`; the Win32 one in `gpu/platform/win32/`); the overlay's font embedded in an ELF or COFF section (`gpu/platform/<os>/embedded_font.cpp`) |

On Windows: `QueryPerformanceCounter`, `GetSystemTimePreciseAsFileTime`, `GetThreadTimes` and `GetProcessTimes`,
sleeps on a per-thread high-resolution waitable timer (`win32/host_time.c`); `BCryptGenRandom` and
`GetModuleFileNameW` with `/` separators, the wide environment through `_wputenv_s` (`win32/host_system.c`); `LoadLibraryExW`, `GetProcAddress`,
`GetModuleHandleExW` and the PE export directory (`win32/host_library.c`); `WaitOnAddress`, `WakeByAddressSingle`
and `WakeByAddressAll`, `SetThreadDescription`, Toolhelp thread lists, `GetCurrentThreadStackLimits`, the process
affinity mask and `THREAD_PRIORITY_IDLE`, `RtlCaptureStackBackTrace`, the guest's TCB in a `TlsAlloc` slot of the TEB's 64 inline ones, which
the loader's patched loads read at `0x1480 + 8 * slot` (`win32/host_thread.c`); UTF-8 paths and Win32 errors as errno values in
`win32/host_win32.c`; the arena as one `VirtualAlloc2` placeholder that fixed mappings are carved into and filled with
`MapViewOfFile3` views of `CreateFileMapping2` sections (`CreateFileMappingW` where Wine lacks it, logged) (private memory a section of its own, `SEC_RESERVE` committed as views
map it past the commit limit), `VirtualProtect` and `VirtualQuery` (`win32/host_memory.c`).
Files are `CreateFileW` handles in `win32/host_file.c`'s descriptor table (0-2 copies of the standard
handles; a duplicate onto them moves the C runtime's stdio and `SetStdHandle` too, as `dup2` does), `pread` and `pwrite`
on a second `ReOpenFile` handle so that the file pointer the descriptor shares stays put, append through Windows'
end-of-file offset, folders through `FILE_FLAG_BACKUP_SEMANTICS` and `FindFirstFileExW`, paths past `MAX_PATH` with
`\\?\`, pipes from `CreatePipe` polled with `PeekNamedPipe`, flock as `LockFileEx` over the whole file
(mandatory locks on Windows); other modules hand handles to the table with `ps_win32_fd_adopt` and read them with `ps_win32_fd_handle`.
The host paths `/dev/urandom` and `/dev/random`, where `runtime_file.c` mounts the guest's random devices as
on Linux, open the `NUL` device (writes taken and dropped) whose reads come from `ps_host_random_bytes`
(`BCryptGenRandom`), seek to 0, poll always ready and stat as a character device.
`win32/host_process.c` has no exec: `CreateProcessW` (arguments quoted for `CommandLineToArgvW`, only descriptors 0-2
inherited) and the caller waits for the new process and exits with its code; `ps_host_output_tee` and
`ps_host_on_exit` answer `ENOSYS` (no `fork`, no status in `atexit`). `win32/host_net.c` starts Winsock once and
maps its errors (`WSAEWOULDBLOCK` as `EAGAIN`), with `FIONBIO`, `FIONREAD` and `SIO_TCP_INFO`; socket descriptors are
`SOCKET` values and there are no socket pairs (`EOPNOTSUPP`, logged with a count). Recovery points are `PsHostRecoveryPoint` (`sigjmp_buf` on Linux, `jmp_buf` until the win32 fault handling
brings its own); the GPU library reaches the runtime's thread-local one through `runtime_fault_recover_slot`
there, since a DLL cannot import an executable's thread-local variable.

What the Windows port changed in code Linux runs too (feat/614-win32), for the Linux regress to
watch, all meant to behave as before:

- `runtime_socket.c`: `recvfrom` and `sendto` go through `ps_host_net_receive`/`_send`, `recvmsg`
  and `sendmsg` on Linux; `socketpair` through `ps_host_net_socketpair`; a socket's event queue
  readiness through `ps_host_net_ready` (`ps_host_fs_poll` on Linux).
- `runtime_mutex.c`: a condition's clock goes with each timed wait (`pthread_cond_clockwait`) instead
  of its attributes.
- `runtime_trophy.c`: `trophyset.json` streamed into `<path>.tmp` and renamed (was built in an
  `open_memstream` buffer first); trophy times parsed with `sscanf` instead of `strptime`.
- `runtime_savedata.c`: a slot's used size from the `ps_host_fs_stat_link` that `ps_host_fs_walk` makes
  of each file, not `nftw`'s stat.
- `runtime_ult.c`: `sceUltUlthreadExit` jumps back through `PS_HOST_RECOVERY_JUMP`, which does not
  save and restore the signal mask (`sigsetjmp` savemask 1 before, 0 now); the exit is refused
  inside exception delivery and while a runtime lock is held, so the mask is the same at both ends.
- `game_info.c`: the install folder (title patch files) is `PS_INSTALL_DIR`, else the nearest folder
  above the executable holding `patches/` (was always two folders up).
- `psport_guest_memory.cpp`: when the host import probe fails on its shared view, it imports private
  memory too and says so in the reason (`private memory imports: the driver takes anonymous memory
  only`, RADV's case); the choice of mode is the same.

The guest's memory map, its region table and the low allocator (`runtime_low_map`, host memory the
guest sees in `[0x0800000000, 0x1000000000)`) stay in `runtime_memory.c` and reach the host only
through `ps_host_vm_*`; the GPU library's chunk allocator hands the runtime a `PsHostGpuShare`
(an exported dma-buf, which `ps_host_gpu_memory_map` views at the guest's addresses and over the
backing view) or none (the memfd imported in place) or an errno (the chunk stays in the memfd), and
`-no-pie` stays in `build.sh` as the Linux half of
`ps_host_vm_keep_heap_low`. The guest's thread pointer is set and read (the guest exception's
context) through `ps_host_tls_*`; the loader finds the guest's `mov r64, fs:[0]` (any of the 16
registers) and rewrites it to GS with `ps_host_tls_tcb_offset` as its displacement. The mod view's cleaner process (`fork`,
`pipe2`, `dup2` in `loader_mods.c`) and RenderDoc's `dlopen(RTLD_NOLOAD)` stay where they are until
their own steps. Every fault and interrupt handler, the GPU library's included, reads and changes the
interrupted registers only through `PsHostFaultContext`: `Core::SignalDispatch` hands one on as its
`void *` context, and shadPS4's `Common::GetRip` and `Common::IsWriteError` read it through the
accessors. Thread signal masks and `pthread_kill` of the guest exception signal stay POSIX, and
recovery points are still set with `sigsetjmp`. The guest's mounts, case-insensitive lookups, save
commits and FreeBSD flags, modes and errors stay in `runtime_file.c` and `runtime_socket.c`, which
reach the host only through `ps_host_fs_*` and `ps_host_net_*`; files read whole go through stdio
streams `ps_host_fs_open_stream` opens, tree walks through `ps_host_fs_walk`, and the socket calls themselves (`socket`, `accept4`, `ppoll`, ...) stay
the BSD ones. Guest threads, their mutexes, condition variables and read-write locks stay
plain pthreads (portable), and the user-level threads (`runtime_ult.c`) run each one to its end on
the worker's own stack, with no context switch to move.
The environment is read through `runtime_env`, `runtime_env_int` and `runtime_env_flag`
(`runtime_config.c`) over `ps_host_env_get`, which `runtime_quirks.c` calls directly because
`psport-gpu-capabilities` links it without `runtime_config.c`.

`src/platform/host_features.inc` is the capability table, an X-macro list like the quirk registry:
`FEATURE(id, area, sz_desc)` names a capability, `FEATURE_HOST(id, os, state, sz_reason, sz_ref)` gives
its state on one OS (`LINUX`, `WIN32`, `MACOS`; `IMPLEMENTED`, `EXPERIMENTAL` or `UNAVAILABLE`, with
how or why not and the decision or task behind it; an OS without a row has none) and
`FEATURE_PROBED(id)` marks one decided at run time too. Empty texts, unknown areas, OSes and states
fail the build (`host_features.c`). The GPU library reports its probes with `ps_host_feature_probed`:
the dma-buf and host memory import checks of `gpu/memory/psport_guest_memory.cpp` (`dmabuf=` and
`host_import=` of the `Guest memory: detected` line) during a launch, and for `--list-features`
`psport_gpu_probe_host_features` (`gpu/port/psport_host_features.cpp`): the `upscaler` rows
(`upscaler_fsr4`, `upscaler_fsr411`, `upscaler_dlss`, `frame_gen_fsr`) from a `Vulkan::Instance` made as
the game makes it (`PS_GPU_ID`, no window) with the game's own checks (`IsFsr4Int8Supported`,
`IsFsr411Supported`, `Dlss::Available`, `FrameGenerator::IsFsrSupported`), and the titles' rows through `Game::Title::pfn_probe_host_features` (Bloodborne's
`intel_tonemap_fix`, area `cpu`).

`psport --list-features` runs those probes (their log lines on stderr), prints the table as JSON and
exits before any game is loaded: `os`, then per feature `id`, `area`, `description`, this host's
`state`, `reason` and `reference` (a probe that found the feature missing makes it `UNAVAILABLE` with
what it found as the reason), `probed_at_run_time`, `probe_result` (what the probe found, `null`
when it did not run) and every OS row under `hosts`. Launchers hide the options of an `upscaler` or
`cpu` row that is `UNAVAILABLE`. A launch logs `Host features: linux, N
implemented, N experimental, N unavailable (ids)` before the quirks, a `Host features: <id> probed:`
line per probe, and the exit summary repeats the counts with each probe's result.

## GPU library

`libpsport-gpu.so` holds the C++ side. `gpu/port/psport_gpu.cpp` is its C interface (`gpu/psport_gpu.h`),
with the window's input in `psport_gpu_input.cpp` and the game title's C view (`psport_gpu_title`)
in `psport_gpu_games.cpp`; the shadPS4 parts the port replaces (memory manager, kernel services) are
stood in for in `gpu/shim/psport_stand_ins.cpp`:

- `psport_gpu_register_kernel` registers the kernel event queues and the libraries that do not need a
  window: AvPlayer, Ngs2, Fiber, Font, FontFt, Audiodec, Videodec2, PngEnc, PngDec, JpegDec.
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

### Errors

The GPU library does not stop the game for what the game, the host or the environment can cause.
A host failure while the GPU starts (SDL video, the window, its video driver) makes
`psport_gpu_init` return -1, and the loader exits with `GPU initialization failed`; so does a
Vulkan failure at that point (no loader or instance, no device with Vulkan 1.3 and the required
swapchain, push descriptor, vertex attribute divisor and robustness2 extensions with
`robustBufferAccess2`, `robustImageAccess2` and `nullDescriptor`, no surface, swapchain or
allocator, or a layout, pipeline, frame image or command pool the library builds at start): the
object is left null, `Vulkan::ReportInitFailure` notes it and `Presenter::Create` returns null.
While the game runs, a swapchain that is out of date or suboptimal, or whose surface was lost or
could not be recreated, is recreated at the next present and the frame is skipped; a pipeline the
driver rejects is left null; a descriptor set that cannot be had from a fresh pool is not bound;
a command pool that cannot grow reuses its oldest buffer once the GPU is done with it.
An image whose memory cannot be allocated within the driver's budget makes the texture cache
evict up to 64 images unused in the current submission whose contents need no write-back
(`TextureCache::EvictForAllocation`, only for a new image `FindImage` creates) and try again, then
allocate over the budget; failing that the image has no memory (`Image::IsAllocated`): its views
are null views (`nullDescriptor`), so the draws reading it see zeros and those rendering to it
write nothing, and the uploads, downloads, copies, clears and barriers on it are skipped. A
sampler the driver refuses is replaced by a default linear sampler made at start. FSR's images and
the presenter's frame images retry over the budget too; FSR is left out of a frame that cannot
have them, and a frame image keeps its previous size.
`Vulkan::Check` (`vk_platform.h`) logs a failed call and leaves its result null for the caller to
check. A lost device cannot be recovered from: `Vulkan::StopOnDeviceLost` prints the driver's
fault report (`GPU fault report: ...`, from `VK_EXT_device_fault` where the device has it), the
breadcrumbs, `STOP: GPU device lost (<where>)` and the call summary, and exits with code 24. A value from
the game that the translator cannot map (a data or number format, tile mode, macro tile mode,
array mode, sample count, primitive type, blend, stencil or sampler enum) takes a fallback: a
compatible format (the same data format with another number format, D32 for depth), linear
tiling (no detiling), a 2D view, the neutral state (`One`, `Add`, `Copy`, `Keep`, `Always`, no
culling), or the image operation is skipped (a download larger than the image's memory, a blit or
tiling pass whose views or pipeline failed). Each fallback is logged with `LOG_ERROR_COUNTED`
(`gpu/shim/common/logging/log.h`): on its 1st, 2nd, 4th, 8th... occurrence, with the running count,
and listed with its total in the call summary at exit (`Calls: summary fallback`, FORK.md).
Assertions remain for the library's own invariants (texture cache registration and page tracking)
and for callers' contracts (`FindImage` with address 0, which the rasterizer filters out). A draw
is skipped when one of its render targets has no memory, when its pipeline was rejected by the
driver or cannot be specialized, or when its draw pipe packet would take over a quarter of the ring
(`DrawPipe::Begin` returns null; an ordered task that large runs in place). A pipeline cache entry
cut short, holding damaged SPIR-V or rejected by the driver while the cache is preloaded marks its
`Serialization::Archive` corrupt (nothing is thrown), and `PipelineCache::WarmUp` rebuilds the
cache. Images of the upscaler that cannot be allocated turn it off (its UI images keep the UI at
the scene's resolution and are tried again after two seconds, since each attempt waits for the
GPU; its display buffer leaves the game's buffer shown), a scene proxy that
cannot be allocated keeps its image at native size, and a missing object motion image drops the
motion vectors. Both leave a null attachment the pipeline declares, which only
`VK_EXT_dynamic_rendering_unused_attachments` allows: where the device lacks it, the draw is
skipped. A buffer that cannot be allocated within the memory budget is tried again over it while
the texture collector evicts by submissions (`TextureCache::NoteVramShort`); where the caller can
free memory itself (VRAM copies of guest memory, the staging pool) it does so first and only its
second attempt may exceed the budget, and compaction never does. One that still fails, has no
device address or cannot be bound to a guest memory chunk is left with a null handle
(`Buffer::IsAllocated`). Copies, fills and image transfers on it are skipped, a draw or dispatch
that binds it (or a stream buffer copy, the null index buffer or the stream-out counters that
failed) is skipped, a VRAM copy of guest memory that cannot be made leaves its blocks in place
(retired mirrors are freed and the allocation tried once more first), a chunk buffer leaves its
ranges to the arena and is tried again once a second, VRAM blocks whose GPU data cannot be copied
back stay in VRAM until it can, the staging pool frees its idle ring blocks and dedicated buffers
and tries again, then waits for the GPU to reuse a block (never inside a submission: the submit
callback or a pending operation gets no memory instead), and page table entries without staging
travel in the command buffer. An upload or readback without staging leaves its range CPU- or
GPU-modified, and an image that cannot be detiled stays dirty, so that the next use tries again;
occlusion queries and GPU timestamps whose buffers are missing are written by the CPU as without
them. An arena for guest memory without a device
address stops the launch as its other requirements do.

### Memory model

One model on every GPU: the GPU uses the game's memory where it is (GPU-visible system memory),
keeps often-read data in VRAM, and the command processor's work is translated. The backing is chosen
by what the device reports and does at startup, never by vendor (`PsGuestMemory::Usable`,
`gpu/memory/psport_guest_memory.cpp`):

- Sparse arena or memory module: the game's memory goes into a sparse arena when the device has
  `sparseBinding`, `sparseResidencyBuffer` and a graphics queue with sparse binding, and a test bind
  (64 MiB of VRAM bound, rebound and unbound in the arena's blocks) completes within 250 ms; else
  through the memory module in `gpu/layer/`, which binds the game's memory in place without sparse
  binding. The `HOST` quirk `nvidia_slow_sparse_rebind` ([QUIRKS.md](QUIRKS.md)) keeps NVIDIA's
  proprietary driver on the module: its rebinding stalls grow with the pages bound, beyond what the
  test reaches. `PS_LAYER_MEMORY=1/0` chooses by hand.
- Chunks: dma-buf chunks (exported cached system memory) when the driver's dma-buf maps at page
  offsets and CPU reads through it run at cached speed (1000 MB/s or more); host memory the GPU
  imports (`VK_EXT_external_memory_host`) when the dma-buf maps at offset 0 only or not at all, and
  dma-buf in whole chunks when the import fails too. Reporting the import is not enough to prefer
  it: RADV reports it and passes the probe, yet stalls on imported memfd memory.
  `PS_GUEST_MEMORY=dmabuf/host` chooses by hand.

One line says what was found and chosen:

```
Guest memory: detected device="..." driver=MesaRadv vram_mib=.. rebar_mib=.. host_visible_mib=.. uma=0 sparse_binding=1 sparse_residency_buffer=1 sparse_queue=1 sparse_bind_ms=.. dmabuf=offsets dmabuf_read_mbps=.. host_import=untested backing=sparse-arena backing_why="sparse buffers bound fast" chunks=dmabuf chunks_why="dma-buf maps at page offsets at cached speed"
```

`rebar_mib` is the heap behind a device-local, host-visible type (Resizable BAR when above 256 MiB);
`sparse_bind_ms` is negative when not measured or failed; `dmabuf` is `offsets`, `whole`, `slow`,
`unmappable`, `unsupported` or `untested`; `host_import` is `works`, `fails`, `unsupported` or
`untested`. A driver that can do neither stops the launch with a `Guest memory:` line. bbport 0.3's model (VRAM copies kept current by write tracking) is gone;
`PS_PC_MODEL`, `PS_AS_0_3` and the other switches of the old launcher are ignored with one log line.
Write traps on guest pages are shared by the runtime and the GPU library
(`gpu/layer/pslayer_write_traps.h`, `runtime_memory_trap`).

With the memory module, a block whose VRAM copy the CPU writes unannounced is caught by its write
trap, which is lifted under the trap lock before the hit is queued. A range the GPU writes may lie
in VRAM even when it covers its first or last block in part: those edge blocks are trapped, and the
GPU's bytes in them are copied back before the CPU's write sends them in place. Blocks the GPU once
wrote in place stay there until the loaders rewrite them whole (`PS_LAYER_RELOAD_RESET`) or the game
unmaps them (`PS_LAYER_UNMAP_RESET`). A reading binding resolved before is reused without the
module's walk while no block moved and no page became CPU-modified (`PS_BIND_MEMO`).

### Buffer V#s picked at run time

The recompiler binds a buffer by tracing its V# (the 4-dword buffer descriptor) to where the CPU
can read it when the draw is bound: user data registers or constant memory the SRT walker
flattens. Some shaders pick the V# at run time instead (Blood Omen 2's PS2 emulator: dword 0 is a
`Phi` of a user data register and a constant buffer read). Resource discovery
(`shader_recompiler/ir/passes/resource_discover_pass.cpp`) marks every dword that is not a sharp
source as dynamic and collects, through `Phi`s, the values it can take that the CPU reads
(immediates, user data, flattenable constant buffer reads); anything else makes the V# unbounded.
Statically traced V#s keep the bound-buffer path unchanged.

A dynamic V# becomes a `BufferType::Dynamic` binding (`resource_patching_pass.cpp`,
`PatchDynamicBufferSharp`): just before each access the shader hands the V# it computed to the
binding (`SetDynamicBufferSharp`, a private `uvec4`), and the access goes through the buffer
cache's page table as a paged buffer's does (`EmitContext::PagedPointer`): address from dwords
0-1, size as `AmdGpu::Buffer::GetSize` (`num_records`, times the stride when there is one),
translated block by block to a buffer device address. Out of range or on a block without an
entry, loads read zero and stores and atomics go to a trash buffer (GCN range checking). The
address calculation's compile-time parts come from the static dwords; a run-time dword 1 gives
the stride at run time, and a run-time V# that turns on swizzling or `add_tid` reads as out of
range. Shapes the path cannot follow (no `bufferDeviceAddress`, an inline constant buffer,
swizzling with a run-time dword 3) bind a null buffer (loads read zero, stores are dropped), with
a warning and a count. Each translated shader with dynamic V#s says so once (`buffer V#s picked
at run time`).

Sync rule: at bind time (`Rasterizer::BindDynamicBuffer`) every V# the shader can pick from the
CPU-readable sources (at most 16) is evaluated from the flattened user data, and its range is
prepared as a bound buffer's would be (`BufferCache::PrepareDynamicRange`): made resident with
current page table entries and, when the binding writes, counted as GPU-modified with its images
invalidated, the conservative rule paged bindings follow (`LayerPagedRecord` in layer mode,
`ObtainBuffer`'s arena path otherwise). The draw takes global barriers before and after, as paged
bindings do. A value the CPU cannot read (unbounded) reaches only memory other bindings already
made resident, and its writes are not tracked; the shader's log line says when it has one.

### Shaders the recompiler cannot translate

A shader is game data: an instruction, operand, format, resource or control flow the recompiler
does not handle, or IR it cannot follow, fails that shader's translation and never stops the port.
The recompiler's checks (`SHADER_ASSERT`, `SHADER_ASSERT_MSG`, `SHADER_UNREACHABLE`,
`SHADER_UNREACHABLE_MSG`, `shader_recompiler/translation_error.h`) throw `Shader::TranslationError`
(reason, file and line). Only its two entry points catch it, `Shader::TranslateProgram` (frontend
and IR passes) and `Shader::Backend::SPIRV::EmitSPIRV`, and return it in a `std::expected`, so no
exception leaves the recompiler and a shader that translates costs nothing more. The pipeline cache
(`PipelineCache::CompileModule`) logs `Shader 0x<hash>: not translated: <reason> (<stage> stage,
permutation <n>, <file>:<line>; <n> so far)` once per program permutation and remembers the failure
(`m_untranslated_programs`, `Program::untranslated_specs`), so the shader is not translated again;
the draws and dispatches that need it get no pipeline and are skipped. Both counts are in the call
summary: `GPU shader not translated by the recompiler` and `GPU draw or dispatch skipped: shader not
translated`. Unknown opcodes are still logged one by one as the frontend meets them, and fail the
translation when it ends.

Rule for `shader_recompiler/`: a condition a game, the host or the environment can cause is checked
with `SHADER_ASSERT*`/`SHADER_UNREACHABLE*`, never `ASSERT`/`UNREACHABLE`, and no function on that
path is `noexcept` (GCC's `-Wterminate` flags a direct throw in one). Code that runs outside the two
entry points must not throw: `Info` and `PushData` at bind time, the fetch shader parse that keys
pipelines (`Gcn::ParseFetchShader`, which catches its decoder's errors), the auxiliary tessellation
and discard shaders. It logs with `SHADER_LOG_FALLBACK` (the first time and at each power of two,
with the count) and takes a defined fallback. Only `DEBUG_ASSERT`s remain, compiled out of release
builds.

### Occlusion queries

A ZPASS_DONE event (the game's occlusion query) has the PS4's depth blocks write their running
sample counts. By default they are counted by Vulkan occlusion queries
(`renderer_vulkan/vk_occlusion.*`): a chain of query segments, one always active once the game
has used them, cut at every event and at every boundary a query may not span (a render pass, a
command buffer: `Scheduler::CarriedScope`). Outside a render pass one dispatch
(`host_shaders/occlusion_resolve.comp`) adds the ended segments up in order and writes each
pending event's total through the game memory's device address, before any later label
(`PS_OCCLUSION_LABEL_ORDER=0` lets labels go ahead); the valid bits stay clear until then. Each
segment is first scaled by its render pass's factor (8.8 fixed point, from the rasterizer's
`target_scale`), so a pass the upscaler draws smaller (the reduced scene) or larger (the
output-size UI) counts as at the game's resolution. `PS_OCCLUSION_QUERIES=0` keeps the fixed
sequence written when the event is decoded (everything visible); the default is one constant,
`OCCLUSION_QUERIES_BY_DEFAULT` (`Rasterizer::OcclusionTranslated`). On by default since 13 titles ran
with no regression and Bloodborne gained 8 % (the game culls by the real counts). Diagnostics: `PS_OCCLUSION_TRACE=N` (the first N events),
`PS_OCCLUSION_SAMPLES=N` (the samples the game read, N queries), `PS_OCCLUSION_READ_TRACE=1` (when
the game first reads its counters) and the 5-second line of `PS_FRAME_STATS`.

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
| `default_controls` | its keyboard and mouse layout as `psport.ini` `key.`/`pad.`/`mouse_look_` lines over the neutral one (Bloodborne: close to Dark Souls III on PC; Black Ops and Black Ops II: the PC games' layout); the player's lines win |
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
| `drives_frame_rate` | true once its hooks pace the game's frames: the video out shows each frame as it comes, whatever flip rate the game asks for (Bloodborne's frame rate hooks) |
| `mouse` | the mouse in the game's code: native mouse look (`camera_look`: Bloodborne's camera turn detoured; Black Ops and Black Ops II, the counts added to their own mouse input), a pointer in its menus (`wants_pointer`, `window_event`, `filter_pad`), a look at each pad sample (`filter_pad`: Black Ops' crosshair) |
| `display_aspect` | the output's aspect ratio; true when its camera and UI follow it (Bloodborne), else the core keeps the game's 16:9 picture in the middle ([upscaler.md](upscaler.md#output-aspect-ratios-2026-10-17)) |
| `report_stats` | profile diagnostics with each frame statistics report |
| `label_release` | the game's label release code, for the free check's `PS_LABEL_TRAP=2` |
| `small_object_pool` | the layout of the pool holding its GPU labels, for the free check (`PS_FREE_CHECK`) |
| `texture_substitution` | textures of the game it knows by their bytes and the pictures the port draws for them: the texture cache asks it about each upload of a single-level block-compressed texture (and, for small shapes, at their binds), samples the picture or a copy of the texture with pictures in its cells while they are drawn, and redraws them when its generation changes (`texture_substitutes.*`); a profile with `glyphs` leaves it empty |
| `glyphs` | its button glyphs as data (`gpu/games/common/glyphs.h`): textures known by shape and hash, given or computed at start from the user's files through an extractor of their format, each replaced whole by the port's picture of its inputs or an atlas with glyphs in some cells ([ADDING_A_TITLE.md](ADDING_A_TITLE.md#6-button-glyphs)) |

Hooks and patches in a game's code go through `gpu/port/psport_guest_code.h` (`PsGuestCode`), the
same for any profile: `InstallHook` (a trampoline that hands a host function every register and may
let it return from the game's function), `InstallDetour` (a stub the profile writes with `Emitter`)
and `RedirectOperand` (an instruction reads a value next to the image instead of its constant,
`MapNear`). The host functions they call (a hook's handler, `Emitter::CallAbs`) are declared
`PS4_SYSV_ABI`, since the trampolines pass arguments in rdi, rsi and rdx on Windows too. Each
compares the site with the bytes the profile expects and returns a
`PsGuestCode::Error` instead of writing when they differ; each hook is logged as `Guest code: <name>
hooked at <address>` or `... not hooked: <why>`.
`PsGuestCode::KeepWritable` is for a game module that writes the game's code itself and switches
some of it while the game runs (a title's online extension, below): the image's code becomes read,
write and execute, and the layer's later writes leave it so; nothing calls it unless such an
extension starts.

A profile makes a title faster or easier to diagnose; it never makes it work. The core must run
every title correctly on its generic paths, which `PS_GAME_PROFILE=none` checks. Black Ops and
Black Ops II (`gpu/games/black_ops/`: `title.cpp`, their keyboard and mouse layout; `profile.cpp`,
the mouse camera of `mouse_camera.cpp` feeding their own mouse input, unless `PS_NATIVE_MOUSE=0` or
another build, and `crosshair.cpp`, `cg_drawCrosshair` held on with keyboard and mouse while the game
shows its HUD, unless `PS_FORCE_CROSSHAIR=0`) bring no patches, nor does Atelier Ayesha DX
(`gpu/games/ayesha/`: its profile is its button glyph table, one upside-down UI atlas). Bloodborne has the most (`gpu/games/bloodborne/`: `title.cpp` and, for the launch, `patches.cpp`: the
`PS_FPS` presets 30/60/90/uncap (by hooks in its code, `frame_rate.cpp`, unless `PS_FRAME_HOOKS=0`
or the executable is not 1.09), `PS_PATCHES`, the effect switches and `model_lod` of `psport.ini`,
the `PS_RENDER_RES` scene size, the Intel tone mapping and online-choice fixes, all from
`patches/Bloodborne.xml`; `game_check.cpp`: the 1.09 executable and intact shaders; the in-game menu
pages, `game_menu.cpp`; its profile in `profile.cpp`, with Gnm entry hooks and `gnm_table.inc`,
guest hooks and heap sites; its button prompts for the device in use, the glyph table in
`button_prompts.cpp` read from `menu/common.tpf.dcx` by `fromsoftware_tpf.cpp`, unless
`PS_BUTTON_PROMPTS=0`), listed in `gpu/games/game_profile.cpp`. What any title's profile may share
lives in `gpu/games/common/`: `glyphs.cpp` substitutes a title's glyph table and
`button_prompt_art.cpp` draws the prompt icons (key caps, the mouse, another gamepad's buttons) in
the palette the table carries.
Another title runs unpatched, on the console's 60 Hz vblank.

## Launch scripts

A launch runs no Python, except for online play:

| Script | Use |
|---|---|
| `scripts/online.py` | addresses for `PS_ONLINE=1` from the title's servers (`psport --online-overrides`; it probes the shadNet WebAPI for the summon service), and the command of the title extension's local game server (`local-server`); the bundled launcher imports it too |
| `scripts/bbsc_gameserver/` | BBSC's game server (Bloodborne's extension), started by `run.sh` only with `PS_BBSC=1` and `PS_BBSC_LOCAL_SERVER` |
| `scripts/sce_file.py` | `param.sfo` and SELF/ELF readers for the Python tools (`tools/got_import.py`, `tools/re/`) |
| `tools/dcx_check.py` | every `.dcx` file of a Bloodborne folder checked (the launch checks the shaders only) |
| `tools/check_os_code.py` | OS conditionals and OS or SDL3 includes found outside `src/platform` and `gpu/platform` (`gpu/third_party`, `gpu/shadps4` and `gpu/net/shadps4` exempt); `--strict` exits 1 while any remain; the last run reported 17 findings in 16 files (2 conditionals, 15 includes) |
| `tools/check_licenses.py` | `LICENSES.json` against the tree: schema, paths, `LICENSES/<id>.txt` for every SPDX id, every `LicenseRef` defined, vendored trees and tracked files covered; run by `tools/regress/night.sh` in report mode, `--strict` exits 1 on any finding |

## Online play

`PS_ONLINE=1` (launcher: *Online*) routes the game's network and PSN imports to the online module
(`gpu/net/`, `libpsport-net.so`, loaded by `src/net/runtime_net.c` on the first such import). The
servers a game talks to are its title's data (`Game::Title` `online`), never the core's:

| Field | Bloodborne |
|---|---|
| `sz_game_server` (the game's own server, as it addresses it) | `https://ss4.scej-network.jp:20443` |
| `sz_community_server` (answers for it on the same port; `PS_COMMUNITY_SERVER` names another) | `http://play.seamlesshunt.com` (Seamless Hunt) |
| `community_api_port` (where the community server sends the game for its API) | `18671` |
| `sz_summon_route` (the path of its summon calls on that API) | `/summon_messenger` |
| `extension` (its own part of the online module, below) | `bbsc` (BBSC, Bloodborne's seamless co-op), switch `PS_BBSC` |

`psport --online-overrides (--game <dir> | --title <TITLE_ID>)` prints them as `key=value` lines
(`game_server`, `community_server`, `community_api_port`, `summon_route`) and exits before any game
is loaded; a title without online servers prints nothing. `run.sh` passes that to
`scripts/online.py overrides --title-online`, which writes `user/host_overrides.json` for the launch
(`SHADPS4_HTTP_HOST_OVERRIDES_JSON`): the game server to the community server, and the summon route
on the community API port to the shadNet WebAPI (`PS_SHADNET_WEBAPI`) when that answers the summon
probe. A title without servers gets an empty file, so a file another game's launch left is not
used; `user/host_overrides.custom.json`, when present, is used as it is for any game.

The shadNet server is the core's, the same for every game: `PS_SHADNET_SERVER`, by default
`play.seamlesshunt.com:31313` (Seamless Hunt; `run.sh`, `scripts/online.py`, the launcher), and its
WebAPI `http://<host>:31315` unless `PS_SHADNET_WEBAPI` names another. shadPS4's public server
(`srv.shadps4.net`) accepts only shadPS4's own builds since 2026-10-12; the launcher replaces that
earlier default, and The Hunter's Dream as game server, in saved settings (`online.OLD_DEFAULTS`).
After login the client reports its version (command 13, `psport <VERSION>[-<commit>] (...)` from
`VERSION` through `gpu/net/CMakeLists.txt`, the commit only between releases; `PS_SHADNET_CLIENT_VERSION` overrides it). A server that refuses it
(notification 19) ends the login: shadNet stays off for the session, without reconnects, and the
player is told; one that never answers counts as accepting after 15 s. Notification 18 (a service
refused, e.g. matchmaking full) is shown to the player.

The module learns the summon route through `PsNetHost` `sz_summon_route` (`PSPORT_NET_ABI_VERSION`
3; a runtime and a module of different versions refuse each other and the game stays offline). The
route picks the title's summon client (`PsNetTitle::SummonClient`, `gpu/net/psport_net_title.h`),
code under `gpu/games/<game>/net/` registered for that route, whether or not the title's extension
runs; `http.cpp` hands it every request (before its worker, before sending, once answered and when
finished, `gpu/net/shadps4/core/libraries/network/http_plan.h`) and it acts on its own calls.
Bloodborne's (`gpu/games/bloodborne/net/summon_client.cpp`, `/summon_messenger`) carries the host
placement header, the seamless route of `SHADPS4_BLOODBORNE_SEAMLESS_COOP=1` and the exchange
capture of `SHADPS4_CAPTURE_BLOODBORNE_SUMMON=1`; for a title that declares no route, either switch
still forces it, logged as `Co-op: summon route /summon_messenger forced`. The module logs `Co-op:
summon route ... (the title's)` or that the title declares none.

A title may carry its own part of the online module (`Game::OnlineExtension` in `online`): code
under `gpu/games/<game>/net/` that needs the network libraries, built into `libpsport-net.so`
(`gpu/net/CMakeLists.txt` takes every `games/*/net/` folder) and registered there by name
(`gpu/net/psport_net_title.h`). It stays off unless the player sets its switch (`sz_switch`=1):
`src/net/runtime_net.c` then names it in `PsNetHost` `sz_extension` (ABI 3) and the module starts
it; the log says `Online: the title's extension <name> is on` or `off (<switch>=1 turns it on)`.
Once the image is mapped and patched, before its first instruction, the runtime calls the module's
optional `psport_net_image_ready`, which hands the image to the started extension. What such an
extension may ask of the GPU library is generic: `psport_gpu_add_event_filter` (the window's events
before the profile's), `psport_gpu_overlay_banner` (a line over the game) and
`PsGuestCode::KeepWritable`. Its own game server, run on this PC when the community server's does
not answer, is declared there too (`sz_local_server`, `sz_local_switch`, `sz_status_path`) and
printed by `--online-overrides` (`extension`, `extension_switch`, `local_server`,
`local_server_args`, `local_switch`, `status_path`).

Bloodborne's extension is BBSC, its seamless co-op (`gpu/games/bloodborne/net/`: BBSC's own files
under `core/` and `imgui/`, unchanged but for `Core::Bbsc::Started`, and `bbsc_glue.cpp`). Started
(`PS_BBSC=1`), it turns on BBSC's default switches (`SHADPS4_BBSC_*`, `SHADPS4_BLOODBORNE_*`, listed
in `core/bbsc_persistence.h`; `=0` turns one off) and the P2P relay; at `psport_net_image_ready` it
keeps the code writable, gives its cinematic camera (F1) the window's events and installs its hooks
(through `Core::InstallGuestCodeHook`, `gpu/net/include/core/cpu_patches.h`, which is
`PsGuestCode::InstallHook`) and byte patches, each checked against CUSA03173 01.09's bytes. The
network side of BBSC is in the inherited libraries: its signaling packets (`np_signaling_bbsc.h`,
handed to its code only while it runs, through the extension's `pfn_signaling_packet`) and the room
replay; the extension also traces `sceNpMatching2LeaveRoom` (`pfn_leave_room`). The summon broker's
connect-first area, stale record cleanup and fast sign are in Bloodborne's summon client.

Switching it on: the launcher's *Online* page, "Seamless co-op (BBSC)" (`online_bbsc`, off by
default), sets `PS_BBSC=1`; every player needs it and the same server, a shadNet server with BBSC's
game server next to it (Seamless Hunt's). When that game server does not answer, BBSC's own
(`scripts/bbsc_gameserver/`, BBSC's files) can run on this PC: `PS_BBSC_LOCAL_SERVER=1` always,
`auto` when `<community>:20443/bb-eu/ss.info` does not answer within 5 s, unset or `0` never.
`run.sh` asks `scripts/online.py local-server` (from the title's `--online-overrides` lines), starts
it on 127.0.0.1 with its data and logs in `<user>/bbsc/`, points `PS_COMMUNITY_SERVER` at it and
stops it with the game; summons then go to the shadNet server's WebAPI, messages and bloodstains
stay on this PC.

## Other parts

- `launcher/`: bbport's GTK4 launcher, built around Bloodborne.
- `packaging/`: `port.nix` (build in Nix's sandbox) and the AppImage script.
- `patches/Bloodborne.xml`: community patches for Bloodborne.
- `tools/`: regression runs, GCN disassembler (`gcn_dis.sh`), FSR 4 asset tools, scripted-run
  helpers, `import_outparams.py` and `import_returns.py` (generate `src/runtime/imports/import_*.inc`).
- [history/](history/): bbport's design notes, plans, measurements and change logs in English; the
  Russian originals, under the same names, are in [docs/ru/](../ru/).
