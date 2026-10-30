#!/usr/bin/env bash
# A/B play session with per-frame statistics: bash tools/play_ab.sh MODE [ENV=VALUE...]
# MODE names the run (out/ab_MODE_<time>.log/.frames.csv). Extra ENV=VALUE pairs are passed to
# run.sh. Runs in the foreground; close the game to end it.
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:?usage: tools/play_ab.sh MODE [ENV=VALUE...]}
shift
env_args=("$@")
source tools/builtin_patches.sh
resolve_builtin_patches "$PWD" play_ab
echo "$BUILTIN_PATCHES_NOTE" >&2
base=out/ab_${mode}_$(date +%m%d_%H%M%S)
echo "Log: $base.log"
env PS_FRAME_LOG="$PWD/$base.frames.csv" PS_FRAME_STATS=1 \
    PS_GAME_DIR="${PS_GAME_DIR:-$PWD/../game_files/CUSA03173}" "${env_args[@]}" \
    bash run.sh > "$base.log" 2>&1
echo "Done: $base.log"
