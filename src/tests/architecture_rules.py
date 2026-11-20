#!/usr/bin/env python3
# Executable architecture rules — the C++ answer to Java's ArchUnit.
# Each rule pins a structural property of the codebase that is true today
# and must stay true. A violation fails the test, so architecture drift
# surfaces in the same ctest run as everything else.
#
# Every rule reports how many files it actually scanned: a rule whose filter
# matches nothing is indistinguishable from a passing rule, and that is exactly
# how R1 slept for its whole life (it filtered by file NAME, so no gui/ file was
# ever looked at).
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'qimgv')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tree_scope  # noqa: E402  (needs the path above first)

def sources():
    # The roots come from tree_scope, the same module the format rule and the
    # pre-commit hook ask: each gate used to name qimgv/ itself, and each
    # therefore missed plugins/ — nine of these rules never saw plugin code.
    # root-relative, which is what every rule below was written against
    # ('gui/overlays/x.cpp', 'core.h', 'tests/...'). The plugin files arrive
    # as 'player_mpv/src/x.cpp' — no rule's prefix matches them, so they are
    # scanned (R2, R3, R10, R11, R14) without any rule changing meaning.
    for path, rel in tree_scope.sources(repo_relative=False):
        yield path, rel

def text_of(path):
    return open(path, encoding='utf-8', errors='replace').read()

failures = []
scanned = {}

# Rule 1: the widget tree never speaks to the engine. The gui layer talks to
# Core / DirectoryModel only; engine access lives behind the components layer.
# The filter is on the path, not the file name — gui/ files are named
# mainwindow.cpp, viewerwidget.cpp and friends.
# Matched by header, not by the substring "engine": QPaintEngine and friends are
# ordinary Qt includes and the loose version of this rule flagged them.
ENGINE_ENTRY_POINTS = re.compile(r'(?:^|/)(?:qimgv_engine|enginebridge)\.h$')
for path, rel in sources():
    if not rel.startswith('gui/'):
        continue
    scanned['R1'] = scanned.get('R1', 0) + 1
    for inc in re.findall(r'#include\s+[<"]([^">]+)[">]', text_of(path)):
        if ENGINE_ENTRY_POINTS.search(inc):
            failures.append(f"R1 gui must not touch the engine: {rel} includes {inc}")

# Rule 2: the FFI header has a fixed set of authorized callers. Anything else
# including qimgv_engine.h must join this list consciously — that review is
# the point of the rule.
#
# The rule used to look ONLY at #include lines, which is a check on one door
# and not on the room: the FFI type reached other files through a PUBLIC
# HEADER instead, as a parameter and a member of EngineHandle. R2 saw nothing,
# because nothing included qimgv_engine.h. Two changes close that:
#
#   (a) the type is erased from enginebridge.h (the handle stores void* and
#       adopts through void*, so the header names no FFI symbol and no FFI
#       type), and
#   (b) this rule now refuses any mention of the FFI vocabulary outside the
#       allowlist — a qimgv_engine_* call, a qimgv_engine* type, an
#       #include — in ANY file, header or source. A future leak through a
#       header fails here by name instead of passing unseen.
#
# Comments count as mentions on purpose: a header that documents "takes a
# qimgv_engine*" is teaching the next reader to write one.
ALLOWED_FFI_CALLERS = {
    'main.cpp',
    'components/directorymanager/enginebridge.cpp',
    'components/directorymanager/directorymanager.cpp',
    'components/thumbnailer/thumbnailerrunnable.cpp',
}
# the FFI vocabulary as it appears in code or prose: the type name in any form,
# its functions, and the header
FFI_VOCABULARY = re.compile(r'qimgv_engine')
for path, rel in sources():
    if rel.startswith('tests/'):
        continue # the tests audit the C ABI and must include its header
    scanned['R2'] = scanned.get('R2', 0) + 1
    text = text_of(path)
    if FFI_VOCABULARY.search(text):
        if rel not in ALLOWED_FFI_CALLERS:
            hits = [i + 1 for i, line in enumerate(text.splitlines()) if 'qimgv_engine' in line]
            failures.append(f"R2 unauthorized FFI mention: {rel}:{hits[0]} names the engine's C ABI "
                            "(add to ALLOWED_FFI_CALLERS only after review; prefer hiding it "
                            "behind an owner type)")

# Rule 3: Rust internals are invisible to C++. The engine is reachable only
# through its C ABI header; no source file may reach into the crate sources.
for path, rel in sources():
    scanned['R3'] = scanned.get('R3', 0) + 1
    if re.search(r'#include\s+.*(qimgv-engine/src|engine/qimgv-engine/src)', text_of(path)):
        failures.append(f"R3 Rust internals leaked: {rel}")

