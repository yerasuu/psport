#!/usr/bin/env bash
# Runs a Windows build of psport (tools/win32-cross.sh, out/win32) under Proton through umu-run (#777):
#   tools/win32-proton-run.sh out/win32/psport.exe [ARGS...]
# The CLANG64 DLLs it loads come from the sysroot and libpsport-gpu.dll from out/win32/gpu, through
# WINEPATH, so nothing is copied next to the executable. Its exit status is the program's.
#
# PS_WIN32_SYSROOT: the sysroot tools/win32-cross.sh unpacked (same default). PS_WIN32_PREFIX: the Wine
# prefix (default ~/.local/share/psport/win32-prefix; made on first use). PS_UMU_RUN: umu-run (default:
# the one on PATH). PROTONPATH, WINEDEBUG and WINEDLLOVERRIDES pass through to umu-run.
set -euo pipefail
if (($# < 1)); then echo "Usage: tools/win32-proton-run.sh EXE [ARGS...]" >&2; exit 2; fi
exe=$(realpath -- "$1")
shift
here=$(cd -- "$(dirname -- "$0")/.." && pwd)
root=${PS_WIN32_SYSROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/psport/win32-sysroot}
prefix=${PS_WIN32_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/psport/win32-prefix}
umu=${PS_UMU_RUN:-$(command -v umu-run || true)}
if [[ -z $umu ]]; then echo 'Need umu-run (set PS_UMU_RUN).' >&2; exit 1; fi
if [[ ! -d $root/clang64/bin ]]; then echo "No CLANG64 sysroot in $root (tools/win32-cross.sh sysroot)." >&2; exit 1; fi
# pressure-vessel cannot resolve its own architecture from umu-run's nested /nix/store symlinks and
# Vulkan fails in the container (decanter's crates/runtime/src/umu.rs): a copy outside the store works.
if [[ $(realpath -- "$umu") == /nix/store/* ]]; then
    echo "psport: $umu lives in /nix/store, where pressure-vessel loses Vulkan; set PS_UMU_RUN to a copy outside it." >&2
fi
mkdir -p -- "$prefix"
to_wine() { printf 'Z:%s' "${1//\//\\}"; }
winepath="$(to_wine "$root/clang64/bin");$(to_wine "$(dirname -- "$exe")/gpu");$(to_wine "$here/out/win32/gpu")"
# runinprefix starts Wine on the executable directly: the default verb goes through Proton's
# steam.exe, which does not hand its standard handles to the program, so its output was lost.
# The launcher's own library path and Nix's Vulkan drivers would leak into the container (decanter
# strips the same ones, crates/core/src/launcher_env.rs).
exec env -u LD_LIBRARY_PATH -u LD_PRELOAD -u VK_DRIVER_FILES -u VK_ICD_FILENAMES \
    WINEPREFIX="$prefix" GAMEID="${GAMEID:-umu-psport}" PROTON_VERB=runinprefix PROTON_USE_XALIA=0 \
    WINEDEBUG="${WINEDEBUG:-fixme-all}" WINEPATH="$winepath" "$umu" "$exe" "$@"
