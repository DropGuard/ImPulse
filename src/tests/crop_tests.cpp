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

#include "gui/crop/cropcontroller.h"
#include "gui/crop/cropoverlay.h"
#include "gui/crop/croppanel.h"
class TestCrop : public ::testing::Test {
  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons;
    QString m_base, m_alpha;

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
        m_base = QDir(m_home.path()).filePath("crop");
        m_alpha = QDir(m_base).filePath("alpha");
        EXPECT_TRUE(QDir().mkpath(m_alpha));
        EXPECT_TRUE(QDir().mkpath(m_base));
    }
};

// entering crop used to force a fit window ("so CropOverlay won't go
// crazy") — throwing away the zoom the user had framed the crop in. The
// overlay's geometry is pushed from the viewer at entry and the zoom
// relays are physically disconnected while cropping, so there is nothing
// for the reset to guard: the view must be exactly where the user left it.
TEST_F(TestCrop, cropEntryRespectsTheUserZoom) {
    QImage big(2000, 1200, QImage::Format_RGB32);
    big.fill(Qt::darkCyan);
    EXPECT_TRUE(big.save(QDir(m_base).filePath("big.png"), "PNG"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("big.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == QDir(m_base).filePath("big.png");
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("big.png"));
    core.mainWindow()->fitOriginal();
    auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
    ASSERT_TRUE(viewer);
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return viewer->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(viewer->fitMode(), FIT_ORIGINAL);
    float scaleBefore = viewer->currentScale();

    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    EXPECT_EQ(viewer->fitMode(), FIT_ORIGINAL);
    EXPECT_TRUE(qAbs(viewer->currentScale() - scaleBefore) < 0.001f) << "the zoom must survive crop entry";
}

// X on a framed view — mode 3, free-zoomed to 1.44x, panned, in a
// visible 1600x900 window — must preserve everything the user framed:
// scale, mode and the viewport rect, exactly. The old entry forced a
// fit window ("so CropOverlay won't go crazy"), resetting the very
// framing the user had composed.
TEST_F(TestCrop, cropEntryPreservesFramedView) {
    QImage big(2000, 1200, QImage::Format_RGB32);
    big.fill(Qt::darkMagenta);
    EXPECT_TRUE(big.save(QDir(m_base).filePath("framed.png"), "PNG"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("framed.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == QDir(m_base).filePath("framed.png");
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("framed.png"));
    auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
    ASSERT_TRUE(viewer);
    core.mainWindow()->resize(1600, 900);
    core.mainWindow()->show();
    coretest::drainEvents();
    core.mainWindow()->fitOriginal();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return viewer->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(viewer->fitMode(), FIT_ORIGINAL);
    viewer->zoomIn();
    viewer->zoomIn();
    viewer->scrollLeft();
    coretest::drainEvents();
    float scaleBefore = viewer->currentScale();
    QRect rectBefore = viewer->imageRect();
    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    coretest::drainEvents();
    EXPECT_TRUE(qFuzzyCompare(viewer->currentScale(), scaleBefore));
    EXPECT_EQ(static_cast<int>(viewer->fitMode()), static_cast<int>(FIT_FREE));
    EXPECT_EQ(viewer->imageRect(), rectBefore);
}

// zoom stays available while crop is active (Framing policy): it is how
// the user fine-frames the selection, and the selection is image-anchored
// so it rides the zoom unchanged. Fit stays blocked — a fit would reset
// the view the user is framing in.
TEST_F(TestCrop, cropAllowsZoomFineTuning) {
    QImage big(2000, 1200, QImage::Format_RGB32);
    big.fill(Qt::darkYellow);
    EXPECT_TRUE(big.save(QDir(m_base).filePath("fine.png"), "PNG"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("fine.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == QDir(m_base).filePath("fine.png");
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("fine.png"));
    auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
    ASSERT_TRUE(viewer);
    core.mainWindow()->fitOriginal();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return viewer->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(viewer->fitMode(), FIT_ORIGINAL);
    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    auto *overlay = core.mainWindow()->findChild<CropOverlay *>();
    ASSERT_TRUE(overlay);
    overlay->selectAll();

    viewer->zoomIn();
    EXPECT_TRUE(viewer->currentScale() > 1.0f);
    // the selection is image-anchored: zooming re-derives the mapping,
    // the selection itself does not move or scale
    EXPECT_EQ(overlay->imageScale(), viewer->currentScale());

    // panning works while framing: the image rect moves under the
    // overlay, and the mapping re-derives (canvasViewportChanged)
    QRect rectBefore = viewer->imageRect();
    viewer->scrollLeft();
    // scrollSmooth animates through a QTimeLine — poll until it moves
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return viewer->imageRect() != rectBefore;
        },
        2000));
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());

    // fit remains blocked while framing — a fit would reset the very
    // view the user is framing in
    float scale = viewer->currentScale();
    viewer->fitWindow();
    EXPECT_TRUE(qAbs(viewer->currentScale() - scale) < 0.001f);
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());

    // leaving crop releases the policy: the fit keys work again — the
    // mode semantics were never vetoed, only deferred while the overlay
    // held the canvas. (No navigation assert here: fork-per-slot means
    // this fork's directory holds only the files this test created, and
    // a cross-test file assumption is exactly what the isolation
    // forbids — prevImage in a one-file directory legitimately no-ops.
    // The arrows' navigation is pinned in
    // fitOriginalPersistsAndArrowsStayInInspectMode.)
    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(!core.mainWindow()->crop()->isActive());
    viewer->fitWindow();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return viewer->fitMode() == FIT_WINDOW;
        },
        5000));
    EXPECT_EQ(viewer->fitMode(), FIT_WINDOW);
}

