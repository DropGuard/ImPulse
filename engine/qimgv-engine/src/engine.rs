use std::collections::HashSet;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock, RwLockWriteGuard};
use std::thread;
use std::time::Instant;

use crossbeam_channel::{Receiver, Sender, unbounded};

use crate::error::EngineError;
use crate::filter::NameFilter;
use crate::model::DirectoryState;
use crate::scan::{ScanOptions, ScanOutput};
use crate::sort::SortMode;
#[cfg(any(target_os = "linux", windows))]
use crate::watcher::WatcherHandle;
use crate::watcher::{self, WatchMessage};

/// Called (from an engine thread) after an event was queued. The FFI layer
/// supplies an eventfd writer; tests supply a channel send. Foreign code may
/// panic — it is caught and ignored.
pub type Notify = Arc<dyn Fn() + Send + Sync>;

#[derive(Debug, Clone)]
pub enum Command {
    /// Opens a directory. `sort` is applied atomically with the scan (one
    /// DirListed event) — passing it separately would emit a stale resort
    /// event for the *previous* directory's state. `epoch` identifies the
    /// request (allocated by the caller via `next_epoch`).
    OpenDir {
        path: PathBuf,
        sort: Option<SortMode>,
        epoch: u64,
    },
    SetSort(SortMode),
    /// Adds a path to the listing bypassing the extension filter (app-side
    /// mime fallback); persists across rescans until the file disappears.
    IncludePath(PathBuf),
    /// Stops watching the current directory (listing reset on the C++ side).
    StopWatch,
    /// Re-read the folder of the current request. Raised by the engine itself when
    /// the kernel queue overflowed or the watched folder vanished, and by a scan
    /// that was interrupted mid-flight; addressable from a test for the same reason
    /// it is needed in production: both are "the answer I have is not stable".
    Rescan(PathBuf),
    Shutdown,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WatchChangeKind {
    Added,
    Modified,
    Removed,
    /// One entry, a new path: `path`/`name`/`size`/`mtime_ms`/`index` describe
    /// what it is now and `from` is what it was. A consumer that knows only
    /// `path` would treat this as an add and leave the old row behind, which is
    /// why the old path rides in the kind rather than in a caller's memory of
    /// the previous event.
    Renamed {
        from: String,
    },
}

#[derive(Debug)]
pub enum Event {
    /// Directory listing replaced. Rows are new, gone or changed.
    DirListed {
        dir: PathBuf,
        epoch: u64,
        revision: u64,
    },
    /// A sort committed within the installed session.
    SortChanged {
        dir: PathBuf,
        epoch: u64,
        revision: u64,
    },
    OpenFailed {
        dir: PathBuf,
        error: Arc<EngineError>,
        epoch: u64,
    },
    /// Diagnostic record of a committed watcher change. Its index belongs to
    /// this commit only; snapshot readers use it solely as a wake.
    Mutated {
        /// This engine's own count of the mutations it has emitted, starting at
        /// 1. Crosses the ABI as `qimgv_event.seq`.
        seq: u64,
        /// The listing version after this change. Crosses the ABI as
        /// `qimgv_event.revision`.
        revision: u64,
        kind: WatchChangeKind,
        path: String,
        is_dir: bool,
        size: u64,
        mtime_ms: i64,
        index: u32,
        epoch: u64,
    },
}

#[derive(Clone)]
pub struct EngineConfig {
    pub show_hidden: Arc<AtomicBool>,
    pub filter: NameFilter,
    pub initial_sort: SortMode,
}

/// The current open request and the newest scan that may answer it.
struct ListingRequest {
    dir: PathBuf,
    epoch: u64,
    dispatch: u64,
}

impl ListingRequest {
    /// Take the listing again, as the same epoch: a rescan answers the request
    /// that already exists, it does not start one.
    fn rescan(&mut self, next: &mut u64) -> ScanJob {
        *next += 1;
        self.dispatch = *next;
        ScanJob {
            epoch: self.epoch,
            dispatch: self.dispatch,
            path: self.dir.clone(),
        }
    }
}

pub(crate) struct ScanJob {
    epoch: u64,
    dispatch: u64,
    path: PathBuf,
}
pub(crate) struct ScanDone {
    pub(crate) epoch: u64,
    pub(crate) dispatch: u64,
    pub(crate) path: PathBuf,
    pub(crate) result: Result<ScanOutput, EngineError>,
    pub(crate) scan_ms: u128,
}

pub struct Engine {
    cmd_tx: Sender<Command>,
    events: Receiver<Event>,
    loop_join: Option<thread::JoinHandle<()>>,
    show_hidden: Arc<AtomicBool>,
    epoch: Arc<AtomicU64>,
    pub state: Arc<RwLock<DirectoryState>>,
}

fn state_write(lock: &RwLock<DirectoryState>) -> RwLockWriteGuard<'_, DirectoryState> {
    match lock.write() {
        Ok(g) => g,
        Err(poisoned) => poisoned.into_inner(),
    }
}

