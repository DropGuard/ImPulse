#!/usr/bin/env python3
"""A translated string never loses its translation to a class move.

QObject::tr() keys a translation on (context, source), and the context is the
class name. Moving a tr() call into another class, or renaming the class,
therefore orphans every translation it had: the catalog still holds the text
under the old context, the lookup asks the new one, and the UI silently
falls back to English. Nothing else notices. NavigationController's
extraction (e721d0b9) orphaned seven of Core's strings in every language this
way, and the SSideBar -> SettingsSidebar rename seven more; neither showed up
in any suite.

The instrument is lupdate itself, so the contexts are the ones Qt will look
up: it extracts the tree (cpp, h, ui) into a scratch catalog, and every
translated (context, source) in a shipped catalog is checked against it. A
source the code no longer has at all is ordinary obsolescence and passes (the
next lupdate run marks it). A source the code still has, but under a context
this catalog has no entry for at all, is a lost translation and fails, naming
where it went. A (context, source) the catalog already lists, translated or
not, is its own message: an empty one is a translator's open item, not a
loss, and the check does not fill it from a namesake elsewhere.

Skips (77) when lupdate is absent. An extraction that yields nothing is a
failure, not a pass: a check that scanned nothing is indistinguishable from
one that found nothing wrong. The repair this check asks for is mechanical and
lives in scripts/retarget_translations.py, which reuses the functions below.
"""
import glob, os, shutil, subprocess, sys, tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CATALOGS = os.path.join(ROOT, 'src', 'res', 'translations')
UNTRANSLATED = ('unfinished', 'obsolete', 'vanished')


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
    return code


def catalogs():
    return sorted(glob.glob(os.path.join(CATALOGS, '*.ts')))


def lost(code):
    """([(catalog, old context, source, [contexts the code asks under])], translated entries checked)"""
    homes = {}
    for context, source in code:
        homes.setdefault(source, set()).add(context)
    found, checked = [], 0
    for catalog in catalogs():
        listed, translated = read(catalog)
        checked += len(translated)
        for context, source in sorted(translated):
            if (context, source) in code:
                continue
            missing = sorted(h for h in homes.get(source, ()) if (h, source) not in listed)
            if missing:
                found.append((catalog, context, source, missing))
    return found, checked


def main():
    lupdate = find_lupdate(sys.argv[1] if len(sys.argv) > 1 else None)
    if not lupdate:
        print('translation rules SKIPPED: no lupdate (install the Qt6 linguist tools)')
        return 77
    try:
        code = extract(lupdate)
    except RuntimeError as error:
        print('translation rules:', error)
        return 1
    if not code:
        print('translation rules: lupdate extracted no strings from the tree')
        return 1
    found, checked = lost(code)
    if found:
        print('translation rules: translations lost to a context change:')
        for catalog, context, source, missing in found:
            print(f'   {os.path.relpath(catalog, ROOT)}: {source!r} is translated under {context}, but the code '
                  f'asks for it under {", ".join(missing)}, where this catalog has no entry')
        print(f'{len(found)} lost across {len(catalogs())} catalogs. Move each message to the context the code '
              'now uses: python3 scripts/retarget_translations.py')
        return 1
    print(f'translation rules OK \u2014 {len(catalogs())} catalogs, {checked} translated entries, '
          f'{len(code)} strings in the code')
    return 0


if __name__ == '__main__':
    sys.exit(main())
