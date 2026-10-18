/* The host capability table (decision #540, docs/en/ARCHITECTURE.md), expanded by ps_host.h and
 * host_features.c, which define each macro before including this file:
 *
 *   FEATURE(id, area, sz_desc)
 *       a capability psport needs from the host; area is one of the platform layer's groups (vm,
 *       gpu_memory, fault, tls, thread, time, fs, proc, dl, rand, io, net), sz_desc a non-empty
 *       string literal.
 *   FEATURE_HOST(id, os, state, sz_reason, sz_ref)
 *       its state on one OS (LINUX, WIN32 or MACOS): IMPLEMENTED, EXPERIMENTAL or UNAVAILABLE, with
 *       how (or why not) and the decision, task or issue behind it, both non-empty string literals.
 *       An OS without a row has no implementation (UNAVAILABLE).
 *   FEATURE_PROBED(id)
 *       the state on this host is decided at run time too: the probing code reports it with
 *       ps_host_feature_probed, and the log and the exit summary say what it found. */

FEATURE(guest_memory, vm, "the guest's address space at the PS4's addresses: fixed mappings below 1 TiB and direct memory "
                          "mapped at several addresses")
FEATURE_HOST(guest_memory, LINUX, IMPLEMENTED,
             "mmap with MAP_FIXED_NOREPLACE and MAP_NORESERVE, direct memory in a memfd punched with fallocate, malloc "
             "kept below 1 TiB in the -no-pie brk heap (src/platform/linux/host_memory.c; the guest's map in "
             "runtime_memory.c)",
             "#540")

FEATURE(gpu_memory_dmabuf, gpu_memory, "the GPU uses guest memory in place: cached system memory exported as a dma-buf and "
                                       "mapped at the game's addresses")
FEATURE_HOST(gpu_memory_dmabuf, LINUX, IMPLEMENTED,
             "VK_EXT_external_memory_dma_buf; whether the driver's dma-buf maps at page offsets at cached speed is probed "
             "when the Vulkan device starts (gpu/memory/psport_guest_memory.cpp)",
             "#540")
FEATURE_HOST(gpu_memory_dmabuf, WIN32, UNAVAILABLE, "dma-buf is a Linux kernel interface: host memory import is the path",
             "#540")
FEATURE_HOST(gpu_memory_dmabuf, MACOS, UNAVAILABLE, "dma-buf is a Linux kernel interface: host memory import is the path",
             "#540")
FEATURE_PROBED(gpu_memory_dmabuf)

FEATURE(gpu_memory_host_import, gpu_memory,
        "the GPU imports the guest's host memory (VK_EXT_external_memory_host) where dma-buf chunks do not work")
FEATURE_HOST(gpu_memory_host_import, LINUX, IMPLEMENTED,
             "probed when dma-buf maps at offset 0 only or not at all, or with PS_GUEST_MEMORY=host; not preferred for "
             "being reported, since RADV passes the probe and stalls (gpu/memory/psport_guest_memory.cpp)",
             "#540")
FEATURE_PROBED(gpu_memory_host_import)

FEATURE(write_traps, fault, "faults on guest memory: the write traps that track what the GPU reads, fault recovery and "
                            "fault reports")
FEATURE_HOST(write_traps, LINUX, IMPLEMENTED,
             "SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP handlers reading the ucontext, siglongjmp recovery and "
             "process_vm_readv (src/platform/linux/host_fault.c) over mprotect",
             "#540")

FEATURE(guest_tls, tls, "the guest's thread-local storage: a TCB and static TLS per thread, the thread pointer on GS")
FEATURE_HOST(guest_tls, LINUX, IMPLEMENTED,
             "arch_prctl(ARCH_SET_GS, ARCH_GET_GS) (src/platform/linux/host_thread.c), the TCB in runtime_thread.c",
             "#540")

FEATURE(threads, thread, "guest threads on host threads: creation, signals to one thread, waits on an address")
FEATURE_HOST(threads, LINUX, IMPLEMENTED,
             "pthreads (runtime_thread.c); gettid, futex, thread names and stacks, sched_getaffinity and SCHED_IDLE "
             "(src/platform/linux/host_thread.c); interrupts as tgkill of SIGRTMIN+3, SIGUSR2 and SIGPROF, the "
             "watchdog as alarm (src/platform/linux/host_fault.c)",
             "#540")
FEATURE_HOST(threads, WIN32, EXPERIMENTAL,
             "WaitOnAddress and WakeByAddress (within the process; the wake count is the one asked for), "
             "SetThreadDescription, Toolhelp thread lists, GetCurrentThreadStackLimits, the process affinity mask and "
             "THREAD_PRIORITY_IDLE (src/platform/win32/host_thread.c), compile-checked with mingw-w64, not run yet; "
             "no interrupts (guest exception, thread dump, watchdog, samples)",
             "#614")

