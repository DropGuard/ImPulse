use std::path::{Path, PathBuf};
use std::time::SystemTime;

use crate::sort::SortMode;

#[derive(Debug, Clone)]
pub struct EntryData {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
    pub size: u64,
    pub mtime: SystemTime,
}

/// The single source of truth for the current directory. Mutation happens on
/// the engine loop thread; the FFI layer reads it through the shared
/// `Arc<RwLock<_>>` on the UI thread.
///
/// The rows live directly in the two order vectors — files and directories
/// sorted independently. There are no handles: the only consumer that ever
/// keyed on one was a set of FFI navigation functions nothing called (the C++
/// mirror keys on path and navigates its own vectors), so the generational
/// slotmap, the two position maps synced at four mutation sites, and the
/// handle bytes on the wire all retired together.
#[derive(Debug)]
pub struct DirectoryState {
    dir: Option<PathBuf>,
    file_order: Vec<EntryData>,
    dir_order: Vec<EntryData>,
    sort: SortMode,
    /// Bumped whenever the listing's contents OR its order change. A reader
    /// that wants a snapshot it can trust reads this before and after: the
    /// same number means the listing did not move under it. Without it the
    /// only way to notice a change is to compare the data, which is a guess
    /// about how long the comparison takes.
    revision: u64,
}

impl DirectoryState {
    pub fn new(sort: SortMode) -> Self {
        Self {
            dir: None,
            file_order: Vec::new(),
            dir_order: Vec::new(),
            sort,
            revision: 0,
        }
    }

    /// The listing's version. Every mutation bumps it; nothing else does.
    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn dir(&self) -> Option<&Path> {
        self.dir.as_deref()
    }

    pub fn sort(&self) -> SortMode {
        self.sort
    }

    /// Replaces the whole listing. The rows the reader held are gone; the new
    /// ones are assigned here and ordered.
    pub fn replace_all(&mut self, dir: PathBuf, files: Vec<EntryData>, dirs: Vec<EntryData>) {
        self.dir = Some(dir);
        self.file_order = files;
        self.dir_order = dirs;
        self.reorder();
    }

    pub fn resort(&mut self, sort: SortMode) {
        self.sort = sort;
        self.reorder();
    }

    /// Records the sort a listing is asked for, without reordering anything.
    /// For an open: the listing this mode will govern does not exist yet, and
    /// resorting the outgoing one would spend an O(n log n) pass on rows
    /// `replace_all` is about to discard — and strand a revision bump that no
    /// event will ever carry. The reorder happens when the listing lands.
    pub fn set_sort_mode(&mut self, sort: SortMode) {
        self.sort = sort;
    }

    fn reorder(&mut self) {
        // the one place an order is decided, so the one place the version
        // moves for an order change
        self.revision = self.revision.wrapping_add(1);
        let mode = self.sort;
        self.file_order.sort_by(|a, b| mode.compare(a, b));
        self.dir_order.sort_by(|a, b| mode.compare(a, b));
    }

    pub fn file_count(&self) -> usize {
        self.file_order.len()
    }

    pub fn dir_count(&self) -> usize {
        self.dir_order.len()
    }

    pub fn file_at(&self, index: usize) -> Option<&EntryData> {
        self.file_order.get(index)
    }

    pub fn dir_at(&self, index: usize) -> Option<&EntryData> {
        self.dir_order.get(index)
    }

    // --- incremental mutations (engine watcher) ---------------------------------

    /// Inserts an entry keeping the current sort order. Returns the position
    /// the engine chose for it, or None if the path is already listed. The
    /// position is what the mutation event carries: the mirror splices there
    /// instead of re-deriving an order.
    pub fn insert_entry(&mut self, data: EntryData) -> Option<u32> {
        if self.contains_path(&data.path) {
            return None;
        }
        let (order, _) = if data.is_dir {
            (&mut self.dir_order, true)
        } else {
            (&mut self.file_order, false)
        };
        let mode = self.sort;
        let idx = order
            .binary_search_by(|e| mode.compare(e, &data))
            .unwrap_or_else(|i| i);
        order.insert(idx, data);
        self.revision = self.revision.wrapping_add(1);
        Some(idx as u32)
    }

    /// Is this path one of the listing's entries?
    pub fn contains_path(&self, path: &str) -> bool {
        self.file_order.iter().any(|e| e.path == path)
            || self.dir_order.iter().any(|e| e.path == path)
    }

    /// Removes by full path from whichever order it lives in. Returns the
    /// position the entry held before removal.
    pub fn remove_entry_by_path(&mut self, path: &str) -> Option<u32> {
        let index = self.file_order.iter().position(|e| e.path == path);
        let order = match index {
            Some(_) => &mut self.file_order,
            None => &mut self.dir_order,
        };
        let index = match index {
            Some(i) => i,
            None => order.iter().position(|e| e.path == path)?,
        };
        order.remove(index);
        self.revision = self.revision.wrapping_add(1);
        Some(index as u32)
    }

