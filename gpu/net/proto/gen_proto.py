#!/usr/bin/env python3
"""C++ classes for a proto3 schema, without libprotobuf (kb #850).

    python3 gen_proto.py SCHEMA.proto OUT_DIR

Writes OUT_DIR/<stem>.pb.h and OUT_DIR/<stem>.pb.cc: one class per message with the subset of
protobuf's generated C++ API that the shadNet code calls (x(), set_x(), mutable_x(), has_x(),
add_x(), x_size(), ParseFromString, ParseFromArray, SerializeAsString, SerializeToString, Clear),
so that the inherited shadPS4 sources keep their calls, on top of proto_wire.h. The online module
links neither protobuf nor abseil (Apache-2.0) that way.

Only what shadnet.proto uses is understood: proto3, one package, top-level enums and messages,
scalar fields of type int32, uint32, int64, uint64, bool, string, bytes, an enum or a message,
singular or repeated. Anything else (oneof, map, nested types, optional, other scalar types,
imports) stops the generator with the line it met, rather than being ignored. A message that
contains itself, directly or not, is refused too, because decoding recurses into nested messages
and the schema's own depth is then the bound on that recursion.

Wire format: https://protobuf.dev/programming-guides/encoding/
proto3 language: https://protobuf.dev/programming-guides/proto3/
C++ generated API: https://protobuf.dev/reference/cpp/cpp-generated/
"""
import re
import sys
from pathlib import Path

SCALARS = {
    'int32': 'std::int32_t',
    'uint32': 'std::uint32_t',
    'int64': 'std::int64_t',
    'uint64': 'std::uint64_t',
    'bool': 'bool',
}
TOKEN = re.compile(r'[A-Za-z_][A-Za-z0-9_.]*|\d+|"(?:[^"\\]|\\.)*"|\S')


class SchemaError(Exception):
    pass


def tokenize(text):
    text = re.sub(r'/\*.*?\*/', lambda match: '\n' * match.group(0).count('\n'), text, flags=re.S)
    return [(match.group(0), line_no) for line_no, line in enumerate(text.splitlines(), 1)
            for match in TOKEN.finditer(line.split('//', 1)[0])]


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def peek(self):
        return self.tokens[self.pos][0] if self.pos < len(self.tokens) else None

    def take(self, expected=None):
        if self.pos >= len(self.tokens):
            raise SchemaError(f'unexpected end of schema, expected {expected or "a token"}')
        value, line_no = self.tokens[self.pos]
        if expected is not None and value != expected:
            raise SchemaError(f'line {line_no}: expected {expected!r}, found {value!r}')
        self.pos += 1
        return value

    def line(self):
        return self.tokens[min(self.pos, len(self.tokens) - 1)][1]

    def parse(self):
        package, enums, messages = None, {}, {}
        while self.peek() is not None:
            word = self.take()
            if word == 'syntax':
                self.take('=')
                if self.take() != '"proto3"':
                    raise SchemaError('only proto3 schemas are supported')
                self.take(';')
            elif word == 'package':
                package = self.take()
                self.take(';')
            elif word == 'option':
                while self.take() != ';':
                    pass
            elif word == 'enum':
                name = self.take()
                enums[name] = self.parse_enum()
            elif word == 'message':
                name = self.take()
                messages[name] = self.parse_message(name)
            else:
                raise SchemaError(f'line {self.line()}: unsupported declaration {word!r}')
        if package is None:
            raise SchemaError('the schema declares no package')
        return package, enums, messages

    def parse_enum(self):
        self.take('{')
        values = []
        while self.peek() != '}':
            name = self.take()
            self.take('=')
            values.append((name, int(self.take())))
            self.take(';')
        self.take('}')
        if not values or values[0][1] != 0:
            raise SchemaError('a proto3 enum starts with the value 0')
        return values

    def parse_message(self, message):
        self.take('{')
        fields = []
        while self.peek() != '}':
            line_no = self.line()
            repeated = self.peek() == 'repeated'
            if repeated:
                self.take()
            kind = self.take()
            if kind in ('message', 'enum', 'oneof', 'map', 'optional', 'reserved', 'option',
                        'extensions', 'required'):
                raise SchemaError(f'line {line_no}: {message}: unsupported {kind!r}')
            name = self.take()
            self.take('=')
            number = int(self.take())
            if self.peek() == '[':
                raise SchemaError(f'line {line_no}: {message}.{name}: field options unsupported')
            self.take(';')
            fields.append({'kind': kind, 'name': name, 'number': number, 'repeated': repeated,
                           'line': line_no})
        self.take('}')
        return fields


