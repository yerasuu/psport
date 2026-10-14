# Changes after 0.5 (since 2026-10-10)

Measurements — RX 7800 XT / RADV, user's save (Cathedral Ward), 1080p.

## All retail editions get the game profile (#119)

- The Bloodborne profile (`gpu/games/bloodborne.cpp`) was selected only for CUSA03173. The US
  edition (CUSA00900) and the other regions run the same 1.09 executable, but ran without a profile,
  and the new model crashed on entering the world: `Guest fault ... at guest offset 0x2085xxx,
  address (nil)` in the game's allocator (#119, RX 7600, US edition).
- Reproduced locally on CUSA03173 with `BB_GAME_PROFILE=none`: the same crash (`0x208596a`) on
  entering the world. With the profile but without hooks (`BB_GUEST_HOOKS=0`) — no crash: what saves
  it is `DynamicWriters` — allocator code whose bookkeeping data lives in GPU memory; without this
  mark their blocks go to the VRAM copy.
- The profile, the game check (`scripts/game_check.py`), patches (`scripts/patches.py`), the save
  "sound fix" and the mod folders now know all editions: CUSA00900, CUSA00207, CUSA00208,
  CUSA00299, CUSA01363 and the editions with The Old Hunters CUSA03179, CUSA03173, CUSA03014,
  CUSA03023.
