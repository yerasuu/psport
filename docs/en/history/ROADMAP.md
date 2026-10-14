# Native Bloodborne port: what is done and what is next

The main text is the state as of 2026-09-26; below, as the first section, is what changed by 30.09.
The most detailed snapshot: [CURRENT_STATUS_2026-09-29.md](CURRENT_STATUS_2026-09-29.md).
Report on the work of 30.09–01.10 (GPU pipeline, upscaler, crash, launcher, AppImage):
[SESSION_2026-09-30.md](SESSION_2026-09-30.md).
Details on individual topics:
[parallel_gpu.md](../parallel_gpu.md) (GPU multithreading, stutters during streaming),
[upscaler.md](../upscaler.md) (game frame analysis, scene constants).

## Update 30.09

- Done after 26.09: FSR 4 INT8 (v07), object motion vectors, preset change without restart
  (reduced scene targets; D32S8 depth is copied by a shader), LTO and PGO, scaling of draw
  preparation to the thread count (Steam Deck too).
- Problems from the "Known problems" section below: item 3 (restart on preset change) is solved;
  item 1 (object vectors) is implemented, FSR 4 still has ghosting on the character at Ultra
  Performance; item 2 (blurry interface) — the UI is drawn at 1920×1080.
- Limitation: reduced scene rendering (`SceneTargets::Eligible`) requires a 1920×1080 target, that
  is, it only works with 1080p output.
- Parallelising the GPU command thread (step 2 of [parallel_gpu.md](../parallel_gpu.md)): textures
  on a separate thread in parallel with buffers work correctly but give no gain — each draw's data
  moves between cores. The bottleneck is the GPU command thread: 91% on CPU, ~9% waiting for small
  guest memory copies before a fence. Next — a two-stage pipeline (a second thread owns the texture
  cache and draw recording, register copy from scanner deltas) or memoisation of texture bindings
  across frames; see "What would scale" in parallel_gpu.md.
- **Two-stage draw pipeline** (`vk_draw_pipe.h`): the GPU command thread decodes PM4 and selects
  the pipeline, a second thread (`bb:DrawRec`) with its own register copy binds resources and
  records the draw. +19% FPS on 16 threads, +18% on 8 (like Steam Deck). Enabled by default with
  8+ threads (`BB_DRAW_PIPE=0/1`). The bottleneck is now the second stage; details and plan in
  parallel_gpu.md.
- Presets and GPU load: the scene costs little on an RX 7800 XT, so the preset changes GPU time by
  only ~0.5 ms, while FSR 4 itself at 1080p output costs ~1.4 ms (FSR 4 Ultra Performance is more
  expensive than native rendering without an upscaler). Fixed: lighting passes stayed at 1920×1080
  with any preset (the check looked at other target slots); reduced targets are now sampled
  directly, without resampling to native (the shader cache is rebuilt once). Next for Steam Deck —
  the guest memory copy of render targets `3d5ebf4e` (see parallel_gpu.md).
- **Fixed crash with the pipeline** (guest heap corruption, guest offset `0x263b8e7`, after 13–17
  minutes): the "GPU idle" interrupt went out before EOP labels deferred to the Vulkan recording
  thread, and the game freed objects with those labels. Now the pipeline and deferred labels are
  waited for before it; 2 runs of 30 minutes without crashes (before the fix — 7 crashes out of 7).
- **Output resolution 1440p/2160p** ("Output resolution" (Разрешение вывода) menu, bbport.ini
  `output_res`, after restart): the scene is drawn at the preset's resolution relative to the
  output, the upscaler (FSR 3 or FSR 4) completes the frame, the interface is rasterised at the
  output resolution. Game effects (aberration, DoF, blur, SSAO, game anti-aliasing, dynamic
  shadows, SSR, cutscenes, LOD) — there as well.
- **Launcher** (`launcher/bb-launcher.sh`, GTK4/libadwaita) and **AppImage** for Steam Deck
  (`bash packaging/appimage.sh` → `dist/`; data in `~/.local/share/bbport`, `--play` — launch
  without the launcher window).
- Tools: `BB_PAD_FILE` (scripted gamepad input), `ab.sh` (A/B of `BB_TOGGLE_FILE` bits in the
  running game), idle and blocking of the GPU thread in `Frame stats`.

## How the port works

- The original eboot (CUSA03173 v01.09) runs directly on x86-64. The PS4 system libraries are
  replaced by our own HLE runtime in C (`runtime_*.c`, `probe.c`).
