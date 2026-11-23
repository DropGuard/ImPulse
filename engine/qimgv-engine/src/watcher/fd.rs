//! The watcher thread and its file-descriptor machinery: the inotify fd, the
//! eventfd wake pipe, poll, and the one thread that owns them. Nothing here
//! runs under an interpreter (real syscalls only). The decisions about what a
//! record means for the stream live in `super::pair` (the cookie pairing and
//! the validation drops) and `super::apply` (what an event means to the
//! listing) — this file only feeds them and sends what they yield.

use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;

use crossbeam_channel::{Receiver, Sender};
use inotify::{Inotify, WatchMask};

// the shared glue lives with the contract it serves: the command vocabulary,
// the stream dispatch, the baseline enumeration (see the watcher module doc)
use super::{WatchCmd, dispatch, enumerate_and_report};

/// A handle to the watcher thread: sends commands, wakes the poll, joins on
/// shutdown. (Events are tagged with the generation they belong to, so
/// in-flight events for a previous target are dropped after a retarget.)
pub(crate) struct WatcherHandle {
    cmd_tx: Sender<WatchCmd>,
    wake: OwnedFd,
    join: Option<JoinHandle<()>>,
    running: Arc<AtomicBool>,
}

impl WatcherHandle {
    pub(crate) fn spawn(
        options: crate::scan::ScanOptions,
    ) -> (WatcherHandle, Receiver<super::WatchMessage>) {
        let (cmd_tx, cmd_rx) = crossbeam_channel::unbounded::<WatchCmd>();
        let (ev_tx, ev_rx) = crossbeam_channel::unbounded::<super::WatchMessage>();
        let running = Arc::new(AtomicBool::new(true));
        let running_flag = Arc::clone(&running);

        // wake fd lets command sends interrupt the poll from any thread
        let wake = new_eventfd();
        // the loop gets its own descriptor onto the same eventfd: each end closes
        // exactly once by ownership, so no fd number is ever released twice
        let loop_wake = wake.try_clone().expect("dup the watcher wake fd");

        let join = std::thread::Builder::new()
            .name("qimgv-watch".into())
            .spawn(move || watcher_loop(&cmd_rx, &ev_tx, &options, loop_wake, running_flag))
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
        let one: u64 = 1;
        // SAFETY: `one` is a live local u64, so the pointer is valid for the 8
        // bytes written; `self.wake` is an eventfd owned by this handle, open
        // for as long as `self` is. The result is deliberately ignored: the fd
        // is non-blocking, and EAGAIN means the counter is already non-zero --
        // a wake is pending, which is all this write is for.
        unsafe {
            libc::write(
                self.wake.as_raw_fd(),
                &one as *const u64 as *const libc::c_void,
                8,
            );
        }
    }

