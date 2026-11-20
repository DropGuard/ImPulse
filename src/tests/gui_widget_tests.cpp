// Widget-level tests for the GUI surfaces: overlays, panels, the folder grid's
// keyboard and drag behaviour, the settings dialog, and the small pure helpers.
//
// Split out of core_nav_tests.cpp, which had grown to 3595 lines and 112 tests
// covering eight unrelated surfaces. The line drawn here is dependency, not
// domain: nothing in this file constructs a Core, a DirectoryModel or a
// Thumbnailer, so none of it pays for an engine, a watcher or an async listing.
// The tests that DO need those stayed behind in core_nav_tests.cpp.
//
// The `base`/`alpha`/`beta` members of the old shared fixture are gone on
// purpose: a test that needs a directory makes its own. Sharing one tree across
// a file is how a test that forgets to clean up poisons its neighbours.
#include "gtest_qt.h"
#include "gui/flowlayout.h"
#include <QtTest/QSignalSpy>
#include <QtTest/qtestcase.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QEventLoop>
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include "gtest_qt.h"

#include "components/actionmanager/actionmanager.h"
#include "core.h"
#include "core_test_helpers.h"
#include "gui/contextmenu.h"
#include "gui/customwidgets/entryinfoitem.h"
#include "gui/customwidgets/floatingwidget.h"
#include "gui/customwidgets/floatingwidgetcontainer.h"
#include "gui/customwidgets/settingsidebar.h"
#include "gui/customwidgets/styledcombobox.h"
#include "gui/dialogs/resizedialog.h"
#include "gui/dialogs/settingsdialog.h"
#include "gui/folderview/filesystemmodelcustom.h"
#include "gui/folderview/folderview.h"
#include "gui/folderview/foldergridview.h"
#include "gui/folderview/fvoptionspopup.h"
#include "gui/overlays/clickzoneoverlay.h"
#include "gui/crop/cropoverlay.h"
#include "gui/crop/cropcontroller.h"
#include "gui/overlays/copyoverlay.h"
#include "gui/overlayhost.h"
#include "gui/overlays/renameoverlay.h"
#include "gui/overlays/floatingmessageproxy.h"
#include "gui/overlays/imageinfooverlayproxy.h"
#include "gui/overlays/saveconfirmoverlay.h"
#include "gui/overlays/videocontrols.h"
#include "gui/overlays/zoomindicatoroverlay.h"
#include "gui/crop/croppanel.h"
#include "gui/panels/mainpanel/mainpanel.h"
#include "gui/viewers/imageviewerv2.h"
#include "settings.h"
#include "testhelpers.h"
#include "utils/fileoperations.h"

// FloatingWidget::setContainerSize is protected; a pin reaches it the way the
// widget hierarchy does — through a subclass.
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

// The widgets in the grid's own visual order, which is the order an
// IDirectoryView index means.

// The overlay-proxy lifetime helper: a mechanism, not a test case.
template <typename Proxy> void proxyOutlivesItsContainer() {
    auto *container = new FloatingWidgetContainer();
    auto proxy = std::make_unique<Proxy>(container);
    proxy->init();    // materializes the overlay as the container's child
    delete container; // the tree frees the overlay right here
    proxy.reset();    // the proxy destructor must not touch it now
}

class TestGuiWidgets : public ::testing::Test {
  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons; // the app's process-wide state, by construction

