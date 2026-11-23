#!/usr/bin/env python3
"""Move translations to the context the code now asks for them under.

QObject::tr() keys a translation on (context, source), and the context is the
class name. Moving a tr() call into another class, or renaming the class,
orphans every translation it had: the catalog keeps the text under the old
context, the lookup asks the new one, and the UI falls back to English. The
fallback is an accepted state (the project keeps no completeness gate), so
nothing runs this automatically: run it after a class move whose translations
should survive.

lupdate is the instrument, so the contexts are the ones Qt will look up: it
extracts the tree (cpp, h, ui) into a scratch catalog. A translated (context,
source) the code no longer asks for, whose source the code still has under a
context this catalog has no entry for, is moved there: same source, same
translation. A source the code no longer has at all is ordinary obsolescence
and is left for lupdate. The <location> lines are dropped, because they name
the old file and the next lupdate run writes the right ones. Only contexts the
catalog has no entry for receive a move, so no existing message is ever
overwritten. The old message is removed, and its
context too once it is empty.

Edits are textual, so every byte of a catalog that is not moved stays as it
was: no reflow, no re-escaping, no reordering.

Usage: python3 scripts/retarget_translations.py [path-to-lupdate]
"""
import glob, os, re, shutil, subprocess, sys, tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOGS = os.path.join(ROOT, 'src', 'res', 'translations')
UNTRANSLATED = ('unfinished', 'obsolete', 'vanished')

CONTEXT = re.compile(r'<context>\s*<name>(.*?)</name>.*?</context>\n?', re.S)
MESSAGE = re.compile(r'[ \t]*<message\b[^>]*>.*?</message>\n', re.S)
LOCATION = re.compile(r'[ \t]*<location\b[^>]*/>\n')


def find_lupdate(explicit=None):
    if explicit and os.path.exists(explicit):
        return explicit
    for name in ('lupdate-qt6', 'lupdate'):
        found = shutil.which(name)
        if found:
            return found
    for path in ('/usr/lib/qt6/bin/lupdate', '/usr/lib/qt6/libexec/lupdate'):
        if os.path.exists(path):
            return path
    return None


def read(path):
    """(every (context, source), the translated ones)"""
    entries, translated = set(), set()
    for ctx in ET.parse(path).getroot().iter('context'):
        name = ctx.findtext('name')
        for msg in ctx.iter('message'):
            source = msg.findtext('source')
            if source is None:
                continue
            entries.add((name, source))
            tr = msg.find('translation')
            if tr is not None and tr.get('type') not in UNTRANSLATED and (tr.text or '').strip():
                translated.add((name, source))
    return entries, translated


def extract(lupdate):
    """every (context, source) the tree's code asks for, as lupdate sees it"""
    with tempfile.TemporaryDirectory() as scratch:
        current = os.path.join(scratch, 'current.ts')
        run = subprocess.run([lupdate, '-silent', '-locations', 'none', '-no-obsolete', '-extensions', 'cpp,h,ui',
                              'src', 'plugins', '-ts', current], cwd=ROOT, capture_output=True, text=True)
        if run.returncode != 0 or not os.path.exists(current):
            raise RuntimeError(f'lupdate failed: {run.stdout} {run.stderr}')
        code, _ = read(current)
    if not code:  # an extraction that saw nothing would report nothing to move
        raise RuntimeError('lupdate extracted no strings from the tree')
    return code


def lost(code):
    """[(catalog, old context, source, [contexts the code asks under])]"""
    homes = {}
    for context, source in code:
        homes.setdefault(source, set()).add(context)
    found = []
    for catalog in sorted(glob.glob(os.path.join(CATALOGS, '*.ts'))):
        listed, translated = read(catalog)
        for context, source in sorted(translated):
            if (context, source) in code:
                continue
            missing = sorted(h for h in homes.get(source, ()) if (h, source) not in listed)
            if missing:
                found.append((catalog, context, source, missing))
    return found


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
    lupdate = find_lupdate(sys.argv[1] if len(sys.argv) > 1 else None)
    if not lupdate:
        print('no lupdate found (install the Qt6 linguist tools)')
        return 1
    found = lost(extract(lupdate))
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
