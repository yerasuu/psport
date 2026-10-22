# The port built from source in Nix's sandbox by build.sh, with shell.nix's dependencies: its output
# is laid out as a checkout after build.sh (run.sh, scripts/, patches/, out/psport,
# out/psport-gpu-capabilities, out/gpu/), so run.sh runs from it with PS_PREBUILT=1 and
# PS_PROBE=<output>/out/psport. CI publishes it to the binary cache (tools/nix_cache.sh) with
# withNet = false: the online module (libpsport-net.so) links OpenSSL and abseil (Apache-2.0), which
# GPL-2.0 psport can't redistribute until kb #850, so the public cache leaves it and their closure out.
# `nix-build packaging/port.nix [--arg withNet false]`
{ pkgs ? import ./nixpkgs.nix
, withNet ? true
}:
let
  lib = pkgs.lib;
  root = ./..;
  shell = import ../shell.nix { inherit pkgs; };
  imgui = pkgs.fetchFromGitHub {
    owner = "ocornut";
    repo = "imgui";
    rev = "f1cc2ae15e53a861a874c3034aae6798fde194ab";
    hash = "sha256-IjW+qddzKu9jOj3QCGhkChVK2UOvwl493ffUIIn/ZVQ=";
  };
in
pkgs.stdenv.mkDerivation {
  pname = "psport";
  version = lib.fileContents ../VERSION;
  # Tracked files only: the checkout also holds builds, profiles (no PGO here) and captures.
  src = lib.fileset.toSource {
    inherit root;
    fileset = lib.fileset.gitTracked root;
  };
  nativeBuildInputs = shell.nativeBuildInputs;
  dontConfigure = true;
  postPatch = ''
    rm -rf gpu/third_party/imgui
    cp -r ${imgui} gpu/third_party/imgui
    chmod -R u+w gpu/third_party/imgui
  '';
  buildPhase = ''
    runHook preBuild
    PS_IN_NIX_SHELL=1 PS_ORIGIN_RPATH_ONLY=1 CCACHE_DISABLE=1 PS_NET=${if withNet then "ON" else "OFF"} bash build.sh
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out/out/gpu
    cp -r LICENSE run.sh scripts patches $out/
    # FSR 4 v07 assets are fetched on the user's machine, the optimized passes built there.
    install -Dm755 -t $out/tools tools/fetch_fsr4_assets.sh tools/fsr4_optimize.sh
    install -m644 -t $out/tools tools/fsr4_post_lds.pl tools/fsr4_pass11_guard.pl tools/Fsr4SpirvCrossFixes.pm
    install -m755 out/psport out/psport-gpu-capabilities $out/out/
    install -m755 out/gpu/libpsport-gpu.so $out/out/gpu/
    if [ -f out/gpu/libpsport-net.so ]; then install -m755 out/gpu/libpsport-net.so $out/out/gpu/; fi
    runHook postInstall
  '';
  # Debug info names the build environment's headers, which Nix would keep as references.
  stripDebugList = [ "out" ];
  # patchelf corrupts the non-PIE game binary's symbol versions (see default.nix).
  dontPatchELF = true;
  passthru = { inherit imgui; };
}
