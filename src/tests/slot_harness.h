// One process per unit of work — the crash-isolation harness every suite main
// shares. A red or crashing unit must name itself, not take the suite with it;
// three consecutive abnormal units stop the run before the burst outlives its
// evidence (the parent-side counter lives in this loop, the per-child core cap
// in the spawn modifier below).
//
// The routing contract, one sentence: the unit label rides argv[1], and every
// suite main dispatches on it itself — `suite.exe someUnit` (or a single test
// slot name, via runFiltered) runs exactly that one, bare argv runs all units.
// There is no environment routing: one used to carry the unit to a class suite
// whose main read it, while the spawn wrote a different channel entirely, and
// every spawned child re-entered this harness and re-spawned — the 2026-11-16
// fork bomb (see the re-entry guard below, which converts the next protocol
// break into a loud red instead of a process tree).
//
// One process per unit means SPAWN, not fork, on every platform: a fork of the
// suite parent would inherit whatever its threads hold at fork time — a lock
// taken by a pre-fork thread stays taken in the child forever, and the parent
// carries QtGui's own pooled threads. The 2026-11-19 stranded-entry hunt
// burned real effort on exactly this suspect before exonerating it (the
// minimal repro hung zero times in 3000 forks); the class stays eliminated
// rather than trusted. QProcess re-launches THIS executable once per unit and
// reads the exit code where waitpid stood. The child's QTest log rides a
// per-unit result file (named with this process's pid — two instances of one
// suite must not trample each other); the parent prints each unit's Totals
// line so a run is readable while it happens, and surfaces a red unit's whole
// log. The child-side fixture construction stays where it belongs: in each
// suite main's argc>1 branch, which the spawned child enters like any human
// single-unit invocation. Nothing about the units' semantics lives here.
#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaMethod>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif

namespace slot_harness {

// The per-child core cap, set inside the spawned child through
// QProcess::childProcessModifier (POSIX-only hook; on Windows the cap is
// WER's business and the function is a no-op). Lives here because it is the
// harness's own guard, and the spawned child is the only process it applies
// to — a cap set in the parent would bound the parent's own crash dumps.
inline void setChildCoreLimit() {
#ifdef Q_OS_WIN
    // no RLIMIT_CORE here: crash capture is WER's, and the core-file
    // write-through the cap exists to bound does not happen on this platform
#else
    if(qEnvironmentVariableIsSet("QIMGV_TEST_CORE_UNLIMITED"))
        return; // chasing fresh evidence: the full image is the evidence
    rlimit rl{};
    if(getrlimit(RLIMIT_CORE, &rl) != 0)
        return;                     // best-effort: the guard never breaks a working harness
    const rlim_t cap = 64ULL << 20; // stacks + notes + some heap
    rl.rlim_cur = (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < cap) ? rl.rlim_max : cap;
    setrlimit(RLIMIT_CORE, &rl);
#endif
}

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

// Whether `test` has a method named `name`. qExec stops a class with
// "Function not found" when asked for a test function it does not have, which
// would make `engine_stack_tests someSlot` report a failure in every other
// class; the lookup scans the method table, because a private slot is not
// where indexOfSlot looks.
inline bool owns(const QObject &test, const QByteArray &name) {
    const QMetaObject *mo = test.metaObject();
    for(int i = 0; i < mo->methodCount(); ++i)
        if(mo->method(i).name() == name)
            return true;
    return false;
}

// The single-test entry every suite main's argc>1 branch shares. QTest answers
// a typo'd positional filter with zero tests and a green exit — a filter typo
// runs nothing while the suite looks fine, the silent-green trap the operating
// manual keeps a rule about. This validates every positional name against the
// meta-object first and refuses the run loudly, listing the tests that exist.
// Dash args pass through; the -o trio (file,format) is skipped by the same
// arithmetic the harness's own spawns ride.
inline int runFiltered(QObject &test, int argc, char **argv) {
    for(int i = 1; i < argc; ++i) {
        if(std::strcmp(argv[i], "-o") == 0) {
            i += 2;
            continue;
        }
        if(argv[i][0] == '-')
            continue;
        if(!owns(test, argv[i])) {
            std::fprintf(stderr, "%s: no test named '%s' — tests:\n", test.metaObject()->className(), argv[i]);
            // named first: range-for over the temporary would share its list
            // and then detach it (clazy range-loop-detach; the lint gate is
            // green and a new family fails it by name). NOT named `slots` —
            // that is Qt's own keyword macro, and the obvious fix does not
            // compile.
            const QStringList available = testSlots(test.metaObject());
            for(const QString &slot : available)
                std::fprintf(stderr, "  %s\n", slot.toUtf8().constData());
            return 2;
        }
    }
    return QTest::qExec(&test, argc, argv);
}

inline int run_units_one_process_each(int argc, char **argv, const QStringList &units, const char *suiteName) {
    (void)argc;
    (void)argv;
    // The re-entry guard. A child spawned by this harness carries the
    // QIMGV_TEST_HARNESS_CHILD marker in its environment; a process that
    // reaches this function while marked was spawned but did not consume its
    // unit routing, and one more spawn from here is the recursive
    // self-relaunch that multiplied a single suite run into a system-eating
    // process tree (2026-11-16). Die loudly, spawn nothing.
    if(!qEnvironmentVariableIsEmpty("QIMGV_TEST_HARNESS_CHILD")) {
        std::fprintf(stderr,
                     "%s: a spawned child re-entered the unit harness "
                     "(QIMGV_TEST_HARNESS_CHILD is set) without consuming its unit "
                     "routing — refusing to spawn. This is a harness protocol break, "
                     "not a test failure.\n",
                     suiteName);
        return 1;
    }
    int failures = 0, consecutiveAbnormal = 0;
    for(const QString &unit : units) {
        std::fflush(nullptr); // keep the parent's own output ahead of the child's forwarded channels
        // The child writes its QTest log to a per-unit result file: the default
        // stdout logger is not reliably visible across these redirected
        // spawns (even passing units print nothing), so every unit runs with
        // -o and the parent reads the file back. The pid is in the name: two
        // instances of one suite are separate trees by construction, and their
        // result files must not be one tree's leftovers read by the other.
        const QString resultPath =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
            QString("/%1_%2_%3_result.txt").arg(QCoreApplication::applicationPid()).arg(suiteName, unit);
        QProcess proc;
        proc.setProgram(QCoreApplication::applicationFilePath());
        proc.setArguments({unit, "-o", resultPath + ",txt"});
        // The marker arms the re-entry guard at the top of this function; the
        // unit label itself rides argv[1], which every suite main dispatches
        // on before anything else looks at argv.
        QProcessEnvironment childEnv = QProcessEnvironment::systemEnvironment();
        childEnv.insert(QStringLiteral("QIMGV_TEST_HARNESS_CHILD"), QStringLiteral("1"));
        proc.setProcessEnvironment(childEnv);
        // the child's own stderr (engine trace, crashes) still forwards live
        proc.setProcessChannelMode(QProcess::ForwardedChannels);
#ifndef Q_OS_WIN
        proc.setChildProcessModifier([] {
            setChildCoreLimit();
        });
#endif
        proc.start();
        const bool started = proc.waitForStarted(10000);
        bool normalExit = false;
        int exitCode = -1;
        if(started && proc.waitForFinished(300000)) {
            normalExit = proc.exitStatus() == QProcess::NormalExit;
            exitCode = proc.exitCode();
        } else if(started) {
            // a timed-out waitForFinished does NOT kill the child: without
            // this, a hung unit outlives the accounting that tracks it,
            // carrying its spawn rights into the next run
            proc.kill();
            proc.waitForFinished(5000);
        }
        // the exit-code verdict lands BEFORE the dump: at this point
        // normalExit still means "the process exited normally", and a unit
        // that ran to completion and failed its tests exits normally with a
        // nonzero code — checking the dump against the pre-verdict state
        // skipped the log for exactly the units whose failure someone needs
        // to read (2026-11-16: five red engine_stack units surfaced as one
        // bare line each, their logs never shown)
        const bool ok = started && normalExit && exitCode == 0;
        {
            QFile res(resultPath);
            if(res.open(QIODevice::ReadOnly)) {
                const QByteArray logged = res.readAll();
                res.close();
                res.remove();
                // one Totals line per unit, pass or fail — a suite that prints
                // nothing for minutes and then a wall of text teaches nothing
                // about where it is
                const int totalsAt = logged.lastIndexOf("Totals:");
                if(totalsAt >= 0) {
                    int end = logged.indexOf('\n', totalsAt);
                    if(end < 0)
                        end = logged.size();
                    std::fprintf(stderr, "%s: unit '%s' %s\n", suiteName, unit.toUtf8().constData(),
                                 logged.mid(totalsAt, end - totalsAt).trimmed().constData());
                }
                if(!ok && !logged.isEmpty())
                    std::fwrite(logged.constData(), 1, logged.size(), stderr);
            }
        }
        if(!ok) {
            std::fprintf(stderr, "%s: unit '%s' %s (exit=%d)\n", suiteName, unit.toUtf8().constData(),
                         started && normalExit ? "failed" : "exited abnormally or was killed", exitCode);
            ++failures;
            if(++consecutiveAbnormal == 3) {
                std::fprintf(stderr,
                             "%s: three consecutive abnormal units — stopping early, "
                             "the remaining unit list is not run\n",
                             suiteName);
                break;
            }
            continue;
        }
        consecutiveAbnormal = 0;
    }
    return failures;
}

} // namespace slot_harness
