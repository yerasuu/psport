# New memory model on NVIDIA: outcome of the work (2026-10-09)

The measurements below refer to the module's development stage. Current `0.5-pre2` changes and the
results of verifying the original copy shader: [CHANGES_0.5-pre2.md](CHANGES_0.5-pre2.md) and
[TRANSLATION_GPU_COPY_LABELS.md](TRANSLATION_GPU_COPY_LABELS.md).

## Task

The new memory and translation model (`BB_PC_MODEL=1`) kept game memory in one sparse buffer (the
"arena") and moved hot blocks to VRAM by rebinding pages (`vkQueueBindSparse`). On the proprietary
NVIDIA driver for Linux this does not work: starting with version 555 the time of
`vkQueueBindSparse` grows with the number of already bound pages, and the call itself holds a
global lock. For the tester (GTX 1660 Ti, driver 615.71) each move stalled the GPU for 10 seconds
or longer, and the desktop froze together with the game. Therefore the new model was available
only on AMD.

Goal: the new memory model on all graphics cards.

## Solution: the layer's memory module

Instead of the sparse arena, our own memory module runs (`gpu/layer/bblayer_gpu_memory.*`,
`gpu/shadps4/video_core/buffer_cache/buffer_cache_layer.cpp`, plan — `MEMORY_MODULE_PLAN.md`):

- game memory is bound **in place**, via buffers of its chunks (dma-buf on AMD, host memory import
  `VK_EXT_external_memory_host` on NVIDIA);
- frequently read data is copied to VRAM into separate "mirror" buffers. A move comes down to a
  copy and bookkeeping in the module, without page rebinding;
- a binding takes the mirror if the range is entirely in VRAM, otherwise game memory in place.
  Ranges of 64 MiB and more go through a page table in translated shaders.

**`vkQueueBindSparse` is never called in this mode**: the counter is printed in the log line
`Layer memory: ... vkQueueBindSparse: 0 calls`.

### When it is enabled

| Graphics card | Default |
|---|---|
| AMD | sparse arena, as before |
| NVIDIA, Intel and others | memory module, automatically |

`BB_LAYER_MEMORY=1` / `=0` selects manually. The launcher no longer hides the "New memory and
translation model" (Новая модель памяти и трансляции) toggle on non-AMD cards. The startup host
memory import check in module mode verifies GPU and CPU writes through the imported buffer itself,
without a sparse buffer.

## What had to be fixed for the module to work correctly and fast

### Speed: from 48–60 FPS to ~205 FPS on AMD (standing at the lamp)

1. **Replacement of image copies and clears did not trigger.** The detectors `IsComputeImageCopy`
   and `IsComputeImageClear` expected exactly two buffers on the shader, and the module added a
   third to every shader — the page table. The render target copy (`cs 3d5ebf4e`) and clear
   (`cs 8b355b5a`, 31 times per frame) ran as real shaders through memory, and 1080p images were
   reloaded every frame. Now the page table is not counted (`ShaderBufferCount`): 60 → 136 FPS.
2. **Mixed ranges.** If all blocks of a range are in VRAM except one or two edge ones with
   "unknown" bytes (a gap between images, foreign data), the whole range was read over the bus.
   Such blocks are now "volatile": they live in the mirror and are updated from game memory before
   each read binding. 139 → 207 FPS. Volatile blocks are not used for write bindings.
3. **Move to VRAM on the first binding** of a range of known data (as the arena did). It respects
   `BB_GARLIC_VRAM=0` and the VRAM budget (`BB_VRAM_PROMOTE_PERCENT`, important for 6 GB cards).
4. **CPU load.** Module answers are cached per placement generation; repeated "can it be moved"
   checks — no more than once per frame per range; the "does the range have a VRAM copy" check —
   lock-free, by the module's version counter. This removed about 20% of binding time and almost
   all locks in the command processor thread.

### Correctness: vertex explosions and garbage textures

The cause was one: **the VRAM copy diverged from game memory**. Several paths were found and
closed:

1. **Game code writes to memory without our knowledge**: its heap headers (`0x2085a60` /
   `0x20858a0`), part of the resource loader (`0x216d420`). Blocks with a VRAM copy are now
   write-protected by the module's own traps. The write goes through immediately, and the block is
   returned to its place before the next binding. Frequently written blocks stay in place forever
   (`NoteCpuWrite`).
2. **Reads bypassing the module.** Small bindings, the constant ring and command processor copies
   took data from game memory, although the new GPU data lay in VRAM: in the new model
   `IsRegionGpuModified` knows only about unfinished writes. Now everything that touches a block
   with a VRAM copy goes through the module (`LayerMirrored`).
3. **GPU data overwrote new CPU data.** When returning a block to its place and when re-issuing
   memory to the game, the module copied "GPU-modified" data back, including over what the CPU had
   written later: the marks were set too broadly. Now declared CPU writes clear the marks on their
   bytes (`LayerForgetGpuData`), re-issuing memory clears the marks on its range, and after an
   undeclared write the GPU data in the block is considered dead and is not copied back.
4. **Where the GPU may write in VRAM** (`BB_LAYER_GPU_WRITES`): by default (`1`) — only into blocks
   the CPU has not touched since allocation: render targets and other GPU-only data. `0` — never
   (correct, but 60–70 FPS: writes go over the bus), `2` — everywhere (gave explosions).
5. **The game's copy shader** in `0.5-pre2` is translated to SPIR-V by default and runs on the GPU.
   The page table contains the current GPU representation and game memory addresses; the copy
   shader writes the result into both representations. This eliminates the divergence between the
   VRAM mirror and game memory without replacing the original copy list on the CPU.
   `BB_COPY_SHADER_NATIVE=0` keeps HLE for comparison.

At the previous stage it was tested in game on AMD (module enabled manually, copy shader via HLE):
with `BB_LAYER_GPU_WRITES=0` and with the rule "the GPU writes to VRAM only inside images" — the
Hunter's Dream, dogs and corpses in Yharnam are clean. On the latest build (the rule "blocks the
CPU has not touched") no artifacts were noticed during play, but there has not yet been a separate
check of all locations.

## Measurements (RX 7800 XT, AMD, module enabled manually)

| Location | Module | Sparse arena | Old model |
|---|---|---|---|
| At the lamp, morning (before the correctness fixes) | ~205 FPS | ~205–238 | ~220 |
| Hunter's Dream, now | 130–144 FPS | — | — |

In the Hunter's Dream we are bound now by the GPU command stream on the CPU (~3.9 µs per call at
~1500 calls per frame), now by the GPU itself: G-buffer passes 960×540, FSR ~1.07 ms.

**Open question:** in some runs at the same location the GPU works noticeably slower (70–95 FPS,
FSR up to 3.2 ms instead of 1.07). The cause has not been found; the hypotheses of shader
compilation, the per-section profile `BB_SECTIONS` and volatile block updates were not confirmed.
Comparing runs is complicated by the game drawing different passes depending on the camera angle
and scene.

## What is not tested

- **NVIDIA itself.** The NVIDIA path (host memory import + module) cannot be reproduced on AMD:
  RADV does not import memfd. A tester's log is needed.
- Cards with little VRAM (6 GB): behaviour with full VRAM was tested only artificially
  (`BB_VRAM_PROMOTE_PERCENT=1`: no copies, ~38 FPS, stable).
- Intel.

## What to ask of the NVIDIA tester

A build with "New memory and translation model" enabled in the launcher. The log should contain
the lines:

- `Guest memory: host memory imported by the GPU (VK_EXT_external_memory_host ...)`;
- `Guest memory: the new memory model through the layer's memory module on this GPU (vendor 0x10de)`;
- `Layer memory: ... vkQueueBindSparse: 0 calls, 0 ranges; write traps hit N`;
- `Frame stats: ... FPS`.

If the line `vkQueueBindSparse of N ranges took X ms` appears, sparse is still used somewhere —
that is a bug.

## First logs from NVIDIA and fixes (evening of 2026-10-09, `0.5-pre3`)

