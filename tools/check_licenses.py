#!/usr/bin/env python3
"""Checks LICENSES.json against the tree (kb #1072).

    python3 tools/check_licenses.py [--strict]

Verifies that the manifest parses and has the schema, that every component path exists, that every SPDX
identifier in a licence expression has LICENSES/<id>.txt and every LicenseRef is defined, that every
vendored tree (gpu/third_party/*, third_party/*, the directories gpu/VENDOR.txt names) is covered by a
component, and that every tracked file is covered by a component or is on the tooling list. Review states
other than "reviewed" and "generated" are listed. The default mode prints every finding and exits 0, like
check_os_code.py, so the check can run in the night report before it blocks; --strict exits 1 while any
finding remains. Needs git, no network.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REVIEWS = {'reviewed', 'pending', 'generated'}
KEYS = {'name', 'paths', 'license', 'review', 'provenance'}
# Tracked files that carry or state the licences themselves, or that configure the repository.
TOOLING = ('.githooks', '.gitignore', '.gitmodules', 'LICENSE', 'LICENSES', 'LICENSES.json', 'LICENSES.md',
           'CLAUDE.md', 'VERSION')
# Trees whose directories are components of their own.
VENDOR_PARENTS = ('gpu/third_party', 'third_party')
TOKEN = re.compile(r'\(|\)|[A-Za-z0-9.+:-]+')
OPERATORS = {'AND', 'OR', 'WITH'}


def under(path, prefixes):
    return any(path == p or path.startswith(p + '/') for p in prefixes)


def spdx_ids(expression):
    """Returns (ids, refs) of an SPDX expression; the identifier after WITH is an exception, not a licence."""
    ids, refs, previous = set(), set(), None
    for token in TOKEN.findall(expression):
        if token in '()' or token in OPERATORS:
            previous = token
        elif previous == 'WITH':
            previous = token
        elif token.startswith('LicenseRef-'):
            refs.add(token)
            previous = token
        else:
            ids.add(token.rstrip('+'))
            previous = token
    return ids, refs


def check_schema(manifest):
    problems = []
    for key in ('schema_version', 'review_date', 'original_code_license', 'license_references', 'components'):
        if key not in manifest:
            problems.append(f'LICENSES.json: no "{key}"')
    if manifest.get('schema_version') != 1:
        problems.append(f'LICENSES.json: schema_version is {manifest.get("schema_version")!r}, expected 1')
    if not isinstance(manifest.get('license_references'), dict):
        problems.append('LICENSES.json: license_references is not an object')
    components = manifest.get('components')
    if not isinstance(components, list):
        problems.append('LICENSES.json: components is not a list')
    return problems


def check_component(c, references, tracked):
    name = c.get('name', '?')
    problems = []
    missing = KEYS - c.keys()
    if missing:
        return [f'{name}: no {", ".join(sorted(missing))}']
    if c['review'] not in REVIEWS:
        problems.append(f'{name}: review {c["review"]!r} is not one of {", ".join(sorted(REVIEWS))}')
    if not c['provenance'].strip():
        problems.append(f'{name}: empty provenance')
    for path in c['paths']:
        if not (ROOT / path).exists() and path not in tracked:
            problems.append(f'{name}: path {path} does not exist')
    ids, refs = spdx_ids(c['license'])
    for spdx_id in sorted(ids):
        if not (ROOT / 'LICENSES' / f'{spdx_id}.txt').is_file():
            problems.append(f'{name}: no LICENSES/{spdx_id}.txt')
    for ref in sorted(refs):
        if ref not in references:
            problems.append(f'{name}: {ref} is not defined in license_references')
    if not ids and not refs:
        problems.append(f'{name}: license {c["license"]!r} names no licence')
    return problems


def vendored_trees():
    trees = []
    for parent in VENDOR_PARENTS:
        base = ROOT / parent
        if base.is_dir():
            trees += [f'{parent}/{d.name}' for d in sorted(base.iterdir()) if d.is_dir()]
    for line in (ROOT / 'gpu' / 'VENDOR.txt').read_text().splitlines():
        if line.startswith('Directories:'):
            trees.append('gpu/shadps4')
            trees += [f'gpu/third_party/{n}' for n in re.findall(r'externals/(\w+)', line)]
    return sorted(set(trees))


def tracked_files():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z'], capture_output=True, check=True)
    return [p for p in out.stdout.decode().split('\0') if p]


def main():
    strict = '--strict' in sys.argv[1:]
    if any(a != '--strict' for a in sys.argv[1:]):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        manifest = json.loads((ROOT / 'LICENSES.json').read_text())
    except (OSError, ValueError) as error:
        print(f'LICENSES.json: {error}')
        return 1
    problems = check_schema(manifest)
    components = manifest.get('components') if isinstance(manifest.get('components'), list) else []
    references = manifest.get('license_references') if isinstance(manifest.get('license_references'), dict) else {}
    files = tracked_files()
    tracked_dirs = set(files) | {'/'.join(p.split('/')[:i]) for p in files for i in range(1, p.count('/') + 1)}
    for c in components:
        problems += check_component(c, references, tracked_dirs)
    paths = [p for c in components if isinstance(c, dict) for p in c.get('paths', ())]
    for tree in vendored_trees():
        if not under(tree, paths) and not any(p.startswith(tree + '/') for p in paths):
            problems.append(f'{tree}: vendored tree not covered by a component')
    uncovered = sorted({'/'.join(f.split('/')[:2]) for f in files
                        if not under(f, paths) and not under(f, TOOLING)})
    problems += [f'{u}: tracked but covered by no component' for u in uncovered]
    used = {ref for c in components if isinstance(c, dict) for ref in spdx_ids(c.get('license', ''))[1]}
    problems += [f'{ref}: defined in license_references but used by no component'
                 for ref in sorted(set(references) - used)]
    for p in problems:
        print(p)
    pending = [c['name'] for c in components if isinstance(c, dict) and c.get('review') == 'pending']
    for name in pending:
        print(f'pending: {name}')
    print(f'licenses: {len(components)} components, {len(files)} tracked files, {len(problems)} findings, '
          f'{len(pending)} pending')
    return 1 if strict and problems else 0


sys.exit(main())
