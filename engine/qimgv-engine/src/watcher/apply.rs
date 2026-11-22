//! Event semantics: parse one inotify record's mask, then apply the parsed
//! event to the listing state. Nothing here touches libc, channels or threads —
//! the only I/O is `fs::metadata` on the subject path, taken from a
//! [`Prestat`] the caller supplies (the engine takes it before the state's
//! write lock; standalone callers stat inline). This file is the surface Miri
//! can run.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::filter::NameFilter;
use crate::model::DirectoryState;

/// The filesystem facts one event's application will need, taken before the
/// state's write lock. The engine pre-statss the event's subject path — for a
/// rename that IS the destination, the pairing resolved the name — so no
/// filesystem I/O runs under the lock, and a UI-side export never waits on a
/// stat. A path the prestat did not predict is statted live at the point of
/// use, keeping the application total over any event.
#[derive(Debug, Default)]
pub struct Prestat {
    entries: HashMap<PathBuf, std::io::Result<fs::Metadata>>,
}

impl Prestat {
    pub fn of_event(ev: &RawWatchEvent) -> Self {
        let mut entries = HashMap::new();
        if let Some(name) = &ev.name {
            let p = PathBuf::from(subject_path(&ev.dir, name));
            entries.insert(p.clone(), fs::metadata(&p));
        }
        Prestat { entries }
    }

    /// The stat for `path`: the pre-taken one when the plan predicted it, a
    /// live one otherwise. An Err means the path is gone.
    pub fn stat(&self, path: impl AsRef<Path>) -> std::io::Result<fs::Metadata> {
        let key = path.as_ref();
        match self.entries.get(key) {
            Some(Ok(m)) => Ok(m.clone()),
            Some(Err(e)) => Err(std::io::Error::new(e.kind(), e.to_string())),
            None => fs::metadata(key),
        }
    }
}

/// One parsed inotify event, before it touches the engine state.
///
/// `dir` is the directory the *watch that produced this event* was added for,
/// resolved by the watcher thread — not by the consumer. An event that carries
/// only a name forces the receiver to supply the directory from its own state,
/// and a stale event then gets a live directory bolted onto it: deleting
/// `/A/img1.jpg` would remove `/B/img1.jpg`, which is a file that still exists.
/// With the subject inside the message, that deletion simply does not resolve.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawWatchEvent {
    /// The kernel's watch descriptor. Only `Overflow` events have no watch
    /// (`wd == -1`); everything else was produced by exactly one directory.
    pub wd: i32,
    pub dir: PathBuf,
    /// The name the record names. For a paired move this is the DESTINATION —
    /// the source rides inside [`RawWatchKind::Renamed`], because a rename that
    /// could be described by one name is not a rename.
    pub name: Option<String>,
    pub is_dir: bool,
    pub kind: RawWatchKind,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RawWatchKind {
    Created,
    Modified,
    Deleted,
    MovedFrom,
    MovedTo,
    /// The two halves of one move, linked by the cookie the kernel gave them.
    /// Emitted only when the watcher thread saw a MOVED_TO whose cookie matched
    /// the MOVED_FROM directly before it; `name` is the destination and `from`
    /// the source. Everything else about a rename is decided by `apply_event`,
    /// which is the half that can see the listing.
    Renamed {
        from: String,
    },
    /// Kernel dropped events (queue full) — full rescan required.
    Overflow,
    /// The watched directory itself went away.
    WatchGone,
}

// Raw inotify masks (inotify(7)). The kernel's record header is parsed by the
// inotify crate; these constants classify its mask bits, kept as plain u32 so
// the mapping can be tested without the crate's bitflags in the public API.
mod mask {
    pub const MODIFY: u32 = 0x0000_0002;
    pub const MOVED_FROM: u32 = 0x0000_0040;
    pub const MOVED_TO: u32 = 0x0000_0080;
    pub const CREATE: u32 = 0x0000_0100;
    pub const DELETE: u32 = 0x0000_0200;
    pub const DELETE_SELF: u32 = 0x0000_0400;
    pub const MOVE_SELF: u32 = 0x0000_0800;
    pub const Q_OVERFLOW: u32 = 0x0000_4000;
    pub const IGNORED: u32 = 0x0000_8000;
    pub const ISDIR: u32 = 0x4000_0000;
}

