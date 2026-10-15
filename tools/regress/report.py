#!/usr/bin/env python3
"""Classifies the runs of tools/regress.sh and compares them with an earlier set.

    python3 tools/regress/report.py OUTDIR [--compare PREVIOUS/results.json] [--compare-shots BASE_DIR]
                                    [--psnr-min DB] [-j JOBS]

Each OUTDIR/<title>/ holds run.tsv (written by regress.sh), log and shots/. Milestones are read from
the log in the order the port reaches them; frames needs a dumped frame that is not black, audio a
PS_AUDIO_STATS peak above -60 dBFS. submit_forced counts the GPU library's "Forcing submitDone" lines:
the game went without a submitDone long enough for the driver to send one itself, the sign of a stall
on the GPU. A regression is a milestone the earlier run had and this one lacks, or a crash signature
that is new or different. Python's standard library only, so that it runs wherever run.sh does.

perf holds the means of the last 10 PS_FRAME_STATS 'Frame stats' lines before the stop (FPS, stage B
us/draw and busy share, GPU busy ms/frame), census the PS_CP_CENSUS 'CP census:' block (section ->
key -> value); both are null in a run that did not print them. --compare-shots compares each present
dump with the base run's dump taken for the same schedule second (PSNR over RGB and the largest
channel difference; identical dumps read 99 dB) and lists those under --psnr-min (default 25 dB):
listed, not counted as regressions, since animation alone moves a frame between two runs.
"""
import argparse, collections, json, math, os, re, sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

MILESTONES = ('loaded', 'entered', 'initialized', 'gpu', 'frames', 'audio')
LOG_MILESTONES = {
    'loaded': re.compile(r'^Mapped \d+ bytes, \d+ segments'),
    'entered': re.compile(r'^Entering original x86-64 code'),
    # The game's own code got far enough to start a thread: its initializers ran.
    'initialized': re.compile(r"^Runtime: guest thread '"),
    'gpu': re.compile(r'^Presenter: first game frame presented'),
}
# The imports without implementation the game called, as the call report counts them at exit; the
# count of those bound before the game starts ('Runtime: imports ...') includes code that never runs.
CALLS_SUMMARY = re.compile(r'^Calls: summary: (\d+) unimplemented')
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
FRAME_STATS = re.compile(r'^Frame stats: ([\d.]+) FPS')
STAGE_B = re.compile(r'; stage B ([\d.]+) us/draw, busy ([\d.]+)%')
GPU_BUSY = re.compile(r'; GPU busy ([\d.]+) ms/frame \((\d+) frames sampled')
CENSUS = re.compile(r'^CP census: ([a-z][a-z ]*): (.*)$')
PERF_LINES = 10
IDENTICAL_PSNR_DB = 99.0


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


def rgb_bytes(path):
    """(width, height, the R, G and B planes interleaved as read) of a present dump, or None."""
    m = DUMP_NAME.search(path.name)
    if not m:
        return None
    w, h, order = int(m[1]), int(m[2]), m[3]
    data = path.read_bytes()
    if len(data) < w * h * 4:
        return None
    if order.startswith('a2'):
        data = eight_bit(data, w * h, order == 'a2b10g10r10')
    data = memoryview(data)[:w * h * 4]
    # Alpha is left out: the presenter's alpha says nothing about the image.
    return w, h, bytes(data[0::4]) + bytes(data[1::4]) + bytes(data[2::4])


def compare_dumps(pair):
    """PSNR (dB) over RGB and the largest channel difference of two dumps, or a reason they can't
    be compared. Pixel pairs are counted as 16-bit codes in one C pass (Counter), not per byte."""
    path, base_path = pair
    now, before = rgb_bytes(Path(path)), rgb_bytes(Path(base_path))
    if now is None or before is None:
        return {'error': 'unreadable dump'}
    if now[:2] != before[:2]:
        return {'error': f'size {now[0]}x{now[1]} vs {before[0]}x{before[1]}'}
    a, b = now[2], before[2]
    pairs = bytearray(2 * len(a))
    pairs[0::2], pairs[1::2] = a, b
    squared, largest = 0, 0
    for code, count in collections.Counter(memoryview(pairs).cast('H')).items():
        d = abs((code & 255) - (code >> 8))
        squared += count * d * d
        largest = max(largest, d)
    if squared == 0:
        return {'psnr_db': IDENTICAL_PSNR_DB, 'max_abs': 0}
    return {'psnr_db': round(10 * math.log10(255 * 255 * len(a) / squared), 2), 'max_abs': largest}


