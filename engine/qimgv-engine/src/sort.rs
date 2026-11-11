use std::cmp::Ordering;

use crate::model::EntryData;

/// Mirrors qimgv's `SortingMode` (settings.h). The C++ bridge maps values
/// explicitly, so the numbering here is the FFI contract (see qimgv_engine.h).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum SortMode {
    Name = 0,
    NameDesc = 1,
    Time = 2,
    TimeDesc = 3,
    Size = 4,
    SizeDesc = 5,
}

impl TryFrom<i32> for SortMode {
    type Error = crate::EngineError;

    fn try_from(v: i32) -> Result<Self, Self::Error> {
        match v {
            0 => Ok(Self::Name),
            1 => Ok(Self::NameDesc),
            2 => Ok(Self::Time),
            3 => Ok(Self::TimeDesc),
            4 => Ok(Self::Size),
            5 => Ok(Self::SizeDesc),
            other => Err(crate::EngineError::BadSortMode(other)),
        }
    }
}

/// Natural, case-insensitive name comparison, with character classes
/// weighted the way ICU/DUCET (and therefore Dolphin) orders them:
/// whitespace and punctuation before digits before letters. A plain
/// codepoint comparison has digits before brackets — `0老王…` before
/// `[Pure Media]…` — which is the Dolphin mismatch the owner reported
/// live (2026-11-11, /mnt/disk4t/已阅/色图新). Digit runs compare by
/// numeric value ("2" < "10"); full ICU collation is still deliberately
/// out of scope, this is its practical core.
pub fn natural(a: &str, b: &str) -> Ordering {
    fn class(c: char) -> u8 {
        if c.is_numeric() {
            2 // digits
        } else if c.is_alphabetic() {
            3 // letters, CJK ideographs included
        } else {
            1 // whitespace, punctuation, symbols
        }
    }

    let av: Vec<(char, u8)> = a.chars().map(|c| (c, class(c))).collect();
    let bv: Vec<(char, u8)> = b.chars().map(|c| (c, class(c))).collect();
    let (mut i, mut j) = (0usize, 0usize);
    while i < av.len() && j < bv.len() {
        let (ca, ka) = av[i];
        let (cb, kb) = bv[j];
        if ka != kb {
            return ka.cmp(&kb);
        }
        if ka == 2 {
            // digit runs compare by numeric value: strip leading zeros,
            // then longer wins, then digit-wise
            let ra: String = av[i..]
                .iter()
                .take_while(|(c, _)| c.is_numeric())
                .map(|(c, _)| c)
                .collect();
            let rb: String = bv[j..]
                .iter()
                .take_while(|(c, _)| c.is_numeric())
                .map(|(c, _)| c)
                .collect();
            i += ra.chars().count();
            j += rb.chars().count();
            let ta = ra.trim_start_matches('0');
            let tb = rb.trim_start_matches('0');
            let ord = ta.len().cmp(&tb.len()).then_with(|| ta.cmp(tb));
            if ord != Ordering::Equal {
                return ord;
            }
            continue;
        }
        let la = ca.to_lowercase().next().unwrap_or(ca);
        let lb = cb.to_lowercase().next().unwrap_or(cb);
        if la != lb {
            return la.cmp(&lb);
        }
        i += 1;
        j += 1;
    }
    (av.len() - i).cmp(&(bv.len() - j)).then_with(|| a.cmp(b))
}

impl SortMode {
    pub fn compare(self, a: &EntryData, b: &EntryData) -> Ordering {
        match self {
            Self::Name => natural(&a.name, &b.name),
            Self::NameDesc => natural(&b.name, &a.name),
            Self::Time => a
                .mtime
                .cmp(&b.mtime)
                .then_with(|| natural(&a.name, &b.name)),
            Self::TimeDesc => b
                .mtime
                .cmp(&a.mtime)
                .then_with(|| natural(&a.name, &b.name)),
            Self::Size => a.size.cmp(&b.size).then_with(|| natural(&a.name, &b.name)),
            Self::SizeDesc => b.size.cmp(&a.size).then_with(|| natural(&a.name, &b.name)),
        }
    }
}
