# THIRD-PARTY-NOTICES and SOURCES.txt for what psport distributes (tools/bundle.sh, default.nix):
# for each store path given (the libraries a bundle copies) and each package given, its name,
# version, licence and source, with the licence files found at the top of its source (the SPDX
# text when it has none); then the code psport's binaries carry from this tree and from store
# packages that leave no library behind (headers, static libraries).
# `nix-build packaging/licences.nix --argstr storePaths '/nix/store/...-ffmpeg-headless-9.0.1-lib ...'`
{ pkgs ? import ./nixpkgs.nix
, storePaths ? ""
, packages ? [ ]
  # The online module's own code (cpp-httplib, nlohmann_json), for a local bundle that keeps
  # libpsport-net.so.
, withNet ? false
  # The DLSS bridge and fsr4cap.exe, which only the AppImage (default.nix) carries.
, withDlss ? false
, withFsr4cap ? false
}:
let
  lib = pkgs.lib;
  shell = import ../shell.nix { inherit pkgs; };
  port = import ./port.nix { inherit pkgs; };
  # Header-only or static: in psport's binaries, never in lib/ (gpu/CMakeLists.txt).
  compiledIn = with pkgs; [ boost magic-enum robin-map vulkan-memory-allocator vulkan-headers zydis miniz ]
    ++ lib.optional withNet pkgs.nlohmann_json;
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
    # Every file is "Apache-2.0 OR GPL-2.0-or-later" (its headers' SPDX lines); nixpkgs lists both
    # licences as if both applied. The online module links it (kb #850).
    mbedtls = {
      licences = [ { spdx = "Apache-2.0 OR GPL-2.0-or-later"; name = ""; } ];
      note = "dual-licensed Apache-2.0 OR GPL-2.0-or-later; psport uses it under the GPL-2.0-or-later";
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
  # packaging/vendored.txt, the list the Windows package's notices take too.
  carried = { "" = true; net = withNet; dlss = withDlss; fsr4cap = withFsr4cap; };
  # Dear ImGui is a submodule, empty in this tree's copy in the store: its licence comes from
  # port.nix's fetch of the same revision.
  treeFile = path:
    let imgui = "gpu/third_party/imgui/";
    in if lib.hasPrefix imgui path then "${port.imgui}/${lib.removePrefix imgui path}" else ./.. + "/${path}";
  vendorFile = spec:
    let parts = lib.splitString ":" spec;
    in { file = treeFile (lib.head parts); } // lib.optionalAttrs (lib.length parts > 1) { lines = lib.toInt (lib.elemAt parts 1); };
  vendorEntry = line:
    let fields = lib.splitString "|" line;
    in {
      name = lib.elemAt fields 0;
      url = lib.elemAt fields 1;
      spdx = lib.elemAt fields 2;
      files = map vendorFile (lib.filter (spec: spec != "") (lib.splitString " " (lib.elemAt fields 3)));
      when = if lib.length fields > 4 then lib.elemAt fields 4 else "";
    };
  vendored = map (v: removeAttrs v [ "when" ]) (lib.filter (v: carried.${v.when})
    (map vendorEntry (lib.filter (line: line != "" && !lib.hasPrefix "#" line)
      (lib.splitString "\n" (builtins.readFile ./vendored.txt)))));
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
