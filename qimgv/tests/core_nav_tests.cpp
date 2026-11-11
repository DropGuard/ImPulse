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

// The scripted IUserInteractions: answers queued as DialogResults (consumed in
// order), call counts and last arguments as the assertions' teeth. An empty
// queue answers default-constructed DialogResult (no, no all, no cancel) —
// a deliberate trap: an unexpected question surfaces as a failed expectation,
// not as a silently-tolerated dialog.
class FakeInteractions : public IUserInteractions {
  public:
    QQueue<DialogResult> replaceAnswers;
    QList<QPair<QString, QString>> replaceArgs;
    int replaceCalls = 0;
    int confirmCalls = 0;
    bool confirmAnswer = true;

    bool showConfirmation(const QString &, const QString &) override {
        ++confirmCalls;
        return confirmAnswer;
    }
    DialogResult fileReplaceDialog(const QString &src, const QString &dst, FileReplaceMode, bool) override {
        ++replaceCalls;
        replaceArgs.append({src, dst});
        return replaceAnswers.isEmpty() ? DialogResult{} : replaceAnswers.dequeue();
    }
    int messageCalls = 0;
    QString lastMessage;
    void showMessage(const QString &text) override {
        ++messageCalls;
        lastMessage = text;
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
        // IsolatedHome redirected HOME and the config identity
        QVERIFY(home.path() != nullptr);

        // the same globals main.cpp sets, in the same order
        inputMap = InputMap::getInstance();
        appActions = Actions::getInstance();
        settings = Settings::getInstance();
        scriptManager = ScriptManager::getInstance();
        actionManager = ActionManager::getInstance();
        shrRes = SharedResources::getInstance();
        QVERIFY(settings != nullptr);
        QVERIFY(actionManager != nullptr);

        base = QDir(home.path()).filePath("nav");
        alpha = QDir(base).filePath("alpha");
        beta = QDir(base).filePath("beta");
        QVERIFY(QDir().mkpath(alpha));
        QVERIFY(QDir().mkpath(beta));
        writeImage(QDir(alpha).filePath("a.png"));
        writeImage(QDir(beta).filePath("b.png"));
    }

    void pagingKeysHaveNamesAndDefaults() {
        // a press with no name in the scancode table cannot reach the action
        // layer at all; a name with no default never fires for a config that
        // predates the binding — pin both halves, plus the lookup itself
        QVERIFY(inputMap->keys().value(112) == "PgUp");
        QVERIFY(inputMap->keys().value(117) == "PgDown");
        QVERIFY(actionManager->actionForShortcut("PgUp") == "prevImage");
        QVERIFY(actionManager->actionForShortcut("PgDown") == "nextImage");
        QVERIFY(actionManager->invokeAction(actionManager->actionForShortcut("PgDown")));
        QVERIFY(actionManager->invokeAction(actionManager->actionForShortcut("PgUp")));
        QVERIFY(actionManager->actionForShortcut("F13").isEmpty());
    }