def classify(fields, enums, messages, message):
    numbers, names = set(), set()
    for field in fields:
        kind = field['kind']
        if kind in SCALARS:
            field['class'] = 'scalar'
        elif kind in ('string', 'bytes'):
            field['class'] = kind
        elif kind in enums:
            field['class'] = 'enum'
        elif kind in messages:
            field['class'] = 'message'
        else:
            raise SchemaError(f'line {field["line"]}: {message}.{field["name"]}: '
                              f'unsupported type {kind!r}')
        field['accessor'] = field['name'].lower()
        if not 1 <= field['number'] <= (1 << 29) - 1 or 19000 <= field['number'] <= 19999:
            raise SchemaError(f'line {field["line"]}: {message}: invalid field number')
        if field['number'] in numbers or field['accessor'] in names:
            raise SchemaError(f'line {field["line"]}: {message}: duplicate field')
        numbers.add(field['number'])
        names.add(field['accessor'])
    methods = {}
    for field in fields:
        for method in method_names(field):
            if methods.setdefault(method, field['accessor']) != field['accessor']:
                raise SchemaError(f'{message}: {method}() would belong to two fields')
    fields.sort(key=lambda field: field['number'])


def method_names(field):
    name = field['accessor']
    names = [name, f'set_{name}', f'clear_{name}']
    if field['repeated']:
        names += [f'add_{name}', f'{name}_size', f'mutable_{name}']
    elif field['class'] in ('string', 'bytes', 'message'):
        names.append(f'mutable_{name}')
    if field['class'] == 'message' and not field['repeated']:
        names.append(f'has_{name}')
    return names


def order_messages(messages):
    ordered, state = [], {}

    def visit(name, chain):
        if state.get(name) == 'done':
            return
        if state.get(name) == 'visiting':
            raise SchemaError(f'recursive message: {" -> ".join(chain + [name])}')
        state[name] = 'visiting'
        for field in messages[name]:
            if field['class'] == 'message':
                visit(field['kind'], chain + [name])
        state[name] = 'done'
        ordered.append(name)

    for name in messages:
        visit(name, [])
    return ordered


def cpp_type(field):
    if field['class'] == 'scalar':
        return SCALARS[field['kind']]
    if field['class'] in ('string', 'bytes'):
        return 'std::string'
    return field['kind']


def member_type(field):
    if field['repeated']:
        container = 'std::vector' if field['class'] in ('scalar', 'enum') else 'std::deque'
        return f'{container}<{cpp_type(field)}>'
    if field['class'] == 'message':
        return f'std::optional<{field["kind"]}>'
    return cpp_type(field)


def member_init(field):
    if field['repeated'] or field['class'] in ('string', 'bytes', 'message'):
        return ''
    if field['kind'] == 'bool':
        return ' = false'
    if field['class'] == 'enum':
        return '{}'
    return ' = 0'


def declare_accessors(field):
    name, member, kind = field['accessor'], f'm_{field["accessor"]}', cpp_type(field)
    lines = []
    if field['repeated']:
        lines += [
            f'const {member_type(field)}& {name}() const {{ return {member}; }}',
            f'{member_type(field)}* mutable_{name}() {{ return &{member}; }}',
            f'int {name}_size() const {{ return static_cast<int>({member}.size()); }}',
            f'void clear_{name}() {{ {member}.clear(); }}',
        ]
        if field['class'] in ('scalar', 'enum'):
            lines += [
                f'{kind} {name}(int i_element) const {{ return {member}[i_element]; }}',
                f'void set_{name}(int i_element, {kind} value) {{ {member}[i_element] = value; }}',
                f'void add_{name}({kind} value) {{ {member}.push_back(value); }}',
            ]
        else:
            lines += [
                f'const {kind}& {name}(int i_element) const {{ return {member}[i_element]; }}',
                f'{kind}* mutable_{name}(int i_element) {{ return &{member}[i_element]; }}',
                f'{kind}* add_{name}() {{ return &{member}.emplace_back(); }}',
            ]
            if field['class'] in ('string', 'bytes'):
                lines.append(f'void add_{name}(std::string_view value) {{ {member}.emplace_back(value); }}')
    elif field['class'] in ('scalar', 'enum'):
        lines += [
            f'{kind} {name}() const {{ return {member}; }}',
            f'void set_{name}({kind} value) {{ {member} = value; }}',
            f'void clear_{name}() {{ {member} = {kind}{{}}; }}',
        ]
    elif field['class'] in ('string', 'bytes'):
        lines += [
            f'const std::string& {name}() const {{ return {member}; }}',
            f'void set_{name}(std::string_view value) {{ {member}.assign(value); }}',
            f'void set_{name}(const void* p_value, std::size_t cb_value) {{',
            f'    {member}.assign(static_cast<const char*>(p_value), cb_value);',
            '}',
            f'std::string* mutable_{name}() {{ return &{member}; }}',
            f'void clear_{name}() {{ {member}.clear(); }}',
        ]
    else:
        lines += [
            f'bool has_{name}() const {{ return {member}.has_value(); }}',
            f'const {kind}& {name}() const {{',
            f'    return {member} ? *{member} : {kind}::default_instance();',
            '}',
            f'{kind}* mutable_{name}() {{ return {member} ? &*{member} : &{member}.emplace(); }}',
            f'void clear_{name}() {{ {member}.reset(); }}',
        ]
    return lines


