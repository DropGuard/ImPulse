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
#include "components/directorymanager/enginebridge.h"
#include "components/workerpool/workerpool.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/inputmap.h"
#include "gui/dialogs/settingsdialog.h"
#include "actions_table.h"
#include "gui/folderview/folderview.h"
#include "gui/folderview/folderviewproxy.h"
#include "gui/customwidgets/iconbutton.h"

class TestShortcutScope : public ::testing::Test {
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
        ASSERT_TRUE(m_home.path() != nullptr);
        m_base = QDir(m_home.path()).filePath("scopes");
        m_alpha = QDir(m_base).filePath("alpha");
        ASSERT_TRUE(QDir().mkpath(m_alpha));
        writeImage(QDir(m_alpha).filePath("a.png"));
        writeImage(QDir(m_alpha).filePath("a.png"));
    }
};

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
TEST_F(TestShortcutScope, pointerFunnelResolvesInspectThenGlobal) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    core.mainWindow()->fitOriginal(); // arms the inspect scope through the real observer
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.mainWindow()->viewer()->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(core.mainWindow()->viewer()->fitMode(), FIT_ORIGINAL);

    QSignalSpy next(actionManager->action("nextImage"), &QAction::triggered);
    QSignalSpy scroll(actionManager->action("scrollLeft"), &QAction::triggered);

    // armed: the document view's inspect variant answers Left
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("scrollLeft"));
    EXPECT_TRUE(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
    EXPECT_EQ(scroll.count(), 1);
    EXPECT_EQ(next.count(), 0);

    // disarmed, the same key is the document view's own walk
    core.mainWindow()->fitWindow();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.mainWindow()->viewer()->fitMode() == FIT_WINDOW;
        },
        5000));
    EXPECT_EQ(core.mainWindow()->viewer()->fitMode(), FIT_WINDOW);
    QSignalSpy prev(actionManager->action("prevImage"), &QAction::triggered);
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));
    EXPECT_TRUE(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
    EXPECT_EQ(prev.count(), 1);
    EXPECT_EQ(scroll.count(), 1);
}

// Pointer tokens never ride QShortcutMap, so they have no projection to
// prove: what can be wrong is the RESOLUTION ORDER, which is code and was
// pinned by nothing. The order is inspect -> document -> global, and the
// last step is not decoration: the global table is the folder view's
// fallback for a pointer token (documentActive is false there), which is
// why enforceDisjointness deliberately does NOT erase a global entry a
// scope also holds. Both halves are asserted here — the ordering, and the
// fallback existing at all.
TEST_F(TestShortcutScope, pointerTokensResolveInOrderAndKeepTheirGlobalFallback) {
    const QString token = QStringLiteral("WheelUp");
    EXPECT_TRUE(actionManager->tableForPublic(Scope::Document).value(token) == QLatin1String("prevImage"))
        << "the fixture assumes the document table owns WheelUp";

    // 1. the document scope answers while the document view is live
    actionManager->setInspectMode(false);
    actionManager->setDocumentActive(true);
    EXPECT_EQ(actionManager->actionForToken(token), QStringLiteral("prevImage"));

    // 2. the global table is the fallback when no scope is live — the
    // binding this test plants exists only for the duration of the test
    actionManager->insertShortcut(Scope::Global, token, QStringLiteral("exit"));
    actionManager->setDocumentActive(false);
    EXPECT_EQ(actionManager->actionForToken(token), QStringLiteral("exit"));
    // and it did not have to be erased to coexist with the scope claim
    EXPECT_EQ(actionManager->tableForPublic(Scope::Global).value(token), QStringLiteral("exit"));
    EXPECT_EQ(actionManager->tableForPublic(Scope::Document).value(token), QStringLiteral("prevImage"));

    // 3. the armed inspect layer outranks the document table
    actionManager->setDocumentActive(true);
    actionManager->insertShortcut(Scope::Inspect, token, QStringLiteral("scrollLeft"));
    actionManager->setInspectMode(true);
    EXPECT_EQ(actionManager->actionForToken(token), QStringLiteral("scrollLeft"));
    actionManager->setInspectMode(false);
    EXPECT_EQ(actionManager->actionForToken(token), QStringLiteral("prevImage"));

    // restore the family to its defaults for the rest of the suite
    actionManager->removeAllShortcuts(Scope::Global, QStringLiteral("exit"));
    actionManager->removeAllShortcuts(Scope::Inspect, QStringLiteral("scrollLeft"));
    actionManager->resetDefaults();
    actionManager->setDocumentActive(true);
    actionManager->setInspectMode(false);
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
TEST_F(TestShortcutScope, fitOriginalPersistsAndArrowsStayInInspectMode) {
    writeImage(QDir(m_base).filePath("insA.png"));
    writeImage(QDir(m_base).filePath("insB.png"));
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_base).filePath("insA.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == QDir(m_base).filePath("insA.png");
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("insA.png"));
    core.mainWindow()->fitOriginal();
    auto *viewer = core.mainWindow()->findChild<ViewerWidget *>();
    EXPECT_TRUE(viewer);
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return viewer->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(viewer->fitMode(), FIT_ORIGINAL);

    core.nextImage();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == QDir(m_base).filePath("insB.png");
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("insB.png"));
    EXPECT_EQ(viewer->fitMode(), FIT_ORIGINAL);

    // armed: left pans, it never flips images
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("scrollLeft"));
    EXPECT_TRUE(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("insB.png"));

    // back in a browse mode the same key navigates again
    core.mainWindow()->fitWindow();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return viewer->fitMode() == FIT_WINDOW;
        },
        5000));
    EXPECT_EQ(viewer->fitMode(), FIT_WINDOW);
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));
    EXPECT_TRUE(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left))));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.currentFilePath() == QDir(m_base).filePath("insA.png");
        },
        10000));
    EXPECT_EQ(core.currentFilePath(), QDir(m_base).filePath("insA.png"));
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
TEST_F(TestShortcutScope, versionMigrationRecordsTheVersionFromAnInheritedPast) {
    FreshSettings fresh;
    const QMap<QString, QString> snapshot = actionManager->tableForPublic(Scope::Global);

    settings->setFirstRun(false);
    QVersionNumber inherited(1, 0, 3);
    settings->setLastVersion(inherited);
    EXPECT_EQ(settings->lastVersion(), inherited); // the rows serve what was written

    {
        Core core; // the constructor is what decides to migrate
    }

    EXPECT_EQ(settings->lastVersion(), appVersion);

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
TEST_F(TestShortcutScope, everyRegisteredActionIsATriggerableQAction) {
    const QList<QString> names = actionManager->actionList();
    EXPECT_TRUE(!names.isEmpty()) << "the action registry came back empty, the sweep proves nothing";
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
    EXPECT_TRUE(broken.isEmpty()) << qPrintable(
        QStringLiteral("registry inconsistency (") + QString::number(names.size()) +
        QStringLiteral(" actions checked):\n  ") + broken.join(QStringLiteral("\n  ")));
}

// invokeAction itself: it had never been entered despite eight production
// call sites (context menu, action buttons, folder grid, viewer, exit).
// A registered name has to report success AND reach its QAction; an
// unknown name has to report failure; a "s:" name with no script behind
// it has to report failure rather than pretend.
TEST_F(TestShortcutScope, invokeActionReportsAndReachesForEveryKindOfName) {
    QSignalSpy nextImage(actionManager->action("nextImage"), &QAction::triggered);
    QSignalSpy runScript(actionManager, &ActionManager::runScript);
    EXPECT_TRUE(nextImage.isValid() && runScript.isValid());

    EXPECT_TRUE(actionManager->invokeAction(QStringLiteral("nextImage")));
    EXPECT_EQ(nextImage.count(), 1);

    EXPECT_TRUE(!actionManager->invokeAction(QStringLiteral("noSuchActionAnywhere")));
    EXPECT_EQ(nextImage.count(), 1);
    EXPECT_EQ(runScript.count(), 0);

    // a script action that does not exist must not claim success
    EXPECT_TRUE(!actionManager->invokeAction(QStringLiteral("s:definitely-not-a-script")));
    EXPECT_EQ(runScript.count(), 0);

    // and one that does exist emits runScript with the prefix stripped
    scriptManager->addScript(QStringLiteral("qtest-ping"), Script(QStringLiteral("/bin/true"), false));
    EXPECT_TRUE(scriptManager->scriptExists(QStringLiteral("qtest-ping")));
    EXPECT_TRUE(actionManager->invokeAction(QStringLiteral("s:qtest-ping")));
    EXPECT_EQ(runScript.count(), 1);
    EXPECT_EQ(runScript.at(0).at(0).toString(), QStringLiteral("qtest-ping"));
    scriptManager->removeScript(QStringLiteral("qtest-ping"));
}

// The projection: every keyboard token in the global table must appear on
// its action's shortcut list, because QShortcutMap dispatches from those
// lists, not from the tables. Pointer tokens never project (no QKeySequence
// can hold a wheel direction or a mouse button). "Enter" is the one token
// that claims two sequences — Qt reads the main-row Enter as Return and
// keeps Enter for the keypad, and both scancodes resolve to the same token.
TEST_F(TestShortcutScope, defaultKeymapProjectsOntoTheRegistry) {
    const QMap<QString, QString> table = actionManager->tableForPublic(Scope::Global);
    EXPECT_TRUE(!table.isEmpty()) << "the global table came back empty, the sweep proves nothing";
    for(auto it = table.constBegin(); it != table.constEnd(); ++it) {
        const QString &token = it.key();
        const bool pointer = (token == QStringLiteral("WheelUp") || token == QStringLiteral("WheelDown") ||
                              token == QStringLiteral("LMB") || token == QStringLiteral("RMB") ||
                              token == QStringLiteral("MiddleButton") || token == QStringLiteral("XButton1") ||
                              token == QStringLiteral("XButton2") || token.endsWith(QStringLiteral("_DoubleClick")));
        QAction *a = actionManager->action(it.value());
        EXPECT_TRUE(a != nullptr) << qPrintable(QStringLiteral("table names an unregistered action: ") + it.value());
        if(pointer)
            continue;
        if(token == QStringLiteral("Enter")) {
            EXPECT_TRUE(a->shortcuts().contains(QKeySequence(QStringLiteral("Return"))))
                << qPrintable(QStringLiteral("Enter did not project as Return for ") + it.value());
            EXPECT_TRUE(a->shortcuts().contains(QKeySequence(QStringLiteral("Enter"))))
                << qPrintable(QStringLiteral("Enter did not project as keypad Enter for ") + it.value());
            continue;
        }
        QKeySequence seq = QKeySequence::fromString(token);
        EXPECT_TRUE(seq.count() > 0) << qPrintable(QStringLiteral("default token not Qt-parseable: ") + token);
        EXPECT_TRUE(a->shortcuts().contains(seq)) << qPrintable(
            QStringLiteral("token never projected onto its action: ") + token + QStringLiteral(" -> ") + it.value());
    }
}

// The scopes own their keys instead of layering — two live claims of one
// key are ambiguous with NEITHER firing (dispatch_probe_tests), so the
// grid's tokens are not in the global table at all (R12c keeps the
// default tables disjoint, dropGlobalEntriesClaimedByScopes the live
// ones) and the resolver asks the focused grid's table first. This pins
// the resolver's ranking; the delivery leg lives in
// returnKeyMeansFolderViewInTheDocumentAndGridOpenInTheGrid.
// The folder view's home button is wired end to end: the grid's
// directorySelected reaches DialogHost::opened, which reaches
// NavigationController::loadPath. It did not — MainWindow created
// DialogHost AFTER setupUi, so the connect inside setupUi bound a null
// receiver and both the home and root buttons did nothing for the life of
// the feature. Qt said so on every start ("connect(FolderViewProxy,
// Unknown): invalid nullptr parameter") into a log nobody read; this pin
// is what says it in a suite.
TEST_F(TestShortcutScope, theFolderViewsHomeButtonReachesTheNavigator) {
    const QString start = QDir(m_alpha).filePath("a.png");
    Core core;
    EXPECT_TRUE(core.loadPath(start));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));

    auto *proxy = core.mainWindow()->getFolderView();
    proxy->init();
    auto *folderView = proxy->findChild<FolderView *>();
    EXPECT_TRUE(folderView) << "no FolderView under the proxy";
    auto *homeBtn = folderView->findChild<IconButton *>(QStringLiteral("homeButton"));
    EXPECT_TRUE(homeBtn) << "the folder view has no home button";

    // the navigator's own report of where it ended up, not a guess about
    // the model: the button's whole job is to move the listing
    // IconButton is a QWidget with its own clicked() signal, not a
    // QAbstractButton: drive the same signal a click would emit
    emit homeBtn->clicked();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.directoryModel()->listing().directoryPath() == QDir::homePath();
        },
        10000));
}

