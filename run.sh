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
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
# PS_PREBUILT=1 (packaged builds, the AppImage): out/psport and its GPU library are installed
# next to this script; nothing is built and no nix-shell is needed.
# PS_DATA_DIR: writable directory for the generated files (out/), saves (user/) and bbport.ini;
# by default this directory.
data=${PS_DATA_DIR:-.}
out=$data/out
mkdir -p "$out"
export PS_CONFIG=${PS_CONFIG:-$data/bbport.ini}
# GPU paths are initialized when libpsport-gpu is loaded, before bbgpu_init receives --user.
# Select the same user directory now, including for an AppImage or a separate save profile.
export PS_GPU_USER_DIR=${PS_GPU_USER_DIR:-${PS_USER_DIR:-$data/user}}
# The drivers' own shader caches keep compiled pipelines between sessions (the port's cache gives
# them the shaders again at startup): NVIDIA's without its cleanup and with room for the game's
# pipelines (as Steam sets it), Mesa's with more room than its default.
export __GL_SHADER_DISK_CACHE=${__GL_SHADER_DISK_CACHE:-1}
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP:-1}
export __GL_SHADER_DISK_CACHE_SIZE=${__GL_SHADER_DISK_CACHE_SIZE:-10737418240}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
mkdir -p "$PS_GPU_USER_DIR"
# FSR 4.1.1 assets (tools/fsr4cap/build_assets.sh): next to run.sh or in the data directory.
if [[ -z ${PS_FSR411_DIR:-} && ! -d fsr4_411 && -d $data/fsr4_411 ]]; then
    export PS_FSR411_DIR=$data/fsr4_411
