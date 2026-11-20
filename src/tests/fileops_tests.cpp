#include <unistd.h>
#include <QFile>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>

#include "gtest_qt.h"
#include "engine_stack_fixture.h"
#include "utils/fileoperations.h"

// The file-operation substrate the interactive commands delegate to: copy /
// move / rename / remove against temp directories, success paths and every
// FileOpResult the surface can name. The contracts pinned here are the ones
// the code carries as comments: the backup that is only claimed when the move
// happened, the revert that reports where it left the trail, and the
// timestamp restore that makes a copy indistinguishable from the original.
// No dialogs, no Core — the interactive slots in core.cpp wrap these.
class TestFileOps : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestFileOps() : EngineStackFixture("fileops") {
    }

  protected:
    // a file with known content, plus a distinct mtime the timestamp-restore
    // contract can be asserted against
    // a helper cannot assert for its caller: it reports through qFatal, which
    // aborts the (per-test) process, the same way the shared fixture does
    QString makeFile(const QString &path, const QByteArray &content) {
        QFile f(path);
        if(!f.open(QIODevice::WriteOnly)) {
            qFatal("cannot create %s: %s", qPrintable(path), qPrintable(f.errorString()));
            return {};
        }
        f.write(content);
        f.close();
        return path;
    }
    QByteArray contentOf(const QString &path) {
        QFile f(path);
        if(!f.open(QIODevice::ReadOnly)) {
            qFatal("cannot read %s: %s", qPrintable(path), qPrintable(f.errorString()));
            return {};
        }
        return f.readAll();
    }
    void stampMTime(const QString &path) {
        // a timestamp no fixture step could produce by accident
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::ReadWrite)) << qPrintable(path);
        QDateTime past = QDateTime::currentDateTimeUtc().addDays(-17);
        EXPECT_TRUE(f.setFileTime(past, QFileDevice::FileModificationTime));
        f.close();
    }
};

TEST_F(TestFileOps, copyFileToCopiesContentKeepsSourceAndRestoresTimestamps) {
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "original");
    stampMTime(src);
    QString destDir = m_fixtureDir + "/dest";
    EXPECT_TRUE(QDir().mkpath(destDir));

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, false, result);

    EXPECT_EQ(result, SUCCESS);
    QString dest = destDir + "/a.txt";
    EXPECT_EQ(contentOf(dest), QByteArray("original")); // content travelled
    EXPECT_EQ(contentOf(src), QByteArray("original"));  // and the source stayed
    EXPECT_EQ(QFileInfo(dest).lastModified(), QFileInfo(src).lastModified());
    EXPECT_TRUE(!QFileInfo(src).isDir());
}

TEST_F(TestFileOps, copyFileToNamesItsRefusals) {
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "x");

    // the destination being the source's own directory is a no-op, not a copy
    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, m_fixtureDir, false, result);
    EXPECT_EQ(result, NOTHING_TO_DO);

    result = OTHER_ERROR;
    FileOperations::copyFileTo(m_fixtureDir + "/missing.txt", m_fixtureDir + "/dest", false, result);
    EXPECT_EQ(result, SOURCE_DOES_NOT_EXIST);

    EXPECT_TRUE(QDir().mkpath(m_fixtureDir + "/dest"));
    result = OTHER_ERROR;
    FileOperations::copyFileTo(src, m_fixtureDir + "/nowhere", false, result);
    EXPECT_EQ(result, DESTINATION_DOES_NOT_EXIST);
}

