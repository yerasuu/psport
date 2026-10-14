bbport is the counterpart of Wine + DXVK for a single game: Bloodborne (v1.09) on Linux.
Changes since [0.5](https://github.com/deadinside28/bloodborne_pc/releases/tag/0.5); details and
measurements: [CHANGES_after_0.5.md](https://github.com/deadinside28/bloodborne_pc/blob/master/docs/CHANGES_after_0.5.md) (Russian).

### New

- **Online play (shadNet)**, as a separate module (`libbbnet.so`, loaded only when online play is
  on): messages, bloodstains and ghosts from The Hunter's Dream, bells and summons through a
  shadNet server (`srv.shadps4.net:31313` by default) with a shadNet account. Launcher → *Online*:
  servers, account, UPnP, connection test. The router port is opened by UPnP (#121). The same
  servers as the Windows port. The network and PSN libraries come from shadp2p (a shadPS4 fork,
  GPL-2.0) by way of the Linux and Windows forks of bbport.
- **Mouse look**: click into the game to take the mouse, F1 releases it; mouse buttons and the
  wheel can be bound like keys (Mouse Left = R1, Mouse Right = L2, Middle = R3, side buttons
  R2/L1, wheel = quick items). Launcher → *Controls*: mouse look, sensitivity, invert Y (#51, #52).
- **DLSS in the AppImage** (NVIDIA GeForce RTX): the bridge and NVIDIA's DLSS 310.9.1 library are
  included (NVIDIA's RTX SDK license allows it within an application; its text is in the package).
  A newer `libnvidia-ngx-dlss.so.*` can be chosen in the launcher (*Upscaler → DLSS: NVIDIA's
  library*), like the FSR 4.1.1 DLL.
- **Every retail release** of Bloodborne with update 1.09 is accepted (US CUSA00900, EU, UK, JP,
  Asia and The Old Hunters editions; PR #117); an `eboot.bin` with a 60 fps patch baked in is
  reported.
- **Quit the game** from the overlay (Insert / L3+R3; press twice) — #74, #116.

### Fixes

- The US release (and other regions) crashed with code 139 on entering the world in the new
  memory model: they ran without the game's profile (#119).
- Motion blur smeared the whole frame while the camera turned in the new memory model (#122).
- Cards with 4 GB (NVIDIA): "Failed to allocate 64 MiB for guest memory copies" stopped the game;
  now it frees textures sooner and uses other memory instead.
- The main menu flickered with output resolutions above 1080p (live resolution).
- Online, "Could not get information from the Bloodborne server" in the AppImage on NixOS (HTTPS
  certificates).
- NVIDIA on Wayland: the swapchain is recreated through `oldSwapchain` (PR #93).
- Building from git after updating from 0.4: "patch failed: src/ffx_vk_fsr4_v07.c" (#124).
- `bb-launcher.sh --play` through nix-shell (PR #120); FSR 4.1.1 recording from the AppImage on
  SteamOS (PR #89).

### Smaller download

The AppImage is **399 MB instead of 925 MB**: only Mesa's Vulkan drivers (RADV without LLVM and
ANV) instead of all of Mesa, no debug info in the game binary, MangoHud without its 32-bit half,
the launcher's GTK without GStreamer.

**Known limits:** online login and summons were not tested by the author (no shadNet account);
the UDP side of the public shadNet server did not answer in #121. Mouse look and DLSS need
testing on more systems.

The AppImage contains no game files. AMD's FSR 4 model assets are not bundled. `SHA256SUMS`
accompanies the download.
