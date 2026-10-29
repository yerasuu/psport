# Licenses

psport is licensed under the GNU General Public License, version 2 ([LICENSE](LICENSE)). Files marked
GPL-2.0-only in their headers are under version 2 only; the other psport files are GPL-2.0-or-later, used as
version 2, so the binaries are distributed under GPL-2.0 (kb #843: psport does not move to GPL-3.0). Every other
component keeps the terms of its own authors. A release is a collection of components with these terms, not one
licence applied to every file. [LICENSES.json](LICENSES.json) lists the components machine-readably, and
`python3 tools/check_licenses.py` verifies that it matches the tree.

## Components

| Component | Terms and source |
| --- | --- |
| psport runtime, GPU glue, launcher and tooling (`src`, `gpu/shim`, `gpu/platform`, `gpu/port`, `gpu/memory`, `gpu/diag`, `gpu/layer`, `launcher`, `tools`, `packaging`, scripts) | GPL-2.0-or-later (GPL-2.0-only where a header says so); `LICENSE`, `README.md` and each file's SPDX line (the generated import tables have none) |
| Per-title game profiles (`gpu/games`) | GPL-2.0-or-later; the BBSC co-op code is Copyright 2026 CLS_AMG |
| libSceNgs2 port (`gpu/ngs2_anyps5`) | GPL-2.0-only; from AnyPS5 (https://github.com/boykopovar/AnyPS5) at `5edae353` (FORK.md); psport's own files in it (glue, envelope, filter, limiter, peak meter, VAG) GPL-2.0-or-later |
| libSceAudiodec and the native libSceNgs2 (`gpu/audiodec`, `gpu/ngs2`) | GPL-2.0-or-later; psport's own, over libavcodec and LibAtrac9 (Audiodec) and shadPS4's Ngs2 headers and LibAtrac9 (Ngs2) |
| shadPS4 vendored tree (`gpu/shadps4`) | GPL-2.0-or-later; https://github.com/shadps4-emu/shadPS4 at `b4e7ae7` and the fixes `gpu/VENDOR.txt` lists |
| Files in that tree under their own licence | MIT (`sha1.h`, `ssa_rewrite_pass.cpp`, `fsr.comp`, AMD's `ffx_a.h` and `ffx_fsr1.h`), BSL-1.0 (`aes.h`), CC0-1.0 (`alignment.h`), BSD-3-Clause AND GPL-2.0-or-later (`bit_field.h`), Zlib (`vk_barrier_tracker.*`, DXVK) |
| libSceFiber and libSceFont (`gpu/fiber`, `gpu/font`) | GPL-2.0-or-later; shadPS4 at `3a9f572` |
| Online module `libpsport-net.so` (`gpu/net`) | GPL-2.0-or-later, with `np_handler.*` and `shadnet/client.*` GPL-2.0-only; shadp2p by way of bbport 0.51. Not in any public bundle, see below |
| cpp-httplib (`gpu/third_party/httplib`) | MIT; Copyright (c) 2017 yhirose; v0.45.0 with one psport change; online module only |
| DLSS bridge (`gpu/dlss_bridge`) | MIT; Copyright (c) 2026 IFreemz and bbport contributors: IFreemz's bridge as Supermedo adapted it in his Windows fork of bbport (`9514b19`). NVIDIA's DLSS SDK is not included |
| FSR-Vulkan with the AMD FidelityFX SDK sources (`gpu/third_party/fsr-vulkan`) | MIT; FireBurn's FSR-Vulkan at `c64f093`, AMD's per-file MIT notices, every SDK 2.3 file on the MIT list of SDK 2.3's `docs/license.md`; `NOTICE.md` |
| AMD FidelityFX API headers (`tools/fsr4cap/ffx`) | MIT; Copyright (C) 2026 Advanced Micro Devices, Inc. |
| sirit with SPIRV-Headers (`gpu/third_party/sirit`) | BSD-3-Clause (sirit `c58f4d4`, three files changed) and the Khronos Group's MIT-style grant (SPIRV-Headers `2acb319`), recorded as MIT; JsonCpp (public domain or MIT) in its build tools |
| half, xbyak, AMD GCN headers, RenderDoc header, Dear ImGui, LibAtrac9 | MIT (half 1.12.0, GCN headers, RenderDoc, ImGui, LibAtrac9) and BSD-3-Clause (xbyak); copyright lines in LICENSES.json |
| DejaVu fonts 2.37 (`gpu/third_party/fonts`) | Bitstream Vera Fonts licence with public domain DejaVu changes and the Arev licence (`LICENSES/LicenseRef-DejaVu-Fonts.txt`) |
| Tracy (`gpu/third_party/tracy`) | BSD-3-Clause; Copyright (c) 2017-2025 Bartosz Taudul; v0.13.1, only in `PS_TRACY=ON` builds |
| BBSC game server (`scripts/bbsc_gameserver`: `bbsc_gameserver.py`, `README.md`, `NOTICE.txt`) | GPL-2.0-or-later AND MIT; Copyright 2026 CLS_AMG (NOTICE.txt), with NoDreamForHunter's ss.info format and response shapes (MIT, fhody125-web) |
| BBSC's bbhost service (`scripts/bbsc_gameserver/bbhost_service.py`) | GPL-3.0-or-later; written from bbhost's client source (droogie/bbhost, GPL-3.0-or-later), which NOTICE.txt's or-later grant allows |
| NoDreamForHunter's ss.info template (`ss_info_template.xml`, `LICENSE.NoDreamForHunter`) | MIT; Copyright (c) 2026 fhody125-web |
| Bloodborne patches (`patches`) | Community authors named in each patch; no licence stated, each author keeps their rights; kept as it is (kb #1131) |
| dxil-spirv patch (`tools/fsr4cap/dxil-spirv-class-bindings.patch`) | MIT (dxil-spirv's `dxil_spirv.cpp`) AND GPL-2.0-or-later (bbport's added lines) |
| FFmpeg recipe for Windows (`packaging/windows/ffmpeg`) | BSD-3-Clause (MSYS2's MINGW-packages, `LICENSE.MINGW-packages`) AND GPL-2.0-or-later (psport's changes) |
| Documentation | Assumed GPL-2.0-or-later (review: pending) |
| Libraries linked into the bundles (FFmpeg, SDL3, FreeType, glibc, DLLs and others) | Their own licences, listed by each bundle's generated notices |

## Boundaries between components

**GPL-2.0 binary.** psport's programs and `libpsport-gpu.so` link GPL-2.0-or-later and GPL-2.0-only code into
one binary, so they are distributed under GPL-2.0 and nothing under GPL-3.0 or the Apache-2.0 may be linked into
them. The GPL-2.0-or-later files (shadPS4, bbport, psport's own) are used as version 2; the GPL-2.0-only files
(AnyPS5's Ngs2, two RPCS3-derived files in the online module) stay as they are. FFmpeg is built as
LGPL-2.1-or-later without GPL, version 3 or network code, so it can be linked.

**AnyPS5.** The code of `gpu/ngs2_anyps5` is AnyPS5's, GPL-2.0-only. For everything else AnyPS5 is
read for behaviour only (`gpu/audiodec` took the contract of its exports from AnyPS5's `Export.cpp`, not its
code); the firmware is read with REA and the result is written natively.

**bbport and its forks.** bbport (GPL-2.0-or-later) is psport's origin; changes from SrShadowy's and
Teefpyre's forks of bbport (both GPL-2.0-or-later: their LICENSE is bbport's, their READMEs say "or later" and the
files taken carry GPL-2.0-or-later headers) are ported by hand and recorded in FORK.md with their source commits.
Supermedo's fork (`windows` branch, GPL-2.0-or-later) is read the same way; its `thd/` and bbbridge parts are
GPL-3.0 and are never taken, and its `dlss_bridge` is MIT (see the DLSS bridge row). Mrsuss60's Windows port
(GPL-2.0, its README says "v2 or later") is read for its Windows runtime and GPU parts; its menu pointer comes from
bbhost (GPL-3.0-or-later) and is never taken. willl03's fork carries a plain GPL-2.0 LICENSE, so only its behaviour is
taken, written natively. Nothing is merged wholesale.

**Online module.** `libpsport-net.so` is a separate shared library that the GPU library loads, with its own
dependencies: OpenSSL, protobuf, nlohmann_json, zlib and miniupnpc (OpenSSL and abseil, through protobuf, are
Apache-2.0). Since Apache-2.0 cannot be combined with GPL-2.0, no public bundle, binary cache or AppImage carries
it. It is left out until kb #850 replaces OpenSSL and abseil; `PS_BUNDLE_NET=1` and `PS_PACKAGE_NET=1` keep it
in a package that is never redistributed. A source build keeps it.

**Vulkan loader and drivers.** The bundles do not carry the Vulkan loader (Apache-2.0): they use the host's
`libvulkan.so.1` and the host's drivers.

**Downloaded or user-supplied.** AMD's signed FSR 4 DLLs and model data, the FSR 4 v07 assets that
`tools/fetch_fsr4_assets.sh` downloads on the user's machine, NVIDIA's DLSS SDK and libraries, game files and
console system modules are not included in any bundle. dxil-spirv (with its patch) and fsr4cap.exe (with AMD's API
headers) are built on the user's machine, or into the AppImage built locally by `packaging/default.nix`; no release
bundle carries them.

**BBSC game server.** `scripts/bbsc_gameserver` is a separate Python program that `run.sh` starts as a
process; psport neither links nor imports it. Each file carries its own SPDX header: `bbsc_gameserver.py`
imports `bbhost_service.py`, so the running server is used under GPL-3.0-or-later, which BBSC's
GPL-2.0-or-later grant allows. psport's binaries stay GPL-2.0.

**decanter.** decanter (GPL-3.0-or-later) runs psport as a separate process; it neither links psport nor
distributes it.

## Required notices and acknowledgements

- BSD-3-Clause (sirit, xbyak): the copyright notice, the conditions and the disclaimer are kept in source
  (`gpu/third_party/sirit/LICENSE.txt`, `gpu/third_party/xbyak/COPYRIGHT`) and reproduced in binary
  distributions, here through the bundles' notices. Their authors' names are not used to endorse psport.
- MIT, BSL-1.0 and Zlib components: their copyright and permission notices travel with the source and, for
  binaries, in the bundles' THIRD-PARTY-NOTICES (`packaging/licences.nix` lists the vendored code, including the
  shadPS4 files under their own licence).
- DejaVu fonts: Copyright (c) 2003 Bitstream, Inc.; the DejaVu changes are public domain; glyphs from Arev
  Fonts are (c) Tavmjong Bah. The licence text must accompany the fonts and the fonts are not sold by themselves.
- FidelityFX SDK and FSR-Vulkan: keep AMD's MIT notices embedded in each file and `LICENSE.txt` of the SDK
  (`gpu/third_party/fsr-vulkan/upstream/ffx-1.1.4/sdk/LICENSE.txt`). SDK 2.3's repository-level licence
  (`docs/license.md` at v2.3.0) allows other files in binary form only and forbids reverse engineering; it puts
  the source files it lists under the MIT licence, and every 2.3 file psport carries is on that list (LICENSES.json).
  A file imported from 2.3 later must be on it too (`UPSTREAM.md`). Do not describe the experimental source-v07 provider as AMD FSR 4.1.1, Ray
  Regeneration or ML Frame Generation (`NOTICE.md`).
- Credits for the projects psport builds on and for the community patches are in README.md.

## Release contents

The release workflow publishes, per tag, what the repository's scripts build today:

| File | What it carries |
| --- | --- |
| `psport-VERSION-x86_64.tar.zst` and its `.sha256` (Linux, `tools/bundle.sh`) | psport's programs and `libpsport-gpu.so`, `run.sh`, `scripts/`, `patches/`, five FSR 4 asset scripts in `tools/`, every library they link from the Nix store with the store's glibc and its loader in `lib/`, `LICENSE`, `LICENSES.md`, `LICENSES.json`, `LICENSES/`, `THIRD-PARTY-NOTICES` and `SOURCES.txt` (generated by `packaging/licences.nix`: each library's licence text and source URL, the code compiled in, and a written source offer valid for three years). No online module, no Vulkan loader |
| `psport-windows-VERSION-x86_64.zip` and its `.sha256` (Windows, `tools/win32-bundle.sh`) | `psport.exe`, `libpsport-gpu.dll`, `psport-gpu-capabilities.exe`, `psport-windows-probe.exe`, the CLANG64 DLLs they import, `LICENSE.txt`, `LICENSES.md`, `LICENSES.json`, `LICENSES/`, `THIRD-PARTY-NOTICES.txt` and `SOURCES.txt` (one entry per package with the licence files it installs, then the code from this tree under its own licence), `BUILD-INFO.txt`. A DLL whose licence is not GPL-2.0-compatible is left out and named; FFmpeg is psport's own LGPL build. The sources of the DLLs built from `packaging/windows/*/PKGBUILD` are published beside the zip |
| Source (the tag's archive on GitHub) | psport's corresponding source, with `shell.nix`, `packaging/port.nix` and `packaging/nixpkgs.nix`, which pin every Nix library to one nixpkgs revision |

`tools/check_bundle_licences.sh` runs on every Linux bundle and fails the release if it holds OpenSSL, abseil,
the Vulkan loader, GPL-encoder libraries, an FFmpeg built with version 3, a GPL-3.0-only library or the online
module, or lacks `LICENSE`, the licence files, `THIRD-PARTY-NOTICES`, `SOURCES.txt` or the notices of an entry of
`packaging/vendored.txt`. The Windows script checks each DLL against an allow-list of GPL-2.0-compatible licences.

Every bundle carries this file, `LICENSES.json` and `LICENSES/` beside `LICENSE`; the licence texts of the
vendored code are also in the generated notices, whose list of it is `packaging/vendored.txt`, shared by the Linux
and Windows scripts. See [docs/en/PACKAGING.md](docs/en/PACKAGING.md) for how a release is assembled and checked.

License texts are in [LICENSES](LICENSES). File notices remain authoritative; a manifest does not replace them
or grant permission over third-party work.

- [GPL version 2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
- [GNU license compatibility](https://www.gnu.org/licenses/license-compatibility.html)
- [SPDX license list](https://spdx.org/licenses/)
