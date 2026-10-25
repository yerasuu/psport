#!/usr/bin/env bash
# Windows build from Linux (#777), to check locally what the CI's MSYS2 CLANG64 job builds (decision
# #623): nixpkgs' clang and lld 23, the version CLANG64 ships, aimed at x86_64-w64-windows-gnu
# (llvm-mingw's way: libc++, compiler-rt, libunwind, UCRT, winpthreads) over a sysroot unpacked from
# CLANG64's own packages, the ones .github/workflows/windows.yml installs with their dependencies, so
# that both link the same headers and DLLs.
#
#   tools/win32-cross.sh sysroot   downloads and unpacks the packages into $PS_WIN32_SYSROOT
#   tools/win32-cross.sh ffmpeg    builds psport's LGPL FFmpeg (packaging/windows/ffmpeg/PKGBUILD) into it,
#                                  as the CI job does with makepkg; again after every sysroot
#   tools/win32-cross.sh ffmpeg-package FILE (a path from the checkout)
#                                  installs that FFmpeg as makepkg-mingw built it instead (the CI's)
#   tools/win32-cross.sh build     builds libpsport-gpu.dll, psport-gpu-capabilities.exe and
#                                  psport-windows-probe.exe in out/win32
#   tools/win32-cross.sh runtime   then psport.exe; when it fails, lists the files that do not compile
#                                  and the symbols missing at the link
#
# PS_WIN32_SYSROOT: where the packages go (default ~/.cache/psport/win32-sysroot, about 1 GB), outside
# the checkout. PS_LTO=ON for link-time optimisation (off by default, as in the CI job).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=${PS_WIN32_SYSROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/psport/win32-sysroot}
repo=https://repo.msys2.org/mingw/clang64
prefix=mingw-w64-clang-x86_64-
llvm=23
# MSYS2's master keys, as msys2-keyring 1~20260814 lists them in msys2-trusted (the same in
# https://github.com/msys2/MSYS2-keyring at 5d249077a0cf).
masters=(D55E7A6D7CE9BA1587C0ACACF40D263ECA25678A B91BCF3303284BF90CC043CA9F418C233E652008
    9DD0D4217D75A33B896159E6DA7EF2ABAEEA755C 6E8FEAFF9644F54EED90EEA0790AE56A1D3CFDDC
    69985C5EB351011C78DF7F6D755B8182ACD22879)

# The database's signature must come from a master key or a key one of them certified, as pacman's
# web of trust has it, so that its SHA-256s (which the toolchain package's manifest repeats) rest on
# those keys and not only on TLS to repo.msys2.org. The keyring package only supplies the keys: an
# altered one fails the check. Writes the signing key's fingerprint to db/signer.
verify_database() (
    local keys=$root/db/keyring signer master issuer file sum
    rm -rf -- "$keys" && mkdir -p -- "$keys/desc"
    curl -fsSL --retry 3 -o "$keys/msys.db" https://repo.msys2.org/msys/x86_64/msys.db
    tar -xf "$keys/msys.db" -C "$keys/desc" --wildcards 'msys2-keyring-*/desc'
    file=$(awk '/^%FILENAME%$/ { getline; print }' "$keys"/desc/msys2-keyring-*/desc)
    sum=$(awk '/^%SHA256SUM%$/ { getline; print }' "$keys"/desc/msys2-keyring-*/desc)
    curl -fsSL --retry 3 -o "$keys/$file" "https://repo.msys2.org/msys/x86_64/$file"
    sha256sum --quiet -c <<< "$sum  $keys/$file" || { echo "Checksum mismatch: $file" >&2; exit 1; }
    tar --zstd -xf "$keys/$file" -C "$keys" usr/share/pacman/keyrings
    GNUPGHOME=$(mktemp -d)
    export GNUPGHOME
    trap 'rm -rf -- "$GNUPGHOME"' EXIT
    gpg -q --batch --import "$keys/usr/share/pacman/keyrings/msys2.gpg" 2> /dev/null
    signer=$(gpg --batch --status-fd 1 --verify "$root/db/clang64.db.sig" "$root/db/clang64.db" 2> /dev/null |
        awk '$2 == "GOODSIG" { good = 1 } $2 == "VALIDSIG" { key = $NF } END { if (good) print key }')
    [[ -n $signer ]] || { echo "clang64.db: no good signature from a key in MSYS2's keyring" >&2; exit 1; }
    if grep -qix -- "$signer" "$keys/usr/share/pacman/keyrings/msys2-revoked"; then
        echo "clang64.db: signed by $signer, which MSYS2 revoked" >&2; exit 1
    fi
    for master in "${masters[@]}"; do
        [[ $signer == "$master" ]] && { echo "$signer" > "$root/db/signer"; exit 0; }
        while IFS=: read -r _ validity _ _ issuer _; do
            if [[ $validity == '!' && $issuer == "${master: -16}" && $issuer != "${signer: -16}" ]]; then
                echo "$signer" > "$root/db/signer"; exit 0
            fi
        done < <(gpg --batch --with-colons --check-sigs -- "$signer" 2> /dev/null | grep '^sig:')
    done
    echo "clang64.db: signed by $signer, which no MSYS2 master key certified" >&2; exit 1
)

