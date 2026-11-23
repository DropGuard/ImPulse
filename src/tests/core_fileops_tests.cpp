// Core's file operations: copy, move, rename, permanent delete and trash, and
// the scripted dialogs they run through. Split out of core_nav_tests.cpp, where
// 21 of these tests sat among navigation, editing and slideshow.
//
// The boundary is what a test needs: these drive Core with a FakeInteractions
// answering the overwrite and confirm questions, and read the filesystem back.
// None of them reads an engine listing, so they do not need the async machinery
// the rest of core_nav exists for.
#include <QtTest/QSignalSpy>
#include <QtTest/qtestcase.h>
#include "gtest_qt.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <cerrno>
#include <cstdio> // std::rename: the external rename below is what a file manager
                  // does, and QFile::rename is RENAME_NOREPLACE

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

class TestCoreFileOps : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestCoreFileOps() : EngineStackFixture("core-fileops") {
    }

    // The tree the operations run against: one folder of images, addressed the
    // way these tests read it. Named here rather than inherited because this
    // suite does not share core_nav's fixture any more.
    QString m_base, m_alpha;

    static void writeImage(const QString &path) {
        EXPECT_TRUE(makeImageFile(path, QSize(8, 8)));
    }

    QString uniqueDir(const QString &tag) {
        const QString dir =
            QDir(m_home.path()).filePath(tag + QStringLiteral("-%1").arg(QCoreApplication::applicationPid()));
        QDir().mkpath(dir);
        return dir;
    }

    void SetUp() override {
        EXPECT_TRUE(m_home.path() != nullptr);
        m_base = QDir(m_home.path()).filePath("ops");
        m_alpha = QDir(m_base).filePath("alpha");
        EXPECT_TRUE(QDir().mkpath(m_alpha));
        writeImage(QDir(m_alpha).filePath("a.png"));
    }

    // A headless Core over the three fakes, built on first use. Most of this
    // suite's tests opened with the same eight lines building exactly this —
    // the fixture is what a gtest suite is for — but a few drive a component
    // directly and have no Core at all, so constructing it eagerly would make
    // them pay for a window-less Core they never touch.
    Core &core() {
        if(!m_core)
            m_core = std::make_unique<Core>(headlessDeps());
        return *m_core;
    }

    Core::Deps headlessDeps() {
        Core::Deps deps;
        deps.headless = true;
        deps.ui = &m_ui;
        deps.overlays = &m_overlays;
        deps.view = &m_view;
        return deps;
    }

    coretest::FakeInteractions m_ui;
    coretest::FakeOverlayHost m_overlays;
    coretest::RecordingView m_view;
    std::unique_ptr<Core> m_core;
};

TEST_F(TestCoreFileOps, quickCopyLandsInTheConfiguredFolder) {
    QString dest = QDir(m_home.path()).filePath("quickcopy-dest");
    settings->setQuickCopyDirectory(dest);
    EXPECT_TRUE(QDir().mkpath(dest));

    QString img = QDir(m_base).filePath("qc.png");
    writeImage(img);
    EXPECT_TRUE(core().loadPath(img));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core().currentFilePath()) == (img);
            },
            10000);
        EXPECT_EQ(core().currentFilePath(), img)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    actionManager->invokeAction(QStringLiteral("quickCopy"));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (QFileInfo::exists(QDir(dest).filePath("qc.png"))) == (true);
            },
            10000);
        EXPECT_EQ(QFileInfo::exists(QDir(dest).filePath("qc.png")), true)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // second press: the same-name file is skipped, still exactly one copy
    actionManager->invokeAction(QStringLiteral("quickCopy"));
    EXPECT_EQ(QDir(dest).entryList(QDir::Files).size(), 1);
}

TEST_F(TestCoreFileOps, moveCurrentFileRelandsOnDiskAndCopyKeepsTheOriginal) {
    const QString src = uniqueDir("mvsrc");
    const QString copyDst = uniqueDir("mvcopydst");
    const QString moveDst = uniqueDir("mvmovedst");
    const QString file = src + QStringLiteral("/moving.png");
    writeImage(file);

    EXPECT_TRUE(core().loadPath(file));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().isSettled();
            },
            10000);
        EXPECT_TRUE(core().isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    core().fileOps().copyCurrentFile(copyDst);
    EXPECT_TRUE(QFileInfo::exists(copyDst + QStringLiteral("/moving.png")))
        << "copy did not produce the file at the destination";
    EXPECT_TRUE(QFileInfo::exists(file)) << "copy consumed the original";

    // Separate destinations on purpose. Copying into the same directory the
    // move targets first makes the move a collision, and an un-scripted
    // collision is worse than a failed assert here: Core asks ui_ for the
    // overwrite decision, and the production provider (the main window)
    // blocks on a real modal dialog — the test does not fail, it hangs
    // until the QTest watchdog kills the process at 300 s.
    core().fileOps().moveCurrentFile(moveDst);
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return !QFileInfo::exists(file);
            },
            10000);
        EXPECT_TRUE(!QFileInfo::exists(file)) << "move left the original behind"
                                              << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(QFileInfo::exists(moveDst + QStringLiteral("/moving.png")));
    EXPECT_EQ(m_ui.confirmCalls, 0); // no collision on this path
}

