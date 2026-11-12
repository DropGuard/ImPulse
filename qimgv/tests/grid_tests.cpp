#include <QTest>
#include <sys/wait.h>
#include <unistd.h>
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
#include "utils/actions.h"
#include "gui/dialogs/settingsdialog.h"

class TestGrid : public QObject {
    Q_OBJECT

  private:
    IsolatedHome home;
    coretest::AppSingletons singletons;
    QString base;

    void writeImage(const QString &path) {
        QVERIFY(makeImageFile(path, QSize(8, 8)));
    }
    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(base).filePath(tag + QStringLiteral("-{}").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

  private slots:
    void initTestCase() {
        QVERIFY(home.path() != nullptr);
        base = QDir(home.path()).filePath("grid");
        QVERIFY(QDir().mkpath(base));
    }

    void navigationMovesByColumnsAndBack() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        grid.select(0);

        grid.selectBelow();
        const int rowJump = coretest::lastSelection(grid); // = the column count
        QVERIFY2(rowJump >= 1, "selectBelow must leave the first row");
        grid.selectBelow();
        QCOMPARE(coretest::lastSelection(grid), rowJump * 2); // column-preserving on a regular row
        grid.selectAbove();
        QCOMPARE(coretest::lastSelection(grid), rowJump);
        grid.selectAbove();
        QCOMPARE(coretest::lastSelection(grid), 0);
        grid.selectAbove(); // first row: already at the top, stays
        QCOMPARE(coretest::lastSelection(grid), 0);
    }

    void navigationRefusesToLeaveTheGridEdges() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        grid.select(0);

        grid.selectAbove(); // first row: already at the top
        QCOMPARE(coretest::lastSelection(grid), 0);

