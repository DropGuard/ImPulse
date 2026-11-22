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
/// codepoint comparison sorts a digit-led name before a bracket-led one,
/// while the desktop collation puts the bracket first — the mismatch that
/// motivated this comparator. Digit runs compare by
/// numeric value ("2" < "10"); full ICU collation is still deliberately
/// out of scope, this is its practical core.
// The class weight behind both natural() and sort_key(): punctuation before
// digits before letters, ASCII by range check, the rest by the Unicode
// tables.
fn class(c: char) -> u8 {
    if c.is_ascii() {
        if c.is_ascii_digit() {
            2 // digits
        } else if c.is_ascii_alphabetic() {
            3 // letters
        } else {
            1 // punctuation, symbols, whitespace
        }
    } else if c.is_numeric() {
        2 // digits
    } else if c.is_alphabetic() {
        3 // letters, CJK ideographs included
    } else {
        1 // whitespace, punctuation, symbols
    }
}

pub fn natural(a: &str, b: &str) -> Ordering {
    type Chars<'a> = std::iter::Peekable<std::str::Chars<'a>>;

    // consumes the digit run the cursor stands on
    fn consume_run<'a>(it: &mut Chars<'a>) {
        while it.peek().is_some_and(|c| c.is_numeric()) {
            it.next();
        }
    }

    // first differing digit of two significant parts of equal length: the
    // first gap decides, exactly like the old string compare did digit-wise
    fn digit_order<'a>(x: &mut Chars<'a>, y: &mut Chars<'a>) -> Ordering {
        loop {
            match (x.peek().copied(), y.peek().copied()) {
                (Some(dx), Some(dy)) if dx.is_numeric() && dy.is_numeric() => {
                    if dx != dy {
                        return dx.cmp(&dy);
                    }
                    x.next();
                    y.next();
                }
                _ => return Ordering::Equal,
            }
        }
    }

    // The comparator is allocation-free ON PURPOSE: one cold open sorts
    // 15k entries at ~N log N comparisons, and the previous version
    // materialized a Vec<(char, class)> per string per comparison — measured
    // 1338 ms of insert/sort on a 15413-entry directory, dwarfing the 36 ms
    // scan. Chars iterators clone for free (a pointer and a length), so the
    // digit-run probes below walk clones while the real cursors consume.
    let mut ai = a.chars().peekable();
    let mut bi = b.chars().peekable();
    loop {
        let (Some(ca), Some(cb)) = (ai.peek().copied(), bi.peek().copied()) else {
            break;
        };
        let (ka, kb) = (class(ca), class(cb));
        if ka != kb {
            return ka.cmp(&kb);
        }
        if ka == 2 {
            // digit runs compare by numeric value: strip leading zeros,
            // then longer wins, then digit-wise. The probes skip zeros and
            // measure/walk only the significant parts; both REAL cursors
            // consume the full runs (zeros included) whatever the verdict —
            // equal-valued runs of different widths stay equal and keep
            // walking.
            let mut pa = ai.clone();
            let mut pb = bi.clone();
            while pa.peek().copied() == Some('0') {
                pa.next();
            }
            while pb.peek().copied() == Some('0') {
                pb.next();
            }
            let sa = pa.clone().take_while(|c| c.is_numeric()).count();
            let sb = pb.clone().take_while(|c| c.is_numeric()).count();
            let mut ord = sa.cmp(&sb);
            if ord == Ordering::Equal && sa > 0 {
                ord = digit_order(&mut pa, &mut pb);
            }
            consume_run(&mut ai);
            consume_run(&mut bi);
            if ord != Ordering::Equal {
                return ord;
            }
            continue;
        }
        // constructing the ToLowercase iterator is a table search per call;
        // non-cased chars (CJK, digits, punctuation) need none of it
        let la = if ca.is_ascii() {
            ca.to_ascii_lowercase()
        } else if ca.is_uppercase() {
            ca.to_lowercase().next().unwrap_or(ca)
        } else {
            ca
        };
        let lb = if cb.is_ascii() {
            cb.to_ascii_lowercase()
        } else if cb.is_uppercase() {
            cb.to_lowercase().next().unwrap_or(cb)
        } else {
            cb
        };
        if la != lb {
            return la.cmp(&lb);
        }
        ai.next();
        bi.next();
    }
    ai.count().cmp(&bi.count()).then_with(|| a.cmp(b))
}

/// The exact byte encoding of natural()'s order: class-weighted positions
/// (punctuation < digits < letters), digit runs as single big-endian
/// length-prefixed numeric units, the lowered char's UTF-8 for everything
/// else, and the raw name bytes as the full-string tiebreak. Sorting by
/// these keys decides identically to sorting by natural() — the tests pin
/// the agreement across a mixed corpus — and one comparison is a memcmp
/// instead of a character walk. Built once per row (name changes are the
/// only invalidation), because a cold open sorts tens of thousands of names
/// and per-comparison character walks measured at hundreds of milliseconds.
pub fn sort_key(name: &str) -> Vec<u8> {
    let mut key = Vec::with_capacity(name.len() * 2 + 8);
    let mut chars = name.chars().peekable();
    while let Some(c) = chars.peek().copied() {
        let k = class(c);
        if k == 2 {
            // the whole run is one numeric unit: significant length (BE, so
            // length compare is numeric), then the significant digits —
            // equal-length digit strings compare numerically by bytes
            let mut run = String::new();
            while let Some(d) = chars.peek().copied() {
                if !d.is_numeric() {
                    break;
                }
                run.push(d);
                chars.next();
            }
            let sig = run.trim_start_matches('0');
            key.push(2);
            key.extend_from_slice(&(sig.len() as u32).to_be_bytes());
            key.extend_from_slice(sig.as_bytes());
        } else {
            chars.next();
            key.push(k);
            // constructing the ToLowercase iterator is a table search per
            // call; non-cased chars (CJK, digits, punctuation) need none of it
            let lowered = if c.is_ascii() {
                c.to_ascii_lowercase()
            } else if c.is_uppercase() {
                c.to_lowercase().next().unwrap_or(c)
            } else {
                c
            };
            let mut buf = [0u8; 4];
            key.extend_from_slice(lowered.encode_utf8(&mut buf).as_bytes());
        }
    }
    key.push(0); // the natural part ends here; the raw bytes are the tiebreak
    key.extend_from_slice(name.as_bytes());
    key
}

impl SortMode {
    /// The natural()-based comparator: the model sorts by cached keys
    /// (sort_key) instead of calling this per comparison, but the property
    /// tests keep it as the semantic oracle — model order built from keys
    /// must match this order everywhere, and a divergence fails by name.
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
