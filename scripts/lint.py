#!/usr/bin/env python3
# Static analysis on demand. Four tools, one table, one rule: a count may only
# shrink. The families that are already at zero do not live here — they are
# -Werror'd in the build itself (see the root CMakeLists); this script reports
# the debt that would be noise to compile in, so that it can be watched instead
# of ignored. Families judged noise by policy are not baselined either — they
# are filtered out of the scan (GATED/REPORT/FROZEN below), with the rationale
# in lint-baseline.txt.
#
#   scripts/lint.py [build-dir]          report only
#   scripts/lint.py [build-dir] --gate   fail if any count moved
#   scripts/lint.py [build-dir] --no-cache   bypass the cache entirely
#
# gcc, clazy and clang-tidy are per-TU and cached in <build>/.lint-cache: the key
# hashes
# the command line, the tool version, the TU and every transitively included
# header (contents, never mtimes — cp/mv carry stale mtimes), plus the -I
# directory listings (a new file on an include path can shadow an existing
# include without any tracked file changing). The include graph comes from
# the gcc pass's -MD depfile; a missing/unreadable depfile or any listed file
# means a miss, never a guess. Cached records hold raw tool output so parser
# changes never invalidate them. cppcheck is whole-program and always runs.
#
# The build dir must have a compile_commands.json, which the root CMakeLists now
# generates by default. Every tool reads its command line from that database
# instead of flags guessed at here — a hand-rolled cppcheck invocation once
# reported this tree as unanalyzable, which is how a tool that scans nothing
# looks identical to one that works.
import collections, concurrent.futures, hashlib, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = next((a for a in sys.argv[1:] if not a.startswith('-')), os.path.join(ROOT, 'build'))
GATE = '--gate' in sys.argv
NOCACHE = '--no-cache' in sys.argv
CACHE = os.path.abspath(os.path.join(BUILD, '.lint-cache'))
BASELINE = os.path.join(ROOT, 'scripts', 'lint-baseline.txt')

# fatal in the build, so they must be zero here too. Keys match what gcc prints
# inside [-W...]: the earlier W-prefixed spelling never matched a single
# diagnostic, so those families silently fell through to report-only and the
# build's -Werror was doing all the work.
GATED = {'sign-compare', 'use-after-free', 'unused-parameter'}
# reported, baseline-controlled. -Wconversion/float-conversion are retired from
# the scan entirely (REPORT drives the gcc pass cost): the bulk is GUI
# coordinate truncation that is the intent, not a bug, and scanning them cost
# every TU on every run while producing nothing anyone acts on.
REPORT = ['-Wshadow', '-Wold-style-cast', '-Wsuggest-override']
# policy-frozen families: the findings are real, but every fix was judged a
# non-improvement for this tree, so they are filtered out instead of counted —
# the numbers cannot rot, and the gate stops paying to re-derive them.
#   cppcheck passedByValue / duplInheritedMember — Qt's implicit sharing and
#   signal re-declaration idioms make these recommendations wrong here.
FROZEN = {'duplInheritedMember'}  # passedByValue re-armed at zero (2026-11-05)
# clang-tidy: the bugprone census (2026-11-10). Excluded at selection, not
# post-filtered — the gate stops paying to re-derive judged noise (same
# philosophy as REPORT driving the gcc pass). Rationale per family lives in
# lint-baseline.txt.
TIDY_EXCLUDE = ['bugprone-narrowing-conversions',
                'bugprone-easily-swappable-parameters',
                'bugprone-integer-division',
                'bugprone-parent-virtual-call',
                'bugprone-implicit-widening-of-multiplication-result']
