# Bloodborne on PC: actual state of the port and the upscaler

Snapshot as of **29 September 2026**. Sources: the files in `game_files`, the Git history of
`native_probe` up to `ac9fe90`, the current uncommitted changes, saved run logs and local tests. In
this document "is in the code", "was run" and "verified by image" are not treated as the same
thing.

## Summary

This is a working experimental **native Linux port**, not a run through a full emulator: the
original x86-64 `CUSA03173/eboot.bin` executes on the PC CPU, PS4 system calls are handled by our
own HLE runtime, graphics are output by a Vulkan core based on shadPS4. The game reaches the scene
with the character, accepts input, works with sound and saves; this is confirmed by run logs,
screenshots and videos in the root of `native_probe`. Completing the whole game, all locations and
the absence of crashes are **not confirmed** by these materials.

**FSR 3.1 temporal upscaling really works on the verified path with the scene resolution set at
startup**: the log `логи.md` shows `960×540 → 1920×1080`, UI composition at 1920×1080 and a run on
RX 7800 XT. The current path for changing the preset without a restart remains problematic on this
GPU: the latest `last_run.log` shows the requested scene sizes switching, but an FSR context of
`1920×1080 → 1920×1080`. Per the code, reduced targets are not selected here when `D32S8` does not
support blit. Therefore the preset switching log **does not prove an actual reduction of the 3D
resolution or an FPS gain**.

## Project layout and evolution

| Path | Role |
|---|---|
| `../CUSA03173/` | Local game files, `eboot.bin`, PS4 resources and modules. |
| `../np_base/` | Snapshot of the early port base; active development happens in `native_probe`. |
| `prepare.py`, `link_libc.py`, `link_modules.py`, `probe.c`, `runtime_*.c` | Executable image preparation, module binding, loader and HLE. |
| `gpu/` | Vulkan video core, shader recompiler, game-specific changes, FSR and ImGui. The borrowed shadPS4 core is described in `gpu/VENDOR.txt`. |
| `patches/Bloodborne.xml`, `patches.py` | FPS and resolution patches applied to guest code at startup. |
| `bbport.ini`, `run.sh`, `build.sh` | Settings, full launch and build. |
| `out/`, `out/dev/`, `last_run.log`, `логи.md` | Built binaries and traces of checks; some of the logs belong to earlier versions of the code. |

Evolution per Git: on 25 September the loader, HLE, graphics, sound and CPU/GPU optimizations were
added; on 26 September — frame analysis, camera vectors, jitter, FSR 3.1, presets and the menu; on
27 September the last commit `ac9fe90` fixed preset switching and mask display. After it, the
working tree has uncommitted work on object vectors, reduced render targets, separating scene/UI
resolutions, scripts and tests. This is the **current working tree**, not a finished release. Older
sections of `README.md`, `docs/en/upscaler.md` and `docs/en/history/ROADMAP.md` describe different stages
and are partly outdated.

## What is implemented

### Launch and performance

- The guest `eboot` and some of the original PRX run together with implementations of memory,
  threads, files, time, controller, audio, saves and services. Networking works in offline mode;
  PSN is not implemented.
- SDL3 handles the gamepad and fallback keyboard controls. Sound is output through the host audio
  system; the code has an `AudioOut` tempo fix and a save flag for missing sounds.
- The Vulkan renderer got a separate command recording thread, draw preparation on four threads,
  texture and buffer caches, guest memory copying and a staging buffer pool. `docs/en/parallel_gpu.md`
  and `docs/en/history/ROADMAP.md` contain measurements in individual scenes; they cannot be
  generalized to all configurations.
- `BB_FPS=uncap` by default applies a patch with a simulation step based on frame time; `30`, `60`,
  `90` are available. The output rate is limited by the monitor and a maximum of 120 FPS. For
  diagnostics there are `BB_FRAME_STATS=1`, `BB_TOGGLE_FILE` and frame structure capture via
  `BB_CAPTURE_TRIGGER`.

### Upscaler

- The game has no ready-made velocity buffer. The port computes camera vectors from the depth and
  matrices of two frames; for scene geometry it applies Halton subpixel jitter. Reprojection of
  static geometry was verified earlier (`docs/en/upscaler.md`).
- FSR 3.1 is integrated via `gpu/third_party/fsr-vulkan`. Native AA runs 1:1 on the HDR scene color
  before post-processing. The verified reduced-resolution mode uses a guest resolution patch, FSR
  after tonemapping and separate UI composition at 1920×1080. The UI patch in the current tree
  keeps its coordinates native. An explicit `BB_RENDER_RES=WxH` enables this path; for example, the
  log `логи.md` confirms `960x540`.
- Presets in the code: Native AA `1920×1080`, Quality `1280×720`, Balanced `1130×636`, Performance
  `960×540`, Ultra Performance `640×360` for 1920×1080 output. The ImGui menu opens with `Insert`
  or `L3+R3`. `bbport.ini` stores FSR, preset, sharpness, jitter, reactive mask, debug mode, object
  vectors and the FPS counter. The current config contains `upscaler=fsr3`, `preset=4`, `jitter=1`,
  `reactive=1`, `object_motion=1`, `debug_view=0`.