/// Maps one event's mask bits to what the engine cares about. `None` means the
/// record is bookkeeping the listing does not react to (IN_IGNORED after a
/// removal, attrib-only changes, and so on).
pub fn classify(mask: u32) -> Option<RawWatchKind> {
    let kind = if mask & mask::Q_OVERFLOW != 0 {
        RawWatchKind::Overflow
    } else if mask & (mask::DELETE_SELF | mask::MOVE_SELF) != 0 {
        RawWatchKind::WatchGone
    } else if mask & mask::IGNORED != 0 {
        return None; // bookkeeping after rm_watch / auto-removal
    } else if mask & mask::CREATE != 0 {
        RawWatchKind::Created
    } else if mask & mask::MODIFY != 0 {
        RawWatchKind::Modified
    } else if mask & mask::DELETE != 0 {
        RawWatchKind::Deleted
    } else if mask & mask::MOVED_FROM != 0 {
        RawWatchKind::MovedFrom
    } else if mask & mask::MOVED_TO != 0 {
        RawWatchKind::MovedTo
    } else {
        return None;
    };
    Some(kind)
}

pub fn is_dir_mask(mask: u32) -> bool {
    mask & mask::ISDIR != 0
}

/// True when an event belongs to the currently installed watch. Overflow is
/// the exception: it names no watch (the kernel sends `wd == -1`) because it
/// describes the queue as a whole, and the queue's only subject is the live
/// directory — dropping it would silently stop the engine from ever
/// reconciling after a kernel-side loss.
pub fn event_is_live(wd: i32, kind: &RawWatchKind, live_wd: Option<i32>) -> bool {
    if matches!(*kind, RawWatchKind::Overflow) {
        return true;
    }
    live_wd == Some(wd)
}

/// The joined subject path, in the engine's wire form. On Windows the join
/// would hand back mixed separators — the watch directory arrives in Qt's
/// forward-slash form and the FFI contract hands paths to a C++ side that
/// compares them as strings against the form it opened the directory with.
/// The form itself is `scan::wire_path`'s, one rule for both producers: the
/// scan baseline mints listing paths the same way, and a divergence between
/// them would put one file in the listing twice. POSIX joins are byte-exact (a
/// backslash is a legal filename character there) and the no-op replace keeps
/// that side identical.
pub fn subject_path(dir: &Path, name: &str) -> String {
    crate::scan::wire_path(&dir.join(name))
}

/// What the engine loop should do with a parsed event.
#[derive(Debug, Clone, PartialEq)]
pub enum Applied {
    Added,
    Modified,
    Removed,
    /// One entry, a new path. `from` is the path it had; the event's own path,
    /// name, size and mtime describe what it is now, and `index` is where the
    /// engine's order puts it. That is the whole result, so a consumer needs
    /// nothing else to decide — in particular not the old position: the row it
    /// replaces is found by path, the same way a Removed row is.
    Renamed {
        from: String,
    },
}

/// Why an event touched nothing. A bare "ignored" is the one answer that cannot
/// be investigated: when a file is missing from the listing, "the watcher never
/// reported it", "the filter refused its name" and "it was already in the
/// listing" all look identical from outside, and only the first means a bug. So
/// every `Ignored` has to say which of them it is, and a trace line can name it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IgnoreReason {
    /// The event is about a folder other than the one this state describes.
    WrongSubject,
    /// A create or move with no name attached — inotify always supplies one, so
    /// reaching this means the parse produced something it should not have.
    NoName,
    /// Dotfile, and the scanner's hidden rule says it is not listable.
    Hidden,
    /// Not a directory and its name matches neither the whitelist nor an override.
    ExtensionRejected,
    /// The path was gone by the time it was stat'd.
    StatFailed,
    /// A create for a path the listing already holds.
    AlreadyListed,
    /// A delete or modify for a path this listing never contained.
    NotListed,
    /// A modify on a directory: its own mtime has no listing meaning.
    DirIsCanonical,
    /// A modify whose on-disk size AND mtime both equal the ones already
    /// stored (on NTFS the mtime alone cannot be trusted: ~10 ms stamp
    /// granularity makes same-tick writes carry an identical mtime).
    MtimeUnchanged,
    /// The application step panicked; the engine loop's guard reports this and
    /// prints the payload.
    Panicked,
}

