#!/usr/bin/env python3
# Static analysis on demand. Four tools, one table, one rule: a count may only
# shrink. The families that are already at zero do not live here — they are
# -Werror'd in the build itself (see the root CMakeLists); this script reports
# the debt that would be noise to compile in, so that it can be watched instead
# of ignored. Families judged noise by policy are not baselined either — they
# are filtered out of the scan (GATED/REPORT/FROZEN below), with the rationale
# in lint-baseline.txt.
#
# Every text-mode open() names its encoding: Python's default is the ANSI
# codepage on Windows (GBK on a zh-CN box, not UTF-8), and the baseline's
# comments carry UTF-8 punctuation that codepage cannot decode — the gate died
# inside open(BASELINE) on the first Windows run before comparing a count.
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
import collections, concurrent.futures, hashlib, json, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = next((a for a in sys.argv[1:] if not a.startswith('-')), os.path.join(ROOT, 'build'))
GATE = '--gate' in sys.argv
NOCACHE = '--no-cache' in sys.argv
# Sites, not just counts. A gate that says "9 strict-iterators" without naming
# them cannot be acted on from a CI log — the run has to be repeated locally to
# learn anything, and a local run on a different compiler may not reproduce it
# at all. Printing the file:line costs nothing and makes a red run diagnosable
# where it happened.
SHOW_SITES = '--show-sites' in sys.argv
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
# -Wshadow moved to the build itself (-Werror=shadow in the root CMakeLists):
# once every member is m_-prefixed, a shadow is a mistake, not a style choice,
# and a family that is an error in the build must not also be a baseline here.
REPORT = ['-Wold-style-cast', '-Wsuggest-override']
# policy-frozen families: the findings are real, but every fix was judged a
# non-improvement for this tree, so they are filtered out instead of counted —
# the numbers cannot rot, and the gate stops paying to re-derive them.
#   cppcheck passedByValue / duplInheritedMember — Qt's implicit sharing and
#   signal re-declaration idioms make these recommendations wrong here.
FROZEN = {'duplInheritedMember'}  # passedByValue re-armed at zero (2026-11-05)
# gcc-family findings that name Qt's own macro contract, not tree code: the
# qCDebug(category) streaming form omits the macro's variadic slot BY DESIGN,
# and clang names the invocation site. Unfixable at the call site, so the
# family is counted nowhere.
GCC_FROZEN = {'variadic-macro-arguments-omitted'}
# families whose findings live only in test files, where the shape is the
# test's own vocabulary: an EXPECT guards the optional the macro hides from
# tidy's dataflow, a bench reads its own argv, a test main may throw into the
# harness. Exempted under /tests/ only - production code stays armed.
TEST_NOISE = {'bugprone-unchecked-optional-access',
              'bugprone-unchecked-string-to-number-conversion',
              'bugprone-exception-escape'}
# clang-tidy: the bugprone census (2026-11-10). Excluded at selection, not
# post-filtered — the gate stops paying to re-derive judged noise (same
# philosophy as REPORT driving the gcc pass). Rationale per family lives in
# lint-baseline.txt.
TIDY_EXCLUDE = ['bugprone-narrowing-conversions',
                'bugprone-easily-swappable-parameters',
                'bugprone-integer-division',
                'bugprone-parent-virtual-call',
                'bugprone-implicit-widening-of-multiplication-result']
