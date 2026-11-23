#!/usr/bin/env python3
"""Move translations to the context the code now asks for them under.

The repair for what src/tests/translation_rules.py reports. Moving a tr() call
into another class (or renaming the class) changes its translation context,
and every catalog keeps the translation under the old one. This moves each
such message to the context lupdate now extracts: same source, same
translation. The <location> lines are dropped, because they name the old file
and the next lupdate run writes the right ones. Only contexts the catalog has no
entry for receive a move (the check's own definition of a loss), so no
existing message is ever overwritten. The old message is removed, and its
context too once it is empty.

Edits are textual, so every byte of a catalog that is not moved stays as it
was: no reflow, no re-escaping, no reordering.

Usage: python3 scripts/retarget_translations.py [path-to-lupdate]
"""
import os, re, sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'src', 'tests'))
import translation_rules as rules  # noqa: E402

CONTEXT = re.compile(r'<context>\s*<name>(.*?)</name>.*?</context>\n?', re.S)
MESSAGE = re.compile(r'[ \t]*<message\b[^>]*>.*?</message>\n', re.S)
LOCATION = re.compile(r'[ \t]*<location\b[^>]*/>\n')


def contexts(text):
    return [(m.group(1), m.start(), m.end()) for m in CONTEXT.finditer(text)]


def find_context(text, name):
    for found, start, end in contexts(text):
        if found == name:
            return start, end
    return None


def find_message(text, span, source):
    for m in MESSAGE.finditer(text, span[0], span[1]):
        if ET.fromstring(m.group(0).strip()).findtext('source') == source:
            return m.start(), m.end()
    return None


def retarget(text, old, source, homes):
    start, end = find_message(text, find_context(text, old), source)
    block = LOCATION.sub('', text[start:end])
    for home in homes:
        span = find_context(text, home)
        if span:
            close = text.rindex('</context>', span[0], span[1])
            text = text[:close] + block + text[close:]
        else:
            after = [s for name, s, _ in contexts(text) if name.lower() > home.lower()]
            at = after[0] if after else text.rindex('</TS>')
            text = text[:at] + f'<context>\n    <name>{home}</name>\n{block}</context>\n' + text[at:]
    start, end = find_message(text, find_context(text, old), source)
    text = text[:start] + text[end:]
    span = find_context(text, old)
    if not MESSAGE.search(text, span[0], span[1]):
        text = text[:span[0]] + text[span[1]:]
    return text


def main():
    lupdate = rules.find_lupdate(sys.argv[1] if len(sys.argv) > 1 else None)
    if not lupdate:
        print('no lupdate found (install the Qt6 linguist tools)')
        return 1
    found, _ = rules.lost(rules.extract(lupdate))
    moves = {}
    for catalog, context, source, homes in found:
        moves.setdefault(catalog, []).append((context, source, homes))
    for catalog, items in sorted(moves.items()):
        with open(catalog, encoding='utf-8', newline='') as f:
            text = f.read()
        for context, source, homes in items:
            text = retarget(text, context, source, homes)
        ET.fromstring(text.encode('utf-8'))  # still well-formed, or nothing is written
        with open(catalog, 'w', encoding='utf-8', newline='') as f:
            f.write(text)
        print(f'{os.path.relpath(catalog, ROOT)}: {len(items)} moved')
    if not moves:
        print('nothing to move')
    return 0


if __name__ == '__main__':
    sys.exit(main())
