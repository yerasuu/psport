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
 * A title names its LAUNCH, OPTIMIZATION and WORKAROUND quirks in its Game::Title (quirks). */

QUIRK(amd_pc_memory_model, HOST,
      "the PC memory model (the GPU uses the game's memory in place) is measured on AMD GPUs only; the others keep "
      "the 0.3 model (run.sh, PS_PC_MODEL) unless the layer's memory module or PS_PC_MODEL_ANY_GPU=1 carries it",
      "#261")
QUIRK_GPU(amd_pc_memory_model, 0x1002, 0, "", 0, 0xffffffff)
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
