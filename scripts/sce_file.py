#!/usr/bin/env python3
"""PS4 file formats the remaining launch scripts read: SELF/ELF (the eboot's segments, for patches)
and param.sfo. The game itself is loaded by bb-probe (src/loader_*.c).

No decryption and no Python dependencies. All offsets are checked before use.
"""
import base64
import hashlib
import struct


def span(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"out of bounds: {offset:#x}+{size:#x} / {len(data):#x}")
    return data[offset:offset + size]


def unpack(fmt, data, offset):
    return struct.unpack(fmt, span(data, offset, struct.calcsize(fmt)))


def parse_elf(data):
    """A plain ELF (some dumpers write the executable without the SELF wrapper)."""
    header = unpack('<16sHHIQQQIHHHHHH', data, 0)
    if header[0][:7] != b'\x7fELF\x02\x01\x01' or header[2] != 62:
        raise ValueError("expected little-endian x86-64 ELF")
    if header[9] != 56 or not 0 < header[10] < 256:
        raise ValueError("unsupported program headers")
    ph = [dict(zip(('type', 'flags', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'align'),
                   unpack('<IIQQQQQQ', data, header[5] + i * 56)))
          for i in range(header[10])]
    end = max(header[5] + header[10] * 56, max(p['offset'] + p['filesz'] for p in ph))
    if end > 512 * 1024 * 1024:
        raise ValueError("probe image exceeds 512 MiB limit")
    return bytearray(span(data, 0, end)), header, ph, [], []


def parse_self(data):
    if span(data, 0, 4) == b'\x7fELF':
        return parse_elf(data)
    if span(data, 0, 4) != b'O\x15=\x1d':
        raise ValueError("expected PS4 SELF or ELF (eboot.bin is neither: an encrypted or damaged dump?)")
    count, = unpack('<H', data, 24)
    base = 32 + count * 32
    header = unpack('<16sHHIQQQIHHHHHH', data, base)
    if header[0][:7] != b'\x7fELF\x02\x01\x01' or header[2] != 62:
        raise ValueError("expected little-endian x86-64 ELF")
    if header[9] != 56 or not 0 < header[10] < 256:
        raise ValueError("unsupported program headers")
    ph = [dict(zip(('type', 'flags', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'align'),
                   unpack('<IIQQQQQQ', data, base + header[5] + i * 56)))
          for i in range(header[10])]
    end = max(p['offset'] + p['filesz'] for p in ph)
    if end > 512 * 1024 * 1024:
        raise ValueError("probe image exceeds 512 MiB limit")
    elf = bytearray(end)
    header_end = header[5] + header[10] * 56
    elf[:header_end] = span(data, base, header_end)
    covered = [(0, header_end)]
    segments = []
    for i in range(count):
        flags, off, size, mem = unpack('<QQQQ', data, 32 + i * 32)
        segments.append(dict(flags=hex(flags), offset=off, size=size))
        if not flags & 0x800:
            continue
        if flags & 10:
            raise ValueError("encrypted/compressed SELF segment is unsupported")
        index = (flags >> 20) & 4095
        if index >= len(ph):
            raise ValueError("invalid segment index")
        p = ph[index]
        if size != p['filesz'] or size != mem:
            raise ValueError("unsupported blocked segment layout")
        elf[p['offset']:p['offset'] + size] = span(data, off, size)
        covered.append((p['offset'], p['offset'] + size))
    missing = []
    for i, p in enumerate(ph):
        if p['filesz'] and not any(a <= p['offset'] and p['offset'] + p['filesz'] <= z
                                  for a, z in covered):
            missing.append(i)
            if p['type'] in (1, 2, 0x61000000, 0x61000010):
                raise ValueError(f"required segment {i} is unavailable")
    return elf, header, ph, segments, missing


def sfo(data):
    magic, version, keys, values, count = unpack('<4sIIII', data, 0)
    if magic != b'\0PSF':
        raise ValueError('bad SFO signature')
    result = {}
    for i in range(count):
        key, fmt, size, capacity, off = unpack('<HHIII', data, 20 + i * 16)
        k = data[keys + key:data.index(0, keys + key)].decode()
        v = span(data, values + off, size)
        result[k] = struct.unpack('<I', v)[0] if fmt == 0x404 else v.rstrip(b'\0').decode('utf8', 'replace')
    return result


def nid(name):
    salt = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
    digest = hashlib.sha1(name.encode('ascii') + salt).digest()[:8][::-1]
    return base64.b64encode(digest).decode().rstrip('=').replace('/', '-')
