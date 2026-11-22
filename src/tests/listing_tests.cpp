#include <QThread>
#include "gtest_qt.h"
#include "engine_stack_fixture.h"

#include <cstdio> // std::rename: RENAME_NOREPLACE is QFile::rename, and an
                  // overwrite is exactly what the paired event is for
#ifdef Q_OS_WIN
#include <qt_windows.h> // MoveFileExW: mingw's rename() has no atomic replace
#endif

// the bridge event pump, DirectoryManager's async open, the deferred-listing
// primitive and the single-writer rule
class TestListing : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestListing() : EngineStackFixture("listing") {
    }
};

TEST_F(TestListing, engineBridgePumpDeliversListed) {
    resetFixture();
    auto &bridge = EngineBridge::instance();
    QSignalSpy listed(&bridge, &EngineBridge::dirListed);
    EXPECT_TRUE(listed.isValid());

    EXPECT_TRUE(bridge.openDir(m_fixtureDir, 0 /*QIMGV_SORT_NAME*/, false));
    ASSERT_TRY_EQ_TIMEOUT(listed.count(), 1, 10000);
    // the event, not a count, is what says this listing is ours
    EXPECT_EQ(bridge.listedDirectory(), m_fixtureDir);

    EXPECT_EQ(bridge.fileCount(), 3); // txt filtered
    EXPECT_EQ(bridge.dirCount(), 1);
    EngineBridge::EngineEntry e;
    EXPECT_TRUE(bridge.entryAt(0, false, e));
    EXPECT_EQ(e.name, QStringLiteral("img1.jpg")); // natural order: 1 < 2 < 10
    EXPECT_TRUE(bridge.entryAt(2, false, e));
    EXPECT_EQ(e.name, QStringLiteral("img10.jpg"));
}

TEST_F(TestListing, directoryManagerOpenIsAsynchronous) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    QSignalSpy loaded(&dm, &DirectoryManager::loaded);
    QSignalSpy failed(&dm, &DirectoryManager::loadingFailed);

    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    // THE async assertion: listing must NOT be there yet (old code was sync)
    EXPECT_EQ(dm.fileCount(), 0UL);
    EXPECT_EQ(loaded.count(), 0);

    ASSERT_TRY_EQ_TIMEOUT(loaded.count(), 1, 10000);
    EXPECT_EQ(dm.fileCount(), 3UL);
    EXPECT_EQ(dm.dirCount(), 1UL);
    EXPECT_EQ(dm.filePathAt(0), m_fixtureDir + "/img1.jpg");
    EXPECT_EQ(dm.filePathAt(2), m_fixtureDir + "/img10.jpg");
    EXPECT_EQ(failed.count(), 0);
}

TEST_F(TestListing, aSortIsAPermutationNotARelisting) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    dm.setSortingMode(SORT_NAME);
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);
    const QList<quint64> original{dm.fileEntryAt(0).id, dm.fileEntryAt(1).id, dm.fileEntryAt(2).id};

    dm.setSortingMode(SORT_NAME_DESC);
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);

    EXPECT_EQ(dm.fileCount(), 3UL);
    EXPECT_EQ(dm.fileEntryAt(0).id, original.at(2));
    EXPECT_EQ(dm.fileEntryAt(1).id, original.at(1));
    EXPECT_EQ(dm.fileEntryAt(2).id, original.at(0));
    EXPECT_EQ(dm.filePathAt(0), m_fixtureDir + "/img10.jpg");
    EXPECT_EQ(dm.filePathAt(2), m_fixtureDir + "/img1.jpg");
}