# readability-identifier-naming rides along: .clang-tidy carries the rule (the
# m_ prefix on private/protected members, no prefix on public ones), and it is
# part of the gate so the convention cannot rot the way an unenforced one did.
TIDY_CHECKS = '-*,bugprone-*,readability-identifier-naming' + ''.join(',-' + c for c in TIDY_EXCLUDE)
DIAG = re.compile(r'^((?:[A-Za-z]:)?[\\/][^:]+):(\d+):\d+: (?:warning|error): .*?\[-?W?([a-z0-9-]+)\]$')
# The check set is explicit, not clazy's default. AGENTS.md documents
# level0,level1,detaching for the build-clazy tree, where clazy is the compiler
# and reads CLAZY_CHECKS from the environment; clazy-standalone does NOT read
# that variable, so an unqualified invocation would silently run its own
# default (level1). The two sets happen to be identical today — verified by
# diffing --list-checks — but "identical today" is not a rule: the moment
# upstream moves a check between levels, the gate and the documented set part
# ways with nothing to show for it.
# level0,level1 is the whole set AGENTS.md documents: the detaching checks
# (range-loop-detach, detaching-temporary, readlock-detaching) are level 1, so
# a literal `detaching` token was not a group name at all — clazy answered
# "Invalid check: detaching" on every TU and ran level0,level1 anyway. Passing
# it never changed a verdict, which is exactly why it survived.
CLAZY_CHECKS = 'level0,level1'
CLAZY = re.compile(r'^((?:[A-Za-z]:)?[\\/][^:]+):(\d+):\d+: warning: .* \[-Wclazy-([a-z0-9-]+)\]$')
TIDY = re.compile(r'^((?:[A-Za-z]:)?[\\/][^:]+):(\d+):\d+: (?:warning|error): .*?\[((?:bugprone|readability)-[a-z0-9-]+)(?:,.*)?\]$')
CPPCHECK = re.compile(r'^((?:[A-Za-z]:)?[\\/][^:]+):(\d+):\d+: (?:style|warning|performance|portability): .* \[([a-zA-Z0-9_]+)\]$')
# .rc is a Windows resource script: windres compiles it (so the compile db
# lists it as a TU), none of the four tools can read it, and cppcheck's
# coverage assertion would then name it as silently unchecked. Not review
# surface - excluded at the source like the generated directories above.
SKIP = ('/3rdparty/', '_autogen/', '/corrosion/', '.rc')

# Known tool failures, tracked like the baseline and for the same reason: an
# exempted TU is documented, reviewed data, not a silent hole. The format is
# <tool> <path> <reason>, and the file is REQUIRED to exist once a crash is
# exempted — a stale entry that no longer matches anything fails the gate, so
# the list cannot quietly outlive the bug.
EXEMPT = os.path.join(ROOT, 'scripts', 'lint-exempt.txt')

def read_exemptions():
    """{(tool, relpath): reason} — empty when the file is absent."""
    out = {}
    try:
        lines = open(EXEMPT, encoding='utf-8').read().splitlines()
    except OSError:
        return out
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(None, 2)
        if len(parts) == 3:
            out[(parts[0], parts[1])] = parts[2]
    return out

def entries():
    db = os.path.join(BUILD, 'compile_commands.json')
    if not os.path.exists(db):
        sys.exit(f'no compile_commands.json in {BUILD} — configure that directory first')
    return [e for e in json.load(open(db, encoding='utf-8')) if not any(s in e['file'] for s in SKIP)]

def resolve(tool):
    # The tool goes to CreateProcess as a bare name, and CreateProcess searches
    # the running interpreter's own directory BEFORE PATH - which, on this
    # toolchain, is the real clazy's home, so a bare name can never be
    # shadowed by a PATH entry and the LINT_CACHE stand-in could not exist.
    # shutil.which honors PATH order, which is the contract the stand-in (and
    # anyone pinning one tool over another) needs; falling back to the bare
    # name keeps the missing tool loud on machines that have none.
    return shutil.which(tool) or tool

def instrument_versions(srcs):
    # Verdicts belong to the tool that issued them. A compiler or clazy upgrade
    # moves counts under an unchanged (file, command) set, and the gate would
    # then read that as the source having changed — loud, but misattributed.
    # The path is already in the command; the version is not, so it goes in
    # here. The whole output, not the first line: clazy-standalone prints LLVM's
    # version first and its own ("clazy version 1.17") on the third line, so a
    # first-line slice would miss exactly the tool whose counts proved unstable.
    tools = sorted({e['command'].split()[0] for e in srcs}) + ['clazy-standalone', 'clang-tidy', 'cppcheck']
    return [[t, run_status([resolve(t), '--version'], ROOT)[0].strip()] for t in tools]

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

