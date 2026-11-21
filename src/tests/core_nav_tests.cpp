// Core-level navigation test. This is the layer where the folder-boundary
// setting is actually consumed, so it is where "the configured behavior happens"
// can be asserted end to end: key action -> ActionManager -> Core -> settings ->
// DirectoryManager -> engine.
//
// It builds a real MainWindow under the offscreen platform and initializes the
// same globals main.cpp does, because a half-wired app would prove nothing.
// That is this suite's identity, not a migration debt: the end-to-end chain
// (action -> window -> Core -> engine), the window-title projection, the crop
// panel, the grid selection and the tree teardown are window subjects — they
// stay here when other Core suites go headless.
// Navigation is triggered by emitting ActionManager::nextImage() — the very
// signal a bound key produces.

#include "gtest_qt.h"
#include <QtTest/QSignalSpy>
#ifdef Q_OS_WIN
#include <qt_windows.h> // CreateFileW: the exclusive lock is the platform's refusal channel
#endif
#include <QtTest/qtestcase.h>
#include <QQueue>
#include "gui/overlays/imageinfooverlayproxy.h"
#include "gui/crop/cropcontroller.h"
#include "gui/overlayhost.h"
#include "gui/overlays/copyoverlay.h"
#include "gui/overlays/saveconfirmoverlay.h"
#include "gui/crop/cropoverlay.h"
#include "gui/overlays/floatingmessageproxy.h"
#include <utility>
#include <QApplication>

#include <cerrno>
#include <cstdio> // std::rename: the external rename below is what a file
                  // manager does, and QFile::rename is RENAME_NOREPLACE
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
#include "gui/dialogs/settingsdialog.h"
#include "gui/contextmenu.h"
#include "themestore.h"
#include "core_test_helpers.h"
#include "testhelpers.h"
#include "gui/customwidgets/floatingwidget.h"
#include "gui/customwidgets/floatingwidgetcontainer.h"
#include "gui/dialogs/resizedialog.h"
#include "gui/overlays/videocontrols.h"
#include "gui/crop/cropoverlay.h"
#include "gui/crop/croppanel.h"
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
        qFatal("no thumbnail at index %d", index);
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
    DeadlineJob(std::atomic_bool *_gate, std::atomic_int *_holds) : m_gate(_gate), m_holds(_holds) {
    }
    void run() override {
        m_holds->store(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while(!m_gate->load() && std::chrono::steady_clock::now() < deadline)
            QThread::msleep(1);
        m_holds->store(0);
    }

  private:
    std::atomic_bool *m_gate;
    std::atomic_int *m_holds;
};

class TestCoreNav : public ::testing::Test {
  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons; // the app's process-wide state, by construction
    QString m_base, m_alpha, m_beta;

    static bool sawDir(const QSignalSpy &spy, const QString &dir, int from = 0) {
        for(int i = from; i < spy.count(); ++i)
            if(spy.at(i).at(0).toString() == dir)
                return true;
        return false;
    }

    // 8x8 red PNG via the shared factory
    void writeImage(const QString &path) {
        EXPECT_TRUE(makeImageFile(path, QSize(8, 8)));
    }

    void SetUp() override {
        // IsolatedHome redirected HOME and the config identity; the singletons
        // arrive with the AppSingletons member, so there is no list to keep in
        // sync here.
        EXPECT_TRUE(m_home.path() != nullptr);

        m_base = QDir(m_home.path()).filePath("nav");
        m_alpha = QDir(m_base).filePath("alpha");
        m_beta = QDir(m_base).filePath("beta");
        EXPECT_TRUE(QDir().mkpath(m_alpha));
        EXPECT_TRUE(QDir().mkpath(m_beta));
        writeImage(QDir(m_alpha).filePath("a.png"));
        writeImage(QDir(m_beta).filePath("b.png"));
    }