    // a directory of this test's own, never shared with a sibling
    QString uniqueDir(const QString &tag) {
        const QString dir =
            QDir(m_home.path()).filePath(tag + QStringLiteral("-%1").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    void SetUp() override {
        EXPECT_TRUE(m_home.path() != nullptr);
    }
};

TEST_F(TestGuiWidgets, fixturesRestoreTheProcessStateTheyChange) {
    const QByteArray homeBefore = qgetenv("HOME");
    const QByteArray cacheBefore = qgetenv("XDG_CACHE_HOME");
    const QByteArray configBefore = qgetenv("XDG_CONFIG_HOME");
    const QString appBefore = QCoreApplication::applicationName();
    const QString orgBefore = QCoreApplication::organizationName();
    {
        IsolatedHome scoped;
        EXPECT_TRUE(scoped.path() != homeBefore) << "the fixture did not redirect HOME";
    }
    EXPECT_EQ(qgetenv("HOME"), homeBefore);
    EXPECT_EQ(qgetenv("XDG_CACHE_HOME"), cacheBefore);
    EXPECT_EQ(qgetenv("XDG_CONFIG_HOME"), configBefore);
    EXPECT_EQ(QCoreApplication::applicationName(), appBefore);
    EXPECT_EQ(QCoreApplication::organizationName(), orgBefore);

    {
        FreshSettings scoped;
        EXPECT_TRUE(QCoreApplication::applicationName() != appBefore)
            << "the fixture did not swap the application identity";
    }
    EXPECT_EQ(QCoreApplication::applicationName(), appBefore);
    EXPECT_TRUE(::settings != nullptr) << "the fixture did not restore the settings pointer";
}

TEST_F(TestGuiWidgets, connectsToTheAppLifetimeSenderDieWithTheWidget) {
    QSignalSpy survivor(settings, &Settings::settingsChanged);

    {
        StyledComboBox box;
        FileSystemModelCustom model;
        FolderGridView grid;
    }

    emit settings->settingsChanged();

    EXPECT_EQ(survivor.count(), 1);
}

TEST_F(TestGuiWidgets, contextMenuKeyboardNavigationMovesAndWraps) {
    TestableContextMenu menu;
    menu.showAt(QPoint(40, 40));
    EXPECT_EQ(menu.navigationIndex(), -1);

    menu.pressKey(Qt::Key_Down); // from nothing: the first item
    EXPECT_EQ(menu.navigationIndex(), 0);
    menu.pressKey(Qt::Key_Up); // wrap backwards: the last item
    EXPECT_TRUE(menu.navigationIndex() > 0);
    menu.pressKey(Qt::Key_Down); // wrap forwards: back to the first
    EXPECT_EQ(menu.navigationIndex(), 0);
    menu.pressKey(Qt::Key_Down);
    EXPECT_EQ(menu.navigationIndex(), 1);
}

TEST_F(TestGuiWidgets, destroyingAWidgetFreesEveryWidgetItCreated) {
    const QMap<QString, int> before = coretest::widgetClasses();
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
        EXPECT_TRUE(QApplication::allWidgets().size() > beforeTotal);
    }
    const QMap<QString, int> after = coretest::widgetClasses();
    QStringList surplus;
    for(auto it = after.constBegin(); it != after.constEnd(); ++it) {
        const int left = it.value() - before.value(it.key(), 0);
        if(left > 0)
            surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
    }
    EXPECT_TRUE(surplus.isEmpty()) << qPrintable(QStringLiteral("widgets outliving their parent: ") +
                                                 surplus.join(", "));
}

TEST_F(TestGuiWidgets, pendingRegistryHoldsOnlyTypedButUnsubmittedInput) {
    const QString dir = uniqueDir("pendingmembers");
    const QString img = QDir(dir).filePath("a.png");
    EXPECT_TRUE(makeImageFile(img, QSize(8, 8)));
    Core core;
    EXPECT_TRUE(core.loadPath(img));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core.isSettled();
            },
            10000);
        EXPECT_TRUE(core.isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    auto *overlays = core.mainWindow()->overlays();
    ASSERT_TRUE(overlays);

    // nothing is open yet
    EXPECT_TRUE(!overlays->hasPendingSurface());

    // the save prompt: visible, but NOT a pending surface — it reports an
    // edit the state leg already covers
    settings->setShowSaveOverlay(true);
    overlays->showSaveOverlay();
    auto *saveOverlay = core.mainWindow()->findChild<SaveConfirmOverlay *>();
    ASSERT_TRUE(saveOverlay && saveOverlay->isVisible());
    EXPECT_TRUE(!overlays->hasPendingSurface())
        << "the save prompt registered as pending: it would answer the "
           "isEdited() question twice, and a stale entry would outlive the edit";
    // the contract is structural, not just observed: these two surfaces
    // must not even BE IPendingSurface — a registry entry can be removed,
    // but an interface cast is what would let a future setup re-register
    // them without touching this test
    EXPECT_TRUE(dynamic_cast<IPendingSurface *>(saveOverlay) == nullptr)
        << "SaveConfirmOverlay still implements IPendingSurface";
    overlays->hideSaveOverlay();

    // the copy menu: typed-but-unsubmitted target — pending while open
    overlays->triggerCopyOverlay();
    auto *copyOverlay = core.mainWindow()->findChild<CopyOverlay *>();
    ASSERT_TRUE(copyOverlay && copyOverlay->isVisible());
    EXPECT_TRUE(overlays->hasPendingSurface()) << "the copy menu did not register: navigation would walk "
                                                  "away from a target the user has not chosen";
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(copyOverlay, &esc);
    // the gtest vocabulary's wait, not QTest::Internal::defaultTryTimeout: the
    // latter is private QTest API and does not exist in every 6.x
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return !copyOverlay->isVisible();
        },
        5000))
        << "the copy overlay stayed visible past the deadline";
    EXPECT_TRUE(!overlays->hasPendingSurface());

    // the rename field: same class, same answer
    overlays->toggleRenameOverlay(QStringLiteral("a.png"));
    auto *renameOverlay = core.mainWindow()->findChild<RenameOverlay *>();
    ASSERT_TRUE(renameOverlay && renameOverlay->isVisible());
    EXPECT_TRUE(overlays->hasPendingSurface()) << "the rename field did not register: a half-typed name "
                                                  "would be dropped by a walk";
    QApplication::sendEvent(renameOverlay, &esc);
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return !renameOverlay->isVisible();
        },
        5000))
        << "the rename overlay stayed visible past the deadline";
    EXPECT_TRUE(!overlays->hasPendingSurface());

    // the crop surface: pending only once the crop is APPLIED, and then
    // through isEdited(), not through the registry
    core.toggleCropPanel();
    EXPECT_TRUE(core.mainWindow()->crop()->isActive());
    EXPECT_TRUE(!overlays->hasPendingSurface()) << "the crop surface registered as pending: an unapplied "
                                                   "selection loses nothing when the walk moves on";
    auto *cropOverlay = core.mainWindow()->findChild<CropOverlay *>();
    ASSERT_TRUE(cropOverlay);
    cropOverlay->selectAll();
    EXPECT_TRUE(dynamic_cast<IPendingSurface *>(cropOverlay) == nullptr)
        << "CropOverlay still implements IPendingSurface";
    EXPECT_TRUE(!overlays->hasPendingSurface())
        << "a selection alone changed the registry: nothing is applied yet, so nothing can be lost";
}

