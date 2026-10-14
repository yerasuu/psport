# Changes in 0.4 (after 0.3, since 2026-10-06)

Measurements: RX 7800 XT / RADV, Yahar'gul standing still, 1080p with no FPS cap; "4 cores" means
`CPUS=0-3,8-11` (4 cores / 8 threads, like the Steam Deck).

## PC memory model: default again, and for NVIDIA too

- `BB_GUEST_IN_PLACE=1` by default (`run.sh`). In the launcher, "New memory model" is replaced by
  the *Old memory model* toggle (key `legacy_memory_model`): the 0.2 model as a fallback.
- NVIDIA maps dma-buf only from offset 0. Previously the port fell back to the old model in that
  case; now, on such drivers, each of the game's direct memory allocations is a separate Vulkan
  memory chunk (the game makes 14 allocations and maps each one once in full, from offset 0). The
  startup check prints "the driver's dma-buf maps at offset 0 only: one chunk per direct memory
  allocation". `BB_GUEST_WHOLE_CHUNKS=1` enables this mode on other drivers too (testing without
  NVIDIA): 188–190 FPS on the RX 7800 XT, image is fine. **Not tested on NVIDIA.**

## API level: the game's libGnm

The game is built with a static libGnm: PM4 packets are written not by a driver but by the game
itself, through the `sce::Gnm::DrawCommandBuffer` / `DispatchCommandBuffer` /
`ConstantCommandBuffer` functions in `0x1068000–0x10a0000` and `0x11fd000–0x11fe000`. This is the
game's graphics API, the level at which layers like DXVK work.

- `tools/re/`: search for PM4 packets in code (`pm4_scan.py`), call graph over function boundaries
  from `.eh_frame` (`callgraph.py`), function signatures by registers (`gnm_sigs.py`), hook table
  (`gen_gnm_table.py` → `gpu/shim/bbport_gnm_table.inc`), function names (`gnm_names.txt`).
- `gpu/shim/bbport_gnm_hooks.cpp`: hooks 513 libGnm functions at their entry (jmp to a stub near
  the image, relocated prologue) before the game code starts; each site is checked against eboot
  1.09.
  - `BB_GNM_CENSUS=1`: call counts (`BB_GNM_CENSUS_FILE`: full list to a file).
  - `BB_GNM_OBSERVE=1`: each command buffer method marks the packets it wrote; at submission the
    origin of every packet is checked. Result: **100.0 % of packets (99.9 % of words) are written
    by known API calls**; the game's command stream is fully described by Gnm calls.
  - `BB_GNM_TRACE=address[:count]`: arguments, calling code and written packets.
- Found: the game calls 99 command buffer methods; the reserve callback `0x26af230` starts a new
  buffer chunk (the chunk list goes to submission as separate DCBs); `writeDataInline`
  (`0x1073b80`) from GX zeroes 192 bytes once per pass; EOP labels (`writeAtEndOfPipe`, value 1)
  are set by the GPU memory allocator, and `waitOnAddress` waits on those same labels: this is an
  in-queue barrier, not a CPU wait.

## Faster on weak CPUs

