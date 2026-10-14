# Bloodborne: first native launch experiment

## Frame rate, audio, diagnostics (2026-09-25)

- **FPS**: `BB_FPS=uncap` (default) applies the "Uncap FPS++" patch
  (Lance McDonald, Kyo) from `patches/Bloodborne.xml`: flip interval 1 and
  simulation steps from the real frame delta time. Vblank then follows the
  monitor refresh rate (`BB_VBLANK_HZ=0`). `BB_FPS=60`/`90` — patches with a
  fixed step, `BB_FPS=30` — no patches. Additional patches from the XML:
  `BB_PATCHES="Skip Intro;Disable Motion Blur (perf increase)"`.
  `patches.py` compiles them into `out/patches.bin`; addresses in the XML are
  PS4 virtual addresses (eboot base 0x400000), the loader writes them at
  offset `address-0x400000` after relocations (`--patches`).
- **Audio**: `sceAudioOutOutput` now returns control strictly once per
  buffer period (like the hardware). Previously calls came in bursts once per
  PipeWire quantum (21 ms), FMOD's output thread overtook the mixer and played
  unwritten halves of 512-frame blocks — a click every 10.7 ms.
- **GPU thread performance** (`shadPS4:GpuCommandProcessor` is the
  bottleneck: ~97% of one core). Measurement in one scene: 26.5 → 47 FPS.
  - `runtime_memory.c`: region table under an rwlock instead of a recursive
    mutex (the GPU thread spent 18% of its time waiting on it), plus a
    per-thread region cache validated by the table's generation counter —
    the video core's per-draw queries go without locking.
  - `vk_scheduler.cpp`: `PopPendingOperations` did a timeline semaphore poll
    ioctl on every draw; now only when the queue is non-empty and no more
    often than once per 250 µs.
  - `fetch_shader.cpp`: fetch shader parsing is cached by code address with
    a check against a copy of the code.
  - `region_manager.h`: with no dirty pages in the range, protection is not
    recomputed.
  - Vulkan command recording in a separate thread `bb:VkRecorder`
    (`Scheduler::Record`, commands and their data in 128 KiB blocks; draw,
    dispatch, descriptors, barriers, copies, dynamic state, detiling).
    `CommandBuffer()` for untranslated code synchronizes and enables
    direct recording until the end of the draw. `BB_VK_RECORD_THREAD=0` — old behaviour.
  - Texture description cache (`ImageDesc`) by T# and resource flags,
    `FindImage` cache, lock-free dirty page check for read-only bindings,
    guest buffer data upload in the recording thread.
  - Measurement in one scene: all optimizations 64 FPS, without them 40 FPS (×1.6).
  - `BB_TOGGLE_FILE=<file>`: mask of disabled optimizations, changeable during
    gameplay (1 region cache, 2 fetch shader cache, 4 early exit of page
    tracking, 8 driver poll limit, 16 recording thread, 32 texture cache,
    64 lock-free page check, 128 FindImage cache, 256 buffer upload
    in the recording thread, 512 barrier tracker memo) — for hunting
    regressions without restarting.
