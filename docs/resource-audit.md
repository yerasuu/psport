# Resource audit: from manual levers to automatic management (kb #1251)

Scope: every `PS_*` variable and setting that changes how psport uses VRAM, host memory, threads or
CPU cores, taken from `src/`, `gpu/` and `launcher/` (grep of `getenv`/`runtime_env`) and from
decanter (`crates/ps`, `app/src/views/ps`). Read-only audit of `develop` at 81988dbae2. Rule from
the owner (#1250): psport decides every resource itself from what the OS and the driver report and
adapts while running; `PS_*` stay only as test levers; no vendor id outside HOST quirks.

## 1. Findings that frame everything

1. **Nothing reads the host's memory state.** There is no use of `/proc/meminfo`, cgroup limits,
   PSI, `GlobalMemoryStatusEx` or `GetProcessMemoryInfo` anywhere in `src/` or `gpu/`. Host RAM
   (guest direct memory 5056 MiB, up to 9152 MiB with the Bloodborne heap patch, `PS_DMEM_MB`
   in `src/kernel/runtime_memory.c:20`, `gpu/games/bloodborne/patches.cpp:103,341`) is mapped
   without checking that the machine or its cgroup can hold it.
2. **The only dynamic signal is `VK_EXT_memory_budget`**, and it is used unevenly:
   - Texture-collector marks (`trigger/pressure/critical_gc_memory`) are computed once in the
     `TextureCache` constructor from the startup budget (`texture_cache.cpp:103-137`). They are
     re-derived from the live budget only on integrated GPUs or with `PS_GC_BUDGET_MB`
     (`texture_cache.cpp:1682-1693`). On a discrete GPU another process taking VRAM later leaves
     the marks stale; the only live reaction is `vram_short_until` at 92 % of the current budget
     (`texture_cache.cpp:1706-1713`).
   - Without the extension (`CanReportMemoryUsage()` false) the marks fall back to fixed 1.5 GB /
     3 GB (`texture_cache.h:46-47`, `texture_cache.cpp:96-100`) and promotions are never paused
     (`buffer_cache.cpp` `VramPromotionsPaused`).
3. **Fixed absolute constants sit beside percentages**: `PromoteCriticalMargin` 384 MiB,
   `StaleHeadroom` 256 MiB, `ShrinkCriticalMargin` 128 MiB (`buffer_cache.h:524-530`), the 1 GB
   system reserve and `/8` (`vk_instance.cpp:1087-1091`), the 8 GB integrated reserve
   (`vk_instance.cpp:1093-1096`), `DEFAULT_*_GC_MEMORY`, `TARGET_GC_THRESHOLD` 8 GB, the 2 GB
   vacancy floor (`texture_cache.cpp:111`), `MaxSparseBindMs` 250, `MinCachedReadMBps` 1000
   (`psport_guest_memory.cpp:594,555`), `ResidencyBlock` 64 MiB (`buffer_cache.cpp:1866`).
   Small budgets (<= 3 GB) break the absolute ones; the sweep below shows where.
4. **Thread counts use two different CPU sources.** `PsThreads::Available()` honours the affinity
   mask (`host_thread.c:85-89`, so `taskset` and cgroup cpusets work), but `vk_pipeline_cache.cpp:671`
   uses `std::thread::hardware_concurrency()`, which ignores both. Cgroup CPU quota
   (`cpu.max`) is read by neither.
5. **Sparse binding already follows the rule** (capability test bind + HOST quirk). It is the model
   to copy: measure, then decide, with the reason logged (`sz_backing_why`).

### Known measurement (wbc3, RTX 2080, `PS_VRAM_LIMIT_MB` sweep)

| Limit (MiB) | Result |
|---|---|
| 4096, 3072 | 60 FPS held |
| 2560, 2048 | about 28 % lower |
| <= 1024 | collapse |

With a limit of 2560 the budget is 2240 MiB (limit minus `min(limit/8, 1 GB)`), so 3 GB of
critical floor sat above it (#1245, fixed in `d85d267705`).

### Bug: texture-cache mark overflow below 2 GB (already fixed on develop)

Symptom: `Texture cache: 735 of 17592186044288 MiB` and `marks: ... pressure 17592186044288`.
17592186044288 MiB is 2^64 bytes in MiB: the mark became `0xFFFF...` so the pressure mode never
started and nothing was ever evicted by pressure.

Cause, in `gpu/shadps4/video_core/texture_cache/texture_cache.cpp` before `d0b055e238`
(lines 112-118 of that revision):

```
pressure_gc_memory = static_cast<u64>(
    std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                  DEFAULT_PRESSURE_GC_MEMORY));
critical_gc_memory = static_cast<u64>(
    std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                  DEFAULT_CRITICAL_GC_MEMORY));
```

`std::min(s64, s64)` is negative when the budget is under about 2 GB (the 2 GB vacancy floor at
line 111 and the `-1 GB` / `-512 MiB` spacings make it negative); `std::max<u64>` converts the
negative to a huge `u64`, which wins the max. The same conversion pattern remains a trap in the
`PS_GC_PRESSURE_PERCENT` block (`texture_cache.cpp:131-134`, `std::max<u64>(pressure_gc_memory,
u64(...))`), harmless today only because both operands are non-negative after the fix.

Status: fixed by `d0b055e238` (signed math, cap below 2 GB) and `d85d267705` (every mark capped at
the budget). Both are ancestors of `develop`. Residual weakness: with the cap, below about 3 GB the
pressure and critical marks both equal the budget (absolute floors clamped down), so the collector
only goes aggressive at 100 % use and relies on the 92 % `vram_short_until` path. Marks should be
percentages of the live budget (rule in section 3, R1), not floors that are clamped afterwards.
The fix needs a run at a 1024 and a 2048 MiB limit on wbc3 to confirm the log line is sane.

## 2. Inventory

Columns: variable or setting, file:line, what it controls, default and how it is chosen, whether the
default is already by capability, risk if wrong, recommended automatic rule. "Lever" means keep the
variable only as a test lever once the automatic rule exists.

### 2.1 VRAM budget and texture collector

| Variable | file:line | Controls | Default and origin | By capability? | Risk if wrong | Recommended rule |
|---|---|---|---|---|---|---|
| `PS_VRAM_LIMIT_MB` | `vk_instance.cpp:39`, applied `:1085`, `:1181` | Caps the driver budget the collector and copies see; allocations unlimited | none (0): budget = sum of device-local `heapBudget` minus `min(budget/8, 1 GB)` on discrete; fixed 1/8 and 1 GB | partly: budget is reported, reserve is a constant | too high: eviction thrash or OOM; too low: slow | Lever only. Reserve from reported usage by other processes (`heapUsage - ours`) instead of `/8, 1 GB`. |
| `PS_GC_BUDGET_MB` | `texture_cache.cpp:1682` | Forces the integrated-GPU marks rule with a fixed budget on any GPU | none; rule on when `IsIntegrated()` | rule keyed on `deviceType` (a capability) | marks never follow other processes on discrete | Make the live-budget rule (70/85/95 %) the rule for every GPU with `VK_EXT_memory_budget`; keep as lever. |
| `PS_GC_PRESSURE_PERCENT` | `texture_cache.cpp:131` | Raises the pressure mark below 8 GB budgets (10-75) | 60 | no: fixed percent | too low: eviction storm; too high: no headroom | Derive from `g_cb_live_image` vs usage (what is not textures): pressure = budget minus game's non-image memory plus margin. |
| `PS_GC_IDLE_SECONDS` | `texture_cache.cpp:1728` | Idle age for eviction below pressure | 20 s | no | VRAM grows over areas | Scale with pressure: age shrinks as usage/budget grows (continuous, not 20/5/1 steps). |
| `PS_GC_PRESSURE_IDLE_SECONDS` | `texture_cache.cpp:1738` | Idle age under pressure | 5 s | no | thrash or leak | Same continuous rule. |
| (constants) | `texture_cache.h:46-48`, `texture_cache.cpp:103-127` | `DEFAULT_PRESSURE` 1.5 GB, `DEFAULT_CRITICAL` 3 GB, `TARGET_GC_THRESHOLD` 8 GB, vacancy 2 GB | fixed | no | the overflow bug and #1245 | Express every mark as a fraction of the live budget; floors only as fractions. |
| `PS_VRAM_PROMOTE_PERCENT` | `buffer_cache.cpp:4001` | Usage percent past which VRAM copies of guest memory pause | 90 | no | stall on bus reads vs eviction of used textures | Pause threshold from `critical` and the budget headroom the collector can free (`g_cb_live_image`), re-read each second. |
| `PS_LAYER_MIRROR_FLOOR_PERCENT` | `buffer_cache.cpp:3987` | Mirrors' protected share of the budget (<= 90) | 25 | no | 4 GB card: all reads over the bus (5 FPS after 4 min, noted in code) | Floor = what a measured bus-read penalty justifies: derive from `ReBAR` size and PCIe link width where reported, else keep a share of the budget. |
| (constants) | `buffer_cache.h:524-530` | Margins 384/256/128 MiB | fixed | no | wrong on 1-2 GB budgets | Fractions of budget with a lower bound of one residency block. |
| `PS_VRAM_IDLE_SECONDS` | `buffer_cache.cpp:1993` | Idle age before a VRAM copy goes back in place | 60 s | no | VRAM held by idle mirrors | Scale with usage/budget as above. |
| `PS_VRAM_PRESSURE_IDLE_SECONDS` | `buffer_cache_layer.cpp:1126` | Same, under pressure | 3 s | no | same | Same. |
| `PS_RESIDENCY_VRAM_LIMIT_MB` | `buffer_cache.cpp:1872` | Test: VRAM "full" after N MiB of residency blocks | none | n/a (test lever) | none | Keep as lever. Real path already falls back to system memory on `ErrorOutOfDeviceMemory`. |
| `PS_VMA_BLOCK_MB` | `vk_instance.cpp:984` | VMA block size | 64 MiB (VMA default 256) | no | holes held by half-empty blocks; many small allocations | Scale from budget (e.g. budget/64, 16-128 MiB) and fall to the driver `maxMemoryAllocationCount` limit. |
| `PS_VMA_DEDICATED_MB` | `image.cpp:159` | Images at or over this size get dedicated memory | 4 MiB | no | allocation count limit (`maxMemoryAllocationCount`, often 4096) on many large images | Bound by `maxMemoryAllocationCount` and budget. |
| `PS_STAGING_KEEP_MB` | `vk_staging_buffer_pool.cpp:43` | Staging memory kept after use | 512 MiB | no | 512 MiB is 25 % of a 2 GB budget | Min(512 MiB, 5 % of budget), shrink on pressure. |
| `PS_IMAGES_PREFER_HOST` | `image.cpp:118` | Sampled textures to the shared heap | on when device-local < 768 MiB and a shared heap exists | yes (heap sizes) | none | Lever only. |
| `PS_GARLIC_VRAM` | `buffer_cache.cpp:457` | Garlic data cached in VRAM | on unless integrated or fixed arena | yes (`deviceType`, backing mode) | extra copies on integrated | Lever only. |
| `PS_GARLIC_VRAM_RANGE` | `buffer_cache.cpp:2245` | Debug range for VRAM copies | all | n/a | none | Diagnostic, leave. |
| `PS_FIXED_ARENA` | `buffer_cache.cpp:429` | Arena keeps first bindings (imported host memory) | `HostImported()` and not layer mode | yes (mode) | wrong forced value halves speed | Lever only. |
| `PS_GPU_WRITE_TWINS`, `_MAX` | `buffer_cache.cpp:301,309`; set by decanter `crates/ps/src/bbport/direct.rs:111-112` (=1, 65536) | Twin pages for small GPU-written ranges | off, 1024 B; decanter sets on, 64 KiB | no | memory held by twins; set outside psport by the launcher | Move the decision into psport (default on, max by page size) and drop the decanter override. |

### 2.2 Guest memory backing and sparse usage

| Variable | file:line | Controls | Default and origin | By capability? | Risk if wrong | Recommended rule |
|---|---|---|---|---|---|---|
| `PS_GUEST_MEMORY` | `psport_guest_memory.cpp:956` | `dmabuf` or `host` chunk backing | dma-buf if offsets map at cached speed (`MinCachedReadMBps` 1000), else host import | yes (probes + measured read speed) | wrong backing: GPU stall (RADV host import) or crawl | Lever only. Make the 1000 MB/s threshold relative to a measured host-RAM read of the same block. |
| `PS_GUEST_WHOLE_CHUNKS` | `psport_guest_memory.cpp:800` | Forces the offset-0-only (NVIDIA) path | off | n/a (test) | none | Lever only. |
| `PS_GUEST_VRAM` | `psport_guest_memory.cpp:47` | Guest memory in BAR VRAM | off (experiment) | n/a | CPU reads over PCIe | Candidate for automatic use when ReBAR size >= guest pool and measured CPU read/write speed passes; today off. |
| `PS_LAYER_MEMORY` | `psport_guest_memory.cpp:687,697,735` | Sparse arena (0) or layer memory (1) | arena only if sparseBinding + sparseResidencyBuffer + sparse queue, test bind <= 250 ms, no `nvidia_slow_sparse_rebind` HOST quirk (`runtime_quirks.inc:21-26`, driver id 4) | yes (capabilities + measured bind) plus a driver-id HOST quirk, allowed | the test bind cannot reach the NVIDIA slow rebind, hence the quirk | Lever only. Replace the quirk by a growth test (rebind time at 1 then N pages, extrapolate) when feasible; until then the quirk is permitted. |
| `PS_LAYER_GPU_WRITES` | `buffer_cache_layer.cpp:1480` | GPU writes into VRAM copies mode 0/1/2 | 1 | no | mode 2 gave exploding vertices (documented) | Correctness mode, not a resource: leave as lever. |
| `PS_LAYER_PAGED` | `buffer_cache_layer.cpp:1469` | Page table for huge bindings | on | no | none known | Lever only. |
| `PS_LAYER_MIRROR_GROWTH_MB` | `buffer_cache_layer.cpp:746` | Room a growing mirror gets | 64 MiB | no | fragmentation vs copies | Fraction of budget with floor 16 MiB. |
| `PS_LAYER_VOLATILE`, `_IN_PASS`, `_WATCH`, `_VERIFY`, `_RELOAD_RESET`, `_UNMAP_RESET`, `_READ_TRAPS`, `_WRITE_TRAPS`, `_WRITE_LOG`, `PS_RESIDENCY_MIX`, `_REPORT`, `PS_BIND_MEMO` | `buffer_cache_layer.cpp:74,374,482,531,548,1504,1559,1715,2101`; `buffer_cache.cpp:442,2652,3177,3379` | Layer behaviour switches and diagnostics | fixed | n/a | none | Not resource controls; leave. |
| `PS_VRAM_ACCESS_TRAP`, `PS_VRAM_TRAP_RANGE` | `buffer_cache.cpp:3064,3047` | Diagnostics on VRAM copies | off | n/a | none | Leave. |
| (constant) `ResidencyBlock` | `buffer_cache.cpp:1866` | 64 MiB residency allocation unit | fixed | no | too big on small budgets | `clamp(budget/32, 16, 64)` MiB. |
| (constant) `MaxChunks` | `psport_guest_memory.cpp:27` | 256 guest chunks | fixed | no | game with more chunks fails | Size from the direct-memory pool / chunk size. |
| `PS_DMEM_MB` | `runtime_memory.c:20`; set by `gpu/games/bloodborne/patches.cpp:339-341` | Guest direct memory (5056-16384 MiB) | 5056; 9152 when the Bloodborne heap patch is on (a game's profile) | no host check | OOM kill or swap on a machine or cgroup that cannot hold it | Before sizing, read `MemAvailable` and the cgroup `memory.max`/`memory.high`; refuse the heap patch (resolutions above 1080p) with a log line when the pool plus working set does not fit. Mapping is lazy, so this protects only against actual commit. |
| `PS_FAULT_WINDOW` | `buffer_cache.cpp:666` | Write-fault extension window | 256 KiB | no | fault storms | Leave (performance tuning, not a resource). |
| `PS_HOT_PAGES` | `region_manager.h:249` | Hot page tracking | off | no | experiment | Leave. |
| `PS_COPY_MERGE_KB` | `vk_shader_hle.cpp:228` | Copy merge distance | 64 KiB | no | tuning | Leave. |
| `PS_INDIRECT_GUARD_MAX` | `vk_indirect_guard.cpp:62` | Indirect dispatch cap | 4 M (could use device limit) | partly | none | Derive from `maxComputeWorkGroupCount` already read at line 58; lever only. |

### 2.3 Threads, stacks and CPU cores

| Variable | file:line | Controls | Default and origin | By capability? | Risk if wrong | Recommended rule |
|---|---|---|---|---|---|---|
| `PS_PREP_WORKERS` | `vk_draw_prep.cpp:168` | Draw-preparation worker threads | `clamp(Available()/2, 1, 8)`, SCHED_IDLE | yes (affinity-aware count) | idle-class, low risk | Count from affinity and cgroup `cpu.max` quota; lever only. |
| `PS_VK_RECORD_THREADS` | `vk_scheduler.cpp:129` | Recording threads | 3 at >= 12 threads, 2 at >= 8, else 1 | threshold on count only | normal priority threads take time from the game on few cores | Same CPU source; derive from measured recording time per frame vs frame budget (adaptive), lever only. |
| `PS_VK_RECORD_THREAD` | `vk_scheduler.cpp:149` | Recording off the calling thread | on | n/a | none | Lever. |
| `PS_VK_SEGMENT_KB` | `vk_scheduler.cpp:155` | Recording segment size | 64 KiB | no | tuning | Leave. |
| `PS_ASYNC_SUBMIT` | `vk_scheduler.cpp:181` | Submit from recording threads | on | n/a | none | Lever. |
| `PS_ASYNC_PIPELINE_THREADS`, `PS_ASYNC_PIPELINES` | `vk_pipeline_cache.cpp:669,749` | Pipeline compile threads | `clamp(hardware_concurrency()/4, 1, 4)` | **no: ignores affinity and cgroup** | oversubscription under `taskset`, containers, Steam Deck game mode | Use `PsThreads::Available()` here (same rule as the others); compile threads in the idle class. |
| `PS_COPY_THREADS` | `psport_copy.cpp:30` | Guest memory copy threads | `clamp(Available()/4, 1, 4)`, normal priority | yes (affinity-aware) | critical path | Lever only. |
| `PS_GPU_SPIN_US` | `liverpool.cpp:252` | Spin before sleeping on the GPU queue | 200 us at >= 12 threads, else 0 | count threshold | a spinning thread takes a core from a 4-core CPU | Spin only when the cgroup/affinity share leaves a free core, from measured idle time (`/proc/stat` or PSI cpu) rather than a fixed 12. |
| `PS_PIPE_SPIN_US` | `vk_draw_pipe.h:215` | Draw pipe spin | 200 at >= 12, else 50 | same | same | Same. |
| `PS_PRESENT_THREAD` | `videoout/driver.cpp:80` | Separate swap thread | on | n/a | none | Lever. |
| `PS_FRAMES_AHEAD` | `vk_presenter.cpp:570` | Frames the command thread runs ahead | 1 | no | latency vs bursts | Leave. |
| `PS_SUBMIT_AHEAD` | `gnmdriver.cpp:160`; decanter `crates/ps/src/learned.rs:18-41` | Submissions overlapping previous frame | game profile, else 0; decanter learns 0 after a failure | profile | game rewrites buffers | Already adaptive (`PsSubmitGuard`); leave. |
| `PS_READBACK_QUEUE` | `vk_instance.cpp:520` | Extra compute/transfer queue | on if the family exists | yes | none | Lever. |
| guest thread stacks | `runtime_thread.c:665-684` | Stack from `STACK_MARGIN` + guest request; mapped low or `pthread_attr_setstacksize` fallback | guest's own size | n/a | no cap on total thread count or stack bytes | Count threads and stack bytes; expose against RLIMIT_AS/cgroup limit in the exit summary. Low priority. |
| fiber stacks | `gpu/fiber/fiber.cpp:28,103` | Fiber stack guard | guest | n/a | none | Leave. |

### 2.4 Caches and render targets

| Variable / setting | file:line | Controls | Default and origin | By capability? | Risk | Recommended rule |
|---|---|---|---|---|---|---|
| `PS_PIPELINE_CACHE`, `PS_DRIVER_PIPELINE_CACHE` | `gpu/shim/core/emulator_settings.h:71,80` (`vk_driver_pipeline_cache.cpp`) | Shader/driver pipeline cache on disk | on, no size cap found in `vk_driver_pipeline_cache.*` | no | unbounded disk growth | Cap by free space on the cache volume (`statvfs`) and prune oldest; keep the switch as lever. |
| `PS_DIRECT_MEMORY_ACCESS`, `PS_COPY_GPU_BUFFERS` | `emulator_settings.h:72-73`; decanter `direct.rs:110` | Direct access and copy of GPU buffers | off, decanter sets `PS_COPY_GPU_BUFFERS=1` | no | launcher decides a psport internal | Fold into psport (on by default), remove from decanter's env. |
| `PS_ANISO`, setting `aniso` | `vk_rasterizer.cpp:4060`; `groups.rs:204` | Anisotropy | game profile, else off | no | GPU time and bandwidth on weak GPUs | Choose from `maxSamplerAnisotropy` and measured GPU time headroom; user setting stays an upper bound. |
| `PS_RENDER_RES`, `PS_OUTPUT_RES`, `PS_AUTO_RENDER_RES`, settings `output_res`, `live_resolution` | `patches.cpp:434`, `vk_temporal_upscaler.cpp:239-251`, `psport_overlay.cpp:667`; `groups.rs:133,167` | Render and output resolution, the largest VRAM consumer | user choice and presets | no | render targets plus a 4 GB heap patch overrun small VRAM | Offer "auto": pick the preset the VRAM budget and measured GPU time sustain (R5); keep manual as override. |
| `PS_UPSCALER`, `PS_FSR*`, `PS_DLSS`, `PS_FRAME_GEN*` | `psport_settings.cpp:317-339`, `vk_temporal_upscaler.cpp`, `vk_dlss.cpp:172` | Upscalers and frame generation | user choice | availability by capability | extra history buffers | Not a resource default; leave. |
| settings `present_mode`, `readbacks`, `draw_pipe`, `preupload`, `pc_model`, `as_0_3` | `groups.rs:188,551,529,569`; `names.rs:95` | Presentation, readback mode, draw pipe, legacy memory model | user | no | `preupload`/`pc_model`/`as_0_3` are legacy memory-model switches | Remove the legacy three where the port has a single memory model (already hidden by `single_memory_model`). |
| `PS_SKIP_CPU_CHECK` | `src/main/main.c:36` | Skips the CPU feature check | off | yes | none | Leave. |
| `PS_GPU_ID` | `emulator_settings.h:26`; `psport_launcher.py:384` | Selects the GPU | first suitable | yes | none | Leave; by `deviceType`, not by vendor. |

Variables seen in the grep that do not change resource use (tracing, dumps, input, game patches,
network, paths): about 230, among them every `PS_*_TRACE`, `PS_*_STATS`, `PS_DUMP_*`, `PS_FRAME_*`
diagnostics, `PS_PAD_*`, `PS_SHADNET_*`. They are out of scope. `PS_DUMP_SMALL_TEXTURES_*_MB`
(`psport_small_textures.cpp:190-191`) cap diagnostic disk use only.

### 2.5 Vendor checks (must stay HOST quirks)

Outside the quirk registry, vendor or driver ids appear in `vk_dlss.cpp:243` (`vendorID != 0x10de`,
DLSS availability: a vendor-gated feature, not a resource), `vk_instance.h:531` (KosmicKrisp
sampler count cap, a driver defect), `vk_instance.cpp:261-268` (version string formatting) and
`vk_driver_pipeline_cache.cpp:36-86` (cache header identity). None decides a resource default.
The single resource-relevant HOST quirk is `nvidia_slow_sparse_rebind` (driver id 4,
`runtime_quirks.inc:26`), allowed (driver defect with reference, disableable by `PS_QUIRKS`).
`vk_dlss.cpp:243` should become a feature probe (NGX availability) rather than a vendor id, but it
is not a resource issue. Comments naming NVIDIA elsewhere (`buffer_cache.cpp:422`,
`psport_guest_memory.cpp:672-678`) describe behaviour reached by capability, not by id.

## 3. Proposed automatic rules (design)

- **R1, marks from the live budget.** One function `ComputeMarks(budget, usage_outside_images)`
  returns trigger/pressure/critical as fractions of `GetDeviceMemoryBudgetNow()` (about 70/85/95 %,
  the integrated rule that already works), clamped so critical is never below pressure plus one
  residency block. Recompute once a second for every GPU with `VK_EXT_memory_budget`. Without the
  extension: derive from the heap size, no fixed 1.5/3 GB. This removes the overflow class of bug,
  #1245, the 8 GB `TARGET_GC_THRESHOLD` branch and `PS_GC_PRESSURE_PERCENT`.
- **R2, one pressure signal.** Collector, promotion pause and mirror floor read one struct
  (budget, usage, images, mirrors, headroom) so their thresholds cannot disagree (today three files
  carry separate percents and margins).
- **R3, host memory.** At startup and each second read `MemAvailable` and the cgroup
  `memory.max`/`memory.high` (Linux) or `GlobalMemoryStatusEx`/`GetProcessMemoryInfo` (Windows,
  Steam Deck game mode runs in a cgroup). Use PSI `memory` `some avg10` as the pressure trigger to
  shrink staging, residency spare blocks and idle ages. Decide `PS_DMEM_MB` growth against it.
- **R4, CPU.** One `PsThreads` source for every pool (affinity plus cgroup `cpu.max`), idle-class
  for speculative pools; spin only when PSI `cpu` or `/proc/stat` shows a free core.
- **R5, resolution auto.** Offer an "auto" preset that picks resolution from the budget and
  measured GPU time, since render targets dominate VRAM; manual stays an override.

## 4. Prioritised table (impact vs effort)

| # | Item | Impact | Effort | Test |
|---|---|---|---|---|
| 1 | R1: marks as fractions of the live budget for all GPUs, no absolute floors | High: fixes <= 3 GB budgets (the 28 % loss, collapse at <= 1024) and stale marks when other processes use VRAM | Medium | wbc3 sweep 4096/3072/2560/2048/1024 MiB, 5-min route, compare FPS and `Texture cache:` lines |
| 2 | R3: host memory and cgroup awareness (`MemAvailable`, `memory.max`, PSI) for `PS_DMEM_MB` and staging | High on Steam Deck game mode and containers; nothing exists today | Medium | cgroup-limited run (`systemd-run -p MemoryMax=`), Deck |
| 3 | Pipeline-compile threads use `PsThreads::Available()` | Medium, correctness under `taskset` and cgroups | Small | `taskset -c 0-3` run, log thread count |
| 4 | Move decanter's `PS_COPY_GPU_BUFFERS`/`PS_GPU_WRITE_TWINS(_MAX)` env defaults into psport | Medium (removes launcher-held resource decisions) | Small | night run without env |
| 5 | Promotion pause, mirror floor, margins as budget fractions (R2) | Medium-high on 4 GB cards | Medium | wbc3 4 GB limit, mirror count and bus-read stats |
| 6 | VMA block, dedicated size, residency block, staging keep scale with budget | Medium on small budgets | Small | 1024/2048 MiB sweep, allocation counts |
| 7 | Spin thresholds from free-core signal | Low-medium | Medium | Deck vs 12-thread desktop |
| 8 | Pipeline cache disk cap | Low-medium | Small | fill test on a small volume |
| 9 | Resolution auto preset (R5) | High for users, large | Large | per-GPU matrix |
| 10 | Remove legacy memory-model settings from decanter | Low | Small | UI check |

## 5. Proposed implementation tasks

Each task: its own commit, no new tests in the repo (CLAUDE.md), proven on wbc3 or the night run.

| Task | Files | Model |
|---|---|---|
| Texture marks as fractions of the live budget, recomputed each second, no fixed 1.5/3/8 GB | `gpu/shadps4/video_core/texture_cache/texture_cache.cpp`, `texture_cache.h`, `renderer_vulkan/vk_instance.cpp` | opus |
| One shared pressure struct read by collector, promotion pause and mirror floor; margins as fractions | `buffer_cache/buffer_cache.cpp`, `buffer_cache_layer.cpp`, `buffer_cache.h`, `texture_cache.cpp` | opus |
| Host memory probe (`MemAvailable`, cgroup limits, PSI; Windows equivalents) in a platform layer file, used by `PS_DMEM_MB` sizing and staging/residency spare | `src/platform/linux/host_memory.c` (new), `src/platform/windows/...`, `src/platform/ps_host.h`, `src/kernel/runtime_memory.c`, `gpu/games/bloodborne/patches.cpp` | opus |
| Idle ages continuous with usage/budget instead of 20/5/1 s and 60/3 s | `texture_cache.cpp:1728-1738`, `buffer_cache.cpp:1993`, `buffer_cache_layer.cpp:1126` | sonnet |
| VMA block, dedicated threshold, residency block and staging keep from budget and `maxMemoryAllocationCount` | `vk_instance.cpp:984`, `image.cpp:159`, `buffer_cache.cpp:1866`, `vk_staging_buffer_pool.cpp:43` | sonnet |
| `PsThreads::Available()` in pipeline compile threads; one CPU source with cgroup `cpu.max` | `vk_pipeline_cache.cpp:671`, `gpu/port/psport_threads.h`, `src/platform/linux/host_thread.c` | sonnet |
| Spin thresholds from a free-core signal | `liverpool.cpp:252`, `vk_draw_pipe.h:215` | opus |
| Fold `PS_COPY_GPU_BUFFERS` and `PS_GPU_WRITE_TWINS(_MAX)` defaults into psport; remove them from decanter's `run_sh_defaults` | `gpu/shim/core/emulator_settings.h`, `buffer_cache.cpp:301-309`, decanter `crates/ps/src/bbport/direct.rs` | sonnet |
| Pipeline cache disk cap from free space | `vk_driver_pipeline_cache.cpp`, `vk_pipeline_cache.cpp` | sonnet |
| Resolution auto preset from budget and GPU time | `gpu/port/psport_settings.cpp`, `vk_temporal_upscaler.cpp`, decanter `groups.rs` | opus |
| Remove legacy `preupload`, `pc_model`, `as_0_3` settings from decanter when the port has one memory model | `crates/ps/src/bbport/groups.rs`, `names.rs` | sonnet |
| Confirm the overflow fix at 1024 and 2048 MiB (run only, no code) | wbc3 | night harness |
