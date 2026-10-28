# Profiling psport

Tools for finding where the port spends its time and for checking that a change made it faster.
None of them is in a normal build: `bash build.sh` without the variables below compiles every zone to
nothing and links nothing extra, and the RenderDoc hook does nothing unless its variable is set.

| Tool | Build | Runs a game | Output |
|---|---|---|---|
| Tracy zones and frame marks | `PS_TRACY=ON` | to collect, yes | Tracy's profiler, or `trace.tracy` and `zones.csv` |
| perf call graphs of a regress run (`tools/profile/perf.sh`) | any; `PS_PROFILE=1` for `out/psport`'s own frames | yes (headless) | `flame.svg`, `perf.folded`, `top.tsv` |
| Comparing two builds (`tools/profile/compare.sh`) | two checkouts | yes (headless, one after the other) | `compare.txt` |
| One-frame RenderDoc capture | any | yes | an `.rdc` file |

Everything that runs a game belongs to the night session: by day the machine is the user's. The
scripts run games the way `tools/regress.sh` does (a headless gamescope, `PS_AUDIO=none`,
`PS_PAD_HOST=0`): no window, no sound, none of the user's controllers.

## Tracy

```
PS_TRACY=ON PS_PROFILE=1 bash build.sh
```

The first such build checks out `gpu/third_party/tracy` (Tracy's client, a submodule pinned at
v0.13.1; `update = none` in `.gitmodules` keeps it out of a plain `git submodule update`). The GPU
library compiles `TracyClient.cpp` with `TRACY_ON_DEMAND` (nothing is collected until a profiler
connects), `TRACY_ONLY_LOCALHOST` and `TRACY_NO_BROADCAST` (it listens on 127.0.0.1:8086 only and
does not announce itself on the network) and `TRACY_NO_CRASH_HANDLER` (the port's write traps and
fault reports own those signals). The runtime (`out/psport`) is compiled with `TRACY_ENABLE` and calls
the client's C interface exported by `libpsport-gpu.so`. A later `bash build.sh` without
`PS_TRACY` configures the GPU library back to a normal build.

Even with no profiler connected, the zones cost time: with `PS_PROFILE=1` as well, Bloodborne's perf
scene ran at 187.9 FPS against 205.5 for the normal build (−8.6 %, 2026-10-18). Compare a Tracy
build only with another Tracy build.

How it switches: shadPS4's code includes `common/debug.h` and `<tracy/Tracy.hpp>`, which resolve
to psport's stand-ins in `gpu/shim/`. Without `TRACY_ENABLE` they define every macro as nothing; with
it they include shadPS4's own `common/debug.h` and Tracy's real header, so shadPS4's existing
markers (`RENDERER_TRACE`, `HLE_TRACE`, `FRAME_END`) come back as they are upstream. GPU-side Vulkan
zones stay off (`TRACY_GPU_ENABLED 0`, as upstream).

Zones:

| Zone | Thread | Where |
|---|---|---|
| `Stage A decode` | GPU command thread (`shadPS4:GpuCommandProcessor`) | one resumption of a queue's decoding task, `Liverpool::Process` |
| `Stage B packet` | draw recording thread (`psport:DrawRec`) | one packet of the draw pipe, `DrawPipe::Run` |
| `Draw scan` | `psport:DrawScan` | register scan of one submission, `DrawPreparation::ScannerLoop` |
| `Draw prep` | `psport:Prep<N>` | one claimed submission, `DrawPreparation::WorkerLoop` |
| `Shader compile` | wherever pipelines are built | `PipelineCache::CompileModule` (GCN to SPIR-V and the module) |
| `Graphics pipeline`, `Compute pipeline` | same | pipeline construction, `vkCreate*Pipelines` included |
| `Present` | present or swap thread | `Presenter::Present` |
| `Swap` | `psport:Present` | one queued presentation, `VideoOutDriver::SwapThread` |
| `mutex wait`, `cond wait`, `sema wait`, `event flag wait`, `sleep` | the game's threads | the runtime's blocking waits (`src/kernel/`), the same ones `PS_FRAME_STATS` times |
| shadPS4's `RENDERER_TRACE` / `HLE_TRACE` | various | draws and dispatches in `vk_rasterizer.cpp`, page and region tracking, texture cache, `sceGnm*` submits, event queues |

Imports bind straight to their functions (there is no dispatcher to instrument), so the runtime's
zones sit on its waits: they show which guest thread is blocked, and on what, while the GPU threads
work. A frame mark is set at each flip the VideoOut present thread performs (shadPS4's `FRAME_END`
in `VideoOutDriver::PresentThread`), so Tracy's frame view is the game's frames.

Looking at a run: Tracy's profiler from the same nixpkgs psport pins (the protocol must match the
client's version):

```
"$(nix-build --no-out-link packaging/nixpkgs.nix -A tracy)/bin/tracy"     # GUI, connect to 127.0.0.1
```

That opens a window, so by day only. At night `tools/profile/perf.sh -z` records the same window it
samples with `tracy-capture` into `trace.tracy` and exports the zone statistics with
`tracy-csvexport` (`zones.csv`: count, total, mean, min, max per zone), which `compare.sh` diffs; the
trace file opens in the GUI the next day.

## perf

