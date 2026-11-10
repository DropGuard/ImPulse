#!/usr/bin/env python3
# Pins the lint cache's contract: a warm run must produce the identical table
# cold, a header edit must fan out to every dependent TU, a .cpp edit must not
# touch its siblings, --no-cache must bypass, and a new file shadowing an
# include path must invalidate. The biggest failure mode this guards: a stale
# hit reporting zero while real debt grows — the counts-only-shrink gate would
# be fooled silently.
import json, os, re, shutil, subprocess, sys, tempfile

LINT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts', 'lint.py')

for tool in ('c++', 'clazy-standalone', 'clang-tidy', 'cppcheck'):
    if not shutil.which(tool):
        print(f'{tool} not installed — lint cache test skipped')
        sys.exit(77)

failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)

def write(path, text):
    with open(path, 'w') as f:
        f.write(text)

def lint(build, *args):
    p = subprocess.run([sys.executable, LINT, build, *args], capture_output=True, text=True)
    check(p.returncode == 0, f'lint {args} exited {p.returncode}:\n{p.stdout}{p.stderr}')
    return p.stdout

def hits(out, tool):
    m = re.search(rf'^cache: {tool} (\d+) hits, (\d+) misses$', out, re.M)
    check(m is not None, f'no cache status line for {tool} in:\n{out}')
    return (int(m.group(1)), int(m.group(2))) if m else (-1, -1)

def table(out):
    # everything except the first line (build dir + fingerprint) and the
    # cache status lines is the verdict table
    return [l for l in out.splitlines()[1:] if not l.startswith('cache:')]

with tempfile.TemporaryDirectory() as tmp:
    inc1, inc2 = os.path.join(tmp, 'inc1'), os.path.join(tmp, 'inc2')
    os.makedirs(inc1)
    os.makedirs(inc2)
    write(os.path.join(tmp, 'h.h'), '#pragma once\ninline int add(int a, int b) { return a + b; }\n')
    write(os.path.join(inc1, 'shadow.h'), '#pragma once\n#define SHADOW 1\n')
    write(os.path.join(tmp, 'a.cpp'), '#include "h.h"\n#include "shadow.h"\nint a() { return add(1, 2) + SHADOW; }\n')
    write(os.path.join(tmp, 'b.cpp'), '#include "h.h"\nint b() { return add(3, 4); }\n')
    db = [{'directory': tmp, 'file': os.path.join(tmp, f),
           'command': f'c++ -I{tmp}' + (f' -I{inc2} -I{inc1}' if f == 'a.cpp' else '')
                      + f' -c {os.path.join(tmp, f)} -o {os.path.join(tmp, f)}.o'}
          for f in ('a.cpp', 'b.cpp')]
    write(os.path.join(tmp, 'compile_commands.json'), json.dumps(db))

    cold = lint(tmp)
    check(hits(cold, 'gcc') == (0, 2), 'cold run must miss every TU (gcc)')
    check(hits(cold, 'clazy') == (0, 2), 'cold run must miss every TU (clazy)')
    check(hits(cold, 'clang-tidy') == (0, 2), 'cold run must miss every TU (clang-tidy)')

    warm = lint(tmp)
    check(hits(warm, 'gcc') == (2, 0), 'warm run must hit every TU (gcc)')
    check(hits(warm, 'clazy') == (2, 0), 'warm run must hit every TU (clazy)')
    check(hits(warm, 'clang-tidy') == (2, 0), 'warm run must hit every TU (clang-tidy)')
    check(table(warm) == table(cold), 'warm table differs from cold table')

    # header edit: both TUs include h.h, both must re-scan
    with open(os.path.join(tmp, 'h.h'), 'a') as f:
        f.write('// touched\n')
    fanout = lint(tmp)
    check(hits(fanout, 'gcc') == (0, 2), 'header edit must fan out to all dependents (gcc)')
    check(hits(fanout, 'clazy') == (0, 2), 'header edit must fan out to all dependents (clazy)')
    check(hits(fanout, 'clang-tidy') == (0, 2), 'header edit must fan out to all dependents (clang-tidy)')
    check(table(fanout) == table(cold), 'comment-only header edit changed the table')

    # .cpp edit: only that TU re-scans
    with open(os.path.join(tmp, 'a.cpp'), 'a') as f:
        f.write('// touched\n')
    local = lint(tmp)
    check(hits(local, 'gcc') == (1, 1), '.cpp edit must miss exactly one TU (gcc)')
    check(hits(local, 'clazy') == (1, 1), '.cpp edit must miss exactly one TU (clazy)')
    check(hits(local, 'clang-tidy') == (1, 1), '.cpp edit must miss exactly one TU (clang-tidy)')

    bypass = lint(tmp, '--no-cache')
    check('cache:' not in bypass, '--no-cache must not report cache status')
    check(table(bypass) == table(cold), '--no-cache table differs from cold table')
    check(hits(lint(tmp), 'gcc') == (2, 0), '--no-cache must not poison the cache')

    # new file on an earlier -I dir shadows the existing include: no tracked
    # file changed, but the directory listing did, so a.cpp must re-scan
    write(os.path.join(inc2, 'shadow.h'), '#pragma once\n#define SHADOW 2\n')
    shadow = lint(tmp)
    check(hits(shadow, 'gcc') == (1, 1), 'shadowing new header must invalidate the including TU (gcc)')
    check(hits(shadow, 'clazy') == (1, 1), 'shadowing new header must invalidate the including TU (clazy)')
    check(hits(shadow, 'clang-tidy') == (1, 1), 'shadowing new header must invalidate the including TU (clang-tidy)')

if failures:
    print('\n'.join(f'FAIL: {f}' for f in failures))
    sys.exit(1)
print('lint cache contract holds')