TIDY_CHECKS = '-*,bugprone-*' + ''.join(',-' + c for c in TIDY_EXCLUDE)
DIAG = re.compile(r'^(/[^:]+):(\d+):\d+: (?:warning|error): .*?\[-?W?([a-z0-9-]+)\]$')
CLAZY = re.compile(r'^(/[^:]+):(\d+):\d+: warning: .* \[-Wclazy-([a-z0-9-]+)\]$')
TIDY = re.compile(r'^(/[^:]+):(\d+):\d+: (?:warning|error): .*?\[(bugprone-[a-z0-9-]+)(?:,.*)?\]$')
CPPCHECK = re.compile(r'^(/[^:]+):(\d+):\d+: (?:style|warning|performance|portability): .* \[([a-zA-Z0-9_]+)\]$')
SKIP = ('/3rdparty/', '_autogen/', '/corrosion/')

def entries():
    db = os.path.join(BUILD, 'compile_commands.json')
    if not os.path.exists(db):
        sys.exit(f'no compile_commands.json in {BUILD} — configure that directory first')
    return [e for e in json.load(open(db)) if not any(s in e['file'] for s in SKIP)]

def instrument_versions(srcs):
    # Verdicts belong to the tool that issued them. A compiler or clazy upgrade
    # moves counts under an unchanged (file, command) set, and the gate would
    # then read that as the source having changed — loud, but misattributed.
    # The path is already in the command; the version is not, so it goes in
    # here. The whole output, not the first line: clazy-standalone prints LLVM's
    # version first and its own ("clazy version 1.17") on the third line, so a
    # first-line slice would miss exactly the tool whose counts proved unstable.
    tools = sorted({e['command'].split()[0] for e in srcs}) + ['clazy-standalone', 'clang-tidy', 'cppcheck']
    return [[t, run([t, '--version'], ROOT).strip()] for t in tools]

def fingerprint(srcs):
    # The baseline counts are only meaningful for the instrument state that
    # produced them. clazy's verdicts proved sensitive to that state: the same
    # source measured 14 and 18 across sessions whose compile_commands.json had
    # been regenerated by different configure contexts (a local act run
    # re-configures the build dir in its container). The fingerprint pins the
    # scanned (file, command) set and the versions of the tools that judged it;
    # a change means the instrument itself moved and the counts must be
    # re-verified deliberately, not absorbed silently.
    payload = json.dumps([[e['file'], e['command']] for e in srcs] + instrument_versions(srcs),
                         sort_keys=True)
    return hashlib.sha256(payload.encode()).hexdigest()[:16]

def run(cmd, cwd):
    p = subprocess.run(cmd, shell=isinstance(cmd, str), cwd=cwd, capture_output=True, text=True)
    return p.stdout + p.stderr

def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()[:16]

def depfile_path(e):
    return os.path.join(CACHE, 'deps', sha(e['file']) + '.d')

def read_deps(e):
    try:
        text = open(depfile_path(e)).read()
    except OSError:
        return None
    return text.replace('\\\n', ' ').partition(':')[2].split()

def content_hashes(files, memo):
    # None on any unreadable file: the caller must treat the whole key as a
    # miss. A deleted header that is still included is a real state change,
    # not something to replay a cached verdict for.
    out = []
    for f in sorted(set(files)):
        if f not in memo:
            try:
                memo[f] = hashlib.sha256(open(f, 'rb').read()).hexdigest()[:16]
            except OSError:
                memo[f] = None
        if memo[f] is None:
            return None
        out.append([f, memo[f]])
    return out

def include_listings(e):
    dirs = [a or b for a, b in re.findall(r'-I\s*(\S+)|-isystem\s*(\S+)', e['command'])]
    out = []
    for d in sorted(set(dirs)):
        d = os.path.normpath(os.path.join(e['directory'], d))
        try:
            listing = sorted(os.listdir(d))
        except OSError:
            listing = None
        out.append([d, listing])
    return out

def tu_key(e, tool, command, version, deps, memo):
    if deps is None:
        return None
    hashes = content_hashes([e['file']] + deps, memo)
    if hashes is None:
        return None
    return sha(json.dumps([tool, command, version, hashes, include_listings(e)], sort_keys=True))

def cache_read(tool, key):
    if key is None:
        return None
    try:
        return open(os.path.join(CACHE, tool, key + '.out')).read()
    except OSError:
        return None

def cache_write(tool, key, out):
    if key is None:
        return
    d = os.path.join(CACHE, tool)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, key + '.out'), 'w') as f:
        f.write(out)

