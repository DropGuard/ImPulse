#!/bin/sh
# Runs the full `check` target on the Windows box against the commit checked out
# HERE (HEAD), without touching any branch there. The box's own checkout of
# rustcore is never a push target (git refuses it), so the commit travels to a
# throwaway ref named after its hash -- no force needed, nothing to clean up on
# a second run -- and the box checks it out detached, which is HEAD pointing at
# a commit instead of a branch.
#
# The box sleeps (S3) after 15 minutes idle and ssh input does not count as
# activity, so a build that outlives that gets cut off. ~/bin/keep-awake.ps1 on
# the box holds it awake for exactly the run; the request dies with its process,
# and its own ceiling covers a dropped ssh session. That script knows nothing
# about this project, so it lives on the box and not in this repo.
#
# usage: scripts/verify-windows.sh [remote] [repo dir on the remote]
#   defaults: win  dev/impulse
set -u
cd "$(dirname "$0")/.." || exit 2

remote=${1:-win}
repo=${2:-dev/impulse}
sha=$(git rev-parse --short=12 HEAD) || exit 2
ref=refs/verify/$sha

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "verify-windows: tracked changes are not committed -- only HEAD ($sha) travels" >&2
fi

ssh_opts="-o BatchMode=yes -o ConnectTimeout=15"

# shellcheck disable=SC2086
ssh $ssh_opts "$remote" 'test -f ~/bin/keep-awake.ps1'
probe=$?
if [ "$probe" -eq 255 ]; then
    echo "verify-windows: $remote is not reachable (asleep? wake it and rerun)" >&2
    exit 3
elif [ "$probe" -ne 0 ]; then
    echo "verify-windows: ~/bin/keep-awake.ps1 is missing on $remote; it only needs to exist there, it blocks sleep via SetThreadExecutionState for -Seconds" >&2
    exit 3
fi
git push -q "$remote" "HEAD:$ref" || exit 4

# The remote half. Unquoted heredoc: \$ is for the remote shell, the rest is ours.
# SC2087 is the point: $repo and $ref expand here, \$-escaped names on the box.
# shellcheck disable=SC2086,SC2087
ssh $ssh_opts "$remote" bash -s <<EOF
export MSYS2_ARG_CONV_EXCL='*'
cd $repo || exit 5
if [ -n "\$(git status --porcelain)" ]; then
    echo 'verify-windows: the box has uncommitted changes in $repo; not touching them' >&2
    exit 6
fi
orig=\$(git branch --show-current)
awake=''
cleanup() {
    [ -n "\$awake" ] && kill "\$awake" 2>/dev/null
    if [ -n "\$orig" ]; then git checkout -q "\$orig"; else git checkout -q --detach; fi
    git update-ref -d $ref
}
trap cleanup EXIT
git checkout -q --detach $ref || exit 7
powershell -NoProfile -ExecutionPolicy Bypass -File "\$(cygpath -w ~/bin/keep-awake.ps1)" -Seconds 2700 >/dev/null 2>&1 &
awake=\$!
echo "verify-windows: \$(git log --oneline -1) (was on \${orig:-detached HEAD})"
cmake --build build --target check > /tmp/verify-check.log 2>&1
rc=\$?
grep -a -E 'tests passed|tests failed|\*\*\*Failed|\*\*\*Skipped|error:|FAILED:' /tmp/verify-check.log | sort | uniq -c | head -30
tail -3 /tmp/verify-check.log
echo "verify-windows: check exit \$rc"
exit \$rc
EOF
rc=$?
echo "verify-windows: exit $rc"
exit $rc