# Rule 4: code that runs off the UI thread never dereferences the settings
# global. One shared QSettings instance touched from several threads is a data
# race, and ambient reads cannot be pinned to the moment a request was made:
# the value travels in the request instead — ScalerRequest::smoothUpscaling,
# LoadPolicy for the decode/classification chain, explicit quality for saves.
# The DEBT list that used to live here retired at zero on 2026-11-05
# (thumbnailerrunnable, documentinfo, imagestatic, video); the rule is a pure
# assertion now.
OFFTHREAD = {
    'components/loader/loaderrunnable.cpp',
    'components/scaler/scalerrunnable.cpp',
    'components/thumbnailer/thumbnailerrunnable.cpp',
    'sourcecontainers/documentinfo.cpp',
    'sourcecontainers/imagestatic.cpp',
    'sourcecontainers/video.cpp',
}
for path, rel in sources():
    if rel not in OFFTHREAD:
        continue
    scanned['R4'] = scanned.get('R4', 0) + 1
    lines = text_of(path).splitlines()
    hits = [i + 1 for i, l in enumerate(lines) if re.search(r'\bsettings\s*->', l)]
    if hits:
        failures.append(f"R4 off-thread settings deref: {rel}:{hits[0]} — carry the "
                        f"value in the request (see ScalerRequest::smoothUpscaling / LoadPolicy)")

# Rule 5: both sides of the ABI assert the same layout. qimgv_engine.h pins the
# by-value structs with QIMGV_PIN, lib.rs mirrors the numbers with size_of/
# offset_of. This has to be a rule because neither compiler can see the other: a
# layout mismatch still links (the symbol names are unchanged), so the only thing
# that can catch a field reordered on one side is comparing the two sets of
# literals — and sizes alone would miss exactly that case, since reordering keeps
# the size identical.
HEADER = os.path.join(ROOT, 'engine/qimgv-engine-ffi/include/qimgv_engine.h')
RUST = os.path.join(ROOT, 'engine/qimgv-engine-ffi/src/lib.rs')
for side, path in (('header', HEADER), ('rust', RUST)):
    if not os.path.exists(path):
        failures.append(f"R5 cannot run: the {side} moved ({path}) — update this rule, do not let it scan nothing")
if not any(f.startswith('R5 cannot run') for f in failures):
    scanned['R5'] = 2

    def pascal_to_snake(name):
        return re.sub(r'(?<!^)(?=[A-Z])', '_', name).lower()

    def c_layout(text):
        out = {}
        for struct, n in re.findall(r'QIMGV_PIN\("(qimgv_[a-z_]+)", sizeof\(qimgv_[a-z_]+\) == (\d+)\)', text):
            out[(struct, '')] = int(n)
        for field, struct, n in re.findall(
                r'QIMGV_PIN\("qimgv_[a-z_]+\.([a-z_]+)", offsetof\((qimgv_[a-z_]+), \w+\) == (\d+)\)', text):
            out[(struct, field)] = int(n)
        return out

    def rust_layout(text):
        out = {}
        for struct, n in re.findall(r'size_of::<(Qimgv\w+)\>\(\) == (\d+)', text):
            out[('qimgv_' + pascal_to_snake(struct.removeprefix('Qimgv')), '')] = int(n)
        for struct, field, n in re.findall(r'offset_of!\((Qimgv\w+), (\w+)\) == (\d+)', text):
            out[('qimgv_' + pascal_to_snake(struct.removeprefix('Qimgv')), field)] = int(n)
        return out

    c, r = c_layout(text_of(HEADER)), rust_layout(text_of(RUST))
    for key in sorted(set(c) | set(r)):
        struct, field = key
        where = f"{struct}.{field}" if field else struct
        if key not in c:
            failures.append(f"R5 unpinned in the C header: {where}")
        elif key not in r:
            failures.append(f"R5 unpinned in Rust: {where}")
        elif c[key] != r[key]:
            failures.append(f"R5 ABI layout disagrees: {where} is {c[key]} in qimgv_engine.h, {r[key]} in lib.rs")

