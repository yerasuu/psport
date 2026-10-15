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
# A relocatable bundle (tools/bundle.sh, a release's psport-<tag>-x86_64.tar.zst) is prebuilt, laid
# out as packaging/port.nix's output; relocate.sh points its programs at its own glibc loader for
# wherever it now is. Its Vulkan loader, Nix's, finds the host's drivers everywhere but in /etc.
if [[ -f relocate.sh ]]; then
    export PS_PREBUILT=1
    PS_PROBE=${PS_PROBE:-out/psport}
    bash relocate.sh || exit 1
    if [[ -z ${VK_DRIVER_FILES:-}${VK_ICD_FILENAMES:-} && -d /etc/vulkan/icd.d ]]; then
        export VK_ADD_DRIVER_FILES=/etc/vulkan/icd.d${VK_ADD_DRIVER_FILES:+:$VK_ADD_DRIVER_FILES}
    fi
fi
# PS_PREBUILT=1 (packaged builds, the AppImage): out/psport and its GPU library are installed
# next to this script; nothing is built and no nix-shell is needed.
# PS_DATA_DIR: writable directory for the generated files (out/), saves (user/) and psport.ini;
# by default this directory.
data=${PS_DATA_DIR:-.}
out=$data/out
mkdir -p "$out"
# psport.ini was bbport.ini before psport renamed it: a data directory an older release used keeps
# its settings, copied once (bbport.ini stays as it was for that release).
if [[ -z ${PS_CONFIG:-} && ! -e $data/psport.ini && -f $data/bbport.ini ]]; then
    cp -- "$data/bbport.ini" "$data/psport.ini" && echo "psport: settings copied from bbport.ini to psport.ini"