The GPU library always keeps frame pointers (`-fno-omit-frame-pointer` in `gpu/CMakeLists.txt`, for
the watchdog's thread dumps); `PS_PROFILE=1` adds them to `out/psport`, so `--call-graph fp` walks the
runtime as well. `-g dwarf` unwinds without them, at a much larger `perf.data`. The game's own code
has no symbols and shows as addresses in its anonymous mappings.

```
tools/profile/perf.sh [-n NAME] [-t TITLES] [-C CHECKOUT] [-o OUTDIR] [-w SECONDS] [-d SECONDS] \
    [-g fp|dwarf] [-F HZ] [-z] [-u EARLIER] GAMES_ROOT
```

It starts `tools/regress.sh -j 1` for one title of a titles file (default `tools/regress/perf.txt`,
the titles whose schedules play rather than sit in menus; the first one unless `-n`), waits for the
title's game process (recognised by the data folder regress gives it), lets it run `-w` seconds
(default 60, past loading), samples it with `perf record -p` for `-d` seconds (default 60, 999 Hz)
and lets the schedule finish. In OUTDIR (default `out/profile/<time>-<name>`):

- `perf.data` and `perf.folded` (stacks folded by FlameGraph's `stackcollapse-perf.pl`);
- `flame.svg`, the flame graph;
- `top.tsv`: share of the samples, object and symbol, by self time (above 0.05 %);
- `regress/` and `regress.log`: the regress run itself (log, dumps, results);
- with `-z`, `trace.tracy` and `zones.csv` (needs a `PS_TRACY=ON` build).

perf is the host's when it has one, else Nix's; FlameGraph and Tracy's tools come from
`packaging/nixpkgs.nix` (`nix-build -A flamegraph`, `-A tracy`). With
`kernel.perf_event_paranoid` above 1 (SteamOS: 2) an unprivileged perf only samples user space, so
the script asks for `cycles:u`; kernel time is then missing from the graph.

Bloodborne's perf schedule starts from a save (`-u <folder>` holding `bloodborne/user/savedata`, see
`tools/regress/perf.txt`):

```
PS_PROFILE=1 bash build.sh
tools/profile/perf.sh -n bloodborne -u <saves folder> -w 70 -d 60 <games root>
```

## Comparing two builds

```
tools/profile/compare.sh [-o OUTDIR] [-k ROWS] [perf.sh options] BUILD_A BUILD_B GAMES_ROOT
```

`BUILD_A` and `BUILD_B` are checkouts (or worktrees) after `build.sh`, for instance `develop` and a
task branch. The same `perf.sh` run goes on A, then on B (never both at once: a second game on the
machine moves the result by several percent), into `OUTDIR/a` and `OUTDIR/b`. `compare.txt` lists
the functions whose share of the samples moved most (A, B, B-A in points) and, with `-z`, the Tracy
zones whose mean time moved most (with their counts: a zone that runs more often but faster shows
both). Shares are relative to each run's own samples: a function that got cheaper raises the share
of every other one, so read the largest moves, not the small ones.

For the night session, after building both trees with the same flags:

```
tools/profile/compare.sh -n bloodborne -u <saves folder> -z ../develop . <games root>
```

## RenderDoc: one frame on demand

`PS_RENDERDOC_CAPTURE_FRAME=N` makes the presenter ask RenderDoc to capture the frame after the Nth
game frame it presented (`gpu/diag/psport_renderdoc.cpp`). It uses RenderDoc's in-application API
(the header is vendored in `gpu/third_party/renderdoc`), found with `dlopen("librenderdoc.so",
RTLD_NOLOAD)`: only a RenderDoc already in the process is used, nothing links against it, and
without the variable nothing is looked up. The capture covers one present-to-present interval, so
it holds all the Vulkan work of the GPU threads in between. The log says when it triggered and where
the file went (`RenderDoc: frame captured to <path>`); `PS_RENDERDOC_CAPTURE_PATH=<dir>/<prefix>` sets
RenderDoc's path template. With the variable set and RenderDoc absent, it says so once.

RenderDoc has to hook the process from its start. With RenderDoc from the same nixpkgs as the build
(a host RenderDoc may not load into a Nix-built program):

```
rd=$(nix-build --no-out-link packaging/nixpkgs.nix -A renderdoc)
PS_RENDERDOC_CAPTURE_FRAME=600 PS_RENDERDOC_CAPTURE_PATH=$PWD/out/rdc/frame \
    "$rd/bin/renderdoccmd" capture --opt-hook-children bash run.sh
```

`--opt-hook-children` follows `run.sh` into `psport`. With a window, RenderDoc's own capture keys
(F12, Print Screen) work as well; psport leaves them as they are. At night, in a headless regress
run, `PS_RENDERDOC_CAPTURE_FRAME` and `PS_RENDERDOC_CAPTURE_PATH` go in the title's environment
column, and RenderDoc comes in as a Vulkan layer instead of through `renderdoccmd`:
`VK_ADD_LAYER_PATH=$rd/share/vulkan/implicit_layer.d VK_LOADER_LAYERS_ENABLE=VK_LAYER_RENDERDOC_Capture`
in the same column, with `rd` exported (regress fills `$NAME` in from the environment). The `.rdc`
opens in `qrenderdoc` the next day. Neither way has been tried on a game yet (added by day, when no
game runs); the first night run that uses one confirms it here.
