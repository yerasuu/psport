# Changes after 0.4 (since 2026-10-08)

Measurements: RX 7800 XT / RADV, Hunter's Nightmare at the lamp (user's save), 1080p with no FPS
cap, new model (`BB_PC_MODEL=1`); "4 cores" means `CPUS=0-3,8-11` (4 cores / 8 threads).
Details and what remains: [EMULATION_REMOVAL_PLAN.md](EMULATION_REMOVAL_PLAN.md), "Step 3".

## EOP/EOS labels are written by the GPU (new model)

- `EVENT_WRITE_EOP` without interrupt, `EVENT_WRITE_EOS` and compute queue `RELEASE_MEM`: a Vulkan
  buffer marker (`VK_AMD_buffer_marker`, bottom of pipe) straight into game memory, at the same
  point in the stream where the PS4 command processor writes them. Previously each label was
  written by a CPU thread after the whole Vulkan submission finished: ~330 labels per frame, 61
  thousand per second.
- A 64-bit label is two 32-bit writes, the upper half first (whoever sees the new lower half also
  sees the new upper half).
- Labels with interrupt, as well as without `VK_AMD_buffer_marker` (NVIDIA) and with
  `BB_FREE_CHECK=1`: as before. `BB_GPU_LABELS=0`: as before everywhere.
- EOS with a write from GDS: a GPU copy in stream order instead of waiting for the whole GPU
  (`Finish()`) and a CPU write.
- Statistics: the `EOP fences` line shows how many labels per frame the GPU wrote.
- Same FPS (16 threads: 233; 4 cores: ~217 versus ~215), same image; 8 minutes of walking: see
  below.

## Game code hooks without traps

- The sites where the game allocates GPU memory (range allocator, three heap sites) and where the
  loaders copy resources (`memcpy`) used to be `int3`: every call was a kernel signal (SIGTRAP),
  ~1200 per second from the allocator alone. Now they jump to a stub near the game image: the stub
  calls the handler and executes the overwritten instructions; the `memcpy` call goes through a
  trampoline.
- The bytes are rewritten over the `int3` already in place (a thread hitting it at that moment
  goes into the stub). `BB_GUEST_HOOK_TRAPS=1`: traps, as before. Diagnostic sites
  (`BB_FRAME_STATS`, `BB_HEAP_SITES`) remain traps.

## New memory model on NVIDIA

- A new way to hand game memory to the GPU: host memory import (`VK_EXT_external_memory_host`):
  a direct memory chunk stays in the runtime's memfd, the GPU imports its second mapping. The CPU
  works with normal cacheable memory, the GPU over the bus, as with a PC game's upload heap. Freed
  memory in such chunks is zeroed rather than discarded from the memfd (the GPU holds those
  pages).
- At startup, a self-test exactly as in operation: import of a memfd chunk, binding to a sparse
  buffer, GPU write then CPU read through the other mapping, CPU write then GPU copy; the sparse
  buffer for such memory must also accept VRAM. If it fails: dma-buf (on NVIDIA, "one chunk per
  allocation"), for which the CPU read speed through the driver's mapping is now measured (below
  1 GB/s it is not used: the game code runs on this memory). If that fails too: the old model. In
  the log: `Guest memory: host memory imported by the GPU`, `host memory import does not work
  here: …`, `CPU reads through the driver's dma-buf: N MB/s`.
- Order: on AMD, dma-buf (as before; RADV does not import memfd: `ErrorInvalidExternalHandle`,
  verified), on the others, import first. `BB_GUEST_MEMORY=host` or `dmabuf` chooses manually.
- The arena (sparse buffer of game memory) is created with the selected external memory type; if
  the driver does not allow VRAM into the arena with it: the old model, with a message in the log.
- Launcher: the *New memory and translation model* toggle is available on AMD and NVIDIA (texts in
  three languages). Without the launcher: `BB_PC_MODEL=1`.
- **Not tested on NVIDIA** (the author has only AMD): a log from NVIDIA is needed.

## Diagnostics

- `BB_DCB_STATS=1`: besides command buffer structure, PM4 packets per second by type,
  `EVENT_WRITE` events by type, EOP by data source and interrupt, EOS by command, `WAIT_REG_MEM`
  by location and engine, and how the waits ended (VO label, in-stream label, satisfied
  immediately, waited, and how many ms per second).