fetch_sysroot() {
    # The workflow's compilers and build tools come from nixpkgs here, and from llvm-mingw, CMake and
    # Ninja in the Windows toolchain package (tools/win32-toolchain.sh), which runs CLANG64's pkgconf
    # and glslang on Windows; the toolchain's own runtime is what clang links into every binary.
    local -a wanted=(crt headers winpthreads libc++ libunwind compiler-rt)
    local name
    for name in $(grep -o '[a-z0-9+-]*:p' .github/workflows/windows.yml); do
        name=${name%:p}
        case $name in clang | lld | llvm-tools | cmake | ninja | nasm) ;; *) wanted+=("$name") ;; esac
    done
    mkdir -p -- "$root/pkgs" "$root/db"
    curl -fsSL --retry 3 -o "$root/db/clang64.db" "$repo/clang64.db"
    curl -fsSL --retry 3 -o "$root/db/clang64.db.sig" "$repo/clang64.db.sig"
    verify_database
    echo "clang64.db signed by $(<"$root/db/signer"), certified by an MSYS2 master key"
    rm -rf -- "$root/db/desc" && mkdir -p -- "$root/db/desc"
    tar -xf "$root/db/clang64.db" -C "$root/db/desc"
    # One line per package: name, file, sha256, then its dependencies and what it provides (P:).
    local index=$root/db/index
    awk 'FNR == 1 { if (n) print n, f, s, d; n = f = s = d = "" }
        /^%/ { k = $0; next } /^$/ { k = ""; next }
        k == "%NAME%" { n = $0 } k == "%FILENAME%" { f = $0 } k == "%SHA256SUM%" { s = $0 }
        k == "%DEPENDS%" { sub(/[<>=].*/, ""); d = d " " $0 } k == "%PROVIDES%" { sub(/[<>=].*/, ""); d = d " P:" $0 }
        END { if (n) print n, f, s, d }' "$root"/db/desc/*/desc > "$index"
    declare -A file sha deps provider
    local f s rest dep
    while read -r name f s rest; do
        file[$name]=$f sha[$name]=$s deps[$name]=
        for dep in $rest; do
            if [[ $dep == P:* ]]; then provider[${dep#P:}]=$name; else deps[$name]+=" $dep"; fi
        done
    done < "$index"
    declare -A seen
    local -a queue=("${wanted[@]/#/$prefix}") closure=()
    while ((${#queue[@]})); do
        name=${queue[0]} queue=("${queue[@]:1}")
        [[ -n ${file[$name]+set} ]] || name=${provider[$name]:-$name}
        [[ -z ${seen[$name]+set} ]] || continue
        seen[$name]=1
        if [[ -z ${file[$name]+set} ]]; then echo "No CLANG64 package provides $name" >&2; exit 1; fi
        closure+=("$name")
        for dep in ${deps[$name]}; do queue+=("$dep"); done
    done
    echo "${#closure[@]} packages into $root"
    for name in "${closure[@]}"; do
        f=$root/pkgs/${file[$name]}
        if [[ ! -f $f ]] || ! sha256sum --quiet -c <<< "${sha[$name]}  $f" 2>/dev/null; then
            curl -fsSL --retry 3 -o "$f.part" "$repo/${file[$name]}"
            sha256sum --quiet -c <<< "${sha[$name]}  $f.part" || { echo "Checksum mismatch: ${file[$name]}" >&2; exit 1; }
            mv -- "$f.part" "$f"
        fi
        tar --zstd -xf "$f" -C "$root" --exclude='.PKGINFO' --exclude='.BUILDINFO' --exclude='.MTREE' --exclude='.INSTALL'
    done
    printf '%s\n' "${closure[@]}" > "$root/packages.txt"
}

enter_nix() {
    if [[ ! -f $root/clang64/lib/clang/$llvm/lib/windows/libclang_rt.builtins-x86_64.a ]]; then
        echo "No CLANG64 sysroot with compiler-rt $llvm in $root: run tools/win32-cross.sh sysroot first." >&2; exit 1
    fi
    if [[ -z ${PS_WIN32_IN_NIX:-} ]]; then
        exec env PS_WIN32_IN_NIX=1 nix-shell -p "llvmPackages_$llvm.clang-unwrapped" "llvmPackages_$llvm.lld" \
            "llvmPackages_$llvm.llvm" cmake ninja pkg-config glslang nasm gnupg zstd --run "bash tools/win32-cross.sh $1"
    fi
    export PKG_CONFIG_SYSROOT_DIR=$root PKG_CONFIG_LIBDIR=$sysroot/lib/pkgconfig:$sysroot/share/pkgconfig
    unset PKG_CONFIG_PATH
}

toolchain() {
    mkdir -p -- "$out/bin" "$out/resource/lib"
    # clang's own headers from nixpkgs, the Windows compiler-rt from CLANG64: unwrapped clang has no
    # Windows runtime of its own.
    ln -sfn -- "$(clang -print-resource-dir)/include" "$out/resource/include"
    ln -sfn -- "$sysroot/lib/clang/$llvm/lib/windows" "$out/resource/lib/windows"
    local tool
    for tool in clang clang++; do
        printf '#!/bin/sh\nexec %s --target=x86_64-w64-windows-gnu --sysroot=%s -resource-dir=%s --start-no-unused-arguments -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ -fuse-ld=lld --end-no-unused-arguments "$@"\n' \
            "$(command -v "$tool")" "$sysroot" "$out/resource" > "$out/bin/x86_64-w64-mingw32-$tool"
        chmod +x -- "$out/bin/x86_64-w64-mingw32-$tool"
    done
}

# What makepkg-mingw does with packaging/windows/ffmpeg/PKGBUILD in the CI job, from the same source
# and configure flags, cross-compiled: the package replaces MSYS2's GPL-3.0-or-later FFmpeg in the
# sysroot and is registered in its database, so that tools/win32-bundle.sh reads its licence and ships
# its sources as it does there. The source tarball stays next to the PKGBUILD, where makepkg puts it.
ffmpeg() (
    local dir=packaging/windows/ffmpeg MINGW_PACKAGE_PREFIX=mingw-w64-clang-x86_64 MSYSTEM=CLANG64
    source "$dir/PKGBUILD"
    local tarball=$dir/$_realname-$pkgver.tar.xz download
    for download in "${source[@]}"; do
        [[ ! -f $dir/${download##*/} ]] || continue
        curl -fsSL --retry 3 -o "$dir/${download##*/}.part" "$download"
        mv -- "$dir/${download##*/}.part" "$dir/${download##*/}"
    done
    sha256sum --quiet -c <<< "${sha256sums[0]}  $tarball" || { echo "Checksum mismatch: $tarball" >&2; exit 1; }
    GNUPGHOME=$(mktemp -d)
    export GNUPGHOME
    gpg -q --import "$dir"/keys/pgp/*.asc 2> /dev/null
    gpg -q --status-fd 1 --verify "$tarball.asc" "$tarball" 2> /dev/null | grep -q "VALIDSIG ${validpgpkeys[0]}" ||
        { echo "Bad or missing signature: $tarball.asc" >&2; exit 1; }
    rm -rf -- "$GNUPGHOME"
    local work=$out/ffmpeg stage=$out/ffmpeg/stage
    rm -rf -- "$work" && mkdir -p -- "$work/build"
    tar -xf "$tarball" -C "$work"
    toolchain
    # FFmpeg's DLLs carry version resources, which include windows.h from the sysroot, and import
    # libraries made with the cross prefix's dlltool.
    printf '#!/bin/sh\nexec llvm-windres --preprocessor-arg=--sysroot=%s "$@"\n' "$sysroot" > "$out/bin/x86_64-w64-mingw32-windres"
    chmod +x -- "$out/bin/x86_64-w64-mingw32-windres"
    ln -sfn -- "$(command -v llvm-dlltool)" "$out/bin/x86_64-w64-mingw32-dlltool"
    (
        cd -- "$work/build"
        GIT_DIR=/dev/null "../$_realname-$pkgver/configure" --prefix=/clang64 --target-os=mingw32 --arch=x86_64 \
            --enable-cross-compile --cross-prefix="$out/bin/x86_64-w64-mingw32-" --cc="$out/bin/x86_64-w64-mingw32-clang" \
            --cxx="$out/bin/x86_64-w64-mingw32-clang++" --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip \
            --pkg-config=pkg-config "${_config[@]}" --logfile=config.log > configure.out 2>&1 ||
            { tail -20 configure.out >&2; echo "FFmpeg configure failed ($work/build/config.log)" >&2; exit 1; }
        _check_licence . || exit 1
        make -j"$(nproc)" > make.log 2>&1 || { tail -30 make.log >&2; echo "FFmpeg build failed ($work/build/make.log)" >&2; exit 1; }
        make DESTDIR="$stage" install > /dev/null
        rm -f -- "$stage"/clang64/lib/*.def "$stage"/clang64/bin/*.lib
        local licences=$stage/clang64/share/licenses/$_realname-lgpl
        install -Dm644 "../$_realname-$pkgver/LICENSE.md" "$licences/LICENSE.md"
        install -Dm644 "../$_realname-$pkgver/COPYING.LGPLv2.1" "$licences/COPYING.LGPLv2.1"
        _write_configuration . "$licences/CONFIGURATION"
    )
    local package=$pkgname-$pkgver-$pkgrel-any.pkg.tar.zst
    tar --zstd -cf "$work/$package" -C "$stage" clang64
    install_ffmpeg "$work/$package"
    echo "FFmpeg licence: $(sed -n 's/^#define FFMPEG_LICENSE "\(.*\)"$/\1/p' "$work/build/config.h")"
)

# The package replaces MSYS2's FFmpeg in the sysroot and is registered in its database, whether built
# above or by makepkg-mingw in the CI job (.github/workflows/toolchain.yml hands that one over).
install_ffmpeg() (
    local dir=packaging/windows/ffmpeg MINGW_PACKAGE_PREFIX=mingw-w64-clang-x86_64 file=$1
    source "$dir/PKGBUILD"
    local name=$pkgname-$pkgver-$pkgrel package=$pkgname-$pkgver-$pkgrel-any.pkg.tar.zst old
    [[ ${file##*/} == "$package" ]] || { echo "$file is not $package, which $dir/PKGBUILD builds" >&2; exit 1; }
    # tar replaces files by unlinking them, but MSYS2's FFmpeg also installed libraries this one has not.
    for old in "$root"/pkgs/"$MINGW_PACKAGE_PREFIX"-ffmpeg-[0-9]*-any.pkg.tar.zst; do
        [[ -f $old ]] || continue
        tar --zstd -tf "$old" | grep -v -e '^\.' -e '/$' | sed "s|^|$root/|" | xargs -r -d '\n' rm -f --
    done
    [[ $file -ef $root/pkgs/$package ]] || cp -f -- "$file" "$root/pkgs/$package"
    tar --zstd -xf "$root/pkgs/$package" -C "$root" --exclude='.PKGINFO' --exclude='.BUILDINFO' --exclude='.MTREE' --exclude='.INSTALL'
    mkdir -p -- "$root/db/desc/$name"
    {
        printf '%%NAME%%\n%s\n\n%%BASE%%\n%s\n\n%%VERSION%%\n%s-%s\n\n%%DESC%%\n%s\n\n%%URL%%\n%s\n\n' \
            "$pkgname" "$pkgbase" "$pkgver" "$pkgrel" "$pkgdesc" "$url"
        printf '%%LICENSE%%\n'; printf '%s\n' "${license[@]}"
        printf '\n%%PROVIDES%%\n'; printf '%s\n' "${provides[@]}"
    } > "$root/db/desc/$name/desc"
    local sha
    sha=$(sha256sum < "$root/pkgs/$package" | cut -d' ' -f1)
    grep -v -e "^$MINGW_PACKAGE_PREFIX-ffmpeg " -e "^$pkgname " "$root/db/index" > "$root/db/index.part" || true
    echo "$pkgname $package $sha  P:$MINGW_PACKAGE_PREFIX-ffmpeg" >> "$root/db/index.part"
    mv -f -- "$root/db/index.part" "$root/db/index"
    { grep -vx -e "$MINGW_PACKAGE_PREFIX-ffmpeg" -e "$pkgname" "$root/packages.txt" || true; echo "$pkgname"; } > "$root/packages.txt.part"
    mv -f -- "$root/packages.txt.part" "$root/packages.txt"
    rm -f -- "$root/db/files/$pkgname.list"
    echo "Installed $package into $root"
)

build() {
    toolchain
    cmake -S gpu -B "$out/gpu" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_C_COMPILER="$out/bin/x86_64-w64-mingw32-clang" \
        -DCMAKE_CXX_COMPILER="$out/bin/x86_64-w64-mingw32-clang++" -DCMAKE_RC_COMPILER="$(command -v llvm-windres)" \
        -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
        -DCMAKE_FIND_ROOT_PATH="$sysroot" -DCMAKE_PREFIX_PATH="$sysroot" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DPS_PGO=off -DPS_LTO="${PS_LTO:-OFF}" -DPS_NET=OFF > "$out/configure.log"
    echo "GPU library (cross, CLANG64 sysroot $root): LTO ${PS_LTO:-OFF}, no PGO, no online module"
    if ! ninja -C "$out/gpu" psport-gpu > "$out/gpu-build.log" 2>&1; then
        grep -v '^\[' "$out/gpu-build.log" | tail -40 >&2
        echo "GPU library build failed (full log: $out/gpu-build.log)" >&2; exit 1
    fi
    echo "Built $out/gpu/libpsport-gpu.dll"
    read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
    read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
    "$out/bin/x86_64-w64-mingw32-clang" -std=c11 -O2 -Wall -Wextra -Werror "${includes[@]}" -Isrc/runtime -Isrc/platform \
        tools/gpu_capabilities.c src/runtime/runtime_quirks.c src/runtime/runtime_json_print.c \
        src/platform/common/host_env.c "${libraries[@]}" \
        -o "$out/psport-gpu-capabilities.exe"
    echo "Built $out/psport-gpu-capabilities.exe (its DLLs: $sysroot/bin)"
    read -r -a vulkan_headers <<< "$(pkg-config --cflags vulkan)"
    "$out/bin/x86_64-w64-mingw32-clang" -std=c11 -O2 -Wall -Wextra -Werror -municode "${vulkan_headers[@]}" \
        -DPS_VERSION="\"$(<VERSION)\"" -DPS_COMMIT="\"$(git rev-parse --short=12 HEAD)\"" tools/win32_probe/*.c \
        -ladvapi32 -o "$out/psport-windows-probe.exe"
    echo "Built $out/psport-windows-probe.exe"
}

# The runtime with build.sh's sources, src/platform/win32 in place of src/platform/linux and the same
# compile and link flags as build.sh's Windows branch. psport.exe exports what libpsport-gpu.dll
# imports from it (gpu/port/psport_runtime.def).
runtime() {
    local cc=$out/bin/x86_64-w64-mingw32-clang
    if [[ ! -f $out/gpu/libpsport-gpu.dll.a ]]; then echo 'Run tools/win32-cross.sh build first.' >&2; exit 1; fi
    read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
    read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 | sed 's/ -mwindows//')"
    local -a areas=(main loader runtime runtime/imports kernel system audio input net platform) objects=() failed=()
    local version commit source object
    version=$(<VERSION)
    commit=$(git rev-parse --short=12 HEAD)
    rm -rf -- "$out/runtime" && mkdir -p -- "$out/runtime"
    for source in src/main/main.c src/main/cli.c src/main/game_info.c src/main/bind.c src/main/signals.c \
        src/loader/loader*.c src/*/runtime*.c src/platform/*.c src/platform/win32/*.c src/main/vulkan_smoke.c; do
        object=$out/runtime/${source//\//_}.o
        if "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -include src/platform/win32/posix_compat.h \
            -DPS_VERSION="\"$version\"" -DPS_COMMIT="\"$commit\"" "${includes[@]}" -I. "${areas[@]/#/-Isrc/}" \
            -c "$source" -o "$object" 2>> "$out/runtime/compile.log"; then
            objects+=("$object")
        else
            failed+=("$source")
        fi
    done
    for source in third_party/LibAtrac9/C/src/*.c; do
        "$cc" -std=c99 -O2 -g -w -c "$source" -o "$out/runtime/atrac9_${source##*/}.o"
    done
    llvm-ar rcs "$out/runtime/libatrac9.a" "$out"/runtime/atrac9_*.o
    if "$cc" "${objects[@]}" "$out/runtime/libatrac9.a" gpu/port/psport_runtime.def -L"$out/gpu" -lpsport-gpu \
        "${libraries[@]}" -pthread -lws2_32 -lbcrypt -lsynchronization -lonecore -Wl,--disable-high-entropy-va \
        -Wl,--error-limit=0 -o "$out/psport.exe" 2> "$out/runtime/link.log" &&
        ((${#failed[@]} == 0)); then
        echo "Built $out/psport.exe"; return
    fi
    if ((${#failed[@]})); then
        echo "psport.exe: ${#failed[@]} files do not compile for Windows (first errors in $out/runtime/compile.log):" >&2
        printf '  %s\n' "${failed[@]}" >&2
    fi
    local -a missing
    mapfile -t missing < <(grep -o 'undefined symbol: .*' "$out/runtime/link.log" | sed 's/.*: //; s/__declspec(dllimport) //' | sort -u)
    echo "psport.exe: ${#missing[@]} symbols undefined at the link ($out/runtime/link.log), ps_host_* among them:" >&2
    printf '%s\n' "${missing[@]}" | grep '^ps_host_' | sed 's/^/  /' >&2
    exit 1
}

out=$PWD/out/win32 sysroot=$root/clang64
case ${1:-} in
sysroot) fetch_sysroot ;;
ffmpeg) enter_nix ffmpeg; ffmpeg ;;
ffmpeg-package)
    (($# == 2)) || { echo "Usage: tools/win32-cross.sh ffmpeg-package FILE (one package, not $(($# - 1)))" >&2; exit 2; }
    install_ffmpeg "$2"
    ;;
build) enter_nix build; build ;;
runtime) enter_nix runtime; runtime ;;
*) echo "Usage: tools/win32-cross.sh sysroot|ffmpeg|ffmpeg-package FILE|build|runtime" >&2; exit 2 ;;
esac