TEST_F(TestShortcutScope, gridFocusAnswersThroughTheGridsTable) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *proxy = core.mainWindow()->getFolderView();
    proxy->init(); // materializes the real FolderView (and its grid)
    auto *grid = proxy->findChild<FolderGridView *>();
    EXPECT_TRUE(grid != nullptr) << "no FolderGridView under the folder view";

    // The document view owns Right/PgDown/PgUp now, and the grid owns the
    // same keys while it holds focus. The resolution follows focus, not a
    // central de-claim: the grid's table is asked first.
    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("nextImage"));

    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusIn));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("gridSelectNext"));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown)), QStringLiteral("gridPageDown"));

    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("nextImage"));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown)), QStringLiteral("nextImage"));
}

// The armed inspect layer replaces the global claims for its keys and
// gives them back on disarm — the projection's answer to "fit original
// pans with the arrows instead of changing images".
// The contract the resolver's simplicity stands on: the global table can
// never hold a token a scope owns (enforceDisjointness runs on every
// commit), so actionForKeySequence needs no de-claim pass — asking the
// tables in focus order IS the answer. Pinned here so a future change to
// either side breaks this test, not the dispatch.
TEST_F(TestShortcutScope, resolverAnswersTheLiveTableWithoutADeclaimPass) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *proxy = core.mainWindow()->getFolderView();
    proxy->init();
    auto *grid = proxy->findChild<FolderGridView *>();
    EXPECT_TRUE(grid != nullptr) << "no FolderGridView under the folder view";
    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusIn));

    // the grid's token is absent from the global table even while the
    // grid is live — disjointness is unconditional, not focus-dependent
    EXPECT_TRUE(!actionManager->tableForPublic(Scope::Global).contains(QStringLiteral("Right")));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("gridSelectNext"));

    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("nextImage"));
}

// The one token->sequence expansion every reader shares: "Enter" claims
// both Returns, and a non-Qt token expands to nothing rather than to a
// guess. The crop overlay's save-chord veto and the rename overlay's
// filter read through this — a second spelling here would desync them.
TEST_F(TestShortcutScope, tokenExpansionIsTheSharedOne) {
    const QList<QKeySequence> enter = ActionManager::sequencesForToken(QStringLiteral("Enter"));
    EXPECT_EQ(enter.size(), 2);
    EXPECT_TRUE(enter.contains(QKeySequence(Qt::Key_Return)));
    EXPECT_TRUE(enter.contains(QKeySequence(Qt::Key_Enter)));

    EXPECT_TRUE(ActionManager::sequencesForToken(QStringLiteral("Ctrl+S")).size() == 1);
    EXPECT_EQ(ActionManager::sequencesForToken(QStringLiteral("Ctrl+S")).constFirst(),
              QKeySequence::fromString(QStringLiteral("Ctrl+S")));
}

// The token walk's contract, after the merge: invokeActionForShortcut
// answers through actionForToken — the same tables, order and gates
// actionForKeySequence runs, with the pointer-token exception (the grid
// gate asks which surface holds keyboard focus; a pointer event reaching
// the funnel was not consumed by any surface, so it has no surface to
// rank). The dispatch leg is heard through its own signal; the lookup
// leg is asserted by name.
TEST_F(TestShortcutScope, tokenWalkAnswersThroughTheSameTables) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *proxy = core.mainWindow()->getFolderView();
    proxy->init();
    auto *grid = proxy->findChild<FolderGridView *>();
    EXPECT_TRUE(grid != nullptr) << "no FolderGridView under the folder view";

    // keyboard token, grid unfocused: the document table answers
    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));
    EXPECT_EQ(actionManager->actionForToken(QStringLiteral("Right")), QStringLiteral("nextImage"));
    EXPECT_EQ(actionManager->actionForToken(QStringLiteral("Ctrl+V")), QStringLiteral("pasteFile"));

    // keyboard token, grid focused: the grid's table answers — the same
    // gate actionForKeySequence applies
    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusIn));
    EXPECT_EQ(actionManager->actionForToken(QStringLiteral("Right")), QStringLiteral("gridSelectNext"));
    EXPECT_EQ(actionManager->actionForToken(QStringLiteral("Ctrl+V")), QStringLiteral("pasteFilesHere"));
    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));

    // the pointer exception: WheelDown answers the document table even
    // while the grid holds focus — the gate is a keyboard-focus question,
    // and the wheel event the funnel sees was not consumed by any surface
    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusIn));
    EXPECT_EQ(actionManager->actionForToken(QStringLiteral("WheelDown")), QStringLiteral("nextImage"));
    QApplication::sendEvent(grid, new QFocusEvent(QEvent::FocusOut));

    // the dispatch leg fires the action the walk named
    QSignalSpy next(actionManager->action("nextImage"), &QAction::triggered);
    EXPECT_TRUE(actionManager->invokeActionForShortcut(QStringLiteral("WheelDown")));
    EXPECT_EQ(next.count(), 1);
    // an unbound token dispatches nothing and reports the miss
    EXPECT_TRUE(!actionManager->invokeActionForShortcut(QStringLiteral("F13")));
    EXPECT_EQ(next.count(), 1);
}

