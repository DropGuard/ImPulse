// Core-level navigation test. This is the layer where the folder-boundary
// setting is actually consumed, so it is where "the configured behavior happens"
// can be asserted end to end: key action -> ActionManager -> Core -> settings ->
// DirectoryManager -> engine.
//
// It builds a real MainWindow under the offscreen platform and initializes the
// same globals main.cpp does, because a half-wired app would prove nothing.
// Navigation is triggered by emitting ActionManager::nextImage() — the very
// signal a bound key produces.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QtTest/qtestcase.h>
#include <QQueue>
#include "gui/overlays/imageinfooverlayproxy.h"
#include "gui/cropcontroller.h"
#include "gui/overlayhost.h"
#include "gui/overlays/copyoverlay.h"
#include "gui/overlays/saveconfirmoverlay.h"
#include "gui/overlays/cropoverlay.h"
#include "gui/overlays/floatingmessageproxy.h"
#include <utility>
#include <QApplication>

#include <cerrno>
#include <cstdio> // std::rename: the external rename below is what a file
                  // manager does, and QFile::rename is RENAME_NOREPLACE
#include <sys/wait.h>
#include <unistd.h>
#include <QSignalSpy>
#include <QThread>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <atomic>
#include <chrono>
#include <QTemporaryDir>
#include <QImage>
#include <QDir>
#include <QFile>

#include "core.h"
#include "settings.h"
#include "utils/fileoperations.h"
#include "sharedresources.h"
#include "components/actionmanager/actionmanager.h"
#include "components/directorymanager/enginebridge.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/actions.h"
#include "gui/dialogs/settingsdialog.h"
#include "gui/contextmenu.h"
#include "themestore.h"
#include "core_test_helpers.h"
#include "testhelpers.h"
#include "gui/customwidgets/floatingwidget.h"
#include "gui/customwidgets/floatingwidgetcontainer.h"
#include "gui/dialogs/resizedialog.h"
#include "gui/overlays/videocontrols.h"
#include "gui/overlays/cropoverlay.h"
#include "gui/panels/croppanel/croppanel.h"
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include "gui/overlays/clickzoneoverlay.h"
#include "gui/overlays/zoomindicatoroverlay.h"
#include "gui/panels/mainpanel/mainpanel.h"
#include "gui/viewers/imageviewerv2.h"
#include "gui/folderview/folderview.h"
#include "gui/folderview/fvoptionspopup.h"
#include "gui/folderview/foldergridview.h"
#include "gui/folderview/filesystemmodelcustom.h"
#include "gui/customwidgets/styledcombobox.h"
#include "gui/customwidgets/settingsidebar.h"
#include "gui/customwidgets/entryinfoitem.h"
#include "gui/customwidgets/iconwidget.h"
#include <QCheckBox>
#include <QSettings>

// FloatingWidget::setContainerSize is protected; the pin reaches it the way
// the widget hierarchy does — through a subclass.
class ProbeWidget : public FloatingWidget {
  public:
    explicit ProbeWidget(FloatingWidgetContainer *parent) : FloatingWidget(parent) {
    }
    void pokeContainerSize(QSize size) {
        setContainerSize(size);
    }
};

class TestableContextMenu : public ContextMenu {
  public:
    using ContextMenu::keyPressEvent;
    using ContextMenu::showAt;
    void pressKey(Qt::Key k) {
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
        keyPressEvent(&press);
    }
};

class TestableFolderGridView : public FolderGridView {
  public:
    using FolderGridView::dragEnterEvent;
    using FolderGridView::dragLeaveEvent;
    using FolderGridView::dragMoveEvent;
    using FolderGridView::dropEvent;
    using FolderGridView::keyPressEvent;
};

// The presenter's remaining two view connections (thumbnailsRequested and
// draggedOver) end in IDirectoryView *methods* rather than signals, so the
// observable is the call arriving at the view. This proxy counts them and still
// forwards, so the real strip path runs underneath.
class CountingStripProxy : public ThumbnailStripProxy {
  public:
    using ThumbnailStripProxy::ThumbnailStripProxy;
    void setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) override {
        ++thumbnailCalls;
        lastThumbnailPos = pos;
        ThumbnailStripProxy::setThumbnail(pos, thumb);
    }
    void setDragHover(int index) override {
        ++hoverCalls;
        ThumbnailStripProxy::setDragHover(index);
    }
    void removeItem(int index) override {
        removed << index;
        ThumbnailStripProxy::removeItem(index);
    }
    void insertItem(int index) override {
        inserted << index;
        ThumbnailStripProxy::insertItem(index);
    }
    void reloadItem(int index) override {
        reloaded << index;
        ThumbnailStripProxy::reloadItem(index);
    }
    void renameItem(int oldIndex, int newIndex, const QString &name) override {
        renamed << oldIndex << newIndex;
        renamedTo << name;
        ThumbnailStripProxy::renameItem(oldIndex, newIndex, name);
    }
    int thumbnailCalls = 0;
    int hoverCalls = 0;
    int lastThumbnailPos = -1;
    QList<int> removed, inserted, reloaded, renamed;
    QList<QString> renamedTo;
};

// The widgets in the grid's own visual order, which is the order an
// IDirectoryView index means. Shared by the drop probe below and by the
// row-identity check in renameMovesTheRowAndCarriesItsWidget.
QList<ThumbnailWidget *> orderedThumbnails(FolderGridView &grid) {
    QList<ThumbnailWidget *> widgets;
    const QList<QGraphicsItem *> sceneItems = grid.items(); // named: no detach
    for(QGraphicsItem *item : sceneItems)
        if(auto *tw = dynamic_cast<ThumbnailWidget *>(item))
            widgets << tw;
    std::sort(widgets.begin(), widgets.end(), [](const ThumbnailWidget *a, const ThumbnailWidget *b) {
        const QPointF pa = a->scenePos(), pb = b->scenePos();
        return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
    });
    return widgets;
}

QPoint dropPosOverThumbnail(FolderGridView &grid, int index) {
    const QList<ThumbnailWidget *> widgets = orderedThumbnails(grid);
    if(widgets.count() <= index) {
        QTest::qFail(qPrintable(QStringLiteral("no thumbnail #%1").arg(index)), __FILE__, __LINE__);
        return {};
    }
    const QPointF center = widgets.at(index)->mapToScene(widgets.at(index)->boundingRect().center());
    return grid.mapFromScene(center);
}

// The overlay-proxy lifetime helper. A file-scope template rather than a class
// member: moc refuses a template inside a slots section ("Template function as
// signal or slot"), and this is a mechanism, not a test case.
template <typename Proxy> void proxyOutlivesItsContainer() {
    auto *container = new FloatingWidgetContainer();
    auto proxy = std::make_unique<Proxy>(container);
    proxy->init();    // materializes the overlay as the container's child
    delete container; // the tree frees the overlay right here
    proxy.reset();    // the proxy destructor must not touch it now
}

// The settings bus's pool wait, measured rather than asserted as a duration:
// the job releases itself at a deadline, so the narrow (owner-scoped) wait
// returns long before it and the old global wait could only return after it.
// A bound test, not a hang test.
class DeadlineJob : public PoolJob {
  public:
    DeadlineJob(std::atomic_bool *_gate, std::atomic_int *_holds) : gate(_gate), holds(_holds) {
    }
    void run() override {
        holds->store(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while(!gate->load() && std::chrono::steady_clock::now() < deadline)
            QThread::msleep(1);
        holds->store(0);
    }

  private:
    std::atomic_bool *gate;
    std::atomic_int *holds;
};

class TestCoreNav : public QObject {
    Q_OBJECT
    IsolatedHome home;
    coretest::AppSingletons singletons; // the app's process-wide state, by construction
    QString base, alpha, beta;

    static bool sawDir(const QSignalSpy &spy, const QString &dir, int from = 0) {
        for(int i = from; i < spy.count(); ++i)
            if(spy.at(i).at(0).toString() == dir)
                return true;
        return false;
    }

    // 8x8 red PNG via the shared factory
    void writeImage(const QString &path) {
        QVERIFY(makeImageFile(path, QSize(8, 8)));
    }

