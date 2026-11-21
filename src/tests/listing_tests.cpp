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

// A sort is a permutation of rows the mirror already holds. It must not look
// like a directory re-listing: no row arrives, none leaves, and the view is
// told to re-read. This is the contract the engine's SetSort half was written
// for, and it is what separates a 3-row fixture from a 100k-row folder where
// rebuilding the vectors is the visible cost.
TEST_F(TestListing, aSortIsAPermutationNotARelisting) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    QSignalSpy added(&dm, &DirectoryManager::fileAdded);
    QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
    QSignalSpy resort(&dm, &DirectoryManager::sortingChanged);
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);
    added.clear();
    removed.clear();

    dm.setSortingMode(SORT_NAME_DESC);
    ASSERT_TRY_EQ_TIMEOUT(resort.count(), 1, 10000);

    EXPECT_EQ(added.count(), 0) << "a sort reported rows arriving";
    EXPECT_EQ(removed.count(), 0) << "a sort reported rows leaving";
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
        // the wire's own width: the mirror stores the low 32 bits
        return bridge.exportListing(arena, fc, dc, rev) ? quint32(rev) : 0;
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
    QSignalSpy added(&dm, &DirectoryManager::fileAdded);
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    // a file appears while the directory is open; on Linux the engine
    // watcher applies it incrementally (no rescan involved)
    makeFile("latecomer.jpg");
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 4UL, 10000);
    EXPECT_EQ(added.count(), 1);
    EXPECT_EQ(added.at(0).at(0).toString(), m_fixtureDir + "/latecomer.jpg");
    // the engine listing is authoritative: natural order kept
    EXPECT_EQ(dm.filePathAt(0), m_fixtureDir + "/img1.jpg");
    EXPECT_EQ(dm.filePathAt(3), m_fixtureDir + "/latecomer.jpg");
}

TEST_F(TestListing, liveFileDeletionEmitsRemoved) {
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    EXPECT_TRUE(QFile::remove(m_fixtureDir + "/img2.jpg"));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 2UL, 10000);
    EXPECT_EQ(removed.count(), 1);
    EXPECT_EQ(dm.filePathAt(0), m_fixtureDir + "/img1.jpg");
    EXPECT_EQ(dm.filePathAt(1), m_fixtureDir + "/img10.jpg");
}

TEST_F(TestListing, liveFileModificationKeepsTheMirrorEntryWhole) {
    // the engine watcher's "modified" event is applied by re-statting the
    // file through FSEntry's path constructor. That constructor once
    // produced an empty entry for every path: the mirror replaced a valid
    // entry with an empty one, the fileModified signal still fired,
    // nothing crashed — the file just silently vanished from the mirror.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    QSignalSpy modified(&dm, &DirectoryManager::fileModified);
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    QFile f(m_fixtureDir + "/img2.jpg");
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Append));
    EXPECT_TRUE(f.write("appended") == 8);
    f.close();

    ASSERT_TRY_TRUE_TIMEOUT(modified.count() >= 1, 10000);
    bool sawImg2Modified = false;
    for(int i = 0; i < modified.count(); ++i)
        sawImg2Modified |= modified.at(i).at(0).toString() == m_fixtureDir + "/img2.jpg";
    EXPECT_TRUE(sawImg2Modified);

    // the entry survives the round trip whole: re-stat, not emptied
    int index = dm.indexOfFile(m_fixtureDir + "/img2.jpg");
    EXPECT_TRUE(index >= 0);
    ASSERT_TRY_EQ_TIMEOUT(dm.fileEntryAt(index).size, 9UL, 10000);
    EXPECT_EQ(dm.fileEntryAt(index).name, QStringLiteral("img2.jpg"));
    EXPECT_EQ(dm.fileEntryAt(index).path, m_fixtureDir + "/img2.jpg");
    EXPECT_TRUE(!dm.fileEntryAt(index).isDirectory);
}

