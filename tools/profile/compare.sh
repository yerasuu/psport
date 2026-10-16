#!/usr/bin/env bash
# tools/profile/compare.sh [options] BUILD_A BUILD_B GAMES_ROOT: the same perf.sh run on two builds
# (checkouts after build.sh), one after the other so that neither slows the other, then a text
# report of the functions whose share of the samples moved most (and, with -z, of the Tracy zones
# whose mean time moved most): OUTDIR/compare.txt, also on stdout. Runs games: night session only.
#   -o OUTDIR   default out/profile/compare-<time>; the runs go to OUTDIR/a and OUTDIR/b
#   -k ROWS     rows per table (default 30)
#   -n -t -w -d -g -F -z -u   as perf.sh
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out='' rows=30 is_tracy=0 pass=()
while getopts 'o:k:n:t:w:d:g:F:zu:' opt; do
    case $opt in
        o) out=$OPTARG ;; k) rows=$OPTARG ;; z) is_tracy=1; pass+=(-z) ;;
        n | t | w | d | g | F | u) pass+=("-$opt" "$OPTARG") ;; *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(($# == 3)) || { sed -n '2,8p' "$0" | cut -c3-; exit 2; }
[[ $rows =~ ^[1-9][0-9]*$ ]] || { echo "compare.sh: -k takes a positive number" >&2; exit 2; }
printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
out=$(realpath -m -- "${out:-$root/out/profile/compare-$stamp}")
mkdir -p "$out"
for side in a b; do
    build=$1; [[ $side == b ]] && build=$2
    "$root/tools/profile/perf.sh" "${pass[@]}" -C "$build" -o "$out/$side" "$3" > /dev/null ||
        { echo "compare.sh: the run on $build failed (see $out/$side)" >&2; exit 1; }
done
export LC_ALL=C
{
    printf 'A: %s\nB: %s\n\n' "$(realpath -- "$1")" "$(realpath -- "$2")"
    printf 'Functions, share of the samples (%%), largest moves first\n'
    printf '%7s %7s %7s  %s\n' A B B-A 'object  symbol'
    # top.tsv: percent, object, symbol. A function missing from one side was under perf.sh's limit.
    awk -F'\t' 'FNR == 1 { side++ }
        { key = $2 "\t" $3; share[key, side] = $1; keys[key] = 1 }
        END {
            for (key in keys) {
                a = share[key, 1] + 0; b = share[key, 2] + 0; split(key, part, "\t")
                printf "%.2f\t%7.2f %7.2f %+7.2f  %s  %s\n", (b > a ? b - a : a - b), a, b, b - a, part[1], part[2]
            }
        }' "$out/a/top.tsv" "$out/b/top.tsv" | sort -t$'\t' -k1,1gr | head -n "$rows" | cut -f2-
    if ((is_tracy)) && [[ -s $out/a/zones.csv && -s $out/b/zones.csv ]]; then
        printf '\nTracy zones, mean time (us) and count, largest moves first\n'
        printf '%10s %10s %8s %9s %9s  %s\n' A B 'B/A-1' 'count A' 'count B' zone
        # zones.csv (tracy-csvexport): name,src_file,src_line,total_ns,total_perc,counts,mean_ns,...
        awk -F, 'FNR == 1 { side++; next }
            # By name and file: lines move between the two builds.
            { file = $2; sub(/.*\//, "", file); key = $1 " (" file ")"
              mean[key, side] = $7; count[key, side] = $6; keys[key] = 1 }
            END {
                for (key in keys) {
                    a = mean[key, 1] / 1000; b = mean[key, 2] / 1000
                    moved = a > 0 ? b / a - 1 : 0
                    printf "%.4f\t%10.2f %10.2f %+7.1f%% %9d %9d  %s\n", (moved < 0 ? -moved : moved), a, b,
                        moved * 100, count[key, 1], count[key, 2], key
                }
            }' "$out/a/zones.csv" "$out/b/zones.csv" | sort -t$'\t' -k1,1gr | head -n "$rows" | cut -f2-
    fi
    printf '\nFlame graphs: %s/a/flame.svg, %s/b/flame.svg\n' "$out" "$out"
} | tee "$out/compare.txt"