- Graphics: shadPS4's video core and shader recompiler (GPL), built into `libbbgpu.so` (`gpu/`),
  with our own modifications. The shadPS4 emulator as a whole is not used.
- Game patches (FPS, resolution, etc.) — community XML (`patches/Bloodborne.xml`), compiled by
  `patches.py` and applied by the loader at startup.

## Done

### Performance and smoothness

| What | Result |
|---|---|
| Vulkan command recording on a separate thread, texture/buffer caches, rwlock instead of a mutex | 26 → 64 FPS in one scene |
| Draw call preparation on 4 threads (`bb:DrawPrep`), check on the GPU thread | +11.5% |
| 256 KiB unprotect window when the game writes to memory (fewer page faults) | +22% |
| Texture description cache, fast path for the same render targets | up to ~94 FPS outdoors |
| Guest memory copies on a thread pool, staging buffer pool | streaming freezes 60–170 ms → 40–50 ms |
| Waiting for copies before game-visible fences | interface flicker removed |
| "Readbacks Relaxed" mode | no "vertex explosions" (FaceGen) |
| 480 Hz vblank + frame limit up to min(monitor, 120), output immediately on flip | even frame pacing; running does not slow down above 120 FPS |

Everything is toggled during gameplay via `BB_TOGGLE_FILE` (bitmask) for A/B comparisons.
Statistics: `BB_FRAME_STATS=1`.

### Sound, input

- Audio output at the correct tempo (previously clicks every 10.7 ms).
- Gamepad via SDL3, fallback keyboard layout.

### Frame analysis

- `BB_CAPTURE_TRIGGER=<file>` records one frame: passes, targets, shaders, textures, constants,
  blend modes.
- The structure of a Bloodborne frame was worked out (G-buffer, lighting, RGBA16F scene colour,
  fog, post-processing, tonemapping, game anti-aliasing, interface), as well as the 864-byte scene
  constants (view and projection matrices).

### Temporal upscaler (FSR 3.1)

- **Camera motion vectors**: from scene depth and the current and previous frame matrices
  (`vk_camera_motion.cpp`, `camera_motion.comp`). Verified by reprojecting the previous frame:
  static geometry matches.
- **Jitter**: subpixel offset of scene geometry via the viewport, Halton(2,3) sequence; number of
  phases 8·(output/render)². The interface and post-processing are not offset.
- **FSR 3.1** (FireBurn/FSR-Vulkan, native Vulkan, `gpu/third_party/fsr-vulkan`):
  - *Native AA*: on the HDR scene colour before post-processing, 1:1.
  - *Quality/Balanced/Performance/Ultra Performance presets* (x1.5/1.7/2/3): the game renders at
    reduced resolution (a resolution patch that `patches.py` derives from the community 720p
    patch), FSR upscales the finished frame to 1920x1080 before the first interface pass. The
    interface and output pass are redirected into full-size images, the presenter shows them
    instead of the game's buffer.
- **Reactive mask** (a scene snapshot before transparent draws, comparison before fog is applied):
  works, but on Bloodborne's haze it causes shimmering — disabled by default.
- **In-game menu** (Dear ImGui, Insert or L3+R3): upscaler, preset, sharpness, jitter, mask, FPS
  counter. Settings in `bbport.ini`. Preset change — the "Apply and restart game" (Применить и
  перезапустить игру) button.

## Known problems

State of the last commit. The working tree has a prepared move of presets to reduced render
targets while keeping guest buffers, post-processing and UI at 1920×1080. Transitions between
presets and colour/depth/stencil copying passed a Vulkan test on Lavapipe. The RX 7800 XT does not
support D32S8 blit, so the new path left the FSR input at 1920×1080 and lowered FPS. For such GPUs
startup now automatically selects the verified path with the game resolution patch and UI at
1920×1080; a preset change again requires a restart. On GPUs with the required support, switching
without restart remains. Items 2–3 below refer to the last commit, before these working tree
changes.

The general object motion vector pass was too expensive: Performance gave about 49 FPS with it and
77–78 FPS without it. Now the extra target and shaders are enabled only for G-buffer draws with a
pose/bone buffer; the rest use camera vectors. In a trial run the selective path held about 81–87
FPS, with no skips due to history capacity. Object vectors are enabled by default;
`BB_OBJECT_MOTION=0` disables them, and `BB_OBJECT_MOTION_ALL=1` restores the old general path for
comparison. Selection by buffer size is heuristic for now: the quality of weapons and clothing in
motion needs to be checked in game.