    QString uniqueDir(const QString &tag) {
        const QString dir = QDir(m_base).filePath(tag + QStringLiteral("-%1").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }
};

TEST_F(TestCoreNav, titleFollowsTheProjection) {
    writeImage(QDir(m_base).filePath("t.png"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("t.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (QDir(m_base).filePath("t.png"));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("t.png"))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.mainWindow()->windowTitle().contains(QStringLiteral("t.png"));
            },
            10000);
        EXPECT_TRUE(core.mainWindow()->windowTitle().contains(QStringLiteral("t.png")))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // the slideshow toggle rides the same wiring
    core.toggleSlideshow();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.mainWindow()->windowTitle().contains(QStringLiteral("[slideshow]"));
            },
            10000);
        EXPECT_TRUE(core.mainWindow()->windowTitle().contains(QStringLiteral("[slideshow]")))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    core.toggleSlideshow();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return !core.mainWindow()->windowTitle().contains(QStringLiteral("[slideshow]"));
            },
            10000);
        EXPECT_TRUE(!core.mainWindow()->windowTitle().contains(QStringLiteral("[slideshow]")))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

TEST_F(TestCoreNav, folderViewFollowsTheDirectoryState) {
    QDir().mkpath(QDir(m_base).filePath("projdir"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("projdir")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.mainWindow()->getFolderView()->directoryPath()) == (QDir(m_base).filePath("projdir"));
            },
            10000);
        EXPECT_EQ(core.mainWindow()->getFolderView()->directoryPath(), QDir(m_base).filePath("projdir"))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

TEST_F(TestCoreNav, cropSurfaceHintsItsKeys) {
    writeImage(QDir(m_base).filePath("cropkeys.png"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("cropkeys.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    core.mainWindow()->crop()->toggle();
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    auto *panel = core.mainWindow()->findChild<CropPanel *>();
    ASSERT_TRUE(panel);

    EXPECT_TRUE(
        panel->findChild<QPushButton *>(QStringLiteral("cropButton"))->text().contains(QLatin1String("[Enter]")))
        << "the crop button carries no key hint";
    EXPECT_TRUE(!panel->findChild<QPushButton *>(QStringLiteral("cropSaveButton"))
                     ->text()
                     .contains(QLatin1String("[Shift+Enter]")))
        << "the disk-writing button still advertises a key";
    EXPECT_TRUE(
        panel->findChild<QPushButton *>(QStringLiteral("cancelButton"))->text().contains(QLatin1String("[Esc]")))
        << "the cancel button carries no key hint";

    auto *combo = panel->findChild<QComboBox *>(QStringLiteral("ARcomboBox"));
    ASSERT_TRUE(combo);
    EXPECT_EQ(combo->currentIndex(), 0); // Free: reset on image change
    EXPECT_EQ(combo->count(), 7);        // Free | Custom | Current Image | This Screen | 1:1 | 4:3 | 16:9

    // the hand-typed ratio's fields belong to Custom alone: hidden under
    // every other preset, visible only while it is the chosen one — and
    // they keep their own values instead of being seeded into confusion
    auto *arInput = panel->findChild<QWidget *>(QStringLiteral("ARInputWidget"));
    ASSERT_TRUE(arInput);
    EXPECT_TRUE(!arInput->isVisible()) << "the custom-ratio fields leak into the Free preset";
    combo->setCurrentIndex(1); // Custom
    EXPECT_TRUE(arInput->isVisible()) << "choosing Custom did not reveal its fields";
    combo->setCurrentIndex(4); // 1:1
    EXPECT_TRUE(!arInput->isVisible()) << "the custom-ratio fields leak into a numeric preset";
    combo->setCurrentIndex(0); // back to Free
    EXPECT_EQ(combo->currentIndex(), 0);

    auto *overlay = core.mainWindow()->findChild<CropOverlay *>();
    ASSERT_TRUE(overlay);

    // the keyboard's Apply is the in-memory crop and nothing else: the
    // Enter lands on the tool signal, and no bare key reaches the
    // disk-writing signal. (A REAL subset first: a whole-image selection
    // is refused by design — cropping everything changes no pixels — so
    // the boxes drive one row/column off.)
    auto *widthBox = panel->findChild<QSpinBox *>(QStringLiteral("width"));
    auto *heightBox = panel->findChild<QSpinBox *>(QStringLiteral("height"));
    ASSERT_TRUE(widthBox && heightBox);
    EXPECT_TRUE(widthBox->maximum() > 1 && heightBox->maximum() > 1)
        << "precondition broken: the fixture image is too small to crop";
    const int originalWidth = widthBox->maximum();
    const int originalHeight = heightBox->maximum();
    widthBox->setValue(originalWidth - 1);
    heightBox->setValue(originalHeight - 1);

    QSignalSpy applied(panel, &CropPanel::crop);
    QSignalSpy savedToDisk(panel, &CropPanel::cropAndSave);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, coretest::scanCodeFor(QStringLiteral("Enter")),
                    0u, 0u);
    QApplication::sendEvent(overlay, &enter);
    EXPECT_EQ(applied.count(), 1);     // the subset existed, and the answer fired
    EXPECT_EQ(savedToDisk.count(), 0); // the bare key never writes

    // Apply does not dismiss: the surface stays open and, once the
    // applied crop settles into the viewer, the selection becomes the
    // whole new image — re-cropping is dragging again, no key needed
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (widthBox->value()) == (originalWidth - 1);
            },
            10000);
        EXPECT_EQ(widthBox->value(), originalWidth - 1)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (heightBox->value()) == (originalHeight - 1);
            },
            10000);
        EXPECT_EQ(heightBox->value(), originalHeight - 1)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(widthBox->maximum(), originalWidth - 1); // the readout tracks the new ground truth
    EXPECT_EQ(heightBox->maximum(), originalHeight - 1);

    // the save chord, pressed while a selection stands, commits the crop
    // and writes — end to end, disk included, from the surface that
    // never left
    widthBox->setValue(widthBox->maximum() - 1);
    heightBox->setValue(heightBox->maximum() - 1);

    QSignalSpy applied2(panel, &CropPanel::crop);
    QKeyEvent ctrlS(QEvent::KeyPress, Qt::Key_S, Qt::ControlModifier, coretest::scanCodeFor(QStringLiteral("S")), 0u,
                    0u);
    QApplication::sendEvent(overlay, &ctrlS);
    EXPECT_EQ(applied2.count(), 1); // the chord commits the pending crop...
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (QImage(QDir(m_base).filePath("cropkeys.png")).size()) ==
                       (QSize(originalWidth - 2, originalHeight - 2));
            },
            10000);
        EXPECT_EQ(QImage(QDir(m_base).filePath("cropkeys.png")).size(), QSize(originalWidth - 2, originalHeight - 2))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    } // ...and writes it
    EXPECT_TRUE(core.mainWindow()->crop()->isActive())
        << "the write went through the global save action, which does not own the surface — Esc is the only exit";
}

