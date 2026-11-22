#include "gtest_qt.h"
#include "engine_stack_fixture.h"

// the listing arena: model.rs::write_listing against EngineBridge::parseListing,
// written independently of each other, and the SortingMode -> QIMGV_SORT_* map
class TestWireFormat : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestWireFormat() : EngineStackFixture("wire-format") {
    }
};

// model.rs::write_listing is the only producer and EngineBridge::parseListing
// the only consumer, written independently of each other. Drift between them
// shows up as a short or garbled listing, never as a crash — so it has to be
// asserted by round-tripping the real bytes.

TEST_F(TestWireFormat, listingArenaRoundTripsThroughTheCppParser) {
    resetFixture();
    // multi-byte UTF-8: the arena stores BYTE lengths, so this is what
    // catches a producer/consumer disagreement about chars vs bytes
    makeFile(QStringLiteral("únicøde-日本.jpg"));
    makeFile(QString(120, QChar('x')) + QStringLiteral(".jpg")); // past any fixed buffer

    auto &bridge = EngineBridge::instance();
    openListed(bridge, m_fixtureDir, 0, false);

    EXPECT_EQ(bridge.fileCount(), 5);
    EXPECT_EQ(bridge.dirCount(), 1);

    QByteArray arena;
    quint32 fc = 0, dc = 0;
    quint64 revision = 0;
    EXPECT_TRUE(bridge.exportListing(arena, fc, dc, revision));
    EXPECT_EQ(int(fc), bridge.fileCount());
    EXPECT_EQ(int(dc), bridge.dirCount());

    EngineBridge::Snapshot snapshot;
    EXPECT_TRUE(EngineBridge::parseListing(arena, snapshot));
    EXPECT_EQ(snapshot.directory, m_fixtureDir);
    EXPECT_EQ(snapshot.session, bridge.session());
    EXPECT_EQ(snapshot.revision, revision);
    EXPECT_EQ(snapshot.sort, QIMGV_SORT_NAME);
    const auto &files = snapshot.files;
    const auto &dirs = snapshot.dirs;
    EXPECT_EQ(int(files.size()), int(fc));
    EXPECT_EQ(int(dirs.size()), int(dc));

    // field-by-field against the engine's own index-based reader: same
    // source of truth, two independent code paths
    for(size_t i = 0; i < files.size(); ++i) {
        EngineBridge::EngineEntry ref;
        EXPECT_TRUE(bridge.entryAt(int(i), false, ref));
        EXPECT_EQ(files[i].name, ref.name);
        EXPECT_EQ(files[i].path, ref.path);
        EXPECT_EQ(files[i].size, ref.size);
        EXPECT_EQ(files[i].mtimeMs, ref.mtimeMs);
        EXPECT_FALSE(files[i].isDir);
        EXPECT_NE(files[i].id, 0U);
    }
    EngineBridge::EngineEntry dirRef;
    EXPECT_TRUE(bridge.entryAt(0, true, dirRef));
    EXPECT_EQ(dirs[0].name, dirRef.name);
    EXPECT_EQ(dirs[0].path, dirRef.path);
    EXPECT_EQ(dirs[0].isDir, true);
}