    /// Safe to call twice, because the engine loop shuts the watcher down and
    /// then drops it: this only re-sends a command and re-joins an already-joined
    /// thread. Nothing releases a descriptor by hand anymore — `wake` is an
    /// OwnedFd and closes once when the handle drops.
    pub(crate) fn shutdown(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        let _ = self.cmd_tx.send(WatchCmd::Stop);
        let one: u64 = 1;
        // SAFETY: same as in `send`: a live local u64 written to the eventfd
        // this handle owns; a failed write means a wake is already pending.
        unsafe {
            libc::write(
                self.wake.as_raw_fd(),
                &one as *const u64 as *const libc::c_void,
                8,
            );
        }
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

fn new_eventfd() -> OwnedFd {
    // CLOEXEC so the wake pipe cannot leak into a process the app spawns
    // SAFETY: eventfd takes no pointers; the return value is checked below.
    let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    assert!(fd >= 0, "eventfd for the watcher wake pipe failed");
    // SAFETY: `fd` was just returned by eventfd and checked non-negative, so
    // it is an open descriptor that nothing else owns; OwnedFd takes the sole
    // ownership and closes it exactly once.
    unsafe { OwnedFd::from_raw_fd(fd) }
}

impl Drop for WatcherHandle {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn watcher_loop(
    cmd_rx: &Receiver<WatchCmd>,
    ev_tx: &Sender<super::WatchMessage>,
    options: &crate::scan::ScanOptions,
    wake: OwnedFd,
    running: Arc<AtomicBool>,
) {
    let mut inotify = match Inotify::init() {
        Ok(i) => i,
        Err(e) => {
            log::warn!("watcher: init failed: {e}");
            return;
        }
    };
    let mut session = 0;
    let mut watch: Option<inotify::WatchDescriptor> = None;
    // the directory `watch` was added for; every event this thread emits names it
    let mut watch_dir: PathBuf = PathBuf::new();
    let mut buffer = vec![0u8; 64 * 1024];
    // MOVED_FROM and MOVED_TO share an inotify cookie, and that pairing is the
    // only thing that can report a rename as one event instead of Removed+Added.
    // The cookie is visible only to the pairing layer — `apply_event` cannot
    // see it, because a record's subject is one path and a rename has two — so
    // that layer decides *whether* the pair is a rename and leaves *what it
    // means* to the listing: a rename to a non-member name, or from outside
    // the watch, is answered by apply_event exactly as an unpaired half is.
    let mut pairer = super::Pairer::new();

    while running.load(Ordering::Relaxed) {
        let mut fds = [
            libc::pollfd {
                fd: inotify.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: wake.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        // SAFETY: `fds` is a live, exclusively borrowed array of exactly the 2
        // pollfd entries the count argument states.
        let ready = unsafe { libc::poll(fds.as_mut_ptr(), 2, 500) };
        if ready < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() != Some(libc::EINTR) {
                log::warn!("watcher: poll failed: {err}");
                return;
            }
            continue;
        }
        if ready == 0 {
            continue;
        }

        // The fd is drained before any command is applied. Both orderings are
        // safe once events carry their own directory, and this one keeps the
        // generation tag honest: a backlog from the outgoing watch is still
        // tagged with the outgoing generation instead of the incoming one.
        if fds[0].revents & libc::POLLIN != 0 {
            match inotify.read_events(&mut buffer) {
                Ok(events) => {
                    let live_wd = watch.as_ref().map(|w| w.get_watch_descriptor_id());
                    let dir: Option<&Path> = if watch_dir.as_os_str().is_empty() {
                        None
                    } else {
                        Some(watch_dir.as_path())
                    };
                    for ev in events {
                        let rec = super::Record {
                            wd: ev.wd.get_watch_descriptor_id(),
                            bits: ev.mask.bits(),
                            cookie: ev.cookie,
                            name: ev.name.map(|c| c.to_string_lossy().into_owned()),
                        };
                        for out in pairer.intake(rec, dir, live_wd) {
                            if !dispatch(ev_tx, out, live_wd, session) {
                                return;
                            }
                        }
                    }
                    // The batch ended without the other half, if one was held.
                    for out in pairer.settle() {
                        if !dispatch(ev_tx, out, live_wd, session) {
                            return;
                        }
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
                Err(e) => {
                    log::warn!("watcher: read failed: {e}");
                    return;
                }
            }
        }

        if fds[1].revents & libc::POLLIN != 0 {
            // commands: drain the wake counter, then the channel
            let mut buf = [0u8; 8];
            // SAFETY: `buf` is a live 8-byte buffer and the length passed is 8;
            // `wake` is the eventfd this thread holds for the whole loop. The
            // loop ends on the first short or failed read (EAGAIN once the
            // counter is drained).
            unsafe {
                while libc::read(wake.as_raw_fd(), buf.as_mut_ptr().cast(), 8) as isize == 8 {}
            }
            while let Ok(cmd) = cmd_rx.try_recv() {
                match cmd {
                    WatchCmd::Stop => return,
                    WatchCmd::Enumerate {
                        path,
                        epoch,
                        dispatch,
                        install,
                    } => {
                        if install {
                            session = epoch;
                            if let Some(w) = watch.take() {
                                let _ = inotify.watches().remove(w);
                            }
                            match inotify.watches().add(
                                &path,
                                WatchMask::CREATE
                                    | WatchMask::MODIFY
                                    | WatchMask::DELETE
                                    | WatchMask::MOVED_FROM
                                    | WatchMask::MOVED_TO
                                    | WatchMask::DELETE_SELF
                                    | WatchMask::MOVE_SELF,
                            ) {
                                Ok(wd) => {
                                    watch = Some(wd);
                                    watch_dir = path.clone();
                                }
                                Err(e) => {
                                    log::warn!("watcher: add_watch {:?}: {e}", path);
                                    watch = None;
                                    watch_dir = PathBuf::new();
                                }
                            }
                        }
                        // The baseline is taken here, by this thread, after the
                        // subscription has been installed (or after installing it
                        // failed, in which case the listing still works and live
                        // updates do not — visible in the warning above). No other
                        // thread can enumerate this folder, so a change cannot land
                        // in a gap between the two acts: it is either in the snapshot
                        // or it arrives on the event queue afterwards.
                        enumerate_and_report(&path, epoch, dispatch, ev_tx, options);
                    }
                    WatchCmd::Unwatch => {
                        if let Some(w) = watch.take() {
                            let _ = inotify.watches().remove(w);
                        }
                        watch_dir = PathBuf::new();
                    }
                }
            }
        }
    }
}