// Enter is the crop's own key: with no selection it is consumed — it
// must not resolve as folder view (its global binding), with or without
// the pending-work lock. Owned keys do not pass through.
TEST_F(TestCrop, cropOwnsEnterEvenWithoutSelection) {
    QImage img(64, 32, QImage::Format_RGB32);
    img.fill(Qt::red);
    EXPECT_TRUE(img.save(QDir(m_base).filePath("enter.png"), "PNG"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("enter.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == QDir(m_base).filePath("enter.png");
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("enter.png"));
    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    auto *overlay = core.mainWindow()->findChild<CropOverlay *>();
    ASSERT_TRUE(overlay);

    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(overlay, &enter);

    EXPECT_TRUE(core.mainWindow()->crop()->isActive());                                            // the surface stayed
    EXPECT_EQ(core.viewState().viewMode.value(), MODE_DOCUMENT);                                   // no folder switch
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("enter.png"));                         // no navigation
    EXPECT_TRUE(!core.directoryModel()->getImage(QDir(m_base).filePath("enter.png"))->isEdited()); // nothing applied
}

TEST_F(TestCrop, cropPanelSurvivesAnEarlyTrigger) {
    writeImage(QDir(m_base).filePath("crop.png"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("crop.png")));
    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(!core.mainWindow()->crop()->isActive());
}

TEST_F(TestCrop, cropPanelConnectionsDeliver) {
    // the overlay pulls its geometry from the viewer, so the fixture is a
    // real viewer with a 100x100 image at scale 1.0 — imageRect reads
    // (0,0,100,100), exactly what the old push used to fake
    ViewerWidget viewer;
    QPixmap pm(100, 100);
    pm.fill(Qt::red);
    viewer.showImage(std::make_unique<QPixmap>(pm));
    CropOverlay overlay(&viewer);
    CropPanel panel(&overlay);
    panel.setImageRealSize(QSize(100, 100));

    auto *cancelBtn = panel.findChild<QPushButton *>("cancelButton");
    auto *cropBtn = panel.findChild<QPushButton *>("cropButton");
    auto *cropSaveBtn = panel.findChild<QPushButton *>("cropSaveButton");
    auto *posX = panel.findChild<QSpinBox *>("posX");
    auto *width = panel.findChild<QSpinBox *>("width");
    auto *height = panel.findChild<QSpinBox *>("height");
    ASSERT_TRUE(cancelBtn && cropBtn && cropSaveBtn && posX && width && height);

    QSignalSpy cancel(&panel, &CropPanel::cancel);
    QSignalSpy crop(&panel, &CropPanel::crop);
    QSignalSpy cropSave(&panel, &CropPanel::cropAndSave);
    QSignalSpy selection(&panel, &CropPanel::selectionChanged);
    QSignalSpy overlaySelection(&overlay, &CropOverlay::selectionChanged);

    // CropPanel selectAll -> CropOverlay::selectAll, and the overlay's
    // selection travels back into the geometry boxes (both directions).
    // This is what gives the rest of the test a real selection to crop.
    emit panel.selectAll();
    EXPECT_EQ(overlaySelection.count(), 1);
    EXPECT_EQ(width->value(), 100);
    EXPECT_EQ(height->value(), 100);

    // ui->cancelButton clicked -> CropPanel::cancel, nothing else
    cancelBtn->click();
    EXPECT_EQ(cancel.count(), 1);
    EXPECT_EQ(crop.count(), 0);
    EXPECT_EQ(cropSave.count(), 0);

    // ui->width valueChanged -> onSelectionChange -> selectionChanged
    selection.clear();
    width->setValue(50);
    EXPECT_EQ(selection.count(), 1);

    // ui->cropButton clicked -> doCrop -> crop(rect); cancel stays put.
    // doCrop builds its target from the live boxes, so that is what the
    // expectation is read from; the precondition is asserted separately
    // because the overlay writes its own selection back into the boxes.
    const QRect target(posX->value(), panel.findChild<QSpinBox *>("posY")->value(), width->value(), height->value());
    ASSERT_TRUE(target.width() > 0 && target.height() > 0 && target.size() != QSize(100, 100)) << qPrintable(
        QStringLiteral("precondition broken: selection is ") +
        QStringLiteral("%1x%2, doCrop would emit cancel instead of crop").arg(target.width()).arg(target.height()));
    cropBtn->click();
    EXPECT_EQ(crop.count(), 1);
    EXPECT_EQ(crop[0][0].toRect(), target);
    EXPECT_EQ(cancel.count(), 1);
    EXPECT_EQ(cropSave.count(), 0);

    // ui->cropSaveButton clicked -> doCropSave -> cropAndSave(rect)
    cropSaveBtn->click();
    EXPECT_EQ(cropSave.count(), 1);
    EXPECT_EQ(cropSave[0][0].toRect(), target);
    EXPECT_EQ(cancel.count(), 1);

    // ui->posX valueChanged -> selectionChanged
    selection.clear();
    posX->setValue(7);
    EXPECT_EQ(selection.count(), 1);

    // overlay escPressed -> cancel only: a connection to either crop
    // action would move crop/cropSave and leave cancel at zero
    cancel.clear();
    emit overlay.escPressed();
    EXPECT_EQ(cancel.count(), 1);
    EXPECT_EQ(crop.count(), 1);
    EXPECT_EQ(cropSave.count(), 1);

    // overlay cropApply -> doCrop -> crop(rect), deterministic now: the
    // keyboard's apply is always the in-memory crop, whatever the
    // default-action setting emphasizes for the buttons
    cancel.clear();
    emit overlay.cropApply();
    EXPECT_EQ(crop.count(), 2);
    EXPECT_EQ(cropSave.count(), 1);
    EXPECT_EQ(cancel.count(), 0);
}

TEST_F(TestCrop, cropAndSaveWritesTheCroppedFile) {
    Core core;
    coretest::FakeInteractions fake;
    core.setUserInteractions(&fake);
    const QString img = QDir(m_alpha).filePath("rect.png");
    coretest::writeRectImage(img);
    EXPECT_TRUE(core.loadPath(img));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));

    core.cropAndSave(QRect(0, 0, 8, 8));

    EXPECT_EQ(QImage(img).size(), QSize(8, 8)); // cropped AND saved to disk
    EXPECT_EQ(fake.confirmCalls, 0);            // cropAndSave bypasses the confirmation
}