TEST_F(TestWireFormat, unicodeNamesSurviveTheArenaByteForByte) {
    resetFixture();
    const QString wide = QStringLiteral("日本-ünicode-🎨.jpg");
    makeFile(wide);

    auto &bridge = EngineBridge::instance();
    openListed(bridge, m_fixtureDir, 0, false);

    EXPECT_EQ(bridge.fileCount(), 4);

    QByteArray arena;
    quint32 fc = 0, dc = 0;
    quint64 revision = 0;
    EXPECT_TRUE(bridge.exportListing(arena, fc, dc, revision));
    EngineBridge::Snapshot snapshot;
    EXPECT_TRUE(EngineBridge::parseListing(arena, snapshot));

    bool found = false;
    for(const auto &f : snapshot.files) {
        if(f.name == wide) {
            found = true;
            EXPECT_EQ(f.path, m_fixtureDir + QStringLiteral("/") + wide);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(TestWireFormat, truncatedArenaStopsAtARecordBoundary) {
    resetFixture();
    auto &bridge = EngineBridge::instance();
    openListed(bridge, m_fixtureDir, 0, false);

    EXPECT_EQ(bridge.fileCount(), 3);

    QByteArray arena;
    quint32 fc = 0, dc = 0;
    quint64 revision = 0;
    EXPECT_TRUE(bridge.exportListing(arena, fc, dc, revision));
    EngineBridge::Snapshot full;
    EXPECT_TRUE(EngineBridge::parseListing(arena, full));
    EXPECT_EQ(int(full.files.size()), 3);
    EXPECT_EQ(int(full.dirs.size()), 1);

    // Cut into the final record. A rejected arena yields NOTHING: a prefix
    // that decoded cleanly is indistinguishable, at the call site, from a
    // directory that genuinely has fewer entries — and the mirror would
    // install it as such (missing entries in the view, in the counts and in
    // end-of-folder navigation, with nothing crashed to point at).
    EngineBridge::Snapshot snapshot;
    EXPECT_TRUE(!EngineBridge::parseListing(arena.left(arena.size() - 3), snapshot));
    EXPECT_TRUE(snapshot.files.empty());
    EXPECT_TRUE(snapshot.dirs.empty());

    // and a header that overstates the records is rejected the same way
    const quint32 overstated = fc + 1;
    QByteArray lyingHeader = arena;
    lyingHeader.replace(0, 4, QByteArray::fromRawData(reinterpret_cast<const char *>(&overstated), 4));
    EXPECT_TRUE(!EngineBridge::parseListing(lyingHeader, snapshot));
    EXPECT_TRUE(snapshot.files.empty());
    EXPECT_TRUE(snapshot.dirs.empty());
}

TEST_F(TestWireFormat, headerCountsDisagreeingWithTheArenaAreRejected) {
    // the failure mode when producer and consumer drift: a header promising
    // entries the bytes do not contain. Must not allocate for them, must not
    // report success with a silently empty listing
    QByteArray arena(32, '\0');
    auto *h = reinterpret_cast<unsigned char *>(arena.data());
    for(int i = 0; i < 4; ++i)
        h[i] = 0xff; // file_count = 0xffffffff
    // dir_count stays 0

    EngineBridge::Snapshot snapshot;
    EXPECT_TRUE(!EngineBridge::parseListing(arena, snapshot));
    EXPECT_TRUE(snapshot.files.empty());
    EXPECT_TRUE(snapshot.dirs.empty());

    // an arena shorter than the header is not a listing at all
    EXPECT_TRUE(!EngineBridge::parseListing(QByteArray(), snapshot));
    EXPECT_TRUE(!EngineBridge::parseListing(arena.left(7), snapshot));

    // header with zero counts IS a complete empty listing
    const QByteArray empty(32, '\0');
    EXPECT_TRUE(EngineBridge::parseListing(empty, snapshot));
    EXPECT_TRUE(snapshot.files.empty());
}

// settings.h orders the modes name, name-desc, size, size-desc, time,
// time-desc; the wire codes swap the size and time pairs. Mapping them
// positionally sorts by the wrong column, which never crashes and always
// looks plausible. The behavior behind each code is pinned from the Rust
// side (ffi_contract.rs::sort_codes_map_to_the_documented_orders).

TEST_F(TestWireFormat, sortModeMappingMatchesTheWireCodes) {
    // the codes are a documented wire format: pin the values themselves, the
    // same way test_engine_contract.cpp pins struct layouts
    EXPECT_EQ(QIMGV_SORT_NAME, 0);
    EXPECT_EQ(QIMGV_SORT_NAME_DESC, 1);
    EXPECT_EQ(QIMGV_SORT_TIME, 2);
    EXPECT_EQ(QIMGV_SORT_TIME_DESC, 3);
    EXPECT_EQ(QIMGV_SORT_SIZE, 4);
    EXPECT_EQ(QIMGV_SORT_SIZE_DESC, 5);

    EXPECT_EQ(DirectoryManager::engineSortMode(SORT_NAME), QIMGV_SORT_NAME);
    EXPECT_EQ(DirectoryManager::engineSortMode(SORT_NAME_DESC), QIMGV_SORT_NAME_DESC);
    EXPECT_EQ(DirectoryManager::engineSortMode(SORT_TIME), QIMGV_SORT_TIME);
    EXPECT_EQ(DirectoryManager::engineSortMode(SORT_TIME_DESC), QIMGV_SORT_TIME_DESC);
    EXPECT_EQ(DirectoryManager::engineSortMode(SORT_SIZE), QIMGV_SORT_SIZE);
    EXPECT_EQ(DirectoryManager::engineSortMode(SORT_SIZE_DESC), QIMGV_SORT_SIZE_DESC);
}