# Rule 6: no class re-declares a signal an ancestor already declares. Qt gives the
# derived class its OWN signal when the name is re-declared, so a consumer that
# connects through &Base::sig silently receives nothing — measured, not theorised:
# a base-pointer connection to a re-declared signal got 0 deliveries while the
# derived one got 1. That is how "video finished, advance" was dead: the proxy
# re-declared playbackFinished while ViewerWidget connects via
# &VideoPlayer::playbackFinished (viewerwidget.cpp:53).
BASE_RE = re.compile(r'^\s*class\s+(\w+)\b[^;{]*?:\s*((?:\w+\s*:\s*)?public\s+\w+(?:\s*,\s*public\s+\w+)*)')
SIG_RE = re.compile(r'^\s*(?:void|[A-Za-z_]\w*<[^>]*>)\s+(\w+)\s*\([^;]*\)\s*;')
MODE_END = re.compile(r'^\s*(public|protected|private|signals|Q_SIGNALS|slots)\b')
signals_of, parents = {}, {}
for root in (SRC, os.path.join(ROOT, 'plugins')):  # R6's own walk: plugins carry signals too
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ('3rdparty', 'res', 'translations')]
        for f in filenames:
            if not f.endswith('.h'):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
            scanned['R6'] = scanned.get('R6', 0) + 1
            current, in_signals = None, False
            for line in text_of(path).splitlines():
                m = BASE_RE.match(line)
                if m:
                    current = m.group(1)
                    signals_of.setdefault(current, set())
                    parents.setdefault(current, set())
                    parents[current] |= {b.strip().split()[-1] for b in m.group(2).split(',')}
                    in_signals = False
                    continue
                if not current:
                    continue
                e = MODE_END.match(line)
                if e:
                    in_signals = e.group(1) in ('signals', 'Q_SIGNALS')
                    continue
                if in_signals:
                    s = SIG_RE.match(line)
                    if s:
                        signals_of[current].add(s.group(1))

def ancestors(name, seen=()):
    for parent in parents.get(name, ()):
        if parent in seen:
            continue
        yield parent
        yield from ancestors(parent, seen + (parent,))

for name, sigs in sorted(signals_of.items()):
    for parent in sorted(set(ancestors(name))):
        for sig in sorted(sigs & signals_of.get(parent, set())):
            failures.append(f"R6 signal re-declared: {name}::{sig}() also exists in {parent} — "
                            f"connections made through &{parent}::{sig} will never fire")

# Rule 7: the mirror has exactly one writer family. fileEntryVec/dirEntryVec may
# only be mutated inside the engine-apply functions; every other local write is
# the forceInsert-shaped exception that retired this rule's subject. Deterministic
# single-file scan (function attribution by definition regex) — not the rejected
# line-level lock heuristic: no sampling, no green-means-sampled downgrade.
# The appliers reach the vectors through one alias and one splice helper, so the
# scan covers all three forms: a named mutation, an alias mutation, a call of the
# helper. An alias under another name would be invisible, which is why ALIAS_RE
# fails the rule instead of quietly going green — a writer set nobody can see is
# not a rule.
MIRROR_WRITERS = {'applyArena', 'removeFileEntry', 'removeDirEntry',
                  'applyEngineModification', 'applyEngineRename',
                  'onEngineMutated', 'insertAtEngineIndex'}
MUT_RE = re.compile(r'(?:(?:file|dir)EntryVec|\bvec)\.(push_back|insert|erase|clear|assign)')
FN_RE = re.compile(r'^[a-zA-Z].*\bDirectoryManager::(\w+)')
FREE_FN_RE = re.compile(r'^(?:static\s+)?[A-Za-z_][A-Za-z0-9_:<>&*]*\s+\*?(\w+)\(')
SPLICE_RE = re.compile(r'\binsertAtEngineIndex\(')
ALIAS_RE = re.compile(r'std::vector<FSEntry>\s*&\s*(\w+)')
dm_path = os.path.join(SRC, 'components', 'directorymanager', 'directorymanager.cpp')
if os.path.exists(dm_path):
    scanned['R7'] = scanned.get('R7', 0) + 1
    dm_text = text_of(dm_path)
    fn = '?'
    for lineno, line in enumerate(dm_text.splitlines(), 1):
        m = FN_RE.match(line) or FREE_FN_RE.match(line)
        if m:
            fn = m.group(1)
        writes = MUT_RE.search(line) or SPLICE_RE.search(line)
        if writes and fn not in MIRROR_WRITERS:
            failures.append(f"R7 mirror writer: directorymanager.cpp:{lineno} mutates the "
                            f"listing inside {fn}() — only {sorted(MIRROR_WRITERS)} may")
    for alias in sorted(set(ALIAS_RE.findall(dm_text)) - {'vec'}):
        failures.append(f"R7 blind alias: directorymanager.cpp binds std::vector<FSEntry>& to "
                        f"'{alias}' — the scan only sees 'fileEntryVec', 'dirEntryVec' and 'vec', "
                        f"so renaming the appliers' alias hides new writers instead of failing")