def schedule_shots(run_dir, meta):
    """The seconds of the schedule's shots, and of its end (the last dump regress.sh takes), from
    the schedule run.tsv names, or this checkout's one of that name; None without either."""
    recorded = Path(meta.get('schedule', ''))
    for path in (recorded, Path(__file__).parent / recorded.name):
        if path.name and path.is_file():
            shots, end = [], None
            for line in path.read_text().splitlines():
                fields = line.split()
                if len(fields) >= 2 and fields[0].isdigit():
                    if fields[1] == 'shot':
                        shots.append(int(fields[0]))
                    elif fields[1] == 'end':
                        end = int(fields[0])
            return shots, end
    return None


def keyed_dumps(run_dir):
    """Present dumps by the schedule second they were taken for ('60', 'end'), from their times
    since the run started (run.tsv's mtime less its seconds); by index ('#3') when the schedule or
    the times are missing."""
    meta = dict(line.split('\t', 1) for line in (run_dir / 'run.tsv').read_text().splitlines() if '\t' in line)
    dumps = sorted((run_dir / 'shots').glob('*_present_*.raw'))
    schedule = schedule_shots(run_dir, meta)
    if not schedule or 'seconds' not in meta:
        return {f'#{i}': d for i, d in enumerate(dumps)}
    shots, end = schedule
    start = (run_dir / 'run.tsv').stat().st_mtime - int(meta['seconds'])
    keyed = {}
    for dump in dumps:
        # A dump lands a moment after its trigger; mtimes and the start are whole seconds.
        at = dump.stat().st_mtime - start + 1.5
        if end is not None and at >= end:
            key = 'end'
        else:
            earlier = [s for s in shots if s <= at]
            key = str(earlier[-1]) if earlier else f'@{int(at)}'
        keyed.setdefault(key, dump)
    return keyed


def compare_shots(out, results, base, psnr_min, jobs):
    """Fills each result's shots_vs_base; returns the rows of shots_compare.tsv."""
    work, owners = [], []
    for r in results:
        run_dir, base_dir = out / r['name'], base / r['name']
        if not (run_dir / 'shots').is_dir() or not (base_dir / 'run.tsv').exists():
            continue
        now, before = keyed_dumps(run_dir), keyed_dumps(base_dir)
        common = [k for k in now if k in before]
        r['shots_vs_base'] = {'base': str(base_dir), 'compared': 0, 'only_here': len(now) - len(common),
                              'only_base': len(before) - len(common), 'min_psnr_db': None, 'mean_psnr_db': None,
                              'max_abs': None, 'flagged': [], 'shots': []}
        for key in common:
            work.append((str(now[key]), str(before[key])))
            owners.append((r, key, now[key].name))
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        measured = list(pool.map(compare_dumps, work, chunksize=1))
    rows = []
    for (r, key, dump), m in zip(owners, measured):
        shot = {'second': key, 'dump': dump, **m}
        summary = r['shots_vs_base']
        summary['shots'].append(shot)
        if 'error' in m:
            summary['flagged'].append(shot)
        else:
            summary['compared'] += 1
            lowest = summary['min_psnr_db']
            summary['min_psnr_db'] = m['psnr_db'] if lowest is None else min(m['psnr_db'], lowest)
            summary['max_abs'] = max(m['max_abs'], summary['max_abs'] or 0)
            if m['psnr_db'] < psnr_min:
                summary['flagged'].append(shot)
        rows.append([r['name'], key, dump, str(m.get('psnr_db', '')), str(m.get('max_abs', '')), m.get('error', '')])
    for r in results:
        summary = r.get('shots_vs_base')
        if summary and summary['compared']:
            values = [s['psnr_db'] for s in summary['shots'] if 'psnr_db' in s]
            summary['mean_psnr_db'] = round(sum(values) / len(values), 2)
    return rows


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
              'seconds': int(meta.get('seconds', 0)), 'crash': '', 'no_impl_called': None, 'submit_forced': None,
              'peak_dbfs': None, 'frames_drawn': 0, 'frames_dumped': 0, 'max_luma': None, 'max_lit_pct': None,
              'perf': None, 'census': None}
    result.update({m: False for m in MILESTONES})
    log = run_dir / 'log'
    if 'skipped' in meta or not log.exists():
        return result
    stop_offset = int(meta.get('stop_offset', -1))
    result['submit_forced'] = 0
    offset = 0
    frame_stats = []
    census = {}
    for raw in log.read_bytes().splitlines(keepends=True):
        stopping = 0 <= stop_offset <= offset
        offset += len(raw)
        line = raw.decode(errors='replace').rstrip('\r\n')
        for name, pattern in LOG_MILESTONES.items():
            if not result[name] and pattern.match(line):
                result[name] = True
        if m := CALLS_SUMMARY.match(line):
            result['no_impl_called'] = int(m[1])
        elif m := FRAME_STATS.match(line):
            if not stopping:
                frame_stats.append((float(m[1]), STAGE_B.search(line), GPU_BUSY.search(line)))
        elif m := CENSUS.match(line):
            census[m[1]] = {k: number(v) for k, _, v in (p.partition('=') for p in m[2].split()) if v}
        elif m := AUDIO.match(line):
            peak = float(m[1])
            if result['peak_dbfs'] is None or peak > result['peak_dbfs']:
                result['peak_dbfs'] = peak
        elif SUBMIT_FORCED in line:
            result['submit_forced'] += not stopping
        elif not result['crash'] and not stopping and (sig := crash_signature(line)):
            result['crash'] = sig
    result['census'] = census or None
    if frame_stats:
        last = frame_stats[-PERF_LINES:]
        mean = lambda values: round(sum(values) / len(values), 2) if values else None
        stage_b = [m for _, m, _ in last if m]
        busy = [m for _, _, m in last if m and int(m[2])]
        result['perf'] = {'fps': mean([fps for fps, _, _ in last]), 'lines': len(last),
                          'stage_b_us_per_draw': mean([float(m[1]) for m in stage_b]),
                          'stage_b_busy_pct': mean([float(m[2]) for m in stage_b]),
                          'gpu_busy_ms': mean([float(m[1]) for m in busy])}
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