fi
if [[ -z ${PS_PREBUILT:-} && -z ${PS_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env PS_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
# PS_SAVE_LOG=1 (launcher: "Save the log and statistics to a file"): this run's output and its
# per-frame statistics also go to $data/logs/<time>.log, .frames.csv and .readbacks.csv.
if [[ ${PS_SAVE_LOG:-} == 1 ]]; then
    logs=$data/logs
    mkdir -p "$logs"
    printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
    export PS_FRAME_STATS=1 PS_FRAME_LOG=${PS_FRAME_LOG:-$logs/$stamp.frames.csv}
    export PS_READBACK_LOG=${PS_READBACK_LOG:-$logs/$stamp.readbacks.csv}
    echo "Log: $logs/$stamp.log"
    exec 3>"$logs/$stamp.log"
    # Bash builtins only (the AppImage's PATH has no tee).
    exec > >(while IFS= read -r line || [[ -n $line ]]; do printf '%s\n' "$line"; printf '%s\n' "$line" >&3; done) 2>&1
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
# PS_GAME_DIR: the game's folder (eboot.bin, sce_module, ...); default next to this directory.
game=${PS_GAME_DIR:-../CUSA03173}
if [[ ! -f $game/eboot.bin ]]; then echo "No eboot.bin in $game (set PS_GAME_DIR)." >&2; exit 1; fi
original_game=$game
game=$("$PYTHON" scripts/mods.py "$game" --out "$out" \
    --mods-dir "${PS_MODS_DIR:-$data/mods}" --config "${PS_MODS_CONFIG:-$data/mods.json}" \
    --enabled "${PS_MODS_ENABLED:-1}")
# A private merged view lasts for this launch, including restarts. Cleanup only our own view.
if [[ $game != "$(realpath "$original_game")" ]]; then
    mod_game=$game
    trap '"$PYTHON" -c '\''import shutil,sys; shutil.rmtree(sys.argv[1])'\'' "$mod_game"' EXIT
fi
"$PYTHON" scripts/game_check.py --launch "$game"
# psport loads and links the game from its folder (src/loader_*.c). PS_SYS_MODULES: a folder with
# the user's own dump of the console's system modules (*.sprx), linked when the game needs their library; PS_HLE=libA,libB keeps those to the port instead, and
# PS_LLE=libC links one the port implements itself, to compare them.
link_args=()
if [[ -n ${PS_SYS_MODULES:-} ]]; then
    link_args+=(--system-modules "$PS_SYS_MODULES" --hle "${PS_HLE:-}" --lle "${PS_LLE:-}")
fi
boot_args=(--game "$game" "${link_args[@]}")
"$PYTHON" scripts/content_profile.py "$game" --out "$out" --sku "${PS_CONTENT_SKU:-full}"
# Sizes chosen below for the previous launch are recomputed after an in-game restart.
if [[ ${PS_AUTO_RENDER_RES:-} == 1 ]]; then
    unset PS_RENDER_RES PS_OUTPUT_RES PS_AUTO_RENDER_RES
fi
# PS_RENDER_RES=WxH explicitly sets the game's render resolution (a patch at start).
# Frame rate: PS_FPS=uncap (default; delta-time patch, vblank 480 Hz, frames shown at once),
# 60/90 (fixed-timestep patches) or 30 (unpatched). PS_PATCHES adds patch names ("a;b").
fps=${PS_FPS:-uncap}
# The presets are patches to Bloodborne's executable: another title runs unpatched, on the console's
# 60 Hz vblank (at 480 Hz a title asking for 30 FPS through its flip rate got 240).
if ! "$PYTHON" scripts/patches.py --game-dir "$game" --is-bloodborne; then fps=30; fi
# bbport.ini output_res other than 1080p (720p for the Steam Deck, 1440p, 2160p): the whole game
# renders at the preset's size of the output (a patch), the upscaler fills the output, the UI is
# drawn at the output size. Preset and output changes need a restart.
# Live resolution changes keep the guest at 1920x1080 and scale host targets at run time instead
# (output and presets change in the menu without a restart, but post-processing stays at 1080p
# and scene targets are copied back: much slower on the Steam Deck and older GPUs). Chosen by
# PS_LIVE_RES=0/1, else bbport.ini live_resolution=0/1/auto (auto: the GPU check, strong
# discrete GPUs get them); off when unset. 1080p output and TAA always use the live path.
# The GPU tools (bb-gpu-capabilities: the game's GPU, its live resolution support).
if [[ -n ${PS_PROBE:-} ]]; then caps=$(dirname -- "$PS_PROBE")/bb-gpu-capabilities
elif [[ -n ${PS_PREBUILT:-} ]]; then caps=bin/bb-gpu-capabilities
else caps=out/bb-gpu-capabilities; fi
if [[ -z ${PS_RENDER_RES:-} ]]; then
    read -r scaled_render scaled_output < <("$PYTHON" scripts/patches.py --print-scaled --settings "$PS_CONFIG") || true
fi
live=0
if [[ -n ${scaled_output:-} ]]; then
    live=${PS_LIVE_RES:-}
    # Bash builtins only: the AppImage's PATH has no sed/grep (a missing one ended run.sh silently).
    if [[ -z $live && -f $PS_CONFIG ]]; then
        while IFS= read -r line || [[ -n $line ]]; do
            [[ $line =~ ^live_resolution=([01]|auto)$ ]] && live=${BASH_REMATCH[1]}
        done < "$PS_CONFIG"
    fi
    if [[ $live == auto ]]; then
        live=$("$caps" --live-resolution 2> >(while IFS= read -r line; do
            [[ $line == *MANGOHUD* ]] || printf '%s\n' "$line"; done >&2)) || live=0
    fi
    [[ $live == 1 ]] || live=0
fi
if [[ $live == 1 ]]; then
    echo "Output ${scaled_output}: live resolution changes (live_resolution=0: startup patch)"
elif [[ -n ${scaled_output:-} ]]; then
    export PS_RENDER_RES=$scaled_render PS_OUTPUT_RES=$scaled_output PS_AUTO_RENDER_RES=1
    export PS_DMEM_MB=${PS_DMEM_MB:-9152}
    echo "Output ${scaled_output}: scene ${scaled_render}, direct memory ${PS_DMEM_MB} MiB (live_resolution=1: live changes)"
fi
"$PYTHON" scripts/patches.py --out "$out" --fps "$fps" --extra "${PS_PATCHES:-}" --settings "$PS_CONFIG" --game-dir "$game" --render-res "${PS_RENDER_RES:-}" --output-res "${PS_OUTPUT_RES:-}" \
    --patches-dir "${PS_PATCHES_DIR:-$data/patches}" --patches-config "${PS_PATCHES_CONFIG:-$data/patches.json}"
# Background upload of the game's GPU memory into VRAM ahead of use (BufferCache::Preupload):
# 1 = only memory already in VRAM that the game rewrote (no new VRAM), 2 = all of it (~3 GB more
# VRAM), 0 = off. PS_GUEST_GPU_MEMORY=1 puts guest direct memory in GPU-visible dma-buf chunks
# (PS_GUEST_IN_PLACE below implies it: it commits all allocated memory up front and rules out PS_UFFD).
export PS_PREUPLOAD=${PS_PREUPLOAD:-1}
# Memory model and translation. PS_PC_MODEL=1 (the launcher's "Memory model": "Auto" gives it to
# AMD GPUs, the 0.3 model to the others; PS_PC_MODEL=1/0 by hand) selects how a PC release would
# work: the GPU uses the game's memory where it is (GPU-visible system memory) and keeps the data
# it reads often in VRAM, the command processor's work is translated rather than emulated
# (PS_GUEST_IN_PLACE=1 and what depends on it). AMD keeps the game's memory in a sparse arena;
# other GPUs get the layer's memory module, which binds it in place and keeps VRAM copies without
# sparse binding (NVIDIA's sparse binding stalls the GPU for seconds); PS_LAYER_MEMORY=1/0 by hand.
# 0 (the default off AMD): the model of 0.3 (VRAM copies of the game's memory, write tracking),
# with the fixes made since. PS_GUEST_IN_PLACE set by hand overrides it.
# PS_AS_0_3=1 (the launcher's developer switch "Synchronisation as in 0.3"): what changed since the
# 0.3 release is reverted for comparisons: the 0.3 memory model, WRITE_DATA/DMA waiting for every
# host copy, the scheduler's concurrent recording check.
# The game's GPU (vendor ID and name), for the choices below that depend on it.
gpu_vendor= gpu_name=
if [[ -z ${PS_PC_MODEL:-} || ( ${PS_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} ) ]]; then
    IFS=$'\t' read -r gpu_vendor gpu_name < <("$caps" --device 2> /dev/null) || true
fi
# Memory model left to the port: the new one on AMD (0x1002), the 0.3 one elsewhere (NVIDIA's
# path through the memory module is still being measured with testers).
if [[ -z ${PS_PC_MODEL:-} ]]; then
    if [[ $gpu_vendor == 0x1002 ]]; then
        export PS_PC_MODEL=1
        echo "Memory model: new (AMD GPU${gpu_name:+, $gpu_name}; PS_PC_MODEL=0: the 0.3 model)"
    else
        export PS_PC_MODEL=0
        echo "Memory model: the 0.3 one (${gpu_name:-GPU unknown}; PS_PC_MODEL=1: the new one)"
    fi
fi
# GCN 3-5 under RADV (Polaris, Vega and their APUs): with HiZ, depth compression showed black
# halos and missing geometry (issues #54, #99); RADV_DEBUG=nohiz fixed it at some cost. Set
# unless RADV_DEBUG is the user's, PS_AUTO_NOHIZ=0: off.
if [[ ${PS_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} && $gpu_name == *"(RADV "* ]]; then
    for chip in TONGA ICELAND CARRIZO FIJI STONEY POLARIS VEGA RAVEN RENOIR; do
        if [[ $gpu_name == *"(RADV $chip"* ]]; then
            export RADV_DEBUG=nohiz
            echo "RADV: $gpu_name: RADV_DEBUG=nohiz (depth compression artifacts on GCN 3-5; PS_AUTO_NOHIZ=0: off)"
            break
        fi
    done
fi
if [[ ${PS_AS_0_3:-0} == 1 ]]; then
    export PS_GUEST_IN_PLACE=0 PS_HOST_COPY_WAITS=all PS_PRODUCER_CHECK=1
fi
export PS_GUEST_IN_PLACE=${PS_GUEST_IN_PLACE:-${PS_PC_MODEL:-0}}
# MangoHud (launcher switch: MANGOHUD=1) must be drawn once. Two overlays on top of each other
# showed doubled, offset text: the Steam Deck's performance overlay (mangoapp, game mode) plus the
# in-game layer, or the AppImage's bundled layer plus a system MangoHud (the layer names differ,
# so the Vulkan loader loads both).
if [[ ${MANGOHUD:-0} == 1 ]]; then
    if pgrep -x mangoapp > /dev/null 2>&1; then
        echo "MangoHud: Steam's performance overlay is on; the in-game MangoHud stays off"
        unset MANGOHUD
        export DISABLE_MANGOHUD=1
    elif grep -qs '"VK_LAYER_MANGOHUD' /usr/share/vulkan/implicit_layer.d/*.json \
            /etc/vulkan/implicit_layer.d/*.json \
            "${XDG_DATA_HOME:-$HOME/.local/share}"/vulkan/implicit_layer.d/*.json; then
        # The system's own MangoHud (and its config) is used; the bundled one is skipped.
        export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:+$VK_LOADER_LAYERS_DISABLE,}VK_LAYER_MANGOHUD_overlay_64_x86_64
    fi
fi
# Write tracking with userfaultfd write-protection instead of mprotect (no address-space write lock:
# a streaming burst re-protected thousands of pages, ~10-20 ms); falls back to mprotect when the
# kernel lacks it. Off by default for now: the Steam Deck crashed 4 times in 9 minutes with it
# (2026-10-04, PM4 type 0 = zeroed command buffers, heap corruption); 1 turns it on.
export PS_UFFD=${PS_UFFD:-0}
# The command buffers of each submission are copied when the game submits them and decoded from
# the copy: while an area loads the game reused that memory before the GPU thread got to it
# ("Unimplemented PM4 type 0" a few seconds after loading a save). 0 decodes guest memory.
export PS_COPY_GPU_BUFFERS=${PS_COPY_GPU_BUFFERS:-1}
# A guest write next to small GPU outputs the GPU is still writing (counters, compute results,
# up to PS_GPU_WRITE_TWINS_MAX bytes per page) does not wait for the GPU (BufferCache twins).
export PS_GPU_WRITE_TWINS=${PS_GPU_WRITE_TWINS:-1} PS_GPU_WRITE_TWINS_MAX=${PS_GPU_WRITE_TWINS_MAX:-65536}
if [[ -z ${PS_VBLANK_HZ:-} ]]; then
    case $fps in uncap) export PS_VBLANK_HZ=480 ;; 90) export PS_VBLANK_HZ=90 ;; *) export PS_VBLANK_HZ=60 ;; esac
fi
# FSR 4: faster post passes next to the downloaded ones (incremental; tools/fsr4_optimize.sh).
if [[ -z ${PS_PREBUILT:-} && -d fsr4_shaders ]] && command -v spirv-cross >/dev/null; then
    bash tools/fsr4_optimize.sh || echo 'FSR 4: optimized post passes not built' >&2
fi
if [[ -n ${PS_PREBUILT:-} ]]; then
    probe=${PS_PROBE:-bin/psport}
else
    bash build.sh
    probe=${PS_PROBE:-out/psport}  # PS_PROBE: a wrapper (gdb) around it
fi
# Online play (PS_ONLINE=1, launcher: Online; the online module gpu/bbnet): the game's own server
# goes to the community server, summon signs to the shadNet WebAPI (scripts/online.py).
if [[ ${PS_ONLINE:-0} == 1 && -f scripts/online.py ]]; then
    export PS_SHADNET_SERVER=${PS_SHADNET_SERVER:-srv.shadps4.net:31313}
    # HTTPS needs CA certificates: OpenSSL from Nix looks only in its own store path (the
    # packaged wrapper brings a bundle; from source the system's is taken).
    if [[ ! -r ${SSL_CERT_FILE:-} ]]; then
        for bundle in "${NIX_SSL_CERT_FILE:-}" /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem /etc/ssl/ca-bundle.pem; do
            [[ -n $bundle && -r $bundle ]] && { export SSL_CERT_FILE=$bundle; break; }
        done
    fi
    [[ -n ${PS_SHADNET_WEBAPI:-} ]] || export PS_SHADNET_WEBAPI=http://${PS_SHADNET_SERVER%:*}:31315
    SHADPS4_HTTP_HOST_OVERRIDES_JSON=$("$PYTHON" scripts/online.py overrides --user "${PS_USER_DIR:-$data/user}") || true
    export SHADPS4_HTTP_HOST_OVERRIDES_JSON
fi
probe_args=("${boot_args[@]}" --content-profile "$out/content.bin" --patches "$out/patches.bin" --app0 "$game" --user "${PS_USER_DIR:-$data/user}" --timeout "${PS_TIMEOUT:-0}" "$@")
if [[ -n ${mod_game:-} ]]; then
    "$probe" "${probe_args[@]}" &
    mod_pid=$!
    trap 'kill -TERM "$mod_pid" 2>/dev/null || true' TERM INT
    mod_status=0
    wait "$mod_pid" || mod_status=$?
    # An interrupted wait must finish the child before removing its mounted view.
    if kill -0 "$mod_pid" 2>/dev/null; then wait "$mod_pid" || mod_status=$?; fi
    exit "$mod_status"
fi
exec "$probe" "${probe_args[@]}"
