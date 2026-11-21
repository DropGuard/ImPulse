use std::time::{Duration, SystemTime};

use qimgv_engine::{DirectoryState, EntryData, SortMode};

fn entry(name: &str, size: u64, age_secs: u64) -> EntryData {
    EntryData {
        name: name.to_owned(),
        path: format!("/tmp/{name}"),
        is_dir: false,
        size,
        mtime: SystemTime::now() - Duration::from_secs(age_secs),
    }
}

#[test]
fn natural_name_order() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![
            entry("img10.jpg", 1, 0),
            entry("img2.jpg", 1, 0),
            entry("img1.jpg", 1, 0),
        ],
        vec![],
    );
    let names: Vec<&str> = (0..3)
        .map(|i| st.file_at(i).unwrap().name.as_str())
        .collect();
    assert_eq!(names, ["img1.jpg", "img2.jpg", "img10.jpg"]);
}

#[test]
fn name_desc_reverses() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![entry("a.jpg", 1, 0), entry("b.jpg", 1, 0)],
        vec![],
    );
    st.resort(SortMode::NameDesc);
    let names: Vec<&str> = (0..2)
        .map(|i| st.file_at(i).unwrap().name.as_str())
        .collect();
    assert_eq!(names, ["b.jpg", "a.jpg"]);
}

#[test]
fn size_and_time_sorts() {
    let mut st = DirectoryState::new(SortMode::Size);
    st.replace_all(
        "/tmp".into(),
        vec![entry("big.jpg", 100, 0), entry("small.jpg", 1, 0)],
        vec![],
    );
    assert_eq!(st.file_at(0).unwrap().name, "small.jpg");
    st.resort(SortMode::SizeDesc);
    assert_eq!(st.file_at(0).unwrap().name, "big.jpg");

    st.resort(SortMode::Time);
    // entry() ages newer files less; Time ascending = oldest first
    let mut st2 = DirectoryState::new(SortMode::Time);
    st2.replace_all(
        "/tmp".into(),
        vec![entry("new.jpg", 1, 10), entry("old.jpg", 1, 100)],
        vec![],
    );
    assert_eq!(st2.file_at(0).unwrap().name, "old.jpg");
}

#[test]
fn insert_entry_reports_the_position_it_chose() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![entry("a.jpg", 1, 0), entry("c.jpg", 1, 0)],
        vec![],
    );
    let pos = st.insert_entry(entry("b.jpg", 1, 0)).unwrap();
    assert_eq!(pos, 1);
    let names: Vec<&str> = (0..3)
        .map(|i| st.file_at(i).unwrap().name.as_str())
        .collect();
    assert_eq!(names, ["a.jpg", "b.jpg", "c.jpg"]);
}

#[test]
fn meta_update_repositions_under_size_sort() {
    // without the re-seating, a size change under SIZE sort leaves the order
    // vectors unsorted — the export itself stops agreeing with the comparator
    let mut st = DirectoryState::new(SortMode::Size);
    st.replace_all(
        "/tmp".into(),
        vec![entry("big.jpg", 100, 0), entry("small.jpg", 1, 0)],
        vec![],
    );
    let mtime = SystemTime::now();
    let pos = st.update_entry_meta("/tmp/small.jpg", 500, mtime).unwrap();
    assert_eq!(pos, 1);
    assert_eq!(st.file_at(0).unwrap().name, "big.jpg");
    assert_eq!(st.file_at(1).unwrap().name, "small.jpg");
}

#[test]
fn meta_update_keeps_the_position_under_name_sort() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all("/tmp".into(), vec![entry("a.jpg", 1, 0)], vec![]);
    let pos = st
        .update_entry_meta("/tmp/a.jpg", 9, SystemTime::now())
        .unwrap();
    assert_eq!(pos, 0);
}

#[test]
fn a_rename_reseats_even_under_name_sort() {
    // The opposite answer to the test above, and the reason rename_entry is a
    // method of its own: a metadata change cannot move a name-sorted row, but
    // a rename IS a change of the thing the name sort compares.
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![entry("img2.jpg", 1, 0), entry("img3.jpg", 1, 0)],
        vec![],
    );
    let pos = st
        .rename_entry(
            "/tmp/img3.jpg",
            "/tmp/img1.jpg",
            "img1.jpg",
            1,
            SystemTime::now(),
        )
        .unwrap();
    assert_eq!(pos, 0);
    let names: Vec<&str> = (0..2)
        .map(|i| st.file_at(i).unwrap().name.as_str())
        .collect();
    assert_eq!(names, ["img1.jpg", "img2.jpg"]);
}

#[test]
fn a_rename_is_a_move_not_a_death() {
    // To the engine a rename is one entry that moved, not an entry that died
    // and a second one born next door: the row survives with its new name, the
    // listing keeps its size, and the old path is gone. The old wire carried a
    // handle to say the same thing; the handle retired with the FFI navigation
    // nothing used, and the row-level contract is what these asserts keep.
    let mut st = DirectoryState::new(SortMode::Name);
    st.insert_entry(entry("img2.jpg", 1, 0)).unwrap();
    st.insert_entry(entry("img1.jpg", 1, 0)).unwrap();
    let mtime = SystemTime::now();
    let pos = st
        .rename_entry("/tmp/img2.jpg", "/tmp/img0.jpg", "img0.jpg", 1, mtime)
        .unwrap();
    assert_eq!(pos, 0);
    assert_eq!(st.file_at(0).unwrap().path, "/tmp/img0.jpg");
    assert_eq!(
        st.file_count(),
        2,
        "a rename does not grow or shrink the listing"
    );
    assert!(st.contains_path("/tmp/img0.jpg"));
    assert!(!st.contains_path("/tmp/img2.jpg"));
}

#[test]
fn a_rename_refuses_a_destination_the_listing_holds() {
    // Two rows cannot share a path, and the state has no way to pick which one
    // dies — that decision belongs to the event applier, which takes the old
    // destination row out first and then renames into it. Refusing is what keeps
    // the ordering a listing from ever holding the same path twice.
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![entry("a.jpg", 1, 0), entry("b.jpg", 2, 0)],
        vec![],
    );
    assert_eq!(
        st.rename_entry("/tmp/a.jpg", "/tmp/b.jpg", "b.jpg", 1, SystemTime::now()),
        None
    );
    assert_eq!(st.file_count(), 2, "a refusal changes nothing");
    assert_eq!(st.file_at(0).unwrap().name, "a.jpg");
}