## The game's copy shader: writes stay in game memory; translation instead of replacement, optional

- The game's compute copy shader (`cs fefebf9f`, 2–50 dispatches per frame) ran as a replacement:
  while recording commands, the CPU read the copy list from game memory and performed the copies
  with our own shader. Now it can be translated as is (`BB_COPY_SHADER_NATIVE=1`, new model only):
  the GPU reads the list at execution time, as on PS4.
- Found: next to where it copies lie shader code and descriptor tables that our translator reads
  on the CPU. While these blocks are in game memory, everything is correct; a copy in VRAM (the
  "GPU writes this 3 frames out of 60" rule) gave the translator stale code: black scene. The
  replacement kept them in game memory by accident (its ranges differ every time). Now this
  shader's writes stay in game memory in both modes (`force_writes_in_place`).
- Default: replacement. The native shader on 4 cores costs the draw recording thread ~0.3 µs per
  packet (+10 %), the GPU +2 %. Bit 2 of the second number of `BB_TOGGLE_FILE` switches it at
  runtime.

## Constants in place (optional) and buffer binding

- `BB_CONSTANTS_IN_PLACE=1` (new model): the GPU reads small read-only buffers (constants)
  straight from game memory rather than from a copy made during parsing (constant ring) or during
  recording (stream buffer). The copies protected drawing from command processor writes that the
  CPU executed "ahead"; in the new model these writes are executed by the GPU in stream order, so
  the copy is no longer needed for correctness. Measurement (4 cores, at the lamp): parsing
  −0.8 µs per draw, GPU +3 % (constants go over PCIe), same FPS; hence off by default. Bit 1 of
  the second number of `BB_TOGGLE_FILE` switches it at runtime.
- The binding of a read-only buffer already residing in game memory is remembered until residency
  changes (no block walk on every binding); "is the buffer in place" checks for the
  `BB_TRACE_SHADER` diagnostic are no longer performed when it is off (there were two per
  binding).

## Fix

- The memory access fault handler registry lost a handler with the same priority as an already
  registered one (the set was ordered by priority only). The order is now explicit: VRAM
  diagnostic trap, page manager, SRT walker.

## Diagnostics (new)

- `BB_RES_STATS=1`: where shaders get resource descriptors from, registers or tables in memory
  (SRT walker), by kind, per binding.
- `BB_VRAM_ACCESS_TRAP=1` (+ `BB_VRAM_TRAP_RANGE=address,size`): game memory blocks moved to VRAM
  are closed to the CPU; the code sites accessing them are printed (who reads the stale copy).
- `BB_WATCH=address,size`: what the translator reads from the range (pointers in user data,
  buffers, shader code). `BB_TRACE_ADDR=address,size` (with `BB_TRACE_SIZE`): bindings over the
  range from the very start. `BB_TILE_BACK_LOG=1`: images written back to memory because they are
  read as a texture buffer. `BB_COPY_SHADER_TRACE=1`: copy shader buffers.

## The game's occlusion queries are real Vulkan queries (new model)

- Bloodborne issues about 60 occlusion queries per frame (`ZPASS_DONE`); previously they got a
  fake "everything visible" sequence. Now Vulkan queries count the samples, and the GPU writes the
  results into game memory in PS4 format (depth block counters with a ready bit). Render passes
  are not split: a query is cut at pass and command buffer boundaries, writing the results is
  deferred to the nearest point outside a pass and done with a single shader dispatch. On 4 cores
  the difference from the fake counter is within noise. `BB_OCCLUSION_QUERIES=0`: as before.
- `COPY_DATA` (constants, registers, clock, memory, GDS): GPU writes and copies in stream order;
  `COND_EXEC`: condition on a label in stream order or after GPU work up to the packet;
  `SET_PREDICATION` (ZPass, Bool64/32): draws and dispatches with the predicate bit are skipped if
  the predicate does not hold. Bloodborne does not use the last three; this is for porting other
  games.
- `BB_PM4_SELFTEST=1`: self-test of all this in the running game (8 checks, prints PASS/FAIL).
- Fixed a deadlock when the CPU reads GPU data from the parsing thread.

## The game's copy shader: as is by default

- The game's copy shader (`cs fefebf9f`, ~36 dispatches per frame) is now translated and executed
  itself by default: the GPU reads the copy list. `BB_COPY_SHADER_NATIVE=0`: the previous
  replacement (the CPU reads the list). Cost on 4 cores: +6 % on the draw recording thread, GPU
  and FPS unchanged.