def split_argv(cmd):
    # The compile db's command field is shell-escaped, but CMake on Windows
    # mixes the two conventions: quotes are POSIX-escaped (\") while paths stay
    # native backslashes. shlex mangles that in both modes - posix=True eats
    # every path backslash, posix=False leaves \" in the -D values and the
    # preprocessor dies on the mangled command line (measured: 153 TUs of
    # <command line> warnings and zero verdicts). This splitter honors both
    # conventions at once: backslashes are literal except before a quote,
    # quotes group and carry the escape - which is exactly what a shell would
    # hand the compiler on either platform.
    out, cur, in_quote, i = [], [], False, 0
    while i < len(cmd):
        c = cmd[i]
        if c == '\\' and i + 1 < len(cmd) and cmd[i + 1] == '"':
            cur.append('"')
            i += 2
            continue
        if c == '"':
            in_quote = not in_quote
            i += 1
            continue
        if c in ' \t' and not in_quote:
            if cur:
                out.append(''.join(cur))
                cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    if cur:
        out.append(''.join(cur))
    return out

def run_status(cmd, cwd, timeout=1800):
    """Output AND exit status.

    The status is not decoration. A tool that crashed produced no verdicts, and
    once only the text is kept, "crashed" and "found nothing" are the same
    string-shaped value. Matching the tools' crash wording cannot fix that: the
    wording is theirs, it changes between versions, and the first attempt at
    this check shipped two markers that never fire (`stack dump:` is really
    `Stack dump:`) and one that was missing entirely. An exit status is
    structural — 134 is SIGABRT whatever the tool decides to print.

    Timeout is the same argument for the third failure mode: a tool that hangs
    is not a tool that found nothing, and without a bound the gate waits
    forever instead of naming the file.
    """
    try:
        # argv, never a shell. The compile db hands us argv joined with
        # spaces, and splitting it back is the faithful round-trip; handing
        # the string to a shell on Windows means cmd.exe, whose parser
        # answers the forward-slash tokens a clang command line is full of
        # with "The syntax of the command is incorrect" - a finished, silent,
        # zero-finding run (measured: every gcc-family TU read as clean while
        # nothing was compiled and no depfile was written). The db's commands
        # carry no shell syntax beyond quoting, so the split loses nothing;
        # the list-form tool invocations below already ran this way. posix        # stays off on Windows so native backslash paths survive the split.
        if isinstance(cmd, str):
            cmd = split_argv(cmd)
        p = subprocess.run(cmd, cwd=cwd,
                           capture_output=True, text=True, timeout=timeout)
        # subprocess reports a signalled child as the negated signal number
        # (-6 for SIGABRT). The shell's 128+n convention is what a reader
        # expects in a log, and it keeps every nonzero status in one range.
        status = 128 - p.returncode if p.returncode < 0 else p.returncode
        return p.stdout + p.stderr, status
    except subprocess.TimeoutExpired as e:
        blob = (e.stdout or b'') + (e.stderr or b'')
        if isinstance(blob, bytes):
            blob = blob.decode('utf-8', 'replace')
        return blob, TIMEOUT_STATUS

# A status no compiler or checker uses for "I ran and finished". 0 is success,
# 1 is "ran, reported problems", and 2 is gcc's "ran, bad invocation" — all
# three mean the tool got to the end of its work.
FINISHED_STATUSES = {0, 1, 2}
# The status rides in the cached text on this line so a warm run keeps the
# verdict "this TU never finished". Annotating the cache rather than widening
# its format keeps every existing .out file readable.
STATUS_PREFIX = '@@status '

def status_of(out):
    """The recorded exit status of a cached run, or 0 when the line is absent."""
    for line in out.splitlines():
        if line.startswith(STATUS_PREFIX):
            try:
                return int(line[len(STATUS_PREFIX):])
            except ValueError:
                return 0
    return 0
TIMEOUT_STATUS = 124  # coreutils timeout(1)'s convention, so logs read familiarly

def unfinished(status):
    """True when the tool did not finish, i.e. its output is not a verdict."""
    return status not in FINISHED_STATUSES

# The one line that explains an unfinished run, for the report. Purely
# presentational: the DECISION is the exit status, this only says which of the
# tool's own lines to quote beside it.
def summarize(out):
    for line in out.splitlines():
        if 'Assertion `' in line or 'fatal error' in line or 'PLEASE submit' in line:
            return line.strip()
    for line in out.splitlines():
        if line.strip() and not line.startswith(STATUS_PREFIX):
            return line.strip()
    return 'no output'

