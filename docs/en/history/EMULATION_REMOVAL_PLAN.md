# Moving away from emulation: what is done and what is next (2026-10-06)

Historical plan and measurements by development stage. In `0.5-pre2` the new model remains
experimental and disabled by default; for NVIDIA and other non-AMD GPUs a memory module without
sparse rebinding was added. Current state and checks:
[CHANGES_0.5-pre2.md](CHANGES_0.5-pre2.md).

## Where the API boundary lies

Bloodborne is built with a static libGnm. PM4 packets are written not by a driver but by the game
itself, via `sce::Gnm::*CommandBuffer` functions (`0x1068000–0x10a0000`, `0x11fd000–0x11fe000`).
Hooking all such functions (`BB_GNM_OBSERVE=1`) showed: 100 % of submitted packets are written by
known API calls, 99 methods in total. Gnm is an API that writes registers: `setDepthStencilControl`
= one `SET_CONTEXT_REG`. Therefore parsing the PM4 from these 99 functions and translating the calls
themselves give the same result; PM4 here is a recording format for calls, like a command list. It
is already parsed in parallel (DrawPreparation).

Hardware emulation is not packet parsing, but what is done in place of the GPU:

| What | State |
|---|---|
| Memory model 0.2 (VRAM copies, page protection, readback, early labels) | Fallback. PC model is the default, "chunk per allocation" mode for NVIDIA. Remove the 0.2 code after verification on NVIDIA. |
| EOP/EOS labels | PC model: written after GPU work completes (honestly). |
| `WaitRegMem` on labels of the same queue | Translated as a satisfied wait (queue order); no CPU wait needed. |
| `WRITE_DATA`/DMA waited for all host copies | Wait only for overlapping copies (like the command processor, which does not wait for shaders). |
| Constant Engine (CE RAM, dumps, counters) | Executed on the CPU in stream order: our parser reads descriptors from memory. |
| SRT walker (GCN shader code executed on the CPU) | Needed for the same reason. |
| Tracking GPU writes for the parser (`PendingWriteOverlaps`, `IsRegionGpuModified`) | ~15 % of the GpuComm thread, same reason. |
| Texture cache by address + hash | Same reason plus the absence of "texture" objects in PM4. |

