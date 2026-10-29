#!/usr/bin/env python3
"""Writes THIRD-PARTY-NOTICES and SOURCES.txt from licences.nix's manifest.

    python3 licences.py MANIFEST SPDX_TEXT_DIR OUT_DIR

A package's licence files are the COPYING, LICENSE, COPYRIGHT and NOTICE files at the top of its
source (and in LICENSES/ and docs/), which carry its copyright lines; the plain SPDX text stands in
only when the source has none. A text already printed for another component is named instead of
repeated. Fails, writing nothing, when a GPL, LGPL or MPL component ends up with no licence text or
no URL for its source, since then the package can't honour those licences.
"""
import hashlib
import json
import re
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath

# gcc keeps the GPL-3.0 its runtime exception refers to in COPYING3, gmp its texts in COPYINGv2/v3.
LICENCE_FILE = re.compile(r'(?i)^(copying|licen[cs]e|copyright|notice|unlicense)(v?\d.*|[._-].*)?$')
COPYLEFT = re.compile(r'(?<![A-Za-z])(A?L?GPL|MPL)')
OFFER = """\
Sources of the GPL, LGPL and MPL components in this package

psport itself (GPL-2.0, see LICENSE) is at https://github.com/yerasuu/psport, at the tag of the
release this package belongs to; that tag's shell.nix, packaging/port.nix and packaging/nixpkgs.nix
pin every library below to one nixpkgs revision.

Written offer (GNU GPL version 2, section 3(b); GNU LGPL version 2.1, section 6(c)): for at least
three years from the day this package was published, the psport maintainer will give anyone who
asks, for a charge no more than the cost of physically performing the source distribution, a
complete machine-readable copy of the corresponding source code of psport and of every component
listed below, on a medium customarily used for software interchange. Ask through
https://github.com/yerasuu/psport/issues.

Each component below: its version, licence, the URL its source was fetched from and that source's
hash, as nixpkgs {nixpkgs} built it.
"""

def is_licence(parts):
    if len(parts) == 1:
        return LICENCE_FILE.match(parts[0])
    if len(parts) != 2:
        return False
    return parts[0] == 'LICENSES' or (parts[0] == 'docs' and LICENCE_FILE.match(parts[1]))

def top_level(names):
    paths = [PurePosixPath(name).parts for name in names]
    is_wrapped = len({parts[0] for parts in paths}) == 1 and all(len(parts) > 1 for parts in paths)
    strip = 1 if is_wrapped else 0
    return [(name, parts[strip:]) for name, parts in zip(names, paths, strict=True)]

def licence_files(source):
    if not source:
        return []
    path = Path(source)
    if path.is_dir():
        return [(str(f.relative_to(path)), f.read_bytes()) for f in sorted(path.rglob('*'))
                if f.is_file() and is_licence(f.relative_to(path).parts)]
    try:
        if tarfile.is_tarfile(path):
            with tarfile.open(path) as archive:
                members = [m for m in archive.getmembers() if m.isfile()]
                named = zip(top_level([m.name for m in members]), members, strict=True)
                return [('/'.join(parts), archive.extractfile(m).read()) for (_, parts), m in named
                        if is_licence(parts)]
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as archive:
                names = [n for n in archive.namelist() if not n.endswith('/')]
                return [('/'.join(parts), archive.read(name)) for name, parts in top_level(names)
                        if is_licence(parts)]
    except (tarfile.TarError, zipfile.BadZipFile, OSError, EOFError) as error:
        print(f'licences: {source}: {error}', file=sys.stderr)
    return []

def spdx_texts(licences, spdx):
    texts = []
    for licence in licences:
        for spdx_id in re.findall(r'[A-Za-z0-9.+-]+', licence['spdx']):
            if spdx_id not in ('AND', 'OR', 'WITH') and (spdx / f'{spdx_id}.txt').is_file():
                texts.append((f'SPDX {spdx_id}', (spdx / f'{spdx_id}.txt').read_bytes()))
    return texts

def describe(licences):
    def one(licence):
        if licence['spdx'] and licence['name'] and licence['name'] != licence['spdx']:
            return f"{licence['spdx']} ({licence['name']})"
        return licence['spdx'] or licence['name']
    return '; '.join(one(licence) for licence in licences) or 'not stated by nixpkgs'