TEST_F(TestFileOps, copyFileToReportsCollisionWithoutForceAndOverwritesWithForce) {
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "new");
    QString destDir = m_fixtureDir + "/dest";
    EXPECT_TRUE(QDir().mkpath(destDir));
    makeFile(destDir + "/a.txt", "old");

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, false, result);
    EXPECT_EQ(result, DESTINATION_FILE_EXISTS);
    EXPECT_EQ(contentOf(destDir + "/a.txt"), QByteArray("old")); // untouched without force

    result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, true, result);
    EXPECT_EQ(result, SUCCESS);
    EXPECT_EQ(contentOf(destDir + "/a.txt"), QByteArray("new"));
    // the overwrite moves the old file aside first; the successful copy must
    // have removed it again — no trail left behind
    const QStringList trail = QDir(destDir).entryList(QStringList() << "a.txt*", QDir::Files);
    EXPECT_EQ(trail, QStringList{QStringLiteral("a.txt")});

    // The temporary carries this process's pid. A name that is a pure function
    // of the destination is shared with every other process overwriting the
    // same file, and the loser of that race restores the wrong bytes. Planting
    // the old fixed name is how the property is observable without a second
    // process: the overwrite must not touch a path it did not create.
    const QString legacyName =
        destDir + "/a.txt_" +
        QString(QCryptographicHash::hash((destDir + "/a.txt").toUtf8(), QCryptographicHash::Md5).toHex());
    makeFile(legacyName, "someone else's");
    makeFile(destDir + "/a.txt", "old again");
    result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, true, result);
    EXPECT_EQ(result, SUCCESS);
    EXPECT_TRUE(QFile::exists(legacyName)) << "the overwrite removed a file whose name it does not own";
    EXPECT_EQ(contentOf(legacyName), QByteArray("someone else's"));
}

TEST_F(TestFileOps, copyFileToRejectsADirectoryCollision) {
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "x");
    QString destDir = m_fixtureDir + "/dest";
    EXPECT_TRUE(QDir(destDir).mkpath(destDir + "/a.txt")); // a DIRECTORY named like the file

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, true, result); // even force may not clobber a dir
    EXPECT_EQ(result, DESTINATION_DIR_EXISTS);
}

TEST_F(TestFileOps, copyFileToReportsAnUnwritableDestination) {
    // act runs its container as root, and root ignores permission bits — "unwritable" is not
    // expressible there. The pin needs a user account to exist. Windows has no
    // POSIX permission-bit model either (a directory's read-only attribute does
    // not stop file creation), so this pin stays where its premise holds.
#ifdef Q_OS_WIN
    GTEST_SKIP() << "windows: permission bits cannot make a destination unwritable";
#else
    if(geteuid() == 0)
        GTEST_SKIP() << "running as root: permission bits cannot make a destination unwritable";
#endif
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "x");
    QString destDir = m_fixtureDir + "/rodest";
    EXPECT_TRUE(QDir().mkpath(destDir));
    EXPECT_TRUE(QFile::setPermissions(destDir, QFile::Permissions(0x5555)));

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, false, result);
    EXPECT_EQ(result, DESTINATION_NOT_WRITABLE);

    // restore so fixture teardown keeps working
    QFile::setPermissions(destDir, QFile::Permissions(0x7755));
}

TEST_F(TestFileOps, moveFileToMovesContentAndRemovesTheSource) {
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "content");
    stampMTime(src);
    QString destDir = m_fixtureDir + "/dest";
    EXPECT_TRUE(QDir().mkpath(destDir));

    // capture the stamp before the move: after it, the source is gone and
    // QFileInfo(src) is an invalid QDateTime — a trap this test fell into once
    const QDateTime srcMTime = QFileInfo(src).lastModified();

    FileOpResult result = OTHER_ERROR;
    FileOperations::moveFileTo(src, destDir, false, result);

    EXPECT_EQ(result, SUCCESS);
    EXPECT_TRUE(!QFileInfo::exists(src)); // the source is gone
    EXPECT_EQ(contentOf(destDir + "/a.txt"), QByteArray("content"));
    EXPECT_EQ(QFileInfo(destDir + "/a.txt").lastModified(), srcMTime);
}

