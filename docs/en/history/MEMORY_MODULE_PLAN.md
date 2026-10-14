# Layer memory module: PS4 GPU addresses without sparse (plan, 2026-10-09)

## Why

The new memory model keeps game memory in a single sparse arena and moves blocks between game
memory and VRAM by rebinding (`vkQueueBindSparse`). On NVIDIA (Linux, drivers ≥ 555) bind time
grows with the number of pages already bound, and the call holds a global lock: every move to
VRAM cost seconds, and the game and the desktop froze (GTX 1660 Ti, 615.71). Without moves
(`BB_FIXED_ARENA=1`) — 48.6 FPS versus ~205 on an RX 7800 XT. We need a model that works the same
on AMD, NVIDIA and Intel and is not tied to shadPS4's buffer cache: part of the future layer for
running PS4 games.

## Idea

The PS4 GPU has its own virtual memory (GPUVM) over shared memory; every V#, T#, pointer in user
data is a virtual address. On PC we do the same with Vulkan 1.2 means (BDA):

- **Memory sources** — ordinary (non-sparse) Vulkan buffers:
  - game memory chunks: dma-buf (AMD) or host memory import (NVIDIA, Intel), one chunk per game
    direct memory allocation (allocations are mapped whole — a range inside one mapping is
    always contiguous in one chunk);
  - VRAM mirrors: segments aligned to game addresses (game page X lies in the segment at the
    same offset), allocated when the first VRAM page appears in the segment.
- **Page table** (a buffer on the GPU): PS4 address page → device address (BDA) of its current
  source. Changed by a copy in the command stream — in order with draws, without semaphores.
- **Direct path**: the V# range is entirely in one source (almost all bindings) → an ordinary
  descriptor (source buffer, offset, size). The shader translator does not change.
- **Paged path**: the range is mixed (≈85 huge bindings per frame: shaders bound to all of
  memory) → the descriptor points to a record {PS4 address, size}, the shader translates each
  address through the table. A pipeline variant only for such bindings (specialization key).
- **Vertices, indices, indirect arguments, copies, texture uploads**: direct path, otherwise
  gathering into a contiguous temporary buffer (read-only) or per-page copies (the CPU knows the
  table).
- **Moves to VRAM and back** — the policy of the current new model (data whose writes we know
  about: loaders, the GPU; unknown writers — in place), but the execution is a copy + a table
  write + source selection at bind time. Bindings recorded before the move read the old source —
  it is still valid; VRAM is freed once the GPU has passed those commands.

## Work order

1. **Module core** (`gpu/layer/`, no dependencies on shadPS4): sources, pages, table,
   `Resolve(address, size)` → a direct source or "mixed", walking over sources for copies.
   The command recording interface is abstract (copy, table write) so as not to pull in the
   scheduler.
2. **Hook-up without VRAM**: the new model serves ranges in place through chunk buffers rather
   than through the arena; the sparse arena is not created in the new model. Expected: ~48 FPS
   on AMD (like `BB_FIXED_ARENA`), no `vkQueueBindSparse` at all. Check the image, then a build
   for NVIDIA.
3. **VRAM mirrors + moves**: the policy from `BufferCache`, the page table, the paged path in
   shaders for mixed bindings. Goal — parity with the sparse arena on AMD (~205 FPS).
4. **NVIDIA/Intel**: test build, tester log; then remove sparse from the new model entirely.

Measurements — RX 7800 XT, standing still (`tools/bench.sh`), `BB_PC_MODEL=1`; diagnostics
`BB_RESIDENCY_MIX=1`.

## Status (2026-10-09, morning)

- Steps 1–3 done. On RX 7800 XT standing still: module ~205 FPS, sparse arena ~205–238, old model
  ~220 (at the lamp in the Hunter's Nightmare); at another spot (1170 draw calls) — 167.5 / 164.5 /
  146. `vkQueueBindSparse` is never called in module mode (counter in the `Layer memory:` line).
- Mixed bindings are solved without the paged path: "volatile" blocks (`layer_volatile`) — a
  block with unknown bytes (range edge, gap between images) goes into the mirror together with
  the rest of the range, is refreshed from game memory before each binding, and what the dispatch
  wrote into it is copied back after the dispatch (`LayerAfterBindings`). The paged path remains
  for bindings of 64 MiB and up. `BB_LAYER_VOLATILE=0` — turn off (for comparison).
- Default: AMD — sparse arena, other GPUs — the module (`BB_LAYER_MEMORY=1/0` — manually). Step 4
  (NVIDIA): needs a tester log; the host memory import path cannot be checked on AMD (RADV does
  not import memfd).

## Addendum: original copying and portable labels (2026-10-09)

In the new module the paged path of the original copy shader is fixed: writes update game memory and the current mirror; the native shader is now selected by default. For GPU labels a standard Vulkan path without the AMD buffer marker was added. Implementation, switches and the limits of verification are described in [TRANSLATION_GPU_COPY_LABELS.md](TRANSLATION_GPU_COPY_LABELS.md). This removes the dependency of label writes on the AMD extension, but does not replace hardware verification on NVIDIA.