#[derive(Debug, Clone, PartialEq)]
pub enum WatchAction {
    Changed {
        kind: Applied,
        path: String,
        is_dir: bool,
        size: u64,
        mtime_ms: i64,
        /// The entry's position in the engine's own order at the time of the
        /// event. The engine decides where things sit; consumers splice.
        index: u32,
    },
    /// The watcher snapshot can't be trusted for this directory.
    Reconcile {
        dir: PathBuf,
    },
    /// The watched directory is gone.
    DirGone {
        dir: PathBuf,
    },
    Ignored(IgnoreReason),
}

/// Applies one parsed event to the state. The directory comes from the event:
/// a caller that supplied its own current directory would apply a stale event
/// from one folder against another folder's listing.
pub fn apply_event(
    state: &mut DirectoryState,
    filter: &NameFilter,
    overrides: &mut std::collections::HashSet<PathBuf>,
    show_hidden: bool,
    ev: &RawWatchEvent,
) -> WatchAction {
    // standalone callers (the suites) stat inline: same facts, taken at the
    // caller's own timing instead of the engine's pre-lock pass
    let stats = Prestat::of_event(ev);
    apply_event_with_stats(state, filter, overrides, show_hidden, ev, &stats)
}

/// The engine's entry: identical semantics to [`apply_event`], with the
/// event's filesystem facts supplied from outside the state's write lock —
/// the discipline-5 rule (state locks never span waits) applied to the one
/// syscall this path performs.
pub fn apply_event_with_stats(
    state: &mut DirectoryState,
    filter: &NameFilter,
    overrides: &mut std::collections::HashSet<PathBuf>,
    show_hidden: bool,
    ev: &RawWatchEvent,
    stats: &Prestat,
) -> WatchAction {
    use RawWatchKind as K;
    let dir = ev.dir.as_path();
    // The event's subject has to be the directory this state describes. A delete
    // for elsewhere already resolves to nothing here, but a *create* elsewhere
    // would otherwise insert a foreign path as a row of this listing — the two
    // halves of the same mistake, and both closed by one check at the point of
    // mutation rather than by callers remembering to filter.
    if state.dir() != Some(dir) {
        return WatchAction::Ignored(IgnoreReason::WrongSubject);
    }
    match &ev.kind {
        K::Overflow => WatchAction::Reconcile {
            dir: dir.to_path_buf(),
        },
        K::WatchGone => WatchAction::DirGone {
            dir: dir.to_path_buf(),
        },
        K::Created => arrive(
            state,
            filter,
            overrides,
            show_hidden,
            dir,
            ev,
            Arrival::Created,
            stats,
        ),
        K::MovedTo => arrive(
            state,
            filter,
            overrides,
            show_hidden,
            dir,
            ev,
            Arrival::Moved,
            stats,
        ),
        K::Deleted | K::MovedFrom => remove_listed(state, dir, ev),
        K::Modified => {
            // A directory's own mtime carries no listing meaning (see the
            // canonical-dir note in `admit`), and no consumer mirrors it: the
            // engine state stays exactly as the scanner left it.
            if ev.is_dir {
                return WatchAction::Ignored(IgnoreReason::DirIsCanonical);
            }
            restat(
                state,
                subject_path(dir, ev.name.as_deref().unwrap_or("")),
                &stats,
            )
        }
        K::Renamed { from } => rename(state, filter, overrides, show_hidden, dir, ev, from, stats),
    }
}

/// How the record says a name came to be in this directory. It decides what an
/// already-listed destination means: a create over a path we hold is a
/// duplicate, a move over it replaced the file behind that row.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Arrival {
    Created,
    Moved,
}

/// The scanner's membership rules, in the scanner's order: a hidden entry is not
/// admitted whatever else would have allowed it, and directories bypass the
/// extension whitelist — scan.rs puts every directory in the listing without
/// consulting it, and the watcher used to run dir names through it too, so an
/// external mkdir matched no image extension and the folder never appeared until
/// a full rescan. `Some(reason)` is the name saying this is not a member.
fn rejection(
    is_dir: bool,
    path: &Path,
    name: &str,
    filter: &NameFilter,
    overrides: &std::collections::HashSet<PathBuf>,
    show_hidden: bool,
) -> Option<IgnoreReason> {
    if crate::scan::is_hidden(path, name) && !show_hidden {
        return Some(IgnoreReason::Hidden);
    }
    if !is_dir && !filter.matches(name) && !overrides.contains(path) {
        return Some(IgnoreReason::ExtensionRejected);
    }
    None
}