- **Audio**: at startup for Bloodborne flag `0x204E` is set in
  `SPRJ0005/userdata0010` (rainvmaker's hack from the Diegolix29 fork): with the
  flag at zero some sounds, for example the player's weapon, do not play.
  `BB_SOUND_HACK=0` disables it.
  Profiling without perf: on SIGUSR2 the loader prints RIP, `rdi` (mutex
  address while waiting on a futex) and the thread's frame chain; the GPU library
  is built with `-fno-omit-frame-pointer`.
- **Diagnostics**: `BB_FRAME_STATS=1` — FPS, worst frame and shader/pipeline
  compile time every 5 s; `BB_AUDIO_STATS=1` — audio queue rhythm and
  fill level; `BB_AUDIO_DUMP=path` — raw PCM of each port.

## Current state (2026-09-25)

The original x86-64 code of `eboot.bin` runs directly on Linux and gets through
global constructors, memory initialization and the start of the game's own thread
system: 13 guest threads created (`FD4JobWorker:0`, `EzWorkPool_*` ×9,
`MOMainThread`, logger). The game allocates ~4.8 GiB of direct and ~337 MiB of
flexible memory. The current stop is `sceAjmInitialize` (ATRAC9 audio codec),
caller `0xd18a6a`.

Changes in this stage:

- `runtime_memory.c` rewritten: a single sparse memfd for the whole direct pool
  (5056 MiB, like `sceKernelGetDirectMemorySize` on PS4), a region table
  with splitting, flexible memory (448 MiB), `ReserveVirtualRange`, `Mprotect`,
  `QueryMemoryProtection`, `VirtualQuery`, `GetDirectMemoryType`, `BatchMap(2)`,
  partial unmap/release, `MAP_FIXED`/`NO_OVERWRITE`, GPU protection bits.
- **All guest-visible addresses are placed below 1 TiB**: the image, thread stacks,
  the main stack, trampolines (`runtime_low_map`), guest memory
  (`0x10_0000_0000..0xfc_0000_0000`), host object heaps (non-PIE + `mallopt`).
  The game packs pointers into 40 bits: for example, the lock at `0x20368c0`
  stores its owner as `scePthreadSelf() & 0xffffffffff` and compares it with the full
  value. With `0x7f…` handles this caused a permanent deadlock.
- The watchdog (`--timeout`) prints RIP and the frame chain of all threads
  (offsets relative to the image and `bb-probe` for `addr2line`).
  `BB_TRACE_SEMA=1` prints blocking semaphore waits.
  `got_import.py <GOT address>` names the import behind a PLT stub.
- Network in "cable unplugged" mode: HTTP templates/connections/requests are created,
  sending returns a network error; NpWebApi/Matching2/Signaling/Score —
  "not signed in to PSN"; voice chat — ports without sound.

Next subsystems in order of appearance: Ajm (ATRAC9 via ffmpeg),
Fios2, SaveData, Rtc, Pad, AudioOut, AvPlayer, VideoOut and GnmDriver.

## Previous description


**The original x86-64 code of `eboot.bin` is executed directly by the Linux
processor and gets through the initial part of the constructors.** It now returns from
`_init_env`, registers three `atexit` handlers and 279 C++ destructors,
passes 1416 initialization guards, creates 1422 mutexes, one rwlock and 17 semaphores,
allocates/maps 116 MiB for the game allocator.
The original `libc.prx` is loaded together with the eboot, its initializer returns 0,
the `std::ios_base::Init` constructor is passed. The next controlled stop is
`sceSysmoduleLoadModule(0xb4)` (`g8cM39EUZ6o#M#N`, caller `0x2026149`).
A separate Vulkan test executes a buffer fill command and checks the
returned 4096 bytes. shadPS4 is not run in this experiment.

**This is not yet a recompilation of the whole game nor a playable port.** There is no game
window, menu, rendering of game assets or working game loop. Vulkan is not yet
connected to the game's graphics commands. A controlled stop at the next
unsupported import is the measurable boundary of the prototype; the game launch is not complete.

## Running in the current environment

From `game_files`:

```bash
bash native_probe/run.sh --software
```

The script finds local Python, GCC and Vulkan SDK, prepares the image,
builds the loader and runs it. On this machine the tools were found
in `/nix/store`; nothing had to be downloaded or installed.
`--software` selects Lavapipe and disables implicit Vulkan layers for the test.
In the tested environment `/dev/dri` is absent, so a real GPU was not tested.

Expected result:

```text
Vulkan: llvmpipe (LLVM 21.1.8, 256 bits); command submission + 4096-byte readback PASS
Mapped 91973152 bytes, 4 segments; applied 237083 relocations
Starting native libc.prx at image offset 0x56e0000; fallback exports=202
Runtime: application heap API registered
Native libc module initializer returned 0
Entering original x86-64 code at guest offset 0xa0
Runtime: _init_env returned (verified libc implementation: RET)
Runtime: direct allocation offset=0x0, size=121634816 bytes
Runtime: mapped direct memory, size=121634816, alignment=0x200000
Runtime: semaphore id=1, attr=2, initial=0, max=1
Runtime: semaphore id=2, attr=2, initial=0, max=1
Runtime: semaphore id=3, attr=2, initial=0, max=1
Runtime: semaphore id=4, attr=2, initial=0, max=1
Runtime: semaphore id=5, attr=2, initial=0, max=1
Runtime: semaphore id=6, attr=2, initial=0, max=1
Runtime: semaphore id=7, attr=2, initial=0, max=1
Runtime: semaphore id=8, attr=2, initial=0, max=1
Runtime: semaphore id=9, attr=2, initial=0, max=1
Runtime: semaphore id=10, attr=2, initial=0, max=1
Runtime: semaphore id=11, attr=2, initial=0, max=1
Runtime: semaphore id=12, attr=2, initial=0, max=1
Runtime: semaphore id=13, attr=2, initial=0, max=1
Runtime: semaphore id=14, attr=2, initial=0, max=1
Runtime: semaphore id=15, attr=2, initial=0, max=1
Runtime: semaphore id=16, attr=2, initial=0, max=1
Runtime: semaphore id=17, attr=2, initial=0, max=1
STOP: first unsupported PS4 import: g8cM39EUZ6o#M#N (index 249)
API: sceSysmoduleLoadModule
Caller return offset: 0x2026149; first argument: 0xb4
Runtime: _init_env=1, atexit=3, __cxa_atexit=279, registered handlers=282
Runtime: static guards acquired=1416, released=1416
Runtime: mutexes created=1422, locks=3208, unlocks=3208
Runtime: rwlocks created=1, reads=21, writes=1, unlocks=22
Runtime: semaphores created=17, deleted=0, acquired=0, signals=0, timeouts=0
Runtime: direct memory allocations=1, maps=1, live=121634816, budget=536870912
Runtime: memory operations=9884
Original guest entry instructions executed; game initialization is incomplete.
```

**Exit code 20 means the expected stop at a missing PS4 API
implementation.** It deliberately differs from 0. Load errors return 1;
explicitly unsupported runtime modes — 21, a stack protector trigger — 22.
Linux signals — 128 + signal number. Execution of guest code on Linux has
a 10-second timer.

On a system with an available GPU you can try `bash native_probe/run.sh`.
This uses the normal Vulkan device selection; the hardware result has not
been verified here. To rerun already built files:

```bash
native_probe/out/bb-probe native_probe/out/boot-libc.bin --cpu-only
native_probe/out/bb-probe --vulkan-only
```

The base image without the native libc is kept in `out/boot.bin` for comparison
with the earlier stop at `std::ios_base::Init`.

From the `native_probe` directory the command stays the same: `bash run.sh --software`.
To reproduce the earliest stop at `_init_env`, use
`out/bb-probe out/boot.bin --cpu-only --strict-imports` from that directory.

The last command uses the normal Vulkan settings. For the software
driver set `VK_DRIVER_FILES` to its JSON and
`VK_LOADER_LAYERS_DISABLE='~implicit~'`, as `run.sh --software` does.

## What was found in the files

| Parameter | Result |
|---|---|
| Game | Bloodborne, CUSA03173 |
| Version from `param.sfo` | APP_VER 01.09, VERSION 01.00 |
| Container | SELF, 8 segment entries |
| CPU | x86-64, ELF64 little-endian, PS4/FreeBSD ABI |
| ELF type | ET_SCE_DYNEXEC (`0xfe10`) |
| Entry point | `0xa0` relative to the load base |
| Loadable segments | 2: code/read-only data and writable data/BSS |
| In-memory image | 91 042 036 bytes |
| Imports | 686 unique imported symbols |
| DT_NEEDED dependencies | 42 modules |
| Relocations | 231 478 RELATIVE, 2 838 ABS64, 23 GLOB_DAT, 661 JUMP_SLOT |
| `dvdroot_ps4` assets | 31 322 968 005 bytes |

Among the dependencies: libc, libkernel, GnmDriver, VideoOut, Pad, Fios2,
AudioOut, SaveData, Sysmodule, network and dialog APIs. `sce_module`
contains only seven files; the presence of module files does not replace support for their ABI.

The SELF segments used are available in plaintext. Extraction performs no
decryption. Unsupported compressed/encrypted segments are rejected.
One non-loadable metadata segment (`program header 9`, type `0x6fffff01`)
is not present in the available SELF segments. In the diagnostic `eboot.elf` its
range is filled with zeros: **this is not a byte-for-byte complete ELF reconstruction**.
All segments needed by this experiment have been checked to be present.

Details, the SHA-256 of the original eboot, the list of dependencies, NIDs, segments
and asset counts per directory are in `out/analysis.json`.
The names of several imports were verified by computing NIDs from guessed names;
the results are recorded in `import_name_hints`. For the first call, `_init_env`,
the NID `bzQExy189ZI` was obtained; the next call corresponds to `atexit`.

## How the prototype works

`prepare.py` reads the SELF and SFO, checks bounds, reconstructs the available
segments, parses the PS4 dynamic/symbol/RELA tables and produces `out/boot.bin`.
Source files are opened read-only. All results go to `out/`.

In addition, the actual `_init_env` export in `sce_module/libc.prx` is checked:
NID `bzQExy189ZI`, address `0x5fef0`, size 1, byte `c3` (RET).
The libc SHA-256 and the evidence are written to `analysis.json/libc_evidence`.
The initial assumption of complex initialization inside `_init_env`
was not confirmed for this dump. The return matches the function's real code.
The `BBPROBE2` format contains a flag for this check; without the flag the runtime is disabled.
Old `BBPROBE1` images are read with the runtime disabled.

`link_libc.py` creates a separate `out/boot-libc.bin` in `BBPROBE3` format:
it adds the two segments of the original libc, its relocations and 202
fallback export bindings. It matches NIDs together with library/module names
and versions: the local suffixes of libc `#C#A` and of eboot `#q#q` differ.
Implemented host contracts take priority. Other matched imports
point to real libc code/data, including C++ objects and tables.
The game's original files are not modified; binding information and the libc SHA-256
are written to `out/libc-link.json`.

Before entering the eboot, libc's `DT_INIT` is executed. This library's TLS is supported:
module 2, 544-byte template, 1184-byte block per host thread, zeroed remainder;
14 `DTPMOD64` relocations write the module number, not a pointer to the image.
`__tls_get_addr` checks the module and bounds. Full TLS for all PS4 modules,
its release when guest threads exit and direct access via FS
are not implemented. For the current single guest thread the block lives until exit.
`sceKernelGetProcParam` returns the eboot's original relocated procparam;
`_sceKernelRtldSetApplicationHeapAPI` stores the allocator callback table.

`runtime_thread.c` implements self and the init/get/getaffinity/destroy queries
of the current guest thread's attributes. For this prototype there is one
virtual CPU (mask 1); this is not a host CPU mask nor a setting of its affinity.
Creating guest threads and changing attributes remain unsupported.

`probe.c` allocates memory, copies segments and applies relocations.
Unknown functions are routed to executable interceptors that print the NID
and terminate the process. Unsupported imported data are routed to inaccessible
pages: using them causes a fault rather than returning fake data.
After loading, page permissions are set per the ELF, code gets RX permissions.
Then the original entry point is called directly per the SysV ABI.
The argument setup is sufficient for the observed section; a full PS4 kernel
startup stack, exceptions and a general mechanism for loading dependent modules
are not implemented; for now only libc is specifically linked in.

`runtime.c` implements the verified return from `_init_env`, the `atexit`/
`__cxa_atexit` registry with reverse order and DSO filtering, `__cxa_finalize`,
`exit`, guard acquire/release/abort, a random process-local stack canary,
a stack corruption failure handler, four memory functions and `strlen`.
Matching is done by full NID with the library/module suffix of exactly
this eboot, with no automatic "return 0" for unknown imports.
The termination registry and guards are designed for the current single guest thread;
recursive/concurrent guard initialization stops execution with code 21.

`runtime_mutex.c` maps PS4 mutexes/attributes to Linux pthread, converts
types and error codes, supports recursive and statically created mutexes.
Priority protocols, the PS4 scheduler and guest threads are not implemented.
Opaque handles are intended for the implemented APIs; the binary layout
of internal PS4 mutexes is not reproduced. The observed libc section works
through these APIs; direct access to mutex internals will require a separate implementation.

POSIX mutex imports from `libScePosix` use the same backend through
separate wrappers: they return positive PS4 errno values, not the negative
`scePthread` codes. The two-argument `pthread_mutex_init` does not read the missing
third argument. Linux errno is not passed to the guest directly.

`runtime_sema.c` implements kernel create/delete, wait/poll, signal/cancel.
Handles are unique 32-bit IDs; deleted IDs are not reused.
Verified: counters and overflow, microsecond timeout with a monotonic
clock, token reservation before wake-up, cancellation and deletion with waiting
host threads. The object is freed after all waiting calls have returned.
In FIFO mode eligible requests are served in queue order; a
smaller request may pass a waiting request for which there are no tokens yet.
This matches the shadPS4 reference model used; a comparison
on a real PS4 has not yet been done.
Attributes 0/2 allow creation and non-blocking operations, but a blocking
wait explicitly stops the prototype (21): the priority PS4 scheduler is still
missing. Non-zero option parameters are also not implemented. In the current
run the game created 17 semaphores with attr=2, initial=0, max=1, but has not yet
performed wait/signal; these operations are verified by separate C tests.

`runtime_time.c` supports POSIX `gettimeofday`: the time is taken from the host,
fields are copied into explicit PS4 timeval/timezone. Field bounds and the
microsecond range are verified. An unexpected host API error stops the prototype:
a full POSIX errno/TLS bridge is not yet implemented.

`runtime_rwlock.c` supports init/destroy, read/write, try-read/try-write,
timed-read/timed-write and unlock on top of Linux pthread. The registry synchronizes
initialization of static handles, tracks owners, readers and
waiting calls: destroying a held lock returns EBUSY,
a foreign unlock — EPERM, a timeout — Orbis ETIMEDOUT. Waiting does not hold
the registry mutex. Concurrent operation of several host threads is verified,
but guest thread creation is still missing. Only default attributes
are supported; non-standard attributes stop the prototype with code 21.

`runtime_memory.c` provides a **limited 512 MiB CPU pool for research**.
This is the prototype's chosen budget, not the PS4's RAM size. Allocation returns
a virtual physical offset; a Linux memfd provides real shared
storage for multiple mappings and 16 KiB/2 MiB alignment.
Allocate/map, full unmap and full release are implemented. Memory types
0/3 for the CPU, read/write permissions, flags=0, automatic address are supported;
any other mode stops with diagnostics. Partial release/unmap currently
return EINVAL. GPU coherency, fixed address reservation,
the full PS4 address space and connecting this pool to Vulkan are absent.

`vulkan_smoke.c` creates an instance/device/queue, submits `vkCmdFillBuffer`,
sets a barrier for CPU reads, waits for completion and compares all words of the buffer.
This is a check of the future graphics backend, not a GNM translator.

## Building on other systems

Linux x86-64: Python 3, GCC/Clang, Vulkan headers and loader. Usually enough:

```bash
python3 native_probe/prepare.py CUSA03173
python3 native_probe/link_libc.py CUSA03173
bash native_probe/build.sh
```

The script supports `CC`, `VULKAN_INCLUDE`, `VULKAN_LIB`;
if `pkg-config vulkan` is available it uses its settings.

For Windows the source has a `VirtualAlloc`/`VirtualProtect` branch and an explicit
SysV ABI. **MinGW-w64 GCC/Clang x86-64** is assumed, not MSVC.
This branch is **not built and not tested** in the current Linux environment.
Example of a manual build in a MinGW environment with the Vulkan SDK installed:

```text
python native_probe/prepare.py CUSA03173
gcc -std=c11 -O2 -Wall -Wextra native_probe/probe.c native_probe/runtime.c native_probe/runtime_mutex.c native_probe/runtime_thread.c native_probe/runtime_rwlock.c native_probe/runtime_sema.c native_probe/runtime_time.c native_probe/runtime_memory.c native_probe/vulkan_smoke.c -I<SDK>/Include -L<SDK>/Lib -lvulkan-1 -o native_probe/out/bb-probe.exe
native_probe/out/bb-probe.exe native_probe/out/boot.bin
```

SDK paths are replaced with the actual ones; the Linux binary is not portable to Windows.
Signal diagnostics and the timer in the current source are implemented only for Linux.
The new mutex/rwlock/semaphore/time/direct-memory backends are also implemented only for Linux;
Windows will reach an earlier unsupported import.

## Checks

```bash
bash native_probe/build.sh --test
python3 -m unittest discover -s native_probe -v
```

35 Python tests passed, including a run of the runtime C check suite:
SELF/image bounds, the `_init_env` instruction check, direct execution
of x86-64, relocations, strict mode, disabling an unconfirmed runtime,
failure diagnostics. The C checks cover reverse destructor order,
DSO and repeated finalization, registration during finalization and registry growth,
guard acquire/abort/release, mutex errors and recursion, alignment,
non-overlap of physical allocations, shared aliasing and re-allocation of
cleared memory. The stack protector and the recursive guard are checked in a subprocess.
Also verified: the rwlock life cycle, repeated read acquisitions,
foreign unlock, destroying while held, several concurrent readers,
a waiting writer, timeout conversion and `strlen` with a NUL inside the string.
Added checks of the native initializer/export, host API priority,
disabling libc in strict mode, execute permissions and new metadata bounds,
identity matching with different local IDs, rejection of a different library
version and TLS relocation handling. A C test checks separate TLS blocks
in host threads, the template/BSS, attribute queries and heap API registration.
A separate `sema-test` checks the 32-bit handle write without corrupting the neighbouring
field, stale IDs, overflow, timeout, a queue of several host threads,
token reservation, cancellation/deletion with waiters and the explicit refusal of a
priority blocking wait. Positive POSIX mutex errors, recursive acquisition
and the `gettimeofday` output structures are verified.
The Linux build was done with `-Wall -Wextra -Werror`.
The current integration run of the real eboot is recorded in `out/run.log`;
the result of the old mode — in `out/strict-run.log`.

## Next stages of a real port

1. Implement system module loading/registry and the required AppContent
   contracts. The game called `sceSysmoduleLoadModule(0xb4)` from `0x2026149`;
   per the reference table this ID means `libSceAppContent`. There is no such file
   among the seven local `sce_module/*.prx`. An implementation of the
   APIs used and of the module state will be needed; a plain success return was not added.
2. Continue the constructors, extend memory/synchronization, then implement
   TLS for the other modules, thread startup and loading of the required modules.
3. Reach `main`, add a file system (`/app0` → asset directory)
   and record each next unsupported contract.
4. Translate GNM/PM4 commands, resource formats and AMD GCN shaders to Vulkan/SPIR-V;
   then connect VideoOut to a window and swapchain. Having Vulkan by itself
   does not make PS4 graphics code compatible.
5. For static recompilation, reconstruct functions and control flow,
   virtual call tables, exceptions and the ABI; verify behaviour by comparison
   with the original machine code. On x86-64, direct execution with a compatible runtime
   already allows investigating the launch without first translating all CPU code.

Priority of the next change — Sysmodule/AppContent and the next constructors.

## Reference sources

The SELF format and segment mapping rules were checked against
[the shadPS4 loader headers](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.h)
and [segment loading](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.cpp).
PS4 dynamic tables and NID computation were checked against
[module.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/module.cpp),
the startup context — against [linker.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/linker.cpp).
Mutex signatures and type values were checked against
[mutex.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/mutex.cpp)
and [pthread.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/pthread.h),
rwlock — against [rwlock.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/rwlock.cpp),
direct memory signatures — against
[memory.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp).
Semaphores were checked against
[semaphore.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/semaphore.cpp),
time structures — against
[time.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/time.h).
The AppContent identifier — against
[sysmodule_table.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/sysmodule/sysmodule_table.h).
The new heap API registration and procparam contracts were checked against
[linker.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/linker.h)
and [process.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/process.cpp),
thread attributes — against
[pthread_attr.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/pthread_attr.cpp).
The body of `_init_env` and the structure of guards were verified by disassembling the local libc.
The prototype implementation was written separately; there is no dependency on an executable shadPS4.

The `out/` directory contains derived game data, has a `.gitignore` and is not needed
for distributing the tool's sources.