def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()[:16]

def depfile_path(e):
    return os.path.join(CACHE, 'deps', sha(e['file']) + '.d')

def read_deps(e):
    try:
        text = open(depfile_path(e), encoding='utf-8').read()
    except OSError:
        return None
    text = text.replace('\\\n', ' ')
    # The make target separator is ': ' - never the first colon. A Windows
    # drive letter ("C:\\x") owns a colon of its own, and splitting on it
    # turned the target into a token ("\\path\\a.cpp.o:") that no open() can
    # resolve, poisoning the content hashes: the key went None, cache_write
    # stored nothing, and every run after the first was a miss (measured: the
    # whole LINT_CACHE contract flapped to (0, 2) warm).
    head, sep, tail = text.partition(': ')
    if not sep:
        head, sep, tail = text.partition(':')
    return tail.split()

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
        return open(os.path.join(CACHE, tool, key + '.out'), encoding='utf-8').read()
    except OSError:
        return None

def cache_write(tool, key, out):
    if key is None:
        return
    d = os.path.join(CACHE, tool)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, key + '.out'), 'w', encoding='utf-8') as f:
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
    crashes = []
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
                out, status = run_status(cmd, e['directory'])
            else:
                # -MD writes the include graph as a side effect of the pass we
                # already pay for. Via a temp path: if gcc dies before writing
                # it, nothing is promoted and nothing is cached — a crash
                # output must never be replayed as a verdict.
                tmp = depfile_path(e) + '.tmp'
                out, status = run_status(cmd + f' -MD -MF {tmp}', e['directory'])
                try:
                    os.replace(tmp, depfile_path(e))
                except OSError:
                    parse_gcc(out, found)
                    return (False, None)
                # key from the fresh graph: a content change may have added or
                # removed includes, so the pre-run key is not the one this
                # output belongs to
                key = tu_key(e, 'gcc', cmd, version, read_deps(e), memo)
                out = f'{STATUS_PREFIX}{status}\n' + out
                cache_write('gcc', key, out)
        parse_gcc(out, found)
        if unfinished(status_of(out)):
            crashes.append((e['file'], status_of(out), summarize(out)))
        return (hit, key)
    def parse_gcc(out, found):
        for line in out.splitlines():
            m = DIAG.match(line)
            if m and m.group(3) not in GCC_FROZEN:
                found[m.group(3)].add((m.group(1), m.group(2)))
    # LINT_JOBS caps the parallel gcc fan-out (the pass that runs one process
    # per core by default). Unset means full cores; 0 or an empty value means
    # the same, so "export LINT_JOBS=8" is the only way in.
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=int(os.environ.get("LINT_JOBS") or os.cpu_count())
    ) as ex:
        results = list(ex.map(one, srcs))
    if not NOCACHE:
        cache_gc('gcc', {k for _, k in results if k}, '.out')
        cache_gc('deps', {sha(e['file']) for e in srcs}, '.d')
        print(f'cache: gcc {sum(h for h, _ in results)} hits, '
              f'{sum(not h for h, _ in results)} misses')
    return found, crashes

def clazy_counts(srcs, version, memo):
    found = collections.defaultdict(set)
    crashes = []  # (path, first crash line): TUs that produced no verdict at all
    def one(e):
        # the include graph is the gcc pass's depfile — same TU, same db
        # command, so the graph is identical and needs no re-derivation
        # the checks selection is part of the verdict, so it rides in the key
        # (the clang-tidy pass does the same with TIDY_CHECKS)
        key = None if NOCACHE else tu_key(e, 'clazy', e['command'] + ' ' + CLAZY_CHECKS,
                                          version, read_deps(e), memo)
        out = cache_read('clazy', key)
        hit = out is not None
        if not hit:
            out, status = run_status([resolve('clazy-standalone'), '-p', BUILD, '--checks=' + CLAZY_CHECKS, e['file']],
                                     ROOT)
            # The status is cached with the output: a crashed TU stays visible
            # on a warm run instead of becoming a silent zero once cached.
            out = f'{STATUS_PREFIX}{status}\n' + out
            cache_write('clazy', key, out)
        for line in out.splitlines():
            m = CLAZY.match(line)
            if m:
                # non-pod-global-static is what a parameterized test IS:
                # INSTANTIATE_TEST_SUITE_P expands to a global object holding
                # the parameter table, and gtest constructs it before main by
                # design. Flagging the framework's own shape teaches nothing.
                if m.group(3) == 'non-pod-global-static' and '/tests/' in m.group(1).replace('\\', '/'):
                    continue
                found[m.group(3)].add((m.group(1), m.group(2)))
        if unfinished(status_of(out)):
            crashes.append((e['file'], status_of(out), summarize(out)))
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
    return found, crashes