TEST_F(TestCoreFileOps, declinedCollisionLeavesBothFilesWhereTheyWere) {
    const QString src = uniqueDir("colsrc");
    const QString dst = uniqueDir("coldst");
    const QString file = src + QStringLiteral("/same.png");
    const QString clash = dst + QStringLiteral("/same.png");
    writeImage(file);
    writeImage(clash);
    const QByteArray clashBytes = coretest::contentOf(clash);

    m_ui.confirmAnswer = false; // the overwrite question is answered NO
    EXPECT_TRUE(core().loadPath(file));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().isSettled();
            },
            10000);
        EXPECT_TRUE(core().isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    core().fileOps().moveCurrentFile(dst);

    EXPECT_EQ(m_ui.confirmCalls, 1);
    EXPECT_TRUE(QFileInfo::exists(file)) << "a declined overwrite still moved the file";
    EXPECT_TRUE(QFileInfo::exists(clash));

    core().fileOps().copyCurrentFile(dst);
    EXPECT_EQ(m_ui.confirmCalls, 2);
    // the destination kept its own bytes: the declined copy never wrote
    EXPECT_EQ(coretest::contentOf(clash), clashBytes);
    EXPECT_TRUE(QFileInfo::exists(file));
}

TEST_F(TestCoreFileOps, interactiveCopyPlacesFileWithoutDialogs) {
    const QString src = makeTxt(m_base, "f1.txt", "one");
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));

    core().fileOps().interactiveCopy({src}, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 0); // no collision, nothing to ask
    EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath("f1.txt")), QByteArray("one"));
    EXPECT_TRUE(QFileInfo::exists(src)); // a copy leaves the source alone
}

// A directory holding a symlink to a directory must not be followed. Qt's
// QFileInfo::isDir() answers true for a link that points at one, so the
// recursive copy treated it as a real subdirectory and copied the target's
// contents — and a link back up the tree would recurse without end.
TEST_F(TestCoreFileOps, interactiveCopyDoesNotFollowDirectorySymlinks) {

    const QString srcDir = QDir(m_base).filePath("srcdir");
    const QString realDir = QDir(srcDir).filePath("real");
    EXPECT_TRUE(QDir().mkpath(realDir));
    makeTxt(realDir, "inside.txt", "inner");
    // a link to a sibling directory, and one pointing back at the parent.
    // REAL symlinks via FileOperations::createSymlink: QFile::link writes a
    // .lnk shortcut on Windows, which would test the copy against shortcut
    // files. Where the host grants no link privilege, skip loudly - the same
    // discipline as the unreadable-fixture skips.
    if(!FileOperations::createSymlink(realDir, QDir(srcDir).filePath("linkdir")) ||
       !FileOperations::createSymlink(srcDir, QDir(realDir).filePath("up")))
        GTEST_SKIP() << coretest::kSymlinkPrivilegeSkip;

    const QString destDir = QDir(m_base).filePath("linkdst");
    EXPECT_TRUE(QDir().mkpath(destDir));

    core().fileOps().interactiveCopy({srcDir}, destDir);

    // the real directory and its file arrive; the links arrive as links (or
    // are skipped), and nothing recurses through them
    EXPECT_TRUE(QFileInfo::exists(QDir(destDir).filePath("srcdir/real/inside.txt")));
    // the link arrived as a link: if the copy had followed it, the destination
    // would hold a real directory with the target's contents inside
    const QFileInfo copiedLink(QDir(destDir).filePath("srcdir/linkdir"));
    EXPECT_TRUE(copiedLink.isSymLink()) << "the copy followed a directory symlink";
    EXPECT_EQ(copiedLink.symLinkTarget(), realDir);
    // and the upward link did not recurse the copy into itself
    EXPECT_TRUE(QFileInfo(QDir(destDir).filePath("srcdir/real/up")).isSymLink());
}

TEST_F(TestCoreFileOps, interactiveCopyOverwritesWhenScriptedYes) {
    const QString src = makeTxt(m_base, "f1.txt", "new");
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    makeTxt(destDir, "f1.txt", "old");
    m_ui.replaceAnswers << DialogResult{true, false, false};

    core().fileOps().interactiveCopy({src}, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1);
    EXPECT_EQ(m_ui.replaceArgs.at(0).first, src); // asked about THIS file
    EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath("f1.txt")), QByteArray("new"));
}

TEST_F(TestCoreFileOps, interactiveCopyYesToAllSticksAcrossTheBatch) {
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    QList<QString> sources;
    for(const char *n : {"f1", "f2", "f3"}) {
        sources << makeTxt(m_base, QString(n) + ".txt", n);
        makeTxt(destDir, QString(n) + ".txt", "old");
    }
    m_ui.replaceAnswers << DialogResult{true, true, false}; // one answer, whole batch

    core().fileOps().interactiveCopy(sources, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1); // the sticky answer suppresses the rest
    for(const char *n : {"f1", "f2", "f3"})
        EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
}

