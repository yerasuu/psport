#!/usr/bin/env python3
"""Maps the system-library imports psport does not cover, per title, from its debug run folders.

    python3 -I tools/missing_imports.py <debug-dir> [--all] [--out FILE.md] [--title CUSAxxxxx]

<debug-dir> holds <host>/<YYYYmmdd-HHMMSS>-<TITLE_ID>/ folders (PS_DEBUG=1, runtime_debug.c). By
default only the newest folder of each title is read, --all reads every folder. Imports come from
imports.txt, link decisions from link.txt and call reports from calls.txt; a folder written before
those files existed (or one that lacks them) falls back to the same lines in run.log.
Exit code: 0 on success (also with no folder), 2 for a bad <debug-dir>.
"""
import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

FOLDER = re.compile(r'(\d{8}-\d{6})-([A-Za-z0-9_]+)$')
RUNTIME_IMPORT = re.compile(r'Runtime: import (\S+) \((.*?)\) (has no implementation|is a stub)')
UNSUPPORTED = re.compile(r'Runtime: unsupported import (\S+) \((.*?)\)')
LOADER_IMPORT = re.compile(r'Loader: import (\S+) \((.*?)\) of (\S+) version')
IMPORT_TOTALS = re.compile(r'Runtime: imports (\d+): (.*)')
CALL_SUMMARY = re.compile(r'Calls: summary (unimplemented|stub|failed) (\d+) ([^\s#]+#\S+) \((.*?)\)')
NOT_LINKED = re.compile(r'Loader: system module (\S+) not linked(?:: (served natively)|, the port serves (\S+): (.*))')
LINKED_ALTHOUGH = re.compile(r'Loader: system module (\S+) linked although')
LACKS = re.compile(r'lacks (\d+) of the imports of ([\w.]+)')
KIND_ORDER = ('no implementation', 'stub', 'left to host', 'called (unimplemented)', 'called (stub)', 'called (failed)')
RUNTIME_KIND = {'has no implementation': KIND_ORDER[0], 'is a stub': KIND_ORDER[1]}
UNNAMED = 'name not resolved'


def read_lines(folder, name):
    try:
        return (folder / name).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return None


def first_available(folder, name):
    return read_lines(folder, name) or read_lines(folder, 'run.log') or []


def split_nid(token):
    nid, _, tag = token.partition('#')
    return nid, tag


def parse_folder(folder):
    """Returns the imports by NID ({kinds, names, tags}), the totals line and the link decisions."""
    imports = defaultdict(lambda: {'kinds': set(), 'names': set(), 'tags': set()})
    link = {'native': [], 'served': [], 'although': []}
    totals = {}

    def add(token, name, kind, tag=''):
        nid, suffix = split_nid(token)
        entry = imports[nid]
        entry['kinds'].add(kind)
        if name != UNNAMED:
            entry['names'].add(name)
        if tag or suffix:
            entry['tags'].add(tag or suffix)

    for line in first_available(folder, 'imports.txt'):
        if m := RUNTIME_IMPORT.match(line):
            add(m[1], m[2], RUNTIME_KIND[m[3]])
        elif m := UNSUPPORTED.match(line):
            add(m[1], m[2], KIND_ORDER[3])
        elif m := LOADER_IMPORT.match(line):
            add(m[1], m[2], KIND_ORDER[2], m[3])
        elif m := IMPORT_TOTALS.match(line):
            for part in m[2].split(', '):
                label, _, count = part.rpartition(' ')
                if count.isdigit():
                    totals[label] = int(count)
            totals['imports'] = int(m[1])
    for line in first_available(folder, 'calls.txt'):
        if m := CALL_SUMMARY.match(line):
            add(m[3], m[4], f'called ({m[1]})')
    for line in first_available(folder, 'link.txt'):
        if m := NOT_LINKED.match(line):
            if m[2]:
                link['native'].append(m[1])
            else:
                link['served'].append(f'{m[1]} ({m[4].split(" (")[0]})')
        elif m := LINKED_ALTHOUGH.match(line):
            lacks = LACKS.search(line)
            missing = f', lacks {lacks[1]} of {lacks[2]}' if lacks else ''
            link['although'].append(f'{m[1]}{missing}')
    return imports, totals, link


def list_dirs(parent):
    try:
        return [d for d in sorted(parent.iterdir()) if d.is_dir()]
    except OSError:
        return []


def find_folders(debug_dir):
    found = []
    hosts = list_dirs(debug_dir)
    for host_dir in hosts:
        for folder in list_dirs(host_dir):
            if m := FOLDER.match(folder.name):
                found.append({'path': folder, 'host': host_dir.name, 'stamp': m[1], 'title': m[2]})
    if not found:
        for folder in hosts:
            if m := FOLDER.match(folder.name):
                found.append({'path': folder, 'host': debug_dir.name, 'stamp': m[1], 'title': m[2]})
    return found


