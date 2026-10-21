// SPDX-License-Identifier: GPL-2.0-or-later
/* The host capability table (decision #540, docs/en/ARCHITECTURE.md), expanded by ps_host.h and
 * host_features.c, which define each macro before including this file:
 *
 *   FEATURE(id, area, sz_desc)
 *       a capability psport needs from the host; area is one of the platform layer's groups (vm,
 *       gpu_memory, fault, tls, thread, time, fs, proc, dl, rand, io, net), or of the options a
 *       launcher offers only where they run (upscaler, cpu); sz_desc a non-empty string literal.
 *   FEATURE_HOST(id, os, state, sz_reason, sz_ref)
 *       its state on one OS (LINUX, WIN32 or MACOS): IMPLEMENTED, EXPERIMENTAL or UNAVAILABLE, with
 *       how (or why not) and the decision, task or issue behind it, both non-empty string literals.
 *       An OS without a row has no implementation (UNAVAILABLE).
 *   FEATURE_PROBED(id)
 *       the state on this host is decided at run time too: the probing code reports it with
 *       ps_host_feature_probed, and the log, the exit summary and --list-features say what it
 *       found. */

FEATURE(guest_memory, vm, "the guest's address space at the PS4's addresses: fixed mappings below 1 TiB and direct memory "
                          "mapped at several addresses")
FEATURE_HOST(guest_memory, LINUX, IMPLEMENTED,
             "mmap with MAP_FIXED_NOREPLACE and MAP_NORESERVE, direct memory in a memfd punched with fallocate, malloc "
             "kept below 1 TiB in the -no-pie brk heap (src/platform/linux/host_memory.c; the guest's map in "
             "runtime_memory.c)",
             "#540")
FEATURE_HOST(guest_memory, WIN32, EXPERIMENTAL,
             "the arena reserved as VirtualAlloc2 placeholders (ps_host_vm_reserve_arena), fixed mappings as "
             "MapViewOfFile3 views of CreateFileMapping2 sections (CreateFileMappingW where Wine lacks it, logged) "
             "carved into it (UnmapViewOfFile2, placeholders split "
             "and coalesced, cut views mapped again with their protections; private memory a section of its own), "
             "SEC_RESERVE committed as views map it when SEC_COMMIT exceeds the commit limit, discards zeroed "
             "(src/platform/win32/host_memory.c); checked under Proton's Wine with a test program; main reserves "
             "[0x0800000000, 0xfc00000000) first (runtime_memory_reserve_arena), not run with a game yet; a thread "
             "touching the kept ends of a cut view while they are mapped again faults, file views are refused "
             "(ENODEV), malloc stays low as psport.exe is linked --disable-high-entropy-va",
             "#614")

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
FEATURE_HOST(gpu_memory_host_import, WIN32, EXPERIMENTAL,
             "the only path: the GPU imports the backing MapViewOfFile3 view of the direct memory section, probed at "
             "start; under Proton on RADV the import of section views fails (ErrorInvalidExternalHandle; private "
             "VirtualAlloc memory imports, as amdgpu's userptr pins anonymous memory only, and Win32 handles neither "
             "import a section nor export memory MapViewOfFile takes), so the port stops there with the reason; not "
             "run with a driver that imports views (NVIDIA, native Windows)",
             "#870")
FEATURE_PROBED(gpu_memory_host_import)

FEATURE(gpu_memory_share, gpu_memory,
        "the host's half of sharing guest memory with the GPU: an allocation the GPU exported, mapped at the "
        "game's addresses, or a range of the host's view of guest memory the GPU imports in place")
FEATURE_HOST(gpu_memory_share, LINUX, IMPLEMENTED,
             "dma-buf descriptors adopted, mapped with mmap and closed (src/platform/linux/host_gpu_memory.c), memfd "
             "view ranges at page granularity, failures counted per kind (src/platform/common/host_gpu_memory.c); "
             "whether a driver's dma-buf or import works is the two probed rows above",
             "#1009, #1017")