// The revision is what makes "the mirror and the engine at one instant" a
// question with an answer. Every path that changes the rows moves it — the
// initial listing, a watcher mutation, a sort — and it always equals the
// number the engine reports for the listing it came from.
TEST_F(TestListing, theMirrorRevisionTracksTheEngineItApplied) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    auto &bridge = EngineBridge::instance();
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);

    auto exportRevision = [&bridge]() {
        QByteArray arena;
        quint32 fc = 0, dc = 0;
        quint64 rev = 0;
        return bridge.exportListing(arena, fc, dc, rev) ? rev : 0;
    };

    ASSERT_TRY_TRUE_TIMEOUT(dm.appliedRevision() != 0, 10000);
    ASSERT_TRY_EQ_TIMEOUT(dm.appliedRevision(), exportRevision(), 10000);

    const quint64 afterListing = dm.appliedRevision();

    // a watcher mutation moves both
    makeFile("revision-moves.jpg");
    ASSERT_TRY_TRUE_TIMEOUT(dm.containsFile(m_fixtureDir + "/revision-moves.jpg"), 10000);
    EXPECT_TRUE(dm.appliedRevision() > afterListing) << "a mutation did not move the mirror's revision";
    ASSERT_TRY_EQ_TIMEOUT(dm.appliedRevision(), exportRevision(), 10000);

    // and a sort does too, without touching the row set
    const quint64 afterMutation = dm.appliedRevision();
    dm.setSortingMode(SORT_NAME_DESC);
    ASSERT_TRY_TRUE_TIMEOUT(dm.appliedRevision() > afterMutation, 10000);
    ASSERT_TRY_EQ_TIMEOUT(dm.appliedRevision(), exportRevision(), 10000);
}

// Sibling navigation must walk the order the view shows. The view's order is
// the engine's; a QCollator approximates it and disagrees on names that differ
// only in case — measured on {IMG2, img2}, where the collator puts the
// lowercase first and the engine puts the uppercase first. A sibling lookup
// that re-derived the order landed on a different folder than the one next to
// the current row.
TEST_F(TestListing, siblingNavigationUsesTheEnginesNameOrder) {
    resetFixture();
    // two siblings whose names differ only in case: the case-sensitive
    // filesystem keeps them distinct, and the engine's tiebreak decides
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("CaseDir"));
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("casedir"));

    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);

    // whatever the view's three rows are, reading them and walking the
    // siblings forward must produce the same sequence
    QStringList rows;
    for(unsigned long i = 0; i < dm.dirCount(); ++i)
        rows << dm.dirNameAt(int(i));
    QStringList walked;
    QString cur = dm.dirPathAt(0);
    while(!cur.isEmpty() && walked.size() <= rows.size()) {
        walked << QFileInfo(cur).fileName();
        cur = dm.adjacentDirectory(cur, true, SORT_NAME);
    }
    EXPECT_EQ(walked, rows) << "the sibling walk and the rows disagreed";
    // the case pair is the part a collator gets wrong: both are present, and
    // their relative order is the engine's, not ICU's
    EXPECT_TRUE(rows.contains(QStringLiteral("CaseDir")));
    EXPECT_TRUE(rows.contains(QStringLiteral("casedir")));
}

TEST_F(TestListing, missingDirectoryFailsFast) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(!dm.setDirectory("/nonexistent/qimgv-test-42"));
}

TEST_F(TestListing, sortingResortsAsynchronously) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    QSignalSpy resort(&dm, &DirectoryManager::sortingChanged);
    dm.setSortingMode(SORT_NAME_DESC);
    ASSERT_TRY_EQ_TIMEOUT(resort.count(), 1, 10000);
    EXPECT_EQ(dm.filePathAt(0), m_fixtureDir + "/img10.jpg");
    EXPECT_EQ(dm.filePathAt(2), m_fixtureDir + "/img1.jpg");
}

TEST_F(TestListing, liveFileCreationArrivesViaWatcher) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    // a file appears while the directory is open; on Linux the engine
    // watcher applies it incrementally (no rescan involved)
    makeFile("latecomer.jpg");
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 4UL, 10000);
    // the engine listing is authoritative: natural order kept
    EXPECT_EQ(dm.filePathAt(0), m_fixtureDir + "/img1.jpg");
    EXPECT_EQ(dm.filePathAt(3), m_fixtureDir + "/latecomer.jpg");
}

TEST_F(TestListing, liveFileDeletionLeavesTheSurvivingRowsInOrder) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    EXPECT_TRUE(QFile::remove(m_fixtureDir + "/img2.jpg"));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 2UL, 10000);
    EXPECT_FALSE(dm.containsFile(m_fixtureDir + "/img2.jpg"));
    EXPECT_EQ(dm.filePathAt(0), m_fixtureDir + "/img1.jpg");
    EXPECT_EQ(dm.filePathAt(1), m_fixtureDir + "/img10.jpg");
}