def cache_gc(sub, used, suffix):
    d = os.path.join(CACHE, sub)
    if not os.path.isdir(d):
        return
    for name in os.listdir(d):
        if name.endswith(suffix) and name[:-len(suffix)] not in used:
            os.unlink(os.path.join(d, name))

def syntax_command(e):
    # the real command line, minus the object file and minus -Werror: this pass
    # is a report, and the build's own gate has already had its say
    cmd = re.sub(r'\s-o\s+\S+', ' -fsyntax-only', e['command'], count=1)
    cmd = re.sub(r'-Werror(=\S+)?', '', cmd)
    return cmd + ' ' + ' '.join(REPORT)

def gcc_counts(srcs, versions, memo):
    found = collections.defaultdict(set)
    if not NOCACHE:
        os.makedirs(os.path.join(CACHE, 'deps'), exist_ok=True)
    def one(e):
        cmd = syntax_command(e)
        version = versions[e['command'].split()[0]]
        key = None if NOCACHE else tu_key(e, 'gcc', cmd, version, read_deps(e), memo)
        out = cache_read('gcc', key)
        hit = out is not None
        if not hit:
            if NOCACHE:
                out = run(cmd, e['directory'])
            else:
                # -MD writes the include graph as a side effect of the pass we
                # already pay for. Via a temp path: if gcc dies before writing
                # it, nothing is promoted and nothing is cached — a crash
                # output must never be replayed as a verdict.
                tmp = depfile_path(e) + '.tmp'
                out = run(cmd + f' -MD -MF {tmp}', e['directory'])
                try:
                    os.replace(tmp, depfile_path(e))
                except OSError:
                    parse_gcc(out, found)
                    return (False, None)
                # key from the fresh graph: a content change may have added or
                # removed includes, so the pre-run key is not the one this
                # output belongs to
                key = tu_key(e, 'gcc', cmd, version, read_deps(e), memo)
                cache_write('gcc', key, out)
        parse_gcc(out, found)
        return (hit, key)
    def parse_gcc(out, found):
        for line in out.splitlines():
            m = DIAG.match(line)
            if m:
                found[m.group(3)].add((m.group(1), m.group(2)))
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        results = list(ex.map(one, srcs))
    if not NOCACHE:
        cache_gc('gcc', {k for _, k in results if k}, '.out')
        cache_gc('deps', {sha(e['file']) for e in srcs}, '.d')
        print(f'cache: gcc {sum(h for h, _ in results)} hits, '
              f'{sum(not h for h, _ in results)} misses')
    return found

def clazy_counts(srcs, version, memo):
    found = collections.defaultdict(set)
    def one(e):
        # the include graph is the gcc pass's depfile — same TU, same db
        # command, so the graph is identical and needs no re-derivation
        key = None if NOCACHE else tu_key(e, 'clazy', e['command'], version, read_deps(e), memo)
        out = cache_read('clazy', key)
        hit = out is not None
        if not hit:
            out = run(['clazy-standalone', '-p', BUILD, e['file']], ROOT)
            cache_write('clazy', key, out)
        for line in out.splitlines():
            m = CLAZY.match(line)
            if m:
                found[m.group(3)].add((m.group(1), m.group(2)))
        return (hit, key)
    # 8, not nproc: clazy is clang-tidy and each TU pays for the AST twice
    # (~411 MiB peak RSS). Measured on 133 TUs: 4 workers 114s, 8 workers 64s,
    # 16 workers 54s — past 8 the returns flatten (memory-bandwidth/header-I/O
    # contention), so 8 is the sweet spot.
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(one, srcs))
    if not NOCACHE:
        cache_gc('clazy', {k for _, k in results if k}, '.out')
        print(f'cache: clazy {sum(h for h, _ in results)} hits, '
              f'{sum(not h for h, _ in results)} misses')
    return found

