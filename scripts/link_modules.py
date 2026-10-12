"""Link the eboot with the game's bundled system modules into one probe image.

Successor of link_libc.py for several modules (default: libc.prx, then
libSceFios2.prx). Every module is placed after the previous one; imports of
all modules share one import table, so the C runtime can override any
function (host contracts first) and otherwise binds the native export. The other modules the game
bundles in sce_module (libSceNpToolkit2, libSceJobManager, ...) are linked the same way once anything
linked imports their library.

Exports are matched by (NID, library identity, module identity). Imports of
libSceLibcInternal are served by libc.prx's same-NID exports: the internal
library is not shipped with the game and exposes the same functions.
System modules (--system-modules DIR: the user's own dump of the console's *.sprx, never
shipped) are linked too when anything linked imports their library, and their exports win over
the host runtime's (binding kinds 3 and 4), except for libraries named in --hle, so an own
implementation can stand in for one and be compared with it, and for those the port implements
(PORT_IMPLEMENTS) unless --lle names them.
Modules the game loads itself with sceKernelLoadStartModule (any *.prx outside sce_module and
sce_sys, such as Unity's Media/Modules and Media/Plugins) are linked unstarted: the runtime runs
their initializer when the game asks for the path, and their exports win over the host (binding
kinds 5 and 6), as the PS4 linker rebinds imports once such a module is loaded.
Output: out/boot-linked.bin (format BBPROBE6) and out/link.json.
"""
import collections
import hashlib
import json
from pathlib import Path
import struct
from prepare import parse_self, span, unpack
from link_libc import encode_id

DEFAULT_MODULES = ('libc.prx', 'libSceFios2.prx')
# Served by the game's libc.prx, whose functions the runtime partly replaces to track the game's
# writes into GPU memory (memcpy, memset): a native copy of this one would bypass that.
NEVER_SYSTEM = {'libSceLibcInternal'}
# Libraries the port implements itself (the runtime's kernel, GPU, video, pad and audio output; the
# ported gpu/fiber, gpu/font, gpu/audiodec and gpu/ngs2): their system module is linked only when
# --lle names it, to compare the two.
PORT_IMPLEMENTS = {'libkernel', 'libkernel_unity', 'libkernel_sys', 'libSceGnmDriver', 'libSceVideoOut',
                   'libScePad', 'libSceAudioOut', 'libSceFiber', 'libSceFont', 'libSceFontFt',
                   'libSceAudiodec', 'libSceNgs2'}
FS_LOAD = bytes.fromhex('64488b042500000000')  # mov rax, fs:[0]
MAX_MODULE_SEGMENTS = 4  # SCE_DBG_MAX_SEGMENTS of sceKernelGetModuleInfo


def loadable_modules(game):
    """Modules the game itself loads by path, relative to /app0."""
    return [f.relative_to(game).as_posix() for f in sorted(game.rglob('*'))
            if f.suffix.lower() in ('.prx', '.sprx') and f.is_file()
            and f.relative_to(game).parts[0] not in ('sce_module', 'sce_sys')]


def eh_frame(m, base):
    """PT_GNU_EH_FRAME and the .eh_frame it points at, for sceKernelGetModuleInfoForUnwind."""
    hdr = next((p for p in m['ph'] if p['type'] == 0x6474e550), None)
    if not hdr or hdr['filesz'] < 8:
        return 0, 0, 0, 0
    data = span(m['elf'], hdr['offset'], 8)
    if data[0] != 1 or data[1] != 0x1b:  # version 1, eh_frame_ptr DW_EH_PE_pcrel|sdata4
        return base + hdr['vaddr'], hdr['memsz'], 0, 0
    frame = hdr['vaddr'] + 4 + struct.unpack_from('<i', data, 4)[0]
    loads = [p for p in m['ph'] if p['type'] == 1]
    end = max(p['vaddr'] + p['memsz'] for p in loads)
    # As shadPS4: .eh_frame_hdr follows .eh_frame, else .eh_frame runs to the image end.
    size = hdr['vaddr'] - frame if hdr['vaddr'] > frame else end - frame
    return base + hdr['vaddr'], hdr['memsz'], base + frame, max(size, 0)


def module_segments(m, base):
    return [(base + p['vaddr'], p['memsz'], p['flags']) for p in m['ph']
            if p['type'] in (1, 0x61000010)][:MAX_MODULE_SEGMENTS]