Tester: GTX 1660 Ti (laptop, 6 GB, PCIe 3.0), driver 615.71, build 0.5-pre2. Host memory import
and the module were enabled, `vkQueueBindSparse: 0`.

1. **There were no mirrors at all** (10–15 FPS, background flicker on the title screen):
   `FixedArena()` was enabled by default with host memory import and, via
   `GarlicInVramForLayer()`, forbade the module from moving. Now it is always disabled in module
   mode. Until the new build the workaround is `BB_FIXED_ARENA=0`: with it the tester got 30–35 FPS
   (video), vs 60–70 in the old model.
2. **The texture cache and mirrors shared VRAM without a common budget.** The texture cache
   collector compares the whole process VRAM with thresholds, the mirrors too, and the thresholds
   on cards below ~9 GB are 40 and 80 % of the startup budget (for the tester, pressure 1963 MiB,
   critical mark 3927 MiB). Moves, however, stopped only at 90 % of the driver budget. Result —
   3.9–4.0 GB, the collector constantly past the critical mark: 155–1255 evicted images per 5 s vs
   0–29 on AMD. Now:
   - moves to VRAM stop 384 MiB before the critical mark (`VramPromotionsPaused`, line
     `copies of loaded blocks wait`);
   - 128 MiB before it the module frees space itself (`LayerYieldToImages`): first it trims the
     mirrors' growth headroom, then returns to their place whole mirrors unused for 3 s
     (`BB_VRAM_PRESSURE_IDLE_SECONDS`).
3. **Mirror rebuilding.** A section adjacent to a mirror created a whole new mirror with a copy of
   the old one. During level loading this was 15–27 GB of allocations for 1.3 GB of data and a
   VRAM spike up to 7.4 GB (the tester's log showed 5.6 GB with 6 physical). During gameplay, when
   areas stream in, the same. Now an upward-growing mirror gets growth headroom equal to its size,
   but no more than 64 MiB (`BB_LAYER_MIRROR_GROWTH_MB`): 5–10 GB of allocations, worst loading
   frame 555 → 87 ms. Replaced mirrors are freed on every submission, not once per second.
4. **`WRITE_DATA` writes into a mirror.** The command processor writes into game memory on the GPU;
   a block with a mirror used to go back to its place. If the range is entirely in one mirror, the
   write goes into both representations (`CommandWriteMirror`).
5. **Watched blocks** (`BB_LAYER_WATCH`, enabled by default). Blocks the CPU once wrote to without
   warning used to always be read in place, which on NVIDIA means over the bus. Now for reading
   bindings they also get a mirror under a write trap. A CPU write does not return the block to its
   place: it is re-uploaded before the next binding, and the trap is armed again. The GPU writes
   into such blocks in place; a block it has written to is no longer watched. Watching almost
   completely replaced volatile blocks (RX 7800 XT: 1.3 GB/s of re-uploads → 36 MB/s; the tester
   had 455 MB/s of volatile ones). Blocks the CPU writes often (3 frames out of 60) still go back to
   their place: re-uploading them before bindings ended the render pass (GPU 4.8 → 7.2 ms per
   frame). `BB_LAYER_WATCH_HOT=1` — an experiment with them.
6. Runtime file reads (`read`/`pread`) are retried if the kernel returned `EFAULT`: the trap could
   have been armed again between touching the pages and the read.

Check on RX 7800 XT with `BB_VRAM_LIMIT_MB=5600` (collector thresholds as for the tester:
1960/3920): old model ~195 FPS; module ~200–213 FPS, GPU 4.6–5.2 ms; VRAM stays below the critical
mark; route `docs/route2.txt` without frames longer than 25 ms; 3 minutes of walking without
errors; the image in the Hunter's Dream and at the end of the route is the same as without
watching. NVIDIA itself is not verified by these measurements: on AMD in-place reads are almost
free.

## Feedback on 0.5-pre3 and fixes (`0.5-pre4`)

- **GTX 1060 6 GB:** the new model is on par with the old one (41–48 FPS at ~1500 calls per frame
  in both, GPU 21–24 ms). Early drops — shader compilation in the first session (160–241
  compilations per 5 s, stutters of 1–3.7 s): now new pipelines for passes drawn every frame are
  built in the background (`BB_ASYNC_PIPELINES`), and their draws are skipped for a frame or two.
- **4 GB card:** after ~4.5 minutes 5 FPS — a pre3 regression. Moves to VRAM stopped 384 MiB before
  the texture collector's critical mark (a 4 GB card has the lowest threshold values,
  1536/3072 MiB), while textures and internal allocations already held 2808 MiB: copies dropped to
  124 MiB for 1.9 GB of data, GPU ~200 ms per frame. Now mirrors have their own share (25 % of the
  driver budget, `BB_LAYER_MIRROR_FLOOR_PERCENT`): below it the pause due to the collector does not
  apply.