- The reactive mask is built by comparing color before and after the transparent passes. It is
  optional: earlier observations note shimmering of thin haze when it is enabled. The default in
  the code is off, but in the current `bbport.ini` it is on.
- The working tree has experimental vectors for animated objects: previous vertex positions, an
  extra G-buffer target, matching draws between frames and a restriction to suitable pose/bone
  buffers. Changing `object_motion` requires a restart. Per `out/dev/motion-selective.log`, history
  for the selected draws is often found, with no capacity skips in the intervals shown. This is a
  draw metric, **not an estimate of pixel coverage or visual quality**.

## Current dynamic preset defect

In `vk_scene_resolution.cpp` the function `SceneTargets::Eligible` requires `BlitSrc|BlitDst` on
the format of each candidate. In `vk_rasterizer.cpp` the reduced pass is disabled if the depth is
unsuitable; the choice of the reduced FSR input in `vk_temporal_upscaler.cpp` depends on it as
well. The saved log for RX 7800 XT reports that `D32S8` lacks `blit`. A fallback was added later
that **clears the UI depth** instead of copying it, but it does not remove the `blit` requirement
on the **3D scene** depth.

Result on this machine: `last_run.log` from 28 September lists switches to `640×360`, `1280×720`,
`960×540` and other presets, yet the created FSR context stayed
`1920×1080 → 1920×1080 (HDR scene color)`. The message `Scene resolution: raster ...` reflects the
requested size of the proxy targets; it does not measure the size actually used by each draw. The
claims in `README_FIXES.md` and the menu hints about dynamic scaling working on all GPUs are
stronger than the code and the latest log allow. `логи.md` belongs to an earlier run with automatic
selection of the startup patch, which was later changed in `run.sh`.

## Checks and confidence limits

| Check | Result |
|---|---|
| Game run logs and frames from 27–28 September | Loading into the game scene and image output confirmed. No end-to-end playthrough test was done. |
| `логи.md` on RX 7800 XT | Confirmed: scene patch `960×540`, UI `1920×1080`, FSR context `960×540 → 1920×1080`. |
| `last_run.log` | The menu changed presets without a restart, but FSR was 1:1; there are no FPS statistics for this run. |
| `out/dev/motion-selective.log` | For one series of scenes with selective object vectors, intervals of roughly 72–100 FPS are visible, with no `capacity skips` in the summaries shown; this is not a controlled preset comparison. |
| `out/gpu/motion-history-test` and `out/gpu/ui-composition-test` | Re-run on 29 September: both `PASS`. |
| `nix-shell shell.nix --run 'python3 -m unittest -v test_patches'` | Re-run on 29 September: 5 tests `OK`. |
| `out/dev/scene-validation.log` | The Vulkan test of reduced targets on Lavapipe **ended with an assertion** due to a version mismatch between the Vulkan headers and the dispatcher. The log gives no full successful check of this path. |

Object vectors in the game and their effect on weapon and clothing trails have not been visually
confirmed by a systematic A/B test. Early measurements of about 49 FPS with all draws versus 77–78
FPS without them and about 81–87 FPS on a narrower selection come from different runs; this is a
reference for further testing, not guaranteed performance of the current config. The screenshot
`image_2026-09-28_00-44-14.png` shows a game scene and the overlay at about 59 FPS, but does not
reveal the measurement conditions.

## What is not done yet / what to check next

1. Fix and confirm actual reduced 3D rendering in dynamic mode on RX 7800 XT: check the actual size
   of the color/depth attachments and the FSR input on each preset. If the path stays incompatible
   with `D32S8`, explicitly select the startup resolution patch and show in the menu that a restart
   is needed.
2. After the fix, run the same game scene with Native AA, Quality and Performance: record FSR
   input/output, FPS, frame time, UI sharpness and the image in motion.
3. Visually compare object vectors on/off while running, turning the camera and in combat; check
   trails, history coverage and GPU cost.
4. Sort out the assertion in `scene-resolution-test`; the other tests do not replace an in-game
   Vulkan test. Frame generation, FSR 4, DLSS and XeSS are not in the current port — they are
   listed in `docs/en/history/ROADMAP.md` as plans.

## How to reproduce the confirmed path

From `game_files` on this configuration:

```bash
BB_RENDER_RES=960x540 BB_FRAME_STATS=1 bash native_probe/run.sh
```

`BB_RENDER_RES` selects the startup scene resolution patch, and the current `patches.py` keeps the
UI at 1920×1080. Changing the resolution this way requires a new launch. Without `BB_RENDER_RES`
the experimental dynamic path described above is enabled. The launch rebuilds the port and uses the
local `CUSA03173` files; the presence of sources and a binary by itself does not confirm
reproducibility on another PC.

## 29 September fix: live preset switching

There were two reasons why changing the preset in the menu did not change the image:

1. `SceneTargets` copied targets between the native and reduced size only via `vkCmdBlitImage`, and
   RADV does not provide blit for `D32S8`. So the scene depth was considered unsuitable and reduced
   rendering was not enabled. Now, for depth/stencil formats without blit, copying is done with a
   fullscreen draw (`host_shaders/depth_resample.frag`): depth is written via `gl_FragDepth`,
   stencil via `VK_EXT_shader_stencil_export`.
