#include <QTest>
#include "slot_harness.h"
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
class TestCrop : public QObject {
    Q_OBJECT

  private:
    IsolatedHome home;
    coretest::AppSingletons singletons;
    QString base, alpha;

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
        base = QDir(home.path()).filePath("crop");
        alpha = QDir(base).filePath("alpha");
        QVERIFY(QDir().mkpath(alpha));
        QVERIFY(QDir().mkpath(base));
    }

    // entering crop used to force a fit window ("so CropOverlay won't go
    // crazy") — throwing away the zoom the user had framed the crop in. The
    // overlay's geometry is pushed from the viewer at entry and the zoom
    // relays are physically disconnected while cropping, so there is nothing
    // for the reset to guard: the view must be exactly where the user left it.
    void cropEntryRespectsTheUserZoom() {
        QImage big(2000, 1200, QImage::Format_RGB32);
        big.fill(Qt::darkCyan);
        QVERIFY(big.save(QDir(base).filePath("big.png"), "PNG"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("big.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("big.png"), 10000);
        core.mainWindow()->fitOriginal();
        auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        float scaleBefore = viewer->currentScale();

        core.mainWindow()->crop()->toggle();
        QVERIFY(core.mainWindow()->crop()->isActive());
        QCOMPARE(viewer->fitMode(), FIT_ORIGINAL);
        QVERIFY2(qAbs(viewer->currentScale() - scaleBefore) < 0.001f, "the zoom must survive crop entry");
    }

    // X on a framed view — mode 3, free-zoomed to 1.44x, panned, in a
    // visible 1600x900 window — must preserve everything the user framed:
    // scale, mode and the viewport rect, exactly. The old entry forced a
    // fit window ("so CropOverlay won't go crazy"), resetting the very
    // framing the user had composed.
    void cropEntryPreservesFramedView() {
        QImage big(2000, 1200, QImage::Format_RGB32);
        big.fill(Qt::darkMagenta);
        QVERIFY(big.save(QDir(base).filePath("framed.png"), "PNG"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("framed.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("framed.png"), 10000);
        auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        core.mainWindow()->resize(1600, 900);
        core.mainWindow()->show();
        coretest::drainEvents();
        core.mainWindow()->fitOriginal();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        viewer->zoomIn();
        viewer->zoomIn();
        viewer->scrollLeft();
        coretest::drainEvents();
        float scaleBefore = viewer->currentScale();
        QRect rectBefore = viewer->imageRect();
        core.mainWindow()->crop()->toggle();
        QVERIFY(core.mainWindow()->crop()->isActive());
        coretest::drainEvents();
        QVERIFY(qFuzzyCompare(viewer->currentScale(), scaleBefore));
        QCOMPARE(static_cast<int>(viewer->fitMode()), static_cast<int>(FIT_FREE));
        QCOMPARE(viewer->imageRect(), rectBefore);
    }

    // zoom stays available while crop is active (Framing policy): it is how
    // the user fine-frames the selection, and the selection is image-anchored
    // so it rides the zoom unchanged. Fit stays blocked — a fit would reset
    // the view the user is framing in.
    void cropAllowsZoomFineTuning() {
        QImage big(2000, 1200, QImage::Format_RGB32);
        big.fill(Qt::darkYellow);
        QVERIFY(big.save(QDir(base).filePath("fine.png"), "PNG"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("fine.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("fine.png"), 10000);
        auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        core.mainWindow()->fitOriginal();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        core.mainWindow()->crop()->toggle();
        QVERIFY(core.mainWindow()->crop()->isActive());
        auto *overlay = core.mainWindow()->findChild<CropOverlay *>();
        QVERIFY(overlay);
        overlay->selectAll();

        viewer->zoomIn();
        QVERIFY(viewer->currentScale() > 1.0f);
        // the selection is image-anchored: zooming re-derives the mapping,
        // the selection itself does not move or scale
        QCOMPARE(overlay->imageScale(), viewer->currentScale());

        // panning works while framing: the image rect moves under the
        // overlay, and the mapping re-derives (canvasViewportChanged)
        QRect rectBefore = viewer->imageRect();
        viewer->scrollLeft();
        // scrollSmooth animates through a QTimeLine — poll until it moves
        QTRY_VERIFY_WITH_TIMEOUT(viewer->imageRect() != rectBefore, 2000);
        QVERIFY(core.mainWindow()->crop()->isActive());

        // fit remains blocked while framing — a fit would reset the very
        // view the user is framing in
        float scale = viewer->currentScale();
        viewer->fitWindow();
        QVERIFY(qAbs(viewer->currentScale() - scale) < 0.001f);
        QVERIFY(core.mainWindow()->crop()->isActive());

        // leaving crop releases the policy: the fit keys work again — the
        // mode semantics were never vetoed, only deferred while the overlay
        // held the canvas. (No navigation assert here: fork-per-slot means
        // this fork's directory holds only the files this test created, and
        // a cross-test file assumption is exactly what the isolation
        // forbids — prevImage in a one-file directory legitimately no-ops.
        // The arrows' navigation is pinned in
        // fitOriginalPersistsAndArrowsStayInInspectMode.)
        core.mainWindow()->crop()->toggle();
        QVERIFY(!core.mainWindow()->crop()->isActive());
        viewer->fitWindow();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_WINDOW, 5000);
    }

    // Enter is the crop's own key: with no selection it is consumed — it
    // must not resolve as folder view (its global binding), with or without
    // the pending-work lock. Owned keys do not pass through.
    void cropOwnsEnterEvenWithoutSelection() {
        QImage img(64, 32, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(QDir(base).filePath("enter.png"), "PNG"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("enter.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("enter.png"), 10000);
        core.mainWindow()->crop()->toggle();
        QVERIFY(core.mainWindow()->crop()->isActive());
        auto *overlay = core.mainWindow()->findChild<CropOverlay *>();
        QVERIFY(overlay);

        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(overlay, &enter);

        QVERIFY(core.mainWindow()->crop()->isActive());                                          // the surface stayed
        QCOMPARE(core.viewState().viewMode.value(), MODE_DOCUMENT);                              // no folder switch
        QCOMPARE(core.currentFilePath(), QDir(base).filePath("enter.png"));                      // no navigation
        QVERIFY(!core.directoryModel()->getImage(QDir(base).filePath("enter.png"))->isEdited()); // nothing applied
    }

    void cropPanelSurvivesAnEarlyTrigger() {
        writeImage(QDir(base).filePath("crop.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("crop.png")));
        core.mainWindow()->crop()->toggle();
        QVERIFY(core.mainWindow()->crop()->isActive());
        core.mainWindow()->crop()->toggle();
        QVERIFY(!core.mainWindow()->crop()->isActive());
    }

    void cropPanelConnectionsDeliver() {
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
        QVERIFY(cancelBtn && cropBtn && cropSaveBtn && posX && width && height);

        QSignalSpy cancel(&panel, &CropPanel::cancel);
        QSignalSpy crop(&panel, &CropPanel::crop);
        QSignalSpy cropSave(&panel, &CropPanel::cropAndSave);
        QSignalSpy selection(&panel, &CropPanel::selectionChanged);
        QSignalSpy overlaySelection(&overlay, &CropOverlay::selectionChanged);

        // CropPanel selectAll -> CropOverlay::selectAll, and the overlay's
        // selection travels back into the geometry boxes (both directions).
        // This is what gives the rest of the test a real selection to crop.
        emit panel.selectAll();
        QCOMPARE(overlaySelection.count(), 1);
        QCOMPARE(width->value(), 100);
        QCOMPARE(height->value(), 100);

        // ui->cancelButton clicked -> CropPanel::cancel, nothing else
        cancelBtn->click();
        QCOMPARE(cancel.count(), 1);
        QCOMPARE(crop.count(), 0);
        QCOMPARE(cropSave.count(), 0);

        // ui->width valueChanged -> onSelectionChange -> selectionChanged
        selection.clear();
        width->setValue(50);
        QCOMPARE(selection.count(), 1);

        // ui->cropButton clicked -> doCrop -> crop(rect); cancel stays put.
        // doCrop builds its target from the live boxes, so that is what the
        // expectation is read from; the precondition is asserted separately
        // because the overlay writes its own selection back into the boxes.
        const QRect target(posX->value(), panel.findChild<QSpinBox *>("posY")->value(), width->value(),
                           height->value());
        QVERIFY2(target.width() > 0 && target.height() > 0 && target.size() != QSize(100, 100),
                 qPrintable(QStringLiteral("precondition broken: selection is ") +
                            QStringLiteral("%1x%2, doCrop would emit cancel instead of crop")
                                .arg(target.width())
                                .arg(target.height())));
        cropBtn->click();
        QCOMPARE(crop.count(), 1);
        QCOMPARE(crop[0][0].toRect(), target);
        QCOMPARE(cancel.count(), 1);
        QCOMPARE(cropSave.count(), 0);

        // ui->cropSaveButton clicked -> doCropSave -> cropAndSave(rect)
        cropSaveBtn->click();
        QCOMPARE(cropSave.count(), 1);
        QCOMPARE(cropSave[0][0].toRect(), target);
        QCOMPARE(cancel.count(), 1);

        // ui->posX valueChanged -> selectionChanged
        selection.clear();
        posX->setValue(7);
        QCOMPARE(selection.count(), 1);

        // overlay escPressed -> cancel only: a connection to either crop
        // action would move crop/cropSave and leave cancel at zero
        cancel.clear();
        emit overlay.escPressed();
        QCOMPARE(cancel.count(), 1);
        QCOMPARE(crop.count(), 1);
        QCOMPARE(cropSave.count(), 1);

        // overlay cropApply -> doCrop -> crop(rect), deterministic now: the
        // keyboard's apply is always the in-memory crop, whatever the
        // default-action setting emphasizes for the buttons
        cancel.clear();
        emit overlay.cropApply();
        QCOMPARE(crop.count(), 2);
        QCOMPARE(cropSave.count(), 1);
        QCOMPARE(cancel.count(), 0);
    }

    void cropAndSaveWritesTheCroppedFile() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        coretest::writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.cropAndSave(QRect(0, 0, 8, 8));

        QCOMPARE(QImage(img).size(), QSize(8, 8)); // cropped AND saved to disk
        QCOMPARE(fake.confirmCalls, 0);            // cropAndSave bypasses the confirmation
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
    void gridKeysResolveThroughTheScopeTable() {
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
        QVERIFY(QTest::qWaitForWindowExposed(&grid));
        grid.setFocus();
        QVERIFY(grid.hasFocus());

        coretest::deliverWindowKey(&grid, Qt::Key_Right);
        QCOMPARE(coretest::lastSelection(grid), 1);
        coretest::deliverWindowKey(&grid, Qt::Key_Left);
        QCOMPARE(coretest::lastSelection(grid), 0);
        coretest::deliverWindowKey(&grid, Qt::Key_Left);
        QCOMPARE(coretest::lastSelection(grid), 0); // past the start: stays
        coretest::deliverWindowKey(&grid, Qt::Key_Down);
        coretest::deliverWindowKey(&grid, Qt::Key_Up);
        QCOMPARE(coretest::lastSelection(grid), 0);

        coretest::deliverWindowKey(&grid, Qt::Key_End);
        QCOMPARE(coretest::lastSelection(grid), 24);
        coretest::deliverWindowKey(&grid, Qt::Key_Home);
        QCOMPARE(coretest::lastSelection(grid), 0);
        coretest::deliverWindowKey(&grid, Qt::Key_PageDown);
        const int paged = coretest::lastSelection(grid);
        QVERIFY2(paged > 0, "PgDown never left the first row");
        coretest::deliverWindowKey(&grid, Qt::Key_PageUp);
        QCOMPARE(coretest::lastSelection(grid), 0); // and back

        coretest::deliverWindowKey(&grid, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(grid.selection().count(), 25); // Ctrl+A: everything

        QSignalSpy up(actionManager->action("goUp"), &QAction::triggered);
        coretest::deliverWindowKey(&grid, Qt::Key_Backspace);
        QCOMPARE(up.count(), 1); // goUp is the grid scope's own binding

        QSignalSpy activated(&grid, &FolderGridView::itemActivated);
        coretest::deliverWindowKey(&grid, Qt::Key_Return);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.first().first().toInt(), 0);

        grid.select(5);
        coretest::deliverWindowKey(&grid, Qt::Key_Escape);
        QCOMPARE(coretest::lastSelection(grid), 5); // an unscoped key never moves the grid

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
        QCOMPARE(emptyActivation.count(), 0);

        // the QShortcutMap leg, kept last so the door pins above see the
        // selection state they built: with the grid surface attached, a
        // window-delivered key fires the grid's projected QShortcut before
        // any keyPressEvent exists to forward it — and the de-claimed global
        // table stays out of the way (no ambiguity, no stray nextImage)
        grid.select(0);
        actionManager->attachGridSurface(&grid);
        grid.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&grid));
        grid.setFocus();
        QVERIFY(grid.hasFocus());
        QVERIFY(QApplication::focusWidget() != nullptr);
        QSignalSpy nextImageGlobal(actionManager->action("nextImage"), &QAction::triggered);
        QSignalSpy mapActivated(&grid, &FolderGridView::itemActivated);
        const int before = coretest::lastSelection(grid);
        coretest::deliverWindowKey(&grid, Qt::Key_Right);
        QCOMPARE(coretest::lastSelection(grid), before + 1); // via the grid's QShortcut
        QCOMPARE(nextImageGlobal.count(), 0);                // the de-claimed global binding stood down
        coretest::deliverWindowKey(&grid, Qt::Key_Return);
        QCOMPARE(mapActivated.count(), 1); // Enter claims both Returns
        actionManager->attachGridSurface(nullptr);
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
        TestCrop t;
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    return slot_harness::run_units_one_process_each(
        argc, argv, slot_harness::testSlots(&TestCrop::staticMetaObject),
        "crop_tests",
        [argv](const QString &slot) -> int {
            coretest::setChildCoreLimit();
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestCrop t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        });
}

#include "crop_tests.moc"