- To keep the cost at that: a GPU write to memory looks up the affected texture by the first byte
  only (only a texture starting at that address matters; previously the whole range was walked
  under a mutex), and a write over pages the CPU has not changed and that are already marked as
  GPU data goes through without locking the tracker.
- The "texture copy through buffers" and "texture clear through a buffer" replacements are kept:
  the path through memory (download the rendered frame to memory, run the game's shader, re-upload
  the texture) currently gives a frozen image: render target contents do not reach game memory,
  and when the scene is rendered at reduced resolution they are not there at all.
  `BB_HLE_OFF=1|2|4`: experiment with disabling replacements, `BB_HLE_STATS=1`: how often they
  fire (~4, ~15 and ~10 per frame).
- Experiment `BB_GUEST_VRAM=1`: all game memory in VRAM via Resizable BAR: 16 FPS already in the
  menu (the CPU reads it over PCIe), unusable.

## Saves are not corrupted on a crash (#64)

- Bloodborne saves about once a second: deletes `backupN`, renames `userdataN` to `backupN` and
  writes a new `userdataN` (1.3 MB). A crash or the process being killed mid-write left a
  truncated `userdataN`.
- Now a save file (`/savedataN/...`) opened for writing is written to a temporary copy
  (`<name>.<n>.bbtmp` next to it), and on close it is flushed to disk (`fsync`) and moved into the
  file's place with a single `rename`. A crash mid-write leaves the previous file (or its absence,
  if the game had already renamed it to `backupN`: then the game reads `backupN` itself;
  verified).
- While the file is open, path operations (`stat`, `access`, `truncate`, reopening) see the copy:
  the game checks the size of the file being written by path.
- The copy does not appear in directory listings; copies left over after a crash are deleted on
  mount.
- `param.bin` and `icon0.png` are written the same way (temporary file, `fsync`, `rename`).
- `BB_SAVE_TRACE=1`: all of the game's save file operations to the log.

## Game file preparation and gamepad

- `prepare.py` and the other scripts, the launcher: text files are read and written as UTF-8
  explicitly, console output does not fail on characters outside its encoding (#55: Chinese,
  Japanese, Korean Windows locale, `™` in the game title).