def module(path):
    source = path.read_bytes()
    elf, header, ph, _, missing = parse_self(source)
    dp = next(p for p in ph if p['type'] == 2)
    dynamic = []
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        tag, value = unpack('<QQ', elf, pos)
        if not tag:
            break
        dynamic.append((tag, value))
    tags = dict(dynamic)
    lp = next(p for p in ph if p['type'] == 0x61000000)
    blob = span(elf, lp['offset'], lp['filesz'])
    strings = span(blob, tags[0x61000035], tags[0x61000037])

    def string(offset):
        span(strings, offset, 1)
        return strings[offset:strings.index(0, offset)].decode('ascii')
    libraries, modules = {}, {}
    for tag, value in dynamic:
        if tag in (0x61000013, 0x61000015, 0x6100000d, 0x6100000f):
            table = libraries if tag in (0x61000013, 0x61000015) else modules
            key = encode_id(value >> 48)
            identity = (string(value & 0xffffffff), (value >> 32) & 65535)
            if key in table and table[key] != identity:
                raise ValueError('conflicting module/library IDs')
            table[key] = identity

    def identity(name):
        parts = name.split('#')
        if len(parts) != 3:
            return None  # plain names such as module_start are not linkable
        n, lib, mod = parts
        return (n, libraries[lib], modules[mod])
    symbols = []
    table = span(blob, tags[0x61000039], tags[0x6100003f])
    for pos in range(0, len(table), 24):
        n, info, other, section, value, size = unpack('<IBBHQQ', table, pos)
        name = string(n)
        symbols.append(dict(name=name, type=info & 15, binding=info >> 4, section=section,
                            value=value, size=size, identity=identity(name) if name else None))
    relocations = []
    for ot, st in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        table = span(blob, tags[ot], tags[st])
        for pos in range(0, len(table), 24):
            target, info, addend = unpack('<QQq', table, pos)
            relocations.append((target, info & 0xffffffff, info >> 32, addend))
    return dict(elf=elf, header=header, ph=ph, tags=tags, symbols=symbols, relocs=relocations,
                identity=identity, libraries=libraries, modules=modules,
                sha256=hashlib.sha256(source).hexdigest(), missing=missing)


def patch_fs_loads(image, ph, base):
    """Rewrite initial-exec `mov rax, fs:[0]` to GS: glibc owns FS on Linux."""
    patched = 0
    for p in ph:
        if p['type'] != 1 or not p['flags'] & 1:
            continue
        start, end = base + p['vaddr'], base + p['vaddr'] + p['filesz']
        at = image.find(FS_LOAD, start, end)
        while at >= 0:
            image[at] = 0x65
            patched += 1
            at = image.find(FS_LOAD, at + len(FS_LOAD), end)
    return patched