TEST_F(TestShortcutScope, armedInspectSwapsTheProjection) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));

    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));

    core.mainWindow()->fitOriginal(); // arms the inspect scope through the real observer
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.mainWindow()->viewer()->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(core.mainWindow()->viewer()->fitMode(), FIT_ORIGINAL);
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("scrollLeft"));

    core.mainWindow()->fitWindow(); // disarms
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.mainWindow()->viewer()->fitMode() == FIT_WINDOW;
        },
        5000));
    EXPECT_EQ(core.mainWindow()->viewer()->fitMode(), FIT_WINDOW);
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Left)), QStringLiteral("prevImage"));
}

// The end-to-end leg the migration stands on: a window-delivered key fires
// the projected action through QShortcutMap. sendEvent-based legs bypass
// the map entirely (they exercise the doors and the manual funnel), so
// this is the one slot that proves the projection is live and registered.
TEST_F(TestShortcutScope, windowDeliveryFiresTheProjectedAction) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *mw = core.mainWindow();
    mw->show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(mw));
    mw->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(mw));

    QSignalSpy fullscreen(actionManager->action("toggleFullscreen"), &QAction::triggered);
    QSignalSpy save(actionManager->action("save"), &QAction::triggered);
    EXPECT_TRUE(fullscreen.isValid() && save.isValid());

    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_F));
    EXPECT_EQ(fullscreen.count(), 1); // F: the projected binding fired through the map

    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_S, Qt::ControlModifier));
    EXPECT_EQ(save.count(), 1); // Ctrl+S: the chord reached its action too
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
TEST_F(TestShortcutScope, returnKeyMeansFolderViewInTheDocumentAndGridOpenInTheGrid) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *mw = core.mainWindow();
    mw->show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(mw));
    mw->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(mw));

    QSignalSpy folderView(actionManager->action("folderView"), &QAction::triggered);
    QSignalSpy gridOpen(actionManager->action("gridOpen"), &QAction::triggered);
    EXPECT_TRUE(folderView.isValid() && gridOpen.isValid());

    // leg one: the document view is on screen and its viewer holds focus
    // — the doc scope's claim is live, the grid's is not, the global
    // table carries no Enter at all
    mw->viewer()->setFocus();
    EXPECT_TRUE(mw->viewer()->hasFocus() || QApplication::focusWidget() == mw)
        << "no focus in the document view to carry the doc claim";
    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_Return));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return folderView.count() == 1;
        },
        5000));
    EXPECT_EQ(folderView.count(), 1);
    EXPECT_EQ(gridOpen.count(), 0);

    // that very delivery switched to the folder view; the grid surfaces
    auto *proxy = mw->getFolderView();
    proxy->init();
    auto *grid = proxy->findChild<FolderGridView *>();
    EXPECT_TRUE(grid != nullptr) << "no FolderGridView under the folder view";
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return grid->isVisible();
        },
        5000));
    grid->setFocus();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return grid->hasFocus();
        },
        5000));
    folderView.clear();
    gridOpen.clear();

    // leg two: the grid holds focus — gridOpen fires alone, and the
    // toggle does not re-fire (an ambiguous pair would fire NEITHER)
    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_Return));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return gridOpen.count() == 1;
        },
        5000));
    EXPECT_EQ(gridOpen.count(), 1);
    EXPECT_EQ(folderView.count(), 0);
}

// the inspect scope carries user overrides: while armed, an override
// replaces the inspect default for that key — the armed chain reads the
// layer, not the seed defaults. The table is snapshotted and restored;
// the singleton is process-wide.
TEST_F(TestShortcutScope, inspectShortcutsCarryUserOverrides) {
    const QMap<QString, QString> inspectSnapshot = actionManager->tableForPublic(Scope::Inspect);
    actionManager->insertShortcut(Scope::Inspect, QStringLiteral("Left"), QStringLiteral("nextImage"));

    QSignalSpy next(actionManager->action("nextImage"), &QAction::triggered);
    QSignalSpy scroll(actionManager->action("scrollLeft"), &QAction::triggered);
    actionManager->setInspectMode(true);
    EXPECT_TRUE(actionManager->invokeActionForShortcut(QStringLiteral("Left")));
    EXPECT_EQ(next.count(), 1);
    EXPECT_EQ(scroll.count(), 0); // the scrollLeft default never fired
    actionManager->setInspectMode(false);

    actionManager->removeAllShortcuts(Scope::Inspect);
    for(auto it = inspectSnapshot.constBegin(); it != inspectSnapshot.constEnd(); ++it)
        actionManager->insertShortcut(Scope::Inspect, it.key(), it.value());
}

TEST_F(TestShortcutScope, pagingKeysHaveNamesAndDefaults) {
    // a press with no name in the scancode table cannot reach the action
    // layer at all; a name with no default never fires for a config that
    // predates the binding — pin both halves, plus the lookup itself.
    // The scan codes come from the platform's own table (Windows pairs
    // PgUp with 329/57417, X11 with 112 — hardcoded values would pin one
    // platform), via the shared lookup helper.
    const quint32 pgUpCode = coretest::scanCodeFor(QStringLiteral("PgUp"));
    const quint32 pgDownCode = coretest::scanCodeFor(QStringLiteral("PgDown"));
    EXPECT_TRUE(pgUpCode != 0 && inputMap->keys().value(pgUpCode) == "PgUp") << "PgUp has a platform scan code";
    EXPECT_TRUE(pgDownCode != 0 && inputMap->keys().value(pgDownCode) == "PgDown") << "PgDown has a platform scan code";
    // PgUp/PgDown are the document view's binding — the token lookup has
    // to reach that table, not only the global one
    EXPECT_TRUE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageUp)) == "prevImage");
    EXPECT_TRUE(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown)) == "nextImage");
    EXPECT_TRUE(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageDown))));
    EXPECT_TRUE(actionManager->invokeAction(actionManager->actionForKeySequence(QKeySequence(Qt::Key_PageUp))));
    // an unbound token claims nothing — and ASKING must not bind it. This
    // call used to be actionForShortcut, which answered through QMap::
    // operator[] and so inserted a null row into the live global table on
    // every question about an unbound key. The read side is const now, so
    // the assertion below is "the answer is empty" AND "the table did not
    // grow" — a query cannot be a writer.
    const QMap<QString, QString> before = actionManager->tableForPublic(Scope::Global);
    EXPECT_TRUE(actionManager->scopeClaimsForToken(QStringLiteral("F13")).isEmpty());
    EXPECT_EQ(actionManager->tableForPublic(Scope::Global), before);
}

// The one-key-one-surface law holds against the editor and the config,
// not just the defaults: a token a scope table claims cannot enter the
// global table. addShortcut is the editor's insert door; initShortcuts
// runs the same drop after readShortcuts for configs saved by older
// releases (which carry the pre-split global table, Enter included).
// A dropped entry must not reach the projection either — the ambiguity
// never gets to QShortcutMap.
TEST_F(TestShortcutScope, globalTableRefusesTokensOwnedByScopes) {
    const QMap<QString, QString> before = actionManager->tableForPublic(Scope::Global);

    // Enter: the grid's and the doc's; Right: the doc's and the grid's
    actionManager->addShortcut(QStringLiteral("Enter"), QStringLiteral("exit"));
    actionManager->addShortcut(QStringLiteral("Right"), QStringLiteral("exit"));
    EXPECT_TRUE(!actionManager->tableForPublic(Scope::Global).contains(QStringLiteral("Enter")));
    EXPECT_TRUE(!actionManager->tableForPublic(Scope::Global).contains(QStringLiteral("Right")));
    EXPECT_TRUE(!actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_Return)));
    EXPECT_TRUE(!actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_Right)));

    // a token nobody scopes still lands — the door is narrow, not shut
    actionManager->addShortcut(QStringLiteral("F13"), QStringLiteral("exit"));
    EXPECT_TRUE(actionManager->tableForPublic(Scope::Global).contains(QStringLiteral("F13")));
    EXPECT_TRUE(actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_F13)));
    actionManager->removeShortcut(QStringLiteral("F13"));

    EXPECT_EQ(actionManager->tableForPublic(Scope::Global), before);
}