TEST_F(TestListing, liveFileModificationKeepsTheMirrorEntryWhole) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    const QString path = m_fixtureDir + "/img2.jpg";
    const int index = dm.indexOfFile(path);
    ASSERT_GE(index, 0);
    const auto old = dm.fileEntryAt(index);
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Append));
    EXPECT_TRUE(f.write("appended") == 8);
    f.close();

    ASSERT_TRY_EQ_TIMEOUT(dm.fileEntryAt(index).size, 9UL, 10000);
    EXPECT_EQ(dm.fileEntryAt(index).id, old.id);
    EXPECT_GT(dm.fileEntryAt(index).contentRevision, old.contentRevision);
    EXPECT_EQ(dm.fileEntryAt(index).name, QStringLiteral("img2.jpg"));
    EXPECT_EQ(dm.fileEntryAt(index).path, path);
    EXPECT_TRUE(!dm.fileEntryAt(index).isDirectory);
}

TEST_F(TestListing, liveRenameMovesTheSameEntryWithoutChangingItsContent) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);
    const int oldIndex = dm.indexOfFile(m_fixtureDir + "/img2.jpg");
    ASSERT_GE(oldIndex, 0);
    const auto old = dm.fileEntryAt(oldIndex);

    const QString renamed = m_fixtureDir + "/renamed.jpg";
    EXPECT_TRUE(QFile::rename(old.path, renamed));
    ASSERT_TRY_TRUE_TIMEOUT(dm.containsFile(renamed), 10000);

    EXPECT_EQ(dm.fileCount(), 3UL);
    const int newIndex = dm.indexOfFile(renamed);
    EXPECT_TRUE(newIndex != oldIndex);
    EXPECT_FALSE(dm.containsFile(old.path));
    EXPECT_EQ(dm.fileEntryAt(newIndex).id, old.id);
    EXPECT_EQ(dm.fileEntryAt(newIndex).contentRevision, old.contentRevision);
    EXPECT_EQ(dm.fileEntryAt(newIndex).name, QStringLiteral("renamed.jpg"));
    EXPECT_EQ(dm.fileEntryAt(newIndex).size, 1UL);
    assertMirrorMatchesEngineExport(dm);
}

TEST_F(TestListing, overwriteRenameReplacesTheListedRow) {
    // rename(2) onto a path that is already listed. The kernel sends
    // MOVED_FROM + MOVED_TO all the same, and until the pair was answered
    // this was invisible for good: MOVED_TO on a listed path was
    // Ignored(AlreadyListed), so the destination row kept the size, mtime
    // and picture of the file that used to live there.
    resetFixture();
    {
        QFile f(m_fixtureDir + "/over.jpg");
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        EXPECT_EQ(f.write(QByteArray(7, 'x')), 7);
    }
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 4UL, 10000);
    const int source = dm.indexOfFile(m_fixtureDir + "/over.jpg");
    const int dest = dm.indexOfFile(m_fixtureDir + "/img1.jpg");
    ASSERT_TRUE(source >= 0 && dest >= 0);
    const quint64 movingId = dm.fileEntryAt(source).id;
    EXPECT_NE(dm.fileEntryAt(dest).id, movingId);

    // QFile::rename would not do: it maps to RENAME_NOREPLACE and cannot
    // overwrite, so it cannot even produce the event this test is about.
#ifdef Q_OS_WIN
    // mingw's rename() carries the MSRT semantics and answers EEXIST for
    // an existing destination — the exact case this test exists to
    // produce. MoveFileEx(REPLACE_EXISTING) is rename(2)'s overwrite
    // atomics on this platform, the same spelling FileOperations::rename
    // uses for the forced-overwrite branch.
    EXPECT_TRUE(MoveFileExW(reinterpret_cast<const WCHAR *>((m_fixtureDir + QStringLiteral("/over.jpg")).utf16()),
                            reinterpret_cast<const WCHAR *>((m_fixtureDir + QStringLiteral("/img1.jpg")).utf16()),
                            MOVEFILE_REPLACE_EXISTING));
#else
    const QByteArray from = (m_fixtureDir + "/over.jpg").toLocal8Bit();
    const QByteArray to = (m_fixtureDir + "/img1.jpg").toLocal8Bit();
    EXPECT_EQ(std::rename(from.constData(), to.constData()), 0);
