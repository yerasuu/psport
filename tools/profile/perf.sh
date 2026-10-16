#!/usr/bin/env bash
# tools/profile/perf.sh [options] GAMES_ROOT: one title of a regress titles file, run headless by
# tools/regress.sh (its schedule, gamescope without a window, no sound or pad) and sampled by perf
# with call graphs once it has had time to load. In OUTDIR: perf.data, the folded stacks
# (perf.folded), a flame graph (flame.svg), the functions by self time (top.tsv: percent, object,
# symbol) and the regress run (regress/). With -z and a PS_TRACY=ON build, Tracy's zones too
# (trace.tracy, zones.csv). Runs a game: night session only (docs/en/PROFILING.md).
#   -n NAME     the title (default: the titles file's first)
#   -t TITLES   default tools/regress/perf.txt
#   -C CHECKOUT the build to run, a checkout after build.sh (default: this one)
#   -o OUTDIR   default out/profile/<time>-NAME
#   -w SECONDS  from the game's start to the first sample (default 60)
#   -d SECONDS  sampling time (default 60)
#   -g MODE     call graphs: fp (default; PS_PROFILE=1 builds give the port's own frames) or dwarf
#   -F HZ       samples per second (default 999)
#   -z          Tracy zones as well (tracy-capture over the same window)
#   -u EARLIER  passed to regress.sh: the user folders (saves) of an earlier run
# perf is the host's when installed, else Nix's; FlameGraph and Tracy's tools come from the nixpkgs
# psport pins (packaging/nixpkgs.nix), Tracy's at the version of gpu/third_party/tracy.
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
name='' titles=$root/tools/regress/perf.txt checkout=$root out='' warmup=60 duration=60 graph=fp hz=999
is_tracy=0 seed=''
while getopts 'n:t:C:o:w:d:g:F:zu:' opt; do
    case $opt in
        n) name=$OPTARG ;; t) titles=$OPTARG ;; C) checkout=$OPTARG ;; o) out=$OPTARG ;;
        w) warmup=$OPTARG ;; d) duration=$OPTARG ;; g) graph=$OPTARG ;; F) hz=$OPTARG ;;
        z) is_tracy=1 ;; u) seed=$OPTARG ;; *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(($# == 1)) || { sed -n '2,19p' "$0" | cut -c3-; exit 2; }
for value in "$warmup" "$duration" "$hz"; do
    [[ $value =~ ^[0-9]+$ ]] || { echo "perf.sh: -w, -d and -F take whole numbers" >&2; exit 2; }
done
[[ $graph == fp || $graph == dwarf ]] || { echo "perf.sh: -g is fp or dwarf" >&2; exit 2; }
games=$(realpath -- "$1")
titles=$(realpath -- "$titles")
checkout=$(realpath -- "$checkout")
[[ -x $checkout/out/psport ]] || { echo "perf.sh: no $checkout/out/psport (run build.sh)" >&2; exit 1; }
[[ -n $name ]] || name=$(awk '$1 !~ /^#/ && NF { print $1; exit }' "$titles")
[[ -n $name ]] || { echo "perf.sh: no title in $titles" >&2; exit 1; }
printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
out=$(realpath -m -- "${out:-$root/out/profile/$stamp-$name}")
mkdir -p "$out"

nix_tool() {
    local attribute=$1 program=$2 store
    store=$(nix-build --no-out-link "$root/packaging/nixpkgs.nix" -A "$attribute") || return 1
    printf '%s/bin/%s' "$store" "$program"
}
perf=$(command -v perf || nix_tool perf perf) || { echo "perf.sh: no perf" >&2; exit 1; }
flamegraph=$(nix_tool flamegraph flamegraph.pl) || { echo "perf.sh: no FlameGraph" >&2; exit 1; }
stackcollapse=${flamegraph%/*}/stackcollapse-perf.pl
if ((is_tracy)); then
    tracy_capture=$(nix_tool tracy tracy-capture) || { echo "perf.sh: no tracy-capture" >&2; exit 1; }
    tracy_csvexport=${tracy_capture%/*}/tracy-csvexport
fi
# With perf_event_paranoid above 1 an unprivileged perf may only sample user space.
event=cycles
(($(cat /proc/sys/kernel/perf_event_paranoid) > 1)) && event=cycles:u

"$checkout/tools/regress.sh" -j 1 -t "$titles" -n "$name" -o "$out/regress" ${seed:+-u "$seed"} "$games" \
    > "$out/regress.log" 2>&1 &
regress=$!
trap 'kill -TERM $regress 2>/dev/null; wait $regress; exit 130' INT TERM HUP

# game_pid: the title's game process (run.sh's psport --game, not its --has-presets check),
# recognised by the data folder regress.sh gives it.
game_pid() {
    local pid
    for pid in $(pgrep -x psport); do
        tr '\0' '\n' < "/proc/$pid/environ" 2> /dev/null | grep -qxF "PS_DATA_DIR=$out/regress/$name" || continue
        tr '\0' '\n' < "/proc/$pid/cmdline" 2> /dev/null | grep -qxF -- --game || continue
        echo "$pid"; return 0
    done
    return 1
}
pid=''
for ((i = 0; i < 1200; i++)); do
    pid=$(game_pid) && break
    kill -0 $regress 2> /dev/null || break
    sleep 0.5
done
[[ -n $pid ]] || { echo "perf.sh: $name never started (see $out/regress.log)" >&2; wait $regress; exit 1; }
echo "perf.sh: $name is pid $pid; sampling from ${warmup} s for ${duration} s" >&2
sleep "$warmup"
kill -0 "$pid" 2> /dev/null || { echo "perf.sh: $name exited during the first ${warmup} s" >&2; wait $regress; exit 1; }
if ((is_tracy)); then
    "$tracy_capture" -f -o "$out/trace.tracy" -s "$duration" > "$out/tracy-capture.log" 2>&1 &
    capture=$!
fi
"$perf" record -q -e "$event" -F "$hz" --call-graph "$graph" -p "$pid" -o "$out/perf.data" -- sleep "$duration" \
    2> "$out/perf-record.log" || echo "perf.sh: perf record failed (see $out/perf-record.log)" >&2
if ((is_tracy)); then
    wait $capture || echo "perf.sh: tracy-capture failed (see $out/tracy-capture.log)" >&2
fi
wait $regress
trap - INT TERM HUP
[[ -s $out/perf.data ]] || exit 1

# Set only here: the game keeps the session's locale.
export LC_ALL=C
"$perf" script -i "$out/perf.data" 2> /dev/null | "$stackcollapse" > "$out/perf.folded"
"$flamegraph" --title "psport $name ($(basename -- "$checkout"))" "$out/perf.folded" > "$out/flame.svg"
"$perf" report -i "$out/perf.data" --no-children --sort dso,sym --stdio -g none --percent-limit 0.05 \
    --field-separator=$'\t' 2> /dev/null |
    awk -F'\t' '!/^#/ && NF >= 3 {
        for (i = 1; i <= 3; i++) gsub(/^ +| +$/, "", $i)
        sub(/%$/, "", $1); sub(/^\[.\] /, "", $3); print $1 "\t" $2 "\t" $3
    }' > "$out/top.tsv"
if ((is_tracy)) && [[ -s $out/trace.tracy ]]; then
    "$tracy_csvexport" "$out/trace.tracy" > "$out/zones.csv"
fi
echo "perf.sh: $out/flame.svg, $out/top.tsv$([[ -s $out/zones.csv ]] && echo ", $out/zones.csv")" >&2
head -15 "$out/top.tsv"