FEATURE(time, time, "clocks, sleeping and the time stamp counter")
FEATURE_HOST(time, LINUX, IMPLEMENTED, "clock_gettime and nanosleep (src/platform/linux/host_time.c), rdtsc", "#540")
FEATURE_HOST(time, WIN32, EXPERIMENTAL,
             "QueryPerformanceCounter, GetSystemTimePreciseAsFileTime, CPU times at the scheduler tick from "
             "GetThreadTimes and GetProcessTimes, sleeps on a per-thread CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer "
             "(Sleep in milliseconds without one) (src/platform/win32/host_time.c), compile-checked, not run yet",
             "#614")

FEATURE(files, fs, "the guest's file system on host folders: /app0, user data and savedata")
FEATURE_HOST(files, LINUX, IMPLEMENTED,
             "POSIX files and folders (src/platform/linux/host_file.c), the guest's mounts in runtime_file.c", "#540")

FEATURE(processes, proc, "the process starting again on itself (restart, sceSystemServiceLoadExec) and the mod view's "
                         "cleaner process")
FEATURE_HOST(processes, LINUX, IMPLEMENTED,
             "execv of /proc/self/exe, execvp and close_range (src/platform/linux/host_process.c), fork (loader_mods.c)", "#540")

FEATURE(dynamic_libraries, dl, "host libraries loaded at run time (the online module) and the symbols fault reports name")
FEATURE_HOST(dynamic_libraries, LINUX, IMPLEMENTED, "dlopen, dlsym and dladdr (src/platform/linux/host_library.c)", "#540")
FEATURE_HOST(dynamic_libraries, WIN32, EXPERIMENTAL,
             "LoadLibraryExW with UTF-8 paths, GetProcAddress, GetModuleHandleExW and the PE export directory for the "
             "nearest symbol (src/platform/win32/host_library.c), compile-checked, not run yet",
             "#614")

FEATURE(randomness, rand, "random bytes for the stack canary, sceKernel random numbers, UUIDs and kern.arandom")
FEATURE_HOST(randomness, LINUX, IMPLEMENTED, "getrandom (src/platform/linux/host_system.c)", "#540")
FEATURE_HOST(randomness, WIN32, EXPERIMENTAL,
             "BCryptGenRandom with the system's preferred generator (src/platform/win32/host_system.c), "
             "compile-checked, not run yet",
             "#614")

FEATURE(audio, io, "audio output")
FEATURE_HOST(audio, LINUX, IMPLEMENTED, "SDL3 (runtime_audio.c); PS_AUDIO=none runs without a device", "#540")

FEATURE(input, io, "gamepads, keyboard and mouse")
FEATURE_HOST(input, LINUX, IMPLEMENTED, "SDL3 (runtime_pad.c, runtime_keyboard.c, runtime_mouse.c)", "#540")

FEATURE(input_sensors, io, "the gamepad's motion sensors (gyroscope and accelerometer) as the DualShock 4's orientation, "
                           "acceleration and angular velocity")
FEATURE_HOST(input_sensors, LINUX, IMPLEMENTED,
             "SDL3 gamepad sensors (runtime_pad_sensors.c); pads without them get the motion emulation or rest",
             "#550")

FEATURE(port_menu_binding, io, "psport.ini's key.menu and pad.menu: a key, gamepad button or two-button chord that opens "
                               "the port's menu besides Insert and L3+R3")
FEATURE_HOST(port_menu_binding, LINUX, IMPLEMENTED,
             "SDL3 keyboard and gamepad state on the window thread's events (runtime_pad.c runtime_pad_menu_pressed, "
             "gpu/port/psport_overlay.cpp)",
             "#557")

FEATURE(window, io, "the game's window and its Vulkan surface")
FEATURE_HOST(window, LINUX, IMPLEMENTED, "SDL3 (gpu/shim/window.cpp)", "#540")

FEATURE(sockets, net, "the guest's sockets (libSceNet and the POSIX socket calls) on host sockets, loopback when offline")
FEATURE_HOST(sockets, LINUX, IMPLEMENTED,
             "BSD sockets (runtime_socket.c); close, errors, non-blocking mode, queue sizes and TCP state in "
             "src/platform/linux/host_net.c",
             "#540")

FEATURE(online_module, net, "online play through libpsport-net.so, loaded on the first network import with PS_ONLINE=1")
FEATURE_HOST(online_module, LINUX, IMPLEMENTED, "dlopen of gpu/libpsport-net.so when it was built (runtime_net.c)", "#540")
