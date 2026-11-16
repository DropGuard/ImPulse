//! The Windows watcher thread and its kernel machinery: one directory handle
//! with an overlapped ReadDirectoryChangesW, the completion event, the command
//! wake event, and the one thread that owns them. Nothing here runs under an
//! interpreter (real syscalls only). The decisions about what a record means
//! for the stream live in `super::pair` (the cookie pairing and the validation
//! drops) and `super::apply` (what an event means to the listing) — this file
//! only feeds them and sends what they yield.
//!
//! It mirrors `fd.rs` command for command: the same `WatchCmd` vocabulary and
//! the same one-thread ordering. The platform-neutral glue both shells serve
//! (the command vocabulary, the stream dispatch, the baseline enumeration)
//! lives once in the parent module; what differs here is only the machinery —
//! the directory handle, the overlapped read, the wait and the retire.
//!
//! Mapping — ReadDirectoryChangesW into the inotify record vocabulary the
//! pure layer already classifies:
//!
//! * ADDED / REMOVED / MODIFIED carry the IN_CREATE / IN_DELETE / IN_MODIFY
//!   bits. Attribute-only changes arrive as MODIFIED actions here (inotify
//!   would say IN_ATTRIB, which the pure layer drops) — `apply` answers them
//!   with a restat that ends in `MtimeUnchanged`, so the listing stays put.
//! * RENAMED_OLD_NAME + RENAMED_NEW_NAME arrive as the kernel's documented
//!   consecutive pair of one rename — the signal inotify gets from cookies.
//!   Consecutive halves share a synthesized cookie, so the Pairer folds them
//!   into the single `Renamed` record the listing understands; anything else
//!   keeps the inotify meaning: an OLD half alone is a removal (a rename out
//!   of the watch), a NEW half alone is an arrival (a rename in from
//!   outside). One corner is inherent to the channel: a rename out followed,
//!   in the same completion, by an unrelated rename in is indistinguishable
//!   from one rename (RDCW carries no move id) — the listing settles on the
//!   same set either way; only the event narrative can differ.
//! * A zero-byte completion, or ERROR_NOTIFY_ENUM_DIR, is the buffer
//!   overflow: it is reported as IN_Q_OVERFLOW (wd -1) so the engine
//!   reconciles with a full rescan, exactly as the Linux queue loss is.
//! * ERROR_ACCESS_DENIED on the completion is the watched directory going
//!   away: it is reported as IN_DELETE_SELF, which `apply` answers with
//!   `DirGone` and the engine turns into a rescan.
//! * RDCW does not say whether a subject is a directory (inotify sets
//!   IN_ISDIR); the is_dir bit is taken from one stat of the subject — the
//!   syscall shell acquiring the fact the kernel did not hand over, not a
//!   decision about it. A vanished subject reads as a file, which only rides
//!   the message: removal is by path.

use std::ffi::c_void;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;

use crossbeam_channel::{Receiver, Sender};

// the shared glue lives with the contract it serves: the command vocabulary,
// the stream dispatch, the baseline enumeration (see the watcher module doc)
use super::{dispatch, enumerate_and_report, WatchCmd};

// --- the Win32 surface this file is allowed to touch (bounded: no -sys crate
// --- — the whole kernel32 contract is the list below, reviewable at once) ---

type HANDLE = usize;

#[repr(C)]
struct Overlapped {
    internal: usize,
    internal_high: usize,
    offset: u32,
    offset_high: u32,
    event: HANDLE,
}

/// The notification buffer. FILE_NOTIFY_INFORMATION entries are DWORD-aligned
/// variable-length records; the wrapper guarantees the alignment the parse
/// relies on.
#[repr(align(4))]
struct NotifierBuffer([u8; 64 * 1024]);

