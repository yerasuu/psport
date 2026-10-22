# THIRD-PARTY-NOTICES and SOURCES.txt for what psport distributes (tools/bundle.sh, default.nix):
# for each store path given (the libraries a bundle copies) and each package given, its name,
# version, licence and source, with the licence files found at the top of its source (the SPDX
# text when it has none); then the code psport's binaries carry from this tree and from store
# packages that leave no library behind (headers, static libraries).
# `nix-build packaging/licences.nix --argstr storePaths '/nix/store/...-ffmpeg-headless-9.0.1-lib ...'`
{ pkgs ? import ./nixpkgs.nix
, storePaths ? ""
, packages ? [ ]
  # The online module's own code (cpp-httplib), for a local bundle that keeps libpsport-net.so.
, withNet ? false
}:
let
  lib = pkgs.lib;
  shell = import ../shell.nix { inherit pkgs; };
  port = import ./port.nix { inherit pkgs; };
  # Header-only or static: in psport's binaries, never in lib/ (gpu/CMakeLists.txt).
  compiledIn = with pkgs; [ boost magic-enum robin-map vulkan-memory-allocator vulkan-headers xbyak zydis miniz ];
  dependencies = d: lib.filter lib.isDerivation (lib.flatten [ (d.buildInputs or [ ]) (d.propagatedBuildInputs or [ ]) ]);
  key = d: builtins.unsafeDiscardStringContext d.drvPath;
  # The runtime libraries are among the build inputs of what shell.nix lists, plus the compiler's
  # runtime (libgcc_s, libstdc++) and glibc, which the stdenv brings without naming them.
  graph = builtins.genericClosure {
    startSet = map (drv: { key = key drv; inherit drv; })
      (shell.nativeBuildInputs ++ compiledIn ++ packages ++ [ pkgs.stdenv.cc.cc pkgs.stdenv.cc.libc ]);
    operator = item: lib.concatMap (drv:
      let evaluated = builtins.tryEval (key drv);
      in lib.optional evaluated.success { key = evaluated.value; inherit drv; }) (dependencies item.drv);
  };
  byStorePath = builtins.listToAttrs (lib.concatMap (item:
    map (output: {
      name = baseNameOf (builtins.unsafeDiscardStringContext item.drv.${output}.outPath);
      value = item;
    }) (item.drv.outputs or [ "out" ])) graph);
  requested = lib.filter (path: path != "") (lib.splitString " " storePaths);
  unknown = lib.filter (path: !(byStorePath ? ${baseNameOf path})) requested;
  inGraph = map (path: byStorePath.${baseNameOf path}) requested;
  libraries = lib.attrValues (builtins.listToAttrs (map (item: { name = item.key; value = item.drv; }) inGraph));
  # nixpkgs writes "A AND (B OR C)" as a compound licence: kept as that SPDX expression.
  spdxOf = l:
    if builtins.isString l then l
    else if (l.licenseType or "") == "compound" then
      "(" + lib.concatMapStringsSep " ${l.operator} " spdxOf l.licenses + ")"
    else l.spdxId or l.shortName or "";
  licence = l:
    if builtins.isString l then { spdx = l; name = l; }
    else if (l.licenseType or "") == "compound" then { spdx = lib.removeSuffix ")" (lib.removePrefix "(" (spdxOf l)); name = ""; }
    else { spdx = l.spdxId or ""; name = l.fullName or l.shortName or ""; };
  mirrors = import "${pkgs.path}/pkgs/build-support/fetchurl/mirrors.nix";
  resolve = url:
    let mirror = builtins.match "mirror://([^/]+)/(.*)" url;
    in if mirror == null then url else builtins.head mirrors.${builtins.head mirror} + builtins.elemAt mirror 1;
  source = src:
    if src == null then { path = null; urls = [ ]; rev = ""; hash = ""; }
    else {
      path = "${src}";
      urls = map resolve (src.urls or (lib.toList (src.url or [ ])));
      rev = src.rev or "";
      hash = src.outputHash or "";
    };
  # Where nixpkgs' licence list says less than the source does: the licence psport takes a choice
  # under, or which of the listed licences cover what this package carries.
  clarified = {
    # Every header in include/ is "Apache-2.0 OR MIT" (LICENSE.md and their SPDX lines); nixpkgs
    # lists only the Apache-2.0.
    vulkan-headers = {
      licences = [ { spdx = "Apache-2.0 OR MIT"; name = ""; } ];
      note = "the headers compiled in (include/) are Apache-2.0 OR MIT; psport uses them under the MIT";
    };
    libglvnd = {
      note = "its libraries are MIT (its own code, Mesa's, X.Org's list.h, cJSON) and BSD-1-Clause "
        + "(uthash); the Khronos API headers (include/EGL, include/GLES*) and registry files "
        + "(src/generate/xml) they are compiled with and generated from are Apache-2.0; the BSD-3-Clause "
        + "autogen.sh and the GPL-3.0 autoconf macros (under the Autoconf exception) only build it";
    };
  };
  component = role: d:
    let name = d.pname or (builtins.parseDrvName d.name).name;
    in {
      inherit role name;
      version = d.version or "";
      licences = clarified.${name}.licences or (map licence (lib.toList (d.meta.license or [ ])));
      note = clarified.${name}.note or "";
      homepage = d.meta.homepage or "";
      # libidn2 is a copy of its bootstrap build, with no src of its own: its out is the original.
      source = source (d.src or d.out.src or null);
      outputs = map (output: baseNameOf (builtins.unsafeDiscardStringContext d.${output}.outPath)) (d.outputs or [ "out" ]);
    };
  vendor = name: url: spdx: files: { inherit name url spdx files; };
  vendored = [
    (vendor "sirit" "https://github.com/shadps4-emu/sirit" "BSD-3-Clause"
      [ { file = ../gpu/third_party/sirit/LICENSE.txt; } { file = ../gpu/third_party/sirit/externals/SPIRV-Headers/LICENSE; } ])
    (vendor "AMD GCN headers" "https://github.com/GPUOpen-Drivers/pal" "MIT"
      [ { file = ../gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h; lines = 24; } ])
    (vendor "half" "https://half.sourceforge.net/" "MIT" [ { file = ../gpu/third_party/half/LICENSE.txt; } ])
    (vendor "FSR-Vulkan and AMD FidelityFX SDK" "https://github.com/FireBurn/FSR-Vulkan" "MIT" [
      { file = ../gpu/third_party/fsr-vulkan/LICENSE.txt; }
      { file = ../gpu/third_party/fsr-vulkan/NOTICE.md; }
      { file = ../gpu/third_party/fsr-vulkan/upstream/ffx-1.1.4/sdk/LICENSE.txt; }
    ])
    (vendor "LibAtrac9" "https://github.com/Thealexbarney/LibAtrac9" "MIT" [ { file = ../third_party/LibAtrac9/LICENSE; } ])
    (vendor "Dear ImGui" "https://github.com/ocornut/imgui" "MIT" [ { file = "${port.imgui}/LICENSE.txt"; } ])
    (vendor "RenderDoc in-application API header" "https://github.com/baldurk/renderdoc" "MIT"
      [ { file = ../gpu/third_party/renderdoc/renderdoc_app.h; lines = 24; } ])
    (vendor "DejaVu fonts" "https://dejavu-fonts.github.io/" "Bitstream-Vera" [ { file = ../gpu/third_party/fonts/LICENSE-DejaVu.txt; } ])
    # Files shadPS4 took from other projects, compiled into libpsport-gpu.so: their headers carry
    # only the SPDX identifier, so the licences' texts follow (CC0 asks for none).
    ((vendor "Files in the shadPS4 tree under their own licence" "https://github.com/shadps4-emu/shadPS4"
      "MIT, BSD-3-Clause AND GPL-2.0-or-later, Zlib, CC0-1.0" [
        { file = ../gpu/shadps4/video_core/host_shaders/fsr.comp; lines = 2; }
        { file = ../gpu/shadps4/video_core/host_shaders/fsr/ffx_a.h; lines = 41; }
        { file = ../gpu/shadps4/video_core/host_shaders/fsr/ffx_fsr1.h; lines = 30; }
        { file = ../gpu/shadps4/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp; lines = 2; }
        { file = ../gpu/shadps4/common/bit_field.h; lines = 3; }
        { file = ../gpu/shadps4/video_core/renderer_vulkan/vk_barrier_tracker.h; lines = 2; }
        { file = ../gpu/shadps4/common/alignment.h; lines = 2; }
      ]) // { spdxTexts = [ "MIT" "BSD-3-Clause" "Zlib" ]; })
  ] ++ lib.optional withNet
    (vendor "cpp-httplib" "https://github.com/yhirose/cpp-httplib" "MIT" [ { file = ../gpu/third_party/httplib/LICENSE; } ]);
  manifest = {
    nixpkgs = lib.version;
    components = map (component "library") libraries
      ++ map (component "compiled in") compiledIn
      ++ map (component "program") packages;
    inherit vendored;
  };
in
if unknown != [ ] then
  throw "licences.nix: no package in shell.nix's dependencies owns ${toString unknown}; build the port with the same nixpkgs"
else
  pkgs.runCommand "psport-licences" {
    nativeBuildInputs = [ pkgs.python3 pkgs.zstd pkgs.unzip ];
    manifest = builtins.toJSON manifest;
    passAsFile = [ "manifest" ];
    spdx = "${pkgs.spdx-license-list-data.text}/text";
  } ''
    mkdir -p $out
    python3 ${./licences.py} "$manifestPath" "$spdx" $out
  ''