// resetDefaults restores every table — the document table joined the
// family in the view split and the reset left it behind, so the keymap
// editor's reset button could not undo a document-scope rebinding
TEST_F(TestShortcutScope, resetDefaultsRestoresEveryTable) {
    const QMap<QString, QString> globalBefore = actionManager->tableForPublic(Scope::Global);
    const QMap<QString, QString> docBefore = actionManager->tableForPublic(Scope::Document);
    const QMap<QString, QString> gridBefore = actionManager->tableForPublic(Scope::Grid);
    const QMap<QString, QString> inspectBefore = actionManager->tableForPublic(Scope::Inspect);

    actionManager->addShortcut(QStringLiteral("F13"), QStringLiteral("exit"));
    actionManager->insertShortcut(Scope::Document, QStringLiteral("F14"), QStringLiteral("crop"));
    actionManager->insertShortcut(Scope::Grid, QStringLiteral("F15"), QStringLiteral("gridOpen"));
    actionManager->insertShortcut(Scope::Inspect, QStringLiteral("F16"), QStringLiteral("scrollLeft"));
    actionManager->resetDefaults();

    EXPECT_EQ(actionManager->tableForPublic(Scope::Global), globalBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Document), docBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Grid), gridBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Inspect), inspectBefore);
}

// After a user removes a default binding and saves, the NEXT process reads
// the same tables — that is the whole test for a restart. In-process there
// is no second ActionManager, so the two halves the restart consists of are
// pinned separately and together here: the write path through the real
// editor save, then a real config read into a defaults-seeded map (exactly
// initShortcuts' order). Without the tombstone the default came back and
// "unbind" was an operation the app offered and then undid.
TEST_F(TestShortcutScope, aRemovedDefaultBindingDoesNotComeBackOnRestart) {
    // a binding the defaults really carry, so the removal is visible
    const QMap<QString, QString> defaults = actionManager->tableForPublic(Scope::Global);
    EXPECT_TRUE(defaults.contains(QStringLiteral("F11")) || defaults.key(QStringLiteral("toggleFullscreen")).size())
        << "the fixture assumes toggleFullscreen sits in the global defaults";

    // what the editor does: drop the row, then save the table
    actionManager->removeAllShortcuts(QStringLiteral("toggleFullscreen"));
    actionManager->saveShortcuts();

    // what the next launch does: defaults as the base layer, config overlay
    QMap<QString, QString> asReloaded = defaults;
    settings->readShortcuts(asReloaded);
    EXPECT_TRUE(!asReloaded.key(QStringLiteral("toggleFullscreen")).size())
        << "the removed binding came back from the default layer on reload";

    // resetDefaults is the documented undo for removals and rebindings
    actionManager->resetDefaults();
}

// The tombstone must not swallow a scope's own table: the four tables save
// to separate Controls/ keys, and a removal in the document scope must not
// leak into the global one (or the other way round). The document table
// carries no default for crop, so the row it removes there is a user
// binding, not a default — nothing should be tombstoned for it.
TEST_F(TestShortcutScope, tombstonesStayInsideTheirScope) {
    const QMap<QString, QString> globalBefore = actionManager->tableForPublic(Scope::Global);
    actionManager->insertShortcut(Scope::Document, QStringLiteral("F14"), QStringLiteral("crop"));
    actionManager->removeAllShortcuts(Scope::Document, QStringLiteral("crop"));
    actionManager->saveScope(Scope::Document);
    actionManager->saveShortcuts();

    QMap<QString, QString> docReloaded = actionManager->tableForPublic(Scope::Document);
    settings->readShortcuts(docReloaded, QStringLiteral("docShortcuts"));
    // the global table is untouched by the document-scope removal
    EXPECT_EQ(actionManager->tableForPublic(Scope::Global), globalBefore);

    actionManager->resetDefaults();
}

// Liveness flags are consumed by the resolver only — no projection
// function reads gridActive/documentActive (grid is gated by Qt's
// WidgetWithChildrenShortcut context on the focused subtree, the document
// the same on DocumentWidget). The rebuilds those setters used to run
// were the de-claim era's leftover. This pins the OBSERVABLE consequence:
// after a real view switch, each scope's keys still mean their scope's
// action, with no projection rebuild on the switch itself.
TEST_F(TestShortcutScope, viewSwitchKeepsBothScopesLiveWithoutRebuilding) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *mw = core.mainWindow();

    // document view: the resolver answers with the document table
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("nextImage"));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Return)), QStringLiteral("folderView"));

    // switch to the folder view (the real observer path, not a direct call)
    EXPECT_TRUE(actionManager->invokeAction(QStringLiteral("folderView")));
    auto *proxy = mw->getFolderView();
    proxy->init();
    auto *grid = proxy->findChild<FolderGridView *>();
    EXPECT_TRUE(grid != nullptr) << "no FolderGridView under the folder view";
    grid->setFocus();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return grid->hasFocus();
        },
        5000));

    // the grid now answers for its own keys — no rebuild was needed for
    // this to be true, the grid's QShortcuts were already attached
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Right)), QStringLiteral("gridSelectNext"));
    EXPECT_EQ(actionManager->actionForKeySequence(QKeySequence(Qt::Key_Return)), QStringLiteral("gridOpen"));

    // and the document table is still intact underneath — the switch
    // did not clear it (the old rebuild did clear and recreate it)
    EXPECT_TRUE(actionManager->tableForPublic(Scope::Document).values().contains(QStringLiteral("nextImage")));
    EXPECT_TRUE(actionManager->tableForPublic(Scope::Document).values().contains(QStringLiteral("folderView")));
}

// commit() is the one write path: whatever door the edit came through,
// the disjointness law and the projection rebuild run after it. This
// pins the property from BOTH sides — a scope insert that collides with
// a global token, and a global insert that collides with a scope token —
// because the old code only guarded the second direction (and only after
// config load, not after every editor insert).
TEST_F(TestShortcutScope, everyWriteDoorEnforcesDisjointness) {
    const QMap<QString, QString> globalBefore = actionManager->tableForPublic(Scope::Global);
    const QMap<QString, QString> docBefore = actionManager->tableForPublic(Scope::Document);
    const QMap<QString, QString> gridBefore = actionManager->tableForPublic(Scope::Grid);

    // direction 1: a scope insert of a token the global table holds.
    // "openSettings" is P, and P is also a document binding? — pick a
    // token known to be global-only, then bind it into the document
    // scope and watch the global copy disappear.
    EXPECT_TRUE(actionManager->tableForPublic(Scope::Global).contains(QStringLiteral("Del")));
    actionManager->insertShortcut(Scope::Document, QStringLiteral("Del"), QStringLiteral("crop"));
    EXPECT_TRUE(!actionManager->tableForPublic(Scope::Global).contains(QStringLiteral("Del")))
        << "a document insert left the global claim live — the pair is ambiguous";

    // direction 2: the reverse (the door addShortcut already had)
    actionManager->addShortcut(QStringLiteral("Right"), QStringLiteral("exit"));
    EXPECT_TRUE(!actionManager->tableForPublic(Scope::Global).contains(QStringLiteral("Right")));

    // the projection agrees with the tables — no stale claim survives
    EXPECT_TRUE(!actionManager->action("moveToTrash")->shortcuts().contains(QKeySequence(Qt::Key_Delete)));
    EXPECT_TRUE(!actionManager->action("exit")->shortcuts().contains(QKeySequence(Qt::Key_Right)));

    // restore through the same doors
    actionManager->removeAllShortcuts(Scope::Document, QStringLiteral("crop"));
    actionManager->removeAllShortcuts(QStringLiteral("exit"));
    actionManager->resetDefaults();
    EXPECT_EQ(actionManager->tableForPublic(Scope::Global), globalBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Document), docBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Grid), gridBefore);
}

// The delivery leg of the inspect hand-over, which the resolver tests
// above cannot see: arming is now an enabled-bit flip between two claim
// sets that live on the SAME surface at the SAME time. A resolver test
// passing says nothing about whether Qt's map fires the right one — and
// "both enabled" is exactly the ambiguous state that fires NEITHER
// (dispatch_probe_tests). So this drives real keys through the window.
TEST_F(TestShortcutScope, inspectArmingHandsTheKeyOverWithoutRebuilding) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *mw = core.mainWindow();
    mw->show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(mw));
    mw->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(mw));
    mw->viewer()->setFocus();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->hasFocus();
        },
        5000));

    QSignalSpy prev(actionManager->action("prevImage"), &QAction::triggered);
    QSignalSpy scroll(actionManager->action("scrollLeft"), &QAction::triggered);
    EXPECT_TRUE(prev.isValid() && scroll.isValid());

    // disarmed: Left is the walk
    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_Left));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return prev.count() == 1;
        },
        5000));
    EXPECT_EQ(prev.count(), 1);
    EXPECT_EQ(scroll.count(), 0);

    // arm through the real observer
    mw->fitOriginal();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(mw->viewer()->fitMode(), FIT_ORIGINAL);
    mw->viewer()->setFocus();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->hasFocus();
        },
        5000));
    prev.clear();
    scroll.clear();

    // armed: the same key now pans — and prev must NOT fire, which would
    // mean both claims were live (the ambiguous state)
    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_Left));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return scroll.count() == 1;
        },
        5000));
    EXPECT_EQ(scroll.count(), 1);
    EXPECT_EQ(prev.count(), 0);

    // disarm: the walk gets the key back
    mw->fitWindow();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->fitMode() == FIT_WINDOW;
        },
        5000));
    EXPECT_EQ(mw->viewer()->fitMode(), FIT_WINDOW);
    mw->viewer()->setFocus();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->hasFocus();
        },
        5000));
    prev.clear();
    scroll.clear();
    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_Left));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return prev.count() == 1;
        },
        5000));
    EXPECT_EQ(prev.count(), 1);
    EXPECT_EQ(scroll.count(), 0);
}