- Game check: other regions of the base game without the update ask for 1.09 instead of "only
  CUSA03173 is supported"; an eboot.bin with a built-in 60 FPS patch (crashes in the gesture menu) —
  a separate message (PR #117).
- Remaining: the generic path without a profile should not crash on its own (allocator bookkeeping
  data in the VRAM copy). For now it is needed only for other games.

## Full-frame motion blur in the new model (#122)

- With motion blur enabled (default) the frame smeared entirely on camera turns, in the old model it
  did not (#122, RX 9060 XT). Reproduced: frame sharpness during a turn (standard deviation of the
  Laplacian) 6.9 versus 8.5–12 in the old model.
- Search: `BB_GARLIC_VRAM_RANGE=start,end` (new diagnostic key: VRAM copies only in this range) and
  bisecting the range — the 64 KiB block `0x104ff00000`. It is read only by the GPU (an 11.4 MiB
  buffer every frame), there are no writes by buffers or by the CPU: this is render target memory
  that the game then reads as a buffer.
- Cause: an image download to game memory went through staging and a deferred CPU write when that
  memory had a VRAM copy; the copy was updated only after that write (by invalidation) — the buffer
  read data from past frames. Now the GPU also copies the image into the arena (the VRAM copy) in
  stream order; into game memory — as before. And `IsRegionGpuModified` treats as modified what the
  GPU wrote into the VRAM copy (`gpu_written_bytes`, not in place): without this the update after a
  CPU write on top overwrote the copy with old bytes (`BB_GPU_DATA_CHECK=0` — as before). Both fixes
  are needed. After: 11.9–13.0, as in the old model; FPS the same (127–131 in both variants, three
  runs each).

## Running out of VRAM on 4 GB no longer stops the game

- RTX 3050 4 GB, old model (Auto on NVIDIA), 0.5: three crashes in an evening —
  `Failed to allocate 64 MiB for guest memory copies: ErrorOutOfDeviceMemory`. There is no fallback
  memory type (system) for the arena: NVIDIA sparse buffers take only VRAM. In pre5 the texture
  collector started keeping textures for 5 s (FPS did not drop over time), and on 4 GB VRAM ran out.
- Now: a smaller block (exactly the requested size), then any other memory type the arena can be
  bound to; and for 30 s the collector under pressure again evicts by submissions, as before 0.5. It
  also does so on its own when more than 92 % of the driver budget is used
  (`VK_EXT_memory_budget`, once per second). Can only be checked on such a card.

## Quitting the game from the gamepad (#74, #116)

- Overlay (Insert or L3+R3): a "Quit game" button, a second press confirms; the game closes as with
  the window button.

## Mouse

- Mouse camera: a click in the game window captures the mouse (SDL relative mode), F1 or leaving the
  window releases it; the settings menu and name entry also release it. Mouse movement is the right
  stick: deflection by mouse speed (~1000 pixels/s — full deflection at sensitivity 1), decays over
  60 ms; a moving mouse immediately passes the game's dead zone (`BB_MOUSE_DEADZONE`, 16 of 127), so
  slow aiming also turns the camera. The same at any FPS.
- Mouse buttons and the wheel are the same "keys" in the bindings: `Mouse Left` R1, `Mouse Right` L2
  (firearm), `Mouse Middle` R3 (lock-on), `Mouse X1` R2, `Mouse X2` L1, wheel — D-pad up/down (quick
  items); each wheel notch is a short press. The buttons work only with the mouse captured: the
  click that captures it does not attack.
- bbport.ini: `mouse_look=0/1`, `mouse_sensitivity` (0.05–20), `mouse_invert_y`; launcher —
  *Controls*: "Mouse camera", sensitivity, inversion; "Keyboard and mouse" — button assignment,
  "Assign" also accepts a mouse button/wheel. Based on PR #108 (virtual stick from mouse movement);
  the keyboard layout did not change.

## Other

- The swapchain is recreated via `oldSwapchain` (PR #93): on NVIDIA under Wayland recreation crashed
  with "Surface already has a tearing controller".
- `launcher/bb-launcher.sh` passes arguments (`--play`) through nix-shell as well (PR #120).
- FSR 4.1.1 capture from the AppImage on SteamOS and other distributions (PR #89): capture on the
  host via systemd, retry with the GPU presented as RX 7800 XT if the DLL does not offer FSR 4.1.
- Launcher: DLSS has a hint whether it is in this build (it is not in the AppImage: NVIDIA's DLSS SDK
  is not redistributed, the game enables FSR 3.1).

## DLSS in the AppImage and a custom NVIDIA library

- A build with `DLSS_SDK_ROOT` (DLSS SDK 310.9.1 from github.com/NVIDIA/DLSS) puts the bridge
  `libbbport_dlss.so`, `libnvidia-ngx-dlss.so.310.9.1` and `NVIDIA-DLSS-LICENSE.txt` into the package
  (the RTX SDK license allows redistribution as part of an application, not separately). Test
  AppImage: `dist/…-test-1010-dlss.AppImage`.
- Launcher, *Upscaler → DLSS: NVIDIA library → Choose file…* — like the DLL for FSR 4.1.1:
  `libnvidia-ngx-dlss.so.<version>` is copied to `<user>/dlss/`, the game uses it before the built-in
  one (log: `DLSS: NVIDIA's library from …`). A selected `nvngx_dlss.dll` gives a hint that a Linux
  library is needed. The bridge is looked up next to `bb-probe`, then in `<user>/dlss/`.
- The bridge also builds with Nix hardening flags (`-Werror=format-security`).
- Checked on RX 7800 XT only up to "not an NVIDIA GPU": the library path (built-in and custom) is
  chosen correctly. DLSS itself — on an NVIDIA RTX card.

## Building from git after updating from 0.4 (#124)

- `build.sh` stopped: "patch failed: src/ffx_vk_fsr4_v07.c:35" — the FSR-Vulkan submodule still had
  the previous version's patch applied, and the new one applied neither forward nor in reverse. Now
  in this case the submodule is reset and the patches are applied again (checked on a copy in that
  state).

## Online (shadNet) — as a separate module

- `gpu/bbnet` → `out/gpu/libbbnet.so`: shadPS4's network and PSN libraries from shadp2p (a shadPS4
  fork, GPL-2.0) with the shadNet client, ported from the Linux fork of bbport with online (and that
  one from the Windows port). The module builds as a separate target, its C++ is hidden
  (`-fvisibility=hidden`, `-Bsymbolic`), only `bbnet_init` and `bbnet_resolve` are exported
  (`gpu/bbnet/bbnet.h`). The module's own headers (`gpu/bbnet/include`) only supplement the shared
  ones: nothing changed in `gpu/shim` and `gpu/shadps4`.
- Runtime (`src/runtime_net.c`): with `BB_ONLINE=1` on the first network/PSN import it loads the
  module (`dlopen`, `RTLD_LOCAL`; `BB_NET_MODULE` — another path), hands it the game data and guest
  threads for NP callbacks; trophies, voice and store dialogs stay in the runtime. Without
  `BB_ONLINE` the module is not loaded; without the module or if it did not start — offline, as
  before.
- Build: protobuf, nlohmann_json, OpenSSL, zlib, miniupnpc (`shell.nix`); without them the module is
  not built (`BB_NET=OFF` — manually), the port builds as before. The AppImage contains the module.
- `run.sh`: address redirects (`scripts/online.py`: game server → The Hunter's Dream, summon signs →
  shadNet WebAPI), default shadNet server `srv.shadps4.net:31313`.
- Launcher: an *Online* page (game server, co-op server and WebAPI, shadNet account, UPnP,
  connection check); `settings.json` — owner only (0600, it holds the password).
- UPnP: the P2P port is now opened on the router when the transport is ready, and closed on exit
  (#121: `AddMapping` was not called anywhere).
- Checked: `tests/test_online.c` (network and NP imports → module, trophies → runtime); the game with
  `BB_ONLINE=1` without an account reaches the level (module: 1399 functions; "no users connected
  to shadNet"). shadNet login and summoning are not checked: there is no account, and the UDP part of
  the public server, per #121, does not respond.
- HTTPS in the AppImage: "Failed to obtain information from the Bloodborne server" ("Не удалось
  получить информацию от сервера Bloodborne") — The Hunter's Dream certificate verification failed
  (`SSL server verification failed`): OpenSSL from Nix takes the root certificates from
  `/etc/ssl/certs/ca-certificates.crt`, and on NixOS that is a link into the host's `/nix/store`,
  which is not visible inside the AppImage. The package wrappers now set `SSL_CERT_FILE` to their own
  certificate bundle (`nss-cacert`) if the specified one is not readable; `run.sh` from source uses
  the system one. Launcher: the error text in the connection check was no longer lost due to markup
  (`<urlopen error …>`), icons — from the Adwaita theme.

## Main menu flicker with output above 1080p

- With live resolution change (3840×2160 output on a 2560×1080 monitor) the main menu flickered: some
  frames were shown at the game's native 1920×1080, the rest with the interface at output resolution
  (4 of 50 captures). `BB_UI_TRACE=1` (new diagnostic: frames drawn without the interface at output
  resolution) showed: while the menu animates the highlight, the game draws it with other Scaleform
  shaders (`bb4d5f8c`, `e94b5b06`, `9951d199`, `44072322`, `a334a845`), not `09957251`; they were not
  in the interface shader list (`ui_composition.h`), and such frames were not composed at output
  resolution. After: 0 of 80; in a level the image is the same.

## Smaller AppImage

- The package contained all of Mesa with LLVM (~800 MB: only the RADV and ANV Vulkan drivers are
  needed), full GCC and the Vulkan headers (their paths in the debug info of `bb-probe`), the 32-bit
  half of MangoHud (second glibc, gtk3, icu, perl) and GStreamer for the launcher's GTK4. Now: own
  RADV (without LLVM, ACO) and ANV without the rest of Mesa, `bb-probe` without debug info, 64-bit
  MangoHud, GTK4 without GStreamer, printing and tracker.
- Also: SDL3 without built-in zenity (it pulled a second GTK4 with GStreamer; now the host's zenity,
  if present), libdecor without the GTK 3 plugin (for SDL3 and MangoHud's glfw; decorations are drawn
  by its cairo plugin), MangoHud without xdg-utils (Perl; the host's `xdg-open`). The own Mesa build
  includes Dozen only because the nixpkgs recipe expects its `spirv2dxil` output; it does not end up
  in the package.
- Result: AppImage 925 → 399 MB, uncompressed package 2.85 → 1.06 GB. Checked: the launcher opens,
  the game from the package starts on the built-in RADV up to the title screen.
