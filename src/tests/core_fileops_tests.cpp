// Core's file operations: copy, move, rename, permanent delete and trash, and
// the scripted dialogs they run through. Split out of core_nav_tests.cpp, where
// 21 of these tests sat among navigation, editing and slideshow.
//
// The boundary is what a test needs: these drive Core with a FakeInteractions
// answering the overwrite and confirm questions, and read the filesystem back.
// None of them reads an engine listing, so they do not need the async machinery
// the rest of core_nav exists for.
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QtTest/qtestcase.h>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <cerrno>
#include <cstdio> // std::rename: the external rename below is what a file manager
                  // does, and QFile::rename is RENAME_NOREPLACE
#include "slot_harness.h"

#include "components/directorymanager/enginebridge.h"
#include "core.h"
#include "gui/overlays/floatingmessage.h"
#include "core_test_helpers.h"
#include "settings.h"
#include "engine_stack_fixture.h"
#include "testhelpers.h"
#include "utils/fileoperations.h"

// The coordinator kernel, recorded: NavigationController's calls into
// guiSetImage / updateInfoString / showGui are the INavigationHost contract —
// a real Core forwards the first into the window's display sink; this fake
// keeps counts, which is what makes those calls assertable without a window.
class RecordingNavHost : public INavigationHost {
  public:
    int guiSetImageCalls = 0;
    int updateInfoStringCalls = 0;
    int showGuiCalls = 0;
    void guiSetImage(std::shared_ptr<Image> img) override {
        Q_UNUSED(img)
        ++guiSetImageCalls;
    }
    void updateInfoString() override {
        ++updateInfoStringCalls;
    }
    void showGui() override {
        ++showGuiCalls;
    }
    int viewModeChanges = 0;
    ViewMode lastMode = MODE_DOCUMENT;
    void onViewModeChanged(ViewMode mode) override {
        ++viewModeChanges;
        lastMode = mode;
    }
};

class TestCoreFileOps : public QObject, protected EngineStackFixture {
    Q_OBJECT
  public:
    TestCoreFileOps() : EngineStackFixture("core-fileops") {
    }

    // The tree the operations run against: one folder of images, addressed the
    // way these tests read it. Named here rather than inherited because this
    // suite does not share core_nav's fixture any more.
    QString base, alpha;

    static void writeImage(const QString &path) {
        QVERIFY(makeImageFile(path, QSize(8, 8)));
    }

    QString uniqueDir(const QString &tag) {
        const QString dir =
            QDir(home.path()).filePath(tag + QStringLiteral("-%1").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

  private slots:
    void initTestCase() {
        QVERIFY(home.path() != nullptr);
        base = QDir(home.path()).filePath("ops");
        alpha = QDir(base).filePath("alpha");
        QVERIFY(QDir().mkpath(alpha));
        writeImage(QDir(alpha).filePath("a.png"));
    }
    void quickCopyLandsInTheConfiguredFolder() {
        QString dest = QDir(home.path()).filePath("quickcopy-dest");
        settings->setQuickCopyDirectory(dest);
        QVERIFY(QDir().mkpath(dest));

        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        QString img = QDir(base).filePath("qc.png");
        writeImage(img);
        QVERIFY(core.loadPath(img));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), img, 10000);

        actionManager->invokeAction(QStringLiteral("quickCopy"));
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo::exists(QDir(dest).filePath("qc.png")), true, 10000);