else:
    failures.append("R7 cannot run: directorymanager.cpp missing — scanned 0 files is not a pass")

# Rule 8: the settings table is the only description of a setting's bounds.
# A raw QSettings read of a key the table declares bypasses the row's junk and
# range policy — which is how JPEGSaveQuality ended up saving at quality 0 on a
# corrupt config (QSettings applies its second argument only to a MISSING key,
# junk converts to 0, and 0 was inside the declared range). Keys whose accessor
# maps the stored value to something the row cannot express are listed here
# deliberately, the way R2 lists authorized FFI callers: adding a name to this
# set is the review, and the review has to be read.
TABLE_KEYS = set()
table_path = os.path.join(SRC, 'settings_table.h')
if os.path.exists(table_path):
    # The factory NAME is not part of the contract: a row is
    # `<anything>("key", ...)` assigned into a SettingDesc. Matching a fixed
    # name list made this rule go blind without saying so when the factories
    # were renamed off the Win32 BOOL/INT typedef collision (2026-11-18):
    # twelve of the nineteen rows still parsed (REAL/STR kept their names),
    # so the "parsed to zero" guard below stayed quiet while 56 keys dropped
    # out of the set — and an injected raw read of confirmTrash/JPEGSaveQuality
    # passed green. Match the shape, and hold a floor so the next rename is a
    # red by name instead of a silent half-scan.
    TABLE_KEYS = set(re.findall(r'=\s*[A-Za-z_]\w*\(\s*"([^"]+)"', text_of(table_path)))
R8_KEY_FLOOR = 50  # 63 rows today; the floor is the blindness alarm, not a count
RAW_BYPASS = re.compile(r'(?:settingsConf|stateConf|themeConf)->(?:value|setValue)\(\s*"([^"]+)"')
MAPPING_EXEMPT = {
    'mpvBinary',      # resolves to a fallback binary by file existence
    'panelPosition',  # maps a string to an enum member, else-branch is the default
}
settings_path = os.path.join(SRC, 'settings.cpp')
if len(TABLE_KEYS) < R8_KEY_FLOOR:
    failures.append(f"R8 cannot run: settings_table.h parsed to {len(TABLE_KEYS)} keys "
                    f"(floor {R8_KEY_FLOOR}) — the row shape changed, fix this rule's parse "
                    "before trusting the tree; a half-scanned table passes raw reads")
elif os.path.exists(settings_path):
    scanned['R8'] = scanned.get('R8', 0) + 1
    for lineno, line in enumerate(text_of(settings_path).splitlines(), 1):
        for key in RAW_BYPASS.findall(line):
            if key in TABLE_KEYS and key not in MAPPING_EXEMPT:
                failures.append(f"R8 table key read raw: settings.cpp:{lineno} uses \"{key}\" "
                                "through QSettings directly — go through load/store and its row")

# Rule 9: the Thumbnailer has exactly one owner — Core. The grid and the strip
# each constructing their own made every broadcast EngineBridge::thumbNext run
# the same decode twice (the engine dedups requests, but the event fans out to
# every listener with no ownership guard), hand two mpv writers one frame file,
# and double every report path. Ownership is the value member: a reference
# member (`Thumbnailer &name;`) is the shared form and stays legal anywhere.
THUMB_VALUE_OWNER = re.compile(r'\bThumbnailer\s+\w+\s*(?:;|=)')
for path, rel in sources():
    if rel.startswith('tests'):
        continue
    scanned['R9'] = scanned.get('R9', 0) + 1
    for lineno, line in enumerate(text_of(path).splitlines(), 1):
        if THUMB_VALUE_OWNER.search(line) and rel != 'core.h':
            failures.append(f"R9 second Thumbnailer owner: {rel}:{lineno} declares a Thumbnailer "
                            "value member — the process has exactly one, owned by Core; "
                            "take it by reference")

# Rule 10: fd.rs is the watcher's syscall shell, and its function inventory is
# frozen. The watcher's decision logic — classification, the cookie pairing,
# what a record stream means — lives in watcher/pair.rs and watcher/apply.rs,
# the surfaces Miri runs. fd.rs is excluded from the hook's Miri trigger
# (real syscalls), so a decision that sneaks back into this file escapes every
# instrument silently: native cargo test would pass it, Miri never sees it,
# and nothing else would notice. A new fn here fails the rule; adding a name
# to the set is the review, the way R2's caller set and R7's writer set are.
FD_FNS = {'spawn', 'send', 'shutdown', 'drop', 'new_eventfd', 'watcher_loop'}
fd_path = os.path.join(ROOT, 'engine', 'qimgv-engine', 'src', 'watcher', 'fd.rs')
if not os.path.exists(fd_path):
    failures.append("R10 cannot run: watcher/fd.rs missing — scanned 0 files is not a pass")