def link(game, out, module_names=DEFAULT_MODULES, system_modules=None, hle=(), lle=()):
    main = module(game / 'eboot.bin')
    raw = (out / 'boot.bin').read_bytes()
    magic, size, entry, ns, nr, ni, flags = unpack('<8s6Q', raw, 0)
    if magic != b'BBPROBE2':
        raise ValueError('linker expects freshly prepared BBPROBE2')
    pos = 56
    segments = [unpack('<3Q', raw, pos + i * 24) for i in range(ns)]
    pos += ns * 24
    names = [span(raw, pos + i * 128, 128).split(b'\0', 1)[0].decode() for i in range(ni)]
    pos += ni * 128
    relocs = [unpack('<QQqq', raw, pos + i * 32) for i in range(nr)]
    pos += nr * 32
    image = bytearray(span(raw, pos, size))
    if pos + size != len(raw):
        raise ValueError('unexpected boot trailer')

    identities = {main['identity'](name): i for i, name in enumerate(names)}
    main_libraries = {identity: key for key, identity in main['libraries'].items()}
    main_modules = {identity: key for key, identity in main['modules'].items()}

    def imported(symbol):
        key = symbol['identity']
        if key not in identities:
            n, lib, mod = key
            # Main-program spelling when the identity matches (existing host
            # tables use it); otherwise NID#library for new libraries.
            name = (f'{n}#{main_libraries[lib]}#{main_modules[mod]}'
                    if lib in main_libraries and mod in main_modules else f'{n}#{lib[0]}')
            identities[key] = len(names)
            names.append(name)
        return identities[key]

    exports, by_nid = {}, collections.defaultdict(list)
    table = []
    base = (size + 65535) & ~65535
    fs_patched = patch_fs_loads(image, main['ph'], 0)
    tls_module = 2
    module_names = list(module_names)
    system = {}
    if system_modules:
        system = {f.stem: f for f in sorted(Path(system_modules).glob('*.sprx'))
                  if f.stem not in NEVER_SYSTEM and f.stem not in hle
                  and (f.stem not in PORT_IMPLEMENTS or f.stem in lle)}
    from_system = set()

    bundled_modules = {f.stem: f.name for f in sorted((game / 'sce_module').glob('*.prx'))}

    def wanted():
        """Bundled and system modules whose library something linked imports and nothing linked provides."""
        imported_libraries = {identity[1][0] for identity in identities if identity}
        linked = {Path(name).stem for name in module_names}
        return [lib for lib in {**system, **bundled_modules}
                if lib in imported_libraries and lib not in linked]

    def link_wanted():
        for lib in wanted():
            if lib in bundled_modules:
                module_names.append(bundled_modules[lib])
            else:
                module_names.append(system[lib].name)
                from_system.add(system[lib].name)
    link_wanted()
    deferred = loadable_modules(game)
    module_names += deferred
    deferred = set(deferred)
    index = 0
    while index < len(module_names):
        filename = module_names[index]
        index += 1
        bundled = game / 'sce_module' / filename
        if filename in deferred:
            bundled = game / filename
        m = module(bundled if bundled.exists() or filename not in from_system else system[Path(filename).stem])
        loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
        modsize = max(p['vaddr'] + p['memsz'] for p in loads)
        if base + modsize > 512 * 1024 * 1024:
            raise ValueError('linked image exceeds probe limit')
        image.extend(bytes(base + modsize - len(image)))

        def mapped(address, length=8, loads=loads):
            return any(p['vaddr'] <= address and address + length <= p['vaddr'] + p['memsz'] for p in loads)
        for p in loads:
            if p['filesz'] > p['memsz']:
                raise ValueError(f'{filename}: invalid segment size')
            image[base + p['vaddr']:base + p['vaddr'] + p['filesz']] = span(m['elf'], p['offset'], p['filesz'])
            segments.append((base + p['vaddr'], p['memsz'], p['flags']))
        tls = next((p for p in m['ph'] if p['type'] == 7), None)
        module_id = 0
        # A PT_TLS of no size needs no module id: ids are what the runtime keeps a block per
        # thread for, and titles with many modules (Atelier Ryza, 11) mostly have empty ones.
        if tls and tls['memsz']:
            if tls['filesz'] > tls['memsz'] or not mapped(tls['vaddr'], tls['memsz']):
                raise ValueError(f'{filename}: unsupported TLS layout')
            module_id = tls_module
            tls_module += 1
        tls_relocations = 0
        for target, kind, symid, addend in m['relocs']:
            if not mapped(target):
                raise ValueError(f'{filename}: relocation outside image {target:#x}')
            target += base
            if kind == 8:
                relocs.append((target, 0, base + addend, 0))
            elif kind == 16:
                symbol = m['symbols'][symid]
                if symbol['name'] or symbol['type'] != 3 or addend or not module_id:
                    raise ValueError(f'{filename}: unsupported external TLS relocation')
                struct.pack_into('<Q', image, target, module_id)
                tls_relocations += 1
            elif kind in (1, 6, 7):
                symbol = m['symbols'][symid]
                if symbol['section']:
                    if symbol['section'] == 0xfff1:
                        struct.pack_into('<Q', image, target, symbol['value'] + addend)
                    else:
                        relocs.append((target, 0, base + symbol['value'] + addend, 0))
                elif not symbol['identity'] and symbol['binding'] == 2:
                    struct.pack_into('<Q', image, target, 0)  # weak undefined (module_start/stop)
                else:
                    rk = 2 if symbol['type'] == 1 else 1
                    if (rk == 1 and addend) or not 0 <= addend < 4096:
                        raise ValueError(f'{filename}: unsupported import addend')
                    relocs.append((target, rk, imported(symbol), addend))
            else:
                raise ValueError(f'{filename}: unsupported relocation type {kind}')
        own_exports = []
        for s in m['symbols']:
            if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2):
                if not mapped(s['value'], max(1, s['size'])):
                    raise ValueError(f'{filename}: export outside load segments')
                if s['identity'] in exports:
                    raise ValueError(f'{filename}: ambiguous export')
                kind = 2 if s['type'] == 1 else 1
                kind += 4 if filename in deferred else 2 if filename in from_system else 0
                entry_value = (base + s['value'], kind, filename)
                exports[s['identity']] = entry_value
                by_nid[(s['identity'][0], s['identity'][1][0])].append(entry_value)
                own_exports.append((s['identity'][0], base + s['value']))
        fs_patched += patch_fs_loads(image, m['ph'], base)
        table.append(dict(file=filename, base=base, size=modsize, init=base + m['tags'].get(12, 0),
                          tls_address=base + tls['vaddr'] if tls else 0, tls_memsz=tls['memsz'] if tls else 0,
                          tls_filesz=tls['filesz'] if tls else 0, tls_module=module_id,
                          tls_relocations=tls_relocations, exports=len(own_exports), sha256=m['sha256'],
                          deferred=filename in deferred, eh_frame=eh_frame(m, base),
                          segments=module_segments(m, base), export_list=own_exports))
        base = (base + modsize + 65535) & ~65535
        # Linking a module can import a library only another bundled or system module provides.
        link_wanted()

    bindings, unresolved = [], []
    for identity, index in identities.items():
        found = exports.get(identity)
        if not found and identity and identity[1][0] == 'libSceLibcInternal':
            candidates = by_nid.get((identity[0], 'libc'), [])
            found = candidates[0] if len(candidates) == 1 else None
        if found:
            bindings.append((index, found[0], found[1]))
        else:
            unresolved.append(names[index])

    main_tls = next((p for p in main['ph'] if p['type'] == 7), None)
    main_tls_values = (main_tls['vaddr'], main_tls['filesz'], main_tls['memsz'], main_tls['align']) if main_tls else (0, 0, 0, 0)
    if main_tls and (main_tls['filesz'] > main_tls['memsz'] or main_tls['memsz'] > 1024 * 1024
                     or main_tls['vaddr'] + main_tls['filesz'] > size):
        raise ValueError('unsupported eboot TLS layout')
    procparam = next(p for p in main['ph'] if p['type'] == 0x61000001)
    with (out / 'boot-linked.bin').open('wb') as f:
        f.write(struct.pack('<8s6Q', b'BBPROBE6', len(image), entry, len(segments), len(relocs), len(names), flags))
        f.write(struct.pack('<Q', procparam['vaddr']))
        f.write(struct.pack('<4Q', *main_tls_values))

        def described(name, eh, module_segs, export_list):
            f.write(name.encode().ljust(128, b'\0'))
            f.write(struct.pack('<4Q', *eh))
            f.write(struct.pack('<Q', len(module_segs)))
            for i in range(MAX_MODULE_SEGMENTS):
                f.write(struct.pack('<3Q', *(module_segs[i] if i < len(module_segs) else (0, 0, 0))))
            f.write(struct.pack('<Q', len(export_list)))
            for nid, address in export_list:
                f.write(struct.pack('<16sQ', nid.encode(), address))
        # Unity's IL2CPP code asks the eboot (sceKernelDlsym handle 0) for the player's functions.
        main_exports = [(s['identity'][0], s['value']) for s in main['symbols']
                        if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2)]
        described('eboot.bin', eh_frame(main, 0), module_segments(main, 0), main_exports)
        f.write(struct.pack('<Q', len(table)))
        for t in table:
            if len(t['file'].encode()) >= 128:
                raise ValueError(f"{t['file']}: path too long")
            f.write(struct.pack('<8Q', t['base'], t['size'], t['init'], t['tls_address'], t['tls_memsz'],
                                t['tls_filesz'], t['tls_module'], int(t['deferred'])))
            described(t['file'], t['eh_frame'], t['segments'], t['export_list'])
        f.write(struct.pack('<Q', len(bindings)))
        for binding in bindings:
            f.write(struct.pack('<3Q', *binding))
        for segment in segments:
            f.write(struct.pack('<3Q', *segment))
        for name in names:
            if len(name.encode()) >= 128:
                raise ValueError('name too long')
            f.write(name.encode().ljust(128, b'\0'))
        for relocation in relocs:
            f.write(struct.pack('<QQqq', *relocation))
        f.write(image)
    report = dict(modules=[{k: (hex(v) if k in ('base', 'init', 'tls_address') else v) for k, v in t.items()
                            if k not in ('eh_frame', 'segments', 'export_list')} for t in table],
                  bindings=len(bindings), imports=len(names), fs_loads_patched=fs_patched,
                  main_tls=dict(zip(('vaddr', 'filesz', 'memsz', 'align'), main_tls_values)),
                  unresolved_imports=unresolved)
    (out / 'link.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    summary = ', '.join(f"{t['file']}@{t['base']:#x}{' (loaded by the game)' if t['deferred'] else ''}" for t in table)
    print(f'Linked modules: {summary}; {len(bindings)} native bindings, {len(unresolved)} imports left to the host runtime, '
          f'fs->gs patched={fs_patched}')


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))
    p.add_argument('--system-modules', type=Path, help="the user's dump of the console's *.sprx")
    p.add_argument('--hle', default='', help='libraries left to the host runtime, comma-separated')
    p.add_argument('--lle', default='', help="system modules linked although the port implements them")
    a = p.parse_args()
    names = lambda text: {n.strip() for n in text.split(',') if n.strip()}
    link(a.game, a.out, a.modules, a.system_modules, names(a.hle), names(a.lle))
