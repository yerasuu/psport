# Bringing up a new title

How to take a new PS4 dump from "does not start" to "reaches play". The components named here are
described in [ARCHITECTURE.md](ARCHITECTURE.md).

## 1. Run it

The dump must be an extracted game folder (`eboot.bin`, `sce_sys/param.sfo`, `sce_module/`, data)
with its update merged in. Titles that require PS VR (`require_ps_vr` in `param.sfo`) are not
supported.

```bash
bash build.sh
PS_GAME_DIR=/path/to/dump PS_PERMISSIVE=1 PS_SAVE_LOG=1 bash run.sh
```

- `PS_PERMISSIVE=1`: an import psport lacks returns 0 instead of stopping the game at its first
  call. Without it the first missing import ends the run with its NID and name, which is the
  quickest way to see what a title needs first.
- `PS_PERMISSIVE_OUT=error`: with `PS_PERMISSIVE=1`, missing imports that write through a pointer
  argument return `0x80020016`, so the game takes its error path instead of reading memory nothing
  wrote.
- `PS_SAVE_LOG=1`: the output also goes to `<data>/logs/<time>.log`.
- `PS_GAME_PROFILE=none`: run a title that has a profile without it.
- `PS_SYS_MODULES=<folder>`: the user's own dump of system modules, for comparison only (see
  [Missing system libraries](#missing-system-libraries)).

## 2. Read the log

Milestones, in order:

| Line | Meaning |
|---|---|
| `Linked modules: ...; N native bindings, N imports left to the host runtime` | the loader linked the eboot and its modules |
| `Runtime: imports N: host N, game modules N, system modules N, modules the game loads N, stubs N, without implementation N` | every import was bound; the lines before it list each stub and each import without implementation |
| `Mapped N bytes, N segments; applied N relocations` | the image is in place |
| `Starting linked module ...` / `Module N initializer returned 0` | bundled modules started |
| `Entering original x86-64 code at guest offset 0x...` | the game runs |
| `Runtime: guest thread '...' created` | the game initialised |
| `Presenter: first game frame presented` | the GPU path works end to end |

Loader errors (`ERROR: ...` from `src/loader_*.c` or `src/probe.c`) stop before the game runs and
name the file and the limit or structure they rejected.

### The call report

While the game runs, `src/runtime_calls.c` logs the first call of every import that does no real
work, and when the process ends prints a summary sorted by call count:

```
Calls: summary: <n> unimplemented, <n> stub, <n> failed, <n> status (import and error code)
Calls: summary unimplemented <calls> <NID> (<name>)
Calls: summary stub <calls> <NID> (<name>)
Calls: summary failed <calls> <NID> (<name>) -> 0x8xxxxxxx
```

All line formats: [FORK.md](../../FORK.md#call-report). Read it in this order:

1. The last lines before a stop or a fault (`Guest fault`, `Host fault`, `STOP:`): the import or
   the GPU state that ended the run.
2. `unimplemented` entries the game calls often or right before it stalls.
3. `stub` entries whose result the game uses (a stub returns 0 and writes no outputs).
4. `failed` entries: an implemented function that returned an error. Compare with what the title
   expects before treating it as a bug; many failures are normal (no file, no event).
5. `GPU [...] (STUBBED)` lines: GPU library functions that do not do their work.

The report never hides a call: an unimplemented or stubbed function is always logged and counted.

### Stalls and faults

- `Forcing submitDone` lines that keep growing: the game waits on the GPU.
- `SIGUSR2` to a thread of `psport` dumps that thread's frames; the watchdog (`PS_TIMEOUT`
  seconds, `--timeout`) prints `STOP: watchdog timeout`, every thread's stack and the call report,
  then exits.
- Diagnostic variables (`PS_TRACE_MEM`, `PS_TRACE_SEMA`, `PS_TRACE_EVENTFLAG`, `PS_AUDIO_STATS`,
  `PS_NGS2_TRACE`, `PS_FIBER_TRACE`, ...): [FORK.md](../../FORK.md#diagnostics).
- `tools/gcn_dis.sh` disassembles a GCN shader the recompiler rejected.

## 3. Implement what is missing

Pick the layer that owns the library:

| Library | Where | How |
|---|---|---|
| libkernel, libc hooks, threads, memory, files, saves, pad, audio out, system and user services, NP offline, PlayGo, AppContent, Ajm, Audio3d, ... | `src/runtime_<area>.c` | add an `ABI` function and an entry `{"<symbol name>", function}` to the file's `RuntimeExport` table; `runtime_<area>_resolve` finds it with `RUNTIME_LOOKUP` |
| a library with no file yet | new `src/runtime_<area>.c` | a table and `runtime_<area>_resolve`, declared in `src/runtime.h` and added to the chain in `runtime_resolve` (`src/runtime.c`); `build.sh` compiles every `src/runtime*.c` |
| GnmDriver, VideoOut, AvPlayer, Videodec2 (inherited shadPS4 code) | `gpu/shadps4/core/libraries/` | register with `LIB_FUNCTION`; list the change in [FORK.md](../../FORK.md#changes-to-inherited-shadps4-code) |
| Ngs2, Audiodec, Fiber, Font/FontFt, PngEnc, or a new C++ library | `gpu/<library>/` | a `RegisterLib` with `LIB_FUNCTION(nid, library, version, module, function)`, called from `bbgpu_register_kernel` (`gpu/shim/bbgpu.cpp`), the sources added to `gpu/CMakeLists.txt`; ported code is listed in [FORK.md](../../FORK.md#ported-components) |

Notes:

- The C runtime matches by symbol name: the NID must be in `src/import_names_all.inc` (or in the
  `extra` list in `runtime_symbol`), otherwise `runtime_lookup` cannot name it. The GPU library
  matches by NID.
- The C tables are asked before the GPU library, in the order of `runtime_resolve`; the first
  table with the symbol wins.
- Return SCE error codes (`0x8002xxxx` for libkernel) or `-1` with a FreeBSD errno for POSIX
  functions, as the console does: the call report watches results through
  `src/import_returns.inc`. A result that is a polled state rather than a failure (no event,
  timed out, busy) goes in `src/import_statuses.inc`.
- Write every output argument the function documents.
- `runtime_stub` answers success without doing anything; use it only where success with no effect
  is the console's behaviour for this setup (no PSN connection, for instance), since every call to
  it is reported as a stub.

### Missing system libraries

`PS_SYS_MODULES=<folder>` links the user's own system modules for libraries psport does not
provide, `PS_HLE=libSceX` keeps one to psport and `PS_LLE=libSceY` links one psport implements, to
compare the two. This is a diagnostic stopgap, never a dependency: nothing from a system dump is
shipped, every title in `tools/regress/titles.txt` must run without one, and each library a title
needs gets its own implementation.

## 4. Rules for fixes

From [CLAUDE.md](../../CLAUDE.md):

- Fix the underlying behaviour. No game-specific or shader-specific hacks in `src/`, `gpu/shim/`,
  `gpu/shadps4/`, `run.sh` or `build.sh`: no title IDs, no eboot addresses, no shader hashes.
- Optimisations that only make sense for one title go in that title's profile
  (`gpu/games/<game>.cpp` or `gpu/games/<game>/profile.cpp`, a `Game::Profile` listed in
  `gpu/games/game_profile.cpp`): hooks in its code, code ranges that write per-frame data, known
  copy shaders, overlap checks. The core keeps only the generic mechanism and must behave the same
  without a profile. A profile speeds a title up; it never makes it work. Check with
  `PS_GAME_PROFILE=none`.
- No tests in the repository. When a change rests on how the console behaves and the behaviour is
  not documented, confirm it on hardware with an OpenOrbis homebrew test and share it with the
  change.
- Match the surrounding code; follow `.clang-format`.

## 5. Check with tools/regress.sh

Every change is checked against all titles, not only the one being brought up:

```
[SYS_MODULES=<folder>] tools/regress.sh [-j 2] [-o out/regress/<time>] [-c <earlier results.json>] \
    [-n <names>] [-t <titles file>] [-s <toolchain nix file>] [-u <earlier output folder>] <games root>
```

- `tools/regress/titles.txt` lists each title: name, title ID, folder under the games root, pad
  schedule, and its environment (`PS_PERMISSIVE=1` for titles under bring-up). `$NAME` values come
  from the caller's environment; an entry whose variable is empty is left out, so nothing
  user-specific lives in the repository. Add the new title here.
- Schedules (`tools/regress/*.sched`) drive the scripted pad: `<second> shot`,
  `<second> press <a+b> [hold]`, `<second> hold <a+b> [seconds]`, `<second> release`,
  `<second> end`. `generic.sched` presses cross and options past logos and prompts; a title with a
  known route gets its own (`ryza-play.sched`, `symphonia-long.sched`, ...).
- Each title runs in a headless gamescope with `PS_AUDIO=none` and `PS_PAD_HOST=0`; at most `-j`
  at once. Results: a table on stdout, `results.tsv` and `results.json` in the output folder with
  each title's log and frame dumps; milestones `loaded`, `entered`, `initialized`, `gpu`, `frames`,
  `audio`, the crash signature, the `no impl` count and `forced` submitDones.
- `-c <earlier results.json>` lists regressions (a milestone lost, a new or different crash) and
  exits with 1.
- `-u <earlier output folder>` starts each title's user folder (saves, settings) as a copy of its
  folder there, so a run can load what an earlier one saved.
- `tools/regress/perf.txt` holds the titles whose schedules measure the frame rate in play
  (`bloodborne-perf.sched`: from a save, with `PS_FRAME_STATS=1`). Run it alone on the machine:
  `tools/regress.sh -t tools/regress/perf.txt -n bloodborne -u <folder with the save>`.
- `tools/regress/report.py <folder>` classifies an output folder again.

Milestones and evidence: [FORK.md](../../FORK.md#regression-runs).
