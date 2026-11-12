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
#include "utils/actions.h"
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
    int failures = 0;
    for(const QString &slot : std::as_const(testSlots)) {
        fflush(nullptr); // fork would duplicate whatever is buffered in stdout
        pid_t pid = fork();
        if(pid == 0) {
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
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "shortcut_scope_tests.moc"
