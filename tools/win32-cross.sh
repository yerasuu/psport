#!/usr/bin/env bash
# Windows build from Linux (#777), to check locally what the CI's MSYS2 CLANG64 job builds (decision
# #623): nixpkgs' clang and lld 23, the version CLANG64 ships, aimed at x86_64-w64-windows-gnu
# (llvm-mingw's way: libc++, compiler-rt, libunwind, UCRT, winpthreads) over a sysroot unpacked from
# CLANG64's own packages, the ones .github/workflows/windows.yml installs with their dependencies, so
# that both link the same headers and DLLs.
#
#   tools/win32-cross.sh sysroot   downloads and unpacks the packages into $PS_WIN32_SYSROOT
#   tools/win32-cross.sh build     builds libpsport-gpu.dll and psport-gpu-capabilities.exe in out/win32
#   tools/win32-cross.sh runtime   then tries psport.exe: lists the files that do not compile and the
#                                  symbols missing at the link, and fails until it links
#
# PS_WIN32_SYSROOT: where the packages go (default ~/.cache/psport/win32-sysroot, about 2 GB), outside
# the checkout. PS_LTO=ON for link-time optimisation (off by default, as in the CI job).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=${PS_WIN32_SYSROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/psport/win32-sysroot}
repo=https://repo.msys2.org/mingw/clang64
prefix=mingw-w64-clang-x86_64-
llvm=23

fetch_sysroot() {
    # The workflow's host tools come from nixpkgs here; the toolchain's own runtime is what clang
    # links into every binary.
    local -a wanted=(crt headers winpthreads libc++ libunwind compiler-rt)
    local name
    for name in $(grep -o '[a-z0-9+-]*:p' .github/workflows/windows.yml); do
        name=${name%:p}
        case $name in clang | lld | llvm-tools | cmake | ninja | pkgconf | glslang) ;; *) wanted+=("$name") ;; esac
    done
    mkdir -p -- "$root/pkgs" "$root/db"
    curl -fsSL --retry 3 -o "$root/db/clang64.db" "$repo/clang64.db"
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
            "llvmPackages_$llvm.llvm" cmake ninja pkg-config glslang --run "bash tools/win32-cross.sh $1"
    fi
    export PKG_CONFIG_SYSROOT_DIR=$root PKG_CONFIG_LIBDIR=$sysroot/lib/pkgconfig:$sysroot/share/pkgconfig
    unset PKG_CONFIG_PATH
}

build() {
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
    "$out/bin/x86_64-w64-mingw32-clang" -std=c11 -O2 -Wall -Wextra -Werror "${includes[@]}" -Isrc/runtime \
        tools/gpu_capabilities.c src/runtime/runtime_quirks.c src/runtime/runtime_json_print.c "${libraries[@]}" \
        -o "$out/psport-gpu-capabilities.exe"
    echo "Built $out/psport-gpu-capabilities.exe (its DLLs: $sysroot/bin)"
}

# The runtime with build.sh's sources, src/platform/win32 in place of src/platform/linux. psport.exe
# exports what libpsport-gpu.dll imports from it (gpu/port/psport_runtime.def).
runtime() {
    local cc=$out/bin/x86_64-w64-mingw32-clang
    if [[ ! -f $out/gpu/libpsport-gpu.dll.a ]]; then echo 'Run tools/win32-cross.sh build first.' >&2; exit 1; fi
    read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
    read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
    local -a areas=(main loader runtime runtime/imports kernel system audio input net platform) objects=() failed=()
    local version commit source object
    version=$(<VERSION)
    commit=$(git rev-parse --short=12 HEAD)
    rm -rf -- "$out/runtime" && mkdir -p -- "$out/runtime"
    for source in src/main/main.c src/main/cli.c src/main/game_info.c src/main/bind.c src/main/signals.c \
        src/loader/loader*.c src/*/runtime*.c src/platform/*.c src/platform/win32/*.c src/main/vulkan_smoke.c; do
        object=$out/runtime/${source//\//_}.o
        if "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -DPS_VERSION="\"$version\"" -DPS_COMMIT="\"$commit\"" \
            "${includes[@]}" -I. "${areas[@]/#/-Isrc/}" -c "$source" -o "$object" 2>> "$out/runtime/compile.log"; then
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
        "${libraries[@]}" -lbcrypt -lsynchronization -Wl,--error-limit=0 -o "$out/psport.exe" 2> "$out/runtime/link.log" &&
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
build) enter_nix build; build ;;
runtime) enter_nix runtime; runtime ;;
*) echo "Usage: tools/win32-cross.sh sysroot|build|runtime" >&2; exit 2 ;;
esac