fi
export PS_CONFIG=${PS_CONFIG:-$data/psport.ini}
# GPU paths are initialized when libpsport-gpu is loaded, before psport_gpu_init receives --user.
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
# The same check as build.sh's: when it builds in shell.nix, the port it builds links Nix's Vulkan
# loader and must run there too, where shell.nix gives it Nix's Mesa drivers.
if [[ -z ${PS_PREBUILT:-} && -z ${PS_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 freetype2 && command -v cmake >/dev/null && command -v ninja >/dev/null; } && command -v nix-shell >/dev/null; then
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
# PS_GAME_DIR: the game's folder (eboot.bin, sce_module, ...); unset, the only dump next to this
# directory (bbport's layout, ../<title id>).
game=${PS_GAME_DIR:-}
if [[ -z $game ]]; then
    dumps=(../*/eboot.bin)
    if [[ ${#dumps[@]} == 1 && -f ${dumps[0]} ]]; then game=${dumps[0]%/eboot.bin}; fi
fi
if [[ ! -f $game/eboot.bin ]]; then
    echo "No eboot.bin in ${game:-a single folder next to this directory} (set PS_GAME_DIR)." >&2; exit 1
fi
# Loose-file mods (PS_MODS_ENABLED=0: none): psport loads the game through a private view under
# out/ with the enabled mods linked in, removed when it ends (src/loader/loader_mods.c, docs/en/MODS.md).
mod_args=()
if [[ ${PS_MODS_ENABLED:-1} != 0 ]]; then
    mod_args=(--mods "${PS_MODS_DIR:-$data/mods}" --mods-config "${PS_MODS_CONFIG:-$data/mods.json}" --out "$out")
fi
# psport loads and links the game from its folder (src/loader/loader_*.c). PS_SYS_MODULES: a folder with
# the user's own dump of the console's system modules (*.sprx), linked when the game needs their library; PS_HLE=libA,libB keeps those to the port instead, and
# PS_LLE=libC links one the port implements itself, to compare them.
link_args=()
if [[ -n ${PS_SYS_MODULES:-} ]]; then
    link_args+=(--system-modules "$PS_SYS_MODULES" --hle "${PS_HLE:-}" --lle "${PS_LLE:-}")
fi
boot_args=(--game "$game" "${link_args[@]}" "${mod_args[@]}")
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
# Sizes chosen below for the previous launch are recomputed after an in-game restart.
if [[ ${PS_AUTO_RENDER_RES:-} == 1 ]]; then
    unset PS_RENDER_RES PS_OUTPUT_RES PS_AUTO_RENDER_RES
fi
# PS_RENDER_RES=WxH explicitly sets the game's render resolution (a patch at start). The frame
# rate presets (PS_FPS), PS_PATCHES and the render resolution patch are a game profile's
# (gpu/games/<game>/, asked of psport itself): a title without one runs unpatched at its own size,
# on the console's 60 Hz vblank and with the console's direct memory, off the scaled output path
# below.
# psport.ini output_res other than 1080p (720p for the Steam Deck, 1440p, 2160p): the whole game
# renders at the preset's size of the output (a patch), the upscaler fills the output, the UI is
# drawn at the output size. Preset and output changes need a restart.
# Live resolution changes keep the guest at 1920x1080 and scale host targets at run time instead
# (output and presets change in the menu without a restart, but post-processing stays at 1080p
# and scene targets are copied back: much slower on the Steam Deck and older GPUs). Chosen by
# PS_LIVE_RES=0/1, else psport.ini live_resolution=0/1/auto (auto: the GPU check, strong
# discrete GPUs get them); off when unset. 1080p output and TAA always use the live path.
# The GPU tools (psport-gpu-capabilities: the game's GPU, its live resolution support).
if [[ -n ${PS_PROBE:-} ]]; then tools=$(dirname -- "$PS_PROBE")
elif [[ -n ${PS_PREBUILT:-} ]]; then tools=bin
else tools=out; fi
caps=$tools/psport-gpu-capabilities
has_presets=0
"$tools/psport" --has-presets "$game" && has_presets=1
# The scene's size for an output other than 1080p: the output divided by the upscaler preset's
# ratio (1, 1.5, 1.7, 2, 3; 1 without upscaler), rounded half to even to an even size (the game has
# half-resolution buffers). A scene of exactly 1920x1080 (4K Performance) is indistinguishable from
# the game's UI coordinate space, which the port's UI composition recognizes by that size: 1916x1078.
if [[ -z ${PS_RENDER_RES:-} && $has_presets == 1 && -f $PS_CONFIG ]]; then
    output_res= upscaler=fsr3 preset=0
    while IFS= read -r line || [[ -n $line ]]; do
        [[ $line == \#* || $line != *=* ]] && continue
        key=${line%%=*} value=${line#*=}
        key=${key#"${key%%[![:space:]]*}"} key=${key%"${key##*[![:space:]]}"}
        value=${value#"${value%%[![:space:]]*}"} value=${value%"${value##*[![:space:]]}"}
        case $key in output_res) output_res=$value ;; upscaler) upscaler=$value ;; preset) preset=$value ;; esac
    done < "$PS_CONFIG"
    width=1920 height=1080
    if [[ ${output_res,,} =~ ^\ *\+?([0-9]+)\ *x\ *\+?([0-9]+)\ *$ ]] && (( 10#${BASH_REMATCH[1]} && 10#${BASH_REMATCH[2]} )); then
        width=$((10#${BASH_REMATCH[1]})) height=$((10#${BASH_REMATCH[2]}))
    fi
    ratio=1/1
    if [[ $upscaler != off && ${preset:-0} =~ ^\ *([-+]?)([0-9]+)\ *$ ]]; then
        ratios=(1/1 3/2 17/10 2/1 3/1)
        preset=${BASH_REMATCH[1]}$((10#${BASH_REMATCH[2]}))
        ratio=${ratios[preset < 0 ? 0 : preset > 4 ? 4 : preset]}
    elif [[ $upscaler != off ]]; then
        ratio=
    fi
    if [[ -n $ratio && $upscaler != taa && ${width}x$height != 1920x1080 ]]; then
        sizes=()
        for size in "$width" "$height"; do
            (( divisor = 2 * ${ratio%/*}, half = size * ${ratio#*/} / divisor, rest = size * ${ratio#*/} % divisor ))
            (( 2 * rest > divisor || (2 * rest == divisor && half % 2) )) && (( half += 1 ))
            sizes+=($(( half * 2 > 2 ? half * 2 : 2 )))
        done
        scaled_render=${sizes[0]}x${sizes[1]} scaled_output=${width}x$height
        [[ $scaled_render == 1920x1080 ]] && scaled_render=1916x1078
    fi
fi
live=0
if [[ -n ${scaled_output:-} ]]; then
    live=${PS_LIVE_RES:-}
    # Bash builtins only: the AppImage's PATH has no sed/grep (a missing one ended run.sh silently).
    if [[ -z $live && -f $PS_CONFIG ]]; then
        while IFS= read -r line || [[ -n $line ]]; do
            [[ $line == \#* || $line != *=* ]] && continue
            key=${line%%=*} value=${line#*=}
            key=${key#"${key%%[![:space:]]*}"} key=${key%"${key##*[![:space:]]}"}
            value=${value#"${value%%[![:space:]]*}"} value=${value%"${value##*[![:space:]]}"}
            [[ $key == live_resolution && $value =~ ^([01]|auto)$ ]] && live=$value
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
# Third-party shadPS4/GoldHEN XML patches for the game, selected in patches.json (src/loader/loader_patches.c).
patch_args=(--patches-dir "${PS_PATCHES_DIR:-$data/patches}" --patches-config "${PS_PATCHES_CONFIG:-$data/patches.json}")
# The GPU uses the game's memory where it is (GPU-visible system memory) and keeps the data it reads
# often in VRAM, as a PC release would; the command processor's work is translated rather than
# emulated. AMD keeps the game's memory in a sparse arena (amd_guest_sparse_arena); other GPUs get
# the layer's memory module, which binds it in place and keeps VRAM copies without sparse binding
# (NVIDIA's sparse binding stalls the GPU for seconds); PS_LAYER_MEMORY=1/0 by hand. The memory
# model of 0.3 (VRAM copies of the game's memory, write tracking) is gone: an older launcher's
# PS_PC_MODEL, PS_AS_0_3, PS_GUEST_IN_PLACE, PS_PREUPLOAD or PS_UFFD does nothing.
ignored=()
for name in PS_PC_MODEL PS_AS_0_3 PS_GUEST_IN_PLACE PS_PREUPLOAD PS_UFFD; do
    if [[ -n ${!name:-} ]]; then
        ignored+=("$name=${!name}")
        unset "$name"
    fi
done
if (( ${#ignored[@]} )); then
    echo "Memory model: the PC one, the only one now; ignored: ${ignored[*]}"
fi
# The game's GPU decides driver workarounds: HOST quirks of the registry (docs/en/QUIRKS.md), which
# psport-gpu-capabilities --host-env turns into settings with their message: RADV_DEBUG=nohiz on
# GCN 3-5 under RADV (radv_gcn_nohiz, Polaris, Vega and their APUs: with HiZ, depth compression
# showed black halos and missing geometry, issues #54, #99), unless RADV_DEBUG is the user's,
# PS_AUTO_NOHIZ=0: off.
if [[ ${PS_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} ]]; then
    # A command substitution, not < <(...): the shell waits for it here, whereas a process
    # substitution's child outlives the exec below and stays a zombie of the game for its whole run.
    host_env=$("$caps" --host-env 2> /dev/null) || host_env=
    while IFS=$'\t' read -r setting message; do
        [[ ${setting%%=*} == RADV_DEBUG ]] || continue
        export "$setting"
        echo "$message"
    done <<< "$host_env"
fi
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
# The command buffers of each submission are copied when the game submits them and decoded from
# the copy: while an area loads the game reused that memory before the GPU thread got to it
# ("Unimplemented PM4 type 0" a few seconds after loading a save). 0 decodes guest memory.
export PS_COPY_GPU_BUFFERS=${PS_COPY_GPU_BUFFERS:-1}
# A guest write next to small GPU outputs the GPU is still writing (counters, compute results,
# up to PS_GPU_WRITE_TWINS_MAX bytes per page) does not wait for the GPU (BufferCache twins).
export PS_GPU_WRITE_TWINS=${PS_GPU_WRITE_TWINS:-1} PS_GPU_WRITE_TWINS_MAX=${PS_GPU_WRITE_TWINS_MAX:-65536}
# Online play (PS_ONLINE=1, launcher: Online; the online module gpu/net): the servers the game's
# title names (psport --online-overrides) are redirected, its own server to the community server,
# its summon signs to the shadNet WebAPI; a title without any gets no redirects (scripts/online.py,
# the one step of a launch that still runs Python, and only for online play).
if [[ ${PS_ONLINE:-0} == 1 && -f scripts/online.py ]]; then
    if [[ -z ${PYTHON:-} ]]; then
        PYTHON=$(command -v python3 || true)
        if [[ -z $PYTHON ]]; then
            for candidate in /nix/store/*-python3-*/bin/python3; do
                if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
            done
        fi
    fi
    if [[ -z ${PYTHON:-} ]]; then echo 'Online play (PS_ONLINE=1) needs Python 3: install it or set PYTHON.' >&2; exit 1; fi
    export PS_SHADNET_SERVER=${PS_SHADNET_SERVER:-srv.shadps4.net:31313}
    # HTTPS needs CA certificates: OpenSSL from Nix looks only in its own store path (the
    # packaged wrapper brings a bundle; from source the system's is taken).
    if [[ ! -r ${SSL_CERT_FILE:-} ]]; then
        for bundle in "${NIX_SSL_CERT_FILE:-}" /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem /etc/ssl/ca-bundle.pem; do
            [[ -n $bundle && -r $bundle ]] && { export SSL_CERT_FILE=$bundle; break; }
        done
    fi
    [[ -n ${PS_SHADNET_WEBAPI:-} ]] || export PS_SHADNET_WEBAPI=http://${PS_SHADNET_SERVER%:*}:31315
    title_online=$("$tools/psport" --online-overrides --game "$game") || title_online=
    SHADPS4_HTTP_HOST_OVERRIDES_JSON=$("$PYTHON" scripts/online.py overrides --user "${PS_USER_DIR:-$data/user}" --title-online "$title_online") || true
    export SHADPS4_HTTP_HOST_OVERRIDES_JSON
fi
probe_args=("${boot_args[@]}" "${patch_args[@]}" --app0 "$game" --user "${PS_USER_DIR:-$data/user}" --timeout "${PS_TIMEOUT:-0}" "$@")
exec "$probe" "${probe_args[@]}"