  private slots:
    void initTestCase() {
        // IsolatedHome redirected HOME and the config identity; the singletons
        // arrive with the AppSingletons member, so there is no list to keep in
        // sync here.
        QVERIFY(home.path() != nullptr);

        base = QDir(home.path()).filePath("nav");
        alpha = QDir(base).filePath("alpha");
        beta = QDir(base).filePath("beta");
        QVERIFY(QDir().mkpath(alpha));
        QVERIFY(QDir().mkpath(beta));
        writeImage(QDir(alpha).filePath("a.png"));
        writeImage(QDir(beta).filePath("b.png"));
    }
    void titleFollowsTheProjection() {
        writeImage(QDir(base).filePath("t.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("t.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("t.png"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.mainWindow()->windowTitle().contains(QStringLiteral("t.png")), 10000);

        // the slideshow toggle rides the same wiring
        core.toggleSlideshow();
        QTRY_VERIFY_WITH_TIMEOUT(core.mainWindow()->windowTitle().contains(QStringLiteral("[slideshow]")), 10000);
        core.toggleSlideshow();
        QTRY_VERIFY_WITH_TIMEOUT(!core.mainWindow()->windowTitle().contains(QStringLiteral("[slideshow]")), 10000);
    }

    // the folder view learns its directory through the state projection, the
    // same law the title rides — a state write is the only thing that moves
    // it. The write happens after the model has accepted the directory: a
    // refused load must not move the view. (This replaced a direct
    // Core->MW::setDirectoryPath push that never wrote state on next/prev
    // directory navigation at all.)
    void folderViewFollowsTheDirectoryState() {
        QDir().mkpath(QDir(base).filePath("projdir"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("projdir")));
        QTRY_COMPARE_WITH_TIMEOUT(core.mainWindow()->getFolderView()->directoryPath(), QDir(base).filePath("projdir"),
                                  10000);
    }

    // the crop action wiring is live from initActions, but the crop panel is
    // built in setupFullUi — which Core schedules 50ms after showGui. showCropPanel
    // used to dereference the not-yet-created cropPanel/sidePanel pair in that
    // window (a guaranteed SIGSEGV on an early trigger).
    // mode = intent. Fit original declares "I am inspecting this image": the
    // mode persists across image switches, and while it holds, the arrow keys
    // pan (the keymap's inspect layer overrides the base bindings) — the
    // software does not get to decide the inspection is over. Leaving the
    // mode (1/2/4, Space) restores the browse arrows: left/right change
    // images again.
    void fitOriginalPersistsAndArrowsStayInInspectMode() {
        writeImage(QDir(base).filePath("insA.png"));
        writeImage(QDir(base).filePath("insB.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("insA.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("insA.png"), 10000);
        core.mainWindow()->fitOriginal();
        auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);

        core.nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("insB.png"), 10000);
        QCOMPARE(viewer->fitMode(), FIT_ORIGINAL);

        // armed: left pans, it never flips images
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QCOMPARE(core.currentFilePath(), QDir(base).filePath("insB.png"));

        // back in a browse mode the same key navigates again
        core.mainWindow()->fitWindow();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_WINDOW, 5000);
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("insA.png"), 10000);
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

