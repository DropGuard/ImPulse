#include "gtest_qt.h"
#include <utility>
#include <QSignalSpy>
#include <QApplication>

#include "core_test_helpers.h"
#include "testhelpers.h"
#include "core.h"
#include "settings.h"
#include "sharedresources.h"
#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/inputmap.h"
#include "gui/dialogs/settingsdialog.h"

class TestGrid : public ::testing::Test {
  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons;
    QString m_base;

    void writeImage(const QString &path) {
        EXPECT_TRUE(makeImageFile(path, QSize(8, 8)));
    }
    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(m_base).filePath(tag + QStringLiteral("-{}").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    void SetUp() override {
        EXPECT_TRUE(m_home.path() != nullptr);
        m_base = QDir(m_home.path()).filePath("grid");
        EXPECT_TRUE(QDir().mkpath(m_base));
    }
};

TEST_F(TestGrid, navigationMovesByColumnsAndBack) {
    FolderGridView grid;
    grid.resize(500, 600);
    grid.populate(25);
    grid.show();
    grid.select(0);

    grid.selectBelow();
    const int rowJump = coretest::lastSelection(grid); // = the column count
    EXPECT_TRUE(rowJump >= 1) << "selectBelow must leave the first row";
    grid.selectBelow();
    EXPECT_EQ(coretest::lastSelection(grid), rowJump * 2); // column-preserving on a regular row
    grid.selectAbove();
    EXPECT_EQ(coretest::lastSelection(grid), rowJump);
    grid.selectAbove();
    EXPECT_EQ(coretest::lastSelection(grid), 0);
    grid.selectAbove(); // first row: already at the top, stays
    EXPECT_EQ(coretest::lastSelection(grid), 0);
}

TEST_F(TestGrid, navigationRefusesToLeaveTheGridEdges) {
    FolderGridView grid;
    grid.resize(500, 600);
    grid.populate(25);
    grid.show();
    grid.select(0);

    grid.selectAbove(); // first row: already at the top
    EXPECT_EQ(coretest::lastSelection(grid), 0);

    grid.select(24);    // the last item
    grid.selectBelow(); // last row: stays
    EXPECT_EQ(coretest::lastSelection(grid), 24);
    grid.selectNext(); // past the end: stays
    EXPECT_EQ(coretest::lastSelection(grid), 24);
    grid.selectPrev();
    EXPECT_EQ(coretest::lastSelection(grid), 23);
    grid.selectPrev();
    EXPECT_EQ(coretest::lastSelection(grid), 22);
    grid.select(0);
    grid.selectPrev(); // before the start: stays
    EXPECT_EQ(coretest::lastSelection(grid), 0);
}

TEST_F(TestGrid, navigationRestoresTheColumnOnIrregularRows) {
    FolderGridView grid;
    grid.resize(500, 600);
    grid.populate(23); // 23 % cols != 0: the last row is short
    grid.show();
    grid.select(0);
    grid.selectBelow();
    const int cols = coretest::lastSelection(grid);
    // ASSERT: `cols` is the divisor two lines below, and an EXPECT would let a
    // zero column count through into a division by zero.
    ASSERT_TRUE(cols >= 2) << "this calibration needs at least two columns";
    // the last item of the second-to-last row: its column lies beyond the
    // short last row, so selectBelow clamps and the shifted-column logic
    // must remember the original column for selectAbove to restore it
    const int rows = 23 / cols + 1;
    const int orig = (rows - 2) * cols + (cols - 1);
    grid.select(orig);

    grid.selectBelow();
    EXPECT_TRUE(coretest::lastSelection(grid) != orig);
    grid.selectAbove();

    EXPECT_EQ(coretest::lastSelection(grid), orig); // the shifted column was restored
}

TEST_F(TestGrid, pageJumpsMoveFourRows) {
    FolderGridView grid;
    grid.resize(500, 600);
    grid.populate(25);
    grid.show();
    grid.select(0);
    grid.selectBelow();
    const int cols = coretest::lastSelection(grid);
    grid.select(0); // pageDown jumps from lastSelected(): restart from the top

    grid.pageDown();
    EXPECT_EQ(coretest::lastSelection(grid), 4 * cols); // exactly four rows down
    grid.pageUp();
    EXPECT_EQ(coretest::lastSelection(grid), 0); // and exactly four rows back
}

TEST_F(TestGrid, zoomClampsToTheDocumentedLimits) {
    FolderGridView grid;
    grid.resize(500, 600);
    grid.populate(4);
    grid.show();
    QSignalSpy spy(&grid, &FolderGridView::thumbnailSizeChanged);
    ASSERT_TRUE(spy.isValid()); // the spy is indexed below; a bad signal would crash it

    for(int i = 0; i < 20; ++i)
        grid.zoomIn();
    ASSERT_TRUE(spy.count() > 0); // guards the at(count() - 1) on the next line
    EXPECT_EQ(spy.at(spy.count() - 1).at(0).toInt(), grid.THUMBNAIL_SIZE_MAX);

    for(int i = 0; i < 40; ++i)
        grid.zoomOut();
    EXPECT_EQ(spy.at(spy.count() - 1).at(0).toInt(), grid.THUMBNAIL_SIZE_MIN);
}

TEST_F(TestGrid, dragEnterAcceptsUnconditionally) {
    coretest::TestableFolderGridView grid;
    grid.resize(300, 300);
    grid.populate(1);
    grid.show();

    auto mime = std::make_unique<QMimeData>();
    mime->setUrls({QUrl::fromLocalFile("/tmp/whatever.png")});
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    grid.dragEnterEvent(&enter);
    EXPECT_TRUE(enter.isAccepted()) << "dragEnter rejected a drop the grid is built to accept";

    QMimeData plain;
    plain.setText("just text");
    QDragEnterEvent plainEnter(QPoint(10, 10), Qt::CopyAction, &plain, Qt::LeftButton, Qt::NoModifier);
    grid.dragEnterEvent(&plainEnter);
    EXPECT_TRUE(plainEnter.isAccepted())
        << "the grid accepts without inspecting the mime — reporting is the drop's job";
}

TEST_F(TestGrid, dragMoveReportsTheHoveredThumbnailIndex) {
    coretest::TestableFolderGridView grid;
    grid.resize(300, 300);
    grid.populate(2);
    grid.show();
    QSignalSpy spy(&grid, &FolderGridView::draggedOver);
    ASSERT_TRUE(spy.isValid()); // the spy is read below; a bad signal would crash it

    const QPoint pos0 = coretest::dropPosOverThumbnail(grid, 0);
    const QPoint pos1 = coretest::dropPosOverThumbnail(grid, 1);
    EXPECT_TRUE(pos0 != pos1) << "the two thumbnails share a position — layout never ran";

    QDragMoveEvent move0(QPoint(pos0), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&move0);
    EXPECT_EQ(spy.last().at(0).toInt(), 0);

    QDragMoveEvent move1(QPoint(pos1), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&move1);
    EXPECT_EQ(spy.last().at(0).toInt(), 1);

    QDragMoveEvent moveEmpty(QPoint(1, 1), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&moveEmpty);
    EXPECT_EQ(spy.last().at(0).toInt(), -1); // off-grid: the consumer learns "no target"
}

TEST_F(TestGrid, dragReportingSurvivesALeave) {
    coretest::TestableFolderGridView grid;
    grid.resize(300, 300);
    grid.populate(2);
    grid.show();
    QSignalSpy spy(&grid, &FolderGridView::draggedOver);
    ASSERT_TRUE(spy.isValid()); // the spy is read below; a bad signal would crash it

    const QPoint pos0 = coretest::dropPosOverThumbnail(grid, 0);
    QDragMoveEvent move0(QPoint(pos0), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&move0);
    EXPECT_EQ(spy.last().at(0).toInt(), 0);

    QDragLeaveEvent leave;
    grid.dragLeaveEvent(&leave);
    EXPECT_TRUE(leave.isAccepted());

    // a new drag sequence after the leave reports from a clean slate
    QDragMoveEvent move0again(QPoint(pos0), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&move0again);
    EXPECT_EQ(spy.last().at(0).toInt(), 0);
}

TEST_F(TestGrid, dropOutsideAnyThumbnailReportsMinusOne) {
    coretest::TestableFolderGridView grid;
    grid.resize(300, 300);
    grid.populate(2);
    grid.show();
    QSignalSpy spy(&grid, &FolderGridView::droppedInto);
    ASSERT_TRUE(spy.isValid()); // the spy is read below; a bad signal would crash it

    auto mime = std::make_unique<QMimeData>();
    mime->setUrls({QUrl::fromLocalFile("/tmp/x.png")});
    QDropEvent drop(QPointF(1, 1), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    grid.dropEvent(&drop);

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(2).toInt(), -1); // the consumer decides: nothing to highlight
}

TEST_F(TestGrid, dropReportsMimeSourceAndLandingIndex) {
    coretest::TestableFolderGridView grid;
    grid.resize(300, 300);
    grid.populate(2);
    grid.show();
    QSignalSpy spy(&grid, &FolderGridView::droppedInto);
    ASSERT_TRUE(spy.isValid()); // the spy is read below; a bad signal would crash it

    auto mime = std::make_unique<QMimeData>();
    const QList<QUrl> urls{QUrl::fromLocalFile("/tmp/dropped1.png"), QUrl::fromLocalFile("/tmp/dropped2.png")};
    mime->setUrls(urls);

    const QPoint pos1 = coretest::dropPosOverThumbnail(grid, 1);
    QDropEvent drop(QPointF(pos1), Qt::CopyAction, mime.get(), Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
    grid.dropEvent(&drop);
    EXPECT_TRUE(drop.isAccepted());
    ASSERT_EQ(spy.count(), 1);                                         // guards the at(0) chain below
    EXPECT_EQ(spy.at(0).at(0).value<const QMimeData *>(), mime.get()); // payload passes through
    EXPECT_EQ(spy.at(0).at(1).value<QObject *>(), nullptr);            // no source (external drop)
    EXPECT_EQ(spy.at(0).at(2).toInt(), 1);                             // landed on thumbnail #1
    EXPECT_EQ(spy.at(0).at(0).value<const QMimeData *>()->urls(), urls);
}

// The row is the unit a rename moves. Driven straight at the view because
// populate() is a fixture of real rows with no filesystem behind them, and
// everything at stake here is view arithmetic: the widget surviving (its
// pixels ride along with its name), the rows it jumped over sliding the other
// way, and the selection following the row instead of staying on the vacated
// index — that last one is the visible bug shape, a highlight left behind on
// a row the user is no longer on.
TEST_F(TestGrid, renameMovesTheRowAndCarriesItsWidgetAndSelection) {
    FolderGridView grid;
    grid.resize(500, 600);
    grid.populate(25);
    grid.show();
    const QList<ThumbnailWidget *> before = coretest::orderedThumbnails(grid);
    // ASSERT: before.at(1) and before.at(2) below index the list this guards
    ASSERT_TRUE(before.count() == 25) << qPrintable(
        QStringLiteral("populate(25) laid out %1 rows").arg(before.count()));
    grid.select(1);
    ThumbnailWidget *const moving = before.at(1);

    grid.renameItem(1, 4, QStringLiteral("moved.png"));

    const QList<ThumbnailWidget *> after = coretest::orderedThumbnails(grid);
    ASSERT_EQ(after.count(), 25); // guards the at(4)/at(3) reads below
    EXPECT_TRUE(after.at(4) == moving) << "the moved row is a different widget — renaming does not recreate the row";
    EXPECT_TRUE(after.at(1) == before.at(2) && after.at(3) == before.at(4))
        << "the rows the move jumped over did not slide into the space it left";
    EXPECT_EQ(grid.selection(), QList<int>({4}));

    // and back again: the same rows slide the other way
    grid.renameItem(4, 0, QStringLiteral("front.png"));
    const QList<ThumbnailWidget *> back = coretest::orderedThumbnails(grid);
    EXPECT_TRUE(back.at(0) == moving) << "the row did not move back to the front";
    EXPECT_TRUE(back.at(1) == before.at(0) && back.at(4) == before.at(4))
        << "the rows it passed going back did not shift down";
    EXPECT_EQ(grid.selection(), QList<int>({0}));

    // an in-place rename — the name-sort case where the row does not move —
    // touches neither the layout nor the selection
    grid.renameItem(0, 0, QStringLiteral("still.png"));
    EXPECT_TRUE(coretest::orderedThumbnails(grid).at(0) == moving) << "an in-place rename moved a row";
    EXPECT_EQ(grid.selection(), QList<int>({0}));

    // out-of-range on either side is refused rather than clamped: the caller
    // is a live event about a row the view may already have dropped
    grid.renameItem(0, 99, QStringLiteral("nowhere.png"));
    ASSERT_EQ(coretest::orderedThumbnails(grid).count(), 25); // guards the at(0) read below
    EXPECT_TRUE(coretest::orderedThumbnails(grid).at(0) == moving) << "an out-of-range rename moved a row";

    // A multi-row selection is the other half of the remap: the selected row
    // moves and the selected rows it passes shift the other way. One selected
    // index reaches only the first branch, and the branches it does not reach
    // are the ones that leave a highlight on the wrong picture.
    grid.select(QList<int>{1, 2, 3});
    grid.renameItem(1, 4, QStringLiteral("moved.png"));
    QList<int> shifted = grid.selection();
    std::sort(shifted.begin(), shifted.end());
    EXPECT_EQ(shifted, QList<int>({1, 2, 4}));

    grid.renameItem(4, 0, QStringLiteral("front.png"));
    shifted = grid.selection();
    std::sort(shifted.begin(), shifted.end());
    EXPECT_EQ(shifted, QList<int>({0, 2, 3}));
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