def number(text):
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def crash_key(signature):
    """The signature without the offset in a host library, which moves with every rebuild of it."""
    return re.sub(r'^(host fault sig\d+ \S+?)\+0x[0-9a-f]+ ', r'\1 ', signature)


def regressions(now, before):
    found = [f'lost {m}' for m in MILESTONES if before.get(m) and not now[m]]
    if now['crash'] and crash_key(now['crash']) != crash_key(before.get('crash', '')):
        found.append(f"crash {before.get('crash') or 'none'} -> {now['crash']}")
    return found


def print_perf(results):
    """The PS_FRAME_STATS table (means of the last Frame stats lines): one row per title with them."""
    fmt = lambda v: '' if v is None else str(v)
    rows = [[r['name'], fmt(p['fps']), fmt(p['stage_b_us_per_draw']), fmt(p['stage_b_busy_pct']), fmt(p['gpu_busy_ms']),
             str(p['lines'])] for r in results if (p := r.get('perf'))]
    if not rows:
        return
    head = ['title', 'fps', 'stage B us/draw', 'stage B busy%', 'GPU busy ms/fr', 'lines']
    print('Frame stats:')
    widths = [max(len(x) for x in col) for col in zip(head, *rows)]
    for row in [head, *rows]:
        print('  '.join(x.ljust(w) for x, w in zip(row, widths)).rstrip())