fn fire_notify(notify: &Notify) {
    let _ = catch_unwind(AssertUnwindSafe(|| notify()));
}

impl Engine {
    /// Starts the engine loop thread and the platform watcher thread (inotify
    /// on Linux, ReadDirectoryChangesW on Windows). Enumeration runs on the
    /// watcher thread itself; there is no scan worker pool.
    /// Dropping the returned handle shuts everything
    /// down (command channel closes → loop breaks → job channel closes →
    /// workers exit; watcher is joined explicitly).
    pub fn spawn(cfg: EngineConfig, notify: Notify) -> Engine {
        let (cmd_tx, cmd_rx) = unbounded();
        let (ev_tx, ev_rx) = unbounded::<Event>();
        let state = Arc::new(RwLock::new(DirectoryState::new(cfg.initial_sort)));
        let epoch = Arc::new(AtomicU64::new(0));
        let epoch_shared = Arc::clone(&epoch);
        let show_hidden = cfg.show_hidden;
        let filter = cfg.filter;
        let overrides: Arc<Mutex<HashSet<PathBuf>>> = Arc::new(Mutex::new(HashSet::new()));

        #[cfg(any(target_os = "linux", windows))]
        let (watcher, watch_ev_rx) = {
            let options = ScanOptions {
                show_hidden: Arc::clone(&show_hidden),
                filter: filter.clone(),
                overrides: Arc::clone(&overrides),
            };
            let (handle, rx) = WatcherHandle::spawn(options);
            (Some(handle), rx)
        };
        #[cfg(not(any(target_os = "linux", windows)))]
        let watch_ev_rx: Receiver<WatchMessage> = crossbeam_channel::never();

        let loop_join = {
            let state = Arc::clone(&state);
            let show_hidden = Arc::clone(&show_hidden);
            let overrides = Arc::clone(&overrides);
            let filter = filter.clone();
            let cmd_tx_for_loop = cmd_tx.clone();
            thread::spawn(move || {
                let mut listing: Option<ListingRequest> = None;
                let mut requested_sort = cfg.initial_sort;
                // only this thread dispatches scans, so the number needs no atomic
                let mut next_dispatch: u64 = 0;
                // Ordinal of the mutations this engine has emitted. Assigned at
                // birth, before the event can be dropped or truncated by anyone:
                // a number printed by the engine and a number printed by the C++
                // mirror identify the same event, which is what makes the two
                // halves of a trace comparable when one of them is missing a line.
                let mut next_mutation: u64 = 0;
                loop {
                    crossbeam_channel::select! {
                        recv(cmd_rx) -> cmd => {
                            match cmd {
                                Err(_) | Ok(Command::Shutdown) => break,
                                Ok(Command::OpenDir { path, sort, epoch: e }) => {
                                    requested_sort = sort.unwrap_or(requested_sort);
                                    let mut req = ListingRequest {
                                        dir: path.clone(),
                                        epoch: e,
                                        dispatch: 0,
                                    };
                                    let job = req.rescan(&mut next_dispatch);
                                    listing = Some(req);
                                    #[cfg(any(target_os = "linux", windows))]
                                    if let Some(w) = &watcher {
                                        w.send(watcher::WatchCmd::Enumerate {
                                            path: job.path,
                                            epoch: job.epoch,
                                            dispatch: job.dispatch,
                                            install: true,
                                        });
                                    }
                                }
                                Ok(Command::Rescan(dir)) => {
                                    // only the folder the current request is about
                                    // can be re-read; anything else is a result for
                                    // a folder nobody asked about any more
                                    let job = match &mut listing {
                                        Some(req) if req.dir == dir => Some(req.rescan(&mut next_dispatch)),
                                        _ => None,
                                    };
                                    #[cfg(any(target_os = "linux", windows))]
                                    if let Some(job) = job
                                        && let Some(w) = &watcher
                                    {
                                        w.send(watcher::WatchCmd::Enumerate {
                                            path: job.path,
                                            epoch: job.epoch,
                                            dispatch: job.dispatch,
                                            install: false,
                                        });
                                    }
                                }
                                Ok(Command::SetSort(mode)) => {
                                    requested_sort = mode;
                                    let applied = {
                                        let mut st = state_write(&state);
                                        if matches!(&listing, Some(req) if req.epoch == st.session()) && st.resort(mode) {
                                            st.dir().map(|dir| (dir.to_owned(), st.session(), st.revision()))
                                        } else {
                                            None
                                        }
                                    };
                                    if let Some((dir, epoch, revision)) = applied {
                                        let _ = ev_tx.send(Event::SortChanged { dir, epoch, revision });
                                        fire_notify(&notify);
                                    }
                                }
                                Ok(Command::IncludePath(p)) => {
                                    let applied = {
                                        let meta = std::fs::metadata(&p);
                                        let mut ov = overrides.lock().unwrap_or_else(|e| e.into_inner());
                                        ov.insert(p.clone());
                                        let mut st = state_write(&state);
                                        let inserted = matches!(&listing, Some(req) if req.epoch == st.session())
                                            && meta.ok().and_then(|m| st.insert_entry(crate::model::EntryData {
                                                name: p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
                                                path: p.to_string_lossy().into_owned(),
                                                is_dir: m.is_dir(),
                                                size: if m.is_dir() { 0 } else { m.len() },
                                                mtime: if m.is_dir() { std::time::UNIX_EPOCH } else { m.modified().unwrap_or(std::time::UNIX_EPOCH) },
                                            })).is_some();
                                        if inserted {
                                            st.dir().map(|dir| (dir.to_owned(), st.session(), st.revision()))
                                        } else {
                                            None
                                        }
                                    };
                                    if let Some((dir, epoch, revision)) = applied {
                                        let _ = ev_tx.send(Event::DirListed { dir, epoch, revision });
                                        fire_notify(&notify);
                                    }
                                }
                                Ok(Command::StopWatch) => {
                                    listing = None;
                                    #[cfg(any(target_os = "linux", windows))]
                                    if let Some(w) = &watcher {
                                        w.send(watcher::WatchCmd::Unwatch);
                                    }
                                }
                            }
                        }
                        recv(watch_ev_rx) -> message => match message {
                            Err(_) => break,
                            Ok(WatchMessage::Scan(done)) => {
                                let current = listing.as_ref().map(|r| r.dispatch == done.dispatch).unwrap_or(false);
                                if !current {
                                    continue;
                                }
                                match done.result {
                                    Ok(out) => {
                                        let dir = done.path.clone();
                                        let entry_count = out.files.len();
                                        let apply_start = Instant::now();
                                        let applied_revision = {
                                            let mut st = state_write(&state);
                                            st.install(done.epoch, dir.clone(), requested_sort, out.files, out.dirs);
                                            st.revision()
                                        };
                                        eprintln!(
                                            "[engine] scan {} ms + insert/sort {} ms ({} entries, off UI thread)",
                                            done.scan_ms,
                                            apply_start.elapsed().as_millis(),
                                            entry_count
                                        );
                                        let _ = ev_tx.send(Event::DirListed {
                                            dir,
                                            epoch: done.epoch,
                                            revision: applied_revision,
                                        });
                                        fire_notify(&notify);
                                    }
                                    Err(e) => {
                                        let _ = ev_tx.send(Event::OpenFailed {
                                            dir: done.path,
                                            error: Arc::new(e),
                                            epoch: done.epoch,
                                        });
                                        fire_notify(&notify);
                                    }
                                }
                            }
                            Ok(WatchMessage::Change { session, raw }) => {
                                if !matches!(&listing, Some(req) if req.epoch == session && req.dir == raw.dir) {
                                    continue;
                                }
                                // the event's stats are taken BEFORE the write
                                // lock: state locks never span waits (rule 5),
                                // and a slow mount's fs::metadata would stall a
                                // UI-side export for the length of the stat
                                let stats = watcher::Prestat::of_event(&raw);
                                let action = {
                                    let mut st = state_write(&state);
                                    if st.session() != session {
                                        continue;
                                    }
                                    let mut ov = overrides.lock().unwrap_or_else(|e| e.into_inner());
                                    catch_unwind(AssertUnwindSafe(|| {
                                        watcher::apply_event_with_stats(&mut st, &filter, &mut ov,
                                                                        show_hidden.load(Ordering::Relaxed),
                                                                        &raw, &stats)
                                    }))
                                    .unwrap_or_else(|payload| {
                                        // The guard exists so one bad event cannot
                                        // end the loop, but a guard that swallows
                                        // its payload turns a crash into a missing
                                        // file — indistinguishable from "nothing
                                        // happened". Say what actually unwound.
                                        let msg = payload
                                            .downcast_ref::<&str>().map(|s| (*s).to_string())
                                            .or_else(|| payload.downcast_ref::<String>().cloned())
                                            .unwrap_or_else(|| "<non-string payload>".to_string());
                                        eprintln!("[engine] watch event application panicked: {msg}");
                                        watcher::WatchAction::Ignored(watcher::IgnoreReason::Panicked)
                                    })
                                };
                                // read after the change, still inside the same
                                // iteration: this is the version the event leaves
                                // the engine at
                                let applied_revision = state
                                    .read()
                                    .unwrap_or_else(|p| p.into_inner())
                                    .revision();
                                crate::trace::line("apply", || format!("{raw:?} -> {action:?}"));
                                match action {
                                    watcher::WatchAction::Changed { kind, path, is_dir, size, mtime_ms, index } => {
                                        let kind = match kind {
                                            watcher::Applied::Added => WatchChangeKind::Added,
                                            watcher::Applied::Modified => WatchChangeKind::Modified,
                                            watcher::Applied::Removed => WatchChangeKind::Removed,
                                            watcher::Applied::Renamed { from } => {
                                                // Everything the engine keys by path has to
                                                // follow the entry, before the event goes out:
                                                // an include-path override still naming the
                                                // old path is a claim about a file that no
                                                // longer exists.
                                                {
                                                    let mut ov = overrides.lock().unwrap_or_else(|e| e.into_inner());
                                                    if ov.remove(Path::new(&from)) {
                                                        ov.insert(Path::new(&path).to_path_buf());
                                                    }
                                                }
                                                WatchChangeKind::Renamed { from }
                                            }
                                        };
                                        next_mutation += 1;
                                        crate::trace::line("emit", || format!(
                                            "seq={next_mutation} {kind:?} {path} index={index} epoch={session}"));
                                        let _ = ev_tx.send(Event::Mutated {
                                            seq: next_mutation,
                                            revision: applied_revision,
                                            kind,
                                            path,
                                            is_dir,
                                            size,
                                            mtime_ms,
                                            index,
                                            epoch: session,
                                        });
                                        fire_notify(&notify);
                                    }
                                    watcher::WatchAction::Reconcile { dir } => {
                                        // queue overflow: the kernel dropped events,
                                        // converge with a full rescan
                                        crate::trace::line("reconcile", || format!("{dir:?}"));
                                        let _ = cmd_tx_for_loop.send(Command::Rescan(dir));
                                    }
                                    watcher::WatchAction::DirGone { dir } => {
                                        crate::trace::line("reconcile", || format!("gone {dir:?}"));
                                        let _ = cmd_tx_for_loop.send(Command::Rescan(dir));
                                    }
                                    // named by the apply line above
                                    watcher::WatchAction::Ignored(_) => {}
                                }
                            }
                        }
                    }
                }
                #[cfg(any(target_os = "linux", windows))]
                if let Some(mut w) = watcher {
                    w.shutdown();
                }
            })
        };

        Engine {
            cmd_tx,
            events: ev_rx,
            show_hidden,
            epoch: epoch_shared,
            state,
            loop_join: Some(loop_join),
        }
    }