        grid.select(24);    // the last item
        grid.selectBelow(); // last row: stays
        QCOMPARE(coretest::lastSelection(grid), 24);
        grid.selectNext(); // past the end: stays
        QCOMPARE(coretest::lastSelection(grid), 24);
        grid.selectPrev();
        QCOMPARE(coretest::lastSelection(grid), 23);
        grid.selectPrev();
        QCOMPARE(coretest::lastSelection(grid), 22);
        grid.select(0);
        grid.selectPrev(); // before the start: stays
        QCOMPARE(coretest::lastSelection(grid), 0);
    }

    void navigationRestoresTheColumnOnIrregularRows() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(23); // 23 % cols != 0: the last row is short
        grid.show();
        grid.select(0);
        grid.selectBelow();
        const int cols = coretest::lastSelection(grid);
        QVERIFY2(cols >= 2, "this calibration needs at least two columns");
        // the last item of the second-to-last row: its column lies beyond the
        // short last row, so selectBelow clamps and the shifted-column logic
        // must remember the original column for selectAbove to restore it
        const int rows = 23 / cols + 1;
        const int orig = (rows - 2) * cols + (cols - 1);
        grid.select(orig);

        grid.selectBelow();
        QVERIFY(coretest::lastSelection(grid) != orig);
        grid.selectAbove();

        QCOMPARE(coretest::lastSelection(grid), orig); // the shifted column was restored
    }

    void pageJumpsMoveFourRows() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        grid.select(0);
        grid.selectBelow();
        const int cols = coretest::lastSelection(grid);
        grid.select(0); // pageDown jumps from lastSelected(): restart from the top

        grid.pageDown();
        QCOMPARE(coretest::lastSelection(grid), 4 * cols); // exactly four rows down
        grid.pageUp();
        QCOMPARE(coretest::lastSelection(grid), 0); // and exactly four rows back
    }

    void zoomClampsToTheDocumentedLimits() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(4);
        grid.show();
        QSignalSpy spy(&grid, &FolderGridView::thumbnailSizeChanged);
        QVERIFY(spy.isValid());

        for(int i = 0; i < 20; ++i)
            grid.zoomIn();
        QVERIFY(spy.count() > 0);
        QCOMPARE(spy.at(spy.count() - 1).at(0).toInt(), grid.THUMBNAIL_SIZE_MAX);

        for(int i = 0; i < 40; ++i)
            grid.zoomOut();
        QCOMPARE(spy.at(spy.count() - 1).at(0).toInt(), grid.THUMBNAIL_SIZE_MIN);
    }

    void dragEnterAcceptsUnconditionally() {
        coretest::TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(1);
        grid.show();

        auto mime = std::make_unique<QMimeData>();
        mime->setUrls({QUrl::fromLocalFile("/tmp/whatever.png")});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        grid.dragEnterEvent(&enter);
        QVERIFY2(enter.isAccepted(), "dragEnter rejected a drop the grid is built to accept");

        QMimeData plain;
        plain.setText("just text");
        QDragEnterEvent plainEnter(QPoint(10, 10), Qt::CopyAction, &plain, Qt::LeftButton, Qt::NoModifier);
        grid.dragEnterEvent(&plainEnter);
        QVERIFY2(plainEnter.isAccepted(), "the grid accepts without inspecting the mime — reporting is the drop's job");
    }

    void dragMoveReportsTheHoveredThumbnailIndex() {
        coretest::TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QSignalSpy spy(&grid, &FolderGridView::draggedOver);
        QVERIFY(spy.isValid());

        const QPoint pos0 = coretest::dropPosOverThumbnail(grid, 0);
        const QPoint pos1 = coretest::dropPosOverThumbnail(grid, 1);
        QVERIFY2(pos0 != pos1, "the two thumbnails share a position — layout never ran");

        QDragMoveEvent move0(QPoint(pos0), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
        grid.dragMoveEvent(&move0);
        QCOMPARE(spy.last().at(0).toInt(), 0);

        QDragMoveEvent move1(QPoint(pos1), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
        grid.dragMoveEvent(&move1);
        QCOMPARE(spy.last().at(0).toInt(), 1);

        QDragMoveEvent moveEmpty(QPoint(1, 1), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
        grid.dragMoveEvent(&moveEmpty);
        QCOMPARE(spy.last().at(0).toInt(), -1); // off-grid: the consumer learns "no target"
    }

    void dragReportingSurvivesALeave() {
        coretest::TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QSignalSpy spy(&grid, &FolderGridView::draggedOver);
        QVERIFY(spy.isValid());

        const QPoint pos0 = coretest::dropPosOverThumbnail(grid, 0);
        QDragMoveEvent move0(QPoint(pos0), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
        grid.dragMoveEvent(&move0);
        QCOMPARE(spy.last().at(0).toInt(), 0);

        QDragLeaveEvent leave;
        grid.dragLeaveEvent(&leave);
        QVERIFY(leave.isAccepted());

        // a new drag sequence after the leave reports from a clean slate
        QDragMoveEvent move0again(QPoint(pos0), Qt::CopyAction, nullptr, Qt::LeftButton, Qt::NoModifier);
        grid.dragMoveEvent(&move0again);
        QCOMPARE(spy.last().at(0).toInt(), 0);
    }

    void dropOutsideAnyThumbnailReportsMinusOne() {
        coretest::TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QSignalSpy spy(&grid, &FolderGridView::droppedInto);
        QVERIFY(spy.isValid());

        auto mime = std::make_unique<QMimeData>();
        mime->setUrls({QUrl::fromLocalFile("/tmp/x.png")});
        QDropEvent drop(QPointF(1, 1), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        grid.dropEvent(&drop);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(2).toInt(), -1); // the consumer decides: nothing to highlight
    }

    void dropReportsMimeSourceAndLandingIndex() {
        coretest::TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QSignalSpy spy(&grid, &FolderGridView::droppedInto);
        QVERIFY(spy.isValid());

        auto mime = std::make_unique<QMimeData>();
        const QList<QUrl> urls{QUrl::fromLocalFile("/tmp/dropped1.png"), QUrl::fromLocalFile("/tmp/dropped2.png")};
        mime->setUrls(urls);

        const QPoint pos1 = coretest::dropPosOverThumbnail(grid, 1);
        QDropEvent drop(QPointF(pos1), Qt::CopyAction, mime.get(), Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
        grid.dropEvent(&drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<const QMimeData *>(), mime.get()); // payload passes through
        QCOMPARE(spy.at(0).at(1).value<QObject *>(), nullptr);            // no source (external drop)
        QCOMPARE(spy.at(0).at(2).toInt(), 1);                             // landed on thumbnail #1
        QCOMPARE(spy.at(0).at(0).value<const QMimeData *>()->urls(), urls);
    }

    // The row is the unit a rename moves. Driven straight at the view because
    // populate() is a fixture of real rows with no filesystem behind them, and
    // everything at stake here is view arithmetic: the widget surviving (its
    // pixels ride along with its name), the rows it jumped over sliding the other
    // way, and the selection following the row instead of staying on the vacated
    // index — that last one is the visible bug shape, a highlight left behind on
    // a row the user is no longer on.
    void renameMovesTheRowAndCarriesItsWidgetAndSelection() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        const QList<ThumbnailWidget *> before = coretest::orderedThumbnails(grid);
        QVERIFY2(before.count() == 25, qPrintable(QStringLiteral("populate(25) laid out %1 rows").arg(before.count())));
        grid.select(1);
        ThumbnailWidget *const moving = before.at(1);

        grid.renameItem(1, 4, QStringLiteral("moved.png"));

        const QList<ThumbnailWidget *> after = coretest::orderedThumbnails(grid);
        QCOMPARE(after.count(), 25);
        QVERIFY2(after.at(4) == moving, "the moved row is a different widget — renaming does not recreate the row");
        QVERIFY2(after.at(1) == before.at(2) && after.at(3) == before.at(4),
                 "the rows the move jumped over did not slide into the space it left");
        QCOMPARE(grid.selection(), QList<int>({4}));

        // and back again: the same rows slide the other way
        grid.renameItem(4, 0, QStringLiteral("front.png"));
        const QList<ThumbnailWidget *> back = coretest::orderedThumbnails(grid);
        QVERIFY2(back.at(0) == moving, "the row did not move back to the front");
        QVERIFY2(back.at(1) == before.at(0) && back.at(4) == before.at(4),
                 "the rows it passed going back did not shift down");
        QCOMPARE(grid.selection(), QList<int>({0}));

        // an in-place rename — the name-sort case where the row does not move —
        // touches neither the layout nor the selection
        grid.renameItem(0, 0, QStringLiteral("still.png"));
        QVERIFY2(coretest::orderedThumbnails(grid).at(0) == moving, "an in-place rename moved a row");
        QCOMPARE(grid.selection(), QList<int>({0}));

        // out-of-range on either side is refused rather than clamped: the caller
        // is a live event about a row the view may already have dropped
        grid.renameItem(0, 99, QStringLiteral("nowhere.png"));
        QCOMPARE(coretest::orderedThumbnails(grid).count(), 25);
        QVERIFY2(coretest::orderedThumbnails(grid).at(0) == moving, "an out-of-range rename moved a row");

        // A multi-row selection is the other half of the remap: the selected row
        // moves and the selected rows it passes shift the other way. One selected
        // index reaches only the first branch, and the branches it does not reach
        // are the ones that leave a highlight on the wrong picture.
        grid.select(QList<int>{1, 2, 3});
        grid.renameItem(1, 4, QStringLiteral("moved.png"));
        QList<int> shifted = grid.selection();
        std::sort(shifted.begin(), shifted.end());
        QCOMPARE(shifted, QList<int>({1, 2, 4}));

        grid.renameItem(4, 0, QStringLiteral("front.png"));
        shifted = grid.selection();
        std::sort(shifted.begin(), shifted.end());
        QCOMPARE(shifted, QList<int>({0, 2, 3}));
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // One process per test function — the same rationale as engine_stack_main's
    // fork per class, taken one level down: the suites decode images, which
    // makes QtGui spawn and clockjoin-join its own internal pooled threads, and
    // each of those joins leaves a registry-live pthread_t behind (TSan does not
    // intercept clockjoin — LLVM #239326).
    //
    // The fixture is constructed INSIDE the child, not before the fork: a
    // fixture built in the parent would hand every child an IsolatedHome whose
    // tmpDir dies with the first child to exit.
    if(argc > 1) {
        TestGrid t;
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    const QMetaObject *mo = &TestGrid::staticMetaObject;
    QStringList testSlots;
    for(int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        const QByteArray name = m.name();
        if(m.methodType() == QMetaMethod::Slot && m.access() == QMetaMethod::Private && m.parameterCount() == 0 &&
           name != "initTestCase" && name != "cleanupTestCase" && name != "init" && name != "cleanup")
            testSlots << QString::fromUtf8(name);
    }
    int failures = 0;
    for(const QString &slot : std::as_const(testSlots)) {
        fflush(nullptr); // fork would duplicate whatever is buffered in stdout
        pid_t pid = fork();
        if(pid == 0) {
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestGrid t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        // a crashed child loses its buffered output entirely — without naming
        // the slot here, the evidence is a red ctest with nothing in the log
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "grid_tests: slot '%s' exited abnormally (exit=%d, sig=%d)\n", slot.toUtf8().constData(),
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "grid_tests.moc"
