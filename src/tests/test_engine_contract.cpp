// Contract smoke test for the Rust engine, run from the C++ consumption side.
//
// Ownership rule (agreed): this file owns what only a C++ consumer can verify —
// that the header matches the Rust #[repr(C)] structs (compile-time below),
// that every exported symbol links, and the basic async lifecycle works with
// plain C strings. Business semantics are asserted in Rust:
// engine/qimgv-engine-ffi/tests/ffi_contract.rs. One behavior, one owner.

#include "qimgv_engine.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// The five by-value structs are pinned in qimgv_engine.h itself, so the contract
// build and every production TU that includes the header check them; the Rust
// mirror lives in lib.rs and architecture_rules.py R5 compares the two.

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if(!(cond)) {                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while(0)

static std::string makeFixture() {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "qimgv_engine_contract_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "a.jpg") << "x";
    std::ofstream(dir / "b.jpg") << "x";
    std::ofstream(dir / "skip.txt") << "x";
    fs::create_directory(dir / "subdir");
    // the wire hands paths back in the form the caller opened with — Qt's
    // forward slashes on Windows — so the fixture is built forward too and
    // the string compares below mean the same thing on both platforms
    std::string out = dir.string();
#ifdef _WIN32
    std::replace(out.begin(), out.end(), '\\', '/');
#endif
    return out;
}

int main() {
    CHECK(qimgv_engine_version() != nullptr);
    CHECK(std::strlen(qimgv_engine_version()) > 0);

    qimgv_engine *engine = qimgv_engine_new(nullptr, nullptr, "jpg");
    CHECK(engine != nullptr);
    if(!engine)
        return 1;

    // fresh state
    CHECK(qimgv_engine_file_count(engine) == 0);
    CHECK(qimgv_engine_dir_count(engine) == 0);
    qimgv_event ev{};
    qimgv_mutation mut{};
    char dirbuf[512];
    char oldbuf[512];
    CHECK(qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf), oldbuf, sizeof(oldbuf), &mut) == 0);

    // error paths leave a readable diagnostic
    CHECK(qimgv_engine_set_sort(engine, 99) == QIMGV_ERR_BAD_SORT);
    CHECK(qimgv_last_error() != nullptr);
    CHECK(qimgv_engine_open_dir(engine, nullptr, 0, nullptr) == QIMGV_ERR_INVALID);

    // async open on a real directory
    std::string fixture = makeFixture();
    uint64_t openEpoch = 0;
    CHECK(qimgv_engine_open_dir(engine, fixture.c_str(), 0, &openEpoch) == QIMGV_OK);
    CHECK(openEpoch > 0);

    int got = 0;
    for(int i = 0; i < 1000; ++i) {
        got = qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf), oldbuf, sizeof(oldbuf), &mut);
        if(got != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got == 1);
    CHECK(ev.kind == QIMGV_EVENT_DIR_LISTED);
    CHECK(ev.code == QIMGV_OK);
    CHECK(ev.epoch == openEpoch);
    CHECK(std::strcmp(dirbuf, fixture.c_str()) == 0);

    CHECK(qimgv_engine_file_count(engine) == 2);
    qimgv_entry_meta meta{};
    char name[64], path[512];
    CHECK(qimgv_engine_file_entry_at(engine, 0, &meta, name, sizeof(name), path, sizeof(path)) == QIMGV_OK);
    CHECK(std::strcmp(name, "a.jpg") == 0);

    // undersized name buffer ("a.jpg\0" needs 6 bytes, 1 given)
    CHECK(qimgv_engine_file_entry_at(engine, 0, &meta, name, 1, path, sizeof(path)) == QIMGV_ERR_TRUNCATED);

    // --- remaining exported symbols ---------------------------------------
    // A symbol nothing calls here drops out of the link check entirely, which
    // is the one thing this file exists to verify. The semantics behind each
    // are owned by engine/qimgv-engine-ffi/tests/ffi_contract.rs.
    CHECK(qimgv_engine_dir_count(engine) == 1);
    qimgv_entry_meta dmeta{};
    CHECK(qimgv_engine_dir_entry_at(engine, 0, &dmeta, name, sizeof(name), path, sizeof(path)) == QIMGV_OK);
    CHECK(std::strcmp(name, "subdir") == 0);

    CHECK(qimgv_engine_set_show_hidden(engine, 1) == QIMGV_OK);
    CHECK(qimgv_engine_set_show_hidden(engine, 0) == QIMGV_OK);
    CHECK(qimgv_engine_include_path(engine, (fixture + "/subdir").c_str()) == QIMGV_OK);

    // --- rename: one mutation, two subjects ---------------------------------
    // The engine pairs the two inotify halves of a move by their shared cookie,
    // and the cookie exists nowhere else in the pipeline, so the old path needs
    // its own out-buffer. This is the only place the second buffer can be
    // checked from the consumer side, and getting it wrong is invisible to both
    // compilers: the struct still fits, the symbol still links, and the mirror
    // quietly keeps a row for a file that is now named something else.
    {
        const std::string from = fixture + "/b.jpg";
        const std::string to = fixture + "/z.jpg";
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        CHECK(!ec);

        // First with no room for the second subject at all: the rename has to
        // stay queued rather than be delivered as half an event. take_event
        // answers 1 (event written), 0 (nothing pending) or a positive
        // QIMGV_ERR_* code, and a refusal that reported 1 would be the worst
        // outcome — the consumer would apply the PREVIOUS event's subject and
        // mutation struct again, believing the rename had arrived.
        int refused = 0;
        for(int i = 0; i < 1000; ++i) {
            const int r = qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf), nullptr, 0, &mut);
            if(r != 0 && r != 1) {
                refused = r;
                break;
            }
            if(r == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(refused == QIMGV_ERR_INVALID);
        CHECK(mut.kind != QIMGV_MUT_RENAMED); // nothing written by a refused call

        // and the same event, now that both subjects have somewhere to go
        int took = 0;
        for(int i = 0; i < 200; ++i) {
            took = qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf), oldbuf, sizeof(oldbuf), &mut);
            if(took == 1)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(took == 1);
        CHECK(ev.kind == QIMGV_EVENT_DIR_MUTATED);
        CHECK(mut.kind == QIMGV_MUT_RENAMED);
        CHECK(std::strcmp(dirbuf, to.c_str()) == 0);
        if(std::strcmp(dirbuf, to.c_str()) != 0)
            std::printf("  [contract] dirbuf=[%s]  to=[%s]\n", dirbuf, to.c_str());
        CHECK(std::strcmp(oldbuf, from.c_str()) == 0);
        if(std::strcmp(oldbuf, from.c_str()) != 0)
            std::printf("  [contract] oldbuf=[%s]  from=[%s]\n", oldbuf, from.c_str());
        CHECK(mut.size == 1);
        // z.jpg after a.jpg in the engine's own order, and the event says so
        CHECK(mut.index == 1);

        bool sawOld = false, sawNew = false;
        for(uint32_t i = 0; i < uint32_t(qimgv_engine_file_count(engine)); ++i) {
            if(qimgv_engine_file_entry_at(engine, i, &meta, name, sizeof(name), path, sizeof(path)) != QIMGV_OK)
                continue;
            if(std::strcmp(path, from.c_str()) == 0)
                sawOld = true;
            if(std::strcmp(path, to.c_str()) == 0)
                sawNew = true;
        }
        CHECK(sawNew);
        CHECK(!sawOld);
        CHECK(qimgv_engine_file_count(engine) == 2);
    }

    CHECK(qimgv_engine_stop_watch(engine) == QIMGV_OK);

    // bulk export: the cap=0 probe must report the size, then fit exactly.
    // The arena's header is part of the contract this file owns — a consumer
    // reading it from the wrong offset sees a plausible but wrong listing, so
    // the fields are unpacked here rather than trusting the byte count alone.
    uint32_t needed = 0, fc = 0, dc = 0;
    uint64_t revision = 0;
    CHECK(qimgv_engine_export_listing(engine, nullptr, 0, &needed, &fc, &dc, &revision) == QIMGV_ERR_TRUNCATED);
    CHECK(needed > 32);
    std::vector<unsigned char> arena(needed);
    CHECK(qimgv_engine_export_listing(engine, arena.data(), arena.size(), &needed, &fc, &dc, &revision) == QIMGV_OK);
    CHECK(fc == 2);
    CHECK(dc == 1);
    // the revision travels with the bytes: two exports of an unchanged listing
    // report the same number, and it is not the "no listing" zero
    CHECK(revision != 0);
    uint32_t needed2 = 0, fc2 = 0, dc2 = 0;
    uint64_t revision2 = 0;
    CHECK(qimgv_engine_export_listing(engine, nullptr, 0, &needed2, &fc2, &dc2, &revision2) == QIMGV_ERR_TRUNCATED);
    CHECK(revision2 == revision);

    // little-endian readers for the arena header (qimgv_engine.h: u32
    // file_count, u32 dir_count, u64 session, u64 revision, i32 applied_sort,
    // u32 directory_len, then directory bytes, then the rows). The shift
    // counts carry u too: an unsuffixed literal is an int, and LLVM 23's
    // bugprone-signed-bitwise names the expression on it even when the other
    // operand is unsigned — the reads stay unsigned end to end, on both
    // toolchains.
    auto rd32 = [&arena](size_t off) {
        return uint32_t(arena[off]) | uint32_t(arena[off + 1]) << 8u | uint32_t(arena[off + 2]) << 16u |
               uint32_t(arena[off + 3]) << 24u;
    };
    auto rd64 = [&rd32](size_t off) {
        return uint64_t(rd32(off)) | uint64_t(rd32(off + 4)) << 32u;
    };
    CHECK(rd32(0) == 2); // file_count
    CHECK(rd32(4) == 1); // dir_count
    // the header's session is the open session the events carried, and its
    // revision is the number the export also returned: the copy names the
    // listing it came from
    CHECK(rd64(8) == openEpoch);
    CHECK(rd64(16) == revision);
    CHECK(int32_t(rd32(24)) == QIMGV_SORT_NAME);
    const uint32_t dirLen = rd32(28);
    std::string arenaDir(reinterpret_cast<const char *>(arena.data()) + 32, dirLen);
    CHECK(arenaDir == fixture);
    // the rows start after the directory; the first record is a file, and its
    // id/content_revision are what the mirror keys and diffs on — an id of 0
    // would make every row look like a fresh arrival
    const size_t row0 = 32 + size_t(dirLen);
    CHECK(rd64(row0) != 0);      // id
    CHECK(rd64(row0 + 8) != 0);  // content_revision
    CHECK(rd64(row0 + 16) == 1); // size ("x")
    // every listed id is distinct: identity is the whole point of the field
    {
        std::vector<uint64_t> ids;
        size_t off = row0;
        for(uint32_t r = 0; r < fc + dc; ++r) {
            ids.push_back(rd64(off));
            const uint32_t nameLen = rd32(off + 32);
            const uint32_t pathLen = rd32(off + 36);
            off += 40 + nameLen + pathLen;
        }
        CHECK(off == arena.size()); // the arena is consumed exactly
        std::sort(ids.begin(), ids.end());
        CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
    }

    qimgv_engine_free(engine);

    // --- pure name comparator ----------------------------------------------
    // qimgv_engine_compare_names is the engine's own order exposed to a caller
    // that sorts outside the mirror (sibling navigation). It is pure, so it is
    // checked here without an engine instance — and it has to agree with the
    // listing's order, or the two disagree on names that differ only in case.
    {
        // digits sort by numeric value: "img2" before "img10"
        CHECK(qimgv_engine_compare_names("img2.jpg", "img10.jpg", QIMGV_SORT_NAME) < 0);
        // case is a tiebreak, not the primary key: the ascending and descending
        // modes are exact mirror images of each other
        const int ab = qimgv_engine_compare_names("abc", "abc", QIMGV_SORT_NAME);
        CHECK(ab == 0);
        const int lt = qimgv_engine_compare_names("a", "b", QIMGV_SORT_NAME);
        CHECK(lt < 0);
        const int gt = qimgv_engine_compare_names("b", "a", QIMGV_SORT_NAME);
        CHECK(gt > 0);
        CHECK(qimgv_engine_compare_names("a", "b", QIMGV_SORT_NAME_DESC) > 0);
        // the comparator must be a strict weak order, not merely antisymmetric:
        // sorting a shuffled batch with it produces the engine's order
        std::vector<std::string> names = {"img10.jpg", "img2.jpg", "img1.jpg", "IMG2.jpg"};
        std::sort(names.begin(), names.end(), [](const std::string &x, const std::string &y) {
            return qimgv_engine_compare_names(x.c_str(), y.c_str(), QIMGV_SORT_NAME) < 0;
        });
        // the engine's tiebreak puts the uppercase variant first within a
        // case-insensitive tie (natural() falls through to a byte compare) —
        // this is the exact pair a QCollator orders the other way round, which
        // is why sibling navigation asks the engine instead of a collator
        CHECK(qimgv_engine_compare_names("IMG2.jpg", "img2.jpg", QIMGV_SORT_NAME) < 0);
        CHECK(names.front() == "img1.jpg");
        CHECK(names.back() == "img10.jpg");
        // invalid (null) inputs answer 0 rather than crashing across the ABI
        CHECK(qimgv_engine_compare_names(nullptr, "x", QIMGV_SORT_NAME) == 0);
    }

    // --- probe: pure function, no engine instance needed -------------------
    qimgv_probe_result pr{};
    // CRC-free PNG is fine — the header parsers don't validate chunk CRCs
    auto chunk = [](const char tag[4], const unsigned char *data, uint32_t len) {
        std::string c;
        for(unsigned i = 4; i-- > 0;)
            c.push_back(char((len >> (8 * i)) & 0xffu));
        c.append(tag, 4);
        c.append(reinterpret_cast<const char *>(data), len);
        return c;
    };
    unsigned char ihdr[13] = {0, 0, 0, 40, 0, 0, 0, 20, 8, 2, 0, 0, 0}; // 40x20 RGB
    std::string png = "\x89PNG\r\n\x1a\n";
    png += chunk("IHDR", ihdr, 13);
    png += chunk("IDAT", reinterpret_cast<const unsigned char *>(""), 0);
    png += chunk("IEND", reinterpret_cast<const unsigned char *>(""), 0);
    {
        std::filesystem::path pp = std::filesystem::temp_directory_path() / "qimgv_contract_probe.png";
        std::ofstream(pp, std::ios::binary).write(png.data(), png.size());
        CHECK(qimgv_engine_probe(pp.string().c_str(), &pr) == QIMGV_OK);
        CHECK(pr.format == QIMGV_FMT_PNG);
        CHECK(pr.width == 40 && pr.height == 20);
        CHECK(pr.animated == 0);
        CHECK(pr.orientation >= 1 && pr.orientation <= 8);
        CHECK(qimgv_engine_probe(nullptr, &pr) == QIMGV_ERR_INVALID);
    }

    // watcher support query must answer consistently with mutation events
    CHECK(qimgv_engine_supports_watching() == 1);

    std::printf(failures == 0 ? "engine contract: OK\n" : "engine contract: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