    /// Serializes the whole current listing in ONE read-lock pass
    /// (see DirectoryState::write_listing for the wire format).
    /// Returns (needed_bytes, file_count, dir_count, revision).
    ///
    /// The revision is read under the same lock as the bytes, so it names the
    /// listing this copy came from: a reader that compares two rows never has
    /// to wonder whether something moved between them. Sampling it separately
    /// would leave exactly that window, which is the bug this closes.
    pub fn export_listing(&self, buf: &mut [u8]) -> (usize, usize, usize, u64) {
        let st = self.state.read().unwrap_or_else(|p| p.into_inner());
        let (needed, files, dirs) = st.write_listing(buf);
        (needed, files, dirs, st.revision())
    }

    /// Most recently allocated open session, not the published session.
    pub fn epoch(&self) -> u64 {
        self.epoch.load(Ordering::Relaxed)
    }

    /// Allocates the session for an OpenDir the caller is about to send.
    /// It is the only place an epoch is
    /// born: the loop's own rescan (queue overflow, the watched folder deleted)
    /// re-answers the session that is already current and must not allocate here,
    /// because the stale-scan guard below asks "am I the newest" — so an internal
    /// bump silently cancelled every open that was in flight, without emitting a
    /// thing for the caller to notice.
    pub fn next_epoch(&self) -> u64 {
        self.epoch.fetch_add(1, Ordering::Relaxed) + 1
    }

    pub fn send(&self, cmd: Command) -> Result<(), EngineError> {
        self.cmd_tx.send(cmd).map_err(|_| EngineError::EngineGone)
    }

    pub fn events(&self) -> &Receiver<Event> {
        &self.events
    }

    pub fn try_event(&self) -> Option<Event> {
        self.events.try_recv().ok()
    }

    pub fn set_show_hidden(&self, show: bool) {
        self.show_hidden.store(show, Ordering::Relaxed);
    }

    pub fn supports_watching() -> bool {
        cfg!(any(target_os = "linux", windows))
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        // wakes the loop thread; it shuts workers and the watcher down
        let _ = self.cmd_tx.send(Command::Shutdown);
        // ...and wait for it. The C++ side calls the notify callback with a
        // pointer to the bridge itself, and ~EngineBridge frees this engine
        // before the bridge's destruction completes — queued notifications are
        // cancelled with the receiving QObject. Returning here while the loop
        // thread could still fire a callback would leave it posting through a
        // dangling pointer.
        if let Some(j) = self.loop_join.take() {
            let _ = j.join();
        }
    }
}
