//! C ABI for qimgv-engine.
//!
//! Discipline (agreed in stack discussion):
//! - No panic ever crosses the boundary: every entry point is wrapped in
//!   `catch_unwind` and folds panics into `QIMGV_ERR_PANIC`.
//! - Strings cross as NUL-terminated UTF-8 into caller-provided buffers;
//!   `QIMGV_ERR_TRUNCATED` signals an undersized buffer.
//! - All calls must come from a single thread (the Qt main thread).
//!
//! The normative safety contract for every exported function is
//! `include/qimgv_engine.h` (read by the C++ consumer); it is intentionally
//! not duplicated per-function here.
#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use qimgv_engine::{
    Command, Engine, EngineConfig, EngineError, Event, NameFilter, Notify, SortMode,
};

// --- status codes (mirror include/qimgv_engine.h) ---------------------------
pub const QIMGV_OK: c_int = 0;
pub const QIMGV_ERR_IO: c_int = 1;
pub const QIMGV_ERR_ENGINE_GONE: c_int = 2;
pub const QIMGV_ERR_INVALID: c_int = 3;
pub const QIMGV_ERR_PANIC: c_int = 4;
pub const QIMGV_ERR_EMPTY: c_int = 5;
pub const QIMGV_ERR_TRUNCATED: c_int = 6;
pub const QIMGV_ERR_BAD_SORT: c_int = 7;
pub const QIMGV_ERR_BAD_UTF8: c_int = 8;

// --- event kinds ------------------------------------------------------------
pub const QIMGV_EVENT_NONE: c_int = 0;
pub const QIMGV_EVENT_DIR_LISTED: c_int = 1;
pub const QIMGV_EVENT_OPEN_FAILED: c_int = 2;
pub const QIMGV_EVENT_DIR_MUTATED: c_int = 3;
pub const QIMGV_EVENT_SORT_CHANGED: c_int = 4;

// --- sort modes (the sort_mode arguments) -----------------------------------
// The two this library orders names by. The size and time modes need entry
// metadata, which qimgv_engine_compare_names does not take.
pub const QIMGV_SORT_NAME: c_int = 0;
pub const QIMGV_SORT_NAME_DESC: c_int = 1;

// --- mutation kinds (qimgv_mutation.kind) -----------------------------------
pub const QIMGV_MUT_ADDED: c_int = 1;
pub const QIMGV_MUT_MODIFIED: c_int = 2;
pub const QIMGV_MUT_REMOVED: c_int = 3;
// One entry, a new path. `subject_buf` holds the path it has now; the path it
// had is the event's second subject and rides in `old_buf` (see take_event).
pub const QIMGV_MUT_RENAMED: c_int = 4;

// --- image formats (mirror meta::ImageFormat) -------------------------------
pub const QIMGV_FMT_UNKNOWN: c_int = 0;
pub const QIMGV_FMT_JPEG: c_int = 1;
pub const QIMGV_FMT_PNG: c_int = 2;
pub const QIMGV_FMT_GIF: c_int = 3;
pub const QIMGV_FMT_WEBP: c_int = 4;
pub const QIMGV_FMT_AVIF: c_int = 5;
pub const QIMGV_FMT_JXL: c_int = 6;
pub const QIMGV_FMT_BMP: c_int = 7;
pub const QIMGV_FMT_TIFF: c_int = 8;

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

fn set_last_error(msg: &str) {
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new(msg).ok();
    });
}

fn set_last_error_by_code(code: c_int) {
    let msg = match code {
        QIMGV_ERR_IO => "I/O error",
        QIMGV_ERR_ENGINE_GONE => "engine thread is gone",
        QIMGV_ERR_INVALID => "invalid argument",
        QIMGV_ERR_EMPTY => "no such entry or nothing pending",
        QIMGV_ERR_TRUNCATED => "output buffer too small",
        QIMGV_ERR_BAD_SORT => "unknown sort mode",
        QIMGV_ERR_BAD_UTF8 => "input is not valid UTF-8",
        _ => "unknown error",
    };
    set_last_error(msg);
}

