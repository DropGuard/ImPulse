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

#include "gui/overlayhost.h"
#include "gui/cropcontroller.h"
#include "gui/overlays/copyoverlay.h"
#include "gui/overlays/saveconfirmoverlay.h"
#include "gui/overlays/cropoverlay.h"
class TestOverlayKeys : public QObject {
    Q_OBJECT

  private:
    IsolatedHome home;
    coretest::AppSingletons singletons;
    QString base;

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
        base = QDir(home.path()).filePath("okeys");
        QVERIFY(QDir().mkpath(base));
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
        TestOverlayKeys t;
        return QTest::qExec(&t, argc, argv) != 0 ? 1 : 0;
    }
    const QMetaObject *mo = &TestOverlayKeys::staticMetaObject;
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
            TestOverlayKeys t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        // a crashed child loses its buffered output entirely — without naming
        // the slot here, the evidence is a red ctest with nothing in the log
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "overlay_keys_tests: slot '%s' exited abnormally (exit=%d, sig=%d)\n",
                    slot.toUtf8().constData(), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "overlay_keys_tests.moc"