- **After teleport ~10 FPS less:** large bindings over the whole memory (via the page table)
  marked every 2 MiB as used, so nothing in the game was ever idle, and the mirrors of the
  abandoned location held VRAM. Now only direct bindings are counted (`layer_group_use`): when VRAM
  runs short, data not directly bound for 3 s goes first, and mirrors with memory lacking valid
  blocks are recreated only over the valid ones (`LayerCompactMirrors`) — otherwise a mirror
  partially returned to its place held all its memory. On RX 7800 XT with
  `BB_VRAM_LIMIT_MB=4200–4600` this freed 160–490 MiB at a time, the image did not change. Not
  tested on NVIDIA.

## Toggles and diagnostics

| Variable | What it does |
|---|---|
| `BB_LAYER_MEMORY=1/0` | memory module on/off (default: on for non-AMD) |
| `BB_LAYER_GPU_WRITES=0/1/2` | where the GPU may write in VRAM (see above), default 1 |
| `BB_LAYER_WRITE_TRAPS=0` | no write traps (for comparison only: explosions come back) |
| `BB_LAYER_VOLATILE=1` | with volatile blocks (disabled by default since 2026-10-10: their copies broke render passes) |
| `BB_LAYER_PAGED=0` | no page table for bindings of 64 MiB and more |
| `BB_COPY_SHADER_NATIVE=0/1` | HLE / native game copy shader; native by default in the new model |
| `BB_RESIDENCY_MIX=1/2` | bindings in place / in VRAM / mixed; `2` — also the heaviest ones with a block breakdown |
| `BB_LAYER_WRITE_LOG=1` | large GPU writes left in place, and nearby images |
| `BB_TOGGLE_FILE`, experiment bit 8 | read in place instead of VRAM copies (check for stale copies) |
| `BB_TOGGLE_FILE`, experiment bit 16 | no volatile block updates |
| `BB_LAYER_WATCH=0` | no watched blocks (blocks the CPU wrote to are read in place) |
| `BB_LAYER_WATCH_HOT=1` | also watch blocks the CPU writes often (experiment) |
| `BB_LAYER_MIRROR_GROWTH_MB=N` | mirror growth headroom, default 64; `0` — no headroom |
| `BB_VRAM_PRESSURE_IDLE_SECONDS=N` | when VRAM runs short, data not directly bound for N s goes back to its place (3; `0` — no) |
| `BB_LAYER_MIRROR_FLOOR_PERCENT=N` | share of the VRAM budget that the texture collector does not take from mirrors (25) |
| `BB_ASYNC_PIPELINES=0` | new pipelines are compiled immediately, the game waits (default — in the background for passes drawn every frame) |
| `BB_ASYNC_PIPELINE_THREADS=N` | number of background compilation threads (default a quarter of CPU threads, 1–4) |
| `BB_VRAM_LIMIT_MB=N` | test: the driver's VRAM budget is taken as at most N MiB (6 GB card simulation: 5600) |
| `BB_RESIDENCY_MIX=1` | the `Layer in place` line now also says why blocks are not watched |

## What is next

1. A log from NVIDIA.
2. Find the cause of the slow runs (or prove it is a different scene).
3. Volatile blocks are updated at 0.9–1.6 GB/s — it would be cheaper to update them only after a
   CPU write (the traps already exist).
4. If the module is confirmed on NVIDIA, move AMD to it as well and remove the sparse arena from the
   new model.
