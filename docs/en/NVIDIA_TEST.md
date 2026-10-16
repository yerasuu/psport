# Testing the PC memory model on NVIDIA

The commit "Remove the 0.3 memory model so the PC memory model runs on every GPU" makes the PC
model the only one on every GPU. On NVIDIA this means the layer's memory module over host memory
the GPU imports (`VK_EXT_external_memory_host`); before, `run.sh` gave NVIDIA the 0.3 model. It was
checked on AMD only, so an NVIDIA run is still owed. Decanter is not needed.

## Build

```sh
bash build.sh          # enters shell.nix through nix-shell when Vulkan/SDL3 dev files are missing
```

The build leaves `out/psport` and its GPU library `out/libpsport-gpu.so`; `run.sh` and
`tools/regress.sh` start them from there.

For the comparison, keep a build of the commit before the removal in a second worktree
(`git worktree add ../psport-03 <that commit>^ && (cd ../psport-03 && bash build.sh)`).

## Runs

Headless, one game at a time (needs `gamescope`), `GAMES` being the folder that holds the game
folders named in `tools/regress/titles.txt`:

```sh
tools/regress.sh -j 1 -o out/regress/nvidia-pc \
    -n bloodborne,ryza,ayesha,symphonia,astrea,saga,altdeus,bo2,galguardians2,galguardians,bloodomen2,onigiri,bo1 \
    "$GAMES"
```

Leave `octopath0` out (it hangs the GPU). Titles you do not have are listed as skipped. Then the
same command from the second worktree with `-o out/regress/nvidia-03` (it runs the 0.3 model on
NVIDIA) and `-c` pointing at the first run's results to see what differs.

Bloodborne's frame rate, twice on each build, nothing else running (the seed folder holds a save as
`<seed>/bloodborne/user/savedata`; a copy of the AMD machine's `bb-perf-seed` works):

```sh
PS_FRAME_STATS=1 tools/regress.sh -j 1 -t tools/regress/perf.txt -n bloodborne -u <seed> \
    -o out/regress/nvidia-perf-1 "$GAMES"
grep '^Frame stats' out/regress/nvidia-perf-1/bloodborne/log | tail -10 | awk '{s+=$3} END {printf "%.1f\n", s/NR}'
```

Playing by hand instead: `PS_GAME_DIR=<game folder> PS_SAVE_LOG=1 bash run.sh` (the log goes to
`logs/`; settings such as `output_res` and `upscaler` come from `psport.ini` in the data folder,
copied once from an older `bbport.ini`). Variables are `PS_*`; an older launcher's `BB_*` names
still work behind a deprecation line.

## What to look for in each `log`

Expected near the start:

- `Guest memory: host memory imported by the GPU (VK_EXT_external_memory_host, alignment N)`
- `Guest memory: detected ... sparse_bind_ms=X ... dmabuf=whole ... host_import=works backing=layer-module backing_why="HOST quirk nvidia_slow_sparse_rebind" chunks=host-import ...`
  (send this line back: `sparse_bind_ms` says whether the startup test bind alone would have
  caught NVIDIA's slow sparse binding)
- `Guest memory: direct memory chunks come from Vulkan`
- `GPU: end-of-pipe labels are written by the GPU (portable Vulkan updates; ...)`

and every 10 s `Layer memory: ... vkQueueBindSparse: 0 calls, 0 ranges; write traps hit N`.

Problems:

- `Guest memory: ... The port needs the GPU to use the game's memory in place ...`: the launch
  stopped because the driver can neither import host memory nor map dma-buf memory (the line
  before it, `host memory import does not work here: ...`, says why). There is no fallback now.
- `Guest memory: vkQueueBindSparse of N ranges took X ms`, or a non-zero `vkQueueBindSparse` count:
  sparse binding is used where it must not be.
- `VK_ERROR_DEVICE_LOST`, `Unimplemented PM4 type`, `guest fault`, a frozen desktop.
- A title that gets less far than with the 0.3 model in `results.tsv` (columns `gpu`, `frames`).

`Memory model: the PC one, the only one now; ignored: PS_PC_MODEL=0` only means an old setting was
passed in (also as `BB_PC_MODEL`, `BB_AS_0_3`...); it is harmless.

## What to send back

- `nvidia-smi` (GPU, VRAM, driver version) and the distribution.
- `results.tsv` of both regress runs, and the `log` of every title that differs or fails.
- The four Bloodborne FPS numbers (each build twice) and the perf runs' `shots/`.
- Anything visibly wrong in the `shots/` frame dumps (flickering, garbage textures, exploding
  vertices), with the title and the file name.