- `eboot.bin` (and the `sce_module` modules) are also accepted as a plain ELF, without the SELF
  wrapper: some dump programs write them that way (#49). Verified: an ELF extracted from our eboot
  gives the same `boot.bin` byte for byte.
- Sticks (PR #71; #70): a Switch Pro clone whose four axes are all offset by ~±16380 at rest and
  have half the travel is calibrated: neutral is taken from a quiet window after connecting, each
  axis's travel from its own range. Dead zone as in shadPS4: `BB_PAD_DEADZONE` (default 5 of 127)
  and `BB_PAD_DEADZONE_OUTER` (127). `BB_PAD_CENTER=lx,ly,rx,ry`: manual neutral,
  `BB_PAD_CENTER_CAL=0`: no measurement.
  Difference from the PR: neutral is accepted only if all four axes are offset (otherwise a normal
  gamepad connected with a stick held would remember that tilt as neutral until reconnected);
  while neutral is unknown, the sticks are read as before rather than staying still.
- README.ru.md: repository address in the `git clone` command (PR #43; the English README already
  had it).

## GPU hangs: indirect dispatches, VRAM, graphics card info

- Indirect dispatches (#34, comment on #32; new memory model): twice the game lost the device on a
  particle simulation that got garbage instead of a group count (1.2 and 3.8 billion groups; our
  logs from 2026-10-06 had this too: 1.4 billion and 6.4 million, without consequences).
  Bloodborne's real counts are at most a few hundred. Now before each indirect dispatch a
  single-thread shader (`indirect_guard.comp`) copies the group counts into our buffer, zeroing
  implausible ones (above the device limits or `BB_INDIRECT_GUARD_MAX` groups in total, default
  4M), and the dispatch runs from that copy. The worst that can happen now is a skipped particle
  step instead of a crash; skips are printed as `Indirect dispatches: N skipped ...`.
  `BB_INDIRECT_GUARD=0`: as before. The cause of the garbage in game memory has not been found
  (new model only, rare). Cost: within noise (183–184 FPS, GPU 5.42–5.47 ms in one scene). Tested
  with a threshold of 10 (skips fire, the game runs) and under the validation layer (no new
  errors).
- Moving game memory blocks to VRAM now respects the driver's video memory budget: if more than
  `BB_VRAM_PROMOTE_PERCENT` (default 90 %) is in use, no new copies are created; blocks are read
  in place (slower, but without overflowing VRAM on 8 GB cards), and moving resumes once memory is
  freed. A log line on switching. The "MiB of loaded blocks moved ... to VRAM" number is
  cumulative (how much was moved during the session), not occupied space; occupied space is in
  the `Memory:` line.
- The start of the log now has a line with the graphics card, VRAM size, driver and Vulkan
  version (hang reports such as #56 lacked it).
- #56 (old model, hang while loading a map): two logs, two different stall points (an ordinary
  5-index draw and copies between passes); the shader from the first runs 4–12 times per frame
  here without problems, and the same patches (Disable DoF, Motion Blur) are enabled here. Could
  not reproduce; without the graphics card model and Mesa version there is no way forward (they
  will now be in the log).

## Bugs from issues (2026-10-08, day)

- **Building FSR 4.1.1 where the Steam container does not start (#61).** Bazzite + 9070 XT: no
  Proton "started FSR 4.1", the reason in the list was empty, and at the end of the log was
  `bwrap: No permissions to create a new namespace`: the Steam container (pressure-vessel, under
  umu-run too) cannot create a namespace. This happens when the launcher itself runs in a sandbox
  (Flatpak, a Steam shortcut with a runtime) or the kernel forbids it. Now on such an error Proton
  is started directly, without the container (message `Steam's container could not start here
  (...): running Proton without it`, and the same until the end of the build);
  `BB_FSR4CAP_RUNNER=direct`: do so right away.
  The reason each Proton was rejected is the last non-empty line of the recording log, otherwise
  the container or Wine error (previously the last line, often empty). The same Proton is no
  longer tried twice via the `/home/...` and `/var/home/...` paths (Fedora Atomic): identical
  folders are recognised by device and inode, not by `realpath`.
  Tested: recording inside `bwrap --disable-userns` (the same container error): the fallback path
  recorded FSR 4.1.1 frames under GE-Proton 11-6, the result matches a normal recording byte for
  byte (except pointer addresses in `trace.txt`).
- **MangoHud works again with a system MangoHud (#66).** Since 0.3, with a system MangoHud,
  `run.sh` disabled the layer bundled in the AppImage by name (`VK_LOADER_LAYERS_DISABLE`), but
  the system layer usually has the same name (`VK_LAYER_MANGOHUD_overlay_64_x86_64`), so the
  Vulkan loader disabled both. Now the bundled MangoHud is removed from the layer search paths
  (`XDG_DATA_DIRS`) and the system one stays. Verified with the Vulkan loader
  (`VK_LOADER_DEBUG=layer`): previously "forced disabled", now the layer is inserted.
- **The item picture on the loading screen no longer disappears (#67, bmy's patch).** Scaleform
  masks are drawn without colour writes; at output other than 1080p they went into the game's
  depth, while the next UI draw switched to the output-sized UI depth with a cleared stencil, so
  the picture failed the test. Now UI draws without a colour target also go to the UI depth. Not
  tested in game (on the author's macOS: yes).
- **Monitor selection (#69).** The window and fullscreen went to the monitor SDL considers
  primary. *Display → Monitor* in the launcher (`BB_DISPLAY`: number or part of the name; list:
  `bb-gpu-capabilities --displays`); at startup the log has `Display N: ...` lines with the
  selected one marked. Tested on Hyprland with two monitors: the fullscreen window opens on the
  selected one (on Wayland the compositor places a normal window).
- **Debug menu: fonts from a mod are found.** The debug menu patch reads fonts from
  `dvdroot_ps4/adhoc/font/DbgFont14h.ccm` and `.tpf` (and shaders `adhoc/FontShader/debugFont_*`),
  and the game requests the names in lower case (`dbgfont14h.ccm`, found with the new
  `BB_FILE_TRACE=1`). The check in `patches.py` looked for them in `dvdroot_ps4/font` with exact
  case, and the runtime on Linux was case-sensitive: fonts were found neither from a mod nor in
  the dump, and startup failed with a Python traceback. Now: the runtime, failing to find a game
  file, looks for it case-insensitively (like PS4 and shadPS4; only on a miss, no cost for found
  files); the check looks in `adhoc/` case-insensitively; without fonts the debug menu patch is
  not applied (line `Patches: debug menu off: ...`, with a hint if the fonts are in the old
  `dvdroot_ps4/font`) and the game starts; an `adhoc` folder in a mod is recognised as a game
  folder. Verified: the mod `Adhoc/Font/DbgFont14h.*` is opened by the game
  (`Mods: open /app0/dvdroot_ps4/adhoc/font/dbgfont14h.ccm -> .../Adhoc/Font/DbgFont14h.ccm`).
  The menu has not been tested with real fonts (we do not have them).
- `BB_FILE_TRACE=1`: every path the game opens, checks or reads (`File trace: ...`).
- **The "Play online / offline" screen is gone.** The port has no PSN; now the game opens the
  main menu in offline mode right away, at startup and after "Quit Game", with or without a save.
  Game patch "Skip Online/Offline Choice" (`patches/Bloodborne.xml`): the function that builds
  this dialog (`0x1b39030`, found by string IDs 401010/401011 from `menu.msgbnd`) returns what its
  "PLAY OFFLINE" item would return (a call to its handler `0x1b49cd0`, vtable `0x533be30`): the
  next dialog in the chain, so the choice is not shown. Enabled by `patches.py` by default
  (launcher: *Game effects → Skip the choice*, `BB_SKIP_NETWORK_CHOICE=0`: show it). The previous
  variant (the port pressed "cross", the screen flashed) is removed. `tools/restart.sh` still
  shows the choice (its scenario presses on the title itself). Verified with screenshots: start
  without input goes straight to the main menu; "Continue" → "Quit Game" goes straight to the
  main menu.
- Memory on reloading a location (6 cycles of "Continue → quit", new model): RSS 707 → 733 MiB,
  VRAM levels off at ~3.7 GB; old model (4 cycles): RSS 3735 → 3795 MiB, VRAM ~3 GB. A
  multi-gigabyte leak (user report about teleports) is not visible this way; teleporting between
  different locations has not been tested.
- **Running out of video memory for game memory copies does not crash the game.** The old model
  keeps game memory copies in 64 MiB VRAM blocks; on 4 GB cards (RTX 3050 mobile, user report) a
  block allocation could return `ErrorOutOfDeviceMemory`, and the game stopped
  (`vk_platform.h:73`, code 23). Now such a block is taken from system memory that the GPU reads
  over the bus (line `Guest memory: VRAM is full ...`): slower, but no crash.
  `BB_RESIDENCY_VRAM_LIMIT_MB=N`: test (VRAM "runs out" after N MiB of blocks). Tested with
  32 MiB: the game runs, the image is correct, 28 FPS (everything in system memory; in practice
  only the excess goes there). Whether this is the place where the user's report crashed is not
  proven: the report lacks the line before the stop.
- **"GPU data readback: Precise" no longer freezes the first cutscene.** New game → the cutscene
  stuck on the first frame (sound kept playing): the video player wrote a movie frame into memory
  the GPU had written earlier; the handler asked the GPU thread for a readback, and that thread,
  while taking a copy of the game's bytes for the "twins" (`BB_GPU_WRITE_TWINS`) under their
  mutex, itself hit a read-protected page and in the handler waited for the same mutex. Found via
  thread stacks (SIGUSR2). Now the game's bytes for the "twins" are read through an unprotected
  view (`ReadBacking`), and with `BB_READBACKS=2` the "twins" are disabled: a page with GPU bytes
  that the game may write to would be "write-only" (the next stop was `page_manager.cpp: ...
  write-only`). Verified: new game with and without `BB_READBACKS=2`: the cutscene plays until
  character creation.
- **Settings menu like in PC games, in Bloodborne style.** Instead of an ImGui window over the
  game (Insert / L3+R3): the game is dimmed, a central panel with ornamental lines like the game's
  own dialogs (DejaVu Serif serif font, bundled next to DejaVu Sans), a title and the tabs
  "Image / Upscaler / Game effects / Advanced" (L1/R1, Q/E, PageUp/PageDown, mouse). Each setting
  is a "name ◀ value ▶" row: up/down (D-pad, stick, arrows) selects a row, left/right changes the
  value, Cross/Enter goes to the next value, the mouse clicks the arrows or the row, sliders are
  dragged with the mouse. The description of the selected setting is at the bottom (instead of
  "(?)"), together with the restart warning; an "Apply and restart the game" row appears when
  needed. Button hints are in the footer; Esc / Circle / Insert / L3+R3 closes. Unavailable values
  (FSR 4 without support) are skipped, dependent rows are greyed out (preset with TAA, sharpening
  strength without sharpening, reactive mask). Own focus handling (ImGui navigation did not reach
  the rows). `BB_MENU_KEYS_FILE=<file>`: scripted menu control for tests (`toggle up down left
  right enter back l1 r1 kup kdown`).
- **Port settings as native pages in the game's "System" menu.** After "Screen/Sound" there are
  now "Image", "Game effects" and "Game patches" entries: the game itself builds and draws them
  (its rows, lists, slider, hint at the bottom, "Default" with confirmation), both on the title
  screen and in pause. "Image": output resolution, upscaler (the list holds only what this
  graphics card and build support: no DLSS without NVIDIA), preset, sharpening (slider 0–10,
  0 = off, 10 = 2.0), FPS counter; these change immediately and are written to `bbport.ini`.
  "Game effects" / "Game patches": model detail and effect patches (after restart). "Default" sets
  the port's default values. Texts are in the game's own language (Russian if its texts are in
  Cyrillic, otherwise English). How it works (`gpu/shim/bbport_game_menu.cpp`): hooks on the
  game's text lookup (own ids 0x7F0000+) and on adding an entry to the menu (0x1b4e2a0, after
  "Screen/Sound"); a page is the "ControllSetting" layout with our own rows and title; the last
  argument of the game's rows is a pointer to the default value. Each hooked function is verified
  byte for byte, `BB_GAME_MENU=0` disables it. Verified: 16 page openings, memory growth as with
  the game's native pages; the overlay (Insert / L3+R3) remains.
- 60 FPS smoothness over time (user report: "after a while frames are uneven"): 6 minutes
  standing, `BB_FPS=60`: every minute 16.67 ms, spread 0.00–0.01 ms, not a single frame longer than
  20 ms (except loading in the first minute). Not reproducible on the RX 7800 XT.

## Merged PRs (2026-10-08, evening)

- **PR #84: DLC.** Folders `<user>/addcont/<title>/<label>` (as in shadPS4) are reported to the
  game as installed add-ons: saves with The Old Hunters load, DLC areas are accessible (#33).
  Add-on data is not mounted: The Old Hunters needs only the label, the data is in patch 1.09. On
  merge: `_GNU_SOURCE` under `#ifndef` (the test builds with `-D_GNU_SOURCE` and `-Werror`), in
  the AppContent test a user folder stub and a check: the folder
  `addcont/CUSA00900/SPEXPANSIONDLC03` gives one "installed" entry.
- **PR #80:** `BB_FSR4_STATS=1` no longer breaks FSR 4 v07 pipelines without
  `VK_EXT_pipeline_executable_statistics`. The patch to the FSR-Vulkan submodule changed: the
  submodule's working copy needs to be reverted
  (`git -C gpu/third_party/fsr-vulkan checkout -- src/ffx_vk_fsr4_v07.c`), and `build.sh` will
  apply the new version.
- **PR #78:** launcher: "Copy" and "Export log…" buttons in the "Log" tab.
- **PR #75:** launcher in Simplified Chinese (strings added today are in English until
  translated).
- **PR #83: DLSS on Linux.** The bridge `gpu/dlss_bridge` (MIT, the only code using the NVIDIA
  SDK) is built with `DLSS_SDK_ROOT=<github.com/NVIDIA/DLSS> bash build.sh` into
  `out/libbbport_dlss.so`, and the NVIDIA library is copied next to it; the game loads the bridge
  at startup. New upscaler `dlss` (launcher, menu, `bbport.ini`): the same inputs as FSR 3.1,
  Native AA = DLAA, on error falls back to FSR 3.1. Without the bridge and library, on non-NVIDIA
  or with `BB_DLSS=0` nothing changes (DLSS is unavailable in the menu). On merge: the DLSS item
  and its description moved into the new menu, the row in the launcher's upscaler list placed
  after PR #75's language row. Tested here: build without the SDK, game on AMD (FSR 4.1.1, FSR 4
  v07 with statistics). Not tested with DLSS (no NVIDIA); the author has an RTX 4090. The bridge
  and the NVIDIA library are not included in the AppImage.

## After 0.5-pre1 (2026-10-09)

- **The new memory model is AMD-only again.** Log from a GTX 1660 Ti (driver 615.71, Hyprland):
  host memory import passed the startup check, but after the first move of blocks to VRAM
  (`84 MiB of loaded blocks moved`) came `GPU watchdog: submission 4114 not finished after 10 s`:
  the GPU finished the copy shader and did not start the next dispatch. The submission waits on
  the memory rebinding semaphore (the arena's `vkQueueBindSparse`), and on NVIDIA each such
  rebinding held the GPU for 10 seconds or longer; the desktop froze along with it, which from
  the outside looks like "the whole system hangs". The startup check does not catch this (it has
  one small binding). Now on NVIDIA the toggle is unavailable, and anyone who had it on gets the
  old model (the launcher passes `BB_PC_MODEL=0`, the GPU library also does not enable it on
  NVIDIA); `BB_PC_MODEL_ANY_GPU=1`: try anyway. README and launcher texts corrected.
- `BB_RESIDENCY_REPORT=<file>` (new model diagnostics): when the file appears, the log receives
  the game memory blocks the GPU recently read in place rather than from VRAM, and why they were
  not moved (the CPU writes often; bytes whose writes we do not know about; waiting to be moved).
  At the lamp in Hunter's Nightmare: ~1.2 GB of such Garlic memory blocks are entirely "unknown".
- Report "first load is slower, after a teleport 90 FPS" (RX 7900 XTX, new model): not reproduced
  here; first and repeated load (exit to title, "Continue") 131.7 and 131.9 FPS on the
  RX 7800 XT; moves to VRAM after loading finish within seconds. The tester's log is needed.

## New model on NVIDIA: analysis (2026-10-09, continued)

- **The cause of the hang is found.** It is a known regression of the proprietary NVIDIA driver on
  Linux (since 555 and to this day, the tester has 615): the time of `vkQueueBindSparse` grows
  with the number of pages **already** bound across all sparse resources, and the call holds a
  global lock (NVIDIA developer forum, "Sparse texture binding is painfully slow": 9200 pages,
  3.6 s; no fix). The new model's arena has gigabytes of 64 KiB pages (the tester had 863 MiB
  bound in place even before the first moves), so every move of blocks to VRAM cost seconds. The
  game itself noticed the GPU stall even before the first move ("Stall during rendering at flush
  1").
- **Why it cannot be worked around with "one fix".** Measurement with `BB_RESIDENCY_MIX=1` (new
  diagnostic): ~17 thousand bindings per second (~85 per frame), ranges of hundreds of MB holding
  blocks in VRAM and in place at the same time (shaders bound to the whole memory; a third over
  data written by the GPU). One sparse arena mixes them for free; a "VRAM arena + separate game
  memory buffers" scheme cannot serve them. With no rebinding at all (`BB_FIXED_ARENA=1`,
  everything read in place over the bus): **48.6 FPS versus ~205** on the RX 7800 XT, worse than
  the old model.
- Outcome: the new model stays AMD-only. NVIDIA needs either a driver fix (or Mesa NVK, where
  sparse works fine), or software paging in translated shaders instead of sparse: a rework of
  several days.
- Added: `BB_FIXED_ARENA=1/0` (the arena keeps the first bindings: no moves to VRAM, blocks outside
  game memory chunks in system memory; default: with host memory import, i.e. on NVIDIA with
  `BB_PC_MODEL_ANY_GPU=1`); log line `vkQueueBindSparse of N ranges took X ms` (longer than 50 ms,
  in both models); in the GPU watchdog report, `Arena binds: ...` (whether submissions wait on an
  unfinished binding); `BB_RESIDENCY_MIX=1`: "in place / VRAM / mixed" bindings over 2 s.

## New model on all graphics cards: the layer's memory module (2026-10-09)

- **The new model can be enabled on any graphics card.** On AMD, as before, the sparse arena. On
  NVIDIA, Intel and the rest, the layer's memory module (`gpu/layer`,
  docs/en/history/MEMORY_MODULE_PLAN.md): game memory is bound in place through buffers of its
  chunks, copies in VRAM are separate buffers ("mirrors"), a move is a copy plus bookkeeping in the
  module. `vkQueueBindSparse`, which made the game and the desktop hang on NVIDIA, is never called
  in this mode. Manually: `BB_LAYER_MEMORY=1/0`. The launcher no longer hides the toggle on
  non-AMD; README and the tooltip (three languages) corrected. **Not tested on NVIDIA**: a tester's
  log is needed.
- Measurements on the RX 7800 XT, standing still: was 48–60 FPS in module mode, now ~205 (sparse
  ~205–238, old model ~220). In another place: module 167.5, sparse 164.5, old model 146. 5 minutes
  of walking with area streaming: no errors, 166–240 FPS.
- What was in the way (found via the GPU profile):
  - **Image copy and clear HLE did not fire.** The `IsComputeImageCopy` / `IsComputeImageClear`
    detectors require the shader to have exactly two buffers, but in module mode every shader
    with game buffers got a third one: the page table. The render target memory copy
    (`cs 3d5ebf4e`) and the clear (`cs 8b355b5a`, 31 dispatches per frame) ran as real shaders
    through memory, and images were reloaded from it every frame (~16 1080p images instead of
    ~2.6). Now the page table is not counted (`ShaderBufferCount`): 60 → 136 FPS.
  - **Mixed ranges.** A range whose blocks are all in VRAM except one with "unknown" bytes (an
    edge, a gap between images) was read entirely over PCIe (8 MB per binding). Now such a block
    is "volatile" in the mirror: updated from game memory before each binding, and what the
    dispatch wrote is copied back after the dispatch. 139 → 207 FPS. `BB_LAYER_VOLATILE=0`:
    disable.
  - Move to VRAM on the first binding of a range of known data (as the arena did); it respects
    `BB_GARLIC_VRAM=0` and the VRAM budget (`BB_VRAM_PROMOTE_PERCENT`, important for 6 GB cards).
- The startup host memory import check in module mode verifies GPU and CPU writes through the
  imported buffer itself, without a sparse buffer.
- Diagnostics: `BB_RESIDENCY_MIX=2`: also the heaviest in-place / mixed bindings with a block
  breakdown (VRAM, eligible, "dynamic", unknown bytes); the `Layer memory:` line has the number of
  `vkQueueBindSparse` calls and the bytes copied back.

## Memory module: exploding vertices and speed (2026-10-09, day)

The outcome of the work on the new model for NVIDIA is in `docs/en/history/NVIDIA_MEMORY_MODEL.md`:
write traps on blocks with a VRAM copy, reads through the module wherever there is a copy, CPU data
is no longer overwritten by GPU data, GPU writes to VRAM only into blocks the CPU has not touched
(`BB_LAYER_GPU_WRITES`), the copy shader in module mode through HLE, caches of module answers.

## 0.5-pre2: original GPU copy and portable labels

In the final build the copy shader in the new model is now native by default: its writes are
kept consistent between game memory and VRAM mirrors. Added standard Vulkan completion labels
for GPUs without `VK_AMD_buffer_marker`, fixed buffer write/read dependencies and migration of
the whole cache on a format change. Detailed description of the release and tests:
[CHANGES_0.5-pre2.md](CHANGES_0.5-pre2.md).

## 0.5-pre3: memory module on NVIDIA

Based on the first logs from a GTX 1660 Ti: VRAM copies re-enabled for the module (the sparse
arena workaround had disabled them), a shared VRAM budget with the texture cache, growth headroom
for copies instead of reallocating them, WRITE_DATA writes into the copy, watched blocks
(`BB_LAYER_WATCH`), retrying file reads on EFAULT.
Details: [CHANGES_0.5-pre3.md](CHANGES_0.5-pre3.md) and
[NVIDIA_MEMORY_MODEL.md](NVIDIA_MEMORY_MODEL.md).

## 0.5-pre4: background shader compilation, VRAM on 4–6 GB cards, menu as in 0.3

New pipelines for passes drawn every frame are compiled on background threads
(`BB_ASYNC_PIPELINES`); game memory copies in VRAM have their own share of the budget (pre3
regression on 4 GB cards); after travelling, VRAM is freed from copies of the location left
behind; the in-game settings menu is a single window again, as in 0.3 and 0.4. Details:
[CHANGES_0.5-pre4.md](CHANGES_0.5-pre4.md).
