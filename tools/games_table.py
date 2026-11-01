#!/usr/bin/env python3
"""Writes the title tables of the READMEs and docs/<lang>/GAMES.md from src/main/compat_titles.inc (kb #1481).

    python3 tools/games_table.py [--check]

The table psport --list-compat prints is the only place a title's state and detail are edited; this
rewrites the rows of each document's title table (the table whose first row holds a CUSA id) and leaves
the rest of the page alone. --check writes nothing and exits 1 naming each document that differs.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'src/main/compat_titles.inc'
LANGUAGES = ('en', 'es', 'ru')
STATE_LABELS = {
    'en': {'playable': 'playable', 'experimental': 'experimental', 'broken': 'broken', 'unsupported': 'not supported'},
    'es': {'playable': 'jugable', 'experimental': 'experimental', 'broken': 'roto', 'unsupported': 'no soportado'},
    'ru': {
        'playable': 'играбельна',
        'experimental': 'экспериментально',
        'broken': 'не работает',
        'unsupported': 'не поддерживается',
    },
}
# The README lists the state only; GAMES.md adds the detail.
DOCUMENTS = (
    ('README.md', 'en', False),
    ('README.es.md', 'es', False),
    ('README.ru.md', 'ru', False),
    ('docs/en/GAMES.md', 'en', True),
    ('docs/es/GAMES.md', 'es', True),
    ('docs/ru/GAMES.md', 'ru', True),
)
FIELDS = ('title_id', 'name', 'version', 'state', 'en', 'es', 'ru')
STRING = r'"(?:[^"\\\n]|\\.)*"'
ARGUMENT = re.compile(rf'\s*((?:{STRING}\s*)+|\w+)\s*([,)])')
FIRST_ROW = re.compile(r'^\| .* \| CUSA\d{5} \|')


def unquote(literals):
    return ''.join(re.sub(r'\\(.)', r'\1', s[1:-1]) for s in re.findall(STRING, literals))


def read_titles():
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', SOURCE.read_text(encoding='utf-8'), flags=re.S)
    titles = []
    for start in re.finditer(r'\bTITLE\(', text):
        values, pos, end = [], start.end(), ','
        while end == ',':
            m = ARGUMENT.match(text, pos)
            if not m:
                sys.exit(f'{SOURCE.relative_to(ROOT)}: TITLE at offset {start.start()} does not parse')
            values.append(unquote(m[1]) if m[1].startswith('"') else m[1])
            pos, end = m.end(), m[2]
        if len(values) != len(FIELDS):
            sys.exit(f'{SOURCE.relative_to(ROOT)}: TITLE({values[0]}) has {len(values)} fields, not {len(FIELDS)}')
        titles.append(dict(zip(FIELDS, values)))
    return titles


def rows(titles, language, has_detail):
    for t in titles:
        name = f"{t['name']} ({t['version']})" if t['version'] else t['name']
        cells = [name, t['title_id'], STATE_LABELS[language][t['state']]] + ([t[language]] if has_detail else [])
        yield '| ' + ' | '.join(cells) + ' |'


def rewrite(text, new_rows):
    lines = text.split('\n')
    for i in range(1, len(lines) - 1):
        if lines[i].startswith('|---') and FIRST_ROW.match(lines[i + 1]):
            end = i + 1
            while end < len(lines) and lines[end].startswith('|'):
                end += 1
            return '\n'.join(lines[: i + 1] + list(new_rows) + lines[end:])
    return None


def main():
    is_check = sys.argv[1:] == ['--check']
    if sys.argv[1:] and not is_check:
        sys.exit('usage: tools/games_table.py [--check]')
    titles = read_titles()
    differing = []
    for relative, language, has_detail in DOCUMENTS:
        path = ROOT / relative
        text = path.read_text(encoding='utf-8')
        updated = rewrite(text, rows(titles, language, has_detail))
        if updated is None:
            sys.exit(f'{relative}: no title table (a |--- line followed by a row with a CUSA id)')
        if updated == text:
            continue
        differing.append(relative)
        if not is_check:
            path.write_text(updated, encoding='utf-8')
    for relative in differing:
        print(f'{relative}: {"differs from" if is_check else "rewritten from"} {SOURCE.relative_to(ROOT)}')
    return 1 if is_check and differing else 0


if __name__ == '__main__':
    sys.exit(main())
