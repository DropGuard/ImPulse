use std::collections::HashMap;
use std::ops::Deref;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use crate::sort::{SortMode, sort_key};

#[derive(Debug, Clone)]
pub struct EntryData {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
    pub size: u64,
    pub mtime: SystemTime,
}

#[derive(Debug)]
struct Row {
    entry: EntryData,
    id: u64,
    content_revision: u64,
    // the name's byte encoding of natural() order (sort::sort_key): rebuilt
    // only on name change — install, insert, rename — and compared by memcmp
    sort_key: Vec<u8>,
}

impl Row {
    fn build(entry: EntryData, id: u64, content_revision: u64) -> Row {
        let sort_key = sort_key(&entry.name);
        Row {
            entry,
            id,
            content_revision,
            sort_key,
        }
    }
}

// Mode dispatch over the cached keys: Name variants are the keys themselves;
// Time/Size compare their field first and fall back to the name keys, which
// is what SortMode::compare's natural tiebreak did.
fn cmp_rows(mode: SortMode, a: &Row, b: &Row) -> std::cmp::Ordering {
    match mode {
        SortMode::Name => a.sort_key.cmp(&b.sort_key),
        SortMode::NameDesc => b.sort_key.cmp(&a.sort_key),
        SortMode::Time => a
            .entry
            .mtime
            .cmp(&b.entry.mtime)
            .then_with(|| a.sort_key.cmp(&b.sort_key)),
        SortMode::TimeDesc => b
            .entry
            .mtime
            .cmp(&a.entry.mtime)
            .then_with(|| a.sort_key.cmp(&b.sort_key)),
        SortMode::Size => a
            .entry
            .size
            .cmp(&b.entry.size)
            .then_with(|| a.sort_key.cmp(&b.sort_key)),
        SortMode::SizeDesc => b
            .entry
            .size
            .cmp(&a.entry.size)
            .then_with(|| a.sort_key.cmp(&b.sort_key)),
    }
}

impl Deref for Row {
    type Target = EntryData;

    fn deref(&self) -> &EntryData {
        &self.entry
    }
}

/// The published directory, sort, and rows always describe the same commit.
#[derive(Debug)]
pub struct DirectoryState {
    dir: Option<PathBuf>,
    file_order: Vec<Row>,
    dir_order: Vec<Row>,
    sort: SortMode,
    session: u64,
    revision: u64,
    next_id: u64,
}

impl DirectoryState {
    pub fn new(sort: SortMode) -> Self {
        Self {
            dir: None,
            file_order: Vec::new(),
            dir_order: Vec::new(),
            sort,
            session: 0,
            revision: 0,
            next_id: 1,
        }
    }

    /// The listing's version. Every mutation bumps it; nothing else does.
    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn session(&self) -> u64 {
        self.session
    }

    fn commit(&mut self) {
        self.revision += 1;
    }

    pub fn dir(&self) -> Option<&Path> {
        self.dir.as_deref()
    }

    pub fn sort(&self) -> SortMode {
        self.sort
    }

    pub fn replace_all(&mut self, dir: PathBuf, files: Vec<EntryData>, dirs: Vec<EntryData>) {
        self.install(self.session, dir, self.sort, files, dirs);
    }

    pub(crate) fn install(
        &mut self,
        session: u64,
        dir: PathBuf,
        sort: SortMode,
        files: Vec<EntryData>,
        dirs: Vec<EntryData>,
    ) {
        let mut prior = HashMap::new();
        if self.session == session && self.dir.as_ref() == Some(&dir) {
            for row in self.file_order.drain(..).chain(self.dir_order.drain(..)) {
                prior.insert(
                    row.entry.path,
                    (
                        row.id,
                        row.content_revision,
                        row.entry.size,
                        row.entry.mtime,
                        row.entry.is_dir,
                    ),
                );
            }
        }
        let mut identity = |entry: EntryData| {
            if let Some((id, version, size, mtime, is_dir)) = prior.remove(&entry.path)
                && is_dir == entry.is_dir
            {
                let changed = size != entry.size || mtime != entry.mtime;
                return Row::build(entry, id, version + u64::from(changed));
            }
            self.new_row(entry)
        };
        let files = files.into_iter().map(&mut identity).collect();
        let dirs = dirs.into_iter().map(identity).collect();
        self.session = session;
        self.dir = Some(dir);
        self.file_order = files;
        self.dir_order = dirs;
        self.sort = sort;
        self.reorder();
        self.commit();
    }

    pub fn resort(&mut self, sort: SortMode) -> bool {
        if self.sort == sort {
            return false;
        }
        self.sort = sort;
        self.reorder();
        self.commit();
        true
    }

    fn reorder(&mut self) {
        let mode = self.sort;
        // unstable is deliberate: the entries come from ONE directory, and a
        // directory cannot hold two entries whose comparator ties (same-name
        // files cannot coexist; case twins cannot coexist on a real
        // filesystem) — stability protects nothing and pdqsort pays fewer
        // comparisons with no merge buffer. Measured: the stable sort spent
        // ~850 ms of a cold open on a 15413-entry listing.
        self.file_order
            .sort_unstable_by(|a, b| cmp_rows(mode, a, b));
        self.dir_order.sort_unstable_by(|a, b| cmp_rows(mode, a, b));
    }

