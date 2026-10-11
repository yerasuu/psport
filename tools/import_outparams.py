#!/usr/bin/env python3
"""Generates src/import_outparams.inc: the PS4 exports whose functions write through a pointer
argument, from shadPS4's prototypes and their LIB_FUNCTION registrations.

    python3 tools/import_outparams.py <shadPS4 checkout> > src/import_outparams.inc

BB_PERMISSIVE answers an import the port lacks with 0; for these the caller then reads an output
that was never written, and faults far from the import. An argument counts as an output when it is
a non-const pointer that shadPS4's body writes through (itself or by handing it to a function that
does), or whose name or type says it is one (out, result, handle, id, a pointer to a pointer, a
pointer to a scalar that is not a buffer).
"""
import re, sys
from pathlib import Path

REGISTRATION = re.compile(r'LIB_FUNCTION\(\s*"([^"]+)"\s*,[^;]*?,\s*([^,;]+?)\s*\)\s*;', re.S)
PROTOTYPE = re.compile(r'PS4_SYSV_ABI\s+([A-Za-z_][\w:]*)\s*\(')
SCALAR = re.compile(r'^(?:unsigned\s+|signed\s+)?(?:[su](?:8|16|32|64)|u?int(?:\d+_t)?|long|size_t|ssize_t|'
                    r'off_t|bool|float|double|f32|f64|char|uintptr_t|\w*(?:UserId|Handle|Off|Size|Time))$')
OUT_WORDS = {'out', 'result', 'results', 'res', 'ret', 'retval', 'handle'}
OUT_SUFFIX = re.compile(r'(?:info|status|stat|state|time|size|count|num|len|length|'
                        r'mode|flags?|value|val|version|offset|pos)$', re.I)
# Pointers these names carry are inputs or caller-owned buffers the function fills only when it
# says so in its name or body; counting them would flag every write(2)-like call.
INPUT_NAME = re.compile(r'^(?:p?(?:buf|buffer|data|src|source|input|in|param|params|arg|args|argv|'
                        r'cmd|cmds|list|array|ptr|p|userdata|user_data|udata|context|ctx|desc|'
                        r'option|options|opt|config|cfg|attr|callback|cb|addr_in))$', re.I)

STRING_NAME = re.compile(r'(?:path|name|file|url|uri|host|dir|title|text|message|msg|format|fmt)$', re.I)


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: ' ' * len(m.group()), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def matching(text, start, open_, close):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == open_:
            depth += 1
        elif text[i] == close:
            depth -= 1
            if not depth:
                return i
    return -1


def split_params(params):
    parts, depth, current = [], 0, ''
    for ch in params:
        if ch in '<([':
            depth += 1
        elif ch in '>)]':
            depth -= 1
        if ch == ',' and not depth:
            parts.append(current)
            current = ''
        else:
            current += ch
    parts.append(current)
    return [p.strip() for p in parts if p.strip() and p.strip() != 'void']


def pointer_param(param):
    """(type, name, depth) of a non-const pointer parameter, else None."""
    param = param.split('=')[0].strip()
    m = re.match(r'^(.*?)([A-Za-z_]\w*)\s*(\[\s*\w*\s*\])?$', param)
    if not m:
        return None
    type_, name, array = m.group(1).strip(), m.group(2), m.group(3)
    depth = type_.count('*') + (1 if array else 0)
    if not depth or '(' in type_ or '&' in type_:
        return None
    # const anywhere is an input: const T*, and arrays of pointers (const T* x[], T* const*).
    if re.search(r'\bconst\b', type_):
        return None
    base = type_.split('*')[0]
    base = re.sub(r'\b(?:const|volatile|struct|enum)\b', '', base).strip()
    base = base.split('::')[-1]
    return base, name, depth


def writes_through(body, name):
    n = re.escape(name)
    return bool(re.search(rf'(?:\*\s*{n}\s*(?:\[[^\]]*\])?\s*[-+|&]?=(?!=)|\b{n}\s*->\s*[\w.\[\]]+\s*[-+|&]?=(?!=)|'
                          rf'\b{n}\s*\[[^\]]*\]\s*(?:\.\w+\s*)?[-+|&]?=(?!=)|'
                          rf'(?:memcpy|memset|strncpy|strcpy|strlcpy|snprintf|sprintf|memmove|copy_n)\s*\(\s*'
                          rf'(?:\([^)]*\)\s*)?&?\s*{n}\b)', body))


def is_output(base, name, depth, body):
    if body is not None and writes_through(body, name):
        return True
    if INPUT_NAME.match(name):
        return False
    words = [w.lower() for w in re.findall(r'[A-Z]?[a-z0-9]+|[A-Z]+(?![a-z])', name)]
    if OUT_WORDS.intersection(words):
        return True
    if depth >= 2:
        return True
    # A char* named for what it holds (path, name, text) is a string shadPS4 forgot to make const.
    if base == 'char' and STRING_NAME.search(name):
        return False
    if SCALAR.match(base):
        # A plural names an array the caller passes in (slotIds), not one value handed back.
        last = words[-1] if words else ''
        return not (last == 'ids' or len(last) > 3 and re.search(r'[^siu]s$', last))
    return bool(OUT_SUFFIX.search(name))


