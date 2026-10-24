#!/usr/bin/env bash
# tools/win32-toolchain.sh DEST N: writes DEST/psport-toolchain-win32-N-x86_64.zip, its .zip.sha256 and
# DEST/psport-toolchain-win32-N-x86_64.json (the zip's manifest.json): the Windows toolchain package
# the tag toolchain-win32-N publishes (kb #926, docs/en/WINDOWS_TOOLCHAIN.md), with which decanter
# builds a psport branch on Windows. From tools/win32-cross.sh's sysroot (PS_WIN32_SYSROOT, psport's
# FFmpeg installed by its ffmpeg or ffmpeg-package step and its source tarball next to the PKGBUILD).
# llvm-mingw, CMake and Ninja stay out: manifest.json pins them for the caller to download upstream.
set -euo pipefail
(($# == 2)) || { sed -n '2,7p' "$0" | cut -c3-; exit 2; }
dest=$(realpath -m -- "$1") number=$2
[[ $number =~ ^[1-9][0-9]*$ ]] || { echo "Not a toolchain number (1, 2, ...): $number" >&2; exit 2; }
cd -- "$(dirname -- "$0")/.."
root=${PS_WIN32_SYSROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/psport/win32-sysroot}
[[ -s $root/db/index && -s $root/packages.txt && -s $root/db/signer ]] ||
    { echo "No sysroot with a checked database in $root (tools/win32-cross.sh sysroot)" >&2; exit 2; }
for tool in curl jq zip zstd sha256sum; do
    command -v "$tool" > /dev/null || { echo "Need $tool" >&2; exit 2; }
done

# CLANG64's compiler-rt and libc++ headers come from LLVM 23, as llvm-mingw's clang does.
llvm=23
# name|version|URL|sha256|bytes|folder inside the zip|release page
tools=(
    "llvm-mingw|20261020|https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip|fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd|190612575|llvm-mingw-20261020-ucrt-x86_64|https://github.com/mstorsjo/llvm-mingw/releases/tag/20261020"
    "cmake|4.4.4|https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip|bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536|54400562|cmake-4.4.4-windows-x86_64|https://cmake.org/download/"
    "ninja|1.13.2|https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip|07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65|291570||https://github.com/ninja-build/ninja/releases/tag/v1.13.2"
)
# What gpu/CMakeLists.txt fetches by GIT_TAG, as that tag's source archive: name|tag|URL|sha256|licence
vendored=(
    "magic_enum|v0.9.7|https://github.com/Neargye/magic_enum/archive/refs/tags/v0.9.7.tar.gz|b403d3dad4ef542fdc3024fa37d3a6cedb4ad33c72e31b6d9bab89dcaf69edf7|MIT"
    "miniz|3.1.2|https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz|98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb|MIT"
)

name=psport-toolchain-win32-$number-x86_64
commit=$(git rev-parse HEAD)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
package=$work/$name
mkdir -p -- "$package/sources" "$work/json"

field() { awk -v key="%$2%" '$0 == key { on = 1; next } on && $0 == "" { exit } on { print }' "$1"; }
fetch() {
    local url=$1 sha=$2 file=$3
    if [[ ! -f $file ]] || ! sha256sum --quiet -c <<< "$sha  $file" 2> /dev/null; then
        curl -fsSL --retry 3 -o "$file.part" "$url"
        sha256sum --quiet -c <<< "$sha  $file.part" || { echo "Checksum mismatch: $url" >&2; exit 1; }
        mv -- "$file.part" "$file"
    fi
}

# Each package again from its archive, checked against the SHA-256 the sysroot's database gave it, so
# that the package holds what the manifest lists and nothing an earlier sysroot left behind.
declare -A archive sha
while read -r package_name file sum _; do archive[$package_name]=$file sha[$package_name]=$sum; done < "$root/db/index"
# As tools/win32-bundle.sh reads them.
pkgbuild_value() { (MINGW_PACKAGE_PREFIX=mingw-w64-clang-x86_64 && source "$1" && eval "printf '%s\n' \"\${$2[@]}\""); }
declare -A pkgbuild_of own
for pkgbuild in packaging/windows/*/PKGBUILD; do pkgbuild_of[$(pkgbuild_value "$pkgbuild" pkgbase)]=$pkgbuild; done
while read -r package_name; do
    file=${archive[$package_name]:?no archive for $package_name in $root/db/index}
    sha256sum --quiet -c <<< "${sha[$package_name]}  $root/pkgs/$file" || { echo "Checksum mismatch: $root/pkgs/$file" >&2; exit 1; }
    tar --zstd -xf "$root/pkgs/$file" -C "$package" --exclude='.PKGINFO' --exclude='.BUILDINFO' --exclude='.MTREE' --exclude='.INSTALL'
    desc=$root/db/desc/${file%-any.pkg.tar.zst}/desc
    version=$(field "$desc" VERSION) pkgbase=$(field "$desc" BASE)
    licence=$(field "$desc" LICENSE | paste -sd ';' | sed 's/;/; /g')
    pkgbuild=${pkgbuild_of[$pkgbase]:-}
    if [[ -n $pkgbuild ]]; then
        url='' source_ref=sources/$pkgbase-$version origin="psport's $pkgbuild"
        own[$pkgbase]=$version
    else
        url=https://repo.msys2.org/mingw/clang64/$file
        source_ref=https://repo.msys2.org/mingw/sources/$pkgbase-$version.src.tar.zst origin=MSYS2
    fi
    jq -n --arg name "$package_name" --arg version "$version" --arg file "$file" --arg sha256 "${sha[$package_name]}" \
        --arg licence "$licence" --arg url "$url" --arg source "$source_ref" --arg origin "$origin" \
        --arg upstream "$(field "$desc" URL)" \
        '{name: $name, version: $version, file: $file, sha256: $sha256, url: (if $url == "" then null else $url end),
          origin: $origin, licence: $licence, source: $source, upstream: $upstream}' >> "$work/json/packages"
    printf '%s %s\n  Licence: %s\n  Source: %s\n' "${package_name#mingw-w64-clang-x86_64-}" "$version" "$licence" "$source_ref" >> "$work/sources"
done < <(LC_ALL=C sort "$root/packages.txt")
compiler_rt=$package/clang64/lib/clang/$llvm/lib/windows/libclang_rt.builtins-x86_64.a
[[ -f $compiler_rt ]] || { echo "CLANG64's compiler-rt is not LLVM $llvm's (${compiler_rt#"$package"/})" >&2; exit 1; }

# psport's own PKGBUILDs (FFmpeg): the PKGBUILD, its keys and the upstream files it names, checked
# against its sums, since no MSYS2 mirror has their sources.
for pkgbase in "${!own[@]}"; do
    pkgbuild=${pkgbuild_of[$pkgbase]} dir=${pkgbuild_of[$pkgbase]%/PKGBUILD} version=${own[$pkgbase]}
    if [[ $(pkgbuild_value "$pkgbuild" pkgver)-$(pkgbuild_value "$pkgbuild" pkgrel) != "$version" ]]; then
        echo "$pkgbase $version is in the sysroot, but $pkgbuild builds another version" >&2; exit 1
    fi
    target=$package/sources/$pkgbase-$version
    mkdir -p -- "$target"
    cp -- "$pkgbuild" "$target/"
    [[ ! -d $dir/keys ]] || cp -r -- "$dir/keys" "$target/"
    licence=$(pkgbuild_value "$pkgbuild" license | paste -sd ';' | sed 's/;/; /g')
    mapfile -t sums < <(pkgbuild_value "$pkgbuild" sha256sums)
    mapfile -t entries < <(pkgbuild_value "$pkgbuild" source)
    for i in "${!entries[@]}"; do
        if [[ ${entries[i]} == *::* ]]; then file=${entries[i]%%::*} url=${entries[i]#*::}; else file=${entries[i]##*/} url=${entries[i]}; fi
        [[ -f $dir/$file ]] || { echo "$dir/$file, a source $pkgbuild names, is missing (makepkg downloads it there)" >&2; exit 1; }
        if [[ ${sums[i]} != SKIP ]] && ! sha256sum --quiet -c <<< "${sums[i]}  $dir/$file"; then
            echo "$dir/$file does not match $pkgbuild's sha256sums" >&2; exit 1
        fi
        cp -- "$dir/$file" "$target/"
        if [[ ${sums[i]} != SKIP ]]; then
            jq -n --arg name "$pkgbase" --arg version "$version" --arg url "$url" --arg sha256 "${sums[i]}" \
                --arg licence "$licence" --arg path "sources/$pkgbase-$version/$file" \
                '{name: $name, version: $version, url: $url, sha256: $sha256, licence: $licence, path: $path}' >> "$work/json/sources"
        fi
    done
done

# FetchContent's sources, which the toolchain file hands to gpu/CMakeLists.txt so that the build needs
# no network.
for item in "${vendored[@]}"; do
    IFS='|' read -r source_name tag url sum licence <<< "$item"
    if ! grep -q "FetchContent_Declare($source_name GIT_REPOSITORY .* GIT_TAG $tag)" gpu/CMakeLists.txt; then
        echo "gpu/CMakeLists.txt no longer fetches $source_name $tag: update tools/win32-toolchain.sh" >&2; exit 1
    fi
    mkdir -p -- "$root/sources" "$package/sources/$source_name"
    fetch "$url" "$sum" "$root/sources/$source_name-$tag.tar.gz"
    tar -xzf "$root/sources/$source_name-$tag.tar.gz" -C "$package/sources/$source_name" --strip-components=1
    jq -n --arg name "$source_name" --arg version "$tag" --arg url "$url" --arg sha256 "$sum" --arg licence "$licence" \
        --arg path "sources/$source_name" \
        '{name: $name, version: $version, url: $url, sha256: $sha256, licence: $licence, path: $path}' >> "$work/json/sources"
done

cp -- packaging/windows/toolchain/psport-toolchain.cmake "$package/"
sed 's/$/\r/' LICENSE > "$package/LICENSE.txt"
for item in "${tools[@]}"; do
    IFS='|' read -r tool_name version url sum bytes folder page <<< "$item"
    jq -n --arg name "$tool_name" --arg version "$version" --arg url "$url" --arg sha256 "$sum" --argjson size "$bytes" \
        --arg folder "$folder" --arg page "$page" \
        '{name: $name, version: $version, url: $url, sha256: $sha256, size: $size, folder: $folder, page: $page}' >> "$work/json/tools"
done
jq -n --argjson number "$number" --arg name "$name" --arg commit "$commit" --argjson llvm "$llvm" \
    --arg database "$(sha256sum < "$root/db/clang64.db" | cut -d' ' -f1)" --arg signer "$(<"$root/db/signer")" \
    --slurpfile tools "$work/json/tools" --slurpfile packages "$work/json/packages" --slurpfile sources "$work/json/sources" \
    '{schema: 1, name: $name, tag: ("toolchain-win32-" + ($number | tostring)), number: $number, commit: $commit,
      target: "x86_64-w64-windows-gnu", llvm_major: $llvm, toolchain_file: "psport-toolchain.cmake",
      sysroot: "clang64", msys2_database: {file: "clang64.db", sha256: $database, signer: $signer},
      tools: $tools, packages: $packages, sources: $sources}' > "$package/manifest.json"