TEST_F(TestGuiWidgets, dialogCheckboxBindingsAreSymmetric) {
    FreshSettings fresh;
    settings->setTrackpadDetection(true);

    SettingsDialog dlg;
    auto *cb = dlg.findChild<QCheckBox *>("trackpadDetectionCheckBox");
    ASSERT_TRUE(cb != nullptr);
    // read half: the widget reflects the setting
    EXPECT_EQ(cb->isChecked(), true);
    // save half: the widget's state lands in the setting
    cb->setChecked(false);
    dlg.saveSettings();
    EXPECT_EQ(settings->trackpadDetection(), false);
    // and an untouched binding round-trips without drift
    dlg.readSettings();
    EXPECT_EQ(dlg.findChild<QCheckBox *>("trackpadDetectionCheckBox")->isChecked(), false);
    dlg.saveSettings();
    EXPECT_EQ(settings->trackpadDetection(), false);
    // permanent delete is confirm-gated unconditionally now: no checkbox
    // exists for it, and the behavior pin lives in
    // removePermanentDeletesAfterScriptedConfirmation
    EXPECT_TRUE(dlg.findChild<QCheckBox *>("confirmDeleteCheckBox") == nullptr);
}

TEST_F(TestGuiWidgets, dialogComboBindingFollowsTheSetting) {
    FreshSettings fresh;
    settings->setLanguage("ja_JP");

    SettingsDialog dlg;
    auto *combo = dlg.findChild<QComboBox *>("langComboBox");
    ASSERT_TRUE(combo != nullptr);
    // read half: the stored locale maps through the display-name list
    EXPECT_EQ(combo->currentText(), QStringLiteral("日本語"));
    // save half: a user pick lands as the stored locale
    combo->setCurrentIndex(combo->findText(QStringLiteral("简体中文")));
    dlg.saveSettings();
    EXPECT_EQ(settings->language(), QStringLiteral("zh_CN"));
    // an unknown stored value falls back to en_US — the old fallback line
    // ("setCurrentText("en_US")") never matched any item and silently
    // kept the previous selection
    settings->setLanguage("xx_XX");
    dlg.readSettings();
    EXPECT_EQ(combo->currentText(), QStringLiteral("English"));
    dlg.saveSettings();
    EXPECT_EQ(settings->language(), QStringLiteral("en_US"));
}

