/* The quirk registry's entries (docs/en/QUIRKS.md), expanded by runtime_quirks.h and
 * runtime_quirks.c, which define each macro before including this file:
 *
 *   QUIRK(id, kind, sz_reason, sz_reference)
 *       kind: LAUNCH, OPTIMIZATION, HOST or WORKAROUND; reason and reference are non-empty string
 *       literals (checked at compile time): why the quirk exists and the task, issue or decision
 *       that tracks it.
 *   QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last)
 *       a HOST quirk's match on the Vulkan device: PCI vendor id and VkDriverId (0: any), a
 *       substring of the device name ("": any) and an inclusive driverVersion range.
 *   QUIRK_TRAITS(id, traits)
 *       a match on the loader's middleware traits: every bit of the mask present.
 *
 * A title names its LAUNCH, OPTIMIZATION and WORKAROUND quirks in its Game::Title (quirks). A
 * WORKAROUND is a temporary correction for one title: only its title's quirks activate it (no
 * QUIRK_GPU or QUIRK_TRAITS, checked at compile time), its reference names the kb task seeking the
 * general fix ("#<number>", checked by gpu/games/game_profile.cpp) and its code lives in
 * gpu/games/<game>/. */

QUIRK(amd_guest_sparse_arena, HOST,
      "the game's memory in a sparse arena on AMD GPUs, the layer's memory module elsewhere (PS_LAYER_MEMORY): "
      "each rebinding held NVIDIA's GPU for 10 s and more (GTX 1660 Ti, 615.71), while on an RX 7800 XT the module "
      "kept ~1 GB of an area's data out of VRAM (100 FPS against ~200)",
      "#261")
QUIRK_GPU(amd_guest_sparse_arena, 0x1002, 0, "", 0, 0xffffffff)
QUIRK(amd_guest_dmabuf_first, HOST,
      "the game's direct memory in dma-buf chunks first on AMD GPUs (tested the most), host memory import first "
      "elsewhere (PS_GUEST_MEMORY): NVIDIA's dma-buf maps at offset 0 only and CPU access through it may be slow",
      "#261")
QUIRK_GPU(amd_guest_dmabuf_first, 0x1002, 0, "", 0, 0xffffffff)
QUIRK(radv_gcn_nohiz, HOST,
      "GCN 3-5 under RADV (Polaris, Vega and their APUs): with HiZ, depth compression drew black halos and lost "
      "geometry; run.sh sets RADV_DEBUG=nohiz unless RADV_DEBUG is the user's or PS_AUTO_NOHIZ=0",
      "issues #54, #99")
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV TONGA", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV ICELAND", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV CARRIZO", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV FIJI", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV STONEY", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV POLARIS", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV VEGA", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV RAVEN", 0, 0xffffffff)
QUIRK_GPU(radv_gcn_nohiz, 0x1002, 0, "(RADV RENOIR", 0, 0xffffffff)
QUIRK(nvidia_slow_depth_copies, HOST,
      "NVIDIA GPUs without VK_KHR_fragment_shader_barycentric (older than Turing) copy depth/stencil in nine draws: "
      "live resolution changes ran at 7-8 FPS on a GTX 1060, so live_resolution=auto keeps the startup patch there",
      "#337")
QUIRK_GPU(nvidia_slow_depth_copies, 0x10de, 0, "", 0, 0xffffffff)
QUIRK(bloodborne_save_sound_flag, WORKAROUND,
      "Bloodborne's game data save (SPRJ0005/userdata0010) keeps a byte at 0x204E that, left at 0, silences part of "
      "the game's audio (the player's weapon sounds among them): the title sets it to 1 in the user's saves before "
      "the game runs (rainvmaker's fix from the Diegolix29 shadPS4 fork; a save the game creates is fixed at the next "
      "launch). The cause is unknown: the sounds play on a PS4, so under psport something the game decides that byte "
      "from answers differently from the console, most likely an audio output or system state query (AudioOut, "
      "Audio3d, Ngs2, SystemService); the general fix finds what writes or clears the byte and corrects that library",
      "#348")
