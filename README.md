# ImPulse

A high-performance Qt6 image viewer for Linux and Windows — a ground-up
modernization fork of [qimgv](https://github.com/easymodo/qimgv).
GPL-3.0 · v0.1.0

## What changed from upstream

- **A Rust engine owns the backend.** Directory watching, the decode
  schedule and the listing live in Rust; the C++ side renders and executes.
  The multithreading crash class and the directory-update races of the
  original watcher are structurally gone, not patched over — the
  architecture story is in [DEVELOPING.md](DEVELOPING.md).
- **Renames are atomic.** The engine pairs the kernel's inotify cookies
  natively: when another program renames a file, the thumbnail survives and
  the selection stays put — no lost focus, no reloaded rows.
- **The keyboard belongs to the view you are in.** Every key that only means
  something with one image on screen — the walk, zoom, fit, the edit verbs,
  save — lives in the document view's own binding set, and simply does not
  exist while the folder view is up. Instead of commands that wake up, check
  the view mode and quietly do nothing, a binding that is not there is not
  there. The same holds for the thumbnail grid: its keys are its own.
- **The feature set is upstream's, re-architected.** Browsing, the thumbnail
  panel, editing, scripts and shortcuts — the feature tour lives in
  [upstream's README](https://github.com/easymodo/qimgv#readme). The bindings
  themselves are not upstream's: the keymap was rebuilt around view scopes,
  so a qimgv shortcut config does not carry over, and ImPulse does not read
  one. Two features are gone with it. The folder view's bookmarks strip is
  one: the directory tree already follows the image you are viewing, the
  strip's own click targets sat next to the tree's, and a misclick there
  navigated you away from a deep path with no way back — cost without
  payoff. The other is the qimgv config import.

## Install

Download the AppImage from the
[releases page](https://github.com/DropGuard/ImPulse/releases), make it
executable and run — Qt, the codecs and the engine are all inside; nothing to
install on your system.

To build from source instead, see [DEVELOPING.md](DEVELOPING.md).

## For developers and contributors

The platform contract (Linux and Windows/msys2), the architecture behind the
three points above, the build instructions, the test suites (ctest,
ASan/UBSan, TSan, Miri) and the static-analysis gate are documented in
[DEVELOPING.md](DEVELOPING.md).

## License

GPL-3.0, as upstream. ImPulse is a fork of
[qimgv](https://github.com/easymodo/qimgv) by easymodo and contributors; the
interface and the artwork come from there. See `LICENSE`.