// free zoom works mechanically on images that fit the viewport (small
// images included): + steps 1.0 -> 1.2 -> 1.44. Switching to a nominal
// fit mode collapses the free zoom back to that mode's scale — the
// FIT_FREE-is-ephemeral contract the mode-is-intent law rests on.
TEST_F(TestCoreNav, zoomInWorksAndFitSwitchCollapsesFreeZoom) {
    QImage img(400, 300, QImage::Format_RGB32);
    img.fill(Qt::magenta);
    EXPECT_TRUE(img.save(QDir(m_base).filePath("small.png"), "PNG"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("small.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (QDir(m_base).filePath("small.png"));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("small.png"))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
    ASSERT_TRUE(viewer);
    core.mainWindow()->fitOriginal();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (viewer->fitMode()) == (FIT_ORIGINAL);
            },
            5000);
        EXPECT_EQ(viewer->fitMode(), FIT_ORIGINAL)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(qFuzzyCompare(viewer->currentScale(), 1.0f));

    viewer->zoomIn();
    EXPECT_TRUE(qFuzzyCompare(viewer->currentScale(), 1.2f));
    viewer->zoomIn();
    EXPECT_TRUE(qFuzzyCompare(viewer->currentScale(), 1.44f));

    core.mainWindow()->switchFitMode();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (viewer->fitMode()) == (FIT_WINDOW);
            },
            5000);
        EXPECT_EQ(viewer->fitMode(), FIT_WINDOW)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(qFuzzyCompare(viewer->currentScale(), 1.0f));
}

