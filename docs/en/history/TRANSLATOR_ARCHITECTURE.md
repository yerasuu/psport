# Our own translator instead of shadPS4: design and work order (2026-10-10)

Goal: move away from emulation and from shadPS4 code — our own PS4 command translator and our own
graphics renderer that scale to other games (everything about one game lives in its profile,
`gpu/games/`), the same for AMD and NVIDIA. Then — shadnet (online play).

## Where we are

Borrowed from shadPS4 (`gpu/VENDOR.txt`, commit b4e7ae7 of 2026-09-24), about 116 thousand lines:

| Part | Lines | What it does | Ours in it |
|---|---|---|---|
| `video_core/amdgpu` | 8.3k | PM4 parsing (command processor `liverpool.cpp`), registers, tiling | GPU-written labels, occlusion, predication, PM4 self-test |
| `video_core/renderer_vulkan` | 28.6k | Vulkan renderer: pipelines, rasterizer, scheduler, presenter | draw pipeline (stages A/B), recording threads, upscalers, background compilation |
| `video_core/buffer_cache` | 7.4k | game memory for the GPU | almost all rewritten (memory module, VRAM copies, traps) |
| `video_core/texture_cache` | 4.8k | textures by address, detiling | time-based collector, uploads |
| `shader_recompiler` | 38.6k | GCN → IR → SPIR-V | paged buffers (BDA), fixes |
| `core`, `common` | 21.8k | VideoOut, GnmDriver, event queues, utilities | vblank, labels |

Ours, outside shadPS4 code: `gpu/layer/` (memory module `BbLayer::GpuMemory`), `gpu/games/` (game
profiles), `gpu/shim/` (overlay, settings, hooks), the runtime `src/` (kernel, libc, files, saves).

## The design we are heading to

Modules with narrow interfaces; none of them knows game code addresses (that is the profile) or
does GPU work on the CPU (that is emulation):

1. **memory** — game memory in place, VRAM copies as a cache, shared page protection accounting
   (write traps for copies and textures). Exists: `layer/` + the module in
   `buffer_cache_layer.cpp`.
2. **cp** — command processor: parsing PM4 into the translator's command stream (state, draws,
   dispatches, synchronization) in GPU execution order. Replaces `liverpool.cpp`.
3. **state** — context registers → pipeline keys and Vulkan dynamic state. Replaces
   `liverpool_to_vk`, key building in `vk_pipeline_cache`.
4. **resources** — V#/T#/S# descriptors → buffers, images, samplers (caches, detiling). Replaces
   `buffer_cache`, `texture_cache` (the buffer part is already almost ours).
5. **render** — Vulkan: per-thread command recording, barriers, submission, presentation.
   Replaces `renderer_vulkan` (the scheduler and stages A/B are already ours).
6. **shaders** — GCN → SPIR-V. The largest part; replaced last, by stages (decoder, IR, passes,
   generation).
7. **platform** — VideoOut, GnmDriver, kernel events (`core/libraries`), next to the runtime.
8. **games** — game profiles (exists).

## Order

Replacement one module at a time: first an interface in our code with shadPS4 code behind it as
is, then our implementation behind the same interface. After each step: the route
(`route_bench.sh`, frames and snapshots), the PM4 self-test (`BB_PM4_SELFTEST=1`), unit tests,
the game on AMD and (from tester logs) NVIDIA.

| Step | What | Why in this order |
|---|---|---|
| 1 | memory: shared page protection accounting; traps under textures by default; model 0.3 and the sparse arena are removed | removes the remaining memory emulation and the dependency on game hooks |
| 2 | cp: translator command stream; `MEM_SEMAPHORE`, GPU time, compute queues — by the GPU | the last emulation remains in the command processor |
| 3 | state + resources | rely on 1 and 2; the buffer part is already ours |
| 4 | render | the scheduler and stages are already ours; the rasterizer and pipelines remain |
| 5 | shaders | the largest and riskiest; by then the rest is already ours |
| 6 | shadnet | networking, once the graphics are our own |

Steps 1 and 2 are the plan for moving away from emulation (`EMULATION_REMOVAL_PLAN.md`, section of
2026-10-10).

## Progress

### 2026-10-10: our own PM4 decoder (step 2, start)

- `gpu/cp/` — our own code, without shadPS4: packet format and opcodes (`cp_pm4.h`), register
  file and decoder (`cp_decoder.{h,cpp}`). The decoder walks the buffer in stream order, applies
  register packets (`SET_*_REG`, `CLEAR_STATE`, registers written by draw packets), descends into
  nested buffers and hands each packet to a "sink" (`BbCp::Sink`) with the state already applied:
  draws and dispatches — typed, the rest — as packets. `COND_EXEC` is decided by the sink. The
  decoder knows nothing about Vulkan, the game or where things execute.
- Register addresses are checked against the shadPS4 layout at build time (`static_assert`):
  this found a bug — on CIK `CP_STRMOUT_CNTL` is in uconfig (`0xC03F`), not in config.
- Shadow check in game (`BB_CP_SHADOW=1`): the new decoder parses every top-level buffer from
  the same state, and at the end of the buffer the register files are compared. On the route all
  buffers matched (~800 thousand buffers, ~200 million packets, 0 mismatches, 0 parse errors).
  Bloodborne does not use nested buffers (checked by a unit test).
- Next: the decoder as a cursor (packet by packet, so that the command processor loop can yield
  to other queues on waits), the register file as shared state storage (layout matches shadPS4's
  `Regs` word for word), then moving packet handling from `liverpool.cpp` into the translator's
  sink one packet type at a time, each time with the shadow check and the route.

### 2026-10-10: the new decoder — the real parsing path

- The decoder became a cursor (`BbCp::Cursor`: packet by packet, nested buffers — at the caller's
  choice), the register file can live in the owner's storage (for the command processor — its
  `regs`, same word layout), a write observer (`RegisterObserver`, one call per packet) provides
  what draw preparation needs: the written blocks and the packet checksum.
- `ProcessGraphics` takes packets and register writes from the cursor (`BB_CP_DECODER=0` — the
  old walk); what each packet does is still the old code. PM4 self-check — 11 of 11; the share of
  prepared draws is the same (747 thousand used, 19 thousand not, versus 755/24 thousand) —
  checksums match; the image is the same.