// The f8e8b216 "pure renaming" commit renamed constructor and callback
// parameters without updating the bodies, and six call sites silently
// switched to reading members instead of the arguments. These pins hold
// the argument-actually-lands behavior at each of those sites.
TEST_F(TestGuiWidgets, floatingWidgetContainerSizeFollowsTheArgument) {
    FloatingWidgetContainer container;
    ProbeWidget probe(&container);
    probe.pokeContainerSize(QSize(300, 150));
    EXPECT_EQ(probe.containerSize(), QSize(300, 150));
}

TEST_F(TestGuiWidgets, parentedOptionsPopupStillFloatsWhereItIsTold) {
    QWidget owner;
    owner.resize(200, 100);
    owner.show();
    FVOptionsPopup popup(&owner);
    const QPoint where(120, 90);
    popup.showAt(where);
    EXPECT_TRUE(popup.isWindow()) << "parenting turned the popup into an embedded child widget";
    EXPECT_TRUE(popup.windowFlags().testFlag(Qt::Popup)) << "the popup lost its Popup window flag";
    EXPECT_TRUE(popup.isVisible());
    EXPECT_EQ(popup.pos(), where);
    popup.hide();
    owner.hide();
}

TEST_F(TestGuiWidgets, populatedCensusCoversTheRemainingWidgetClasses) {
    const QMap<QString, int> before = coretest::widgetClasses();
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
        EXPECT_TRUE(QApplication::allWidgets().size() > beforeTotal);
    }
    const QMap<QString, int> after = coretest::widgetClasses();
    QStringList surplus;
    for(auto it = after.constBegin(); it != after.constEnd(); ++it) {
        const int left = it.value() - before.value(it.key(), 0);
        if(left > 0)
            surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
    }
    EXPECT_TRUE(surplus.isEmpty()) << qPrintable(QStringLiteral("widgets outliving their owner: ") +
                                                 surplus.join(", "));
}

