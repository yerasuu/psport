#!/usr/bin/env python3
"""Classifies the runs of tools/regress.sh and compares them with an earlier set.

    python3 tools/regress/report.py OUTDIR [--compare PREVIOUS/results.json]

Each OUTDIR/<title>/ holds run.tsv (written by regress.sh), log and shots/. Milestones are read from
the log in the order the port reaches them; frames needs a dumped frame that is not black, audio a
BB_AUDIO_STATS peak above -60 dBFS. A regression is a milestone the earlier run had and this one
lacks, or a crash signature that is new or different. Python's standard library only, so that it
runs wherever run.sh does.
"""
import json, re, sys
from pathlib import Path

MILESTONES = ('loaded', 'entered', 'initialized', 'gpu', 'frames', 'audio')
LOG_MILESTONES = {
    'loaded': re.compile(r'^Mapped \d+ bytes, \d+ segments'),
    'entered': re.compile(r'^Entering original x86-64 code'),
    # The game's own code got far enough to start a thread: its initializers ran.
    'initialized': re.compile(r"^Runtime: guest thread '"),
    'gpu': re.compile(r'^Presenter: first game frame presented'),
}
SUMMARY = re.compile(r'^Runtime: imports \d+:.*without implementation (\d+)')
AUDIO = re.compile(r'^Audio stats port \d+:.* peak (-?[\d.]+|-inf) dBFS')
GUEST_FAULT = re.compile(r'^Guest fault \(signal (\d+)\) at guest offset (0x[0-9a-f]+)')
HOST_FAULT = re.compile(r'^Host fault \(signal (\d+)\) in (\S+)\+(0x[0-9a-f]+) \(([^)]*)\)')
OTHER_FAULT = re.compile(r'^Fault \(signal (\d+)\) at RIP')
STOP = re.compile(r'^(STOP: .*|.*Assertion Failed!.*|terminate called .*)')
AUDIO_FLOOR_DBFS = -60.0
# Mean Rec. 709 luma (0-255) above which a frame counts as drawn: a black clear with a cursor or a
# few dim pixels stays below it, a logo on black does not.
BLACK_LUMA = 4.0
DUMP_NAME = re.compile(r'_(\d+)x(\d+)_(bgra|rgba)\.raw$')


def mean_luma(path):
    """Mean luma of an 8-bit present dump, or None for a format this does not read."""
    m = DUMP_NAME.search(path.name)
    if not m:
        return None
    w, h, order = int(m[1]), int(m[2]), m[3]
    data = path.read_bytes()
    if len(data) < w * h * 4:
        return None
    r, b = (2, 0) if order == 'bgra' else (0, 2)
    pixels = w * h
    return (0.2126 * sum(data[r::4]) + 0.7152 * sum(data[1::4]) + 0.0722 * sum(data[b::4])) / pixels


def crash_signature(line):
    if m := GUEST_FAULT.match(line):
        return f'guest fault sig{m[1]} @{m[2]}'
    if m := HOST_FAULT.match(line):
        return f'host fault sig{m[1]} {Path(m[2]).name}+{m[3]} ({m[4]})'
    if m := OTHER_FAULT.match(line):
        return f'fault sig{m[1]} outside image'
    if m := STOP.match(line):
        return m[1].strip()
    return None


