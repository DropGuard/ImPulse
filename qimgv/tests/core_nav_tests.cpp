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

;

// The presenter's remaining two view connections (thumbnailsRequested and
// draggedOver) end in IDirectoryView *methods* rather than signals, so the
// observable is the call arriving at the view. This proxy counts them and still
// forwards, so the real strip path runs underneath.
;

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

    // ---- the editing cluster: edit_template's executors are dialog-free in
    // document mode (edits stay in memory until an explicit save) and
    // confirmation-gated in folder view (edits hit the disk immediately). The
    // fake scripts the confirmation; the assertions watch memory, disk, and
    // the confirm count. The fixture is a 16x8 with two colored halves: a
    // solid square could never show a rotation.

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

    // #467's silent half: a batch reported only its successes. Qt's moveToTrash
    // refuses a name whose trash target (files/ prefix + name) exceeds
    // NAME_MAX, and that refusal used to vanish — "Moved to trash: 1 files"
    // for a two-file selection. The report must carry both numbers, and each
    // refusal also leaves its path and decoded reason in the session log. The
    // exact split is a property of the trash topology (a mount with no trash
    // at all fails every member), so the pin is the format, not the counts.

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

        coretest::FakeOverlayHost overlayFake;
        Core::Deps deps;
        deps.overlays = &overlayFake;
        Core core(deps);
        coretest::FakeInteractions fake;
        fake.confirmAnswer = true;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.mainWindow()->getFolderView()->select(QList<int>{0, 1});
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 2,
                                  "precondition broken: the folder view holds no two-file selection", 10000);

        core.moveToTrash();

        QVERIFY2(QFileInfo::exists(longFile), "the refused file vanished anyway");
        QVERIFY2(overlayFake.errorCalls > 0, "the failure report never reached the injected host");
        static const QRegularExpression bothHalves(QStringLiteral("\\d+ files, \\d+ failed"));
        QVERIFY2(bothHalves.match(overlayFake.lastError).hasMatch(),
                 qPrintable(QStringLiteral("batch report lost a half: \"") + overlayFake.lastError + "\""));
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

        coretest::FakeOverlayHost overlayFake;
        Core::Deps deps;
        deps.overlays = &overlayFake;
        Core core(deps);
        coretest::FakeInteractions fake;
        fake.confirmAnswer = true;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(QDir(dir).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.mainWindow()->getFolderView()->select(QList<int>{0, 1});
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 2,
                                  "precondition broken: the folder view holds no two-file selection", 10000);

        core.moveToTrash();

        QVERIFY(!QFileInfo::exists(QDir(dir).filePath("a.png")));
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath("b.png")));
        QVERIFY2(overlayFake.successCalls > 0, "the success report never reached the injected host");
        QVERIFY2(overlayFake.lastSuccess.contains(QStringLiteral("2 files")) &&
                     !overlayFake.lastSuccess.contains(QStringLiteral("failed")),
                 qPrintable(QStringLiteral("all-success batch reported: \"") + overlayFake.lastSuccess + "\""));
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
