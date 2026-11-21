# Developing ImPulse

The contributor-facing companion to the README: the platform contract, the
architecture, the build, and the verification machinery. The operating
discipline (each rule mapped to the incident that earned it) lives in
[AGENTS.md](AGENTS.md); the structural rules themselves are executable —
`src/tests/architecture_rules.py` runs as part of ctest.

## Platform: Linux and Windows (msys2)

The listing's watcher is inotify on Linux and ReadDirectoryChangesW on
Windows — the same seam, the same `WatchCmd` vocabulary, twin syscall shells
(`watcher/fd.rs`, `watcher/fd_windows.rs`) over one shared glue (`watcher.rs`)
and one pure layer (`watcher/pair.rs`, `watcher/apply.rs`). Everything else in
the event story is platform-neutral. The old `QFileSystemWatcher` fallback is
gone, and the `inotify` crate rides the linux target gate.

Distro packages named `qimgv` install upstream, not this fork.

### Windows specifics (msys2 / clang64)

The supported Windows toolchain is msys2's **clang64** environment: clang with
the Qt 6.12 stack, ninja, Corrosion driving cargo inside the same build, and
the full feature set. Configure exactly as on Linux
(`cmake -S . -B build -G Ninja` from an msys2 shell, with clang64's bin first
on PATH so the configure finds its own Qt).

**One environment, never a mix.** msys2's `clang64`, `ucrt64` and `mingw64`
each carry their own C/C++ runtime ABI; linking libraries from two of them
produces link errors at best and silent breakage at worst. Every package comes
from clang64.

clang64 rather than ucrt64, which this section used to name. The reason given
for ucrt64 was that exiv2 and libmpv were packaged only there; checked
2026-11-21, both are in clang64 as well. clang64 wins because it is the only
environment that also carries **clazy**, so the machine that builds the code
can also run the lint gate on it — and clang's warnings match what the Linux
tree measures, where gcc's do not.

The Rust side stays `x86_64-pc-windows-gnu`; its self-contained CRT objects
link cleanly through the pure C ABI (measured, zero undefined symbols). The git
gate is bash-shaped by design — run git from the msys2 shell, where bash, the
coreutils and the hook's environment exist; from a bare cmd the hook's
interpreter is not resolvable. Tools the machine does not have are skipped
loudly by the hook, never silently: cppcheck/clang-tidy (the lint baseline),
clang-format, rustfmt and Miri each name their absence, and
cmake/ctest/pytest-equivalents are read from the configure-time cache rather
than hoped for on PATH.

Feature availability on this toolchain, measured rather than assumed:

| Option | msys2 clang64 | note |
| --- | --- | --- |
| `EXIV2` | ON | mingw-w64-clang-x86_64-exiv2 0.28; its 0.28 API takes UTF-8 narrow paths on every platform (`QString::toStdString`, not the old wide-char helper) |
| `VIDEO_SUPPORT` | ON | mingw-w64-clang-x86_64-mpv 0.42; the plugin builds as `build/plugins/player_mpv/player_mpv.dll` |
| `OPENCV_SUPPORT` | ON | mingw-w64-clang-x86_64-opencv 5.0 |

Windows file-system facts the code is written against (each pinned by a test):