TEST_F(TestCoreFileOps, interactiveCopyNoToAllSkipsTheRestWithoutAsking) {
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    QList<QString> sources;
    for(const char *n : {"f1", "f2", "f3"}) {
        sources << makeTxt(m_base, QString(n) + ".txt", "new");
        makeTxt(destDir, QString(n) + ".txt", "old");
    }
    m_ui.replaceAnswers << DialogResult{false, true, false}; // no + apply to all

    core().fileOps().interactiveCopy(sources, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1); // asked once for the first collision only
    for(const char *n : {"f1", "f2", "f3"})
        EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray("old"));
}

TEST_F(TestCoreFileOps, interactiveCopyPlainYesAsksAgainForEachCollision) {
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    QList<QString> sources;
    for(const char *n : {"f1", "f2", "f3"}) {
        sources << makeTxt(m_base, QString(n) + ".txt", n);
        makeTxt(destDir, QString(n) + ".txt", "old");
    }
    for(int i = 0; i < 3; ++i)
        m_ui.replaceAnswers << DialogResult{true, false, false}; // plain yes, no all

    core().fileOps().interactiveCopy(sources, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 3); // the yes resets: one question per file
    for(const char *n : {"f1", "f2", "f3"})
        EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
}

TEST_F(TestCoreFileOps, interactiveCopyCancelStopsTheBatch) {
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    QList<QString> sources;
    for(const char *n : {"f1", "f2", "f3"}) {
        sources << makeTxt(m_base, QString(n) + ".txt", n);
        makeTxt(destDir, QString(n) + ".txt", "old");
    }
    m_ui.replaceAnswers << DialogResult{false, false, true}; // cancel

    core().fileOps().interactiveCopy(sources, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1); // the batch unwound at the first collision
    for(const char *n : {"f1", "f2", "f3"})
        EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray("old"));
}

TEST_F(TestCoreFileOps, interactiveCopyOverwritesADirectoryCollisionWhenScriptedYes) {
    const QString srcDir = QDir(m_base).filePath("sdir");
    EXPECT_TRUE(QDir().mkpath(srcDir));
    makeTxt(srcDir, "inner.txt", "inner");
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    makeTxt(destDir, "sdir", "old file");                    // a FILE where the directory lands
    m_ui.replaceAnswers << DialogResult{true, false, false}; // DIR_TO_FILE: yes

    core().fileOps().interactiveCopy({srcDir}, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1);
    EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath("sdir/inner.txt")), QByteArray("inner"));
}

TEST_F(TestCoreFileOps, interactiveCopyDeclinedDirectoryOverwriteKeepsTheFile) {
    const QString srcDir = QDir(m_base).filePath("sdir");
    EXPECT_TRUE(QDir().mkpath(srcDir));
    makeTxt(srcDir, "inner.txt", "inner");
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    makeTxt(destDir, "sdir", "old file");
    m_ui.replaceAnswers << DialogResult{false, false, false}; // no

    core().fileOps().interactiveCopy({srcDir}, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1);
    EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath("sdir")), QByteArray("old file")); // intact
    EXPECT_TRUE(!QFileInfo::exists(QDir(destDir).filePath("sdir/inner.txt")));
}

// The same guard on the move side: a directory holding a symlink to another
// directory must move the link, not walk into the target.
TEST_F(TestCoreFileOps, interactiveMoveDoesNotFollowDirectorySymlinks) {

    const QString srcDir = QDir(m_base).filePath("mvsrcdir");
    const QString realDir = QDir(srcDir).filePath("real");
    EXPECT_TRUE(QDir().mkpath(realDir));
    makeTxt(realDir, "inside.txt", "inner");
    if(!FileOperations::createSymlink(realDir, QDir(srcDir).filePath("linkdir")) ||
       !FileOperations::createSymlink(srcDir, QDir(realDir).filePath("up")))
        GTEST_SKIP() << coretest::kSymlinkPrivilegeSkip;

    const QString destDir = QDir(m_base).filePath("mvdst");
    EXPECT_TRUE(QDir().mkpath(destDir));

    core().fileOps().interactiveMove({srcDir}, destDir);

    EXPECT_TRUE(QFileInfo::exists(QDir(destDir).filePath("mvsrcdir/real/inside.txt")));
    // The links moved as links, so nothing recursed. The source directory
    // itself survives with no regular contents — removeDir refuses a
    // non-empty directory, and a directory of links is still non-empty.
    const QStringList left = QDir(srcDir).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    EXPECT_TRUE(left.isEmpty()) << "the move left behind: " << left.join(", ").toStdString();
}

TEST_F(TestCoreFileOps, interactiveMoveOverwritesADirectoryCollisionWhenScriptedYes) {
    const QString srcDir = QDir(m_base).filePath("sdir");
    EXPECT_TRUE(QDir().mkpath(srcDir));
    makeTxt(srcDir, "inner.txt", "inner");
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    makeTxt(destDir, "sdir", "old file");
    m_ui.replaceAnswers << DialogResult{true, false, false}; // DIR_TO_FILE: yes

    core().fileOps().interactiveMove({srcDir}, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1);
    EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath("sdir/inner.txt")), QByteArray("inner"));
    EXPECT_TRUE(!QFileInfo::exists(QDir(m_base).filePath("sdir/inner.txt")))
        << "a move that overwrote a file with a directory left the source behind";
}

