"""Diagnostic: name the import bound to a GOT slot (eboot offsets) of the game's eboot.

Usage: got_import.py GAME/eboot.bin OFFSET...
"""
import re, struct, sys
from pathlib import Path
here = Path(__file__).resolve().parent.parent  # repository root
sys.path.insert(0, str(here / 'scripts'))
from sce_file import parse_self, span, unpack

elf, header, ph, _, _ = parse_self(Path(sys.argv[1]).read_bytes())
dynamic = next(p for p in ph if p['type'] == 2)
tags = {}
for pos in range(dynamic['offset'], dynamic['offset'] + dynamic['filesz'], 16):
    tag, value = unpack('<QQ', elf, pos)
    if tag == 0:
        break
    tags[tag] = value
lib = next(p for p in ph if p['type'] == 0x61000000)
blob = span(elf, lib['offset'], lib['filesz'])
strings = span(blob, tags[0x61000035], tags[0x61000037])
syms = span(blob, tags[0x61000039], tags[0x6100003f])
names, sections = [], []
for pos in range(0, len(syms), 24):
    name, info, other, shndx, value, size = unpack('<IBBHQQ', syms, pos)
    names.append(strings[name:strings.index(0, name)].decode('ascii'))
    sections.append(shndx)
slots = {}
for offset_tag, size_tag in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
    table = span(blob, tags[offset_tag], tags[size_tag])
    for pos in range(0, len(table), 24):
        target, info, addend = unpack('<QQq', table, pos)
        kind, sym = info & 0xffffffff, info >> 32
        if kind in (1, 6, 7) and not sections[sym]:
            slots[target] = names[sym]
symbols = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', (here / 'src/import_names_all.inc').read_text()))
for arg in sys.argv[2:]:
    name = slots.get(int(arg, 16))
    print(arg, name, symbols.get(name.split('#')[0]) if name else '')