TEST_F(TestGuiWidgets, proxyDestroyedAfterItsContainerDoesNotTouchTheOverlay) {
    const int baseline = QApplication::allWidgets().size();
    proxyOutlivesItsContainer<ZoomIndicatorOverlayProxy>();
    proxyOutlivesItsContainer<ImageInfoOverlayProxy>();
    proxyOutlivesItsContainer<FullscreenInfoOverlayProxy>();
    proxyOutlivesItsContainer<FloatingMessageProxy>();
    proxyOutlivesItsContainer<VideoControlsProxyWrapper>();
    // Overlays removed with deleteLater land only after the loop runs. A
    // fixed 100 ms was the old answer; this waits for the count itself, so a
    // deletion that never happens fails here instead of being outrun by a
    // slow machine. The round cap keeps it from hanging if the count is
    // stuck above the baseline for some other reason -- the QCOMPARE below
    // then reports the difference.
    for(int i = 0; i < 100 && QApplication::allWidgets().size() != baseline; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(QApplication::allWidgets().size(), baseline);
}

TEST_F(TestGuiWidgets, resizeDialogSeedsItsFieldsFromTheArgument) {
    ResizeDialog dialog(QSize(800, 600));
    EXPECT_EQ(dialog.newSize(), QSize(800, 600));
}

TEST_F(TestGuiWidgets, themeColorRowsSurviveJunk) {
    FreshSettings fresh;
    // the conf backend is per-platform: Linux reads QSettings(org, app),
    // Windows portable mode reads the redirected ini — write through the
    // SAME backend loadTheme reads, or the junk lands where nothing reads
    // it back
#ifdef Q_OS_WIN
    const QString confBase = QString::fromUtf8(qgetenv("QIMGV_CONF_DIR"));
    QSettings theme((confBase.isEmpty() ? QCoreApplication::applicationDirPath() + "/conf" : confBase) +
                        QStringLiteral("/theme.ini"),
                    QSettings::IniFormat);
#else
    QSettings theme(QCoreApplication::organizationName(), "theme");
#endif
    theme.beginGroup("Colors");
    theme.setValue("background", "banana"); // junk -> default
    theme.setValue("accent", "#ff0000");    // valid -> passes through
    theme.endGroup();
    theme.sync();
    settings->loadTheme();
    // Hex ints: the theme model stores the same value and the ctor is the
    // cheaper of the two. The string form also made clazy's
    // qcolor-from-literal abort on this file (KDE bug 528747).
    EXPECT_EQ(settings->colorScheme().background, QColor(0x1a1a1a));
    EXPECT_EQ(settings->colorScheme().accent, QColor(0xff0000));
}

TEST_F(TestGuiWidgets, userVisibleTextAssemblySweepsTheStates) {
    CurrentInfo info{};
    info.index = 1;
    info.fileCount = 3;
    info.fileName = "shot.png";
    info.imageSize = QSize(1920, 1080);
    info.fileSize = 3200000;
    QString size = QLocale().formattedDataSize(3200000, 1);

    // full house: extended info on, nothing toggled
    EXPECT_EQ(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                  QStringLiteral("Folder view")),
              QStringLiteral("[ 2/3 ]  shot.png  -  1920 x 1080  -  ") + size);
    // missing dimensions and size: the segments drop out whole
    info.imageSize = QSize(0, 0);
    info.fileSize = 0;
    EXPECT_EQ(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                  QStringLiteral("Folder view")),
              QStringLiteral("[ 2/3 ]  shot.png"));
    // extended info off: bare name — the position prefix lives in the same gate
    info.imageSize = QSize(1920, 1080);
    info.fileSize = 3200000;
    EXPECT_EQ(assembleWindowTitle(false, info, false, false, false, false, QStringLiteral("ImPulse"),
                                  QStringLiteral("Folder view")),
              QStringLiteral("shot.png"));
    // toggle states ride only when the info bar is not already showing them
    info.slideshow = info.shuffle = true;
    EXPECT_EQ(assembleWindowTitle(false, info, true, true, false, true, QStringLiteral("ImPulse"),
                                  QStringLiteral("Folder view")),
              QStringLiteral("shot.png - [slideshow] [shuffle] [zoom lock] [view lock]"));
    // edited marks the front of the assembled line
    info.slideshow = info.shuffle = false;
    info.edited = true;
    EXPECT_EQ(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                  QStringLiteral("Folder view")),
              QStringLiteral("* [ 2/3 ]  shot.png  -  1920 x 1080  -  ") + size);
    // the two degenerate surfaces
    EXPECT_EQ(assembleWindowTitle(true, info, false, false, true, false, QStringLiteral("ImPulse"),
                                  QStringLiteral("Folder view")),
              QStringLiteral("Folder view"));
    info.fileName.clear();
    info.edited = false;
    EXPECT_EQ(assembleWindowTitle(false, info, false, false, true, false, QStringLiteral("ImPulse"),
                                  QStringLiteral("Folder view")),
              QStringLiteral("ImPulse"));
}

TEST_F(TestGuiWidgets, videoControlsPlaybackPositionFollowsTheArgument) {
    FloatingWidgetContainer container;
    VideoControls controls(&container);
    controls.setMode(PLAYBACK_VIDEO); // exercise the video branch of the label
    controls.setPlaybackPosition(42);
    EXPECT_EQ(controls.playbackPosition(), 42);
    // a repeat is the dedup no-op, not a corruption
    controls.setPlaybackPosition(42);
    EXPECT_EQ(controls.playbackPosition(), 42);
}

