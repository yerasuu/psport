# Temporal upscaling and frame generation — frame analysis and plan

## Frame generation

`frame_gen=fsr` in `psport.ini` (`PS_FRAME_GEN=fsr`) turns on FSR 3.1 frame generation (frame
interpolation 3.1.6 and optical flow from FireBurn's FSR-Vulkan, native Vulkan): between two
finished frames of the game the presenter shows one generated frame, so the display gets two images
per game frame. It is off by default and set up at start (the presentation frames are composed in
RGBA8 with it, whose storage every device supports, and the swapchain gets one image more); a game
started without it makes nothing of it. `auto` and `dlss` are recognised but not available yet: they
are logged and leave it off.

- Capability: chosen by what the device reports, never by vendor. At start, before the swapchain is
  made, `FrameGenerator::IsFsrSupported` asks FSR-Vulkan for `fsr3FrameGenerationPrerequisites`
  (a compute queue, the subgroup operations, the FSR 3 storage formats,
  `shaderStorageImageWriteWithoutFormat`, timeline semaphores) and checks the device was created
  with `shaderStorageImageWriteWithoutFormat`; without them, or without the game profile's
  upscaler placement, `fsr` is turned off with the reason logged. `psport --list-features` reports
  the same probe as the `frame_gen_fsr` row, in the `upscaler` area so that launchers treat it as
  an option, not as something a game needs. The in-game menu offers the switch only where the probe
  says yes.
- Switching while the game runs: in a game started with it on (`PsSettings::is_frame_gen_live`),
  the menu switches it off and on at once. Off frees the FSR 3.1 context (about 100 MB at 1080p) and
  the depth, motion and UI-less copies once the GPU has passed the commands that may still capture
  into them (`FrameGenerator::Release`, logged as `Frame generation: off; FSR 3.1 context (N MB)
  and its inputs (N MB) freed`), and the presenter's generated frames and its UI-less frame; game
  frames shown meanwhile count as `switched off in the menu`, and the FIFO cap is lifted. On again
  makes them at the next scene frame (`Frame generation: on again`). The RGBA8 frames, the extra
  swapchain image and the doubled pass buffers stay for the session, as they cost little. A game
  started with it off needs a restart to turn it on; what that would take is below.

- Launchers: every change in the menu prints `Frame generation setting: off` or
  `Frame generation setting: fsr` on a line of its own (flushed), which a launcher reads from the
  run's output to keep the choice per game (decision #1146, as decanter learns `frame_overlap_off`).
  When `PS_FRAME_GEN` is set the launcher owns the value: it wins over `psport.ini`, the menu says so
  and does not write `frame_gen` to `psport.ini` (one file for every game), and the menu's restart
  sets `PS_FRAME_GEN` to the menu's choice so that run.sh starts with it.
- Inputs: the scene depth (copied to R32F) and the motion vectors (`CameraMotion`, camera and object
  motion) at the upscaler's trigger, so it needs the game profile's upscaler placement and runs with
  the upscaler off too. Frames without them (menus, movies, loading) are presented alone and the
  next generated frame starts over.
- UI: the picture before the UI is copied at the profile's first UI draw (the 1080p UI target, or
  the output-size UI image of the scaled presets) and composed through the presenter's passes like
  the frame; FSR 3.1 interpolates that picture and the frame's UI is drawn over the result where the
  frame and the picture differ (`frame_gen_ui.comp`). `PS_FRAME_GEN_HUDLESS=0` interpolates the UI
  too (diagnostics).
- Pacing: the generated frame is presented first and the game's own half a smoothed game frame
  later; it is dropped when the game is waiting for a presentation frame. When presenting takes
  nearly the whole game frame for about ten frames in a row (FIFO on a slower display, a GPU too
  slow) generation pauses for 5 s, doubling up to 120 s. `frame_gen_fit_display=1`
  (`PS_FRAME_GEN_FIT_DISPLAY`) generates only while the display's refresh rate has room for both
  images. With FIFO (V-Sync) presentation and a frame limit the game is capped at half the
  display's refresh rate (VideoOut's limiter, `PsSettings::frame_gen_flip_limit`).