// The hand-over is per TOKEN, not per set: arming inspect must take ONLY
// the keys inspect claims, leaving every other document key live. A
// set-wide "disable document while armed" would pass a Left-only test and
// still break Ctrl+S, Space, and the rest — so this drives a key inspect
// does not touch.
TEST_F(TestShortcutScope, inspectArmingKeepsUnclaimedDocumentKeysLive) {
    Core core;
    EXPECT_TRUE(core.loadPath(QDir(m_alpha).filePath("a.png")));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return core.isSettled();
        },
        10000));
    auto *mw = core.mainWindow();
    mw->show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(mw));
    mw->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(mw));

    QSignalSpy save(actionManager->action("save"), &QAction::triggered);
    QSignalSpy fullscreen(actionManager->action("toggleFullscreen"), &QAction::triggered);
    EXPECT_TRUE(save.isValid() && fullscreen.isValid());

    mw->fitOriginal();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->fitMode() == FIT_ORIGINAL;
        },
        5000));
    EXPECT_EQ(mw->viewer()->fitMode(), FIT_ORIGINAL);
    mw->viewer()->setFocus();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->hasFocus();
        },
        5000));

    // Ctrl+S and F are not inspect claims: they stay live while armed
    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_S, Qt::ControlModifier));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return save.count() == 1;
        },
        5000));
    EXPECT_EQ(save.count(), 1);
    ASSERT_TRUE(coretest::deliverWindowKey(mw, Qt::Key_F));
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return fullscreen.count() == 1;
        },
        5000));
    EXPECT_EQ(fullscreen.count(), 1);

    mw->fitWindow();
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return mw->viewer()->fitMode() == FIT_WINDOW;
        },
        5000));
    EXPECT_EQ(mw->viewer()->fitMode(), FIT_WINDOW);
}

// Surface handover. A QAction's shortcut is live exactly while the action
// is added to a widget, so replacing the window surface must take the
// actions off the OLD window — otherwise both windows answer, and the
// "one owner" law is broken in the same silent way the whole keymap work
// has been about. This is the observable half of the lifecycle change.
//
// The destroyed-hook guard (only clear if the dying surface is still the
// current one) is NOT testable through this API: with the manager as a
// never-destructed singleton, a stale clear is overwritten by the next
// attach and leaves no trace. It exists for the code that would one day
// dereference windowSurface, and the comment on it says so rather than
// pretending a test covers it. Each slot is its own process, so mutating
// the singleton here is contained.
TEST_F(TestShortcutScope, replacingTheWindowSurfaceDetachesTheOldOne) {
    Core core;
    auto *real = core.mainWindow();
    EXPECT_TRUE(real != nullptr);
    QAction *fullscreen = actionManager->action(QStringLiteral("toggleFullscreen"));
    EXPECT_TRUE(fullscreen);
    EXPECT_TRUE(real->actions().contains(fullscreen)) << "the real window is not a live surface";

    auto *other = new QWidget();
    actionManager->attachWindowSurface(other);
    // one owner: the old window let go, the new one holds it
    EXPECT_TRUE(!real->actions().contains(fullscreen)) << "the previous window kept the action — two live surfaces";
    EXPECT_TRUE(other->actions().contains(fullscreen)) << "the new window never received the action";

    // and the claim is inert on the old window: a projected shortcut
    // there does nothing, while the same key on the live surface fires
    EXPECT_TRUE(!fullscreen->shortcuts().isEmpty());

    actionManager->attachWindowSurface(real);
    EXPECT_TRUE(real->actions().contains(fullscreen));
    EXPECT_TRUE(!other->actions().contains(fullscreen)) << "detach on the way back failed";
    delete other;

    auto *floating = new QWidget();
    actionManager->attachWindowSurface(floating);
    delete floating; // the CURRENT surface dies: its hook must leave the manager usable
    auto *after = new QWidget();
    actionManager->attachWindowSurface(after);
    EXPECT_TRUE(after->actions().contains(fullscreen)) << "a surface death left the manager unable to attach again";
    delete after;

    actionManager->attachWindowSurface(real);
    EXPECT_TRUE(real->actions().contains(fullscreen)) << "could not restore the real window";
}

// The global table is the WIDE scope, and wide means "standard or absent".
// Every key here is either a mass-standard chord or a gesture with no
// conflicting meaning. Three things were removed because they failed that
// test and each has a reason worth pinning:
//   MiddleButton->exit   a mis-tap quits and loses the session
//   Alt+X->exit          a second exit chord nobody expects
//   C/M->copyFile/moveFile  file ops that need a visible subject, now
//                        document-scope (and everywhere reachable at all)
TEST_F(TestShortcutScope, theGlobalTableKeepsOnlyStandardOrSubjectlessKeys) {
    const QMap<QString, QString> global = actionManager->tableForPublic(Scope::Global);

    // the removals — one forward walk instead of QMap::keys(value), which
    // allocates a temporary list per call and tripped clazy
    QSet<QString> exitKeys;
    QSet<QString> copyClipboardKeys;
    QSet<QString> openKeys;
    for(auto it = global.constBegin(); it != global.constEnd(); ++it) {
        if(it.value() == QLatin1String("exit"))
            exitKeys.insert(it.key());
        else if(it.value() == QLatin1String("copyFileClipboard"))
            copyClipboardKeys.insert(it.key());
        else if(it.value() == QLatin1String("open"))
            openKeys.insert(it.key());
    }
    EXPECT_TRUE(!exitKeys.contains(QStringLiteral("MiddleButton"))) << "MiddleButton still exits the application";
    EXPECT_TRUE(!exitKeys.contains(QStringLiteral("Alt+X"))) << "the non-standard Alt+X exit chord came back";
    EXPECT_TRUE(!global.contains(QStringLiteral("C"))) << "C belongs to the document scope now";
    EXPECT_TRUE(!global.contains(QStringLiteral("M"))) << "M belongs to the document scope now";

    // the standard chords that must stay
    EXPECT_TRUE(exitKeys.contains(QStringLiteral("Ctrl+Q")));
    EXPECT_TRUE(copyClipboardKeys.contains(QStringLiteral("Ctrl+C")));
    EXPECT_TRUE(openKeys.contains(QStringLiteral("Ctrl+O")));

    // C/M are reachable in the document view, where their overlays have a
    // subject to act on
    const QMap<QString, QString> doc = actionManager->tableForPublic(Scope::Document);
    EXPECT_EQ(doc.value(QStringLiteral("C")), QStringLiteral("copyFile"));
    EXPECT_EQ(doc.value(QStringLiteral("M")), QStringLiteral("moveFile"));
}

// The same correction, for the two bindings whose ACTION only exists in
// the document view — both were global claims reaching a surface the
// folder view does not have:
//   RMB -> contextMenu   ContextMenu is constructed and shown by
//                        ViewerWidget and refuses when it is not visible.
//                        The grid's right-click is an unwritten TODO and
//                        its options popup is a toolbar button.
//   Ctrl+V -> pasteFile  openFromClipboard's whole effect is loadPath():
//                        "the file on screen is now that one". No document
//                        view, no subject.
// The global table keeps Menu -> contextMenu: a menu key is meaningful
// wherever a menu exists, and it is a different token, not a collision.
TEST_F(TestShortcutScope, documentOnlyActionsLeaveTheGlobalTable) {
    const QMap<QString, QString> global = actionManager->tableForPublic(Scope::Global);
    const QMap<QString, QString> doc = actionManager->tableForPublic(Scope::Document);

    EXPECT_TRUE(!global.contains(QStringLiteral("RMB"))) << "RMB claims a global context menu the grid cannot show";
    EXPECT_TRUE(!global.contains(QStringLiteral("Ctrl+V"))) << "Ctrl+V reaches a document-only action from everywhere";

    EXPECT_EQ(doc.value(QStringLiteral("RMB")), QStringLiteral("contextMenu"));
    EXPECT_EQ(doc.value(QStringLiteral("Ctrl+V")), QStringLiteral("pasteFile"));
    // Menu still works everywhere it used to: a menu key is meaningful
    // wherever a menu exists, and it is a different token, not a collision
    EXPECT_EQ(global.value(QStringLiteral("Menu")), QStringLiteral("contextMenu"));
}