def classify(run_dir):
    meta = dict(line.split('\t', 1) for line in (run_dir / 'run.tsv').read_text().splitlines() if '\t' in line)
    result = {'name': meta.get('name', run_dir.name), 'title': meta.get('title', ''),
              'status': f"skipped ({meta['skipped']})" if 'skipped' in meta else meta.get('ended', 'unknown'),
              'seconds': int(meta.get('seconds', 0)), 'crash': '', 'no_impl': None, 'peak_dbfs': None,
              'frames_drawn': 0, 'frames_dumped': 0, 'max_luma': None}
    result.update({m: False for m in MILESTONES})
    log = run_dir / 'log'
    if 'skipped' in meta or not log.exists():
        return result
    for line in log.read_text(errors='replace').splitlines():
        for name, pattern in LOG_MILESTONES.items():
            if not result[name] and pattern.match(line):
                result[name] = True
        if m := SUMMARY.match(line):
            result['no_impl'] = int(m[1])
        elif m := AUDIO.match(line):
            peak = float(m[1])
            if result['peak_dbfs'] is None or peak > result['peak_dbfs']:
                result['peak_dbfs'] = peak
        elif not result['crash'] and (sig := crash_signature(line)):
            result['crash'] = sig
    for dump in sorted((run_dir / 'shots').glob('*_present_*.raw')):
        luma = mean_luma(dump)
        if luma is None:
            continue
        result['frames_dumped'] += 1
        result['max_luma'] = round(max(luma, result['max_luma'] or 0), 1)
        result['frames_drawn'] += luma > BLACK_LUMA
    result['frames'] = result['frames_drawn'] > 0
    result['audio'] = result['peak_dbfs'] is not None and result['peak_dbfs'] > AUDIO_FLOOR_DBFS
    return result


def regressions(now, before):
    found = [f'lost {m}' for m in MILESTONES if before.get(m) and not now[m]]
    if now['crash'] and now['crash'] != before.get('crash', ''):
        found.append(f"crash {before.get('crash') or 'none'} -> {now['crash']}")
    return found


def main(argv):
    if len(argv) not in (2, 4) or (len(argv) == 4 and argv[2] != '--compare'):
        sys.exit(__doc__)
    out = Path(argv[1])
    results = [classify(d) for d in sorted(out.iterdir()) if (d / 'run.tsv').exists()]
    previous = {}
    if len(argv) == 4:
        path = Path(argv[3])
        path = path / 'results.json' if path.is_dir() else path
        previous = {r['name']: r for r in json.loads(path.read_text())['titles']}
        for r in results:
            before = previous.get(r['name'])
            if before and not r['status'].startswith('skipped'):
                r['regressions'] = regressions(r, before)
                r['no_impl_before'] = before.get('no_impl')

    columns = ['name', 'title', 'status', 'seconds', *MILESTONES, 'frames_drawn', 'frames_dumped', 'max_luma',
               'peak_dbfs', 'no_impl', 'crash'] + (['regressions'] if previous else [])
    def cell(r, c):
        v = r.get(c)
        if isinstance(v, bool):
            return 'yes' if v else 'no'
        if isinstance(v, list):
            return '; '.join(v)
        return '' if v is None else str(v)
    with open(out / 'results.tsv', 'w') as f:
        f.write('\t'.join(columns) + '\n')
        for r in results:
            f.write('\t'.join(cell(r, c).replace('\t', ' ') for c in columns) + '\n')
    (out / 'results.json').write_text(json.dumps({'titles': results}, indent=1) + '\n')

    def reached(r):
        got = [m for m in MILESTONES[:5] if r[m]]
        return got[-1] if got else '-'
    head = ['title', 'id', 'status', 's', 'reached', 'ld', 'en', 'in', 'gp', 'fr', 'au', 'luma', 'peak',
            'no impl', 'crash']
    rows = []
    for r in results:
        mark = lambda m: 'x' if r[m] else '.'
        no_impl = cell(r, 'no_impl')
        if r.get('no_impl_before') is not None and r['no_impl'] is not None and r['no_impl'] != r['no_impl_before']:
            no_impl += f" ({r['no_impl'] - r['no_impl_before']:+d})"
        rows.append([r['name'], r['title'], r['status'], str(r['seconds']), reached(r), *map(mark, MILESTONES),
                     cell(r, 'max_luma'), cell(r, 'peak_dbfs'), no_impl, r['crash']])
    widths = [max(len(x) for x in col) for col in zip(head, *rows)]
    for row in [head, *rows]:
        print('  '.join(x.ljust(w) for x, w in zip(row, widths)).rstrip())
    bad = [r for r in results if r.get('regressions')]
    for r in bad:
        print(f"REGRESSION {r['name']}: {'; '.join(r['regressions'])}")
    print(f'results: {out}/results.tsv, results.json')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
