# Build environment: `nix-shell native_probe/shell.nix --run 'bash native_probe/run.sh'`
{ pkgs ? import <nixpkgs> {} }:
let
  # libdecor (window frames on GNOME's Wayland) without its GTK 3 plugin (GTK 3, tinysparql, ICU:
  # ~100 MB); its cairo plugin draws the frames.
  libdecor = pkgs.libdecor.overrideAttrs (old: {
    buildInputs = pkgs.lib.remove pkgs.gtk3 old.buildInputs;
    mesonFlags = old.mesonFlags ++ [ "-Dgtk=disabled" ];
  });
  # SDL3 names zenity (its message boxes) by store path, and zenity brings a whole GTK4 with
  # GStreamer into the AppImage (~200 MB). This one runs the host's zenity when there is one.
  sdl3 = pkgs.sdl3.override {
    inherit libdecor;
    zenity = pkgs.writeShellScriptBin "zenity" ''
      host=$(command -v zenity) && exec "$host" "$@"
      exit 1
    '';
  };
  # LGPL FFmpeg (no gpl, no version3) since psport's binary is GPL-2.0 (kb #843). psport only
  # decodes local files with FFmpeg's own decoders, so it loses nothing without the GPL encoders
  # (x264, x265, xvid), the network protocols and the TLS, SSH, SRT and RIST libraries that brought
  # OpenSSL into the process, teletext, Blu-ray, OpenCL, subtitle rendering and V4L2, nor without
  # Vulkan hwaccel, for which nixpkgs adds an unversioned libvulkan.so to libavcodec's DT_NEEDED:
  # a host without Vulkan's development files has only libvulkan.so.1, and the bundle leaves the
  # Vulkan loader to the host.
  ffmpeg = pkgs.ffmpeg-headless.override {
    withGPL = false;
    withVersion3 = false;
    withNetwork = false;
    withGnutls = false;
    withSsh = false;
    withSrt = false;
    withRist = false;
    withVulkan = false;
    withZvbi = false;
    withBluray = false;
    withOpencl = false;
    withAss = false;
    withV4l2 = false;
  };
in
pkgs.mkShell {
  packages = with pkgs; [
    gcc gnumake cmake ninja pkg-config python3 binutils ccache
    vulkan-headers vulkan-loader sdl3
    # GPU library (gpu/): shadPS4 video core dependencies
    ffmpeg boost fmt magic-enum robin-map xxhash vulkan-memory-allocator glslang spirv-cross zydis spirv-headers miniz libx11 libxcb xorgproto wayland
    # libSceFont (gpu/font): glyphs rasterized by FreeType
    freetype
    # Online module (gpu/net): shadNet client, HTTPS, UPnP
    protobuf nlohmann_json openssl zlib miniupnpc
  ];
  # The port built here links Nix's Vulkan loader, which cannot load the host's Mesa drivers:
  # it runs on Nix's own, set outright since a value inherited from the host would name drivers
  # it can't load.
  shellHook = ''
    export VK_LOADER_LAYERS_DISABLE=VK_LAYER_MESA_device_select
    export VK_DRIVER_FILES=${pkgs.mesa}/share/vulkan/icd.d/radeon_icd.x86_64.json:${pkgs.mesa}/share/vulkan/icd.d/intel_icd.x86_64.json
  '';
}
