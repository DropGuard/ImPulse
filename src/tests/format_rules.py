#!/usr/bin/env python3
"""The tree's style is .clang-format, and this is the gate that says so.

Why this exists as a ctest and not only as a pre-commit stage: the Windows
port arrived with fifteen hand-formatted files because that machine had no
clang-format, and the hook — correctly — skipped the check rather than block
the commit. A skipped check leaves no trace in the history, so the debt was
invisible to every review on a machine that did have the tool. A ctest that
can skip with a named reason (77) is at least recorded in the run, and a ctest
that runs green proves the whole tree, not just the staged files.

Scope is tree_scope.ROOTS, the same module the pre-commit hook and the
architecture rules ask — the roots used to be spelled out per gate, and each
spelled the main root alone, so the six plugin files this tree owns were never
formatted while this rule reported a clean tree.

Skips (77) when clang-format is absent, and names how to arm it.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tree_scope  # noqa: E402  (needs the path above first)


def sources():
    for path, _rel in tree_scope.sources():
        yield path


def main():
    tool = shutil.which('clang-format')
    if not tool:
        print('clang-format not found — install it to arm this gate '
              '(the pre-commit hook skips the same way, which is how the '
              'Windows port shipped unformatted files)')
        return 77

    files = sorted(sources())
    if not files:
        print('scanned zero files — the rule is blind, fix the walk before '
              'trusting it')
        return 1

    # one invocation for the whole tree: clang-format takes many paths, and a
    # per-file loop would be minutes slower for the same answer
    result = subprocess.run([tool, '--dry-run', '--Werror', *files],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        out = (result.stderr or result.stdout).strip().splitlines()
        print('unformatted files — run: clang-format -i <the files above>')
        # clang-format names every offending file; the summary is the head of
        # the noise, which is what a reviewer needs first
        for line in out[:20]:
            print('  ', line)
        if len(out) > 20:
            print(f'   … {len(out) - 20} more lines')
        return 1
    print(f'format clean — {len(files)} files agree with .clang-format '
          f'({os.path.basename(tool)})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