TEST_F(TestListing, liveRenameIsOneEvent) {
    // The watcher pairs the kernel's MOVED_FROM/MOVED_TO cookie, so a rename
    // reaches the mirror as ONE event moving ONE row: no window in which the
    // file is missing from the listing, and no remove+add churn that costs
    // the view a widget — and the picture inside it — for a file whose bytes
    // never changed.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
    QSignalSpy added(&dm, &DirectoryManager::fileAdded);
    QSignalSpy renamed(&dm, &DirectoryManager::fileRenamed);
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);
    const int oldIndex = dm.indexOfFile(m_fixtureDir + "/img2.jpg");
    EXPECT_TRUE(oldIndex >= 0);

    EXPECT_TRUE(QFile::rename(m_fixtureDir + "/img2.jpg", m_fixtureDir + "/renamed.jpg"));
    ASSERT_TRY_EQ_TIMEOUT(renamed.count(), 1, 10000);
    EXPECT_EQ(removed.count(), 0);
    EXPECT_EQ(added.count(), 0);
    EXPECT_EQ(renamed.at(0).at(0).toString(), m_fixtureDir + "/img2.jpg");
    EXPECT_EQ(renamed.at(0).at(1).toString(), m_fixtureDir + "/renamed.jpg");
    EXPECT_EQ(renamed.at(0).at(2).toInt(), oldIndex);

    // one event, and the row it moved is the engine's own row: name sort
    // lands "renamed" after "img1" and "img10", so the entry repositioned —
    // the rename is a move under this mode, not an in-place edit
    EXPECT_EQ(dm.fileCount(), 3UL);
    const int newIndex = dm.indexOfFile(m_fixtureDir + "/renamed.jpg");
    EXPECT_TRUE(newIndex != oldIndex);
    EXPECT_TRUE(!dm.containsFile(m_fixtureDir + "/img2.jpg"));
    // the entry rode along whole: size is the stat the engine already had,
    // not a re-derive and not a zeroed placeholder
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
    QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
    QSignalSpy added(&dm, &DirectoryManager::fileAdded);
    QSignalSpy renamed(&dm, &DirectoryManager::fileRenamed);
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 4UL, 10000);
    const int source = dm.indexOfFile(m_fixtureDir + "/over.jpg");
    const int dest = dm.indexOfFile(m_fixtureDir + "/img1.jpg");
    EXPECT_TRUE(source >= 0 && dest >= 0);

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

    ASSERT_TRY_EQ_TIMEOUT(renamed.count(), 1, 10000);
    EXPECT_EQ(added.count(), 0);
    EXPECT_EQ(renamed.at(0).at(0).toString(), m_fixtureDir + "/over.jpg");
    EXPECT_EQ(renamed.at(0).at(1).toString(), m_fixtureDir + "/img1.jpg");
    // the row the overwrite took away is announced before the move, because
    // every index after it shifts — including the one just passed to this
    // signal, which is the position the view is holding when it fires
    EXPECT_EQ(removed.count(), 1);
    EXPECT_EQ(removed.at(0).at(0).toString(), m_fixtureDir + "/img1.jpg");
    EXPECT_EQ(renamed.at(0).at(2).toInt(), (source > dest) ? source - 1 : source);
    // four rows became three: an overwrite consumes the destination entry,
    // which is the whole difference between this and a plain rename
    EXPECT_EQ(dm.fileCount(), 3UL);
    EXPECT_TRUE(!dm.containsFile(m_fixtureDir + "/over.jpg"));
    // and the surviving row now describes the file that is actually there
    EXPECT_EQ(dm.fileEntryAt(dm.indexOfFile(m_fixtureDir + "/img1.jpg")).size, 7UL);
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
// writer (the engine event applier) and the update arrives through the
// watch round trip, the same path an external edit takes.

