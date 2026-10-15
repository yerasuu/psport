# psport: what it takes from other projects and what it changes

psport is an independent project derived from [bbport](https://github.com/deadinside28/bloodborne_pc)
and [shadPS4](https://github.com/shadps4-emu/shadPS4). Its goal is bbport's own idea taken past one
game: PS4 titles run natively on Linux, their x86-64 code on the CPU as it is, with the system
libraries provided by psport (a compatibility layer, as Wine is, not an emulator). Bloodborne keeps working, with its own game profile, patches and mods; other
titles start without a game check.

This file is the reference of what psport takes from other projects (bbport, shadPS4 and the
rest), what it changes in inherited code, and the variables and reports it adds. How the system
fits together: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md); bringing up a new title:
[docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md); bbport's own notes and change logs:
[docs/en/history/](docs/en/history/) (Russian originals in [docs/ru/](docs/ru/)).

## Following bbport and shadPS4

psport's history starts from bbport's and carries bbport's releases up to 0.51; since then nothing
is merged wholesale. bbport and shadPS4 are reviewed from time to time, and the changes useful to
psport are ported by hand, each recorded in this file as a row of the table it belongs to (with its
source commit), as the rows below already are. Last reviewed points:

- bbport: `3c57da4` (0.51, 2026-10-11), the last bbport release merged into psport's history
  (merge `884d0ea`); `2d7361a` (FSR 4 v07 pass timing and statistics) was ported by hand after it.
- shadPS4: `b4e7ae7` (2026-10-02), the vendored commit in `gpu/VENDOR.txt`, plus the recompiler
  fixes backported after it that the same file lists. The code ported or written from shadPS4
  outside `gpu/shadps4/` (libSceFiber, libSceFont, Videodec2, the per-NID tables, Audio3d, memory
  pools) follows `3a9f572`, as its rows say.

## Names

The port's binary is `out/psport`, its GPU library `out/gpu/libpsport-gpu.so` and the optional DLSS
bridge `libpsport_dlss.so`; every environment variable it reads is `PS_*`, and the compiler cache is
`~/.cache/psport/ccache`. bbport, and psport releases before the rename, call them `bb-probe`,
`libbbgpu.so`, `libbbport_dlss.so` and `BB_*`. For a transition, so that launchers written for those
names keep working, `run.sh`, `build.sh` and `psport` itself copy each `BB_*` variable that is set to
its `PS_*` name when that one is unset, and say once which they copied:
`psport: BB_* variables are deprecated, use PS_*: BB_A, BB_B` (on stdout; `psport` then starts
itself again, so the GPU library's initializers see the copies too).

Inside, psport's own files swap bbport's `bb` for `ps` and keep the rest of the name:
`gpu/port/`, `gpu/memory/` and `gpu/diag/psport_*` (bbport's `gpu/shim/bbport_*`),
`gpu/layer/pslayer_*` (`bblayer_*`), and the GPU library's C interface `gpu/psport_gpu.h` with its
`psport_gpu_*` calls (`bbgpu.h`, `bbgpu_*`). The prefix stays where the directory alone would say
it: those directories share one include path with `gpu/shim/`, which holds the stand-ins for
shadPS4's own headers (`common/`, `core/`), so a bare `settings.h` would be found by search order. Namespaces are `Ps*` (`PsStats`, `PsToggle`,
`PsLayer`), the CMake target `psport-gpu`, the online module `gpu/net/` (`libpsport-net.so`,
`psport_net_*`, `PsNet*`) and the DLSS bridge's interface `psport_dlss_bridge.h` (`PsDlss*`, entry
point `PsDlssGetApi`; a bridge built before exports `BbDlssGetApi` and is reported unavailable until
`build.sh` builds it again). The comments that mark psport's lines in inherited code say
`psport:` (bbport's `bbport:`), as do the Vulkan debug names of its buffers.

The settings file is `psport.ini` and the GPU check `out/psport-gpu-capabilities` (bbport's
`bbport.ini` and `out/bb-gpu-capabilities`). When `PS_CONFIG` is unset and the data directory has a
`bbport.ini` but no `psport.ini`, `run.sh` copies it once (`psport: settings copied from bbport.ini
to psport.ini`), and so does the launcher; `bbport.ini` itself is left for the older release.

The inherited GTK launcher and the package are psport's too: `launcher/psport-launcher.sh` and
`launcher/psport_*.py` (app id `io.github.psport.Launcher`, settings in
`~/.config/psport-launcher/`, read from `~/.config/bbport-launcher/` until the first save), the
Nix package `psport` with `bin/psport` and `bin/psport-game`, and the AppImage's data directory
`~/.local/share/psport`. The package's wrappers move an existing `~/.local/share/bbport` there once,
when `PS_DATA_DIR` is unset, and leave a link at the old path for an older AppImage.

## Ported components

Code taken from other projects to move forward, each in its own directory. The first commit of each
is the files as they were at the source; later commits adapt them.

| Component | Directory | Source | License | Changes here |
|---|---|---|---|---|
| libSceNgs2 (software audio mixer) | `gpu/ngs2/` | [AnyPS5](https://github.com/boykopovar/AnyPS5) `5edae353`, `core/libs/prx/libSceNgs2.native` | GPL-2.0 | PS4 structure layouts (AnyPS5's are PS5's), exceptions turned into error codes at the game boundary, unimplemented effects pass audio through, decoding over the ported LibAtrac9 |
| libSceFiber (user-level fibers) | `gpu/fiber/` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/fiber` | GPL-2.0-or-later | TCB read through GS, failures logged, `PS_FIBER_TRACE` |
| libSceFont, libSceFontFt (text over FreeType) | `gpu/font/` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/font` | GPL-2.0-or-later | guest paths through the runtime's mounts, system fonts from `PS_SYS_FONTS` else the host's Noto (or the embedded DejaVu Sans), exceptions turned into error codes, `sceFontGlyphRenderImage` and `sceFontCloseFont` implemented |
| libSceAudiodec (AAC, MP3 and ATRAC9 decoders) | `gpu/audiodec/` | [AnyPS5](https://github.com/boykopovar/AnyPS5) `a11c8f7f`, `core/libs/prx/libSceAudiodec` | GPL-2.0 | registered by NID, exceptions turned into error codes at the game boundary, ATRAC9 over the ported LibAtrac9, FFmpeg's log level left to the process, parameters of the first decoders logged |
| NID to name table | `src/runtime/imports/import_names_all.inc` | shadPS4's aerolib list (via universal_ps4_native's `data/aerolib.csv`) | GPL-2.0-or-later | generated, sorted for binary search |
| Output arguments per NID | `src/runtime/imports/import_outparams.inc` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, prototypes and `LIB_FUNCTION` registrations in `src/core/libraries` | GPL-2.0-or-later | generated by `tools/import_outparams.py`, sorted for binary search |
| Failure convention per NID | `src/runtime/imports/import_returns.inc` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, prototypes and `LIB_FUNCTION` registrations in `src/core/libraries` | GPL-2.0-or-later | generated by `tools/import_returns.py`, sorted for binary search |
| ulobjmgr contracts | `src/kernel/runtime_ulobjmgr.c` | written from shadPS4's `core/libraries/ulobjmgr` | GPL-2.0-or-later | ids handed out per object |
| libSceAudio3d (ports, bed and object mix) | `src/audio/runtime_audio3d.c` | written from [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/audio3d/audio3d.cpp` | GPL-2.0-or-later | output through the runtime's AudioOut ports, mixed in float; attribute ids taken from FMOD, Koei Tecmo and CRI rather than shadPS4's enum; objects panned by position and spread over 7.1 (stereo fold-down without a 5.1+ device), passthrough to one speaker, object gain unity until set; sceAudio3dPortSetAttribute |
| libkernel memory pools (`sceKernelMemoryPool*`) | `src/kernel/runtime_mempool.c` | written from [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/kernel/memory.cpp` | GPL-2.0-or-later | over the runtime's direct memory and mapping table; commits take the pool's free 64 KiB blocks wherever they are, decommitted memory reads as zeros again; `sceKernelMemoryPoolBatch` and `sceKernelMemoryPoolGetBlockStats` not implemented |
| FSR-Vulkan (FSR 3.1 and FSR 4 v07 upscaling over Vulkan) | `gpu/third_party/fsr-vulkan/` | [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) `c64f093` | MIT | `PS_FSR4_PROFILE` (GPU time per FSR 4 pass) and `PS_FSR4_STATS` (driver statistics of each pass) in the FSR 4 v07 provider |
| LibAtrac9 (ATRAC9 decoder) | `third_party/LibAtrac9/` | [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) `efca2e3`, `C/src` | MIT | `Atrac9Decode` takes the input's size and the bit reader reads zeros past it instead of past the buffer |

## Changes to inherited shadPS4 code

bbport carries part of shadPS4 (`gpu/shadps4/`: video core, shader recompiler, AvPlayer and other
libraries) from its own first commit. Where psport changes that code, the change is listed here, so
a review of shadPS4 knows what differs. Lines changed carry a `psport:` comment where the reason isn't
obvious.

| Area | Files | Change | Also in upstream shadPS4 |
|---|---|---|---|
| Geometry shader stream-out | `video_core/renderer_vulkan/vk_rasterizer.cpp`, `vk_pipeline_cache.cpp`, `vk_instance.cpp`, `shader_recompiler/` (copy shader, SPIR-V emit), `amdgpu/liverpool.cpp`, regs | captured through `VK_EXT_transform_feedback` instead of skipped; `STRMOUT_BUFFER_UPDATE` through transform feedback counters | upstream skips these draws |
| Point-list GS output | `shader_recompiler/backend/spirv/emit_spirv.cpp` | `OutputPoints` instead of a bare `OutputVertices` (an invalid module); `ShaderBinaryVersion` 10 | bug present at `3a9f572` |
| AvPlayer looping | `core/libraries/avplayer/avplayer_source.cpp` | loops by seeking the streams only, up to `INT64_MAX` so streams without a known duration still seek (rewinding the byte stream first left the mp4 demuxer at end of file forever); a loop that reads nothing or whose seek fails ends the source instead of spinning | bug present at `3a9f572` |
| AvPlayer locking | `core/libraries/avplayer/avplayer_state.cpp`, `avplayer_state.h` | `Start()` stops a playing or finished source through `StopLocked()` under its own lock instead of `Stop()` re-locking the source mutex (`std::system_error` "Resource deadlock avoided", Tales of Symphonia); state events reach the game callback only after the source mutex is released, so the callback may call back into `sceAvPlayer*`; restarting no longer sends a `StateStop` before `StatePlay` | bug present at `3a9f572` |
| Buffer write traps | `video_core/page_manager.cpp` | with the game's memory in place, the pages the buffer cache watches (its VRAM copies) are trapped for writes (`WriteTraps::Buffer`) as the pages under images are: writes no code announced (a game's skinning and UI meshes over blocks a loader filled) reached no copy, and the GPU drew the older data | no (bbport's in-place memory) |
| AvPlayer jumps | `core/libraries/avplayer/avplayer.cpp`, `avplayer_impl.*`, `avplayer_state.*`, `avplayer_source.*` | `sceAvPlayerJumpToTime` restarts the source at the requested time (paused if it was) and reports `WAR_JUMP_COMPLETE`; Unity's video player draws nothing until that warning arrives, so Symphonia's intro movies showed the previous movie's last frame | stubbed at `3a9f572` |
| AvPlayer stopping | `core/libraries/avplayer/avplayer_source.cpp` | `Stop()` no longer rewinds the file streamer's position to 0: FFmpeg's byte stream kept its own position and buffer, so after a restart (`sceAvPlayerJumpToTime`, Unity's movies) its next refill read the file from the start as packet data (`Invalid NAL unit size`, `partial file`) and Symphonia hung at the end of its intro | bug present at `3a9f572` |
| AvPlayer end of file | `core/libraries/avplayer/avplayer_source.cpp`, `avplayer_state.*` | the demuxer reports end of file only when the source ran out, not when `Stop()` ends it: every restart (`sceAvPlayerStart` while playing, `sceAvPlayerJumpToTime`, `sceAvPlayerStop`) put the state machine in `EndOfFile`, so after a jump `Pause()` did nothing and a buffering check racing a stop logged `EndOfFile -> Buffering` (Tales of Symphonia); the controller thread moves between `Play` and `Buffering` only from the state it checked, and `Stop()` holds the source mutex exclusively, so a stop no longer reads as a buffer underrun | bug present at `3a9f572` |
| VideoOut cursor | `core/libraries/videoout/video_out.cpp`, `driver.h` | `sceVideoOutCursorEnable`/`Disable`/`SetPosition`/`SetHotSpot`/`Set2xMagnify` keep each port's cursor state (image, position, hot spot, magnification) and hand it to the overlay, which draws each enabled cursor's 64x64 image over the game frame, behind the menu (`gpu/port/psport_overlay.cpp`); the first enable is logged with how many of its pixels are visible | absent at `3a9f572` |
| VideoOut configure options | `core/libraries/videoout/video_out.cpp` | `sceVideoOutConfigureOptionsInitialize_` zero-fills the options block of the size the caller passes (no option set), as `sceVideoOutModeSetAny_` fills a mode | absent at `3a9f572` |
| VideoOut event lists | `core/libraries/videoout/driver.cpp`, `video_out.cpp` | a port's `flip_events` are added, deleted, triggered by `Flip()` and cleared by `Close()` under its `port_mutex`, and its `vblank_events` under `vo_mutex` (the lock the vblank thread already held to trigger them); `Close()` and the add and delete calls changed the vectors while the present thread walked them | bug present at `3a9f572` |
| Custom border colour without a table | `video_core/texture_cache/sampler.cpp` | a sampler with a custom border colour while `TA_BC_BASE` is 0 samples transparent black (the hardware's fetch reads an unmapped page as zeros) instead of reading the table at guest address 0 (a segfault); logged with a count | bug present at `3a9f572` |
| Synchronous image downloads | `video_core/texture_cache/texture_cache.cpp` | the buffer invalidation after a synchronous image download reads back GPU data in place (`assume_locks`) instead of queuing it to the GPU command thread: downloads run on that thread or the draw recording thread, and the queued readback never ran while the command thread drained the draw pipe (Onigiri hung after its first frame once it had a mouse) | no (bbport's draw recording thread) |
| Command buffer pool growth | `video_core/renderer_vulkan/vk_resource_pool.cpp` | `CommitResource` searches again with the GPU tick it has just refreshed: the search lambda had captured the tick before the refresh, so every pass through the pool overflowed and added four command buffers, and the driver's buffers behind them (~384 KiB of VRAM each time, ~2.5 MiB a minute in play, never freed) | bug present at `3a9f572` |
| Presenter | `video_core/renderer_vulkan/vk_presenter.cpp` | logs the first presented game frame once (`tools/regress.sh` reads it) | no |
| Quirk registry | `video_core/renderer_vulkan/vk_instance.cpp` | the chosen physical device's vendor, driver id, name and driver version go to `runtime_quirks_note_gpu` before `CreateDevice` (`HOST` quirks, [docs/en/QUIRKS.md](docs/en/QUIRKS.md)) | no |
| Prepared vertex streams | `video_core/renderer_vulkan/vk_rasterizer.cpp`, `vk_rasterizer.h` | a draw-preparation worker's vertex inputs are used only while its V#s equal the ones read in order for the draw (`ArePreparedVsharpsCurrent`): the worker reads the V# tables ahead of the constant engine dumps that rewrite them, and the user data it verifies holds only the tables' address (Onigiri's login screen and PSN notice drew with another draw's vertices) | no (bbport's draw preparation) |
| AvPlayer queues | `core/libraries/avplayer/avplayer_common.h` | `AvPlayerQueue::Size()` and `Front()` take the queue's mutex, and `Pop()` checks for an empty queue under it: the decoder threads push while the game thread reads, and an unlocked `size()` or `front()` raced the push | bug present at `3a9f572` |
| AvPlayer start without a source | `core/libraries/avplayer/avplayer_state.cpp` | `Start()` fails when the source is gone instead of calling through a null `m_up_source`: `sceAvPlayerClose` resets the source before it stops the controller thread, and an AutoPlay `StateReady` handled on that thread in between still called `Start()` in the `Ready` state | bug present at `3a9f572` |
| GCN instructions that asserted | `shader_recompiler/frontend/translate/scalar_flow.cpp`, `translate.cpp`, `translate.h`, `vector_memory.cpp`, `frontend/format.cpp`, `ir/passes/inverse_ballot_elimination_pass.cpp` | `S_SLEEP` is a no-op; `S_GETREG_B32` of `HW_ID` reads 0 (other registers still assert, by name); `BUFFER_WBINVL1`/`_SC` decode and become a device-scope memory barrier; a 32-bit write to `EXEC_LO`/`EXEC_HI` replaces that half of EXEC; an `InverseBallot` of an immediate other than 0 or all ones stays for the backend instead of asserting; unhandled flow-control opcodes are named in the assertion | not checked; all five asserted in the inherited tree |

## Loading the game

`psport --game <folder>` loads `eboot.bin` and every module it links straight from the game's
folder (`src/loader/loader_self.c` reads SELF and ELF files, `src/loader/loader_link.c` lays them out, relocates
and binds them), with `--system-modules`, `--hle` and `--lle` as `PS_SYS_MODULES`, `PS_HLE` and
`PS_LLE` set them. Only the permission bits of a segment's flags become its protection: some system
modules set high bits there. The rest of the launch is in `psport` too, and no Python runs for it
(only `scripts/online.py`, with `PS_ONLINE=1`): the loose-file mods' view (`src/loader/loader_mods.c`), the
AppContent profile from `param.sfo`, shadPS4/GoldHEN XML patches (`src/loader/loader_patches.c`), and
through the game's title its own file check and patches (`Game::Title` `check_files` and
`add_patches`; Bloodborne's check of the 1.09 executable and intact shaders and its frame rate,
resolution and effect presets live in `gpu/games/bloodborne/`).

## System modules

`PS_SYS_MODULES=<folder>` links the user's own dump of the console's system modules (`*.sprx`) that a
title needs. They are never part of this repository or of anything built from it, and each is a
stopgap until the port implements its library: `PS_HLE=libSceX` keeps one to the port,
`PS_LLE=libSceFiber` links one the port already implements, to compare the two. The modules a title
bundles in `sce_module` (libSceNpToolkit2, libSceJobManager, ...) are linked the same way, ahead of the
user's dump, once anything linked imports their library, as the console loads them with
`sceSysmoduleLoadModule`. A system module that is a client of a system service is never linked, even
when the folder has it and `PS_LLE` names it: one whose imports include the IPMI client (libSceIpmi's
`IPMI::Client`, the kernel's `ipmimgr_call`, libSceNpCommon's `ServiceIpmiClient`, libSceNpManager's
`GetIpmiClient`, libSceAudioOut's `sceAudioOutInitIpmiGetSession`: any import whose name has `ipmi`)
fails without the service process behind it, so the port serves its library; the log names each one
left out and the import that decided it (`Loader: system module ... not linked`).

`PS_SYS_FONTS=<folder>` likewise reads the user's own dump of the console's system fonts (SST and the
rest). Without it libSceFont draws each font set with the host's Noto fonts (`PS_FALLBACK_FONTS`
adds folders to look in) and logs the file it chose once per set.

## Modules the game loads

Modules a title loads itself with `sceKernelLoadStartModule` (every `*.prx` outside `sce_module` and
`sce_sys`, such as Unity's `Media/Modules` and `Media/Plugins`) are linked into the image at start like
the others, relocated and bound, but left unstarted: loading the path runs their initializer once and
returns their handle, `sceKernelDlsym` finds their exports (and the eboot's, handle 0) by name, and
imports of other modules bound to them, which the PS4 linker would rebind at that point, already
point at them.

## In-game overlay

The overlay (Insert or L3+R3, `gpu/port/psport_overlay.cpp`) is drawn in English, Russian or Spanish:

| Source, first that is set | Values |
|---|---|
| `menu_language` in `psport.ini` (written whenever the overlay saves a setting) | `en`, `ru`, `es` |
| `PS_MENU_LANGUAGE` (decanter passes its own UI language) | a language code: `ru...` Russian, `es...` Spanish, anything else English |
| `LC_ALL`, `LC_MESSAGES`, `LANG`, `LANGUAGE` | the same |

As on the console, the gamepad's Guide (PS) button, or Home on the keyboard, asks whether to close
the game: cross or Enter closes it the way the overlay's Quit game does (the window's close, with the
call report summary), circle, Esc or Guide again goes back to the game. While the prompt is shown the
game keeps running with its input held neutral. `PS_PAD_FILE`'s `ps` token presses Guide.

`PS_LANGUAGE` is not this: it is the system language the game reads (`sceSystemServiceParamGetInt`: 1
English, 3 Spanish, 8 Russian).

## Mouse

libSceMouse (`src/input/runtime_mouse.c`) gives every game that opens a mouse port the host mouse, with no
switch: port index 0 (or a merged port) reports its buttons and its motion, wheel and tilt since the
handle's previous read, collected by the game window (`gpu/shim/window.cpp`); index 1 never has a
mouse. Without a window or a host mouse the port opens and reports no device (headless gamescope has
a pointer, so regression runs see a mouse that never moves or clicks). While
the menu or the close prompt is open the game sees no motion and no buttons. While the game shows a
VideoOut cursor, the window hides the system cursor over it and a click does not take the mouse for
mouse look. The log says once whether the host mouse reaches the game (`Runtime: mouse: ...`).
`game_mouse_sensitivity` in `psport.ini` (0.05 to 20, default 1: the host's pixels as they are)
scales the motion the game receives, keeping fractions for the next read; a value other than 1 is
logged. Mouse look keeps its own `mouse_sensitivity`.

## Keyboard

libSceImeKeyboard (`src/input/runtime_keyboard.c`) gives every game that opens the keyboard the host
keyboard as resource 1, with no switch. `sceImeUpdate` hands the game's handler the open event, a
connection or disconnection when the host keyboard comes or goes, and a key down, up or (when the
open option asks for it) repeat per key, with the USB keycode, the character it types with the held
modifiers and the modifier and lock state; dead keys and input methods do not compose. While the
game has the keyboard open its keys are the game's: the keyboard-as-pad (`src/input/runtime_pad.c`) stops
reading them, a gamepad keeps working, and games that never open the keyboard keep it as a pad. The
port keeps Insert, Home, the menu's and the name dialog's keys and F1 while it holds the mouse.
Without a window or a host keyboard the keyboard opens and lists none, so `GetResourceId` fails with
`0x80BC0004` as on a console with no keyboard. The log says whether the host keyboard reaches the
game (`Runtime: keyboard: ...`) and sums up the events delivered at exit.

libSceIme's on-screen keyboard (`src/system/runtime_ime.c`) is the same text entry as the IME dialog: while
the game has it open, the game window takes the host keyboard's typing and the overlay draws the
panel with the text. `sceImeUpdate` hands the game the open event (the panel's place and size), each
change of the text (also written to the game's buffer) and Enter or Esc as the panel's enter or close
button; the caret stays at the end of the text and no input method composes. Without a window the
panel is entered at once with `PS_IME_TEXT` or the text the game opened it with.

## Trophies

libSceNpTrophy (`src/system/runtime_trophy.c`) serves the game's trophy pack and keeps each user's unlocks
across runs. Registering a context reads `/app0/sce_sys/trophy/trophy<label>.trp` (the context's
service label, two digits): a TRP container (`src/system/runtime_trophy_pack.c`; big-endian header 0x60
bytes: magic `0xDCA24D00`, version, file size, entry count, entry size 0x40; each entry a 32-byte
name, offset, size and flags) holding `ICON0.PNG` (and `ICON0_<language>.PNG`), `GR<group>.PNG`,
`TROP<id>.PNG` and the configuration, `TROPCONF.SFM` with the names in `TROP.SFM` and
`TROP_<language>.SFM` (`PS_LANGUAGE`, the system language, two digits), XML that
`src/system/runtime_trophy_sfm.c` reads. In retail packs, every dump checked so far, the SFM files are
`.ESFM`: encrypted with the console's trophy key, which the port does not have. Then the trophies are
the pack's `TROP<id>.PNG` icons and the groups its `GR<group>.PNG` icons, with no names, grades,
hidden flags, group membership or platinum (every trophy is in the base game's group), the
registration says so in the log and the call report counts it as a stub. Without a pack the set is
empty, as on a console without the title's trophy data, and unlocks of any id below 128 are kept.

The registration logs one line with what it read
(`Runtime: NpTrophy: context <n> (user <n>, service label <n>) registered: NP communication id ...,
<n> trophies, <n> groups, <what the configuration gave>; <n> unlocked (<state folder>)`), each unlock
one (`Runtime: trophy <id> unlocked at <UTC time> (<grade>[, <name>])`, and the platinum the unlock
completes in a second one) and each unlock of a trophy already unlocked one
(`Runtime: trophy <id> already unlocked at <UTC time>`; the call fails with `0x8055160C`).

The state lives in the user folder (`--user`, `PS_USER_DIR`), under the NP communication id that
`sce_sys/npbind.dat` binds to the service label (else the configuration's `npcommid`, else the
title id):

| File | Contents |
|---|---|
| `trophy/<NP comm id>/unlocks_<user id>.tsv` | a `#` comment line, then one line per trophy: id, `1` unlocked or `0`, unlock time in UTC (ISO 8601 with microseconds, `2026-10-15T09:41:07.123456Z`; empty while locked), tab-separated; rewritten whole (temporary file and rename) at every unlock |
| `trophy/<NP comm id>/trophyset.json` | the pack as the port read it, rewritten at every registration: `format` (1), `np_comm_id`, `pack` (guest path), `configuration` (`plain`, `encrypted`, `invalid` or `missing`), `language`, `names` (the SFM the names came from), `title`, `detail`, `groups` (`id`, `icon`, `name`, `detail`) and `trophies` (`id`, `icon` (the entry in the pack), `grade`: `platinum`, `gold`, `silver`, `bronze` or `unknown`, `hidden`, `group` (-1 the base game), `platinum` (the platinum it counts for, -1 none), `name`, `detail`) |

The user id is the one `sceUserServiceGetInitialUser` reports (1). A trophy whose configuration names
a platinum (`pid`) unlocks that platinum with it once every trophy counting for it is unlocked, and
`sceNpTrophyUnlockTrophy` returns its id. Icon calls follow the size query: without a buffer they
give the icon's size; with one, they copy the icon when the size passed holds it
(`0x80551605` otherwise); a hidden trophy's icon waits for its unlock (`0x8055161A`).
`sceNpTrophyShowTrophyList` only logs, as there is no system UI.

## Defaults

Every default is chosen for any game: what the console does, or what changes only how fast the host
does the same work. A value that only pays off in one game is that game's profile's
(`gpu/games/<game>/`, `Game::Profile`), and the variable, when set, overrides both. A game without a
profile (or with `PS_GAME_PROFILE=none`) runs with the values in the first column.

| Setting | Default | Why it holds for any game | Bloodborne's profile |
|---|---|---|---|
| Frame overlap, `PS_SUBMIT_AHEAD` | 0 (`Game::Profile::c_submits_ahead`) | the submission lock's contract: after `sceGnmSubmitDone` a submission waits for the previous frame, and a game may reuse what that frame used as soon as it returns (Gal Guardians 2 rebuilt its command buffers over the frame in flight) | 2: it builds each frame in the context of the frame before the previous; its heap leak check (`frame_overlap_unsafe`) and `PS_SUBMIT_GUARD` still turn it off |
| Frames the GPU command thread runs ahead, `PS_FRAMES_AHEAD` | 1 | pacing only: flips come at the GPU's rate instead of in bursts when the GPU is the bottleneck | |
| Forced anisotropic filtering, `PS_ANISO` | 0: the game's samplers (`Game::Profile::anisotropy`) | the image the game asks for: forced filtering also reaches textures that are not surfaces (Astrea's backgrounds came out brighter and browner with it) | 16: its scene textures have little or none, and behind the upscaler surfaces at a grazing angle stayed blurred |
| Temporal upscaler (`upscaler` in `psport.ini`, `PS_UPSCALER`) | FSR 3.1 at Native AA, where a profile places it (`Game::Profile::upscaler`); elsewhere off, with no scene jitter and no camera or object motion vectors (`Upscaler: off: no game profile places it`) | the upscaler has to know which pass combines the scene and where the UI starts; jittering a frame nothing then resolves would shake its image | its post-processing combine (compute shader `9a9cf8a9`) and its UI's first vertex shader (`34e8a281`), its UI's other shaders, the layout of its scene constants (the camera) and the sizes of its bone palettes (object motion); `PS_UPSCALE_BEFORE_CS` and `PS_UI_TRIGGER_VS` place it by hand in another game, without camera or object motion vectors |
| Scene scaling sizes (`Game::UpscalerPlacement` `scene_width`, `scene_height`, `post_divisor`) | 1920x1080 scene targets and post-processing targets at half that size: the ones scene scaling replaces with proxies | the console's 1080p frame; scene scaling also runs without a profile when the output is not 1080p | the same, set in its profile: its scene at 1920x1080 and its post-processing chain at 960x540 |
| Object motion (`object_motion`, `PS_OBJECT_MOTION`) | on, while the upscaler is available | it only feeds the upscaler | |
| Frame rate (`PS_FPS`) and vblank (`PS_VBLANK_HZ`) | a title without presets: unpatched, 60 Hz vblank (the console's) | the frame rate presets are patches to one executable, its title's (`Game::Title::add_patches`); at 480 Hz a title pacing itself by its flip rate ran 8 times too fast | `uncap` (delta-time patch, 480 Hz vblank, frames shown at once), 60 or 90 (`gpu/games/bloodborne/patches.cpp`, which sets `PS_VBLANK_HZ` for the preset) |
| Scaled output (`output_res`) and `PS_DMEM_MB` 9152 | only for a title with presets (run.sh asks `psport --has-presets`); otherwise its own size and the console's 5056 MiB of direct memory | they are a patch to the same executable, and the larger direct memory exists for its larger targets | as `output_res` and `live_resolution` choose |
| Memory model, `PS_PC_MODEL` | run.sh: the new one on AMD GPUs, the 0.3 one on the others (`HOST` quirk `amd_pc_memory_model`, through `psport-gpu-capabilities --host-env`) | a property of the driver, not of the game: sparse binding of the game's memory works on RADV and stalls NVIDIA's GPU for seconds; the 0.3 model is being retired | |
| `PS_COPY_GPU_BUFFERS` | 1 (run.sh) | the GPU thread decodes a submission after the game's call returns, later than the console's command processor reads it; the copy keeps a game that refills that memory from changing what the GPU gets | |
| `PS_READBACKS` | 1 (relaxed) | the console's memory is shared: the CPU reads what the GPU wrote (vertices, compute results), and relaxed readbacks are the cheapest mode that gives it that | |
| `PS_GPU_WRITE_TWINS`, `PS_GPU_WRITE_TWINS_MAX` | 1, 65536 bytes (run.sh) | a CPU write next to small GPU outputs keeps the GPU's bytes and the CPU's apart instead of waiting for the GPU; the result is the same | |
| `PS_PREUPLOAD` | 1 (run.sh; the 0.3 model only) | uploads only memory already in VRAM that the game rewrote: no new VRAM, it only moves an upload earlier | |
| `PS_UFFD` | 0 | userfaultfd write tracking crashed the Steam Deck; mprotect is the safe path | |
| Draw pipeline `PS_DRAW_PIPE`, recording threads `PS_VK_RECORD_THREADS`, `PS_ASYNC_SUBMIT`, copy threads `PS_COPY_THREADS` | by the host's hardware threads: the draw pipeline from 8, 1/2/3 recording threads from 1/8/12, submissions from the recording threads, a quarter of the threads (1 to 4) for copies | they decide which host thread does the work, not what the GPU or the game sees | |
| Optimizations behind `PS_TOGGLE_FILE` (`gpu/port/psport_toggles.h`) | on (a set bit switches one off) | caches, memos and pipelining that keep every result the same; the TAA, FSR 4, camera and scene scaling bits act only where the upscaler runs; the optional TAA techniques are off | |
| `RADV_DEBUG=nohiz` | on GCN 3-5 under RADV (run.sh, `HOST` quirk `radv_gcn_nohiz`; `PS_AUTO_NOHIZ=0`: off) | a driver bug: depth compression drew black halos and lost geometry on those chips | |

## Diagnostics

| Variable | What it shows |
|---|---|
| `PS_PERMISSIVE=1` | imports the port lacks return 0 instead of stopping, each logged once |
| `PS_QUIRKS=none,-<id>,+<id>` | quirks of the registry turned off (all but `LAUNCH`, one) or forced on, each logged; `psport --list-quirks [--game <dir>]` lists them ([docs/en/QUIRKS.md](docs/en/QUIRKS.md)) |
| `PS_GAME_PROFILE=none` | the game runs without its translator profile (`gpu/games/`, `Game::Profile`), on the generic paths only; its title (`Game::Title`: launch, patches, saves, menu pages) still applies |
| `PS_PERMISSIVE_OUT=error` | with `PS_PERMISSIVE=1`, those of them that write through a pointer argument (`src/runtime/imports/import_outparams.inc`) return `0x80020016` instead of 0, so the game takes its error path rather than reading an output nothing wrote; either way their log line names the output arguments left unwritten and the pointers passed |
| (always) | before the game starts: every import without implementation or answered by a stub, and a summary |
| (always) | calls that nothing answers for real or that fail, see [Call report](#call-report) |
| `PS_CALL_REPORT=<file>` | the call report's summary lines, also written to that file when the process ends |
| `PS_SUBMIT_GUARD=0` | frames overlap (`PS_SUBMIT_AHEAD`) unguarded: a submitted frame's command buffers are neither made read-only nor checksummed until the GPU retires the frame (`gpu/memory/psport_submit_guard.h`), so a game that rewrites them in flight is not caught and the overlap stays on; `=trap` or `=hash` keeps one of the two watches |
| `PS_AUDIO_STATS=1` | per audio port, every 5 s: cadence, peak and RMS in dBFS (sound or silence) |
| `PS_NGS2_TRACE=1` | first calls of each Ngs2 function and the level of each render |
| `PS_FIBER_TRACE=1` | first calls of each Fiber function with their arguments |
| (always) | the first guest path without a mount under each root (`/archive`, `/etc`, ...), and after the call report how many there were per root |
| `PS_TRACE_SEMA=1` | each semaphore created and each blocking wait on one |
| `PS_TRACE_EVENTFLAG=1` | each event flag created; without it the first 8 are listed, then a count line at each power of two, and the totals after the call report |
| `PS_TRACE_MEM=1` | each direct memory allocation, direct mapping, flexible memory map and virtual range reservation; a failed direct or flexible mapping is logged without it, and flexible maps as event flags are (the first 8, counts at powers of two, totals after the call report) |

### Call report

Every import the game calls without getting real work done is logged the first time and counted:

| Line | When |
|---|---|
| `Runtime: unsupported import <NID> (<name>) ...` | first call of an import without implementation (`PS_PERMISSIVE=1`), with its first argument or the output arguments it leaves unwritten |
| `Calls: stub <NID> (<name>) returns 0 without doing anything; arguments <a0>, ..., <a4>` | first call of an import answered by the runtime's stub |
| `GPU [<class>] <<level>> <file>:<line> <function>: (STUBBED) ...` | first call of a GPU library (HLE) function that does not do its work, whatever `PS_GPU_LOG` says |
| `Calls: failed <NID> (<name>) -> 0x8xxxxxxx; arguments <a0>, ..., <a5>` | first time an import fails with that SCE error code |
| `Calls: failed <NID> (<name>) -> -1, errno <n>; arguments <a0>, ..., <a5>` | first time a POSIX import fails with that errno |
| `Calls: status <NID> (<name>) -> 0x8xxxxxxx; arguments <a0>, ..., <a5>` | first time an import reports a state its caller polls or waits for with that code (no event pending, timed out, busy) |
| `Calls: status <NID> (<name>) -> -1, errno <n>; arguments <a0>, ..., <a5>` | the same for a POSIX import (`sem_trywait`'s `EAGAIN`, `sem_timedwait`'s `ETIMEDOUT`) |

Failures are watched for the imports bound to the runtime's or the GPU library's own functions whose
result reports one, per shadPS4's prototypes (`src/runtime/imports/import_returns.inc`, `tools/import_returns.py`:
sce functions returning a 32- or 64-bit result, POSIX functions returning -1 with errno) and whose
arguments all travel in registers; a thunk calls the function and only a result that looks like a
failure takes the slow path. The results `src/runtime/imports/import_statuses.inc` lists for an import (kept by hand
from the codes the runtime's implementations return: `sceUserServiceGetEvent`'s no event, the
timeouts of the `Wait`/`Timedwait`/`Timedlock` calls, the busy result of the `Poll`/`Try` calls,
`scePthreadMutexLock`'s `EDEADLK` on an error-checking or adaptive mutex the thread already holds,
which Unreal Engine's critical sections take for a recursive lock) are no failure but the state the caller asked about: they are logged and counted all the same, as
`status`. When the process ends (the guest exits, the window is closed, SIGTERM,
SIGINT or SIGHUP, a fault, the watchdog, a `STOP:`), a summary follows, most calls first:

```
Calls: summary: <n> unimplemented, <n> stub, <n> failed, <n> status (import and error code)
Calls: summary unimplemented <calls> <NID> (<name>)
Calls: summary stub <calls> <NID> (<name>)
Calls: summary stub <calls> <function> (HLE)
Calls: summary failed <calls> <NID> (<name>) -> 0x8xxxxxxx
Calls: summary failed <calls> <NID> (<name>) -> -1, errno <n>
Calls: summary status <calls> <NID> (<name>) -> 0x8xxxxxxx
Calls: summary status <calls> <NID> (<name>) -> -1, errno <n>
```

Then `Quirks: <n> active: <id>, ...` (the quirk registry, [docs/en/QUIRKS.md](docs/en/QUIRKS.md)).

The `status` count is appended to the first line and its entries carry a list name of their own,
so a parser that knows only `unimplemented`, `stub` and `failed` still reads those as before, skips
the `status` lines and no longer sees these results among the failures.

## Regression runs

`tools/regress.sh` runs every title in `tools/regress/titles.txt` headless and says how far each
got, so a change can be checked against all of them at once:

```
[SYS_MODULES=<folder>] tools/regress.sh [-j 2] [-o out/regress/<time>] [-c <earlier results.json>] \
    [-n ryza,bloodborne] [-t <titles file>] [-s <toolchain nix file>] [-u <earlier output folder>] \
    <games root>
```

Each title runs inside a headless gamescope with `PS_AUDIO=none` and `PS_PAD_HOST=0`: no window, no
sound and none of the user's controllers. Its pad schedule (`tools/regress/*.sched`: `<second> shot`,
`<second> press <a+b> [hold]`, `<second> hold <a+b> [seconds]` which keeps them held until the next
`hold`, `press` or `release`, `<second> release`, `<second> end`; lines with the same second run one
after the other) drives `PS_PAD_FILE` and takes frame dumps; a last one
is taken before it stops. At most `-j` titles (default 2) run at once. The titles file names the
folder under the games root and the title's own environment; `$NAME` there comes from the caller's
environment, and an entry whose variable is empty is left out. Every title runs without a dump; with
`SYS_MODULES` set, the titles that name it get it as `PS_SYS_MODULES` (the folder of system modules
above), for instance to compare a library with `PS_LLE`.

Milestones, from the log and the dumps:

| Milestone | Evidence |
|---|---|
| loaded | `Mapped N bytes, N segments` |
| entered | `Entering original x86-64 code` |
| initialized | the first `Runtime: guest thread '...' created` |
| gpu | `Presenter: first game frame presented` |
| frames | a present dump with at least 0.5 % of its pixels lit (a channel above 24 of 255) |
| audio | a `PS_AUDIO_STATS` peak above -60 dBFS |

It also records the first fault or stop logged before the title is told to end (`Guest fault`
offset, `Host fault` library+offset and function, `STOP:`, assertion; what a game logs while it
loses its display at the end is not a crash), as `no impl` (`no_impl_called`) the count of the call
report's `Calls: summary: N unimplemented` line (the distinct imports without implementation the
game called, not the ones bound before it starts that `Runtime: imports ...` counts, which include
code that never runs) and, as `forced` (`submit_forced`), the number of `Forcing submitDone` lines
before then: the GPU library sends a submitDone itself once the game has gone too long without one,
so a count that keeps growing means the game is stalled on the GPU. The table goes to stdout,
`results.tsv` and `results.json` to the output folder with each title's log and dumps
(`tools/regress/report.py <folder>` classifies a folder again). With `-u`, each title's user folder
(saves and settings) starts as a copy of its own in that earlier output folder, so a run can load
what an earlier one saved. `-t tools/regress/perf.txt` runs the titles whose schedules measure the
frame rate in play (`bloodborne-perf.sched`, from a save given with `-u`; run alone on the machine).
With `-c`, a milestone lost or a crash signature that is new or different is a regression: it is
listed and the exit status is 1. The `no impl` and `forced` columns show their change against the
earlier run without counting it as a regression (blank in an earlier file that predates `forced` or
`no_impl_called`).

## CI and binary cache

`.github/workflows/build.yml` builds `packaging/port.nix` on every push to `master` (and by hand), as
a check only: nothing is published. `packaging/port.nix` is the port from source in Nix's sandbox
with `build.sh`, LTO on, no PGO profile, nixpkgs pinned in `packaging/nixpkgs.nix` to decanter's
revision.

Versions are annotated tags `vX.Y.Z`. Pushing one runs `.github/workflows/nix-cache.yml`: it builds
the tag the same way, publishes its closure with `tools/nix_cache.sh` as a flat Nix binary cache in
the assets of the release `nix-cache`, then creates or updates the GitHub Release of the tag with the
tag's annotation as its notes, marked latest when the tag is the highest version published. Only that
job has `contents: write`, with the workflow's `GITHUB_TOKEN`. The output is laid out as a checkout
after `build.sh` (`run.sh`, `scripts/`, `patches/`, `out/psport`, `out/psport-gpu-capabilities`,
`out/gpu/`); the Vulkan drivers are not in it.

The closure is made content-addressed before it is published (`nix store make-content-addressed`):
Nix accepts such paths without a signature, also from a user the daemon does not trust, so the cache
has no key. Every file sits at the top of
`https://github.com/yerasuu/psport/releases/download/nix-cache/`:

| File | Contents |
|---|---|
| `nix-cache-info` | `StoreDir: /nix/store`, `Priority: 50` |
| `<hash>.narinfo` | one per store path (`<hash>` of the path); its `URL` is the flat `<hash>.nar.zst` |
| `<hash>.nar.zst` | the path's NAR, zstd |
| `closure-<commit>` | the store paths of that commit's closure |
| `commit-<commit>` | the store path of that commit's output |
| `release-<tag>` | the commit of that release |
| `latest-release` | `<tag> <commit> <store path>` of the highest version published, one line, uploaded last |

The current release is read from
`https://github.com/yerasuu/psport/releases/download/nix-cache/latest-release`, fetched with
`nix copy --no-check-sigs --from https://github.com/yerasuu/psport/releases/download/nix-cache <store path>`,
and runs as `PS_PREBUILT=1 PS_PROBE=<path>/out/psport PS_DATA_DIR=<writable folder> bash <path>/run.sh`
with Python and the Vulkan drivers of the same nixpkgs from the caller. A tag pushed for a version
lower than the latest (a fix on an earlier line) is published without moving `latest-release`.
Commits on `master` between releases have no build: build them locally.

Each run first keeps this release, the one `latest-release` names and the newest others up to 30
(`NIX_CACHE_KEEP`), fewer if their paths would not fit in the 1000 files a release holds, and deletes
the other releases' `release-` files, the `commit-`/`closure-` files of commits no kept release names
and the paths no kept commit uses; then it uploads only the paths the release lacks. A closure is
about 220 paths (440 files), and a release that changes only the port adds 5. Runs are serialized (a
run prunes what another could be publishing). `tools/nix_cache.sh <store path> <commit> <tag>
<directory>` writes the same files to a local folder, for `nix copy --from file://<directory>`.

The same job also publishes the release's port without Nix, as assets of the tag's own release:
`psport-<tag>-x86_64.tar.zst` and `psport-<tag>-x86_64.tar.zst.sha256` (`sha256sum` format), made by
`tools/bundle.sh <store path> <tag> <directory>` and uploaded after the release is created (a release
without them, as every one before this, is still fetched from the cache). It holds one folder,
`psport-<tag>-x86_64/`, laid out as the output above plus `lib/` (every library the programs and
`out/gpu/` link from the store, with the store's glibc and its loader) and `relocate.sh`. It runs
from wherever it is extracted, writable, as `PS_DATA_DIR=<writable folder> bash <path>/run.sh` with the
host's own Vulkan drivers: `run.sh` sees `relocate.sh`, takes the build as prebuilt and runs it first.

The programs keep starting as programs: started through `ld.so --library-path`, as AppImages do, the
loader is the program and the kernel puts the brk heap above it at ~85 TiB, where the non-PIE psport
can no longer hand the guest heap pointers below 1 TiB. Their PT_INTERP names the bundled loader
instead, written in place by `relocate.sh` the first time and whenever the folder moved (through a
link in `XDG_RUNTIME_DIR` when the path is longer than Nix's field, 82 bytes with the pinned glibc). Their RUNPATHs
become RPATHs, `$ORIGIN/gpu:$ORIGIN/../lib` and then the host's lib directories, and the libraries'
are removed, all in place (patchelf corrupts the non-PIE binary's symbol versions): a library without
one, the host's Vulkan drivers included, has its dependencies searched in the program's RPATH, so the
bundle's libraries come first and the host's after them. Nix's Vulkan loader searches the host's
ICDs in `XDG_DATA_DIRS` and `/run/opengl-driver` but not `/etc/vulkan/icd.d`, which `run.sh` adds
with `VK_ADD_DRIVER_FILES`.
