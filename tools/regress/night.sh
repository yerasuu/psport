#!/usr/bin/env bash
# tools/regress/night.sh [options] BASE_CHECKOUT BRANCH_CHECKOUT GAMES_ROOT: the standing night order
# for a branch against its base, each checkout with its own build in out/ (build.sh first):
#   0. os-code: tools/check_os_code.py in report mode on the branch (OUT/os-code-report.txt, total on the
#      console), and tools/check_licenses.py the same way (OUT/licenses-report.txt); neither stops
#      the night;
#   1. regress: every title of titles.txt but octopath0 on the branch with PS_CP_CENSUS=1, against
#      the base's regress run (-r, or one run here first on the base checkout);
#   2. perf: the branch's perf.txt with PS_FRAME_STATS=1, base and branch in turn, twice (base 1,
#      branch 1, base 2, branch 2), one title at a time, from the seed folder given with -u;
#   3. probe: the branch's perf.txt once more on the branch alone with PS_NATIVE_PROBE=1 as well (it times
#      every draw, so it stays out of the timed perf runs);
#   4. the branch's dumps against the base's (report.py --compare-shots);
# then a summary table (OUT/summary.txt and stdout). The steps run one after the other and perf
# needs the machine to itself.
#   -o OUT      default out/night/<time> in the branch checkout
#   -r BASE     an earlier regress output folder of the base (skips the base regress run)
#   -u SEED     perf seed folder (<SEED>/<title>/user, see perf.txt); without it perf starts fresh
#   -s SHELL    passed to regress.sh -s (toolchain nix file)
#   -p DB       PSNR under which a dump is listed (default 25)
# SYS_MODULES and the other variables titles.txt names come from the environment.
set -uo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out='' base_run='' seed='' shell_file='' psnr=25
while getopts 'o:r:u:s:p:' opt; do
    case $opt in
        o) out=$OPTARG ;; r) base_run=$(realpath -- "$OPTARG") ;; u) seed=$(realpath -- "$OPTARG") ;;
        s) shell_file=$(realpath -- "$OPTARG") ;; p) psnr=$OPTARG ;; *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(($# == 3)) || { sed -n '2,20p' "$0" | cut -c3-; exit 2; }
base=$(realpath -- "$1") branch=$(realpath -- "$2") games=$(realpath -- "$3")
for checkout in "$base" "$branch"; do
    [[ -x $checkout/out/psport && -x $checkout/tools/regress.sh ]] ||
        { echo "night.sh: $checkout has no out/psport or tools/regress.sh" >&2; exit 1; }
done
printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
out=$(realpath -m -- "${out:-$branch/out/night/$stamp}")
mkdir -p "$out"
log() { printf '[%(%H:%M:%S)T] night: %s\n' -1 "$*" | tee -a "$out/night.log" >&2; }

if (cd -- "$branch" && python3 tools/check_os_code.py) > "$out/os-code-report.txt" 2>&1; then
    log "os-code: $(tail -n 1 "$out/os-code-report.txt")"
else
    log "os-code: check_os_code.py did not run, see os-code-report.txt"
fi

if (cd -- "$branch" && python3 tools/check_licenses.py) > "$out/licenses-report.txt" 2>&1; then
    log "licenses: $(tail -n 1 "$out/licenses-report.txt")"
else
    log "licenses: check_licenses.py did not run, see licenses-report.txt"
fi

titles=$(awk '!/^#/ && NF && $1 != "octopath0" {print $1}' "$branch/tools/regress/titles.txt" | paste -sd,)
shell_args=() seed_args=()
[[ -n $shell_file ]] && shell_args=(-s "$shell_file")
[[ -n $seed ]] && seed_args=(-u "$seed")

# regress CHECKOUT OUTDIR ARGS...: the checkout's own regress.sh and titles file; its report's exit
# status (1 on a regression) is the summary's business, not a reason to stop the night.
regress() {
    local checkout=$1 dir=$2; shift 2
    log "$(basename -- "$dir"): $checkout"
    "$checkout/tools/regress.sh" -o "$dir" "${shell_args[@]}" "$@" "$games" > "$dir.log" 2>&1
    log "$(basename -- "$dir"): exit $?"
}

if [[ -z $base_run ]]; then
    base_run=$out/regress-base
    PS_CP_CENSUS=1 regress "$base" "$base_run" -n "$titles"
fi
PS_CP_CENSUS=1 regress "$branch" "$out/regress-branch" -n "$titles" -c "$base_run"
for round in 1 2; do
    for side in base branch; do
        checkout=$base
        [[ $side == branch ]] && checkout=$branch
        # The branch's perf.txt for both, so that both run the same schedules.
        PS_FRAME_STATS=1 regress "$checkout" "$out/perf-$side-$round" -j 1 -t "$branch/tools/regress/perf.txt" \
            "${seed_args[@]}"
    done
done
PS_FRAME_STATS=1 PS_NATIVE_PROBE=1 regress "$branch" "$out/probe-branch" -j 1 -t "$branch/tools/regress/perf.txt" \
    "${seed_args[@]}"
log "shots: regress-branch against $base_run"
"${PYTHON:-python3}" "$here/report.py" "$out/regress-branch" --compare "$base_run" --compare-shots "$base_run" \
    --psnr-min "$psnr" > "$out/shots.txt" 2>&1

"${PYTHON:-python3}" - "$out" "$base_run" > "$out/summary.txt" <<'EOF'
import json, sys
from pathlib import Path

out, base_run = Path(sys.argv[1]), Path(sys.argv[2])
def load(folder):
    path = folder / 'results.json'
    return {r['name']: r for r in json.loads(path.read_text())['titles']} if path.exists() else {}
def table(head, rows):
    widths = [max(len(x) for x in col) for col in zip(head, *rows)]
    for row in [head, *rows]:
        print('  '.join(x.ljust(w) for x, w in zip(row, widths)).rstrip())

branch = load(out / 'regress-branch')
print(f'night {out.name}: regress of the branch against {base_run}')
rows = []
for name, r in branch.items():
    shots = r.get('shots_vs_base') or {}
    rows.append([name, r['status'], 'yes' if r.get('frames') else 'no', '; '.join(r.get('regressions', [])) or '-',
                 str(shots.get('min_psnr_db', '')), str(len(shots.get('flagged', []))), r.get('crash') or '-'])
table(['title', 'status', 'frames', 'regressions', 'min dB', 'shots flagged', 'crash'], rows)

perf = {(side, n): load(out / f'perf-{side}-{n}') for side in ('base', 'branch') for n in (1, 2)}
names = sorted({name for runs in perf.values() for name in runs})
print('\nperf (PS_FRAME_STATS, mean of the last 10 Frame stats lines): FPS base 1/2, branch 1/2')
rows = []
fmt = lambda v: '' if v is None else f'{v:g}'
for name in names:
    get = lambda side, n, key: ((perf[side, n].get(name) or {}).get('perf') or {}).get(key)
    base_fps = [v for n in (1, 2) if (v := get('base', n, 'fps')) is not None]
    branch_fps = [v for n in (1, 2) if (v := get('branch', n, 'fps')) is not None]
    delta = ''
    if base_fps and branch_fps:
        delta = f'{100 * (sum(branch_fps) / len(branch_fps) / (sum(base_fps) / len(base_fps)) - 1):+.1f}%'
    rows.append([name, '/'.join(fmt(get('base', n, 'fps')) for n in (1, 2)),
                 '/'.join(fmt(get('branch', n, 'fps')) for n in (1, 2)), delta,
                 '/'.join(fmt(get('branch', n, 'stage_b_us_per_draw')) for n in (1, 2)),
                 '/'.join(fmt(get('branch', n, 'stage_b_busy_pct')) for n in (1, 2)),
                 '/'.join(fmt(get('branch', n, 'gpu_busy_ms')) for n in (1, 2))])
table(['title', 'base fps', 'branch fps', 'branch vs base', 'stage B us/draw', 'stage B busy%', 'GPU busy ms'], rows)
probe = load(out / 'probe-branch')
print('\nnative probe (PS_NATIVE_PROBE, branch, exit summary): go if pure >= 90% and GPU busy < 70% of the frame')
rows = []
for name in sorted(probe):
    s = ((probe[name].get('native_probe') or {}).get('summary')) or {}
    reasons = ', '.join(f'{k} {v}' for k, v in (s.get('main_reasons') or {}).items()) or '-'
    rows.append([name, fmt(s.get('pure')), fmt(s.get('all_pure_dcbs')), fmt(s.get('dcbs_per_frame')), fmt(s.get('pure_time')),
                 fmt(s.get('amdahl_4')), fmt(s.get('amdahl_8')), fmt(s.get('gpu_busy')) if s.get('gpu_busy') != 'n/a' else 'n/a',
                 s.get('decision') or 'no summary', reasons])
table(['title', 'pure%', 'pure dcb%', 'dcb/fr', 'pure time%', 'x4', 'x8', 'GPU busy%', 'decision', 'main reasons'], rows)
print(f'\nfull tables: {out}/shots.txt (dumps, CP census), {out}/probe-branch.log, {out}/*.log')
EOF
cat "$out/summary.txt"
