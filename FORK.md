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
is merged wholesale. bbport, shadPS4 and the forks below are reviewed from time to time, and the
changes useful to psport are ported by hand, each recorded in this file as a row of the table it
belongs to (with its source commit), as the rows below already are. A reviewed point means every
commit up to it was read; what was not ported yet waits as a task. Last reviewed points:

- bbport: `3c57da4` (0.51, 2026-10-11), the last bbport release merged into psport's history
  (merge `884d0ea`); `2d7361a` (FSR 4 v07 pass timing and statistics) was ported by hand after it.
  From `72e5c8c` (0.6-pre1, 2026-10-15): the layer memory module fixes, the online module's shadNet
  version report and `sceNpLookupNpId`, and its seamless co-op (BBSC: its game code, the online
  module's relay, signaling and summon broker changes, the image-ready call, event filter and overlay
  line, the launcher switch and the local game server; rows below), except BBSC's animated title card
  and Discord status. Reviewed up to `8185b88` (0.6, 2026-10-19): its online stack is shadPS4 v0.20's
  copied whole and its The Hunter's Dream client derives from bbhost (GPL-3.0-or-later); neither is
  taken.
- SrShadowy's fork (SrShadowy/bloodborne_pc, GPL-2.0): reviewed up to `f5d3dcb` (2026-10-20, on
  bbport `8185b88`, after its release 0.6.1 `b1d3b8b`).
- Teefpyre's fork (Teefpyre/bloodborne_pc-LinuxBBSCFix): reviewed up to `f3b5504` (2026-10-20, on
  bbport `8185b88`).
- AnyPS5 (boykopovar/AnyPS5, GPL-2.0-only): reviewed up to `5b59fbc` (2026-10-21). Its licence keeps
  its code out of psport; only behaviour is taken, confirmed on the firmware and written natively.
- shadPS4: `b4e7ae7` (2026-10-02), the vendored commit in `gpu/VENDOR.txt`, plus the recompiler
  fixes backported after it that the same file lists. The code ported or written from shadPS4
  outside `gpu/shadps4/` (libSceFiber, libSceFont, Videodec2, the per-NID tables, Audio3d, memory
  pools) follows `3a9f572`, as its rows say. Reviewed up to `c1b2655` (2026-10-21).

### Ported from bbport