TEST_F(TestCoreFileOps, interactiveMoveYesToAllSticksAcrossTheBatch) {
    // Its own bridge, so it cannot use the fixture's Core: the global one
    // keeps its inotify watch state for the life of the process, and two tests
    // sharing it would be sharing a watch.
    EngineBridge bridge;
    Core::Deps deps = headlessDeps();
    deps.bridge = &bridge;
    Core core(deps);
    const QString destDir = QDir(m_base).filePath("dst");
    EXPECT_TRUE(QDir().mkpath(destDir));
    QList<QString> sources;
    for(const char *n : {"m1", "m2", "m3"}) {
        sources << makeTxt(m_base, QString(n) + ".txt", n);
        makeTxt(destDir, QString(n) + ".txt", "old");
    }
    m_ui.replaceAnswers << DialogResult{true, true, false};

    core.fileOps().interactiveMove(sources, destDir);

    EXPECT_EQ(m_ui.replaceCalls, 1);
    for(const char *n : {"m1", "m2", "m3"}) {
        EXPECT_EQ(coretest::contentOf(QDir(destDir).filePath(QString(n) + ".txt")), QByteArray(n));
        EXPECT_TRUE(!QFileInfo::exists(QDir(m_base).filePath(QString(n) + ".txt")))
            << qPrintable(QString(n) + " survived a move"); // a move removes the source
    }
}

// upstream issue #612: dragging a directory onto another one corrupted
// every dotted directory name ("1. What" -> "1") — the dir move built
// its destination from baseName() — and let a directory be dropped into
// its own subtree, recursing until the reporter's 5000 entries shredded.
TEST_F(TestCoreFileOps, directoryMoveKeepsDottedNamesAndRefusesSelfMoves) {
    QString root = QDir(m_home.path()).filePath("issue612");
    QDir().mkpath(root + "/thetop/1. What/3. This is weird/X. Okay whatever");
    QDir().mkpath(root + "/thetop/1. What/5. Another one/Normal name works");
    QDir().mkpath(root + "/thetop/Another normal name/This works");
    makeTxt(root, "thetop/Another normal name/5. This is a file with a prefix but no extension", "x");
    makeTxt(root, "thetop/Another normal name/6. This is a file with a prefix and extension.png", "x");
    EXPECT_TRUE(QDir().mkpath(root + "/otherdir"));

    core().fileOps().interactiveMove({QDir(root).filePath("thetop")}, QDir(root).filePath("otherdir"));

    // dotted directory names survive the move whole
    EXPECT_TRUE(QDir(root + "/otherdir/thetop/1. What/3. This is weird/X. Okay whatever").exists());
    EXPECT_TRUE(QDir(root + "/otherdir/thetop/1. What/5. Another one/Normal name works").exists());
    EXPECT_TRUE(QFileInfo::exists(
        root + "/otherdir/thetop/Another normal name/5. This is a file with a prefix but no extension"));
    EXPECT_TRUE(!QDir(root + "/otherdir/thetop/1").exists()); // the corruption signature
    EXPECT_TRUE(!QFileInfo::exists(root + "/thetop"));        // moved, not copied

    // a directory dropped into its own subtree is refused flat
    core().fileOps().interactiveMove({root + "/otherdir/thetop"}, root + "/otherdir/thetop/1. What");
    EXPECT_TRUE(QDir(root + "/otherdir/thetop/1. What/3. This is weird").exists());
    EXPECT_TRUE(!QDir(root + "/otherdir/thetop/1. What/thetop").exists());
    EXPECT_TRUE(!QDir(root + "/otherdir/thetop/1. What/1").exists());
}

// upstream issue #505: a preload failing for a file the user is not
// viewing used to surface "Load failed" over the image on screen. Only
// the on-screen file may announce its own failure.
TEST_F(TestCoreFileOps, removePermanentDeletesAfterScriptedConfirmation) {
    EXPECT_TRUE(core().loadPath(QDir(m_alpha).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().isSettled();
            },
            10000);
        EXPECT_TRUE(core().isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    const int before = core().directoryModel()->listing().fileCount();

    core().fileOps().removePermanent();

    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return static_cast<int>(core().directoryModel()->listing().fileCount()) == (before - 1);
            },
            10000);
        EXPECT_EQ(core().directoryModel()->listing().fileCount(), before - 1)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(!QFileInfo::exists(QDir(m_alpha).filePath("a.png"))) << "the confirmed removal never reached the disk";
    EXPECT_EQ(m_ui.confirmCalls, 1);
}

