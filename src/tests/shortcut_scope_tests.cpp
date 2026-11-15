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
#include "gui/dialogs/settingsdialog.h"

class TestShortcutScope : public QObject {
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
        base = QDir(home.path()).filePath("scopes");
        alpha = QDir(base).filePath("alpha");
        QVERIFY(QDir().mkpath(alpha));
        writeImage(QDir(alpha).filePath("a.png"));
        writeImage(QDir(alpha).filePath("a.png"));
    }

    // The resolution order of the pointer funnel: the armed inspect layer, then
    // the global table. What is deliberately NOT here any more is a
    // caller-declared surface rank — the keyboard left this path entirely, and
    // a focused surface ranks by claiming its keys back (QShortcuts plus
    // ShortcutOverride) before dispatch ever reaches this code. That is pinned
    // where it lives: gridFocusAnswersThroughTheGridsTable and
    // windowDeliveryFiresTheProjectedAction.
    //
    // So this test now pins what the funnel still owns: the inspect layer
    // overrides the global table for its keys, and disarm gives them back.
    // Each layer is heard through its own signal, so the assertion is on what
    // FIRED, not on what returned. The arming runs through the real MW
    // observer and the viewer's fit state, hence this suite stays on
    // QApplication-under-offscreen rather than going headless.
    void pointerFunnelResolvesInspectThenGlobal() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        core.mainWindow()->fitOriginal(); // arms the inspect scope through the real observer
        QTRY_COMPARE_WITH_TIMEOUT(core.mainWindow()->viewer()->fitMode(), FIT_ORIGINAL, 5000);

        QSignalSpy next(actionManager->action("nextImage"), &QAction::triggered);
        QSignalSpy scroll(actionManager->action("scrollLeft"), &QAction::triggered);

        // armed: the document view's inspect variant answers Left
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("scrollLeft"));
        QVERIFY(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
        QCOMPARE(scroll.count(), 1);
        QCOMPARE(next.count(), 0);

        // disarmed, the same key is the document view's own walk
        core.mainWindow()->fitWindow();
        QTRY_COMPARE_WITH_TIMEOUT(core.mainWindow()->viewer()->fitMode(), FIT_WINDOW, 5000);
        QSignalSpy prev(actionManager->action("prevImage"), &QAction::triggered);
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));
        QVERIFY(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
        QCOMPARE(prev.count(), 1);
        QCOMPARE(scroll.count(), 1);
    }

    // the crop action wiring is live from initActions, but the crop panel is
    // built in setupFullUi — which runs on the window's own first show
    // (MW::shownOnce). showCropPanel used to dereference the not-yet-created
    // cropPanel/sidePanel pair before that (a guaranteed SIGSEGV on an early
    // trigger).
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
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("scrollLeft"));
        QVERIFY(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
        QCOMPARE(core.currentFilePath(), QDir(base).filePath("insB.png"));

        // back in a browse mode the same key navigates again
        core.mainWindow()->fitWindow();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->fitMode(), FIT_WINDOW, 5000);
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));
        QVERIFY(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(base).filePath("insA.png"), 10000);
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

    // ActionManager dispatches by name: invokeAction validates the name and
    // triggers the registered QAction. Two lists have to agree — the
    // registration in initActions and the connectAction calls in core.cpp —
    // and the disagreement is silent: a trigger into an action nobody
    // connected just does nothing, so the only symptom a user gets is "this
    // button does nothing". Walk every registered name and require the
    // QAction to exist, carry its own name, and be a stateless token (the
    // checkable/enabled face is contractually unused — see actionmanager.h).
    // Structural assertions only: triggering each name here is safe while
    // this suite connects no Core, but the day one is, the sweep would fire
    // real handlers (exit closes the process) — the behavioral leg lives in
    // invokeActionReportsAndReachesForEveryKindOfName below.
    void everyRegisteredActionIsATriggerableQAction() {
        const QList<QString> names = actionManager->actionList();
        QVERIFY2(!names.isEmpty(), "the action registry came back empty, the sweep proves nothing");
        // the size is printed even on success: a sweep over an empty or
        // accidentally tiny registry is green for the wrong reason
        qDebug() << "[action sweep] checking" << names.size() << "registered actions";

        QStringList broken;
        for(const QString &name : names) {
            const QAction *a = actionManager->action(name);
            if(!a)
                broken << name + QStringLiteral(": no QAction behind the name");
            else if(a->objectName() != name)
                broken << name + QStringLiteral(": objectName is ") + a->objectName();
            else if(a->isCheckable())
                broken << name + QStringLiteral(": registry actions are stateless tokens, not checkable");
        }
        QVERIFY2(broken.isEmpty(),
                 qPrintable(QStringLiteral("registry inconsistency (") + QString::number(names.size()) +
                            QStringLiteral(" actions checked):\n  ") + broken.join(QStringLiteral("\n  "))));
    }

    // invokeAction itself: it had never been entered despite eight production
    // call sites (context menu, action buttons, folder grid, viewer, exit).
    // A registered name has to report success AND reach its QAction; an
    // unknown name has to report failure; a "s:" name with no script behind
    // it has to report failure rather than pretend.
    void invokeActionReportsAndReachesForEveryKindOfName() {
        QSignalSpy nextImage(actionManager->action("nextImage"), &QAction::triggered);
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

    // The projection: every keyboard token in the global table must appear on
    // its action's shortcut list, because QShortcutMap dispatches from those
    // lists, not from the tables. Pointer tokens never project (no QKeySequence
    // can hold a wheel direction or a mouse button). "Enter" is the one token
    // that claims two sequences — Qt reads the main-row Enter as Return and
    // keeps Enter for the keypad, and both scancodes resolve to the same token.
    void defaultKeymapProjectsOntoTheRegistry() {
        const QMap<QString, QString> table = actionManager->allShortcuts();
        QVERIFY2(!table.isEmpty(), "the global table came back empty, the sweep proves nothing");
        for(auto it = table.constBegin(); it != table.constEnd(); ++it) {
            const QString &token = it.key();
            const bool pointer =
                (token == QStringLiteral("WheelUp") || token == QStringLiteral("WheelDown") ||
                 token == QStringLiteral("LMB") || token == QStringLiteral("RMB") ||
                 token == QStringLiteral("MiddleButton") || token == QStringLiteral("XButton1") ||
                 token == QStringLiteral("XButton2") || token.endsWith(QStringLiteral("_DoubleClick")));
            QAction *a = actionManager->action(it.value());
            QVERIFY2(a != nullptr, qPrintable(QStringLiteral("table names an unregistered action: ") + it.value()));
            if(pointer)
                continue;
            if(token == QStringLiteral("Enter")) {
                QVERIFY2(a->shortcuts().contains(QKeySequence(QStringLiteral("Return"))),
                         qPrintable(QStringLiteral("Enter did not project as Return for ") + it.value()));
                QVERIFY2(a->shortcuts().contains(QKeySequence(QStringLiteral("Enter"))),
                         qPrintable(QStringLiteral("Enter did not project as keypad Enter for ") + it.value()));
                continue;
            }
            QKeySequence seq = QKeySequence::fromString(token);
            QVERIFY2(seq.count() > 0, qPrintable(QStringLiteral("default token not Qt-parseable: ") + token));
            QVERIFY2(a->shortcuts().contains(seq),
                     qPrintable(QStringLiteral("token never projected onto its action: ") + token +
                                QStringLiteral(" -> ") + it.value()));
        }
    }

    // The scopes own their keys instead of layering — two live claims of one
    // key are ambiguous with NEITHER firing (dispatch_probe_tests), so the
    // grid's tokens are not in the global table at all (R12c keeps the
    // default tables disjoint, dropGlobalEntriesClaimedByScopes the live
    // ones) and the resolver asks the focused grid's table first. This pins
    // the resolver's ranking; the delivery leg lives in
    // returnKeyMeansFolderViewInTheDocumentAndGridOpenInTheGrid.
    void gridFocusAnswersThroughTheGridsTable() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        auto *proxy = core.mainWindow()->getFolderView();
        proxy->init(); // materializes the real FolderView (and its grid)
        auto *grid = proxy->findChild<FolderGridView *>();
        QVERIFY2(grid != nullptr, "no FolderGridView under the folder view");

        // The document view owns Right/PgDown/PgUp now, and the grid owns the
        // same keys while it holds focus. The resolution follows focus, not a
        // central de-claim: the grid's table is asked first.
        QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("nextImage"));

        QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusIn));
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("gridSelectNext"));
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown)), QStringLiteral("gridPageDown"));

        QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("nextImage"));
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown)), QStringLiteral("nextImage"));
    }

    // The armed inspect layer replaces the global claims for its keys and
    // gives them back on disarm — the projection's answer to "fit original
    // pans with the arrows instead of changing images".
    void armedInspectSwapsTheProjection() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));

        core.mainWindow()->fitOriginal(); // arms the inspect scope through the real observer
        QTRY_COMPARE_WITH_TIMEOUT(core.mainWindow()->viewer()->fitMode(), FIT_ORIGINAL, 5000);
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("scrollLeft"));

        core.mainWindow()->fitWindow(); // disarms
        QTRY_COMPARE_WITH_TIMEOUT(core.mainWindow()->viewer()->fitMode(), FIT_WINDOW, 5000);
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));
    }

    // The end-to-end leg the migration stands on: a window-delivered key fires
    // the projected action through QShortcutMap. sendEvent-based legs bypass
    // the map entirely (they exercise the doors and the manual funnel), so
    // this is the one slot that proves the projection is live and registered.
    void windowDeliveryFiresTheProjectedAction() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        auto *mw = core.mainWindow();
        mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(mw));
        mw->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(mw));

        QSignalSpy fullscreen(actionManager->action("toggleFullscreen"), &QAction::triggered);
        QSignalSpy save(actionManager->action("save"), &QAction::triggered);
        QVERIFY(fullscreen.isValid() && save.isValid());

        coretest::deliverWindowKey(mw, Qt::Key_F);
        QCOMPARE(fullscreen.count(), 1); // F: the projected binding fired through the map

        coretest::deliverWindowKey(mw, Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(save.count(), 1); // Ctrl+S: the chord reached its action too
    }

    // One physical key, two meanings, both delivered for real: Return in the
    // document view is folderView (the toggle belongs to the view it
    // departs), and the same key in the grid is gridOpen. This is the leg
    // 7b20e6e6 lost: it retired the central de-claim while Enter still lived
    // in both the global and the grid tables, and two live claims of one key
    // fire NOTHING (dispatch_probe_tests) — Enter and Backspace went silent
    // in the folder view under a green suite, because the only other grid
    // delivery test (crop_tests) runs a standalone grid with no window
    // surface attached and never sees the global claim. Here both claims'
    // carriers are live at once: the window surface carries the global
    // projection, the grid surface carries its own.
    void returnKeyMeansFolderViewInTheDocumentAndGridOpenInTheGrid() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        auto *mw = core.mainWindow();
        mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(mw));
        mw->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(mw));

        QSignalSpy folderView(actionManager->action("folderView"), &QAction::triggered);
        QSignalSpy gridOpen(actionManager->action("gridOpen"), &QAction::triggered);
        QVERIFY(folderView.isValid() && gridOpen.isValid());

        // leg one: the document view is on screen and its viewer holds focus
        // — the doc scope's claim is live, the grid's is not, the global
        // table carries no Enter at all
        mw->viewer()->setFocus();
        QVERIFY2(mw->viewer()->hasFocus() || QApplication::focusWidget() == mw,
                 "no focus in the document view to carry the doc claim");
        coretest::deliverWindowKey(mw, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(folderView.count(), 1, 5000);
        QCOMPARE(gridOpen.count(), 0);

        // that very delivery switched to the folder view; the grid surfaces
        auto *proxy = mw->getFolderView();
        proxy->init();
        auto *grid = proxy->findChild<FolderGridView *>();
        QVERIFY2(grid != nullptr, "no FolderGridView under the folder view");
        QTRY_VERIFY_WITH_TIMEOUT(grid->isVisible(), 5000);
        grid->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(grid->hasFocus(), 5000);
        folderView.clear();
        gridOpen.clear();

        // leg two: the grid holds focus — gridOpen fires alone, and the
        // toggle does not re-fire (an ambiguous pair would fire NEITHER)
        coretest::deliverWindowKey(mw, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(gridOpen.count(), 1, 5000);
        QCOMPARE(folderView.count(), 0);
    }

    // the inspect scope carries user overrides: while armed, an override
    // replaces the inspect default for that key — the armed chain reads the
    // layer, not the seed defaults. The table is snapshotted and restored;
    // the singleton is process-wide.
    void inspectShortcutsCarryUserOverrides() {
        const QMap<QString, QString> inspectSnapshot = actionManager->allInspectShortcuts();
        actionManager->addInspectShortcut(QStringLiteral("Left"), QStringLiteral("nextImage"));

        QSignalSpy next(actionManager->action("nextImage"), &QAction::triggered);
        QSignalSpy scroll(actionManager->action("scrollLeft"), &QAction::triggered);
        actionManager->setInspectMode(true);
        QVERIFY(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
        QCOMPARE(next.count(), 1);
        QCOMPARE(scroll.count(), 0); // the scrollLeft default never fired
        actionManager->setInspectMode(false);

        actionManager->removeAllInspectShortcuts();
        for(auto it = inspectSnapshot.constBegin(); it != inspectSnapshot.constEnd(); ++it)
            actionManager->addInspectShortcut(it.key(), it.value());
    }

    void pagingKeysHaveNamesAndDefaults() {
        // a press with no name in the scancode table cannot reach the action
        // layer at all; a name with no default never fires for a config that
        // predates the binding — pin both halves, plus the lookup itself
        QVERIFY(inputMap->keys().value(112) == "PgUp");
        QVERIFY(inputMap->keys().value(117) == "PgDown");
        // PgUp/PgDown are the document view's binding — the token lookup has
        // to reach that table, not only the global one
        QVERIFY(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageUp)) == "prevImage");
        QVERIFY(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown)) == "nextImage");
        QVERIFY(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown))));
        QVERIFY(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageUp))));
        QVERIFY(actionManager->actionForShortcut("F13").isEmpty());
    }

    // The one-key-one-surface law holds against the editor and the config,
    // not just the defaults: a token a scope table claims cannot enter the
    // global table. addShortcut is the editor's insert door; initShortcuts
    // runs the same drop after readShortcuts for configs saved by older
    // releases (which carry the pre-split global table, Enter included).
    // A dropped entry must not reach the projection either — the ambiguity
    // never gets to QShortcutMap.
    void globalTableRefusesTokensOwnedByScopes() {
        const QMap<QString, QString> before = actionManager->allShortcuts();

        // Enter: the grid's and the doc's; Right: the doc's and the grid's
        actionManager->addShortcut(QStringLiteral("Enter"), QStringLiteral("exit"));
        actionManager->addShortcut(QStringLiteral("Right"), QStringLiteral("exit"));
        QVERIFY(!actionManager->allShortcuts().contains(QStringLiteral("Enter")));
        QVERIFY(!actionManager->allShortcuts().contains(QStringLiteral("Right")));
        QVERIFY(!actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_Return)));
        QVERIFY(!actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_Right)));

        // a token nobody scopes still lands — the door is narrow, not shut
        actionManager->addShortcut(QStringLiteral("F13"), QStringLiteral("exit"));
        QVERIFY(actionManager->allShortcuts().contains(QStringLiteral("F13")));
        QVERIFY(actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_F13)));
        actionManager->removeShortcut(QStringLiteral("F13"));

        QCOMPARE(actionManager->allShortcuts(), before);
    }

    // resetDefaults restores every table — the document table joined the
    // family in the view split and the reset left it behind, so the keymap
    // editor's reset button could not undo a document-scope rebinding
    void resetDefaultsRestoresEveryTable() {
        const QMap<QString, QString> globalBefore = actionManager->allShortcuts();
        const QMap<QString, QString> docBefore = actionManager->allDocShortcuts();
        const QMap<QString, QString> gridBefore = actionManager->allGridShortcuts();
        const QMap<QString, QString> inspectBefore = actionManager->allInspectShortcuts();

        actionManager->addShortcut(QStringLiteral("F13"), QStringLiteral("exit"));
        actionManager->addDocShortcut(QStringLiteral("F14"), QStringLiteral("crop"));
        actionManager->addGridShortcut(QStringLiteral("F15"), QStringLiteral("gridOpen"));
        actionManager->addInspectShortcut(QStringLiteral("F16"), QStringLiteral("scrollLeft"));
        actionManager->resetDefaults();

        QCOMPARE(actionManager->allShortcuts(), globalBefore);
        QCOMPARE(actionManager->allDocShortcuts(), docBefore);
        QCOMPARE(actionManager->allGridShortcuts(), gridBefore);
        QCOMPARE(actionManager->allInspectShortcuts(), inspectBefore);
    }

    // Liveness flags are consumed by the resolver only — no projection
    // function reads gridActive/documentActive (grid is gated by Qt's
    // WidgetWithChildrenShortcut context on the focused subtree, the document
    // the same on DocumentWidget). The rebuilds those setters used to run
    // were the de-claim era's leftover. This pins the OBSERVABLE consequence:
    // after a real view switch, each scope's keys still mean their scope's
    // action, with no projection rebuild on the switch itself.
    void viewSwitchKeepsBothScopesLiveWithoutRebuilding() {
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        auto *mw = core.mainWindow();

        // document view: the resolver answers with the document table
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("nextImage"));
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Return)), QStringLiteral("folderView"));

        // switch to the folder view (the real observer path, not a direct call)
        QVERIFY(actionManager->invokeAction(QStringLiteral("folderView")));
        auto *proxy = mw->getFolderView();
        proxy->init();
        auto *grid = proxy->findChild<FolderGridView *>();
        QVERIFY2(grid != nullptr, "no FolderGridView under the folder view");
        grid->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(grid->hasFocus(), 5000);

        // the grid now answers for its own keys — no rebuild was needed for
        // this to be true, the grid's QShortcuts were already attached
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("gridSelectNext"));
        QCOMPARE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Return)), QStringLiteral("gridOpen"));

        // and the document table is still intact underneath — the switch
        // did not clear it (the old rebuild did clear and recreate it)
        QVERIFY(actionManager->allDocShortcuts().values().contains(QStringLiteral("nextImage")));
        QVERIFY(actionManager->allDocShortcuts().values().contains(QStringLiteral("folderView")));
    }

    // commit() is the one write path: whatever door the edit came through,
    // the disjointness law and the projection rebuild run after it. This
    // pins the property from BOTH sides — a scope insert that collides with
    // a global token, and a global insert that collides with a scope token —
    // because the old code only guarded the second direction (and only after
    // config load, not after every editor insert).
    void everyWriteDoorEnforcesDisjointness() {
        const QMap<QString, QString> globalBefore = actionManager->allShortcuts();
        const QMap<QString, QString> docBefore = actionManager->allDocShortcuts();
        const QMap<QString, QString> gridBefore = actionManager->allGridShortcuts();

        // direction 1: a scope insert of a token the global table holds.
        // "openSettings" is P, and P is also a document binding? — pick a
        // token known to be global-only, then bind it into the document
        // scope and watch the global copy disappear.
        QVERIFY(actionManager->allShortcuts().contains(QStringLiteral("Del")));
        actionManager->addDocShortcut(QStringLiteral("Del"), QStringLiteral("crop"));
        QVERIFY2(!actionManager->allShortcuts().contains(QStringLiteral("Del")),
                 "a document insert left the global claim live — the pair is ambiguous");

        // direction 2: the reverse (the door addShortcut already had)
        actionManager->addShortcut(QStringLiteral("Right"), QStringLiteral("exit"));
        QVERIFY(!actionManager->allShortcuts().contains(QStringLiteral("Right")));

        // the projection agrees with the tables — no stale claim survives
        QVERIFY(!actionManager->action("moveToTrash")->shortcuts().contains(QKeySequence(Qt::Key_Delete)));
        QVERIFY(!actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_Right)));

        // restore through the same doors
        actionManager->removeAllDocShortcuts(QStringLiteral("crop"));
        actionManager->removeAllShortcuts(QStringLiteral("exit"));
        actionManager->resetDefaults();
        QCOMPARE(actionManager->allShortcuts(), globalBefore);
        QCOMPARE(actionManager->allDocShortcuts(), docBefore);
        QCOMPARE(actionManager->allGridShortcuts(), gridBefore);
    }

    // The settings editor's conflict warning asks the SAME scope walk the
    // disjointness law uses — one implementation, so a table added later is
    // visible to both by construction. The hand-written three-table version
    // this replaced had forgotten the document table, so a document-scope
    // collision produced no warning: the user's key silently landed in two
    // places. "Enter" is the sharpest probe — it is the one token whose
    // spelling ("Enter") differs from what the document/grid tables hold
    // ("Enter" as Return+keypad), so a naive string compare reports nothing.
    void scopeClaimsNameEveryTableHoldingTheKey() {
        // Right lives in the document table and the grid table
        const QStringList right = actionManager->scopeClaimsForToken(QStringLiteral("Right"));
        QVERIFY2(right.contains(QStringLiteral("document")), qPrintable(right.join(", ")));
        QVERIFY2(right.contains(QStringLiteral("folder view")), qPrintable(right.join(", ")));
        // Left additionally lives in inspect
        const QStringList left = actionManager->scopeClaimsForToken(QStringLiteral("Left"));
        QVERIFY2(left.contains(QStringLiteral("document")), qPrintable(left.join(", ")));
        QVERIFY2(left.contains(QStringLiteral("folder view")), qPrintable(left.join(", ")));
        QVERIFY2(left.contains(QStringLiteral("inspect")), qPrintable(left.join(", ")));
        // Enter: document (folderView) and grid (gridOpen) — the token that
        // exposed the missing table, and whose sequence expansion is the
        // reason a spelling comparison would have missed it
        const QStringList enter = actionManager->scopeClaimsForToken(QStringLiteral("Enter"));
        QVERIFY2(enter.contains(QStringLiteral("document")), qPrintable(enter.join(", ")));
        QVERIFY2(enter.contains(QStringLiteral("folder view")), qPrintable(enter.join(", ")));
        // a key nobody scopes claims nothing
        QVERIFY(actionManager->scopeClaimsForToken(QStringLiteral("F13")).isEmpty());
        // pointer tokens never ride QShortcutMap and are exempt by design
        QVERIFY(actionManager->scopeClaimsForToken(QStringLiteral("WheelUp")).isEmpty());
    }

    // The context menu prints a binding hint per item, and it reads the
    // action through shortcutForAction. When 7b20e6e6 moved save/crop/the
    // edit verbs/setWallpaper into the document table, this reader kept
    // reading only the global one — every document-view menu item that had
    // a chord lost its hint, silently, because nothing asserted the hint.
    // The menu only ever appears on ViewerWidget, so the document table is
    // the right answer for it.
    void contextMenuHintsReadTheDocumentTable() {
        // commands the view split moved out of the global table
        for(const QString &action : {QStringLiteral("save"), QStringLiteral("crop"), QStringLiteral("setWallpaper"),
                                     QStringLiteral("rotateLeft"), QStringLiteral("fitWindow")}) {
            QVERIFY2(!actionManager->allShortcuts().values().contains(action),
                     qPrintable(QStringLiteral("expected no global binding for ") + action +
                                " — this action is documented as document-scope only"));
            QVERIFY2(!actionManager->shortcutForAction(action).isEmpty(),
                     qPrintable(QStringLiteral("the menu hint for ") + action +
                                " is blank — the reader lost the document table"));
        }
        // and the answer is the document table's own token, not a stale one
        QCOMPARE(actionManager->shortcutForAction(QStringLiteral("setWallpaper")), QStringLiteral("W"));
        QCOMPARE(actionManager->shortcutForAction(QStringLiteral("save")), QStringLiteral("Ctrl+S"));
        // a global-table action still answers from there
        QCOMPARE(actionManager->shortcutForAction(QStringLiteral("openSettings")), QStringLiteral("P"));
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
        const int docBefore = actionManager->allDocShortcuts().size();

        SettingsDialog dialog;
        dialog.readSettings();
        auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("shortcutsTableWidget"));
        QVERIFY2(table, "the shortcuts table did not materialize");
        int headers = 0, global = 0, grid = 0, inspect = 0, document = 0;
        for(int r = 0; r < table->rowCount(); r++) {
            QTableWidgetItem *item = table->item(r, 0);
            const QString tag = item ? item->data(Qt::UserRole).toString() : QString();
            if(tag.isEmpty())
                headers++;
            else if(tag == QLatin1String("all"))
                global++;
            else if(tag == QLatin1String("grid"))
                grid++;
            else if(tag == QLatin1String("document"))
                document++;
            else if(tag == QLatin1String("inspect"))
                inspect++;
        }
        QCOMPARE(headers, 4); // global, document, folder view, inspect
        QCOMPARE(global, globalBefore);
        QCOMPARE(grid, gridBefore);
        QCOMPARE(document, docBefore);
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
        QCOMPARE(actionManager->allDocShortcuts().size(), docBefore);
        QCOMPARE(actionManager->allGridShortcuts().size(), gridBefore + 1);
        QCOMPARE(actionManager->allGridShortcuts().value(QStringLiteral("F7")), QStringLiteral("gridSelectLast"));

        // put the manager back the way the rest of the suite expects it
        actionManager->removeAllGridShortcuts();
        for(auto it = gridSnapshot.constBegin(); it != gridSnapshot.constEnd(); ++it)
            actionManager->addGridShortcut(it.key(), it.value());
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
        TestShortcutScope t;
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    const QMetaObject *mo = &TestShortcutScope::staticMetaObject;
    QStringList testSlots;
    for(int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        const QByteArray name = m.name();
        if(m.methodType() == QMetaMethod::Slot && m.access() == QMetaMethod::Private && m.parameterCount() == 0 &&
           name != "initTestCase" && name != "cleanupTestCase" && name != "init" && name != "cleanup")
            testSlots << QString::fromUtf8(name);
    }
    int failures = 0, consecutiveAbnormal = 0;
    for(const QString &slot : std::as_const(testSlots)) {
        fflush(nullptr); // fork would duplicate whatever is buffered in stdout
        pid_t pid = fork();
        if(pid == 0) {
            coretest::setChildCoreLimit();
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestShortcutScope t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        // a crashed child loses its buffered output entirely — without naming
        // the slot here, the evidence is a red ctest with nothing in the log
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "shortcut_scope_tests: slot '%s' exited abnormally (exit=%d, sig=%d)\n",
                    slot.toUtf8().constData(), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        // the crash-burst guard: three consecutive abnormal slots mean a
        // fixture-wide failure — the remaining slots carry no new evidence,
        // and each was streaming a full core through systemd-coredump
        consecutiveAbnormal = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : consecutiveAbnormal + 1;
        if(consecutiveAbnormal == 3)
            break;
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "shortcut_scope_tests.moc"