FEATURE_HOST(gpu_memory_share, WIN32, EXPERIMENTAL,
             "ranges of the direct memory section's views at page granularity (src/platform/common/host_gpu_memory.c), "
             "the import path; no exported allocation is taken (EOPNOTSUPP, counted) until a Win32 handle export is "
             "measured on Windows drivers; compile-checked, not run with a game yet",
             "#1009, #1017")

FEATURE(write_traps, fault, "faults on guest memory: the write traps that track what the GPU reads, fault recovery and "
                            "fault reports")
FEATURE_HOST(write_traps, LINUX, IMPLEMENTED,
             "SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP handlers reading the ucontext, siglongjmp recovery and "
             "process_vm_readv (src/platform/linux/host_fault.c) over mprotect",
             "#540")
FEATURE_HOST(write_traps, WIN32, EXPERIMENTAL,
             "a first vectored exception handler mapping access violations, in-page errors, illegal and privileged "
             "instructions, divide and floating-point exceptions, int3 (rip moved past it) and single steps to the "
             "handlers with the CONTEXT's registers; recovery by resuming the thread in a hand-written longjmp outside "
             "the dispatch; NtRead/NtWriteVirtualMemory page by page (src/platform/win32/host_fault.c); checked under "
             "Proton with a test program (recovery, int3, copies, backtrace), not run with a game yet. Windows writes "
             "the exception frame into the 128-byte System V red zone below the guest's rsp before any handler runs "
             "(Supermedo bc893e2): a write trap in a guest leaf function can corrupt its locals, which the handler "
             "cannot prevent; first-chance exceptions a host library would catch itself (SEH) reach psport's handlers "
             "too",
             "#614")

FEATURE(guest_tls, tls, "the guest's thread-local storage: a TCB and static TLS per thread, the thread pointer on GS")
FEATURE_HOST(guest_tls, LINUX, IMPLEMENTED,
             "arch_prctl(ARCH_SET_GS, ARCH_GET_GS) (src/platform/linux/host_thread.c), the TCB in runtime_thread.c",
             "#540")
FEATURE_HOST(guest_tls, WIN32, EXPERIMENTAL,
             "a TlsAlloc slot of the TEB's 64 inline ones taken first thing in main, the loader writing 0x1480 + 8 * slot "
             "into the guest's mov r64, gs:[0] (src/platform/win32/host_thread.c, ps_host_tls_tcb_offset), "
             "compile-checked, not run yet",
             "#614")

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
             "interrupts by suspending the thread and redirecting it below the red zone to a stub that saves its "
             "registers and AVX state, runs the handler and resumes through iretq, held and pending per thread in "
             "place of a signal mask, a thread waiting on an address woken to take it, other blocking calls taking it "
             "when they return; the watchdog a waitable timer on a thread of its own (src/platform/win32/host_fault.c), "
             "checked under Proton with a test program, not run with a game yet. Guest threads are winpthreads threads "
             "on stacks Windows allocates, since it cannot run a thread on memory the caller gives: the stack a game "
             "sets, or the runtime's low one, is only reported (scePthreadAttrGetstack); a new thread starts with no "
             "interrupt held (Windows passes no mask on) and takes the guest exception once its TCB is in place",
             "#614")

FEATURE(time, time, "clocks, sleeping and the time stamp counter")
FEATURE_HOST(time, LINUX, IMPLEMENTED, "clock_gettime, nanosleep, localtime_r's zone at each instant (src/platform/linux/host_time.c), rdtsc", "#540")
FEATURE_HOST(time, WIN32, EXPERIMENTAL,
             "QueryPerformanceCounter, GetSystemTimePreciseAsFileTime, CPU times at the scheduler tick from "
             "GetThreadTimes and GetProcessTimes, the zone at each instant from GetTimeZoneInformationForYear and SystemTimeToTzSpecificLocalTimeEx, sleeps on a per-thread CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer "
             "(Sleep in milliseconds without one) (src/platform/win32/host_time.c), compile-checked, not run yet",
             "#614")

FEATURE(files, fs, "the guest's file system on host folders: /app0, user data and savedata")
FEATURE_HOST(files, LINUX, IMPLEMENTED,
             "POSIX files and folders (src/platform/linux/host_file.c), the guest's mounts in runtime_file.c", "#540")