#endif

    // four rows became three: an overwrite consumes the destination entry,
    // which is the whole difference between this and a plain rename
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);
    EXPECT_TRUE(!dm.containsFile(m_fixtureDir + "/over.jpg"));
    // and the surviving row now describes the file that is actually there
    EXPECT_EQ(dm.fileEntryAt(dm.indexOfFile(m_fixtureDir + "/img1.jpg")).size, 7UL);
    EXPECT_EQ(dm.fileEntryAt(dm.indexOfFile(m_fixtureDir + "/img1.jpg")).id, movingId);
    assertMirrorMatchesEngineExport(dm);
}

// "Decide when the listing lands" used to be a hand-rolled flag in Core
// with three clear/consume sites; whenListed() centralizes the lifecycle,
// and these tests pin it.

TEST_F(TestListing, whenListedFiresWhenTheListingLands) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    bool fired = false;
    dm.whenListed(m_fixtureDir, [&fired] {
        fired = true;
    });
    EXPECT_TRUE(!fired); // armed, the listing is still in flight
    ASSERT_TRY_TRUE_TIMEOUT(fired, 10000);
}

TEST_F(TestListing, whenListedIsSupersededByANewerOpen) {
    resetFixture();
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("second"));
    QFile f(m_fixtureDir + "/second/z.jpg");
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));

    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    bool staleFired = false;
    dm.whenListed(m_fixtureDir, [&staleFired] {
        staleFired = true;
    });
    // a newer open supersedes the queue before the stale listing lands
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir + "/second"));
    EXPECT_TRUE(!staleFired);
    bool freshFired = false;
    dm.whenListed(m_fixtureDir + "/second", [&freshFired] {
        freshFired = true;
    });
    ASSERT_TRY_TRUE_TIMEOUT(freshFired, 10000);
    EXPECT_TRUE(!staleFired);
}

TEST_F(TestListing, aReopenOfTheSameDirectoryIsNotSettledUntilItsListingLands) {
    // The contract isSettled() documents: true when nothing ASKED for is still
    // in flight. A same-directory reopen asks for an open, and for a while the
    // predicate answered true anyway — every POSITION term already held on the
    // old listing, and nothing tracked the open. The flag that tracks it is
    // set synchronously by setDirectory, so the false is deterministic no
    // matter how fast the engine answers; it flips back once the reopen's own
    // listing is applied.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);

    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    EXPECT_FALSE(dm.isSettled());
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
}

TEST_F(TestListing, whenListedSurvivesAReopenOfTheSameDirectory) {
    // The arm-then-open ordering is the natural way to write it: the action is
    // armed, then the directory opened. The unconditional wipe made that a
    // silent kill on a same-directory reopen — the open IS the listing the
    // action is waiting for, so superseding it there served nobody. Only
    // actions for OTHER directories are superseded by a newer open.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);

    bool fired = false;
    dm.whenListed(m_fixtureDir, [&fired] {
        fired = true;
    });
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    // isSettled cannot stand in here: a same-directory reopen reads as settled
    // the instant the call returns (the old listing already satisfies every
    // POSITION and OWED WORK term), while the new listing is still in flight.
    // The action firing is the thing being pinned — wait on it directly.
    ASSERT_TRY_TRUE_TIMEOUT(fired, 10000);
}

TEST_F(TestListing, whenListedCanBeCancelled) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    bool fired = false;
    qint64 token = dm.whenListed(m_fixtureDir, [&fired] {
        fired = true;
    });
    EXPECT_TRUE(dm.cancelListed(token));
    // the listing lands (the counts arrive), the cancelled action stays
    // silent, and a double cancel is a no-op rather than an error
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3, 10000);
    // the pending actions run inside the same listing event, so a settled
    // mirror is the bound; the sleep only hoped the event had been processed
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    EXPECT_TRUE(!fired);
    EXPECT_TRUE(!dm.cancelListed(token));
}

// In-app file operations touch the disk only; the mirror has exactly one
// writer (the snapshot installer) and the update arrives through the
// watch round trip, the same path an external edit takes.