// The dialog and overlay seams are replaceable after construction
// (Core::setUserInteractions / setOverlayHost), and the file operations now
// live in a controller Core builds in its constructor. A controller that
// copied the two pointers once would keep asking the provider the caller
// just replaced: the confirmation would go to the old fake and the message
// to the old host. This pins that a replacement reaches the file operations.
TEST_F(TestCoreFileOps, aSeamReplacedAfterConstructionReachesTheFileOperations) {
    EXPECT_TRUE(core().loadPath(QDir(m_alpha).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().isSettled();
            },
            10000);
        EXPECT_TRUE(core().isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    coretest::FakeInteractions replacementUi;
    replacementUi.confirmAnswer = false; // the fixture's fake would say yes
    coretest::FakeOverlayHost replacementOverlays;
    core().setUserInteractions(&replacementUi);
    core().setOverlayHost(&replacementOverlays);

    core().fileOps().removePermanent();
    EXPECT_EQ(replacementUi.confirmCalls, 1) << "the confirmation never reached the replacement";
    EXPECT_EQ(m_ui.confirmCalls, 0) << "the confirmation went to the replaced provider";
    EXPECT_TRUE(QFileInfo::exists(QDir(m_alpha).filePath("a.png"))) << "the replaced provider's yes deleted the file";

    settings->setQuickCopyDirectory(QString()); // unconfigured: the command answers with a message
    const int oldHostMessages = m_overlays.messageCalls;
    core().fileOps().quickCopyToFixedDirectory();
    EXPECT_EQ(replacementOverlays.messageCalls, 1) << "the message never reached the replacement host";
    EXPECT_EQ(m_overlays.messageCalls, oldHostMessages) << "the message went to the replaced host";
    // the clipboard half borrows the same seam
    core().transfer().copyPathClipboard();
    EXPECT_EQ(replacementOverlays.messageCalls, 2) << "the clipboard's message never reached the replacement host";
    EXPECT_EQ(m_overlays.messageCalls, oldHostMessages) << "the clipboard's message went to the replaced host";

    // the fixture's fakes outlive the test body; the replacements do not
    core().setUserInteractions(&m_ui);
    core().setOverlayHost(&m_overlays);
}

TEST_F(TestCoreFileOps, moveToTrashKeepsFilesWhenConfirmationDeclined) {
    m_ui.confirmAnswer = false;
    EXPECT_TRUE(core().loadPath(QDir(m_alpha).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().isSettled();
            },
            10000);
        EXPECT_TRUE(core().isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    const int before = core().directoryModel()->listing().fileCount();

    core().fileOps().moveToTrash();

    EXPECT_EQ(m_ui.confirmCalls, 1);
    EXPECT_EQ(core().directoryModel()->listing().fileCount(), before);
    EXPECT_TRUE(QFileInfo::exists(QDir(m_alpha).filePath("a.png")));
}

TEST_F(TestCoreFileOps, saveRequestTravelingTheInjectedHostWritesTheEdit) {
    const QString dir = uniqueDir("ioverlay");
    const QString img = QDir(dir).filePath("a.png");
    writeImage(img);

    EXPECT_TRUE(core().loadPath(img));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().isSettled();
            },
            10000);
        EXPECT_TRUE(core().isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // an in-memory crop: the disk still carries the full-size file (a
    // uniform-color fixture would make flipH byte-identical, so the edit
    // must be one a file diff can see)
    core().crop(QRect(0, 0, 6, 8));
    EXPECT_EQ(QImage(img).size(), QSize(8, 8));
    // the edit itself lit the prompt (the edited-state nudge); the leg
    // under test is measured from a clean slate
    m_overlays.saveOverlayShows = 0;
    m_overlays.saveOverlayHides = 0;

    emit m_overlays.saveRequested();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (QImage(img).size()) == (QSize(6, 8));
            },
            10000);
        EXPECT_EQ(QImage(img).size(), QSize(6, 8))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(m_overlays.saveOverlayShows, 0); // the request leg saves; it never opens the prompt
}

TEST_F(TestCoreFileOps, headlessCoreConstructsWithoutAnyWidget) {
    const int widgetsBefore = QApplication::allWidgets().size();

    EXPECT_EQ(QApplication::allWidgets().size(), widgetsBefore);
    EXPECT_TRUE(core().mainWindow() == nullptr) << "a headless Core still built a window";

    // the operations run, the display sink records, the messages land in
    // the overlay fake — the full file-ops contract without a single widget
    const QString img = QDir(m_alpha).filePath("a.png");
    EXPECT_TRUE(core().loadPath(img));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core().currentFilePath()) == (img);
            },
            10000);
        EXPECT_EQ(core().currentFilePath(), img)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (m_view.showImageCalls) == (1);
            },
            10000);
        EXPECT_EQ(m_view.showImageCalls, 1) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(m_view.lastImageSize, QSize(8, 8));

    core().fileOps().copyCurrentFile(QStringLiteral("/nonexistent-dir-xyz/nowhere"));
    EXPECT_TRUE(m_overlays.errorCalls > 0) << "the failed copy never reached the injected host";

    core().enableFolderView();
    EXPECT_EQ(core().viewState().viewMode.value(), MODE_FOLDERVIEW); // the state is the truth
}

