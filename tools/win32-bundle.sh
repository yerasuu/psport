#!/usr/bin/env bash
# tools/win32-bundle.sh DEST: writes DEST/psport-windows-VERSION-COMMIT-x86_64.zip and its .sha256, the
# Windows test package (kb #899): psport.exe, libpsport-gpu.dll, psport-gpu-capabilities.exe and
# psport-windows-probe.exe, the CLANG64 DLLs they load (the closure of their PE import tables), the
# testers' scripts and README-WINDOWS.txt (packaging/windows), LICENSE.txt, THIRD-PARTY-NOTICES.txt,
# SOURCES.txt and BUILD-INFO.txt, all in one folder; next to it, DEST/psport-windows-...-sources holds
# the sources of the DLLs built from psport's own PKGBUILDs (packaging/windows/*/PKGBUILD). From MSYS2's
# CLANG64 shell it takes build.sh's out/ and the DLLs and package database of that MSYS2 (the CI job);
# elsewhere tools/win32-cross.sh's out/win32 and the packages of its sysroot (PS_WIN32_SYSROOT).
# psport is GPL-2.0 (kb #843), so only DLLs whose package licence allows that go in: vulkan-1.dll is
# the GPU driver's (and Apache-2.0), the online module stays out, and FFmpeg is psport's own LGPL build
# (packaging/windows/ffmpeg, kb #906), since MSYS2's is GPL-3.0-or-later with x264, x265 and GnuTLS.
# PS_BUNDLE_LOCAL=1 keeps DLLs that are not redistributable with psport, for a package used on one's
# own machine, never redistributed: its name ends in -local and its BUILD-INFO.txt says so.
set -euo pipefail
(($# == 1)) || { sed -n '2,4p' "$0" | cut -c3-; exit 2; }
dest=$(realpath -m -- "$1")
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
MINGW* | MSYS*)
    mode="MSYS2 ${MSYSTEM:-?}" base='' prefix=${MSYSTEM_PREFIX:?run from MSYS2\'s CLANG64 shell} out=out
    ;;
*)
    root=${PS_WIN32_SYSROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/psport/win32-sysroot}
    mode="cross build over the CLANG64 sysroot in $root" base=$root prefix=$root/clang64 out=out/win32
    [[ -s $root/db/index && -s $root/packages.txt ]] || { echo "No sysroot in $root (tools/win32-cross.sh sysroot)" >&2; exit 2; }
    ;;
esac
rel=${prefix#"$base"/} rel=${rel#/}
objdump=$(command -v llvm-objdump || command -v objdump || true)
[[ -n $objdump ]] || { echo 'Need llvm-objdump or objdump (to read the PE import tables)' >&2; exit 2; }
command -v zip > /dev/null || { echo 'Need zip' >&2; exit 2; }
programs=("$out/psport.exe" "$out/gpu/libpsport-gpu.dll" "$out/psport-gpu-capabilities.exe" "$out/psport-windows-probe.exe")
for file in "${programs[@]}"; do [[ -f $file ]] || { echo "Not built: $file" >&2; exit 2; }; done
is_local=false
[[ ${PS_BUNDLE_LOCAL:-0} == 1 ]] && is_local=true
version=$(<VERSION) commit=$(git rev-parse --short=12 HEAD)
name=psport-windows-$version-$commit-x86_64
$is_local && name+=-local
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
bundle=$work/$name own_sources=$work/$name-sources
mkdir -p "$bundle"

# MSYS2's package metadata: the desc file (pacman's format, the same in its local database and in the
# repository's) and the files each package installs, as paths under the root ("clang64/bin/SDL3.dll").
if [[ -z $base ]]; then
    desc_of() { local package; package=$(pacman -Q -- "$1") && echo "/var/lib/pacman/local/${package/ /-}/desc"; }
    files_of() { pacman -Qlq -- "$1" | sed 's|^/||'; }
    owner_of() { pacman -Qqo -- "/$1" 2> /dev/null; }
else
    declare -A tarball owners
    while read -r package file _; do tarball[$package]=$file; done < "$root/db/index"
    desc_of() { echo "$root/db/desc/${tarball[$1]%-any.pkg.tar.zst}/desc"; }
    files_of() {
        local list=$root/db/files/$1.list
        if [[ ! -f $list || $list -ot $root/pkgs/${tarball[$1]} ]]; then
            mkdir -p -- "$root/db/files"
            tar --zstd -tf "$root/pkgs/${tarball[$1]}" > "$list.part"
            sed -i '/^\./d' -- "$list.part"
            mv -- "$list.part" "$list"
        fi
        cat -- "$list"
    }
    owner_of() { [[ -n ${owners[${1,,}]:-} ]] && echo "${owners[${1,,}]}"; }
    # Listing a package's files reads its whole archive: once per package, then from the lists.
    while read -r package; do
        while read -r file; do owners[${file,,}]=$package; done < <(files_of "$package" | grep -i '\.dll$')
    done < "$root/packages.txt"
fi
field() { awk -v key="%$2%" '$0 == key { on = 1; next } on && $0 == "" { exit } on { print }' "$(desc_of "$1")"; }

# Packages built from psport's own PKGBUILDs instead of MSYS2's: their source is that PKGBUILD with
# the upstream files it names, which makepkg leaves next to it, checked against its sums and shipped
# next to the zip.
pkgbuild_value() { (MINGW_PACKAGE_PREFIX=mingw-w64-clang-x86_64 && source "$1" && eval "printf '%s\n' \"\${$2[@]}\""); }
declare -A pkgbuild_of
for pkgbuild in packaging/windows/*/PKGBUILD; do
    [[ -f $pkgbuild ]] && pkgbuild_of[$(pkgbuild_value "$pkgbuild" pkgbase)]=$pkgbuild
done
ship_sources() {
    local pkgbuild=$1 pkgbase=$2 version=$3 dir=${1%/PKGBUILD} entry file i=0
    local target=$own_sources/$pkgbase-$version
    if [[ $(pkgbuild_value "$pkgbuild" pkgver)-$(pkgbuild_value "$pkgbuild" pkgrel) != "$version" ]]; then
        echo "bundle: $pkgbase $version is installed, but $pkgbuild builds another version" >&2; exit 1
    fi
    local -a sums
    mapfile -t sums < <(pkgbuild_value "$pkgbuild" sha256sums)
    mkdir -p -- "$target"
    cp -- "$pkgbuild" "$target/"
    [[ ! -d $dir/keys ]] || cp -r -- "$dir/keys" "$target/"
    while read -r entry; do
        if [[ $entry == *::* ]]; then file=${entry%%::*}; else file=${entry##*/}; fi
        [[ -f $dir/$file ]] || { echo "bundle: $dir/$file, a source $pkgbuild names, is missing (makepkg downloads it there)" >&2; exit 1; }
        if [[ ${sums[i]} != SKIP ]] && ! sha256sum --quiet -c <<< "${sums[i]}  $dir/$file"; then
            echo "bundle: $dir/$file does not match $pkgbuild's sha256sums" >&2; exit 1
        fi
        cp -- "$dir/$file" "$target/"
        i=$((i + 1))
    done < <(pkgbuild_value "$pkgbuild" source)
}