TEST_F(TestListing, modelFileOperationsRideTheEngineRoundTrip) {
    resetFixture();
    WorkerPool pool(2);
    DirectoryModel model(pool, EngineBridge::instance());
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 3UL, 10000);

    FileOpResult result;
    model.removeFile(m_fixtureDir + "/img2.jpg", false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    ASSERT_TRY_EQ_TIMEOUT(model.listing().fileCount(), 2UL, 10000);
    EXPECT_FALSE(model.listing().containsFile(m_fixtureDir + "/img2.jpg"));

    // A rename issued here follows the same route as an external one.
    model.renameEntry(m_fixtureDir + "/img1.jpg", "renamed.jpg", false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    ASSERT_TRY_TRUE_TIMEOUT(model.listing().containsFile(m_fixtureDir + "/renamed.jpg"), 10000);
    EXPECT_TRUE(!model.listing().containsFile(m_fixtureDir + "/img1.jpg"));

    // copy into the watched directory — engine-driven from the start
    EXPECT_TRUE(QFile::copy(m_fixtureDir + "/renamed.jpg", m_fixtureDir + "/copy.jpg"));
    ASSERT_TRY_TRUE_TIMEOUT(model.listing().containsFile(m_fixtureDir + "/copy.jpg"), 10000);
    EXPECT_EQ(model.listing().fileCount(), 2UL + 1UL);
}

// THE mirror contract: the engine decides content and order; the mirror is
// a faithful projection of it. After any mutation sequence the vectors
// must equal the engine's export without this side re-deriving either.
// (The pre-contract applier sorted incrementally with a lexicographic path
// compare, so "img3.jpg" landed somewhere else than the engine's natural
// order put it.)
TEST_F(TestListing, mirrorStaysAFaithfulProjectionOfTheEngineListing) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    // tricky names — exactly where the two producers used to disagree —
    // plus a removal and a modification, all through the watcher.
    // IMG2/img2 are one name on a case-insensitive filesystem: creating
    // "IMG2.jpg" there would reopen the img2.jpg that is already listed
    // and the add never happens, so the Windows variant stresses the same
    // collation with IMG3 instead.
#ifdef Q_OS_WIN
    makeFile("IMG3.jpg");
#else
    makeFile("IMG2.jpg");
#endif
    makeFile("img 10.jpg");
    makeFile("10.jpg");
    EXPECT_TRUE(QFile::remove(m_fixtureDir + "/img1.jpg"));
    {
        QFile f(m_fixtureDir + "/img2.jpg");
        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Append));
        EXPECT_TRUE(f.write("appended") == 8);
    }
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 5UL, 10000); // 3 - 1 + 3 new
    // A count of 5 is reachable mid-flight (three adds and one removal), so
    // this line only narrows the wait; isSettled() then rules out the things
    // it can actually see — mirrored directory, applied sort, no resync — and
    // the pull below is what waits for the event queue to be empty, because
    // nothing before it can.
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    assertMirrorMatchesEngineExport(dm);

    // under SIZE sort a metadata change re-seats the entry on the engine
    // side; the mirror must follow the wire index, not its own comparator
    dm.setSortingMode(SORT_SIZE);
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000); // the resort itself is applied
    assertMirrorMatchesEngineExport(dm);

    {
        QFile f(m_fixtureDir + "/10.jpg");
        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Append));
        EXPECT_TRUE(f.write(QByteArray(4096, 'x')));
    }
    // the reseat must land; when it does not, the failure names the
    // divergence — which half is behind, and what both orders actually
    // hold. An unattributed index flake is an unmeasured one (this flake
    // failed three full-ctest runs before it could speak).
    const QString tenJpg = m_fixtureDir + QStringLiteral("/10.jpg");
    const QString img2Jpg = m_fixtureDir + QStringLiteral("/img2.jpg");
    bool reseatLanded = false;
    QDeadlineTimer reseatDeadline(10000);
    while(!((reseatLanded = dm.indexOfFile(tenJpg) > dm.indexOfFile(img2Jpg))) && !reseatDeadline.hasExpired())
        QTest::qWait(50);
    if(!reseatLanded) {
        QString diff;
        mirrorMatchesEngineExport(dm, diff);
        const int tenIdx = dm.indexOfFile(tenJpg);
        const int img2Idx = dm.indexOfFile(img2Jpg);
        const quint64 tenSize = tenIdx >= 0 ? dm.fileEntryAt(tenIdx).size : 0;
        const quint64 img2Size = img2Idx >= 0 ? dm.fileEntryAt(img2Idx).size : 0;
        ADD_FAILURE() << qPrintable(QStringLiteral("the size reseat never landed in 10 s: 10.jpg index %1 size %2, "
                                                   "img2.jpg index %3 size %4 — %5")
                                        .arg(tenIdx)
                                        .arg(tenSize)
                                        .arg(img2Idx)
                                        .arg(img2Size)
                                        .arg(diff));
    }
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    assertMirrorMatchesEngineExport(dm);
}