TEST_F(TestCoreFileOps, renameCurrentSelectionForcesAfterScriptedConfirmation) {
    EXPECT_TRUE(core().loadPath(QDir(m_alpha).filePath("a.png")));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().isSettled();
            },
            10000);
        EXPECT_TRUE(core().isSettled()) << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    // the collision the rename will hit: renamed.png already exists
    const QString colliding = QDir(m_alpha).filePath("renamed.png");
    writeImage(colliding);
    const QByteArray original = coretest::contentOf(QDir(m_alpha).filePath("a.png"));

    core().fileOps().renameCurrentSelection("renamed.png");

    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return !QFileInfo::exists(QDir(m_alpha).filePath("a.png"));
            },
            10000);
        EXPECT_TRUE(!QFileInfo::exists(QDir(m_alpha).filePath("a.png")))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(QFileInfo::exists(colliding));
    // a forced rename moves the bytes, so the colliding name now holds
    // exactly what the old name held
    EXPECT_EQ(coretest::contentOf(colliding), original);
    EXPECT_EQ(m_ui.confirmCalls, 1); // the overwrite question was asked and scripted yes
}

TEST_F(TestCoreFileOps, renamingTheCurrentFileConvergesTheListingAndLandsOnTheNewName) {
    const QString dir = uniqueDir("renamelanding");
    const QString from = QDir(dir).filePath("aardvark.png");
    const QString to = QDir(dir).filePath("renamed.png");
    writeImage(from);
    writeImage(QDir(dir).filePath("zzz.png"));
    EXPECT_TRUE(core().loadPath(from));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().directoryModel()->listing().containsFile(from);
            },
            10000);
        EXPECT_TRUE(core().directoryModel()->listing().containsFile(from))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(core().directoryModel()->listing().fileCount(), 2);

    core().fileOps().renameCurrentSelection("renamed.png");

    // isSettled() cannot wait for this: it asks about pending model work,
    // not about whether the watch events arrived, and it is already true
    // the moment the rename returns. Wait on the facts themselves.
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().directoryModel()->listing().containsFile(to);
            },
            10000);
        EXPECT_TRUE(core().directoryModel()->listing().containsFile(to))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(!core().directoryModel()->listing().containsFile(from));
    EXPECT_EQ(core().directoryModel()->listing().fileCount(), 2); // lossless: nothing dropped
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().currentFilePath() == to;
            },
            10000);
        EXPECT_TRUE(core().currentFilePath() == to)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(core().directoryModel()->isLoaded(to));    // landed on it, not merely listed
    EXPECT_TRUE(!core().directoryModel()->isLoaded(from)); // and the old name is gone from memory
}

// The other half of 跟手: a rename this process never issued, done by another
// program while the view sits on the file. No call site knows the
// destination, so only the event can carry the view across — which is what
// Core::onFileRenamed is for, and the reason the old pendingRenameTarget
// arm in onFileAdded is gone: an arm that had to be armed by the code
// performing the rename could never fire for a rename it did not perform.
// The name change also crosses a sort boundary (aardvark -> zebra under name
// sort), so the row moves and the view still has to land on it.
TEST_F(TestCoreFileOps, externallyRenamingTheCurrentFileKeepsItTheCurrentFile) {
    const QString dir = uniqueDir("extrenamelanding");
    const QString from = QDir(dir).filePath("aardvark.png");
    const QString to = QDir(dir).filePath("zebra.png");
    writeImage(from);
    writeImage(QDir(dir).filePath("zzz.png"));
    EXPECT_TRUE(core().loadPath(from));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().directoryModel()->listing().containsFile(from);
            },
            10000);
        EXPECT_TRUE(core().directoryModel()->listing().containsFile(from))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(core().currentFilePath(), from);

    EXPECT_TRUE(std::rename(from.toLocal8Bit().constData(), to.toLocal8Bit().constData()) == 0);

    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().currentFilePath() == to;
            },
            10000);
        EXPECT_TRUE(core().currentFilePath() == to)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(core().directoryModel()->isLoaded(to)); // on it, not merely pointing at it
    EXPECT_TRUE(!core().directoryModel()->isLoaded(from));
    EXPECT_EQ(core().directoryModel()->listing().fileCount(), 2); // lossless, as under an issued rename
}