1. **Moving objects "fall apart"** (weapon on the back, character, cloth, enemies): there are only
   camera motion vectors. The third-person camera follows the character, so camera vectors are
   wrong for it, and the upscaler takes history from the wrong place. This affects any temporal
   upscaler (FSR 3/4, DLSS, XeSS).
2. **The interface is blurry in presets**: with the resolution patch the game considers itself a
   540p game and apparently prepares fonts and interface elements for that resolution. Menus
   (title, pause) do not go through the upscaler at all.
3. **A preset change requires a restart**: the resolution is set by a code patch at startup, and
   the game allocates its buffers once.

## Plan

### 1. Object motion vectors

Needed by all upscalers and frame generation. The game does not compute them, so we compute them
ourselves.

How:
- For each draw into the G-buffer (passes with 5+ targets) the recompiler makes a second variant
  of the vertex shader that outputs the position twice: with the current constants and with the
  previous frame's constants. The difference of projections is the motion vector, written to an
  auxiliary RG16F target as an extra pixel shader output.
- Previous frame constants: for each draw we remember the contents of constant buffers (object
  matrix, skeleton bones) keyed by "shader + draw number in the pass" or by data address. We need
  to find out how the game stores skeleton matrices: in buffers overwritten every frame (then we
  copy the previous version), or in a ring buffer (then the previous frame's address is enough).
- Where the object is not found (first frame, new object) — camera vectors, as now.
- Check: debug view of the vectors (`BB_DEBUG_MOTION`), reprojection of the previous frame with the
  full vectors, visually — the saw on the back while running.

### 2. Scaling at the renderer level (instead of the resolution patch)

The game runs at 1920x1080 (or at the monitor resolution), and only the 3D scene passes are drawn
at reduced resolution. The interface and menus stay sharp, the preset changes on the fly.

How:
- The texture cache allocates a reduced-size copy for scene targets (G-buffer, depth, lighting,
  scene colour, effects); guest memory and the game's descriptions do not change.
- Viewport and scissor of passes into these targets are scaled (as already done for the interface,
  including passes with clipping disabled).
- Shaders that compute by pixel coordinates (`gl_FragCoord`, `texelFetch`, per-pixel compute
  shaders) get a scale factor via the recompiler's push constants. Samples by normalised
  coordinates work unchanged.
- FSR goes in the same place as Native AA: scene colour before post-processing (HDR, best
  quality); post-processing and the interface run at full resolution.
- Risks: passes reading scene targets by pixel coordinates in post-processing, and reads of these
  targets from guest memory (they must be forbidden or scaled back).

### 3. Other upscalers

All use the same inputs (colour, depth, motion vectors, jitter, masks).

- **FSR 4 (INT8)**: FSR-Vulkan has an experimental FSR 4 v07 INT8 provider — add it as a menu
  option and compare with FSR 3.1 after step 1.
- **DLSS**: native Linux Vulkan SDK (NGX). Does not work on an RX 7800 XT, but needed for NVIDIA
  users.
- **XeSS / XeFG and OptiScaler**: Intel ships them only as Windows DLLs. Options: an in-process
  Windows DLL (PE) loader, or running the whole build under Proton with OptiScaler, to which we
  pass the inputs via the DLSS interface.
  Checked 29.09: libxess.dll (identical in DOOM: The Dark Ages, PRAGMATA, etc.) has a Vulkan path
  (`xessVKCreateContext`, `xessVKExecute`), but imports ~300 functions: MSVCP140 (iostream),
  VCRUNTIME140 (C++ exceptions), UCRT, kernel32, setupapi, dxgi.
  Decided to postpone; the most realistic path is a helper under Wine with shared Vulkan memory.

### 4. Frame generation

- FSR 3.1 Frame Generation (available in FSR-Vulkan): an intermediate frame between two finished
  ones, using the same motion vectors; the interface must be drawn on top separately (interface
  redirection exists — it can be rendered into a separate layer).
- XeFG — after a solution for Windows DLLs (see above).
- Requirement: frame delivery through the presenter (queue and VRR already work).

### 5. Other

- Steam Deck: portable build (patchelf, without Nix), testing presets at 1280x800.
- Further offloading of the GPU thread (step 2 of [parallel_gpu.md](../parallel_gpu.md)).