    pub fn file_count(&self) -> usize {
        self.file_order.len()
    }

    pub fn dir_count(&self) -> usize {
        self.dir_order.len()
    }

    pub fn file_at(&self, index: usize) -> Option<&EntryData> {
        self.file_order.get(index).map(|row| &row.entry)
    }

    pub fn dir_at(&self, index: usize) -> Option<&EntryData> {
        self.dir_order.get(index).map(|row| &row.entry)
    }

    fn new_row(&mut self, entry: EntryData) -> Row {
        let id = self.next_id;
        self.next_id += 1;
        Row::build(entry, id, 1)
    }

    // --- incremental mutations (engine watcher) ---------------------------------

    /// Inserts an entry keeping the current sort order. Returns the position
    /// the engine chose for it, or None if the path is already listed.
    pub fn insert_entry(&mut self, data: EntryData) -> Option<u32> {
        if self.contains_path(&data.path) {
            return None;
        }
        let row = self.new_row(data);
        let order = if row.is_dir {
            &mut self.dir_order
        } else {
            &mut self.file_order
        };
        let mode = self.sort;
        let idx = order
            .binary_search_by(|entry| cmp_rows(mode, entry, &row))
            .unwrap_or_else(|i| i);
        order.insert(idx, row);
        self.commit();
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
        let index = self.erase(path)?;
        self.commit();
        Some(index)
    }

    fn erase(&mut self, path: &str) -> Option<u32> {
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
        let index = order.iter().position(|entry| entry.path == path)?;
        let row = &mut order[index];
        row.entry.size = size;
        row.entry.mtime = mtime;
        row.content_revision += 1;
        let index = if matches!(self.sort, SortMode::Name | SortMode::NameDesc) {
            index as u32
        } else {
            Self::reseat(order, self.sort, index)
        };
        self.commit();
        Some(index)
    }

    /// Gives a listed entry a new path and name without replacing it, and
    /// returns the position it lands in. To the engine (and to everything
    /// downstream that keys on the path) this is one entry that moved, not an
    /// entry that died and a second one that was born next door. Unlike a
    /// metadata update, a rename re-seats under EVERY mode, because `Name`
    /// sorts by the name that just changed.
    ///
    /// An overwrite retires the previous destination's ID in the same commit.
    pub fn rename_entry(
        &mut self,
        from: &str,
        to: &str,
        name: &str,
        size: u64,
        mtime: SystemTime,
    ) -> Option<u32> {
        let from_is_file = self.file_order.iter().any(|e| e.path == from);
        if from == to || (!from_is_file && !self.dir_order.iter().any(|e| e.path == from)) {
            return None;
        }
        self.erase(to);
        let order = if from_is_file {
            &mut self.file_order
        } else {
            &mut self.dir_order
        };
        let index = order.iter().position(|entry| entry.path == from)?;
        let row = &mut order[index];
        if row.size != size || row.mtime != mtime {
            row.content_revision += 1;
        }
        row.entry.path = to.to_owned();
        row.entry.name = name.to_owned();
        row.entry.size = size;
        row.entry.mtime = mtime;
        row.sort_key = sort_key(name);
        let index = Self::reseat(order, self.sort, index);
        self.commit();
        Some(index)
    }

    /// Re-seats one entry in its own order after its sort keys changed: remove
    /// from the old position, binary-search the new one, splice. Returns the
    /// new position.
    fn reseat(order: &mut Vec<Row>, mode: SortMode, old: usize) -> u32 {
        let row = order.remove(old);
        let idx = order
            .binary_search_by(|other| cmp_rows(mode, other, &row))
            .unwrap_or_else(|i| i);
        order.insert(idx, row);
        idx as u32
    }

    // --- bulk listing export ------------------------------------------------

    /// Serializes the whole listing into `buf` (little-endian, one read-lock pass):
    ///
    ///   u32 file_count, u32 dir_count, u64 session, u64 revision, i32 sort,
    ///   u32 directory_len, directory bytes (UTF-8, no NUL)
    ///   then file_count + dir_count records (files first):
    ///     u64 id, u64 content_revision, u64 size, i64 mtime_ms,
    ///     u32 name_len, u32 path_len
    ///     name bytes, path bytes (no NUL)
    ///
    /// Returns (needed_bytes, file_count, dir_count). When `buf` is smaller than
    /// needed, nothing is written and needed_bytes tells the caller how much to
    /// allocate.
    pub fn write_listing(&self, buf: &mut [u8]) -> (usize, usize, usize) {
        let dir = self.dir().map(Path::to_string_lossy).unwrap_or_default();
        let mut needed = 32 + dir.len();
        for e in self.file_order.iter().chain(&self.dir_order) {
            needed += 40 + e.name.len() + e.path.len();
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
        put(&self.session.to_le_bytes());
        put(&self.revision.to_le_bytes());
        put(&(self.sort as i32).to_le_bytes());
        put(&(dir.len() as u32).to_le_bytes());
        put(dir.as_bytes());
        for e in self.file_order.iter().chain(&self.dir_order) {
            put(&e.id.to_le_bytes());
            put(&e.content_revision.to_le_bytes());
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