    /// (size, mtime) of a listed entry, for watcher change detection.
    pub fn entry_meta(&self, path: &str) -> Option<(u64, SystemTime)> {
        self.file_order
            .iter()
            .chain(&self.dir_order)
            .find(|e| e.path == path)
            .map(|e| (e.size, e.mtime))
    }

    /// Updates (size, mtime) of a listed entry and returns its position in the
    /// engine's own order afterwards. A size/time sort can reorder on a
    /// metadata change, so the entry is re-seated when the current mode can
    /// move it; name sorts return the position unchanged. Without the
    /// re-seating, the order vectors stop being sorted and the export itself
    /// stops agreeing with the comparator.
    pub fn update_entry_meta(&mut self, path: &str, size: u64, mtime: SystemTime) -> Option<u32> {
        let order = if self.file_order.iter().any(|e| e.path == path) {
            &mut self.file_order
        } else {
            &mut self.dir_order
        };
        let e = order.iter_mut().find(|e| e.path == path)?;
        e.size = size;
        e.mtime = mtime;
        // a name sort keys on the name, which a metadata change never moves
        if matches!(self.sort, SortMode::Name | SortMode::NameDesc) {
            // size/mtime changed, so the listing changed even though no row
            // moved — a reader's snapshot is stale either way
            self.revision = self.revision.wrapping_add(1);
            return order.iter().position(|e| e.path == path).map(|i| i as u32);
        }
        Self::reseat(order, self.sort, path)
    }

    /// Gives a listed entry a new path and name without replacing it, and
    /// returns the position it lands in. To the engine (and to everything
    /// downstream that keys on the path) this is one entry that moved, not an
    /// entry that died and a second one that was born next door. Unlike a
    /// metadata update, a rename re-seats under EVERY mode, because `Name`
    /// sorts by the name that just changed.
    ///
    /// The destination must not already be listed: an overwrite takes the old
    /// destination row out first (`remove_entry_by_path`), so the identity that
    /// survives is the moved-in file's, and one event can describe the result.
    pub fn rename_entry(
        &mut self,
        from: &str,
        to: &str,
        name: &str,
        size: u64,
        mtime: SystemTime,
    ) -> Option<u32> {
        let from_is_file = self.file_order.iter().any(|e| e.path == from);
        if !from_is_file && !self.dir_order.iter().any(|e| e.path == from) {
            return None;
        }
        // the destination must not already be listed, in either order: an
        // overwrite takes the old destination row out first, so the identity
        // that survives is the moved-in file's
        if self.contains_path(to) {
            return None;
        }
        let order = if from_is_file {
            &mut self.file_order
        } else {
            &mut self.dir_order
        };
        let e = order.iter_mut().find(|e| e.path == from)?;
        e.path = to.to_owned();
        e.name = name.to_owned();
        e.size = size;
        e.mtime = mtime;
        Self::reseat(order, self.sort, to)
    }

    /// Re-seats one entry in its own order after its sort keys changed: remove
    /// from the old position, binary-search the new one, splice. Returns the
    /// new position.
    fn reseat(order: &mut Vec<EntryData>, mode: SortMode, path: &str) -> Option<u32> {
        let old = order.iter().position(|e| e.path == path)?;
        let e = order.remove(old);
        let idx = order
            .binary_search_by(|other| mode.compare(other, &e))
            .unwrap_or_else(|i| i);
        order.insert(idx, e);
        Some(idx as u32)
    }

    // --- bulk listing export ------------------------------------------------

    /// Serializes the whole listing into `buf` (little-endian, one read-lock pass):
    ///
    ///   u32 file_count, u32 dir_count
    ///   then file_count + dir_count records (files first):
    ///     u64 size, i64 mtime_ms, u32 name_len, u32 path_len
    ///     name bytes, path bytes (no NUL)
    ///
    /// Returns (needed_bytes, file_count, dir_count). When `buf` is smaller than
    /// needed, nothing is written and needed_bytes tells the caller how much to
    /// allocate.
    pub fn write_listing(&self, buf: &mut [u8]) -> (usize, usize, usize) {
        let mut needed = 8usize;
        for e in self.file_order.iter().chain(&self.dir_order) {
            needed += 24 + e.name.len() + e.path.len();
        }
        let file_count = self.file_order.len();
        let dir_count = self.dir_order.len();
        if buf.len() < needed {
            return (needed, file_count, dir_count);
        }

        let mut off = 0usize;
        let mut put = |bytes: &[u8]| {
            buf[off..off + bytes.len()].copy_from_slice(bytes);
            off += bytes.len();
        };
        put(&(file_count as u32).to_le_bytes());
        put(&(dir_count as u32).to_le_bytes());
        for e in self.file_order.iter().chain(&self.dir_order) {
            put(&e.size.to_le_bytes());
            put(&crate::watcher::mtime_ms(e.mtime).to_le_bytes());
            put(&(e.name.len() as u32).to_le_bytes());
            put(&(e.path.len() as u32).to_le_bytes());
            put(e.name.as_bytes());
            put(e.path.as_bytes());
        }
        (needed, file_count, dir_count)
    }
}
