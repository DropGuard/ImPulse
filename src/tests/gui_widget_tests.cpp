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
#include <QtTest/QTest>
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
#include "slot_harness.h"

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

class TestGuiWidgets : public QObject {
    Q_OBJECT
    IsolatedHome home;
    coretest::AppSingletons singletons; // the app's process-wide state, by construction

    // a directory of this test's own, never shared with a sibling
    QString uniqueDir(const QString &tag) {
        const QString dir =
            QDir(home.path()).filePath(tag + QStringLiteral("-%1").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    // selection() returns QList by value: capturing it in a named local keeps
    // clazy's detaching-temporary quiet AND avoids a real detach per .last()

  private slots:
    // The fixtures are machinery, and machinery that only writes is machinery
    // that leaks. IsolatedHome and FreshSettings both change process-wide state
    // (env vars, the application identity, the settings pointer); each has to put
    // back exactly what it took, or the next test runs against a config path
    // nobody expects. Today every IsolatedHome is a suite member that outlives
    // everything, so these asserts hold trivially -- which is why they are here:
    // the first scoped use is the one that would otherwise leak silently.
    void fixturesRestoreTheProcessStateTheyChange() {
        const QByteArray homeBefore = qgetenv("HOME");
        const QByteArray cacheBefore = qgetenv("XDG_CACHE_HOME");
        const QByteArray configBefore = qgetenv("XDG_CONFIG_HOME");
        const QString appBefore = QCoreApplication::applicationName();
        const QString orgBefore = QCoreApplication::organizationName();
        {
            IsolatedHome scoped;
            QVERIFY2(scoped.path() != homeBefore, "the fixture did not redirect HOME");
        }
        QCOMPARE(qgetenv("HOME"), homeBefore);
        QCOMPARE(qgetenv("XDG_CACHE_HOME"), cacheBefore);
        QCOMPARE(qgetenv("XDG_CONFIG_HOME"), configBefore);
        QCOMPARE(QCoreApplication::applicationName(), appBefore);
        QCOMPARE(QCoreApplication::organizationName(), orgBefore);

        {
            FreshSettings scoped;
            QVERIFY2(QCoreApplication::applicationName() != appBefore,
                     "the fixture did not swap the application identity");
        }
        QCOMPARE(QCoreApplication::applicationName(), appBefore);
        QVERIFY2(::settings != nullptr, "the fixture did not restore the settings pointer");
    }

    void initTestCase() {
        QVERIFY(home.path() != nullptr);
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

    // What a widget destructor forgets cannot be seen by the app: the tree is
    // never torn down, so a leaked child stays reachable from Qt's own widget
    // registry and there is nothing for LSan to call unreachable. Destroying each
    // suspect here is the measurement — the widget class names come from
    // allWidgets(), and the same scopes are what the sanitizer tree uses to size
    // the non-widget heap (a scene, a timeline, a pixmap: not widgets, so not
    // visible to the assertion below and only visible to LSan once unreachable).
    void destroyingAWidgetFreesEveryWidgetItCreated() {
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
                 qPrintable(QStringLiteral("widgets outliving their parent: ") + surplus.join(", ")));
    }

    // The pending-surface registry's membership, checked against the REAL
    // OverlayHost rather than a fake: only the two surfaces that hold typed-
    // but-unsubmitted input belong in it. The save prompt is the projection
    // of an uncommitted edit (Image::isEdited is the lock's condition already)
    // and a crop selection that has not been applied changes nothing — both
    // were answering a question someone else already answers.
    //
    // This is the pin the old registry lacked entirely: every prior test drove
    // a fake's flag, so a surface that never registered looked identical to
    // one that did.
    void pendingRegistryHoldsOnlyTypedButUnsubmittedInput() {
        const QString dir = uniqueDir("pendingmembers");
        const QString img = QDir(dir).filePath("a.png");
        QVERIFY(makeImageFile(img, QSize(8, 8)));
        Core core;
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        auto *overlays = core.mainWindow()->overlays();
        QVERIFY(overlays);

        // nothing is open yet
        QVERIFY(!overlays->hasPendingSurface());

        // the save prompt: visible, but NOT a pending surface — it reports an
        // edit the state leg already covers
        settings->setShowSaveOverlay(true);
        overlays->showSaveOverlay();
        auto *saveOverlay = core.mainWindow()->findChild<SaveConfirmOverlay *>();
        QVERIFY(saveOverlay && saveOverlay->isVisible());
        QVERIFY2(!overlays->hasPendingSurface(), "the save prompt registered as pending: it would answer the "
                                                 "isEdited() question twice, and a stale entry would outlive the edit");
        // the contract is structural, not just observed: these two surfaces
        // must not even BE IPendingSurface — a registry entry can be removed,
        // but an interface cast is what would let a future setup re-register
        // them without touching this test
        QVERIFY2(dynamic_cast<IPendingSurface *>(saveOverlay) == nullptr,
                 "SaveConfirmOverlay still implements IPendingSurface");
        overlays->hideSaveOverlay();

        // the copy menu: typed-but-unsubmitted target — pending while open
        overlays->triggerCopyOverlay();
        auto *copyOverlay = core.mainWindow()->findChild<CopyOverlay *>();
        QVERIFY(copyOverlay && copyOverlay->isVisible());
        QVERIFY2(overlays->hasPendingSurface(), "the copy menu did not register: navigation would walk "
                                                "away from a target the user has not chosen");
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(copyOverlay, &esc);
        QTRY_VERIFY(!copyOverlay->isVisible());
        QVERIFY(!overlays->hasPendingSurface());

        // the rename field: same class, same answer
        overlays->toggleRenameOverlay(QStringLiteral("a.png"));
        auto *renameOverlay = core.mainWindow()->findChild<RenameOverlay *>();
        QVERIFY(renameOverlay && renameOverlay->isVisible());
        QVERIFY2(overlays->hasPendingSurface(), "the rename field did not register: a half-typed name "
                                                "would be dropped by a walk");
        QApplication::sendEvent(renameOverlay, &esc);
        QTRY_VERIFY(!renameOverlay->isVisible());
        QVERIFY(!overlays->hasPendingSurface());

        // the crop surface: pending only once the crop is APPLIED, and then
        // through isEdited(), not through the registry
        core.toggleCropPanel();
        QVERIFY(core.mainWindow()->crop()->isActive());
        QVERIFY2(!overlays->hasPendingSurface(), "the crop surface registered as pending: an unapplied "
                                                 "selection loses nothing when the walk moves on");
        auto *cropOverlay = core.mainWindow()->findChild<CropOverlay *>();
        QVERIFY(cropOverlay);
        cropOverlay->selectAll();
        QVERIFY2(dynamic_cast<IPendingSurface *>(cropOverlay) == nullptr,
                 "CropOverlay still implements IPendingSurface");
        QVERIFY2(!overlays->hasPendingSurface(),
                 "a selection alone changed the registry: nothing is applied yet, so nothing can be lost");
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

    // ---- the drag-drop surface. The handlers are invoked DIRECTLY through a
    // using-exposed subclass: their contracts (enter accepts, move hit-tests
    // and emits draggedOver(index), drop emits droppedInto(mimeData, source,
    // landing index)) are what we pin, and QApplication::notify's drag-protocol
    // gating (a synthetic Drop without an accepted DragMove in the drag
    // manager's state is refused delivery) isQt plumbing this arc must not
    // depend on. Positions are computed from the live scene (item scenePos
    // mapped to viewport coords), not hardcoded.

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
            QVERIFY(QApplication::allWidgets().size() > beforeTotal);
        }
        const QMap<QString, int> after = coretest::widgetClasses();
        QStringList surplus;
        for(auto it = after.constBegin(); it != after.constEnd(); ++it) {
            const int left = it.value() - before.value(it.key(), 0);
            if(left > 0)
                surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
        }
        QVERIFY2(surplus.isEmpty(), qPrintable(QStringLiteral("widgets outliving their owner: ") + surplus.join(", ")));
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
        // Overlays removed with deleteLater land only after the loop runs. A
        // fixed 100 ms was the old answer; this waits for the count itself, so a
        // deletion that never happens fails here instead of being outrun by a
        // slow machine. The round cap keeps it from hanging if the count is
        // stuck above the baseline for some other reason -- the QCOMPARE below
        // then reports the difference.
        for(int i = 0; i < 100 && QApplication::allWidgets().size() != baseline; ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCOMPARE(QApplication::allWidgets().size(), baseline);
    }

    void resizeDialogSeedsItsFieldsFromTheArgument() {
        ResizeDialog dialog(QSize(800, 600));
        QCOMPARE(dialog.newSize(), QSize(800, 600));
    }

    void themeColorRowsSurviveJunk() {
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
        QCOMPARE(settings->colorScheme().background, QColor(0x1a1a1a));
        QCOMPARE(settings->colorScheme().accent, QColor(0xff0000));
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

    // a menuitem icon can exist on disk and still render blank: bf65eac2 added
    // the wallpaper icon files and the setIconPath call but forgot the
    // resources.qrc entry, so the resource did not exist at runtime. Pin the
    // compiled resource, not the file on disk.
    void wallpaperMenuIconIsCompiledIntoResources() {
        QVERIFY(QFile::exists(":/res/icons/common/menuitem/wallpaper16.png"));
        QVERIFY(QFile::exists(":/res/icons/common/menuitem/wallpaper16@2x.png"));
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // One process per test function — the same rationale as engine_stack_main's
    // fork per class, taken one level down: this suite builds widgets that make
    // QtGui spawn and clockjoin-join its own internal pooled threads, and each
    // join leaves a registry-live pthread_t behind (TSan does not intercept
    // clockjoin — LLVM #239326).
    //
    // The fixture is constructed INSIDE the child, not before the fork. Built
    // once in the parent and inherited, every child would share a settings
    // object whose tmpDir names the parent's temp directory; the first child to
    // finish destroys it on its way out (IsolatedHome's destructor), and every
    // later child runs with a tmpDir that does not exist.
    if(argc > 1) {
        TestGuiWidgets t;
        return slot_harness::runFiltered(t, argc, argv) != 0 ? 1 : 0;
    }
    return slot_harness::run_units_one_process_each(
        argc, argv, slot_harness::testSlots(&TestGuiWidgets::staticMetaObject), "gui_widget");
}

#include "gui_widget_tests.moc"