# GPL-2.0-only compatible licences (SPDX). A package's %LICENSE% lines all apply; a line labelled for
# a part (documentation:spdx:...) covers files the package carries besides its DLL, which stay out.
allowed=' MIT BSD-2-Clause BSD-3-Clause Zlib ISC BSL-1.0 FTL ZPL-2.1 Unlicense libpng-2.0 bzip2-1.0.6 '
allowed+='LGPL-2.0-or-later LGPL-2.1-or-later LGPL-2.1-only LGPL-2.1 GPL-2.0-or-later GPL-2.0-only GPL-2.0 '
allowed+='Apache-2.0_WITH_LLVM-exception '
# Where MSYS2's licence field says less than the package's own licence files: bzip2's and libpng's
# "custom" are their own permissive licences; gettext-runtime's GPL-3.0-or-later covers its programs,
# while libintl is LGPL-2.1-or-later (share/licenses/gettext-runtime/intl).
declare -A clarified=([libintl-8.dll]=LGPL-2.1-or-later [bzip2]=bzip2-1.0.6 [libpng]=libpng-2.0)
is_expression_allowed() {
    local expression=${1//Apache-2.0 WITH LLVM-exception/Apache-2.0_WITH_LLVM-exception} alternative term
    if [[ $expression == *'('* ]]; then
        for term in ${expression//[()]/ }; do
            [[ $term == AND || $term == OR || $allowed == *" $term "* ]] || return 1
        done
        return 0
    fi
    while read -r alternative; do
        local is_ok=true
        for term in ${alternative// AND / }; do [[ $allowed == *" $term "* ]] || is_ok=false; done
        $is_ok && return 0
    done < <(sed 's/ OR /\n/g' <<< "$expression")
    return 1
}
licence_of() {
    local package=$1 dll=$2 short=${1#mingw-w64-clang-x86_64-} line
    if [[ -n $dll && -n ${clarified[$dll]:-} ]]; then echo "${clarified[$dll]}"; return; fi
    if [[ -n ${clarified[$short]:-} ]]; then echo "${clarified[$short]}"; return; fi
    local -a parts=()
    while read -r line; do
        case $line in
        spdx:*) parts+=("${line#spdx:}") ;;
        *:spdx:*) ;;
        *) parts+=("unknown ($line)") ;;
        esac
    done < <(field "$package" LICENSE)
    ((${#parts[@]})) || parts=("unknown (none stated)")
    local IFS=';'
    echo "${parts[*]}"
}
is_licence_allowed() {
    local part
    while read -r part; do is_expression_allowed "$part" || return 1; done < <(tr ';' '\n' <<< "$1")
}

imports() { "$objdump" -p "$1" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r'; }
declare -A seen dll_package dll_licence needed_by
included=() left_out=() system=()
pending=("${programs[@]}")
for program in "${programs[@]}"; do
    program=${program##*/}
    seen[${program,,}]=1
done
while ((${#pending[@]})); do
    next=()
    for binary in "${pending[@]}"; do
        while read -r dll; do
            key=${dll,,}
            [[ -n ${needed_by[$key]:-} ]] || needed_by[$key]=${binary##*/}
            [[ -z ${seen[$key]:-} ]] || continue
            seen[$key]=1
            file=$(find "$prefix/bin" -maxdepth 1 -iname "$dll" -print -quit)
            if [[ $key == vulkan-1.dll || -z $file ]]; then
                system+=("$dll")
                continue
            fi
            package=$(owner_of "$rel/bin/${file##*/}") || { echo "bundle: no MSYS2 package owns ${file##*/}" >&2; exit 1; }
            licence=$(licence_of "$package" "$key")
            dll_package[$key]=$package dll_licence[$key]=$licence
            is_licence_allowed "$licence" || left_out+=("${file##*/}")
            if is_licence_allowed "$licence" || $is_local; then
                cp -- "$file" "$bundle/"
                included+=("${file##*/}")
                next+=("$file")
            fi
        done < <(imports "$binary")
    done
    pending=("${next[@]}")
done
for program in "${programs[@]}"; do install -m755 -- "$program" "$bundle/"; done
if [[ -f $out/gpu/libpsport-net.dll ]]; then
    echo "bundle: online module libpsport-net.dll left out: it links OpenSSL and abseil (Apache-2.0), not redistributable with GPL-2.0 psport until kb #850"
fi
if $is_local; then kept='kept (PS_BUNDLE_LOCAL=1: this package must not be redistributed)'; else kept='left out'; fi
for dll in "${left_out[@]}"; do
    key=${dll,,}
    echo "bundle: $dll (${dll_package[$key]#mingw-w64-clang-x86_64-}: ${dll_licence[$key]}, needed by ${needed_by[$key]}) is not redistributable with GPL-2.0 psport: $kept"
done
crlf() { sed 's/\r$//; s/$/\r/' -- "$1" > "$2"; }
for file in packaging/windows/*; do [[ ! -f $file ]] || crlf "$file" "$bundle/${file##*/}"; done
crlf LICENSE "$bundle/LICENSE.txt"

# Notices and sources: one entry per package, with the licence files it installs.
declare -A package_dlls
for dll in "${included[@]}"; do package_dlls[${dll_package[${dll,,}]}]+=" $dll"; done
notices=$bundle/THIRD-PARTY-NOTICES.txt sources=$bundle/SOURCES.txt
rule() { printf "%${1}s\n" '' | tr ' ' "$2"; }
entry() {
    local package=$1 files=$2 licence=$3 short=${1#mingw-w64-clang-x86_64-} pkgbase version url text
    pkgbase=$(field "$package" BASE) version=$(field "$package" VERSION) url=$(field "$package" URL)
    local pkgbuild=${pkgbuild_of[$pkgbase]:-} origin="MSYS2 package $package"
    [[ -z $pkgbuild ]] || origin="package $package, built by psport from $pkgbuild"
    {
        echo; rule 100 -; echo "$short $version ($origin)"
        echo "Licence: $licence"
        [[ -n $files ]] && echo "Files:$files"
        echo "Upstream: $url"
        while read -r text; do
            [[ -f $base/$text ]] || continue
            echo; echo "--- ${text#"$rel"/share/licenses/}"; tr -d '\r' < "$base/$text"
        done < <(files_of "$package" | grep "^$rel/share/licenses/.*[^/]$")
    } >> "$notices"
    local where="https://repo.msys2.org/mingw/sources/$pkgbase-$version.src.tar.zst"
    if [[ -n $pkgbuild ]]; then
        ship_sources "$pkgbuild" "$pkgbase" "$version"
        where="$name-sources/$pkgbase-$version/ next to this package's zip: the PKGBUILD (psport's $pkgbuild at commit $commit) and the upstream files it builds from"
    fi
    printf '%s %s\n  Licence: %s\n  Source: %s\n  Upstream: %s\n' "$short" "$version" "$licence" "$where" "$url" >> "$sources"
}
section() { { echo; rule 100 =; echo "$1"; rule 100 =; } >> "$notices"; }
cat > "$notices" << 'HEAD'
Third-party notices for psport for Windows

psport is licensed under the GNU General Public License version 2 (LICENSE.txt); some of its files
are GPL-2.0-only (marked in their headers), the rest GPL-2.0-or-later used as version 2. It includes
code from bbport and shadPS4 (GPL-2.0-or-later) and from AnyPS5 (GPL-2.0-only). Below, every other
component this package carries: the DLLs next to psport.exe, built in MSYS2's CLANG64 environment
(FFmpeg's by psport itself, as LGPL-2.1-or-later, with the configure line it was built with), and the
code compiled into psport's own programs, each with its licence and the licence files it ships with. Where a component offers a choice of licences, psport uses it under the one compatible with the
GPL version 2. Sources: SOURCES.txt.
HEAD
cat > "$sources" << HEAD
Sources of the components in this package

psport itself (GPL-2.0, see LICENSE.txt) is at https://github.com/yerasuu/psport, commit $commit.

Written offer (GNU GPL version 2, section 3(b); GNU LGPL version 2.1, section 6(c)): for at least
three years from the day this package was published, the psport maintainer will give anyone who asks,
for a charge no more than the cost of physically performing the source distribution, a complete
machine-readable copy of the corresponding source code of psport and of every component listed below,
on a medium customarily used for software interchange. Ask through https://github.com/yerasuu/psport/issues.

Each MSYS2 package below: its version, licence, MSYS2's source package (its PKGBUILD, patches and the
upstream source it was built from) and the upstream project. A package psport builds itself (FFmpeg)
names instead the folder next to this package's zip that holds its PKGBUILD and upstream source; the
same folder is in the CI run's artifacts beside the zip.

HEAD
section 'DLLs next to psport.exe'
for package in $(printf '%s\n' "${!package_dlls[@]}" | sort -f); do
    licences=$(for dll in ${package_dlls[$package]}; do echo "${dll_licence[${dll,,}]}"; done | sort -u | paste -sd'|' | sed 's/|/; /g')
    entry "$package" "${package_dlls[$package]}" "$licences"
done
# What the build links statically or compiles in from CLANG64 (build.sh, gpu/CMakeLists.txt): the
# compiler's runtime and the C runtime's startup code, and the header-only libraries.
section "Packages compiled into psport's programs"
for short in crt compiler-rt boost fmt robin-map vulkan-memory-allocator vulkan-headers; do
    package=mingw-w64-clang-x86_64-$short
    if [[ $short == vulkan-headers ]]; then
        licence='Apache-2.0 OR MIT (every header in include/; psport uses them under the MIT)'
    else
        licence=$(licence_of "$package" '')
    fi
    entry "$package" '' "$licence"
done
# The same components as packaging/licences.nix's vendored list, which the Linux bundle carries.
section "Code in psport's source tree under its own licence"
deps=$out/_deps
[[ -d $deps ]] || deps=$out/gpu/_deps
vendored=(
    "sirit|https://github.com/shadps4-emu/sirit|BSD-3-Clause|gpu/third_party/sirit/LICENSE.txt gpu/third_party/sirit/externals/SPIRV-Headers/LICENSE"
    "AMD GCN headers|https://github.com/GPUOpen-Drivers/pal|MIT|gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h:24"
    "half|https://half.sourceforge.net/|MIT|gpu/third_party/half/LICENSE.txt"
    "Xbyak|https://github.com/herumi/xbyak|BSD-3-Clause|gpu/third_party/xbyak/COPYRIGHT"
    "FSR-Vulkan and AMD FidelityFX SDK|https://github.com/FireBurn/FSR-Vulkan|MIT|gpu/third_party/fsr-vulkan/LICENSE.txt gpu/third_party/fsr-vulkan/NOTICE.md gpu/third_party/fsr-vulkan/upstream/ffx-1.1.4/sdk/LICENSE.txt"
    "LibAtrac9|https://github.com/Thealexbarney/LibAtrac9|MIT|third_party/LibAtrac9/LICENSE"
    "Dear ImGui|https://github.com/ocornut/imgui|MIT|gpu/third_party/imgui/LICENSE.txt"
    "RenderDoc in-application API header|https://github.com/baldurk/renderdoc|MIT|gpu/third_party/renderdoc/renderdoc_app.h:24"
    "DejaVu fonts|https://dejavu-fonts.github.io/|Bitstream-Vera|gpu/third_party/fonts/LICENSE-DejaVu.txt"
    "Files in the shadPS4 tree under their own licence|https://github.com/shadps4-emu/shadPS4|MIT, BSD-3-Clause AND GPL-2.0-or-later, Zlib, CC0-1.0|gpu/shadps4/video_core/host_shaders/fsr.comp:2 gpu/shadps4/video_core/host_shaders/fsr/ffx_a.h:41 gpu/shadps4/video_core/host_shaders/fsr/ffx_fsr1.h:30 gpu/shadps4/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp:2 gpu/shadps4/common/bit_field.h:3 gpu/shadps4/video_core/renderer_vulkan/vk_barrier_tracker.h:2 gpu/shadps4/common/alignment.h:2"
    "magic_enum (fetched by gpu/CMakeLists.txt)|https://github.com/Neargye/magic_enum|MIT|$deps/magic_enum-src/LICENSE"
    "miniz (fetched by gpu/CMakeLists.txt)|https://github.com/richgel999/miniz|MIT|$deps/miniz-src/LICENSE"
)
for item in "${vendored[@]}"; do
    IFS='|' read -r title url licence files <<< "$item"
    {
        echo; rule 100 -; echo "$title"; echo "Licence: $licence"; echo "Upstream: $url"
        for file in $files; do
            lines=${file##*:} file=${file%:*}
            [[ $lines == "$file" ]] && lines=
            if [[ ! -f $file ]]; then
                echo "bundle: $file missing for the notices" >&2; exit 1
            fi
            echo; echo "--- $file"
            if [[ -n $lines ]]; then head -n "$lines" -- "$file"; else cat -- "$file"; fi | tr -d '\r'
        done
    } >> "$notices"
done
for file in "$notices" "$sources"; do sed -i 's/$/\r/' -- "$file"; done

{
    echo "psport for Windows, test package"
    echo "Version $version, commit $commit, built $(date -u +%Y-%m-%dT%H:%M:%SZ) by tools/win32-bundle.sh ($mode)"
    if $is_local; then
        echo
        echo "LOCAL USE ONLY: built with PS_BUNDLE_LOCAL=1, it carries DLLs whose licences GPL-2.0 psport"
        echo "cannot be redistributed with (listed below). Do not share this package."
    fi
    echo
    echo "Programs: ${programs[*]##*/}"
    echo "DLLs included (${#included[@]}):"
    for dll in "${included[@]}"; do echo "  $dll (${dll_package[${dll,,}]#mingw-w64-clang-x86_64-}: ${dll_licence[${dll,,}]})"; done
    echo "DLLs not redistributable with GPL-2.0 psport (${#left_out[@]}), $kept:"
    for dll in "${left_out[@]}"; do echo "  $dll (${dll_package[${dll,,}]#mingw-w64-clang-x86_64-}: ${dll_licence[${dll,,}]}), needed by ${needed_by[${dll,,}]}"; done
    ((${#left_out[@]})) && ! $is_local &&
        echo "  Without them psport.exe does not start (exit code 0xc0000135); psport-windows-probe.exe needs none."
    echo "DLLs Windows provides (vulkan-1.dll comes with the GPU driver):"
    printf '%s\n' "${system[@]}" | sort -fu | paste -sd' ' | fold -s -w 96 | sed 's/^/  /'
} | sed 's/$/\r/' > "$bundle/BUILD-INFO.txt"

mkdir -p -- "$dest"
rm -f -- "$dest/$name.zip.partial"
(cd "$work" && zip -q -r -X "$dest/$name.zip.partial" "$name")
mv -f -- "$dest/$name.zip.partial" "$dest/$name.zip"
(cd "$dest" && sha256sum "$name.zip" > "$name.zip.sha256")
rm -rf -- "$dest/$name-sources"
if [[ -d $own_sources ]]; then
    mv -- "$own_sources" "$dest/$name-sources"
    echo "Sources of the DLLs built here: $dest/$name-sources"
fi
echo "Bundled $dest/$name.zip ($(du -h "$dest/$name.zip" | cut -f1), ${#included[@]} DLLs; ${#left_out[@]} not redistributable, $kept)"
