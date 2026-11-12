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
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

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
#include "gui/overlays/cropoverlay.h"
#include "gui/overlays/floatingmessageproxy.h"
#include "gui/overlays/imageinfooverlayproxy.h"
#include "gui/overlays/saveconfirmoverlay.h"
#include "gui/overlays/videocontrols.h"
#include "gui/overlays/zoomindicatoroverlay.h"
#include "gui/panels/croppanel/croppanel.h"
#include "gui/panels/mainpanel/mainpanel.h"
#include "gui/viewers/imageviewerv2.h"
#include "settings.h"
#include "testhelpers.h"
#include "utils/actions.h"
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

class TestableFolderGridView : public FolderGridView {
  public:
    using FolderGridView::dragEnterEvent;
    using FolderGridView::dragLeaveEvent;
    using FolderGridView::dragMoveEvent;
    using FolderGridView::dropEvent;
    using FolderGridView::keyPressEvent;
};

// The widgets in the grid's own visual order, which is the order an
// IDirectoryView index means.
inline QList<ThumbnailWidget *> orderedThumbnails(FolderGridView &grid) {
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

inline QPoint dropPosOverThumbnail(FolderGridView &grid, int index) {
    const QList<ThumbnailWidget *> widgets = orderedThumbnails(grid);
    if(widgets.count() <= index) {
        QTest::qFail(qPrintable(QStringLiteral("no thumbnail #%1").arg(index)), __FILE__, __LINE__);
        return {};
    }
    const QPointF center = widgets.at(index)->mapToScene(widgets.at(index)->boundingRect().center());
    return grid.mapFromScene(center);
}

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
    int lastSelection(FolderGridView &grid) {
        const QList<int> sel = grid.selection();
        return sel.last();
    }

    static void pressGridKey(TestableFolderGridView &grid, const QString &keyName, Qt::Key key,
                             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QKeyEvent press = coretest::keyPressFor(key, keyName, modifiers);
        grid.keyPressEvent(const_cast<QKeyEvent *>(&press));
    }

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
        QTest::qWait(100); // any deferred deletion must have landed
        // nothing outlives the container it was made in: the tree reclaimed all
        // five overlays with their containers, and no proxy stashed a widget
        QCOMPARE(QApplication::allWidgets().size(), baseline);
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

    void resizeDialogSeedsItsFieldsFromTheArgument() {
        ResizeDialog dialog(QSize(800, 600));
        QCOMPARE(dialog.newSize(), QSize(800, 600));
    }

    // The Save As prefill for an edited image: the desktop's numbered-variant
    // pattern, so the quick path writes a copy and the original survives.
    void saveAsSuggestsTheFirstFreeNumberedVariant() {
        const QString dir = uniqueDir("variant");
        QCOMPARE(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test.jpg")),
                 QDir(dir).filePath("test (1).jpg"));

        QVERIFY(QFile(QDir(dir).filePath("test (1).jpg")).open(QIODevice::WriteOnly));
        QCOMPARE(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test.jpg")),
                 QDir(dir).filePath("test (2).jpg"));

        QVERIFY(QFile(QDir(dir).filePath("test (2).jpg")).open(QIODevice::WriteOnly));
        QCOMPARE(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test.jpg")),
                 QDir(dir).filePath("test (3).jpg"));

        // suffix-less and multi-dot names keep their shape
        QCOMPARE(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test")), QDir(dir).filePath("test (1)"));
        QCOMPARE(FileOperations::nextFreeVariantPath(QDir(dir).filePath("archive.tar.jpg")),
                 QDir(dir).filePath("archive.tar (1).jpg"));
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

    void themeColorRowsSurviveJunk() {
        FreshSettings fresh;
        QSettings theme(QCoreApplication::organizationName(), "theme");
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
        return QTest::qExec(&t, argc, argv);
    }
    const QMetaObject *mo = &TestGuiWidgets::staticMetaObject;
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
            TestGuiWidgets t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "gui_widget: slot '%s' exited abnormally (exit=%d, sig=%d)\n", slot.toUtf8().constData(),
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "gui_widget_tests.moc"
