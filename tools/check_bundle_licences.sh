#!/usr/bin/env bash
# tools/check_bundle_licences.sh BUNDLE | --port PORT: fails when a bundle (tools/bundle.sh's
# folder or its .tar.zst) carries, anywhere in its tree, what GPL-2.0 psport can't redistribute (kb
# #843): OpenSSL, abseil, the Vulkan loader (Apache-2.0), x264, x265, xvidcore (GPL encoders FFmpeg
# only links when built with gpl), libssh, SRT, GnuTLS, RIST (what brings OpenSSL or LGPL-3 in), an
# FFmpeg built with version3 or whose licence can't be read, libunistring or another GPL-3.0-only
# library (THIRD-PARTY-NOTICES: nixpkgs lists the licences of every part of a source, so a library
# counts only when that is all it lists or an AND names it), protobuf (which brings abseil), or the
# online module, not released yet (kb #850); or when it lacks LICENSE, LICENSES.md, LICENSES.json,
# LICENSES/, THIRD-PARTY-NOTICES or SOURCES.txt, or the notices of an entry of
# packaging/vendored.txt (the list the Windows package's notices take too). PS_BUNDLE_NET=1 (a bundle kept for local use)
# allows the online module, and nothing else: since kb #850 it links mbedTLS instead of OpenSSL,
# abseil and protobuf.
# --port checks a port build in the store (packaging/port.nix with withNet = false, what the public
# binary cache publishes) the same way: the build and every library its programs and modules link,
# as they resolve in the store, except nixpkgs' Vulkan loader, which a Nix build links by store path
# since it can't take the host's. Not the rest of its closure: nixpkgs' SDL3 brings PipeWire's whole
# package, whose own plugins and tools link OpenSSL, abseil and a GPL FFmpeg that psport doesn't.
# The cache carries no notices: the bundle made from the same build is checked for them.
set -euo pipefail
is_port=false
if [[ ${1:-} == --port ]]; then is_port=true; shift; fi
(($# == 1)) || { sed -n '2,19p' "$0" | cut -c3-; exit 2; }
bundle=$1
forbidden=('libx264.so*' 'libx265.so*' 'libxvidcore.so*' 'libssh.so*' 'libsrt.so*' 'libgnutls*.so*' 'librist.so*'
    'libunistring.so*' 'libssl.so*' 'libcrypto.so*' 'libabsl_*.so*' 'libprotobuf*.so*' 'libutf8_validity.so*')
if $is_port; then
    bundle=${bundle%/}
    [[ -f $bundle/LICENSE && -x $bundle/out/psport ]] || { echo "check_bundle_licences: not a port build: $1" >&2; exit 2; }
    loader=$(readelf -lW "$bundle/out/psport" | sed -n 's|.*interpreter: \(/nix/store/[^]]*\)\]$|\1|p')
    [[ -x $loader ]] || { echo "check_bundle_licences: no Nix glibc loader in $1/out/psport" >&2; exit 2; }
    declare -A seen
    libraries=()
    pending=("$bundle"/out/psport "$bundle"/out/psport-gpu-capabilities "$bundle"/out/gpu/*.so)
    while ((${#pending[@]})); do
        listed=()
        for elf in "${pending[@]}"; do
            list=$("$loader" --list "$elf") || { echo "check_bundle_licences: $loader --list $elf failed" >&2; exit 2; }
            while read -r path; do
                file=$(realpath -- "$path")
                [[ -n ${seen[$file]:-} ]] && continue
                seen[$file]=1
                libraries+=("$path")
                listed+=("$file")
            done < <(awk '$2 == "=>" && $3 ~ /^\/nix\/store\// { print $3 }' <<< "$list")
        done
        pending=("${listed[@]}")
    done
    roots=("$bundle" "${libraries[@]}")
    required=(LICENSE LICENSES.md LICENSES.json)
else
    if [[ -f $bundle ]]; then
        work=$(mktemp -d)
        trap 'rm -rf "$work"' EXIT
        tar --zstd -xf "$bundle" -C "$work"
        bundle=$(echo "$work"/*/)
    fi
    bundle=${bundle%/}
    [[ -d $bundle/lib ]] || { echo "check_bundle_licences: no lib/ in $1" >&2; exit 2; }
    roots=("$bundle")
    libraries=("$bundle/lib")
    required=(LICENSE LICENSES.md LICENSES.json THIRD-PARTY-NOTICES SOURCES.txt)
    forbidden+=('libvulkan.so*')
fi
problems=()
for file in "${required[@]}"; do
    [[ -s $bundle/$file ]] || problems+=("no $file")
done
[[ -s $bundle/LICENSES/GPL-2.0-only.txt ]] || problems+=("no LICENSES/")
[[ ${PS_BUNDLE_NET:-0} == 1 ]] || forbidden+=(libpsport-net.so)
names=()
for pattern in "${forbidden[@]}"; do names+=(-o -name "$pattern"); done
while IFS= read -r -d '' file; do
    problems+=("${file#"$bundle"/}")
done < <(find "${roots[@]}" \( -type f -o -type l \) \( "${names[@]:1}" \) -print0)
avutil=$(find "${libraries[@]}" \( -type f -o -type l \) -name 'libavutil.so*' -print -quit)
if [[ -z $avutil ]]; then
    problems+=("no libavutil.so*, so FFmpeg's licence can't be checked")
else
    licence=$(grep -a -o -m1 'libavutil license: [^[:cntrl:]]*' "$avutil" || true)
    if [[ -z $licence ]]; then
        problems+=("${avutil#"$bundle"/}: no 'libavutil license:' string")
    elif [[ $licence == *'version 3'* ]]; then
        problems+=("${avutil#"$bundle"/}: ${licence#libavutil license: }")
    fi
fi
if [[ -s $bundle/THIRD-PARTY-NOTICES ]]; then
    carried=''
    [[ ${PS_BUNDLE_NET:-0} == 1 ]] && carried=net
    while IFS='|' read -r name _ _ _ when; do
        [[ -n $name && ( -z $when || $when == "$carried" ) ]] || continue
        grep -qxF -- "$name" "$bundle/THIRD-PARTY-NOTICES" ||
            problems+=("THIRD-PARTY-NOTICES lacks $name (packaging/vendored.txt)")
    done < <(grep -v '^#' "$(dirname -- "$0")/../packaging/vendored.txt")
    while IFS= read -r library; do
        problems+=("lib/ holds $library")
    done < <(awk '
        length($0) == 100 && /^=+$/ { getline title; is_lib = title == "Libraries (lib/)"; getline; next }
        is_lib && length($0) == 100 && /^-+$/ {
            getline name; getline licence
            if (licence !~ /^Licence: /) next
            n = split(substr(licence, 10), parts, "; ")
            is_only_gpl3 = 1
            for (i = 1; i <= n; i++) {
                if (parts[i] ~ / AND / && parts[i] ~ /GPL-3\.0-only/) { is_only_gpl3 = 1; break }
                if (parts[i] !~ /^L?GPL-3\.0-only( |$)/) is_only_gpl3 = 0
            }
            if (is_only_gpl3) print name " (" substr(licence, 10) ")"
        }' "$bundle/THIRD-PARTY-NOTICES")
fi
if ((${#problems[@]})); then
    printf 'check_bundle_licences: %s is not redistributable with GPL-2.0 psport:\n' "$1" >&2
    printf '  %s\n' "${problems[@]}" >&2
    exit 1
fi
if $is_port; then
    echo "check_bundle_licences: $1 is clean (${#libraries[@]} libraries linked)"
else
    echo "check_bundle_licences: $1 is clean ($(find "$bundle/lib" -mindepth 1 | wc -l) libraries)"
fi