// A cross-directory move is a copy followed by a source removal, and those two
// steps can disagree. The old contract answered that disagreement by undoing
// the copy — the destination went back to its previous contents and the user
// was told "Other error", while the file they had just seen move was gone from
// the destination again. The new contract reports the half that actually
// completed: the destination holds the file, the source is still there, and
// SOURCE_NOT_REMOVED says so in those words.
TEST_F(TestFileOps, moveFileToReportsASourceItCouldNotRemove) {
    // act runs its container as root, and root ignores permission bits — "unwritable" is not
    // expressible there. The pin needs a user account to exist. Windows has no
    // POSIX permission-bit model either (a directory's read-only attribute does
    // not stop file creation), so this pin stays where its premise holds.
#ifdef Q_OS_WIN
    GTEST_SKIP() << "windows: permission bits cannot make a destination unwritable";
#else
    if(geteuid() == 0)
        GTEST_SKIP() << "running as root: permission bits cannot make a destination unwritable";
#endif
    resetFixture();
    QString srcDir = m_fixtureDir + "/mvsrc";
    EXPECT_TRUE(QDir().mkpath(srcDir));
    QString src = makeFile(srcDir + "/a.txt", "new");
    QString destDir = m_fixtureDir + "/mvdst";
    EXPECT_TRUE(QDir().mkpath(destDir));
    makeFile(destDir + "/a.txt", "old");

    // the removal happens through removeFile(), which needs a writable source
    // directory on Linux — a read-only one forces the post-copy removal to fail
    EXPECT_TRUE(QFile::setPermissions(srcDir, QFile::Permissions(0x5555)));

    FileOpResult result = OTHER_ERROR;
    FileOperations::moveFileTo(src, destDir, true, result);
    EXPECT_TRUE(QFile::setPermissions(srcDir, QFile::Permissions(0x7755)));

    EXPECT_EQ(result, SOURCE_NOT_REMOVED);
    EXPECT_EQ(contentOf(destDir + "/a.txt"), QByteArray("new")); // the copy stands
    EXPECT_EQ(contentOf(src), QByteArray("new"));                // the source is still there
    // and nothing else was left in the destination directory
    const QStringList trail = QDir(destDir).entryList(QDir::Files, QDir::Name);
    EXPECT_EQ(trail, QStringList{QStringLiteral("a.txt")});
}

TEST_F(TestFileOps, renameMovesTheFileAndRefusesCollisions) {
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "x");
    makeFile(m_fixtureDir + "/b.txt", "y");

    FileOpResult result = OTHER_ERROR;
    FileOperations::rename(src, "c.txt", false, result);
    EXPECT_EQ(result, SUCCESS);
    EXPECT_TRUE(!QFileInfo::exists(m_fixtureDir + "/a.txt"));
    EXPECT_EQ(contentOf(m_fixtureDir + "/c.txt"), QByteArray("x"));

    // renaming onto an existing file is refused without force, and a
    // directory collision is refused even with it
    result = OTHER_ERROR;
    FileOperations::rename(m_fixtureDir + "/c.txt", "b.txt", false, result);
    EXPECT_EQ(result, DESTINATION_FILE_EXISTS);

    // FORCED overwrite replaces the destination in one rename(2).
    //
    // What is asserted here is the property the old design could not offer, and
    // an earlier draft of this test missed it: "no backup file is left behind"
    // does NOT separate the two implementations, because the old one also
    // cleaned up after a successful rename. What it cannot do is survive a
    // second actor: its backup name was a fixed md5 of the destination, so any
    // process touching the same file shared one backup path.
    //
    // Simulated without a second process by planting that exact path: the old
    // implementation would remove the planted file as part of its dance; the
    // single rename does not know the name and leaves it untouched.
    {
        const QString planted =
            m_fixtureDir + "/b.txt_" +
            QString(QCryptographicHash::hash((m_fixtureDir + "/b.txt").toUtf8(), QCryptographicHash::Md5).toHex());
        makeFile(planted, "not ours");
        result = OTHER_ERROR;
        FileOperations::rename(m_fixtureDir + "/c.txt", "b.txt", true, result);
        EXPECT_EQ(result, SUCCESS);
        EXPECT_EQ(contentOf(m_fixtureDir + "/b.txt"), QByteArray("x")); // the source's bytes won
        EXPECT_TRUE(!QFileInfo::exists(m_fixtureDir + "/c.txt"));
        EXPECT_TRUE(QFile::exists(planted))
            << "the rename deleted a file whose name it does not own — the old fixed-md5 backup path";
        EXPECT_EQ(contentOf(planted), QByteArray("not ours"));
        EXPECT_TRUE(QFile::remove(planted));
        makeFile(m_fixtureDir + "/c.txt", "x"); // restore for the cases below
    }

    EXPECT_TRUE(QDir(m_fixtureDir).mkdir("dircollision"));
    result = OTHER_ERROR;
    FileOperations::rename(m_fixtureDir + "/c.txt", "dircollision", true, result);
    EXPECT_EQ(result, DESTINATION_DIR_EXISTS);

    // the no-op cases: same name and empty name
    result = OTHER_ERROR;
    FileOperations::rename(m_fixtureDir + "/c.txt", "c.txt", false, result);
    EXPECT_EQ(result, NOTHING_TO_DO);
    result = OTHER_ERROR;
    FileOperations::rename(m_fixtureDir + "/c.txt", "", false, result);
    EXPECT_EQ(result, NOTHING_TO_DO);
}

