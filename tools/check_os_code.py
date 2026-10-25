#!/usr/bin/env python3
"""Finds OS-specific code outside the platform layer (decision #1009, kb #1011).

    python3 tools/check_os_code.py [--strict]

OS conditionals (_WIN32, __linux__, __APPLE__, __unix__, _MSC_VER, __MINGW32__ in #if, #ifdef, #ifndef,
#elif) and includes of OS or windowing headers (windows.h, winsock2.h, unistd.h, sys/*, fcntl.h, ftw.h,
dlfcn.h, SDL3/*) belong in src/platform/ and gpu/platform/<os>/, behind ps_host_*. The default mode
prints every finding as path:line: text with per-file counts and exits 0; --strict exits 1 while any
finding remains, to become blocking as each phase of the platform layer lands.
"""
import re
import sys
from pathlib import Path

ROOTS = ('src', 'gpu')
ALLOWED = ('src/platform', 'gpu/platform')
# Vendored or inherited code, kept as upstream wrote it (FORK.md, gpu/VENDOR.txt).
EXEMPT = ('gpu/third_party', 'gpu/shadps4', 'gpu/net/shadps4')
SUFFIXES = {'.c', '.h', '.inc', '.cpp', '.hpp', '.cc', '.cxx', '.hh', '.ipp'}

MACROS = r'_WIN32|__linux__|__APPLE__|__unix__|_MSC_VER|__MINGW32__'
CONDITIONAL = re.compile(rf'^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b(?:{MACROS})\b')
HEADERS = r'windows\.h|winsock2\.h|unistd\.h|sys/[^>"]+|fcntl\.h|ftw\.h|dlfcn\.h|SDL3/[^>"]+'
INCLUDE = re.compile(rf'^\s*#\s*include\s*[<"](?:{HEADERS})[>"]')


def under(path, prefixes):
    return any(path == p or path.startswith(p + '/') for p in prefixes)


def scan(root):
    found = {}
    for p in sorted(Path(root).rglob('*')):
        rel = p.as_posix()
        if p.suffix not in SUFFIXES or not p.is_file() or under(rel, ALLOWED + EXEMPT):
            continue
        for n, line in enumerate(p.read_text(errors='replace').splitlines(), 1):
            if CONDITIONAL.match(line):
                found.setdefault(rel, []).append((n, 'conditional', line.strip()))
            elif INCLUDE.match(line):
                found.setdefault(rel, []).append((n, 'include', line.strip()))
    return found


def main():
    strict = '--strict' in sys.argv[1:]
    if any(a != '--strict' for a in sys.argv[1:]):
        print(__doc__, file=sys.stderr)
        return 2
    print('scanned:', ', '.join(ROOTS))
    print('allowed:', ', '.join(ALLOWED))
    print('exempt: ', ', '.join(EXEMPT))
    found = {}
    for root in ROOTS:
        found.update(scan(root))
    kinds = {'conditional': 0, 'include': 0}
    for rel, items in found.items():
        for n, kind, text in items:
            print(f'{rel}:{n}: {text}')
            kinds[kind] += 1
    print()
    for rel, items in found.items():
        print(f'{len(items):4}  {rel}')
    total = sum(kinds.values())
    print(f'total: {total} findings in {len(found)} files '
          f'({kinds["conditional"]} conditionals, {kinds["include"]} includes)')
    return 1 if strict and total else 0


sys.exit(main())