def is_copyleft(component):
    licences = component['licences']
    return any(COPYLEFT.search(licence['spdx'] or licence['name']) for licence in licences)

class Notices:
    def __init__(self):
        self.lines, self.printed = [], {}

    def section(self, title):
        self.lines += ['', '=' * 100, title, '=' * 100]

    def component(self, title, fields, texts):
        self.lines += ['', '-' * 100, title, *(f'{k}: {v}' for k, v in fields if v)]
        for name, text in texts:
            digest = hashlib.sha256(text).hexdigest()
            if digest in self.printed:
                self.lines.append(f'{name}: same text as {self.printed[digest]} above.')
                continue
            self.printed[digest] = f'{title}, {name}'
            self.lines += ['', f'--- {name}', text.decode('utf-8', 'replace').rstrip()]

def main():
    manifest = json.loads(Path(sys.argv[1]).read_text())
    spdx, out = Path(sys.argv[2]), Path(sys.argv[3])
    notices = Notices()
    notices.lines += [
        'Third-party notices for psport',
        '',
        'psport is licensed under the GNU General Public License version 2 (LICENSE); some',
        'of its files are GPL-2.0-only (marked in their headers), the rest GPL-2.0-or-later',
        'used as version 2. It includes code from bbport and shadPS4 (GPL-2.0-or-later).',
        'Below, every other component this package carries: the',
        'libraries in lib/ and the code compiled into psport\'s binaries, each with its licence',
        'and the licence files of its source. Where a component offers a choice of licences,',
        'psport uses it under the one compatible with the GPL version 2. Sources: SOURCES.txt.',
    ]
    titles = {
        'library': 'Libraries (lib/)',
        'compiled in': 'Store packages compiled into psport\'s binaries',
        'program': 'Programs shipped alongside psport',
    }
    sources, problems = [], []
    for role, title in titles.items():
        components = sorted((c for c in manifest['components'] if c['role'] == role),
                            key=lambda c: c['name'].lower())
        if not components:
            continue
        notices.section(title)
        for c in components:
            texts = licence_files(c['source']['path']) or spdx_texts(c['licences'], spdx)
            if not texts:
                print(f"licences: {c['name']}: no licence file in its source and no SPDX text",
                      file=sys.stderr)
            url = ' '.join(c['source']['urls'])
            fields = [('Licence', describe(c['licences'])), ('Note', c['note']),
                      ('Homepage', c['homepage']), ('Source', url)]
            notices.component(f"{c['name']} {c['version']}".strip(), fields, texts)
            if is_copyleft(c):
                sources.append(c)
                if not texts:
                    problems.append(f"{c['name']}: {describe(c['licences'])} with no licence text")
                if not c['source']['urls']:
                    problems.append(f"{c['name']}: {describe(c['licences'])} with no source URL")
    if problems:
        listed = '\n  '.join(problems)
        sys.exit(f"licences: the package can't carry these copyleft components:\n  {listed}")
    notices.section('Code from other projects in psport\'s source tree')
    for v in manifest['vendored']:
        texts = []
        for f in v['files']:
            text = Path(f['file']).read_bytes()
            if 'lines' in f:
                text = b''.join(text.splitlines(keepends=True)[:f['lines']])
            texts.append((Path(f['file']).name.split('-', 1)[-1], text))
        notices.component(v['name'], [('Licence', v['spdx']), ('Homepage', v['url'])], texts)
    (out / 'THIRD-PARTY-NOTICES').write_text('\n'.join(notices.lines).lstrip() + '\n')
    lines = [OFFER.format(nixpkgs=manifest['nixpkgs'])]
    for c in sorted(sources, key=lambda c: c['name'].lower()):
        lines += [f"{c['name']} {c['version']}".strip(), f"  Licence: {describe(c['licences'])}"]
        lines += [f'  Source: {url}' for url in c['source']['urls']]
        if c['source']['rev']:
            lines.append(f"  Revision: {c['source']['rev']}")
        if c['source']['hash']:
            lines.append(f"  Hash: {c['source']['hash']}")
        lines.append('')
    (out / 'SOURCES.txt').write_text('\n'.join(lines))

if __name__ == '__main__':
    main()