def to_wire_value(field, expression):
    if field['kind'] in ('int32',) or field['class'] == 'enum':
        source = f'static_cast<std::int32_t>({expression})' if field['class'] == 'enum' else expression
        return f'ProtoWire::widen_int32({source})'
    if field['kind'] == 'bool':
        return f'({expression} ? 1u : 0u)'
    return f'static_cast<std::uint64_t>({expression})'


def from_wire_value(field, variable):
    if field['kind'] == 'bool':
        return f'{variable} != 0'
    if field['class'] == 'enum':
        return f'static_cast<{field["kind"]}>(static_cast<std::int32_t>({variable}))'
    return f'static_cast<{SCALARS[field["kind"]]}>({variable})'


def define_append(message, fields):
    parameter = 'out' if fields else '/*out*/'
    out = [f'void {message}::append_to(std::string& {parameter}) const {{']
    for field in fields:
        number, member = field['number'], f'm_{field["accessor"]}'
        if field['repeated'] and field['class'] in ('scalar', 'enum'):
            out += [
                f'    if (!{member}.empty()) {{',
                '        std::string packed;',
                f'        for (const auto value : {member}) {{',
                f'            ProtoWire::append_varint(packed, {to_wire_value(field, "value")});',
                '        }',
                f'        ProtoWire::append_len(out, {number}, packed);',
                '    }',
            ]
        elif field['repeated'] and field['class'] in ('string', 'bytes'):
            out += [
                f'    for (const auto& value : {member}) {{',
                f'        ProtoWire::append_len(out, {number}, value);',
                '    }',
            ]
        elif field['repeated']:
            out += [
                f'    for (const auto& value : {member}) {{',
                '        std::string nested;',
                '        value.append_to(nested);',
                f'        ProtoWire::append_len(out, {number}, nested);',
                '    }',
            ]
        elif field['class'] in ('scalar', 'enum'):
            test = member if field['kind'] == 'bool' else f'{member} != {cpp_type(field)}{{}}'
            out += [
                f'    if ({test}) {{',
                f'        ProtoWire::append_tag(out, {number}, ProtoWire::WireType::Varint);',
                f'        ProtoWire::append_varint(out, {to_wire_value(field, member)});',
                '    }',
            ]
        elif field['class'] in ('string', 'bytes'):
            out += [
                f'    if (!{member}.empty()) {{',
                f'        ProtoWire::append_len(out, {number}, {member});',
                '    }',
            ]
        else:
            out += [
                f'    if ({member}) {{',
                '        std::string nested;',
                f'        {member}->append_to(nested);',
                f'        ProtoWire::append_len(out, {number}, nested);',
                '    }',
            ]
    out.append('}')
    return out


def define_merge(message, fields):
    out = [
        f'bool {message}::merge_from(ProtoWire::Reader& reader) {{',
        '    std::uint32_t field = 0;',
        '    ProtoWire::WireType type{};',
        '    while (!reader.is_at_end()) {',
        '        if (!reader.read_tag(field, type)) {',
        '            return false;',
        '        }',
    ]
    if fields:
        out.append('        switch (field) {')
    for field in fields:
        member = f'm_{field["accessor"]}'
        out.append(f'        case {field["number"]}:')
        if field['class'] in ('scalar', 'enum'):
            store = (f'{member}.push_back({from_wire_value(field, "value")})' if field['repeated']
                     else f'{member} = {from_wire_value(field, "value")}')
            if field['repeated']:
                out += [
                    '            if (type == ProtoWire::WireType::Len) {',
                    '                std::string_view packed;',
                    '                if (!reader.read_len(packed)) {',
                    '                    return false;',
                    '                }',
                    '                ProtoWire::Reader packed_reader(packed);',
                    '                while (!packed_reader.is_at_end()) {',
                    '                    std::uint64_t value = 0;',
                    '                    if (!packed_reader.read_varint(value) ||',
                    f'                        {member}.size() >= ProtoWire::MAX_REPEATED_ELEMENTS) {{',
                    '                        return false;',
                    '                    }',
                    f'                    {store};',
                    '                }',
                    '                continue;',
                    '            }',
                ]
            out += [
                '            if (type == ProtoWire::WireType::Varint) {',
                '                std::uint64_t value = 0;',
                '                if (!reader.read_varint(value)) {',
                '                    return false;',
                '                }',
            ]
            if field['repeated']:
                out += [
                    f'                if ({member}.size() >= ProtoWire::MAX_REPEATED_ELEMENTS) {{',
                    '                    return false;',
                    '                }',
                ]
            out += [
                f'                {store};',
                '                continue;',
                '            }',
                '            break;',
            ]
            continue
        out += [
            '            if (type == ProtoWire::WireType::Len) {',
            '                std::string_view bytes;',
        ]
        check = '!reader.read_len(bytes)'
        if field['class'] == 'string':
            check += ' || !ProtoWire::is_valid_utf8(bytes)'
        if field['repeated']:
            check += f' ||\n                    {member}.size() >= ProtoWire::MAX_REPEATED_ELEMENTS'
        out += [
            f'                if ({check}) {{',
            '                    return false;',
            '                }',
        ]
        if field['class'] in ('string', 'bytes'):
            out.append(f'                {member}.emplace_back(bytes);' if field['repeated']
                       else f'                {member}.assign(bytes);')
        else:
            target = (f'{member}.emplace_back().' if field['repeated']
                      else f'mutable_{field["accessor"]}()->')
            out += [
                '                ProtoWire::Reader nested(bytes);',
                f'                if (!{target}merge_from(nested)) {{',
                '                    return false;',
                '                }',
            ]
        out += [
            '                continue;',
            '            }',
            '            break;',
        ]
    if fields:
        out += [
            '        default:',
            '            break;',
            '        }',
        ]
    out += [
        '        if (!reader.skip(field, type)) {',
        '            return false;',
        '        }',
        '    }',
        '    return true;',
        '}',
    ]
    return out