/// A record saying a name arrived, with the name taken off the event. inotify
/// always names a create or a move, so a missing name is a parse that should not
/// have reached here — and saying so is what keeps that from reading as an
/// ignore the engine caused.
fn arrive(
    state: &mut DirectoryState,
    filter: &NameFilter,
    overrides: &mut std::collections::HashSet<PathBuf>,
    show_hidden: bool,
    dir: &Path,
    ev: &RawWatchEvent,
    arrival: Arrival,
    stats: &Prestat,
) -> WatchAction {
    match &ev.name {
        Some(name) => admit(
            state,
            filter,
            overrides,
            show_hidden,
            dir,
            name,
            ev.is_dir,
            arrival,
            stats,
        ),
        None => WatchAction::Ignored(IgnoreReason::NoName),
    }
}

/// A name the record says is now in this directory. For a move it is also the
/// half of a rename that came in from outside the watch.
fn admit(
    state: &mut DirectoryState,
    filter: &NameFilter,
    overrides: &mut std::collections::HashSet<PathBuf>,
    show_hidden: bool,
    dir: &Path,
    name: &str,
    is_dir: bool,
    arrival: Arrival,
    stats: &Prestat,
) -> WatchAction {
    let path_str = subject_path(dir, name);
    let path = Path::new(&path_str);
    if let Some(reason) = rejection(is_dir, &path, name, filter, overrides, show_hidden) {
        return WatchAction::Ignored(reason);
    }
    if state.contains_path(&path_str) {
        // A move landing on a listed path is an overwrite: the contents the row
        // describes are gone and the moved-in file's are here. Answering
        // `AlreadyListed` here made an atomic overwrite invisible to the mirror
        // — the row kept a size and an mtime that described nothing on disk,
        // and the thumbnail cache, which validates by mtime, kept serving the
        // picture that used to be there.
        return if arrival == Arrival::Moved {
            restat(state, path_str, stats)
        } else {
            WatchAction::Ignored(IgnoreReason::AlreadyListed)
        };
    }
    let meta = match stats.stat(&path) {
        Ok(m) => m,
        // vanished already
        Err(_) => return WatchAction::Ignored(IgnoreReason::StatFailed),
    };
    let size = if is_dir { 0 } else { meta.len() };
    // Directories are canonical (size 0, mtime epoch, name-ordered in every
    // mode): scan.rs creates them that way, so a watched creation must not hand
    // them real metadata that would let a size/time sort reorder them against
    // the scanner's rule.
    let mtime = if is_dir {
        UNIX_EPOCH
    } else {
        meta.modified().unwrap_or(UNIX_EPOCH)
    };
    let entry = crate::model::EntryData {
        name: name.to_owned(),
        path: path_str.clone(),
        is_dir,
        size,
        mtime,
    };
    if let Some(index) = state.insert_entry(entry) {
        WatchAction::Changed {
            kind: Applied::Added,
            path: path_str,
            is_dir,
            size,
            mtime_ms: if is_dir { 0 } else { mtime_ms(mtime) },
            index,
        }
    } else {
        WatchAction::Ignored(IgnoreReason::AlreadyListed)
    }
}

/// The path the record names is gone from the directory. Only the listing's own
/// answer says whether that is a change: a delete for a path this listing never
/// held is nothing, and saying so is what keeps a stale event from taking a row
/// out of the wrong folder.
fn remove_listed(state: &mut DirectoryState, dir: &Path, ev: &RawWatchEvent) -> WatchAction {
    let path = subject_path(dir, ev.name.as_deref().unwrap_or(""));
    remove_or_ignore(state, path, ev.is_dir)
}

fn remove_or_ignore(state: &mut DirectoryState, path: String, is_dir: bool) -> WatchAction {
    if let Some(index) = state.remove_entry_by_path(&path) {
        WatchAction::Changed {
            kind: Applied::Removed,
            path,
            is_dir,
            size: 0,
            mtime_ms: 0,
            index,
        }
    } else {
        WatchAction::Ignored(IgnoreReason::NotListed)
    }
}