else:
    scanned['R10'] = scanned.get('R10', 0) + 1
    fd_text = open(fd_path, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'\bfn\s+([a-z_][a-z0-9_]*)', fd_text):
        if m.group(1) not in FD_FNS:
            line = fd_text[:m.start()].count('\n') + 1
            failures.append(f"R10 decision logic in the syscall shell: watcher/fd.rs:{line} defines "
                            f"fn {m.group(1)} — the watcher's decisions live in pair.rs/apply.rs "
                            "(the surfaces Miri runs); extend FD_FNS only after reading the purity seam")
    if re.search(r'#\[cfg\(test\)\]|#\[test\]', fd_text):
        failures.append("R10 uninstrumented tests: watcher/fd.rs carries test code — it is excluded "
                        "from the hook's Miri trigger, so its tests never run under the interpreter "
                        "and fake their coverage; tests for fd behavior live in watcher_tests.rs "
                        "(the real-machinery pins) or in pair.rs/apply.rs (the pure layer)")

# The same rule over the Windows shell: fd_windows.rs is the ReadDirectoryChangesW
# twin of fd.rs (same WatchCmd/WatcherHandle contract, same purity seam), and its
# inventory is frozen for the same reason — decisions live in pair.rs/apply.rs.
FD_WIN_FNS = {'new', 'set', 'reset', 'raw', 'drop', 'spawn', 'send', 'shutdown',
              'watcher_loop', 'retire_watch', 'retire_gone', 'arm_read', 'open_dir', 'drain_records',
              'u32_at',
              # drain_aborted is retire machinery, not a decision: CancelIoEx
              # plus a bounded wait for the abort completion (a delete-pending
              # directory can hold the notify IRP past the cancel, and the
              # unbounded wait it replaces hung Engine::drop — 2026-11-17). It
              # classifies nothing and yields nothing to the stream.
              'drain_aborted'}
fdw_path = os.path.join(ROOT, 'engine', 'qimgv-engine', 'src', 'watcher', 'fd_windows.rs')
if not os.path.exists(fdw_path):
    failures.append("R10 cannot run: watcher/fd_windows.rs missing — scanned 0 files is not a pass")
else:
    scanned['R10'] = scanned.get('R10', 0) + 1
    fdw_text = open(fdw_path, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'\bfn\s+([a-z_][a-z0-9_]*)', fdw_text):
        if m.group(1) not in FD_WIN_FNS:
            line = fdw_text[:m.start()].count('\n') + 1
            failures.append(f"R10 decision logic in the syscall shell: watcher/fd_windows.rs:{line} defines "
                            f"fn {m.group(1)} — the watcher's decisions live in pair.rs/apply.rs "
                            "(the surfaces Miri runs); extend FD_WIN_FNS only after reading the purity seam")
    if re.search(r'#\[cfg\(test\)\]|#\[test\]', fdw_text):
        failures.append("R10 uninstrumented tests: watcher/fd_windows.rs carries test code — it is excluded "
                        "from the hook's Miri trigger, so its tests never run under the interpreter "
                        "and fake their coverage; tests for fd behavior live in watcher_tests.rs "
                        "(the real-machinery pins) or in pair.rs/apply.rs (the pure layer)")

# The glue both shells share — the WatchCmd vocabulary, the stream dispatch,
# the baseline enumeration — is the contract, not the shell: it lives in
# watcher.rs and is not frozen by this rule.

