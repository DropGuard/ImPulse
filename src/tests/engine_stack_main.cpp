// One binary for the engine_stack family: thirteen test classes that share
// EngineStackFixture (a per-process temp tree, isolated HOME/XDG, the app's
// singletons). Discovery, filtering and one-process-per-case are GoogleTest's
// now — gtest_discover_tests registers every TEST_F as its own ctest entry, so
// the per-class process this file used to spell out by hand (for TSan's thread
// registry; see the QIMGV_SANITIZERS block) is the framework's default and is
// finer-grained than before.
//
// The only thing this main still does that the framework cannot is the
// per-process temp-tree sweep: the engine holds inotify watches on these paths
// for the life of the process, so a watched directory must never be deleted out
// from under a running instance. Each process cleans its own tree and the
// leftovers of processes that are gone.
#include <QGuiApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QFile>

#include <gtest/gtest.h>

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/qimgv-engine-stack-test";
    const QString mine = base + "/pid" + QString::number(QCoreApplication::applicationPid());
    QDir(mine).removeRecursively();
    QDir(base).mkpath(QFileInfo(mine).fileName());
#ifdef Q_OS_WIN
    // the /proc sweep is the Linux idiom for "is that pid still alive"; the
    // orphan dirs are per-pid and self-limiting, so Windows only guarantees its
    // own tree is clean rather than guessing at liveness without it
#else
    const QStringList orphans = QDir(base).entryList(QStringList() << QStringLiteral("pid*"), QDir::Dirs);
    for(const QString &stale : orphans)
        if(!QFile::exists(QStringLiteral("/proc/%1").arg(stale.mid(3))))
            QDir(base + "/" + stale).removeRecursively();
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
