# Original GPU copying and portable labels

Snapshot of 09.10.2026. Scope of verification: `BB_PC_MODEL=1 BB_LAYER_MEMORY=1`; new model, without evaluating the old memory model.

## Original copy shader

In game-memory-in-place mode the shader `fefebf9f` is now translated to SPIR-V and executed on the GPU by default. Its copy list is read by the shader itself at execution; the CPU does not replace it with a list of Vulkan copies. `BB_COPY_SHADER_NATIVE=0` keeps the HLE for comparison. Experiment 2 still inverts the choice.

For large bindings the BDA table contains two views: the first half holds the current addresses (mirror or game memory), the second the game memory addresses. The existing addressing of the first half is kept for DMA/ReadConst. The binding description contains a write-through flag and the offset of the second table.

The original copy shader writes the result to the current GPU address and to game memory. Therefore CPU consumers see the result after synchronization, and GPU consumers do not read a stale mirror. When both writes address game memory, they give the same result. Other compute shaders keep their own accounting of GPU ownership of mirrors. Write-through writes do not mark the whole mirror of a huge binding as needing copy-back; previously accumulated GPU writes are kept in the accounting.

The range check does not allow `offset + size` to overflow; a rejected access does not index an arbitrary table page. The shader binary cache version was raised to 9. The cache profile now contains the binary/meta/pipeline versions: when they change, the whole cache is rebuilt before modules are loaded. Old metadata and SPIR-V with matching variation index names are not mixed with new ones.

`run.sh` sets `BB_GPU_USER_DIR` before the process starts, from `BB_USER_DIR` or `$BB_DATA_DIR/user`. GPU directories are created when the library is loaded, so setting this value inside `bbgpu_init` was too late. This ensures the correct cache selection for the AppImage and for separate save profiles; an explicit `BB_GPU_USER_DIR` is kept.

## EOP/EOS and RELEASE_MEM

If `VK_AMD_buffer_marker` is available, the fast AMD path is kept. Without the extension the GPU writes the label via the standard `vkCmdUpdateBuffer`. `BB_GPU_LABELS_PORTABLE=1` forces this path and allows checking it on AMD. `BB_GPU_LABELS=0` keeps the previous CPU callback after GPU completion.

The portable path ends the current render pass, publishes preceding GPU writes to the CPU, writes the upper half of the 64-bit label, sets a dependency and writes the lower half. The following dependency ensures the label is visible to the GPU and the CPU. The scheduler notification and WAIT_REG_MEM handling are kept. Unsuitable addresses and labels with an interrupt keep their respective fallback paths.

The standard command is allowed outside a render pass: [vkCmdUpdateBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdUpdateBuffer.html). The stage and access masks follow the [Vulkan synchronization rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html). Ending passes and barriers have a cost; the forced portable path on AMD is not selected by default.

During verification two dependencies of neighbouring paths were also fixed: update/inline data are accounted at the CLEAR stage, and the index buffer range is accounted after the draw, so that a subsequent upload into the mirror waits for the index reads.

## Checks

The PM4 self-test measures a positive occlusion interval across several submissions. A large separate submission could contain only compute/state and give a false FAIL. The requirements of positive samples, valid bits, zero interval and predication are kept.

Hardware verification was done on RX 7800 XT / RADV. The portable path was checked forced on AMD; execution on real NVIDIA, its host memory import and a long playthrough of the game require a separate hardware run.

Local logs, copies of sources before changes, measurements and snapshots: `out/dev/translation-fixes-20261009/`. Game checks use copies of saves and settings. The original saves and settings are not replaced. Logs and game data are not included in the release.
