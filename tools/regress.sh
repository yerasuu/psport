#!/usr/bin/env bash
# tools/regress.sh [options] GAMES_ROOT: runs every title of the titles file headless, follows its
# pad schedule, and reports how far each got (loaded, entered, initialized, gpu, frames, audio), the
# crash signature and the imports without implementation. Results: OUTDIR/results.{tsv,json} and a
# table on stdout; with -c, regressions against an earlier results.json (or its directory).
#   -j N        titles at once (default 2: each is a full game with its own gamescope)
#   -o OUTDIR   default out/regress/<time>
#   -c PREVIOUS results.json or the directory holding it
#   -t TITLES   default tools/regress/titles.txt
#   -n NAMES    only these titles (comma-separated)
#   -s SHELL    run run.sh inside `nix-shell SHELL --argstr checkout <repo>` (a toolchain env such
#               as decanter's bbport.nix); without it, run.sh as it is
# Variables the titles file refers to ($SYS_MODULES...) come from the environment; a title whose
# variable is empty is skipped, so nothing user-specific lives in the repository.
# Uses the build in out/ (BB_PREBUILT=1): build.sh first.
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
jobs=2 out='' previous='' titles=$root/tools/regress/titles.txt only='' shell_file=''
while getopts 'j:o:c:t:n:s:' opt; do
    case $opt in
        j) jobs=$OPTARG ;; o) out=$OPTARG ;; c) previous=$OPTARG ;; t) titles=$OPTARG ;;
        n) only=,$OPTARG, ;; s) shell_file=$OPTARG ;; *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(($# == 1)) || { sed -n '2,16p' "$0" | cut -c3-; exit 2; }
games=$(realpath -- "$1")
[[ -x $root/out/bb-probe ]] || { echo "regress.sh: no out/bb-probe (run build.sh)" >&2; exit 1; }
printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
out=$(realpath -m -- "${out:-$root/out/regress/$stamp}")
mkdir -p "$out"
sched_dir=$(dirname -- "$(realpath -- "$titles")")
launch=(bash run.sh)
if [[ -n $shell_file ]]; then
    nix_shell=$(command -v nix-shell || echo /nix/var/nix/profiles/default/bin/nix-shell)
    launch=("$nix_shell" "$(realpath -- "$shell_file")" --argstr checkout "$root" --run 'bash run.sh')
fi

# expand VALUE: $NAME / ${NAME} from the environment; when one is empty, prints its name and fails.
expand() {
    local value=$1 name
    while [[ $value =~ \$\{?([A-Za-z_][A-Za-z0-9_]*)\}? ]]; do
        name=${BASH_REMATCH[1]}
        [[ -n ${!name:-} ]] || { printf '%s' "$name"; return 1; }
        value=${value//"${BASH_REMATCH[0]}"/"${!name}"}
    done
    printf '%s' "$value"
}

# stop_session PID: the game first, then gamescope: the other way round the game loses its display
# mid-frame.
stop_session() {
    local pid=$1 game_pid
    game_pid=$(pgrep -s "$pid" -x bb-probe | head -1)
    if [[ -n $game_pid ]]; then
        kill -TERM "$game_pid" 2>/dev/null
        for ((i = 0; i < 10; i++)); do kill -0 "$game_pid" 2>/dev/null || break; sleep 0.5; done
    fi
    kill -INT -- -"$pid" 2>/dev/null
    for ((i = 0; i < 10; i++)); do kill -0 -- -"$pid" 2>/dev/null || break; sleep 0.5; done
    kill -KILL -- -"$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}

# run_title NAME TITLE_ID GAME_DIR SCHEDULE ENV...: one headless session, as the scratch
# session.sh of the porting work: own session (setsid) so the whole tree can be stopped.
run_title() {
    local name=$1 title=$2 game=$3 schedule=$4; shift 4
    local dir=$out/$name
    rm -rf -- "$dir"; mkdir -p "$dir/shots"; : > "$dir/pad"
    printf 'name\t%s\ntitle\t%s\ngame\t%s\nschedule\t%s\n' "$name" "$title" "$game" "$schedule" > "$dir/run.tsv"
    # Inside a headless gamescope (its own X display, no window) and without the desktop's display
    # variables, with BB_AUDIO=none last so a title's env can't turn sound back on: a run never
    # shows anything or plays sound on the user's session; what it shows comes back as dumps.
    ( cd "$root" && exec setsid env -u DISPLAY -u WAYLAND_DISPLAY gamescope --backend headless -W 1920 -H 1080 -- \
        env -u WAYLAND_DISPLAY "$@" SDL_VIDEO_DRIVER=x11 ENABLE_GAMESCOPE_WSI=0 BB_AUDIO=none BB_AUDIO_STATS=1 \
        BB_PREBUILT=1 BB_PROBE="$root/out/bb-probe" BB_DATA_DIR="$dir" BB_USER_DIR="$dir/user" BB_GAME_DIR="$game" \
        BB_MODS_ENABLED=0 BB_PAD_FILE="$dir/pad" BB_PRESENT_DUMP_TRIGGER="$dir/shot" BB_DUMP_DIR="$dir/shots" \
        "${launch[@]}" ) > "$dir/log" 2>&1 < /dev/null &
    local pid=$! start=$SECONDS ended=schedule at what rest buttons hold
    # setsid put the session out of reach of the terminal and of a kill aimed at this script, so
    # an interrupted run would leave the game and gamescope running headless indefinitely.
    trap 'stop_session $pid; exit 143' TERM
    while read -r at what rest; do
        [[ -z $at || $at == \#* ]] && continue
        while ((SECONDS - start < at)); do kill -0 $pid 2>/dev/null || { ended=exited; break 2; }; sleep 0.2; done
        case $what in
            shot) : > "$dir/shot" ;;
            press) read -r buttons hold <<< "$rest"; echo "${buttons//+/ }" > "$dir/pad"; sleep "${hold:-0.3}"; : > "$dir/pad" ;;
            end) break ;;
        esac
    done < "$schedule"
    # A last frame for the frames milestone, whatever the schedule asked for: the presenter
    # removes the trigger once it took the dump.
    if [[ $ended == schedule ]] && kill -0 $pid 2>/dev/null; then
        : > "$dir/shot"
        for ((i = 0; i < 10; i++)); do [[ -e $dir/shot ]] || break; sleep 0.3; done
        sleep 1
    fi
    kill -0 $pid 2>/dev/null || ended=exited
    # stop_offset: what the game logs after it is told to stop (a lost swapchain once gamescope goes)
    # is not how the run went, and report.py takes no crash signature past it.
    printf 'ended\t%s\nseconds\t%d\nstop_offset\t%d\n' "$ended" $((SECONDS - start)) "$(stat -c %s -- "$dir/log")" >> "$dir/run.tsv"
    stop_session $pid
    # The prepared game (out/) is rebuilt by every run and is the bulk of the directory.
    rm -rf -- "$dir/out"
    echo "regress: $name $ended after $((SECONDS - start)) s" >&2
}

# Background jobs of a script ignore SIGINT, and the sessions ignore both: pass the stop on.
trap 'trap "" TERM; kill -TERM $(jobs -rp) 2>/dev/null; wait; exit 130' INT TERM HUP
while read -r name title game schedule envs; do
    [[ -z $name || $name == \#* ]] && continue
    [[ -z $only || $only == *,$name,* ]] || continue
    env_args=() missing=''
    for entry in ${envs:-}; do
        [[ $entry == - ]] && continue
        value=$(expand "${entry#*=}") || { missing=$value; break; }
        env_args+=("${entry%%=*}=$value")
    done
    # A title that can't run still gets a row, so that its absence shows in the table.
    skip=''
    if [[ -n $missing ]]; then skip="\$$missing unset"
    elif [[ ! -f $games/$game/eboot.bin ]]; then skip="no eboot.bin"
    fi
    if [[ -n $skip ]]; then
        rm -rf -- "${out:?}/$name"; mkdir -p "$out/$name"
        printf 'name\t%s\ntitle\t%s\nskipped\t%s\n' "$name" "$title" "$skip" > "$out/$name/run.tsv"
        echo "regress: $name skipped ($skip)" >&2
        continue
    fi
    while (($(jobs -rp | wc -l) >= jobs)); do wait -n; done
    run_title "$name" "$title" "$games/$game" "$sched_dir/$schedule" "${env_args[@]}" < /dev/null &
done < "$titles"
wait
exec "${PYTHON:-python3}" "$root/tools/regress/report.py" "$out" ${previous:+--compare "$previous"}
