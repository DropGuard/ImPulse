//! In-directory watching. The watcher thread owns the platform's notification
//! machinery — inotify on Linux ([`fd`]), ReadDirectoryChangesW on Windows
//! ([`fd_windows`]) — and implements the drain discipline agreed in the
//! consistency discussions: the kernel queue is a conveyor belt, not a
//! warehouse — a dedicated thread reads with a large buffer and hands parsed
//! events to the engine loop through an ordered channel. Queue loss is
//! surfaced (IN_Q_OVERFLOW / ERROR_NOTIFY_ENUM_DIR) so the engine can
//! reconcile with a full rescan.
//!
//! The module is split on a purity seam: [`apply`] is the event semantics —
//! parse, classify, and apply one event to the listing state — with no libc
//! anywhere in it, which makes it the surface the property tests and Miri can
//! run. [`pair`] is the record stream's decision layer — the validation drops
//! and the cookie pairing state machine — equally pure, so the orderings the
//! kernel rarely produces (interleaved cookies, pairs split by a read
//! boundary) are testable against it directly. [`fd`] / [`fd_windows`] are the
//! machinery an interpreter cannot run: the platform notification source, the
//! wake event, the wait, and the watcher thread itself — each only feeds
//! records to `pair` and sends what it yields.

mod apply;
#[cfg(target_os = "linux")]
mod fd;
#[cfg(windows)]
mod fd_windows;
mod pair;

pub use apply::{
    Applied, IgnoreReason, RawWatchEvent, RawWatchKind, WatchAction, apply_event, classify,
    event_is_live, is_dir_mask, mtime_ms,
};
pub use pair::{DropReason, Out, Pairer, Record};

#[cfg(target_os = "linux")]
pub(crate) use fd::WatcherHandle;
#[cfg(windows)]
pub(crate) use fd_windows::WatcherHandle;

use std::path::{Path, PathBuf};

use crossbeam_channel::Sender;

// --- the platform-neutral glue both shells share -----------------------------
//
// fd.rs and fd_windows.rs are the syscall machinery; the command vocabulary,
// the stream dispatch and the baseline enumeration are the contract both
// serve, so they are defined once here — a fix to a trace format or to the
// scan-error wrapping cannot drift between the platforms. The shells import
// them from this module.

/// Everything about a folder that has to happen in one order happens on one
/// thread: install the subscription, then enumerate the baseline. Two threads
/// cannot promise that ordering to each other, which is why a change used to
/// be able to fall into a gap where it was in neither the snapshot nor any
/// event — not unnoticed, but undetectable. `session`/`dispatch` ride along so
/// the result can be attributed to the request it answers.
pub(crate) enum WatchCmd {
    Watch {
        path: PathBuf,
        session: u64,
        dispatch: u64,
    },
    /// Re-enumerate a folder that is already watched (a declared loss: the
    /// kernel queue overflowed, the folder vanished, or a caller asked).
    Enumerate {
        path: PathBuf,
        session: u64,
        dispatch: u64,
    },
    /// Drop the current watch. Explicit, because the previous implementation
    /// of "stop watching" was to add a watch on an empty path and rely on it
    /// failing.
    Unwatch,
    Stop,
}

/// Sends one raw event. False means the consumer is gone and the watcher
/// thread has nothing left to do.
fn send(ev_tx: &Sender<RawWatchEvent>, raw: RawWatchEvent) -> bool {
    crate::trace::line("read", || format!("{raw:?}"));
    ev_tx.send(raw).is_ok()
}

/// Hands one of the pairing layer's outcomes to the stream: the trace line
/// names what happened, the channel gets the event. False means the consumer
/// is gone.
fn dispatch(ev_tx: &Sender<RawWatchEvent>, out: Out, live_wd: Option<i32>) -> bool {
    match out {
        Out::Report {
            ev,
            paired_cookie: Some(cookie),
        } => {
            let from = match &ev.kind {
                RawWatchKind::Renamed { from } => from.as_str(),
                _ => "?",
            };
            crate::trace::line("move", || {
                format!(
                    "PAIRED cookie={cookie} {from} -> {}",
                    ev.name.as_deref().unwrap_or("?")
                )
            });
            send(ev_tx, ev)
        }
        Out::Report {
            ev,
            paired_cookie: None,
        } => send(ev_tx, ev),
        Out::Unpaired { ev, cookie } => {
            crate::trace::line("move", || {
                format!(
                    "UNPAIRED cookie={cookie} {} (no MOVED_TO beside it)",
                    ev.name.as_deref().unwrap_or("?")
                )
            });
            send(ev_tx, ev)
        }
        Out::Drop(reason) => {
            match &reason {
                DropReason::Bookkeeping { bits } => {
                    crate::trace::line("drop", || format!("mask={bits:#x} bookkeeping"))
                }
                DropReason::DeadWatch { wd, kind } => {
                    crate::trace::line("drop", || format!("kind={kind:?} wd={wd} live={live_wd:?}"))
                }
                DropReason::NoDirectory { kind } => crate::trace::line("drop", || {
                    format!("kind={kind:?} no directory to attribute")
                }),
                DropReason::NoName { kind } => {
                    crate::trace::line("drop", || format!("kind={kind:?} no name"))
                }
            }
            true
        }
    }
}

/// Take the baseline for a folder. Called immediately after the watch is
/// installed, by the same thread, so "changes from now on are visible" and
/// "this is what was there" cannot be reordered against each other. The
/// result goes back to the engine loop, which is the only writer of the
/// listing state.
fn enumerate_and_report(
    path: &Path,
    session: u64,
    dispatch: u64,
    res_tx: &Sender<crate::engine::ScanDone>,
    options: &crate::scan::ScanOptions,
) {
    let start = std::time::Instant::now();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        crate::scan::scan_directory(path, options)
    }))
    .unwrap_or_else(|_| {
        Err(crate::error::EngineError::Io {
            path: path.to_path_buf(),
            source: std::io::Error::other("scan worker panicked"),
        })
    });
    let _ = res_tx.send(crate::engine::ScanDone {
        session,
        dispatch,
        path: path.to_path_buf(),
        result,
        scan_ms: start.elapsed().as_millis(),
    });
}