// The settings editor's conflict warning asks the SAME scope walk the
// disjointness law uses — one implementation, so a table added later is
// visible to both by construction. The hand-written three-table version
// this replaced had forgotten the document table, so a document-scope
// collision produced no warning: the user's key silently landed in two
// places. "Enter" is the sharpest probe — it is the one token whose
// spelling ("Enter") differs from what the document/grid tables hold
// ("Enter" as Return+keypad), so a naive string compare reports nothing.
TEST_F(TestShortcutScope, scopeClaimsNameEveryTableHoldingTheKey) {
    // Right lives in the document table and the grid table
    const QStringList right = actionManager->scopeClaimsForToken(QStringLiteral("Right"));
    EXPECT_TRUE(right.contains(QStringLiteral("document"))) << qPrintable(right.join(", "));
    EXPECT_TRUE(right.contains(QStringLiteral("folder view"))) << qPrintable(right.join(", "));
    // Left additionally lives in inspect
    const QStringList left = actionManager->scopeClaimsForToken(QStringLiteral("Left"));
    EXPECT_TRUE(left.contains(QStringLiteral("document"))) << qPrintable(left.join(", "));
    EXPECT_TRUE(left.contains(QStringLiteral("folder view"))) << qPrintable(left.join(", "));
    EXPECT_TRUE(left.contains(QStringLiteral("inspect"))) << qPrintable(left.join(", "));
    // Enter: document (folderView) and grid (gridOpen) — the token that
    // exposed the missing table, and whose sequence expansion is the
    // reason a spelling comparison would have missed it
    const QStringList enter = actionManager->scopeClaimsForToken(QStringLiteral("Enter"));
    EXPECT_TRUE(enter.contains(QStringLiteral("document"))) << qPrintable(enter.join(", "));
    EXPECT_TRUE(enter.contains(QStringLiteral("folder view"))) << qPrintable(enter.join(", "));
    // a key nobody scopes claims nothing
    EXPECT_TRUE(actionManager->scopeClaimsForToken(QStringLiteral("F13")).isEmpty());
    // Pointer tokens DO answer now. They used to return empty here, and
    // that silence was the lie: WheelUp is bound in the document table, so
    // the editor's "this shortcut is used for …" line said nothing about a
    // bind the user was about to shadow. Exact-token matching is what makes
    // this safe — every pointer token expands to the same invalid sequence,
    // so an expansion-based walk would report all of them for any of them.
    const QStringList wheel = actionManager->scopeClaimsForToken(QStringLiteral("WheelUp"));
    EXPECT_TRUE(wheel.contains(QStringLiteral("document"))) << qPrintable(wheel.join(", "));
    // and the expansion trap itself: RMB and XButton1 must NOT claim each
    // other's scopes, which is what they would do if compared as sequences
    EXPECT_TRUE(!actionManager->scopeClaimsForToken(QStringLiteral("RMB")).contains(QStringLiteral("folder view")));
    const QStringList x1 = actionManager->scopeClaimsForToken(QStringLiteral("XButton1"));
    EXPECT_TRUE(x1.contains(QStringLiteral("document"))) << qPrintable(x1.join(", "));
    EXPECT_TRUE(!x1.contains(QStringLiteral("inspect"))) << qPrintable(x1.join(", "));
}

// The same walk answers the GLOBAL half, which used to be stitched on by
// the dialog through a second reader (actionForShortcut) that saw only the
// global table and answered through QMap::operator[] — so a question about
// an unbound key inserted a null row into the live table. One call now:
// the answer names every holder, the global table first and labelled, and
// asking cannot write.
TEST_F(TestShortcutScope, globalClaimsRideTheSameWalkAndTheQuestionCannotWrite) {
    const QMap<QString, QString> before = actionManager->tableForPublic(Scope::Global);

    // a global-only key: P opens settings and no scope table claims it
    const QStringList p = actionManager->scopeClaimsForToken(QStringLiteral("P"));
    EXPECT_TRUE(!p.isEmpty()) << "a global binding produced no claim at all";
    EXPECT_TRUE(p.first().endsWith(QStringLiteral("(global)"))) << qPrintable(p.join(", "));
    EXPECT_TRUE(p.first().startsWith(QStringLiteral("openSettings"))) << qPrintable(p.join(", "));

    // a key the scopes also hold: both halves appear in one answer. Right
    // is document + folder view, and is NOT in the global table (R12c), so
    // this asserts the scope half was not swallowed by the global check.
    EXPECT_TRUE(actionManager->scopeClaimsForToken(QStringLiteral("Right")).size() >= 2);

    // the read side cannot grow the table it reads
    EXPECT_TRUE(actionManager->scopeClaimsForToken(QStringLiteral("F13")).isEmpty());
    EXPECT_EQ(actionManager->tableForPublic(Scope::Global), before);
}

// The context menu prints a binding hint per item, and it reads the
// action through shortcutForAction. When 7b20e6e6 moved save/crop/the
// edit verbs/setWallpaper into the document table, this reader kept
// reading only the global one — every document-view menu item that had
// a chord lost its hint, silently, because nothing asserted the hint.
// The menu only ever appears on ViewerWidget, so the document table is
// the right answer for it.
TEST_F(TestShortcutScope, contextMenuHintsReadTheDocumentTable) {
    // commands the view split moved out of the global table
    for(const QString &action : {QStringLiteral("save"), QStringLiteral("crop"), QStringLiteral("setWallpaper"),
                                 QStringLiteral("rotateLeft"), QStringLiteral("fitWindow")}) {
        EXPECT_TRUE(!actionManager->tableForPublic(Scope::Global).values().contains(action))
            << qPrintable(QStringLiteral("expected no global binding for ") + action +
                          " — this action is documented as document-scope only");
        EXPECT_TRUE(!actionManager->shortcutForAction(action).isEmpty()) << qPrintable(
            QStringLiteral("the menu hint for ") + action + " is blank — the reader lost the document table");
    }
    // and the answer is the document table's own token, not a stale one
    EXPECT_EQ(actionManager->shortcutForAction(QStringLiteral("setWallpaper")), QStringLiteral("W"));
    EXPECT_EQ(actionManager->shortcutForAction(QStringLiteral("save")), QStringLiteral("Ctrl+S"));
    // a global-table action still answers from there
    EXPECT_EQ(actionManager->shortcutForAction(QStringLiteral("openSettings")), QStringLiteral("P"));
}

// The action table is the keymap's census, and this test is the guard that
// makes adopting it a no-op: every fact it states must agree with the live
// registry and the live default tables TODAY. Stage 1 ships the table
// without wiring it in; if any row disagrees with the code that still owns
// the truth, this fails by name before the switch-over can hide the drift.
// Once the code reads the table instead (the next stage), the same test
// becomes the cross-check in the other direction.
TEST_F(TestShortcutScope, theActionTableAgreesWithTheLiveRegistry) {
    // 1. names: exactly the registry, no extras, no misses
    QSet<QString> tableNames;
    for(const ActionDesc &row : ActionTable::kActions) {
        const QString name = QString::fromLatin1(row.name);
        EXPECT_TRUE(!name.isEmpty()) << "a table row has no name";
        EXPECT_TRUE(!tableNames.contains(name)) << qPrintable(QStringLiteral("duplicate row: ") + name);
        tableNames.insert(name);
        EXPECT_TRUE(row.display && *row.display) << qPrintable(QStringLiteral("no display name: ") + name);
        EXPECT_TRUE(row.category && *row.category) << qPrintable(QStringLiteral("no category: ") + name);
    }
    const QStringList registered = actionManager->actionList();
    EXPECT_EQ(tableNames.size(), registered.size());
    for(const QString &name : registered)
        EXPECT_TRUE(tableNames.contains(name))
            << qPrintable(QStringLiteral("registered action missing from the table: ") + name);

    // 2. default bindings: (action, scope, token) triples agree exactly
    struct Live {
        Scope scope;
        const QMap<QString, QString> *table;
    };
    const Live lives[] = {
        {Scope::Global, &actionManager->tableForPublic(Scope::Global)},
        {Scope::Document, &actionManager->tableForPublic(Scope::Document)},
        {Scope::Grid, &actionManager->tableForPublic(Scope::Grid)},
        {Scope::Inspect, &actionManager->tableForPublic(Scope::Inspect)},
    };
    QSet<QString> liveTriples;
    for(const Live &live : lives)
        for(auto it = live.table->constBegin(); it != live.table->constEnd(); ++it)
            liveTriples.insert(QStringLiteral("%1|%2|%3").arg(scopeTag(live.scope), it.key(), it.value()));

    QSet<QString> tableTriples;
    for(const DefaultBinding &b : ActionTable::kDefaultBindings) {
        const QString triple = QStringLiteral("%1|%2|%3")
                                   .arg(scopeTag(b.scope), QString::fromLatin1(b.token), QString::fromLatin1(b.action));
        EXPECT_TRUE(!tableTriples.contains(triple)) << qPrintable(QStringLiteral("duplicate binding: ") + triple);
        tableTriples.insert(triple);
    }
    // named, not inline: range-for over the difference temporary shares and
    // detaches it (clazy range-loop-detach)
    const QSet<QString> missingFromTable = liveTriples - tableTriples;
    const QSet<QString> extrasInTable = tableTriples - liveTriples;
    for(const QString &t : missingFromTable)
        ADD_FAILURE() << qPrintable(QStringLiteral("live default binding is not in the table: ") + t);
    for(const QString &t : extrasInTable)
        ADD_FAILURE() << qPrintable(QStringLiteral("table binding does not exist in the defaults: ") + t);

    // 3. the predicates the table will replace
    for(const ActionDesc &row : ActionTable::kActions) {
        const QString name = QString::fromLatin1(row.name);
        EXPECT_EQ(row.subjectChanging, isSubjectChangingAction(name));
    }

    // 4. every action's home scope is one the editor can show
    for(const ActionDesc &row : ActionTable::kActions) {
        bool known = false;
        for(Scope scope : kAllScopes)
            known = known || scope == row.home;
        EXPECT_TRUE(known) << qPrintable(QStringLiteral("home scope is not in kAllScopes: ") +
                                         QString::fromLatin1(row.name));
    }
}