{
    echo "Sources of the components in psport's Windows toolchain package $name"
    echo
    echo "Built by tools/win32-toolchain.sh from https://github.com/yerasuu/psport, commit $commit;"
    echo "psport's files in it (psport-toolchain.cmake, the PKGBUILDs under sources/) are GPL-2.0 (LICENSE.txt)."
    echo "Each package's licence files are in clang64/share/licenses/; manifest.json lists every package"
    echo "with its archive's SHA-256."
    echo
    echo "Written offer (GNU GPL version 2, section 3(b); GNU LGPL version 2.1, section 6(c)): for at least"
    echo "three years from the day this package was published, the psport maintainer will give anyone who asks,"
    echo "for a charge no more than the cost of physically performing the source distribution, a complete"
    echo "machine-readable copy of the corresponding source code of every component listed below, on a medium"
    echo "customarily used for software interchange. Ask through https://github.com/yerasuu/psport/issues."
    echo
    echo "Each MSYS2 package's source is MSYS2's source package (its PKGBUILD, patches and upstream source);"
    echo "a package psport builds itself names its folder under sources/ instead, which holds its PKGBUILD"
    echo "and the upstream files it builds from. magic_enum and miniz are under sources/ as their release tags."
    echo
    cat -- "$work/sources"
} | sed 's/$/\r/' > "$package/SOURCES.txt"