// ---- FolderGridView's navigation cluster: the selection math (columns,
// shifted columns on irregular last rows, page jumps) and the zoom clamps,
// driven on a real sized+shown grid. Column counts are self-calibrated
// from the observed selectBelow jump instead of hardcoding layout
// arithmetic, so the tests survive spacing/margin changes.

// The cutover half of #365: a key event into the grid's keyPressEvent
// resolves through the grid's OWN scope table, whose defaults are the
// literal translation of the hardcoded switch this replaced — so each
// default binding is driven as a physical key and the grid must behave
// exactly as the hardcoded era did. The actions' receivers live in the
// Core window (proxy → view → grid, pinned by
// gridActionsDriveTheGridThroughTheFullChain); this pin is the key half,
// so the movement methods are connected locally. Scan codes are
// self-calibrated from the platform's own table — a synthesized event
// with scan code 0 yields an empty shortcut name and resolves to
// nothing, which is exactly the trap the sweep must not sit on.
TEST_F(TestCrop, gridKeysResolveThroughTheScopeTable) {
    coretest::TestableFolderGridView grid;
    grid.resize(500, 600);
    grid.populate(25);
    grid.show();
    grid.select(0);
    // the grid-scope wiring, re-created the way core.cpp's
    // connectWindowActions lands it: connectAction on the registry
    actionManager->connectAction("gridSelectNext", &grid, &FolderGridView::selectNext);
    actionManager->connectAction("gridSelectPrev", &grid, &FolderGridView::selectPrev);
    actionManager->connectAction("gridSelectAbove", &grid, &FolderGridView::selectAbove);
    actionManager->connectAction("gridSelectBelow", &grid, &FolderGridView::selectBelow);
    actionManager->connectAction("gridPageUp", &grid, &FolderGridView::pageUp);
    actionManager->connectAction("gridPageDown", &grid, &FolderGridView::pageDown);
    actionManager->connectAction("gridSelectFirst", &grid, &FolderGridView::selectFirst);
    actionManager->connectAction("gridSelectLast", &grid, &FolderGridView::selectLast);
    actionManager->connectAction("gridSelectAll", &grid, &FolderGridView::selectAll);
    actionManager->connectAction("gridOpen", &grid, &FolderGridView::activateCurrent);

    // the grid surface goes live: the scope's keys ride QShortcuts on the
    // grid and fire through QShortcutMap on window-delivered keys
    actionManager->attachGridSurface(&grid);
    grid.activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(&grid));
    grid.setFocus();
    EXPECT_TRUE(grid.hasFocus());

    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Right));
    EXPECT_EQ(coretest::lastSelection(grid), 1);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Left));
    EXPECT_EQ(coretest::lastSelection(grid), 0);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Left));
    EXPECT_EQ(coretest::lastSelection(grid), 0); // past the start: stays
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Down));
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Up));
    EXPECT_EQ(coretest::lastSelection(grid), 0);

    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_End));
    EXPECT_EQ(coretest::lastSelection(grid), 24);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Home));
    EXPECT_EQ(coretest::lastSelection(grid), 0);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_PageDown));
    const int paged = coretest::lastSelection(grid);
    EXPECT_TRUE(paged > 0) << "PgDown never left the first row";
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_PageUp));
    EXPECT_EQ(coretest::lastSelection(grid), 0); // and back

    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_A, Qt::ControlModifier));
    EXPECT_EQ(grid.selection().count(), 25); // Ctrl+A: everything

    QSignalSpy up(actionManager->action("goUp"), &QAction::triggered);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Backspace));
    EXPECT_EQ(up.count(), 1); // goUp is the grid scope's own binding

    QSignalSpy activated(&grid, &FolderGridView::itemActivated);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Return));
    EXPECT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.first().first().toInt(), 0);

    grid.select(5);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Escape));
    EXPECT_EQ(coretest::lastSelection(grid), 5); // an unscoped key never moves the grid

    // activation on an empty selection is guarded, not emitted blind.
    // This grid is never attached or shown, so it has no QShortcut
    // surface: the pin is that an UNCLAIMED Return still cannot activate,
    // which is the grid's own guard rather than the map's.
    coretest::TestableFolderGridView emptyGrid;
    emptyGrid.resize(300, 300);
    emptyGrid.populate(2);
    emptyGrid.show();
    QSignalSpy emptyActivation(&emptyGrid, &FolderGridView::itemActivated);
    coretest::pressGridKey(emptyGrid, "Enter", Qt::Key_Return);
    EXPECT_EQ(emptyActivation.count(), 0);

    // the QShortcutMap leg, kept last so the door pins above see the
    // selection state they built: with the grid surface attached, a
    // window-delivered key fires the grid's projected QShortcut before
    // any keyPressEvent exists to forward it — and the de-claimed global
    // table stays out of the way (no ambiguity, no stray nextImage)
    grid.select(0);
    actionManager->attachGridSurface(&grid);
    grid.activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(&grid));
    grid.setFocus();
    EXPECT_TRUE(grid.hasFocus());
    EXPECT_TRUE(QApplication::focusWidget() != nullptr);
    QSignalSpy nextImageGlobal(actionManager->action("nextImage"), &QAction::triggered);
    QSignalSpy mapActivated(&grid, &FolderGridView::itemActivated);
    const int before = coretest::lastSelection(grid);
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Right));
    EXPECT_EQ(coretest::lastSelection(grid), before + 1); // via the grid's QShortcut
    EXPECT_EQ(nextImageGlobal.count(), 0);                // the de-claimed global binding stood down
    ASSERT_TRUE(coretest::deliverWindowKey(&grid, Qt::Key_Return));
    EXPECT_EQ(mapActivated.count(), 1); // Enter claims both Returns
    actionManager->attachGridSurface(nullptr);
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