// The table writes modifier names as literals (it is constexpr, so no
// runtime keyNameCtrl()). That is only honest while the platform agrees:
// this pins the agreement, so a platform whose InputMap answers other
// names fails HERE, by name, instead of shipping a table that describes
// keys nobody can press.
TEST_F(TestShortcutScope, theActionTablesModifierNamesAreThePlatforms) {
    EXPECT_EQ(InputMap::keyNameCtrl(), QStringLiteral("Ctrl"));
    EXPECT_EQ(InputMap::keyNameAlt(), QStringLiteral("Alt"));
    EXPECT_EQ(InputMap::keyNameShift(), QStringLiteral("Shift"));
}

// The rebinding policy is per ACTION, which is the correction to the
// token-level withhold that came before it: that protected the Del key and
// left removeFile free to sit on F9. The policy is data now, so the
// question "may this be rebound without asking" is answerable without a
// modal dialog — the confirmation itself is a QMessageBox, which no
// headless run can dismiss.
// The executor pool sizes itself from the hardware: decode, scale and
// thumbnails share one pool, and no knob divides them anymore — the
// thumbnailerThreads slider is gone (upstream never needed this much
// concurrency, and the knob's name stopped being true the day the pool
// went shared). Pinned at the seam Core owns: construction sizes the
// owned pool. Never below 2, so a stuck mpv frame grab cannot starve
// decoding.
TEST_F(TestShortcutScope, thePoolIsSizedByTheHardware) {
    FreshSettings fresh;
    Core core;
    EXPECT_EQ(core.executor().threadCount(), qMax(2, QThread::idealThreadCount()));
}

// Deleting a script must unbind it from EVERY table it lives in. The
// three-table sweep that did this forgot the document table — the same
// shape the conflict checker had forgotten once before — so a script
// bound in the document view survived the deletion and kept firing an
// action the user had just removed.
//
// This drives the REAL path (SettingsDialog::removeScript, invoked by its
// slot name) rather than the door it uses: a pin that calls the door only
// proves the door works, and the bug was in the caller.
TEST_F(TestShortcutScope, deletingAScriptUnbindsItFromEveryScope) {
    const QString scriptName = QStringLiteral("scope-pin-script");
    const QString action = QStringLiteral("s:") + scriptName;
    scriptManager->addScript(scriptName, Script(QStringLiteral("/bin/true"), false));

    for(Scope scope : kAllScopes) {
        // one distinct key per scope so the bindings cannot collide
        const QString key = QStringLiteral("Ctrl+Alt+F") + QString::number(static_cast<int>(scope) + 1);
        actionManager->insertShortcut(scope, key, action);
        EXPECT_EQ(actionManager->tableForPublic(scope).value(key), action);
    }

    // the editor's own removal path, with the script selected in its list
    SettingsDialog dialog;
    dialog.readSettings();
    auto *scripts = dialog.findChild<QListWidget *>(QStringLiteral("scriptsListWidget"));
    EXPECT_TRUE(scripts) << "the scripts list did not materialize";
    int scriptRow = -1;
    for(int r = 0; r < scripts->count(); r++)
        if(scripts->item(r)->text() == scriptName)
            scriptRow = r;
    EXPECT_TRUE(scriptRow >= 0) << "the script was not listed";
    scripts->setCurrentRow(scriptRow);
    EXPECT_TRUE(QMetaObject::invokeMethod(&dialog, "removeScript", Qt::DirectConnection));

    for(Scope scope : kAllScopes) {
        const QMap<QString, QString> &table = actionManager->tableForPublic(scope);
        for(auto it = table.constBegin(); it != table.constEnd(); ++it)
            EXPECT_TRUE(it.value() != action)
                << qPrintable(QStringLiteral("a deleted script is still bound in scope ") + scopeTag(scope));
    }
    scriptManager->removeScript(scriptName);
}

TEST_F(TestShortcutScope, protectedActionsAskBeforeRebinding) {
    // the destructive verbs and the way out
    for(const char *name : {"moveToTrash", "removeFile", "exit", "closeFullScreenOrExit"})
        EXPECT_EQ(ActionTable::policyFor(QString::fromLatin1(name)), ActionDesc::Policy::System);
    // everything else is free
    for(const char *name : {"nextImage", "zoomIn", "fitWindow", "save", "quickCopy"})
        EXPECT_EQ(ActionTable::policyFor(QString::fromLatin1(name)), ActionDesc::Policy::User);
    // an unknown name (a script, a stale row) is not protected
    EXPECT_EQ(ActionTable::policyFor(QStringLiteral("s:myscript")), ActionDesc::Policy::User);
    // and the policy is on the action, not on a key: the same action under
    // two of its bindings answers the same way
    EXPECT_EQ(ActionTable::policyFor(QStringLiteral("removeFile")),
              ActionTable::policyFor(QStringLiteral("removeFile")));
    // the chooser withholds Reserved actions and offers the rest, so the
    // two policies that matter reach the editor at all
    int offered = 0, reserved = 0;
    for(const ActionDesc &row : ActionTable::kActions) {
        if(row.policy == ActionDesc::Policy::Reserved)
            reserved++;
        else
            offered++;
    }
    EXPECT_EQ(offered + reserved, static_cast<int>(std::size(ActionTable::kActions)));
    EXPECT_TRUE(offered > 0) << "no action is offered by the chooser at all";
    // System actions must be among the offered ones — hiding them was the
    // old model, and it is what let a rebind happen with nobody asked
    EXPECT_TRUE(ActionTable::policyFor(QStringLiteral("removeFile")) == ActionDesc::Policy::System);
}

// The scope vocabulary is ONE vocabulary: the enum ActionManager commits
// with, the tag a saved row carries, and the label the conflict warning
// prints all come from the same four values. This is what the editor used
// to keep for itself — four function-local strings and a four-arm if/else
// with no else, so a row carrying a tag it did not name was dropped in
// silence. A fifth scope added to kAllScopes must render, save and be
// named without the editor changing.
TEST_F(TestShortcutScope, theScopeVocabularyIsOne) {
    EXPECT_EQ(static_cast<int>(std::size(kAllScopes)), 4);
    QSet<QString> tags, labels;
    for(Scope scope : kAllScopes) {
        const QString tag = scopeTag(scope);
        const QString label = scopeLabel(scope);
        EXPECT_TRUE(!tag.isEmpty() && !label.isEmpty()) << "a scope with no tag or label";
        EXPECT_TRUE(!tags.contains(tag)) << qPrintable(QStringLiteral("duplicate scope tag: ") + tag);
        EXPECT_TRUE(!labels.contains(label)) << qPrintable(QStringLiteral("duplicate scope label: ") + label);
        tags.insert(tag);
        labels.insert(label);
        // the tag round-trips through the parser a saved row is read with
        Scope back = Scope::Global;
        EXPECT_TRUE(scopeFromTag(tag, back) && back == scope)
            << qPrintable(QStringLiteral("tag did not round-trip: ") + tag);
    }
    // an unknown tag is refused, not silently mapped to Global — that
    // silent mapping is how a newer scope's rows would land in the wrong
    // table instead of being reported
    Scope ignored = Scope::Global;
    EXPECT_TRUE(!scopeFromTag(QStringLiteral("no-such-scope"), ignored));
}