#[repr(C)]
pub struct QimgvEntryMeta {
    pub size: u64,
    pub mtime_ms: i64,
    pub is_dir: c_int,
}

#[repr(C)]
pub struct QimgvEvent {
    pub kind: c_int,
    pub code: c_int,
    /// Session the event belongs to; open_dir hands out the session id and
    /// consumers drop events from older sessions.
    pub epoch: u64,
    /// For DIR_MUTATED: the engine's own ordinal of this mutation, assigned
    /// before the event could be dropped anywhere downstream. 0 for the other
    /// kinds. It carries no authority — nothing compares it, and a listing does
    /// not have to be the same size at the same number — it exists so a line
    /// printed by the engine and a line printed by the C++ mirror can be
    /// matched by a human reading both.
    pub seq: u64,
    /// Diagnostic version at emission. A snapshot consumer does not compare it
    /// against its rows: a pull can already include more than the notice that
    /// woke it, so the export's own revision is what the mirror stores. This
    /// field exists to name the commit a trace line came from.
    pub revision: u64,
}

#[repr(C)]
pub struct QimgvProbeResult {
    pub format: c_int,
    pub animated: c_int,
    pub orientation: c_int, // EXIF 1..=8; 1 = normal/unknown
    pub width: c_int,       // 0 = unknown
    pub height: c_int,
}

#[repr(C)]
pub struct QimgvMutation {
    pub kind: c_int, // QIMGV_MUT_*
    pub is_dir: c_int,
    pub size: u64,
    pub mtime_ms: i64,
    /// The entry's position in the engine's own order at the time of the
    /// event: where an Added entry was inserted, where a Modified entry sits
    /// after its metadata re-sort, where a Removed entry lived. Diagnostic
    /// only — it does not index the mirror or a later snapshot.
    pub index: c_int,
}

// ABI layout pins. The C header asserts the same numbers
// (qimgv_engine.h, "Compile-time layout pinning") and tests/architecture_rules.py
// R5 fails if the two literals disagree. This side needs its own assert because
// #[repr(C)] does not freeze a declaration: reorder two fields or widen one and
// everything still compiles here, the symbol names still match, and the consumer
// reads arguments from the wrong bytes.
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<QimgvEntryMeta>() == 24);
    assert!(offset_of!(QimgvEntryMeta, size) == 0);
    assert!(offset_of!(QimgvEntryMeta, mtime_ms) == 8);
    assert!(offset_of!(QimgvEntryMeta, is_dir) == 16);
    assert!(size_of::<QimgvEvent>() == 32);
    assert!(offset_of!(QimgvEvent, kind) == 0);
    assert!(offset_of!(QimgvEvent, code) == 4);
    assert!(offset_of!(QimgvEvent, epoch) == 8);
    assert!(offset_of!(QimgvEvent, seq) == 16);
    assert!(offset_of!(QimgvEvent, revision) == 24);
    assert!(size_of::<QimgvMutation>() == 32);
    assert!(offset_of!(QimgvMutation, kind) == 0);
    assert!(offset_of!(QimgvMutation, is_dir) == 4);
    assert!(offset_of!(QimgvMutation, size) == 8);
    assert!(offset_of!(QimgvMutation, mtime_ms) == 16);
    assert!(offset_of!(QimgvMutation, index) == 24);
    assert!(size_of::<QimgvProbeResult>() == 20);
    assert!(offset_of!(QimgvProbeResult, format) == 0);
    assert!(offset_of!(QimgvProbeResult, animated) == 4);
    assert!(offset_of!(QimgvProbeResult, orientation) == 8);
    assert!(offset_of!(QimgvProbeResult, width) == 12);
    assert!(offset_of!(QimgvProbeResult, height) == 16);
};

/// Raw pointers are not Send; the notify callback ships one to engine threads.
struct SendPtr(*mut c_void);

impl SendPtr {
    // Method (not field) access so the notify closure captures the whole
    // wrapper — a bare `user.0` projection would capture the raw pointer
    // itself, which is neither Send nor Sync.
    fn get(&self) -> *mut c_void {
        self.0
    }
}

