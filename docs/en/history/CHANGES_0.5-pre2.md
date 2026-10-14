# 0.5-pre2 — pre-release for testing

Second 0.5 prerelease, built from the `testing` branch. This release focuses on the experimental
memory and GPU command translation path. Changes are listed relative to
[0.5-pre1](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre1).

- **New memory module for NVIDIA and other non-AMD GPUs.** When the experimental mode is enabled,
  the layer binds chunks of game memory directly and keeps frequently used data in separate VRAM
  buffers. It does not use `vkQueueBindSparse`. The launcher now exposes the mode on all GPU
  vendors; the startup capability check is retained. AMD still selects the sparse arena by
  default within this mode; `BB_LAYER_MEMORY=1` selects the new module manually.
- **Memory coherence fixes.** CPU writes to mirrored blocks are tracked selectively. Buffer
  bindings and command-processor copies use the current GPU data, CPU writes invalidate stale
  GPU ownership, and mixed ranges can use refreshed VRAM fringes. Promotion respects the VRAM
  budget, and cached binding decisions reduce repeated checks and locking.
- **Original GPU copy shader enabled by default in the new model.** The game's copy shader is
  translated to SPIR-V and reads its copy list on the GPU. Its writes update both the current
  GPU view and game memory, keeping VRAM mirrors and CPU consumers consistent.
  `BB_COPY_SHADER_NATIVE=0` retains the HLE path for comparison.
- **Portable GPU completion labels.** Supported EOP/EOS/RELEASE_MEM labels can be written by
  standard Vulkan commands on GPUs without `VK_AMD_buffer_marker`. Prior GPU results become
  visible before the completion label; the AMD fast path remains available. The portable path
  can be forced for testing with `BB_GPU_LABELS_PORTABLE=1`.
- **Synchronization and bounds fixes.** Corrected update/inline-data stage dependencies and
  index-buffer read tracking. Paged buffer accesses reject invalid ranges before accessing the
  page table. Vulkan initialization also enables the required surface extension for headless
  validation.
- **Shader-cache migration.** The shader binary format is now version 9. Binary, metadata and
  pipeline versions are checked together before loading the cache, preventing old and new
  shader variants from being mixed. AppImage startup chooses the GPU cache directory before
  loading the GPU library, following the selected user-data directory.
- **Regression checks.** Added GPU tests for paged copies, mirror/guest writes, range guards and
  32/64-bit portable labels. The PM4 occlusion check now spans multiple submissions to avoid
  false failures when a single submission contains no draws.

**How to test:** enable *New memory and translation model* in the launcher, or set
`BB_PC_MODEL=1`. The experimental mode remains **off by default**. For testing the new module
on AMD, also set `BB_LAYER_MEMORY=1`.

**Validation and limits:** runtime tests, packaged Vulkan-driver discovery tests, the new GPU
tests and all eight in-game PM4 checks passed. GPU and AppImage checks used an RX 7800 XT/RADV;
the portable label path was forced on AMD. Physical NVIDIA/Intel hardware and NVIDIA host-memory
import have not been tested. Full-game Vulkan validation still reports graphics-pipeline and
image-layout errors. A consistent FPS improvement over the previous copy path has not been
established, and long gameplay sessions still need testing.

The earlier 0.5 changes remain included: settings in the game's System menu, display/monitor
selection, effect and patch controls, the updated overlay, launcher log export, Chinese
translation, DLC add-on folders and atomic save-file writes. See the
[0.5-pre1 notes](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre1) for details.

The AppImage contains no game files. Proprietary FSR 4 model assets and NVIDIA's DLSS SDK/libraries
are not bundled. `SHA256SUMS` accompanies the download.
