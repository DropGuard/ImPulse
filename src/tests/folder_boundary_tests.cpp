#include "gtest_qt.h"
#include "engine_stack_fixture.h"

// the configured folder-end behaviour and the agreement between the folder view's
// order and the navigation's
class TestFolderBoundary : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestFolderBoundary() : EngineStackFixture("folder-boundary") {
    }
};

// "After reaching the end of a folder" is a setting with three values, so what
// gets pinned here is the behavior each value produces — not the absence of one
// old bug. decide() takes the mode by value, so these cases inject the value and
// never touch the settings singleton; whether the singleton hands out that value
// at all is its own one assertion below.

TEST_F(TestFolderBoundary, folderEndActionSettingRoundTripsAndClamps) {
    // The boundary cases above take the mode by value; this is the one thing
    // the settings layer adds: the key stores all three values and a junk
    // value in a hand-edited config file falls back to "no action" instead of
    // indexing past the enum.
    const QList<FolderEndAction> valid = {FOLDER_END_NO_ACTION, FOLDER_END_LOOP, FOLDER_END_GOTO_ADJACENT};
    for(const FolderEndAction mode : valid) {
        settings->setFolderEndAction(mode);
        EXPECT_TRUE(settings->folderEndAction() == mode);
    }
    settings->setFolderEndAction(static_cast<FolderEndAction>(7));
    EXPECT_TRUE(settings->folderEndAction() == FOLDER_END_NO_ACTION);
    settings->setFolderEndAction(static_cast<FolderEndAction>(-1));
    EXPECT_TRUE(settings->folderEndAction() == FOLDER_END_NO_ACTION);
    settings->setFolderEndAction(FOLDER_END_NO_ACTION); // leave the default
}

TEST_F(TestFolderBoundary, noActionReportsTheBoundaryAndStaysPut) {
    const FolderEndAction mode = FOLDER_END_NO_ACTION;

    // off the end (forward) and off the start (backward): report, do not move
    EXPECT_TRUE(FolderEnd::decide(mode, 3, 3).outcome == FolderEnd::Outcome::ReportBoundary);
    EXPECT_TRUE(FolderEnd::decide(mode, -1, 3).outcome == FolderEnd::Outcome::ReportBoundary);
    // a step that stays inside the folder still steps
    auto inside = FolderEnd::decide(mode, 1, 3);
    EXPECT_TRUE(inside.outcome == FolderEnd::Outcome::LoadIndex);
    EXPECT_EQ(inside.index, 1);
}

TEST_F(TestFolderBoundary, loopActionWrapsToTheFarEndOfTheSameFolder) {
    const FolderEndAction mode = FOLDER_END_LOOP;

    auto offEnd = FolderEnd::decide(mode, 3, 3);
    EXPECT_TRUE(offEnd.outcome == FolderEnd::Outcome::LoadIndex);
    EXPECT_EQ(offEnd.index, 0);
    auto beforeStart = FolderEnd::decide(mode, -1, 3);
    EXPECT_TRUE(beforeStart.outcome == FolderEnd::Outcome::LoadIndex);
    EXPECT_EQ(beforeStart.index, 2);
}

TEST_F(TestFolderBoundary, adjacentActionStepsOutOfTheFolderAtEitherEdge) {
    const FolderEndAction mode = FOLDER_END_GOTO_ADJACENT;

    EXPECT_TRUE(FolderEnd::decide(mode, 3, 3).outcome == FolderEnd::Outcome::EnterAdjacentFolder);
    EXPECT_TRUE(FolderEnd::decide(mode, -1, 3).outcome == FolderEnd::Outcome::EnterAdjacentFolder);
    // in the middle of the folder it behaves like every other mode
    EXPECT_TRUE(FolderEnd::decide(mode, 1, 3).outcome == FolderEnd::Outcome::LoadIndex);
}

TEST_F(TestFolderBoundary, adjacentActionEntersTheNextFolderInParentOrder) {
    DirectoryManager dm{EngineBridge::instance()};
    // The promise the setting makes: at the last image of alpha you go to beta.
    // adjacentDirectory() is what Core calls, so this covers the resolution
    // itself — routed through the async listing it returns empty here and the
    // feature is dead again.
    resetFixture();
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("alpha"));
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("beta"));
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("zeta"));
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir(".hidden")); // sorts first: proves the skip

    const QString alpha = m_fixtureDir + "/alpha";
    const QString beta = m_fixtureDir + "/beta";
    const QString sub = m_fixtureDir + "/subdir";
    const QString zeta = m_fixtureDir + "/zeta";

    // natural name order: alpha, beta, subdir, zeta
    // the dot dir sorts before alpha and is filtered by the flag: forward
    // from alpha reaches beta, backward from alpha finds nothing
    EXPECT_EQ(dm.adjacentDirectory(alpha, true, SORT_NAME), beta);
    EXPECT_EQ(dm.adjacentDirectory(alpha, false, SORT_NAME), QString());
    EXPECT_EQ(dm.adjacentDirectory(beta, true, SORT_NAME), sub);
    EXPECT_EQ(dm.adjacentDirectory(sub, true, SORT_NAME), zeta);
    EXPECT_EQ(dm.adjacentDirectory(zeta, false, SORT_NAME), sub);
    EXPECT_EQ(dm.adjacentDirectory(alpha, false, SORT_NAME), QString());
    // the last folder has nowhere to go — that is what makes Core show its
    // end-of-folder message instead of jumping
    EXPECT_EQ(dm.adjacentDirectory(zeta, true, SORT_NAME), QString());
}