const INVALID_HANDLE_VALUE: HANDLE = usize::MAX;
const FILE_LIST_DIRECTORY: u32 = 0x0001;
const FILE_SHARE_ALL: u32 = 0x1 | 0x2 | 0x4;
const OPEN_EXISTING: u32 = 3;
const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;
const FILE_FLAG_OVERLAPPED: u32 = 0x4000_0000;
/// file name, dir name, attributes, size, last write — the set whose inotify
/// counterparts the pure layer reacts to
const NOTIFY_FILTER: u32 = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;
const FILE_ACTION_ADDED: u32 = 1;
const FILE_ACTION_REMOVED: u32 = 2;
const FILE_ACTION_MODIFIED: u32 = 3;
const FILE_ACTION_RENAMED_OLD_NAME: u32 = 4;
const FILE_ACTION_RENAMED_NEW_NAME: u32 = 5;
const WAIT_OBJECT_0: u32 = 0;
const WAIT_TIMEOUT: u32 = 258;
const WAIT_FAILED: u32 = 0xFFFF_FFFF;
const ERROR_ACCESS_DENIED: u32 = 5;
const ERROR_IO_PENDING: u32 = 997;
const ERROR_OPERATION_ABORTED: u32 = 995;
const ERROR_NOTIFY_ENUM_DIR: u32 = 1022;

/// The inotify mask bits — the vocabulary `pair`/`apply` classify (inotify(7)).
/// Keeping the kernel's numeric meaning (and not a local enum) is what lets
/// the pure layers run unmodified over a Windows record stream.
mod mask {
    pub const MODIFY: u32 = 0x0000_0002;
    pub const MOVED_FROM: u32 = 0x0000_0040;
    pub const MOVED_TO: u32 = 0x0000_0080;
    pub const CREATE: u32 = 0x0000_0100;
    pub const DELETE: u32 = 0x0000_0200;
    pub const DELETE_SELF: u32 = 0x0000_0400;
    pub const Q_OVERFLOW: u32 = 0x0000_4000;
    pub const ISDIR: u32 = 0x4000_0000;
}

unsafe extern "system" {
    fn CreateEventW(
        attrs: *mut c_void,
        manual: i32,
        initial: i32,
        name: *const u16,
    ) -> HANDLE;
    fn SetEvent(event: HANDLE) -> i32;
    fn ResetEvent(event: HANDLE) -> i32;
    fn CloseHandle(object: HANDLE) -> i32;
    fn CreateFileW(
        name: *const u16,
        access: u32,
        share: u32,
        security: *mut c_void,
        disposition: u32,
        flags: u32,
        template: HANDLE,
    ) -> HANDLE;
    fn ReadDirectoryChangesW(
        dir: HANDLE,
        buffer: *mut c_void,
        len: u32,
        subtree: i32,
        filter: u32,
        returned: *mut u32,
        overlapped: *mut Overlapped,
        routine: *mut c_void,
    ) -> i32;
    fn GetOverlappedResult(
        file: HANDLE,
        overlapped: *mut Overlapped,
        transferred: *mut u32,
        wait: i32,
    ) -> i32;
    fn CancelIoEx(file: HANDLE, overlapped: *mut Overlapped) -> i32;
    fn WaitForMultipleObjects(
        count: u32,
        handles: *const HANDLE,
        wait_all: i32,
        millis: u32,
    ) -> u32;
}

/// One kernel event object, closed exactly once by ownership (the fd.rs
/// OwnedFd discipline — nothing releases a handle by hand).
struct OsEvent(HANDLE);
unsafe impl Send for OsEvent {}

impl OsEvent {
    fn new(manual: bool) -> Option<OsEvent> {
        let h = unsafe {
            CreateEventW(std::ptr::null_mut(), manual as i32, 0, std::ptr::null())
        };
        if h == 0 || h == INVALID_HANDLE_VALUE {
            None
        } else {
            Some(OsEvent(h))
        }
    }
    fn set(&self) {
        unsafe { SetEvent(self.0) };
    }
    fn reset(&self) {
        unsafe { ResetEvent(self.0) };
    }
    fn raw(&self) -> HANDLE {
        self.0
    }
}

impl Drop for OsEvent {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.0) };
    }
}

/// The live subscription: the directory handle, the folder it names, and the
/// generation id its records are tagged with (a retarget's stale events are
/// dropped by the pairing layer, exactly as a dead inotify wd's are).
struct Watch {
    dir: HANDLE,
    path: PathBuf,
    id: i32,
}

/// A handle to the watcher thread: sends commands, wakes the wait, joins on
/// shutdown. (Events are tagged with the generation they belong to, so
/// in-flight events for a previous target are dropped after a retarget.)
pub(crate) struct WatcherHandle {
    cmd_tx: Sender<WatchCmd>,
    wake: OsEvent,
    join: Option<JoinHandle<()>>,
    running: Arc<AtomicBool>,
}