# Rule 11: keyboard policy is a closed list of doors. Every keyPressEvent
# override must live in ALLOWED_KEY_DOORS — a new door outside the list is
# exactly how the up-arrow fit-width easter egg stayed invisible for years:
# it intercepted keys before the action system, in a door no reader suspected.
ALLOWED_KEY_DOORS = {
    'gui/contextmenu.cpp',                   # popup: item navigation, no forwarding
    'gui/folderview/foldergridview.cpp',     # widget-standard grid navigation
    'gui/customwidgets/thumbnailview.cpp',   # shift state for range selection
    'gui/folderview/treeviewcustom.cpp',     # widget-standard tree navigation
    'gui/folderview/fvoptionspopup.cpp',     # Esc-only popup
    'gui/crop/croppanel.cpp',    # modal editing surface: crop workflow keys
    'gui/crop/cropoverlay.cpp',          # crop workflow keys; save chords commit the pending crop
    'gui/overlays/renameoverlay.cpp',        # modal input with a curated passthrough
    'gui/overlays/copyoverlay.cpp',          # modal surface with its own per-destination hotkeys
    'gui/overlays/saveconfirmoverlay.cpp',   # modal prompt: its three answers + Esc, rest to the action system
    'gui/dialogs/resizedialog.cpp',          # modal dialog: Enter applies, Esc rejects
    'gui/overlays/changelogwindow.cpp',      # Esc-only window
    'gui/customwidgets/styledcombobox.cpp',  # input routing (ignore-all)
    'gui/customwidgets/spinboxinputfix.cpp', # numeric input guard
    'gui/customwidgets/keysequenceedit.cpp', # the keymap's capture device
}
for path, rel in sources():
    if rel.startswith('tests/'):
        continue
    scanned['R11'] = scanned.get('R11', 0) + 1
    if '::keyPressEvent(' in text_of(path) and rel not in ALLOWED_KEY_DOORS:
        failures.append(f"R11 unauthorized key door: {rel} (add to ALLOWED_KEY_DOORS only after review)")

# Rule 11b: the ShortcutOverride veto is the same door, new hinges. A surface
# that handles QEvent::ShortcutOverride decides which keys reach QShortcutMap —
# a claim outside this list is a second easter-egg hatch: it can silently
# suppress every binding in the table, from a door no reader suspected.
ALLOWED_OVERRIDE_DOORS = {
    'gui/overlays/saveconfirmoverlay.cpp',   # the walk/view/exit keys are the prompt's to consume
    'gui/overlays/copyoverlay.cpp',          # digits 1-9 are its per-destination hotkeys
    'gui/crop/cropoverlay.cpp',              # the save chord commits the pending crop first
    'gui/customwidgets/spinboxinputfix.cpp', # the spin box owns its editing keys (arrows included)
    'gui/overlays/changelogwindow.cpp',      # Esc-local what's-new popup (filter on the focused button)
    'gui/contextmenu.cpp',                   # popup: owns its keyboard outright
    'gui/folderview/fvoptionspopup.cpp',     # popup: owns its keyboard outright
    'gui/customwidgets/keysequenceedit.cpp', # the capture device takes every raw key
    'gui/overlays/renameoverlay.cpp',        # filter on the focused line edit; exit chords pass through
    'gui/folderview/treeviewcustom.cpp',     # the tree owns its keys (navigation + search)
    'gui/customwidgets/styledcombobox.cpp',  # ignore-all routing: every key must be delivered to be ignored
}
for path, rel in sources():
    if rel.startswith('tests/'):
        continue
    scanned['R11b'] = scanned.get('R11b', 0) + 1
    if 'QEvent::ShortcutOverride' in text_of(path) and rel not in ALLOWED_OVERRIDE_DOORS:
        failures.append(f"R11b unauthorized ShortcutOverride claim: {rel} (add to ALLOWED_OVERRIDE_DOORS "
                        "only after review)")

# Rule 12: every registered action must be connected. invokeAction resolves
# ACTION_NORMAL by triggering the registered QAction — a registration without
# a connectAction is an action that shows in the keymap, is invokable, and
# does absolutely nothing. Registration lives in actionmanager.cpp
# (initActions/addAction), connection in core.cpp (connectAction); the two
# files must agree in both directions. A connectAction naming an unknown
# action is also caught at runtime by the qFatal in connectAction; this scan
# reports it earlier and by name. runScript is a plain signal carrier for the
# dynamic script actions (no QAction exists for "s:<name>"), connects by the
# signal idiom instead of connectAction, and is expected here.
am_text = text_of(os.path.join(SRC, 'components', 'actionmanager', 'actionmanager.cpp'))
connected_actions = set(re.findall(r'connectAction\("([^"]+)"', text_of(os.path.join(SRC, 'core.cpp'))))

# The action census lives in actions_table.h now (actions_table.h's own
# census test proves it equal to the live registry at runtime). R12 reads
# the TABLE, and — while the registry is still written out by hand — also
# the old `addAction("...")` list, asserting the two agree. That two-source
# window is deliberate: this rule once caught real drift, and a rule whose
# input silently changes identity is indistinguishable from a passing one.
# When the registry loop replaces the hand-written list, the old list parse
# goes away WITH its assertion, and the floor below keeps the rule from
# passing on an empty parse.
actions_table_text = text_of(os.path.join(SRC, 'actions_table.h'))
table_actions = set(re.findall(r'MAKE\("([^"]+)"', actions_table_text))
registered_actions = table_actions
R12_ACTION_FLOOR = 75  # 81 today; the floor is the "rule went blind" alarm
if len(table_actions) < R12_ACTION_FLOOR:
    failures.append(f"R12 cannot run: actions_table.h parsed to {len(table_actions)} actions "
                    f"(floor {R12_ACTION_FLOOR}) — the row shape changed, fix this rule's parse "
                    "before trusting the tree")