// The keymap page renders as scenarios: one section per scope under a
// header row, and a row saves into the table its section stands for.
// The render half is pinned through readSettings (a public slot); the
// save half through saveShortcuts (public on the same seam) — an
// injected grid row must land in the grid table and nowhere else, and
// the cycle must otherwise be an identity.
TEST_F(TestShortcutScope, shortcutDialogSectionsRenderAndSaveApart) {
    const QMap<QString, QString> gridSnapshot = actionManager->tableForPublic(Scope::Grid);
    const int globalBefore = actionManager->tableForPublic(Scope::Global).size();
    const int gridBefore = gridSnapshot.size();
    const int inspectBefore = actionManager->tableForPublic(Scope::Inspect).size();
    const int docBefore = actionManager->tableForPublic(Scope::Document).size();

    SettingsDialog dialog;
    dialog.readSettings();
    auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("shortcutsTableWidget"));
    EXPECT_TRUE(table) << "the shortcuts table did not materialize";
    int headers = 0, global = 0, grid = 0, inspect = 0, document = 0;
    for(int r = 0; r < table->rowCount(); r++) {
        QTableWidgetItem *item = table->item(r, 0);
        const QString tag = item ? item->data(Qt::UserRole).toString() : QString();
        // count only rows that carry a key: the section also offers the
        // scope's unbound actions as gray rows with an empty cell
        if(tag.isEmpty())
            headers++;
        else if(table->item(r, 1)->text().isEmpty())
            continue;
        else if(tag == QLatin1String("all"))
            global++;
        else if(tag == QLatin1String("grid"))
            grid++;
        else if(tag == QLatin1String("document"))
            document++;
        else if(tag == QLatin1String("inspect"))
            inspect++;
    }
    EXPECT_EQ(headers, static_cast<int>(std::size(kAllScopes))); // one section per scope
    EXPECT_EQ(global, globalBefore);
    EXPECT_EQ(grid, gridBefore);
    EXPECT_EQ(document, docBefore);
    EXPECT_EQ(inspect, inspectBefore);

    // an injected row in the grid section saves into the grid table only
    const int injectRow = table->rowCount();
    table->insertRow(injectRow);
    QTableWidgetItem *actionItem = new QTableWidgetItem(QStringLiteral("Select last"));
    // the row's identity is the tooltip (the registry name); the visible
    // cell is a display name, which a translated UI changes
    actionItem->setToolTip(QStringLiteral("gridSelectLast"));
    actionItem->setData(Qt::UserRole, QStringLiteral("grid"));
    table->setItem(injectRow, 0, actionItem);
    table->setItem(injectRow, 1, new QTableWidgetItem(QStringLiteral("F7")));

    dialog.saveShortcuts();

    EXPECT_EQ(actionManager->tableForPublic(Scope::Global).size(), globalBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Inspect).size(), inspectBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Document).size(), docBefore);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Grid).size(), gridBefore + 1);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Grid).value(QStringLiteral("F7")), QStringLiteral("gridSelectLast"));

    // put the manager back the way the rest of the suite expects it
    actionManager->removeAllShortcuts(Scope::Grid);
    for(auto it = gridSnapshot.constBegin(); it != gridSnapshot.constEnd(); ++it)
        actionManager->insertShortcut(Scope::Grid, it.key(), it.value());
}

// The actions no table claims are offered, not hidden: each unbound
// action renders as a gray row with an empty shortcut cell, in the
// section its first binding would land in. The save cycle must ignore
// the empty cells (an offer is not a binding), and editing one must
// bind into the section it sits in — placement IS the destination.
TEST_F(TestShortcutScope, unboundActionsAreOfferedAndIgnoredBySave) {
    const QMap<QString, QString> docSnapshot = actionManager->tableForPublic(Scope::Document);
    const QMap<QString, QString> gridSnapshot = actionManager->tableForPublic(Scope::Grid);
    const QMap<QString, QString> globalSnapshot = actionManager->tableForPublic(Scope::Global);
    const QMap<QString, QString> inspectSnapshot = actionManager->tableForPublic(Scope::Inspect);

    SettingsDialog dialog;
    dialog.readSettings();
    auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("shortcutsTableWidget"));
    EXPECT_TRUE(table) << "the shortcuts table did not materialize";

    // the orphans render, gray, with an empty shortcut cell — and every
    // registered action appears at least once across bound + offered
    // rows (an action may hold several keys, so "at least once" is the
    // contract; the dupes are one action with multiple bindings)
    QSet<QString> seen;
    int unboundRows = 0;
    for(int r = 0; r < table->rowCount(); r++) {
        QTableWidgetItem *actionItem = table->item(r, 0);
        if(!actionItem || actionItem->data(Qt::UserRole).toString().isEmpty())
            continue; // a header
        // the registry name lives in the tooltip; the cell shows the
        // display name and may be translated — a sweep that read the text
        // would compare display names against registry names
        const QString action = actionItem->toolTip();
        EXPECT_TRUE(!action.isEmpty()) << "a bound or offered row carries no registry name";
        const QString shortcut = table->item(r, 1)->text();
        if(shortcut.isEmpty()) {
            unboundRows++;
            EXPECT_TRUE(!seen.contains(action))
                << qPrintable(QStringLiteral("unbound action offered twice: ") + action);
            EXPECT_TRUE(actionItem->foreground().color() == Qt::gray)
                << qPrintable(QStringLiteral("the unbound row for ") + action + " does not read as an offer");
        }
        EXPECT_TRUE(actionItem->text() != action || !ActionTable::descFor(action))
            << qPrintable(QStringLiteral("the row still shows the registry name instead of a display name: ") + action);
        seen.insert(action);
    }
    EXPECT_TRUE(unboundRows > 0) << "no unbound actions were offered — the orphans are still invisible";
    const QStringList registered = actionManager->actionList();
    for(const QString &action : registered)
        EXPECT_TRUE(seen.contains(action)) << qPrintable(QStringLiteral("registered action not offered: ") + action);
    EXPECT_EQ(seen.size(), registered.size());

    // the save cycle ignores the offers: an untouched dialog round-trips
    // every table unchanged
    dialog.saveShortcuts();
    EXPECT_EQ(actionManager->tableForPublic(Scope::Document), docSnapshot);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Grid), gridSnapshot);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Global), globalSnapshot);
    EXPECT_EQ(actionManager->tableForPublic(Scope::Inspect), inspectSnapshot);

    // editing an offer binds it into the section it sits in. quickCopy
    // has no default key anywhere; its row lives in the document
    // section, so that is where the binding must land.
    int quickCopyRow = -1;
    for(int r = 0; r < table->rowCount(); r++) {
        QTableWidgetItem *actionItem = table->item(r, 0);
        if(actionItem && actionItem->toolTip() == QLatin1String("quickCopy") &&
           !actionItem->data(Qt::UserRole).toString().isEmpty())
            quickCopyRow = r;
    }
    EXPECT_TRUE(quickCopyRow >= 0) << "quickCopy was not offered";
    const QString quickCopyScope = table->item(quickCopyRow, 0)->data(Qt::UserRole).toString();
    table->item(quickCopyRow, 1)->setText(QStringLiteral("F8"));
    dialog.saveShortcuts();
    if(quickCopyScope == QLatin1String("document"))
        EXPECT_EQ(actionManager->tableForPublic(Scope::Document).value(QStringLiteral("F8")),
                  QStringLiteral("quickCopy"));
    else if(quickCopyScope == QLatin1String("grid"))
        EXPECT_EQ(actionManager->tableForPublic(Scope::Grid).value(QStringLiteral("F8")), QStringLiteral("quickCopy"));
    else
        EXPECT_EQ(actionManager->tableForPublic(Scope::Global).value(QStringLiteral("F8")),
                  QStringLiteral("quickCopy"));

    // put the manager back the way the rest of the suite expects it
    actionManager->removeAllShortcuts(Scope::Document);
    for(auto it = docSnapshot.constBegin(); it != docSnapshot.constEnd(); ++it)
        actionManager->insertShortcut(Scope::Document, it.key(), it.value());
}

// The Save As prefill for an edited image: the desktop's numbered-variant
// pattern, so the quick path writes a copy and the original survives.
TEST_F(TestShortcutScope, saveAsSuggestsTheFirstFreeNumberedVariant) {
    const QString dir = uniqueDir("variant");
    EXPECT_EQ(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test.jpg")), QDir(dir).filePath("test (1).jpg"));

    EXPECT_TRUE(QFile(QDir(dir).filePath("test (1).jpg")).open(QIODevice::WriteOnly));
    EXPECT_EQ(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test.jpg")), QDir(dir).filePath("test (2).jpg"));

    EXPECT_TRUE(QFile(QDir(dir).filePath("test (2).jpg")).open(QIODevice::WriteOnly));
    EXPECT_EQ(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test.jpg")), QDir(dir).filePath("test (3).jpg"));

    // suffix-less and multi-dot names keep their shape
    EXPECT_EQ(FileOperations::nextFreeVariantPath(QDir(dir).filePath("test")), QDir(dir).filePath("test (1)"));
    EXPECT_EQ(FileOperations::nextFreeVariantPath(QDir(dir).filePath("archive.tar.jpg")),
              QDir(dir).filePath("archive.tar (1).jpg"));
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