TEST_F(TestListing, modelFileOperationsRideTheEngineRoundTrip) {
    resetFixture();
    WorkerPool pool(2);
    DirectoryModel model(pool, EngineBridge::instance());
    QSignalSpy removed(&model, &DirectoryModel::fileRemoved);
    QSignalSpy added(&model, &DirectoryModel::fileAdded);
    EXPECT_TRUE(model.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(model.fileCount(), 3UL, 10000);

    FileOpResult result;
    model.removeFile(m_fixtureDir + "/img2.jpg", false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    // the removal signal carries the update when the round trip lands,
    // not synchronously from the operation
    ASSERT_TRY_EQ_TIMEOUT(removed.count(), 1, 10000);
    EXPECT_EQ(removed.at(0).at(0).toString(), m_fixtureDir + "/img2.jpg");
    ASSERT_TRY_EQ_TIMEOUT(model.fileCount(), 2UL, 10000);

    // rename in place surfaces as remove+add, exactly like an external one
    model.renameEntry(m_fixtureDir + "/img1.jpg", "renamed.jpg", false, result);
    EXPECT_EQ(result, FileOpResult::SUCCESS);
    ASSERT_TRY_TRUE_TIMEOUT(model.containsFile(m_fixtureDir + "/renamed.jpg"), 10000);
    EXPECT_TRUE(!model.containsFile(m_fixtureDir + "/img1.jpg"));

    // copy into the watched directory — engine-driven from the start
    EXPECT_TRUE(QFile::copy(m_fixtureDir + "/renamed.jpg", m_fixtureDir + "/copy.jpg"));
    ASSERT_TRY_TRUE_TIMEOUT(model.containsFile(m_fixtureDir + "/copy.jpg"), 10000);
    EXPECT_EQ(model.fileCount(), 2UL + 1UL);
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

TEST_F(TestListing, mutationsOutsideTheMirroredDirectoryAreDropped) {
    // The mirror's vectors are one directory's listing, so a mutation whose
    // path belongs to another folder cannot be an edit of them: the engine's
    // order index means nothing here, and the row would be a file the user
    // never has on screen. The wire carries no directory for a mutation, so
    // the mirror has to make that check itself.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    auto &bridge = EngineBridge::instance();
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    const unsigned long before = dm.fileCount();
    EXPECT_TRUE(before > 0);

    QSignalSpy added(&dm, &DirectoryManager::fileAdded);
    emit bridge.dirMutated(QIMGV_MUT_ADDED, QStringLiteral("/some/other/place/x.jpg"), false, 1, 0, 0, QString(),
                           dm.appliedRevision());
    EXPECT_EQ(added.count(), 0);
    EXPECT_EQ(dm.fileCount(), before);
    EXPECT_TRUE(!dm.containsFile(QStringLiteral("/some/other/place/x.jpg")));

    // and the shape the engine bug actually produced: the *same name* from
    // another folder. The mirror keys on the full path, so it would not
    // collide — it would simply gain a row for a file that is not here, at
    // an index computed against the other folder's order.
    const QString twin = QFileInfo(dm.filePathAt(0)).fileName();
    emit bridge.dirMutated(QIMGV_MUT_ADDED, QStringLiteral("/some/other/place/") + twin, false, 1, 0, 1, QString(),
                           dm.appliedRevision());
    EXPECT_EQ(added.count(), 0);
    EXPECT_EQ(dm.fileCount(), before);

    // neither drop is drift: the engine never lists the other folder, so the
    // current listing is already what it should be, and no repair is armed
    EXPECT_TRUE(dm.isSettled());

    // the guard must not swallow the real case
    makeFile("legit.jpg");
    ASSERT_TRY_TRUE_TIMEOUT(dm.containsFile(m_fixtureDir + "/legit.jpg"), 10000);
}

TEST_F(TestListing, renameFromOutsideTheMirroredDirectoryIsDropped) {
    // A rename has two subjects, and the destination guard above can only
    // see one of them: the source names the ROW to move, so a foreign
    // source would erase a local entry that has nothing to do with it and
    // then splice a row from another folder at an index computed against
    // that other folder's order.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    auto &bridge = EngineBridge::instance();
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    const unsigned long before = dm.fileCount();
    EXPECT_TRUE(before > 0);
    const quint64 beforeRevision = dm.appliedRevision();
    QSignalSpy renamed(&dm, &DirectoryManager::fileRenamed);
    QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);

    // legal destination, foreign source — the shape the guard exists for
    emit bridge.dirMutated(QIMGV_MUT_RENAMED, m_fixtureDir + "/img1.jpg", false, 1, 0, 0,
                           QStringLiteral("/some/other/place/x.jpg"), dm.appliedRevision());
    EXPECT_EQ(renamed.count(), 0);
    EXPECT_EQ(removed.count(), 0);
    EXPECT_EQ(dm.fileCount(), before);
    EXPECT_TRUE(dm.containsFile(m_fixtureDir + "/img1.jpg"));
    // the row it would have moved is still the engine's own row
    assertMirrorMatchesEngineExport(dm);

    // the drop arms its own repair; wait for its listing to land (applied
    // moves — see the sort test for why this, not isSettled, names the
    // moment) before the real rename, so the rename rides the repair's
    // session instead of racing the rescan's open
    ASSERT_TRY_TRUE_TIMEOUT(dm.appliedRevision() != beforeRevision, 10000);

    // and a real rename of the same destination still lands
    EXPECT_TRUE(QFile::rename(m_fixtureDir + "/img2.jpg", m_fixtureDir + "/moved.jpg"));
    ASSERT_TRY_EQ_TIMEOUT(renamed.count(), 1, 10000);
}

TEST_F(TestListing, aDroppedMutationResyncsTheMirror) {
    // A duplicate arrival is dropped: the mirror already holds the row, and
    // applying it would double it — but the engine inserted the path and its
    // revision moved anyway, which is drift as far as this side knows. The
    // drop arms its own repair instead of waiting for a DirListed that a
    // quiet directory will never produce: the rescan re-lists, and the mirror
    // converges with no further event and no row change.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    auto &bridge = EngineBridge::instance();
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    const unsigned long before = dm.fileCount();
    const QString heldRow = dm.filePathAt(0);
    const quint64 applied = dm.appliedRevision();

    QSignalSpy added(&dm, &DirectoryManager::fileAdded);
    QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
    emit bridge.dirMutated(QIMGV_MUT_ADDED, heldRow, false, 1, 0, 0, QString(), applied);
    // the drop armed the repair: unsettled now, and the mirror follows the
    // rescan's listing when it lands (applied moving is the outside-visible
    // sign the repair landed — same reasoning as the sort test)
    EXPECT_FALSE(dm.isSettled());
    ASSERT_TRY_TRUE_TIMEOUT(dm.appliedRevision() != applied, 10000);
    EXPECT_EQ(added.count(), 0);
    EXPECT_EQ(removed.count(), 0);
    EXPECT_EQ(dm.fileCount(), before);
    EXPECT_TRUE(dm.containsFile(heldRow));
    assertMirrorMatchesEngineExport(dm);
}

TEST_F(TestListing, aSortMismatchResyncsInsteadOfWaitingForTheNextListing) {
    // The sort handler confirms the event's revision against the export's
    // before trusting it. A mismatch used to only set m_wantRescan — a flag
    // consumed by the next DirListed, which a quiet directory never produces,
    // leaving the mirror unsettled forever. The mismatch now arms its own
    // rescan, and the mirror converges to the engine's revision, not to the
    // event's claim.
    resetFixture();
    DirectoryManager dm{EngineBridge::instance()};
    auto &bridge = EngineBridge::instance();
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    const quint64 applied = dm.appliedRevision();

    emit bridge.sortChanged(m_fixtureDir, applied + 1);
    EXPECT_FALSE(dm.isSettled());
    // The repair is a same-directory openDir: the engine resorts silently (a
    // revision bump no event carries) and re-lists, and the mirror follows
    // when the DirListed lands. "applied moved" is what "the repair landed"
    // looks like from outside — the listing content is unchanged, so nothing
    // else names it. A bare equality assert cannot stand in here: the
    // engine's loop thread moves the export concurrently, and the macro's
    // wait-then-reassert can read the two sides on opposite sides of that
    // move (seen once under the hook's full-suite load: applied 2, export 3,
    // in a 196 ms test).
    ASSERT_TRY_TRUE_TIMEOUT(dm.appliedRevision() != applied, 10000);
    assertMirrorMatchesEngineExport(dm);
}