# initActions is a loop over the table now, so a hand-written list here would
# be a second registry — the exact shape this rule exists to prevent. Assert
# the loop is what the file has, not only that some list agrees with the table.
if not re.search(r'void ActionManager::initActions\(\) \{\s*for\(const ActionDesc &row : ActionTable::kActions\)',
                 am_text):
    failures.append("R12 initActions is no longer the table loop — a second registry has "
                    "appeared; the table is the only place an action may be declared")
for name in sorted(registered_actions - connected_actions):
    failures.append(f"R12 registered action is never connected: {name} — add the connectAction "
                    "in core.cpp or remove the registration")
for name in sorted(connected_actions - registered_actions):
    failures.append(f"R12 connectAction names an unregistered action: {name} — fix the name or "
                    "register it in ActionManager::initActions")
scanned['R12'] = scanned.get('R12', 0) + 1

# Rule 12b: the hardcoded default keymaps must name registered actions.
# validateShortcuts validates only the config entries, never the defaults —
# a typo in initDefaults was a binding the settings table shows and that
# does nothing, silently. This partially restores the compile-time check the
# old &ActionManager::signal connects gave for free.
# The bindings are rows now. Read them from the table, and while initDefaults
# still spells them out, assert the two agree — the same two-source window as
# R12, for the same reason (a typo'd default was a binding the editor showed
# and that did nothing, silently).
table_bindings = re.findall(r'\{"([^"]+)",\s*Scope::\w+,\s*"([^"]+)"\}', actions_table_text)
default_actions = {m[0] for m in table_bindings}
R12B_BINDING_FLOOR = 70  # 83 today
if len(table_bindings) < R12B_BINDING_FLOOR:
    failures.append(f"R12b cannot run: actions_table.h parsed to {len(table_bindings)} bindings "
                    f"(floor {R12B_BINDING_FLOOR}) — fix the parse before trusting the tree")
for name in sorted(default_actions - registered_actions):
    failures.append(f"R12b default keymap names an unregistered action: {name} — fix the "
                    "row in actions_table.h")
# same for the bindings: initDefaults is the table loop, or it is a second
# keymap. The floor above already proved the table parses; this proves the
# code reads it.
if not re.search(r'void ActionManager::initDefaults\(\) \{[^}]*for\(const DefaultBinding &binding : '
                 r'ActionTable::kDefaultBindings\)',
                 am_text, re.S):
    failures.append("R12b initDefaults is no longer the bindings loop — a second default keymap "
                    "has appeared; the table is the only place a default may be declared")
scanned['R12b'] = scanned.get('R12b', 0) + 1

# Rule 12c: the global default table shares no live claim with any scope
# table. QShortcutMap has no context precedence — two live claims of one key
# are ambiguous and NEITHER fires (dispatch_probe_tests pins this) — and the
# global table's claims are always live (ApplicationShortcut), so a token it
# shares with the document/grid/inspect tables is a dead key in that scope.
# 7b20e6e6 retired the central de-claim believing one key lived in exactly
# one table by construction; Enter and Backspace still lived in two, and
# gridOpen/goUp went silent under a green suite. Scope-vs-scope pairs are
# exempt: the grid's and the document's claims are focus-exclusive (their
# subtrees cannot hold focus at once), and inspect is a variant OF the
# document table (rebuildDocumentShortcuts folds it in as a replacement, it
# never competes). Pointer tokens are exempt: they never ride QShortcutMap.
# "Enter" is the one token claiming two sequences (Return + keypad Enter),
# so tokens are compared through the sequences they expand to.
POINTER_TOKENS = {'WheelUp', 'WheelDown', 'LMB', 'RMB', 'MiddleButton',
                  'XButton1', 'XButton2'}  # + *_DoubleClick — mirrors isPointerToken


def _sequences_of(token):
    if token in ('Enter', 'Return'):
        return {'Enter', 'Return'}
    return {token}