| Change | Files | Source | Here |
|---|---|---|---|
| The device is created again without NGX's extensions when it fails with them (DLSS off, FSR 3.1 stays) | `gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp` (`CreateDevice`), `vk_dlss.*` (`Dlss::Disable`) | bbport `72e5c8c` (#135, #147, #152) | as is; the launcher's `libcuda` is its own row |
| An extension whose commands the driver does not resolve after the dispatcher's init is turned off (RIP=0) | `gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp` (`CreateDevice`) | bbport `72e5c8c` (#161, #177, #181) | `VK_AMD_buffer_marker`, `VK_EXT_vertex_input_dynamic_state`, `VK_EXT_extended_dynamic_state3`, `VK_KHR_external_memory_fd` (guest memory export), `VK_EXT_external_memory_host`; logged, no assert for push descriptors |
| Thread records (the `ScePthread` handles) below 40 bits, carved from `runtime_low_map` (lamp travel hung at loading) | `src/kernel/runtime_thread.c` (`allocate_low_record`) | bbport `72e5c8c` (PR #193) | a record outside low memory is logged and counted in the thread report |
| Every upload of a GPU-modified image stores its level hashes, a `GpuDirty` one too, so the first refresh does not upload unchanged bytes over the GPU's data | `gpu/shadps4/video_core/texture_cache/texture_cache.cpp` (`RefreshImage`) | bbport `72e5c8c` (#178) | as is |
| Pipeline cache: a geometry-shader pipeline keeps its vertex stage's fetch shader; NVIDIA's clip-distance discard shader saved after `xfb_stride` (older entries read without it, no version change); a draw without a pixel shader decides clip-distance emulation from its own registers | `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp`, `vk_graphics_pipeline.cpp`, `vk_pipeline_cache.cpp` (`NeedsClipDistanceEmulation`) | bbport `72e5c8c` | an entry left half loaded lends nothing to the next |
| Bloodborne: a null node from the per-frame GX heap skips the render command's post instead of faulting at `0x2199310` | `gpu/games/bloodborne/guest_hooks.cpp` (`InstallGxNodeGuard`, `Report`), `title.cpp`, `src/runtime/runtime_quirks.inc` | bbport `72e5c8c` (#162, #192) | declared `WORKAROUND` quirk `bloodborne_gx_node_guard` until #504 ([docs/en/QUIRKS.md](docs/en/QUIRKS.md)), over psport's guest hooks (a trap with `PS_GUEST_HOOK_TRAPS=1`); deferred commands counted in the profile's stats report |
| Bloodborne: render job slots given back at the job step's failure exit and before the runner's destructor and reset clear them (Laurence's cinematic), Yebis's blur kernel sample count raised to 3 at the entry (`0xa0020007`), the FX init's three history target clears written in memory without the game's clear (bad PM4 at a save's load), the GX swap chain's two per-frame command heaps at 1 MiB instead of 256 KiB | `gpu/games/bloodborne/workarounds.*`, `profile.cpp`, `title.cpp`, `src/runtime/runtime_quirks.inc` | bbport `8185b88` (`gpu/shim/bbport_guest_hooks.cpp` `InstallJobSlotFix`, `InstallKernelSampleClamp`, `InstallFxInitClear`; `gpu/shim/bbport_fx_init_clear.h`; `patches/Bloodborne.xml` "Larger GX Per-Frame Command Heap") | declared `WORKAROUND` quirks `bloodborne_render_job_slots` (#683), `bloodborne_blur_sample_clamp` (#684), `bloodborne_fx_init_clear` (#685) and `bloodborne_gx_command_heap` (#504) ([docs/en/QUIRKS.md](docs/en/QUIRKS.md)), through `PsGuestCode` detours with the title's `patch_image` (so `PS_GAME_PROFILE=none` keeps them), each installed only when all its sites match; each event logged (the first three, then powers of two) and counted in the profile's stats report; the FX targets get the clear colour in their format (RG16F, uncompressed, not fast-cleared; anything else goes to the game's clear) written in memory with the GPU side told before and after (bbport submits a private `WRITE_DATA` packet of zeros) and on any thread (bbport: the main thread only); the heap sizes are written in the image instead of a patch; bbport's live effect switches, render resource cache backing (#673), job allocation probe and GFX8 CPU labels not ported |
| Bloodborne: "Disable Bloom" as an effect switch (`effect_bloom`) | `gpu/games/bloodborne/effects.h`, `patches.cpp`, `game_menu.cpp`, `launcher/psport_launcher.py`, `patches/Bloodborne.xml` | bbport `8185b88` (`patches/Bloodborne.xml`, `scripts/patches.py` `effect_bloom`) | a start-time patch like the other switches (bbport writes it live); in the overlay menu and the launcher, not on the game's own Effects page (six rows, full) |
| `raise_exception` logs its caller and the game's return addresses up the stack | `src/kernel/runtime_kernel.c` (`raise_exception`), `src/kernel/runtime_module.c` (`runtime_module_locate`) | bbport `72e5c8c` | by loaded module and offset instead of Bloodborne's text bounds; the stack walk stops at the thread's stack end |
| Occlusion query segments scaled by their render pass's factor (8.8 fixed point from `target_scale`), so counts are at the game's resolution under the upscaler; `PS_OCCLUSION_SAMPLES=N` diagnostics | `gpu/shadps4/video_core/renderer_vulkan/vk_occlusion.*` (`SetPassScale`), `host_shaders/occlusion_resolve.comp`, `vk_rasterizer.cpp` (`OcclusionTranslated`, before `BeginRendering`) | bbport `72e5c8c` | over psport's scheduler and rasterizer (bbport's own renderer); on by default (`PS_OCCLUSION_QUERIES=0` turns them off), the default one constant (`OCCLUSION_QUERIES_BY_DEFAULT`) |
| A draw's push constants recorded after its render pass begins: beginning it may record the occlusion resolve's compute dispatch, whose push constants replaced the draw's `xoffset`/`yoffset` (single frames shifted by half the screen) | `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` (`DrawRecord`, `DrawIndirectRecord`: `BindResources` after `Scheduler::BeginRendering`) | bbport `8185b88` (0.6) | both draw paths; bbport's viewport re-invalidation at a new pass and its diagnostics not taken |
| A submission waits on every semaphore at all stages: the presenter's frame was waited at `ColorAttachmentOutput` while the present blit (Transfer) reads it earlier | `gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp` (`SubmitExecution`, `SubmitAsync`) | bbport `8185b88` (0.6, present wait mask) | both submission paths; the direct one's mask array sized for the four waits `SubmitInfo` holds |
| Every swapchain recreation (resize, failed acquire, failed fence reset, HDR change) under `Scheduler::submit_mutex`, since it waits for the device idle while recording threads submit (bbport #211); a 0x0 surface logged once per occurrence with its count, and its return | `gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp` (`Present`), `vk_presenter.h` (`SetHDR`), `vk_swapchain.*` (`Create`) | bbport `8185b88` (0.6); SrShadowy `f26538d` (0x0 surface) | the 0x0 log in psport's swapchain (`LOG_WARNING`) instead of SrShadowy's video backend hook |
| Pipeline cache writes on one writer thread that drains its queue before stopping, each blob to a temporary file renamed into place | `gpu/shadps4/video_core/cache_storage.cpp` | bbport `72e5c8c` (`render/blob_store.*`) | inside shadPS4's `Storage::DataBase`; leftover `.tmp` files removed at open; the archived cache is written to `<serial>.zip.tmp` with the kept entries and renamed at close; `FlushForExit` (through `psport_gpu_flush_caches`) drains the queue within 2 s before psport's `_exit`/exec paths, which never reach `Close`; on-disk format unchanged |
| `--play` sets the launcher's language; the in-game menu saves `menu_language` only once chosen in it or read from the file | `launcher/psport_launcher.py` (`play`), `gpu/port/psport_settings.*` (`is_menu_language_chosen`), `gpu/port/psport_overlay.cpp` | bbport `72e5c8c` (#134) | with psport's Spanish as a third language |
| Steam Game Mode's pressure-vessel Vulkan, layer and `LD_LIBRARY_PATH` entries dropped so the bundled or host drivers are found | `launcher/psport_vulkan.py` (`configure`) | bbport `72e5c8c` (#172) | as is |
| `libcuda.so*` linked next to the NVIDIA ICD, for NGX (DLSS) | `launcher/psport_vulkan.py` (`configure`) | bbport `72e5c8c` (#135) | as is; the bundle needs nothing more |
| `run.sh`'s log on a descriptor bash picks (`exec {log_fd}>`), not 3, so the AppImage runtime's pipe stays open | `run.sh` | bbport `72e5c8c` (AppImage `--play`) | as is |
| miniz built with FetchContent (tag `3.1.2`) when no package exists; the online module's missing dependencies named | `gpu/CMakeLists.txt` | bbport `72e5c8c` (#136, #145) | as is |
| FP8 FSR 4 passes captured under an RX 9070 XT's id on an RX 9000 that needed the RX 7800 XT's id | `tools/fsr4cap/capture_all.sh` (`rdna4_present`) | bbport `72e5c8c` (#186) | the FP8 id kept for every FP8 size, not only the first |
| Guest code hooks: a byte-checked jump to a trampoline that hands a host function every register (shadPS4's `InstallGuestCodeHook` of the shadp2p/BBSC forks), the stub emitter of bbport's detours | `gpu/port/psport_guest_code.*` (`PsGuestCode::InstallHook`, `InstallDetour`, `Emitter`) | bbport `72e5c8c` (`gpu/shim/bbport_guest_code_hooks.*`, `bbport_guest_code.h`) | for any game's profile; results as `PsGuestCode::Error` values instead of `bool`, every hook logged when it goes in or is refused; `RedirectOperand` (a RIP-relative operand moved, checked with Zydis) and `MapNear` added; pages still writable during the loader's patching are written in place |
| Chord bindings (a modifier per key slot; its plain bindings held back while the chord's modifier is down), the device in use and short key names for button prompts, the Dark Souls III-like layout | `src/input/runtime_pad.*` (`apply_binding_line`, `is_chord_held`, `runtime_pad_prompt*`), `gpu/games/bloodborne/title.cpp` (`DefaultControls`), `gpu/games/game_profile.h` (`sz_default_controls`), `launcher/psport_launcher.py` (`CONTROLS`) | bbport `72e5c8c` (`src/runtime_pad.c`) | the layout is Bloodborne's title data, other games keep psport's neutral layout; the motion emulation and two-finger touch are their own rows |
| Bloodborne's frame rate by hooks: the pacer's mode, late frames and the main loop's step detoured, two fixed steps, the run's wall check by speed, 48 RIP-relative constants read from a live page rewritten each frame (dt, damping, approaches, cloth stiffness), 480 FPS cap | `gpu/games/bloodborne/frame_rate.*`, `patches.cpp` (`FrameHooksRefusal`, `FrameHooksChosen`), `game_check.cpp` (`IsSupportedImage`), `profile.cpp`, `gpu/games/game_profile.h` (`pfn_drives_frame_rate`), `video_out.cpp` | bbport `72e5c8c` (`gpu/shim/bbport_frame_rate.*`, `BB_FRAME_HOOKS`) | through `PsGuestCode`; chosen at launch (`PS_FRAME_HOOKS`, the profile, the guest hooks, the 1.09 executable's digest) so the preset's patches stay the fallback; the 60 and 90 presets keep their vblank |
| Bloodborne's play time counted by real time with its frames hooked: the step's milliseconds floored with the fraction carried to the next frame instead of truncated each frame | `gpu/games/bloodborne/frame_rate.cpp` (`HookPlayTime`) | SrShadowy `d54f932` (`gpu/shim/bbport_engine_fixes.cpp` `kPlayTimeAdd`) | a detour through `PsGuestCode` that computes the game's own registers (rcx, xmm0) with the carry in the frame hooks' live page and touches no other register or flag, instead of the fork's stub that saves xmm0-1 only; installed only with the frame hooks |
| Native mouse look (the follow camera's turn increment detoured, the mouse's turn added; a turn not taken for 100 ms hands the mouse back to the right stick) and the menu pointer (Scaleform list hit testing, cursor moves, clicks as pad presses), the window's motion settle after capture | `gpu/games/bloodborne/mouse_look.*`, `menu_pointer.*`, `profile.cpp`, `gpu/games/game_profile.h` (`MouseHooks`), `gpu/port/psport_gpu_input.cpp`, `gpu/shim/window.cpp`, `src/input/runtime_pad.c` (`apply_mouse_look`) | bbport `72e5c8c` (`gpu/shim/bbport_guest_hooks.cpp` `InstallLookHook`, `gpu/games/bloodborne_menu_pointer.*`, `gpu/shim/window.cpp`, `BB_NATIVE_MOUSE`, `BB_MENU_POINTER`) | through `PsGuestCode`, installed with the profile's `patch_image` (after the launch's patches); the window and pad parts are profile callbacks instead of registration lists |
| Output aspect ratios (auto, 4:3, 16:10, 16:9, 21:9, 32:9) and their resolutions, the monitor's group for auto, the UI's edge stretch and the black margin of a frame without a scene | `gpu/port/psport_settings.*` (`Aspect`, `Outputs`, `NearestOutput`, `CurrentOutput`, `SetMonitor`), `gpu/port/psport_overlay.cpp`, `gpu/shim/window.cpp` (`TellMonitor`), the renderer files in [Changes to inherited shadPS4 code](#changes-to-inherited-shadps4-code), `gpu/games/game_profile.h` (`pfn_display_aspect`), `gpu/games/bloodborne/game_menu.cpp` (the output row by height), `launcher/psport_launcher.py`, `launcher/psport_i18n.py` | bbport `72e5c8c` (`gpu/shim/bbport_settings.*`, `bbport_overlay.cpp`, `window.cpp`, `gpu/upscale/`, `gpu/render/raster_targets.cpp`, `gpu/shader/backend/spirv/emit_spirv_special.cpp`, `launcher/bbport_launcher.py`) | 16:9 by default instead of auto; a game whose profile does not follow keeps a 16:9 box; the game menu's output row picks the height within the aspect ratio chosen elsewhere (bbport's new select rows and aspect row in the game's menu not ported) |
| What a title's online extension asks of the engine: a call once the image is mapped and patched, the window's events before the port's, a line over the game, the game's code kept writable | `src/main/main.c`, `src/net/runtime_net.c` (`runtime_net_image_ready`, `title_extension`), `gpu/net/psport_net.h` (`psport_net_image_ready`, `sz_extension`, ABI 3), `gpu/net/psport_net_title.*` (`PsNetTitle`), `gpu/net/CMakeLists.txt` (`games/*/net/`), `gpu/games/game_profile.h` (`OnlineExtension`), `gpu/psport_gpu.h` (`psport_gpu_add_event_filter`, `psport_gpu_overlay_banner`), `gpu/shim/window.cpp`, `gpu/port/psport_overlay.*` (`SetBanner`), `gpu/port/psport_guest_code.*` (`KeepWritable`), `src/main/cli.c` (`--online-overrides`) | bbport `72e5c8c` (`bbnet_image_ready`, `bbgpu_add_event_filter`, `bbgpu_overlay_banner`, `Core::KeepGuestCodeWritable`, the `/proc/self/maps` pass of `gpu/bbnet/bbnet_glue.cpp`) | generic and title-driven instead of a check for Bloodborne's profile name: a title names its extension and switch (`Game::OnlineExtension`), the runtime passes the name only when the switch is on, and the module starts what it carries under that name; the writable code is the guest code layer's, asked for by the extension |
| Online module: the P2P relay fallback (`BBR1` over UDP 3478 on the shadNet server's address, peers kept by online id with every endpoint as an alias), the signaling rejoin fix (a connection activated again after a deactivate is checked with the peer for 10 s; a new peer connection id restarts it), a log line for each refusal of `sceNpSignalingActivateConnection`, the static peer map (`SHADPS4_BBSC_PEERS`, `bbsc_peers.json`), BBSC's signaling packets (`np_signaling_bbsc.h`, `SendBbscFollowToPeers`, `SendBbscWorldFlagsToPeers`) and the room member replay (`BbscReplayRoomMembers`) | `gpu/net/shadps4/core/libraries/network/p2p_sockets.cpp`, `sockets.h`, `gpu/net/shadps4/core/libraries/np/np_signaling/np_signaling.cpp`, `np_signaling_state.*`, `np_signaling_helpers.*`, `np_signaling_bbsc.h`, `gpu/net/shadps4/core/libraries/np/np_matching2/np_matching2_mm.*`, `np_matching2_signaling.cpp` | bbport `72e5c8c` (`gpu/bbnet/shadps4/core/libraries/network/`, `np/`: BBSC's changes to shadp2p `6e97b40` and its relay of 2026-10-14) | three-way merged onto psport's copies; the relay is off unless `SHADPS4_P2P_RELAY` sets it or a title's extension asks for it (`P2PRelaySetDefaultOn`, logged once), where bbport has it on for every game; the static peer file is parsed without exceptions |
| BBSC, Bloodborne's seamless co-op (CLS_AMG, written for shadp2p): the session kept through deaths, lamps, area changes, bosses and the Hunter's Dream for up to five hunters, its byte patches, hooks, world sync, persistence, memory scan, cinematic camera (F1) and co-op texts; the summon broker's connect-first area, stale record cleanup and fast sign (Bloodborne's summon client, `summon_client.cpp`); its signaling packets and `sceNpMatching2LeaveRoom` handed to its code (`PsNetTitle::Extension` `pfn_signaling_packet`, `pfn_leave_room`) | `gpu/games/bloodborne/net/` (`core/bloodborne_re.*`, `core/bbsc_*`, `imgui/bbsc_*.h`: BBSC's files and headers; `bbsc_glue.cpp`: the extension; `summon_client.cpp`: the summon client), `gpu/games/bloodborne/title.cpp` (`extension`), `gpu/net/include/core/cpu_patches.h`, `core/memory.h`, `core/file_format/psf.h`, `common/memory_patcher.h`, `gpu/net/psport_net_glue.cpp` (`Core::InstallGuestCodeHook`), `gpu/net/shadps4/core/libraries/network/http.cpp`, `np/np_signaling/np_signaling_helpers.cpp`, `np/np_matching2/np_matching2.cpp` | bbport `72e5c8c` (`gpu/bbnet/shadps4/core/bloodborne_re.cpp`, `bbsc_*`, `gpu/bbnet/include/`, `gpu/bbnet/bbsc_glue.cpp`, `bbnet_image_ready`, `docs/BBSC.md`) | Bloodborne's extension of the online module instead of a check for the profile's name: off unless `PS_BBSC=1`, and until then its default switches count as off (`Core::Bbsc::Started`, the one change to BBSC's files) so online play without it behaves as before; its hooks go through `PsGuestCode` (byte-checked and logged), its writable code through `PsGuestCode::KeepWritable`, its loading screen is an overlay line (`psport_gpu_overlay_banner`), the cinematic camera's events come from `psport_gpu_add_event_filter`; the save folder is found under any save title; the stale summon cleanup uses the title's summon route and parses without exceptions; the animated title card, Discord status and `BB_BBSC_PROBE` not ported |
| BBSC's switch and its local game server: the launcher's "Seamless co-op (BBSC)" row (`online_bbsc`, `PS_BBSC=1`), BBSC's game server (from BBSC-Server-Linux) run on this PC when the community server's does not answer, its data and logs in `<user>/bbsc/`, stopped with the game | `launcher/psport_launcher.py`, `launcher/psport_i18n.py`, `run.sh`, `scripts/online.py` (`local-server`), `scripts/bbsc_gameserver/` (BBSC's files, unchanged: NoDreamForHunter format, MIT, and BBSC's GPL-2.0-or-later notice), `gpu/games/bloodborne/title.cpp` (`extension` local server fields) | bbport `72e5c8c` (`launcher/bbport_launcher.py`, `bbport_i18n.py`, `run.sh`, `scripts/online.py bbsc-local`, `scripts/bbsc_gameserver/`, `BB_BBSC`, `BB_BBSC_LOCAL_SERVER`) | off by default (bbport's launcher has it on); run.sh and `online.py` know no game: the script, its arguments, its switch and the status path are Bloodborne's title data (`--online-overrides`), and the server runs only with `PS_BBSC=1` and `PS_BBSC_LOCAL_SERVER=1`, or `auto` (5 s probe), never by default (bbport: `auto`) |
| BBSC's param tables taken from the game's SoloParamRepository (63 slots, every resource loaded in each) instead of a search of the address space, which stays as the fallback; that search walks the runtime's map, reading direct memory through its backing view when `process_vm_readv` cannot (its dma-buf mappings); BBSC's reads of the image and other host-owned low memory answered from the host's mapping list (gaps cached) and copied without faulting | `gpu/games/bloodborne/net/core/bloodborne_re.cpp` (`FindBbscParamTables`), `core/bbsc_memscan.cpp` (`FindInMemory`), `gpu/net/include/core/memory.h` (`QueryProtection`) | SrShadowy `f5d3dcb`, `4e0ab58`; Teefpyre `734218ca6` | slots and resource chain from the firmware (kb #681: slot `repo+0x70+i*0x48`, count then `resource*[8]`; the forks read slot i-1 and its first resource only), the repository checked by its vtable, at the global or through it (which one logged once); each fallback search logged with its count and why (no repository, or no such table yet), not behind a variable; low memory asked again on every cache miss (250 ms), not a snapshot taken once; the forks' `extra_safe_read` (an unguarded `memcpy`) not taken |
| BBSC's hunter's kit given to a new character too (no storage box yet; an unreadable box waits), the character marked as checked only once the kit is given or nothing is missing; the save backup tells no save yet from a failed copy, and finds psport's saves (the runtime's user folder, any save title holding `SPRJ0005`), each base backed up to its own folder | `gpu/games/bloodborne/net/core/bloodborne_re.cpp` (`TickBbscDuelBellGive`, `BackupBbscSaves`, `BbscSaveBackup`) | SrShadowy `e4d94b3`; Teefpyre `734218ca6` | never given without a backup: a failed copy blocks the kit, logged with its count and tried again; no save on disk (the runtime's save folder for its title confirmed absent for every user, with no discovery error and a valid `BBSC_SAVE_DIR`) is the one case given without one; one backup per character, not per retry; the forks' `return true` on a failed backup, kit given anyway, per-tick diagnostics and warning-level logs not taken; the shared progress merge still counts only a real copy |
| Bloodborne's live aspect ratio: the camera's projection stores (aspect, and the field of view widened for taller outputs), the HUD markers' placement scale, the HUD's groups moved to the output's edges through Scaleform members, the UI movies' viewport pixel aspect | `gpu/games/bloodborne/aspect.*`, `profile.cpp` (`pfn_display_aspect`) | bbport `72e5c8c` (`gpu/shim/bbport_guest_hooks.cpp` `InstallCameraAspectHooks`, `InstallMarkerHooks`, `HudFrame`, `ViewportAdjust`, `SetDisplayAspect`, `BB_HUD_ANCHOR`) | through `PsGuestCode` detours installed with the profile's `patch_image`; the camera follows only when all five stores and the viewport hook are in (else 16:9 in the middle); the GX command heap guard (#162) and the RE probes not ported |
| The pad's default state reports 1 g of gravity up (the controller lying flat) instead of zero acceleration, which a game reads as free fall | `src/input/runtime_pad.c` (`sample_host`) | bbport `72e5c8c` (`src/runtime_pad.c`) | as is |
| The touchpad keys add a finger each (both held: two fingers, one on each half) instead of one replacing the other | `src/input/runtime_pad.c` (`touch_click`) | bbport `72e5c8c` (`src/runtime_pad.c`) | also for the gamepad's right-touchpad binding and `PS_PAD_FILE`'s `touchpad_left`/`touchpad_right` |
| `PS_PAD_FILE`'s `mdx=` and `mdy=` move the mouse that many pixels at every pad read, as if the window held it (scripted mouse look tests) | `src/input/runtime_pad_script.c` (`runtime_pad_read_inject`), `runtime_pad.h` (`PadInjected`), `runtime_pad.c` (`sample`) | bbport `72e5c8c` (`src/runtime_pad.c`) | through the native camera hook when the profile has one, else the right stick; the pad file's log line shows the motion |
| `PS_PAD_TRACE=1` logs the pad state the game reads when its buttons change and every 5 s | `src/input/runtime_pad.c` (`trace_read`) | bbport `72e5c8c` (`BB_PAD_TRACE`) | outside the pad's lock; triggers and touch count logged too |
| Movie skip: a key or pad button that stops the AvPlayer playing, and Bloodborne's Options press while its input is blocked so the game leaves the scene | `gpu/shadps4/core/libraries/avplayer/avplayer_state.*` (`RequestSkipAll`, `AvEventType::Skip`), `avplayer_common.h`, `gpu/port/psport_gpu_input.cpp` (`psport_gpu_skip_movies`, `psport_gpu_skip_scene`), `gpu/psport_gpu.h`, `src/input/runtime_pad.c` (`skip_update`, `skip_movie`), `gpu/games/game_profile.h` (`pfn_skip_scene`), `gpu/games/bloodborne/scene_skip.*`, `menu_pointer.*` (`IsInputBlocked`), `profile.cpp`, `launcher/psport_launcher.py`, `launcher/psport_i18n.py`, `src/platform/host_features.inc` | SrShadowy `b1d3b8b` (`extra/core/engine_fixes/bbport_video.cpp`, `extra/management/prompts/bbport_pad_hook.h` `extra_pad_apply_cutscene_skip`, `gpu/platform/avplayer/avplayer.cpp`) | off unless `skip_movies=1` or `PS_SKIP_MOVIES=1`, with its own binding (Escape, `misc1`) instead of Enter and Esc; every player playing or buffering a movie that does not loop, not the last one created (nor one paused or not started); the stop is a request the player's controller thread carries out, not a `Stop()` from the input thread racing the decoder; the press is kept from the game only when something was skipped; Bloodborne's Options press is one per press, only when the press stopped a movie while its input is blocked, behind its profile instead of in the core and not repeated every frame; each skip logged with the player and its position, totals in the pad report |
| A refused `scePadOpen` (another user, port type or index) is logged with its arguments | `src/input/runtime_pad.c` (`pad_open`) | bbport `72e5c8c` (`src/runtime_pad.c`) | as is |
| The move and look keys as a stick on the unit circle (a diagonal as long as one key) that ramps to its edge in 70 ms, back in 45 ms, and turns at 1100 degrees a second the short way round | `src/input/runtime_pad.c` (`key_stick`, `apply_keyboard`) | bbport `8185b88` (`src/runtime_pad.c` `key_stick`) | `PS_KEY_RISE_MS`, `PS_KEY_FALL_MS`, `PS_KEY_TURN_DPS`; the same commit's key latch is the next row; its dodge, sprint and jump keys and Escape as Circle in menus are Bloodborne's layout, not ported to the core |
| A press that begins and ends between two pad reads reaches the game: keys, the held mouse's buttons and the gamepad's buttons and triggers latched when the window thread receives their events, each with its own time, folded into the next `scePadReadState`/`scePadRead` once, dropped after 100 ms unread, cleared by `scePadInit`, while the in-game menu is open and when it opens or closes (the presses that do it are the menu's); a trigger keeps its furthest pull | `src/input/runtime_pad.c` (`latch_event`, `latch_take`, `code_down`, `pad_button_value`, `runtime_pad_report`) | AnyPS5 `5786a9628`, `baf107a58`, `f3e05559d` (behaviour only, GPL-2.0-only); bbport `8185b88` (keys only) | written natively at the host events, not at a published pad state; presses SDL still shows as held are not counted; `short presses` and `expired unread` in the pad's exit report; to confirm on the PS4 firmware |
| `scePadReadExt` and `scePadReadStateExt` read as `scePadRead` and `scePadReadState` | `src/input/runtime_pad.c` (`s_exports`) | shadPS4 `8d50471b7` (`src/core/libraries/pad/pad.cpp`) | no extension controller is emulated |
| Motion emulation for controllers without sensors: the spray can (the controller held sideways: G, or View+A on a pad), aiming with the mouse or the right stick, spraying with the left button, shaking (H, View+B), centering (T); touchpad swipes (a touchpad key + the mouse; View + the right stick one finger, View + the left stick two); a View tap as the touchpad; orientation, gravity and angular velocity reported consistently; the window takes the mouse on request | `src/input/runtime_pad_motion.c`, `runtime_pad.h`, `runtime_pad.c` (`load_bindings`, `sample_host`), `gpu/psport_gpu.h` (`psport_gpu_mouse_capture`), `gpu/port/psport_gpu_input.cpp`, `gpu/shim/sdl_window.h` (`RequestMouseCapture`), `gpu/shim/window.cpp` | bbport `72e5c8c` (`src/runtime_pad.c` `motion_*`, `BB_PAD_MOTION`, `bbgpu_mouse_capture`) | its own file instead of runtime_pad.c; off unless `pad_motion=1` in `psport.ini` or `PS_PAD_MOTION=1`; the swipe drag follows both touchpad keys as bindings, not Tab and Backspace |
| The LEGO Dimensions toy pad emulated behind libSceUsbd: the USB HID device (0E6F:0241) and its descriptors, the pad's protocol (wake, TEA seed and challenge, figure reads, writes and model query, lights), figures placed and lifted on the three zones from a file read again when it changes, NTAG213 tag images made on first use; the overlay's Toy pad section places and lifts figures and shows each zone's light | `src/system/runtime_toypad.c`, `runtime_usbd.h`, `runtime_devices.c` (`runtime_devices_resolve`), `gpu/port/psport_overlay.cpp` (`ToyPadSection`), [docs/en/TOYPAD.md](docs/en/TOYPAD.md) | bbport `72e5c8c` (`src/runtime_toypad.c`, `gpu/shim/bbport_overlay.cpp` `ToyPadSection`) | a generic peripheral, off by default (`PS_TOYPAD=1` or `toypad=1` in `psport.ini`; bbport connected it unless `BB_TOYPAD=0`), so other games keep seeing no USB device; files in `<user>/toypad/` (`figures.txt`, `figure_ids.txt`, tag images) instead of `<user>/toypad.txt`; the overlay asks the runtime for the file (`runtime_toypad_file`) instead of an environment variable; libusb's event wait (60 s or the caller's timeout, waking every 100 ms for figure changes) instead of 10 ms; `sceUsbdOpen`, `CancelTransfer`, `HandleEventsTimeout`/`Locked`, `Attach`/`ReleaseInterface` for the pad too; unanswered commands and dropped reports counted in the call report; `PS_TOYPAD_TRACE` not ported; overlay translated into Spanish |
| Button prompts for the device in use: the key guide icons (`KG_*` of `menu/common.tpf.dcx`, 32x32 BC7) and the HUD atlas that packs three of them (`MENU_Common_00091`) known by their bytes, hashed from the game's file; icons drawn by the port (key caps with the bound key's name in DejaVu Sans through stb_truetype, the mouse with its button lit, another gamepad's buttons) sampled instead while the keyboard and mouse or another gamepad was used last | `gpu/games/common/glyphs.*`, `button_prompt_art.*`, `gpu/games/bloodborne/button_prompts.*`, `fromsoftware_tpf.cpp`, `profile.cpp`, `gpu/games/game_profile.h` (`TextureSubstitution`, `p_glyphs`), `gpu/shadps4/video_core/texture_cache/texture_substitutes.*` | bbport `72e5c8c` (`gpu/shim/bbport_button_prompts.*`, `gpu/render/vk_button_prompts.*`, `raster_bind.cpp`, `raster_textures.cpp`, `texture_cache_upload.cpp`, `BB_BUTTON_PROMPTS`) | split into a generic texture substitution in the texture cache (by content hash, a picture array and composed copies with cells, never in a texture set), a generic glyph table any profile declares as data (#721: shapes, hashes given or computed from the game's files by an extractor, atlas cells) and Bloodborne's table with its palette and its DCX/TPF extractor, the drawing shared by every title (Atelier Ayesha DX's table, `gpu/games/ayesha/profile.cpp`, an atlas stored upside down, cells drawn turned over: #782); `PS_BUTTON_PROMPTS=0` turns it off; the copy with cells keeps the texture's sRGB or UNORM encoding; one copy per atlas, not per image; the bind-time rehash only while the port's icons are drawn |
| Windows host calls: `BCryptGenRandom`, `LoadLibraryExW` with `LOAD_WITH_ALTERED_SEARCH_PATH` and wide paths, `GetModuleHandleExW` by address, thread times, the process affinity mask and `THREAD_PRIORITY_IDLE` for background threads; from Supermedo's Windows port, high-resolution waitable timers and `WaitOnAddress` for address waits; the guest's TCB in a TEB TLS slot below 64, the patched `mov r64, gs:[0]` reading `0x1480 + 8 * slot`, and that load matched for all 16 registers | `src/platform/win32/` (`host_system.c`, `host_library.c`, `host_time.c`, `host_thread.c`, `host_win32.c`), `src/loader/loader_link.c` (`patch_fs_loads`), `src/main/main.c` | bbport `72e5c8c` (`gpu/platform/win32/posix_win32.cpp` `bb_guest_tls_slot`, `posix_files.cpp`, `src/probe.c` `patch_guest_tls`, `scripts/link_modules.py` `patch_fs_loads`); Supermedo/bloodborne_pc `windows` (`src/compat_win.c`, GPL-2.0-or-later) | written by hand on psport's `ps_host_*` ABI instead of POSIX emulation (decision #540): Win32 errors as errno values, UTF-8 everywhere, the nearest export named from the PE export directory, one timer per thread closed with the thread (FLS); the slot taken by `ps_host_tls_init` before anything else in `main` and its displacement written while linking (`ps_host_tls_tcb_offset`, 0 on Linux), not in a pass after it; not yet built (#614) |
| Windows guest memory: the guest's range reserved as one `VirtualAlloc2` placeholder, fixed mappings carved into it (views unmapped with `MEM_PRESERVE_PLACEHOLDER`, placeholders coalesced and split, the kept parts of cut views mapped again with their page protections) and filled with `MapViewOfFile3` views of pagefile-backed sections; from Supermedo's Windows port, a section that exceeds the commit limit (errors 1450 and 1455) created `SEC_RESERVE` and committed as views map it | `src/platform/win32/host_memory.c`, `src/platform/ps_host.h` (`ps_host_vm_reserve_arena`), `src/platform/win32/host_win32.h` (`ps_win32_section_close`) | bbport `72e5c8c` (`gpu/platform/win32/posix_memory.cpp`); Supermedo/bloodborne_pc `windows` `7c9ebcb` (`src/runtime_memory.c`, GPL-2.0-or-later) | written by hand on `ps_host_vm_*` (decision #540): errors returned as errno values and logged instead of `exit`, private memory's own sections closed with their last view (bbport kept them open), section descriptors from `PS_WIN32_SECTION_BASE` closed through `ps_win32_section_close`, discards zero only committed pages, `ps_host_vm_protect` passes over pages without storage under a shared lock so that it cannot interleave with a carve; file views refused (`ENODEV`) until host_file.c hands out handles; checked under Proton's Wine with a test program, `main` does not reserve the arena yet (#614) |
| Windows faults and thread interrupts: a first vectored exception handler giving access violations, in-page errors, illegal instructions, arithmetic exceptions and int3 to the fault handlers, a setjmp/longjmp of the Win64 callee-saved state (xmm6-15, MXCSR and the x87 control word) that unwinds nothing, and a faulting recovery resumed in that longjmp outside the exception dispatch, rsp moved below the red zone | `src/platform/win32/host_fault.c`, `src/platform/ps_host.h` (`PsHostRecoveryPoint`, `PS_HOST_RECOVERY_SET`), `gpu/port/psport_runtime.def` | bbport `72e5c8c` (`gpu/platform/win32/posix_win32.cpp` `DispatchException`, `bb_setjmp`, `bb_longjmp`); Supermedo/bloodborne_pc `windows` `3fabfa7` (`src/probe.c` `vectored_fault`, `src/compat_win.c`, GPL-2.0-or-later) | written by hand on psport's `ps_host_*` ABI instead of a `sigaction` emulation (decision #540): the handlers read and set registers through `ps_host_fault_register`, rip past an int3 as on Linux, single steps as breakpoints, a kind faulting again in its own handler left to Windows unless `PS_HOST_FAULT_NESTED`, the last Win32 error kept across handlers; the recovery leaves the dispatch through its own recovery point instead of returning from it; psport's own: thread interrupts (guest exception, thread dump, watchdog, samples) by `SuspendThread` and a `CONTEXT_CONTROL`-only redirect to an entry stub that saves the registers and the AVX state with `xsave` and resumes through `iretq`, held and pending bits per thread for the signal mask, `ps_host_resume` on the same frame, `NtWriteVirtualMemory` so that copies do not lift write protection, unwinding with `RtlVirtualUnwind`; the fork's red-zone trampolines for each store site (`bc893e2`) not taken: the red-zone overwrite is listed in `host_features.inc`; not run with a game yet (#614) |
| Windows files, processes and sockets: `CreateFileW` with full sharing and `FILE_FLAG_BACKUP_SEMANTICS` for folders, `GetFileInformationByHandle` for stat with `FILETIME` converted from 1601, `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` and `MOVEFILE_WRITE_THROUGH` for rename, `FindFirstFileExW` with `FindExInfoBasic` and `FIND_FIRST_EX_LARGE_FETCH` for listings, `pread`/`pwrite` as `ReadFile`/`WriteFile` at an `OVERLAPPED` offset; Winsock started once with `WSAStartup(2.2)`, `ioctlsocket(FIONBIO)` and `FIONREAD`; a restart as `CreateProcessW` | `src/platform/win32/` (`host_file.c`, `host_process.c`, `host_net.c`, `host_system.c` `ps_host_host_name`, `host_win32.c` error codes) | bbport `72e5c8c` (`gpu/platform/win32/posix_files.cpp`, `src/probe.c` `runtime_restart`); Supermedo/bloodborne_pc `windows` (`gpu/bbnet/vendor/shadps4/core/net/host_net.cpp`, GPL-2.0-or-later) | written by hand on psport's `ps_host_*` ABI over Win32 handles instead of the C runtime's descriptors (#613): psport's own descriptor table (0-2 copies of the standard handles, a duplicate onto them followed by the C runtime and `SetStdHandle`), `pread`/`pwrite` on a second `ReOpenFile` handle so that the shared file pointer stays put, `O_APPEND` as the all-ones offset, paths past `MAX_PATH` with `\\?\`, read-only files removable as on Linux, links and junctions reported as links, `GetFinalPathNameByHandleW` for real paths; exec with the arguments quoted for `CommandLineToArgvW`, only descriptors 0-2 inherited through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` and the caller waiting to pass on the exit code (bbport exits at once); WSA errors as errno values and `SIO_TCP_INFO` for queues and TCP state; pipes as `CreatePipe` polled with `PeekNamedPipe`, flock as `LockFileEx` over the whole file (mandatory on Windows, unlike flock's advisory locks), no socket pairs (`EOPNOTSUPP`, counted); not yet built (#614) |
| A faulting SRT walker load supplies zero for that invocation only: the fault handler zeroes `rdi` or `r10` in the thread's context and steps past the load instead of rewriting the shared `mov` (which raced with other threads and left later draws reading zero), and a null or short V# table in the user data reads as an empty descriptor (bbport's crash 139); on a thread that reads ahead of the command processor with a recovery point set (draw preparation workers, the draw pipe's table check) the handler declines instead, so the walk is abandoned and the draw left to the GPU thread, counted as `speculative walks abandoned` (#888) | `gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp` (`SrtWalkerSignalHandler`, `RegisterSrtFaultHandler`), `shader_recompiler/info.h` (`ReadUdReg`), `common/signal_context.*` (`GetRegister`, `SetRegister`, `IncrementRip`) | bbport `8185b88` (`gpu/shader/ir/passes/flatten_extended_userdata_pass.cpp`, `gpu/shader/info.h`) | through the platform layer (`ps_host_fault_register`, `ps_host_fault_set_register`), whose win32 side is still missing (#614); faults counted in the call summary (`LOG_ERROR_COUNTED`), an unexpected instruction or register passed to the next handler instead of an assertion; the handler is also registered for walkers read back from the pipeline cache, and the range it checks ends at the last finished walker |
| Guest memory copies with VRAM full: no `Vulkan::Check` on the system-memory allocations, then a wait for the submitted work, the buffer cache's empty chunks freed and one more try over what is still unbound before giving up (the scheduler's pending operations are not run from there: a fault callback among them binds buffers again); a real failure is logged with its size and counted, and the blocks stay unbound (forgotten when unmapped) until a later binding, which fills them from the game's memory again; the wait happens once a frame at most and never inside the scheduler's own work (pending operations, the submit callback: `Scheduler::IsInsideSchedulerWork`), and a promotion to VRAM never waits | `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp` (`AllocateResidency`, `EnsureResident`, promotion) | bbport `8185b88` (PR #206, `gpu/buffers/buffer_cache.cpp`) | the assertion bbport kept is gone: the arena is sparse resident, so an unbound block reads undefined and drops writes instead of stopping the game; a block that cannot move to VRAM stays in place; the merged unbinds of the same PR not ported |
| An image tiled back into system memory (a texel buffer over a render target) is tiled into VRAM and copied over in one transfer, instead of one write over the bus per texel, where the tiler writes every byte (levels back to back, whole workgroups) | `gpu/shadps4/video_core/texture_cache/tile_manager.*` (`TileImage`), `buffer_cache/buffer_cache.*` (`IsHostMemory`), `gpu/memory/psport_guest_memory.*` (`Chunk::is_device_local`), `gpu/port/psport_toggles.h` (`LocalTiling`, `g_c_local_tilings`), `gpu/shadps4/core/libraries/videoout/driver.cpp` | bbport `8185b88` (PR #210, `gpu/textures/detiler.*`) | chosen by the memory type the range lives in (a guest chunk without `DEVICE_LOCAL`, or a host buffer), never by vendor: psport's arena is sparse, so its own type says nothing; `PsToggle::LocalTiling` turns it off while the game runs; per-second `Local tiling:` stats; binding the texel buffer to the VRAM copy and the profiler mark not ported |
| Minor GPU core fixes: a WRITE_DATA of more than 8 bytes the GPU writes marks the image it covers whole for a refresh; the texture collector's pressure level keeps at most 30 % of the budget free (at least 2 GB) instead of 60 % of the threshold; a compute image copy whose buffers run up to 64 KiB past the image copies those bytes as a buffer; the init-state calls report only the dwords they write to the profile's hook | `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` (`WriteDataOnGpu`, `IsComputeImageCopy`), `texture_cache/texture_cache.*` (`InvalidateMemoryFromGPU`, GC levels), `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp` (`DriverWrite`) | bbport `8185b88` (`gpu/render/raster_signals.cpp`, `raster_compute_hle.cpp`, `gpu/textures/texture_cache_track.cpp`, `texture_cache_memory.cpp`, `gpu/platform/gnm/gnm_commands.cpp`) | the whole-image condition only for WRITE_DATA (bbport: every GPU buffer write); `PS_COPY_GPU_BUFFERS` keeps its default (1 in run.sh) and bbport's forced copy of its own internal submissions is not ported |
| The launcher's GPU choice kept by its Vulkan name and passed at launch as its index (`PS_GPU_ID`, which the shell's own value overrides); a saved GPU not found leaves the choice to psport | `tools/gpu_capabilities.c` (`--gpus`, `pci_address`), `launcher/psport_launcher.py` (`connected_gpus`, `fill_gpus`, `game_environment`), `launcher/psport_i18n.py` | bbport `8185b88` (`tools/gpu_capabilities.c`, `launcher/bbport_launcher.py`) | in the Screen group; two GPUs of one name are told apart and kept by PCI address (`VK_EXT_pci_bus_info`), else by their place among them; the NVIDIA driver warning, `BB_GUEST_UNCACHED` and the GFX8 check of the same commit not ported |
| The game's 21 text languages in the launcher (was 7), by PS4 system language code | `launcher/psport_launcher.py` (`LANGUAGES`), `launcher/psport_i18n.py` | bbport `8185b88` (#249, `launcher/bbport_launcher.py`, `bbport_i18n.py`) | as is |
| The mouse the game held when the window lost focus is taken back when it returns; one let go with F1 stays free | `gpu/shim/window.cpp` (`PollEvents`), `gpu/shim/sdl_window.h` (`m_is_released_for_focus`) | SrShadowy `ba45bb5` (`gpu/shim/window.cpp`) | remembered at focus loss instead of a user-release flag; not while a menu, the name dialog or a game's pointer is up; the background joystick hint and the forced x11 driver not ported |
| A key repeated in `psport.ini` keeps its first line when the in-game menu or the launcher saves, so saving never copies it again | `gpu/port/psport_settings.cpp` (`Save`), `launcher/psport_launcher.py` (`save_ini`) | SrShadowy `f5d3dcb` (`gpu/shim/bbport_settings.cpp`, `launcher/bbport_config.py`) | psport's menu writes no key twice (SrShadowy's cause, a second `put("frame_generation")`), so only the file's own repeats are dropped |
| An HTTP error status from the community game server counts as an answer (the connection check, and `auto` not starting a local game server) | `scripts/online.py` (`check`, `local_server`) | SrShadowy `4e0ab58` (`scripts/online.py`) | its User-Agent, the forced `:20443` and The Hunter's Dream redirects not ported (#669) |
| The launcher's Python without its build configuration, IDLE and turtledemo, tkinter and the test suite, the stdlib's bytecode kept | `packaging/default.nix` (`python3`) | SrShadowy `d843170` (`packaging/default.nix`) | through nixpkgs' own `strip*` switches, with `self` pointed at the stripped interpreter so that `withPackages` builds PyGObject against it (no `buildEnv` nor `PYTHONPATH`); `__pycache__` kept (SrShadowy drops it: the read-only store would recompile the stdlib at every start); `psport-game` and `PYTHON` use it too, so the full Python leaves the closure (interpreter 137 MiB to 61 MiB, package closure 678 MiB to 602 MiB) |
| The GPU library's Windows build: magic_enum fetched and Vulkan Memory Allocator taken as a bare header where no CMake package has them (MSYS2), X11 left out, `-mno-ms-bitfields` on every target so that bit-fields keep GCC's layout, no `-ftls-model=initial-exec` or `--allow-shlib-undefined` | `gpu/CMakeLists.txt`, `build.sh` (the `MINGW*`/`MSYS*` branch), `.github/workflows/windows.yml` | bbport `72e5c8c` (`gpu/CMakeLists.txt` WIN32 branches) | built by hand for psport's tree (#614, decision #623): the library stays a DLL linked against an import library of `psport.exe` made from `gpu/port/psport_runtime.def` (bbport linked it statically into its executable), Linux-only calls in the GPU code go through `ps_host_*` (`ps_host_backtrace`, `ps_host_env_set`, `ps_host_dl_address`, `ps_host_read_memory`, `PS_HOST_RECOVERY_SET`) instead of bbport's POSIX headers over Win32, and the CI job builds it with MSYS2 CLANG64 for the user to try; `psport.exe` links there too (#805): the runtime's own sources with `src/platform/win32`, the C library calls UCRT lacks in a force-included `src/platform/win32/posix_compat.h` instead of bbport's POSIX layer, linked `--disable-high-entropy-va` and without `-mwindows` |

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
| libSceNgs2 (software audio mixer) | `gpu/ngs2/` | [AnyPS5](https://github.com/boykopovar/AnyPS5) `5edae353`, `core/libs/prx/libSceNgs2.native` | GPL-2.0-only | PS4 structure layouts (AnyPS5's are PS5's), exceptions turned into error codes at the game boundary, unimplemented effects pass audio through, decoding over the ported LibAtrac9; the mastering limiter (`Limiter.cpp`) written from firmware 11.00, not AnyPS5 |
| libSceFiber (user-level fibers) | `gpu/fiber/` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/fiber` | GPL-2.0-or-later | TCB read through GS, failures logged, `PS_FIBER_TRACE` |
| libSceFont, libSceFontFt (text over FreeType) | `gpu/font/` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/font` | GPL-2.0-or-later | guest paths through the runtime's mounts, system fonts from `PS_SYS_FONTS` else the host's Noto (or the embedded DejaVu Sans), exceptions turned into error codes, `sceFontGlyphRenderImage` and `sceFontCloseFont` implemented |
| libSceAudiodec (AAC, MP3 and ATRAC9 decoders) | `gpu/audiodec/` | [AnyPS5](https://github.com/boykopovar/AnyPS5) `a11c8f7f`, `core/libs/prx/libSceAudiodec` | GPL-2.0-only | registered by NID, exceptions turned into error codes at the game boundary, ATRAC9 over the ported LibAtrac9, FFmpeg's log level left to the process, parameters of the first decoders logged |
| NID to name table | `src/runtime/imports/import_names_all.inc` | shadPS4's aerolib list (via universal_ps4_native's `data/aerolib.csv`) | GPL-2.0-or-later | generated, sorted for binary search |
| Output arguments per NID | `src/runtime/imports/import_outparams.inc` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, prototypes and `LIB_FUNCTION` registrations in `src/core/libraries` | GPL-2.0-or-later | generated by `tools/import_outparams.py`, sorted for binary search |
| Failure convention per NID | `src/runtime/imports/import_returns.inc` | [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, prototypes and `LIB_FUNCTION` registrations in `src/core/libraries` | GPL-2.0-or-later | generated by `tools/import_returns.py`, sorted for binary search |
| ulobjmgr contracts | `src/kernel/runtime_ulobjmgr.c` | written from shadPS4's `core/libraries/ulobjmgr` | GPL-2.0-or-later | ids handed out per object |
| libSceAudio3d (ports, bed and object mix) | `src/audio/runtime_audio3d.c` | written from [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/audio3d/audio3d.cpp` | GPL-2.0-or-later | output through the runtime's AudioOut ports, mixed in float; attribute ids taken from FMOD, Koei Tecmo and CRI rather than shadPS4's enum; objects panned by position and spread over 7.1 (stereo fold-down without a 5.1+ device), passthrough to one speaker, object gain unity until set; sceAudio3dPortSetAttribute |
| libkernel memory pools (`sceKernelMemoryPool*`) | `src/kernel/runtime_mempool.c` | written from [shadPS4](https://github.com/shadps4-emu/shadPS4) `3a9f572`, `src/core/libraries/kernel/memory.cpp` | GPL-2.0-or-later | over the runtime's direct memory and mapping table; commits take the pool's free 64 KiB blocks wherever they are, decommitted memory reads as zeros again; `sceKernelMemoryPoolBatch` and `sceKernelMemoryPoolGetBlockStats` not implemented |
| libkernel exports for files, descriptors, threads, signals, event queues and sysctl (#661) | `src/kernel/runtime_file.c` (`readv`, `writev`, `preadv`, `pwritev`, `fdatasync`, `sync`, `lstat`, `fstatat`, `chdir`, `flock`, `chflags`, `fchflags`, `pipe`, `dup`, `dup2`, `runtime_file_ready`, `runtime_file_changes`), `src/net/runtime_socket.c` (`socketpair`), `src/kernel/runtime_thread.c` (`scePthreadAttrGetschedpolicy`, `scePthreadAttrGetinheritsched`), `runtime_mutex.c`, `runtime_rwlock.c`, `runtime_sema.c` (`*_setname_np`, `sem_setname`, `pthread_mutex_reltimedlock_np`), `runtime_eventflag.c` (`sceKernelOpenEventFlag`), `runtime_kernel.c` (`sysctl`, `raise`, `kill`, `sigtimedwait`, rlimits, `sceKernelGetProcessType`, `htonl`), `runtime_memory_kernel.c` (`sceKernelClearVirtualRangeName`), `src/platform/linux/host_file.c` (`ps_host_fs_pipe`, `ps_host_fs_lock`, `ps_host_fs_poll`, `ps_host_fs_sync_data`) | written from [shadPS4](https://github.com/shadps4-emu/shadPS4) `149089be7`, `06e813ff8`, `e41215e79`, `eb2fb486e`, `4355d9eb7` (the event queue's `124328bcd` is a row of [Changes to inherited shadPS4 code](#changes-to-inherited-shadps4-code)); behaviour from AnyPS5 `5b59fbc` and the firmware (kb #691), no AnyPS5 code | GPL-2.0-or-later | an unknown sysctl MIB is ENOENT, counted, instead of stopping the game; CTL_USER answered in user space as libkernel does; values the kernel decides (rlimits, `kern.rng_pseudo`, the process type, opening an absent event flag) are psport's, logged and counted; relative paths start at the working directory `chdir` sets; signals are the ones the game sends itself, pending while blocked or awaited and delivered once unblocked; a signal ending the process is counted in the call report; the user context calls (`pthread_suspend_user_context_np` and the rest) fail with ENOSYS, counted, until the runtime knows when a thread leaves its own code for the game's (stopped anywhere it may hold a host lock its suspender needs); a kqueue filter follows its descriptor's open, not its number; `dup2` takes its target over in one step and onto 0-2 is refused; BSD file flags checked and not kept |
| FSR-Vulkan (FSR 3.1 and FSR 4 v07 upscaling over Vulkan) | `gpu/third_party/fsr-vulkan/` | [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) `c64f093` | MIT | `PS_FSR4_PROFILE` (GPU time per FSR 4 pass) and `PS_FSR4_STATS` (driver statistics of each pass) in the FSR 4 v07 provider |
| RenderDoc in-application API header | `gpu/third_party/renderdoc/` | [RenderDoc](https://github.com/baldurk/renderdoc) `v1.46` (`102e541`), `renderdoc/api/app/renderdoc_app.h` | MIT | none; used by `gpu/diag/psport_renderdoc.cpp` (`PS_RENDERDOC_CAPTURE_FRAME`) |
| LibAtrac9 (ATRAC9 decoder) | `third_party/LibAtrac9/` | [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) `efca2e3`, `C/src` | MIT | `Atrac9Decode` takes the input's size and the bit reader reads zeros past it instead of past the buffer |
| Online module (network, HTTP/SSL and PSN libraries with the shadNet client) | `gpu/net/` | shadp2p (a shadPS4 fork) by way of the Linux and Windows co-op forks of bbport, inherited with bbport 0.51 (`3c57da4`, then `gpu/bbnet/`) rather than ported here | GPL-2.0-or-later | renamed `libpsport-net.so` with `psport_net_*`/`PsNet*`; the lock fixes listed under [Changes to inherited shadPS4 code](#changes-to-inherited-shadps4-code); the summon helpers act on the launched title's summon route only (`PsNetHost` `sz_summon_route`, ABI 3); a title's own part of the module started by name (`PsNetHost` `sz_extension`, `games/*/net/`): Bloodborne's BBSC (`PS_BBSC=1`); the P2P relay fallback and BBSC's signaling, summon broker and game code, ported by hand from bbport `72e5c8c` (rows below); the shadNet client's version report, its rejection and service notifications, and `sceNpLookupNpId`, ported by hand from bbport `72e5c8c` (rows below) |

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
| AvPlayer clock | `core/libraries/avplayer/avplayer_source.cpp`, `avplayer_source.h` | follows libSceAvPlayer (read with rea, kb #771): MP4 sources open with FFmpeg's `ignore_editlist`, since its parser does not subtract an edit list's media time (Atelier Ayesha's movies: video 33 ms later, its last frame past the container duration); `sceAvPlayerCurrentTime` with a video stream enabled, with or without audio, is the timestamp of the last frame handed out, not the wall clock bounded by the container duration, and 0 after a stop; frames are released by an unbounded playback clock (`PlaybackClockMillis`), so a video whose timestamps pass the container duration no longer stalls on its last frames | bug present at `3a9f572` |
| AvPlayer stopping | `core/libraries/avplayer/avplayer_source.cpp` | `Stop()` no longer rewinds the file streamer's position to 0: FFmpeg's byte stream kept its own position and buffer, so after a restart (`sceAvPlayerJumpToTime`, Unity's movies) its next refill read the file from the start as packet data (`Invalid NAL unit size`, `partial file`) and Symphonia hung at the end of its intro | bug present at `3a9f572` |
| AvPlayer end of file | `core/libraries/avplayer/avplayer_source.cpp`, `avplayer_state.*` | the demuxer reports end of file only when the source ran out, not when `Stop()` ends it: every restart (`sceAvPlayerStart` while playing, `sceAvPlayerJumpToTime`, `sceAvPlayerStop`) put the state machine in `EndOfFile`, so after a jump `Pause()` did nothing and a buffering check racing a stop logged `EndOfFile -> Buffering` (Tales of Symphonia); the controller thread moves between `Play` and `Buffering` only from the state it checked, and `Stop()` holds the source mutex exclusively, so a stop no longer reads as a buffer underrun | bug present at `3a9f572` |
| VideoOut cursor | `core/libraries/videoout/video_out.cpp`, `driver.h` | `sceVideoOutCursorEnable`/`Disable`/`SetPosition`/`SetHotSpot`/`Set2xMagnify` keep each port's cursor state (image, position, hot spot, magnification) and hand it to the overlay, which draws each enabled cursor's 64x64 image over the game frame, behind the menu (`gpu/port/psport_overlay.cpp`); the first enable is logged with how many of its pixels are visible | absent at `3a9f572` |
| VideoOut configure options | `core/libraries/videoout/video_out.cpp` | `sceVideoOutConfigureOptionsInitialize_` zero-fills the options block of the size the caller passes (no option set), as `sceVideoOutModeSetAny_` fills a mode | absent at `3a9f572` |
| VideoOut event lists | `core/libraries/videoout/driver.cpp`, `video_out.cpp` | a port's `flip_events` are added, deleted, triggered by `Flip()` and cleared by `Close()` under its `port_mutex`, and its `vblank_events` under `vo_mutex` (the lock the vblank thread already held to trigger them); `Close()` and the add and delete calls changed the vectors while the present thread walked them | bug present at `3a9f572` |
| Custom border colour without a table | `video_core/texture_cache/sampler.cpp` | a sampler with a custom border colour while `TA_BC_BASE` is 0 samples transparent black (the hardware's fetch reads an unmapped page as zeros) instead of reading the table at guest address 0 (a segfault); logged with a count | bug present at `3a9f572` |
| Synchronous image downloads | `video_core/texture_cache/texture_cache.cpp` | the buffer invalidation after a synchronous image download reads back GPU data in place (`assume_locks`) instead of queuing it to the GPU command thread: downloads run on that thread or the draw recording thread, and the queued readback never ran while the command thread drained the draw pipe (Onigiri hung after its first frame once it had a mouse) | no (bbport's draw recording thread) |
| Readbacks before whole-page overwrites | `video_core/buffer_cache/buffer_cache.*`, `renderer_vulkan/vk_rasterizer.*`, `core/libraries/videoout/driver.cpp` | a write the runtime announces as covering every byte of a range (`runtime_memory_prepare_overwrite`: the game's libc `memset`, its `memcpy` and `memmove` when the source is outside the destination, Bloodborne's resource loader copies) drops the GPU data of the whole pages inside on the GPU command thread (`BufferCache::DropOverwrittenGpuData`: GPU-modified ranges and tracker bits cleared, the pages CPU-modified) instead of downloading a 512 KiB window at each page's write fault; pages the write covers in part, pages whose GPU write is not done, the layer memory module and writes from GPU-side threads keep the readback; counted in the frame stats (`GPU data overwritten by the CPU`). Bloodborne stuttered 70-220 ms on these readbacks while opening online messages (kb #740) | no |
| Command buffer pool growth | `video_core/renderer_vulkan/vk_resource_pool.cpp` | `CommitResource` searches again with the GPU tick it has just refreshed: the search lambda had captured the tick before the refresh, so every pass through the pool overflowed and added four command buffers, and the driver's buffers behind them (~384 KiB of VRAM each time, ~2.5 MiB a minute in play, never freed) | bug present at `3a9f572` |
| Presenter | `video_core/renderer_vulkan/vk_presenter.cpp` | logs the first presented game frame once (`tools/regress.sh` reads it); publishes the shown frame's aspect ratio (`PsSettings` `shown_frame_aspect`, a menu pointer's window-to-frame mapping) | no |
| Output aspect ratios | `video_core/renderer_vulkan/ui_composition.h` (`ScreenEdges`, `EdgeParam`), `vk_rasterizer.*` (`UiPlacement`, `UiEdgeStretch`, the begin memo), `vk_temporal_upscaler.*` (the output from `PsSettings::CurrentOutput`, 16:9 inside it unless the profile follows, `pfn_display_aspect`, the black margin of a frame without a scene, a fresh FSR 4 context for another output), `shader_recompiler/backend/spirv/emit_spirv_special.cpp` (`EmitUiEdgeStretch`), `vk_pipeline_serialization.cpp` (`ShaderBinaryVersion` 12) | outputs of any aspect ratio, the UI's full-screen fills stretched to the output's edges where the game's UI follows it ([docs/en/upscaler.md](docs/en/upscaler.md#output-aspect-ratios-2026-10-17)) | no (bbport's own renderer) |
| VideoOut flip rate | `core/libraries/videoout/video_out.cpp` | `sceVideoOutSetFlipRate` keeps rate 0 while the game profile's hooks pace the frames (`Game::DrivesFrameRate`), logged once | no |
| Quirk registry | `video_core/renderer_vulkan/vk_instance.cpp` | the chosen physical device's vendor, driver id, name and driver version go to `runtime_quirks_note_gpu` before `CreateDevice` (`HOST` quirks, [docs/en/QUIRKS.md](docs/en/QUIRKS.md)) | no |
| Prepared vertex streams | `video_core/renderer_vulkan/vk_rasterizer.cpp`, `vk_rasterizer.h` | a draw-preparation worker's vertex inputs are used only while its V#s equal the ones read in order for the draw (`ArePreparedVsharpsCurrent`): the worker reads the V# tables ahead of the constant engine dumps that rewrite them, and the user data it verifies holds only the tables' address (Onigiri's login screen and PSN notice drew with another draw's vertices) | no (bbport's draw preparation) |
| AvPlayer skip | `core/libraries/avplayer/avplayer_state.*`, `avplayer_common.h`, `avplayer_source.h` | live players kept in a list; `RequestSkipAll` queues `AvEventType::Skip` to each one playing or buffering a source that does not loop (`AvPlayerSource::IsLooping`), and its controller thread stops it and logs the position, or logs and counts the request dropped when the player left playback first (the runtime's `skip_movies`, [Keyboard](#keyboard)) | no |
| AvPlayer queues | `core/libraries/avplayer/avplayer_common.h` | `AvPlayerQueue::Size()` and `Front()` take the queue's mutex, and `Pop()` checks for an empty queue under it: the decoder threads push while the game thread reads, and an unlocked `size()` or `front()` raced the push | bug present at `3a9f572` |
| AvPlayer start without a source | `core/libraries/avplayer/avplayer_state.cpp` | `Start()` fails when the source is gone instead of calling through a null `m_up_source`: `sceAvPlayerClose` resets the source before it stops the controller thread, and an AutoPlay `StateReady` handled on that thread in between still called `Start()` in the `Ready` state | bug present at `3a9f572` |
| AvPlayer stop after end of file | `core/libraries/avplayer/avplayer_state.*`, `avplayer_source.*` | follows libSceAvPlayer's `Stop`, `Start` and `JumpToTime` (read with rea, kb #771), on the path chosen by the title's compiled SDK version (before 5.50: legacy): stopping a source that ran out succeeds instead of failing with `already stopped` (its threads had ended) and reports `StateStop`; from 5.50 a second `Stop` and any later `Start` return 0 and do nothing, while a legacy `Stop` reports `StateStop` each time and `Start` plays again; `Start` on a playing, paused or finished source reports the stop before `StatePlay`; `Stop` without a source and `JumpToTime` outside play, buffering or pause fail with `OPERATION_FAILED` and no callback | bug present at `3a9f572` |
| AvPlayer end of file event and empty edits | `core/libraries/avplayer/avplayer_state.*`, `avplayer_source.*` | follows libSceAvPlayer (read with rea, kb #771, #792): reaching the end of file sets the state without an event (inherited from shadPS4, psport sent its own `StateStop`), so the game hears `StateStop` only from its own `sceAvPlayerStop` or `sceAvPlayerStart`; since FFmpeg's `ignore_editlist` drops the whole edit list, the MP4 `elst` boxes are read separately and an empty edit followed by an edit at media time 0 (the only form libSceAvPlayer honours) delays its track's timestamps by the empty edit's duration, which jumps subtract again | bug present at `3a9f572` |
| PM4 packets that asserted | `video_core/amdgpu/liverpool.cpp`, `liverpool.h`, `pm4_cmds.h`, `pm4_faults.*` (psport's), `core/libraries/gnmdriver/gnmdriver.cpp` | nothing a command buffer holds asserts any more (`pm4_faults.h`): an unknown opcode, a body shorter than the packet's fields, a `WRITE_CONST_RAM`/`DUMP_CONST_RAM` past the 48 KiB constant RAM, a `DMA_DATA` form not implemented (register destination, `dst_sel` 2, GDS to GDS) or a `WRITE_DATA` other than to memory at consecutive addresses is skipped by its declared size; a type-0 or type-1 header (Steam Deck: dwords 0, 8, 0x10 mid-game), a packet past its buffer's end, or a compute packet split at the ring's end that the next submission does not complete ends that buffer (a ring's read pointer still moves to its end); unsupported fields take the nearest safe behaviour: `SET_BASE` other than the indirect patch table ignored, `WAIT_REG_MEM` with the reserved function or a register past the file taken as met and the PFP engine on compute waiting as the ME, EOP/`RELEASE_MEM` with an unknown `data_sel` writing nothing and with any interrupt select raising the interrupt (`RELEASE_MEM` `data_sel` 0 and `int_sel` 1 included), an EOS GDS store of more than one dword copying the first, a missing CB/DB NOP size hint taking the size from the registers, `SET_*_REG` past the register file or the compute program clipped there. Each is counted by opcode, logged as `PM4: <skipped\|buffer ended\|unsupported field>, opcode <op> (<n> so far): ...` the first time and each time its count doubles (with `ReportBadPacket`'s dump for an ended buffer), and summed up after the call report as `PM4 faults: summary: ...`; an invalid ASC queue index (`sceGnmDingDong` or `sceGnmUnmapComputeQueue` with a vqid `sceGnmMapComputeQueue` did not hand out returns before touching the queues) and copy buffers out of reserved space are logged and counted too; the draw-preparation workers skip the same short packets, so that their register checksums and draw ordinals stay the GPU thread's | not checked; all asserted in the inherited tree, `RELEASE_MEM` `data_sel` 0 and `int_sel` 1 included |
| GCN instructions that asserted | `shader_recompiler/frontend/translate/scalar_flow.cpp`, `translate.cpp`, `translate.h`, `vector_memory.cpp`, `frontend/format.cpp`, `ir/passes/inverse_ballot_elimination_pass.cpp` | `S_SLEEP` is a no-op; `S_GETREG_B32` of `HW_ID` reads 0 (other registers fail the shader's translation, by name); `BUFFER_WBINVL1`/`_SC` decode and become a device-scope memory barrier; a 32-bit write to `EXEC_LO`/`EXEC_HI` replaces that half of EXEC; an `InverseBallot` of an immediate other than 0 or all ones stays for the backend instead of asserting; unhandled flow-control opcodes are named in the translation error | not checked; all five asserted in the inherited tree |
| Shaders the recompiler cannot translate | `shader_recompiler/` (every check; `translation_error.h`, `recompiler.*`, `backend/spirv/emit_spirv.*`, `emit_spirv_discard_frag.cpp`, `frontend/fetch_shader.cpp`, `info.h`, `resource.h`), `video_core/renderer_vulkan/vk_pipeline_cache.*`, `amdgpu/pixel_format.*` | the recompiler's `ASSERT`/`UNREACHABLE` checks throw `Shader::TranslationError` (`SHADER_ASSERT*`, `SHADER_UNREACHABLE*`), caught only by `TranslateProgram` and `EmitSPIRV`, which return it in a `std::expected`; the pipeline cache logs `Shader 0x<hash>: not translated: <reason>` once per permutation, does not translate it again and skips the draws and dispatches that need it, both counted in the call summary; checks reached outside those entry points log with a count and fall back (fetch shader not parsed, user data and buffer offsets not pushed, flat user data truncated, tessellation constants read as zero); the clip-distance discard shader takes all three output locations instead of asserting on a third; `MapNumberConversion` of a number and data format pair without a conversion (#306: `SnormNz`, data format 15) uses none; `Value::Hash` and the throwing helpers of `ssa_rewrite_pass.cpp` and `phi_simplification_pass.cpp` are no longer `noexcept` | assert at `b4e7ae7` |
| Buffer V#s picked at run time | `shader_recompiler/resource.h`, `profile.h`, `ir/passes/resource_discover_pass.cpp`, `resource_pass.h`, `resource_patching_pass.cpp`, `shader_info_collection_pass.cpp`, `ir/opcodes.inc`, `ir_emitter.*`, `microinstruction.cpp`, `backend/spirv/` (context, get/set), `video_core/buffer_cache/buffer_cache.*`, `buffer_cache_layer.cpp`, `renderer_vulkan/vk_rasterizer.*`, `vk_instance.h`, `vk_pipeline_cache.cpp`, `vk_pipeline_serialization.cpp` | a V# dword that is not a sharp source (a `Phi` of user data and a constant buffer read, Blood Omen 2) no longer asserts in resource discovery: the shader hands the V# it computes to a `Dynamic` binding and reaches the game's memory through the page table with the V#'s bounds; the bind prepares every V# the CPU can evaluate from the sources (resident, page table entries, GPU-modified when written); shapes it cannot follow bind a null buffer, logged with a count; `ShaderMetaVersion` 11 | asserts at `b4e7ae7` |
| Profiling zones | `video_core/renderer_vulkan/vk_draw_pipe.h`, `vk_draw_prep.cpp`, `vk_pipeline_cache.cpp`, `vk_graphics_pipeline.cpp`, `vk_compute_pipeline.cpp`, `vk_presenter.cpp`, `amdgpu/liverpool.cpp`, `core/libraries/videoout/driver.cpp` | Tracy zones (`ZoneScopedNC`) on the GPU command thread's decoding (stage A), each packet of the draw recording thread (stage B), the draw scanner and preparation workers, shader compilation, pipeline creation, presentation and the swap thread; empty unless built with `PS_TRACY=ON`, where the shim's `common/debug.h` and `tracy/Tracy.hpp` give way to shadPS4's own `common/debug.h` over Tracy's client (`gpu/third_party/tracy`, docs/en/PROFILING.md). The presenter also calls `PsRenderDoc::OnPresent` (`PS_RENDERDOC_CAPTURE_FRAME`) | no (bbport's threads); shadPS4 links Tracy and keeps its own zones |
| GCN instructions that asserted | `shader_recompiler/frontend/translate/scalar_flow.cpp`, `translate.cpp`, `translate.h`, `vector_memory.cpp`, `frontend/format.cpp`, `ir/passes/inverse_ballot_elimination_pass.cpp` | `S_SLEEP` is a no-op; `S_GETREG_B32` of `HW_ID` reads 0 (other registers still assert, by name); `BUFFER_WBINVL1`/`_SC` decode and become a device-scope memory barrier; a 32-bit write to `EXEC_LO`/`EXEC_HI` replaces that half of EXEC; an `InverseBallot` of an immediate other than 0 or all ones stays for the backend instead of asserting; unhandled flow-control opcodes are named in the assertion | not checked; all five asserted in the inherited tree |
| Command stream census and frame stats fields | `video_core/amdgpu/liverpool.cpp`, `core/libraries/gnmdriver/gnmdriver.cpp`, `core/libraries/videoout/driver.cpp`, `video_core/renderer_vulkan/vk_scheduler.cpp`, `vk_rasterizer.cpp`, `vk_gpu_profiler.*` | `PS_CP_CENSUS=1`: every graphics, constant engine and compute queue buffer is scanned (read-only) as it is processed and every `sceGnmDraw*`/`sceGnmDispatch*` call counted (`gpu/diag/psport_cp_census.*`); `PS_FRAME_STATS`: `GpuBusySampler` puts timestamps around the submissions of one frame in 32, and the `Frame stats` line gains stage B's time per draw and busy share and the GPU busy time per frame; the `Draw pipe` line gains stage B's time per packet | no |
| Parallel recording probe | `video_core/amdgpu/liverpool.cpp`, `renderer_vulkan/vk_rasterizer.cpp`, `vk_pipeline_cache.cpp`, `vk_pipeline_common.cpp`, `vk_scheduler.cpp`, `texture_cache/image.cpp`, `texture_cache.cpp`, `buffer_cache/buffer_cache.cpp`, `core/libraries/videoout/driver.cpp` | `PS_NATIVE_PROBE=1` (`gpu/diag/psport_native_probe.*`): each top-level graphics buffer starts a DCB; `DrawRecord` and `DrawIndirectRecord` are timed and classified pure or mutating by the reasons noted while they run (image or view created, image or buffer upload, arena or residency created, a layout the DCB did not leave, a submission or wait, a pipeline not in the cache, a descriptor set write, a memo miss, a new sampler); the draw packet's spare word carries the DCB and the compile from pipeline selection to the recording thread; `Frame stats` passes the sampled GPU busy time to the probe's line | no |
| Online module: socket locks | `gpu/net/shadps4/core/libraries/network/posix_sockets.cpp`, `unix_sockets.cpp`, `sockets.h`, `net_util.cpp` | accept, connect and send run without the lock close and setsockopt take; the local address is found without the network utility lock held across its TCP connect | not checked (shadp2p) |
| Online module: NP request locks | `gpu/net/shadps4/core/libraries/np/np_handler.cpp`, `np_matching2/np_matching2_mm.cpp`, `np_matching2/np_matching2_internal.cpp`, `np_web_api/np_web_api_internal.cpp`, `network/net_upnp.*` | Score and Matching2 requests are submitted under the pending reply lock; the Matching2 dispatcher and UPnP discovery stop flags are set under their waiters' mutex; the WebApi context lock is released on error returns and push event callbacks run after the global and context locks are released | not checked (shadp2p) |
| Online module: summon route | `gpu/net/shadps4/core/libraries/network/http.cpp`, `http_plan.h` (psport's), `gpu/net/psport_net_title.*` (`PsNetTitle::SummonRoute`, `SummonClient`), `gpu/games/bloodborne/net/summon_client.cpp` | the summon helpers (host placement header, `SHADPS4_BLOODBORNE_SEAMLESS_COOP` route, `SHADPS4_CAPTURE_BLOODBORNE_SUMMON` capture) are Bloodborne's summon client, moved out of `http.cpp` unchanged: `http.cpp` hands every request to the client registered for the launched title's summon route (`Game::Title` `online`, `PsNetHost` `sz_summon_route`) before its worker, before sending, once answered and when finished, and shares its request types and sender (`http_plan.h`); never another game's requests; for a title without a route, either `SHADPS4_*` switch still forces shadp2p's `/summon_messenger` (logged) | no (shadp2p matches the path for any title) |
| Online module: shadNet client version | `gpu/net/shadps4/shadnet/client.cpp`, `client.h`, `shadnet.proto`, `gpu/net/CMakeLists.txt`, `gpu/net/shadps4/core/libraries/np/np_handler.cpp`, `np_handler.h` | ported from bbport `72e5c8c`: after the server features the client reports its version (command 13 `SetClientVersionRequest`, `psport <VERSION>[-<commit>] (PS4 compatibility layer; shadPS4 network code)`, the commit only between releases, `PS_SHADNET_CLIENT_VERSION` overrides it) and the login ends with the answer, or after 15 s for a server that never answers; notification 19 `ClientRejected` during login ends it as `FailureRejected`, and `NpHandler` then stops the client, turns shadNet off for the session without reconnects and tells the player; notification 18 `ServiceUnavailable` (matchmaking full, not allowed, disabled) is logged and shown | yes (shadPS4's protocol since 2026-10-12; the version string is psport's) |
| Online module: NP lookup | `gpu/net/shadps4/core/libraries/np/np_utility.cpp`, `np_utility.h`, `gpu/net/psport_net_glue.cpp` | ported from bbport `72e5c8c`: libSceNpUtility's lookup (`sceNpLookupNpId` and its title context and async request calls) online: on shadNet an NpId is the online id in its handle, answered at once; the offline runtime keeps answering signed out | not checked (unresolved in shadp2p) |
| One memory model | `video_core/buffer_cache/` (`buffer.*`, `buffer_cache.*`, `memory_tracker.h`, `region_manager.h`), `page_manager.*`, `texture_cache/texture_cache.cpp`, `renderer_vulkan/vk_rasterizer.*`, `vk_shader_hle.cpp`, `amdgpu/liverpool.*`, `core/libraries/gnmdriver/gnmdriver.cpp`, `videoout/driver.cpp` | bbport's 0.3 memory model is gone: the GPU always uses the game's memory in place (`GuestInPlace()`, `WriteTracking()`, `Rasterizer::HonestLabels()` and their other branches removed), labels are always written once the GPU is done, the background pre-upload (`BufferCache::Preupload`, `preupload_mb` in `PS_FRAME_LOG`) and the userfaultfd page tracker (`UffdImpl`, `PS_UFFD`) are removed; a driver that cannot share the game's memory stops the launch instead of falling back | no (bbport's in-place memory) |
| Vulkan start, presentation and pipeline errors that asserted | `video_core/renderer_vulkan/vk_instance.*`, `vk_platform.*`, `vk_swapchain.*`, `vk_presenter.*`, `vk_scheduler.*`, `vk_semaphore.cpp`, `vk_resource_pool.*`, `vk_runtime.cpp`, `vk_compute_pipeline.cpp`, `vk_graphics_pipeline.cpp`, `vk_pipeline_common.cpp`, `vk_pipeline_serialization.cpp`, `vk_shader_hle.cpp`, `vk_shader_util.*`, `vk_staging_buffer_pool.cpp`, `vk_constant_ring.h`, `host_passes/fsr_pass.*`, `pp_pass.*`, `texture_cache/tile_manager.cpp`, `blit_helper.cpp`, `core/libraries/gnmdriver/gnmdriver.*` | a failure to create the instance, device (missing extension or feature), allocator, surface, swapchain, frame images, command pools, master semaphores or the layouts and pipelines built at start makes `Presenter::Create` return null and `psport_gpu_init` fail; a physical device index out of range picks the best device; at run time a failed acquire or present recreates the swapchain and skips the frame, a pipeline the driver rejects is left null, a descriptor set that cannot be allocated is not bound, a command pool that cannot grow reuses its oldest buffer, a copy shader with unexpected strides runs as a shader, mismatched image copies and sample count changes are skipped, a pipeline listed twice in the cache keeps the first; `Check` logs and returns a null result; a lost device ends the port with `STOP: GPU device lost` and exit code 24 | not checked; all asserted in the inherited tree |
| Image and sampler creation failures that asserted | `video_core/texture_cache/image.*`, `image_view.cpp`, `texture_cache.*`, `sampler.cpp`, `renderer_vulkan/vk_runtime.cpp`, `host_passes/fsr_pass.*` | `UniqueImage::Create` returns false instead of asserting and retries without `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT`; a texture cache image evicts unused images and retries first, and when it still has no memory its views are null and the runtime skips its uploads, downloads, copies, clears, resolves, barriers and sample count changes; a failed sampler is replaced by a default sampler made at start; FSR skips a frame whose images cannot be made | not checked; all asserted in the inherited tree |
| Pipeline cache, rasterizer and buffer cache errors that asserted | `common/serdes.h`, `shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp`, `video_core/cache_storage.cpp`, `renderer_vulkan/vk_pipeline_cache.cpp`, `vk_pipeline_serialization.cpp`, `vk_compute_pipeline.cpp`, `vk_graphics_pipeline.cpp`, `vk_rasterizer.*`, `vk_instance.*`, `vk_object_motion.*`, `vk_scene_resolution.cpp`, `buffer_cache/buffer_cache.cpp`, `amdgpu/pixel_format.h` | a pipeline cache entry that is cut short, holds damaged SPIR-V or is rejected by the driver marks its `Serialization::Archive` corrupt instead of throwing `CorruptData` (removed), as does a walker cut short or larger than the room left in the walker arena (never registered as code) and a stage resource mask with characters other than `0` and `1` (parsed by hand, `std::bitset` threw), and `WarmUp` rebuilds the cache as before; a cache directory that cannot be listed preloads nothing; a pipeline left null is not returned, so its draws and dispatches are skipped; a draw whose render target has no memory, with more vertex attributes than a pipeline specializes on (without vertex input dynamic state), with an indirect argument stride other than the command's size, or whose draw pipe packet is malformed or over a quarter of the ring is skipped (a task that large runs in place), as is a draw left with a null attachment its pipeline declares (a scene proxy or motion image without memory) where `VK_EXT_dynamic_rendering_unused_attachments` is missing (enabled where the device has it); an unaligned fill is skipped; a vertex stream outside the merged ranges and an unknown buffer type are bound null; an unknown tessellation topology draws triangles, a reserved component swizzle reads zero, a primitive restart index other than all ones uses all ones; a storage image also bound as render target is drawn in the general layout (the feedback loop layout does not cover storage), a depth target with several levels and a constant mip index on several bindings go on as found; a failed pipeline cache object leaves pipelines without one; a sparse block size is rounded to a power of two and a buffer request over more than two arena pages, which no arena covers, gets no arena (a readback that large is skipped; bindings have 32-bit sizes and span two pages at most); a texel buffer over part of an image or over an image without memory is not copied from it; a lost device while waiting for the GPU or binding guest memory stops the port (`StopOnDeviceLost`); all logged with `LOG_ERROR_COUNTED` | not checked; all asserted or threw in the inherited tree |
| GPU buffer creation failures that asserted | `video_core/buffer_cache/buffer.*`, `buffer_cache.*`, `buffer_cache_layer.cpp`, `fault_manager.cpp`, `texture_cache/texture_cache.cpp`, `tile_manager.cpp`, `renderer_vulkan/vk_runtime.cpp`, `vk_rasterizer.*`, `vk_staging_buffer_pool.*`, `vk_shader_hle.cpp`, `vk_occlusion.*`, `vk_timestamps.cpp`, `vk_indirect_guard.*`, `vk_camera_motion.cpp`, `amdgpu/liverpool.*` | `UniqueBuffer::Create` returns false instead of asserting: a buffer VMA cannot allocate within the budget is tried again without `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT` (the texture collector evicting by submissions meanwhile; mirrors and staging free retired mirrors or idle staging first, compaction never exceeds it), and one that still fails or has device address 0 is left null (`Buffer::IsAllocated`), as is a guest memory chunk buffer that cannot be bound; `BufferDeviceAddress` returns 0 instead of asserting and `StreamBuffer::Copy` returns nullopt instead of asserting on a failed map; the runtime skips copies, fills, updates and image transfers on a null buffer; the rasterizer skips a draw or dispatch binding one (GDS, page table, fault buffer, a failed user data, clip plane, LDS or paged record copy) an indexed draw whose null index buffer and a capturing draw whose stream-out counters have none, and an indirect dispatch whose guard has none, skipped dispatches resetting their bindings as draws do; a mirror that cannot be allocated (after freeing retired mirrors) leaves its blocks in place, a compaction whose parts cannot be allocated keeps the mirror, a chunk buffer leaves its ranges to the arena and is created again once a second (as the paged trash buffer), a demotion whose copy back has no chunk buffer keeps the blocks in VRAM and retries at the next submission, the stream path falls back to the arena; the staging pool frees idle ring blocks and dedicated buffers and retries, then waits for the GPU on a ring block (never inside the submit callback or a pending operation), else returns a null buffer that detiling, tiling, image downloads, readbacks and uploads skip (uploads and readbacks mark their ranges CPU- or GPU-modified again, an image left undetiled stays dirty), while page table entries go in the command buffer; occlusion queries and GPU timestamps without buffers are written by the CPU (everything visible, one counter sequence with the untranslated path and noted as late command writes, the CPU clock); an arena without device address stops the launch as `PsGuestMemory::Unsupported`; all logged with `LOG_ERROR_COUNTED` | not checked; all asserted in the inherited tree |
| Guest values and texture operations that asserted | `video_core/renderer_vulkan/liverpool_to_vk.cpp`, `liverpool_to_vk.h`, `amdgpu/tiling.cpp`, `texture_cache/blit_helper.*`, `image.cpp`, `image_info.cpp`, `image_view.cpp`, `texture_cache.cpp`, `tile.h`, `tile_manager.cpp` | an unknown format, tile, macro tile, array or pipe mode, sample count, image type or render-state enum from the game takes a fallback (the same data format with another number format, D32 for depth, linear general tiling and no detiling, 1x4_16 macro tiling, `e1`, `e2D`, the neutral blend, stencil, compare and sampler states) logged by `LOG_ERROR_COUNTED` (1st, 2nd, 4th... occurrence with its count); `ArrayLinearGeneral` textures are sized as linear aligned; a download larger than the image's guest memory and a blit or tiling pass whose views or pipeline failed are skipped, and an unresolvable overlap at one address gets a new image (as a tile mode change does), instead of stopping the game; an image view that fails is bound as a null view (`nullDescriptor` is required) | not checked; all asserted in the inherited tree |
| GNM driver and VideoOut calls that asserted | `core/libraries/gnmdriver/gnmdriver.cpp`, `videoout/video_out.cpp`, `driver.cpp`, `buffer.h`, `core/platform.h` | nothing a game passes to these calls asserts any more, each case logged with its count (`LOG_ERROR_COUNTED`, listed in the call summary): `sceGnmDrawIndexMultiInstanced` takes its real parameters and, as the base `libSceGnmDriver` does, checks them (13 dwords, 2-byte aligned index and 256-byte aligned object id addresses; -1 otherwise) and covers the 13 dwords with a NOP; the Neo module draws there (opcode 0x3A, not handled), so in Neo mode the dropped draw is a `(STUBBED)` line counted in the call report; `sceGnmInsertWaitFlipDone` on a VideoOut handle without buffer labels fills its 7 dwords with a NOP and returns -1; `sceVideoOutOpen` on a bus other than the main output returns `ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE`; an EOP flip of a buffer index past the 16 display buffers returns `ORBIS_VIDEO_OUT_ERROR_INVALID_INDEX`, an out-of-order flip label is logged and the flip still goes ahead, a flip the queue refuses is logged as dropped; a flip of an unregistered buffer shows a blank frame; an unknown pixel format is named `Unknown` in the log (the presenter counts it when it maps it); the IRQ controller ignores an IRQ number past `GfxEop` (registered, unregistered or signalled) and keeps the first handler when the same owner registers an IRQ twice, as a kqueue keeps one event per ident; a handler reached by another IRQ returns | not checked; all asserted in the inherited tree |
| AvPlayer and Videodec2 media that asserted | `core/libraries/avplayer/avplayer_source.*`, `videodec/videodec2.cpp`, `videodec2_impl.cpp`, `video_utils.*` | nothing a movie or a game's buffers bring asserts any more, each case logged with its count (`LOG_ERROR_COUNTED`, listed in the call summary): a stream of a media type AvPlayer has no details for keeps type `Unknown` in its stream info; a game allocator that returns no video or audio frame buffer makes `sceAvPlayerStart` fail with `ORBIS_AVPLAYER_ERROR_OPERATION_FAILED`, as the other start failures do; a decoded video frame that is not NV12 or does not fit its frame buffer (`CopyNV12Data` returns false instead of asserting), an audio frame that is not S16 or holds more than 1024 samples or its buffer's bytes, and an audio frame FFmpeg cannot convert to S16 (dereferenced as null before) stop that decoder and put the player in its error state, as a frame the decoder cannot return already did; `sceVideodec2GetPictureInfo` with a picture count above 1 copies the first picture info, leaves the second as passed and returns `ORBIS_OK`, a `(STUBBED)` line, since the decoder writes no interlaced pair | not checked; all asserted in the inherited tree |
| Layer memory module fixes from bbport 0.6-pre1 | `video_core/buffer_cache/buffer_cache_layer.cpp`, `buffer_cache.cpp`, `buffer_cache.h`, `region_manager.h`, `memory_tracker.h`, `renderer_vulkan/vk_rasterizer.*`, `vk_scheduler.h`, `gpu/port/psport_toggles.h` | ported by hand from bbport `72e5c8c` (0.6-pre1): the write-trap handler lifts its trap under `s_layer_trap_mutex` (unprotected after it, it could undo a trap `LayerProcessTraps` had just set again, and the CPU's next writes reached no VRAM copy); the first and last block a GPU-written range covers in part go to VRAM with it (`layer_edge_blocks`, the GPU's bytes copied back when the CPU writes beside them) instead of sending the whole range in place (#149); blocks the loaders rewrite whole leave `dynamic_blocks` for `layer_cpu_blocks` (`PS_LAYER_RELOAD_RESET=0`: off) and an unmap clears every mark of the blocks it removes whole (`PS_LAYER_UNMAP_RESET=0`: off), so FPS no longer falls over a long session (`Layer memory: held in place` every 10 s); promotion and `LayerMirrored` memos hashed by range and by block and looked at again each second; thread-local memo tables on the heap (static TLS kept small-stack game threads from starting); a read trap hit by the draw recording thread before its own GPU writes are submitted reads the page as it is (`layer_read_trap_stale`) instead of waiting for work it cannot submit (`BufferCache::TrapCaller`); `residency_alloc` counted down when mirrors are merged or compacted; reading bindings resolved before are reused while nothing moved and no page became CPU-modified (`BindMemo`, `g_cpu_change_epoch` in the dirty tracker, `PS_BIND_MEMO=0`: off); `PS_LAYER_VERIFY=1` (diagnostics). Not ported: the 0.3 model's VRAM-copy eviction (#142), bbport's `gpu/memory/` tracker, `BB_LAYER_PROBE`, Windows host import, the module as default on AMD | no (bbport's memory module) |
| Fault contexts | `common/signal_context.cpp`, `video_core/page_manager.cpp`, `buffer_cache/buffer_cache.cpp`, `buffer_cache_layer.cpp` | a fault handler's `void *` context is psport's `PsHostFaultContext` (`src/platform/ps_host.h`, decision #540): `Common::GetRip` and `Common::IsWriteError` read it through `ps_host_fault_register` and `ps_host_fault_access` instead of each OS's `ucontext_t` or `EXCEPTION_POINTERS`, and the page tracker's and traps' reads of RIP, RSP and RBP and of the stack go through `ps_host_fault_register` and `runtime_memory_read_guest_signal` (`ps_host_read_memory`, else the runtime's backing view for guest memory `process_vm_readv` cannot read, as direct memory's dma-buf mappings, only when the mapping table's lock is free without waiting) | upstream reads the host's context per OS in `signal_context.cpp` |
| Guest memory calls | `video_core/page_manager.cpp`, `buffer_cache/buffer_cache.cpp` | the trap owner table and the VRAM-copy traps psport adds map and protect memory through `ps_host_vm_map` and `ps_host_vm_protect` (`src/platform/ps_host.h`, decision #540) instead of `mmap` and `mprotect` | upstream maps and protects through `Core::AddressSpace` per OS |
| Texture substitution | `video_core/texture_cache/texture_substitutes.*`, `texture_cache.*` (`RefreshImage`, `Substitutes`), `image.h` (`p_substitute`, `is_substitute_rescanned`), `renderer_vulkan/vk_rasterizer.cpp` (`BindResources`, `BindTextures`, `BindTexturesFromSet`, `TexturesBindableOnHelper`) | a profile's pictures sampled instead of guest textures it knows by their bytes (identified at upload by bytes, shape and VkFormat, rescanned at bind for small shapes); such bindings are never memoized in a texture set, a set whose image comes to be replaced misses, and the texture helper leaves those images to the recording thread | no (bbport's own renderer, `72e5c8c`) |
| Small texture inventory | `video_core/texture_cache/texture_cache.cpp` (`RefreshImage`), `texture_substitutes.cpp` (`Replaces`), `image.h` (`i_small_texture`) | `PS_DUMP_SMALL_TEXTURES` (`gpu/diag/psport_small_textures.*`, [Diagnostics](#diagnostics)): each upload of a small or atlas-shaped texture is noted after it is recorded, a new one read back through a blit, and each binding the substitution checks counts for its entry | no |
| Texture collector backoff | `video_core/texture_cache/texture_cache.*` (`GarbageCollectImages`, `IsCollectorBackedOff`, `BackOffCollector`), `core/libraries/videoout/driver.cpp`, `gpu/port/psport_toggles.h` | the driver's memory usage (and, on integrated GPUs or with `PS_GC_BUDGET_MB`, its budget) is queried once a frame or second instead of at every submission; a pass over the trigger that frees nothing (everything old enough GPU-written and tiled, or in use) skips the next passes for 32 submissions, doubling up to 1024 and at most 2 s, ending sooner when the usage grows by 64 MiB or VRAM runs short (`Texture cache: collector backs off ...` / `... resumes (<reason>) ...`, each at most every 5 s); the memory line counts passes, skipped passes and backoffs (#479) | no |
| Shader specialization per draw | `video_core/renderer_vulkan/vk_pipeline_cache.*` (`BuildSpecKey`, `Program::last_used_key`), `core/libraries/videoout/driver.cpp`, `gpu/port/psport_toggles.h` | `GetProgram` compares the inputs a `StageSpecialization` reads (runtime info, start bindings, paged buffer setting, program base, user data, flattened user data, fetch shader code and vertex buffer V#s) with those of the last lookup that chose the program's last-used permutation, and reuses it when equal instead of building and comparing a specialization every draw; tessellation stages and unparsed fetch shaders always build one; `PS_FRAME_STATS` prints both counts | no |
| Debug names of the port's images | `video_core/texture_cache/image.*` (`NameBacking`), `texture_cache.cpp` (`CreateImage`), `renderer_vulkan/vk_temporal_upscaler.cpp`, `vk_object_motion.cpp`, `vk_scene_resolution.cpp`, `fsr411/fsr411.cpp`, `texture_cache/texture_substitutes.cpp` | every image the port makes outside the texture cache carries a `psport: ...` Vulkan debug name (upscaler inputs, output, TAA history, UI color and depth, display buffers, object motion, scene proxies, substitute pictures and copies, FSR 4.1.1's images), and a texture cache image allocated on `CreateImage`'s retry is named as one allocated at once, so validation messages say which image they mean | no |
| Control flow of shaders that stopped the GPU thread | `shader_recompiler/frontend/control_flow_graph.cpp` (`MakeCondition`), `structured_control_flow.cpp` (`GotoPass::BuildTree`, `TryFindForwardBlock`), `ir/passes/phi_simplification_pass.cpp` (`IsDisallowedOp`) | a block whose last instruction is a `V_CMPX*` (the next one is a branch target) falls through unconditionally instead of taking an `Execnz` condition with no false branch: the goto pass looked that label up with `unordered_map::at`, and the `std::out_of_range` stopped the GPU command thread (Blood Omen 2's PS2 emulator, a 5764-dword compute shader at its first dispatch); a branch to a block outside the CFG fails the shader's translation instead; the merge block of an `if`, loop or break is no longer looked for past a following break or return (two breaks in a row made the block after them branch twice to the same target); a Phi's common operation is not folded when its operand names a register or an attribute (a Phi of type `ScalarReg` that `LowerPhisToRegsPass` cannot lower) | bug present at `b4e7ae7` |
| Debug mode | `video_core/renderer_vulkan/vk_platform.cpp` (`DebugUtilsCallback`), `vk_instance.cpp` (`CollectDeviceParameters`) | in debug mode (`PS_DEBUG=1`), every validation message also goes to the run folder's `validation.txt`, and the GPU and driver line to its `config.txt` | no |
| Cache folder | `common/path_util.cpp` (`UserPaths`) | `PS_CACHE_DIR`, when set, roots `ShaderDir` and `CacheDir` instead of the user folder, so the caches can sit on another disk than the saves | no |
| Event queue I/O filters | `core/libraries/kernel/equeue.cpp`, `equeue.h` | ported from upstream `124328bcd` (#5192): `EVFILT_READ`, `EVFILT_WRITE` and `EVFILT_VNODE` through kevent and `sceKernelAdd/DeleteRead/Write/FileEvent`, `sceKernelGetEventFflags`, `sceKernelGetEventError`; readiness from psport's descriptor table (`runtime_file_serial`, `runtime_file_ready`, `runtime_file_changes`) instead of shadPS4's handle table and `Core::Net`: regular files as FreeBSD's vnode filters, pipes and sockets by poll, `NOTE_LOWAT`, vnode notes from the file's changes since the last look (upstream reports the asked notes at once), events of a closed descriptor dropped even when its number is open again, `EBADF` at add; read and write filters without the implied clear; `sceKernelGetEventError` reads an `EV_ERROR` event's data (upstream: any event's fflags); getters without assertions | after the vendored `b4e7ae7` |
| Start without HOME | `common/path_util.cpp` (`GetHomeDirectory`, `UserPaths`) | with HOME unset or empty the user folder's fallback takes the account's home from the user database (`getpwuid`) instead of building a path from a null `getenv`, which crashed `libpsport-gpu.so` while it loaded (exit 139); with neither, the portable folder is kept and `psport: neither HOME nor the user database names a home folder` says where GPU files go | bug in the inherited tree |
| Early stop of the image walk | `video_core/texture_cache/texture_cache.h` (`ForEachPage`) | the page walk compares `std::invoke_result_t` with `bool` instead of the `std::invoke_result` trait, so a `ForEachImageInRegion` callback returning true stops the walk (no caller returns a value yet; the same bug as the LRU walk's) | bug present at `3a9f572` |
| Windows build of the GPU library (#614) | `video_core/amdgpu/liverpool.*`, `pm4_selftest.cpp`, `video_core/buffer_cache/buffer_cache.cpp`, `buffer_cache_layer.cpp`, `video_core/page_manager.cpp`, `video_core/renderer_vulkan/vk_scheduler.cpp`, `vk_rasterizer.*`, `vk_pipeline_cache.cpp`, `vk_platform.cpp`, `vk_draw_pipe.h`, `vk_dlss.cpp`, `core/libraries/videoout/driver.cpp`, `common/ntapi.h`, `common/path_util.cpp`, `common/io_file.cpp` | the Linux-only calls psport added go through `ps_host_*` (`gettid`, `dladdr`, `backtrace`, `process_vm_readv`, `sysconf`, `pause`, `sigsetjmp`); the GPU thread's `getrusage` statistics stay Linux-only (zero on Windows); the GPU command processor's thread id on every OS; the DLSS folder lookup shared with Windows; `QUEUE_USER_APC_FLAGS` left to mingw-w64 when its headers declare it; `shlobj.h` in lower case; a lambda named `near` (a Windows macro) renamed; `IOFile::GetFileMapping` through `CreateFileMappingW`, which Wine has, instead of `CreateFileMapping2` | no |

## Loading the game

`psport --game <folder>` loads `eboot.bin` and every module it links straight from the game's
folder (`src/loader/loader_self.c` reads SELF and ELF files, `src/loader/loader_link.c` lays them out, relocates
and binds them), with `--system-modules`, `--hle` and `--lle` as `PS_SYS_MODULES`, `PS_HLE` and
`PS_LLE` set them. Only the permission bits of a segment's flags become its protection: some system
modules set high bits there. Imports bind to exports of the same NID, library, module and versions
of both; one that only an export of other versions would serve (none of 53 190 resolved imports in
612 modules and games does, kb #881) is logged once with both versions and counted after the call
report, and is bound to that export only with `PS_LOOSE_IMPORT_VERSIONS=1` (otherwise the host
runtime serves it, or it stays without implementation). The console's kernel does this binding and
nothing read so far shows whether it compares versions, so the tolerance stays off by default. The rest of the launch is in `psport` too, and no Python runs for it
(only `scripts/online.py`, with `PS_ONLINE=1`): the loose-file mods' view (`src/loader/loader_mods.c`), the
AppContent profile from `param.sfo`, shadPS4/GoldHEN XML patches (`src/loader/loader_patches.c`), and
through the game's title its own file check and patches (`Game::Title` `check_files` and
`add_patches`; Bloodborne's check of the 1.09 executable and intact shaders and its frame rate,
resolution and effect presets live in `gpu/games/bloodborne/`).

## Memory model

The GPU uses the game's memory where it is, on every GPU, as a PC release would, the backing chosen
by Vulkan capability (docs/en/ARCHITECTURE.md, memory model): a sparse arena where the device binds
sparse buffers and a startup test bind is fast, the layer's memory module (`gpu/layer/`, no sparse
binding) elsewhere and on NVIDIA's proprietary driver (`HOST` quirk `nvidia_slow_sparse_rebind`: its
rebinding stalls the GPU for seconds), `PS_LAYER_MEMORY=1/0` to choose by hand. The chunks are
dma-buf memory where the driver's dma-buf maps at page offsets at cached speed, host memory the GPU
imports (`VK_EXT_external_memory_host`) where it maps at offset 0 only or not at all,
`PS_GUEST_MEMORY=host` or `dmabuf` to choose (`gpu/memory/psport_guest_memory.cpp`). bbport's 0.3
model (VRAM copies of the game's memory kept by page protection, readbacks, labels written ahead of the GPU) is removed; when the driver can do
neither, the launch stops with a `Guest memory:` line saying so. `PS_PC_MODEL`, `PS_AS_0_3`,
`PS_GUEST_IN_PLACE`, `PS_PREUPLOAD` and `PS_UFFD` from an older launcher are ignored (`run.sh` logs
one line). Testing it on NVIDIA: [docs/en/NVIDIA_TEST.md](docs/en/NVIDIA_TEST.md).

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

Mouse look (`src/input/runtime_pad.c`, while the window holds the mouse) turns the game's camera
itself where the game's profile hooks it (`Game::MouseHooks` `pfn_camera_look`,
`psport_gpu_camera_look`): as far as the mouse moved, 0.0015 rad a pixel at `mouse_sensitivity` 1,
with none of the stick's dead zone or acceleration. Where the camera does not take the turn (no
hook, lock-on, menus) the mouse drives the right stick as before. Bloodborne's hook is
`gpu/games/bloodborne/mouse_look.cpp`; `PS_NATIVE_MOUSE=0` keeps the stick in every game that has
one. The window ignores the
mouse's motion for 100 ms after taking it, so the pointer's jump into relative mode does not turn
the camera.

Black Ops and Black Ops II (`gpu/games/black_ops/mouse_camera.cpp`, their `Game::Profile`) still
carry the PC games' mouse input (`CL_MouseMove`, kb #723): two int accumulators per axis that no one
writes on the console. Each pad sample adds the mouse's counts to the slot the game reads next with
an atomic add from the host (no code patched): one count a pixel at `mouse_sensitivity` 1 (the hook
takes the turn as 0.0015 rad a count), so `mouse_sensitivity` scales the counts fed and the game
applies its own sensitivity on top, `sensitivity` × `m_yaw` = 5 × 0.022 = 0.11 degrees a count by
default (Black Ops: dvar `sensitivity`; Black Ops II: the profile's `mouseSensitivity`), before its
aim assist and with none of the stick's dead zone, curve, turn rate or slowdown. 1 gives what the
PC games gave the same mouse (relative motion is its counts): at 800 DPI 0.11 degrees a count turns
360 degrees in about 4 inches; 0.5 to 2 covers most players, below 1 for mice above 1600 DPI. +x turns
right, +y looks down (`mouse_invert_y` flips it). Before it turns on, the executable's code around
the reader and the fields below is compared byte for byte (Black Ops `0x3f4625`, `0x3f5401`,
`0x3f545b`, `0x3f565b`; Black Ops II `0x1302a8`, `0x130330`, `0x1303b7`); another build keeps the
stick (`Mouse camera: ...: not the known build` in the log). Each sample checks the pointers (local
client 0 present and in the guest's memory, the slot index 0 or 1) and skips otherwise. Where
`CL_MouseMove` reads and zeroes the counts without turning by them, the sample goes to the stick:
a frame of 0 ms, the client's flags it tests first (Black Ops `+0x34 & 0x800`, `+0x180 == 0x24`;
Black Ops II `+0xc5 & 8`, `+0x230 == 0x2b`, `+0xd1 & 4`: vehicles, killcams and the like) and
Black Ops' `player_topDownCamMode` (Dead Ops Arcade) unless the client's `+0x2c` is 2. Not read:
the `+strafe` key (the mouse then strafes; no pad button binds it) and the guided-missile spin
guard (`cl_preventTvGuidedSpin`). The game shows it reads the counts by flipping its slot index;
counts left unread 100 ms after the last read it showed (or after they were added, when later:
menus, loading, a long frame) are taken back and the stick takes the mouse, with no counts added,
until the index moves again. While the camera takes the mouse the right stick gets nothing from it,
not even the smoothed tail of earlier stick motion (`runtime_pad.c` clears it). The exit summary
counts the samples fed, those that raced the game's slot flip (lost; decision #724 moves to a
detour if the night shows it matters), those sent to the stick while the game ignores the mouse,
skipped, handed to the stick, failed take-backs and half-done adds (`Mouse camera: <game>: ...`;
failures are also logged their first 8 times).

Their crosshair (`gpu/games/black_ops/crosshair.cpp`, kb #728, rea note #729) is on by default in
both games: `CG_ShouldDrawCrosshair` reads dvar `cg_drawCrosshair` (bool, default 1) each frame with
no gamepad, profile or platform gate. What hides it is the game: level scripts set the dvar to 0 for
scripted moments (Black Ops kowloon, zombietron; Black Ops II blackout, karma, nicaragua, `so_*`;
`reset_clientdvars` sets it back to 1 on connect), and `CG_Draw2D` skips it with `cg_draw2D`,
`cg_drawHUD` or `hud_drawHUD` at 0 (cutscenes) or `hud_missionFailed` on, besides pause, vehicles,
turrets, swimming, full ADS, `cg_crosshairAlpha` below 0.01, weapon states and a weapon without a
reticle. Each pad sample (`Game::MouseHooks` `pfn_filter_pad`) reads `cg_drawCrosshair`, `cg_draw2D`,
`cg_drawHUD`, `hud_drawHUD`, `hud_missionFailed` and `cg_crosshairAlpha` through the image's globals
(Black Ops `cg_drawCrosshair` at `0x27702f0`, Black
Ops II at `0x2470d98`), checking the `dvar_t` pointer and its type (`+0x1c`: bool 0 and float 1 in
Black Ops, 1 and 2 in Black Ops II) before reading the current value (`+0x28`), and logs each change
the game makes (`Crosshair: <game>: the game set ...`; `cg_drawCrosshair` and the other dvars
each their first 32, then counted). While the
keyboard and mouse are the device in use (`runtime_pad_prompt_device`) and the game shows its HUD
(`cg_draw2D`, `cg_drawHUD`, `hud_drawHUD` on, `hud_missionFailed` off), psport writes 1 over the
game's 0 at `+0x28` (the readers check neither the modified nor the latched copy), so scripted hides
lift with the keyboard but cutscenes and HUD-off moments stay. With a pad, or once the HUD goes,
psport stops holding it and writes nothing back (`stopped holding cg_drawCrosshair; the game's next
write decides`): a write of 1 by the game over psport's 1 cannot be seen, so giving back an earlier
0 could hide the crosshair where the game wants it. The byte stays 1 until the game writes it, so a
pad taken up during a scripted hide shows the crosshair until the script's next write.
`PS_FORCE_CROSSHAIR=0` keeps only the log. Before it starts, the reader at Black Ops `0x975e2`
(Black Ops II `0xa4288`) is compared byte for byte (`Crosshair: <game>: not the known build`
otherwise); the exit summary counts the samples it held, its writes and releases, the game's changes
(`cg_drawCrosshair` apart from the other dvars, each logged its first 32 times),
samples before registration, refusals (a type other than the build's), unreadable dvars and failed
writes (`Crosshair: <game>: held on by psport for ...`). If the crosshair still does not show with
`cg_drawCrosshair` at 1 and the HUD on, the cause is the game's state, its reticle materials or the
render, not the dvar.

As a stick, the mouse's speed is smoothed exponentially with the time constant
`mouse_look_smoothing_ms` (default 60): a steady speed holds the stick at the same level whatever
the frame rate, the smoothing or a hitch between samples, and a change of speed gets 63% of the way
there in one time constant. Full deflection takes 1000 / (`mouse_sensitivity` × `mouse_look_gain`)
pixels/s (`mouse_look_gain` default 1). The stick starts from `mouse_look_deadzone` (default 16 of
127, `PS_MOUSE_DEADZONE` wins) so the game's own dead zone does not swallow slow aiming. A title's default controls may set the three
(`mouse_look_*` lines in `Game::Title` `sz_default_controls`; Black Ops and Black Ops II carry
provisional values for when the stick takes over, until #457 measures their stick); the player's `psport.ini` lines win, and
values other than the defaults are logged (`Runtime: controls: mouse look ...`).

Aim assist in shooters is the game's own and does not know the device. On the stick (no native
camera) Black Ops and Black Ops II apply their slowdown, lock-on and auto-aim to the mouse as to a
pad, so the mouse feels dragged and the view jumps when aiming down sights. Through the native
camera the slowdown no longer touches the mouse (it scales only the stick's turn); the auto-aim snap
when aiming down sights remains while the game's Aim Assist option is on (Black Ops II: also Target
Assist), so turn it off in the controller options; the lock-on still pulls the view toward a target
while the left stick strafes (keyboard A/D included), which no option of the game turns off. Their layout puts the grenade (R1) on G,
which with `pad_motion=1` is also the fixed key that takes the sideways controller
(`MOTION_KEY_HOLD`, `src/input/runtime_pad_motion.c`): a grenade then also hands the mouse to the
sensors and stops the camera turning, so keep `pad_motion` off for them or bind `key.r1` elsewhere.

A profile may also give the mouse a pointer in the game's own menus (`Game::MouseHooks`
`pfn_wants_pointer`, `pfn_window_event`, `pfn_filter_pad`): while it says a menu is open the window
lets the mouse go and shows the cursor, its events go to the profile first, and the presses it
makes reach the game through the pad state (`psport_gpu_filter_pad`). Bloodborne's
(`gpu/games/bloodborne/menu_pointer.cpp`) hooks its Scaleform list update: hovering selects, the
left button confirms (or moves a slider), the right one goes back, the wheel moves up and down
(`Menu pointer: ...` in the log; `PS_MENU_POINTER=0` turns it off, `PS_MENU_POINTER_FILE=<file>`
with `x y [click|right|up|down]` in stage pixels drives it in tests).

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

The keyboard-as-pad's layout (`src/input/runtime_pad.c`) is the neutral one in that file's header
unless the running title brings its own (`Game::Title` `sz_default_controls`, `psport.ini` lines
read through `psport_gpu_title_controls`; Bloodborne's is close to Dark Souls III on PC, Black Ops
and Black Ops II share the PC games' layout, `gpu/games/black_ops/title.cpp`); the player's
`key.<input>=` and `pad.<input>=` lines in `psport.ini` win over both. A key may be a chord,
`key.r2=Left Shift+Mouse Left`: while its modifier is held the key's plain bindings do not count.
The move and look keys drive their stick as a thumb would (`key_stick` in `runtime_pad.c`): two
opposite keys held give the one pressed last (`key_stick_opposite=last`, the default, or `neutral`,
where they cancel), and the move stick keeps its direction `key_stick_hold_ms` (default 60, 0 to
200) after its keys are let go, so a change of direction is never a stop that ends a sprint.
`key.menu=` (keyboard keys, a chord allowed) and `pad.menu=` (gamepad buttons, each may be a
two-button chord, `pad.menu=back,leftshoulder+rightshoulder`) open and close the overlay menu like
Insert and L3+R3, which keep working; both are unbound unless set, logged when read, and listed as
`port_menu_binding` by `--list-features`. While a menu binding is held its keys and buttons reach
no other input, so it wins over the game's use of them; the built-in Insert and L3+R3 do not.
With `skip_movies=1` (or `PS_SKIP_MOVIES=1`, which wins), a press of `key.skip_movie` (Escape) or
`pad.skip_movie` (`misc1`: Share, Capture or the microphone button; a two-button chord allowed)
asks every AvPlayer that is playing to stop, each on its own controller thread, and gives the
title's profile its own skip (`pfn_skip_scene`; Bloodborne presses Options once when the press
stopped a movie while its input is blocked, in-engine cutscenes are left to the game's Options).
Only movies playing or buffering and not looping are stopped (paused, not yet started and looping
backgrounds are not); only a press that asked one to stop is kept from the game until released, and
a key already down when the overlay menu or a text dialog lets go is no press. Each press, each
stop (`psport: movie skipped at <ms> ms (player <p>, skip <n>)`), each request dropped because the
player left playback first (`psport: movie skip dropped`) and the totals at exit are logged; off,
the binding is cleared.
A gamepad with motion sensors (DualShock 4, DualSense, Switch Pro, the Steam Deck when SDL reaches its
own device) has them enabled when it is opened, again after each reconnection, and read at every pad
read (`src/input/runtime_pad_sensors.c`): SDL's gamepad sensor frame is the DualShock 4's, so the
axes pass through with the acceleration in g (libScePad does not swap them for the DualShock 4), and
the orientation is the angular velocity integrated from identity, its tilt pulled towards gravity only
with `scePadSetTiltCorrectionState` (off by default, within 0.08 g of 1 g, at libScePad's gain); yaw
drifts until `scePadResetOrientation`, which sets identity. `scePadSetMotionSensorState(0)` reports the pad at rest and
`scePadSetAngularVelocityDeadbandState` zeroes turns under 1 degree/s. The log says once per pad
what it found (`Runtime: gamepad sensors: gyro+accel (used)`, or `none: motion emulated` / `at
rest`), failed reads are logged at each power of two and counted at exit. Real sensors win over the
emulation below; `PS_PAD_SENSORS=0` leaves them off.
Games that read the sensors get them from controllers without any with `pad_motion=1` in `psport.ini`
or `PS_PAD_MOTION=1` (`src/input/runtime_pad_motion.c`, keys and pad combinations in its header):
the controller held sideways and aimed with the mouse or the right stick, shaken, and touchpad swipes;
off, the pad lies flat (1 g up, no turn).
`runtime_pad_prompt_device`, `runtime_pad_prompt_generation` and `runtime_pad_prompt` tell button
prompts which device was used last (a PlayStation pad, another gamepad, or the keyboard and mouse)
and the short name of what an input is bound to there (`Shift`, `Esc`, `⇧Left`, `RB`, `A`).

libSceIme's on-screen keyboard (`src/system/runtime_ime.c`) is the same text entry as the IME dialog: while
the game has it open, the game window takes the host keyboard's typing, the overlay draws the
panel with the text under an on-screen keyboard typed with the controller (`gpu/port/psport_keyboard.cpp`:
the left stick or d-pad moves, Cross presses the key, L2 shifts, R2 switches to the symbols, Options
accepts, Circle erases and on an empty field closes), and only what the player types is held to the
panel's type and length; the game's own text, at open or from `sceImeSetText`, is kept as given and
a rewrite leaves the keyboard where the player had it. `sceImeUpdate` hands the game the open event (the panel's place and size), each
change of the text (also written to the game's buffer) and Enter or Esc as the panel's enter or close
button; the caret stays at the end of the text and no input method composes. Without a window the
panel is entered at once with `PS_IME_TEXT`, held to the panel's type and length, or the text the
game opened it with.

libSceMsgDialog (`src/system/runtime_services.c`) runs as on the console: from `sceMsgDialogOpen`
until the player answers, `sceMsgDialogUpdateStatus` reports RUNNING (2), and the overlay draws the
message (the game's text, or the system message's number) with its buttons: Left and Right select,
Cross or Enter chooses, Circle or Esc backs out of any dialog with buttons (`USER_CANCELED`; for a
game built with SDK 3.00 or later, backing out of OK, yes/no or a system message other than types 3
and 5 reports its OK or No button, as libSceMsgDialog does). Then it reports FINISHED (3) and
`sceMsgDialogGetResult` hands back the mode, the result and the button. Open checks the parameters
before the dialog's state, with libSceMsgDialog's codes and order (kb #818): `ARG_NULL`, then
`PARAM_INVALID` (0x80B8000A) for the base parameters, the size, the mode's parameters and reserved
bytes, then `NOT_INITIALIZED`, then `INVALID_STATE` while one runs. `sceMsgDialogClose` and
`sceMsgDialogTerminate` end a running one with no button, and Close fails with `NOT_RUNNING`
otherwise. When the overlay failed to start, the dialog counts as having no window. Progress bars and the `NONE` and `WAIT`
types have no button and stay up until the game closes them. Without a window, after 1000 ms, or
after `PS_MSGDIALOG_AUTO_MS` milliseconds when it is set (`tools/regress.sh` sets 1000), the default
button is chosen and logged (`Runtime: MsgDialog auto-answered ...`); the exit summary counts the
dialogs opened, answered, auto-answered, closed by the game and refused.

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
| Frame rate (`PS_FPS`) and vblank (`PS_VBLANK_HZ`) | a title without presets: unpatched, 60 Hz vblank (the console's) | the frame rate presets are patches to one executable, its title's (`Game::Title::add_patches`); at 480 Hz a title pacing itself by its flip rate ran 8 times too fast | `uncap` (480 Hz vblank, frames shown at once), 60 or 90 (`gpu/games/bloodborne/patches.cpp`, which sets `PS_VBLANK_HZ` for the preset), by hooks in its code (`frame_rate.cpp`: its pacer waits for the preset's shortest frame, or `PS_FPS_LIMIT`'s, and the main loop and 48 places tied to a 30 FPS frame use the frame's real time; flips shown as they come, `Game::Profile::drives_frame_rate`) when `PS_FRAME_HOOKS` is unset or 1, the profile and the guest hooks are on and the executable is 1.09; otherwise the community patches (`Uncap FPS++`, `60 FPS++`, `90 FPS++`, `Sprint Fix (High FPS)`). The log says which: `Patches: FPS preset uncap by hooks` or `by patches (<why>)`, then `Frame rate: free (hooks, no patches) ...` |
| Scaled output (`output_res`) and `PS_DMEM_MB` 9152 | only for a title with presets (run.sh asks `psport --has-presets`); otherwise its own size and the console's 5056 MiB of direct memory | they are a patch to the same executable, and the larger direct memory exists for its larger targets | as `output_res` and `live_resolution` choose |
| Output aspect ratio (`aspect_ratio`) | 16:9; a game whose profile does not follow another one (`Game::Profile::pfn_display_aspect`) keeps its 16:9 picture in the middle of the output | the console's frame; only a profile's hooks can widen a game's camera and move its UI | follows the output: camera, HUD markers, HUD at the edges, UI movies ([docs/en/upscaler.md](docs/en/upscaler.md#output-aspect-ratios-2026-10-17)) |
| Memory model | the PC one on every GPU, the only one (`PS_PC_MODEL`, `PS_AS_0_3`, `PS_GUEST_IN_PLACE`, `PS_PREUPLOAD` and `PS_UFFD` are ignored with one line from run.sh); the game's memory in a sparse arena where the device binds sparse buffers fast (startup test bind), else and on NVIDIA's proprietary driver (`HOST` quirk `nvidia_slow_sparse_rebind`) through the layer's memory module (`PS_LAYER_MEMORY`) | a property of the driver, not of the game: sparse binding of the game's memory works on RADV and stalls NVIDIA's GPU for seconds | |
| `PS_COPY_GPU_BUFFERS` | 1 (run.sh) | the GPU thread decodes a submission after the game's call returns, later than the console's command processor reads it; the copy keeps a game that refills that memory from changing what the GPU gets | |
| `PS_READBACKS` | 1 (relaxed) | the console's memory is shared: the CPU reads what the GPU wrote (vertices, compute results), and relaxed readbacks are the cheapest mode that gives it that | |
| `PS_GPU_WRITE_TWINS`, `PS_GPU_WRITE_TWINS_MAX` | 1, 65536 bytes (run.sh) | a CPU write next to small GPU outputs keeps the GPU's bytes and the CPU's apart instead of waiting for the GPU; the result is the same | |
| Draw pipeline `PS_DRAW_PIPE`, recording threads `PS_VK_RECORD_THREADS`, `PS_ASYNC_SUBMIT`, copy threads `PS_COPY_THREADS` | by the host's hardware threads: the draw pipeline from 8, 1/2/3 recording threads from 1/8/12, submissions from the recording threads, a quarter of the threads (1 to 4) for copies | they decide which host thread does the work, not what the GPU or the game sees | |
| Optimizations behind `PS_TOGGLE_FILE` (`gpu/port/psport_toggles.h`) | on (a set bit switches one off) | caches, memos and pipelining that keep every result the same; the TAA, FSR 4, camera and scene scaling bits act only where the upscaler runs; the optional TAA techniques are off | |
| `RADV_DEBUG=nohiz` | on GCN 3-5 under RADV (run.sh, `HOST` quirk `radv_gcn_nohiz`; `PS_AUTO_NOHIZ=0`: off) | a driver bug: depth compression drew black halos and lost geometry on those chips | |
| Online servers (`PS_ONLINE=1`): `PS_SHADNET_SERVER`, `PS_SHADNET_WEBAPI`, `PS_COMMUNITY_SERVER` | shadNet `play.seamlesshunt.com:31313` (Seamless Hunt; run.sh, `scripts/online.py`, the launcher), its WebAPI on port 31315 of the same host; the community server is the title's (`Game::Title` `online`) | the shadNet server is the same for every game; shadPS4's public one (`srv.shadps4.net`, the default until bbport `72e5c8c`) accepts only shadPS4's own builds since 2026-10-12, and the launcher replaces it, and The Hunter's Dream, in saved settings (`online.OLD_DEFAULTS`) | community server `http://play.seamlesshunt.com` (its title's `online`) |
| BBSC, Bloodborne's seamless co-op: `PS_BBSC`, `PS_BBSC_LOCAL_SERVER`, `SHADPS4_P2P_RELAY` | off: `PS_BBSC` unset (the launcher's switch, off), its local game server never (`PS_BBSC_LOCAL_SERVER` unset; `1` always, `auto` when the community game server does not answer), the P2P relay only while BBSC runs or `SHADPS4_P2P_RELAY` sets it | co-op without it is the game's own, which other players without BBSC share; the relay and a server on this PC change where the game's traffic goes | with `PS_BBSC=1`, BBSC's default switches (`SHADPS4_BBSC_*`, `SHADPS4_BLOODBORNE_*` in `gpu/games/bloodborne/net/core/bbsc_persistence.h`) |
| Emulated LEGO Dimensions toy pad (`PS_TOYPAD`, `toypad` in `psport.ini`) | off: libSceUsbd lists no device | a console has no USB peripheral plugged in unless the player plugs one; a game that enumerates USB devices would otherwise find a pad it never asked for ([docs/en/TOYPAD.md](docs/en/TOYPAD.md)) | |
| User name (`PS_USER_NAME`) | `Player` (`sceUserServiceGetUserName`, the online module's user, the IME dialog's entry without a window); it was Bloodborne's `Hunter` before | a neutral local account name: saves and trophies are kept by user id (1), never by name, so changing it moves nothing | |

## Diagnostics

| Variable | What it shows |
|---|---|
| `PS_PERMISSIVE=1` | imports the port lacks return 0 instead of stopping, each logged once |
| `PS_LOOSE_IMPORT_VERSIONS=1` | an import that matches a linked module's export only when library and module versions are ignored is bound to it instead of being left to the host runtime; with or without it, each such import is logged (`Loader: import <NID> (<name>) of <library> version <v>, module <module> version <v>, matches the export in <file> (library version <v>, module version <v>) only ignoring versions: bound\|left to the host runtime ...`; `... matches <n> exports of other versions ...: left to the host runtime, ambiguous` when several do, never bound) and after the call report `Loader: summary: <n> imports match an export only ignoring versions: <n> bound, <n> left to the host runtime (<n> ambiguous)` |
| `PS_QUIRKS=none,-<id>,+<id>` | quirks of the registry turned off (all but `LAUNCH`, one) or forced on, each logged; `psport --list-quirks [--game <dir>]` lists them ([docs/en/QUIRKS.md](docs/en/QUIRKS.md)) |
| `PS_HUD_ANCHOR=0`, `PS_HUD_DX=<pixels>`, `PS_RE_VIEWPORT=1` | Bloodborne on an output of another aspect ratio: its HUD's groups left in the 16:9 middle; another shift of them to the sides; the first 24 viewports its UI movies set, logged |
| `PS_FRAME_HOOKS=0` | Bloodborne's frame rate presets by the community's patches instead of its profile's hooks (comparisons) |
| `PS_FRAME_DT_SMOOTH=<a>`, `PS_CLOTH_STIFFNESS=<value>` | with Bloodborne's frame rate hooks: the step follows the measured frame time by the fraction `a` (0..1) a frame instead of at once; the cloth constraints' stiffness bound at any frame rate instead of 1.0 at 30 FPS down to 0.5 from 60 FPS |
| `PS_GAME_PROFILE=none` | the game runs without its translator profile (`gpu/games/`, `Game::Profile`), on the generic paths only; its title (`Game::Title`: launch, patches, saves, menu pages) still applies |
| `PS_PERMISSIVE_OUT=error` | with `PS_PERMISSIVE=1`, those of them that write through a pointer argument (`src/runtime/imports/import_outparams.inc`) return `0x80020016` instead of 0, so the game takes its error path rather than reading an output nothing wrote; either way their log line names the output arguments left unwritten and the pointers passed |
| (always) | before the game starts: every import without implementation or answered by a stub, and a summary |
| (always) | calls that nothing answers for real or that fail, see [Call report](#call-report) |
| `PS_CALL_REPORT=<file>` | the call report's summary lines, also written to that file when the process ends |
| `PS_DEBUG=1` (`debug=1` in `psport.ini`; `PS_DEBUG=0` overrides the file) | debug mode (decision #620, `src/runtime/runtime_debug.c`): each run gathers its logs in `<PS_DEBUG_DIR>/<host>/<YYYYmmdd-HHMMSS>-<serial>/` (`nogame` without a title ID; host: the machine's name with anything outside `[A-Za-z0-9._-]` as `_`, leading and trailing dots dropped, `unknown-host` when empty, as decanter names it). Fixed names, a file only when something went into it: `run.log` (the process's whole stdout and stderr, GPU library and children included, copied by a helper process `psport-output` that outlives it; `run.sh`'s own lines before it starts psport are not in it), `gpu.log` (the GPU library's log lines), `calls.txt` (the call report, as `PS_CALL_REPORT`), `faults.txt` (fault and watchdog reports with `PS_FAULT_DUMP`'s rows), `validation.txt` (every Vulkan validation message, whatever `PS_GPU_LOG` shows), `frame-stats.txt` (the `Frame stats`, `Stall` with its indented lines, `Draw pipe`, `Shader specializations`, `Native probe`, `Guest waits per frame` and `Guest heap` lines), `env.txt` (`PS_*`, `BB_*`, `VK_*`, `RADV_*`, `MESA_*`, `DXVK_*`), `config.txt` (version and commit, command line, `psport.ini` as read, `--list-features`, the GPU and driver) and `summary.txt` (how it ended, exit code, duration, the call summary line, the last 40 lines of output). Names holding `TOKEN`, `PASS`, `SECRET`, `KEY`, `CREDENTIAL`, `COOKIE` or `AUTH`, and values that look like tokens, are written `<redacted>`. `PS_FRAME_STATS=1` unless set (psport starts again on itself to set it, since the GPU library reads it before `main`). For a synchroniser (Syncthing): every file is written as `<name>.partial` and renamed once nothing writes it; `summary.txt` comes last, also after `kill -9` (the helper writes it then), so a folder is complete once it holds `summary.txt`. A folder that cannot be created is logged and the run goes on without the mode |
| `PS_DEBUG_DIR=<dir>` | debug mode's folder, without the host part; `<PS_DATA_DIR>/debug` by default |
| `PS_DEBUG_KEEP=<n>` | run folders of this host debug mode keeps, the current one included (default 20): older ones are removed at start, only folders named as runs (`<8 digits>-<6 digits>-<letters, digits, - or _>`) and holding nothing but files; any other folder or file stays |
| `PS_CACHE_DIR=<dir>` | the shader and pipeline caches (`shader/`, `cache/<serial>`) and, from `run.sh`, Mesa's and NVIDIA's shader caches (`mesa/`, `nvidia/`) go in that folder instead of the user folder; decanter sets it to the cache location of the game's library folder |
| `PS_FAULT_DUMP=<range>[,<range>...]` | guest memory in the fault report, after its first line, as hex rows of 16 bytes (`src/main/signals.c`). A range is `<address>:<length>`, or `*<address>[/4\|/8][+<offset>\|-<offset>...]:<length>`: read the u64 (`/4`: u32, zero-extended) at the address, add the offsets and dump from there, to follow a saved pointer (a stack pointer, an emulated CPU's register). Numbers in C syntax (`0x` hex, else decimal). Parsed once at startup: a malformed entry is logged (`Runtime: PS_FAULT_DUMP entry ... ignored: <why>`) and skipped; at most 16 ranges and 64 KiB in total. Reads go through `runtime_memory_read_guest_signal` (`ps_host_read_memory`, else the backing view for mapped guest memory such as direct memory's dma-buf mappings, unless the mapping table is being changed), so unmapped rows print as `unreadable (<n> bytes)` and a pointer that cannot be read as `pointer unreadable` |
| `PS_SUBMIT_GUARD=0` | frames overlap (`PS_SUBMIT_AHEAD`) unguarded: a submitted frame's command buffers are neither made read-only nor checksummed until the GPU retires the frame (`gpu/memory/psport_submit_guard.h`), so a game that rewrites them in flight is not caught and the overlap stays on; `=trap` or `=hash` keeps one of the two watches |
| `PS_CP_TRANSLATOR=1` | off by default (`0`: Liverpool's path unchanged, nothing recorded). Each top-level graphics buffer is also recorded as the translator's command stream (`gpu/cp/cp_stream.h`, decoder and registers of its own) and checked packet by packet against what Liverpool runs: packet sequence (address, opcode, size, indirect buffer depth), the registers each packet wrote, the VGT draw registers at each draw, draw and dispatch packets unchanged since recording, the `SET_BASE` of indirect ones. The first 16 mismatches are logged in full (`CP translator: buffer N: <kind> mismatch: ...`), then a count line each time a kind's count doubles; `CP translator: summary: ...` after the call report. Off with `PS_PM4_SELFTEST` |
| (always) | PM4 packets Liverpool skipped, buffers it ended early and unsupported fields it ran with a fallback (`PM4: ...`, by opcode, the first time and each time a count doubles); `PM4 faults: summary: skipped <n> <op>:<n> ...; buffer ended <n> ...; unsupported field <n> ...` after the call report when there was any |
| (always) | the first fault of each SRT walker table (`gpu/diag/psport_srt_faults.cpp`, up to 4 tables a shader, 64 in all): `SRT walker load faulted: <stage> shader <pgm hash>, user data s<n>:s<n+1> = <both registers, unmasked> (`computed` when the walker indexed it, `past the <n> user data registers` when out of range), table s<n>[<dword>]..., pointer\|value at dword <offset> (address <the load's effective address, rdi + displacement: a non-canonical one faults with address 0>, fault <k> of this table); <source>`, the table being the user-data SGPR followed by the dword offsets of the nested pointers the walker followed, and the source the packet the command processor thread was executing with the SET_SH_REG in its command buffer that wrote that SGPR (or why there is none: a draw preparation worker, a register not written there); after the call report `SRT walker faults: summary: <n> faults, <n> tables, <n> untracked, <n> unattributed, <n> speculative walks abandoned: <stage> <hash> <table> <faults> ...`; a fault on a thread reading ahead with a recovery point is not one of these: `SRT walker load at <code> ahead of the command processor faulted at <address>: walk abandoned, the draw is left to the GPU thread (<n> so far)` at powers of two |
| `PS_AUDIO_STATS=1` | per audio port, every 5 s: cadence, peak and RMS in dBFS (sound or silence) |
| `PS_NGS2_TRACE=1` | first calls of each Ngs2 function and the level of each render |
| `PS_FIBER_TRACE=1` | first calls of each Fiber function with their arguments |
| (always) | the first guest path without a mount under each root (`/archive`, `/etc`, ...), and after the call report how many there were per root |
| `PS_TRACE_SEMA=1` | each semaphore created and each blocking wait on one |
| `PS_TRACE_EVENTFLAG=1` | each event flag created; without it the first 8 are listed, then a count line at each power of two, and the totals after the call report |
| `PS_LAYER_VERIFY=1` | with the layer's memory module, every reading binding of a VRAM copy hashes its blocks against the game's memory once a frame; a block changed with no announced write while its write trap counts as set but pages of it are writable is logged as `Layer verify: STALE` (at most 300); slow |
| `PS_RENDERDOC_CAPTURE_FRAME=N` | with RenderDoc in the process (`renderdoccmd capture`), it captures the frame after the Nth game frame presented (`gpu/diag/psport_renderdoc.cpp`, over RenderDoc's in-application API, looked up with `dlopen(RTLD_NOLOAD)`: nothing links against it) and logs the capture's path; `PS_RENDERDOC_CAPTURE_PATH` sets RenderDoc's file path template. Set without RenderDoc loaded, it says so once. See [docs/en/PROFILING.md](docs/en/PROFILING.md) |
| `PS_TRACE_MEM=1` | each direct memory allocation, direct mapping, flexible memory map and virtual range reservation; a failed direct or flexible mapping is logged without it, and flexible maps as event flags are (the first 8, counts at powers of two, totals after the call report) |
| `PS_FRAME_STATS=1` | every 5 s, among other lines, `Frame stats: ...` whose end reads `; stage B <us> us/draw, busy <pct>%` (the draw recording thread's time per draw and its busy share of the wall clock; `; stage B off` without the draw pipe) and `; GPU busy <ms> ms/frame (<n> frames sampled, <n> not ready)` (graphics queue time of the submissions of one frame in 32, from Vulkan timestamps at the end of all earlier work at each submission's start and end, so the GPU waiting for work is left out; `; GPU busy n/a` when the graphics queue has no timestamps); `Draw pipe: ...` ends with `, stage B <us> us/packet`; `Shader specializations: <n>/frame reused by equal inputs, <n>/frame built and compared` (shader stage lookups of the pipeline cache) |
| `PS_CP_CENSUS=1` | after the call report, the command stream census of the whole run, `CP census: <section>: <key>=<value> ...`: `run` (seconds, frames shown, top-level graphics buffers and per frame), `packets` (graphics, compute, constant engine, predicated), `sync` (`SET_PREDICATION`, `COND_EXEC`, `MEM_SEMAPHORE`, `WAIT_REG_MEM` on a register, on memory a GPU packet writes (EOP, EOS, `RELEASE_MEM`, `WRITE_DATA`) or on other memory, which the CPU writes), `buffers` (indirect buffers from top-level buffers and nested deeper, deepest level, compute queue submissions, constant engine dumps a second and KB per frame), the packets by opcode of each engine, and `driver calls` (`sceGnmDraw*`/`sceGnmDispatch*`, per function; games that link Gnm statically call none) |
| `PS_NATIVE_PROBE=1` | whether stage B's draws could be recorded in parallel per DCB (decision #361, P1): with `PS_FRAME_STATS=1`, every 5 s after `Frame stats`, `Native probe: draws=<n> draws_per_frame=<x> pure=<pct>% mutating=<pct>%` and per reason (`new_image`, `new_view`, `upload`, `buffer`, `layout`, `sync`, `compile`, `descriptors`, `memo_miss`, `other`: a draw counts once, under the earliest of this list among the reasons it noted), `; dcbs=<n> dcbs_per_frame=<x> dcbs_with_draws_per_frame=<x> all_pure_dcbs=<pct>% dcb_first=<reason>:<pct>,...` (the reason of each mutating DCB's first mutating draw), `; us_per_draw=<us> pure_time=<pct>% time_base=stage_b|draws amdahl_4=<x> amdahl_8=<x>` (pure draws' share of stage B's busy time, or of all draw time without the draw pipe, and Amdahl's speedup with 4 and 8 recorders) and `; gpu_busy_ms=<ms> frame_ms=<ms> gpu_busy=<pct>%` (or `gpu_busy=n/a`); after the census, `Native probe summary:` with the same fields for the whole run and `layout_overflow`, then `Native probe summary: decision=Go|No-go|Undecided pure=<pct>% gpu_busy=<pct>% rule=pure>=90%,gpu_busy<70% main_reasons=<reason>:<pct>,...`; it times every draw, so its runs are not perf runs |
| `PS_DUMP_SMALL_TEXTURES=<dir>` (`1`: `<PS_DATA_DIR>/small_textures`, or `small_textures` in the working folder without `PS_DATA_DIR`) | an inventory of the small textures the game uploads, to see how it stores its button glyphs before a title gets substitutes (#706, `gpu/diag/psport_small_textures.*`): every texture uploaded from the game's memory whose level 0 fits `PS_DUMP_SMALL_TEXTURES_MAX` (default 256) on both sides, any format, levels or layers, and the UI-looking atlases up to `PS_DUMP_SMALL_TEXTURES_ATLAS` (default 2048, `0` none: flat 2D colour textures with an alpha channel, one level, one layer, never a render target), once per XXH3 of its guest bytes, up to `PS_DUMP_SMALL_TEXTURES_LIMIT` (default 4096, at most 65536; uploads of textures not inventoried after it are counted as `over_limit`). `<dir>/index.tsv` (rewritten every 5 s while textures come in or are bound, and at a normal exit, not after a fault or the watchdog) has a row each: `hash`, `guest_address`, `guest_size`, `level0_size` and `level0_hash` (level 0's bytes alone, what a profile hashes from the game's file when the guest size is padded), `width`, `height`, `depth`, `levels`, `layers`, `type`, `format` (host Vulkan format), `tiling`, `kind` (`small` or `atlas`), `first_frame`, `uploads`, `binds` (the bindings that sample it and the texture sets reusing it) and `png`: `png` for `<dir>/<hash>.png` (its level 0 layer 0 blitted to RGBA8 in its own encoding after the upload, read back and encoded on a thread of its own), `pending` (read back, not written yet), `failed` (encoding or writing it failed), or `no:<reason>`: `depth`, `volume`, `msaa`, `unallocated`, `integer`, `blit` (the device cannot blit the format), `memory`, `pending_cap` (more than `PS_DUMP_SMALL_TEXTURES_PENDING_MB`, default 512, of readbacks in flight; tried again at its next upload), `disk_cap` (`PS_DUMP_SMALL_TEXTURES_DISK_MB`, default 2048, of PNGs written: none after it), `rewritten` (an atlas address already read back 8 times, a texture the CPU keeps rewriting such as a font cache). After the call report: `Small textures: summary: dir=... unique=<n> small=<n> atlas=<n> png=<n> png_pending=<n> png_failed=<n> no_png=<n> png_mib=<n> over_limit=<n> uploads=<n> binds=<n>`. It reads only, but each upload of a candidate hashes its guest bytes on the recording thread (up to 16 MiB for a 2048x2048 RGBA8 atlas) and each new texture adds a blit and a readback to the stream; off, each hook is one load of a constant flag. A night step per title: `tools/regress/small-textures.plan` |

### Call report

Every import the game calls without getting real work done is logged the first time and counted:

| Line | When |
|---|---|
| `Runtime: unsupported import <NID> (<name>) ...` | first call of an import without implementation (`PS_PERMISSIVE=1`), with its first argument or the output arguments it leaves unwritten |
| `Calls: stub <NID> (<name>) returns 0 without doing anything; arguments <a0>, ..., <a4>` | first call of an import answered by the runtime's stub |
| `GPU [<class>] <<level>> <file>:<line> <function>: (STUBBED) ...` | first call of a GPU library (HLE) function that does not do its work, whatever `PS_GPU_LOG` says |
| `GPU [Render.Vulkan] <Error> vk_pipeline_cache.cpp:<line> ReportUntranslated: Shader 0x<hash>: not translated: <reason> ...` | first translation failure of a shader permutation; its draws and dispatches are skipped, and the summary counts both as `stub ... (HLE)`: `GPU shader not translated by the recompiler`, `GPU draw or dispatch skipped: shader not translated` |
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
Calls: summary: <n> unimplemented, <n> stub, <n> failed, <n> status (import and error code), <n> fallback (GPU)
Calls: summary unimplemented <calls> <NID> (<name>)
Calls: summary stub <calls> <NID> (<name>)
Calls: summary stub <calls> <function> (HLE)
Calls: summary failed <calls> <NID> (<name>) -> 0x8xxxxxxx
Calls: summary failed <calls> <NID> (<name>) -> -1, errno <n>
Calls: summary status <calls> <NID> (<name>) -> 0x8xxxxxxx
Calls: summary status <calls> <NID> (<name>) -> -1, errno <n>
Calls: summary fallback <times> <function> (GPU): <message format>
```

`fallback` lists the GPU library's errors that took a fallback (`LOG_ERROR_COUNTED`: an unknown
format, a pipeline the driver rejected, a skipped present...), by the function that logged them,
with how many times each happened.

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
frame rate in play (`bloodborne-perf.sched` from a save, `bo2-perf.sched` from a `bo2-play.sched`
run's user folder, both given with `-u`, and `ryza-perf.sched` from New Game; perf.txt says what each
seed is; run alone on the machine, `-j 1`).
With `-c`, a milestone lost or a crash signature that is new or different is a regression: it is
listed and the exit status is 1. The `no impl` and `forced` columns show their change against the
earlier run without counting it as a regression (blank in an earlier file that predates `forced` or
`no_impl_called`).

`tools/profile/perf.sh` runs one title of a titles file through `tools/regress.sh` and samples it
with perf (flame graph, functions by self time, Tracy zones with `-z`); `tools/profile/compare.sh`
does it for two builds and reports what moved. Both run games, so they belong to the night session:
[docs/en/PROFILING.md](docs/en/PROFILING.md).

`report.py` also keeps, per title, `perf` (the means of the last 10 `Frame stats` lines: FPS, stage B
us/draw and busy share, GPU busy ms/frame) and `census` (the `CP census:` block), with a table of each
when a run printed them; both are null for older runs. `--compare-shots <base folder>` compares each
present dump with the base run's dump taken for the same schedule second (found from the dump's time
since the run started; by order when the schedule is gone): PSNR over RGB and the largest channel
difference, 99 dB for identical dumps, into `shots_compare.tsv` and `shots_vs_base` in results.json;
those under `--psnr-min` (default 25 dB) are listed as `SHOT` lines but are no regression, since
animation and timing alone move a frame between two runs of the same build (13 titles: about 15 s).

`tools/regress/night.sh [-r <base regress folder>] [-u <perf seed folder>] <base checkout> <branch
checkout> <games root>` is the night order for a branch: regress of every title but octopath0 with
`PS_CP_CENSUS=1` against the base, then perf.txt with `PS_FRAME_STATS=1` on base and branch in turn,
twice, then the shot comparison, and a summary table in `summary.txt` (regressions and shots per
title; FPS of each perf run, the branch against the base, stage B and GPU busy).

`tools/regress/plan.sh [-n] [-w HH:MM-HH:MM] [-a N] <plan> <state folder>` runs a longer night as a
list of steps (`step ID DESCRIPTION -- COMMAND...` in a sourced bash file) that survives a cut: each
step's end is written to the state folder (`ID.done`), so a new start skips what finished and runs a
step left `ID.running` again in a fresh `ID.attemptN/` folder (up to `-a`, default 2), after stopping
what the cut runner left behind. A failing step does not stop the plan; `status.txt` and `plan.log`
are the morning's record. One instance at a time (`flock` on the folder's `lock`); with `-w` no step
starts outside the window, so a reboot by day does not start a game; every step runs with online
play off (`PS_ONLINE`, `PS_BBSC`, `PS_BBSC_LOCAL_SERVER`, `PS_UPNP` exported as 0, the server
variables unset; a step that sets them again is refused). `tools/regress/psport-night.service` and
`.timer` are user-unit templates that start it at the night's start and again after a boot or a new
session, with their install and uninstall commands in their headers.

## CI and binary cache

`.github/workflows/build.yml` builds `packaging/port.nix` on every push to `develop` and `main` (and by hand) and
publishes it from those two branches (below). `packaging/port.nix` is the port from source in Nix's sandbox
with `build.sh`, LTO on, no PGO profile, nixpkgs pinned in `packaging/nixpkgs.nix` to decanter's
revision.

CI builds it with `--arg withNet false`: the public binary cache, like the bundle, has no online
module (`libpsport-net.so`, which links OpenSSL and abseil, Apache-2.0) until kb #850, and its
`--list-features` reports `online_module` unavailable, so decanter hides online play for it.
`nix-build packaging/port.nix` and `build.sh` keep the module (`PS_NET=OFF` leaves it out of
`build.sh`'s). Before anything is published, `tools/check_bundle_licences.sh --port` checks the build
and every library its programs and modules link; the rest of the closure is nixpkgs' packages as they
are (SDL3 brings PipeWire's, whose own plugins and tools link OpenSSL, abseil and a GPL FFmpeg).

`.github/workflows/windows.yml` (same triggers, `contents: read`, no secrets) builds what builds on
Windows so far (decision #623): `build.sh`'s Windows branch in MSYS2's CLANG64 on `windows-latest`,
without LTO, gives `psport.exe`, `libpsport-gpu.dll`, `psport-gpu-capabilities.exe` and
`psport-windows-probe.exe`. FFmpeg is not MSYS2's (GPL-3.0-or-later) but psport's own,
`packaging/windows/ffmpeg/PKGBUILD` (kb #906): MSYS2's PKGBUILD for the same release (9.0.2, as
nixpkgs' pin) reduced to `--disable-everything --disable-autodetect --disable-network
--disable-programs --disable-avdevice --disable-avfilter --enable-shared` with the H.264, HEVC, AAC,
AAC LATM, MP3 and MJPEG decoders and parsers, the mov demuxer and the file protocol, no external
library, `license=('spdx:LGPL-2.1-or-later')`, providing and conflicting with
`mingw-w64-clang-x86_64-ffmpeg`; the tarball is checked against its sha256 and the FFmpeg release
key in `keys/pgp/`, and the build fails unless `config.h` has `CONFIG_GPL`, `CONFIG_VERSION3` and
`CONFIG_NONFREE` at 0. The job builds it with `makepkg-mingw` only when `actions/cache` has no
package for its version and PKGBUILD hash (the cache keeps the tarball too) and installs it with
`pacman -U`. `tools/win32-bundle.sh dist` makes them the test package (kb #899), and the artifact is
that zip as it is (`archive: false`), `psport-windows-<version>-<commit>-x86_64.zip`, with
`psport-windows-<version>-<commit>-x86_64-sources` beside it (FFmpeg's PKGBUILD, key and upstream
tarball); kept 14 days, nothing published. When the build fails, `psport-windows-logs-<commit>`
holds its output instead.

The package is one folder: the four programs, the CLANG64 DLLs they load (the closure of their PE
import tables, read with `llvm-objdump`; `vulkan-1.dll` is the GPU driver's), `psport-probe.bat`,
`psport-probe.ps1` and `README-WINDOWS.txt` from `packaging/windows/` (CRLF), `LICENSE.txt`,
`THIRD-PARTY-NOTICES.txt` (each DLL's MSYS2 package with its licence and licence files, the
packages compiled in, and the tree's own third-party code as in the Linux bundle), `SOURCES.txt`
(MSYS2's source package and upstream of each; for a package built from one of psport's own
PKGBUILDs, `packaging/windows/*/PKGBUILD`, the sources folder next to the zip, filled from the files
makepkg left beside the PKGBUILD after checking them against its sums) and `BUILD-INFO.txt` (commit,
DLLs in and out). Only DLLs whose package licence is GPL-2.0-compatible go in (`PS_BUNDLE_LOCAL=1`
keeps the others, and whatever only they load, for a package used on one's own machine, named
`-local`); FFmpeg's notice carries its LGPL texts and the configure line it was built with
(`share/licenses/ffmpeg-lgpl/CONFIGURATION`). Testers start `psport-probe.bat`, which runs
`psport-windows-probe.exe` (`tools/win32_probe/`: no game, Vulkan loaded at run time, only system
DLLs) and opens `psport-windows-report.txt`: the system, each Vulkan device's capabilities, the
memory sharing tests and a verdict on the guest-memory mode (decision #873); see
[docs/en/WINDOWS_TEST.md](docs/en/WINDOWS_TEST.md).

Locally, without Windows (#777): `tools/win32-cross.sh sysroot` unpacks the CLANG64 packages the
workflow installs, with their dependencies, from repo.msys2.org (checksums from its database) into
`PS_WIN32_SYSROOT` (default `~/.cache/psport/win32-sysroot`, ~2 GB); `tools/win32-cross.sh ffmpeg`
cross-builds FFmpeg from the PKGBUILD's source and configure flags, with the same checks, and
registers it in that sysroot as the package the job installs (again after each `sysroot`);
`tools/win32-cross.sh build`
builds `out/win32/gpu/libpsport-gpu.dll`, `out/win32/psport-gpu-capabilities.exe` and `out/win32/psport-windows-probe.exe` with nixpkgs'
clang and lld at CLANG64's LLVM version over that sysroot (same libc++, UCRT and DLLs); `tools/win32-cross.sh runtime` then links `out/win32/psport.exe` with
`build.sh`'s Windows flags and, when that fails, lists the files that do not compile and the symbols missing
at the link. `tools/win32-proton-run.sh EXE [ARGS...]` runs a
result under Proton with umu-run (`PS_WIN32_PREFIX`, `PS_UMU_RUN`), the CLANG64 DLLs and
`libpsport-gpu.dll` found through `WINEPATH`. `tools/win32-bundle.sh DEST` makes the test package
from `out/win32`, the sysroot's DLLs and its packages' metadata (file lists cached in its `db/files`).

The version lives in `VERSION` alone: `0-unstable` between releases, `X.Y.Z` in a release, read by
`build.sh`, `packaging/port.nix` (`version`) and `gpu/net/CMakeLists.txt` (the shadNet client's
version, with the commit only when unstable). With `X.Y.Z` there, `build.sh` stops unless
`tag-changelog/vX.Y.Z.md` exists: a release's notes are written first (terse, from the tree at that
version; `CHANGELOG.md` is the other half, by area), then `VERSION` is bumped in its own commit,
`develop` is fast-forwarded into `main`, and the annotated, signed tag `vX.Y.Z` is made on `main`
with that file as its message (`git tag -s -a vX.Y.Z -F tag-changelog/vX.Y.Z.md`).

Versions are annotated tags `vX.Y.Z`. Pushing one runs `.github/workflows/nix-cache.yml`: it checks
that the tag names `VERSION`, builds the tag the same way, publishes its closure with `tools/nix_cache.sh` as a flat Nix binary cache in
the assets of the release `nix-cache`, then creates or updates the GitHub Release of the tag with the
tag's annotation as its notes, marked latest when the tag is the highest version published. Only that
job has `contents: write`, with the workflow's `GITHUB_TOKEN`. The output is laid out as a checkout
after `build.sh` (`run.sh`, `scripts/`, `patches/`, `out/psport`, `out/psport-gpu-capabilities`,
`out/gpu/`, and in `tools/` `fetch_fsr4_assets.sh` with what `fsr4_optimize.sh` needs, since the FSR 4
v07 assets are fetched on the user's machine); the Vulkan drivers are not in it.

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
| `branch-<commit>` | marks a develop/main commit published by `build.yml`, uploaded after its `commit-` |
| `latest-release` | `<tag> <commit> <store path>` of the highest version published, one line, uploaded last |

The current release is read from
`https://github.com/yerasuu/psport/releases/download/nix-cache/latest-release`, fetched with
`nix copy --no-check-sigs --from https://github.com/yerasuu/psport/releases/download/nix-cache <store path>`,
and runs as `PS_PREBUILT=1 PS_PROBE=<path>/out/psport PS_DATA_DIR=<writable folder> bash <path>/run.sh`
with Python and the Vulkan drivers of the same nixpkgs from the caller. A tag pushed for a version
lower than the latest (a fix on an earlier line) is published without moving `latest-release`.

Every push to `develop` or `main` also publishes that commit (`.github/workflows/build.yml`): the
`build` job builds it, makes its closure content-addressed and hands it on as an artifact; the
`publish` job, the only one with `contents: write` and only on this repository, imports it and runs
`tools/nix_cache.sh <store path> <commit> - github:yerasuu/psport`, which uploads `commit-`,
`closure-` and `branch-<commit>` without touching `release-`/`latest-release`. A commit already
published (the release's commit pushed to `main`, or the same commit on both branches) keeps the
build there and only gets its `branch-` file. decanter's Testing channel fetches `commit-<commit>`
for the commit it is on and builds locally when there is none.

Each run first keeps its own commit, the release `latest-release` names, the newest branch commits up
to 20 (`NIX_CACHE_KEEP_COMMITS`) and the newest other releases up to 30 (`NIX_CACHE_KEEP`), in that
order and fewer if their paths would not fit in the 1000 files a release holds, and deletes the other
`release-`/`branch-` files, the `commit-`/`closure-` files of commits nothing kept names and the paths
no kept commit uses; then it uploads only the paths the cache lacks. A closure is about 220 paths
(440 files); a release or branch commit that changes only the port adds one path (about 9 MB), so 5
files for a branch commit and 5 for a release. Runs are serialized in the `nix-cache` concurrency
group (a run prunes what another could be publishing). `tools/nix_cache.sh <store path> <commit> <tag>
<directory>` writes the same files to a local folder, for `nix copy --from file://<directory>`.

The same job also publishes the release's port without Nix, as assets of the tag's own release:
`psport-<tag>-x86_64.tar.zst` and `psport-<tag>-x86_64.tar.zst.sha256` (`sha256sum` format), made by
`tools/bundle.sh <store path> <tag> <directory>` and uploaded after the release is created (a release
without them, as every one before this, is still fetched from the cache). It holds one folder,
`psport-<tag>-x86_64/`, laid out as the output above plus `lib/` (every library the programs and
`out/gpu/` link from the store, with the store's glibc and its loader), `relocate.sh`, and
`THIRD-PARTY-NOTICES` and `SOURCES.txt` from `packaging/licences.nix` (each library's licence and
the licence files of its source, the code compiled in, and the written source offer). Since psport is
GPL-2.0 (kb #843), nothing under the Apache-2.0 goes in: not the Vulkan loader, and not the online
module `libpsport-net.so` (OpenSSL, abseil) until kb #850 (`PS_BUNDLE_NET=1` keeps it in a bundle
for local use; `PS_PACKAGE_NET=1` does the same for the AppImage). `tools/check_bundle_licences.sh`,
run by `bundle.sh` and again by the job on the archive, fails the release when the bundle's tree
holds OpenSSL, abseil, the Vulkan loader, x264, x265, xvidcore, libssh, SRT, GnuTLS, RIST,
libunistring (LGPL-3.0-or-later in nixpkgs) or the online module, when `THIRD-PARTY-NOTICES` lists a
GPL-3.0-only library, when libavutil is missing, reports version 3 or no licence, or when LICENSE,
THIRD-PARTY-NOTICES or SOURCES.txt is missing; with `PS_BUNDLE_NET=1` it allows only the online
module, OpenSSL and abseil. `licences.nix` fails when a GPL, LGPL or MPL component has no licence
text or no source URL. It runs
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
bundle's libraries come first and the host's after them. The Vulkan loader is the host's, found in
those directories, and it finds the host's ICDs as the host's own programs do; a host with no
`libvulkan.so.1` there (NixOS keeps none in `/run/opengl-driver/lib`) can't start the bundle.
