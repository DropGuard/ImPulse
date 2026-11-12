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
#include <sys/wait.h>
#include <unistd.h>

#include "components/directorymanager/enginebridge.h"
#include "core.h"
#include "gui/overlays/floatingmessage.h"
#include "core_test_helpers.h"
#include "settings.h"
#include "engine_stack_fixture.h"
#include "testhelpers.h"
#include "utils/fileoperations.h"

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

        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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

        Core core;
        coretest::FakeInteractions fake; // must be installed: see below
        core.setUserInteractions(&fake);
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

        Core core;
        coretest::FakeInteractions fake;
        fake.confirmAnswer = false; // the overwrite question is answered NO
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        const QString src = makeTxt(base, "f1.txt", "one");
        const QString destDir = QDir(base).filePath("dst");
        QVERIFY(QDir().mkpath(destDir));

        core.interactiveCopy({src}, destDir);

        QCOMPARE(fake.replaceCalls, 0); // no collision, nothing to ask
        QCOMPARE(coretest::contentOf(QDir(destDir).filePath("f1.txt")), QByteArray("one"));
        QVERIFY(QFileInfo::exists(src)); // a copy leaves the source alone
    }

    void interactiveCopyOverwritesWhenScriptedYes() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core({.bridge = &bridge});
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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

        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const int before = core.directoryModel()->fileCount();

        core.removePermanent();

        QTRY_COMPARE_WITH_TIMEOUT(core.directoryModel()->fileCount(), before - 1, 10000);
        QVERIFY2(!QFileInfo::exists(QDir(alpha).filePath("a.png")), "the confirmed removal never reached the disk");
        QCOMPARE(fake.confirmCalls, 1);
    }

    void moveToTrashKeepsFilesWhenConfirmationDeclined() {
        Core core;
        coretest::FakeInteractions fake;
        fake.confirmAnswer = false;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        const int before = core.directoryModel()->fileCount();

        core.moveToTrash();

        QCOMPARE(fake.confirmCalls, 1);
        QCOMPARE(core.directoryModel()->fileCount(), before);
        QVERIFY(QFileInfo::exists(QDir(alpha).filePath("a.png")));
    }

    // #467's silent half: a batch reported only its successes. Qt's moveToTrash
    // refuses a name whose trash target (files/ prefix + name) exceeds
    // NAME_MAX, and that refusal used to vanish — "Moved to trash: 1 files"
    // for a two-file selection. The report must carry both numbers, and each
    // refusal also leaves its path and decoded reason in the session log. The
    // exact split is a property of the trash topology (a mount with no trash
    // at all fails every member), so the pin is the format, not the counts.

    // #467's silent half: a batch reported only its successes. Qt's moveToTrash
    // refuses a name whose trash target (files/ prefix + name) exceeds
    // NAME_MAX, and that refusal used to vanish — "Moved to trash: 1 files"
    // for a two-file selection. The report must carry both numbers, and each
    // refusal also leaves its path and decoded reason in the session log. The
    // exact split is a property of the trash topology (a mount with no trash
    // at all fails every member), so the pin is the format, not the counts.
    void batchTrashReportsHowManyFailed() {
        // 250 'l' + ".png" fits the source directory (254 <= NAME_MAX) but
        // cannot fit any real trash: even HOME=/ gives a 26-char files/ prefix.
        const QString longName = QString(250, QLatin1Char('l')) + QStringLiteral(".png");
        const QString longFile = QDir(alpha).filePath(longName);
        writeImage(longFile);

        coretest::FakeOverlayHost overlayFake;
        Core::Deps deps;
        deps.overlays = &overlayFake;
        Core core(deps);
        coretest::FakeInteractions fake;
        fake.confirmAnswer = true;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.mainWindow()->getFolderView()->select(QList<int>{0, 1});
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 2,
                                  "precondition broken: the folder view holds no two-file selection", 10000);

        core.moveToTrash();

        QVERIFY2(QFileInfo::exists(longFile), "the refused file vanished anyway");
        QVERIFY2(overlayFake.errorCalls > 0, "the failure report never reached the injected host");
        static const QRegularExpression bothHalves(QStringLiteral("\\d+ files, \\d+ failed"));
        QVERIFY2(bothHalves.match(overlayFake.lastError).hasMatch(),
                 qPrintable(QStringLiteral("batch report lost a half: \"") + overlayFake.lastError + "\""));
    }

    // The other half of the same report: a batch where nothing failed keeps
    // the success styling and the bare count — the repair must not turn every
    // batch into an error. Needs a working trash, so it skips where the
    // fileops suite's probe skips too.
    void batchTrashAllSuccessStillReportsTheCount() {
        const QString dir = uniqueDir("trashok");
        const QString probe = QDir(dir).filePath("trash-probe.txt");
        QVERIFY(QFile(probe).open(QIODevice::WriteOnly));
        FileOpResult probeResult;
        FileOperations::moveToTrash(probe, probeResult);
        if(probeResult != FileOpResult::SUCCESS)
            QSKIP(
                "moveToTrash unavailable on this filesystem topology (no writable top-level trash for the fixture mount)");

        writeImage(QDir(dir).filePath("a.png"));
        writeImage(QDir(dir).filePath("b.png"));

        coretest::FakeOverlayHost overlayFake;
        Core::Deps deps;
        deps.overlays = &overlayFake;
        Core core(deps);
        coretest::FakeInteractions fake;
        fake.confirmAnswer = true;
        core.setUserInteractions(&fake);
        QVERIFY(core.loadPath(QDir(dir).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);

        core.enableFolderView();
        core.mainWindow()->getFolderView()->select(QList<int>{0, 1});
        QTRY_VERIFY2_WITH_TIMEOUT(core.currentSelection().count() == 2,
                                  "precondition broken: the folder view holds no two-file selection", 10000);

        core.moveToTrash();

        QVERIFY(!QFileInfo::exists(QDir(dir).filePath("a.png")));
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath("b.png")));
        QVERIFY2(overlayFake.successCalls > 0, "the success report never reached the injected host");
        QVERIFY2(overlayFake.lastSuccess.contains(QStringLiteral("2 files")) &&
                     !overlayFake.lastSuccess.contains(QStringLiteral("failed")),
                 qPrintable(QStringLiteral("all-success batch reported: \"") + overlayFake.lastSuccess + "\""));
    }

    // The other leg of the interface, driven the other way: the fake emits
    // saveRequested — the save prompt's button, relayed — and Core saves the
    // edited image in place. No widget, no prompt, just the contract.
    void saveRequestTravelingTheInjectedHostWritesTheEdit() {
        const QString dir = uniqueDir("ioverlay");
        const QString img = QDir(dir).filePath("a.png");
        writeImage(img);

        coretest::FakeOverlayHost overlayFake;
        Core::Deps deps;
        deps.overlays = &overlayFake;
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
        QCOMPARE(view.folderEntries, 1);   // the folder leg fired exactly once
        QCOMPARE(view.documentEntries, 1); // loadPath's entry stands; no new document push
    }

    void renameCurrentSelectionForcesAfterScriptedConfirmation() {
        Core core;
        coretest::FakeInteractions fake;
        core.setUserInteractions(&fake);
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
        Core core;
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
        Core core;
        QVERIFY(core.loadPath(from));
        QTRY_VERIFY_WITH_TIMEOUT(core.directoryModel()->containsFile(from), 10000);
        QCOMPARE(core.currentFilePath(), from);

        QVERIFY(std::rename(from.toLocal8Bit().constData(), to.toLocal8Bit().constData()) == 0);

        QTRY_VERIFY_WITH_TIMEOUT(core.currentFilePath() == to, 10000);
        QVERIFY(core.directoryModel()->isLoaded(to)); // on it, not merely pointing at it
        QVERIFY(!core.directoryModel()->isLoaded(from));
        QCOMPARE(core.directoryModel()->fileCount(), 2); // lossless, as under an issued rename
    }

    // ---- DirectoryPresenter lifecycle: the view is an injected observer, the
    // model connections are per-model, and disconnectView (until now a todo)
    // is what allows a presenter to outlive one view and bind another.
};

int main(int argc, char **argv) {
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
    const QMetaObject *mo = &TestCoreFileOps::staticMetaObject;
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
        fflush(nullptr);
        pid_t pid = fork();
        if(pid == 0) {
            QByteArray nameBytes = slot.toUtf8();
            char *childArgv[] = {argv[0], nameBytes.data(), nullptr};
            TestCoreFileOps t;
            return QTest::qExec(&t, 2, childArgv) != 0 ? 1 : 0;
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            fprintf(stderr, "core_fileops: slot '%s' exited abnormally (exit=%d, sig=%d)\n", slot.toUtf8().constData(),
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
    }
    return failures;
}

#include "core_fileops_tests.moc"