# The bindings are read from actions_table.h — the declarative source both the
# projection and the config load will use — instead of scraping initDefaults'
# call expressions. While initDefaults still spells them out, R12b asserts the
# two agree, so this rule is not alone in trusting the new source.
table_tokens = {}
for _action, _scope, _token in re.findall(r'\{"([^"]+)",\s*Scope::(\w+),\s*"([^"]+)"\}', actions_table_text):
    if _token in POINTER_TOKENS or _token.endswith('_DoubleClick'):
        continue
    table_tokens.setdefault(_scope, {})[_token] = _sequences_of(_token)
if 'Global' not in table_tokens:
    failures.append("R12c the global bindings were not found in actions_table.h — the rule is "
                    "blind, fix the parse before trusting the tree")
else:
    for scope in ('Document', 'Grid', 'Inspect'):
        for g_tok, g_seqs in sorted(table_tokens.get('Global', {}).items()):
            for s_tok, s_seqs in sorted(table_tokens.get(scope, {}).items()):
                if g_seqs & s_seqs:
                    failures.append(f"R12c global table shares a live claim with {scope}: "
                                    f"'{g_tok}' vs '{s_tok}' — two live claims of one key are "
                                    "ambiguous and fire nothing (dispatch_probe_tests); move the "
                                    "binding into the scope that owns it")
scanned['R12c'] = scanned.get('R12c', 0) + 1

# Rule 13: no suite spawns itself. The per-unit process isolation the hand-
# written slot_harness used to provide (QProcess re-launching the suite once per
# unit, plus a re-entry guard against the 2026-11-16 fork bomb) is now the test
# framework's: gtest_discover_tests registers every TEST_F as its own ctest
# entry, so each test function already gets a private process — and with it a
# private TSan thread registry, the reason the harness existed at all.
#
# The failure mode the old harness guarded against is not gone, it changed
# owner: a suite that relaunches its own executable from inside a test re-enters
# that test and spawns again, unbounded. GoogleTest never does that on its own,
# so the rule is a plain absence check on the one thing that can re-enter a
# test — QCoreApplication::applicationFilePath, the suite's own binary.
# QProcess itself stays legal: thumbnailer_tests starts `sleep`/`ping` to pin
# waitForProcess against a real child.
for path, rel in sources():
    if not rel.startswith('tests/'):
        continue
    text = text_of(path)
    if 'applicationFilePath' not in text:
        continue
    scanned['R13'] = scanned.get('R13', 0) + 1
    failures.append(f"R13 self-spawn: {rel} launches its own executable "
                    "(QCoreApplication::applicationFilePath) — a test that re-runs the "
                    "suite re-enters itself and spawns again (the 2026-11-16 fork bomb). "
                    "Per-test process isolation is gtest_discover_tests' job now")

# Rule 14: no QTest assertion macro in a GoogleTest translation unit, in ANY
# preprocessor branch. gtest_qt.h undefs and #pragma-GCC-poison's them, which
# is the primary guard — but poison is a preprocessor-time check on code that
# is actually preprocessed, and the branches that matter most are the ones
# THIS platform does not compile: a `#ifdef Q_OS_WIN` block reached for QSKIP
# and the Linux-green build never saw it (2026-11-20, thumbnail_store_tests).
# The same blindness would hit a Windows-only QVERIFY. So the rule is textual:
# the macro names may appear in a comment, never as code.
POISONED_MACROS = re.compile(r'\b(QVERIFY2?|QCOMPARE|QFAIL|QSKIP|QEXPECT_FAIL|QTRY_VERIFY2?|QTRY_COMPARE'
                             r'|QTRY_VERIFY_WITH_TIMEOUT|QTRY_VERIFY2_WITH_TIMEOUT|QTRY_COMPARE_WITH_TIMEOUT)\b')
for path, rel in sources():
    if not rel.startswith('tests/') or rel == 'tests/gtest_qt.h':
        continue
    for lineno, line in enumerate(text_of(path).splitlines(), 1):
        code = line.split('//')[0]
        hit = POISONED_MACROS.search(code)
        if hit:
            failures.append(f"R14 QTest macro in a gtest TU: {rel}:{lineno} uses '{hit.group(1)}' — "
                            "it reports to a result object that only exists inside QTest::qExec, so "
                            "it is silently green here. Use EXPECT_*/ASSERT_*, ASSERT_TRY_*, or "
                            "GTEST_SKIP(); the compile-time poison cannot see a branch this "
                            "platform does not preprocess")
            break
    scanned['R14'] = scanned.get('R14', 0) + 1

if failures:
    print("architecture violations:")
    for f in failures:
        print("  ", f)
    sys.exit(1)
print("architecture rules OK — " + ", ".join(f"{k} ({scanned.get(k, 0)} files)" for k in sorted(scanned)))