TEST_F(TestCoreNav, preloaderFollowsTheDirectionOfTravel) {
    settings->setUsePreloader(true);
    EXPECT_TRUE(settings->usePreloader());

    // four files so both directions have an uncached neighbor to prove
    // themselves with: forward from c reaches d, backward from d reaches b
    QString seq = QDir(m_base).filePath("seq");
    EXPECT_TRUE(QDir().mkpath(seq));
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
    EXPECT_TRUE(listed.isValid());

    Core::Deps deps;
    deps.bridge = &bridge;
    Core core(deps);
    EXPECT_TRUE(core.loadPath(files.at(0)));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return sawDir(listed, seq);
            },
            10000);
        EXPECT_TRUE(sawDir(listed, seq)) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    // the cold-open load rides whenListed: it fires against a landed
    // listing, so the first preload has a real neighbor to decode

    // one flip forward: from b the forward neighbor c decodes
    actionManager->action("nextImage")->trigger();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (files.at(1));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), files.at(1))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.directoryModel()->isLoaded(files.at(2));
            },
            10000);
        EXPECT_TRUE(core.directoryModel()->isLoaded(files.at(2)))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // another flip forward: from c the forward neighbor is d
    actionManager->action("nextImage")->trigger();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (files.at(2));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), files.at(2))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.directoryModel()->isLoaded(files.at(3));
            },
            10000);
        EXPECT_TRUE(core.directoryModel()->isLoaded(files.at(3)))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // jump to the last file: still a forward move, so the backward
    // neighbor b must not decode — and the cache trim drops it
    EXPECT_TRUE(core.loadPath(files.at(3)));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (files.at(3));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), files.at(3))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(!core.directoryModel()->isLoaded(files.at(1)));

    // one backward move: the preload must follow the travel direction and
    // decode b, which a forward-facing policy would never have touched
    actionManager->action("prevImage")->trigger();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (files.at(2));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), files.at(2))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.directoryModel()->isLoaded(files.at(1));
            },
            10000);
        EXPECT_TRUE(core.directoryModel()->isLoaded(files.at(1)))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

// The full-res upgrade is a dwell decision, not a reflex: a file that
// holds the view gets its full-res pass after the dwell, and a file the
// flight left behind never starts one. Before the dwell, every
// intermediate file of a flight queued an upgrade the next flip made
// stale — measured at 5-19 zombies per flight, each burning 150-250ms of
// decode on exactly the cores the flight's own decodes need (flipbench on
// taskset-constrained cores, 2026-11-19: per-flip display latency ran to
// ~1s at 2 cores).
TEST_F(TestCoreNav, theFullResUpgradeWaitsForTheDwell) {
    settings->setUsePreloader(true);
    QString dir = QDir(m_base).filePath("dwell");
    EXPECT_TRUE(QDir().mkpath(dir));
    QStringList files;
    for(const char *n : {"a", "b", "c"}) {
        const QString path = QDir(dir).filePath(QString(n) + ".jpg");
        EXPECT_TRUE(makeImageFile(path, QSize(3000, 2000), "JPG"));
        files << path;
    }

    EngineBridge bridge;
    QSignalSpy listed(&bridge, &EngineBridge::dirListed);
    Core::Deps deps;
    deps.bridge = &bridge;
    Core core(deps);
    EXPECT_TRUE(core.loadPath(files.at(0)));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return sawDir(listed, dir);
            },
            10000);
        EXPECT_TRUE(sawDir(listed, dir)) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    QElapsedTimer clock;
    clock.start();
    qint64 scaledAt = -1, fullAt = -1;
    QObject::connect(core.directoryModel().get(), &DirectoryModel::imageReady,
                     [&](std::shared_ptr<Image>, const QString &path, bool scaled) {
                         if(path != files.at(2))
                             return;
                         if(scaled)
                             scaledAt = clock.elapsed();
                         else
                             fullAt = clock.elapsed();
                     });

    // b is left behind mid-flight (its tier-1 may or may not land before
    // the flip away — either way the dwell decides its upgrade), and c
    // holds the view
    actionManager->action("nextImage")->trigger();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (files.at(1));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), files.at(1))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    actionManager->action("nextImage")->trigger();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (files.at(2));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), files.at(2))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // the deterministic contract of the dwell: the full-res pass of the
    // file that holds the view starts no earlier than the dwell after its
    // tier-1 landed. The old code fired the upgrade in the same dispatch
    // as the tier-1 delivery, so its gap was one decode (~35ms here); the
    // dwell makes the gap dwell+decode. The "left-behind file never
    // upgrades" half is NOT pinned here: its broken-code redness depends
    // on a microsecond pickup-vs-clearPool race (the worker's job-destruction
    // window), which would make this pin flaky in both directions.
    QTest::qWait(2000); // let both deliveries land
    EXPECT_TRUE(scaledAt >= 0 && fullAt >= 0) << "c's two tiers never both arrived";
    const qint64 dwellGap = fullAt - scaledAt;
    EXPECT_TRUE(dwellGap >= 300) << qPrintable(
        QStringLiteral("the full-res pass started before the dwell elapsed (gap %1ms)").arg(dwellGap));
    EXPECT_TRUE(dwellGap <= 10000) << "the full-res pass took absurdly long";
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