- Counts: the FPS counter adds the game's own rate; the exit report prints a `Frame generation:
  summary:` line (game frames, generated, presented, dropped, frames without capture, held,
  pauses, failures, the flip limit) and a `Frame generation: game frames without a generated frame:`
  line counting each game frame shown without one by reason (HDR output, without capture, paused,
  no room on the display, no memory, after a failure, dropped because the game waited, no swapchain
  image); the first frame of each reason is logged when it happens. A third line,
  `Frame generation: without capture:`, splits the frames without capture into those where the game
  profile's upscaler trigger never ran (no scene pass: menus, movies, loading) and those where it ran
  without a scene depth, gives the game time spent without and with capture, and counts the
  generated frames whose UI was interpolated (no picture before the UI) and those that restarted
  FSR 3.1's history (a camera cut, or the first after a frame without capture). Frame counts alone
  mislead: in night n1029 (Bloodborne, `bloodborne-perf.sched`) 28797 of 43401 game frames had no
  capture, all of them the title menu and loading flipping uncapped at the 480 Hz vblank (about 60 s
  at 480 FPS by the `Frame stats` lines, 6 to 24 draws a frame, no scene pass), while the ~90 s of
  play at 160 FPS that followed (about 14500 frames) got 14605 generated frames, none held or
  dropped; the validation step, playing at 50 FPS, had the same ~28000 menu frames and 4259
  generated.
- HDR output presents without generated frames. DLSS Frame Generation is NVIDIA-only and is not
  in the port.

### Turning it on in a game started without it (not done)

What depends on the start-time choice, in `gpu/shadps4/video_core/renderer_vulkan/` (develop after
#1165):

- Frame format: `vk_presenter.cpp:184` fixes `m_frame_format` (RGBA8 with it, else the swapchain's
  format, which may be B8G8R8A8 without storage support or 10-bit for HDR); `RecreateFrame`
  (`:261`) makes every presentation frame in it, `pp_pass.Create(device, m_frame_format)` (`:212`,
  `host_passes/pp_pass.h:23`) builds the post-process pipeline for that one attachment format, and
  `DumpFinalFrameIfDue` (`:990`) reads it on the present thread. Switching needs the frames remade
  in the new format (lazily in `GetRenderFrame`, `:1162`, as a size change is) and the post-process
  pass to hold a pipeline per format chosen per frame, since frames of both formats coexist until
  each has come back.
- The last presented frame is shown again by the present thread (`Present(last_submit_frame, true)`,
  `:781`, `PrepareLastFrame`, `:339`) while it sits in the free queue, where the GPU thread may
  remake it: a size change has the same window, a menu switch would open it on demand.
- Pass buffers: `c_pass_images` (`:187`) sizes the FSR pass's per-image buffers and
  `PsOverlay::Init` (`:215`) for two images per game frame only with it on; both would have to be
  made for the larger count at start (or remade with every frame idle).
- Swapchain: its image count (`vk_swapchain.cpp:306`) follows `is_frame_gen_live`; a switch would
  recreate it on the present thread under `Scheduler::submit_mutex`, as a resize does.
- Generator: `m_p_frame_generator` is made in the constructor (`:179`) and handed to the upscaler
  (`:180`, `vk_temporal_upscaler.h:71`), which reads it on the GPU thread without a lock; it would
  have to be made on the GPU thread at a frame boundary, and its exit summaries registered once
  (`vk_frame_generation.cpp:148`).

Each is ordinary work, but together they change the off path every game runs (frame format,
pass buffers, the free-queue reuse) and need a handshake between the GPU, present and recording
threads that only a person at the display can check (no black frame, no stutter, memory back
down); the night harness cannot switch the menu. Hence the restart for now.

## Intermediate targets under live scaling (2026-10-22)

Live scene scaling gives proxies only to targets of the scene size (1920x1080) and of the
post-processing size (960x540, `post_divisor`). Targets the game sizes from its scene at other
ratios stayed native: Bloodborne's depth of field blur buffers (1176x441, 2352x882 when the
startup patch renders 4K) were sampled by passes drawn at the larger scene size, which came out
blurred above 1080p. `PS_SCALE_REDUCED_TARGETS=1` makes `SceneTargets::IsIntermediateTarget`
also accept color targets from a quarter of the scene size up to it, other than power-of-two
sizes; their proxies are their size times the scene factor, rounded to whole pixels. Depth
targets, smaller reductions and LUTs keep the native size. Off by default until a game run
checks it (kb #787).

## Output aspect ratios (2026-10-17)

`aspect_ratio` in `psport.ini` (`auto`, `4:3`, `16:10`, `16:9`, `21:9`, `32:9`; 16:9 unless chosen,
in the overlay or the launcher, or given by a launcher that detects it in `PS_ASPECT_RATIO`, which
wins over the file) picks the group of output resolutions `output_res` is taken from
(`PsSettings::Outputs`, the same lists in `launcher/psport_launcher.py`); another aspect ratio
keeps the height (`NearestOutput`), and `auto` is the group nearest the monitor the window is on
(`SetMonitor`) plus the monitor's own size. The game keeps its 1920x1080 frame. When the output's
aspect ratio changes, the upscaler tells the profile (`Game::Profile::pfn_display_aspect`): a
profile whose camera and UI follow it (Bloodborne) returns true, and its scene and UI fill the
output, the UI drawn at `UiComposition::Scale` (output / 1920 by output / 1080) because its movies
draw their wider view squeezed into the 1920x1080 frame. Its full-screen fills reach the output's
edges: the UI's draws in native coordinates get `UiComposition::EdgeParam` in the push constant
`motion_param`, and every vertex shader but the motion vectors' moves a vertex on or past a corner
of the 16:9 screen (or on a side edge in the lower part: subtitles' backdrops) to the output's
(`EmitUiEdgeStretch`; skipped for stencil-masked draws and textures of 2048 or more, so pictures and
text keep their proportions; `ShaderBinaryVersion` 12). A frame without a scene keeps the 16:9
screen in the middle and black past it. Any other game keeps its 16:9 picture: its output is the
largest 16:9 size inside the chosen one, and the presenter puts it in the middle.

## Native TAA and missing FSR 4 assets (2026-10-01)

`upscaler=taa` / `PS_UPSCALER=taa` is a separate native-resolution temporal AA
mode, available live in the launcher and overlay. It uses the existing jitter and
camera/object motion vectors, reprojection, depth rejection and 3×3 neighborhood
clipping in one compute pass with two ping-pong RGBA16F histories. History stores
depth in alpha; the scene's original alpha is preserved by the HDR merge. HUD is
composed afterwards and never enters history. Output changes and AA/provider
changes reset history. TAA uses Native AA dimensions while preserving the saved
FSR preset, and does not allocate an FSR context or execute an FSR model/RCAS.
It still adds GPU work compared with disabling temporal AA; native 4K raster cost
also remains. The explicit `PS_RENDER_RES` compatibility path must be unset.

FSR Native AA is not a fast pass-through: it renders at the output resolution and
also executes the temporal reconstruction (including the model for FSR 4) and
optional sharpening. It can therefore be slower than no AA at the same output.

FSR 4.1.1 assets have separate 1080p/2160p tiers and standard/Ultra models:
`t1080_m0`, `t1080_m1`, `t2160_m0`, `t2160_m1`. Outputs above 1080p need the 2160
tier, irrespective of the preset. A missing `t2160_m0/spd.spv` means missing
assets, not a GPU feature failure. These assets are not bundled: build the full
set with `tools/fsr4cap/build_assets.sh` and put it in `PS_FSR411_DIR` or the
packaged data directory's `fsr4_411`. Fatal asset failures now select FSR 3.1 in
the live settings, so the menu shows the active provider, retains the error, and
allows retrying FSR 4 after changing the output/installing assets.

`out/taa-live-validation.log` and `out/taa-appimage-validation.log`
check TAA at 1080p/720p/1440p/4K, TAA ↔ FSR 3, and a missing 2160-tier FSR 4.1.1
model in one gameplay process, including output captures and camera movement.
`out/taa-launcher-validation.log` checks actual GTK controls and saved settings.
TAA has not yet been tested on the tester's GTX 1060.

## Startup patch is the default again for outputs other than 1080p (2026-10-02)

The live path below made the Steam Deck and a GTX 1060 + 4-core Haswell drop to 7–8 FPS
(release 0.1 AppImage: 40–50 FPS on the Deck). With guest allocations at 1920×1080 the game's
post-processing stays at 1080p even for 720p output, guest compute passes resolve scene proxies
back to 1080p every frame (RX 7800 XT, 720p FSR 3 Performance on 4 cores: 826 vs 576
draws/frame, 185 vs 210 FPS), and on GPUs without shader stencil export every depth/stencil
copy took nine draws.

- `run.sh` again patches the game's render size for outputs other than 1080p (as in 0.1) and
  marks it with `PS_AUTO_RENDER_RES=1`, so an in-game restart recomputes it. Preset and output
  changes then need a restart (the menu offers it). `PS_LIVE_RES=1` selects the live path.
  1080p output and TAA keep the live path.
- Depth/stencil scene proxies need `VK_EXT_shader_stencil_export` again; without it these
  targets stay native (as in 0.1). `PS_SCENE_STENCIL_BITS=1` still forces the portable
  resampler for tests.

## Live resolution changes (2026-10-01)

The in-game output selector now resizes 720p/1080p/1440p/2160p host targets at a
display-pass boundary. Presets use `output / scale`, including raster sizes above
1080p (4K Quality: 2560×1440; Native AA: 3840×2160). Guest allocations stay at
1920×1080; `run.sh` no longer inserts a resolution patch for normal output choices.
The explicit `PS_RENDER_RES` compatibility override is retained.

`SceneTargets` resolves and retires old proxies before changing size. FSR resources
and history are rebuilt for the new input/output, with completion waits for both
FSR 3 and FSR 4. The 1080p path keeps HDR reconstruction before game post;
other outputs reconstruct tonemapped scene proxies at the first Scaleform pass,
then draw HUD/menu geometry into the output-size target. The display buffer is
resized too. Movie shader identification prevents a native-size fullscreen post
pass from being mistaken for UI.

Guest compute passes retain their native dispatch sizes and can resolve proxies
back to 1080p. This change does not scale every post-processing pass; performance
and intermediate detail can differ from the old startup-patched render sizes.

Verification: `out/dev/live-resolution-summary.log` switches output, preset and FSR
off/on in one PID. `PS_PRESENT_DUMP_TRIGGER` captures the completed host display
buffer (including HUD) into `PS_DUMP_DIR`. Captures confirm actual 720p/1440p/2160p
buffers. `PS_PRESET_FILE` accepts
`preset [output-index [upscaler-index]]` for scripted menu-equivalent changes.

Frame analyzer: `PS_CAPTURE_TRIGGER=<file> PS_CAPTURE_DIR=<dir>`; creating the file records
the next frame (boundary: the pass writing a display buffer) — passes, targets, shaders,
sampled textures, and the first 1 KiB of bound constants for small passes and first draws.

## Bloodborne's frame (1920x1080, Hunter's Dream / Nightmare)

| Passes | What |
|---|---|
| first pass of a frame | copy of the previous UI target into the display buffer (sRGB) |
| shadow | 4096x4096 D32 depth |
| G-buffer | 6 targets 1920x1080 (RGBA8 x3, sRGB albedo, B10G11R11, RGBA16F) + D32S8 depth |
| lighting | light volumes into two B10G11R11 targets |
| scene color | RGBA16F 1920x1080, also forward/transparent draws and effects |
| volumetric fog | compute, reads linear depth (R32F 1920x1080) and composites into scene color |
| half-res effects | RGBA8 960x540 with half-res depth |
| post | combine/bloom pyramid in RGBA16F / B10G11R11 |
| tonemap | into RGBA8 1920x1080 (the UI target) |
| game AA | ping-pong RGBA8 1920x1080 after tonemap (to be skipped when upscaling) |
| UI | stencil-masked draws over the same RGBA8 target |

There is no velocity buffer, also with the camera moving: motion vectors are computed.

## Scene constants (864 bytes, bound by most passes)

Signature: `[0]=3000 (far) [1]=1/3000 [4]=1920 [5]=1080 [6]=1/1920 [7]=1/1080`.

| Floats | Meaning |
|---|---|
| 8–19 | view matrix, 3x4 rows (rotation + translation); the only block that changes with the camera |
| 36–51 | inverse projection (0.700285 = 1/1.42799, 0.39391 = 1/2.53865) |
| 52, 57, 62, 63 | projection (rows 52–67), D3D depth 0..1: x 1.42799, y 2.53865, z 1.00002 / -0.0500679 (near 0.05, far 3000) |
| 112–175 | shadow cascade matrices |
| 180–191 | inverse view (camera to world), 3x4 (176–179: 2, 8, 15, 0) |

The previous frame's matrices are not there; the port keeps them itself. Bloodborne's profile hands
this layout to the core (`Game::CameraConstants` in `upscaler.camera`); a game without it has no camera.

## Plan

1. Find the scene constants every frame (signature), keep the previous view/projection.
2. Camera motion vectors: compute pass from depth and current/previous matrices into an
   RG16F target; debug view to check them. Object motion later (vertex shader replay with the
   previous frame's constants through the recompiler).
3. Jitter: sub-pixel offset of clip-space positions in the scene passes (recompiler).
4. Upscaler at the scene color stage (before post and UI): FSR 3.1 first (open, native Vulkan,
   also frame generation), then DLSS (native on Linux), XeSS/XeFG and OptiScaler through a
   loader for their Windows DLLs.

## Status

- Camera motion vectors: verified by reprojecting the previous frame (`PS_DEBUG_MOTION=1`,
  toggles 1<<22 reprojected frame, 1<<23 error map): static geometry matches.
- FSR 3.1 (`PS_UPSCALER=fsr3`, FireBurn/FSR-Vulkan in `gpu/third_party/fsr-vulkan`): scene color before the
  post-processing combine (compute shader 9a9cf8a9), 1:1, RGB written back (the game keeps data
  in the alpha). Toggle 1<<24 switches it off at run time.
- Jitter: viewport offset of scene geometry (drawn with the scene depth, not full-screen
  quads), Halton(2,3) 8 phases; sign checked by sharpness (correct 255, flipped 208, off 271 —
  1:1 jitter trades high-frequency aliasing for a slightly softer image). Toggle 1<<25 off.
- Frame-to-frame difference while standing still: ~33% lower with FSR.
- Missing: motion of animated objects (characters, cloth, foliage), reactive/transparency
  masks for particles and fog, render-resolution scaling, frame generation.

## 2026-10-01: FSR 4 at 1440p/2160p output did not work (flickering and jitter)

Video `video_2026-10-01_03-58-59.mp4` (output 3840x2160, FSR 4 Performance): flickering and jitter
of all objects. Cause: the upscaler did not run at all in this mode, while jitter stayed
enabled: the screen got a stretched scene frame, each frame shifted by its own Halton phase.

1. The UI pass was identified by the exact `PS_RENDER_RES` target size (1916x1078), while the game
   allocates targets with an aligned height (1916x1080; the scene constants also say 1916x1080).
   `RunScaled` was never called (the log had no `UI: native composition`). Alignment up to
   8 pixels is now allowed (`RenderTarget`), and the scene size for FSR is taken from the scene
   constants (`CameraMotion::RenderSize`, `SceneSize`).
2. After that FSR 4 failed with `external image registration failed (-1000069000)`: `RunScaled`
   created new image views every frame, and the FSR 4 registry holds eight. Now it uses the same
   `CachedView` as the Native AA path.

Dump check (`PS_DUMP_TRIGGER=<file> PS_DUMP_DIR=<dir>`, `PS_DUMP_FRAMES`, default 8:
FSR input, motion vectors and output, raw): with a static camera, PSNR between adjacent output
frames is ~45 dB versus ~28 dB for the jittered input; no trails when turning the camera or
walking. FPS in this mode is ~107 instead of ~220 — previously FSR 4 simply did not run.

Remaining: sprites (4 indices each) drawn into scene colour with scene depth — glows, small
lights — are still not shifted by jitter (the "≤ 6 indices = fullscreen pass" rule).

## 2026-10-01: faster FSR 4 — the post pass (2.9 → 0.8 ms at 4K)

Measurements: `PS_FSR4_PROFILE=1` (time of each FSR 4 pass, in the provider
`gpu/third_party/fsr-vulkan/src/ffx_vk_fsr4_v07.c`), out-of-game benchmark `out/gpu/fsr4-bench` (`ninja -C out/gpu
fsr4-bench`; `--stats` — registers and instructions from RADV). 4K Balanced (2260x1272 → 3840x2160),
RX 7800 XT: all of FSR 4 — 5.8 ms, of which **post 2.5–2.9 ms** (not the neural network: the last
layers, 2x2 pixel shuffle and history blending), 12 model passes — 2.4 ms, pre 0.55.

Cause: each thread computes a 2x2 block of output pixels and writes them one by one into three images
(recurrent state, history, output) — each wave write instruction writes every other
pixel. Removing any one of the three writes sped the pass up 1.3–4 times with the same code.

Solution: `tools/fsr4_optimize.sh` decompiles post (spirv-cross), `tools/fsr4_post_lds.pl`
gathers the workgroup's 16x16 block in shared memory and writes contiguous rows, glslang
compiles it back into `fsr4_shaders/opt/`; `vk_fsr4.cpp` takes it from there (`PS_FSR4_OPT=0` —
original). Two subtleties found by comparing outputs:
- spirv-cross translates signed int8 unpacking (`OpBitcast` to `i8vec4`) as `unpack8(uint)`
  (unsigned) — without a fix the result is completely different (PSNR 17 dB);
- values in shared memory must stay float: the compiler turns half from shared memory
  into a 16-bit write, which rounds differently into unorm8.

`tools/fsr4_verify.sh` compares the original and optimised post for all presets at
1080p/1440p/4K output on pseudo-random inputs: 1440p and 4K are bit-identical; post 4K 2.1–3.5 →
0.81–0.87 ms, 1440p 1.2–1.5 → 0.36–0.38 ms, 1080p 0.37–0.69 → 0.20–0.22 ms. In-game (4K
Balanced): FSR 4 5.8 → 4.0 ms, GPU frame 13.1 → ~11.6 ms; beyond that FPS is CPU-bound (the
GPU command thread waits for guest memory copies, "host copies" ~20%).

Found along the way: **the original FSR 4 at the 1080 tier (output 1920x1080) is non-deterministic** —
from run to run on identical inputs a strip at the left edge changes (columns 0–132), i.e.
there is a race or an uninitialised memory read somewhere in the model/provider. At 1080p output
this can cause flickering at the left edge of the frame. Not investigated.

The reactive mask is no longer computed with FSR 4 (FSR 4 does not accept it).

### What else was checked (same setup, 4K Balanced)

- **A/B in one process** (`ab.sh`, bit 24 — FSR off, UI copy without upscaling): 86.5 FPS with
  FSR 4 versus 87.7 without it. After the post optimisation the frame is CPU-bound (GPU command
  thread, waiting for guest memory copies), not GPU-bound. Async compute for FSR (overlapping it
  with the start of the next frame) would give no more than this ~1% — postponed until the CPU part
  is faster; besides, it requires reordering the UI and presentation commands of frame N after the
  work of frame N+1.
- **WMMA (`VK_KHR_cooperative_matrix`, supported by RADV on RDNA3)**: prototype of model pass 1
  (16-channel residual block: 3x3 16→16, 1x1 16→32 ReLU, 1x1 32→16) — 0.85 ms in the first
  version, 0.49 ms with a word layout in shared memory, versus 0.30 ms for the original dot4.
  The arithmetic on WMMA would take ~0.1 ms, but with 16 channels tile staging, epilogues through
  shared memory and occupancy (6 waves/SIMD, LDS-limited) eat the gain. The ceiling is about 1 ms
  for all 12 passes with careful manual rework of each; not started.
- `RADV_PERFTEST=cswave32` (wave32 for compute): FSR 4 is slower, 4.2 → 5.4 ms.
- The pre pass (0.54 ms) is not write-bound (0.50 ms without writes).

## 2026-10-01: FSR 4 flickering at the left edge at 1080p output — a race in model pass 11

The 1080-tier non-determinism (see above) is a bug in the v07 model shader, pass 11 (decoder,
1/4 → 1/2 resolution). Each thread is a 1/4-resolution input pixel and writes a 2x2 block of the
1/2-resolution output. The dispatch rounds the width up to 64 threads, and threads beyond the input
width do not stop: at the 1080 tier (input 480 wide) threads 480..511 write output pixels
960..1023, i.e. the first pixels of the next row (a row of any tensor is 15360 bytes), and
race with their real writers. At 4K output the width 960 is divisible by 64, no bug.

`tools/fsr4_pass11_guard.pl` adds an early exit for out-of-bounds threads (the size is taken from
the pass's own neighbour check); `tools/fsr4_optimize.sh` builds it into `fsr4_shaders/opt`
for all presets together with post; `vk_fsr4.cpp` and the benchmark take any pass from `opt/`.
`tools/fsr4_verify.sh`: 1440p/4K — bit-identical to the original (the guard does not trigger
there); 1080p — identical result from run to run, differences from the original after the first
frame only in the former strip at the left edge.

Found in the process: spirv-cross translates all signed int8 unpackings (`OpBitcast` to `v4char`)
as unsigned `unpack8(uint)`; pass 11 has nine different forms of them. The fix is shared across
the scripts — `tools/Fsr4SpirvCrossFixes.pm` (all `unpack8` through a signed helper).

## 2026-10-01: FSR 4.1.1 — what is inside and how to run it in Vulkan (reconnaissance)

Source: `amd_fidelityfx_upscaler_dx12.dll` 4.1.1.2740 from OptiScaler (`FSR4_LATEST`).
- The DLL has the provider `ffxProvider_FSR4_Int8` / `Fsr4Int8UpscalerModel` (version "FSR4-i8 4.1.1").
  1028 DXIL containers (25.7 MB): model passes `fsr4_model_v07_fp8_no_scale_pass1..12` (+ `_post`),
  270 `prepass` variants, 144 `postpass`, `fsr_rcas_pass` and a fallback FSR 3.1/2. No compressed data.
- Despite "fp8" in the name, the model passes are plain DXIL with `dot4AddPacked` (int8 dot4), without
  AMD extensions (WMMA). The architecture is the same v07 (12 passes), but weights and biases are not
  baked into the shader; they are read from `InitializerBuffer` (t18, typed buffer), tensor sizes from
  the cbuffer `CsTensorSizes`, scratch is `u11`. Each pass has 9 variants: the grid is baked in
  (tiers 1080 / 4K / 8K: 480x270, 960x540, 1920x1080 for pass 5) and, apparently, presets.
- `amdxcffx64.dll` (loader 2.3.0.2913, loaded by Proton) — the same FP8 FSR 4 passes and ML
  frame generation (`mlfi_*`), no separate INT8 model. `FSR4_INT8` 4.0.2 is `v07_i8`, i.e.
  what the port already uses.
- dxil-spirv (HansKristian-Work/dxil-spirv, built from source) translates the passes into
  correct SPIR-V: `dxil-spirv <pass> --enable-shader-i8-dot --ssbo-uav --use-reflection-names`.

What is missing for our own 4.1.1 provider: the container mapping (preset, tier, pass),
the contents of `InitializerBuffer`, the `CsTensorSizes` values, prepass/postpass permutations,
constants and dispatch sizes. The most reliable path is not to reverse the provider's x86 code but
to record one frame: a small test program on the FFX API (DX12, MinGW) under Wine/Proton with
vkd3d-proton, explicit selection of the "FSR4-i8" provider via a version override, and a Vulkan
layer that writes pipelines (SPIR-V from dxil-spirv), buffer contents and dispatch sizes. Then —
replay with our own provider through the same benchmark (`fsr4-bench`), checked against the
recorded output.

## 2026-10-01: FSR 4.1.1 in the port (upscaler=fsr411)

The AMD 4.1.1 model (INT8) runs natively in Vulkan and matches the DLL bit for bit.

**How it was obtained.** `tools/fsr4cap/fsr4cap.exe` (C, MinGW) calls FidelityFX API 2.3 from
`amd_fidelityfx_loader_dx12.dll` with explicit version 4.1.1 and, by hooking the D3D12 method
tables, records everything the DLL does under vkd3d-proton: pipelines (DXIL), root signatures,
descriptors, uploads, constants, dispatches. A frame is 29 dispatches: auto-exposure SPD, prepass,
pass0_post, model passes 1..12 (each followed by `_post`, zeroing the tensor border), postpass, RCAS.
- There are two models: m0 (Native..Performance) and m1 (Ultra Performance, ratio 3.0), weights are
  128 KB (`InitializerBuffer`). Tiers: t1080 (output up to 1920x1080), t2160 (larger, incl. 2560x1080).
- Tensors are computed from the output aligned to 8; the `CsTensorSizes` table (17 sizes), group
  counts and constants (MLSR as in v07, but width/height are not aligned and `inv_scale = 1 / scale`;
  SPD; RCAS) are verified by `extract.py` on 20 recordings.
- dxil-spirv translates DXIL the same way as vkd3d-proton: `--mixed-float-dot-product`
  (`VK_VALVE_shader_mixed_float_dot_product`, half dot2 with float accumulation) — without it
  PSNR is 40 dB instead of an exact match. `--class-bindings` (patch) lays out SRV/UAV/
  CBV/sampler registers at bindings +0/+32/+64/+96, the runtime binds resources by name.
- `fsr411/fsr411.cpp` — an independent Vulkan core (game and `fsr4-bench --fsr411`).
- `verify.sh`: DLL and replay on identical inputs, 8 frames — bit-identical for
  1080p (both models), 1440p, 2560x1080, 4K (both models). 1600x900 diverges only at the right
  edge, where the DLL itself is non-deterministic (a race in its shaders when the tensor width is
  not a multiple of 64).

**Speed-up.** postpass was rewritten using workgroup memory at the SPIR-V assembly level
(`postpass_lds.py`; translating through GLSL is inexact: `DenormPreserve` and the exact placement
of `RelaxedPrecision` are lost, and something else also changes the result): 2.18 → 0.95 ms at 4K,
bit-identical to the DLL. All of FSR 4.1.1: 1.09 ms at 1080p (1280x720), 4.2 ms at 4K Balanced.
In-game (4K Balanced) the upscaler takes 4.12 ms versus 4.32 ms for the accelerated v07.

**Building the assets** (by the user, from their own DLLs; the repository contains nothing from AMD):

    bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x> <amd_fidelityfx_loader_dx12.dll 2.3.x>

Requires nix-shell (or MinGW, cmake, ninja, python3, SPIRV-Tools, umu-run), network access for
dxil-spirv and the FidelityFX SDK headers, and Proton (GE-Proton). The DLLs are available, for
example, in OptiScaler (`FSR4_LATEST/`) and in games with FSR 3.1/4. A build from scratch gives the
same files; `VERIFY=1` at the end compares against the DLL.

**Correction to the A/B above ("the frame is CPU-bound").** Those measurements were taken while
12 hung shadPS4 processes each occupied a core. On an idle system: "host copies" 1.6%, GPU wait
~41% — the frame is GPU-bound, so async compute for FSR makes sense again.
