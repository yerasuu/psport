#!/usr/bin/env bash
# tools/bundle.sh PORT TAG DIR: writes DIR/psport-TAG-x86_64.tar.zst and its .sha256, a bundle of
# PORT (nix-build packaging/port.nix, or a directory laid out the same) that runs without Nix: its
# programs, every library they link from the store, and the store's glibc loader, with LICENSE,
# THIRD-PARTY-NOTICES and SOURCES.txt (packaging/licences.nix). psport is GPL-2.0 (kb #843), so
# nothing under the Apache-2.0 goes in: the Vulkan loader is the host's, and the online module
# (libpsport-net.so: OpenSSL, abseil) stays out until kb #850 replaces them; PS_BUNDLE_NET=1 keeps
# it for a bundle that is not redistributed.
# The bundle's programs keep starting as programs, not through `ld.so --library-path`, since a
# loader started as the program puts the brk heap at ~85 TiB, where psport's non-PIE binary can't
# hand heap pointers to the guest (40 bits). Instead each program's PT_INTERP names the bundled
# loader: the bundle's relocate.sh writes its path in place, the first time and whenever the bundle
# moved (run.sh calls it). The programs' RUNPATHs become RPATHs naming the bundle first and the
# host's lib directories after, written in place too (patchelf corrupts the non-PIE binary's symbol
# versions), and the libraries' go: a library without one has its dependencies searched in the
# program's RPATH, and so do the host's Vulkan drivers it loads later.
set -euo pipefail
(($# == 3)) || { sed -n '2,4p' "$0" | cut -c3-; exit 2; }
port=$1 tag=$2 dest=$3
[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Not a vX.Y.Z tag: $tag" >&2; exit 2; }
[[ -f $port/LICENSE && -f $port/run.sh && -x $port/out/psport && -f $port/out/gpu/libpsport-gpu.so ]] ||
    { echo "Not a port build: $port" >&2; exit 2; }
name=psport-$tag-x86_64
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
bundle=$work/$name
mkdir -p "$bundle/lib" "$bundle/out/gpu"
cp -r "$port/LICENSE" "$port/run.sh" "$port/scripts" "$port/patches" "$bundle/"
mkdir -p "$bundle/tools"
cp -- "$port"/tools/{fetch_fsr4_assets.sh,fsr4_optimize.sh,fsr4_post_lds.pl,fsr4_pass11_guard.pl,Fsr4SpirvCrossFixes.pm} \
    "$bundle/tools/"
install -m755 "$port/out/psport" "$port/out/psport-gpu-capabilities" "$bundle/out/"
install -m755 "$port/out/gpu/libpsport-gpu.so" "$bundle/out/gpu/"
is_with_net=false
if [[ -f $port/out/gpu/libpsport-net.so ]]; then
    if [[ ${PS_BUNDLE_NET:-0} == 1 ]]; then
        install -m755 "$port/out/gpu/libpsport-net.so" "$bundle/out/gpu/"
        is_with_net=true
        echo "bundle: online module libpsport-net.so included (PS_BUNDLE_NET=1): it links OpenSSL and abseil (Apache-2.0), so this bundle must not be redistributed"
    else
        echo "bundle: online module libpsport-net.so left out: it links OpenSSL and abseil (Apache-2.0), not redistributable with GPL-2.0 psport until kb #850; PS_BUNDLE_NET=1 keeps it for local use"
    fi
fi
chmod -R u+w "$bundle"
programs=(out/psport out/psport-gpu-capabilities)
modules=()
for module in "$bundle"/out/gpu/*.so; do modules+=("out/gpu/${module##*/}"); done
loader=$(readelf -lW "$bundle/out/psport" | sed -n 's|.*interpreter: \(/nix/store/[^]]*\)\]$|\1|p')
[[ -x $loader && ${loader##*/} == ld-linux-x86-64.so.2 ]] || { echo "No Nix glibc loader: $loader" >&2; exit 1; }
cp -L "$loader" "$bundle/lib/ld-linux-x86-64.so.2"
# What each one links, resolved through its RUNPATHs as it is in the store; the port's own
# libraries ($ORIGIN/gpu) are already there. Each library is listed again on its own: a listing
# shows a file once, under the first name it was loaded by, and another library may link the same
# file by another name. Another name for a file is a link to it. The Vulkan loader is left to the
# RPATH's host directories.
declare -A copied packages
pending=("${programs[@]/#/$bundle/}" "${modules[@]/#/$bundle/}")
while ((${#pending[@]})); do
    listed=()
    for elf in "${pending[@]}"; do
        list=$("$loader" --list "$elf") || { echo "bundle: $loader --list $elf failed" >&2; exit 1; }
        while read -r soname path; do
            [[ -e $bundle/lib/$soname || $soname == libvulkan.so* ]] && continue
            file=$(realpath -- "$path")
            if [[ -n ${copied[$file]:-} ]]; then
                ln -s -- "${copied[$file]}" "$bundle/lib/$soname"
            else
                cp -- "$file" "$bundle/lib/$soname"
                copied[$file]=$soname
                package=${file#/nix/store/}
                packages[/nix/store/${package%%/*}]=1
                listed+=("$path")
            fi
        done < <(awk '$2 == "=>" && $3 ~ /^\/nix\/store\// { print $1, $3 }' <<< "$list")
    done
    pending=("${listed[@]}")
done
chmod -R u+w "$bundle/lib"
[[ -e $bundle/lib/libc.so.6 ]] || { echo "bundle: no libc.so.6 among the libraries the programs link" >&2; exit 1; }
# The nixpkgs the port was built with: packaging/port.nix's pinned one for a store path, shell.nix's
# <nixpkgs> for a checkout built by build.sh.
nixpkgs=()
[[ $(realpath -- "$port") == /nix/store/* ]] || nixpkgs=(--arg pkgs 'import <nixpkgs> { }')
licences=$(nix-build --no-out-link "$(dirname -- "$0")/../packaging/licences.nix" "${nixpkgs[@]}" \
    --argstr storePaths "${!packages[*]}" --arg withNet "$is_with_net")
cp -- "$licences/THIRD-PARTY-NOTICES" "$licences/SOURCES.txt" "$bundle/"
chmod u+w "$bundle/THIRD-PARTY-NOTICES" "$bundle/SOURCES.txt"
# The host's library directories, after the bundle's own: where the host's Vulkan drivers (loaded
# by path from their ICD manifests or, NVIDIA's, by name) find what they link, and where SDL finds
# the windowing and audio libraries it opens by name. NixOS keeps its drivers in /run/opengl-driver.
host=/usr/lib/x86_64-linux-gnu:/usr/lib64:/usr/lib:/lib/x86_64-linux-gnu:/lib64:/lib:/run/opengl-driver/lib
python3 - "$bundle" "$host" "${programs[*]}" "${modules[*]}" > "$work/interp" <<'PY'
import struct, sys
from pathlib import Path

bundle, host = Path(sys.argv[1]), sys.argv[2]
programs, modules = sys.argv[3].split(), sys.argv[4].split()
DT_NEEDED, DT_STRTAB, DT_SONAME, DT_RPATH, DT_RUNPATH = 1, 5, 14, 15, 29
PT_LOAD, PT_DYNAMIC, PT_INTERP = 1, 2, 3
SHT_DYNSYM, SHT_VERDEF, SHT_VERNEED = 11, 0x6ffffffd, 0x6ffffffe

def fail(message):
    sys.exit(f"bundle: {message}")

def rewrite(path, search=None):
    """Makes the RUNPATH (or RPATH) an RPATH naming `search`, in place: the new value must fit
    in the old one's bytes, and no other name in .dynstr may share them. Without `search` the entry
    goes: a library's dependencies are then searched in the RPATH of the programs that load it."""
    data = bytearray(path.read_bytes())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail(f"{path}: not a 64-bit little-endian ELF")
    phoff, shoff = struct.unpack_from("<QQ", data, 0x20)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 0x36)
    headers = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize) for i in range(phnum)]
    loads = [h for h in headers if h[0] == PT_LOAD]
    def offset(vaddr):
        for _, _, off, va, _, filesz, _, _ in loads:
            if va <= vaddr < va + filesz:
                return off + vaddr - va
        fail(f"{path}: address {vaddr:#x} outside the file")
    interp = next(((h[2], h[5]) for h in headers if h[0] == PT_INTERP), None)
    dynamic = next((h for h in headers if h[0] == PT_DYNAMIC), None)
    if dynamic is None:
        return interp
    entries, at = [], dynamic[2]
    while True:
        tag, value = struct.unpack_from("<qQ", data, at)
        entries.append((at, tag, value))
        at += 16
        if tag == 0:
            break
    values = {tag: value for _, tag, value in entries}
    paths = [(at, tag, value) for at, tag, value in entries if tag in (DT_RPATH, DT_RUNPATH)]
    if not paths:
        return interp
    if len(paths) > 1:
        fail(f"{path}: both an RPATH and a RUNPATH")
    entry, _, start = paths[0]
    if search is None:
        kept = [(tag, value) for at, tag, value in entries if at != entry] + [(0, 0)]
        for i, (tag, value) in enumerate(kept):
            struct.pack_into("<qQ", data, dynamic[2] + i * 16, tag, value)
        path.write_bytes(data)
        return interp
    strtab = offset(values[DT_STRTAB])
    end = data.index(b"\0", strtab + start)
    old = data[strtab + start:end].decode()
    new = search.encode()
    if len(new) > end - (strtab + start):
        fail(f"{path}: {search!r} doesn't fit in the RUNPATH's {end - strtab - start} bytes ({old!r})")
    names = [value for _, tag, value in entries if tag in (DT_NEEDED, DT_SONAME)]
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum if shoff else 0)]
    for _, type_, _, _, off, size, _, info, _, entsize in sections:
        if type_ == SHT_DYNSYM:
            names += [struct.unpack_from("<I", data, off + i * entsize)[0] for i in range(1, size // entsize)]
        at, remaining = off, info
        while type_ == SHT_VERNEED and remaining:
            _, count, file, aux, following = struct.unpack_from("<HHIII", data, at)
            names.append(file)
            child = at + aux
            for _ in range(count):
                _, _, _, name, next_aux = struct.unpack_from("<IHHII", data, child)
                names.append(name)
                child += next_aux
            at, remaining = at + following, remaining - 1
        while type_ == SHT_VERDEF and remaining:
            _, _, _, count, _, aux, following = struct.unpack_from("<HHHHIII", data, at)
            child = at + aux
            for _ in range(count):
                name, next_aux = struct.unpack_from("<II", data, child)
                names.append(name)
                child += next_aux
            at, remaining = at + following, remaining - 1
    if any(start < name < start + len(old) + 1 for name in names):
        fail(f"{path}: another name shares the RUNPATH's bytes")
    data[strtab + start:end] = new + b"\0" * (end - strtab - start - len(new))
    struct.pack_into("<q", data, entry, DT_RPATH)
    path.write_bytes(data)
    return interp

for program in programs:
    interp = rewrite(bundle / program, f"$ORIGIN/gpu:$ORIGIN/../lib:{host}")
    if interp is None:
        fail(f"{program}: no PT_INTERP")
    print(program, *interp)
for library in [bundle / module for module in modules] + sorted((bundle / "lib").iterdir()):
    if library.name != "ld-linux-x86-64.so.2" and not library.is_symlink():
        rewrite(library)
PY
{
    cat <<'HEAD'
#!/usr/bin/env bash
# Written by tools/bundle.sh: points the bundle's programs at its glibc loader, lib/, wherever the
# bundle is now (their PT_INTERP, written in place on a copy, so a running program keeps its own).
# A loader path longer than the field Nix left goes through a link in XDG_RUNTIME_DIR, made again
# on every run since that directory is emptied at logout.
set -euo pipefail
export LC_ALL=C
cd -- "$(dirname -- "$0")"
here=$(pwd -P)
loader=$here/lib/ld-linux-x86-64.so.2
HEAD
    read -r _ _ size < "$work/interp"
    printf 'size=%d\n' "$size"
    cat <<'BODY'
interp=$loader
if ((${#interp} >= size)); then
    if [[ -z ${XDG_RUNTIME_DIR:-} || ! -d $XDG_RUNTIME_DIR ]]; then
        echo "relocate.sh: $loader is longer than $((size - 1)) bytes and XDG_RUNTIME_DIR is not set" >&2
        exit 1
    fi
    sum=$(printf '%s' "$here" | cksum)
    interp=$XDG_RUNTIME_DIR/psport-ld-${sum%% *}
    ln -sfn -- "$loader" "$interp"
    if ((${#interp} >= size)); then
        echo "relocate.sh: neither $loader nor $interp fits in $((size - 1)) bytes" >&2
        exit 1
    fi
fi
[[ $(cat lib/interp 2>/dev/null) == "$interp" ]] && exit 0
while read -r program offset field; do
    cp -p -- "$program" "$program.relocating"
    { printf '%s' "$interp"; head -c $((field - ${#interp})) /dev/zero; } |
        dd of="$program.relocating" bs=1 seek=$((offset)) conv=notrunc 2>/dev/null
    mv -f -- "$program.relocating" "$program"
done <<'PROGRAMS'
BODY
    cat "$work/interp"
    echo PROGRAMS
    echo "printf '%s\n' \"\$interp\" > lib/interp"
} > "$bundle/relocate.sh"
chmod 755 "$bundle/relocate.sh"
net=0
[[ $is_with_net == true ]] && net=1
PS_BUNDLE_NET=$net "$(dirname -- "$0")/check_bundle_licences.sh" "$bundle"
mkdir -p "$dest"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@${SOURCE_DATE_EPOCH:-0}" -C "$work" -cf - "$name" |
    zstd -q -19 -T0 > "$dest/$name.tar.zst.partial"
mv -f "$dest/$name.tar.zst.partial" "$dest/$name.tar.zst"
(cd "$dest" && sha256sum "$name.tar.zst" > "$name.tar.zst.sha256")
echo "Bundled $port as $dest/$name.tar.zst ($(du -h "$dest/$name.tar.zst" | cut -f1), $(ls "$bundle/lib" | wc -l) libraries)"
