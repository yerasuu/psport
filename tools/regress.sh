#!/usr/bin/env bash
# tools/regress.sh [options] GAMES_ROOT: runs every title of the titles file headless, follows its
# pad schedule, and reports how far each got (loaded, entered, initialized, gpu, frames, audio), the
# crash signature, the imports without implementation and the forced submitDones (GPU stalls).
# Results: OUTDIR/results.{tsv,json} and a table on stdout; with -c, regressions against an earlier
# results.json (or its directory).
#   -j N        titles at once (default 2: each is a full game with its own gamescope)
#   -o OUTDIR   default out/regress/<time>
#   -c PREVIOUS results.json or the directory holding it
#   -t TITLES   default tools/regress/titles.txt
#   -n NAMES    only these titles (comma-separated)
#   -s SHELL    run run.sh inside `nix-shell SHELL --argstr checkout <repo>` (a toolchain env such
#               as decanter's bbport.nix); without it, run.sh as it is
#   -u EARLIER  start each title's user folder (saves, settings) as a copy of its folder in the
#               EARLIER output folder, to load in this run what that one saved
# Variables the titles file refers to ($SYS_MODULES...) come from the environment; an entry whose
# variable is empty is left out, so nothing user-specific lives in the repository.
# Uses the build in out/ (PS_PREBUILT=1): build.sh first.
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
jobs=2 out='' previous='' titles=$root/tools/regress/titles.txt only='' shell_file='' seed=''
while getopts 'j:o:c:t:n:s:u:' opt; do
    case $opt in
        j) jobs=$OPTARG ;; o) out=$OPTARG ;; c) previous=$OPTARG ;; t) titles=$OPTARG ;;
        n) only=,$OPTARG, ;; s) shell_file=$OPTARG ;; u) seed=$(realpath -- "$OPTARG") ;; *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo "regress.sh: -j needs a positive integer" >&2; exit 2; }
(($# == 1)) || { sed -n '2,18p' "$0" | cut -c3-; exit 2; }
games=$(realpath -- "$1")
[[ -x $root/out/psport ]] || { echo "regress.sh: no out/psport (run build.sh)" >&2; exit 1; }
printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
out=$(realpath -m -- "${out:-$root/out/regress/$stamp}")
mkdir -p "$out"
sched_dir=$(dirname -- "$(realpath -- "$titles")")
launch=(bash run.sh)
if [[ -n $shell_file ]]; then
    nix_shell=$(command -v nix-shell || echo /nix/var/nix/profiles/default/bin/nix-shell)
    launch=("$nix_shell" "$(realpath -- "$shell_file")" --argstr checkout "$root" --run 'bash run.sh')
fi

# expand VALUE: $NAME / ${NAME} from the environment; fails when one is empty.
expand() {
    local value=$1 name
    while [[ $value =~ \$\{?([A-Za-z_][A-Za-z0-9_]*)\}? ]]; do
        name=${BASH_REMATCH[1]}
        [[ -n ${!name:-} ]] || return 1
        value=${value//"${BASH_REMATCH[0]}"/"${!name}"}
    done
    printf '%s' "$value"
}

# stop_session PID: the game first, then gamescope: the other way round the game loses its display
# mid-frame.
stop_session() {
    local pid=$1 game_pid
    game_pid=$(pgrep -s "$pid" -x psport | head -1)
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
    [[ -n $seed && -d $seed/$name/user ]] && cp -a -- "$seed/$name/user" "$dir/user"
    printf 'name\t%s\ntitle\t%s\ngame\t%s\nschedule\t%s\n' "$name" "$title" "$game" "$schedule" > "$dir/run.tsv"
    # Inside a headless gamescope (its own X display, no window) and without the desktop's display
    # variables, with PS_AUDIO=none and PS_PAD_HOST=0 last so a title's env can't turn sound or the
    # user's controllers back on: a run never shows anything, plays sound or reads a pad on the
    # user's session; what it shows comes back as dumps.
    ( cd "$root" && exec setsid env -u DISPLAY -u WAYLAND_DISPLAY gamescope --backend headless -W 1920 -H 1080 -- \
        env -u WAYLAND_DISPLAY "$@" SDL_VIDEO_DRIVER=x11 ENABLE_GAMESCOPE_WSI=0 PS_AUDIO=none PS_PAD_HOST=0 PS_AUDIO_STATS=1 \
        PS_PREBUILT=1 PS_PROBE="$root/out/psport" PS_DATA_DIR="$dir" PS_USER_DIR="$dir/user" PS_GAME_DIR="$game" \
        PS_MODS_ENABLED=0 PS_PAD_FILE="$dir/pad" PS_PRESENT_DUMP_TRIGGER="$dir/shot" PS_DUMP_DIR="$dir/shots" \
        "${launch[@]}" ) > "$dir/log" 2>&1 < /dev/null &
    local pid=$! start=$SECONDS ended=schedule at what rest buttons hold
    # Replaced by a rename, never rewritten in place: the runtime polls the file and could read it
    # truncated, an empty pad that drops the stick for a frame and stops a run-up.
    pad() { printf '%s\n' "$1" > "$dir/pad.new" && mv -f -- "$dir/pad.new" "$dir/pad"; }
    # setsid put the session out of reach of the terminal and of a kill aimed at this script, so
    # an interrupted run would leave the game and gamescope running headless indefinitely.
    trap 'stop_session $pid; exit 143' TERM HUP INT
    while read -r at what rest; do
        [[ -z $at || $at == \#* ]] && continue
        while ((SECONDS - start < at)); do kill -0 $pid 2>/dev/null || { ended=exited; break 2; }; sleep 0.2; done
        case $what in
            shot) : > "$dir/shot" ;;
            press) read -r buttons hold <<< "$rest"; pad "${buttons//+/ }"; sleep "${hold:-0.3}"; pad '' ;;
            hold) read -r buttons hold <<< "$rest"; pad "${buttons//+/ }"; sleep "${hold:-0}" ;;
            release) pad '' ;;
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
    env_args=()
    for entry in ${envs:-}; do
        [[ $entry == - ]] && continue
        value=$(expand "${entry#*=}") || continue
        env_args+=("${entry%%=*}=$value")
    done
    # A title that can't run still gets a row, so that its absence shows in the table.
    skip=''
    [[ -f $games/$game/eboot.bin ]] || skip="no eboot.bin"
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
