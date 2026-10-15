#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
# bbport, and psport releases before the rename, read BB_* names: launchers written for them keep
# working for a transition. Each one set is copied to its PS_* name unless that is set too.
deprecated=()
for name in $(compgen -e BB_); do
    renamed=PS_${name#BB_}
    if [[ -z ${!renamed+set} ]]; then export "$renamed=${!name}"; deprecated+=("$name"); fi
done
if (( ${#deprecated[@]} )); then
    printf -v list '%s, ' "${deprecated[@]}"
    echo "psport: BB_* variables are deprecated, use PS_*: ${list%, }"
fi
mkdir -p out
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 freetype2 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${PS_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env PS_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan, sdl3 and freetype2, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libpsport-gpu.so.
# PS_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. PS_LTO=OFF disables link-time optimization.
pgo=${PS_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init).
if [[ ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# ccache when installed, one cache for every checkout and worktree: paths under each one's root
# are hashed relative to it. Empty otherwise, so a launcher cached by an earlier configure goes.
launcher=
if command -v ccache >/dev/null; then
    launcher=ccache
    export CCACHE_DIR=${CCACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/psport/ccache}
    export CCACHE_BASEDIR=${CCACHE_BASEDIR:-$PWD} CCACHE_NOHASHDIR=${CCACHE_NOHASHDIR:-1}
fi
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS_PGO="$pgo" \
    -DPS_LTO="${PS_LTO:-ON}" -DPS_PGO_DIR="$PWD/pgo" \
    -DCMAKE_C_COMPILER_LAUNCHER="$launcher" -DCMAKE_CXX_COMPILER_LAUNCHER="$launcher" \
    ${PS_ORIGIN_RPATH_ONLY:+-DCMAKE_BUILD_RPATH_USE_ORIGIN=ON} >/dev/null
echo "GPU library: PGO $pgo, LTO ${PS_LTO:-ON}"
# A failed GPU build must stop here: an older libpsport-gpu.so would otherwise be used silently.
if ! ninja -C out/gpu psport-gpu $(grep -q "^build psport-net:" out/gpu/build.ninja && echo psport-net) > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# Optional DLSS (NVIDIA RTX): DLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> builds the bridge
# (gpu/dlss_bridge, the only code using NVIDIA's SDK) and puts it next to psport with NVIDIA's
# libnvidia-ngx-dlss.so. Without them the DLSS upscaler is listed as unavailable.
if [[ -n ${DLSS_SDK_ROOT:-} ]]; then
    cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DDLSS_SDK_ROOT="$DLSS_SDK_ROOT" \
        -DCMAKE_C_COMPILER_LAUNCHER="$launcher" -DCMAKE_CXX_COMPILER_LAUNCHER="$launcher" >/dev/null
    ninja -C out/dlss-bridge >/dev/null
    rm -f out/libnvidia-ngx-dlss.so.*
    cp out/dlss-bridge/libpsport_dlss.so "$DLSS_SDK_ROOT"/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* out/
    cp "$DLSS_SDK_ROOT/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
    echo "DLSS bridge: out/libpsport_dlss.so"
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
# The build's own out/gpu too, except for a Nix build (PS_ORIGIN_RPATH_ONLY, packaging/port.nix,
# also for the libraries CMake links above): its sandbox's path would stay in the store's binaries.
gpu=(-Lout/gpu -lpsport-gpu -Wl,-rpath,'$ORIGIN/gpu' -rdynamic)
[[ -n ${PS_ORIGIN_RPATH_ONLY:-} ]] || gpu+=(-Wl,-rpath,"$PWD/out/gpu")
areas=(main loader runtime runtime/imports kernel system audio input net)
# By file name, as the flat src/ listed them, so that the link order and the binary's layout stay.
mapfile -t runtime < <(printf '%s\n' src/*/runtime*.c | LC_ALL=C sort -t/ -k3)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. "${areas[@]/#/-Isrc/}" src/main/probe.c src/loader/loader*.c "${runtime[@]}" src/main/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/psport
echo "Built $PWD/out/psport"
# GPU check for run.sh (live_resolution=auto) and the launcher's gamepad list (--gamepads).
"$CC" -std=c11 -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/psport-gpu-capabilities