FEATURE_HOST(files, WIN32, EXPERIMENTAL,
             "CreateFileW and the other W calls with UTF-8 paths, '/' taken as '\\', \\\\?\\ past MAX_PATH; psport's int "
             "descriptors as slots over Win32 handles (0-2 copies of the standard handles, followed by the C runtime "
             "when duplicated onto), pread and pwrite on a second ReOpenFile handle so that the shared file pointer stays, "
             "O_APPEND as Windows' end-of-file offset, folders with FILE_FLAG_BACKUP_SEMANTICS and FindFirstFileExW, "
             "symbolic links and junctions as links; only the owner's write bit maps (the read-only attribute) and "
             "O_NONBLOCK is kept without effect (src/platform/win32/host_file.c), compile-checked, not run yet; "
             "fopen and nftw (savedata, trophies, content, mod views) go through the C runtime's ANSI code page, so a "
             "path outside it fails there",
             "#614")

FEATURE(processes, proc, "the process starting again on itself (restart, sceSystemServiceLoadExec) and the mod view's "
                         "cleaner process")
FEATURE_HOST(processes, LINUX, IMPLEMENTED,
             "execv of /proc/self/exe, execvp and close_range (src/platform/linux/host_process.c), fork for the mod view's cleaner "
             "(ps_host_remove_tree_at_exit)", "#540")
FEATURE_HOST(processes, WIN32, EXPERIMENTAL,
             "exec as CreateProcessW with the command line quoted for CommandLineToArgvW, SearchPathW for names "
             "without a folder, only descriptors 0-2 inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST); the caller waits "
             "for the new process, keeping its Vulkan device meanwhile, and exits with its code "
             "(src/platform/win32/host_process.c), compile-checked, not run yet; no output helper for debug mode "
             "(ps_host_output_tee needs fork), no exit hook (atexit gets no status), no mod view cleaner "
             "(ps_host_remove_tree_at_exit: the view stays and the log says so); the settings menu's restart runs "
             "bash run.sh, which a Windows machine lacks, and ends the process with the error",
             "#614")

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

FEATURE(audio_out, io, "libSceAudioOut's ports as streams to the default playback device")
FEATURE_HOST(audio_out, LINUX, IMPLEMENTED,
             "SDL3, a shared backend for Linux and Windows (src/platform/common/host_audio.c; libSceAudioOut in "
             "runtime_audio.c); PS_AUDIO=none runs without a device",
             "#540, #1009")
FEATURE_HOST(audio_out, WIN32, EXPERIMENTAL,
             "SDL3, the backend shared with Linux (src/platform/common/host_audio.c), not run with a game yet",
             "#1009")

FEATURE(input_pad, io, "gamepads as libScePad's pad: buttons, sticks, triggers, touchpad, rumble and light bar")
FEATURE_HOST(input_pad, LINUX, IMPLEMENTED,
             "SDL3, a shared backend for Linux and Windows (src/platform/common/host_input.c; libScePad in "
             "runtime_pad.c)",
             "#1009")
FEATURE_HOST(input_pad, WIN32, EXPERIMENTAL,
             "SDL3, the backend shared with Linux (src/platform/common/host_input.c), not run with a game yet",
             "#1009")

FEATURE(input_pad_motion, io, "the gamepad's motion sensors (gyroscope and accelerometer) as the DualShock 4's "
                              "orientation, acceleration and angular velocity")
FEATURE_HOST(input_pad_motion, LINUX, IMPLEMENTED,
             "SDL3 gamepad sensors, a shared backend for Linux and Windows (src/platform/common/host_input.c; "
             "runtime_pad_sensors.c); pads without them get the motion emulation or rest",
             "#550, #1009")
FEATURE_HOST(input_pad_motion, WIN32, EXPERIMENTAL,
             "SDL3 gamepad sensors, the backend shared with Linux (src/platform/common/host_input.c), not run with "
             "a game yet",
             "#1009")

FEATURE(input_keyboard, io, "the keyboard: its state for the pad's bindings, its keys and modifiers for libSceImeKeyboard")
FEATURE_HOST(input_keyboard, LINUX, IMPLEMENTED,
             "SDL3, a shared backend for Linux and Windows (src/platform/common/host_input.c; runtime_pad.c, "
             "runtime_keyboard.c)",
             "#1009")