def clang_tidy_counts(srcs, version, memo):
    found = collections.defaultdict(set)
    crashes = []
    def one(e):
        # the checks selection is part of the verdict, so it rides in the key
        key = None if NOCACHE else tu_key(e, 'clang-tidy', e['command'] + ' ' + TIDY_CHECKS,
                                          version, read_deps(e), memo)
        out = cache_read('clang-tidy', key)
        hit = out is not None
        if not hit:
            # the header filter is in .clang-tidy, not here: a rule that only
            # holds when this script runs it is not the tree's rule. clang-tidy's
            # default suppresses header diagnostics, which is where a member
            # declaration lives — measured: a probe field in cache.h passed
            # unseen until the filter moved into the project config.
            out, status = run_status([resolve('clang-tidy'), '-p', BUILD, '--checks=' + TIDY_CHECKS, '--quiet', e['file']], ROOT)
            out = f'{STATUS_PREFIX}{status}\n' + out
            cache_write('clang-tidy', key, out)
        for line in out.splitlines():
            m = TIDY.match(line)
            if m:
                # TEST_NOISE's hits are all in tests/, where the guard is the
                # test's own: QVERIFY is invisible to tidy's dataflow, a bench
                # reads its argv, a test main may throw into the harness.
                # Production code stays armed. The path check is
                # separator-agnostic: Windows diagnostics spell it with
                # backslashes, and a '/tests/' probe against them never
                # matched (measured: 36 findings rode straight through).
                if m.group(3) in TEST_NOISE and '/tests/' in m.group(1).replace('\\', '/'):
                    continue
                # Generated code is not review surface: with --header-filter=.*
                # the ui_*.h AUTOGEN output comes back too, and its reserved
                # identifiers are uic's spelling, not ours. The same SKIP the
                # other passes use.
                if any(s in m.group(1) for s in SKIP):
                    continue
                found[m.group(3)].add((m.group(1), m.group(2)))
        if unfinished(status_of(out)):
            crashes.append((e['file'], status_of(out), summarize(out)))
        return (hit, key)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(one, srcs))
    if not NOCACHE:
        cache_gc('clang-tidy', {k for _, k in results if k}, '.out')
        print(f'cache: clang-tidy {sum(h for h, _ in results)} hits, '
              f'{sum(not h for h, _ in results)} misses')
    return found, crashes

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
    db_entries = json.load(open(raw_db, encoding='utf-8'))
    kept = [e for e in db_entries if not any(s in e['file'] for s in SKIP)]
    with open(db, 'w', encoding='utf-8') as f:
        json.dump(kept, f)
    # no --quiet: the progress lines ("Checking <file> ...") are the liveness
    # signal — cppcheck answers a broken invocation (phantom -i, unresolvable
    # project) with one error line, exit code 0, and zero findings, which is
    # indistinguishable from a clean scan unless coverage is asserted.
    cmd = [resolve('cppcheck'), f'--project={os.path.abspath(db)}',
           '--library=qt', '--suppress=unknownMacro', '--suppress=syntaxError',
           '--suppress=missingIncludeSystem',
           '--enable=warning,performance,portability', '--inline-suppr']
    cwd = os.path.abspath(BUILD)
    out, status = run_status(cmd, cwd)
    # cppcheck scans the whole tree in one process, so an unfinished run has no
    # per-file attribution; the coverage assertion below is what names the
    # damage. This is the coarser first question: did it get to the end at all?
    if unfinished(status):
        sys.exit(f'cppcheck exited {status} — its findings are untrustworthy ({summarize(out)})')

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
        if SHOW_SITES:
            for path, line in sorted(sites):
                print(f'          {path}:{line}')
        counts[check] = len(sites)
    return counts