// 14a61f7a armed the cold open to the listing landing but left main's
// unconditional showGui in place: the window appeared empty first and the
// image arrived after. The reveal belongs to the first ready frame.
TEST_F(TestCoreNav, coldOpenKeepsTheWindowHiddenUntilTheFirstFrame) {
    QString dir = QDir(m_base).filePath("waitfirst");
    EXPECT_TRUE(QDir().mkpath(dir));
    QStringList files;
    for(const char *n : {"one", "two"}) {
        const QString path = QDir(dir).filePath(QString(n) + ".png");
        writeImage(path);
        files << path;
    }

    Core core;
    EXPECT_TRUE(!core.mainWindow()->isVisible());
    EXPECT_TRUE(core.loadPath(files.at(0)));
    EXPECT_TRUE(core.firstFramePending());
    // main's tail, faithfully: show only when nothing is pending
    if(!core.firstFramePending())
        core.showGui();
    EXPECT_TRUE(!core.mainWindow()->isVisible());

    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return !core.firstFramePending();
            },
            10000);
        EXPECT_TRUE(!core.firstFramePending())
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(core.mainWindow()->isVisible());
    EXPECT_EQ(core.currentFilePath(), files.at(0));
    // the heavy panels arrive with the window's own first show (shownOnce,
    // not a timer): crop panel existence is the probe — it is built in
    // setupFullUi and nowhere else
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.mainWindow()->findChild<CropPanel *>();
            },
            10000);
        EXPECT_TRUE(core.mainWindow()->findChild<CropPanel *>())
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

// The scaling timing contract: requests are suppressed while the window
// is hidden (cold open), and the show itself triggers the fresh pass —
// "window ready, then act" — instead of leaking a visibility query into
// Core.
TEST_F(TestCoreNav, scalingWaitsForTheWindowThenActs) {
    QString dir = QDir(m_base).filePath("scalingtiming");
    EXPECT_TRUE(QDir().mkpath(dir));
    // a large image: at fit-window it is DOWNSCALED (scale < 1) — the one
    // shape requestScaling actually serves; an 8x8 fixture would sit at
    // scale 1.0 and legitimately request nothing
    const QString path = QDir(dir).filePath("s.png");
    QImage big(2000, 1200, QImage::Format_RGB32);
    big.fill(Qt::darkCyan);
    EXPECT_TRUE(big.save(path, "PNG"));

    Core core;
    EXPECT_TRUE(core.loadPath(path));
    EXPECT_TRUE(!core.mainWindow()->isVisible()); // hidden at load: the frame has not arrived
    auto *viewer = core.mainWindow()->viewer();
    QSignalSpy requests(viewer, &ViewerWidget::scalingRequested);
    EXPECT_TRUE(requests.isValid());

    // the frame arrives (the QTRY pumps the loop): the handshake raises
    // the window, and the show itself triggers the fresh scaling pass
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (path);
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), path)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.mainWindow()->isVisible();
            },
            10000);
        EXPECT_TRUE(core.mainWindow()->isVisible())
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return requests.count() >= 1;
            },
            10000);
        EXPECT_TRUE(requests.count() >= 1) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    } // ready → act
}

TEST_F(TestCoreNav, gotoAdjacentAtFolderEndEntersNextFolder) {
    settings->setFolderEndAction(FOLDER_END_GOTO_ADJACENT);
    EXPECT_TRUE(settings->folderEndAction() == FOLDER_END_GOTO_ADJACENT);

    QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
    EXPECT_TRUE(listed.isValid());

    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    // opening a file defers the folder listing until the image is ready
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return sawDir(listed, m_alpha);
            },
            10000);
        EXPECT_TRUE(sawDir(listed, m_alpha)) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(core.currentFilePath(), QDir(m_alpha).filePath("a.png"));

    const int before = listed.count();
    actionManager->action("nextImage")->trigger();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return sawDir(listed, m_beta, before);
            },
            10000);
        EXPECT_TRUE(sawDir(listed, m_beta, before))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    // it must land on the new folder's image, not on whatever the previous
    // folder's mirror still had at index 0
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (QDir(m_beta).filePath("b.png"));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), QDir(m_beta).filePath("b.png"))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

