"""The tree's own source roots, in one place.

Every gate that walks the tree — the format rule, the architecture rules, the
pre-commit hook — asks this module instead of naming a root itself. They had
each named `qimgv/` and each therefore missed `plugins/`: the hook's staged
filter was `^qimgv/`, so a plugin edit bypassed format checking entirely, and
the format ctest walked the same narrow root and reported a clean tree while
six of the plugin's seven files were unformatted. A gate that scans nothing is
indistinguishable from a passing one, which is the failure this module exists
to make impossible.

Vendored code is excluded by directory name, not by root: a new root is
covered the moment it is listed here, and 3rdparty stays excluded wherever it
appears. `qthelper.hpp` is libmpv's example file and is excluded by name —
it carries its upstream include guard and upstream spelling.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The roots this tree owns. `engine/` is Rust and is gated by cargo/rustfmt,
# not by these walkers.
ROOTS = ('qimgv', 'plugins')

SKIP_DIRS = {'3rdparty', 'res', 'translations', 'build', 'build-clazy', 'target'}

# Files that are not ours to reformat or rename, matched on the repo-relative
# path (no leading slash — that is what os.path.relpath produces).
SKIP_FILES = ('plugins/player_mpv/src/qthelper.hpp',)

CPP_SUFFIXES = ('.cpp', '.h', '.hpp')


def is_vendored(rel):
    """True for a repo-relative path this tree does not own."""
    return any(rel.endswith(s) for s in SKIP_FILES)


def sources(suffixes=CPP_SUFFIXES, repo_relative=True):
    """Yield (absolute_path, rel) for every owned source file.

    `rel` is repo-relative by default. Pass repo_relative=False to get it
    relative to the file's own root (qimgv/ or plugins/): the architecture
    rules were written against paths like 'gui/overlays/x.cpp' and 'core.h',
    and they keep working unchanged against that form.
    """
    for root_name in ROOTS:
        root = os.path.join(ROOT, root_name)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for f in sorted(filenames):
                if not f.endswith(suffixes):
                    continue
                path = os.path.join(dirpath, f)
                repo_rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
                if is_vendored(repo_rel):
                    continue
                if repo_relative:
                    yield path, repo_rel
                else:
                    yield path, os.path.relpath(path, root).replace(os.sep, '/')
