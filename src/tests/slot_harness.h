// One process per unit of work — the crash-isolation harness every suite main
// shares. A red or crashing unit must name itself, not take the suite with it;
// three consecutive abnormal units stop the run before the burst outlives its
// evidence (the parent-side counter lives in this loop, the per-child core cap
// in core_test_helpers.h).
//
// POSIX forks; Windows has no fork, and QProcess is the faithful translation:
// the parent re-launches THIS executable once per unit — every suite's own
// argc>1 child path already runs exactly one unit — and reads the exit code
// where waitpid stood. The child-side fixture construction stays where it
// belongs: in each suite's lambda (the fork child) and main argc>1 branch (the
// re-spawned child). Nothing about the units' semantics lives here.
#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaMethod>
#include <QStandardPaths>
#include <QMetaObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstdlib>
#include <functional>

#ifndef Q_OS_WIN
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace slot_harness {

// QTest slots of one fixture class, in declaration order, minus the framework
// slots — the enumeration every suite main carried inline, identical in each.
inline QStringList testSlots(const QMetaObject *mo) {
    QStringList out;
    for(int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        const QByteArray name = m.name();
        if(m.methodType() == QMetaMethod::Slot && m.access() == QMetaMethod::Private && m.parameterCount() == 0 &&
           name != "initTestCase" && name != "cleanupTestCase" && name != "init" && name != "cleanup")
            out << QString::fromUtf8(name);
    }
    return out;
}

// runUnitInChild(unit) runs one unit and returns its process exit code. On
// POSIX it is called in the forked child; on Windows the parent re-spawns the
// executable with the unit name as argv[1] and the suite's own main(argc > 1)
// branch decides what that name means.
inline int run_units_one_process_each(int argc, char **argv,
                                      const QStringList &units,
                                      const char *suiteName,
                                      [[maybe_unused]] const std::function<int(const QString &)> &runUnitInChild) {
    (void)argc;
    (void)argv;
    // The re-entry guard. A child spawned by this harness carries the
    // QIMGV_TEST_HARNESS_CHILD marker in its environment; a process that
    // reaches this function while marked was spawned but did not consume its
    // unit routing, and one more spawn from here is the recursive
    // self-relaunch that multiplied a single suite run into a system-eating
    // process tree (2026-11-16: engine_stack's child path read QIMGV_TEST_UNIT,
    // which nothing ever set). Die loudly, spawn nothing.
    if(!qEnvironmentVariableIsEmpty("QIMGV_TEST_HARNESS_CHILD")) {
        std::fprintf(stderr,
                     "%s: a spawned child re-entered the unit harness "
                     "(QIMGV_TEST_HARNESS_CHILD is set) without consuming its unit "
                     "routing — refusing to spawn. This is a harness protocol break, "
                     "not a test failure.\n", suiteName);
        return 1;
    }
    int failures = 0, consecutiveAbnormal = 0;
    for(const QString &unit : units) {
        std::fflush(nullptr); // fork would duplicate whatever is buffered in stdout
#ifdef Q_OS_WIN
        // The child writes its QTest log to a per-unit result file: the default
        // stdout logger is not reliably visible across these redirected
        // spawns (even passing units print nothing), so every unit runs with
        // -o and the parent surfaces the log when the unit is red. The unit
        // rides argv[1] (slot suites filter on it; the class suite routes
        // through the QIMGV_TEST_UNIT environment).
        const QString resultPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
            + QString("/%1_%2_result.txt").arg(suiteName, unit);
        QProcess proc;
        proc.setProgram(QCoreApplication::applicationFilePath());
        proc.setArguments({unit, "-o", resultPath + ",txt"});
        // The child's routing, carried in the environment where the child reads
        // it: the unit label rides argv[1] for the slot suites (their main
        // filters QTest on it) and QIMGV_TEST_UNIT for the class suites (a label
        // like "listing" owns no slot, so a suite routing through the meta-object
        // lookup needs it here); the marker arms the re-entry guard at the top
        // of this function. The env was the half that was never written once
        // (2026-11-16 fork bomb).
        QProcessEnvironment childEnv = QProcessEnvironment::systemEnvironment();
        childEnv.insert(QStringLiteral("QIMGV_TEST_UNIT"), unit);
        childEnv.insert(QStringLiteral("QIMGV_TEST_HARNESS_CHILD"), QStringLiteral("1"));
        proc.setProcessEnvironment(childEnv);
        // the child's own stderr (engine trace, crashes) still forwards live
        proc.setProcessChannelMode(QProcess::ForwardedChannels);
        proc.start();
        bool ok = false;
        int exitCode = -1;
        if(proc.waitForStarted(10000) && proc.waitForFinished(300000)) {
            ok = proc.exitStatus() == QProcess::NormalExit;
            exitCode = proc.exitCode();
        } else {
            // a timed-out waitForFinished does NOT kill the child: without
            // this, a hung unit outlives the accounting that tracks it,
            // carrying its spawn rights into the next run
            proc.kill();
            proc.waitForFinished(5000);
        }
        // the exit-code verdict lands BEFORE the dump: at this point `ok` still
        // means "the process exited normally", and a unit that ran to completion
        // and failed its tests exits normally with a nonzero code — checking the
        // dump against the pre-verdict `ok` skipped the log for exactly the units
        // whose failure someone needs to read (2026-11-16: five red engine_stack
        // units surfaced as one bare line each, their logs never shown)
        if(ok)
            ok = exitCode == 0;
        {
            QFile res(resultPath);
            if(res.open(QIODevice::ReadOnly)) {
                const QByteArray logged = res.readAll();
                if(!ok && !logged.isEmpty())
                    std::fwrite(logged.constData(), 1, logged.size(), stderr);
                res.close();
                res.remove();
            }
        }
        if(!ok) {
            std::fprintf(stderr, "%s: unit '%s' failed to run or exited abnormally (exit=%d)\n",
                         suiteName, unit.toUtf8().constData(), exitCode);
            ++failures;
            if(++consecutiveAbnormal == 3)
                break;
            continue;
        }
        consecutiveAbnormal = 0;
#else
        const pid_t pid = fork();
        if(pid == 0) {
            // the child: run this unit and exit with its code — exit, not a
            // return through the parent's stack, whose destructors were built
            // for the parent's lifetime
            std::exit(runUnitInChild(unit));
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if(!ok) {
            std::fprintf(stderr, "%s: unit '%s' exited abnormally (exit=%d, sig=%d)\n",
                         suiteName, unit.toUtf8().constData(),
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                         WIFSIGNALED(status) ? WTERMSIG(status) : -1);
            ++failures;
            if(++consecutiveAbnormal == 3)
                break;
            continue;
        }
        consecutiveAbnormal = 0;
#endif
    }
    return failures;
}

} // namespace slot_harness