- `WRITE_DATA` and DMA waited for all pending host copies (22 µs per `WRITE_DATA`, ~35 per frame,
  12 % of the draw recording thread's time on 4 cores). Now they wait only for copies that read
  the written range (`Scheduler::WaitHostCopiesFor`, source table at 64 KiB granularity); the PS4
  command processor does not wait for previous draws before such a write either. Waits: 37 → 2
  per frame. `BB_HOST_COPY_WAITS=all`: previous behaviour.
- The concurrent scheduler write check (`ProducerScope`) is enabled only with
  `BB_PRODUCER_CHECK=1` (launcher: *Crash diagnostics*).
- 4 cores: 158 → 167 FPS.
- `BB_SECTIONS=1` also shows stage B tasks by function.

## Fixes

- A minimised window (Win+D, "Show desktop") no longer terminates the game (issue #31): a 0×0
  surface gets into neither the frame size nor the swapchain; frames are not presented while the
  window is minimised. Only verified that normal output did not change: in Hyprland the window
  does not minimise to 0×0.

## Comparison with 0.3

Launcher, "For developers" section: *Synchronisation as in 0.3* (`BB_AS_0_3=1` in `run.sh`)
reverts what changed after the release: old memory model, `WRITE_DATA`/DMA wait for all host
copies (`BB_HOST_COPY_WAITS=all`), concurrent write check (`BB_PRODUCER_CHECK=1`). Yahar'gul
standing still, 1080p: 123 FPS versus ~186 with the new memory and translation model. From the
command line: `bash tools/play_ab.sh v03 BB_AS_0_3=1`.

## Depth of field (DoF) works again (issue #44)

With DoF and any upscaler (FSR 3/4/4.1.1, TAA), dark rectangles with sharp edges, stripes and an
upside-down copy of the scene appeared in the far blur (sky, fog); 0.2 did not have this. Cause:
the targeted constant dump waits from 0.3 (`DumpConstRam` waits for the draw recording thread
only if it overlaps their reads): only constant buffers went into the read list, but the
recording thread also reads vertices and indices from game memory. The game writes small draw
data (including the DoF fullscreen quads) through the constant engine, and the dump for the
following draws overwrote it before it was read. The window opened when the recording thread was
delayed right before the DoF passes, on the upscaler passes. Now vertex and index ranges are in
the list too (unprepared draws wait in full). Waits went to ~850/s from ~150/s, FPS in the
tested places did not change. `BB_CONSTRAM_PIPE=0`: full wait on every dump (A/B).

This may also fix flickering UI icons and glyphs (issue #11); not verified.

## Shader cache no longer breaks startup (issues #28, #38, #39)

- Cache files are written under a temporary name and renamed: a crash or power loss leaves the
  old file or none, not a truncated one.
- A truncated entry on read no longer stops the game (`Invalid or corrupted deserialization
  container`, code 23): damaged entries are counted and the cache is rebuilt. Same for SPIR-V
  with a bad header and for pipelines the driver rejected during preload. SRT walker code from
  the cache is registered only after a length check.

## Other

- A CPU without AVX, BMI1, MOVBE, LZCNT, POPCNT or SSE4.2 (Intel before Haswell): a clear message
  at startup instead of `Guest fault (signal 4)`, code 132 (issue #26). `BB_SKIP_CPU_CHECK=1`:
  start anyway.
- `tools/fsr4cap/build_assets.sh` exited silently if one of the Steam folders had no GE-Proton
  (issue #40).
- Game memory copies queued in direct recording mode (drawing after the upscaler passes,
  `BB_VK_RECORD_THREAD=0`) run immediately rather than with the next submission.
- In-game menu in English or Russian: "Language" toggle in the menu (PR #36); defaults to the
  launcher language. Launcher in Brazilian Portuguese (PR #10); untranslated strings are in
  English. README: repository address in the clone command (PR #43).
- `BB_TOGGLE_FILE` toggle: bit 31 disables the texture collector; the second number in the file
  holds temporary experiment bits (`BbToggle::Experiment`).

## Motion vectors (night of 2026-10-07, being tested)

- The Y-axis sign of camera vectors is taken from the G-buffer viewport every frame
  (`BB_CAMERA_Y=up/down` forces it) instead of a constant: 0.2 and 0.3 were each wrong in their
  own places (issues #41, #42, #47, #18).
- If a frame has several G-buffer passes, the camera is taken from the one whose depth feeds the
  vectors.
- FSR 4 on upscaling presets receives the frame in linear light (`BB_FSR4_LINEAR=0`: as before).
- The reactive mask works with FSR 4 too: its result is blended with the current frame on effects
  (fog, light, particles).

## The draw recording thread does not wait for the command recording threads (2026-10-07)

- The presenter (frame barrier, FSR 1, post-processing) is recorded the same way as draws, on the
  command recording threads. Previously it took the command buffer directly: the draw thread
  waited until the recording threads finished everything handed to them (~90 µs every frame), and
  submitted the frame itself (~45 µs).
- Where commands are still recorded directly (upscaler passes: motion vectors, TAA, FSR 3/4,
  sharpening), they go into a separate segment with its own command pool, after the handed-off
  segments, without waiting (`BB_DIRECT_SEGMENT=0`: wait, as before). Signals and ordered tasks in
  such a segment stay in the queue so as not to overtake earlier labels.
- The present thread waits until the frame submission (from the recording thread) has reached the
  queue before placing its wait on it: otherwise one queue would wait on a later submission (GPU
  hang at startup, found during this work).
- DMA into memory the GPU uses in place (PC model) or that the GPU wrote is a GPU copy in stream
  order; the draw thread no longer waits for copies and deferred signals before it (~16 µs each,
  ~900 per second: 4-byte GDS counters).
- Fixed two long-standing rare cases of recording into an already submitted command buffer: a
  size change with sharpening above 1 (`Finish()` in the middle of an upscaler pass) and an FSR 4
  preset change in the menu.
- `BB_FINAL_DUMP_TRIGGER` (`KIND=final tools/frame.sh`): frame snapshot after FSR and
  post-processing.

Measurements (Hunter's Dream, standing still, 4 cores / 8 threads, PC model): 143–144 → 146 FPS;
the draw thread no longer blocks on command recording (was 1.8–3.3 % of its time). 16 threads:
PC model ~186, old model 144–151 (run-to-run spread ±3 %, no difference from the previous build).

## Operating mode in the launcher (2026-10-07)

- *Operating mode* group, *New memory and translation model* toggle (`BB_PC_MODEL=1`), off by
  default. Off: the old memory model, as in 0.3, with all fixes since. On: game memory is used by
  the GPU in place, the command processor's work is translated into Vulkan commands
  (experimental).
- AMD graphics cards only: the launcher detects them via `/sys/class/drm/card*/device/vendor`
  (0x1002) and without AMD makes the toggle unavailable; the GPU library itself keeps the mode
  off if the selected Vulkan device is not AMD (hybrid laptops). `BB_PC_MODEL_ANY_GPU=1`: enable
  anyway, for testing.
- The former *As in version 0.3* toggle moved to the "For developers" section under the name
  *Synchronisation as in 0.3*: it is a rollback for hunting regressions, not one of the two modes.
- Both READMEs describe both modes, a table of differences from shadPS4, and plans.

## FSR 4.1.1 from your own DLL, via a launcher button (2026-10-07)

- *Upscaler → FSR 4.1.1 from your own AMD DLL → Choose DLL…*: you pick
  `amd_fidelityfx_upscaler_dx12.dll` 4.1.x, the launcher builds the assets
  (`tools/fsr4cap/build_assets.sh`) in the background, progress shows in the row caption and in
  the "Log" tab, and at the end FSR 4.1.1 is selected. Cancel with the same button. Without a
  window: `--build-fsr411 <DLL> [loader]`.
- The DLL is checked before recording (`tools/fsr4cap/dll_info.py`: version from file
  properties, FSR 4 models by shader names). OptiScaler community builds (4.0.2b/c: model
  `v07_i8`, 16 passes instead of 29, different weights) do not fit yet; this is reported right
  away, without recording. The loader is looked for next to the DLL and one folder up under both
  names (`amd_fidelityfx_loader_dx12.dll`, OptiScaler's `amd_fidelityfx_dx12.dll`), otherwise the
  launcher asks for it.
- Proton is started through umu-run if installed, otherwise in the Steam container that this
  Proton requires (`toolmanifest.vdf`: Steam Linux Runtime 4 for GE-Proton 11); umu is no longer
  needed.
- The AppImage contains prebuilt dxil-spirv and fsr4cap.exe (Nix) and SPIRV-Tools: no compilers or
  network needed, only GE-Proton. The working folder (Proton prefix ~300 MB) is deleted after the
  build.
- FidelityFX API headers (MIT) live in `tools/fsr4cap/ffx`: the SDK is no longer cloned.
- A new set replaces the old one only as a whole; the launcher shows the script's exit codes
  (2–7) as readable text. A previously built dxil-spirv that does not start (the tree moved) is
  rebuilt.

Tested: from source (Nix, umu) and the package (Steam Linux Runtime 4 in an FHS environment); the
result matches the previous `fsr4_411` byte for byte; launcher button (headless session):
progress "N of 20", rejection for 4.0.2c. Not tested: AppImage on a regular distribution / Steam
Deck (on NixOS the AppImage hides the system `/nix` and the FHS links).
- After the first test (DLL from Pragmata): it is the official FSR 4.0.3 with the same model
  `v07_fp8_no_scale`, but AMD's provider exposes it only on RDNA4; on the RX 7800 XT under any
  Proton the DLL offers only FSR 3.1.5 and 2.3.4. Now 4.0.x is rejected right away with an
  explanation. Also rejected is a "loader" that is actually the upscaler DLL (this happened in the
  video: there was no loader next to it, and the same DLL ended up in the file picker).
- Proton is chosen by trying in turn: the first one under which the DLL enables FSR 4.1
  (GE-Proton 10+, Proton-CachyOS, Proton Experimental, then Proton 11.0/10.0 from Steam), each
  with its own prefix. Tested on the RX 7800 XT: GE-Proton 11-6, Proton-CachyOS 11 (July),
  Experimental (October): yes; Proton 11.0-2c and GE-Proton 9: no. The Steam Deck had only
  GE-Proton 9.
- On NixOS the AppImage says right away that the Steam runtime cannot be run there (code 8),
  instead of an unclear "did not start under Proton".

## FSR 4.1.1: a single DLL and an FP8 variant for RDNA4 (2026-10-07, day)

- **The loader is no longer needed.** AMD's upscaler DLL exports the FidelityFX API itself
  (`ffxCreateContext` …), and `fsr4cap.exe` loads it directly; `dll_info.py` checks the exports in
  the PE table. The loader is asked for only for a DLL without these functions (none seen so far).
  Recording with the DLL directly matches recording through the loader (except two bytes of an
  uninitialised constant in frame 0); the set matches byte for byte.
- Rebuilding from the AppImage failed: `cp: cannot create regular file …/fsr4cap.exe: Permission
  denied`: the file copied from the AppImage is read-only, and a second copy cannot be created
  over it. Now the old file is deleted before copying (and the DLL too, if it is read-only).
- **FP8 on RDNA4.** On RDNA4 vkd3d-proton gives the DLL FP8 matrices, and it runs a different
  pass variant (issue #12): the same 29 passes, constants and buffers, but shaders on WMMA FP8,
  their own weights and their own group grid (each pass has its own tile: 16×8, 32×1, 16×4 …;
  `_post`: tensor width aligned to 32, not 4). After INT8, the build tries FP8 at the first size
  and, if the DLL returns different shaders (RDNA4), records all 20 sizes a second time
  (`fp8/capture_*`). `extract.py` checks the rules of both variants and writes `fsr4_411/fp8/`
  (FP8 matrices, `VK_EXT_shader_float8`) and `fsr4_411/fp8emu/` (FP8 through FP16 matrices, like
  vkd3d-proton on RDNA3; for verification only). The game picks FP8 itself if the graphics card
  has FP8 matrices and the set is built; `BB_FSR411_VARIANT=int8|fp8|fp8emu` forces it. The
  `Upscaler: FSR 4.1.1 replay, …` line now states the variant.
- Passes with matrices run with subgroup size 32 and full subgroups, like vkd3d-proton (their
  wave size heuristic coincides with matrix use; `extract.py` checks this). The game's device
  enables `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`, `vulkanMemoryModel`,
  `computeFullSubgroups` if available.
- Tested on the RX 7800 XT: emulated FP8 (GE-Proton 11-6, `BB_FSR4CAP_FP8=1`): DLL and replay are
  **byte-identical** at 7 sizes, both models, 4 frames (`VARIANT=fp8emu verify.sh`; with a
  dxil-spirv of the same version as in GE-Proton 11-6's vkd3d-proton: our pinned one is newer and
  rounds FP8 conversions differently in emulation). INT8: as before (1600×900 diverges at the
  right edge due to a race in the DLL itself). **Not tested** live on RDNA4: the FP8 model passes
  from both dxil-spirv versions are identical, only `_post` differs.
- On RDNA3 FP8 is not recorded by default: emulation is almost twice as slow as INT8 (1440p
  Balanced, RX 7800 XT: 3.35 ms versus 1.82 ms). `BB_FSR4CAP_FP8=1`: record there too,
  `BB_FSR4CAP_FP8=0`: record nowhere.

## Controls and the in-game menu (2026-10-07)

- **Gesture menu from the touchpad.** Touchpad touches arrived with id 0 and no hold time, and
  the game did not accept them: neither the touchpad nor Back nor Tab opened gestures. Now each
  new touch gets its own id (1–127), as on a DualShock 4, and a hold time. Tested in game: left
  half of the touchpad opens "Gestures", right half "Items" (with the debug menu enabled, the left
  half opens it). The launcher notes this at the touchpad bindings.
- **The keyboard works with a gamepad connected.** Previously it controlled the game only when
  there was no gamepad (and on the Steam Deck there always is one); only Tab and Backspace worked.
  Now keyboard buttons are added to the gamepad buttons, and a held movement/camera key deflects
  the stick fully. Not tested live with a keyboard (only build and tests).
- **The settings menu (Insert / L3+R3) remembers its position**, both between openings and
  between runs (`menu_pos` in bbport.ini, as a fraction of the screen: survives resolution
  changes). Previously it opened at the left edge every time.
- **Mouse cursor over the menu.** While the menu is open, the system cursor is shown (in
  fullscreen and after 3 s without movement it is hidden), and the menu knows where the mouse is
  right away, not after the first movement.
- **Settings from the menu no longer wipe button bindings.** The menu wrote bbport.ini in full,
  with only its own keys: keyboard and gamepad bindings from the launcher (`key.*`, `pad.*`) and
  its other keys disappeared after any change in game. Now the menu changes only its own lines and
  leaves the rest; the file is replaced as a whole (an interrupted write leaves the old one).
  Test: `ninja -C out/gpu settings-test`.

## BB_READBACKS=2 no longer hangs at startup

The exact readback mode also protects GPU-written pages from reads. They were also read by the
port's copy threads (upload to VRAM: `bb:Copy*`, `bb:VkRecorder`): the read faulted into the
fault handler, which queued a readback to the GPU thread and waited, while the GPU thread waited
for the draw recording thread, which waited for those copies. Now the port's copies in exact mode
read game memory through a second mapping of the same pages that the protection does not touch.
Tested: with `BB_READBACKS=2` the game reaches the title screen and runs (previously it stalled
during loading). Not tested in the game itself in this mode. `BB_PROBE=<wrapper>` in run.sh: run
under gdb etc., from source too.

## FSR 4.1.1 from the AppImage on NixOS (2026-10-07, evening)

The AppImage runs in its own mount namespace with a chroot where its `/nix/store` replaces the
system one: from inside, neither `umu-run`, nor `nix-shell`, nor Steam's FHS environment is
visible, and the build button on NixOS ended with error 8. Now `capture_all.sh` on NixOS without
`umu-run`:
- if `nix-shell` is available (from source, on the system itself), restarts itself in
  `nix-shell -p umu-launcher`;
- in the AppImage, copies itself and `proton.sh` to the working folder
  (`~/.local/share/bbport/fsr4cap`, visible to both sides) and runs the recording on the system
  via `systemd-run --user --wait --pipe` in a login shell (the user's PATH and NIX_PATH);
  progress and exit code come back, cancelling stops the service. Translation to SPIR-V happens
  inside the AppImage. `systemd-run` from systemd-minimal (it was already in the package) is
  added to the launcher's PATH.

Tested on this PC (NixOS): the windowless button (`--build-fsr411`) from the AppImage builds
FSR 4.1.1 to the end, the set is byte-identical to the reference, the working folder is deleted;
cancelling mid-recording stops the service on the system.