        core.mainWindow()->crop()->trigger();
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());
        QCOMPARE(viewer->fitMode(), FIT_ORIGINAL);
        QVERIFY2(qAbs(viewer->currentScale() - scaleBefore) < 0.001f, "the zoom must survive crop entry");
    }

    // surfaced floating panels own Esc: it dismisses the surface and never
    // reaches the action system, where "Esc" maps to closeFullScreenOrExit
    // (app exit in windowed mode). Each surface's own key door handles it.
    void overlayEscDismissesTheSurface() {
        writeImage(QDir(base).filePath("esc.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("esc.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("esc.png"), 10000);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, 0, 1);

        // copy overlay (serves copy and move alike)
        core.mainWindow()->overlays()->triggerCopyOverlay();
        auto *copyOverlay = core.mainWindow()->findChild<CopyOverlay *>();
        QVERIFY(copyOverlay && copyOverlay->isVisible());

        // a bare Enter is no answer in a digits-answer menu: it must not
        // toggle the folder view behind the overlay
        QKeyEvent enterKey(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                           coretest::scanCodeFor(QStringLiteral("Enter")), 0u, 0u);
        QApplication::sendEvent(copyOverlay, &enterKey);
        QCOMPARE(core.mainWindow()->currentViewMode(), MODE_DOCUMENT);
        QVERIFY(copyOverlay->isVisible());

        QApplication::sendEvent(copyOverlay, &esc);
        QVERIFY(!copyOverlay->isVisible());

        // save-confirm prompt (surfaced by edits — flips H/V land here too):
        // Esc means "discard edits", the same chain as the discard button
        settings->setShowSaveOverlay(true);
        core.mainWindow()->overlays()->showSaveOverlay();
        auto *saveOverlay = core.mainWindow()->findChild<SaveConfirmOverlay *>();
        QVERIFY(saveOverlay && saveOverlay->isVisible());
        QApplication::sendEvent(saveOverlay, &esc);
        QVERIFY(!saveOverlay->isVisible());

        // crop surface: Esc = cancel the selection and leave crop
        core.mainWindow()->crop()->trigger();
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());
        auto *cropOverlay = core.mainWindow()->findChild<CropOverlay *>();
        QVERIFY(cropOverlay);
        QApplication::sendEvent(cropOverlay, &esc);
        QTRY_COMPARE_WITH_TIMEOUT(!core.mainWindow()->crop()->isCropPanelActive(), true, 5000);
    }

    // The save prompt keeps the disk off the bare keys: Save and Save as are
    // the platform chords (which live in the global table and work through
    // this surface's fall-through), Esc discards — and a bare Enter is no
    // answer at all, because Enter once fell through to the folder-view
    // toggle behind the prompt and must now neither overwrite in place nor
    // abandon the edits. The keys are driven as physical events — with a
    // real scan code, because a synthesized key with scan code 0 has no name
    // and would fall through.
    void saveConfirmOverlayAnswersItsKeys() {
        writeImage(QDir(base).filePath("svkeys.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("svkeys.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        settings->setShowSaveOverlay(true);
        core.mainWindow()->overlays()->showSaveOverlay();
        auto *saveOverlay = core.mainWindow()->findChild<SaveConfirmOverlay *>();
        QVERIFY(saveOverlay && saveOverlay->isVisible());

        // the hints are the platform chords and the way out
        QVERIFY2(saveOverlay->findChild<QPushButton *>(QStringLiteral("saveButton"))
                     ->text()
                     .contains(QLatin1String("[Ctrl+S]")),
                 "the save button carries no key hint");
        QVERIFY2(saveOverlay->findChild<QPushButton *>(QStringLiteral("saveAsButton"))
                     ->text()
                     .contains(QLatin1String("[Ctrl+Shift+S]")),
                 "the save-as button carries no key hint");
        QVERIFY2(saveOverlay->findChild<QPushButton *>(QStringLiteral("discardButton"))
                     ->text()
                     .contains(QLatin1String("[Esc]")),
                 "the discard button carries no key hint");

        QSignalSpy discard(saveOverlay, &SaveConfirmOverlay::discardClicked);

        // the write chords are the global table's actions — the save-as one
        // ends at Core::requestSavePath, whose file dialog would block this
        // headless run forever, so its Core leg is disconnected and the
        // assertion is on the action emission itself
        QObject::disconnect(actionManager, &ActionManager::saveAs, nullptr, nullptr);
        QSignalSpy saveAction(actionManager, &ActionManager::save);
        QSignalSpy saveAsAction(actionManager, &ActionManager::saveAs);
        QSignalSpy folderViewAction(actionManager, &ActionManager::folderView);

        auto press = [&](Qt::Key key, const char *keyName, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QKeyEvent ev(QEvent::KeyPress, key, mods, coretest::scanCodeFor(QLatin1String(keyName)), 0u, 0u);
            QApplication::sendEvent(saveOverlay, &ev);
        };

        press(Qt::Key_S, "S", Qt::ControlModifier);
        QCOMPARE(saveAction.count(), 1);    // Ctrl+S: the deliberate write...
        QVERIFY(!saveOverlay->isVisible()); // ...which also dismisses the prompt

        press(Qt::Key_S, "S", Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(saveAsAction.count(), 1); // Ctrl+Shift+S: save as

        core.mainWindow()->overlays()->showSaveOverlay(); // back up for the way out
        QVERIFY(saveOverlay->isVisible());

        press(Qt::Key_Return, "Enter");
        QCOMPARE(saveAction.count(), 1);
        QCOMPARE(saveAsAction.count(), 1);
        QCOMPARE(folderViewAction.count(), 0); // a bare Enter is swallowed, it toggles nothing
        QVERIFY(saveOverlay->isVisible());

        press(Qt::Key_Escape, "Esc");
        QCOMPARE(discard.count(), 1);
        QVERIFY(!saveOverlay->isVisible()); // discard also dismisses the surface
    }
    void cropSurfaceHintsItsKeys() {
        writeImage(QDir(base).filePath("cropkeys.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("cropkeys.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.mainWindow()->crop()->trigger();
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());
        auto *panel = core.mainWindow()->findChild<CropPanel *>();
        QVERIFY(panel);

        QVERIFY2(
            panel->findChild<QPushButton *>(QStringLiteral("cropButton"))->text().contains(QLatin1String("[Enter]")),
            "the crop button carries no key hint");
        QVERIFY2(!panel->findChild<QPushButton *>(QStringLiteral("cropSaveButton"))
                      ->text()
                      .contains(QLatin1String("[Shift+Enter]")),
                 "the disk-writing button still advertises a key");
        QVERIFY2(
            panel->findChild<QPushButton *>(QStringLiteral("cancelButton"))->text().contains(QLatin1String("[Esc]")),
            "the cancel button carries no key hint");

        auto *combo = panel->findChild<QComboBox *>(QStringLiteral("ARcomboBox"));
        QVERIFY(combo);
        QCOMPARE(combo->currentIndex(), 0); // Free: reset on image change
        QCOMPARE(combo->count(), 7);        // Free | Custom | Current Image | This Screen | 1:1 | 4:3 | 16:9

        // the hand-typed ratio's fields belong to Custom alone: hidden under
        // every other preset, visible only while it is the chosen one — and
        // they keep their own values instead of being seeded into confusion
        auto *arInput = panel->findChild<QWidget *>(QStringLiteral("ARInputWidget"));
        QVERIFY(arInput);
        QVERIFY2(!arInput->isVisible(), "the custom-ratio fields leak into the Free preset");
        combo->setCurrentIndex(1); // Custom
        QVERIFY2(arInput->isVisible(), "choosing Custom did not reveal its fields");
        combo->setCurrentIndex(4); // 1:1
        QVERIFY2(!arInput->isVisible(), "the custom-ratio fields leak into a numeric preset");
        combo->setCurrentIndex(0); // back to Free
        QCOMPARE(combo->currentIndex(), 0);

        auto *overlay = core.mainWindow()->findChild<CropOverlay *>();
        QVERIFY(overlay);

        // the keyboard's Apply is the in-memory crop and nothing else: the
        // Enter lands on the tool signal, and no bare key reaches the
        // disk-writing signal. (A REAL subset first: a whole-image selection
        // is refused by design — cropping everything changes no pixels — so
        // the boxes drive one row/column off.)
        auto *widthBox = panel->findChild<QSpinBox *>(QStringLiteral("width"));
        auto *heightBox = panel->findChild<QSpinBox *>(QStringLiteral("height"));
        QVERIFY(widthBox && heightBox);
        QVERIFY2(widthBox->maximum() > 1 && heightBox->maximum() > 1,
                 "precondition broken: the fixture image is too small to crop");
        const int originalWidth = widthBox->maximum();
        const int originalHeight = heightBox->maximum();
        widthBox->setValue(originalWidth - 1);
        heightBox->setValue(originalHeight - 1);

        QSignalSpy applied(panel, &CropPanel::crop);
        QSignalSpy savedToDisk(panel, &CropPanel::cropAndSave);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                        coretest::scanCodeFor(QStringLiteral("Enter")), 0u, 0u);
        QApplication::sendEvent(overlay, &enter);
        QCOMPARE(applied.count(), 1);     // the subset existed, and the answer fired
        QCOMPARE(savedToDisk.count(), 0); // the bare key never writes

        // Apply does not dismiss: the surface stays open and, once the
        // applied crop settles into the viewer, the selection becomes the
        // whole new image — re-cropping is dragging again, no key needed
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());
        QTRY_COMPARE_WITH_TIMEOUT(widthBox->value(), originalWidth - 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(heightBox->value(), originalHeight - 1, 10000);
        QCOMPARE(widthBox->maximum(), originalWidth - 1); // the readout tracks the new ground truth
        QCOMPARE(heightBox->maximum(), originalHeight - 1);

        // the save chord, pressed while a selection stands, commits the crop
        // and writes — end to end, disk included, from the surface that
        // never left
        widthBox->setValue(widthBox->maximum() - 1);
        heightBox->setValue(heightBox->maximum() - 1);

        QSignalSpy applied2(panel, &CropPanel::crop);
        QKeyEvent ctrlS(QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier, coretest::scanCodeFor(QStringLiteral("S")),
                        0u, 0u);
        QApplication::sendEvent(overlay, &ctrlS);
        QCOMPARE(applied2.count(), 1); // the chord commits the pending crop...
        QTRY_COMPARE_WITH_TIMEOUT(QImage(QDir(base).filePath("cropkeys.png")).size(),
                                  QSize(originalWidth - 2, originalHeight - 2), 10000); // ...and writes it
        QVERIFY2(
            core.mainWindow()->crop()->isCropPanelActive(),
            "the write went through the global save action, which does not own the surface — Esc is the only exit");
    }

    // free zoom works mechanically on images that fit the viewport (small
    // images included): + steps 1.0 -> 1.2 -> 1.44. Switching to a nominal
    // fit mode collapses the free zoom back to that mode's scale — the
    // FIT_FREE-is-ephemeral contract the mode-is-intent law rests on.
    void zoomInWorksAndFitSwitchCollapsesFreeZoom() {
        QImage img(400, 300, QImage::Format_RGB32);
        img.fill(Qt::magenta);
        QVERIFY(img.save(QDir(base).filePath("small.png"), "PNG"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("small.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("small.png"), 10000);
        auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        core.mainWindow()->fitOriginal();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        QVERIFY(qFuzzyCompare(viewer->currentScale(), 1.0f));

        viewer->zoomIn();
        QVERIFY(qFuzzyCompare(viewer->currentScale(), 1.2f));
        viewer->zoomIn();
        QVERIFY(qFuzzyCompare(viewer->currentScale(), 1.44f));

        core.mainWindow()->switchFitMode();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_WINDOW, 5000);
        QVERIFY(qFuzzyCompare(viewer->currentScale(), 1.0f));
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
        core.mainWindow()->crop()->trigger();
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());
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
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());

        // fit remains blocked while framing — a fit would reset the very
        // view the user is framing in
        float scale = viewer->currentScale();
        viewer->fitWindow();
        QVERIFY(qAbs(viewer->currentScale() - scale) < 0.001f);
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());

        // leaving crop releases the policy: the fit keys work again — the
        // mode semantics were never vetoed, only deferred while the overlay
        // held the canvas. (No navigation assert here: fork-per-slot means
        // this fork's directory holds only the files this test created, and
        // a cross-test file assumption is exactly what the isolation
        // forbids — prevImage in a one-file directory legitimately no-ops.
        // The arrows' navigation is pinned in
        // fitOriginalPersistsAndArrowsStayInInspectMode.)
        core.mainWindow()->crop()->trigger();
        QVERIFY(!core.mainWindow()->crop()->isCropPanelActive());
        viewer->fitWindow();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_WINDOW, 5000);
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
        core.mainWindow()->crop()->trigger();
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());
        coretest::drainEvents();
        QVERIFY(qFuzzyCompare(viewer->currentScale(), scaleBefore));
        QCOMPARE(static_cast<int>(viewer->fitMode()), static_cast<int>(FIT_FREE));
        QCOMPARE(viewer->imageRect(), rectBefore);
    }

    void cropPanelSurvivesAnEarlyTrigger() {
        writeImage(QDir(base).filePath("crop.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("crop.png")));
        core.mainWindow()->crop()->trigger();
        QVERIFY(core.mainWindow()->crop()->isCropPanelActive());
        core.mainWindow()->crop()->trigger();
        QVERIFY(!core.mainWindow()->crop()->isCropPanelActive());
    }

    void preloaderFollowsTheDirectionOfTravel() {
        settings->setUsePreloader(true);
        QVERIFY(settings->usePreloader());

        // four files so both directions have an uncached neighbor to prove
        // themselves with: forward from c reaches d, backward from d reaches b
        QString seq = QDir(base).filePath("seq");
        QVERIFY(QDir().mkpath(seq));
        QStringList files;
        for(const char *n : {"a", "b", "c", "d"}) {
            const QString path = QDir(seq).filePath(QString(n) + ".png");
            writeImage(path);
            files << path;
        }

        // The bridge this test watches must be the one Core drives. Before
        // Deps these were the same object only because both reached for the
        // process-wide singleton; now the link is explicit, and a Core built
        // without the bridge would make every wait below time out.
        EngineBridge bridge;
        QSignalSpy listed(&bridge, &EngineBridge::dirListed);
        QVERIFY(listed.isValid());

        Core core({.bridge = &bridge});
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, seq), 10000);
        // the cold-open load rides whenListed: it fires against a landed
        // listing, so the first preload has a real neighbor to decode

        // one flip forward: from b the forward neighbor c decodes
        emit actionManager->nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(1), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(files.at(2)), 10000);

        // another flip forward: from c the forward neighbor is d
        emit actionManager->nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(2), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(files.at(3)), 10000);

        // jump to the last file: still a forward move, so the backward
        // neighbor b must not decode — and the cache trim drops it
        QVERIFY(core.loadPath(files.at(3)));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(3), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.directoryModel()->isLoaded(files.at(1)));

        // one backward move: the preload must follow the travel direction and
        // decode b, which a forward-facing policy would never have touched
        emit actionManager->prevImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(2), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(files.at(1)), 10000);
    }

    // 14a61f7a armed the cold open to the listing landing but left main's
    // unconditional showGui in place: the window appeared empty first and the
    // image arrived after. The reveal belongs to the first ready frame.
    void coldOpenKeepsTheWindowHiddenUntilTheFirstFrame() {
        QString dir = QDir(base).filePath("waitfirst");
        QVERIFY(QDir().mkpath(dir));
        QStringList files;
        for(const char *n : {"one", "two"}) {
            const QString path = QDir(dir).filePath(QString(n) + ".png");
            writeImage(path);
            files << path;
        }

        Core core;
        QVERIFY(!core.mainWindow()->isVisible());
        QVERIFY(core.loadPath(files.at(0)));
        QVERIFY(core.firstFramePending());
        // main's tail, faithfully: show only when nothing is pending
        if(!core.firstFramePending())
            core.showGui();
        QVERIFY(!core.mainWindow()->isVisible());

        QTRY_VERIFY_WITH_TIMEOUT(!core.firstFramePending(), 10000);
        QVERIFY(core.mainWindow()->isVisible());
        QCOMPARE(core.currentFilePath(), files.at(0));
    }

    void gotoAdjacentAtFolderEndEntersNextFolder() {
        settings->setFolderEndAction(FOLDER_END_GOTO_ADJACENT);
        QVERIFY(settings->folderEndAction() == FOLDER_END_GOTO_ADJACENT);

        QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
        QVERIFY(listed.isValid());

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        // opening a file defers the folder listing until the image is ready
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, alpha), 10000);
        QCOMPARE(core.currentFilePath(), QDir(alpha).filePath("a.png"));

        const int before = listed.count();
        emit actionManager->nextImage();
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, beta, before), 10000);
        // it must land on the new folder's image, not on whatever the previous
        // folder's mirror still had at index 0
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(beta).filePath("b.png"), 10000);
    }

    void noActionAtFolderEndStaysInThisFolder() {
        settings->setFolderEndAction(FOLDER_END_NO_ACTION);
        QVERIFY(settings->folderEndAction() == FOLDER_END_NO_ACTION);

        QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, alpha), 10000);

        const int before = listed.count();
        emit actionManager->nextImage();
        // settled = nothing outstanding *and* no deferred hop armed; the old
        // 500ms sleep could only hope the 40ms one was slow enough to be seen
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(listed.count(), before);
        QVERIFY(!sawDir(listed, beta, before));
        QCOMPARE(core.currentFilePath(), QDir(alpha).filePath("a.png"));
    }

    void setWallpaperOnAnUnsupportedSessionIsASafeNoOp() {
        FreshSettings fresh;
        const QByteArray oldSession = qgetenv("DESKTOP_SESSION");
        qputenv("DESKTOP_SESSION", "twm");

        QString wpBase = QDir(home.path()).filePath("wp");
        QVERIFY(QDir().mkpath(wpBase));
        QString img = QDir(wpBase).filePath("w.png");
        writeImage(img);

        Core core;
        QVERIFY(core.loadPath(img));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), img, 10000);

        // the full chain runs; with no session match nothing executes and the
        // wallpaper is untouched — the assertion is the safe return
        QMetaObject::invokeMethod(&core, "setWallpaper");
        qputenv("DESKTOP_SESSION", oldSession);
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

    // gcov reported execution_count == 0 for this whole cluster: no test had
    // ever entered startSlideshow / stopSlideshow / nextImageSlideshow /
    // startSlideshowTimer / jumpToFirst / jumpToLast / toggleShuffle /
    // syncRandomizer. They are driven through the private slots directly
    // (TestCoreNav is a friend of Core) and asserted on the three things they
    // actually produce: the file on screen, the timer, and the loop-playback
    // request Core sends the main window. The listing is a dedicated four-file
    // directory rather than alpha, whose contents other tests in this class
    // add to — a slideshow test that counted files in alpha would be counting
    // whatever the earlier tests left behind.
    QStringList slideshowFiles(const QString &tag) {
        const QString dir = QDir(base).filePath(tag);
        // not QVERIFY: it expands to `return;` and this helper has a value to
        // hand back — the same reason dropPosOverThumbnail uses qFail
        if(!QDir().mkpath(dir))
            QTest::qFail("cannot create the slideshow fixture directory", __FILE__, __LINE__);
        QStringList files;
        for(int i = 1; i <= 4; ++i) {
            const QString f = dir + QStringLiteral("/s%1.png").arg(i);
            QImage img(8, 8, QImage::Format_RGB32);
            img.fill(Qt::red);
            if(!img.save(f, "PNG"))
                QTest::qFail(qPrintable(QStringLiteral("cannot write ") + f), __FILE__, __LINE__);
            files << f;
        }
        return files;
    }

    void slideshowLifecycleRequestsLoopPlaybackOffThenOn() {
        const QStringList files = slideshowFiles("show1");
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QSignalSpy loop(core.mainWindow(), &MW::setLoopPlayback);
        QVERIFY(loop.isValid());

        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);
        QCOMPARE(loop.count(), 1);
        QCOMPARE(loop.at(0).at(0).toBool(), false);

        // a second start with the flag already set is the no-op it must be:
        // re-arming would emit a second request and re-enter enableDocumentView
        core.startSlideshow();
        QCOMPARE(loop.count(), 1);

        core.stopSlideshow();
        QVERIFY(!core.viewState().slideshow);
        QCOMPARE(loop.count(), 2);
        QCOMPARE(loop.at(1).at(0).toBool(), true);

        core.stopSlideshow();
        QCOMPARE(loop.count(), 2);
    }

    void slideshowAdvanceIsSequentialAndArmsTheTimer() {
        const QStringList files = slideshowFiles("show2");
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.startSlideshow();
        // startSlideshow arms the timer itself, through startSlideshowTimer,
        // for a static image — measured, this assertion's first draft claimed
        // the opposite and failed
        QVERIFY(core.slideshowClock().isActive());

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), files.at(1));
        QVERIFY2(core.slideshowClock().isActive(), "a static image under slideshow must keep the timer armed");

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), files.at(2));
    }

    void nonLoopingSlideshowStopsAtTheEndOfTheListing() {
        const QStringList files = slideshowFiles("show3");
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.slideshowLoops());

        core.jumpToLast();
        QCOMPARE(core.currentFilePath(), files.at(3));
        // jumpToLast stops the show, so the state below is a fresh arm
        QVERIFY(!core.viewState().slideshow);

        core.jumpToFirst();
        QCOMPARE(core.currentFilePath(), files.at(0));

        core.startSlideshow();
        core.jumpToLast();
        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);
        const QString atEnd = core.currentFilePath();

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), atEnd);
        QVERIFY2(!core.viewState().slideshow, "a non-looping slideshow must stop itself at the end of the directory");
        QVERIFY(!core.slideshowClock().isActive());
    }

    void shuffleKeepsTheSlideshowCyclingPastTheEnd() {
        const QStringList files = slideshowFiles("show4");
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.viewState().shuffle);

        core.toggleShuffle();
        QVERIFY(core.viewState().shuffle); // this is what syncRandomizer sized for

        core.startSlideshow();
        core.jumpToLast();
        core.startSlideshow();
        QVERIFY(core.viewState().slideshow);

        // the sequential branch would have stopped here (pinned above with the
        // same setup); the randomizer branch starts a new cycle instead
        core.nextImageSlideshow();
        QVERIFY2(core.viewState().slideshow, "shuffle mode stopped at the end of the listing");
        QVERIFY(files.contains(core.currentFilePath()));
    }

    // Core's file-operation and directory-navigation members: gcov reports
    // execution_count == 0 for moveCurrentFile, copyCurrentFile, onDropIn,
    // prevDirectory, nextDirectory, loadParentDir and toggleSlideshow. Each is
    // asserted on disk state and on currentFilePath, and the collision paths
    // run through the scripted IUserInteractions seam rather than a dialog.
    // The directories are created per test: siblingDirectory() below needs a
    // parent holding exactly two children, and this class's shared alpha/base
    // accumulate files and directories from every earlier test.
    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(base).filePath(tag + QStringLiteral("-%1").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    void dropIntoTheAppOpensTheDroppedFileAndIgnoresItself() {
        const QString dir = uniqueDir("dropin");
        const QString target = dir + QStringLiteral("/dropped.png");
        writeImage(target);

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const QString before = core.currentFilePath();

        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(target)});

        // the self-drop guard: a drag that started in this app must not re-open
        core.onDropIn(&mime, &core);
        QCOMPARE(core.currentFilePath(), before);

        core.onDropIn(&mime, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), target, 10000);
    }

    void adjacentDirectoryNavigationMovesBetweenSiblingFolders() {
        const QString parent = uniqueDir("adjparent");
        const QString dirA = parent + QStringLiteral("/aaa");
        const QString dirB = parent + QStringLiteral("/bbb");
        QDir().mkpath(dirA);
        QDir().mkpath(dirB);
        writeImage(dirA + QStringLiteral("/one.png"));
        writeImage(dirB + QStringLiteral("/two.png"));

        Core core;
        QVERIFY(core.loadPath(dirB + QStringLiteral("/two.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(core.directoryModel()->directoryPath(), dirB);

        core.prevDirectory(false);
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(core.currentFilePath(), dirA + QStringLiteral("/one.png"));
        QCOMPARE(core.directoryModel()->directoryPath(), dirA);

        core.nextDirectory();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(core.currentFilePath(), dirB + QStringLiteral("/two.png"));

        // selectLast is the folder-view contract: arriving from the end of a
        // folder must land on the last file, not the first
        core.prevDirectory(true);
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(core.currentFilePath(), dirA + QStringLiteral("/one.png"));
    }

    void toggleSlideshowStartsThenStops() {
        const QStringList files = slideshowFiles("show5");
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.viewState().slideshow);

        core.toggleSlideshow();
        QVERIFY(core.viewState().slideshow);
        QVERIFY(core.slideshowClock().isActive());

        core.toggleSlideshow();
        QVERIFY2(!core.viewState().slideshow, "the second toggle did not stop the slideshow");
        QVERIFY(!core.slideshowClock().isActive());
    }

    // Core::openFromClipboard is 84 lines that gcov had never entered. The URL
    // branch is the whole of it that needs no dialog (the image branch stops at
    // MW::getSaveFileName), and the offscreen platform does provide a working
    // clipboard. Each of the three ways in is pinned: a local file url opens it,
    // a non-local url is refused with the current file untouched, and a
    // text-only clipboard does nothing at all.
    void clipboardUrlOpensThatFileAndOtherFormsAreRefused() {
        const QString dir = uniqueDir("clip");
        const QString target = dir + QStringLiteral("/clip-target.png");
        writeImage(target);

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const QString before = core.currentFilePath();

        // 1. a local file url is opened
        QMimeData *local = new QMimeData();
        local->setUrls({QUrl::fromLocalFile(target)});
        QApplication::clipboard()->setMimeData(local);
        core.openFromClipboard();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), target, 10000);

        // 2. a remote url: toLocalFile() is empty, so nothing is opened and the
        // screen keeps showing what it showed
        QMimeData *remote = new QMimeData();
        remote->setUrls({QUrl(QStringLiteral("https://example.com/notlocal.png"))});
        QApplication::clipboard()->setMimeData(remote);
        core.openFromClipboard();
        // "nothing happens" is checked after the queue is drained, not after an
        // arbitrary 200 ms: a sleep would pass for the wrong reason on a fast
        // machine and fail on a slow one, and neither says anything about the
        // behavior
        coretest::drainEvents();
        QCOMPARE(core.currentFilePath(), target);

        // 3. text only: no urls, no image, no action
        QMimeData *textOnly = new QMimeData();
        textOnly->setText(QStringLiteral("just a sentence"));
        QApplication::clipboard()->setMimeData(textOnly);
        core.openFromClipboard();
        // "nothing happens" is checked after the queue is drained, not after an
        // arbitrary 200 ms: a sleep would pass for the wrong reason on a fast
        // machine and fail on a slow one, and neither says anything about the
        // behavior
        coretest::drainEvents();
        QCOMPARE(core.currentFilePath(), target);
        QVERIFY(!before.isEmpty());
    }

    // The no-extra-arguments instantiation of Core::edit_template is the one
    // flipH and flipV use, and gcov showed it had never been entered: the two
    // flips were the only image operations with no test at all. What is pinned
    // here is the `save` flag's contract, because that flag decides whether the
    // user's file gets written: from the folder view an edit saves immediately,
    // so it has to ask first; in document mode the edit stays in memory, asks
    // nothing and must leave the file untouched. The fixture is 16x8 with the
    // right half blue, so a horizontal flip is visible in pixels while the
    // size stays the same.
    void flipsFollowTheConditionalSaveContract() {
        const QString dir = uniqueDir("flips");
        const QString img = dir + QStringLiteral("/half.png");
        writeRectImage(img);

        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(img), 10000);
        const QByteArray diskBefore = coretest::contentOf(img);

        core.flipH();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(fake.confirmCalls, 0);
        QCOMPARE(coretest::contentOf(img), diskBefore);

        auto edited = std::dynamic_pointer_cast<ImageStatic>(core.directoryModel()->getImage(img));
        QVERIFY2(edited && edited->isEdited(), "flipH left no edited image in memory");
        const QImage shown = edited->getPixmap()->toImage();
        QCOMPARE(shown.size(), QSize(16, 8));
        QCOMPARE(shown.pixelColor(0, 0), QColor(Qt::blue)); // mirrored: blue moved left
        QCOMPARE(shown.pixelColor(15, 0), QColor(Qt::red));

        core.flipV();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(fake.confirmCalls, 0);
        QCOMPARE(coretest::contentOf(img), diskBefore);

        // the folder view is where an edit hits the disk, so the question has
        // to come first — and a declined question must mean nothing was written
        core.enableFolderView();
        QCOMPARE(core.mainWindow()->currentViewMode(), MODE_FOLDERVIEW);
        fake.confirmAnswer = false;
        core.flipH();
        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(coretest::contentOf(img), diskBefore);
    }

    // Core::onUpdate: the version-migration path, run from the constructor when
    // firstRun is off and appVersion differs from the stored version. The
    // pre-1.0.1 binding rewrites retired with the rename — ImPulse has no
    // released ancestor whose users carry those formats, and the only
    // population that could ever arrive with them was hypothetical. What the
    // path still does is record the version, and the trigger is 'different'
    // rather than 'greater': a qimgv 1.0.3 past compares as a numeric
    // downgrade against 0.1.0, and the migration must fire for it anyway —
    // that inverted comparison is exactly what this pin guards. lastVer is
    // read back through the K_LastVer* rows, so this also pins that those
    // rows serve a written value. ActionManager is a process-wide singleton,
    // so its shortcut map is snapshotted and put back.
    void versionMigrationRecordsTheVersionFromAnInheritedPast() {
        FreshSettings fresh;
        const QMap<QString, QString> snapshot = actionManager->allShortcuts();

        settings->setFirstRun(false);
        QVersionNumber inherited(1, 0, 3);
        settings->setLastVersion(inherited);
        QCOMPARE(settings->lastVersion(), inherited); // the rows serve what was written

        {
            Core core; // the constructor is what decides to migrate
        }

        QCOMPARE(settings->lastVersion(), appVersion);

        // put the singleton back the way the rest of the suite expects it
        actionManager->removeAllShortcuts();
        for(auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
            actionManager->addShortcut(it.key(), it.value());
    }

    // The scope chain's order is the contract: the surface the caller declares
    // wins (a grid holding focus outranks an inspect mode armed before the
    // folder view was opened — the stale-arm case), then the armed inspect
    // layer, then the global table. Each layer is heard through its own
    // signal, so the assertion is on what FIRED, not on what returned. The
    // grid binding is injected through the public API and swept away after —
    // the singleton is process-wide and every later test inherits whatever
    // this leaves.
    void shortcutScopeChainResolvesCallerFirst() {
        actionManager->addGridShortcut(QStringLiteral("Left"), QStringLiteral("nextImage"));

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.mainWindow()->fitOriginal(); // arms the inspect scope through the real observer
        QTRY_COMPARE_WITH_TIMEOUT(core.mainWindow()->viewer()->fitMode(), FIT_ORIGINAL, 5000);

        QSignalSpy next(actionManager, &ActionManager::nextImage);
        QSignalSpy scroll(actionManager, &ActionManager::scrollLeft);

        // caller-declared grid scope wins over the armed inspect layer
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left"), ShortcutScope::FolderGrid));
        QCOMPARE(next.count(), 1);
        QCOMPARE(scroll.count(), 0);

        // the same key, no surface declared: the armed inspect layer answers
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QCOMPARE(scroll.count(), 1);
        QCOMPARE(next.count(), 1);

        // disarmed, the same key falls through to the global table
        core.mainWindow()->fitWindow();
        QTRY_COMPARE_WITH_TIMEOUT(core.mainWindow()->viewer()->fitMode(), FIT_WINDOW, 5000);
        QSignalSpy prev(actionManager, &ActionManager::prevImage);
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QCOMPARE(prev.count(), 1);
        QCOMPARE(scroll.count(), 1);

        actionManager->removeAllGridShortcuts();
    }
    void folderViewEditWithConsentRewritesTheFile() {
        const QString dir = uniqueDir("consent");
        const QString img = dir + QStringLiteral("/half.png");
        writeRectImage(img);
        const QByteArray diskBefore = coretest::contentOf(img);

        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.folderPresenter().selectAndFocus(img);
        QTRY_VERIFY2_WITH_TIMEOUT(!core.currentSelection().isEmpty(),
                                  "precondition broken: the folder view holds no selection", 10000);

        fake.confirmAnswer = true;
        core.flipH();

        QCOMPARE(fake.confirmCalls, 1);
        QTRY_VERIFY2_WITH_TIMEOUT(coretest::contentOf(img) != diskBefore,
                                  "a consented folder-view edit never reached the disk", 10000);
    }

    // copyFileClipboard builds the GNOME/KDE companion formats out of
    // mimeData->text(), while getMimeDataForImage only ever sets urls. Whether
    // text() sees the url list at all is a Qt behaviour, so this test decides it
    // by measurement rather than by reading the comment that claims it. If the
    // companion format ends up carrying no url, a file manager is handed the
    // bare word "copy" and cannot paste the file.
    // The edited-image branch is pinned in the same run because it is the one
    // that writes: the temp file is uniquely named per clipboard entry and
    // owned by the mime data it serves, so it autoremoves when that entry is
    // replaced — unique names without accumulating litter.
    void clipboardFileCopyAdvertisesTheFileInEveryFormat() {
        const QString dir = uniqueDir("clipcopy");
        const QString img = dir + QStringLiteral("/one.png");
        writeImage(img);

        Core core;
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.copyFileClipboard();
        const QMimeData *cb = QApplication::clipboard()->mimeData();
        QVERIFY2(cb->hasUrls(), "a copied file offered no urls");
        QCOMPARE(cb->urls().constFirst(), QUrl::fromLocalFile(img));
        qDebug() << "[clipcopy] text() =" << cb->text()
                 << "gnome-copied-files =" << cb->data("x-special/gnome-copied-files");
        QVERIFY2(cb->data("x-special/gnome-copied-files").contains("file://"),
                 qPrintable(QStringLiteral("the gnome companion format carries no file url, so a "
                                           "file manager cannot paste it; payload was \"") +
                            QString::fromUtf8(cb->data("x-special/gnome-copied-files")) + '\"'));
        QCOMPARE(cb->data("application/x-kde-cutselection"), QByteArray("0"));

        // an edited image is written to a uniquely-named temp file and the url
        // points at what was written — never at the one fixed name two
        // concurrent consumers used to share
        core.flipH();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.copyFileClipboard();
        const QUrl editedUrl = QApplication::clipboard()->mimeData()->urls().constFirst();
        const QString editedPath = editedUrl.toLocalFile();
        QVERIFY2(editedPath.startsWith(settings->tmpDir()),
                 qPrintable(QStringLiteral("the edited copy advertised a file outside the tmp dir: ") + editedPath));
        QVERIFY2(editedPath != settings->tmpDir() + QStringLiteral("image.png"),
                 qPrintable(QStringLiteral("the edited copy was handed the old fixed name: ") + editedPath));
        QVERIFY2(QFileInfo::exists(editedPath),
                 qPrintable(QStringLiteral("the edited copy advertised a temp file that was never "
                                           "written: ") +
                            editedPath));
        QVERIFY2(QFileInfo(editedPath).size() > 0, "the edited temp image was written empty");

        // copying again replaces the clipboard entry, and the entry the file is
        // owned by dies with the replacement: a new unique name, and the old
        // file autoremoved — unique names without accumulating litter
        core.flipV();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.copyFileClipboard();
        const QString secondPath = QApplication::clipboard()->mimeData()->urls().constFirst().toLocalFile();
        QVERIFY2(secondPath != editedPath, "two clipboard entries shared one temp name");
        QVERIFY2(!QFileInfo::exists(editedPath),
                 qPrintable(QStringLiteral("the replaced clipboard entry left its temp file behind: ") + editedPath));
        QVERIFY(QFileInfo::exists(secondPath));
    }
    void disconnectViewAfterTheViewDiedIsSafe() {
        WorkerPool pool(2);
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        {
            auto *proxy = new ThumbnailStripProxy();
            presenter.setView(proxy);
            delete proxy; // the view dies while the presenter lives
        }
        presenter.disconnectView(); // must not reach the freed sender
        // and rebinding to a fresh view still works afterwards
        ThumbnailStripProxy second;
        presenter.setView(&second);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        presenter.setModel(model);
        QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);
        QVERIFY(activated.isValid());
        QMetaObject::invokeMethod(&second, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 1);
    }
    void destroyingTheCoreFreesTheWholeWindowTree() {
        const QMap<QString, int> before = coretest::widgetClasses();
        const int beforeTotal = QApplication::allWidgets().size();
        {
            Core core;
            // the probe only means something if the window tree is really live
            QVERIFY(QApplication::allWidgets().size() > beforeTotal);
        }
        const QMap<QString, int> after = coretest::widgetClasses();
        QStringList surplus;
        for(auto it = after.constBegin(); it != after.constEnd(); ++it) {
            const int left = it.value() - before.value(it.key(), 0);
            if(left > 0)
                surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
        }
        QVERIFY2(surplus.isEmpty(),
                 qPrintable(QStringLiteral("widgets outliving the destroyed core: ") + surplus.join(", ")));
    }

    // ---- the interactive command machines, driven through the scripted
    // IUserInteractions fake (core.ui_). The DialogResult semantics they
    // consume ARE the contract under test: yes is force, all is sticky for
    // the batch (for yes and for no alike), cancel unwinds, plain yes
    // re-asks per file.

    void loadFailureOfUnviewedFileStaysSilent() {
        writeImage(QDir(base).filePath("keep.png"));
        // the load-failure message rides the overlay interface now — the
        // dialog layer asks questions, it does not announce
        coretest::FakeOverlayHost overlayFake;
        Core::Deps deps;
        deps.overlays = &overlayFake;
        Core core(deps);
        QVERIFY(core.loadPath(QDir(base).filePath("keep.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("keep.png"), 10000);
        // the constructor's version/first-run message already landed here —
        // baseline it, the way the old FakeInteractions test did
        int before = overlayFake.messageCalls;

        // a failure for a file the user never opened stays silent
        emit core.directoryModel()->loadFailed(QDir(base).filePath("never-seen.png"));
        QCOMPARE(overlayFake.messageCalls, before);

        // the on-screen file's own failure announces itself and closes the view
        emit core.directoryModel()->loadFailed(QDir(base).filePath("keep.png"));
        QCOMPARE(overlayFake.messageCalls, before + 1);
        QVERIFY(overlayFake.lastMessage.contains(QStringLiteral("Load failed")));
    }

    void draggingAnUndecodedEntryStillOffersTheFile() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // far.png sits beyond the preload neighborhood: a.png preloads its
        // immediate neighbor (mid.png), never the far one — undecoded
        writeImage(QDir(alpha).filePath("mid.png"));
        const QString far = QDir(alpha).filePath("far.png");
        writeImage(far);
        QVERIFY2(!core.directoryModel()->isLoaded(far),
                 "precondition broken: the far image is decoded, the test measures nothing");

        std::unique_ptr<QMimeData> mime(core.mimeDataForPaths({far}));

        QVERIFY2(mime->hasUrls(), "dragging an undecoded entry produced a mime without urls");
        QCOMPARE(mime->urls().constFirst(), QUrl::fromLocalFile(far));
    }

    // An image whose decoded size exceeds the allocation limit: the Image
    // object EXISTS (the entry is listed) but its QImage was refused by
    // QImageReader's allocation limit — and the drag must STILL offer the FILE
    // on disk. Measured: the mime carries the file path regardless of the
    // decode refusal (the refusal lives at the decode layer, the mime at the
    // path layer — two different floors of the same building).
    void draggingAnOverLimitImageStillOffersTheFile() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // the user-side limit knob, tightened for the test: 8MB refuses the
        // ~70MB decode deterministically, whatever Qt 6.10's own default is
        settings->setMemoryAllocationLimit(8);
        const QString huge = QDir(alpha).filePath("huge.png");
        QImage big(4200, 4200, QImage::Format_ARGB32); // ~70MB decoded, over the limit
        big.fill(Qt::red);
        QVERIFY(big.save(huge, "png"));

        std::unique_ptr<QMimeData> mime(core.mimeDataForPaths({huge}));

        QVERIFY2(mime->hasUrls(), "an over-limit entry produced a mime without urls");
        QCOMPARE(mime->urls().constFirst(), QUrl::fromLocalFile(huge));
        QVERIFY(QFileInfo::exists(huge)); // the offered payload is the real file
    }

    // A drag is an OPERATION, not a session object: QDrag::exec() blocks until
    // the gesture ends, so nothing it carries may outlive the call that started
    // it. The old shape (mDrag = new QDrag(this)) overwrote the member on every
    // gesture and left the previous QDrag — plus the QMimeData it owns, which
    // for image drags can carry pixels — parented to Core for the whole session.
    // Pin: repeated drag-outs retain nothing on Core.
    void repeatedDragOutsRetainNothing() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const QString file = QDir(alpha).filePath("a.png");
        const int before = core.children().size();

        for(int i = 0; i < 8; ++i)
            core.onDraggedOut({file});

        int drags = 0;
        for(QObject *child : core.children())
            if(child->metaObject()->className() == QLatin1String("QDrag"))
                ++drags;
        QVERIFY2(drags == 0, qPrintable(QStringLiteral("QDrag objects retained by Core after 8 drag-outs")));
        QCOMPARE(core.children().size(), before);
    }

    void presenterPopulatesTheViewWhenModelArrives() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);

        presenter.setView(&proxy);
        presenter.setModel(model);
        proxy.show(); // the strip materializes on showEvent, not on populate

        QVERIFY2(proxy.isInitialized(), "show never materialized the strip");
    }

    void presenterSetViewTwiceKeepsTheFirstBinding() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy first, second;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);

        presenter.setView(&first);
        first.show();
        presenter.setView(&second); // rejected: the binding is already live
        presenter.setModel(model);

        QVERIFY(first.isInitialized());
        QVERIFY2(!second.isInitialized(), "the second setView silently replaced the first");
    }

    void presenterForwardsActivationToTheFilePath() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&proxy);
        proxy.show(); // materialize the strip so the forwarding path is the real one
        presenter.setModel(model);
        QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);
        QVERIFY(activated.isValid());

        QMetaObject::invokeMethod(&proxy, "itemActivated", Q_ARG(int, 0));

        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.at(0).at(0).toString(), QDir(alpha).filePath("a.png"));
    }

    void presenterDisconnectViewStopsForwardingAndAllowsRebinding() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy first, second;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&first);
        presenter.setModel(model);
        QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);

        QMetaObject::invokeMethod(&first, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 1);

        presenter.disconnectView();
        QMetaObject::invokeMethod(&first, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 1); // the detached view no longer reaches us

        presenter.setView(&second); // rebinding was impossible before disconnectView
        second.show();
        presenter.setModel(model);
        QVERIFY2(second.isInitialized(), "rebinding after disconnectView does not populate");
        QMetaObject::invokeMethod(&second, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 2);
        QCOMPARE(activated.at(1).at(0).toString(), QDir(alpha).filePath("a.png"));
    }

    void presenterUnsetModelKeepsTheViewBinding() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&proxy);
        proxy.show();
        presenter.setModel(model);

        presenter.unsetModel(); // detaches the model, keeps the view on purpose

        QVERIFY(proxy.isInitialized()); // the binding survived the model swap
        auto model2 = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model2->setDirectory(beta));
        QTRY_COMPARE_WITH_TIMEOUT(model2->fileCount(), 1, 10000);
        presenter.setModel(model2);
        QVERIFY2(proxy.isInitialized(), "re-setting a model on a live binding must repopulate");
    }

    // ---- the editing cluster: edit_template's executors are dialog-free in
    // document mode (edits stay in memory until an explicit save) and
    // confirmation-gated in folder view (edits hit the disk immediately). The
    // fake scripts the confirmation; the assertions watch memory, disk, and
    // the confirm count. The fixture is a 16x8 with two colored halves: a
    // solid square could never show a rotation.

    QString writeRectImage(const QString &path) {
        QImage img(16, 8, QImage::Format_RGB32);
        img.fill(Qt::red);
        QPainter p(&img);
        p.fillRect(8, 0, 8, 8, Qt::blue);
        p.end();
        if(!img.save(path, "png")) {
            QTest::qFail(qPrintable(QStringLiteral("cannot encode %1").arg(path)), __FILE__, __LINE__);
            return {};
        }
        return path;
    }

    // The presenter's five view connections are compiled against the concrete
    // proxy type (setView is a template: see directorypresenter.h), so a signal
    // or slot that changed shape fails the build instead of silently disconnect
    // at runtime — which is what the SIGNAL()/SLOT() strings this replaced used
    // to allow. What a compiler cannot check is delivery: whether a signal the
    // view really emits reaches the handler and produces the effect. The tests
    // in this cluster pin delivery per endpoint — itemActivated both ways,
    // draggedOut and droppedInto below.
    // The presenter's two remaining string-based view connections. Neither has
    // a signal at the far end — thumbnailsRequested drives the thumbnailer and
    // the result comes back as a view *method call*, draggedOver reaches the
    // view only while directories are listed — so both are pinned by following
    // the call back into the view rather than by spying on a signal.
    void presenterRoutesThumbnailsAndDragHoverToTheView() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        CountingStripProxy view;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&view);
        view.show(); // materialize so the forwarding path is the real one
        presenter.setModel(model);

        // the strip requests thumbnails for itself once it materializes; wait
        // that out, then clear the counter so the number below is attributable
        // only to the invoke that drives the presenter's connection
        QTRY_VERIFY_WITH_TIMEOUT(view.thumbnailCalls > 0, 10000);
        view.thumbnailCalls = 0;
        view.lastThumbnailPos = -1;

        QMetaObject::invokeMethod(&view, "thumbnailsRequested", Q_ARG(QList<int>, {0}), Q_ARG(int, 128),
                                  Q_ARG(bool, true), Q_ARG(bool, false));
        QTRY_COMPARE_WITH_TIMEOUT(view.thumbnailCalls, 1, 10000);
        QCOMPARE(view.lastThumbnailPos, 0);

        // the draggedOver branch needs a directory index while dirs are shown,
        // and onDraggedOver skips anything already selected — populate selects
        // index 0, so the hover target is the second directory
        QVERIFY(QDir(alpha).mkpath("subdir"));
        QVERIFY(QDir(alpha).mkpath("subdir2"));
        presenter.setShowDirs(true);
        presenter.reloadModel();
        QTRY_COMPARE_WITH_TIMEOUT(model->dirCount(), 2, 10000);
        // onDraggedOver has three guards (model, selection, dirs-visible) and
        // a hover index that must name a directory: each is asserted right
        // before the invoke so a lost delivery names the guard it hit instead
        // of reporting a bare zero.
        QVERIFY2(presenter.showDirs(), "precondition broken: the presenter is in files-only mode");
        QVERIFY2(model->dirCount() > 1,
                 qPrintable(QStringLiteral("precondition broken: only %1 directories listed").arg(model->dirCount())));
        QVERIFY2(!view.selection().contains(1),
                 "precondition broken: index 1 is selected, onDraggedOver returns early");

        QMetaObject::invokeMethod(&view, "draggedOver", Q_ARG(int, 1));
        QVERIFY2(view.hoverCalls == 1,
                 qPrintable(QStringLiteral("setDragHover never reached the view (dirCount=%1, "
                                           "showDirs=%2, selection=%3)")
                                .arg(model->dirCount())
                                .arg(presenter.showDirs())
                                .arg(view.selection().isEmpty() ? QStringLiteral("empty")
                                                                : QStringLiteral("%1").arg(view.selection().at(0)))));
    }

    void presenterForwardsDragFromTheView() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        ThumbnailStripProxy proxy;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&proxy);
        proxy.show(); // materialize so the forwarding path is the real one
        presenter.setModel(model);

        QSignalSpy out(&presenter, &DirectoryPresenter::draggedOut);
        QSignalSpy into(&presenter, &DirectoryPresenter::droppedInto);
        QVERIFY(out.isValid() && into.isValid());

        // view draggedOut() -> presenter onDraggedOut -> presenter draggedOut(paths)
        // Qt6 signals are public, so the forwarding chain is driven directly:
        // a by-name invoke would put the signature in a string for no service
        emit proxy.draggedOut();
        QCOMPARE(out.count(), 1);

        // view droppedInto(mime, source, index) -> presenter droppedInto(paths, dir)
        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(QDir(alpha).filePath("a.png"))});
        emit proxy.droppedInto(&data, nullptr, 0);
        QCOMPARE(into.count(), 1);
    }

    // The presenter is the only place that turns an engine-side index — files
    // and directories counted in separate lists, dirs first — into the view's
    // single flat grid index. Nothing else checks that arithmetic, and a wrong
    // offset is silent: the grid still splices, just one row away from the
    // entry the event named. The events are emitted by hand because the
    // contract under test is the translation, not the arrival — arrival and
    // index provenance are pinned by the mirror-contract test.
    void presenterTranslatesEngineIndicesIntoGridIndices() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        const QString dir = uniqueDir("splice");
        QVERIFY(QDir(dir).mkpath("d1"));
        QVERIFY(QDir(dir).mkpath("d2"));
        writeImage(QDir(dir).filePath("f1.png"));
        writeImage(QDir(dir).filePath("f2.png"));
        QVERIFY(model->setDirectory(dir));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(model->dirCount(), 2, 10000);
        const QString f0 = model->filePathAt(0);
        const QString f1 = model->filePathAt(1);
        const QString d0 = model->dirPathAt(0);
        const QString d1 = model->dirPathAt(1);
        const int dirCount = model->dirCount();
        QVERIFY2(dirCount > 0, "precondition broken: no directories to offset by");

        CountingStripProxy view;
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&view);
        presenter.setModel(model);

        // files-only: the engine index goes to the grid untranslated, and the
        // directory handlers refuse the event outright
        QMetaObject::invokeMethod(model.get(), "fileModified", Q_ARG(QString, f0));
        QCOMPARE(view.reloaded, QList<int>({0}));
        QMetaObject::invokeMethod(model.get(), "fileAdded", Q_ARG(QString, f1));
        QCOMPARE(view.inserted, QList<int>({1}));
        QMetaObject::invokeMethod(model.get(), "fileRemoved", Q_ARG(QString, f0), Q_ARG(int, 0));
        QCOMPARE(view.removed, QList<int>({0}));
        QMetaObject::invokeMethod(model.get(), "dirRemoved", Q_ARG(QString, d0), Q_ARG(int, 0));
        QMetaObject::invokeMethod(model.get(), "dirAdded", Q_ARG(QString, d0));
        QCOMPARE(view.removed.count(), 1); // dirs are not in the grid: no splice
        QCOMPARE(view.inserted.count(), 1);

        // A rename is one call, not a remove plus an insert: the old row comes
        // from the event (the mirror has already spliced, so the listing cannot
        // answer "where was it") and the new row and the name come from the
        // listing. In files-only mode a directory rename is refused outright.
        QMetaObject::invokeMethod(model.get(), "fileRenamed", Q_ARG(QString, f0), Q_ARG(QString, f1), Q_ARG(int, 0));
        QCOMPARE(view.renamed, QList<int>({0, 1}));
        QCOMPARE(view.renamedTo, QList<QString>({QFileInfo(f1).fileName()}));
        QMetaObject::invokeMethod(model.get(), "dirRenamed", Q_ARG(QString, d0), Q_ARG(QString, d1), Q_ARG(int, 0));
        QCOMPARE(view.renamed.count(), 2);

        view.removed.clear();
        view.inserted.clear();
        view.reloaded.clear();
        view.renamed.clear();
        view.renamedTo.clear();

        // dirs shown: every file index shifts by dirCount, dir indices do not
        presenter.setShowDirs(true);
        QMetaObject::invokeMethod(model.get(), "fileModified", Q_ARG(QString, f0));
        QCOMPARE(view.reloaded, QList<int>({dirCount}));
        QMetaObject::invokeMethod(model.get(), "fileAdded", Q_ARG(QString, f1));
        QCOMPARE(view.inserted, QList<int>({dirCount + 1}));
        QMetaObject::invokeMethod(model.get(), "fileRemoved", Q_ARG(QString, f0), Q_ARG(int, 0));
        QCOMPARE(view.removed, QList<int>({dirCount}));
        QMetaObject::invokeMethod(model.get(), "dirRemoved", Q_ARG(QString, d0), Q_ARG(int, 0));
        QMetaObject::invokeMethod(model.get(), "dirAdded", Q_ARG(QString, d0));
        QCOMPARE(view.removed, QList<int>({dirCount, 0}));
        QCOMPARE(view.inserted, QList<int>({dirCount + 1, 0}));

        // with dirs in front, both rename sides of a *file* shift by dirCount
        // while a dir row — already in the leading block — does not shift at all
        view.renamed.clear();
        view.renamedTo.clear();
        QMetaObject::invokeMethod(model.get(), "fileRenamed", Q_ARG(QString, f0), Q_ARG(QString, f1), Q_ARG(int, 0));
        QMetaObject::invokeMethod(model.get(), "dirRenamed", Q_ARG(QString, d0), Q_ARG(QString, d1), Q_ARG(int, 0));
        QCOMPARE(view.renamed, QList<int>({dirCount, dirCount + 1, 0, 1}));
        QCOMPARE(view.renamedTo, QList<QString>({QFileInfo(f1).fileName(), QFileInfo(d1).fileName()}));
    }
    void viewlessProxyRemapsItsBufferedSelectionOnRename() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool, EngineBridge::instance());
        const QString dir = uniqueDir("proxyrename");
        writeImage(QDir(dir).filePath("a.png"));
        writeImage(QDir(dir).filePath("b.png"));
        writeImage(QDir(dir).filePath("c.png"));
        QVERIFY(model->setDirectory(dir));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 3, 10000);

        CountingStripProxy view; // never shown: the buffer is all there is
        Thumbnailer thumbnailer(pool, EngineBridge::instance());
        DirectoryPresenter presenter(thumbnailer);
        presenter.setView(&view);
        presenter.setModel(model); // populate + select(0)
        QCOMPARE(view.selection(), QList<int>({0}));
        QCOMPARE(model->fileNameAt(0), QStringLiteral("a.png"));

        QVERIFY(QFile::rename(QDir(dir).filePath("a.png"), QDir(dir).filePath("z.png")));

        QTRY_COMPARE_WITH_TIMEOUT(view.renamed.count(), 2, 10000);
        // a.png was row 0 and z.png is the last row: the buffer moved with it,
        // so a strip materialized later replays the highlight onto the renamed row
        QCOMPARE(view.renamed, QList<int>({0, 2}));
        QCOMPARE(view.selection(), QList<int>({2}));
    }

    // The settings bus drains a pool on the UI thread, and the scope of that
    // drain is what a Save press costs. With a Loader job provably running, a
    // bus that waits for every owner cannot return before the job's deadline;
    // a bus scoped to the owner its settings actually feed returns at once.
    // The held job is observed still-held afterwards, so the pass is not just
    // "it was fast" — it is "it returned while the work was in there".
    void settingsBusWaitsOnlyForThePipelineItsSettingsActuallyFeed() {
        Core core;
        std::atomic_bool gate{false};
        std::atomic_int holds{0};
        core.executor().submit(PoolOwner::Loader, 0, std::make_unique<DeadlineJob>(&gate, &holds));
        QTRY_COMPARE_WITH_TIMEOUT(holds.load(), 1, 10000);

        QElapsedTimer t;
        t.start();
        settings->sendChangeNotification(); // what the settings dialog's Save does
        const qint64 elapsed = t.elapsed();

        QCOMPARE(holds.load(), 1); // the Loader job was still running when it returned
        QVERIFY2(elapsed < 1000, qPrintable(QStringLiteral("the settings bus waited %1 ms — a pool drain that is not "
                                                           "owner-scoped waits for work none of its settings feed")
                                                .arg(elapsed)));
        gate = true;
        QTRY_COMPARE_WITH_TIMEOUT(holds.load(), 0, 10000);
    }

    // The chain is not a fixture project: MainWindow builds the whole graph at
    // startup (mainwindow.cpp:71-73 — ViewerWidget, InfoBarProxy,
    // DocumentWidget), so every Core in this suite already owns it and the test
    // only has to reach it. This is the runtime half of the claim that
    // MainPanel deliberately stays off the bus: the refresh arrives through
    // DocumentWidget::readSettings -> MainPanel::readSettings ->
    // ThumbnailStrip::readSettings, and a subscription of MainPanel's own would
    // run it twice per change.
    void settingsBusReachesTheThumbnailStripThroughThePanelChain() {
        Core core;
        const int original = settings->panelPreviewsSize(); // singleton: snapshot and restore
        auto *strip = core.mainWindow()->findChild<ThumbnailStripProxy *>();
        QVERIFY2(strip, "no ThumbnailStripProxy under MW — the chain is not what this claims");
        // init(), not show(): the proxy is a child here, and a child's show()
        // fires no showEvent while its ancestors are hidden, which is what made
        // the first version of this test fail 5 times out of 5 rather than
        // sometimes. init() is the public entry showEvent itself calls.
        strip->init();
        QVERIFY2(strip->isInitialized(), "the strip did not materialize under init()");

        settings->setPanelPreviewsSize(30);
        settings->sendChangeNotification();
        const QSize small = strip->itemSize();
        settings->setPanelPreviewsSize(200);
        settings->sendChangeNotification();
        const QSize big = strip->itemSize();

        settings->setPanelPreviewsSize(original);
        settings->sendChangeNotification();

        QVERIFY2(big.height() > small.height() || big.width() > small.width(),
                 qPrintable(QStringLiteral("the settings bus does not reach ThumbnailStrip::readSettings "
                                           "(30 -> %1x%2, 200 -> %3x%4)")
                                .arg(small.width())
                                .arg(small.height())
                                .arg(big.width())
                                .arg(big.height())));
    }

    void editingInDocumentModeEditsInMemoryWithoutConfirmation() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(img), 10000);
        const QByteArray diskBefore = coretest::contentOf(img);

        core.rotateLeft();

        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(8, 16), 10000);
        QCOMPARE(fake.confirmCalls, 0);                 // document mode: in memory, no save, no ask
        QCOMPARE(coretest::contentOf(img), diskBefore); // the disk file is untouched until save
    }

    // The edited-image export used to hand every consumer one fixed path,
    // tmpDir/image.png: a drag in flight and a clipboard entry from two
    // exports shared one file and one of them read the other's pixels. The
    // file is now a uniquely-named QTemporaryFile owned by the QMimeData it
    // serves — it exists exactly as long as that mime data does, and the
    // autoremove takes it away with it.
    void theEditedImageExportGetsAUniqueTempFileOwnedByItsMime() {
        Core core;
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->isLoaded(img), 10000);
        core.rotateLeft();
        auto image = core.directoryModel()->getImage(img);
        QVERIFY(image->isEdited());

        QMimeData *first = core.getMimeDataForImage(image, TARGET_DROP);
        QMimeData *second = core.getMimeDataForImage(image, TARGET_DROP);
        const QString firstPath = QUrl(first->urls().value(0)).toLocalFile();
        const QString secondPath = QUrl(second->urls().value(0)).toLocalFile();
        QVERIFY2(firstPath != secondPath, "two exports were handed one fixed temp name");
        QVERIFY(QFile::exists(firstPath));
        QVERIFY(QFile::exists(secondPath));

        delete first; // the drag machinery does this when the gesture ends
        QVERIFY2(!QFile::exists(firstPath), "the temp file outlived its mime data");
        QVERIFY(QFile::exists(secondPath));
        delete second;
        QVERIFY2(!QFile::exists(secondPath), "the temp file outlived its mime data");
    }

    void editingInFolderViewSavesAfterScriptedConfirmation() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.toggleFolderView();

        core.rotateLeft();

        QCOMPARE(fake.confirmCalls, 1);             // folder view saves immediately, so it asks first
        QCOMPARE(QImage(img).size(), QSize(8, 16)); // the disk file is the rotated one
    }

    void editingDeclinedInFolderViewLeavesTheDiskAlone() {
        Core core;
        coretest::FakeInteractions fake;
        fake.confirmAnswer = false;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.toggleFolderView();

        core.rotateLeft();

        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(QImage(img).size(), QSize(16, 8)); // declined: the disk is untouched
    }

    void resizeEditsTheLoadedImage() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.resize(QSize(4, 4));

        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(4, 4), 10000);
        QCOMPARE(fake.confirmCalls, 0); // resize never asks
    }

    void cropAndSaveWritesTheCroppedFile() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.cropAndSave(QRect(0, 0, 8, 8));

        QCOMPARE(QImage(img).size(), QSize(8, 8)); // cropped AND saved to disk
        QCOMPARE(fake.confirmCalls, 0);            // cropAndSave bypasses the confirmation
    }

    void discardEditsRestoresTheOnDiskImage() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.rotateLeft();
        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(img)->size(), QSize(8, 16), 10000);

        core.discardEdits();

        QCOMPARE(core.directoryModel()->getImage(img)->size(), QSize(16, 8)); // back to the disk state
    }
    void gridActionsDriveTheGridThroughTheFullChain() {
        const QString dir = uniqueDir("gridact");
        writeImage(QDir(dir).filePath("a.png"));
        writeImage(QDir(dir).filePath("b.png"));

        Core core;
        QVERIFY(core.loadPath(QDir(dir).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.enableFolderView();
        core.folderPresenter().selectAndFocus(0);
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 1,
                                  "precondition broken: the folder view holds no selection", 10000);

        QVERIFY(actionManager->invokeAction(QStringLiteral("gridSelectNext")));
        QCOMPARE(core.currentSelection().count(), 1);
        QCOMPARE(core.currentSelection().constFirst(), QDir(dir).filePath("b.png"));

        QVERIFY(actionManager->invokeAction(QStringLiteral("gridOpen")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(dir).filePath("b.png"), 10000);
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // One process per test function — the same rationale as engine_stack_main's
    // fork per class, taken one level down: this suite decodes images, which
    // makes QtGui spawn and clockjoin-join its own internal pooled threads, and
    // each of those joins leaves a registry-live pthread_t behind (TSan does not
    // intercept clockjoin — LLVM #239326).
    //
    // The fixture is constructed INSIDE the child, not before the fork. Built
    // once in the parent and inherited, every child would share a settings
    // object whose tmpDir names the parent's temp directory; the first child to
    // finish destroys it on its way out (IsolatedHome's destructor), and every
    // later child runs with a tmpDir that does not exist. QTemporaryFile then
    // opens into nothing and the edited-image export silently falls back to one
    // fixed name — what theEditedImageExportGetsAUniqueTempFileOwnedByItsMime
    // and clipboardFileCopyAdvertisesTheFileInEveryFormat exist to catch.
    if(argc > 1) {
        TestCoreNav t;
        return QTest::qExec(&t, argc, argv);
    }
    const QMetaObject *mo = &TestCoreNav::staticMetaObject;
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
            TestCoreNav t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        // a crashed child loses its buffered output entirely — without naming
        // the slot here, the evidence is a red ctest with nothing in the log
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "core_nav: slot '%s' exited abnormally (exit=%d, sig=%d)\n", slot.toUtf8().constData(),
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "core_nav_tests.moc"