// The watcher's contract for the image on screen: an external rewrite is
// followed (the entry re-decodes, the screen repaints through itemUpdated),
// while an image holding uncommitted edits is never clobbered — the disk
// does not write over memory, exactly as bare keys do not write over disk.
// The clobber used to BE the model's reload (cache.remove over an edited
// entry), and Core's onFileModified was the empty slot under a `!! fixme`.
TEST_F(TestCoreFileOps, externalModificationFollowsTheDiskButNeverClobbersEdits) {
    const QString dir = uniqueDir("watchmod");
    const QString file = QDir(dir).filePath("watched.png");
    writeImage(file);
    EXPECT_TRUE(core().loadPath(file));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core().currentFilePath()) == (file);
            },
            10000);
        EXPECT_EQ(core().currentFilePath(), file)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    // the ctor's first-run message and anything loadPath says are the baseline
    // noise; what the test asserts is that the watcher path below adds nothing
    const int messageBaseline = m_overlays.messageCalls;

    // (a) unedited, through the real watcher: an external rewrite is
    // followed down to a repaint. The watcher leg gets a wide window —
    // under the check target's -j4 the rescan→re-list chain stretches
    // past the suite's usual 10 s, while a quiet machine lands it in
    // milliseconds (QTRY exits early).
    QImage bigger(20, 10, QImage::Format_RGB32);
    bigger.fill(Qt::blue);
    EXPECT_TRUE(bigger.save(file, "png"));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core().directoryModel()->getImage(file)->size()) == (QSize(20, 10));
            },
            30000);
        EXPECT_EQ(core().directoryModel()->getImage(file)->size(), QSize(20, 10))
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return m_view.showImageCalls >= 2;
            },
            30000);
        EXPECT_TRUE(m_view.showImageCalls >= 2)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    } // the repaint rode itemUpdated, same event
    // (and the flip below must not race it)
    EXPECT_EQ(m_overlays.messageCalls, messageBaseline); // silent: nothing held

    // (b) edited, driven at the model slot itself (invoked by name): flip
    // first, rewrite the disk, then hand the mirror event straight to the
    // edge detector. This pins the whole contract — announce the
    // divergence once, keep the edit, land no repaint — without betting
    // on watcher latency.
    core().flipH();
    EXPECT_TRUE(core().directoryModel()->getImage(file)->isEdited());
    const int showCallsBefore = m_view.showImageCalls;
    QImage other(4, 4, QImage::Format_RGB32);
    other.fill(Qt::green);
    // separate the two stamps: a coarse-jiffy filesystem stamps same-tick
    // saves identically, and the mtime IS the change signal here
    QTest::qWait(150);
    EXPECT_TRUE(other.save(file, "png"));
    EXPECT_TRUE(QMetaObject::invokeMethod(core().directoryModel().get(), "onFileModified", Q_ARG(QString, file)));

    EXPECT_EQ(m_overlays.messageCalls, messageBaseline + 1);
    EXPECT_TRUE(core().directoryModel()->getImage(file)->isEdited());
    EXPECT_EQ(core().directoryModel()->getImage(file)->size(),
              QSize(20, 10));                          // the edited geometry, not the disk's 4x4
    EXPECT_EQ(m_view.showImageCalls, showCallsBefore); // no clobber repaint

    // one external save surfaces as several wire/mirror cycles: the same
    // divergence must not re-announce (the model dedups per path+mtime)
    EXPECT_TRUE(QMetaObject::invokeMethod(core().directoryModel().get(), "onFileModified", Q_ARG(QString, file)));
    EXPECT_EQ(m_overlays.messageCalls, messageBaseline + 1);
}

// The pending-work lock: while any registered surface holds unfinished
// work, folder entry and navigation are denied with feedback; resolving
// unlocks. The fake's pendingSurfaces is the test-set stand-in for the
// OverlayHost registry.
TEST_F(TestCoreFileOps, pendingSurfacesLockFolderEntryAndNavigation) {
    const QString dir = uniqueDir("pendinglock");
    const QString a = QDir(dir).filePath("a.png");
    const QString b = QDir(dir).filePath("b.png");
    writeImage(a);
    writeImage(b);

    EXPECT_TRUE(core().loadPath(a));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core().currentFilePath()) == (a);
            },
            10000);
        EXPECT_EQ(core().currentFilePath(), a)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    // after the Core exists and the load is done: the denials below are what
    // the count is about
    const int messageBaseline = m_overlays.messageCalls;

    m_overlays.pendingSurfaces = true;

    EXPECT_TRUE(!core().loadPath(b)); // denied, target untouched
    EXPECT_EQ(core().currentFilePath(), a);
    core().enableFolderView(); // denied: folder stays out of reach
    EXPECT_EQ(core().viewState().viewMode.value(), MODE_DOCUMENT);
    core().nextImage(); // denied
    EXPECT_EQ(core().currentFilePath(), a);
    EXPECT_EQ(m_overlays.messageCalls, messageBaseline + 3); // one message per denial

    m_overlays.pendingSurfaces = false;
    EXPECT_TRUE(core().loadPath(b)); // resolved: the walk is open again
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core().currentFilePath()) == (b);
            },
            10000);
        EXPECT_EQ(core().currentFilePath(), b)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    // the STATE-held leg: an uncommitted edit locks navigation even with
    // no visible surface (the save prompt may be settings-off — the work
    // exists regardless of its rendering). discard resolves it.
    EXPECT_TRUE(core().loadPath(b)); // settle on b
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core().currentFilePath()) == (b);
            },
            10000);
        EXPECT_EQ(core().currentFilePath(), b)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    core().flipH();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return core().directoryModel()->getImage(b)->isEdited();
            },
            10000);
        EXPECT_TRUE(core().directoryModel()->getImage(b)->isEdited())
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(!core().loadPath(b)); // the walk away is denied: the edit is uncommitted
    EXPECT_EQ(core().currentFilePath(), b);
    core().discardEdits();
    EXPECT_TRUE(core().loadPath(b)); // resolved by discard
}