// SAFETY: the pointer is an opaque token this crate never dereferences; it is
// only handed back to the notify callback. Moving it across threads is sound
// because the C++ side's contract is that the callback and the `user` pointer
// it receives may be used from any thread (the callback only wakes the GUI).
unsafe impl Send for SendPtr {}
// SAFETY: shared access is only `get`, which copies the address out; there is
// no interior state to race on. Same caller contract as the Send impl above.
unsafe impl Sync for SendPtr {}

type NotifyCb = extern "C" fn(user: *mut c_void);

pub struct QimgvEngine {
    engine: Mutex<Engine>,
    /// One event that the caller's buffer could not hold, kept for the next
    /// call. See qimgv_engine_take_event: an event that is popped and then lost
    /// is indistinguishable from one that never happened.
    stashed: Mutex<Option<Event>>,
}

fn map_engine_error(e: &EngineError) -> c_int {
    match e {
        EngineError::Io { .. } => QIMGV_ERR_IO,
        EngineError::EngineGone => QIMGV_ERR_ENGINE_GONE,
        EngineError::BadSortMode(_) => QIMGV_ERR_BAD_SORT,
    }
}

// --- minimal, audited unsafe blocks -----------------------------------------

/// # Safety
/// `ptr` must point to a valid NUL-terminated string for the duration of the call.
fn read_path(ptr: *const c_char) -> Result<PathBuf, c_int> {
    if ptr.is_null() {
        return Err(QIMGV_ERR_INVALID);
    }
    // SAFETY: `ptr` was checked non-null just above; the function's contract
    // (see # Safety) is that it points to a NUL-terminated string that stays
    // valid for the duration of this call. The borrow does not outlive it:
    // the result is copied into an owned PathBuf.
    let s = unsafe { CStr::from_ptr(ptr) };
    match s.to_str() {
        Ok(s) if !s.is_empty() => Ok(PathBuf::from(s)),
        Ok(_) => Err(QIMGV_ERR_INVALID),
        Err(_) => Err(QIMGV_ERR_BAD_UTF8),
    }
}

fn write_str(buf: *mut c_char, cap: usize, s: &str) -> Result<(), c_int> {
    fits(buf, cap, s)?;
    let bytes = s.as_bytes();
    // SAFETY: `fits` returned Ok, so `buf` is non-null and `cap` is at least
    // bytes.len() + 1. The caller guarantees `buf` is writable for `cap`
    // bytes, so both the copy and the terminator write (at offset
    // bytes.len() < cap) stay inside the buffer. `bytes` is a Rust slice, so
    // it cannot overlap the caller's C buffer.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr().cast::<c_char>(), buf, bytes.len());
        buf.add(bytes.len()).write(0);
    }
    Ok(())
}

/// The checks `write_str` makes, without the copy. take_event promises that a
/// refused event "has written nothing", so a call site that needs two buffers
/// validates both before touching the first one.
fn fits(buf: *mut c_char, cap: usize, s: &str) -> Result<(), c_int> {
    if buf.is_null() {
        return Err(QIMGV_ERR_INVALID);
    }
    if s.len() + 1 > cap {
        return Err(QIMGV_ERR_TRUNCATED);
    }
    Ok(())
}

/// # Safety
/// `engine` must be a live pointer returned by `qimgv_engine_new`, not yet freed.
fn with_engine<T>(engine: *mut QimgvEngine, f: impl FnOnce(&Engine) -> T) -> T {
    // SAFETY: the function's contract (see # Safety) is a live pointer from
    // qimgv_engine_new that has not been freed. Only a shared reference is
    // taken, and the engine's mutable state is behind its own locks.
    let eng = unsafe { &*engine };
    let guard = eng.engine.lock().unwrap_or_else(|p| p.into_inner());
    f(&guard)
}