TEST_F(TestGuiWidgets, wallpaperCommandDispatchesBySession) {
    // plasma
    auto [p, a] = Core::wallpaperCommand("plasma", "/pics/a b.png");
    EXPECT_EQ(p, QStringLiteral("plasma-apply-wallpaperimage"));
    EXPECT_EQ(a, QStringList{QStringLiteral("/pics/a b.png")}); // spaces verbatim
    // a quote in the filename cannot escape: there is no shell to escape into
    auto [p2, a2] = Core::wallpaperCommand("plasma", QStringLiteral("/pics/x\";y.png"));
    EXPECT_EQ(a2, QStringList{QStringLiteral("/pics/x\";y.png")});
    // gnome
    auto [p3, a3] = Core::wallpaperCommand("gnome", "/pics/a.png");
    EXPECT_EQ(p3, QStringLiteral("gsettings"));
    QStringList gnomeArgs{"set", "org.gnome.desktop.background", "picture-uri", "/pics/a.png"};
    EXPECT_EQ(a3, gnomeArgs);
    // unsupported session: empty program, the caller reports and bails
    auto [p4, a4] = Core::wallpaperCommand("twm", "/pics/a.png");
    EXPECT_TRUE(p4.isEmpty());
    EXPECT_TRUE(a4.isEmpty());
}

TEST_F(TestGuiWidgets, wallpaperMenuIconIsCompiledIntoResources) {
    EXPECT_TRUE(QFile::exists(":/res/icons/common/menuitem/wallpaper16.png"));
    EXPECT_TRUE(QFile::exists(":/res/icons/common/menuitem/wallpaper16@2x.png"));
}

// FlowLayout is the folder grid's placement engine and had no test at all:
// 278 lines deciding where every thumbnail goes, and which one the arrow keys
// land on. The navigation helpers are pure arithmetic over m_columns, so they
// are pinned here without a scene.
TEST_F(TestGuiWidgets, flowLayoutNavigationFollowsItsColumnCount) {
    // QGraphicsLayoutItem is abstract; the layout only ever asks an item for
    // its geometry, so a fixed-size stub is the whole contract it needs.
    struct StubItem : QGraphicsLayoutItem {
        void setGeometry(const QRectF &) override {
        }
        QSizeF sizeHint(Qt::SizeHint, const QSizeF &) const override {
            return QSizeF(10, 10);
        }
    };
    FlowLayout layout;
    QList<QGraphicsLayoutItem *> items;
    for(int i = 0; i < 10; ++i) {
        auto *item = new StubItem();
        layout.insertItem(i, item);
        items << item;
    }
    EXPECT_EQ(layout.count(), 10);
    // lay out into a width that fits four 10px items plus spacing
    layout.setSpacing(Qt::Horizontal, 0);
    layout.setGeometry(QRectF(0, 0, 40, 1000));
    ASSERT_TRUE(layout.columns() > 0);
    const int cols = layout.columns();

    // the first item has nothing above it and stays put
    EXPECT_EQ(layout.itemAbove(0), 0);
    // and the one a column below is exactly one column away
    EXPECT_EQ(layout.itemBelow(0), std::min(cols, 9));
    // columnOf is index mod columns
    for(int i = 0; i < 10; ++i)
        EXPECT_EQ(layout.columnOf(i), i % cols);
    // sameRow agrees with columnOf's quotient
    EXPECT_TRUE(layout.sameRow(0, cols - 1));
    EXPECT_TRUE(!layout.sameRow(0, cols));
    // out-of-range answers -1 rather than reading past the list
    EXPECT_EQ(layout.itemAbove(-1), -1);
    EXPECT_EQ(layout.itemAbove(10), -1);
    EXPECT_EQ(layout.columnOf(10), -1);

    layout.clear();
    EXPECT_EQ(layout.count(), 0);
    qDeleteAll(items);
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
