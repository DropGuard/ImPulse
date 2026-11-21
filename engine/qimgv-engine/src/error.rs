use std::path::PathBuf;

#[derive(Debug, thiserror::Error)]
pub enum EngineError {
    #[error("I/O error on {}: {source}", path.display())]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("engine command channel closed")]
    EngineGone,
    #[error("unknown sort mode: {0}")]
    BadSortMode(i32),
}