impl WatcherHandle {
    pub(crate) fn spawn(
        options: crate::scan::ScanOptions,
        res_tx: Sender<crate::engine::ScanDone>,
    ) -> (WatcherHandle, Receiver<super::RawWatchEvent>) {
        let (cmd_tx, cmd_rx) = crossbeam_channel::unbounded::<WatchCmd>();
        let (ev_tx, ev_rx) = crossbeam_channel::unbounded::<super::RawWatchEvent>();
        let running = Arc::new(AtomicBool::new(true));
        let running_flag = Arc::clone(&running);
        // the wake event lets command sends interrupt the wait from any
        // thread; the loop borrows the raw handle value, the OsEvent field
        // owns the object — and Drop joins the loop before that close runs
        let wake = OsEvent::new(true).expect("create the watcher wake event");
        let wake_raw = wake.raw();

        let join = std::thread::Builder::new()
            .name("qimgv-watch".into())
            .spawn(move || {
                watcher_loop(&cmd_rx, &ev_tx, &res_tx, &options, wake_raw, running_flag)
            })
            .expect("spawn watcher thread");

        (
            WatcherHandle {
                cmd_tx,
                wake,
                join: Some(join),
                running,
            },
            ev_rx,
        )
    }

    pub(crate) fn send(&self, cmd: WatchCmd) {
        let _ = self.cmd_tx.send(cmd);
        self.wake.set();
    }