def stamp_date(stamp):
    return f'{stamp[:4]}-{stamp[4:6]}-{stamp[6:8]} {stamp[9:11]}:{stamp[11:13]}:{stamp[13:15]}'


def cell(text):
    return text.replace('|', '\\|')


def render(folders, newest, runs):
    stamps = sorted(f['stamp'] for f in folders)
    incomplete = [f for f in folders if not (f['path'] / 'summary.txt').is_file()]
    partial = [f for f in folders if any(f['path'].glob('*.partial'))]
    out = ['# Imports psport does not cover', '']
    out.append(f'- Folders read: {len(folders)} (incomplete, no summary.txt: {len(incomplete)}; '
               f'with unfinished *.partial files, skipped: {len(partial)})')
    out.append(f'- Hosts: {", ".join(sorted({f["host"] for f in folders}))}')
    out.append(f'- Folder stamps: {stamp_date(stamps[0])} to {stamp_date(stamps[-1])}')
    out.append(f'- Titles: {len(runs)}')
    out.append('')
    out.append('Folders:')
    out.extend(f'- `{f["host"]}/{f["path"].name}`{" (incomplete)" if f in incomplete else ""}' for f in folders)

    rows = defaultdict(lambda: {'kinds': set(), 'names': set(), 'tags': set(), 'titles': defaultdict(int)})
    parsed = {}
    for folder in folders:
        imports, totals, link = parse_folder(folder['path'])
        parsed[folder['path']] = (totals, link)
        for nid, entry in imports.items():
            row = rows[nid]
            row['kinds'] |= entry['kinds']
            row['names'] |= entry['names']
            row['tags'] |= entry['tags']
            row['titles'][folder['title']] += 1

    out += ['', '## Imports', '', '| Import (NID, name, library tag) | Kind | Titles (runs with it / runs read) |',
            '|---|---|---|']
    ordered = sorted(rows.items(), key=lambda kv: (-len(kv[1]['titles']), sorted(kv[1]['names'])[:1] or [''], kv[0]))
    for nid, row in ordered:
        name = '/'.join(sorted(row['names'])) or UNNAMED
        tags = '/'.join(sorted(row['tags'])) or '-'
        kinds = ', '.join(k for k in KIND_ORDER if k in row['kinds'])
        titles = ', '.join(f'{t} ({n}/{runs[t]})' for t, n in sorted(row['titles'].items()))
        out.append(f'| `{nid}` {cell(name)} [{cell(tags)}] | {kinds} | {titles} |')
    if not rows:
        out.append('| none | | |')

    out += ['', '## Per title', '', 'Counts and link decisions come from the newest folder of each title.']
    for title in sorted(newest):
        folder = newest[title]
        totals, link = parsed[folder['path']]
        counts = defaultdict(int)
        for row in rows.values():
            if title in row['titles']:
                for kind in row['kinds']:
                    counts[kind] += 1
        out += ['', f'### {title}', '', f'Newest folder: `{folder["path"].name}` (runs read: {runs[title]})', '']
        out.append('- Imports by kind, all runs read: ' + (', '.join(f'{k} {counts[k]}' for k in KIND_ORDER if counts[k]) or 'none'))
        if totals:
            out.append('- Runtime totals: ' + ', '.join(f'{k} {v}' for k, v in totals.items()))
        out.append(f'- Served natively ({len(link["native"])}): ' + (', '.join(link['native']) or 'none'))
        out.append(f'- Served by the port, not linked ({len(link["served"])}): ' + ('; '.join(link['served']) or 'none'))
        out.append(f'- Linked although the port lacks imports ({len(link["although"])}): '
                   + ('; '.join(link['although']) or 'none'))
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('debug_dir', type=Path)
    parser.add_argument('--all', action='store_true', help='read every folder, not the newest per title')
    parser.add_argument('--out', type=Path, help='write the Markdown here instead of stdout')
    parser.add_argument('--title', help='only this title ID, e.g. CUSA16458')
    args = parser.parse_args()
    if not args.debug_dir.is_dir():
        print(f'error: {args.debug_dir} is not a directory', file=sys.stderr)
        return 2
    folders = find_folders(args.debug_dir)
    if args.title:
        folders = [f for f in folders if f['title'] == args.title]
    if not folders:
        print('No debug run folders found (<host>/<YYYYmmdd-HHMMSS>-<TITLE_ID>/).')
        return 0
    folders.sort(key=lambda f: (f['stamp'], f['host']))
    newest = {f['title']: f for f in folders}
    runs_all = defaultdict(int)
    for f in folders:
        runs_all[f['title']] += 1
    if not args.all:
        folders = list(newest.values())
        runs_all = dict.fromkeys(newest, 1)
    text = render(folders, newest, runs_all)
    if args.out:
        try:
            args.out.write_text(text, encoding='utf-8')
        except OSError as err:
            print(f'error: cannot write {args.out}: {err}', file=sys.stderr)
            return 2
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