        // second press: the same-name file is skipped, still exactly one copy
        actionManager->invokeAction(QStringLiteral("quickCopy"));
        QCOMPARE(QDir(dest).entryList(QDir::Files).size(), 1);
    }

    void moveCurrentFileRelandsOnDiskAndCopyKeepsTheOriginal() {
        const QString src = uniqueDir("mvsrc");
        const QString copyDst = uniqueDir("mvcopydst");
        const QString moveDst = uniqueDir("mvmovedst");
        const QString file = src + QStringLiteral("/moving.png");
        writeImage(file);

        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps); // must be installed: see below
        QVERIFY(core.loadPath(file));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.copyCurrentFile(copyDst);
        QVERIFY2(QFileInfo::exists(copyDst + QStringLiteral("/moving.png")),
                 "copy did not produce the file at the destination");
        QVERIFY2(QFileInfo::exists(file), "copy consumed the original");

        // Separate destinations on purpose. Copying into the same directory the
        // move targets first makes the move a collision, and an un-scripted
        // collision is worse than a failed assert here: Core asks ui_ for the
        // overwrite decision, and the production provider (the main window)
        // blocks on a real modal dialog — the test does not fail, it hangs
        // until the QTest watchdog kills the process at 300 s.
        core.moveCurrentFile(moveDst);
        QTRY_VERIFY2_WITH_TIMEOUT(!QFileInfo::exists(file), "move left the original behind", 10000);
        QVERIFY(QFileInfo::exists(moveDst + QStringLiteral("/moving.png")));
        QCOMPARE(fake.confirmCalls, 0); // no collision on this path
    }

    void declinedCollisionLeavesBothFilesWhereTheyWere() {
        const QString src = uniqueDir("colsrc");
        const QString dst = uniqueDir("coldst");
        const QString file = src + QStringLiteral("/same.png");
        const QString clash = dst + QStringLiteral("/same.png");
        writeImage(file);
        writeImage(clash);
        const QByteArray clashBytes = coretest::contentOf(clash);

        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        fake.confirmAnswer = false; // the overwrite question is answered NO
        QVERIFY(core.loadPath(file));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.moveCurrentFile(dst);

        QCOMPARE(fake.confirmCalls, 1);
        QVERIFY2(QFileInfo::exists(file), "a declined overwrite still moved the file");
        QVERIFY(QFileInfo::exists(clash));

        core.copyCurrentFile(dst);
        QCOMPARE(fake.confirmCalls, 2);
        // the destination kept its own bytes: the declined copy never wrote
        QCOMPARE(coretest::contentOf(clash), clashBytes);
        QVERIFY(QFileInfo::exists(file));
    }

    void interactiveCopyPlacesFileWithoutDialogs() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString src = makeTxt(base, "f1.txt", "one");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));

        core.interactiveCopy({src}, destDir);

        QCOMPARE(fake.replaceCalls, 0); // no collision, nothing to ask
        QCOMPARE(coretest::contentOf(QDir(destDir).filePath("f1.txt")), QByteArray("one"));
        QVERIFY(QFileInfo::exists(src)); // a copy leaves the source alone
    }

    void interactiveCopyOverwritesWhenScriptedYes() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString src = makeTxt(base, "f1.txt", "new");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "f1.txt", "old");
        fake.replaceAnswers << DialogResult{true, false, false};

        core.interactiveCopy({src}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(fake.replaceArgs.at(0).first, src); // asked about THIS file
        QCOMPARE(coretest::contentOf(QDir(destDir).filePath("f1.txt")), QByteArray("new"));
    }

    void interactiveCopyYesToAllSticksAcrossTheBatch() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{true, true, false}; // one answer, whole batch

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1); // the sticky answer suppresses the rest
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
    }

    void interactiveCopyNoToAllSkipsTheRestWithoutAsking() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", "new");
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{false, true, false}; // no + apply to all

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1); // asked once for the first collision only
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray("old"));
    }

    void interactiveCopyPlainYesAsksAgainForEachCollision() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        for(int i = 0; i < 3; ++i)
            fake.replaceAnswers << DialogResult{true, false, false}; // plain yes, no all

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 3); // the yes resets: one question per file
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
    }

    void interactiveCopyCancelStopsTheBatch() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"f1", "f2", "f3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{false, false, true}; // cancel

        core.interactiveCopy(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1); // the batch unwound at the first collision
        for(const char *n : {"f1", "f2", "f3"})
            QCOMPARE(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray("old"));
    }

    void interactiveCopyOverwritesADirectoryCollisionWhenScriptedYes() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString srcDir = QDir(base).filePath("sdir");
        QVERIFY(QDir().mkpath(srcDir));
        makeTxt(srcDir, "inner.txt", "inner");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "sdir", "old file");                    // a FILE where the directory lands
        fake.replaceAnswers << DialogResult{true, false, false}; // DIR_TO_FILE: yes

        core.interactiveCopy({srcDir}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(coretest::contentOf(QDir(destDir).filePath("sdir/inner.txt")), QByteArray("inner"));
    }

    void interactiveCopyDeclinedDirectoryOverwriteKeepsTheFile() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString srcDir = QDir(base).filePath("sdir");
        QVERIFY(QDir().mkpath(srcDir));
        makeTxt(srcDir, "inner.txt", "inner");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "sdir", "old file");
        fake.replaceAnswers << DialogResult{false, false, false}; // no

        core.interactiveCopy({srcDir}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(coretest::contentOf(QDir(destDir).filePath("sdir")), QByteArray("old file")); // intact
        QVERIFY(!QFileInfo::exists(QDir(destDir).filePath("sdir/inner.txt")));
    }

    void interactiveMoveOverwritesADirectoryCollisionWhenScriptedYes() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        const QString srcDir = QDir(base).filePath("sdir");
        QVERIFY(QDir().mkpath(srcDir));
        makeTxt(srcDir, "inner.txt", "inner");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        makeTxt(destDir, "sdir", "old file");
        fake.replaceAnswers << DialogResult{true, false, false}; // DIR_TO_FILE: yes

        core.movePathsTo({srcDir}, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        QCOMPARE(coretest::contentOf(QDir(destDir).filePath("sdir/inner.txt")), QByteArray("inner"));
        QVERIFY2(!QFileInfo::exists(QDir(base).filePath("sdir/inner.txt")),
                 "a move that overwrote a file with a directory left the source behind");
    }

    void interactiveMoveYesToAllSticksAcrossTheBatch() {
        // Constructed through Deps rather than the no-argument form: this test
        // moves files and answers a dialog, so it needs neither the process-wide
        // engine nor a MainWindow's overlay surface. Supplying its own bridge is
        // the domain this suite owns -- the global one keeps its inotify watch
        // state for the life of the process, and two tests sharing it would be
        // sharing a watch.
        EngineBridge bridge;
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.bridge = &bridge;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));
        QList<QString> sources;
        for(const char *n : {"m1", "m2", "m3"}) {
            sources << makeTxt(base, QString(n) + ".txt", n);
            makeTxt(destDir, QString(n) + ".txt", "old");
        }
        fake.replaceAnswers << DialogResult{true, true, false};

        core.movePathsTo(sources, destDir);

        QCOMPARE(fake.replaceCalls, 1);
        for(const char *n : {"m1", "m2", "m3"}) {
            QCOMPARE(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
            QVERIFY2(!QFileInfo::exists(QDir(base).filePath(QString(n) + ".txt")),
                     qPrintable(QString(n) + " survived a move")); // a move removes the source
        }
    }

    // upstream issue #612: dragging a directory onto another one corrupted
    // every dotted directory name ("1. What" -> "1") — the dir move built
    // its destination from baseName() — and let a directory be dropped into
    // its own subtree, recursing until the reporter's 5000 entries shredded.

    // upstream issue #612: dragging a directory onto another one corrupted
    // every dotted directory name ("1. What" -> "1") — the dir move built
    // its destination from baseName() — and let a directory be dropped into
    // its own subtree, recursing until the reporter's 5000 entries shredded.
    void directoryMoveKeepsDottedNamesAndRefusesSelfMoves() {
        QString root = QDir(home.path()).filePath("issue612");
        QDir().mkpath(root + "/thetop/1. What/3. This is weird/X. Okay whatever");
        QDir().mkpath(root + "/thetop/1. What/5. Another one/Normal name works");
        QDir().mkpath(root + "/thetop/Another normal name/This works");
        makeTxt(root, "thetop/Another normal name/5. This is a file with a prefix but no extension", "x");
        makeTxt(root, "thetop/Another normal name/6. This is a file with a prefix and extension.png", "x");
        QVERIFY(QDir().mkpath(root + "/otherdir"));

        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        core.interactiveMove({QDir(root).filePath("thetop")}, QDir(root).filePath("otherdir"));

        // dotted directory names survive the move whole
        QVERIFY(QDir(root + "/otherdir/thetop/1. What/3. This is weird/X. Okay whatever").exists());
        QVERIFY(QDir(root + "/otherdir/thetop/1. What/5. Another one/Normal name works").exists());
        QVERIFY(QFileInfo::exists(
            root + "/otherdir/thetop/Another normal name/5. This is a file with a prefix but no extension"));
        QVERIFY(!QDir(root + "/otherdir/thetop/1").exists()); // the corruption signature
        QVERIFY(!QFileInfo::exists(root + "/thetop"));        // moved, not copied

        // a directory dropped into its own subtree is refused flat
        core.interactiveMove({root + "/otherdir/thetop"}, root + "/otherdir/thetop/1. What");
        QVERIFY(QDir(root + "/otherdir/thetop/1. What/3. This is weird").exists());
        QVERIFY(!QDir(root + "/otherdir/thetop/1. What/thetop").exists());
        QVERIFY(!QDir(root + "/otherdir/thetop/1. What/1").exists());
    }

    // upstream issue #505: a preload failing for a file the user is not
    // viewing used to surface "Load failed" over the image on screen. Only
    // the on-screen file may announce its own failure.

    void removePermanentDeletesAfterScriptedConfirmation() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const int before = core.directoryModel()->fileCount();

        core.removePermanent();

        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->fileCount(), before - 1, 10000);
        QVERIFY2(!QFileInfo::exists(QDir(alpha).filePath("a.png")), "the confirmed removal never reached the disk");
        QCOMPARE(fake.confirmCalls, 1);
    }

    void moveToTrashKeepsFilesWhenConfirmationDeclined() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        fake.confirmAnswer = false;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const int before = core.directoryModel()->fileCount();

        core.moveToTrash();

        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(core.directoryModel()->fileCount(), before);
        QVERIFY(QFileInfo::exists(QDir(alpha).filePath("a.png")));
    }

    // The other leg of the interface, driven the other way: the fake emits
    // saveRequested — the save prompt's button, relayed — and Core saves the
    // edited image in place. No widget, no prompt, just the contract.
    void saveRequestTravelingTheInjectedHostWritesTheEdit() {
        const QString dir = uniqueDir("ioverlay");
        const QString img = QDir(dir).filePath("a.png");
        writeImage(img);

        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        coretest::FakeInteractions fake;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(img));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        // an in-memory crop: the disk still carries the full-size file (a
        // uniform-color fixture would make flipH byte-identical, so the edit
        // must be one a file diff can see)
        core.crop(QRect(0, 0, 6, 8));
        QCOMPARE(QImage(img).size(), QSize(8, 8));
        // the edit itself lit the prompt (the edited-state nudge); the leg
        // under test is measured from a clean slate
        overlayFake.saveOverlayShows = 0;
        overlayFake.saveOverlayHides = 0;

        emit overlayFake.saveRequested();
        QTRY_COMPARE_WITH_TIMEOUT(QImage(img).size(), QSize(6, 8), 10000);
        QCOMPARE(overlayFake.saveOverlayShows, 0); // the request leg saves; it never opens the prompt
    }

    // The headless consumer: Core constructed with NO window tree at all —
    // the three seams are the whole surface. The smoke half catches any
    // construction-phase mw-> the guards missed (a widget appearing is a
    // leaked window dependency); the ops half drives the file operations and
    // watches them land in the seams, not in widgets.
    void headlessCoreConstructsWithoutAnyWidget() {
        const int widgetsBefore = QApplication::allWidgets().size();

        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);

        QCOMPARE(QApplication::allWidgets().size(), widgetsBefore);
        QVERIFY2(core.mainWindow() == nullptr, "a headless Core still built a window");

        // the operations run, the display sink records, the messages land in
        // the overlay fake — the full file-ops contract without a single widget
        const QString img = QDir(alpha).filePath("a.png");
        QVERIFY(core.loadPath(img));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), img, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(view.showImageCalls, 1, 10000);
        QCOMPARE(view.lastImageSize, QSize(8, 8));

        core.copyCurrentFile(QStringLiteral("/nonexistent-dir-xyz/nowhere"));
        QVERIFY2(overlayFake.errorCalls > 0, "the failed copy never reached the injected host");

        core.enableFolderView();
        QCOMPARE(core.viewState().viewMode.value(), MODE_FOLDERVIEW); // the state is the truth
    }

    void renameCurrentSelectionForcesAfterScriptedConfirmation() {
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        // the collision the rename will hit: renamed.png already exists
        const QString colliding = QDir(alpha).filePath("renamed.png");
        writeImage(colliding);
        const QByteArray original = coretest::contentOf(QDir(alpha).filePath("a.png"));

        core.renameCurrentSelection("renamed.png");

        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QDir(alpha).filePath("a.png")), 10000);
        QVERIFY(QFileInfo::exists(colliding));
        // a forced rename moves the bytes, so the colliding name now holds
        // exactly what the old name held
        QCOMPARE(coretest::contentOf(colliding), original);
        QCOMPARE(fake.confirmCalls, 1); // the overwrite question was asked and scripted yes
    }

    // A rename is one event: the engine pairs the kernel's MOVED_FROM and MOVED_TO
    // by cookie and answers with RENAMED(old -> new), so the mirror moves a row
    // instead of dropping one and growing another (DirectoryManager::applyEngineRename,
    // with the wire half pinned by listing_tests' liveRenameIsOneEvent). Following
    // the renamed file is still the app's job, and the two ways a file gets renamed
    // are two separate pins: this one is the rename the app issued, where a call
    // site knows the destination, and the next is one done behind its back.

    // A rename is one event: the engine pairs the kernel's MOVED_FROM and MOVED_TO
    // by cookie and answers with RENAMED(old -> new), so the mirror moves a row
    // instead of dropping one and growing another (DirectoryManager::applyEngineRename,
    // with the wire half pinned by listing_tests' liveRenameIsOneEvent). Following
    // the renamed file is still the app's job, and the two ways a file gets renamed
    // are two separate pins: this one is the rename the app issued, where a call
    // site knows the destination, and the next is one done behind its back.
    void renamingTheCurrentFileConvergesTheListingAndLandsOnTheNewName() {
        const QString dir = uniqueDir("renamelanding");
        const QString from = QDir(dir).filePath("aardvark.png");
        const QString to = QDir(dir).filePath("renamed.png");
        writeImage(from);
        writeImage(QDir(dir).filePath("zzz.png"));
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        QVERIFY(core.loadPath(from));
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->containsFile(from), 10000);
        QCOMPARE(core.directoryModel()->fileCount(), 2);

        core.renameCurrentSelection("renamed.png");

        // isSettled() cannot wait for this: it asks about pending model work,
        // not about whether the watch events arrived, and it is already true
        // the moment the rename returns. Wait on the facts themselves.
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->containsFile(to), 10000);
        QVERIFY(!core.directoryModel()->containsFile(from));
        QCOMPARE(core.directoryModel()->fileCount(), 2); // lossless: nothing dropped
        QTRY_VERIFY_WITH_TIMEOUT(core.currentFilePath() == to, 10000);
        QVERIFY(core.directoryModel()->isLoaded(to));    // landed on it, not merely listed
        QVERIFY(!core.directoryModel()->isLoaded(from)); // and the old name is gone from memory
    }

    // The other half of 跟手: a rename this process never issued, done by another
    // program while the view sits on the file. No call site knows the
    // destination, so only the event can carry the view across — which is what
    // Core::onFileRenamed is for, and the reason the old pendingRenameTarget
    // arm in onFileAdded is gone: an arm that had to be armed by the code
    // performing the rename could never fire for a rename it did not perform.
    // The name change also crosses a sort boundary (aardvark -> zebra under name
    // sort), so the row moves and the view still has to land on it.

    // The other half of 跟手: a rename this process never issued, done by another
    // program while the view sits on the file. No call site knows the
    // destination, so only the event can carry the view across — which is what
    // Core::onFileRenamed is for, and the reason the old pendingRenameTarget
    // arm in onFileAdded is gone: an arm that had to be armed by the code
    // performing the rename could never fire for a rename it did not perform.
    // The name change also crosses a sort boundary (aardvark -> zebra under name
    // sort), so the row moves and the view still has to land on it.
    void externallyRenamingTheCurrentFileKeepsItTheCurrentFile() {
        const QString dir = uniqueDir("extrenamelanding");
        const QString from = QDir(dir).filePath("aardvark.png");
        const QString to = QDir(dir).filePath("zebra.png");
        writeImage(from);
        writeImage(QDir(dir).filePath("zzz.png"));
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.overlays = &overlayFake;
        deps.view = &view;
        deps.ui = &fake;
        Core core(deps);
        QVERIFY(core.loadPath(from));
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->containsFile(from), 10000);
        QCOMPARE(core.currentFilePath(), from);

        QVERIFY(std::rename(from.toLocal8Bit().constData(), to.toLocal8Bit().constData()) == 0);

        QTRY_VERIFY_WITH_TIMEOUT(core.currentFilePath() == to, 10000);
        QVERIFY(core.directoryModel()->isLoaded(to)); // on it, not merely pointing at it
        QVERIFY(!core.directoryModel()->isLoaded(from));
        QCOMPARE(core.directoryModel()->fileCount(), 2); // lossless, as under an issued rename
    }

    // The watcher's contract for the image on screen: an external rewrite is
    // followed (the entry re-decodes, the screen repaints through itemUpdated),
    // while an image holding uncommitted edits is never clobbered — the disk
    // does not write over memory, exactly as bare keys do not write over disk.
    // The clobber used to BE the model's reload (cache.remove over an edited
    // entry), and Core's onFileModified was the empty slot under a `!! fixme`.
    void externalModificationFollowsTheDiskButNeverClobbersEdits() {
        const QString dir = uniqueDir("watchmod");
        const QString file = QDir(dir).filePath("watched.png");
        writeImage(file);
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        // the ctor's first-run message is this fork's own baseline noise
        const int messageBaseline = overlayFake.messageCalls;
        QVERIFY(core.loadPath(file));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), file, 10000);

        // (a) unedited, through the real watcher: an external rewrite is
        // followed down to a repaint. The watcher leg gets a wide window —
        // under the check target's -j4 the rescan→re-list chain stretches
        // past the suite's usual 10 s, while a quiet machine lands it in
        // milliseconds (QTRY exits early).
        QImage bigger(20, 10, QImage::Format_RGB32);
        bigger.fill(Qt::blue);
        QVERIFY(bigger.save(file, "png"));
        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->getImage(file)->size(), QSize(20, 10), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(view.showImageCalls >= 2, 30000); // the repaint rode itemUpdated, same event
                                                                   // (and the flip below must not race it)
        QCOMPARE(overlayFake.messageCalls, messageBaseline);       // silent: nothing held

        // (b) edited, driven at the model slot itself (invoked by name): flip
        // first, rewrite the disk, then hand the mirror event straight to the
        // edge detector. This pins the whole contract — announce the
        // divergence once, keep the edit, land no repaint — without betting
        // on watcher latency.
        core.flipH();
        QVERIFY(core.directoryModel()->getImage(file)->isEdited());
        const int showCallsBefore = view.showImageCalls;
        QImage other(4, 4, QImage::Format_RGB32);
        other.fill(Qt::green);
        // separate the two stamps: a coarse-jiffy filesystem stamps same-tick
        // saves identically, and the mtime IS the change signal here
        QTest::qWait(150);
        QVERIFY(other.save(file, "png"));
        QVERIFY(QMetaObject::invokeMethod(core.directoryModel().get(), "onFileModified", Q_ARG(QString, file)));

        QCOMPARE(overlayFake.messageCalls, messageBaseline + 1);
        QVERIFY(core.directoryModel()->getImage(file)->isEdited());
        QCOMPARE(core.directoryModel()->getImage(file)->size(),
                 QSize(20, 10));                        // the edited geometry, not the disk's 4x4
        QCOMPARE(view.showImageCalls, showCallsBefore); // no clobber repaint

        // one external save surfaces as several wire/mirror cycles: the same
        // divergence must not re-announce (the model dedups per path+mtime)
        QVERIFY(QMetaObject::invokeMethod(core.directoryModel().get(), "onFileModified", Q_ARG(QString, file)));
        QCOMPARE(overlayFake.messageCalls, messageBaseline + 1);
    }

    // The pending-work lock: while any registered surface holds unfinished
    // work, folder entry and navigation are denied with feedback; resolving
    // unlocks. The fake's pendingSurfaces is the test-set stand-in for the
    // OverlayHost registry.
    void pendingSurfacesLockFolderEntryAndNavigation() {
        const QString dir = uniqueDir("pendinglock");
        const QString a = QDir(dir).filePath("a.png");
        const QString b = QDir(dir).filePath("b.png");
        writeImage(a);
        writeImage(b);

        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        const int messageBaseline = overlayFake.messageCalls;
        QVERIFY(core.loadPath(a));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), a, 10000);

        overlayFake.pendingSurfaces = true;

        QVERIFY(!core.loadPath(b)); // denied, target untouched
        QCOMPARE(core.currentFilePath(), a);
        core.enableFolderView(); // denied: folder stays out of reach
        QCOMPARE(core.viewState().viewMode.value(), MODE_DOCUMENT);
        core.nextImage(); // denied
        QCOMPARE(core.currentFilePath(), a);
        QCOMPARE(overlayFake.messageCalls, messageBaseline + 3); // one message per denial

        overlayFake.pendingSurfaces = false;
        QVERIFY(core.loadPath(b)); // resolved: the walk is open again
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), b, 10000);

        // the STATE-held leg: an uncommitted edit locks navigation even with
        // no visible surface (the save prompt may be settings-off — the work
        // exists regardless of its rendering). discard resolves it.
        QVERIFY(core.loadPath(b)); // settle on b
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), b, 10000);
        core.flipH();
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->getImage(b)->isEdited(), 10000);
        QVERIFY(!core.loadPath(b)); // the walk away is denied: the edit is uncommitted
        QCOMPARE(core.currentFilePath(), b);
        core.discardEdits();
        QVERIFY(core.loadPath(b)); // resolved by discard
    }

    // The NavigationController pilot's payoff pin: the controller constructs,
    // attaches and walks WITHOUT Core — no crop seam, no DialogHost, no ui_
    // fake, no window tree. The kernel contract lands on the recording host,
    // which is the first time guiSetImage/updateInfoString/showGui are
    // assertable independently of the window. The engine stack below is the
    // same shape Core's constructor builds, minus everything that is not
    // navigation's.
    void navigationControllerConstructsAndWalksStandalone() {
        WorkerPool pool(2);
        EngineBridge &bridge = EngineBridge::instance();
        Thumbnailer thumbnailer(pool, bridge);
        DirectoryPresenter thumbPresenter(thumbnailer);
        DirectoryPresenter folderPresenter(thumbnailer);

        State state;
        coretest::RecordingView view;
        coretest::FakeOverlayHost overlays;
        RecordingNavHost host;

        NavigationController nav(&state, &host, &view, &overlays, thumbPresenter, folderPresenter);
        DirectoryModel model(pool, bridge);
        nav.attachModel(&model);

        const QString dir = uniqueDir("navstandalone");
        const QString a = QDir(dir).filePath("a.png");
        const QString b = QDir(dir).filePath("b.png");
        writeImage(a);
        writeImage(b);

        QVERIFY(nav.loadPath(a));
        QTRY_COMPARE_WITH_TIMEOUT(state.currentFilePath.value(), a, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(host.guiSetImageCalls >= 1, 10000);
        QVERIFY(host.updateInfoStringCalls >= 1);
        // State's default delayModel IS the cold-open handshake: the first
        // ready item raises the GUI — the controller honoring the state, not
        // a load event of its own. Exactly once: the handshake consumes it.
        QCOMPARE(host.showGuiCalls, 1);

        nav.nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(state.currentFilePath.value(), b, 10000);
        QCOMPARE(host.showGuiCalls, 1); // the handshake is spent; later items do not re-raise

        nav.startSlideshow();
        QVERIFY(nav.slideshowClock().isActive());
        nav.stopSlideshow();
        QVERIFY(!nav.slideshowClock().isActive());
    }

    // The staleness guard on scaling results, pinned at the seam Core owns:
    // a result for the file on screen is delivered, a result for any other
    // path is dropped. Zoom supersession is the viewer's own guard
    // (setScaledPixmap compares sizes), and the scaler serializes its flights
    // — the layer ownership is documented at Core::onScalingFinished. Driven
    // at the scaler's signal, the very one Core attaches to.
    void scalingResultsAreDeliveredForTheCurrentPathOnly() {
        const QString dir = uniqueDir("scaling");
        const QString img = QDir(dir).filePath("target.png");
        writeImage(img);
        coretest::FakeInteractions fake;
        coretest::FakeOverlayHost overlayFake;
        coretest::RecordingView view;
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &fake;
        deps.overlays = &overlayFake;
        deps.view = &view;
        Core core(deps);
        QVERIFY(core.loadPath(img));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), img, 10000);

        auto image = core.directoryModel()->getImage(img);
        QVERIFY(image);
        const ScalerRequest req(image, QSize(16, 16), img, settings->scalingFilter(), settings->smoothUpscaling());
        auto result = std::make_shared<const QImage>(16, 16, QImage::Format_RGB32);

        QCOMPARE(view.scalingResults, 0);
        emit core.directoryModel()->scaler->scalingFinished(result, req);
        QCOMPARE(view.scalingResults, 1); // delivered: the path on screen

        const ScalerRequest stale(req.image, req.size, dir + "/elsewhere.png", req.filter, req.smoothUpscaling);
        emit core.directoryModel()->scaler->scalingFinished(result, stale);
        QCOMPARE(view.scalingResults, 1); // dropped: a path nobody is looking at
    }

    // ---- DirectoryPresenter lifecycle: the view is an injected observer, the
    // model connections are per-model, and disconnectView (until now a todo)
    // is what allows a presenter to outlive one view and bind another.
};

int main(int argc, char **argv) {
    // QApplication, not QCoreApplication: the tested paths still touch no
    // widget, pixmap, movie frame or clipboard — but the action registry now
    // carries QActions, and Qt's QAction constructor resolves its
    // private-class factory through QGuiApplicationPrivate::self, which a
    // bare QCoreApplication leaves null: the first action creation would
    // segfault before any test could run. The stylesheet loader's
    // QApplication guard now passes for Settings — offscreen keeps the run
    // headless, and the QApplication suites have run that loader this way
    // all along.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Its own binary, not a second class in core_nav's: a shared binary needs a
    // slot router that inspects the class in the parent, and any fixture the
    // router constructs would be inherited by every forked child -- the
    // parent-fixture bug this suite was already bitten by.
    if(argc > 1) {
        TestCoreFileOps t;
        return QTest::qExec(&t, argc, argv);
    }
    return slot_harness::run_units_one_process_each(
        argc, argv, slot_harness::testSlots(&TestCoreFileOps::staticMetaObject),
        "core_fileops",
        [argv](const QString &slot) -> int {
            coretest::setChildCoreLimit();
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestCoreFileOps t;
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        });
}

#include "core_fileops_tests.moc"