fn mtime_ms(t: &SystemTime) -> i64 {
    t.duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

// --- lifecycle --------------------------------------------------------------

/// `formats_extensions_utf8`: '\n'-joined extension whitelist (e.g. "jpg\npng"),
/// or NULL for "accept everything". Sourced from Settings::supportedFormats().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_new(
    notify: Option<NotifyCb>,
    user: *mut c_void,
    formats_extensions_utf8: *const c_char,
) -> *mut QimgvEngine {
    let attempt = || -> Result<QimgvEngine, String> {
        let filter = if formats_extensions_utf8.is_null() {
            // documented as "NULL to accept everything" in qimgv_engine.h
            NameFilter::from_extensions(std::iter::empty::<&str>())
        } else {
            // SAFETY: the branch above checked non-null; the caller guarantees
            // a NUL-terminated string valid for this call, and it is copied
            // into owned data (NameFilter) before the call returns.
            let joined = unsafe { CStr::from_ptr(formats_extensions_utf8) }
                .to_str()
                .map_err(|_| "extension list is not valid UTF-8".to_string())?;
            NameFilter::from_extensions(joined.split('\n'))
        };
        let notify_fn: Notify = match notify {
            Some(cb) => {
                let user = SendPtr(user);
                Arc::new(move || cb(user.get()))
            }
            None => Arc::new(|| {}),
        };
        let engine = Engine::spawn(
            EngineConfig {
                show_hidden: Arc::new(AtomicBool::new(false)),
                filter,
                initial_sort: SortMode::Name,
            },
            notify_fn,
        );
        Ok(QimgvEngine {
            engine: Mutex::new(engine),
            stashed: Mutex::new(None),
        })
    };

    match catch_unwind(attempt) {
        Ok(Ok(engine)) => Box::into_raw(Box::new(engine)),
        Ok(Err(msg)) => {
            set_last_error(&msg);
            std::ptr::null_mut()
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_new");
            std::ptr::null_mut()
        }
    }
}

/// Destroys the engine and joins its threads. NULL is tolerated and does
/// /// nothing. A panic inside is contained and reported via qimgv_last_error()
/// /// rather than unwinding across the boundary.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_free(engine: *mut QimgvEngine) {
    if engine.is_null() {
        return;
    }
    // SAFETY: `engine` is non-null (checked above) and, by the caller's
    // contract, came from qimgv_engine_new and is freed exactly once: the
    // Box was created by Box::into_raw in that function, so from_raw restores
    // the same allocation. No other call may use the pointer afterwards.
    if catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(engine)) })).is_err() {
        set_last_error("panic in qimgv_engine_free");
    }
}

/// The engine crate's semantic version, as a static NUL-terminated string
/// /// valid for the program's lifetime.
#[unsafe(no_mangle)]
pub extern "C" fn qimgv_engine_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr().cast()
}

#[unsafe(no_mangle)]
pub extern "C" fn qimgv_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| {
        slot.borrow()
            .as_ref()
            .map(|s| s.as_ptr())
            .unwrap_or(std::ptr::null())
    })
}

// --- commands ---------------------------------------------------------------

/// Scans `path` and makes it the current directory. `sort_mode`: QIMGV_SORT_*
/// /// applied atomically with the scan, or -1 to keep the current sort.
/// /// `epoch_out` (nullable) receives the session epoch — events with a smaller
/// /// epoch belong to older sessions and must be dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_open_dir(
    engine: *mut QimgvEngine,
    path: *const c_char,
    sort_mode: c_int,
    epoch_out: *mut u64,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let path = read_path(path)?;
        let sort = if sort_mode < 0 {
            None // keep the engine's current sort
        } else {
            Some(SortMode::try_from(sort_mode).map_err(|_| QIMGV_ERR_BAD_SORT)?)
        };
        with_engine(engine, |e| {
            let epoch = e.next_epoch();
            e.send(Command::OpenDir { path, sort, epoch })
                .map(|_| {
                    if !epoch_out.is_null() {
                        // SAFETY: non-null checked on the line above; the
                        // caller guarantees a writable u64.
                        unsafe { *epoch_out = epoch };
                    }
                    QIMGV_OK
                })
                .map_err(|er| map_engine_error(&er))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_open_dir");
            QIMGV_ERR_PANIC
        }
    }
}

