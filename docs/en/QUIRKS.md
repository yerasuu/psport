# Quirks

A quirk is a fix that concerns one title, one host (GPU and driver) or one middleware, kept out
of the general paths: declared once in a registry, decided once before the game's code runs,
logged, and read where the behaviour it changes is set up. The model is DXVK's per-application
and per-driver tables. Fixing the general behaviour is still the rule ([CLAUDE.md](../../CLAUDE.md));
the registry is where what cannot be general goes, visible and measurable.

## Registry

`src/runtime/runtime_quirks.inc` is an X-macro list shared by C and C++:

```c
QUIRK(id, kind, sz_reason, sz_reference)
QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last)
QUIRK_TRAITS(id, traits)
```

- `QUIRK` declares an entry. `kind` is one of the kinds below. `sz_reason` (why it exists) and
  `sz_reference` (the task, issue or decision that tracks it: `#337`) are mandatory: the build
  fails unless both are non-empty string literals.
- `QUIRK_GPU` matches a `HOST` quirk on the Vulkan device: PCI vendor id and `VkDriverId` (0: any),
  a substring of the device name (`""`: any) and an inclusive `driverVersion` range. Only `HOST`
  quirks may have one (checked at build time); several lines for one id match on any of them.
- `QUIRK_TRAITS` matches on the loader's middleware traits (`LOADER_TRAIT_*` in `src/loader/loader_traits.h`; the first such entry adds that include to `runtime_quirks.c`): every bit of the mask present. `--list-quirks --game` uses the same traits, read from the dump. Never on a `WORKAROUND` (checked at build time).
- A title names its quirks in its `Game::Title` (`quirks`, `gpu/games/game_profile.h`), which
  reaches the C side as `PsTitleInfo` `p_quirks`/`c_quirks`.

The list generates the ids (`RUNTIME_QUIRK_<id>`, `src/runtime/runtime_quirks.h`), the metadata
table, the matchers and `psport --list-quirks`.

## Host entries

