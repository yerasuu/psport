#!/usr/bin/env python3
"""Bloodborne's game files: every DFLT .dcx file under dvdroot_ps4 that does not unpack, listed.

An extraction tool that leaves parts of compressed sectors stale (LibOrbisPkg PkgTool on .NET 6 or
newer, issue #81) makes the game hang while it loads them, with no error. Bloodborne's launch checks
its shaders (gpu/games/bloodborne/game_check.cpp); this checks every file: dcx_check.py GAME_DIR.
"""
import struct
import sys
import zlib
from pathlib import Path


def dcx_broken(path):
    """True when a DFLT .dcx file does not unpack to its stated size or fails zlib's checksum.
    Other compressions are not checked."""
    data = Path(path).read_bytes()
    if data[:4] != b'DCX\0' or data[0x18:0x1C] != b'DCS\0' or data[0x28:0x2C] != b'DFLT':
        return False
    size, packed = struct.unpack_from('>II', data, 0x1C)
    try:
        return len(zlib.decompress(data[0x4C:0x4C + packed])) != size
    except zlib.error:
        return True


def main(argv):
    if len(argv) != 2:
        print('Usage: dcx_check.py GAME_DIR', file=sys.stderr)
        return 2
    game = Path(argv[1])
    files = sorted((game / 'dvdroot_ps4').rglob('*.dcx'))
    broken = []
    for number, path in enumerate(files, 1):
        if dcx_broken(path):
            broken.append(path)
            print(f'damaged: {path.relative_to(game)}')
        if number % 1000 == 0:
            print(f'{number} of {len(files)} checked', file=sys.stderr)
    print(f'{len(broken)} of {len(files)} .dcx files do not unpack')
    return 1 if broken else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