/// Name order, as the engine sorts listings. Pure and thread-safe: it touches
/// no engine instance, so a caller outside this library can sort by the same
/// rule instead of approximating it with a collator that disagrees on ties.
///
/// Returns <0, 0 or >0 like strcmp. The third argument picks the mode: 0 is
/// Name, 1 is NameDesc (the only two QIMGV_SORT_* codes whose order this
/// answers — the size and time modes need metadata this function does not take).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_compare_names(
    a: *const c_char,
    b: *const c_char,
    sort_mode: c_int,
) -> c_int {
    let attempt = || -> c_int {
        let Ok(a) = read_path(a) else { return 0 };
        let Ok(b) = read_path(b) else { return 0 };
        let name_a = a.to_string_lossy();
        let name_b = b.to_string_lossy();
        let ord = if sort_mode == QIMGV_SORT_NAME_DESC {
            qimgv_engine::natural(&name_b, &name_a)
        } else {
            qimgv_engine::natural(&name_a, &name_b)
        };
        match ord {
            std::cmp::Ordering::Less => -1,
            std::cmp::Ordering::Equal => 0,
            std::cmp::Ordering::Greater => 1,
        }
    };
    catch_unwind(AssertUnwindSafe(attempt)).unwrap_or(0)
}

/// Re-sorts the current listing by QIMGV_SORT_*; an out-of-range mode is
/// /// QIMGV_ERR_BAD_SORT.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_set_sort(engine: *mut QimgvEngine, mode: c_int) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let mode = SortMode::try_from(mode).map_err(|_| QIMGV_ERR_BAD_SORT)?;
        with_engine(engine, |e| {
            e.send(Command::SetSort(mode))
                .map(|_| QIMGV_OK)
                .map_err(|e| map_engine_error(&e))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_set_sort");
            QIMGV_ERR_PANIC
        }
    }
}

/// Toggles hidden-entry visibility (nonzero = show); the listing rescans when
/// /// this changes anything.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_set_show_hidden(
    engine: *mut QimgvEngine,
    show: c_int,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        with_engine(engine, |e| {
            e.set_show_hidden(show != 0);
            Ok(QIMGV_OK)
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_set_show_hidden");
            QIMGV_ERR_PANIC
        }
    }
}

// --- state reads ------------------------------------------------------------

fn count(engine: *mut QimgvEngine, dirs: bool) -> c_int {
    if engine.is_null() {
        set_last_error("null engine");
        return -1;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        with_engine(engine, |e| {
            let st = e.state.read().unwrap_or_else(|p| p.into_inner());
            (if dirs {
                st.dir_count()
            } else {
                st.file_count()
            }) as c_int
        })
    }));
    match r {
        Ok(n) => n,
        Err(_) => {
            set_last_error("engine panic");
            -1
        }
    }
}

/// The current file count, or -1 on a null engine or a panic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_file_count(engine: *mut QimgvEngine) -> c_int {
    count(engine, false)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_dir_count(engine: *mut QimgvEngine) -> c_int {
    count(engine, true)
}

#[allow(clippy::too_many_arguments)] // out-struct + two buffers is the FFI shape
fn entry_at(
    engine: *mut QimgvEngine,
    index: c_int,
    dirs: bool,
    out: *mut QimgvEntryMeta,
    name_buf: *mut c_char,
    name_cap: usize,
    path_buf: *mut c_char,
    path_cap: usize,
) -> c_int {
    if engine.is_null() || out.is_null() {
        set_last_error("invalid argument");
        return QIMGV_ERR_INVALID;
    }
    if index < 0 {
        set_last_error("negative index");
        return QIMGV_ERR_INVALID;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        with_engine(engine, |e| {
            let st = e.state.read().unwrap_or_else(|p| p.into_inner());
            let entry = if dirs {
                st.dir_at(index as usize)
            } else {
                st.file_at(index as usize)
            };
            let Some(entry) = entry else {
                return Err(QIMGV_ERR_EMPTY);
            };
            write_str(name_buf, name_cap, &entry.name)?;
            write_str(path_buf, path_cap, &entry.path)?;
            // SAFETY: `out` was checked non-null at the top of entry_at; the
            // caller guarantees it is valid and writable for one
            // QimgvEntryMeta (a plain repr(C) struct of Copy fields, so the
            // assignment drops nothing).
            unsafe {
                *out = QimgvEntryMeta {
                    size: entry.size,
                    mtime_ms: mtime_ms(&entry.mtime),
                    is_dir: if dirs { 1 } else { 0 },
                };
            }
            Ok(QIMGV_OK)
        })
    }));
    match r {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("engine panic");
            QIMGV_ERR_PANIC
        }
    }
}

