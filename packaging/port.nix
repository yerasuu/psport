# The port built from source in Nix's sandbox by build.sh, with shell.nix's dependencies: its output
# is laid out as a checkout after build.sh (run.sh, scripts/, patches/, out/psport,
# out/bb-gpu-capabilities, out/gpu/), so run.sh runs from it with PS_PREBUILT=1 and
# PS_PROBE=<output>/out/psport. CI publishes it to the binary cache (tools/nix_cache.sh).
# `nix-build packaging/port.nix`
{ pkgs ? import ./nixpkgs.nix }:
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
  version = "0-unstable";
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
    PS_IN_NIX_SHELL=1 PS_ORIGIN_RPATH_ONLY=1 CCACHE_DISABLE=1 bash build.sh
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out/out/gpu
    cp -r run.sh scripts patches $out/
    install -m755 out/psport out/bb-gpu-capabilities $out/out/
    install -m755 out/gpu/libpsport-gpu.so $out/out/gpu/
    if [ -f out/gpu/libpsport-net.so ]; then install -m755 out/gpu/libpsport-net.so $out/out/gpu/; fi
    runHook postInstall
  '';
  # Debug info names the build environment's headers, which Nix would keep as references.
  stripDebugList = [ "out" ];
  # patchelf corrupts the non-PIE game binary's symbol versions (see default.nix).
  dontPatchELF = true;
}