FEATURE_HOST(input_keyboard, WIN32, EXPERIMENTAL,
             "SDL3, the backend shared with Linux (src/platform/common/host_input.c), not run with a game yet",
             "#1009")

FEATURE(input_mouse, io, "the mouse's buttons for the pad's bindings and libSceMouse")
FEATURE_HOST(input_mouse, LINUX, IMPLEMENTED,
             "SDL3, a shared backend for Linux and Windows (src/platform/common/host_input.c; runtime_pad.c, "
             "runtime_mouse.c)",
             "#1009")
FEATURE_HOST(input_mouse, WIN32, EXPERIMENTAL,
             "SDL3, the backend shared with Linux (src/platform/common/host_input.c), not run with a game yet",
             "#1009")

FEATURE(port_menu_binding, io, "psport.ini's key.menu and pad.menu: a key, gamepad button or two-button chord that opens "
                               "the port's menu besides Insert and L3+R3")
FEATURE_HOST(port_menu_binding, LINUX, IMPLEMENTED,
             "SDL3 keyboard and gamepad state on the window thread's events (runtime_pad.c runtime_pad_menu_pressed, "
             "gpu/port/psport_overlay.cpp)",
             "#557")
FEATURE_HOST(port_menu_binding, WIN32, EXPERIMENTAL, "SDL3 as on Linux (runtime_pad.c runtime_pad_menu_pressed), not run yet",
             "#614")

FEATURE(key_stick, io, "psport.ini's key_stick_opposite (last or neutral) and key_stick_hold_ms (0 to 200): of two "
                       "opposite move or look keys the last pressed wins, and the move stick keeps its direction that "
                       "long after its keys are let go")
FEATURE_HOST(key_stick, LINUX, IMPLEMENTED,
             "SDL3 keyboard state at pad reads (runtime_pad.c key_axis, key_stick, load_bindings)",
             "#785")
FEATURE_HOST(key_stick, WIN32, EXPERIMENTAL, "SDL3 as on Linux (runtime_pad.c key_stick), not run yet", "#614")

FEATURE(movie_skip, io, "psport.ini's skip_movies (PS_SKIP_MOVIES): key.skip_movie or pad.skip_movie stops the movies "
                        "playing and gives the title's profile its own skip")
FEATURE_HOST(movie_skip, LINUX, IMPLEMENTED,
             "SDL3 keyboard and gamepad state at pad reads (runtime_pad.c skip_update); AvPlayer stopped on its "
             "controller thread (avplayer_state.cpp RequestSkipAll)",
             "#670")
FEATURE_HOST(movie_skip, WIN32, EXPERIMENTAL, "SDL3 and AvPlayer as on Linux (runtime_pad.c skip_update), not run yet",
             "#614")

FEATURE(window, io, "the game's window and its Vulkan surface")
FEATURE_HOST(window, LINUX, IMPLEMENTED,
             "SDL3, a shared backend for Linux and Windows (gpu/platform/common/host_window.cpp; gpu/shim/window.cpp), "
             "X11 and Wayland surfaces through VK_KHR_xlib_surface and VK_KHR_wayland_surface "
             "(gpu/platform/linux/host_window_surface.cpp)",
             "#1009")
FEATURE_HOST(window, WIN32, EXPERIMENTAL,
             "SDL3, the backend shared with Linux (gpu/platform/common/host_window.cpp), the Win32 surface through "
             "VK_KHR_win32_surface (gpu/platform/win32/host_window_surface.cpp), not run with a game yet",
             "#1009")

FEATURE(sockets, net, "the guest's sockets (libSceNet and the POSIX socket calls) on host sockets, loopback when offline")
FEATURE_HOST(sockets, LINUX, IMPLEMENTED,
             "BSD sockets (runtime_socket.c); socket, accept4, connect, sendmsg, recvmsg and ppoll, close, errors, "
             "non-blocking mode, queue sizes and TCP state in src/platform/linux/host_net.c",
             "#540")
