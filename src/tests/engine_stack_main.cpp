// One binary, one class per domain: see engine_stack_fixture.h for what
// the classes share. Order here is the order QTest reports them in.
#include <QTest>

#include "core_test_helpers.h"
#include <QGuiApplication>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QStandardPaths>

#include <cstring>
#include <vector>

#include "slot_harness.h"

int runListingTests(int argc, char **argv);
int runWireFormatTests(int argc, char **argv);
int runFolderBoundaryTests(int argc, char **argv);
int runDecodeTests(int argc, char **argv);
int runSettingsTableTests(int argc, char **argv);
int runFileLogTests(int argc, char **argv);
int runMachineryTests(int argc, char **argv);
int runThumbnailStoreTests(int argc, char **argv);
int runThumbnailerTests(int argc, char **argv);
int runWorkerPoolTests(int argc, char **argv);
int runFileOpsTests(int argc, char **argv);
int runImageOpsTests(int argc, char **argv);
int runInfraTests(int argc, char **argv);

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    // This process's own tree, at the point where nothing is watched yet — and
    // the leftovers of runs whose process is gone. Sweeping the whole base was
    // wrong: it deletes the directories a concurrently running instance is
    // watching, which shows up later as a listing that fails or goes stale.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/qimgv-engine-stack-test";
    const QString mine = base + "/pid" + QString::number(QCoreApplication::applicationPid());
    QDir(mine).removeRecursively();
    QDir(base).mkpath(QFileInfo(mine).fileName());
#ifdef Q_OS_WIN
    // the /proc sweep is the Linux idiom for "is that pid still alive"; the
    // orphan dirs are per-pid and self-limiting, so Windows only guarantees
    // its own tree is clean rather than guessing at liveness without it
#else
    const QStringList orphans = QDir(base).entryList(QStringList() << QStringLiteral("pid*"), QDir::Dirs);
    for(const QString &stale : orphans)
        if(!QFile::exists(QStringLiteral("/proc/%1").arg(stale.mid(3))))
            QDir(base + "/" + stale).removeRecursively();
#endif
    static const struct {
        const char *name;
        int (*run)(int, char **);
    } kRuns[] = {
        {"listing", runListingTests},                {"wire_format", runWireFormatTests},
        {"folder_boundary", runFolderBoundaryTests}, {"decode", runDecodeTests},
        {"settings", runSettingsTableTests},         {"filelog", runFileLogTests},
        {"machinery", runMachineryTests},            {"thumbnail_store", runThumbnailStoreTests},
        {"thumbnailer", runThumbnailerTests},        {"worker_pool", runWorkerPoolTests},
        {"fileops", runFileOpsTests},                {"image_ops", runImageOpsTests},
        {"infra", runInfraTests},
    };
    // One process per class. Not for parallelism and not primarily for crash
    // isolation: QtTest starts a WatchDog QThread per class and joins it at
    // class end through QThread::wait, i.e. pthread_clockjoin_np — the one
    // join TSan does not intercept, same root cause as the QThreadPool waves
    // (see the QIMGV_SANITIZERS block). Ten classes in one process means ten
    // registry-live thread ids for the next class's pthread_create to collide
    // with; one process per class gives every watchdog a private registry that
    // is abandoned at exit before any pthread_t value can be reused. Per-class
    // LSan attribution at exit is a side effect worth having.
    // The unit label rides argv[1], the same channel every other suite
    // dispatches on: a unit name runs that class whole (the label is consumed
    // here and stripped from the argv the class sees — runIfOwned would answer
    // "not owned" for it, since a label like "listing" owns no slot); any other
    // positional name goes to the classes as a function filter, and the first
    // that owns it runs just that slot (the -1 yield tells not-owned apart from
    // owned-and-passed). A name that is neither is refused with the vocabulary
    // that exists — QTest would run zero tests and exit green on it.
    if(argc > 1) {
        for(const auto &r : kRuns)
            if(std::strcmp(argv[1], r.name) == 0) {
                coretest::setChildCoreLimit();
                std::vector<char *> unitArgv;
                unitArgv.push_back(argv[0]);
                for(int i = 2; i < argc; ++i)
                    unitArgv.push_back(argv[i]);
                unitArgv.push_back(nullptr);
                const int unitArgc = static_cast<int>(unitArgv.size()) - 1;
                return r.run(unitArgc, unitArgv.data()) != 0 ? 1 : 0;
            }
        for(const auto &r : kRuns) {
            coretest::setChildCoreLimit();
            const int rc = r.run(argc, argv);
            if(rc >= 0)
                return rc != 0 ? 1 : 0;
        }
        std::fprintf(stderr, "engine_stack: '%s' names neither a unit nor a test slot — units:", argv[1]);
        for(const auto &r : kRuns)
            std::fprintf(stderr, " %s", r.name);
        std::fprintf(stderr, "\n");
        return 2;
    }
    QStringList units;
    for(const auto &r : kRuns)
        units << r.name;
    return slot_harness::run_units_one_process_each(
        argc, argv, units, "engine_stack",
        [argc, argv](const QString &unit) -> int {
            coretest::setChildCoreLimit();
            for(const auto &r : kRuns)
                if(unit == r.name)
                    return r.run(argc, argv) != 0 ? 1 : 0;
            return 2;
        });
}