/// Entry `index` (0-based) of the file listing: fills `out` (handle/size/
/// /// mtime/is_dir) and copies the name and path into the caller's buffers —
/// /// NUL-terminated; a too-small buffer fails with QIMGV_ERR_TRUNCATED (never
/// /// silently truncated). Out of range = QIMGV_ERR_EMPTY.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_file_entry_at(
    engine: *mut QimgvEngine,
    index: c_int,
    out: *mut QimgvEntryMeta,
    name_buf: *mut c_char,
    name_cap: usize,
    path_buf: *mut c_char,
    path_cap: usize,
) -> c_int {
    entry_at(
        engine, index, false, out, name_buf, name_cap, path_buf, path_cap,
    )
}

/// Entry `index` (0-based) of the directory listing; same buffer contract as
/// /// qimgv_engine_file_entry_at.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_dir_entry_at(
    engine: *mut QimgvEngine,
    index: c_int,
    out: *mut QimgvEntryMeta,
    name_buf: *mut c_char,
    name_cap: usize,
    path_buf: *mut c_char,
    path_cap: usize,
) -> c_int {
    entry_at(
        engine, index, true, out, name_buf, name_cap, path_buf, path_cap,
    )
}

// --- bulk listing export ------------------------------------------------------

/// Serializes the current listing into `buf` in one lock pass (LE wire format,
/// see model::write_listing). If `cap` is too small, returns QIMGV_ERR_TRUNCATED
/// with `needed_out` set; counts are always written.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_export_listing(
    engine: *mut QimgvEngine,
    buf: *mut u8,
    cap: usize,
    needed_out: *mut u32,
    file_count_out: *mut u32,
    dir_count_out: *mut u32,
    revision_out: *mut u64,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        if engine.is_null()
            || needed_out.is_null()
            || file_count_out.is_null()
            || dir_count_out.is_null()
            || revision_out.is_null()
        {
            return Err(QIMGV_ERR_INVALID);
        }
        with_engine(engine, |e| {
            // cap==0 with a null buf is the size-probe call
            let slice = if buf.is_null() && cap == 0 {
                &mut [][..]
            } else if buf.is_null() {
                return Err(QIMGV_ERR_INVALID);
            } else {
                // SAFETY: `buf` is non-null here and the caller guarantees it
                // is valid for reads and writes of `cap` bytes with no other
                // reference alive for this call; u8 has no invalid bit
                // patterns, so an uninitialized caller buffer is not read as
                // anything but bytes the engine then overwrites.
                unsafe { std::slice::from_raw_parts_mut(buf, cap) }
            };
            let (needed, fc, dc, revision) = e.export_listing(slice);
            // SAFETY: all four out pointers were checked non-null at the top of
            // this closure; the caller guarantees each is valid and writable
            // for its type (u32, u32, u32, u64).
            unsafe {
                *needed_out = needed as u32;
                *file_count_out = fc as u32;
                *dir_count_out = dc as u32;
                *revision_out = revision;
            }
            if slice.len() < needed {
                Err(QIMGV_ERR_TRUNCATED)
            } else {
                Ok(QIMGV_OK)
            }
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_export_listing");
            QIMGV_ERR_PANIC
        }
    }
}

// --- header probe -----------------------------------------------------------