TEST_F(TestCoreNav, noActionAtFolderEndStaysInThisFolder) {
    settings->setFolderEndAction(FOLDER_END_NO_ACTION);
    EXPECT_TRUE(settings->folderEndAction() == FOLDER_END_NO_ACTION);

    QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return sawDir(listed, m_alpha);
            },
            10000);
        EXPECT_TRUE(sawDir(listed, m_alpha)) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    const int before = listed.count();
    actionManager->action("nextImage")->trigger();
    // settled = nothing outstanding *and* no deferred hop armed; the old
    // 500ms sleep could only hope the 40ms one was slow enough to be seen
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(listed.count(), before);
    EXPECT_TRUE(!sawDir(listed, m_beta, before));
    EXPECT_EQ(core.currentFilePath(), QDir(m_alpha).filePath("a.png"));
}

TEST_F(TestCoreNav, setWallpaperOnAnUnsupportedSessionIsASafeNoOp) {
    FreshSettings fresh;
    const QByteArray oldSession = qgetenv("DESKTOP_SESSION");
    qputenv("DESKTOP_SESSION", "twm");

    QString wpBase = QDir(m_home.path()).filePath("wp");
    EXPECT_TRUE(QDir().mkpath(wpBase));
    QString img = QDir(wpBase).filePath("w.png");
    writeImage(img);

    Core core;
    EXPECT_TRUE(core.loadPath(img));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (img);
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), img)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // the full chain runs; with no session match nothing executes and the
    // wallpaper is untouched — the assertion is the safe return. Called
    // directly, not by name: setWallpaper is one of Core's commands (the
    // block that used to be private behind friend declarations), and the
    // string form bought nothing.
    core.setWallpaper();
    qputenv("DESKTOP_SESSION", oldSession);
}

// Core's file-operation and directory-navigation members: gcov reports
// execution_count == 0 for moveCurrentFile, copyCurrentFile, onDropIn,
// prevDirectory, nextDirectory, loadParentDir and toggleSlideshow. Each is
// asserted on disk state and on currentFilePath, and the collision paths
// run through the scripted IUserInteractions seam rather than a dialog.
// The directories are created per test: siblingDirectory() below needs a
// parent holding exactly two children, and this class's shared alpha/base
// accumulate files and directories from every earlier test.
TEST_F(TestCoreNav, adjacentDirectoryNavigationMovesBetweenSiblingFolders) {
    const QString parent = uniqueDir("adjparent");
    const QString dirA = parent + QStringLiteral("/aaa");
    const QString dirB = parent + QStringLiteral("/bbb");
    QDir().mkpath(dirA);
    QDir().mkpath(dirB);
    writeImage(dirA + QStringLiteral("/one.png"));
    writeImage(dirB + QStringLiteral("/two.png"));

    Core core;
    EXPECT_TRUE(core.loadPath(dirB + QStringLiteral("/two.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(core.directoryModel()->listing().directoryPath(), dirB);

    core.prevDirectory(false);
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(core.currentFilePath(), dirA + QStringLiteral("/one.png"));
    EXPECT_EQ(core.directoryModel()->listing().directoryPath(), dirA);

    core.nextDirectory();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(core.currentFilePath(), dirB + QStringLiteral("/two.png"));

    // selectLast is the folder-view contract: arriving from the end of a
    // folder must land on the last file, not the first
    core.prevDirectory(true);
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(core.currentFilePath(), dirA + QStringLiteral("/one.png"));
}

TEST_F(TestCoreNav, destroyingTheCoreFreesTheWholeWindowTree) {
    const QMap<QString, int> before = coretest::widgetClasses();
    const int beforeTotal = QApplication::allWidgets().size();
    {
        Core core;
        // the probe only means something if the window tree is really live
        EXPECT_TRUE(QApplication::allWidgets().size() > beforeTotal);
    }
    const QMap<QString, int> after = coretest::widgetClasses();
    QStringList surplus;
    for(auto it = after.constBegin(); it != after.constEnd(); ++it) {
        const int left = it.value() - before.value(it.key(), 0);
        if(left > 0)
            surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
    }
    EXPECT_TRUE(surplus.isEmpty()) << qPrintable(QStringLiteral("widgets outliving the destroyed core: ") +
                                                 surplus.join(", "));
}

TEST_F(TestCoreNav, settingsBusWaitsOnlyForThePipelineItsSettingsActuallyFeed) {
    Core core;
    std::atomic_bool gate{false};
    std::atomic_int holds{0};
    core.executor().submit(PoolOwner::Loader, 0, std::make_unique<DeadlineJob>(&gate, &holds));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (holds.load()) == (1);
            },
            10000);
        EXPECT_EQ(holds.load(), 1) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    QElapsedTimer t;
    t.start();
    settings->sendChangeNotification(); // what the settings dialog's Save does
    const qint64 elapsed = t.elapsed();

    EXPECT_EQ(holds.load(), 1); // the Loader job was still running when it returned
    EXPECT_TRUE(elapsed < 1000) << qPrintable(QStringLiteral("the settings bus waited %1 ms — a pool drain that is not "
                                                             "owner-scoped waits for work none of its settings feed")
                                                  .arg(elapsed));
    gate = true;
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (holds.load()) == (0);
            },
            10000);
        EXPECT_EQ(holds.load(), 0) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