    /// Safe to call twice: this only re-sends a command and re-joins an
    /// already-joined thread. Nothing releases a handle by hand — `wake` is
    /// an OsEvent and closes once when the handle drops.
    pub(crate) fn shutdown(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        let _ = self.cmd_tx.send(WatchCmd::Stop);
        self.wake.set();
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

impl Drop for WatcherHandle {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Opens the directory for change notification: backup semantics (a directory
/// needs them to open at all), overlapped (the completion model the loop is
/// built on), full sharing so the apps being watched are never blocked.
fn open_dir(path: &Path) -> Result<HANDLE, std::io::Error> {
    let mut wide: Vec<u16> = path.as_os_str().encode_wide().collect();
    wide.push(0);
    let h = unsafe {
        CreateFileW(
            wide.as_ptr(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_ALL,
            std::ptr::null_mut(),
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            0,
        )
    };
    if h == INVALID_HANDLE_VALUE {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(h)
    }
}

/// Arms one overlapped read. The manual-reset completion event is cleared
/// HERE, immediately before the arm — a signal can then only mean "this read
/// completed", and an abort completion drained during a retire cannot leak
/// into the next wait (the race that would otherwise spin or kill the loop).
/// Overlapped reuse is legal at this point: the previous read on this handle
/// has completed (a completion was just processed) or was retired through
/// cancel plus a drained abort before the handle closed.
fn arm_read(
    dir: HANDLE,
    completion: HANDLE,
    buffer: &mut NotifierBuffer,
    overlapped: &mut Overlapped,
) -> bool {
    unsafe { ResetEvent(completion) };
    *overlapped = unsafe { std::mem::zeroed() };
    overlapped.event = completion;
    let mut unused: u32 = 0;
    let queued = unsafe {
        ReadDirectoryChangesW(
            dir,
            buffer.0.as_mut_ptr() as *mut c_void,
            buffer.0.len() as u32,
            0, // the watched directory itself, never the subtree
            NOTIFY_FILTER,
            &mut unused,
            overlapped,
            std::ptr::null_mut(),
        )
    };
    if queued != 0 {
        return true;
    }
    let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0) as u32;
    err == ERROR_IO_PENDING
}

/// Cancels any pending read, waits out its abort completion, then closes the
/// handle — in that order: closing (or re-arming) while a read is pending is
/// undefined, and the abort's completion would otherwise signal the shared
/// event after the next arm.
///
/// The drain is bounded, not a plain wait: a delete-pending directory can
/// hold the notify IRP past the cancel (the kernel completes it at handle
/// cleanup instead), and an unbounded GetOverlappedResult(wait=TRUE) there
/// wedged the watcher thread mid-retire — the engine's own rescan test hung
/// forever inside Engine::drop's join, because the thread it was joining was
/// the one waiting (ENGINE_RUST timeout, 2026-11-17). Closing with an
/// undrained read is safe here: this watch is retiring, nothing re-arms on
/// this overlapped, and a late abort signal lands on the manual-reset
/// completion event whose next consumer clears it (the unarmed-wait branch).
const RETIRE_DRAIN: std::time::Duration = std::time::Duration::from_millis(100);

fn drain_aborted(dir: HANDLE, overlapped: &mut Overlapped) {
    let deadline = std::time::Instant::now() + RETIRE_DRAIN;
    loop {
        let mut drained: u32 = 0;
        let done = unsafe { GetOverlappedResult(dir, overlapped, &mut drained, 0) };
        if done != 0 || std::time::Instant::now() >= deadline {
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
}

fn retire_watch(watch: Option<Watch>, overlapped: &mut Overlapped, armed: bool) {
    if let Some(w) = watch {
        if armed {
            unsafe { CancelIoEx(w.dir, std::ptr::null_mut()) };
            drain_aborted(w.dir, overlapped);
        }
        unsafe { CloseHandle(w.dir) };
    }
}

fn u32_at(buf: &[u8], off: usize) -> u32 {
    unsafe { core::ptr::read_unaligned(buf.as_ptr().add(off) as *const u32) }
}

/// Parses one completion buffer into records the Pairer already understands.
/// Names are relative to the watched directory (subtree is off, so they carry
/// no separators); the is_dir bit comes from one stat (see the module doc);
/// the rename pair's shared cookie is assigned only when the OLD half is the
/// record directly before the NEW one — the kernel's documented shape — and
/// every other ordering keeps the inotify meaning of its halves.
fn drain_records(
    buffer: &[u8],
    watch_id: i32,
    dir: &Path,
    next_cookie: &mut u32,
) -> Vec<super::Record> {
    // walk the variable-length FILE_NOTIFY_INFORMATION array
    let mut raw: Vec<(u32, String)> = Vec::new();
    let mut off: usize = 0;
    while off + 12 <= buffer.len() {
        let next = u32_at(buffer, off);
        let action = u32_at(buffer, off + 4);
        let name_len = u32_at(buffer, off + 8) as usize;
        if off + 12 + name_len > buffer.len() {
            break;
        }
        let units = name_len / 2;
        let name = if units == 0 {
            String::new()
        } else {
            // unaligned reads: the kernel aligns entries to DWORD in practice,
            // but the contract does not promise it, and a u16 slice built with
            // from_raw_parts would have to assume it
            let mut words = Vec::with_capacity(units);
            for k in 0..units {
                words.push(unsafe {
                    core::ptr::read_unaligned(
                        buffer.as_ptr().add(off + 12 + k * 2) as *const u16,
                    )
                });
            }
            String::from_utf16_lossy(&words)
        };
        raw.push((action, name));
        if next == 0 {
            break;
        }
        off += next as usize;
    }

    let mut records: Vec<super::Record> = Vec::with_capacity(raw.len());
    // (index of a seen OLD half, the cookie it was given)
    let mut held: Option<(usize, u32)> = None;
    for i in 0..raw.len() {
        let (action, name) = &raw[i];
        let bits = match *action {
            FILE_ACTION_ADDED => mask::CREATE,
            FILE_ACTION_REMOVED => mask::DELETE,
            FILE_ACTION_MODIFIED => mask::MODIFY,
            FILE_ACTION_RENAMED_OLD_NAME => mask::MOVED_FROM,
            FILE_ACTION_RENAMED_NEW_NAME => mask::MOVED_TO,
            // the kernel grew a vocabulary this mapping has not read —
            // dropping the record keeps the stream conservative
            _ => continue,
        };
        let cookie = if *action == FILE_ACTION_RENAMED_OLD_NAME {
            let c = *next_cookie;
            *next_cookie = next_cookie.wrapping_add(1);
            held = Some((i, c));
            c
        } else if *action == FILE_ACTION_RENAMED_NEW_NAME {
            match held.take() {
                Some((idx, c)) if idx + 1 == i => c,
                _ => {
                    let c = *next_cookie;
                    *next_cookie = next_cookie.wrapping_add(1);
                    c
                }
            }
        } else {
            0
        };
        let is_dir = std::fs::metadata(dir.join(name))
            .map(|m| m.is_dir())
            .unwrap_or(false);
        let bits = if is_dir { bits | mask::ISDIR } else { bits };
        records.push(super::Record {
            wd: watch_id,
            bits,
            cookie,
            name: Some(name.clone()),
        });
    }
    records
}

/// The watched directory is gone (deleted, or renamed away from under the
/// watch): retire the handle and report WatchGone through the pairing layer,
/// exactly as IN_DELETE_SELF does — the engine answers DirGone with its own
/// rescan. The record carries the watch's generation so the liveness check
/// passes. The pending read, if any, is cancelled and drained before the
/// close (reuse or close while pending is undefined).
fn retire_gone(
    watch: &mut Option<Watch>,
    overlapped: &mut Overlapped,
    ev_tx: &Sender<super::RawWatchEvent>,
    pairer: &mut super::Pairer,
) {
    let Some(w) = watch.take() else { return };
    let generation = w.id;
    let dir = w.path.clone();
    unsafe { CancelIoEx(w.dir, std::ptr::null_mut()) };
    drain_aborted(w.dir, overlapped);
    unsafe { CloseHandle(w.dir) };
    let live_wd = Some(generation);
    let rec = super::Record {
        wd: generation,
        bits: mask::DELETE_SELF,
        cookie: 0,
        name: None,
    };
    for out in pairer.intake(rec, Some(dir.as_path()), live_wd) {
        if !dispatch(ev_tx, out, live_wd) {
            return;
        }
    }
    for out in pairer.settle() {
        if !dispatch(ev_tx, out, live_wd) {
            return;
        }
    }
}

fn watcher_loop(
    cmd_rx: &Receiver<WatchCmd>,
    ev_tx: &Sender<super::RawWatchEvent>,
    res_tx: &Sender<crate::engine::ScanDone>,
    options: &crate::scan::ScanOptions,
    wake: HANDLE,
    running: Arc<AtomicBool>,
) {
    // manual-reset: the reset discipline lives in arm_read (cleared only
    // immediately before an arm), so a signal always names a real completion
    let completion = match OsEvent::new(true) {
        Some(e) => e,
        None => {
            log::warn!("watcher: create completion event failed");
            return;
        }
    };
    let mut pairer = super::Pairer::new();
    let mut watch: Option<Watch> = None;
    let mut armed = false;
    let mut next_id: i32 = 0;
    let mut next_cookie: u32 = 0;
    let mut buffer = NotifierBuffer([0u8; 64 * 1024]);
    let mut overlapped: Overlapped = unsafe { std::mem::zeroed() };

    while running.load(Ordering::Relaxed) {
        let handles = [completion.raw(), wake];
        let waited = unsafe { WaitForMultipleObjects(2, handles.as_ptr(), 0, 500) };
        if waited == WAIT_FAILED {
            log::warn!("watcher: wait failed: {}", std::io::Error::last_os_error());
            return;
        }

        // The completion is drained before any command is applied — the fd.rs
        // discipline: a backlog still belongs to the watch it was armed for,
        // so the generation tag stays honest across a retarget.
        if waited == WAIT_OBJECT_0 {
            if armed {
                armed = false;
                let dir_handle = watch
                    .as_ref()
                    .map(|w| w.dir)
                    .unwrap_or(0);
                let mut transferred: u32 = 0;
                let completed = unsafe {
                    GetOverlappedResult(dir_handle, &mut overlapped, &mut transferred, 0)
                };
                let live_wd = watch.as_ref().map(|w| w.id);
                let dir_opt: Option<&Path> = watch.as_ref().map(|w| w.path.as_path());
                if completed == 0 {
                    let err =
                        std::io::Error::last_os_error().raw_os_error().unwrap_or(0) as u32;
                    match err {
                        // a cancel of our own (retarget or shutdown); its
                        // completion is normally drained inside retire_watch,
                        // and there is no verdict about the directory here
                        ERROR_OPERATION_ABORTED => {}
                        // the kernel could not deliver a consistent stream:
                        // the buffer overflowed, everything in between is lost
                        ERROR_NOTIFY_ENUM_DIR => {
                            let rec = super::Record {
                                wd: -1,
                                bits: mask::Q_OVERFLOW,
                                cookie: 0,
                                name: None,
                            };
                            for out in pairer.intake(rec, dir_opt, live_wd) {
                                if !dispatch(ev_tx, out, live_wd) {
                                    return;
                                }
                            }
                        }
                        // the watched directory itself went away
                        ERROR_ACCESS_DENIED => {
                            retire_gone(&mut watch, &mut overlapped, ev_tx, &mut pairer);
                        }
                        _ => {
                            log::warn!(
                                "watcher: read failed: {}",
                                std::io::Error::last_os_error()
                            );
                            return;
                        }
                    }
                } else if transferred == 0 {
                    // zero bytes is the other documented overflow shape
                    let rec = super::Record {
                        wd: -1,
                        bits: mask::Q_OVERFLOW,
                        cookie: 0,
                        name: None,
                    };
                    for out in pairer.intake(rec, dir_opt, live_wd) {
                        if !dispatch(ev_tx, out, live_wd) {
                            return;
                        }
                    }
                } else {
                    let w = watch
                        .as_ref()
                        .expect("a completed read implies a live watch");
                    for rec in drain_records(&buffer.0, w.id, &w.path, &mut next_cookie) {
                        for out in pairer.intake(rec, dir_opt, live_wd) {
                            if !dispatch(ev_tx, out, live_wd) {
                                return;
                            }
                        }
                    }
                }
                // one settle per completed read (the batch ended — a held
                // rename half never had its pair beside it)
                for out in pairer.settle() {
                    if !dispatch(ev_tx, out, live_wd) {
                        return;
                    }
                }
                if let Some(w) = &watch {
                    armed = arm_read(w.dir, completion.raw(), &mut buffer, &mut overlapped);
                }
            } else {
                // an unarmed wait can only see a stale signal (an abort
                // drained during a retire); clear it so the loop cannot spin
                completion.reset();
            }
        }

        if waited == WAIT_TIMEOUT {
            // RDCW's pending read does not complete when the watched directory
            // is deleted or renamed away — the open handle holds it in
            // pending-delete and the read never signals. The half-second beat
            // (the same beat fd.rs's poll timeout keeps) is what notices the
            // loss: IN_DELETE_SELF's Windows answer, fed through the same
            // pairing layer so the engine's DirGone → rescan answer runs
            // unchanged.
            if watch.as_ref().is_some_and(|w| !w.path.exists()) {
                retire_gone(&mut watch, &mut overlapped, ev_tx, &mut pairer);
            }
        }

        if waited == WAIT_OBJECT_0 + 1 {
            // commands: reset the wake, then drain the channel
            unsafe { ResetEvent(wake) };
            while let Ok(cmd) = cmd_rx.try_recv() {
                match cmd {
                    WatchCmd::Stop => {
                        retire_watch(watch.take(), &mut overlapped, armed);
                        return;
                    }
                    WatchCmd::Enumerate { path, session, dispatch } => {
                        enumerate_and_report(&path, session, dispatch, res_tx, options);
                    }
                    WatchCmd::Unwatch => {
                        retire_watch(watch.take(), &mut overlapped, armed);
                        armed = false;
                    }
                    WatchCmd::Watch { path, session, dispatch } => {
                        retire_watch(watch.take(), &mut overlapped, armed);
                        armed = false;
                        match open_dir(&path) {
                            Ok(dir) => {
                                next_id += 1;
                                let w = Watch { dir, path: path.clone(), id: next_id };
                                if arm_read(
                                    w.dir,
                                    completion.raw(),
                                    &mut buffer,
                                    &mut overlapped,
                                ) {
                                    armed = true;
                                    watch = Some(w);
                                } else {
                                    log::warn!(
                                        "watcher: arm read on {:?}: {}",
                                        path,
                                        std::io::Error::last_os_error()
                                    );
                                    unsafe { CloseHandle(w.dir) };
                                }
                            }
                            Err(e) => {
                                log::warn!("watcher: open {:?}: {e}", path);
                            }
                        }
                        // The baseline is taken here, by this thread, after
                        // the subscription has been installed (or after
                        // installing it failed, in which case the listing
                        // still works and live updates do not). No other
                        // thread can enumerate this folder, so a change cannot
                        // land in a gap between the two acts: it is either in
                        // the snapshot or it arrives on the notification
                        // queue afterwards.
                        enumerate_and_report(&path, session, dispatch, res_tx, options);
                    }
                }
            }
        }
    }
}