// The NavigationController pilot's payoff pin: the controller constructs,
// attaches and walks WITHOUT Core — no crop seam, no DialogHost, no ui_
// fake, no window tree. The kernel contract lands on the recording host,
// which is the first time guiSetImage/updateInfoString/showGui are
// assertable independently of the window. The engine stack below is the
// same shape Core's constructor builds, minus everything that is not
// navigation's.
TEST_F(TestCoreFileOps, navigationControllerConstructsAndWalksStandalone) {
    WorkerPool pool(2);
    EngineBridge &bridge = EngineBridge::instance();
    Thumbnailer thumbnailer(pool);
    DirectoryPresenter thumbPresenter(thumbnailer);
    DirectoryPresenter folderPresenter(thumbnailer);

    State state;
    RecordingNavHost host;

    NavigationController nav(&state, &host, &m_view, &m_overlays, thumbPresenter, folderPresenter);
    DirectoryModel model(pool, bridge);
    nav.attachModel(&model);

    const QString dir = uniqueDir("navstandalone");
    const QString a = QDir(dir).filePath("a.png");
    const QString b = QDir(dir).filePath("b.png");
    writeImage(a);
    writeImage(b);

    EXPECT_TRUE(nav.loadPath(a));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (state.currentFilePath.value()) == (a);
            },
            10000);
        EXPECT_EQ(state.currentFilePath.value(), a)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return host.guiSetImageCalls >= 1;
            },
            10000);
        EXPECT_TRUE(host.guiSetImageCalls >= 1)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_TRUE(host.updateInfoStringCalls >= 1);
    // State's default delayModel IS the cold-open handshake: the first
    // ready item raises the GUI — the controller honoring the state, not
    // a load event of its own. Exactly once: the handshake consumes it.
    EXPECT_EQ(host.showGuiCalls, 1);

    nav.nextImage();
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (state.currentFilePath.value()) == (b);
            },
            10000);
        EXPECT_EQ(state.currentFilePath.value(), b)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }
    EXPECT_EQ(host.showGuiCalls, 1); // the handshake is spent; later items do not re-raise

    nav.startSlideshow();
    EXPECT_TRUE(nav.slideshowClock().isActive());
    nav.stopSlideshow();
    EXPECT_TRUE(!nav.slideshowClock().isActive());
}

// The staleness guard on scaling results, pinned at the seam Core owns:
// a result for the file on screen is delivered, a result for any other
// path is dropped. Zoom supersession is the viewer's own guard
// (setScaledPixmap compares sizes), and the scaler serializes its flights
// — the layer ownership is documented at Core::onScalingFinished. Driven
// at the scaler's signal, the very one Core attaches to.
TEST_F(TestCoreFileOps, scalingResultsAreDeliveredForTheCurrentPathOnly) {
    const QString dir = uniqueDir("scaling");
    const QString img = QDir(dir).filePath("target.png");
    writeImage(img);
    // A mock view instead of the fixture's recording one: this test is about
    // the CALLS, so it builds its own Core around the mock.
    ::testing::StrictMock<coretest::MockCoreView> mockView;
    Core::Deps deps = headlessDeps();
    deps.view = &mockView;
    Core core(deps);
    // the calls Core makes while loading are not this test's subject; the
    // scaling hand-off is. Stated here so an unmentioned call still fails.
    EXPECT_CALL(mockView, showImage).Times(::testing::AnyNumber());
    EXPECT_CALL(mockView, repaintView).Times(::testing::AnyNumber());
    EXPECT_CALL(mockView, setUpdatesEnabled).Times(::testing::AnyNumber());
    EXPECT_CALL(mockView, closeImage).Times(::testing::AnyNumber());
    EXPECT_CALL(mockView, onSortingChanged).Times(::testing::AnyNumber());
    EXPECT_CALL(mockView, showWindowDefault).Times(::testing::AnyNumber());
    EXPECT_TRUE(core.loadPath(img));
    {
        const bool qt_test_held = QTest::qWaitFor(
            [&] {
                return (core.currentFilePath()) == (img);
            },
            10000);
        EXPECT_EQ(core.currentFilePath(), img)
            << (qt_test_held ? "" : "the condition did not hold before the deadline");
    }

    auto image = core.directoryModel()->getImage(img);
    ASSERT_TRUE(image);
    const ScalerRequest req(image, QSize(16, 16), img, settings->scalingFilter(), settings->smoothUpscaling());
    auto result = std::make_shared<const QImage>(16, 16, QImage::Format_RGB32);

    // The whole contract in two lines: the result for the path on screen is
    // delivered exactly once, and the one for a path nobody is looking at is
    // never delivered. Under RecordingView these were a counter read before
    // and after; here the expectation IS the assertion.
    {
        ::testing::InSequence seq;
        EXPECT_CALL(mockView, onScalingFinished).Times(1); // delivered: the path on screen
        EXPECT_CALL(mockView, onScalingFinished).Times(0); // dropped: a path nobody is looking at
    }
    emit core.directoryModel()->scaler->scalingFinished(result, req);
    const ScalerRequest stale(req.image, req.size, dir + "/elsewhere.png", req.filter, req.smoothUpscaling);
    emit core.directoryModel()->scaler->scalingFinished(result, stale);
}

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
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