TEST_F(TestCoreNav, settingsBusReachesTheThumbnailStripThroughThePanelChain) {
    Core core;
    const int original = settings->panelPreviewsSize(); // singleton: snapshot and restore
    auto *strip = core.mainWindow()->findChild<ThumbnailStrip *>();
    ASSERT_TRUE(strip) << "no ThumbnailStrip under MW — the chain is not what this claims";

    settings->setPanelPreviewsSize(30);
    settings->sendChangeNotification();
    const QSize small = strip->itemSize();
    settings->setPanelPreviewsSize(200);
    settings->sendChangeNotification();
    const QSize big = strip->itemSize();

    settings->setPanelPreviewsSize(original);
    settings->sendChangeNotification();

    EXPECT_TRUE(big.height() > small.height() || big.width() > small.width())
        << qPrintable(QStringLiteral("the settings bus does not reach ThumbnailStrip::readSettings "
                                     "(30 -> %1x%2, 200 -> %3x%4)")
                          .arg(small.width())
                          .arg(small.height())
                          .arg(big.width())
                          .arg(big.height()));
}

TEST_F(TestCoreNav, gridActionsDriveTheGridThroughTheFullChain) {
    const QString dir = uniqueDir("gridact");
    writeImage(QDir(dir).filePath("a.png"));
    writeImage(QDir(dir).filePath("b.png"));

    Core core;
    EXPECT_TRUE(core.loadPath(QDir(dir).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    core.enableFolderView();
    core.folderPresenter().selectAndFocus(0);
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.currentSelection().count() == 1;
            },
            10000);
        EXPECT_TRUE(core.currentSelection().count() == 1)
            << "precondition broken: the folder view holds no selection"
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    EXPECT_TRUE(actionManager->invokeAction(QStringLiteral("gridSelectNext")));
    EXPECT_EQ(core.currentSelection().count(), 1);
    EXPECT_EQ(core.currentSelection().constFirst(), QDir(dir).filePath("b.png"));

    EXPECT_TRUE(actionManager->invokeAction(QStringLiteral("gridOpen")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (QDir(dir).filePath("b.png"));
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), QDir(dir).filePath("b.png"))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
}