/// Pure function (thread-safe, does not touch the engine instance): one file
/// open, header-only parse. Parse failures are QIMGV_OK with FMT_UNKNOWN and
/// zero dimensions; only an unreadable file is ERR_IO.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_probe(
    path: *const c_char,
    out: *mut QimgvProbeResult,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        if out.is_null() {
            return Err(QIMGV_ERR_INVALID);
        }
        let p = read_path(path)?;
        let info = qimgv_engine::meta::probe(&p).map_err(|e| map_engine_error(&e))?;
        // SAFETY: `out` was checked non-null at the top of this closure; the
        // caller guarantees it is valid and writable for one QimgvProbeResult
        // (a repr(C) struct of c_int fields).
        unsafe {
            *out = QimgvProbeResult {
                format: info.format as c_int,
                animated: info.animated as c_int,
                orientation: info.orientation as c_int,
                width: info.width as c_int,
                height: info.height as c_int,
            };
        }
        Ok(QIMGV_OK)
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_probe");
            QIMGV_ERR_PANIC
        }
    }
}

// --- watcher ----------------------------------------------------------------

/// Force-includes a path in the current listing (app-side mime fallback);
/// persists across rescans until the file disappears. Returns OK even if the
/// file is gone — the state simply won't change.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_include_path(
    engine: *mut QimgvEngine,
    path: *const c_char,
) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        let path = read_path(path)?;
        with_engine(engine, |e| {
            e.send(Command::IncludePath(path))
                .map(|_| QIMGV_OK)
                .map_err(|e| map_engine_error(&e))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_include_path");
            QIMGV_ERR_PANIC
        }
    }
}

/// Stops watching the current directory (after a listing reset).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_stop_watch(engine: *mut QimgvEngine) -> c_int {
    let attempt = || -> Result<c_int, c_int> {
        with_engine(engine, |e| {
            e.send(Command::StopWatch)
                .map(|_| QIMGV_OK)
                .map_err(|e| map_engine_error(&e))
        })
    };
    match catch_unwind(AssertUnwindSafe(attempt)) {
        Ok(Ok(code)) => code,
        Ok(Err(code)) => code,
        Err(_) => {
            set_last_error("panic in qimgv_engine_stop_watch");
            QIMGV_ERR_PANIC
        }
    }
}

/// 1 when the engine ships its own inotify watcher (Linux); 0 means the
/// caller keeps using its platform watcher and the mutation events never fire.
#[unsafe(no_mangle)]
pub extern "C" fn qimgv_engine_supports_watching() -> c_int {
    if Engine::supports_watching() { 1 } else { 0 }
}

// --- events -----------------------------------------------------------------

