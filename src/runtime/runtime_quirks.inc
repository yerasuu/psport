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

QUIRK(nvidia_slow_sparse_rebind, HOST,
      "NVIDIA's proprietary driver: each rebinding of the game's memory in the sparse arena held the GPU for 10 s and "
      "more, the desktop with it (GTX 1660 Ti, 615.71; its sparse binding has slowed since 555, the time growing with "
      "the pages already bound, which the startup test bind does not reach): the layer's memory module there",
      "#261, #373")
QUIRK_GPU(nvidia_slow_sparse_rebind, 0, 4, "", 0, 0xffffffff)
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
QUIRK(bloodborne_gx_node_guard, WORKAROUND,
      "Bloodborne's GX swap chain takes its per-frame render command's node from a per-frame bump allocator that "
      "returns null once the frame's heap is used up, and writes through it unchecked at 0x2199310 (a guest fault at "
      "address 8, bbport #162, #192): the title's guest hook skips the post for a null node, so the command goes out "
      "with the next frame. The heap does not fill on a PS4; the general fix finds what makes it fill under psport "
      "(nodes the render thread releases late, or a frame pacing difference) and corrects that",
      "#504")
QUIRK(bloodborne_gx_command_heap, WORKAROUND,
      "Bloodborne's GX swap chain sets up its two per-frame command heaps with 256 KiB (mov edx, 0x40000 at "
      "0x2195c0a and 0x2195c47) and under psport they fill, the render command's node coming back null (what "
      "bloodborne_gx_node_guard catches): the title gives each 1 MiB (bbport 0.6). The heap does not fill on a PS4; "
      "the general fix finds what makes it fill under psport and corrects that",
      "#504")
QUIRK(bloodborne_render_job_slots, WORKAROUND,
      "Bloodborne's render job runner step (0x1067f60) leaves through its failure branches with its pool slot still "
      "held, and the runner's destructor and reset drop a held slot without giving it back: after six failed jobs "
      "every thread waiting for a slot sleeps for good (Laurence's cinematic above 30 FPS). The title gives the slot "
      "back at the step's exit and before those clears (bbport 0.6); the general fix finds why the steps fail under "
      "psport (0x2165150, 0x2169c30) and not on a PS4",
      "#683")
QUIRK(bloodborne_blur_sample_clamp, WORKAROUND,
      "Bloodborne's Yebis blur kernel (GPUTexUtil_GetRecursiveSampleParameters, 0xbbc3e0) asserts nBaseSamples >= 3 "
      "in the retail build and now and then gets 2 under psport: the assertion raises 0xa0020007 and the game stops. "
      "The title raises the count to 3 at the entry (bbport 0.6); the general fix finds where the 2 comes from (a "
      "resolution, format or value psport reports differently from a PS4)",
      "#684")
QUIRK(bloodborne_fx_init_clear, WORKAROUND,
      "Bloodborne's FX init (0xebb960) clears three 2x2 RG16F history targets through a graphics context's clear "
      "(0x2168630), whose packets came out as bad PM4 (opcode 23) at a save's load: the title writes the clear colour "
      "into those targets in memory instead, in their format (bbport 0.6 zeroes them with a private WRITE_DATA). The "
      "general fix finds what the translator does wrong with those clears",
      "#685")
QUIRK(bloodborne_intel_tonemap, WORKAROUND,
      "Bloodborne's tone mapping turns black on Intel CPUs (DLC areas most; darker than on a PS4): the game skips a "
      "block gated on the byte at +0x128 of its tone mapping state (je at 0x25843e5), a flag the community traces to "
      "approximate float instructions (rcpps/rsqrtps, ~5,800 VEX and SSE ones in eboot.bin) whose results differ "
      "between Intel and the PS4's AMD Jaguar (not yet confirmed in the code that writes it). The title applies the "
      "community's Intel Black Tonemap Fix (that je as NOPs) on GenuineIntel CPUs, PS_INTEL_TONEMAP_FIX=0/1 forcing "
      "it; the general fix makes those instructions answer as the Jaguar's for every title",
      "#350")