// #467's silent half: a batch reported only its successes. Qt's moveToTrash
// refuses a name whose trash target (files/ prefix + name) exceeds
// NAME_MAX, and that refusal used to vanish — "Moved to trash: 1 files"
// for a two-file selection. The report must carry both numbers, and each
// refusal also leaves its path and decoded reason in the session log. The
// exact split is a property of the trash topology (a mount with no trash
// at all fails every member), so the pin is the format, not the counts.
TEST_F(TestCoreNav, batchTrashReportsHowManyFailed) {
#ifdef Q_OS_WIN
    // the 254-char fixture cannot even be written here: the path crosses
    // MAX_PATH before the name exhausts any NAME_MAX, writeImage fails
    // silently, and exists() answers false for a file that never existed.
    // The refusable subject on this platform is a file held open with no
    // sharing — the shell's recycle cannot take it while the handle
    // lives, and it stays a visible, existing refusal.
    const QString longFile = QDir(m_alpha).filePath(QStringLiteral("locked.png"));
    writeImage(longFile);
    HANDLE held = CreateFileW(reinterpret_cast<const WCHAR *>(longFile.utf16()), GENERIC_READ | GENERIC_WRITE,
                              0 /* no sharing */, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(held != INVALID_HANDLE_VALUE) << "the lock fixture failed to open the refusable subject";
#else
    // 250 'l' + ".png" fits the source directory (254 <= NAME_MAX) but
    // cannot fit any real trash: even HOME=/ gives a 26-char files/ prefix.
    const QString longName = QString(250, QLatin1Char('l')) + QStringLiteral(".png");
    const QString longFile = QDir(m_alpha).filePath(longName);
    writeImage(longFile);
#endif

    coretest::FakeOverlayHost overlayFake;
    Core::Deps deps;
    deps.overlays = &overlayFake;
    Core core(deps);
    coretest::FakeInteractions fake;
    fake.confirmAnswer = true;
    core.setUserInteractions(&fake);
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    core.enableFolderView();
    core.mainWindow()->getFolderView()->select(QList<int>{0, 1});
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.currentSelection().count() == 2;
            },
            10000);
        EXPECT_TRUE(core.currentSelection().count() == 2)
            << "precondition broken: the folder view holds no two-file selection"
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    core.moveToTrash();

    EXPECT_TRUE(QFileInfo::exists(longFile)) << "the refused file vanished anyway";
    EXPECT_TRUE(overlayFake.errorCalls > 0) << "the failure report never reached the injected host";
    static const QRegularExpression bothHalves(QStringLiteral("\\d+ files, \\d+ failed"));
    EXPECT_TRUE(bothHalves.match(overlayFake.lastError).hasMatch())
        << qPrintable(QStringLiteral("batch report lost a half: \"") + overlayFake.lastError + "\"");
#ifdef Q_OS_WIN
    EXPECT_TRUE(CloseHandle(held));
#endif
}

TEST_F(TestCoreNav, batchTrashAllSuccessStillReportsTheCount) {
    const QString dir = uniqueDir("trashok");
    const QString probe = QDir(dir).filePath("trash-probe.txt");
    EXPECT_TRUE(QFile(probe).open(QIODevice::WriteOnly));
    FileOpResult probeResult;
    FileOperations::moveToTrash(probe, probeResult);
    if(probeResult != FileOpResult::SUCCESS)
        GTEST_SKIP()
            << "moveToTrash unavailable on this filesystem topology (no writable top-level trash for the fixture mount)";

    writeImage(QDir(dir).filePath("a.png"));
    writeImage(QDir(dir).filePath("b.png"));

    coretest::FakeOverlayHost overlayFake;
    Core::Deps deps;
    deps.overlays = &overlayFake;
    Core core(deps);
    coretest::FakeInteractions fake;
    fake.confirmAnswer = true;
    core.setUserInteractions(&fake);
    EXPECT_TRUE(core.loadPath(QDir(dir).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    core.enableFolderView();
    core.mainWindow()->getFolderView()->select(QList<int>{0, 1});
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.currentSelection().count() == 2;
            },
            10000);
        EXPECT_TRUE(core.currentSelection().count() == 2)
            << "precondition broken: the folder view holds no two-file selection"
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    core.moveToTrash();

    EXPECT_TRUE(!QFileInfo::exists(QDir(dir).filePath("a.png")));
    EXPECT_TRUE(!QFileInfo::exists(QDir(dir).filePath("b.png")));
    EXPECT_TRUE(overlayFake.successCalls > 0) << "the success report never reached the injected host";
    EXPECT_TRUE(overlayFake.lastSuccess.contains(QStringLiteral("2 files")) &&
                !overlayFake.lastSuccess.contains(QStringLiteral("failed")))
        << qPrintable(QStringLiteral("all-success batch reported: \"") + overlayFake.lastSuccess + "\"");
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
