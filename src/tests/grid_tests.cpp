#include "gtest_qt.h"
#include <utility>
#include <QSignalSpy>
#include <QApplication>
#include <QTimeLine>

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
    auto enter = coretest::dragEnterEventAt(QPoint(10, 10), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    grid.dragEnterEvent(&enter);
    EXPECT_TRUE(enter.isAccepted()) << "dragEnter rejected a drop the grid is built to accept";

    QMimeData plain;
    plain.setText("just text");
    auto plainEnter =
        coretest::dragEnterEventAt(QPoint(10, 10), Qt::CopyAction, &plain, Qt::LeftButton, Qt::NoModifier);
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

    auto move0 = coretest::dragMoveEventAt(pos0, Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&move0);
    EXPECT_EQ(spy.last().at(0).toInt(), 0);

    auto move1 = coretest::dragMoveEventAt(pos1, Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&move1);
    EXPECT_EQ(spy.last().at(0).toInt(), 1);

    auto moveEmpty = coretest::dragMoveEventAt(QPoint(1, 1), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
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
    auto move0 = coretest::dragMoveEventAt(pos0, Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
    grid.dragMoveEvent(&move0);
    EXPECT_EQ(spy.last().at(0).toInt(), 0);

    QDragLeaveEvent leave;
    grid.dragLeaveEvent(&leave);
    EXPECT_TRUE(leave.isAccepted());

    // a new drag sequence after the leave reports from a clean slate
    auto move0again = coretest::dragMoveEventAt(pos0, Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
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

TEST_F(TestGrid, snapshotMovesReuseWidgetsAndOnlyInvalidateChangedContent) {
    FolderGridView grid;
    grid.resize(500, 600);
    grid.populate(4);
    grid.show();
    const auto before = coretest::orderedThumbnails(grid);
    ASSERT_EQ(before.size(), 4);
    grid.select(QList<int>{1, 2});
    QPointer<ThumbnailWidget> removed = before[0];
    for(int row : {1, 2}) {
        before[row]->setThumbnail(std::make_shared<Thumbnail>(QStringLiteral("file.png"), QString(), 120,
                                                              std::make_shared<QImage>(16, 16, QImage::Format_RGB32)));
    }

    grid.applyRows({{2, {}, true}, {3}, {1, QStringLiteral("renamed.png")}, {-1}});

    const auto after = coretest::orderedThumbnails(grid);
    ASSERT_EQ(after.size(), 4);
    EXPECT_TRUE(removed.isNull());
    EXPECT_EQ(after[0], before[2]);
    EXPECT_FALSE(after[0]->isLoaded);
    EXPECT_EQ(after[1], before[3]);
    EXPECT_EQ(after[2], before[1]);
    EXPECT_TRUE(after[2]->isLoaded);
    EXPECT_EQ(grid.selection(), (QList<int>{2, 0}));
}

namespace {
int thumbnailWidgetsIn(const QGraphicsScene &scene) {
    int count = 0;
    const QList<QGraphicsItem *> sceneItems = scene.items(); // named: no detach
    for(QGraphicsItem *item : sceneItems)
        if(dynamic_cast<ThumbnailWidget *>(item))
            ++count;
    return count;
}

QList<QPointer<ThumbnailWidget>> trackedThumbnails(const QGraphicsScene &scene) {
    QList<QPointer<ThumbnailWidget>> tracked;
    const QList<QGraphicsItem *> sceneItems = scene.items();
    for(QGraphicsItem *item : sceneItems)
        if(auto *widget = dynamic_cast<ThumbnailWidget *>(item))
            tracked << widget;
    return tracked;
}

int stillAlive(const QList<QPointer<ThumbnailWidget>> &tracked) {
    int alive = 0;
    for(const auto &widget : tracked)
        if(widget)
            ++alive;
    return alive;
}
} // namespace

// The materialized window has one owner, ThumbnailWindow: a widget leaving it
// is out of the scene and dead, and nothing in the scene is a widget the
// window does not hold. The three ways a whole window goes away — the
// listing emptying, the strip's settings rebuild, the view's own death — used
// to be three hand-written delete loops in three files; this pins what each
// must leave behind, whichever code does it.
TEST_F(TestGrid, everyWidgetLeavingTheWindowIsDestroyedAndNoneOutlivesItInTheScene) {
    auto *grid = new coretest::TestableFolderGridView();
    grid->resize(500, 600);
    grid->populate(200);
    grid->show();
    QList<QPointer<ThumbnailWidget>> first = trackedThumbnails(*grid->scene());
    ASSERT_FALSE(first.isEmpty()) << "nothing materialized — this pin would pass vacuously";

    // the listing empties: every widget is gone, and the scene holds none
    grid->populate(0);
    EXPECT_EQ(stillAlive(first), 0) << "a widget outlived its row leaving the listing";
    EXPECT_EQ(thumbnailWidgetsIn(*grid->scene()), 0) << "an orphan widget stayed in the scene";

    // and the view's death takes the re-materialized window with it
    grid->populate(200);
    QList<QPointer<ThumbnailWidget>> second = trackedThumbnails(*grid->scene());
    ASSERT_FALSE(second.isEmpty());
    delete grid;
    EXPECT_EQ(stillAlive(second), 0) << "a widget outlived its view";

    // the strip's settings rebuild replaces the window wholesale: the old
    // widgets are dead and the scene holds exactly the new window
    ThumbnailStrip strip;
    strip.resize(800, 140);
    strip.populate(100);
    strip.show();
    QList<QPointer<ThumbnailWidget>> before = trackedThumbnails(*strip.scene());
    ASSERT_FALSE(before.isEmpty()) << "the strip materialized nothing";
    strip.readSettings();
    EXPECT_EQ(stillAlive(before), 0) << "the rebuild kept a widget of the old window";
    EXPECT_GT(thumbnailWidgetsIn(*strip.scene()), 0) << "the rebuild did not re-materialize";
    EXPECT_EQ(trackedThumbnails(*strip.scene()).size(), thumbnailWidgetsIn(*strip.scene()));
}

// The smooth-scroll frame used to repaint by running the whole event loop
// (qApp->processEvents()) from inside QTimeLine::frameChanged. Anything
// queued then ran nested inside a half-finished frame: a wheel event that
// replaces m_scrollTimeLine while the handler still has lines to run against
// it, a presenter diff that rebuilds the window mid-frame. A frame paints and
// returns; queued work runs after it, on the loop's own turn. The probe posts
// one queued call from inside each frame (the scrollbar's valueChanged fires
// inside centerOn) and records whether it ran before that frame's handler
// returned.
TEST_F(TestGrid, aSmoothScrollFrameRunsNoQueuedWorkInsideItself) {
    coretest::TestableFolderGridView grid;
    grid.resize(500, 600);
    grid.populate(2000);
    grid.show();

    QTimeLine *timeline = nullptr;
    bool inFrame = false;
    int posted = 0, ran = 0, ranInsideAFrame = 0;
    QObject::connect(grid.verticalScrollBar(), &QScrollBar::valueChanged, &grid, [&] {
        if(!timeline)
            return;
        inFrame = true;
        ++posted;
        QMetaObject::invokeMethod(
            &grid,
            [&] {
                ++ran;
                if(inFrame)
                    ++ranInsideAFrame;
            },
            Qt::QueuedConnection);
    });

    grid.scrollSmooth(-1500, 1.0, 1.0, false);
    const QList<QTimeLine *> timelines = grid.findChildren<QTimeLine *>();
    for(QTimeLine *candidate : timelines)
        if(candidate->state() == QTimeLine::Running)
            timeline = candidate;
    ASSERT_NE(timeline, nullptr) << "scrollSmooth started no timeline";
    // connected after the view's own handler, so this runs when that returns
    QObject::connect(timeline, &QTimeLine::frameChanged, &grid, [&] {
        inFrame = false;
    });

    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return timeline->state() == QTimeLine::NotRunning;
        },
        5000));
    QCoreApplication::processEvents(); // drain the last frame's post
    ASSERT_GT(posted, 0) << "no frame moved the scrollbar — this pin would pass vacuously";
    EXPECT_EQ(ran, posted);
    EXPECT_EQ(ranInsideAFrame, 0) << "queued work ran nested inside a scroll frame";
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// The view is widget-virtualized: rows near the viewport carry widgets, the
// rest are a count. These pins keep the materialized window bounded and the
// index-based state (selection, focus geometry) working across it.
TEST_F(TestGrid, coldPopulateMaterializesOnlyTheViewportWindow) {
    coretest::TestableFolderGridView grid;
    grid.resize(500, 600);
    const int big = 15413;
    grid.populate(big);
    grid.show();
    // the viewport (~4 rows x 3 cols) plus the 2300px preload band, twice for
    // band symmetry — anything at or beyond the full listing is a regression
    // to eager materialization
    const int materialized = grid.scene()->items().count();
    EXPECT_LT(materialized, 500) << "materialized widgets for the whole listing";
    EXPECT_EQ(grid.itemCount(), big) << "the row count is the model, not the widget count";
}

TEST_F(TestGrid, selectingAndFocusingAnOffWindowRowSurvivesVirtualization) {
    coretest::TestableFolderGridView grid;
    grid.resize(500, 600);
    grid.populate(15413);
    grid.show();
    // far beyond any materialized window (named farRow: windef.h defines `far` as an empty macro)
    const int farRow = 15000;
    grid.select(farRow);
    EXPECT_EQ(grid.selection(), (QList<int>{farRow}));
    // the arithmetic geometry scrolls to a row that has no widget
    grid.focusOn(farRow);
    EXPECT_FALSE(grid.verticalScrollBar()->value() == 0) << "focusOn an off-window row did not scroll";
    // after the scroll, the row's widget is materialized and carries the
    // highlight the selection promised
    QTest::qWait(50);
    bool found = false;
    const QList<ThumbnailWidget *> highlighted = coretest::orderedThumbnails(grid);
    for(auto *widget : highlighted)
        if(widget->rowIndex() == farRow && widget->isHighlighted())
            found = true;
    EXPECT_TRUE(found) << "the selected off-window row did not surface highlighted";
}

TEST_F(TestGrid, thumbnailDeliveryToAnOffWindowRowIsDroppedAndReRequestedOnScroll) {
    coretest::TestableFolderGridView grid;
    grid.resize(500, 600);
    grid.populate(15413);
    grid.show();
    const int farRow = 15000;
    auto thumb = std::make_shared<Thumbnail>(QStringLiteral("far.png"), QString(), 120,
                                             std::make_shared<QImage>(16, 16, QImage::Format_RGB32));
    // no widget at that index: the delivery is dropped, not crashed into
    grid.setThumbnail(farRow, thumb);
    // scrolling there materializes the row and re-requests its thumbnail
    grid.focusOn(farRow);
    QTest::qWait(50);
    bool found = false;
    const QList<ThumbnailWidget *> materialized = coretest::orderedThumbnails(grid);
    for(auto *widget : materialized)
        if(widget->rowIndex() == farRow)
            found = true;
    EXPECT_TRUE(found) << "the scrolled-to row did not materialize";
}
