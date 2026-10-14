# 0.5-pre4 — pre-release for testing

Fourth 0.5 prerelease, built from the `testing` branch, after the first tests of 0.5-pre3 on
NVIDIA (GTX 1060 6 GB, GTX 1660 Ti 6 GB and a 4 GB card). Changes are listed relative to
[0.5-pre3](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5-pre3). Details and
measurements: [NVIDIA_MEMORY_MODEL.md](https://github.com/deadinside28/bloodborne_pc/blob/testing/docs/NVIDIA_MEMORY_MODEL.ru.md).

- **The new memory model on NVIDIA no longer stutters while areas stream in.** On NVIDIA (and
  any GPU other than AMD) the new model keeps VRAM copies of the game's memory itself. After
  each run of blocks it copied to VRAM, it rewrote the page table entries of the whole copy (up
  to 160 000 entries in one frame), looking up every block in the runtime's memory map under a
  lock the game's threads hold while they map memory: the GPU thread waited 30–70 ms in frames
  where an area streamed in. Only the entries that changed are written now, with one lookup per
  mapping. On a recorded route (RX 7800 XT forced into this path, five runs) the 99th percentile
  frame went from 11–13 ms to 5.1–7.5 ms (9.4 ms in a run where the GPU itself ran slower), the
  worst frame from 32–57 ms to 9–16 ms, close to AMD's default path (5.2–6.0 ms).
- **No more copying the same memory to VRAM and back every frame.** Where the GPU writes
  memory the CPU also wrote, the write goes to the game's memory; a reading binding then copied
  the range to VRAM again, the next writing binding sent it back, every frame (seen at one
  spot with 42 FPS: about 200 MB/s each way). A range sent
  back for the GPU's writes a second time now stays where it is. Write traps on VRAM copies are
  set and cleared with one call per run of blocks instead of one per 64 KiB.
- **Fewer shader-compilation hitches.** A new graphics pipeline of a pass that draws into a
  render target used in each of the last 8 frames is compiled on worker threads while its draws
  go without it for a frame or two. Translating a new shader and compute pipelines still happen at once, and so
  do passes rendered once and loading screens. `BB_ASYNC_PIPELINES=0` restores the old
  behaviour, `BB_ASYNC_PIPELINE_THREADS=N` sets the worker count. The drivers' own shader caches
  are kept between sessions (NVIDIA: no cleanup, 10 GB; Mesa: 4 GB). As before, the first start
  of a new build compiles its pipeline cache again.
- **4 GB cards: the new memory model no longer collapses to ~5 FPS.** 0.5-pre3 stopped copying
  the game's memory to VRAM short of the texture collector's critical mark; on a 4 GB card the
  textures alone held the usage there, so no copy was made again and everything was read over
  the bus after a few minutes. VRAM copies now have their own share (25 % of the driver's
  budget, `BB_LAYER_MIRROR_FLOOR_PERCENT`) that the texture collector's mark does not take.
  Cards with 4 GB are still faster with the 0.3 memory model.
- **No VRAM left behind after travelling.** Copies of an area left behind stayed in VRAM (large
  bindings over all memory marked every copy as in use), and the copies of the new area waited,
  read over the bus (about 10 FPS less after a lamp travel on a GTX 1060). When VRAM is short,
  copies not bound directly for 3 s now go back first, and copies with memory left without
  valid data are made again over their valid data only, giving that memory back.
- **Damaged game extractions are reported instead of hanging (#81).** Some extraction tools
  (LibOrbisPkg PkgTool on .NET 6 or newer) leave parts of compressed sectors stale without an
  error; the game then hung while loading shaders (`shaderBinarySize … is not equal to Program
  size`). The launcher and the game's preparation now unpack the shader files (under a second)
  and say that the extraction is damaged. `scripts/game_check.py GAME_DIR` checks every file.
- **NVIDIA driver diagnostics (#107).** `--vulkan-info` now lists the NVIDIA libraries linked
  for the package, the kernel module, the `/dev/nvidia*` nodes, hybrid-graphics variables and
  the NVIDIA libraries the dynamic linker searched for and did not load: one log shows why the
  host driver gave no `vkCreateInstance` inside the AppImage.
- **A crash window in command recording closed.** Handing recorded commands to the recording
  threads left the current command chunk empty for a moment; anything recording on that thread
  then (a fault handler) read a null chunk. The same crash was reported for a Windows build
  (#100). The replacement chunk is now swapped in before the old one leaves.
- **The in-game settings menu (Insert, L3+R3) looks as in 0.3 and 0.4 again:** one movable window
  with sections, check boxes, sliders and `(?)` hints, keyboard and gamepad navigation. Its
  content is that of 0.5 (DLSS, debug views, current hints).

**How to test:** enable *New memory and translation model* in the launcher, or set
`BB_PC_MODEL=1`; the mode remains **off by default**. On AMD it uses the sparse arena;
`BB_LAYER_MEMORY=1` forces NVIDIA's path there. Logs with `BB_FRAME_STATS=1` show compiles per 5 s,
a line `GPU: N pipelines compiled in the background, M draws went without theirs`, and for
frames over 40 ms where the new model's VRAM copies spent their time (`layer memory ms`).

**Validation and limits:** RX 7800 XT/RADV, a recorded 25 s route: AMD's default path p99
5.2–6.0 ms; NVIDIA's path forced, before/after as above; no frames over 20 ms in either. A 4-minute
session on NVIDIA's path with the VRAM budget limited to 4.5 GB (`BB_VRAM_LIMIT_MB=4608`): 177 FPS
on average, p99 9.7 ms, VRAM at most 3.0 GB; 11 of the 12 frames over 40 ms were compiles or the
loading screen. Runtime, unit (106) and GPU tests passed. NVIDIA's own path (the game's memory imported as
host memory) cannot run on AMD: the gains there are not measured yet, logs from NVIDIA testers
are welcome. Physical Intel GPUs are not tested.

The AppImage contains no game files. Proprietary FSR 4 model assets and NVIDIA's DLSS SDK/libraries
are not bundled. `SHA256SUMS` accompanies the download.