2. The reactive mask (`reactive=1`) captured color at 1920×1080 and recreated the FSR context
   1920→1920, while the main pass immediately recreated it for the reduced input. The context was
   recreated every frame. Now the mask reads the reduced scene target.

Check on RX 7800 XT (`out/dyn-res-test.log`): switches in the menu give
`FSR 3 context 1280x720 / 1130x636 / 960x540 / 640x360 -> 1920x1080`, one recreation per preset
change, no failures. FPS in this scene is about 54–56 on all presets, i.e. the bottleneck here is
not the 3D resolution.

## 30 September: FSR 4 INT8 and performance

### FSR 4 (v07 INT8/DOT4)

- `vk_fsr4.cpp/.h`: FSR-Vulkan provider `ffxFsr4V07*` with a model per preset (native, quality,
  balanced, performance, ultraperf), tier 1080. Inputs are the same as for FSR 3.1. In the menu
  "Upscaler → FSR 4 (INT8)" («Апскейлер → FSR 4 (INT8)»), in the ini `upscaler=fsr4`, env
  `BB_UPSCALER=fsr4`. If assets or GPU capabilities are missing, FSR 3.1 is enabled automatically.
- Assets: `fetch_fsr4_assets.sh` downloads the set built by Q2RTX from the MIT FidelityFX SDK
  sources (`fsr4_shaders/`, in `.gitignore`, 98 SHA-256 checked against the manifests).
  `BB_FSR4_DIR` sets a different directory.
- Device: `shaderIntegerDotProduct` and `VK_KHR_compute_shader_derivatives` are enabled
  (`Instance::IsFsr4Int8Supported`).
- Verified on RX 7800 XT: the quality/ultraperf models are created, a ~6.5 s freeze on preset change
  (compilation). User: the image is sharper than FSR 3.1, but strong ghosting on the character and
  weapon. Vectors were checked with the debug view, they are correct: the character gets object
  vectors. In the SPIR-V of the pre-pass, depth is selected by minimum (as we do), vectors are
  interpreted as "previous − current". For A/B in the menu: "FSR 4: auto exposure"
  («FSR 4: авто-экспозиция»), "FSR 4: invert jitter sign" («FSR 4: обратный знак jitter»).

### Performance (perf, GPU command thread)

- `SceneTargets::Eligible` queried the driver for format properties on every draw: ~13–15% of the
  GPU thread time. Now cached per format.
- `ObjectMotion::Attach`, the color snapshot and the reactive mask called
  `Scheduler::CommandBuffer()`. This moved recording of the whole rest of the submit (almost the
  entire G-buffer) onto the GPU thread. Now they go through `Scheduler::Record`. Two direct calls
  remain at the end of the frame (`TemporalUpscaler::Run`, the presenter).
- Result: GPU thread ~74% instead of ~94% of one core. In game scenes 61–98 FPS versus 53–87 before
  the fixes (the scenes do not match one to one, this is a reference).

### Profile with DWARF stack (perf, GPU command thread, 30.09)

- `Motion::IndexedRange` (scanning and hashing indices for object vectors) took ~10% of the GPU
  thread. Added `Motion::IndexRangeCache`: the result is reused and fully revalidated once every 32
  frames. In game ~98% of ranges come from the cache (260 entries).
- `SceneTargets`: a flat table of format properties and memory of the last eight targets; together
  with Attachment/Get/View/Eligible ~4% instead of ~7%.
- After this the GPU thread is loaded with work without noticeable waits. The Vulkan recording
  thread is ~40% work and ~57% waiting. The GPU (RX 7800 XT) is ~65% busy on average. The frame is
  limited by the GPU command thread. The main items are the base costs of the video core: FindView
  ~5.6%, BindTextures ~5.6%, memmove ~4.6%, SmallGuestCopy ~3.5%, CachedImageDescEntry ~3.2%,
  BindBuffers/BindVertexBuffers ~3% each. The next step is moving these computations to worker
  threads (step 2 of parallel_gpu.md).

### FSR 4: ghosting on the character and weapon (30.09, re-check)

Video `video_2026-09-30_03-43-12.mp4`: FSR 4, Ultra Performance (640×360 → 1920×1080). Behind the
moving saw disc a dark imprint remains on the disoccluded background. Verified:

- debug view of vectors (FSR 4): the character and weapon get object vectors;
- the index range cache does not go stale (0 changes on revalidations);
- the jitter sequence matches the provider (Halton 2/3, ceil(8·ratio²) phases);
- A/B in one run with the post-29.09 optimizations disabled (bits 4096, 8192, 1<<30): the trails
  are the same in both phases, so this is not a regression from them. Incidentally: these
  optimizations together give 84.4 versus 70.8 FPS while running (+19%).

The v07 model detects disoccluded background by color (depth is used only for dilating the
vectors), and with a 640×360 input it has little information. FSR 3.1 rejects such history by
depth, so at Ultra Performance it has fewer trails.