def clang_tidy_counts(srcs, version, memo):
    found = collections.defaultdict(set)
    def one(e):
        # the checks selection is part of the verdict, so it rides in the key
        key = None if NOCACHE else tu_key(e, 'clang-tidy', e['command'] + ' ' + TIDY_CHECKS,
                                          version, read_deps(e), memo)
        out = cache_read('clang-tidy', key)
        hit = out is not None
        if not hit:
            out = run(['clang-tidy', '-p', BUILD, '--checks=' + TIDY_CHECKS, '--quiet', e['file']], ROOT)
            cache_write('clang-tidy', key, out)
        for line in out.splitlines():
            m = TIDY.match(line)
            if m:
                # unchecked-optional's hits are all in tests/, where QVERIFY
                # guards the access but the macro is invisible to tidy's
                # dataflow. Production code stays armed.
                if m.group(3) == 'bugprone-unchecked-optional-access' and '/tests/' in m.group(1):
                    continue
                found[m.group(3)].add((m.group(1), m.group(2)))
        return (hit, key)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(one, srcs))
    if not NOCACHE:
        cache_gc('clang-tidy', {k for _, k in results if k}, '.out')
        print(f'cache: clang-tidy {sum(h for h, _ in results)} hits, '
              f'{sum(not h for h, _ in results)} misses')
    return found

def cppcheck_counts():
    found = collections.defaultdict(set)
    # The compile db is scrubbed before cppcheck sees it: it contains the
    # AUTOMOC mocs_compilation TUs, and preprocessing one of those takes the
    # #error branch — the moc stub expects Q_MOC_OUTPUT_REVISION from the
    # build context cppcheck does not fully inherit (the db entries lack it;
    # the real build gets it from the Qt headers an earlier moc include pulls
    # in). A preprocessor error aborts a whole worker batch, and the findings
    # set then flaps with thread scheduling — observed: passedByValue 0 vs 24
    # between identical runs. Generated code is not review surface anyway
    # (SKIP excludes it from every other pass), so the TUs are dropped from a
    # filtered copy of the db: cppcheck sees exactly what the other passes
    # see, and no worker can be poisoned. The coverage assertion below checks
    # the same filtered set.
    raw_db = os.path.join(BUILD, 'compile_commands.json')
    # inside the cache dir, not the build root: the build root can itself be
    # an -I directory, and a file appearing there after the gcc pass computed
    # its keys would flap every key on the next run (observed: warm run 0 hits)
    os.makedirs(CACHE, exist_ok=True)
    db = os.path.join(CACHE, 'compile_commands.cppcheck.json')
    db_entries = json.load(open(raw_db))
    kept = [e for e in db_entries if not any(s in e['file'] for s in SKIP)]
    with open(db, 'w') as f:
        json.dump(kept, f)
    # no --quiet: the progress lines ("Checking <file> ...") are the liveness
    # signal — cppcheck answers a broken invocation (phantom -i, unresolvable
    # project) with one error line, exit code 0, and zero findings, which is
    # indistinguishable from a clean scan unless coverage is asserted.
    cmd = ['cppcheck', f'--project={os.path.abspath(db)}',
           '--library=qt', '--suppress=unknownMacro', '--suppress=syntaxError',
           '--suppress=missingIncludeSystem',
           '--enable=warning,performance,portability', '--inline-suppr']
    cwd = os.path.abspath(BUILD)
    out = run(cmd, cwd)

    # coverage: every TU the compile db offers (after the same SKIP filter the
    # other tools apply) must appear in cppcheck's Checking lines, normalized
    # to absolute paths
    checked = set()
    for line in out.splitlines():
        m = re.match(r'^Checking (\S+) \.{3}$', line)
        if m:
            checked.add(os.path.abspath(os.path.join(cwd, m.group(1))))
    expected = {os.path.abspath(e['file']) for e in entries()}
    missing = sorted(expected - checked)
    if missing:
        print(f'cppcheck coverage {len(checked)}/{len(expected)} TUs — '
              f'{len(missing)} silently unchecked, first few:')
        for f in missing[:5]:
            print('  ', f)
        sys.exit('cppcheck did not scan the whole tree; its findings are untrustworthy')

    for line in out.splitlines():
        m = CPPCHECK.match(line)
        if m and not any(s in m.group(1) for s in SKIP) and m.group(3) not in FROZEN:
            found[m.group(3)].add((m.group(1), m.group(2)))
    return found