def print_census(results):
    """The PS_CP_CENSUS readiness table: one row per title that printed the block."""
    rows = []
    for r in results:
        c = r.get('census')
        if not c:
            continue
        get = lambda section, key: c.get(section, {}).get(key)
        fmt = lambda v: '' if v is None else str(v)
        driver = c.get('driver calls', {})
        rows.append([r['name'], fmt(get('run', 'dcbs_per_frame')), fmt(get('packets', 'graphics_per_frame')),
                     fmt(get('buffers', 'asc_per_frame')), fmt(get('buffers', 'ce_dumps_per_s')),
                     fmt(get('buffers', 'indirect_top')), fmt(get('buffers', 'indirect_nested')),
                     fmt(get('sync', 'set_predication')), fmt(get('sync', 'cond_exec')), fmt(get('packets', 'predicated')),
                     f"{fmt(get('sync', 'wait_reg_mem_gpu_memory'))}/{fmt(get('sync', 'wait_reg_mem_cpu_memory'))}",
                     fmt(get('sync', 'mem_semaphore')), fmt(driver.get('total'))])
    if not rows:
        return
    head = ['title', 'dcb/fr', 'pkt/fr', 'asc/fr', 'ce dump/s', 'ib', 'ib nested', 'setpred', 'condexec',
            'predicated', 'wrm gpu/cpu', 'memsem', 'driver calls']
    print('CP census:')
    widths = [max(len(x) for x in col) for col in zip(head, *rows)]
    for row in [head, *rows]:
        print('  '.join(x.ljust(w) for x, w in zip(row, widths)).rstrip())


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], usage=__doc__.split('\n\n')[1].strip())
    parser.add_argument('out', type=Path)
    parser.add_argument('--compare')
    parser.add_argument('--compare-shots', type=Path)
    parser.add_argument('--psnr-min', type=float, default=25.0)
    parser.add_argument('-j', '--jobs', type=int, default=min(4, os.cpu_count() or 1))
    args = parser.parse_args(argv[1:])
    out = args.out
    results = [classify(d) for d in sorted(out.iterdir()) if (d / 'run.tsv').exists()]
    previous = {}
    if args.compare:
        path = Path(args.compare)
        path = path / 'results.json' if path.is_dir() else path
        previous = {r['name']: r for r in json.loads(path.read_text())['titles']}
        for r in results:
            before = previous.get(r['name'])
            if before and not r['status'].startswith('skipped'):
                r['regressions'] = regressions(r, before)
                r['no_impl_called_before'] = before.get('no_impl_called')
                r['submit_forced_before'] = before.get('submit_forced')

    columns = ['name', 'title', 'status', 'seconds', *MILESTONES, 'frames_drawn', 'frames_dumped', 'max_luma', 'max_lit_pct',
               'peak_dbfs', 'no_impl_called', 'submit_forced', 'crash'] + (['regressions'] if previous else [])
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
    shot_rows = compare_shots(out, results, args.compare_shots, args.psnr_min, args.jobs) if args.compare_shots else []
    if args.compare_shots:
        with open(out / 'shots_compare.tsv', 'w') as f:
            f.write('name\tsecond\tdump\tpsnr_db\tmax_abs\terror\n')
            for row in shot_rows:
                f.write('\t'.join(row) + '\n')
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
                     cell(r, 'max_luma'), cell(r, 'max_lit_pct'), cell(r, 'peak_dbfs'), with_delta(r, 'no_impl_called'),
                     with_delta(r, 'submit_forced'), r['crash']])
    widths = [max(len(x) for x in col) for col in zip(head, *rows)]
    for row in [head, *rows]:
        print('  '.join(x.ljust(w) for x, w in zip(row, widths)).rstrip())
    print_perf(results)
    print_census(results)
    if args.compare_shots:
        print(f'shots against {args.compare_shots} (PSNR over RGB, flagged under {args.psnr_min:g} dB):')
        shot_head = ['title', 'compared', 'min dB', 'mean dB', 'max diff', 'flagged', 'unmatched here/base']
        shot_table = []
        for r in results:
            v = r.get('shots_vs_base')
            if v:
                shot_table.append([r['name'], str(v['compared']), cell(v, 'min_psnr_db'), cell(v, 'mean_psnr_db'),
                                   cell(v, 'max_abs'), str(len(v['flagged'])), f"{v['only_here']}/{v['only_base']}"])
        widths = [max(len(x) for x in col) for col in zip(shot_head, *shot_table)]
        for row in [shot_head, *shot_table]:
            print('  '.join(x.ljust(w) for x, w in zip(row, widths)).rstrip())
        for r in results:
            for shot in (r.get('shots_vs_base') or {}).get('flagged', []):
                what = shot.get('error') or f"{shot['psnr_db']} dB, max diff {shot['max_abs']}"
                second = shot['second'] + ('s' if shot['second'].isdigit() else '')
                print(f"SHOT {r['name']} {second} ({shot['dump']}): {what}")
    bad = [r for r in results if r.get('regressions')]
    for r in bad:
        print(f"REGRESSION {r['name']}: {'; '.join(r['regressions'])}")
    print(f'results: {out}/results.tsv, results.json' + (', shots_compare.tsv' if args.compare_shots else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