/// The content behind a path the listing already holds is different: take the
/// new size and mtime off the disk and report the row where the engine's order
/// now puts it. Shared by IN_MODIFY, by a move that overwrote a listed name and
/// by the destination of a rename — all three say "this row's file changed", and
/// none of them may be answered by re-deriving an order here.
fn restat(state: &mut DirectoryState, path: String, stats: &Prestat) -> WatchAction {
    let Some((old_size, old_mtime)) = state.entry_meta(&path) else {
        return WatchAction::Ignored(IgnoreReason::NotListed);
    };
    let meta = match stats.stat(&path) {
        Ok(m) => m,
        Err(_) => return WatchAction::Ignored(IgnoreReason::StatFailed),
    };
    let size = meta.len();
    let mtime = meta.modified().unwrap_or(UNIX_EPOCH);
    // the gate is BOTH facts, not the mtime alone: NTFS stamps at ~10 ms
    // granularity, so a write that closes in the tick the file was created in
    // carries an identical mtime — under load that is the common case, and a
    // mtime-only gate swallowed a 4 KiB append whole (the listing flake:
    // three full-ctest runs, the mirror faithful to an engine that never saw
    // the change). ext4's nanosecond stamps never collide, which is why the
    // mtime-only form survived on Linux.
    if mtime == old_mtime && size == old_size {
        return WatchAction::Ignored(IgnoreReason::MtimeUnchanged);
    }
    let Some(index) = state.update_entry_meta(&path, size, mtime) else {
        return WatchAction::Ignored(IgnoreReason::NotListed);
    };
    WatchAction::Changed {
        kind: Applied::Modified,
        path,
        is_dir: false,
        size,
        mtime_ms: mtime_ms(mtime),
        index,
    }
}

/// One move, both ends seen, linked by the kernel's cookie. What it means is
/// decided by which ends the listing has a stake in, and every answer but one is
/// already in the vocabulary — which is what makes the pair safe to introduce:
/// a rename whose destination is not a member (`img.jpg_<hash>`, `.hidden.png`,
/// `notes.txt`) is still exactly the removal an unpaired MOVED_FROM reported,
/// and a rename in from outside the watch is still exactly an arrival.
fn rename(
    state: &mut DirectoryState,
    filter: &NameFilter,
    overrides: &mut std::collections::HashSet<PathBuf>,
    show_hidden: bool,
    dir: &Path,
    ev: &RawWatchEvent,
    from: &str,
    stats: &Prestat,
) -> WatchAction {
    let Some(name) = &ev.name else {
        return WatchAction::Ignored(IgnoreReason::NoName);
    };
    let from_path = subject_path(dir, from);
    let to_str = subject_path(dir, name);
    let to_path = Path::new(&to_str);
    // Renaming a file onto the name it already has is a no-op in POSIX and
    // emits nothing, so arriving here means the kernel or a test said otherwise.
    // Take it as no change: the alternative is removing the row for the
    // destination — which is this entry — and then having nothing to rename.
    if from_path == to_str {
        return WatchAction::Ignored(IgnoreReason::AlreadyListed);
    }
    let listed = state.contains_path(&from_path);
    match (
        listed,
        rejection(ev.is_dir, &to_path, name, filter, overrides, show_hidden),
    ) {
        // The row is here and the new name is a member: one entry, one path.
        (true, None) => {
            let meta = match stats.stat(&to_path) {
                Ok(m) => m,
                // Gone already: the only fact left is that the source is not
                // here any more, which is what an unpaired MOVED_FROM said.
                Err(_) => return remove_or_ignore(state, from_path, ev.is_dir),
            };
            let size = if ev.is_dir { 0 } else { meta.len() };
            let mtime = if ev.is_dir {
                UNIX_EPOCH
            } else {
                meta.modified().unwrap_or(UNIX_EPOCH)
            };
            let Some(index) = state.rename_entry(&from_path, &to_str, name, size, mtime) else {
                return WatchAction::Ignored(IgnoreReason::NotListed);
            };
            WatchAction::Changed {
                kind: Applied::Renamed { from: from_path },
                path: to_str,
                is_dir: ev.is_dir,
                size,
                mtime_ms: if ev.is_dir { 0 } else { mtime_ms(mtime) },
                index,
            }
        }
        // The name it moved to is not one this listing holds.
        (true, Some(reason)) => match remove_or_ignore(state, from_path, ev.is_dir) {
            WatchAction::Ignored(IgnoreReason::NotListed) => WatchAction::Ignored(reason),
            removed => removed,
        },
        // The source was never ours: this is an arrival from wherever it came,
        // including a save's backup landing on the file it is replacing.
        (false, None) => admit(
            state,
            filter,
            overrides,
            show_hidden,
            dir,
            name,
            ev.is_dir,
            Arrival::Moved,
            stats,
        ),
        (false, Some(reason)) => WatchAction::Ignored(reason),
    }
}

pub fn mtime_ms(t: SystemTime) -> i64 {
    t.duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}