HELPER = re.compile(r'^[ \t]*(?:(?:static|inline|constexpr)\s+)*[A-Za-z_][\w:<>]*[\s*&]+([A-Za-z_][\w:]*)\s*\(', re.M)
KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'case', 'else', 'do', 'new', 'delete'}
CALL = re.compile(r'\b([A-Za-z_][\w:]*)\s*\(')


def definitions(text, pattern):
    """(name, params text, body or None) of each match of pattern followed by a parameter list."""
    for m in pattern.finditer(text):
        name = m.group(1).split('::')[-1]
        if name in KEYWORDS:
            continue
        close = matching(text, m.end() - 1, '(', ')')
        if close < 0:
            continue
        rest = text[close + 1:close + 200].lstrip()
        if re.match(r'^(?:const\s*)?(?:noexcept\s*)?\{', rest):
            open_ = text.index('{', close)
            end = matching(text, open_, '{', '}')
            yield name, text[m.end():close], text[open_:end] if end > 0 else ''
        elif rest.startswith(';'):
            yield name, text[m.end():close], None


def forwarded(body, name, functions):
    """Whether body passes the pointer on, unchanged, as an argument its callee writes through."""
    for m in CALL.finditer(body):
        callee = functions.get(m.group(1).split('::')[-1])
        if not callee:
            continue
        close = matching(body, m.end() - 1, '(', ')')
        if close < 0:
            continue
        for index, argument in enumerate(split_params(body[m.end():close])):
            if argument == name and index < 8 and any(f['mask'] >> index & 1 for f in callee):
                return True
    return False


def prototypes(root):
    """Function name -> {file: [(mask, has_body)]}, from every PS4_SYSV_ABI prototype.

    A pointer the body hands on to another function (an export or a helper of shadPS4's) that writes
    through it counts as written, so sceKernelFstat inherits fstat's output and scePthreadMutexInit
    MutexInit's; this runs to a fixed point over every definition."""
    exports, functions = [], {}
    for path in sorted(root.rglob('*')):
        if path.suffix not in ('.cpp', '.h', '.hpp', '.inc'):
            continue
        text = strip_comments(path.read_text(errors='replace'))
        for pattern, exported in ((PROTOTYPE, True), (HELPER, False)):
            for name, params, body in definitions(text, pattern):
                if not exported and body is None:
                    continue
                pointers = [(index, pointer_param(param)) for index, param in enumerate(split_params(params))]
                pointers = [(index, p) for index, p in pointers if p and index < 8]
                mask = 0
                for index, pointer in pointers:
                    # A helper has no PS4 contract to read names against: only its writes count.
                    written = is_output(*pointer, body) if exported else writes_through(body, pointer[1])
                    if written:
                        mask |= 1 << index
                entry = {'mask': mask, 'pointers': pointers, 'body': body}
                functions.setdefault(name, []).append(entry)
                if exported:
                    exports.append((name, path, entry))
    changed = True
    while changed:
        changed = False
        for entries in functions.values():
            for entry in entries:
                if entry['body'] is None:
                    continue
                for index, (_, pname, _) in entry['pointers']:
                    if not entry['mask'] >> index & 1 and forwarded(entry['body'], pname, functions):
                        entry['mask'] |= 1 << index
                        changed = True
    found = {}
    for name, path, entry in exports:
        found.setdefault(name, {}).setdefault(path, []).append((entry['mask'], entry['body'] is not None))
    return found


def choose(entries, path):
    """A definition beats a declaration; the registering file, then its directory, beat the rest."""
    def best(items):
        defined = [mask for mask, has_body in items if has_body]
        return (defined or [mask for mask, _ in items])[0]
    if path in entries:
        return best(entries[path])
    near = [e for p, items in entries.items() if p.parent == path.parent for e in items]
    if near:
        return best(near)
    return best([e for items in entries.values() for e in items])


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: import_outparams.py <shadPS4 checkout>')
    root = Path(sys.argv[1]) / 'src/core/libraries'
    if not root.is_dir():
        sys.exit(f'{root}: not a shadPS4 checkout')
    protos = prototypes(root)
    table = {}
    for path in sorted(root.rglob('*.cpp')):
        text = strip_comments(path.read_text(errors='replace'))
        for m in REGISTRATION.finditer(text):
            nid, function = m.group(1), m.group(2).strip()
            wrapped = re.fullmatch(r'ORBIS\(\s*([\w:]+)\s*\)', function)
            function = (wrapped.group(1) if wrapped else function).split('::')[-1]
            if not re.fullmatch(r'[A-Za-z_]\w*', function) or function not in protos:
                continue
            mask = choose(protos[function], path)
            if mask:
                old = table.get(nid)
                table[nid] = (old[0] if old else function, mask | (old[1] if old else 0))
    print('/* NID -> shadPS4 function name and output arguments (bit N: argument N, from 0) of every PS4')
    print(' * export that writes through a pointer argument, sorted by NID in strcmp order for a binary')
    print(' * search. Generated by tools/import_outparams.py from shadPS4\'s src/core/libraries; do not')
    print(' * edit by hand. */')
    for nid in sorted(table, key=lambda n: n.encode()):
        function, mask = table[nid]
        print(f'{{"{nid}","{function}",0x{mask:02x}}},')


if __name__ == '__main__':
    main()
