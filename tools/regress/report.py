#!/usr/bin/env python3
"""Classifies the runs of tools/regress.sh and compares them with an earlier set.

    python3 tools/regress/report.py OUTDIR [--compare PREVIOUS/results.json]

Each OUTDIR/<title>/ holds run.tsv (written by regress.sh), log and shots/. Milestones are read from
the log in the order the port reaches them; frames needs a dumped frame that is not black, audio a
BB_AUDIO_STATS peak above -60 dBFS. submit_forced counts the GPU library's "Forcing submitDone" lines:
the game went without a submitDone long enough for the driver to send one itself, the sign of a stall
on the GPU. A regression is a milestone the earlier run had and this one lacks, or a crash signature
that is new or different. Python's standard library only, so that it runs wherever run.sh does.
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
SUBMIT_FORCED = 'Forcing submitDone'
AUDIO = re.compile(r'^Audio stats port \d+:.* peak (-?[\d.]+|-inf) dBFS')
GUEST_FAULT = re.compile(r'^Guest fault \(signal (\d+)\) at guest offset (0x[0-9a-f]+)')
HOST_FAULT = re.compile(r'^Host fault \(signal (\d+)\) in (\S+)\+(0x[0-9a-f]+) \(([^)]*)\)')
OTHER_FAULT = re.compile(r'^Fault \(signal (\d+)\) at RIP')
# glibc's assert() as well as the GPU library's: the runtime uses both.
STOP = re.compile(r'^(STOP: .*|.*Assertion Failed!.*|.*: Assertion .* failed\.|terminate called .*)')
AUDIO_FLOOR_DBFS = -60.0
# A frame counts as drawn when this share of its pixels is lit (a channel above LIT_LEVEL): a black
# clear with a cursor stays far below it. Mean luma can't tell: Bloodborne's logo on black and its
# dark brightness screen measure 4.6 to 7 against 0 for black, its lit share is 3.5 to 4 %.
DRAWN_LIT_SHARE = 0.005
LIT_LEVEL = 24
LIT = bytes(v > LIT_LEVEL for v in range(256))
DUMP_NAME = re.compile(r'_(\d+)x(\d+)_(bgra|rgba|a2r10g10b10|a2b10g10r10)\.raw$')


def eight_bit(data, pixels, red_low):
    """RGBA bytes from packed 10-bit pixels (the top 8 bits of each channel)."""
    words = memoryview(data)[:pixels * 4].cast('I')
    out = bytearray(pixels * 4)
    for c, shift in enumerate((2, 12, 22) if red_low else (22, 12, 2)):
        out[c::4] = bytes((v >> shift) & 255 for v in words)
    return bytes(out)


def measure(path):
    """Mean Rec. 709 luma (0-255) and lit share of an 8-bit or 10-bit present dump, or None for a
    format this does not read."""
    m = DUMP_NAME.search(path.name)
    if not m:
        return None
    w, h, order = int(m[1]), int(m[2]), m[3]
    data = path.read_bytes()
    if len(data) < w * h * 4:
        return None
    if order.startswith('a2'):
        data, order = eight_bit(data, w * h, order == 'a2b10g10r10'), 'rgba'
    r, b = (2, 0) if order == 'bgra' else (0, 2)
    pixels = w * h
    luma = (0.2126 * sum(data[r::4]) + 0.7152 * sum(data[1::4]) + 0.0722 * sum(data[b::4])) / pixels
    lit = sum(sum(data[c:pixels * 4:4].translate(LIT)) for c in (0, 1, 2)) / (3 * pixels)
    return luma, lit


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
              'seconds': int(meta.get('seconds', 0)), 'crash': '', 'no_impl': None, 'submit_forced': None,
              'peak_dbfs': None, 'frames_drawn': 0, 'frames_dumped': 0, 'max_luma': None, 'max_lit_pct': None}
    result.update({m: False for m in MILESTONES})
    log = run_dir / 'log'
    if 'skipped' in meta or not log.exists():
        return result
    stop_offset = int(meta.get('stop_offset', -1))
    result['submit_forced'] = 0
    offset = 0
    for raw in log.read_bytes().splitlines(keepends=True):
        stopping = 0 <= stop_offset <= offset
        offset += len(raw)
        line = raw.decode(errors='replace').rstrip('\r\n')
        for name, pattern in LOG_MILESTONES.items():
            if not result[name] and pattern.match(line):
                result[name] = True
        if m := SUMMARY.match(line):
            result['no_impl'] = int(m[1])
        elif m := AUDIO.match(line):
            peak = float(m[1])
            if result['peak_dbfs'] is None or peak > result['peak_dbfs']:
                result['peak_dbfs'] = peak
        elif SUBMIT_FORCED in line:
            result['submit_forced'] += not stopping
        elif not result['crash'] and not stopping and (sig := crash_signature(line)):
            result['crash'] = sig
    # SIGABRT, SIGFPE or SIGKILL leave no fault line, yet the game ended before its schedule did.
    if not result['crash'] and result['status'] == 'exited':
        result['crash'] = 'exited without a fault line'
    for dump in sorted((run_dir / 'shots').glob('*_present_*.raw')):
        measured = measure(dump)
        if measured is None:
            continue
        luma, lit = measured
        result['frames_dumped'] += 1
        result['max_luma'] = round(max(luma, result['max_luma'] or 0), 1)
        result['max_lit_pct'] = round(max(100 * lit, result['max_lit_pct'] or 0), 2)
        result['frames_drawn'] += lit >= DRAWN_LIT_SHARE
    result['frames'] = result['frames_drawn'] > 0
    result['audio'] = result['peak_dbfs'] is not None and result['peak_dbfs'] > AUDIO_FLOOR_DBFS
    return result


def crash_key(signature):
    """The signature without the offset in a host library, which moves with every rebuild of it."""
    return re.sub(r'^(host fault sig\d+ \S+?)\+0x[0-9a-f]+ ', r'\1 ', signature)


def regressions(now, before):
    found = [f'lost {m}' for m in MILESTONES if before.get(m) and not now[m]]
    if now['crash'] and crash_key(now['crash']) != crash_key(before.get('crash', '')):
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
                r['submit_forced_before'] = before.get('submit_forced')

    columns = ['name', 'title', 'status', 'seconds', *MILESTONES, 'frames_drawn', 'frames_dumped', 'max_luma', 'max_lit_pct',
               'peak_dbfs', 'no_impl', 'submit_forced', 'crash'] + (['regressions'] if previous else [])
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
    head = ['title', 'id', 'status', 's', 'reached', 'ld', 'en', 'in', 'gp', 'fr', 'au', 'luma', 'lit%', 'peak',
            'no impl', 'forced', 'crash']
    def with_delta(r, c):
        now, before = r[c], r.get(f'{c}_before')
        text = cell(r, c)
        if before is not None and now is not None and now != before:
            text += f' ({now - before:+d})'
        return text
    rows = []
    for r in results:
        mark = lambda m: 'x' if r[m] else '.'
        rows.append([r['name'], r['title'], r['status'], str(r['seconds']), reached(r), *map(mark, MILESTONES),
                     cell(r, 'max_luma'), cell(r, 'max_lit_pct'), cell(r, 'peak_dbfs'), with_delta(r, 'no_impl'),
                     with_delta(r, 'submit_forced'), r['crash']])
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
