# Changelog

What each psport version brings, grouped by area, for players and launcher authors. The technical
record of a version, terse and anchored in code, is `tag-changelog/vX.Y.Z.md`: it is the message of
the annotated, signed tag `vX.Y.Z`, which the release workflow (`.github/workflows/nix-cache.yml`)
publishes as the GitHub Release's notes. How a release is made: [FORK.md, CI and binary
cache](FORK.md#ci-and-binary-cache).

Versions follow [SemVer](https://semver.org) as it reads below 1.0: in `0.y.z` a breaking change
bumps `y`, a feature, fix or performance change bumps `z`, and documentation, refactoring or build
changes alone make no release. What breaks is psport's launch interface: `run.sh` variables
(`PS_*`), `psport.ini` keys, the launcher queries (`psport --list-*`, `--check-game`,
`--has-presets`, `--online-overrides`) and their output, and the on-disk layout of the data folder
(`user/` saves, trophies and caches, `out/`). Between releases `VERSION` reads `0-unstable`.

The tags without a `v` (`0.1` to `0.51`) are bbport's releases, carried in psport's history; their
notes are in [docs/en/history/](docs/en/history/).

## v0.2.0

The shadNet login now goes over TLS or is refused in clear, and the account secrets leave the
environment; the Ngs2 and Audiodec audio libraries are psport's own, FSR 3.1 frame generation, a
driver pipeline cache and VRR pacing arrive, and Atelier Ryza saves and loads. Windows is still
groundwork, not supported.

Breaking: online play against a shadNet server off this machine without TLS, including the default
`play.seamlesshunt.com:31313`, needs `PS_ONLINE_ALLOW_PLAINTEXT_LOGIN=1`; use `PS_SHADNET_TLS=1` for a
server that speaks TLS. Launchers should also pass the title's built-in patch folder with
`--builtin-patches-dir` (or `PS_BUILTIN_PATCHES_DIR`), since `Bloodborne.xml` no longer ships in psport.
Every title rebuilds its shader cache once.

### Network
- The shadNet login and the WebAPI token no longer travel in clear to a remote server: TLS with full
  certificate checks (`PS_SHADNET_TLS=1`), or loopback, or `PS_ONLINE_ALLOW_PLAINTEXT_LOGIN=1`.
- The password and token can come from a one-use file (`PS_SHADNET_SECRETS_FILE`) or stdin
  (`PS_SHADNET_SECRETS=stdin`) instead of the environment, and a run holding them is kept out of core
  dumps. The old variables still work.
- Logs redact header values, cookies and URL queries, and other players' NPIDs move to debug.
- The online module uses mbedTLS and no longer needs OpenSSL or protobuf.
- Bloodborne online: more BBSC packets are understood, shadNet drops get a 20 s grace, and characters
  get their own ids from the local server.
- libSceNet's own socket calls are served offline, local sockets can be created, and epoll waits retry
  after a host signal. Calls with no connection are counted as `offline` in the call report.

### Audio
- libSceNgs2 is native (racks, voices, sampler, submixer, mastering, reverb, custom racks, waveforms,
  pan, geometry and FFT), written from the firmware; AnyPS5's port is retired and `PS_NGS2=native` is the
  only backend. Ryza's sound set-up no longer runs out of memory (ATRAC9 state moves to host memory).
- libSceAudiodec is native too; its output matches the old port byte for byte offline.
- Pad vibration checks its arguments in the firmware's order. Mouse motion made by a pushed controller
  stick (Steam's Desktop Layout, reWASD) is dropped so the camera does not turn twice;
  `mouse_pad_filter=0` or `PS_MOUSE_PAD_FILTER=0` turns it off.

### Graphics
- Off by default: FSR 3.1 frame generation (`frame_gen=fsr`, `PS_FRAME_GEN`, `frame_gen_fit_display`),
  switchable from the menu in a session started with it. Launchers read the choice from the line
  `Frame generation setting: <off|fsr>`.
- `PS_VRR=1` paces frames under the refresh rate; the driver's pipeline cache is saved beside the shader
  cache (`PS_DRIVER_PIPELINE_CACHE=0` turns it off); pipeline compile threads respect the CPU affinity
  and cgroup limits.
- The texture collector follows the live VRAM budget (70, 85 and 95 %), fixing pressure never starting
  on budgets under 2 GB; moves to VRAM are paced by the driver's sparse bind time (100 ms stalls on
  RADV); empty or failed sparse binds no longer hang the queue.
- Tiled render targets up to `PS_READBACK_TILED_KB` (64) are read back for the CPU, which fixes Atelier
  Ryza's lens flare.
- Shader translation: switch jump tables, geometry shader stream-out (Ryza's water), valid-mask exports
  inside EXEC scopes (Ryza's foliage), alpha to mask, depth to colour copies and stencil planes of
  depth images.
- Chained command buffers and packet sizes are followed correctly, and more asserts in the GPU code
  became counted, logged errors.
- Layer mode copies GPU data back before CPU writes (Saga's heap corruption on an RTX 2080).

### Kernel and system
- Guest threads, stacks and TLS are reused, so Atelier Ryza's thousands of workers no longer exhaust
  memory; the thread creation log is throttled.
- `PS_DMEM_MB` growth is limited by the memory the host has free; the guest's debug output and
  `/dev/dipsw`, `shm_open`, process identity calls and shared semaphores are handled natively.
- The system-dialog modules (CommonDialog, SaveDataDialog, WebBrowserDialog) are served by psport:
  Atelier Ryza saves and loads again. NpCommon's heap, PlayGo locus queries and more UserService calls
  are implemented.
- Short reads of regular files are completed, queue events come back beside small timers, and fiber
  and queue asserts are counted errors.
- Exits end in order after writing the call report and `summary.txt`; a launcher without SIGTERM
  creates `PS_STOP_FILE`.

### Loader
- Linked modules start after their dependencies; libSceHmd is served natively.
- The red zone guard (`PS_RED_ZONE_GUARD`) protects guest leaf functions from Windows' exception
  frame; it does nothing on Linux, where `PS_FAULT_CLOBBER_RED_ZONE` simulates that frame to test it.
- A `--user-id` longer than 10 digits is rejected instead of cut.

### Launcher interface
- New query `psport --list-compat`: the titles psport has been seen to run and their state.
- New option `--builtin-patches-dir` and variable `PS_BUILTIN_PATCHES_DIR`; a missing built-in patch
  file no longer stops the launch.
- `--list-features` gains `secrets_handoff` and `frame_gen_fsr`; `audio` is `audio_out` and `input` is
  split in four rows.
- `run.sh` no longer exports `PS_COPY_GPU_BUFFERS` or the write twins variables: psport picks them on
  every host and logs why. `PS_GC_PRESSURE_PERCENT` is removed (`PS_GC_BUDGET_MB` still caps).
- A debug run folder gains `link.txt` and `imports.txt`.

### Windows (groundwork, not supported)
- The platform layer moves window, audio, input, files and GPU memory sharing behind `ps_host_*`;
  Linux behaves as before. Several Windows-only crashes and hangs are fixed (hook ABI, view
  protections, exits).
- A tag publishes `psport-windows-vX.Y.Z-x86_64.zip` on its release, built with a top-level
  `CMakeLists.txt`.

### Diagnostics and tests
- Frame captures gain `PS_SKIP_DRAWS`, `PS_CAPTURE_FRAMES`, `PS_CAPTURE_SUMMARIZE` and the dumps
  `PS_CAPTURE_DUMP`, `PS_CAPTURE_DUMP_TEX` and `PS_CAPTURE_DUMP_VSCONST`.
- OpenOrbis hardware tests for Ngs2 and Audio3d in `tools/hwtest/`; `tools/missing_imports.py` lists
  what each title imports that psport does not cover.

### Documentation and licences
- `LICENSES.json`, `LICENSES/` and `tools/check_licenses.py`; every bundle carries the licence files.
  Supermedo, Mrsuss60 and willl03 are credited for what was taken from their forks.
- Atelier Ryza is marked playable; CUSA50989 is named Symphonia as its `param.sfo` does.

## v0.1.4

The Windows package builds again: v0.1.3's Windows build failed in MSYS2's compiler on one line of the
texture cache, which now uses the form the code already used elsewhere. Nothing changes on Linux.
Windows is still groundwork, not supported.

### Windows (groundwork, not supported)
- `libpsport-gpu.dll` compiles with MSYS2's libc++ again, so the CI job produces the test package
  (`docs/en/WINDOWS_TEST.md`).

## v0.1.3

The network libraries start as on the console, draw preparation no longer reads another draw's data,
GPU memory arenas fit the driver's limits, several crashes and asserts became logged errors, and the
Windows test package arrives with psport's own LGPL FFmpeg. Windows is still groundwork, not
supported: no game runs there, though `psport.exe` now starts from the package (verified under
Proton; the CI-built package is unverified). The launch interface does not change; the new
variables are off by default.

Not completely validated: the readback change for CPU overwrites (#740: the harness could not
measure its effect, a manual offline run dropped about 13 overwrites per second, online messages
were not tested) and the preloaded-pipeline check (#796: no effect on its own, it matters only with
P7, #370).

### Network
- libSceNet and its pool, an SSL context and an HTTP context now start when a game links
  libSceNpManager, before the game's code, as on the console. Games that do not link it see the same
  answers as before (`not initialized`).

### Graphics
- Draw preparation follows nested command buffers and catches up with the GPU thread's registers when
  they drift apart. In Octopath 0 the 122 728 faults of the shader resource walker become none, and
  95.7% of the prepared pipelines are used.
- A walker fault on a preparation thread leaves that draw to the GPU thread instead of reading
  zeros; the call summary counts them as `speculative walks abandoned`, and the fault line shows the
  load's real address.
- Buffer arenas are sized from the driver's `maxBufferSize` (1 GiB pages, at most 3 per arena, on
  RADV) instead of fixed 4 GiB pages that exceeded it. A range no arena can cover is counted and
  clamped or left without one instead of stopping the launch.
- Shaders that set the execution mask to a constant (one lane doing an atomic add) now run those
  instructions in fewer lanes; Blood Omen 2's emulator dispatch finishes. Every title rebuilds its
  shader cache once (`ShaderBinaryVersion` 15).
- Copies and fills over whole pages the GPU wrote no longer wait for a readback of data they
  overwrite, which removes stutters of 70 to 220 ms in Bloodborne (#740; see above). The frame
  statistics gain a `GPU data overwritten by the CPU` line.
- An image freed during the frame no longer crashes the upscaler or camera motion (Bloodborne's lamp
  travel); the kept image is forgotten and logged.
- Preloaded pipelines whose cached shaders disagree with their motion vectors setting are dropped and
  counted at warm-up; no effect without P7.
- Opt-in `PS_GPU_WRITE_OVERLAP=1`: a shader write refreshes every image over its memory, not only one
  starting at the same address.
- Opt-in `PS_SCALE_REDUCED_TARGETS=1`: live scene scaling also scales intermediate targets such as
  Bloodborne's depth of field, which blurred the scene above 1080p (see `docs/en/upscaler.md`).

### Video
- A game that locks a display buffer again right after its flip event no longer loses its label
  (Bloodborne's death screen).
- AvPlayer picks the right FFmpeg stream when an unsupported stream comes before a supported one.
- AvPlayer and Videodec2 no longer stop the run on an unknown stream type, a frame in an unexpected
  format or too big for its buffer, a missing game allocation or a second picture info: each is
  logged with a count, and the player stops or the call returns an error.

### Loader
- An import that matches an export only when library and module versions are ignored is logged and
  counted in a summary line. It stays unbound unless `PS_LOOSE_IMPORT_VERSIONS=1` is set. None of the
  53 190 imports checked in 612 modules and games is affected.

### Windows (groundwork, not supported)
- The CI job now produces a test package, a zip with `psport.exe`, the GPU library, the capability
  check, a probe and the DLLs they load, still never as a release. The probe
  (`psport-windows-probe.exe`) needs no game: it writes `psport-windows-report.txt` with the
  system, each GPU's capabilities, seven memory sharing tests and a verdict on how psport's guest
  memory could work on that machine. `docs/en/WINDOWS_TEST.md` explains how to run it.
- FFmpeg in the package is psport's own LGPL-2.1-or-later build (`packaging/windows/ffmpeg/PKGBUILD`;
  H.264, HEVC, AAC, MP3 and MJPEG decoders, MP4 files), since MSYS2's is GPL-3.0-or-later, which
  psport (GPL-2.0) cannot redistribute. Its PKGBUILD and source tarball ship next to the zip.
  `psport.exe` now starts from the package; verified under Proton, the CI path is not.
  `tools/win32-cross.sh ffmpeg` builds it from Linux.
- When the GPU cannot import guest memory, the message now says why (private memory imports,
  shared views do not, as under Proton on RADV).

### Diagnostics and tests
- Opt-in `PS_JOB_STEP_PROBE=1` counts and logs the two failure exits of Bloodborne's render job step
  (#683).
- `tools/regress/octopath0-play.sched` plays Octopath 0 through character creation (not yet the 3D
  scene), answering the dish name with `PS_IME_TEXT=Steak`.

### Documentation
- `docs/en/WINDOWS_TEST.md` describes the Windows test package and its FFmpeg.

### Validation
- Test runs of the integrated version: all 13 titles behave as before; Bloodborne holds 220-225 FPS; validation shows no new error; Octopath 0 no longer logs shader resource walker faults.
- Included without complete validation, at the owner's request: the cut in readbacks of pages the CPU overwrites whole (the scripted runs do not stream resources; a manual offline session dropped 13 overwrites per second and the effect is not measured against a base) and the motion preload filter (no effect until P7 is integrated). The movie, flip and image fixes are in but the scripted runs do not play movies.
- `PS_GPU_WRITE_OVERLAP=1` is very costly (Bloodborne in play 224 to 20 FPS) and stays off by default.
- Every title rebuilds its shader cache once on its first run.

## v0.1.2

GPU buffer failures no longer stop a game, and the groundwork of a Windows port lands. Windows is
not supported yet: `psport.exe` starts under Proton and stops when it creates its window. The
launch interface does not change.

### Graphics
- When the GPU cannot allocate a buffer, psport frees what it can and tries again over the memory
  budget; if that fails too, it skips the copy, draw or dispatch that needed the buffer instead of
  stopping. Each failure is logged and counted in the call summary.

### Windows (groundwork, not supported)
- The platform layer for guest memory, faults and thread interrupts, files, processes and sockets
  exists, and `psport.exe` and the GPU library build with MSYS2's CLANG64; a CI job on `develop` and
  `main` publishes them as an artifact to try, never as a release.
- Under Proton, `psport.exe` creates its window and Vulkan surface and stops at the guest-memory
  check (`Guest memory: direct memory cannot be shared with this driver`); no game has run. `--list-features` still reports every Windows capability as experimental or
  unavailable.
- For contributors: `tools/win32-cross.sh` builds the Windows binaries on Linux, and
  `tools/win32-proton-run.sh` runs them under Proton and ends the prefix's Wine processes afterwards.
- Linux runs through the same code paths as before (sockets, trophies, mutex waits, the install
  folder lookup, which now also accepts the nearest folder above the executable holding `patches/`).

### Documentation
- The README names the AMD Radeon RX 7900 XT with Mesa (RADV) as the tested machine.
- xbyak 7.43 ships in `gpu/third_party/xbyak`, so builds no longer need it from the Nix shell.

## v0.1.1

Fixes and ports on v0.1.0: bbport 0.6-pre1's and shadPS4's later fixes ported by hand, firmware
behaviour read from the original libraries, and keyboard and mouse support for more titles. The
launch interface does not change. The Windows port is not part of this version.

### Packaging and licences
- The bundle and the Nix cache leave out the online module (`libpsport-net.so`) until its mbedTLS
  port (#850), and ship the third-party notices and the source offer; `--list-features` reports
  `online_module` as unavailable on such builds, so launchers can hide the online settings.
- A licence check gates every release and cache publication; files that mix licences now say
  `GPL-2.0-only` in their headers.
- The README keeps only each game's state (playable, experimental, broken, not supported); the detail
  is in `docs/en/GAMES.md`, `docs/es/GAMES.md` and `docs/ru/GAMES.md`.
- A smaller Python in the package (678 to 602 MiB); the Nix cache also holds the newest 20 branch
  commits for launchers that follow `develop`.

### Launching and diagnostics
- `PS_CACHE_DIR` puts the shader and pipeline caches, and the drivers' caches, on another disk.
- `PS_DEBUG=1` gathers a run's logs, calls, faults, frame statistics and settings in one folder.
- `psport --list-controls` prints the title's default controls; `--list-features` also reports
  which upscalers and CPU fixes a machine can run.
- The GPU is kept by name, and a game's 21 text languages are available.
- Fixed: `run.sh` quitting silently at 4K, 1440p or 720p, a crash at start without `HOME`, a lost
  log when the game is stopped as a group, and an incomplete exit report on a window close.
- The default user name is `Player`; `PS_USER_NAME` overrides it.

### Loader and system libraries
- Mods from mod sites are recognised in more layouts and rejected folders say why; an external patch
  replaces a profile patch it overlaps instead of stacking on it.
- Mutexes, read-write locks, time zones, Rtc arithmetic, AvPlayer, Voice, MsgDialog and the network
  address functions follow the firmware's results; libSceUlt sync objects, JpegDec, LoginDialog and
  many libkernel calls are native.
- Dialogs and the IME can be answered with a controller alone, including typing text.
- Ngs2's mastering limiter and LFE filter run; 7.1 audio folds to stereo with the fronts at full level.
- Optional movie skip (`skip_movies=1`).

### Graphics
- Shader recompiler, texture cache and command processor fixes from shadPS4; cached shaders are
  rebuilt once.
- Fewer stops: assertions in the caches, GNM and VideoOut became counted fallbacks, and shader and
  depth buffer bugs that hung the GPU (Blood Omen 2, Gal Guardians 2) are fixed.
- Better behaviour on small VRAM (APUs) and when VRAM fills; faster tiling and shader specialization.
- Temporal upscalers reset on camera cuts, so cutscenes no longer flash.

### Input
- Short taps are no longer lost; the keyboard stick keeps a sprint through direction changes; View +
  d-pad clicks the touchpad halves.
- Call of Duty: Black Ops 1 and 2 play with keyboard and mouse; Atelier Ayesha DX draws its button
  glyphs.

### Bloodborne
- Play time counted in real time, replay ghosts at the right speed, a crash with `PS_RENDER_RES` above
  1080p fixed, stale external FPS patches ignored.
- Workarounds from bbport 0.6 declared as quirks (`PS_QUIRKS`), including the Intel tone mapping fix;
  new Disable Bloom switch.
- Seamless co-op gives its kit to new characters only with a verified save backup.

### Online
- Network calls Bloodborne polls (epoll abort, HTTP status, `sceNetInit`, SSL contexts) no longer show
  as failures.

## v0.1.0

The first version of psport as an independent project: bbport's native Bloodborne port taken to a
compatibility layer for PS4 titles in general, on shadPS4's video core, with bbport 0.6-pre1's
fixes and features ported by hand.

### Launching
- `run.sh` and `out/psport` launch any dump (`PS_PERMISSIVE=1` for titles other than Bloodborne);
  data, saves, shader caches and `psport.ini` in `PS_DATA_DIR`.
- Launcher queries print JSON: `--list-patches`, `--list-mods`, `--list-quirks`, `--list-traits`,
  `--list-effects`, `--list-features`, plus `--check-game`, `--has-presets`, `--online-overrides`.
- `BB_*` variables and `bbport.ini` from bbport still work for a transition, with a deprecation line.

### Loader and system libraries
- The game's eboot and modules load straight from its folder; mods, XML patches and middleware
  detection (Unity, Mono, Unreal, FMOD, Wwise, Havok).
- System libraries are implemented natively; no tested title needs a dump of system modules
  (`PS_SYS_MODULES` stays optional, for comparison).
- Trophies kept per user; system fonts from a dump or the host's Noto fonts.
- Every unimplemented, stubbed or failing call is logged and counted in the exit call report.

### Graphics
- Cross-vendor Vulkan 1.3; the game's memory used in place on every GPU (bbport's 0.3 memory model
  and its variables are gone).
- Malformed command buffers, untranslatable shaders and unsupported formats are skipped and
  counted instead of stopping the game.
- Upscalers FSR 3.1, FSR 4, FSR 4.1.1, native TAA and DLSS where a game profile places them;
  output aspect ratios from 4:3 to 32:9.
- Occlusion queries counted for real by default (Bloodborne +8 %).
- The pipeline cache is written atomically and kept across exits.

### Input
- Key and button bindings with chords; a binding for the port's menu (`key.menu`, `pad.menu`).
- Real gyroscope and accelerometer of DualShock 4, DualSense, Switch Pro and Steam Deck; motion and
  touchpad emulation for other controllers (`pad_motion=1`).
- Host mouse and keyboard for games that use them.
- Emulated LEGO Dimensions toy pad (`toypad=1`), off by default.

### Bloodborne
- Frame rate by hooks up to 480 FPS, mouse camera and menu pointer, live aspect ratio, button
  prompts for the keyboard or gamepad in use.

### Online
- Seamless Hunt's shadNet server by default; the client reports its psport version.
- P2P relay fallback; Bloodborne seamless co-op (BBSC) as an opt-in switch with a local game
  server.

### Platform and tools
- `ps_host_*` platform layer with a capability table (`--list-features`); Linux implemented.
- Declared quirks per host or title, listed and disableable (`PS_QUIRKS`).
- Releases ship a relocatable bundle and a Nix binary cache; `tools/regress.sh`, the night script
  and Tracy/perf/RenderDoc profiling for development.