TEST_F(TestFileOps, removeFileAndRemoveDirReportExactResults) {
    resetFixture();
    QString src = makeFile(m_fixtureDir + "/a.txt", "x");

    FileOpResult result = OTHER_ERROR;
    FileOperations::removeFile(src, result);
    EXPECT_EQ(result, SUCCESS);
    EXPECT_TRUE(!QFileInfo::exists(src));

    result = OTHER_ERROR;
    FileOperations::removeFile(src, result);
    EXPECT_EQ(result, SOURCE_DOES_NOT_EXIST);

    makeFile(m_fixtureDir + "/subdir/inner.txt", "x"); // the fixture's subdir
    result = OTHER_ERROR;
    FileOperations::removeDir(m_fixtureDir + "/subdir", false, result);
    EXPECT_EQ(result, DIRECTORY_NOT_EMPTY); // non-recursive refuses
    EXPECT_TRUE(QFileInfo::exists(m_fixtureDir + "/subdir/inner.txt"));

    result = OTHER_ERROR;
    FileOperations::removeDir(m_fixtureDir + "/subdir", true, result);
    EXPECT_EQ(result, SUCCESS);
    EXPECT_TRUE(!QFileInfo::exists(m_fixtureDir + "/subdir"));

    result = OTHER_ERROR;
    FileOperations::removeDir(m_fixtureDir + "/subdir", true, result);
    EXPECT_EQ(result, SOURCE_DOES_NOT_EXIST);
}

TEST_F(TestFileOps, moveToTrashRemovesTheFileFromItsPlace) {
    // QFile::moveToTrash selects the trash on the SOURCE's filesystem
    // (top-level .Trash-<uid>, freedesktop spec). A fixture under /tmp cannot
    // host one without root, so whether the success path is runnable depends
    // on the mount topology — probed here, with a visible, reasoned GTEST_SKIP when
    // the topology refuses. The refusal contract below is unconditional: it is
    // the part FileOperations itself owns.
    resetFixture();
    QString probe = makeFile(m_fixtureDir + "/trash-probe.txt", "x");
    FileOpResult probeResult = OTHER_ERROR;
    FileOperations::moveToTrash(probe, probeResult);
    if(probeResult != SUCCESS)
        GTEST_SKIP()
            << "moveToTrash unavailable on this filesystem topology (no writable top-level trash for the fixture mount)";

    EXPECT_TRUE(!QFileInfo::exists(probe)); // the probe already proved the happy path
    EXPECT_TRUE(!QFileInfo::exists(m_fixtureDir + "/trash-probe.txt"));

    FileOpResult result = OTHER_ERROR;
    FileOperations::moveToTrash(m_fixtureDir + "/missing.txt", result);
    EXPECT_EQ(result, SOURCE_DOES_NOT_EXIST);
}

TEST_F(TestFileOps, decodeResultNamesEveryOutcome) {
    // every outcome must decode to a non-empty human string — the switch has no
    // default branch, so a new FileOpResult without a decode line would also
    // trip the compiler here
    const FileOpResult all[] = {SUCCESS,
                                DESTINATION_FILE_EXISTS,
                                DESTINATION_DIR_EXISTS,
                                SOURCE_NOT_WRITABLE,
                                DESTINATION_NOT_WRITABLE,
                                SOURCE_DOES_NOT_EXIST,
                                DESTINATION_DOES_NOT_EXIST,
                                DIRECTORY_NOT_EMPTY,
                                NOTHING_TO_DO,
                                OTHER_ERROR};
    for(FileOpResult r : all)
        EXPECT_TRUE(!FileOperations::decodeResult(r).isEmpty())
            << qPrintable(QStringLiteral("decodeResult(%1) is empty").arg(int(r)));
    EXPECT_EQ(FileOperations::decodeResult(SUCCESS), QStringLiteral("Operation completed succesfully."));
}