    // one slot sweeping the title bar's assembly across every state — the
    // simple user-visible surfaces ride together by design, the way the user
    // reads them: one glance, not eight test functions
    void userVisibleTextAssemblySweepsTheStates() {
        CurrentInfo info{};
        info.index = 1;
        info.fileCount = 3;
        info.fileName = "shot.png";
        info.imageSize = QSize(1920, 1080);
        info.fileSize = 3200000;
        QString size = QLocale().formattedDataSize(3200000, 1);

        // full house: extended info on, nothing toggled
        QCOMPARE(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                     QStringLiteral("Folder view")),
                 QStringLiteral("[ 2/3 ]  shot.png  -  1920 x 1080  -  ") + size);
        // missing dimensions and size: the segments drop out whole
        info.imageSize = QSize(0, 0);
        info.fileSize = 0;
        QCOMPARE(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                     QStringLiteral("Folder view")),
                 QStringLiteral("[ 2/3 ]  shot.png"));
        // extended info off: bare name — the position prefix lives in the same gate
        info.imageSize = QSize(1920, 1080);
        info.fileSize = 3200000;
        QCOMPARE(assembleWindowTitle(false, info, false, false, false, false, QStringLiteral("ImPulse"),
                                     QStringLiteral("Folder view")),
                 QStringLiteral("shot.png"));
        // toggle states ride only when the info bar is not already showing them
        info.slideshow = info.shuffle = true;
        QCOMPARE(assembleWindowTitle(false, info, true, true, false, true, QStringLiteral("ImPulse"),
                                     QStringLiteral("Folder view")),
                 QStringLiteral("shot.png - [slideshow] [shuffle] [zoom lock] [view lock]"));
        // edited marks the front of the assembled line
        info.slideshow = info.shuffle = false;
        info.edited = true;
        QCOMPARE(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                     QStringLiteral("Folder view")),
                 QStringLiteral("* [ 2/3 ]  shot.png  -  1920 x 1080  -  ") + size);
        // the two degenerate surfaces
        QCOMPARE(assembleWindowTitle(true, info, false, false, true, false, QStringLiteral("ImPulse"),
                                     QStringLiteral("Folder view")),
                 QStringLiteral("Folder view"));
        info.fileName.clear();
        info.edited = false;
        QCOMPARE(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                     QStringLiteral("Folder view")),
                 QStringLiteral("ImPulse"));
    }

    // upstream issue #636, promoted from the author's own external script:
    // one keypress copies the current file into the configured folder —
    // no dialog, no clobbering. Unconfigured, it says so instead of
    // guessing a destination.
    void quickCopyLandsInTheConfiguredFolder() {
        QString dest = QDir(home.path()).filePath("quickcopy-dest");
        settings->setQuickCopyDirectory(dest);
        QVERIFY(QDir().mkpath(dest));

        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        QString img = QDir(base).filePath("qc.png");
        writeImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), img, 10000);

        actionManager->invokeAction(QStringLiteral("quickCopy"));
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo::exists(QDir(dest).filePath("qc.png")), true, 10000);

        // second press: the same-name file is skipped, still exactly one copy
        actionManager->invokeAction(QStringLiteral("quickCopy"));
        QCOMPARE(QDir(dest).entryList(QDir::Files).size(), 1);
    }

    // the popup owns the keyboard while open: Up/Down move a selection over
    // the visible items with wrap-around, and Enter activates the selected
    // row through the same path a mouse press takes
    void contextMenuKeyboardNavigationMovesAndWraps() {
        TestableContextMenu menu;
        menu.showAt(QPoint(40, 40));
        QCOMPARE(menu.navigationIndex(), -1);

        menu.pressKey(Qt::Key_Down); // from nothing: the first item
        QCOMPARE(menu.navigationIndex(), 0);
        menu.pressKey(Qt::Key_Up); // wrap backwards: the last item
        QVERIFY(menu.navigationIndex() > 0);
        menu.pressKey(Qt::Key_Down); // wrap forwards: back to the first
        QCOMPARE(menu.navigationIndex(), 0);
        menu.pressKey(Qt::Key_Down);
        QCOMPARE(menu.navigationIndex(), 1);
    }

    // the projection drives the rendered title: state write -> observer ->
    // refresh. This is the wiring the whole pull-model rests on; without it
    // the title freezes at the ctor default and no suite could tell.
    void titleFollowsTheProjection() {
        writeImage(QDir(base).filePath("t.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("t.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("t.png"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.mw->windowTitle().contains(QStringLiteral("t.png")), 10000);

        // the slideshow toggle rides the same wiring
        core.toggleSlideshow();
        QTRY_VERIFY_WITH_TIMEOUT(core.mw->windowTitle().contains(QStringLiteral("[slideshow]")), 10000);
        core.toggleSlideshow();
        QTRY_VERIFY_WITH_TIMEOUT(!core.mw->windowTitle().contains(QStringLiteral("[slideshow]")), 10000);
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
        QTRY_COMPARE_WITH_TIMEOUT(core.mw->getFolderView()->directoryPath(), QDir(base).filePath("projdir"), 10000);
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
        core.mw->fitOriginal();
        auto *viewer = core.mw->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);

        core.nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("insB.png"), 10000);
        QCOMPARE(viewer->fitMode(), FIT_ORIGINAL);

        // armed: left pans, it never flips images
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QCOMPARE(core.currentFilePath(), QDir(base).filePath("insB.png"));

        // back in a browse mode the same key navigates again
        core.mw->fitWindow();
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
        core.mw->fitOriginal();
        auto *viewer = core.mw->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        float scaleBefore = viewer->currentScale();

        core.mw->crop()->trigger();
        QVERIFY(core.mw->crop()->isCropPanelActive());
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
        core.mw->overlays()->triggerCopyOverlay();
        auto *copyOverlay = core.mw->findChild<CopyOverlay *>();
        QVERIFY(copyOverlay && copyOverlay->isVisible());
        QApplication::sendEvent(copyOverlay, &esc);
        QVERIFY(!copyOverlay->isVisible());

        // save-confirm prompt (surfaced by edits — flips H/V land here too):
        // Esc means "discard edits", the same chain as the discard button
        settings->setShowSaveOverlay(true);
        core.mw->overlays()->showSaveOverlay();
        auto *saveOverlay = core.mw->findChild<SaveConfirmOverlay *>();
        QVERIFY(saveOverlay && saveOverlay->isVisible());
        QApplication::sendEvent(saveOverlay, &esc);
        QVERIFY(!saveOverlay->isVisible());

        // crop surface: Esc = cancel the selection and leave crop
        core.mw->crop()->trigger();
        QVERIFY(core.mw->crop()->isCropPanelActive());
        auto *cropOverlay = core.mw->findChild<CropOverlay *>();
        QVERIFY(cropOverlay);
        QApplication::sendEvent(cropOverlay, &esc);
        QTRY_COMPARE_WITH_TIMEOUT(!core.mw->crop()->isCropPanelActive(), true, 5000);
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
        core.mw->overlays()->showSaveOverlay();
        auto *saveOverlay = core.mw->findChild<SaveConfirmOverlay *>();
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
            QKeyEvent ev(QEvent::KeyPress, key, mods, scanCodeFor(QLatin1String(keyName)), 0u, 0u);
            QApplication::sendEvent(saveOverlay, &ev);
        };

        press(Qt::Key_S, "S", Qt::ControlModifier);
        QCOMPARE(saveAction.count(), 1);    // Ctrl+S: the deliberate write...
        QVERIFY(!saveOverlay->isVisible()); // ...which also dismisses the prompt

        press(Qt::Key_S, "S", Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(saveAsAction.count(), 1); // Ctrl+Shift+S: save as

        core.mw->overlays()->showSaveOverlay(); // back up for the way out
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

    // The crop surface's answers are on faces, and the aspect presets answer
    // to their digits — 1 Free through 8 16:10 — keys that used to fall
    // through to the Framing-blocked fit slots and their toast noise. An
    // out-of-range digit is dropped, Esc still leaves crop, and the
    // keyboard's Apply is the in-memory crop only: the disk write never sits
    // on a key, so Crop && Save carries no key hint and the retired
    // Shift+Enter degrades to the safe answer.
    void cropSurfaceHintsItsKeysAndDigitsPickPresets() {
        writeImage(QDir(base).filePath("cropkeys.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("cropkeys.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.mw->crop()->trigger();
        QVERIFY(core.mw->crop()->isCropPanelActive());
        auto *panel = core.mw->findChild<CropPanel *>();
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
        QCOMPARE(combo->count(), 8);

        auto *overlay = core.mw->findChild<CropOverlay *>();
        QVERIFY(overlay);
        auto pressDigit = [&](int digit) {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_0 + digit, Qt::NoModifier, scanCodeFor(QString::number(digit)), 0u,
                         0u);
            QApplication::sendEvent(overlay, &ev);
        };

        pressDigit(5); // the fifth preset: 1:1
        QCOMPARE(combo->currentIndex(), 4);
        pressDigit(8); // the eighth preset: 16:10
        QCOMPARE(combo->currentIndex(), 7);
        pressDigit(9); // no ninth preset: dropped
        QCOMPARE(combo->currentIndex(), 7);

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
        widthBox->setValue(widthBox->maximum() - 1);
        heightBox->setValue(heightBox->maximum() - 1);

        QSignalSpy applied(panel, &CropPanel::crop);
        QSignalSpy savedToDisk(panel, &CropPanel::cropAndSave);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, scanCodeFor(QStringLiteral("Enter")), 0u, 0u);
        QApplication::sendEvent(overlay, &enter);
        QCOMPARE(applied.count(), 1);     // the subset existed, and the answer fired
        QCOMPARE(savedToDisk.count(), 0); // the bare key never writes
        QTRY_COMPARE_WITH_TIMEOUT(!core.mw->crop()->isCropPanelActive(), true, 5000); // applied and dismissed

        // the save chord, pressed while a selection stands, commits the crop
        // and writes — end to end, disk included: crop again, then Ctrl+S
        core.mw->crop()->trigger();
        QVERIFY(core.mw->crop()->isCropPanelActive());
        auto *panel2 = core.mw->findChild<CropPanel *>();
        QVERIFY(panel2);
        auto *width2 = panel2->findChild<QSpinBox *>(QStringLiteral("width"));
        auto *height2 = panel2->findChild<QSpinBox *>(QStringLiteral("height"));
        QVERIFY(width2 && height2);
        QVERIFY2(width2->maximum() > 1 && height2->maximum() > 1,
                 "precondition broken: the first crop left nothing to crop");
        width2->setValue(width2->maximum() - 1);
        height2->setValue(height2->maximum() - 1);

        QSignalSpy applied2(panel2, &CropPanel::crop);
        QKeyEvent ctrlS(QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier, scanCodeFor(QStringLiteral("S")), 0u, 0u);
        QApplication::sendEvent(core.mw->findChild<CropOverlay *>(), &ctrlS);
        QCOMPARE(applied2.count(), 1); // the chord commits the pending crop...
        QTRY_COMPARE_WITH_TIMEOUT(QImage(QDir(base).filePath("cropkeys.png")).size(),
                                  QSize(width2->maximum() - 1, height2->maximum() - 1), 10000); // ...and writes it
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
        auto *viewer = core.mw->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        core.mw->fitOriginal();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        QVERIFY(qFuzzyCompare(viewer->currentScale(), 1.0f));

        viewer->zoomIn();
        QVERIFY(qFuzzyCompare(viewer->currentScale(), 1.2f));
        viewer->zoomIn();
        QVERIFY(qFuzzyCompare(viewer->currentScale(), 1.44f));

        core.mw->switchFitMode();
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
        auto *viewer = core.mw->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        core.mw->fitOriginal();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        core.mw->crop()->trigger();
        QVERIFY(core.mw->crop()->isCropPanelActive());
        auto *overlay = core.mw->findChild<CropOverlay *>();
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
        QVERIFY(core.mw->crop()->isCropPanelActive());

        // fit remains blocked while framing — a fit would reset the very
        // view the user is framing in
        float scale = viewer->currentScale();
        viewer->fitWindow();
        QVERIFY(qAbs(viewer->currentScale() - scale) < 0.001f);
        QVERIFY(core.mw->crop()->isCropPanelActive());

        // leaving crop releases the policy: the fit keys work again — the
        // mode semantics were never vetoed, only deferred while the overlay
        // held the canvas. (No navigation assert here: fork-per-slot means
        // this fork's directory holds only the files this test created, and
        // a cross-test file assumption is exactly what the isolation
        // forbids — prevImage in a one-file directory legitimately no-ops.
        // The arrows' navigation is pinned in
        // fitOriginalPersistsAndArrowsStayInInspectMode.)
        core.mw->crop()->trigger();
        QVERIFY(!core.mw->crop()->isCropPanelActive());
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
        auto *viewer = core.mw->findChild<ViewerWidget *>();
        QVERIFY(viewer);
        core.mw->resize(1600, 900);
        core.mw->show();
        QTest::qWait(300);
        core.mw->fitOriginal();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_ORIGINAL, 5000);
        viewer->zoomIn();
        viewer->zoomIn();
        viewer->scrollLeft();
        QTest::qWait(600);
        float scaleBefore = viewer->currentScale();
        QRect rectBefore = viewer->imageRect();
        core.mw->crop()->trigger();
        QVERIFY(core.mw->crop()->isCropPanelActive());
        QTest::qWait(300);
        QVERIFY(qFuzzyCompare(viewer->currentScale(), scaleBefore));
        QCOMPARE(static_cast<int>(viewer->fitMode()), static_cast<int>(FIT_FREE));
        QCOMPARE(viewer->imageRect(), rectBefore);
    }

    void cropPanelSurvivesAnEarlyTrigger() {
        writeImage(QDir(base).filePath("crop.png"));
        Core core;
        QVERIFY(core.loadPath(QDir(base).filePath("crop.png")));
        core.mw->crop()->trigger();
        QVERIFY(core.mw->crop()->isCropPanelActive());
        core.mw->crop()->trigger();
        QVERIFY(!core.mw->crop()->isCropPanelActive());
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

        QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
        QVERIFY(listed.isValid());

        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, seq), 10000);
        // the cold-open load rides whenListed: it fires against a landed
        // listing, so the first preload has a real neighbor to decode

        // one flip forward: from b the forward neighbor c decodes
        emit actionManager->nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(1), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(files.at(2)), 10000);

        // another flip forward: from c the forward neighbor is d
        emit actionManager->nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(2), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(files.at(3)), 10000);

        // jump to the last file: still a forward move, so the backward
        // neighbor b must not decode — and the cache trim drops it
        QVERIFY(core.loadPath(files.at(3)));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(3), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.model->isLoaded(files.at(1)));

        // one backward move: the preload must follow the travel direction and
        // decode b, which a forward-facing policy would never have touched
        emit actionManager->prevImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(2), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(files.at(1)), 10000);
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
        QVERIFY(!core.mw->isVisible());
        QVERIFY(core.loadPath(files.at(0)));
        QVERIFY(core.firstFramePending());
        // main's tail, faithfully: show only when nothing is pending
        if(!core.firstFramePending())
            core.showGui();
        QVERIFY(!core.mw->isVisible());

        QTRY_VERIFY_WITH_TIMEOUT(!core.firstFramePending(), 10000);
        QVERIFY(core.mw->isVisible());
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

    void themeColorRowsSurviveJunk() {
        FreshSettings fresh;
        QSettings theme(QCoreApplication::organizationName(), "theme");
        theme.beginGroup("Colors");
        theme.setValue("background", "banana"); // junk -> default
        theme.setValue("accent", "#ff0000");    // valid -> passes through
        theme.endGroup();
        theme.sync();
        settings->loadTheme();
        QCOMPARE(settings->colorScheme().background, QColor::fromString(QStringLiteral("#1a1a1a")));
        QCOMPARE(settings->colorScheme().accent, QColor::fromString(QStringLiteral("#ff0000")));
    }

    void dialogCheckboxBindingsAreSymmetric() {
        FreshSettings fresh;
        settings->setTrackpadDetection(true);

        SettingsDialog dlg;
        auto *cb = dlg.findChild<QCheckBox *>("trackpadDetectionCheckBox");
        QVERIFY(cb != nullptr);
        // read half: the widget reflects the setting
        QCOMPARE(cb->isChecked(), true);
        // save half: the widget's state lands in the setting
        cb->setChecked(false);
        dlg.saveSettings();
        QCOMPARE(settings->trackpadDetection(), false);
        // and an untouched binding round-trips without drift
        dlg.readSettings();
        QCOMPARE(dlg.findChild<QCheckBox *>("trackpadDetectionCheckBox")->isChecked(), false);
        dlg.saveSettings();
        QCOMPARE(settings->trackpadDetection(), false);
        // permanent delete is confirm-gated unconditionally now: no checkbox
        // exists for it, and the behavior pin lives in
        // removePermanentDeletesAfterScriptedConfirmation
        QVERIFY(dlg.findChild<QCheckBox *>("confirmDeleteCheckBox") == nullptr);
    }

    // The import is a user request, so its two UI facts matter: whether the
    // button is offered at all, and what the user is told afterwards. Both are
    // decided in the dialog, not in Settings::importQimgvConfig, and until now
    // neither had a pin.
    //
    // The message is built by importMessage() rather than read off a
    // QMessageBox: information() blocks on a modal dialog no headless run can
    // dismiss, so the text had to become reachable without showing it.
    void importIsOfferedOnlyWhenThereIsSomethingToImportAndReportsWhatHappened() {
        FreshSettings fresh;
        // IsolatedHome already redirected XDG_CONFIG_HOME, so the "qimgv" tree
        // below is the one Settings::qimgvConfigDir() resolves
        const QString qimgvDir = Settings::qimgvConfigDir();
        QVERIFY(QDir().mkpath(qimgvDir));
        const QString qimgvConf = qimgvDir + "/qimgv.conf";

        // nothing to import: the control is not offered, and its absence is the
        // message — a permanently dead button would teach the user nothing
        QVERIFY(QFile::remove(qimgvConf) || !QFileInfo::exists(qimgvConf));
        {
            SettingsDialog dlg;
            auto *button = dlg.findChild<QPushButton *>("importQimgvButton");
            QVERIFY(button != nullptr);
            QVERIFY2(button->isHidden(), "the import was offered with no qimgv configuration to import");
        }

        // with a source present, the button appears
        writeTestFile(qimgvConf, "theConf=fromQimgv\n");
        {
            SettingsDialog dlg;
            auto *button = dlg.findChild<QPushButton *>("importQimgvButton");
            QVERIFY(button != nullptr);
            QVERIFY2(!button->isHidden(), "the import was hidden although a source exists");

            // and each outcome has its own text, because they ask the user to do
            // different things ("restart" vs "nothing happened" vs "this failed")
            const QString dest = QDir(home.path()).filePath("imported-conf");
            const QString imported = dlg.importMessage(qimgvDir, dest);
            QVERIFY2(imported.contains(QStringLiteral("Imported")), qPrintable(imported));
            QVERIFY2(imported.contains(QStringLiteral("Restart")), qPrintable(imported));

            const QString again = dlg.importMessage(qimgvDir, dest);
            QVERIFY2(again.contains(QStringLiteral("already exists")), qPrintable(again));

            const QString none = dlg.importMessage(QDir(home.path()).filePath("nothing-here"), dest);
            QVERIFY2(none.contains(QStringLiteral("No qimgv configuration")), qPrintable(none));
        }
        QFile::remove(qimgvConf);
    }

    void dialogComboBindingFollowsTheSetting() {
        FreshSettings fresh;
        settings->setLanguage("ja_JP");

        SettingsDialog dlg;
        auto *combo = dlg.findChild<QComboBox *>("langComboBox");
        QVERIFY(combo != nullptr);
        // read half: the stored locale maps through the display-name list
        QCOMPARE(combo->currentText(), QStringLiteral("日本語"));
        // save half: a user pick lands as the stored locale
        combo->setCurrentIndex(combo->findText(QStringLiteral("简体中文")));
        dlg.saveSettings();
        QCOMPARE(settings->language(), QStringLiteral("zh_CN"));
        // an unknown stored value falls back to en_US — the old fallback line
        // ("setCurrentText("en_US")") never matched any item and silently
        // kept the previous selection
        settings->setLanguage("xx_XX");
        dlg.readSettings();
        QCOMPARE(combo->currentText(), QStringLiteral("English"));
        dlg.saveSettings();
        QCOMPARE(settings->language(), QStringLiteral("en_US"));
    }

    void wallpaperCommandDispatchesBySession() {
        // plasma
        auto [p, a] = Core::wallpaperCommand("plasma", "/pics/a b.png");
        QCOMPARE(p, QStringLiteral("plasma-apply-wallpaperimage"));
        QCOMPARE(a, QStringList{QStringLiteral("/pics/a b.png")}); // spaces verbatim
        // a quote in the filename cannot escape: there is no shell to escape into
        auto [p2, a2] = Core::wallpaperCommand("plasma", QStringLiteral("/pics/x\";y.png"));
        QCOMPARE(a2, QStringList{QStringLiteral("/pics/x\";y.png")});
        // gnome
        auto [p3, a3] = Core::wallpaperCommand("gnome", "/pics/a.png");
        QCOMPARE(p3, QStringLiteral("gsettings"));
        QStringList gnomeArgs{"set", "org.gnome.desktop.background", "picture-uri", "/pics/a.png"};
        QCOMPARE(a3, gnomeArgs);
        // unsupported session: empty program, the caller reports and bails
        auto [p4, a4] = Core::wallpaperCommand("twm", "/pics/a.png");
        QVERIFY(p4.isEmpty());
        QVERIFY(a4.isEmpty());
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

    // The f8e8b216 "pure renaming" commit renamed constructor and callback
    // parameters without updating the bodies, and six call sites silently
    // switched to reading members instead of the arguments. These pins hold
    // the argument-actually-lands behavior at each of those sites.

    void floatingWidgetContainerSizeFollowsTheArgument() {
        FloatingWidgetContainer container;
        ProbeWidget probe(&container);
        probe.pokeContainerSize(QSize(300, 150));
        QCOMPARE(probe.containerSize(), QSize(300, 150));
    }

    void resizeDialogSeedsItsFieldsFromTheArgument() {
        ResizeDialog dialog(QSize(800, 600));
        QCOMPARE(dialog.newSize(), QSize(800, 600));
    }

    void videoControlsPlaybackPositionFollowsTheArgument() {
        FloatingWidgetContainer container;
        VideoControls controls(&container);
        controls.setMode(PLAYBACK_VIDEO); // exercise the video branch of the label
        controls.setPlaybackPosition(42);
        QCOMPARE(controls.playbackPosition(), 42);
        // a repeat is the dedup no-op, not a corruption
        controls.setPlaybackPosition(42);
        QCOMPARE(controls.playbackPosition(), 42);
    }

    // CropPanel makes all 17 of its connections with pointer-to-member syntax,
    // so the compiler checks every name and signature — but "compiles" is not
    // "delivers". Each pair is driven at one endpoint and observed at the other.
    // Measured first: an overlay with no drawn selection writes 0 back into the
    // width/height boxes, so doCrop()/doCropSave() take their else-branch and
    // emit cancel — the crop buttons are therefore pinned on "exactly one of
    // {crop, cropAndSave, cancel} fired", not on a guessed branch.
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
        auto *ary = panel.findChild<QDoubleSpinBox *>("ARY");
        QVERIFY(cancelBtn && cropBtn && cropSaveBtn && posX && width && height && ary);

        QSignalSpy cancel(&panel, &CropPanel::cancel);
        QSignalSpy crop(&panel, &CropPanel::crop);
        QSignalSpy cropSave(&panel, &CropPanel::cropAndSave);
        QSignalSpy selection(&panel, &CropPanel::selectionChanged);
        QSignalSpy ratio(&panel, &CropPanel::aspectRatioChanged);
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

        // ui->ARY valueChanged -> onAspectRatioChange -> aspectRatioChanged
        ratio.clear();
        ary->setValue(3.0);
        QCOMPARE(ratio.count(), 1);

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
        QSignalSpy loop(core.mw, &MW::setLoopPlayback);
        QVERIFY(loop.isValid());

        core.startSlideshow();
        QVERIFY(core.state.slideshow);
        QCOMPARE(loop.count(), 1);
        QCOMPARE(loop.at(0).at(0).toBool(), false);

        // a second start with the flag already set is the no-op it must be:
        // re-arming would emit a second request and re-enter enableDocumentView
        core.startSlideshow();
        QCOMPARE(loop.count(), 1);

        core.stopSlideshow();
        QVERIFY(!core.state.slideshow);
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
        QVERIFY(core.slideshowTimer.isActive());

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), files.at(1));
        QVERIFY2(core.slideshowTimer.isActive(), "a static image under slideshow must keep the timer armed");

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), files.at(2));
    }

    void nonLoopingSlideshowStopsAtTheEndOfTheListing() {
        const QStringList files = slideshowFiles("show3");
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.loopSlideshow);

        core.jumpToLast();
        QCOMPARE(core.currentFilePath(), files.at(3));
        // jumpToLast stops the show, so the state below is a fresh arm
        QVERIFY(!core.state.slideshow);

        core.jumpToFirst();
        QCOMPARE(core.currentFilePath(), files.at(0));

        core.startSlideshow();
        core.jumpToLast();
        core.startSlideshow();
        QVERIFY(core.state.slideshow);
        const QString atEnd = core.currentFilePath();

        core.nextImageSlideshow();
        QCOMPARE(core.currentFilePath(), atEnd);
        QVERIFY2(!core.state.slideshow, "a non-looping slideshow must stop itself at the end of the directory");
        QVERIFY(!core.slideshowTimer.isActive());
    }

    void shuffleKeepsTheSlideshowCyclingPastTheEnd() {
        const QStringList files = slideshowFiles("show4");
        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.state.shuffle);

        core.toggleShuffle();
        QVERIFY(core.state.shuffle); // this is what syncRandomizer sized for

        core.startSlideshow();
        core.jumpToLast();
        core.startSlideshow();
        QVERIFY(core.state.slideshow);

        // the sequential branch would have stopped here (pinned above with the
        // same setup); the randomizer branch starts a new cycle instead
        core.nextImageSlideshow();
        QVERIFY2(core.state.slideshow, "shuffle mode stopped at the end of the listing");
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

    void moveCurrentFileRelandsOnDiskAndCopyKeepsTheOriginal() {
        const QString src = uniqueDir("mvsrc");
        const QString copyDst = uniqueDir("mvcopydst");
        const QString moveDst = uniqueDir("mvmovedst");
        const QString file = src + QStringLiteral("/moving.png");
        writeImage(file);

        Core core;
        FakeInteractions fake; // must be installed: see below
        core.ui_ = &fake;
        QVERIFY(core.loadPath(file));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.copyCurrentFile(copyDst);
        QVERIFY2(QFileInfo::exists(copyDst + QStringLiteral("/moving.png")),
                 "copy did not produce the file at the destination");
        QVERIFY2(QFileInfo::exists(file), "copy consumed the original");

        // Separate destinations on purpose. Copying into the same directory the
        // move targets first makes the move a collision, and an un-scripted
        // collision is worse than a failed assert here: Core asks ui_ for the
        // overwrite decision, and the production provider (the main window)
        // blocks on a real modal dialog — the test does not fail, it hangs
        // until the QTest watchdog kills the process at 300 s.
        core.moveCurrentFile(moveDst);
        QTRY_VERIFY2_WITH_TIMEOUT(!QFileInfo::exists(file), "move left the original behind", 10000);
        QVERIFY(QFileInfo::exists(moveDst + QStringLiteral("/moving.png")));
        QCOMPARE(fake.confirmCalls, 0); // no collision on this path
    }

    void declinedCollisionLeavesBothFilesWhereTheyWere() {
        const QString src = uniqueDir("colsrc");
        const QString dst = uniqueDir("coldst");
        const QString file = src + QStringLiteral("/same.png");
        const QString clash = dst + QStringLiteral("/same.png");
        writeImage(file);
        writeImage(clash);
        const QByteArray clashBytes = contentOf(clash);

        Core core;
        FakeInteractions fake;
        fake.confirmAnswer = false; // the overwrite question is answered NO
        core.ui_ = &fake;
        QVERIFY(core.loadPath(file));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.moveCurrentFile(dst);

        QCOMPARE(fake.confirmCalls, 1);
        QVERIFY2(QFileInfo::exists(file), "a declined overwrite still moved the file");
        QVERIFY(QFileInfo::exists(clash));

        core.copyCurrentFile(dst);
        QCOMPARE(fake.confirmCalls, 2);
        // the destination kept its own bytes: the declined copy never wrote
        QCOMPARE(contentOf(clash), clashBytes);
        QVERIFY(QFileInfo::exists(file));
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
        QCOMPARE(core.model->directoryPath(), dirB);

        core.prevDirectory(false);
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(core.currentFilePath(), dirA + QStringLiteral("/one.png"));
        QCOMPARE(core.model->directoryPath(), dirA);

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
        QVERIFY(!core.state.slideshow);

        core.toggleSlideshow();
        QVERIFY(core.state.slideshow);
        QVERIFY(core.slideshowTimer.isActive());

        core.toggleSlideshow();
        QVERIFY2(!core.state.slideshow, "the second toggle did not stop the slideshow");
        QVERIFY(!core.slideshowTimer.isActive());
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
        QTest::qWait(200);
        QCOMPARE(core.currentFilePath(), target);

        // 3. text only: no urls, no image, no action
        QMimeData *textOnly = new QMimeData();
        textOnly->setText(QStringLiteral("just a sentence"));
        QApplication::clipboard()->setMimeData(textOnly);
        core.openFromClipboard();
        QTest::qWait(200);
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
        FakeInteractions fake;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(img), 10000);
        const QByteArray diskBefore = contentOf(img);

        core.flipH();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(fake.confirmCalls, 0);
        QCOMPARE(contentOf(img), diskBefore);

        auto edited = std::dynamic_pointer_cast<ImageStatic>(core.model->getImage(img));
        QVERIFY2(edited && edited->isEdited(), "flipH left no edited image in memory");
        const QImage shown = edited->getPixmap()->toImage();
        QCOMPARE(shown.size(), QSize(16, 8));
        QCOMPARE(shown.pixelColor(0, 0), QColor(Qt::blue)); // mirrored: blue moved left
        QCOMPARE(shown.pixelColor(15, 0), QColor(Qt::red));

        core.flipV();
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(fake.confirmCalls, 0);
        QCOMPARE(contentOf(img), diskBefore);

        // the folder view is where an edit hits the disk, so the question has
        // to come first — and a declined question must mean nothing was written
        core.enableFolderView();
        QCOMPARE(core.mw->currentViewMode(), MODE_FOLDERVIEW);
        fake.confirmAnswer = false;
        core.flipH();
        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(contentOf(img), diskBefore);
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
        core.mw->fitOriginal(); // arms the inspect scope through the real observer
        QTRY_COMPARE_WITH_TIMEOUT(core.mw->viewer()->fitMode(), FIT_ORIGINAL, 5000);

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
        core.mw->fitWindow();
        QTRY_COMPARE_WITH_TIMEOUT(core.mw->viewer()->fitMode(), FIT_WINDOW, 5000);
        QSignalSpy prev(actionManager, &ActionManager::prevImage);
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QCOMPARE(prev.count(), 1);
        QCOMPARE(scroll.count(), 1);

        actionManager->removeAllGridShortcuts();
    }

    // the inspect scope carries user overrides: while armed, an override
    // replaces the inspect default for that key — the armed chain reads the
    // layer, not the seed defaults. The table is snapshotted and restored;
    // the singleton is process-wide.
    void inspectShortcutsCarryUserOverrides() {
        const QMap<QString, QString> inspectSnapshot = actionManager->allInspectShortcuts();
        actionManager->addInspectShortcut(QStringLiteral("Left"), QStringLiteral("nextImage"));

        QSignalSpy next(actionManager, &ActionManager::nextImage);
        QSignalSpy scroll(actionManager, &ActionManager::scrollLeft);
        actionManager->setInspectMode(true);
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QCOMPARE(next.count(), 1);
        QCOMPARE(scroll.count(), 0); // the scrollLeft default never fired
        actionManager->setInspectMode(false);

        actionManager->removeAllInspectShortcuts();
        for(auto it = inspectSnapshot.constBegin(); it != inspectSnapshot.constEnd(); ++it)
            actionManager->addInspectShortcut(it.key(), it.value());
    }

    // ActionManager dispatches by name: an action lives in the Actions registry
    // and is fired as a metaobject method on ActionManager itself. Two lists
    // that have to agree, and the disagreement is silent — invokeMethod on a
    // missing method warns and returns, so the only symptom a user gets is
    // "this button does nothing". Walk every registered name and require a
    // matching method on the manager, which covers all of them at once instead
    // of one button per test.
    void everyRegisteredActionIsInvokableOnActionManager() {
        const QList<QString> names = appActions->getList();
        QVERIFY2(!names.isEmpty(), "the action registry came back empty, the sweep proves nothing");
        // the size is printed even on success: a sweep over an empty or
        // accidentally tiny registry is green for the wrong reason
        qDebug() << "[action sweep] checking" << names.size() << "registered actions against"
                 << actionManager->metaObject()->methodCount() << "methods";

        QStringList missing;
        for(const QString &name : names) {
            const QByteArray signature = (name + QStringLiteral("()")).toLatin1();
            if(actionManager->metaObject()->indexOfMethod(QMetaObject::normalizedSignature(signature.constData())) < 0)
                missing << name;
        }
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("registered with no method to invoke (") + QString::number(names.size()) +
                            QStringLiteral(" actions checked):\n  ") + missing.join(QStringLiteral("\n  "))));
    }

    // invokeAction itself: it had never been entered despite eight production
    // call sites (context menu, action buttons, folder grid, viewer, exit).
    // A registered name has to report success AND reach its signal; an unknown
    // name has to report failure; a "s:" name with no script behind it has to
    // report failure rather than pretend.
    void invokeActionReportsAndReachesForEveryKindOfName() {
        QSignalSpy nextImage(actionManager, &ActionManager::nextImage);
        QSignalSpy runScript(actionManager, &ActionManager::runScript);
        QVERIFY(nextImage.isValid() && runScript.isValid());

        QVERIFY(actionManager->invokeAction(QStringLiteral("nextImage")));
        QCOMPARE(nextImage.count(), 1);

        QVERIFY(!actionManager->invokeAction(QStringLiteral("noSuchActionAnywhere")));
        QCOMPARE(nextImage.count(), 1);
        QCOMPARE(runScript.count(), 0);

        // a script action that does not exist must not claim success
        QVERIFY(!actionManager->invokeAction(QStringLiteral("s:definitely-not-a-script")));
        QCOMPARE(runScript.count(), 0);

        // and one that does exist emits runScript with the prefix stripped
        scriptManager->addScript(QStringLiteral("qtest-ping"), Script(QStringLiteral("/bin/true"), false));
        QVERIFY(scriptManager->scriptExists(QStringLiteral("qtest-ping")));
        QVERIFY(actionManager->invokeAction(QStringLiteral("s:qtest-ping")));
        QCOMPARE(runScript.count(), 1);
        QCOMPARE(runScript.at(0).at(0).toString(), QStringLiteral("qtest-ping"));
        scriptManager->removeScript(QStringLiteral("qtest-ping"));
    }

    // The other half of the conditional-save contract. With the folder view
    // asking and the user saying yes, the edit has to land on disk: neither the
    // document-mode run nor the declined run reaches the file loop, so this is
    // the only path where a folder-view edit rewrites the user's file.
    void folderViewEditWithConsentRewritesTheFile() {
        const QString dir = uniqueDir("consent");
        const QString img = dir + QStringLiteral("/half.png");
        writeRectImage(img);
        const QByteArray diskBefore = contentOf(img);

        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.folderViewPresenter.selectAndFocus(img);
        QTRY_VERIFY2_WITH_TIMEOUT(!core.currentSelection().isEmpty(),
                                  "precondition broken: the folder view holds no selection", 10000);

        fake.confirmAnswer = true;
        core.flipH();

        QCOMPARE(fake.confirmCalls, 1);
        QTRY_VERIFY2_WITH_TIMEOUT(contentOf(img) != diskBefore, "a consented folder-view edit never reached the disk",
                                  10000);
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

    // Every overlay proxy creates its overlay as a child of the container and
    // used to delete that child from its own destructor. A proxy destroyed after
    // its container therefore read a handle whose object ~QWidget had already
    // freed. Measured, and the two halves separate cleanly: with the raw handle
    // plus deleteLater restored on FloatingMessageProxy, ASan names
    // floatingmessageproxy.cpp:13; with the handle as a QPointer the same
    // deleteLater is harmless because the pointer nulls itself when the overlay
    // dies. So the QPointer is what closes the use-after-free, and dropping the
    // deleteLater restores the ownership rule - the tree owns widgets, a proxy
    // only observes. Invariant deliberately left as-is: init() hands the raw
    // container to the new overlay, so a proxy must not be *used* after its
    // container is gone; nothing in the app does that, and this test exercises
    // destruction only, not that assumption.
    void proxyDestroyedAfterItsContainerDoesNotTouchTheOverlay() {
        const int baseline = QApplication::allWidgets().size();
        proxyOutlivesItsContainer<ZoomIndicatorOverlayProxy>();
        proxyOutlivesItsContainer<ImageInfoOverlayProxy>();
        proxyOutlivesItsContainer<FullscreenInfoOverlayProxy>();
        proxyOutlivesItsContainer<FloatingMessageProxy>();
        proxyOutlivesItsContainer<VideoControlsProxyWrapper>();
        QTest::qWait(100); // any deferred deletion must have landed
        // nothing outlives the container it was made in: the tree reclaimed all
        // five overlays with their containers, and no proxy stashed a widget
        QCOMPARE(QApplication::allWidgets().size(), baseline);
    }

    // The presenter outliving its view. The teardown checks a QPointer first,
    // and the honest accounting of that guard: ASan did NOT flag the unguarded
    // version on this path (removing it changed nothing here), because Qt has
    // already dropped the connections when the sender dies. The guard is
    // contract-based rather than a crash fix - handing a freed sender to
    // disconnect() is undefined by Qt's own rules even when one build happens
    // not to read it - so this test pins the supported order, not a detected
    // use-after-free. What is measured here is that rebinding afterwards still
    // delivers.
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
        auto model = std::make_shared<DirectoryModel>(pool);
        QVERIFY(model->setDirectory(alpha));
        QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 1, 10000);
        presenter.setModel(model);
        QSignalSpy activated(&presenter, &DirectoryPresenter::fileActivated);
        QVERIFY(activated.isValid());
        QMetaObject::invokeMethod(&second, "itemActivated", Q_ARG(int, 0));
        QCOMPARE(activated.count(), 1);
    }

    // What a widget destructor forgets cannot be seen by the app: the tree is
    // never torn down, so a leaked child stays reachable from Qt's own widget
    // registry and there is nothing for LSan to call unreachable. Destroying each
    // suspect here is the measurement — the widget class names come from
    // allWidgets(), and the same scopes are what the sanitizer tree uses to size
    // the non-widget heap (a scene, a timeline, a pixmap: not widgets, so not
    // visible to the assertion below and only visible to LSan once unreachable).
    void destroyingAWidgetFreesEveryWidgetItCreated() {
        const QMap<QString, int> before = widgetClasses();
        const int beforeTotal = QApplication::allWidgets().size();
        {
            ImageViewerV2 viewer;
            FolderView folderView;
            FloatingWidgetContainer container;
            ClickZoneOverlay zones(&container);
            ZoomIndicatorOverlay zoom(&container);
            MainPanel panel(&container);
            IconWidget icon;
            // the probe only means something if these really are live widgets
            QVERIFY(QApplication::allWidgets().size() > beforeTotal);
        }
        const QMap<QString, int> after = widgetClasses();
        QStringList surplus;
        for(auto it = after.constBegin(); it != after.constEnd(); ++it) {
            const int left = it.value() - before.value(it.key(), 0);
            if(left > 0)
                surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
        }
        QVERIFY2(surplus.isEmpty(),
                 qPrintable(QStringLiteral("widgets outliving their parent: ") + surplus.join(", ")));
    }

    // The popup is now owned by the view that shows it, which is what frees it when
    // the view dies. That is only a win if it still behaves like a popup: a free-
    // standing window landing where the button told it, not an embedded child.
    void parentedOptionsPopupStillFloatsWhereItIsTold() {
        QWidget owner;
        owner.resize(200, 100);
        owner.show();
        FVOptionsPopup popup(&owner);
        const QPoint where(120, 90);
        popup.showAt(where);
        QVERIFY2(popup.isWindow(), "parenting turned the popup into an embedded child widget");
        QVERIFY2(popup.windowFlags().testFlag(Qt::Popup), "the popup lost its Popup window flag");
        QVERIFY(popup.isVisible());
        QCOMPARE(popup.pos(), where);
        popup.hide();
        owner.hide();
    }

    // The broad form of this instrument, unlocked by the ownership unification:
    // build a real Core — the full window tree the app never used to tear down —
    // and destroy it, then require the widget census to return exactly to the
    // pre-existing baseline. Until every widget shared_ptr became an observer,
    // this crashed at the first double-free, exactly as the comment it replaces
    // predicted; that prediction is what this test now pins.
    void destroyingTheCoreFreesTheWholeWindowTree() {
        const QMap<QString, int> before = widgetClasses();
        const int beforeTotal = QApplication::allWidgets().size();
        {
            Core core;
            // the probe only means something if the window tree is really live
            QVERIFY(QApplication::allWidgets().size() > beforeTotal);
        }
        const QMap<QString, int> after = widgetClasses();
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

    void interactiveCopyPlacesFileWithoutDialogs() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString src = makeTxt(base, "f1.txt", "one");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));

        core.interactiveCopy({src}, destDir);

        QCOMPARE(fake.replaceCalls, 0); // no collision, nothing to ask
        QCOMPARE(contentOf(QDir(destDir).filePath("f1.txt")), QByteArray("one"));
        QVERIFY(QFileInfo::exists(src)); // a copy leaves the source alone
    }

    void interactiveCopyOverwritesWhenScriptedYes() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString src = makeTxt(base, "f1.txt", "new");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "f1.txt", "old");
        fake.replaceAnswers << DialogResult{true, false, false};

        core.interactiveCopy({src}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(fake.replaceArgs.at(0).first, src); // asked about THIS file
        QCOMPARE(contentOf(QDir(destDir).filePath("f1.txt")), QByteArray("new"));
    }

    void interactiveCopyYesToAllSticksAcrossTheBatch() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{true, true, false}; // one answer, whole batch

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1); // the sticky answer suppresses the rest
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
    }

    void interactiveCopyNoToAllSkipsTheRestWithoutAsking() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", "new");
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{false, true, false}; // no + apply to all

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1); // asked once for the first collision only
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray("old"));
    }

    void interactiveCopyPlainYesAsksAgainForEachCollision() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        for(int i = 0; i < 3; ++i)
            fake.replaceAnswers << DialogResult{true, false, false}; // plain yes, no all

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 3); // the yes resets: one question per file
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
    }

    void interactiveCopyCancelStopsTheBatch() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{false, false, true}; // cancel

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1); // the batch unwound at the first collision
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray("old"));
    }

    void interactiveCopyOverwritesADirectoryCollisionWhenScriptedYes() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString srcDir = QDir(base).filePath("sdir");
        QVERIFY(QDir().mkpath(srcDir));
        makeTxt(srcDir, "inner.txt", "inner");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "sdir", "old file");                    // a FILE where the directory lands
        fake.replaceAnswers << DialogResult{true, false, false}; // DIR_TO_FILE: yes

        core.interactiveCopy({srcDir}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(contentOf(QDir(destDir).filePath("sdir/inner.txt")), QByteArray("inner"));
    }

    void interactiveCopyDeclinedDirectoryOverwriteKeepsTheFile() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString srcDir = QDir(base).filePath("sdir");
        QVERIFY(QDir().mkpath(srcDir));
        makeTxt(srcDir, "inner.txt", "inner");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "sdir", "old file");
        fake.replaceAnswers << DialogResult{false, false, false}; // no

        core.interactiveCopy({srcDir}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(contentOf(QDir(destDir).filePath("sdir")), QByteArray("old file")); // intact
        QVERIFY(!QFileInfo::exists(QDir(destDir).filePath("sdir/inner.txt")));
    }

    void interactiveMoveOverwritesADirectoryCollisionWhenScriptedYes() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString srcDir = QDir(base).filePath("sdir");
        QVERIFY(QDir().mkpath(srcDir));
        makeTxt(srcDir, "inner.txt", "inner");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "sdir", "old file");
        fake.replaceAnswers << DialogResult{true, false, false}; // DIR_TO_FILE: yes

        core.movePathsTo({srcDir}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(contentOf(QDir(destDir).filePath("sdir/inner.txt")), QByteArray("inner"));
        QVERIFY2(!QFileInfo::exists(QDir(base).filePath("sdir/inner.txt")),
                 "a move that overwrote a file with a directory left the source behind");
    }

    void interactiveMoveYesToAllSticksAcrossTheBatch() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"m1", "m2", "m3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{true, true, false};

        core.movePathsTo(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        for(const char *n : {"m1", "m2", "m3"}) {
            QCOMPARE(contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
            QVERIFY2(!QFileInfo::exists(QDir(base).filePath(QString(n) + ".txt")),
                     qPrintable(QString(n) + " survived a move")); // a move removes the source
        }
    }

    // upstream issue #612: dragging a directory onto another one corrupted
    // every dotted directory name ("1. What" -> "1") — the dir move built
    // its destination from baseName() — and let a directory be dropped into
    // its own subtree, recursing until the reporter's 5000 entries shredded.
    void directoryMoveKeepsDottedNamesAndRefusesSelfMoves() {
        QString root = QDir(home.path()).filePath("issue612");
        QDir().mkpath(root + "/thetop/1. What/3. This is weird/X. Okay whatever");
        QDir().mkpath(root + "/thetop/1. What/5. Another one/Normal name works");
        QDir().mkpath(root + "/thetop/Another normal name/This works");
        makeTxt(root, "thetop/Another normal name/5. This is a file with a prefix but no extension", "x");
        makeTxt(root, "thetop/Another normal name/6. This is a file with a prefix and extension.png", "x");
        QVERIFY(QDir().mkpath(root + "/otherdir"));

        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        core.interactiveMove({QDir(root).filePath("thetop")}, QDir(root).filePath("otherdir"));

        // dotted directory names survive the move whole
        QVERIFY(QDir(root + "/otherdir/thetop/1. What/3. This is weird/X. Okay whatever").exists());
        QVERIFY(QDir(root + "/otherdir/thetop/1. What/5. Another one/Normal name works").exists());
        QVERIFY(QFileInfo::exists(
            root + "/otherdir/thetop/Another normal name/5. This is a file with a prefix but no extension"));
        QVERIFY(!QDir(root + "/otherdir/thetop/1").exists()); // the corruption signature
        QVERIFY(!QFileInfo::exists(root + "/thetop"));        // moved, not copied

        // a directory dropped into its own subtree is refused flat
        core.interactiveMove({root + "/otherdir/thetop"}, root + "/otherdir/thetop/1. What");
        QVERIFY(QDir(root + "/otherdir/thetop/1. What/3. This is weird").exists());
        QVERIFY(!QDir(root + "/otherdir/thetop/1. What/thetop").exists());
        QVERIFY(!QDir(root + "/otherdir/thetop/1. What/1").exists());
    }

    // upstream issue #505: a preload failing for a file the user is not
    // viewing used to surface "Load failed" over the image on screen. Only
    // the on-screen file may announce its own failure.
    void loadFailureOfUnviewedFileStaysSilent() {
        writeImage(QDir(base).filePath("keep.png"));
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(QDir(base).filePath("keep.png")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("keep.png"), 10000);
        int before = fake.messageCalls;

        // a failure for a file the user never opened stays silent
        emit core.model->loadFailed(QDir(base).filePath("never-seen.png"));
        QCOMPARE(fake.messageCalls, before);

        // the on-screen file's own failure announces itself and closes the view
        emit core.model->loadFailed(QDir(base).filePath("keep.png"));
        QCOMPARE(fake.messageCalls, before + 1);
        QVERIFY(fake.lastMessage.contains(QStringLiteral("Load failed")));
    }

    void removePermanentDeletesAfterScriptedConfirmation() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const int before = core.model->fileCount();

        core.removePermanent();

        QTRY_COMPARE_WITH_TIMEOUT(core.model->fileCount(), before - 1, 10000);
        QVERIFY2(!QFileInfo::exists(QDir(alpha).filePath("a.png")), "the confirmed removal never reached the disk");
        QCOMPARE(fake.confirmCalls, 1);
    }

    void moveToTrashKeepsFilesWhenConfirmationDeclined() {
        Core core;
        FakeInteractions fake;
        fake.confirmAnswer = false;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const int before = core.model->fileCount();

        core.moveToTrash();

        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(core.model->fileCount(), before);
        QVERIFY(QFileInfo::exists(QDir(alpha).filePath("a.png")));
    }

    // #467's silent half: a batch reported only its successes. Qt's moveToTrash
    // refuses a name whose trash target (files/ prefix + name) exceeds
    // NAME_MAX, and that refusal used to vanish — "Moved to trash: 1 files"
    // for a two-file selection. The report must carry both numbers, and each
    // refusal also leaves its path and decoded reason in the session log. The
    // exact split is a property of the trash topology (a mount with no trash
    // at all fails every member), so the pin is the format, not the counts.
    void batchTrashReportsHowManyFailed() {
        // 250 'l' + ".png" fits the source directory (254 <= NAME_MAX) but
        // cannot fit any real trash: even HOME=/ gives a 26-char files/ prefix.
        const QString longName = QString(250, QLatin1Char('l')) + QStringLiteral(".png");
        const QString longFile = QDir(alpha).filePath(longName);
        writeImage(longFile);

        Core core;
        FakeInteractions fake;
        fake.confirmAnswer = true;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.mw->getFolderView()->select(QList<int>{0, 1});
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 2,
                                  "precondition broken: the folder view holds no two-file selection", 10000);

        core.moveToTrash();

        QVERIFY2(QFileInfo::exists(longFile), "the refused file vanished anyway");
        auto *floating = core.mw->findChild<FloatingMessage *>();
        QVERIFY2(floating, "no floating message widget exists after a batch removal");
        auto *label = floating->findChild<QLabel *>("textLabel");
        QVERIFY2(label, "the floating message carries no text label");
        static const QRegularExpression bothHalves(QStringLiteral("\\d+ files, \\d+ failed"));
        QVERIFY2(bothHalves.match(label->text()).hasMatch(),
                 qPrintable(QStringLiteral("batch report lost a half: \"") + label->text() + "\""));
    }

    // The other half of the same report: a batch where nothing failed keeps
    // the success styling and the bare count — the repair must not turn every
    // batch into an error. Needs a working trash, so it skips where the
    // fileops suite's probe skips too.
    void batchTrashAllSuccessStillReportsTheCount() {
        const QString dir = uniqueDir("trashok");
        const QString probe = QDir(dir).filePath("trash-probe.txt");
        QVERIFY(QFile(probe).open(QIODevice::WriteOnly));
        FileOpResult probeResult;
        FileOperations::moveToTrash(probe, probeResult);
        if(probeResult != FileOpResult::SUCCESS)
            QSKIP(
                "moveToTrash unavailable on this filesystem topology (no writable top-level trash for the fixture mount)");

        writeImage(QDir(dir).filePath("a.png"));
        writeImage(QDir(dir).filePath("b.png"));

        Core core;
        FakeInteractions fake;
        fake.confirmAnswer = true;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(QDir(dir).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.mw->getFolderView()->select(QList<int>{0, 1});
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 2,
                                  "precondition broken: the folder view holds no two-file selection", 10000);

        core.moveToTrash();

        QVERIFY(!QFileInfo::exists(QDir(dir).filePath("a.png")));
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath("b.png")));
        auto *floating = core.mw->findChild<FloatingMessage *>();
        QVERIFY2(floating, "no floating message widget exists after a batch removal");
        auto *label = floating->findChild<QLabel *>("textLabel");
        QVERIFY2(label, "the floating message carries no text label");
        QVERIFY2(label->text().contains(QStringLiteral("2 files")) && !label->text().contains(QStringLiteral("failed")),
                 qPrintable(QStringLiteral("all-success batch reported: \"") + label->text() + "\""));
    }

    // a menuitem icon can exist on disk and still render blank: bf65eac2 added
    // the wallpaper icon files and the setIconPath call but forgot the
    // resources.qrc entry, so the resource did not exist at runtime. Pin the
    // compiled resource, not the file on disk.
    void wallpaperMenuIconIsCompiledIntoResources() {
        QVERIFY(QFile::exists(":/res/icons/common/menuitem/wallpaper16.png"));
        QVERIFY(QFile::exists(":/res/icons/common/menuitem/wallpaper16@2x.png"));
    }

    // The teardown census extended over the classes the first sweep never
    // covered — constructed, POPULATED, shown, and destroyed. The folder-grid
    // SEGV lived exactly in the populated+destroyed state, which is why the
    // construct-only sweep missed it. Each class here carries value-member
    // widgets parented through its own layout: the pattern whose teardown had
    // never once run. (SSideBar — used by SettingsDialog's .ui as sideBar2 —
    // was renamed SettingsSidebar after this census initially shipped with a
    // "dead code" note that was a case-sensitive grep miss; the rename also
    // buries the case trap that produced it.)
    void populatedCensusCoversTheRemainingWidgetClasses() {
        const QMap<QString, int> before = widgetClasses();
        const int beforeTotal = QApplication::allWidgets().size();
        {
            SettingsSidebarItem sidebarItem("icon", "label");
            SettingsSidebar sidebar;
            EntryInfoItem info;
            FolderView fv;
            fv.populate(1); // the populated-grid state that killed the folder grid
            sidebar.show();
            info.show();
            fv.show();
            QVERIFY(QApplication::allWidgets().size() > beforeTotal);
        }
        const QMap<QString, int> after = widgetClasses();
        QStringList surplus;
        for(auto it = after.constBegin(); it != after.constEnd(); ++it) {
            const int left = it.value() - before.value(it.key(), 0);
            if(left > 0)
                surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
        }
        QVERIFY2(surplus.isEmpty(), qPrintable(QStringLiteral("widgets outliving their owner: ") + surplus.join(", ")));
    }

    // Dragging an entry whose full-size image was never DECODED: every listed
    // entry has an Image object, but an undecoded one carries no pixels — the
    // mime must still offer the FILE (its path), because the drag is the file
    // reference, not the pixels. mimeDataForPaths guarantees the path even for
    // an entry whose Image object was never decoded (and the fallback also
    // covers the defensive case of getImage returning null).
    void draggingAnUndecodedEntryStillOffersTheFile() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // far.png sits beyond the preload neighborhood: a.png preloads its
        // immediate neighbor (mid.png), never the far one — undecoded
        writeImage(QDir(alpha).filePath("mid.png"));
        const QString far = QDir(alpha).filePath("far.png");
        writeImage(far);
        QVERIFY2(!core.model->isLoaded(far),
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

    // A connect() with no context object binds the connection to the SENDER.
    // When the sender is the app-lifetime `settings` global and the lambda
    // captures `this`, the connection outlives the widget and every later
    // settingsChanged() runs against a dead object — production keeps these
    // widgets alive for the whole session, so the only way to reach the window
    // is to destroy one here and emit into it. That makes this test the probe:
    // in the sanitizer tree a context-free connect is a heap-use-after-free
    // naming the lambda, and in any tree the emission still has to reach its
    // legitimate receivers.
    void connectsToTheAppLifetimeSenderDieWithTheWidget() {
        QSignalSpy survivor(settings, &Settings::settingsChanged);

        {
            StyledComboBox box;
            FileSystemModelCustom model;
            FolderGridView grid;
        }

        emit settings->settingsChanged();

        QCOMPARE(survivor.count(), 1);
    }

    void renameCurrentSelectionForcesAfterScriptedConfirmation() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // the collision the rename will hit: renamed.png already exists
        const QString colliding = QDir(alpha).filePath("renamed.png");
        writeImage(colliding);
        const QByteArray original = contentOf(QDir(alpha).filePath("a.png"));

        core.renameCurrentSelection("renamed.png");

        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QDir(alpha).filePath("a.png")), 10000);
        QVERIFY(QFileInfo::exists(colliding));
        // a forced rename moves the bytes, so the colliding name now holds
        // exactly what the old name held
        QCOMPARE(contentOf(colliding), original);
        QCOMPARE(fake.confirmCalls, 1); // the overwrite question was asked and scripted yes
    }

    // A rename is one event: the engine pairs the kernel's MOVED_FROM and MOVED_TO
    // by cookie and answers with RENAMED(old -> new), so the mirror moves a row
    // instead of dropping one and growing another (DirectoryManager::applyEngineRename,
    // with the wire half pinned by listing_tests' liveRenameIsOneEvent). Following
    // the renamed file is still the app's job, and the two ways a file gets renamed
    // are two separate pins: this one is the rename the app issued, where a call
    // site knows the destination, and the next is one done behind its back.
    void renamingTheCurrentFileConvergesTheListingAndLandsOnTheNewName() {
        const QString dir = uniqueDir("renamelanding");
        const QString from = QDir(dir).filePath("aardvark.png");
        const QString to = QDir(dir).filePath("renamed.png");
        writeImage(from);
        writeImage(QDir(dir).filePath("zzz.png"));
        Core core;
        QVERIFY(core.loadPath(from));
        QTRY_VERIFY_WITH_TIMEOUT(core.model->containsFile(from), 10000);
        QCOMPARE(core.model->fileCount(), 2);

        core.renameCurrentSelection("renamed.png");

        // isSettled() cannot wait for this: it asks about pending model work,
        // not about whether the watch events arrived, and it is already true
        // the moment the rename returns. Wait on the facts themselves.
        QTRY_VERIFY_WITH_TIMEOUT(core.model->containsFile(to), 10000);
        QVERIFY(!core.model->containsFile(from));
        QCOMPARE(core.model->fileCount(), 2); // lossless: nothing dropped
        QTRY_VERIFY_WITH_TIMEOUT(core.currentFilePath() == to, 10000);
        QVERIFY(core.model->isLoaded(to));    // landed on it, not merely listed
        QVERIFY(!core.model->isLoaded(from)); // and the old name is gone from memory
    }

    // The other half of 跟手: a rename this process never issued, done by another
    // program while the view sits on the file. No call site knows the
    // destination, so only the event can carry the view across — which is what
    // Core::onFileRenamed is for, and the reason the old pendingRenameTarget
    // arm in onFileAdded is gone: an arm that had to be armed by the code
    // performing the rename could never fire for a rename it did not perform.
    // The name change also crosses a sort boundary (aardvark -> zebra under name
    // sort), so the row moves and the view still has to land on it.
    void externallyRenamingTheCurrentFileKeepsItTheCurrentFile() {
        const QString dir = uniqueDir("extrenamelanding");
        const QString from = QDir(dir).filePath("aardvark.png");
        const QString to = QDir(dir).filePath("zebra.png");
        writeImage(from);
        writeImage(QDir(dir).filePath("zzz.png"));
        Core core;
        QVERIFY(core.loadPath(from));
        QTRY_VERIFY_WITH_TIMEOUT(core.model->containsFile(from), 10000);
        QCOMPARE(core.currentFilePath(), from);

        QVERIFY(std::rename(from.toLocal8Bit().constData(), to.toLocal8Bit().constData()) == 0);

        QTRY_VERIFY_WITH_TIMEOUT(core.currentFilePath() == to, 10000);
        QVERIFY(core.model->isLoaded(to)); // on it, not merely pointing at it
        QVERIFY(!core.model->isLoaded(from));
        QCOMPARE(core.model->fileCount(), 2); // lossless, as under an issued rename
    }

    // ---- DirectoryPresenter lifecycle: the view is an injected observer, the
    // model connections are per-model, and disconnectView (until now a todo)
    // is what allows a presenter to outlive one view and bind another.

    void presenterPopulatesTheViewWhenModelArrives() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool);
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
        auto model = std::make_shared<DirectoryModel>(pool);
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
        auto model = std::make_shared<DirectoryModel>(pool);
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
        auto model = std::make_shared<DirectoryModel>(pool);
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
        auto model = std::make_shared<DirectoryModel>(pool);
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
        auto model2 = std::make_shared<DirectoryModel>(pool);
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
        auto model = std::make_shared<DirectoryModel>(pool);
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
        auto model = std::make_shared<DirectoryModel>(pool);
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
        auto model = std::make_shared<DirectoryModel>(pool);
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
        const QList<ThumbnailWidget *> before = orderedThumbnails(grid);
        QVERIFY2(before.count() == 25, qPrintable(QStringLiteral("populate(25) laid out %1 rows").arg(before.count())));
        grid.select(1);
        ThumbnailWidget *const moving = before.at(1);

        grid.renameItem(1, 4, QStringLiteral("moved.png"));

        const QList<ThumbnailWidget *> after = orderedThumbnails(grid);
        QCOMPARE(after.count(), 25);
        QVERIFY2(after.at(4) == moving, "the moved row is a different widget — renaming does not recreate the row");
        QVERIFY2(after.at(1) == before.at(2) && after.at(3) == before.at(4),
                 "the rows the move jumped over did not slide into the space it left");
        QCOMPARE(grid.selection(), QList<int>({4}));

        // and back again: the same rows slide the other way
        grid.renameItem(4, 0, QStringLiteral("front.png"));
        const QList<ThumbnailWidget *> back = orderedThumbnails(grid);
        QVERIFY2(back.at(0) == moving, "the row did not move back to the front");
        QVERIFY2(back.at(1) == before.at(0) && back.at(4) == before.at(4),
                 "the rows it passed going back did not shift down");
        QCOMPARE(grid.selection(), QList<int>({0}));

        // an in-place rename — the name-sort case where the row does not move —
        // touches neither the layout nor the selection
        grid.renameItem(0, 0, QStringLiteral("still.png"));
        QVERIFY2(orderedThumbnails(grid).at(0) == moving, "an in-place rename moved a row");
        QCOMPARE(grid.selection(), QList<int>({0}));

        // out-of-range on either side is refused rather than clamped: the caller
        // is a live event about a row the view may already have dropped
        grid.renameItem(0, 99, QStringLiteral("nowhere.png"));
        QCOMPARE(orderedThumbnails(grid).count(), 25);
        QVERIFY2(orderedThumbnails(grid).at(0) == moving, "an out-of-range rename moved a row");

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

    // The same arithmetic exists in a second place, for the second owner of a
    // selection: a strip proxy that has never been shown holds no rows at all,
    // only a buffered selection that init() replays. Driven end to end — the
    // rename happens on disk, so the engine pairs it, the mirror moves its row,
    // the presenter offsets and looks up, and the buffer is what is left to move.
    void viewlessProxyRemapsItsBufferedSelectionOnRename() {
        WorkerPool pool(2);
        auto model = std::make_shared<DirectoryModel>(pool);
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
        core.pool_.submit(PoolOwner::Loader, 0, std::make_unique<DeadlineJob>(&gate, &holds));
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
        auto *strip = core.mw->findChild<ThumbnailStripProxy *>();
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
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(img), 10000);
        const QByteArray diskBefore = contentOf(img);

        core.rotateLeft();

        QTRY_COMPARE_WITH_TIMEOUT(core.model->getImage(img)->size(), QSize(8, 16), 10000);
        QCOMPARE(fake.confirmCalls, 0);       // document mode: in memory, no save, no ask
        QCOMPARE(contentOf(img), diskBefore); // the disk file is untouched until save
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
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(img), 10000);
        core.rotateLeft();
        auto image = core.model->getImage(img);
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
        FakeInteractions fake;
        core.ui_ = &fake;
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
        FakeInteractions fake;
        fake.confirmAnswer = false;
        core.ui_ = &fake;
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
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.resize(QSize(4, 4));

        QTRY_COMPARE_WITH_TIMEOUT(core.model->getImage(img)->size(), QSize(4, 4), 10000);
        QCOMPARE(fake.confirmCalls, 0); // resize never asks
    }

    void cropAndSaveWritesTheCroppedFile() {
        Core core;
        FakeInteractions fake;
        core.ui_ = &fake;
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
        FakeInteractions fake;
        core.ui_ = &fake;
        const QString img = QDir(alpha).filePath("rect.png");
        writeRectImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.rotateLeft();
        QTRY_COMPARE_WITH_TIMEOUT(core.model->getImage(img)->size(), QSize(8, 16), 10000);

        core.discardEdits();

        QCOMPARE(core.model->getImage(img)->size(), QSize(16, 8)); // back to the disk state
    }

    // ---- the drag-drop surface. The handlers are invoked DIRECTLY through a
    // using-exposed subclass: their contracts (enter accepts, move hit-tests
    // and emits draggedOver(index), drop emits droppedInto(mimeData, source,
    // landing index)) are what we pin, and QApplication::notify's drag-protocol
    // gating (a synthetic Drop without an accepted DragMove in the drag
    // manager's state is refused delivery) isQt plumbing this arc must not
    // depend on. Positions are computed from the live scene (item scenePos
    // mapped to viewport coords), not hardcoded.

    void dragEnterAcceptsUnconditionally() {
        TestableFolderGridView grid;
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
        TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QTest::qWait(50); // let the flow layout position the two cells
        QSignalSpy spy(&grid, &FolderGridView::draggedOver);
        QVERIFY(spy.isValid());

        const QPoint pos0 = dropPosOverThumbnail(grid, 0);
        const QPoint pos1 = dropPosOverThumbnail(grid, 1);
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

    void dropReportsMimeSourceAndLandingIndex() {
        TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QTest::qWait(50);
        QSignalSpy spy(&grid, &FolderGridView::droppedInto);
        QVERIFY(spy.isValid());

        auto mime = std::make_unique<QMimeData>();
        const QList<QUrl> urls{QUrl::fromLocalFile("/tmp/dropped1.png"), QUrl::fromLocalFile("/tmp/dropped2.png")};
        mime->setUrls(urls);

        const QPoint pos1 = dropPosOverThumbnail(grid, 1);
        QDropEvent drop(QPointF(pos1), Qt::CopyAction, mime.get(), Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
        grid.dropEvent(&drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<const QMimeData *>(), mime.get()); // payload passes through
        QCOMPARE(spy.at(0).at(1).value<QObject *>(), nullptr);            // no source (external drop)
        QCOMPARE(spy.at(0).at(2).toInt(), 1);                             // landed on thumbnail #1
        QCOMPARE(spy.at(0).at(0).value<const QMimeData *>()->urls(), urls);
    }

    void dropOutsideAnyThumbnailReportsMinusOne() {
        TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QTest::qWait(50);
        QSignalSpy spy(&grid, &FolderGridView::droppedInto);
        QVERIFY(spy.isValid());

        auto mime = std::make_unique<QMimeData>();
        mime->setUrls({QUrl::fromLocalFile("/tmp/x.png")});
        QDropEvent drop(QPointF(1, 1), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        grid.dropEvent(&drop);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(2).toInt(), -1); // the consumer decides: nothing to highlight
    }

    void dragReportingSurvivesALeave() {
        TestableFolderGridView grid;
        grid.resize(300, 300);
        grid.populate(2);
        grid.show();
        QTest::qWait(50);
        QSignalSpy spy(&grid, &FolderGridView::draggedOver);
        QVERIFY(spy.isValid());

        const QPoint pos0 = dropPosOverThumbnail(grid, 0);
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
        TestableFolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        grid.select(0);
        connect(actionManager, &ActionManager::gridSelectNext, &grid, &FolderGridView::selectNext);
        connect(actionManager, &ActionManager::gridSelectPrev, &grid, &FolderGridView::selectPrev);
        connect(actionManager, &ActionManager::gridSelectAbove, &grid, &FolderGridView::selectAbove);
        connect(actionManager, &ActionManager::gridSelectBelow, &grid, &FolderGridView::selectBelow);
        connect(actionManager, &ActionManager::gridPageUp, &grid, &FolderGridView::pageUp);
        connect(actionManager, &ActionManager::gridPageDown, &grid, &FolderGridView::pageDown);
        connect(actionManager, &ActionManager::gridSelectFirst, &grid, &FolderGridView::selectFirst);
        connect(actionManager, &ActionManager::gridSelectLast, &grid, &FolderGridView::selectLast);
        connect(actionManager, &ActionManager::gridSelectAll, &grid, &FolderGridView::selectAll);
        connect(actionManager, &ActionManager::gridOpen, &grid, &FolderGridView::activateCurrent);

        pressGridKey(grid, "Right", Qt::Key_Right);
        QCOMPARE(lastSelection(grid), 1);
        pressGridKey(grid, "Left", Qt::Key_Left);
        QCOMPARE(lastSelection(grid), 0);
        pressGridKey(grid, "Left", Qt::Key_Left);
        QCOMPARE(lastSelection(grid), 0); // past the start: stays
        pressGridKey(grid, "Down", Qt::Key_Down);
        pressGridKey(grid, "Up", Qt::Key_Up);
        QCOMPARE(lastSelection(grid), 0);

        pressGridKey(grid, "End", Qt::Key_End);
        QCOMPARE(lastSelection(grid), 24);
        pressGridKey(grid, "Home", Qt::Key_Home);
        QCOMPARE(lastSelection(grid), 0);
        pressGridKey(grid, "PgDown", Qt::Key_PageDown);
        const int paged = lastSelection(grid);
        QVERIFY2(paged > 0, "PgDown never left the first row");
        pressGridKey(grid, "PgUp", Qt::Key_PageUp);
        QCOMPARE(lastSelection(grid), 0); // and back

        pressGridKey(grid, "A", Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(grid.selection().count(), 25); // Ctrl+A: everything

        QSignalSpy up(actionManager, &ActionManager::goUp);
        pressGridKey(grid, "Backspace", Qt::Key_Backspace);
        QCOMPARE(up.count(), 1); // goUp is the grid scope's own binding

        QSignalSpy activated(&grid, &FolderGridView::itemActivated);
        pressGridKey(grid, "Enter", Qt::Key_Return);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.first().first().toInt(), 0);

        grid.select(5);
        pressGridKey(grid, "Esc", Qt::Key_Escape);
        QCOMPARE(lastSelection(grid), 5); // an unscoped key never moves the grid

        // activation on an empty selection is guarded, not emitted blind
        TestableFolderGridView emptyGrid;
        emptyGrid.resize(300, 300);
        emptyGrid.populate(2);
        emptyGrid.show();
        QSignalSpy emptyActivation(&emptyGrid, &FolderGridView::itemActivated);
        pressGridKey(emptyGrid, "Enter", Qt::Key_Return);
        QCOMPARE(emptyActivation.count(), 0);
    }

    // The grid scope actions drive the grid through the FULL chain —
    // ActionManager signal, core.cpp connect, FolderViewProxy, FolderView,
    // grid — because R12 guards exactly this wiring, and a broken link is
    // silent: the action validates, invokes, and does nothing. Movement and
    // activation are both pinned; the key events land on this in the cutover.
    void gridActionsDriveTheGridThroughTheFullChain() {
        const QString dir = uniqueDir("gridact");
        writeImage(QDir(dir).filePath("a.png"));
        writeImage(QDir(dir).filePath("b.png"));

        Core core;
        QVERIFY(core.loadPath(QDir(dir).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.enableFolderView();
        core.folderViewPresenter.selectAndFocus(0);
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 1,
                                  "precondition broken: the folder view holds no selection", 10000);

        QVERIFY(actionManager->invokeAction(QStringLiteral("gridSelectNext")));
        QCOMPARE(core.currentSelection().count(), 1);
        QCOMPARE(core.currentSelection().constFirst(), QDir(dir).filePath("b.png"));

        QVERIFY(actionManager->invokeAction(QStringLiteral("gridOpen")));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(dir).filePath("b.png"), 10000);
    }

    // The keymap page renders as scenarios: one section per scope under a
    // header row, and a row saves into the table its section stands for.
    // The render half is pinned through readSettings (a public slot); the
    // save half through saveShortcuts (public on the same seam) — an
    // injected grid row must land in the grid table and nowhere else, and
    // the cycle must otherwise be an identity.
    void shortcutDialogSectionsRenderAndSaveApart() {
        const QMap<QString, QString> gridSnapshot = actionManager->allGridShortcuts();
        const int globalBefore = actionManager->allShortcuts().size();
        const int gridBefore = gridSnapshot.size();
        const int inspectBefore = actionManager->allInspectShortcuts().size();

        SettingsDialog dialog;
        dialog.readSettings();
        auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("shortcutsTableWidget"));
        QVERIFY2(table, "the shortcuts table did not materialize");
        int headers = 0, global = 0, grid = 0, inspect = 0;
        for(int r = 0; r < table->rowCount(); r++) {
            QTableWidgetItem *item = table->item(r, 0);
            const QString tag = item ? item->data(Qt::UserRole).toString() : QString();
            if(tag.isEmpty())
                headers++;
            else if(tag == QLatin1String("all"))
                global++;
            else if(tag == QLatin1String("grid"))
                grid++;
            else if(tag == QLatin1String("inspect"))
                inspect++;
        }
        QCOMPARE(headers, 3);
        QCOMPARE(global, globalBefore);
        QCOMPARE(grid, gridBefore);
        QCOMPARE(inspect, inspectBefore);

        // an injected row in the grid section saves into the grid table only
        const int injectRow = table->rowCount();
        table->insertRow(injectRow);
        QTableWidgetItem *actionItem = new QTableWidgetItem(QStringLiteral("gridSelectLast"));
        actionItem->setData(Qt::UserRole, QStringLiteral("grid"));
        table->setItem(injectRow, 0, actionItem);
        table->setItem(injectRow, 1, new QTableWidgetItem(QStringLiteral("F7")));

        dialog.saveShortcuts();

        QCOMPARE(actionManager->allShortcuts().size(), globalBefore);
        QCOMPARE(actionManager->allInspectShortcuts().size(), inspectBefore);
        QCOMPARE(actionManager->allGridShortcuts().size(), gridBefore + 1);
        QCOMPARE(actionManager->allGridShortcuts().value(QStringLiteral("F7")), QStringLiteral("gridSelectLast"));

        // put the manager back the way the rest of the suite expects it
        actionManager->removeAllGridShortcuts();
        for(auto it = gridSnapshot.constBegin(); it != gridSnapshot.constEnd(); ++it)
            actionManager->addGridShortcut(it.key(), it.value());
    }

    void navigationMovesByColumnsAndBack() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        grid.select(0);

        grid.selectBelow();
        const int rowJump = lastSelection(grid); // = the column count
        QVERIFY2(rowJump >= 1, "selectBelow must leave the first row");
        grid.selectBelow();
        QCOMPARE(lastSelection(grid), rowJump * 2); // column-preserving on a regular row
        grid.selectAbove();
        QCOMPARE(lastSelection(grid), rowJump);
        grid.selectAbove();
        QCOMPARE(lastSelection(grid), 0);
        grid.selectAbove(); // first row: already at the top, stays
        QCOMPARE(lastSelection(grid), 0);
    }

    void navigationRefusesToLeaveTheGridEdges() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        grid.select(0);

        grid.selectAbove(); // first row: already at the top
        QCOMPARE(lastSelection(grid), 0);

        grid.select(24);    // the last item
        grid.selectBelow(); // last row: stays
        QCOMPARE(lastSelection(grid), 24);
        grid.selectNext(); // past the end: stays
        QCOMPARE(lastSelection(grid), 24);
        grid.selectPrev();
        QCOMPARE(lastSelection(grid), 23);
        grid.selectPrev();
        QCOMPARE(lastSelection(grid), 22);
        grid.select(0);
        grid.selectPrev(); // before the start: stays
        QCOMPARE(lastSelection(grid), 0);
    }

    void pageJumpsMoveFourRows() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(25);
        grid.show();
        grid.select(0);
        grid.selectBelow();
        const int cols = lastSelection(grid);
        grid.select(0); // pageDown jumps from lastSelected(): restart from the top

        grid.pageDown();
        QCOMPARE(lastSelection(grid), 4 * cols); // exactly four rows down
        grid.pageUp();
        QCOMPARE(lastSelection(grid), 0); // and exactly four rows back
    }

    void navigationRestoresTheColumnOnIrregularRows() {
        FolderGridView grid;
        grid.resize(500, 600);
        grid.populate(23); // 23 % cols != 0: the last row is short
        grid.show();
        grid.select(0);
        grid.selectBelow();
        const int cols = lastSelection(grid);
        QVERIFY2(cols >= 2, "this calibration needs at least two columns");
        // the last item of the second-to-last row: its column lies beyond the
        // short last row, so selectBelow clamps and the shifted-column logic
        // must remember the original column for selectAbove to restore it
        const int rows = 23 / cols + 1;
        const int orig = (rows - 2) * cols + (cols - 1);
        grid.select(orig);

        grid.selectBelow();
        QVERIFY(lastSelection(grid) != orig);
        grid.selectAbove();

        QCOMPARE(lastSelection(grid), orig); // the shifted column was restored
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

  private:
    // selection() returns QList by value: capturing it in a named local keeps
    // clazy's detaching-temporary quiet AND avoids a real detach per .last()
    int lastSelection(FolderGridView &grid) {
        const QList<int> sel = grid.selection();
        return sel.last();
    }
    // the scancode the platform's own table pairs with a key name — looked
    // up, not hardcoded, so the sweep survives table edits and stays honest
    // about the platform it runs on
    static quint32 scanCodeFor(const QString &keyName) {
        const QMap<quint32, QString> keyTable = inputMap->keys();
        for(auto it = keyTable.constBegin(); it != keyTable.constEnd(); ++it)
            if(it.value() == keyName)
                return it.key();
        return 0;
    }
    // a KeyPress the way ShortcutBuilder will see it: with a real native scan
    // code, because a synthesized event's default 0 yields an empty shortcut
    // name and would silently resolve to nothing
    static void pressGridKey(TestableFolderGridView &grid, const QString &keyName, Qt::Key key,
                             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, key, modifiers, scanCodeFor(keyName), 0u, 0u, QString(), false, 0);
        grid.keyPressEvent(&press);
    }
    QByteArray contentOf(const QString &path) {
        QFile f(path);
        if(!f.open(QIODevice::ReadOnly)) {
            QTest::qFail(qPrintable(QStringLiteral("cannot read %1").arg(path)), __FILE__, __LINE__);
            return {};
        }
        return f.readAll();
    }
    static QMap<QString, int> widgetClasses() {
        QMap<QString, int> counts;
        const QList<QWidget *> widgets = QApplication::allWidgets();
        for(QWidget *w : widgets)
            counts[QString::fromUtf8(w->metaObject()->className())]++;
        return counts;
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestCoreNav t;
    // One process per test function — the same rationale as engine_stack_main's
    // fork per class, taken one level down: this suite decodes images, which
    // makes QtGui spawn and clockjoin-join its own internal pooled threads, and
    // each of those joins leaves a registry-live pthread_t behind (TSan does not
    // intercept clockjoin — LLVM #239326). A later test's WorkerPool or engine
    // threads then collide with the stale value. A private registry per test
    // function ends the reuse; explicit argv (a named test) skips the fork for
    // targeted debugging.
    if(argc > 1)
        return QTest::qExec(&t, argc, argv);
    const QMetaObject *mo = t.metaObject();
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
