#include <QTest>
#include "slot_harness.h"
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

#include "gui/overlayhost.h"
#include "gui/crop/cropcontroller.h"
#include "gui/overlays/copyoverlay.h"
#include "gui/overlays/saveconfirmoverlay.h"
#include "gui/crop/cropoverlay.h"
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
        core.mainWindow()->crop()->toggle();
        QVERIFY(core.mainWindow()->crop()->isActive());
        auto *cropOverlay = core.mainWindow()->findChild<CropOverlay *>();
        QVERIFY(cropOverlay);
        QApplication::sendEvent(cropOverlay, &esc);
        QTRY_COMPARE_WITH_TIMEOUT(!core.mainWindow()->crop()->isActive(), true, 5000);
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
        QObject::disconnect(actionManager->action("saveAs"), &QAction::triggered, nullptr, nullptr);
        QSignalSpy saveAction(actionManager->action("save"), &QAction::triggered);
        QSignalSpy saveAsAction(actionManager->action("saveAs"), &QAction::triggered);
        QSignalSpy folderViewAction(actionManager->action("folderView"), &QAction::triggered);

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

        // the QShortcutMap leg: with the surface focused, a window-delivered
        // Ctrl+S fires the save action through the map — the sendEvent legs
        // above ride the manual funnel instead, which the door keeps for the
        // keys only it can arbitrate
        core.mainWindow()->overlays()->showSaveOverlay();
        QVERIFY(saveOverlay->isVisible());
        saveOverlay->setFocus();
        QVERIFY(QApplication::focusWidget() == saveOverlay);
        core.mainWindow()->show();
        QVERIFY(QTest::qWaitForWindowExposed(core.mainWindow()));
        QSignalSpy mapSave(actionManager->action("save"), &QAction::triggered);
        coretest::deliverWindowKey(saveOverlay, Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(mapSave.count(), 1); // through the map, no veto: the chord is the prompt's own answer
        QVERIFY(!saveOverlay->isVisible());
    }

    // A floating surface that yields an unowned key to the action system is
    // following the overlay idiom — and it is right to: zoom and fullscreen
    // must keep working while a panel is open. The one class it must NOT yield
    // is the actions that change WHICH FILE is being acted on. A copy menu
    // whose Right arrow swapped the image no longer says which file it copies;
    // a crop whose Right arrow swapped the image has already lost the
    // selection (onImageChanged clears it). Pinned per surface, with the
    // display keys as the control: they must still pass through.
    void surfacesConsumeTheWalkButStillYieldDisplayKeys() {
        const QString dir = uniqueDir("subjectkeys");
        const QString a = QDir(dir).filePath("a.png");
        const QString b = QDir(dir).filePath("b.png");
        writeImage(a);
        writeImage(b);
        Core core;
        QVERIFY(core.loadPath(a));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(core.currentFilePath(), a);

        auto press = [](QWidget *w, Qt::Key key, const char *keyName, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QKeyEvent ev(QEvent::KeyPress, key, mods, coretest::scanCodeFor(QLatin1String(keyName)), 0u, 0u);
            QApplication::sendEvent(w, &ev);
        };

        // The observation is at ActionManager's own signal: the registry lock
        // (hasPendingSurface) would also stop the walk one layer up, so the
        // file path alone cannot tell whether THIS guard fired. The signal
        // fires if and only if the surface yielded the event.
        QSignalSpy nextImage(actionManager->action("nextImage"), &QAction::triggered);
        QSignalSpy fullscreen(actionManager->action("toggleFullscreen"), &QAction::triggered);

        // --- the copy menu: Right must not be yielded at all
        core.mainWindow()->overlays()->triggerCopyOverlay();
        auto *copyOverlay = core.mainWindow()->findChild<CopyOverlay *>();
        QVERIFY(copyOverlay && copyOverlay->isVisible());
        press(copyOverlay, Qt::Key_Right, "Right");
        QCOMPARE(nextImage.count(), 0); // consumed: never reached the action system
        QVERIFY(copyOverlay->isVisible());
        // ...while a display key still is (F toggles fullscreen)
        press(copyOverlay, Qt::Key_F, "F");
        QCOMPARE(fullscreen.count(), 1);
        QKeyEvent closeCopy(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(copyOverlay, &closeCopy);

        // --- the save prompt: Right must not walk either, Ctrl+S still works
        settings->setShowSaveOverlay(true);
        core.mainWindow()->overlays()->showSaveOverlay();
        auto *saveOverlay = core.mainWindow()->findChild<SaveConfirmOverlay *>();
        QVERIFY(saveOverlay && saveOverlay->isVisible());
        QObject::disconnect(actionManager->action("saveAs"), &QAction::triggered, nullptr, nullptr);
        QSignalSpy saveAction(actionManager->action("save"), &QAction::triggered);
        press(saveOverlay, Qt::Key_Right, "Right");
        QCOMPARE(nextImage.count(), 0);
        QVERIFY(saveOverlay->isVisible());
        press(saveOverlay, Qt::Key_S, "S", Qt::ControlModifier);
        QCOMPARE(saveAction.count(), 1); // the chord still gets through
        QVERIFY(!saveOverlay->isVisible());

        // --- the crop surface: Right must not walk, and the selection survives
        core.mainWindow()->crop()->toggle();
        QVERIFY(core.mainWindow()->crop()->isActive());
        auto *cropOverlay = core.mainWindow()->findChild<CropOverlay *>();
        QVERIFY(cropOverlay);
        // the selection is private; its broadcast is not. selectAll emits it
        // synchronously, and a walk would clear it through onImageChanged.
        QSignalSpy selection(cropOverlay, &CropOverlay::selectionChanged);
        cropOverlay->selectAll();
        QCOMPARE(selection.count(), 1);
        press(cropOverlay, Qt::Key_Right, "Right");
        QCOMPARE(nextImage.count(), 0); // consumed: never reached the action system
        QCOMPARE(selection.count(), 1); // and the frame was not cleared
        QCOMPARE(core.directoryModel()->getImage(a)->size(), QSize(8, 8));
    }

    // The list itself: what counts as "changes which file is being acted on".
    // Checked by name, not by key, because bindings are user-editable and a
    // remapped key must not silently reopen the hole.
    void subjectChangingActionListIsWhatItClaims() {
        QVERIFY(isSubjectChangingAction(QStringLiteral("nextImage")));
        QVERIFY(isSubjectChangingAction(QStringLiteral("prevImage")));
        QVERIFY(isSubjectChangingAction(QStringLiteral("nextDirectory")));
        QVERIFY(isSubjectChangingAction(QStringLiteral("folderView")));
        QVERIFY(isSubjectChangingAction(QStringLiteral("toggleSlideshow")));
        QVERIFY(isSubjectChangingAction(QStringLiteral("moveToTrash")));
        // the display keys are deliberately NOT in it — they are what the
        // overlay idiom exists to keep working
        QVERIFY(!isSubjectChangingAction(QStringLiteral("toggleFullscreen")));
        QVERIFY(!isSubjectChangingAction(QStringLiteral("zoomIn")));
        QVERIFY(!isSubjectChangingAction(QStringLiteral("fitWindow")));
        QVERIFY(!isSubjectChangingAction(QStringLiteral("save")));
        QVERIFY(!isSubjectChangingAction(QStringLiteral("saveAs")));
        QVERIFY(!isSubjectChangingAction(QString()));
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
        return slot_harness::runFiltered(t, argc, argv) != 0 ? 1 : 0;
    }
    return slot_harness::run_units_one_process_each(
        argc, argv, slot_harness::testSlots(&TestOverlayKeys::staticMetaObject),
        "overlay_keys_tests",
        [argv](const QString &slot) -> int {
            coretest::setChildCoreLimit();
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestOverlayKeys t; // this child's own fixture, its own temp tree and settings
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        });
}

#include "overlay_keys_tests.moc"