TEST_F(TestFolderBoundary, folderBoundaryLookupKeepsWatchingTheCurrentFolder) {
    // A boundary that has nowhere to go must leave the user's own folder
    // watched: the lookup is not allowed to take the engine off it, or live
    // updates die for the rest of the session with nothing on screen to show
    resetFixture();

    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.fileCount(), 3UL, 10000);

    auto decision = FolderEnd::decide(FOLDER_END_GOTO_ADJACENT, static_cast<int>(dm.fileCount()), dm.fileCount());
    EXPECT_TRUE(decision.outcome == FolderEnd::Outcome::EnterAdjacentFolder);
    // the transition this keypress would take: nowhere — subdir is the only and
    // therefore the last folder here
    EXPECT_TRUE(dm.adjacentDirectory(m_fixtureDir + "/subdir", true, SORT_NAME).isEmpty());

    QSignalSpy added(&dm, &DirectoryManager::fileAdded);
    makeFile("after-boundary.jpg");
    ASSERT_TRY_EQ_TIMEOUT(added.count(), 1, 10000);
    EXPECT_EQ(dm.fileCount(), 4UL);
}

// The view's sibling order is the engine's (scan.rs gives every directory the
// same size/mtime, so its comparator lands on the name tiebreak) and the mirror
// keeps that order. The sibling lookup sorts locally, so under a TIME or SIZE
// mode the two were free to disagree: std::sort over an all-equivalent range is
// not stable, so "the next folder" could be a different one than the next row.

TEST_F(TestFolderBoundary, folderNavigationWalksSiblingsInTheOrderTheViewShows) {
    resetFixture();
    // created in reverse name order on purpose, so a degenerate local sort
    // cannot land on the expected order by accident
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("zulu"));
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("mike"));
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("bravo"));
    QStringList names = {m_fixtureDir + "/bravo", m_fixtureDir + "/mike", m_fixtureDir + "/subdir",
                         m_fixtureDir + "/zulu"};
    QStringList reversed;
    for(int i = names.size() - 1; i >= 0; --i)
        reversed << names.at(i);

    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.dirCount(), 4UL, 10000);

    const SortingMode modes[] = {SORT_NAME, SORT_NAME_DESC, SORT_TIME, SORT_TIME_DESC, SORT_SIZE, SORT_SIZE_DESC};
    for(SortingMode mode : modes) {
        dm.setSortingMode(mode);
        ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
        // the rows are the check, not the mode: sortingMode() reports the
        // mode this instance *asked* for, which is written before the
        // engine is told and so was true the moment the call returned
        QStringList view;
        for(unsigned long i = 0; i < dm.dirCount(); ++i)
            view << dm.dirPathAt(int(i));

        // walking the siblings must reproduce the rows, in the same direction
        QStringList walked;
        QString cur = dm.dirPathAt(0);
        while(!cur.isEmpty() && walked.size() <= names.size()) {
            walked << cur;
            cur = dm.adjacentDirectory(cur, true, mode);
        }

        const QStringList &want = (mode == SORT_NAME_DESC) ? reversed : names;
        EXPECT_EQ(view, want);
        EXPECT_EQ(walked, want);
    }
}

TEST_F(TestFolderBoundary, newlyCreatedFoldersKeepTheirPlaceInNameOrder) {
    // The other half of the same rule: a folder arriving through the watcher is
    // *inserted*, not re-sorted. The insert used to share the file comparator,
    // and since every directory carries the same size/mtime (scan.rs sets them,
    // and onEngineMutated zeroes modifyTime for dirs) a time or size comparator
    // saw all entries as equal and appended the newcomer at the end of the rows.
    resetFixture();
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("alpha"));
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("zulu"));

    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_EQ_TIMEOUT(dm.dirCount(), 3UL, 10000); // alpha, subdir, zulu

    dm.setSortingMode(SORT_TIME);
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);

    // a name that belongs in the middle, never at the end
    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("mike"));
    ASSERT_TRY_EQ_TIMEOUT(dm.dirCount(), 4UL, 10000);
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);

    QStringList rows;
    for(unsigned long i = 0; i < dm.dirCount(); ++i)
        rows << dm.dirNameAt(int(i));
    const QStringList want = {QStringLiteral("alpha"), QStringLiteral("mike"), QStringLiteral("subdir"),
                              QStringLiteral("zulu")};
    EXPECT_EQ(rows, want);
    // navigation walks the same order
    EXPECT_EQ(dm.adjacentDirectory(m_fixtureDir + "/mike", true, SORT_TIME), m_fixtureDir + "/subdir");
}

TEST_F(TestFolderBoundary, settlePredicateTracksTheReplyInsteadOfTheRequest) {
    // isSettled() is only worth waiting on if it is false while the engine
    // still owes a reply; otherwise every QTRY_VERIFY(isSettled()) in this
    // file is a sleep with a better name. The reverse order is used because
    // it always changes the listing, whatever the persisted mode was.
    resetFixture();

    DirectoryManager dm{EngineBridge::instance()};
    EXPECT_TRUE(dm.setDirectory(m_fixtureDir));
    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
    EXPECT_TRUE(dm.fileCount() > 1) << "a one-row resort proves nothing";

    const SortingMode flip = (dm.sortingMode() == SORT_NAME) ? SORT_NAME_DESC : SORT_NAME;
    dm.setSortingMode(flip);
    // synchronous window: the requested mode is already stored (this is what
    // the old wait tested) while the reply can only arrive via the event loop
    EXPECT_TRUE(dm.sortingMode() == flip);
    EXPECT_TRUE(!dm.isSettled());

    ASSERT_TRY_TRUE_TIMEOUT(dm.isSettled(), 10000);
}