def generate(schema_path, out_dir):
    package, enums, messages = Parser(tokenize(schema_path.read_text())).parse()
    for name, fields in messages.items():
        classify(fields, enums, messages, name)
    ordered = order_messages(messages)
    stem = schema_path.stem
    banner = [
        f'// Generated by gpu/net/proto/gen_proto.py from {schema_path.name}: do not edit.',
        '// SPDX-License-Identifier: GPL-2.0-or-later',
    ]
    header = banner + [
        '',
        '#pragma once',
        '',
        '#include <cstddef>',
        '#include <cstdint>',
        '#include <deque>',
        '#include <optional>',
        '#include <string>',
        '#include <string_view>',
        '#include <vector>',
        '',
        '#include "proto_wire.h"',
        '',
        f'namespace {package} {{',
        '',
    ]
    for name, values in enums.items():
        header.append(f'enum {name} : int {{')
        header += [f'    {value_name} = {number},' for value_name, number in values]
        header += ['};', '']
    for name in ordered:
        header += [f'class {name} {{', 'public:', f'    static const {name}& default_instance();', '']
        for field in messages[name]:
            header += ['    ' + line for line in declare_accessors(field)]
        header += [
            '',
            '    bool ParseFromString(std::string_view data) {',
            '        Clear();',
            '        ProtoWire::Reader reader(data);',
            '        return merge_from(reader);',
            '    }',
            '    bool ParseFromArray(const void* p_data, int cb_data) {',
            '        return cb_data >= 0 &&',
            '               ParseFromString(std::string_view(static_cast<const char*>(p_data),',
            '                                                static_cast<std::size_t>(cb_data)));',
            '    }',
            '    std::string SerializeAsString() const {',
            '        std::string out;',
            '        append_to(out);',
            '        return out;',
            '    }',
            '    bool SerializeToString(std::string* p_out) const {',
            '        p_out->clear();',
            '        append_to(*p_out);',
            '        return true;',
            '    }',
            f'    void Clear() {{ *this = {name}(); }}',
            '',
            '    bool merge_from(ProtoWire::Reader& reader);',
            '    void append_to(std::string& out) const;',
        ]
        if messages[name]:
            header += ['', 'private:']
            header += [f'    {member_type(field)} m_{field["accessor"]}{member_init(field)};'
                       for field in messages[name]]
        header += ['};', '']
    header += [f'}} // namespace {package}', '']

    source = banner + ['', f'#include "{stem}.pb.h"', '', f'namespace {package} {{', '']
    for name in ordered:
        source += [
            f'const {name}& {name}::default_instance() {{',
            f'    static const {name} s_default;',
            '    return s_default;',
            '}',
            '',
        ]
        source += define_merge(name, messages[name]) + ['']
        source += define_append(name, messages[name]) + ['']
    source += [f'}} // namespace {package}', '']

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f'{stem}.pb.h').write_text('\n'.join(header))
    (out_dir / f'{stem}.pb.cc').write_text('\n'.join(source))


def main():
    if len(sys.argv) != 3:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    try:
        generate(Path(sys.argv[1]), Path(sys.argv[2]))
    except SchemaError as error:
        print(f'gen_proto.py: {sys.argv[1]}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
