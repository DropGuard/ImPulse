//! One switch for every point a watch event can stop.
//!
//! A file missing from a listing has a long candidate list — the kernel never
//! reported it, the loop dropped it as not-the-current-folder, the filter
//! rejected its name, the stat lost the race, the entry was already listed, the
//! ABI truncated it, the mirror refused it — and all of them produce the same
//! observable state, which is nothing. A print statement per file does not
//! settle that: the lines have to say which node produced them and which event
//! they are holding, and they have to be off by default. So this is one env
//! variable (`QIMGV_ENGINE_TRACE`, unset or `0` means silent), one function,
//! and a name at every call site.
//!
//! The sink: stderr by default, or a per-process file when
//! `QIMGV_ENGINE_TRACE_FILE` names a prefix (`<prefix><pid>.log`). The file
//! sink exists because the sink itself is part of the measurement — an
//! eprintln into a ctest pipe paces the engine loop thread enough to hide a
//! burst-order race (the mirrorStays flake: trace-on passed every run the
//! untraced build failed), so the diagnosis has to be able to run at full
//! speed with the trace still on.

use std::env;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::sync::{Mutex, OnceLock};
use std::time::Instant;

fn enabled() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| match env::var_os("QIMGV_ENGINE_TRACE") {
        Some(v) => v != "0",
        None => false,
    })
}

fn file_sink() -> Option<&'static Mutex<File>> {
    static SINK: OnceLock<Option<Mutex<File>>> = OnceLock::new();
    SINK.get_or_init(|| {
        let prefix = env::var_os("QIMGV_ENGINE_TRACE_FILE")?;
        let path = std::path::PathBuf::from(prefix)
            .with_extension(format!("{}.log", std::process::id()));
        match OpenOptions::new().create(true).append(true).open(&path) {
            Ok(f) => Some(Mutex::new(f)),
            Err(e) => {
                eprintln!("[engine trace] cannot open {:?}: {e} — falling back to stderr", path);
                None
            }
        }
    })
    .as_ref()
}

static T0: OnceLock<Instant> = OnceLock::new();

/// `detail` is a closure, not an argument: when tracing is off the only cost is
/// a branch, so a node in the event path does not format paths that nobody will
/// read. Callers are the nodes themselves — `read`, `apply`, `emit` — and a line
/// is expected to be readable next to the C++ mirror's own.
pub fn line(node: &str, detail: impl FnOnce() -> String) {
    if !enabled() {
        return;
    }
    let t0 = T0.get_or_init(Instant::now);
    let line = format!(
        "[engine trace] +{:>8.1}ms {node} {}",
        t0.elapsed().as_secs_f64() * 1000.0,
        detail()
    );
    match file_sink() {
        Some(file) => {
            let mut f = file.lock().unwrap_or_else(|e| e.into_inner());
            let _ = writeln!(f, "{line}");
        }
        None => eprintln!("{line}"),
    }
}