* **Sharing is the denial channel.** There are no mode bits; a file another
  handle holds open without sharing cannot be moved, renamed, deleted or
  opened, and Qt's ACL-derived `isReadable()` never answers false for an
  existing file. Where POSIX checks mode bits, the Windows branches probe with
  an actual open (ThumbnailCache's unreadable-original gate) and the tests
  hold exclusive handles as the refusal fixture.
* **`rename()` has no atomic replace** (mingw's MSRT semantics answer EEXIST);
  `MoveFileEx(REPLACE_EXISTING)` is the spelling both `FileOperations::rename`
  and the tests use.
* **Open handles block deletion and rename** — DeleteFile and MoveFileEx fail
  on a file any handle in the same process holds; fixtures close their
  read-back handles before mutating.
* **`IMG2.jpg` and `img2.jpg` are one name.** The case-insensitive file system
  makes case-pair fixtures impossible; where the Linux test stresses
  case-collation the Windows branch uses a non-colliding variant.
* **`SetFileTime` needs `FILE_WRITE_ATTRIBUTES`** — a read-only open does not
  carry it, unlike futimens through an O_RDONLY fd.
* **A delete-pending directory can hold the notify IRP past the cancel** —
  the watcher's retire drains the abort completion with a bound, never a
  plain wait, or `Engine::drop`'s join waits on the thread that is waiting.

Test fixture environment: portable mode reads conf and cache from the exe's
directory; `QIMGV_CONF_DIR` and `QIMGV_CACHE_DIR` redirect both (FreshSettings
sets them per test instance), and the `cacheDir` hand-edit key outranks the
redirect — logs and temp frames must follow the same directory the app picks.

## Architecture: what changed underneath

**One writer for the listing.** The Rust engine (`engine/qimgv-engine`) owns the
directory listing — the one order decision, handed to the mirror as engine-side
indices so C++ never re-derives it. Decode execution and its scheduling live on
the C++ side: the shared WorkerPool runs the work, and each owner carries its
own staleness policy (the Scaler coalesces to the latest request, the
Thumbnailer invalidates per batch, the Loader drains its queue on a priority
submit).
The C++ side is otherwise a read-only mirror plus Qt rendering: mutation events
carry engine-side indices and watcher-statted data, and C++ only splices. The
monopoly is enforced by a test, not by convention.

**A rename is one event, end to end.** The engine pairs the kernel's inotify
cookie, the wire carries both subjects, and the mirror moves a row instead of
killing one and growing another — so the thumbnail survives, the selection stays
put, and the viewer keeps showing the file you were looking at when another
program renames it. Pairing is best-effort by design: a pair can straddle a read
boundary, so remove+add remains a shape consumers must tolerate.

**Settings never touch worker threads.** The decode environment is captured once
per request on the UI thread and travels with the work (`LoadPolicy`); pool
threads receive values instead of reaching for global configuration.

**Static analysis, as a gate.** A per-family finding count is pinned together
with the instrument that produced it: the scanned `(file, command)` set plus the
tool versions. A re-configured build tree or a compiler upgrade therefore fails
the gate by name instead of moving the counts silently. Counts may only shrink,
and a reduction ships with the fix that earned it.

## Building

Requirements: CMake >= 3.16, a C++17 compiler, ninja, and a Rust toolchain —
[Corrosion](https://github.com/corrosion-rs/corrosion) is fetched by CMake and runs
cargo inside the same build.

### Qt ≥ 6.9 — two roads

How you configure depends on where your Qt comes from. Note that re-running
`cmake -S . -B build` **without** `CMAKE_PREFIX_PATH` re-configures against the
system Qt and will fail the version check if that is older than 6.9 — keep the
flag from whichever road you took.

**Your distro ships Qt ≥ 6.9** (Debian 14, Ubuntu 25.10, Arch, …):

```bash
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev libxkbcommon-dev \
                 qt6-base-dev libopencv-dev libexiv2-dev libmpv-dev libgtest-dev
cmake -S . -B build -G Ninja
```

**Otherwise** (Ubuntu 24.04 LTS ships 6.4, Debian 13 ships 6.8) — the road the CI
itself takes is [aqtinstall](https://github.com/miurahr/aqtinstall), which downloads
official Qt binaries into a prefix of your choosing:

```bash
pip install aqtinstall
aqt install-qt linux desktop 6.10.2 linux_gcc_64 -O /opt/qt
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev libxkbcommon-dev \
                 libopencv-dev libexiv2-dev libmpv-dev libgtest-dev
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/qt/6.10.2/gcc_64
```

Either way, then:

```bash
cmake --build build -j$(nproc)
```

The binary is `build/src/impulse`. For desktop integration (menu entry,
icons, portal registration) run `scripts/install-user.sh` after the build — it
installs into `$HOME/.local` and needs no root.

| Option | Default | Effect |
| --- | --- | --- |
| `EXIV2` | ON | Read/write EXIF tags (needs Exiv2) |
| `OPENCV_SUPPORT` | ON | High-quality scaling filters (needs OpenCV core, imgproc) |
| `VIDEO_SUPPORT` | ON | Video playback plugin (needs libmpv) |
| `KDE_SUPPORT` | OFF | Blur behind windows on KDE (needs KF6WindowSystem) |
| `QIMGV_SANITIZERS` | empty | `address,undefined` or `thread`; use a separate build dir |
| `LINT_TESTS` | OFF | Add the static-analysis sweep to ctest (needs Python 3) |
| `MIRI_TESTS` | OFF | Add the Rust pure-layer Miri run to ctest (needs nightly + miri) |

Warnings are contractual: the build runs `-Wall -Wextra -Wpedantic`, and zero is
the normal result.

## Running

```bash
impulse /path/to/image-or-directory
```

## Testing

Verify a build:

```bash
cd build && ctest -j4                 # the suites, including the architecture rules
cd engine/qimgv-engine && cargo test    # the engine's own suites
```

Every suite is a GoogleTest binary (`find_package(GTest CONFIG REQUIRED)` —
`libgtest-dev` and `libgmock-dev` on Debian/Ubuntu; they are separate packages
exporting separate targets, and the suites link both through
`QIMGV_GTEST_TARGETS`. The module-mode `FindGTest` cannot find either, only
their config files exist). `gtest_discover_tests` registers every
`TEST_F` as its own ctest case, so ctest's own process-per-case layout gives
each test function a private process — and with it a private TSan registry.
Run one test function by name with GoogleTest's filter, not a positional arg:

```bash
build/src/tests/crop_tests                            # the whole suite
build/src/tests/crop_tests --gtest_filter='*cropRatioLocks'
build/src/tests/engine_stack_tests --gtest_filter='TestListing.*'
```

The assertion vocabulary is `EXPECT_*`/`ASSERT_*` only. `gtest_qt.h` poisons
`QVERIFY`/`QCOMPARE`/`QFAIL`/`QSKIP`/`QTRY_*` because inside a gtest TU those
report to a QTest result object that never exists and are silently green; the
event-loop waits are `ASSERT_TRY_TRUE_TIMEOUT` / `ASSERT_TRY_EQ_TIMEOUT`
(built on `QTest::qWaitFor`, which returns a bool rather than reporting), and
a skip is `GTEST_SKIP()`

`coretest::MockCoreView` is gmock over `ICoreView`, and
`coretest::RecordingView` beside it is a plain tally. They answer different
questions: the mock is right when the CALLS are the contract ("delivered
exactly once for the path on screen, never for one nobody is looking at"), and
the tally is right when only the side effects matter ("what size arrived, in
what order"). Neither replaces the other, and a test that would have to
declare ten irrelevant expectations wants the tally.

A table of values that each deserve their own verdict is `TEST_P`, not a loop:
a failure then names the case. `folder_boundary_tests` shows the shape —
`INSTANTIATE_TEST_SUITE_P` plus a `PrintTo` so gtest prints the mode's name
rather than its bytes.

Maintainer gate (runs in the pre-commit hook) — the clazy/clang-tidy/cppcheck/gcc counts are baselined against
a pinned toolchain, and on a foreign toolchain the gate fails by design (the
instrument fingerprint names the change):

```bash
python3 scripts/lint.py build --gate    # static analysis against its pinned baseline
python3 scripts/coverage.py             # line coverage, logic and GUI reported separately
```

Sanitizers live in their own trees so the default build stays untouched:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQIMGV_SANITIZERS=address,undefined
cmake --build build-asan -j$(nproc) && cd build-asan && ASAN_OPTIONS=detect_leaks=1 ctest
```

The thread-sanitizer tree uses the same option with `thread`. Its enumerated
third-party noise baseline is `src/tests/tsan-noise.supp`, and a report naming an
in-tree frame is still a failure. The engine's pure layer is checked with Miri
(`scripts/miri.sh`). New tests are run by name before their suite is trusted: a
green suite is only evidence if the test you think ran actually ran.

## AppImage

The self-contained bundle is built by the `appimage` job in
`.github/workflows/ci.yml`, inside a ubuntu:22.04 container (the glibc floor
the bundle inherits: 2.35 — it covers Debian 12+ and Ubuntu 22.04+).
Everything rides inside: Qt 6.9, OpenCV, exiv2, libmpv and the engine. The job
runs on `v*` tag pushes, `workflow_dispatch` and the nightly schedule; the
pipeline itself was validated locally through `act` (build, pack and the
headless smoke green there), but the release step needs the real GitHub API
and is only proven on a runner.

A tag push lands a **draft** release with `ImPulse-<version>-x86_64.AppImage`
and its sha256; the tag must match the version in `src/appversion.cpp`.
Publish the draft only after the bundle has survived a start on a real
desktop — that check is the one thing CI cannot do
(`gh release edit <tag> --draft=false`, or the web UI).