FEATURE_HOST(sockets, WIN32, EXPERIMENTAL,
             "Winsock 2 (src/platform/win32/host_net.c): WSAStartup once, WSA errors as errno, WSASocketW without "
             "inheritance, connect's WSAEWOULDBLOCK as EINPROGRESS, WSASendTo and WSARecvFrom over WSABUF arrays for "
             "send, sendto, sendmsg and the receives (no control data; a truncated datagram as MSG_TRUNC), MSG_DONTWAIT "
             "as a zero-timeout WSAPoll first, select over WSAPoll (sockets only), FIONBIO, FIONREAD and SIO_TCP_INFO "
             "for queues and TCP state; SO_REUSEPORT accepted without effect; compile-checked, not run yet",
             "#614")

FEATURE(online_module, net, "online play through libpsport-net.so, loaded on the first network import with PS_ONLINE=1")
FEATURE_HOST(online_module, LINUX, IMPLEMENTED,
             "dlopen of gpu/libpsport-net.so when it was built (runtime_net.c); probed by --list-features, "
             "unavailable in a build without it (the public binary cache and bundle, until #850)",
             "#540")
FEATURE_PROBED(online_module)

FEATURE(upscaler_fsr4, upscaler, "FSR 4 (the v07 INT8 shaders) on the GPU the game takes")
FEATURE_HOST(upscaler_fsr4, LINUX, IMPLEMENTED,
             "vk_fsr4.cpp when the device has Float16, Int8, Int16, integer dot products and linear compute "
             "derivatives (Vulkan::Instance::IsFsr4Int8Supported); probed by --list-features "
             "(gpu/port/psport_host_features.cpp)",
             "#757")
FEATURE_PROBED(upscaler_fsr4)

FEATURE(upscaler_fsr411, upscaler, "FSR 4.1.1 on the GPU the game takes; its assets are a separate build")
FEATURE_HOST(upscaler_fsr411, LINUX, IMPLEMENTED,
             "fsr411/ when the device also has VK_VALVE_shader_mixed_float_dot_product "
             "(Vulkan::Instance::IsFsr411Supported), FP8 cooperative matrices where it has them; probed by "
             "--list-features (gpu/port/psport_host_features.cpp)",
             "#757")
FEATURE_PROBED(upscaler_fsr411)

FEATURE(upscaler_dlss, upscaler, "DLSS Super Resolution through libpsport_dlss.so and NVIDIA's libnvidia-ngx-dlss.so")
FEATURE_HOST(upscaler_dlss, LINUX, IMPLEMENTED,
             "vk_dlss.cpp when both libraries load, the device takes NGX's extensions and NGX starts on it "
             "(Vulkan::Dlss::Available); probed by --list-features (gpu/port/psport_host_features.cpp)",
             "#757")
FEATURE_PROBED(upscaler_dlss)

FEATURE(frame_gen_fsr, upscaler, "FSR 3.1 frame generation (FSR-Vulkan 3.1.6 interpolation and optical flow) on the GPU "
                                 "the game takes")
FEATURE_HOST(frame_gen_fsr, LINUX, IMPLEMENTED,
             "vk_frame_generation.cpp when FSR-Vulkan reports fsr3FrameGenerationPrerequisites on the device and it was "
             "created with shaderStorageImageWriteWithoutFormat (FrameGenerator::IsFsrSupported); probed by "
             "--list-features (gpu/port/psport_host_features.cpp)",
             "#597")
FEATURE_PROBED(frame_gen_fsr)

FEATURE(intel_tonemap_fix, cpu, "Bloodborne's Intel Black Tonemap Fix, for Intel CPUs; the cause is open (#770)")
FEATURE_HOST(intel_tonemap_fix, LINUX, IMPLEMENTED,
             "the patch on by default on GenuineIntel CPUs of /proc/cpuinfo, PS_INTEL_TONEMAP_FIX=0 or 1 forces it "
             "(gpu/games/bloodborne/patches.cpp), behind the WORKAROUND quirk bloodborne_intel_tonemap (#770); "
             "probed by --list-features",
             "#757")
FEATURE_PROBED(intel_tonemap_fix)