Almost all remaining emulation exists because **the translator reads resource descriptors
(T#/V#/S#) from game memory on the CPU during parsing**. A PC driver and DXVK do not do this: the
application's memory is read by the GPU during execution.

## Next, in order

1. **NVIDIA.** Test build `dist/Bloodborne-bbport-x86_64-test-1006.AppImage` (PC model by default,
   "chunk per allocation" mode). If it works, remove model 0.2 entirely (`page_manager`,
   `fault_manager`, readback, BB_UFFD, preloading, early labels).
2. **Parallelise stage B** (draw recording; at 4 cores it is 99 % busy, ~3.3 µs per draw, spread
   evenly over buffer/texture binding, pass begin, vertices). Options:
   (a) several stage B threads over draw ranges within a render pass (each with its own command
   segment; shared caches are already under locks; barrier order is per pass);
   (b) resolving bindings (buffers/textures → Vulkan) on stage A, which is 28 % idle, with B only
   recording. Walkthrough tests needed.
3. **Descriptors on the GPU (the main step toward "like DXVK").** The shader reads T#/V#/S# itself
   (in-place buffers and BDA already exist); textures are a bindless array, T# → index via a table
   on the GPU, maintained by the texture cache when creating images. Then the SRT walker, CE on the
   CPU and pending-write tracking are not needed; `WRITE_DATA`/DMA/CE dumps become GPU writes in
   stream order. A big project.
4. **Textures as objects.** Hook `Gnm::Texture::init*` and the TPF loader
   (`CSGXTextureCreateFragment`): the image is created at load time rather than guessed by
   address; hashes and address overlaps go away. Assets can be converted to linear layout ahead of
   time (detiling once at install).
5. **Shaders ahead of time.** All GCN shaders from the game files → SPIR-V at install; no
   compilation during gameplay.

What remains translation, as in DXVK: GCN shaders → SPIR-V, descriptors and formats → Vulkan.

## Step 1 (2026-10-07): the constant engine no longer synchronises with draw recording

Measurements (`BB_CE_STATS=1`, Yahar'gul): ~14,000 `DumpConstRam` per second, ~4–5 MB/s, 25 pages
in total (ring ~100 KB). Each dump is read by exactly one pointer from user data — these are
descriptor tables for the SRT walker. Constants, vertices, indices and large buffers are not read
from these pages; there are no shaders that read memory on their own (`uses_dma`). That is, CE
dumps are needed only by the translator — it is descriptor table construction, not data for the
GPU.

Stage A waits on dumps (`WaitForPendingReads`) were needed only because stage B, for draws without
preparation, re-read the V# tables (vertex buffer descriptors) itself — by then overwritten by the
next dump (~15 % of V# turned out to be foreign: DoF bug, issue #44).

Done:
- stage A captures V# into the draw packet, stage B takes them from there and does not read the
  tables;
- a DMA source is marked with an exact range, not as "reads who knows what".

Result: dump waits ~850/s → ~46/s, stage A waits 1.5 % of the time instead of ~30 % (16 threads).
On 4 cores / 8 threads FPS is the same (~170): there the bottleneck is stage B (3.4 µs per packet;
the most is `ObtainBuffer` at 0.57 µs: mapping game memory addresses to GPU buffers).

Command processor writes (`BB_CE_STATS`): `WRITE_DATA` ~8,400/s into 2 pages (read as constants
~240/s), DMA ~1,000/s into 15 pages. They write data for the GPU, and their translation is GPU
commands in stream order (`vkCmdUpdateBuffer`; DMA for in-place memory already works that way). For
`WRITE_DATA` this runs into the rule moving frequently GPU-written blocks to VRAM — to be done
together with simplifying the memory model.

## Step 2 (2026-10-07): stage B synchronisations

Stage B (draw recording) is the bottleneck on 4 cores (99 % busy). Frame tasks on it:
- flip signal = frame preparation by the presenter; 600k cycles per frame, of which ~40k is work,
  the rest is waiting for command recording threads (`CommandBuffer()`) and synchronous
  submission. Now ~50k: the presenter is recorded via `Record`, submission is asynchronous;
- upscaler passes write commands directly — they get a separate segment without waiting;
- DMA (~900/s, 4 bytes) waited for copies and signals even when the GPU does the write — they wait
  only for CPU writes.

What remains on stage B (µs per packet, 3.5 total): resource binding 1.0 (buffers 0.4, textures
0.45), `ObtainBuffer` 0.6, pass begin 0.26, vertices 0.34, tasks 0.2. Next — splitting stage B or
resource binding on stage A (see item 2 above).

## Step 3 (2026-10-08): the GPU writes labels, hooks without signals, NVIDIA

What the game actually submits (new counter `BB_DCB_STATS=1`: PM4 packets by type, events, labels,
`WAIT_REG_MEM` outcomes; Hunter's Nightmare, at the lamp, ~185 FPS): `EVENT_WRITE_EOP` 61k/s (~330
per frame, all without interrupt, 32- and 64-bit labels), `WAIT_REG_MEM` 31k/s (all on memory and
all satisfied by labels pending in stream order — the CPU never waits), `EVENT_WRITE_EOS` 371/s,
`WRITE_DATA` 3.5k/s, `DUMP_CONST_RAM` 6.6k/s, cache flushes (`EVENT_WRITE`, `ACQUIRE_MEM`) — about
70k/s. Not encountered: occlusion queries (`ZPASS_DONE`), `SET_PREDICATION`, `COND_EXEC`,
`MEM_SEMAPHORE`, `REWIND`, `COPY_DATA`, labels with GPU time, submissions to compute queues.

Done:
- **EOP/EOS labels are written by the GPU** (`Rasterizer::WriteLabelOnGpu`, `BB_GPU_LABELS=0` — as
  before): a Vulkan buffer marker (`VK_AMD_buffer_marker`, bottom of pipe) directly into game
  memory (a guest memory chunk), at the same point in the stream where the PS4 command processor
  writes the label. A 64-bit label is two 32-bit writes, upper half first. Previously the label was
  written by a CPU thread after the whole Vulkan submission completed (61k writes per second).
  Labels with an interrupt are still signalled from the CPU (the interrupt comes from there). The
  same for `RELEASE_MEM` labels of compute queues. EOS with a write from GDS is a GPU copy in
  stream order instead of `Finish()` and a CPU read.
- **Hooks into game code without traps** (`gpu/shim/bbport_guest_hooks.cpp`,
  `BB_GUEST_HOOK_TRAPS=1` — `int3` traps): loader, range allocator and GPU heap points jump into
  stubs next to the image that call the handler and execute the overwritten instructions; loader
  `memcpy` calls go through a trampoline. Previously each call was a kernel signal (SIGTRAP),
  ~1200 per second for the allocator alone.
- **New model on NVIDIA** (`gpu/shim/bbport_guest_memory.cpp`): besides dma-buf chunks (AMD) — a
  "host memory imported by the GPU" mode (`VK_EXT_external_memory_host`): the chunk stays in the
  runtime's memfd, the GPU imports its second mapping. At startup a self-test exactly as in
  operation: memfd import, binding to a sparse buffer, a GPU write and a CPU read through the other
  mapping, and back. If it fails — dma-buf (on NVIDIA "chunk per allocation"), for which CPU read
  speed is now measured (an uncached mapping would slow down game code; below 1 GB/s it is not
  used). If that fails too — the old model. Selection: `BB_GUEST_MEMORY=host|dmabuf`. The launcher
  toggle is available on AMD and NVIDIA. On AMD (RADV) the driver does not accept memfd import —
  there it is dma-buf, as before. Not tested on NVIDIA: the log shows which path was chosen and
  why.

Measurements (at the lamp): 16 threads — 233 FPS both with and without GPU labels; 4 cores /
8 threads — ~217 vs ~215 FPS (within noise; the gain is a free signal thread).

### What else remains and why

Everything left of emulation rests on one thing: **the translator reads resource descriptors
(V#/T#/S#) and the tables holding them from game memory during parsing**. Because of this:
- `DUMP_CONST_RAM` is executed by the CPU (dumps are read only by the translator — they are its
  descriptor tables, the GPU never reads them);
- DMA and `WRITE_DATA` into memory that is not "in place" are executed by the CPU (shader code and
  descriptors arrive there, which the translator reads immediately);
- the SRT walker (shader code translated to x86) walks the tables on the CPU;
- the shader is specialised by descriptor fields (stride, format, channel swizzle, image type),
  the pipeline is selected by registers;
- textures are identified by address and format from T#.

Moving this to the GPU is a new recompiler mode: the shader reads V# itself (address via the BDA
arena, format and stride at run time), images via a bindless array with a T# → index table,
barriers from the game's own cache flushes (`ACQUIRE_MEM`/`EVENT_WRITE`), residency of all memory
at once on mapping. That is several days of work with risk to the image: not started.

The rest is not emulation in the sense of "in place of the GPU": GCN → SPIR-V translation during
gameplay (as in DXVK), synthetic vblank (HLE VideoOut), stubs for packets the game does not use
(occlusion — dummy counter, `SET_PREDICATION`, `COND_EXEC`, `MEM_SEMAPHORE`).

## Step 4 (2026-10-08): where the boundary between emulation and translation lies

Measurement (`BB_RES_STATS=1`, Hunter's Nightmare at the lamp): ~97k resource bindings per second;
for almost every one (0.96 stages per binding) the descriptors lie in tables in memory, not in
registers: per binding, buffers — 1.3 from registers and 2.8 from memory, textures — 0.6 and 3.3,
samplers — 1.0 and 0.9.

**Descriptors on the GPU cannot be done correctly for textures.** A PS4 texture lies in memory in
the PS4 layout (tiling), which does not exist in Vulkan: it must be converted into a Vulkan image,
and for that the CPU must know the T# (address, format, dimensions) before the draw executes. If
the shader reads T# from memory itself and looks up the image in a table on the GPU, then on the
first use of a texture the image does not yet exist — the GPU has no correct fallback path (only a
frame with the wrong texture). Therefore reading descriptors on the CPU (SRT walker, tables filled
by the constant engine) is not emulation but translation of resource binding, as in DXVK and
vkd3d, which also read bindings on the CPU and create Vulkan objects from them. For buffers,
descriptors on the GPU are possible (BDA), but the gain is only descriptor writes (~0.1 µs per
packet), unless residency and barriers are redone entirely; not started.

What actually remains of emulation, and whether Bloodborne uses it:

| What | Bloodborne |
|---|---|
| Waiting for flip (VideoOut label) blocks the parsing thread | yes — this is display synchronisation, like the wait in Present in DXVK |
| Occlusion query counter is a dummy | **yes**, ~60 `ZPASS_DONE` events per frame (not visible in the packet counter: they are in nested buffers); translated — "Step 5" |
| `SET_PREDICATION`, `COND_EXEC`, `COPY_DATA` are not translated | no; translated — "Step 5" |
| `MEM_SEMAPHORE`, `REWIND` — wait on the CPU | no |
| Compute queues (ASC) — coroutines in one thread | no (0 submissions per frame) |
| CPU writes to GPU-visible memory are "declared" (`memcpy` wrappers, hooks on loaders, heaps, allocator, file reads) | yes — for VRAM copies and textures; `BB_WRITE_VERIFY` on the route found no undeclared writes besides expected ones |
| The game's copy shader — replacement (the list is read by the CPU) | yes; can now be translated (`BB_COPY_SHADER_NATIVE=1`) |

The "no" rows matter only when porting other games (Driveclub, inFAMOUS: they have async compute
and occlusion) — they cannot be tested on Bloodborne.

Copy shader (`cs fefebf9f`): the replacement can be swapped for translation of the shader itself.
This found a real hazard of the new model: next to where it copies lie shader code and descriptor
tables that the translator reads on the CPU; if these blocks move to VRAM ("the GPU writes this 3
frames out of 60"), the translator reads stale code (black scene). The replacement avoided this by
accident. Now this shader's writes always stay in game memory. A general rule "what the GPU writes
but does not read itself stays in game memory" (like a readback heap) was tried: it breaks
performance (GPU 4.2 → 6.1 ms per frame — that way system RAM and render-target-sized regions get
caught), removed.

Constants: copying into the ring during parsing was needed while command processor writes were
executed by the CPU "ahead". In the new model it is no longer needed for correctness —
`BB_CONSTANTS_IN_PLACE=1` reads them in place; disabled by default (GPU +3 % due to PCIe, FPS not
higher). That is, the constant ring is now an optimisation (a VRAM copy), not emulation.

There is almost nothing left to do on translation for Bloodborne; further work is performance
(stage B on weak CPUs) and porting: real occlusion queries (`vkCmdBeginQuery` + GPU copy of the
result into game memory), predication (`VK_EXT_conditional_rendering`), compute queues as a Vulkan
queue with semaphores — when a game appears that uses them.

## Step 5 (2026-10-08): occlusion queries, COPY_DATA, COND_EXEC, predication

Correction to steps 3 and 4: Bloodborne **uses** occlusion queries — about 60 `ZPASS_DONE` events
per frame (~14k per second at the lamp in Hunter's Nightmare). The packet counter did not show
them: it only looked at top-level command buffers. Until now the game received a dummy "everything
visible" sequence.

Done (new model only; `BB_OCCLUSION_QUERIES=0` — as before, bit 4 of the second number in
`BB_TOGGLE_FILE` — toggle on the fly):

- **Occlusion queries → Vulkan queries** (`vk_occlusion.{h,cpp}`, `occlusion_resolve.comp`). On
  PS4 each depth block keeps a counter of samples that passed the depth test; the `ZPASS_DONE`
  event writes the counters (16 bytes between blocks, ready bit 63), the game takes the difference
  "after − before". Here a Vulkan query is always active from the first event; it is cut into
  segments at each event and at each boundary across which a query cannot be extended (render pass
  begin and end, command buffer — `Scheduler::CarriedScope`, Suspend/Resume). An event remembers
  how many segments ended before it. At the nearest point outside a pass the segment results are
  copied, and one shader dispatch sums them in order and writes to each pending event its
  accumulated value in game memory via the GPU address (BDA). Render passes are not broken; the
  cost on 4 cores is within noise (220 vs 219 FPS with the dummy counter, GPU 4.54 vs 4.56 ms).
  Results appear slightly later than on PS4; until then the ready bit is clear — the game checks
  it, as on PS4. The counters always stay in game memory (the CPU reads them).
- **COPY_DATA**: values known at parse time (constant, register, clock) are written by the GPU in
  stream order, like `WRITE_DATA`; copies from memory and GDS are GPU copies in stream order, like
  `DMA_DATA`. A write to a register is a warning for now.
- **COND_EXEC**: the condition is taken from a pending label in stream order, otherwise after the
  GPU has executed the work before the packet (previously it was read at parse time, ahead of the
  GPU).
- **SET_PREDICATION** (ZPass, Bool64, Bool32; CONTINUE — OR with the previous one): draws and
  dispatches with the predicate bit set in their header are skipped. The predicate is computed at
  the first such packet, after the GPU has executed the work before it (correct, but with a GPU
  wait; Bloodborne does not use predication). `VK_EXT_conditional_rendering` did not fit: it would
  also skip our internal dispatches (texture detiling, copies), and does not cross command buffer
  boundaries.

Verification: `BB_PM4_SELFTEST=1` — after ~40 s of gameplay test packets are mixed into the game's
submissions (into memory allocated by the test itself), a few seconds later the results are
checked: occlusion over a submission with draws (> 0 samples) and an empty interval (0),
`COPY_DATA` (32 and 64 bits, memory → memory in order, clock), `COND_EXEC` (false skips the write,
true executes it), predication (ZPass and Bool32: 2 of 4 empty predicated draws skipped). All 8 —
PASS. The Vulkan validation layer found no query rule violations over 67k events.

Along the way a deadlock was found and fixed: a memory read for the CPU from the parsing thread
(`ReadMemory`) sent a task to that same thread and waited for it.

What remains untranslated: compute queues (ASC) still run on the same thread as graphics and are
submitted to the same Vulkan queue (this is acceptable serialisation); `MEM_SEMAPHORE` changes the
counter on the CPU at parse time; GPU time in labels and `COPY_DATA` is the CPU clock. Bloodborne
uses none of this.

## Addendum 2026-10-09: copy shader in the memory module and GPU labels without the AMD extension

In `BB_PC_MODEL=1 BB_LAYER_MEMORY=1` the original `fefebf9f` now runs by default: paged stores update game memory and the current mirrors. HLE is available via `BB_COPY_SHADER_NATIVE=0`.

Without `VK_AMD_buffer_marker`, EOP/EOS labels and applicable RELEASE_MEM are written by the GPU with standard Vulkan updates with barriers. AMD keeps the fast markers; `BB_GPU_LABELS_PORTABLE=1` lets you test the portable variant. Details and hardware limitations: [TRANSLATION_GPU_COPY_LABELS.md](TRANSLATION_GPU_COPY_LABELS.md).

## Plan from 2026-10-10: what remains and how to move it to translation

Requirement: the same for AMD and NVIDIA and without relying on Bloodborne-specific code (game
function addresses, hashes of its shaders) where the translation model is responsible for
correctness.

| What remains | Where | Why it is emulation or a dependency |
|---|---|---|
| Memory model 0.3 (VRAM copies with page protection, readback, `fault_manager`, BB_UFFD, preloading) | **default** | does in place of the GPU what a PC does not have: one memory |
| Two paths of the new model: sparse arena (AMD) and memory module (others) | new model | sparse rebinding is a single-vendor path (on NVIDIA it stalls the GPU for seconds) |
| Loader writes into textures and buffers become visible to the GPU via hooks into Bloodborne code (the game's `memcpy` at 0x216d420/0x2171d60, allocator 0x26aa070, TLSF heap) | new model | another game does not have these addresses: writes would go unnoticed |
| `MEM_SEMAPHORE` changes at parse time (ahead of the GPU), GPU time in labels and `COPY_DATA` is the CPU clock, compute queues are on one thread with graphics | command processor | Bloodborne does not use them; needed for other games |
| CE and SRT walker read descriptors on the CPU, texture cache by address, synthetic vblank | — | binding translation and platform layer, as in DXVK/vkd3d (step 4): stays |

Stages:

1. **One memory path for all GPUs.** Memory module by default on AMD too (on the 2026-10-09 route
   on par with the sparse arena: p99 5.1–7.5 ms vs 5.2–6.0). Sparse arena is the fallback
   `BB_LAYER_MEMORY=0`. At the same time, on AMD the game's own copy shader runs by default instead
   of the replacement keyed on its hash.
2. **Memory coherence without game hooks.** CPU writes to memory that the GPU holds as an image or
   copy are caught by a general mechanism (write traps per 64 KiB, as in the module), the
   Bloodborne hooks remain an acceleration (fewer trap hits). Check: a run with `BB_GUEST_HOOKS=0`
   and `BB_WRITE_VERIFY=1` (undeclared writes), image and frame time.
3. **Command processor leftovers onto the GPU.** `MEM_SEMAPHORE`: the signal is a GPU write in
   stream order; GPU time — Vulkan timestamps converted to the PS4 clock (100 MHz) and written by
   the GPU. Check — `BB_PM4_SELFTEST` (Bloodborne does not use them). Compute queues as a separate
   Vulkan queue — later, when there is a game to test with.
4. **New model by default** (after logs from NVIDIA on 0.5-pre4), then removal of model 0.3 and
   the sparse arena.

### Progress (2026-10-10)

- **Stage 1 done:** memory module by default on all GPUs (on AMD too; sparse arena —
  `BB_LAYER_MEMORY=0`), 0 `vkQueueBindSparse` calls.
- **Modularity:** game profiles `gpu/games/` (`game_profile.h`, `bloodborne.cpp`): hooks into game
  code, its code that writes data every frame, the copy shader hash — only there. The profile is
  selected by TITLE_ID; `BB_GAME_PROFILE=none` — the game as an unknown one, `BB_GUEST_HOOKS=0` —
  without hooks.
- **Stage 2, writes:** common accounting of write traps in the runtime (`runtime_memory_trap`,
  per-page reasons; `gpu/layer/bblayer_write_traps.h`): module VRAM copies and textures catch
  writes on the same pages without interfering with each other; the game's `mprotect` preserves the
  traps, a new mapping starts without them. Pages under textures are trapped by
  default (`BB_IMAGE_TRAPS=0` — as before), writes by libc and hooked loaders are declared before
  the write (no hits). On the route with the profile — same as without traps (4.18 ms GPU), 4–29
  hits per 5 s.
- **Found along the way — the "some runs 1.3–1.7 times slower" spread:** "volatile" module blocks
  (blocks the CPU writes to, within a shared VRAM copy of a range) were updated by a copy before
  every read binding; the copy breaks the render pass, and light volumes (one draw each) broke
  their pass up to ~100 times per frame (`BB_PASS_BREAK_TRACE=1`: 92–109k breaks per 5 s from
  `Runtime::CopyBuffer` vs ~700). Volatile blocks are disabled by default (`BB_LAYER_VOLATILE=1` —
  enable; then reads inside an open pass go in place): 10 of 10 runs fast. The order "occlusion
  results before subsequent labels" (`OcclusionQueries::WriteBeforeLabel`) was kept as PS4
  behaviour, but was not the cause: the game reads the results 2–10 ms later and finds them written
  (`BB_OCCLUSION_READ_TRACE=1`, read traps in the runtime — reasons from 16 make the page
  no-access).
- **Unknown game (stage 2, reads):** the earlier conclusion "without a profile — slow mode again"
  was the same spread caused by volatile blocks (one run). Without a profile the route runs just as
  fast (3 of 3, 4.29–4.30 ms) and looks the same. Read traps for data that exists only in VRAM (copy
  back on CPU read) remain necessary for correctness in other games; the runtime "no-access" reason
  mechanism already exists and is covered by a test.
- **Stage 3 done (except the separate compute queue):**
  - `MEM_SEMAPHORE`: the signal is in stream order, after the GPU work before it completes
    (`RunMemSemaphoreSignal`, like EOP labels); the wait flushes the recording pipeline and submits
    what was recorded, like `WAIT_REG_MEM`. `BB_MEM_SEMAPHORE_ORDER=0` — signal at parse time (as
    before).
  - GPU clock: PS4 has 100 MHz; now there is one scale for CPU and GPU — the host monotonic clock in
    10 ns ticks (`GetGpuClock64`, previously wall-clock nanoseconds). EOP/RELEASE_MEM labels with
    the clock and `COPY_DATA` from the clock are written by the GPU itself: Vulkan timestamp → copy
    from the query → a short shader (`gpu_timestamp.comp`) converts to the PS4 scale and writes to
    game memory (`vk_timestamps.cpp`). Ticks are tied to the host clock once, on an idle GPU (on an
    RX 7800 XT accuracy is 152 µs). `BB_GPU_TIMESTAMPS=0` — the CPU writes.
  - Check — `BB_PM4_SELFTEST=1`: 11 of 11; new checks "signal not earlier than the preceding label"
    (with the old order — FAIL), "wait and decrement", "100 MHz clock in the CPU scale" (2 s between
    labels: 1.961–1.999 s). Bloodborne uses neither.
- **Stage 2, reads — ready, enabled with `BB_LAYER_READ_TRAPS=1`:** what the GPU wrote only into
  the VRAM copy (binding write into the mirror) is under a "no-access" trap (reason `VramData` in
  the runtime); the first CPU touch (game, translator, runtime libc) copies the block data back
  through the readback queue (`FastReadback`, as in model 0.3: waits only for that write's tick)
  and removes the trap. A file write from such a page first reads it (`touch_for_read`), a file read
  into it has already touched it (`touch_for_write`). On the route: 60 MiB under traps, 0–1 touches
  per 10 s, GPU time the same (4.28–4.30 ms). Bloodborne does not read such data on the CPU, so the
  copy back itself is not tested here — disabled by default until a game needs it.
- **Revised before 0.5 (evening of 2026-10-10):** on AMD the sparse arena is the default again
  (`BB_LAYER_MEMORY=1` — module). The module was on par with the arena on the Yharnam route, but in
  the Hunter's Dream it kept ~1 GB of area data outside VRAM (ranges with several "unknown" blocks
  without volatile blocks stay in place): 100 FPS vs 154 with the arena; with volatile blocks — 120,
  but with ~225k trap hits per second. Per-block access for mixed ranges is needed.
  Occlusion queries as Vulkan queries are optional (`BB_OCCLUSION_QUERIES=1`): with the upscaler
  (smaller and jittered scene) the lamp glow flashed grey over the whole frame; counting needs to be
  made robust to this. Background compilation no longer skips fullscreen passes (grey frames).
