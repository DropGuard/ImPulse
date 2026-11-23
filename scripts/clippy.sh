#!/bin/sh
# The Rust tree's clippy gate, machine-absorbed: warnings are errors, on every
# target the tree builds on that this machine can check.
#   1. the host target, all targets (lib, tests, integration tests);
#   2. x86_64-pc-windows-gnu, lib only, when the host is not itself a Windows
#      target (there the host run above already compiled the cfg(windows)
#      branch) and the rustc in use has that target's std in its sysroot
#      (AGENTS discipline 4: a cfg(windows) branch is only a fact where it
#      compiles). The sysroot is asked, not rustup: on a machine with two
#      toolchains, rustup can list a target that the cargo first on PATH
#      cannot use. Tests are not built for it here: they cannot run on this
#      host anyway.
# Exit 77 (ctest SKIP) names the missing tool rather than pretending to check.
set -u
cd "$(dirname "$0")/.." || exit 2

if ! cargo clippy --version >/dev/null 2>&1; then
    echo "CLIPPY: cargo clippy not available — rustup component add clippy"
    exit 77
fi

echo "== host: all targets =="
cargo clippy --workspace --all-targets -- -D warnings || {
    echo "CLIPPY: host target has findings"
    exit 1
}

other=x86_64-pc-windows-gnu
host=$(rustc -vV | sed -n 's/^host: //p')
case "$host" in
    x86_64-pc-windows-*)
        exit 0
        ;;
esac
sysroot=$(rustc --print sysroot)
if [ -d "$sysroot/lib/rustlib/$other" ]; then
    echo "== $other: lib =="
    cargo clippy --workspace --lib --target "$other" -- -D warnings || {
        echo "CLIPPY: $other has findings"
        exit 1
    }
else
    echo "CLIPPY: $other std not in $sysroot — Windows half not checked (rustup target add $other)"
fi
exit 0
