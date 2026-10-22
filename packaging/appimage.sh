#!/usr/bin/env bash
# Builds dist/Bloodborne-psport-x86_64.AppImage: the port's current build (run build.sh first),
# its scripts and the launcher, bundled with their Nix closure (nix-appimage: the AppImage mounts
# its /nix/store with user namespaces, available on SteamOS and most desktops).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, psport.ini): ~/.local/share/psport (PS_DATA_DIR).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/psport && -f out/gpu/libpsport-gpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
root=$PWD
# The online module stays out unless asked for: default.nix says why.
net=
if [[ -f out/gpu/libpsport-net.so ]]; then
    if [[ ${PS_PACKAGE_NET:-0} == 1 ]]; then
        net=out/gpu/libpsport-net.so
        echo 'appimage: online module libpsport-net.so included (PS_PACKAGE_NET=1): it links OpenSSL and abseil (Apache-2.0), so this AppImage must not be redistributed'
    else
        echo 'appimage: online module libpsport-net.so left out: it links OpenSSL and abseil (Apache-2.0), not redistributable with GPL-2.0 psport until kb #850; PS_PACKAGE_NET=1 keeps it for local use'
    fi
fi
# The libraries' store paths: for each library a binary needs (NEEDED), the RUNPATH directory it
# is found in (their closures come along). Not every RUNPATH entry: built in nix-shell, the
# binaries list the lib directories of the whole build environment (the full GCC, Vulkan headers,
# SPIRV-Tools, ...), hundreds of MB the game never loads.
needed_dirs() {
    local elf=$1 lib dir
    local -a rpath
    mapfile -t rpath < <(readelf -d "$elf" | sed -n 's/.*R\{0,1\}U\{0,1\}N\{0,1\}PATH.*\[\(.*\)\]/\1/p' | tr ':' '\n')
    for lib in $(readelf -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        for dir in "${rpath[@]}"; do
            [[ $dir == /nix/store/* && -e $dir/$lib ]] && { echo "$dir"; break; }
        done
    done
}
{
    echo '['
    for elf in out/psport out/gpu/libpsport-gpu.so $(ls out/libpsport_dlss.so 2>/dev/null) $net; do
        needed_dirs "$elf"
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && nix bundle --impure --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
mkdir -p dist
install -m755 "$work/psport.AppImage" dist/Bloodborne-psport-x86_64.AppImage
# A GC root for the package: nix-collect-garbage keeps it, and with it the outputs built here from
# source (the Vulkan-only Mesa, GTK 4 without GStreamer, libadwaita, SDL3), so the next AppImage
# does not build them again (only a nixpkgs update does).
nix-build --impure packaging -o out/nix-roots/psport > /dev/null
ls -lh dist/Bloodborne-psport-x86_64.AppImage