/// Polls one notice. All three payload pointers can be null to read only its
/// kind and stamp. Diagnostic callers can request the subjects and mutation.
/// A refused diagnostic read does not consume the notice.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qimgv_engine_take_event(
    engine: *mut QimgvEngine,
    out: *mut QimgvEvent,
    subject_buf: *mut c_char,
    subject_cap: usize,
    old_buf: *mut c_char,
    old_cap: usize,
    mutation: *mut QimgvMutation,
) -> c_int {
    if engine.is_null() || out.is_null() {
        set_last_error("invalid argument");
        return QIMGV_ERR_INVALID;
    }
    let r = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: `engine` was checked non-null at the top of the function;
        // the caller guarantees it is live and not freed concurrently. Only a
        // shared reference is taken; the stash and engine are locked below.
        let eng = unsafe { &*engine };
        // Lock order is stashed then engine, everywhere; take_event is the only
        // function that touches both.
        let mut stash = eng.stashed.lock().unwrap_or_else(|p| p.into_inner());
        let notice_only = subject_buf.is_null() && old_buf.is_null() && mutation.is_null();
        let pending: Option<Event> = match stash.take() {
            Some(ev) => Some(ev),
            None => with_engine(engine, |e| e.try_event()),
        };
        // An inner closure: the arms' `?` has to mean "this event did not fit",
        // not "leave take_event" — the latter would skip the put-it-back below,
        // which is the whole point of the stash.
        let outcome: Result<c_int, c_int> = (|| -> Result<c_int, c_int> {
            match pending.as_ref() {
                None => Ok(0),
                Some(ev) => match ev {
                    Event::DirListed {
                        dir,
                        epoch,
                        revision,
                    } => {
                        if !notice_only {
                            write_str(subject_buf, subject_cap, &dir.to_string_lossy())?;
                        }
                        // SAFETY: `out` was checked non-null at the top of
                        // take_event; the caller guarantees it is valid and
                        // writable for one QimgvEvent.
                        unsafe {
                            *out = QimgvEvent {
                                kind: QIMGV_EVENT_DIR_LISTED,
                                code: QIMGV_OK,
                                epoch: *epoch,
                                seq: 0,
                                revision: *revision,
                            };
                        }
                        Ok(1)
                    }
                    Event::SortChanged {
                        dir,
                        epoch,
                        revision,
                    } => {
                        if !notice_only {
                            write_str(subject_buf, subject_cap, &dir.to_string_lossy())?;
                        }
                        // SAFETY: same `out` contract as the DIR_LISTED arm.
                        unsafe {
                            *out = QimgvEvent {
                                kind: QIMGV_EVENT_SORT_CHANGED,
                                code: QIMGV_OK,
                                epoch: *epoch,
                                seq: 0,
                                revision: *revision,
                            };
                        }
                        Ok(1)
                    }
                    Event::OpenFailed { dir, error, epoch } => {
                        if !notice_only {
                            write_str(subject_buf, subject_cap, &dir.to_string_lossy())?;
                        }
                        let code = map_engine_error(error);
                        set_last_error(&error.to_string());
                        // SAFETY: same `out` contract as the DIR_LISTED arm.
                        unsafe {
                            *out = QimgvEvent {
                                kind: QIMGV_EVENT_OPEN_FAILED,
                                code,
                                epoch: *epoch,
                                seq: 0,
                                revision: 0,
                            };
                        }
                        Ok(1)
                    }
                    Event::Mutated {
                        seq,
                        revision,
                        kind,
                        path,
                        is_dir,
                        size,
                        mtime_ms,
                        index,
                        epoch,
                    } => {
                        // A rename is the one mutation with two subjects, and both have
                        // to fit before either is written: a caller that cannot receive
                        // the old path is left with an event it can only mis-apply, so
                        // the event stays queued instead.
                        let from = match kind {
                            qimgv_engine::WatchChangeKind::Renamed { from } => Some(from),
                            _ => None,
                        };
                        if !notice_only {
                            if let Some(from) = from {
                                fits(old_buf, old_cap, from)?;
                            }
                            write_str(subject_buf, subject_cap, path)?;
                            if let Some(from) = from {
                                write_str(old_buf, old_cap, from)?;
                            }
                        }
                        let mkind = match kind {
                            qimgv_engine::WatchChangeKind::Added => QIMGV_MUT_ADDED,
                            qimgv_engine::WatchChangeKind::Modified => QIMGV_MUT_MODIFIED,
                            qimgv_engine::WatchChangeKind::Removed => QIMGV_MUT_REMOVED,
                            qimgv_engine::WatchChangeKind::Renamed { .. } => QIMGV_MUT_RENAMED,
                        };
                        if !mutation.is_null() {
                            // SAFETY: non-null checked on the line above; the
                            // caller guarantees it is valid and writable for
                            // one QimgvMutation.
                            unsafe {
                                *mutation = QimgvMutation {
                                    kind: mkind,
                                    is_dir: *is_dir as c_int,
                                    size: *size,
                                    mtime_ms: *mtime_ms,
                                    index: *index as c_int,
                                };
                            }
                        }
                        // SAFETY: same `out` contract as the DIR_LISTED arm.
                        unsafe {
                            *out = QimgvEvent {
                                kind: QIMGV_EVENT_DIR_MUTATED,
                                code: QIMGV_OK,
                                epoch: *epoch,
                                seq: *seq,
                                revision: *revision,
                            };
                        }
                        Ok(1)
                    }
                },
            }
        })();
        if outcome.is_err() {
            // Nothing was written: every arm checks the buffer before touching an
            // out-parameter. Putting the event back keeps take_event total — a
            // DirListed swallowed by a short buffer looks exactly like an open
            // that never answers, which is the worse of the two failures.
            *stash = pending;
        }
        outcome
    }));
    match r {
        Ok(Ok(n)) => n,
        Ok(Err(code)) => {
            set_last_error_by_code(code);
            code
        }
        Err(_) => {
            set_last_error("panic in qimgv_engine_take_event");
            QIMGV_ERR_PANIC
        }
    }
}