def rel_to_root(path):
    # os.path.relpath cannot cross drives on Windows: the repo lives on one
    # and the scanned tree (or the cache test's temp dir) may live on another,
    # and a cross-drive relpath is a ValueError that would take the whole
    # crash report down instead of naming the TU. A same-drive path shortens;
    # a different drive is already as short as it gets.
    if os.path.splitdrive(path)[0].lower() != os.path.splitdrive(ROOT)[0].lower():
        return path
    return os.path.relpath(path, ROOT)

def main():
    srcs = entries()
    versions = dict(instrument_versions(srcs))
    fp = fingerprint(srcs)
    print(f'{len(srcs)} translation units, build dir {BUILD}, instrument {fp}')
    memo = {}
    gcc, gcc_crashes = gcc_counts(srcs, versions, memo)
    # counts, not the raw (file, line) sets: show() converts for the
    # report-only families, and the gated print and the baseline compare
    # need the same shape — a gated family with findings formatted its set
    # and took the whole gate down
    gated = {k: len(v) for k, v in gcc.items() if k in GATED}
    report_only = {k: v for k, v in gcc.items() if k not in GATED}
    now = {}
    now.update({('gcc', k): v for k, v in show('gcc, report-only families', report_only).items()})
    clazy_found, clazy_crashes = clazy_counts(srcs, versions['clazy-standalone'], memo)
    tidy_found, tidy_crashes = clang_tidy_counts(srcs, versions['clang-tidy'], memo)
    now.update({('clazy', k): v for k, v in show('clazy', clazy_found).items()})
    now.update({('clang-tidy', k): v for k, v in show('clang-tidy', tidy_found).items()})
    now.update({('cppcheck', k): v for k, v in show('cppcheck', cppcheck_counts()).items()})
    print('\ngcc, families the build fails on:')
    for check in sorted(GATED):
        print(f'  {gated.get(check, 0):6d}  {check}')
        now[('gcc-gated', check)] = gated.get(check, 0)

    base = {}
    base_fp = None
    if os.path.exists(BASELINE):
        for line in open(BASELINE, encoding='utf-8'):
            if line.strip() and not line.startswith('#'):
                parts = line.split()
                if parts[0] == 'instrument':
                    base_fp = parts[1]
                    continue
                tool, check, count = line.split()
                base[(tool, check)] = int(count)

    problems = []
    # A crashed tool produced no verdicts for that TU, so its counts are a
    # silence, not a pass. Reported before the count comparison: a crash
    # explains why a family may have shrunk, and a shrunk number is otherwise
    # reported as STALE with no hint that the measurement never happened.
    exemptions = read_exemptions()
    seen_exemptions = set()
    for tool, crash_list in (('gcc', gcc_crashes), ('clazy', clazy_crashes),
                             ('clang-tidy', tidy_crashes)):
        for path, status, first in sorted(crash_list):
            rel = rel_to_root(path)
            if (tool, rel) in exemptions:
                # Documented and reviewed: the reason lives in
                # scripts/lint-exempt.txt beside the bug it belongs to. Still
                # printed, because "checked nothing here" must stay visible.
                seen_exemptions.add((tool, rel))
                print(f'exempt  {tool}: {rel} produced no verdict — {exemptions[(tool, rel)]}')
                continue
            problems.append(f'UNFINISHED {tool}: {rel} exited {status}, '
                            f'so it produced no verdict — {first}')
    # an exemption that no longer describes reality: the tool recovered, the TU
    # left the tree, or the check was removed. Either way the line is stale and
    # hides nothing any more, so it fails rather than lingering.
    for key in sorted(set(exemptions) - seen_exemptions):
        problems.append(f'EXEMPT-STALE {key[0]}: {key[1]} is listed in lint-exempt.txt '
                        f'but did not fail — delete the line')
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