# What decanter's extractor refuses (decision #924): links, names Windows reserves or can't hold, and
# names only case tells apart.
if [[ -n $(find "$package" -type l -print -quit) ]]; then echo "Links in the package:" >&2; find "$package" -type l >&2; exit 1; fi
bad=$(cd -- "$work" && find "$name" -print | grep -E -i '(^|/)(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.[^/]*)?$|[<>:"\\|?*]|[ .]$' || true)
[[ -z $bad ]] || { echo "Names Windows cannot hold:" >&2; echo "$bad" >&2; exit 1; }
clash=$(cd -- "$work" && find "$name" -print | tr '[:upper:]' '[:lower:]' | LC_ALL=C sort | uniq -d)
[[ -z $clash ]] || { echo "Names only case tells apart:" >&2; echo "$clash" >&2; exit 1; }

# Fixed times and order: the same sysroot, FFmpeg package, commit and zip program give the same zip.
# Only the same FFmpeg package does, since an FFmpeg built again (makepkg-mingw) differs in its bytes.
epoch=$(git log -1 --format=%ct)
find "$package" -exec touch -h -d "@$epoch" -- {} +
mkdir -p -- "$dest"
rm -f -- "$dest/$name.zip.partial"
(cd -- "$work" && find "$name" -print | LC_ALL=C sort | TZ=UTC zip -q -X -@ "$dest/$name.zip.partial")
mv -f -- "$dest/$name.zip.partial" "$dest/$name.zip"
cp -- "$package/manifest.json" "$dest/$name.json"
(cd -- "$dest" && sha256sum -- "$name.zip" > "$name.zip.sha256")
file_count=$(find "$package" -type f | wc -l)
longest=$(cd -- "$work" && find "$name" -print | awk '{ if (length($0) > n) n = length($0) } END { print n }')
echo "Packaged $dest/$name.zip: $(stat -c %s "$dest/$name.zip") bytes, $(du -sb "$package" | cut -f1) bytes unpacked," \
    "$file_count files, longest path $longest characters"
