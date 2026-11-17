use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering as AtomicOrdering};
use std::time::UNIX_EPOCH;

use crate::error::EngineError;
use crate::filter::NameFilter;
use crate::model::EntryData;

#[derive(Clone)]
pub struct ScanOptions {
    pub show_hidden: Arc<AtomicBool>,
    pub filter: NameFilter,
    /// Paths force-included by the app (mime fallback) — scanned even when
    /// the extension filter doesn't match.
    pub overrides: Arc<std::sync::Mutex<std::collections::HashSet<PathBuf>>>,
}

#[derive(Debug, Default)]
pub struct ScanOutput {
    pub files: Vec<EntryData>,
    pub dirs: Vec<EntryData>,
}

/// One pass over a single directory. Per-entry failures are skipped with a
/// warning (matching qimgv's behavior); only a failure to open the directory
/// itself is fatal.
///
/// Note on parity with directorymanager.cpp: directory entries get no
/// size/mtime (same as FSEntry defaults); symlink targets are followed for
/// type detection (fs::directory_entry::is_directory() behavior).
/// The listing's hidden rule, in one place. The scanner consulted it and the
/// watcher did not, so a dot-named file or folder created while show_hidden was
/// false became a row the next rescan would remove: one path was a member and a
/// non-member depending on which code looked at it. Note the order — hidden is
/// decided before the extension whitelist and before `overrides`, so a
/// force-included path is still not admitted while it is hidden.
pub fn is_hidden(path: &Path, name: &str) -> bool {
    // The dotfile half is the fork's own semantic and holds on every platform
    // (the hidden rule is one rule: a dot-named entry is not a member while
    // show_hidden is false, whatever the file system says). Windows adds the
    // file-system hidden attribute on top — the only platform-specific fact
    // here.
    if name.starts_with('.') {
        return true;
    }
    #[cfg(windows)]
    {
        std::fs::metadata(path)
            .map(|m| std::os::windows::fs::MetadataExt::file_attributes(&m) & 0x2 != 0)
            .unwrap_or(false)
    }
    #[cfg(not(windows))]
    {
        let _ = path;
        false
    }
}

/// THE wire form of a listing path: what the FFI hands to C++, and what the
/// listing keys its rows on. Two producers mint subjects — the scan baseline
/// (`full_path` per entry) and the watcher (the consumed watch dir joined with
/// a record's name) — and if the two spell one path differently, the same file
/// arrives twice: a row the mirror cannot match, an add that removes nothing.
/// So the rule lives here, once, and both producers call it. (It was two
/// copies of `replace('\\', "/')` for one commit; the drift pin that would
/// have caught a divergence watched only the watcher's copy.)
///
/// Qt's forward slashes on Windows, because the C++ side compares paths as
/// strings against the form it opened the directory with. POSIX joins are
/// byte-exact — a backslash is a legal filename character there — and the
/// no-op replace keeps that side identical.
pub fn wire_path(path: &Path) -> String {
    let joined = path.to_string_lossy().into_owned();
    #[cfg(windows)]
    let joined = joined.replace('\\', "/");
    joined
}

pub fn scan_directory(path: &Path, opts: &ScanOptions) -> Result<ScanOutput, EngineError> {
    let show_hidden = opts.show_hidden.load(AtomicOrdering::Relaxed);
    let mut out = ScanOutput::default();

    let rd = fs::read_dir(path).map_err(|source| EngineError::Io {
        path: path.to_owned(),
        source,
    })?;

    for entry in rd {
        let entry = match entry {
            Ok(e) => e,
            Err(source) => {
                log::warn!("scan: unreadable entry in {}: {source}", path.display());
                continue;
            }
        };
        let name = match entry.file_name().into_string() {
            Ok(n) => n,
            Err(_) => {
                log::warn!("scan: skipping non-UTF-8 name in {}", path.display());
                continue;
            }
        };
        if is_hidden(&entry.path(), &name) && !show_hidden {
            continue;
        }

        let mut file_type = match entry.file_type() {
            Ok(ft) => ft,
            Err(source) => {
                log::warn!("scan: {}: {source}", path.display());
                continue;
            }
        };
        if file_type.is_symlink() {
            // DirEntry::file_type()/metadata() do not follow links; qimgv
            // (via std::filesystem) does.
            match fs::metadata(entry.path()) {
                Ok(m) => file_type = m.file_type(),
                Err(source) => {
                    log::warn!("scan: stat {}: {source}", entry.path().display());
                    continue;
                }
            }
        }

        let full_path = entry.path();
        // one wire form, shared with the watcher (see wire_path)
        let full_str = wire_path(&full_path);

        let allowed = opts.filter.matches(&name)
            || opts
                .overrides
                .lock()
                .map(|o| o.contains(&full_path))
                .unwrap_or(false);

        if file_type.is_dir() {
            out.dirs.push(EntryData {
                name,
                path: full_str,
                is_dir: true,
                size: 0,
                mtime: UNIX_EPOCH,
            });
        } else if allowed {
            let meta = match entry.metadata() {
                Ok(m) => m,
                Err(source) => {
                    log::warn!("scan: stat {full_str}: {source}");
                    continue;
                }
            };
            out.files.push(EntryData {
                name,
                path: full_str,
                is_dir: false,
                size: meta.len(),
                mtime: meta.modified().unwrap_or(UNIX_EPOCH),
            });
        }
    }
    Ok(out)
}