TEST_F(TestListing, aBusyEnginePublishesItsLatestOrderWhenQtCatchesUp) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto resize = [&dir](const QString &name, qint64 size) {
        QFile file(dir.filePath(name));
        return file.open(QIODevice::ReadWrite) && file.resize(size);
    };
    ASSERT_TRUE(resize("a.jpg", 10));
    ASSERT_TRUE(resize("b.jpg", 20));
    ASSERT_TRUE(resize("c.jpg", 30));

    auto &bridge = EngineBridge::instance();
    DirectoryManager mirror(bridge);
    mirror.setSortingMode(SORT_SIZE);
    QStringList openedRows;
    quint64 openedRevision = 0;
    QObject::connect(&mirror, &DirectoryManager::loaded, &mirror, [&] {
        openedRevision = mirror.appliedRevision();
        for(int index = 0; index < 3; ++index)
            openedRows << mirror.fileNameAt(index);
    });
    EXPECT_TRUE(mirror.setDirectory(dir.path()));
    const auto awaitOrderWithoutPumpingQt = [&](const QStringList &expected) {
        QElapsedTimer deadline;
        deadline.start();
        while(deadline.elapsed() < 10000) {
            EngineBridge::Snapshot snapshot;
            if(bridge.readSnapshot(snapshot) && snapshot.session == bridge.session()) {
                QStringList names;
                for(const auto &entry : snapshot.files)
                    names << entry.name;
                if(names == expected)
                    return snapshot.revision;
            }
            QThread::msleep(2);
        }
        return quint64(0);
    };
    ASSERT_NE(awaitOrderWithoutPumpingQt({"a.jpg", "b.jpg", "c.jpg"}), 0U);
    ASSERT_TRUE(resize("a.jpg", 25));
    ASSERT_NE(awaitOrderWithoutPumpingQt({"b.jpg", "a.jpg", "c.jpg"}), 0U);
    ASSERT_TRUE(resize("b.jpg", 40));
    const QStringList finalOrder{"a.jpg", "c.jpg", "b.jpg"};
    const quint64 finalRevision = awaitOrderWithoutPumpingQt(finalOrder);
    ASSERT_NE(finalRevision, 0U);

    ASSERT_TRY_TRUE_TIMEOUT(mirror.isSettled(), 10000);
    EXPECT_EQ(openedRows, finalOrder);
    EXPECT_EQ(openedRevision, finalRevision);
    emit bridge.listingChanged();
    EXPECT_EQ(mirror.appliedRevision(), finalRevision);
    assertMirrorMatchesEngineExport(mirror);

    ASSERT_TRUE(resize("c.jpg", 5));
    ASSERT_TRY_EQ_TIMEOUT(mirror.fileNameAt(0), QStringLiteral("c.jpg"), 10000);
    assertMirrorMatchesEngineExport(mirror);
}

TEST_F(TestListing, sortingDuringAnOpenLandsWithTheRequestedOrder) {
    resetFixture();
    DirectoryManager mirror{EngineBridge::instance()};
    mirror.setSortingMode(SORT_NAME);
    EXPECT_TRUE(mirror.setDirectory(m_fixtureDir));
    mirror.setSortingMode(SORT_NAME_DESC);
    ASSERT_TRY_TRUE_TIMEOUT(mirror.isSettled(), 10000);
    EXPECT_EQ(mirror.appliedSortingMode(), SORT_NAME_DESC);
    EXPECT_EQ(mirror.fileNameAt(0), QStringLiteral("img10.jpg"));
    assertMirrorMatchesEngineExport(mirror);
}
