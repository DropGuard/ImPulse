#!/usr/bin/env bash
# Build wrapper: refresh the mtime of any source file whose inode changed
# without its mtime following.
#
# Why this exists: ninja decides what to rebuild from mtime. `cp -p`, `mv` and
# `tar -x` restore a file's mtime to what it was, so a source that was
# genuinely replaced looks older than the object built from it and ninja
# prints nothing — the tree then runs a binary that is not the code. This has
# bitten a break-probe and its own revert in the same run (AGENTS.md rule 7).
#
# The signal is ctime: the inode's own change time, which no userland copy
# restores. mtime < ctime means "the file was replaced or had its metadata
# touched after its content was last written", which is exactly the case.
#
# Usage:  scripts/build.sh [ninja args...]      (default: build)
#         BUILD_DIR=build-asan scripts/build.sh
#
# On Windows the same idea runs under msys2's bash, where `stat -c %Z` gives
# ctime the same way (msys2 implements it over the NTFS MFT change time, the
# field GetFileInformationByHandleEx calls ChangeTime).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-build}
cd "$ROOT"

# The roots the tree owns; 3rdparty is vendored and not ours to rebuild.
SOURCE_ROOTS=(src plugins engine)

refreshed=0
for root in "${SOURCE_ROOTS[@]}"; do
    [ -d "$root" ] || continue
    while IFS= read -r -d '' file; do
        # GNU stat: %Y mtime, %Z ctime. msys2's coreutils answers both.
        mtime=$(stat -c %Y "$file" 2>/dev/null) || continue
        ctime=$(stat -c %Z "$file" 2>/dev/null) || continue
        # a strict comparison: touch sets both, so equal means nothing to do
        # and re-touching on equality would loop forever
        if [ "$mtime" -lt "$ctime" ]; then
            touch -c "$file"
            refreshed=$((refreshed + 1))
        fi
    done < <(find "$root" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.rs' \
                                    -o -name '*.toml' -o -name 'CMakeLists.txt' \) \
                       -not -path '*/3rdparty/*' -not -path '*/target/*' -print0)
done

if [ "$refreshed" -gt 0 ]; then
    echo "build.sh: refreshed the mtime of $refreshed replaced source file(s)" >&2
fi

exec cmake --build "$BUILD_DIR" -j"$(nproc)" "$@"