| Id | Matches | Read by | Without it |
|---|---|---|---|
| `nvidia_slow_sparse_rebind` | driver `4` (`VK_DRIVER_ID_NVIDIA_PROPRIETARY`) | `PsGuestMemory::Usable` (`LayerMemory`'s default) | the sparse arena when the capability rule allows it ([ARCHITECTURE.md](ARCHITECTURE.md#memory-model)); `PS_LAYER_MEMORY=1/0` by hand |
| `radv_gcn_nohiz` | vendor `0x1002`, device `(RADV TONGA`, `ICELAND`, `CARRIZO`, `FIJI`, `STONEY`, `POLARIS`, `VEGA`, `RAVEN`, `RENOIR` | run.sh (`RADV_DEBUG=nohiz` unless `RADV_DEBUG` is set or `PS_AUTO_NOHIZ=0`) | HiZ on |
| `nvidia_slow_depth_copies` | vendor `0x10de` | `psport-gpu-capabilities --live-resolution`, with no `VK_KHR_fragment_shader_barycentric` | live resolution by size and type only |

run.sh decides before psport runs (a driver reads `RADV_DEBUG` when it loads), so
`psport-gpu-capabilities --host-env` evaluates the same entries for the device psport will take,
with `PS_QUIRKS` applied (`runtime_quirk_applies_to_gpu`, which logs and publishes nothing), and
prints `KEY=VALUE<tab>message` lines; run.sh takes each one the user has not set and prints its
message. psport logs the same entries again when its Vulkan device is chosen.

## Kinds and policy

| Kind | What | On by default | `PS_GAME_PROFILE=none` | `PS_QUIRKS=none` |
|---|---|---|---|---|
| `LAUNCH` | what a title needs to start or keep its data (decision #298) | yes | on | on |
| `OPTIMIZATION` | a speed-up valid for one title or middleware only (decision #265) | yes | off | off |
| `HOST` | a GPU or driver defect worked around: it corrects the host, not the emulation | yes | on | off |
| `WORKAROUND` | a temporary correction for one title, until the general fix lands (decision #331, below) | yes, for the titles that declare it | on | off |

`PS_QUIRKS` is read once at launch, comma-separated, left to right:

| Entry | Effect |
|---|---|
| `none` | every quirk off except `LAUNCH` |
| `-<id>` | that quirk off, `LAUNCH` included |
| `+<id>` | that quirk on even where nothing matched, logged as forced; a `WORKAROUND` only for the titles that declare it (otherwise `ERROR: Quirks: PS_QUIRKS=+<id> ignored`) |

An unknown id or a malformed entry is logged as `ERROR: Quirks: ...` and ignored.

## Workarounds

The rule is still to fix the general behaviour. A `WORKAROUND` lets one title work in the meantime,
visibly and for a limited time (decision #331), on these terms:

- Declared: a `QUIRK(<game>_<what>, WORKAROUND, reason, "#<task>")` entry whose reason says what
  is wrong and what general fix it stands in for, and whose reference names the kb task that seeks
  that fix (`#<number>`; the build fails otherwise, checked in `gpu/games/game_profile.cpp`
  because C cannot read a string literal at compile time).
- Title-scoped: only the title's `Game::Title` `quirks` activate it. A `QUIRK_GPU` or
  `QUIRK_TRAITS` on it fails the build, and `PS_QUIRKS=+<id>` never applies it to another game.
- In the game's folder: the code it gates lives in `gpu/games/<game>/`, never in `src/`,
  `gpu/shim/`, `gpu/port/`, `gpu/memory/`, `gpu/diag/` or `gpu/shadps4/`, and reads
  `PsQuirk::Active(RUNTIME_QUIRK_<id>)` before acting.
- Logged and removable: its launch line says `(WORKAROUND, temporary until #<task>)`, and
  `PS_QUIRKS=-<id>` or `PS_QUIRKS=none` turns it off. It makes the game work, so
  `PS_GAME_PROFILE=none` (which checks the translator's generic paths) leaves it on, as it does
  `LAUNCH`.
- Temporary: when the general fix lands, the entry, its id in the title and the code it gates are
  removed in the same change, and the task is closed.

| Id | Title | Code | Stands in for |
|---|---|---|---|
| `bloodborne_save_sound_flag` | Bloodborne | `gpu/games/bloodborne/title.cpp` `PrepareSaves` (`Title::pfn_prepare_saves`): byte `0x204E` of `SPRJ0005/userdata0010` set to 1 in the user's saves before the game runs | #348: find what the game decides that byte from (an audio output or system state answer that differs from the console's) and fix that library |
| `bloodborne_gx_node_guard` | Bloodborne | `gpu/games/bloodborne/guest_hooks.cpp` `InstallGxNodeGuard` and `OnTrap` (installed with the profile's guest hooks): a null node from the GX swap chain's per-frame heap skips the render command's post at `0x2199310` instead of faulting, the command going out with the next frame; deferred commands counted in the profile's stats report; the first null and every 64th logged with the swap chain's state (`GX node null N:` host frame, FPS, current frame context, its bump allocators and job list, the no-reset flags `+0x3a9`/`+0x131`, frames begun and done) | #504: find why the per-frame GX heap fills under psport and not on a PS4, and fix that |
| `bloodborne_gx_command_heap` | Bloodborne | `gpu/games/bloodborne/workarounds.cpp` `PatchGxCommandHeap` (with the profile's `patch_image`): the GX swap chain's two per-frame command heaps set up with 1 MiB instead of 256 KiB (`mov edx` immediates at `0x2195c0a`, `0x2195c47`) | #504, as above |
| `bloodborne_render_job_slots` | Bloodborne | `gpu/games/bloodborne/workarounds.cpp` `InstallJobSlots`: the render job pool's slot held at `[runner + 0x120]` given back (`0x1056990`) at the job step's exit `0x1068092` when the step failed, and before the runner's destructor (`0x106798a`) and reset (`0x1067a5c`) clear it; each give-back logged (first three, then powers of two) and counted in the stats report | #683: find why the job runner's steps (`0x1067f60`: `0x2165150`, `0x2169c30`) fail under psport and not on a PS4 |
| `bloodborne_blur_sample_clamp` | Bloodborne | `gpu/games/bloodborne/workarounds.cpp` `InstallBlurClamp`: Yebis's `GPUTexUtil_GetRecursiveSampleParameters` (`0xbbc3e0`) given 3 base samples when asked for 2, before its compiled-in assertion raises `0xa0020007`; each clamp logged and counted | #684: find where the 2 comes from under psport |
| `bloodborne_fx_init_clear` | Bloodborne | `gpu/games/bloodborne/workarounds.cpp` `InstallFxInitClear`: the FX init's (`0xebb960`) three clears of 2x2 RG16F history targets to (0, 0, 0, 1) written in memory in the target's format instead of the graphics context's clear `0x2168630` (a fast-cleared target, another format or a colour RG16F does not hold exactly still goes there, with the reason logged); both counted | #685: find what the translator does wrong with those clears (bad PM4 at a save's load) |
| `bloodborne_intel_tonemap` | Bloodborne | `gpu/games/bloodborne/patches.cpp` `AddPatches`: the community's Intel Black Tonemap Fix (the `je` at `0x25843e5`, gated on the byte at `+0x128` of the tone mapping state, as NOPs) on GenuineIntel CPUs, `PS_INTEL_TONEMAP_FIX=0/1` forcing it while the quirk is on | #350: make `rcpps`/`rsqrtps`/`rcpss`/`rsqrtss` (and their VEX forms) answer as the PS4's AMD Jaguar on every host CPU |

## Evaluation and readers

1. Launch (`src/main/main.c`, after the game is loaded and its title found, before anything of
   the title runs): `runtime_quirks_evaluate` validates `PS_QUIRKS`, decides the title's quirks,
   the middleware matches and the forced ones, and publishes the result.
2. The Vulkan physical device is chosen (`Vulkan::Instance`, before `CreateDevice`, so a `HOST`
   quirk can shape device creation): `runtime_quirks_note_gpu(vendor, driver_id, name, version)`
   decides the `QUIRK_GPU` matches and publishes again. A second call is ignored and said.

After that the bitset (`g_runtime_quirks`) never changes. Readers ask at setup, never per draw or
per call: `runtime_quirk_active(RUNTIME_QUIRK_<id>)` in C, `PsQuirk::Active(RUNTIME_QUIRK_<id>)`
in the GPU library (`gpu/port/psport_quirks.h`), a relaxed load and a bit test.

## Log

| Line | When |
|---|---|
| `Quirk: <id> (<kind>) for <what matched>: <reason> [PS_QUIRKS=-<id> disables]` | a quirk activated (the title, `GPU <name> (vendor, driver, version)`, `middleware traits 0x..`) |
| `Quirk: <id> (<kind>) for <what matched>: forced by PS_QUIRKS=+<id>: <reason>` | a quirk forced on |
| `Quirk: <id> (<kind>) for <what matched>: disabled by PS_QUIRKS=-<id>: <reason>` | matched but turned off; also `by PS_QUIRKS=none`, `by PS_GAME_PROFILE=none` |
| `Quirk: <id> (WORKAROUND, temporary until #<task>) for ...` | a `WORKAROUND`'s lines, with the task its removal waits for in place of the kind |
| `Quirks: none active` | at launch, when nothing is on |
| `Quirks: none for GPU ...` | at device creation, when the registry has GPU matchers and none applies |
| `Quirks: <n> active: <id>, <id>` | when the process ends, after the call report's summary |

## psport --list-quirks

`psport --list-quirks [--game <dir>]` prints JSON and exits before any game is loaded: every entry
with its kind, reason, reference and matchers; with `--game`, the title found in the folder's
`param.sfo` and the quirks its title data would apply, each with the state `PS_QUIRKS` and
`PS_GAME_PROFILE` give it. GPU and middleware matches are not evaluated there.

```
{"quirks": [...],
  "game": {"serial": "CUSA03173", "title": "Bloodborne"},
  "applies": [
    {"id": "bloodborne_save_sound_flag", "matched": "Bloodborne (CUSA03173)", "state": "active"}
  ]}
```

## Adding a quirk

1. Fix the general behaviour if it can be fixed; a quirk is for what cannot (yet: a
   `WORKAROUND` follows the terms above).
2. Add a `QUIRK` line with the right kind, the reason and the task that tracks it; for `HOST`,
   one or more `QUIRK_GPU` lines.
3. A title's quirk: add the id to its `Game::Title` `quirks`.
4. Read it with `runtime_quirk_active`/`PsQuirk::Active` where the behaviour is set up (pipeline
   or device creation, a setting's default), never in a per-draw or per-call path.
5. Check `psport --list-quirks --game <dir>`, the `Quirk:` line of a run and the same run with
   `PS_QUIRKS=-<id>`.