def show(name, found):
    print(f'\n{name}:')
    counts = {}
    for check, sites in sorted(found.items(), key=lambda kv: -len(kv[1])):
        # one header diagnostic repeats in every TU that includes it; what
        # matters is how many places in the tree still need fixing
        print(f'  {len(sites):6d}  {check}')
        counts[check] = len(sites)
    return counts

def main():
    srcs = entries()
    versions = dict(instrument_versions(srcs))
    fp = fingerprint(srcs)
    print(f'{len(srcs)} translation units, build dir {BUILD}, instrument {fp}')
    memo = {}
    gcc = gcc_counts(srcs, versions, memo)
    gated = {k: v for k, v in gcc.items() if k in GATED}
    report_only = {k: v for k, v in gcc.items() if k not in GATED}
    now = {}
    now.update({('gcc', k): v for k, v in show('gcc, report-only families', report_only).items()})
    now.update({('clazy', k): v for k, v in show('clazy', clazy_counts(srcs, versions['clazy-standalone'], memo)).items()})
    now.update({('clang-tidy', k): v for k, v in show('clang-tidy', clang_tidy_counts(srcs, versions['clang-tidy'], memo)).items()})
    now.update({('cppcheck', k): v for k, v in show('cppcheck', cppcheck_counts()).items()})
    print('\ngcc, families the build fails on:')
    for check in sorted(GATED):
        print(f'  {gated.get(check, 0):6d}  {check}')
        now[('gcc-gated', check)] = gated.get(check, 0)

    base = {}
    base_fp = None
    if os.path.exists(BASELINE):
        for line in open(BASELINE):
            if line.strip() and not line.startswith('#'):
                parts = line.split()
                if parts[0] == 'instrument':
                    base_fp = parts[1]
                    continue
                tool, check, count = line.split()
                base[(tool, check)] = int(count)

    problems = []
    for key, n in sorted(now.items()):
        if key[0] == 'gcc-gated':
            if n:
                problems.append(f'GATED   {key[1]}: {n} — -Werror in the build, so this cannot be baselined')
            continue
        old = base.get(key)
        if n == 0 and old is None:
            continue
        if old is None:
            problems.append(f'NEW     {key[0]} {key[1]}: {n}')
        elif n > old:
            problems.append(f'GREW    {key[0]} {key[1]}: {old} -> {n}')
        elif n < old:
            problems.append(f'STALE   {key[0]} {key[1]}: {old} -> {n}, and a shrunk number is a deliberate edit')
        elif old == 0:
            problems.append(f'ZERO    {key[0]} {key[1]}: delete the line, it pins nothing')
    for key, old in sorted(base.items()):
        if key[0] != 'gcc-gated' and key not in now:
            problems.append(f'GONE    {key[0]} {key[1]}: baseline says {old}, tool reports nothing')

    if not GATE:
        print('\n(report only; --gate compares against scripts/lint-baseline.txt)')
        return
    if base_fp is None:
        problems.append('the baseline records no instrument fingerprint — measure once and '
                        'add "instrument <sha>" deliberately, so count drift can never ride in '
                        'silently on an unnamed instrument change')
    elif base_fp != fp and base:
        # the fingerprint protects COUNTS; with an empty table there is nothing
        # to protect — every finding fails as NEW/GONE on its own, so a moved
        # instrument (typically a new TU in the scanned set) is not a gate event.
        # It is still recorded, deliberately, for the day a row returns.
        problems.append(f'INSTRUMENT changed since the baseline was recorded ({base_fp} -> {fp}) — '
                        're-verify the counts against the new instrument and update the '
                        'fingerprint deliberately')
    if problems:
        print('\nbaseline differs:')
        for p in problems:
            print('  ', p)
        sys.exit(1)
    print('\nlint baseline holds')

main()
