// Engine stack integration tests. This is the layer that has no Rust-side
// coverage (it is Qt event-loop behavior) and no other test -- it exists because
// bridge debugging by printf/strace was the slowest iteration loop in the project.
//
// Ownership rule: business semantics live in Rust tests; this suite owns the
// Qt-side integration -- async delivery through the event loop, signal ordering,
// rescan convergence.
//
// One class per domain, one binary. Each class gets its own fixture subdirectory
// because the engine holds inotify watches on those paths for the whole process:
// a class that deleted another class's directory would be deleting a live watch,
// which is how the listing used to die mid-scan with EBADF.
#pragma once

#include <cstring>

#include <QDeadlineTimer> // the convergence deadline in assertMirrorMatchesEngineExport
#include <QtEndian>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QImageIOHandler>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include "components/directorymanager/directorymanager.h"
#include "components/directorymanager/enginebridge.h"
#include "components/scaler/scaler.h"
#include "components/scaler/scalerrequest.h"
#include "components/directorymodel.h"
#include "components/cache/cache.h"
#include "components/thumbnailer/thumbnailer.h"
#include "sourcecontainers/documentinfo.h"
#include "sourcecontainers/image.h"
#include "utils/folderend.h"
#include "utils/imagelib.h"
#include "utils/filelog.h"
#include "utils/imagefactory.h"
#include "utils/randomizer.h"
#include "core_test_helpers.h"
#include "testhelpers.h"
#include "qimgv_engine.h"

class EngineStackFixture {
  public:
    explicit EngineStackFixture(const QString &domain)
        // the pid is in the path because two instances of this binary can run at
        // once (a manual run beside a ctest run), and a shared tree means one
        // process deletes directories the other is still watching
        : m_fixtureBase(QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                        "/qimgv-engine-stack-test/pid" + QString::number(QCoreApplication::applicationPid()) + "/" +
                        domain) {
        // Initialization lives in the constructor, not in an init() every suite
        // has to remember to call. Eleven classes inherited this fixture and each
        // opened its initTestCase with a bare `init();` -- a call that can be
        // omitted, and whose omission leaves a suite running against half a
        // fixture. That is the same failure shape as the AppSingletons change:
        // the hazard is "a line someone can forget", and the cure is to make
        // there be no such line.
        //
        // Upstream registers these two metatypes; a probe says the suite passes
        // without them (a pointer-to-member connection carries its argument types
        // from the declaration, so registration is only load-bearing for a
        // payload that must live in a QVariant). Kept because removing someone
        // else's safety net on an unfinished theory is not a win.
        qRegisterMetaType<std::shared_ptr<Image>>("std::shared_ptr<Image>");
        qRegisterMetaType<ScalerRequest>("ScalerRequest");
        // no removeRecursively here: the engine keeps inotify watches on these
        // paths for the life of the process, and deleting a watched directory out
        // from under it is how a later listing dies with EBADF. main() sweeps
        // this process's own tree, before any fixture has opened anything.
        //
        // Recorded rather than QVERIFY'd: this runs while the enclosing suite is
        // still being constructed, where QTest has no result to attach a failure
        // to. resetFixture() reports it, where every test goes through.
        m_fixtureReady = QDir().mkpath(m_fixtureBase);
    }

    // qFatal, not an assertion macro: this fixture is compiled into suites on
    // both frameworks, and QVERIFY reports to a QTest result object that gtest
    // never reads (silent green). A fixture that cannot write its own files has
    // no test to report into — stop, loudly, with the path named.
    void makeFile(const QString &name) {
        if(writeTestFile(m_fixtureDir + "/" + name, "x").isEmpty())
            qFatal("fixture cannot write %s/%s", qPrintable(m_fixtureDir), qPrintable(name));
    }

    // each slot gets its own directory: the engine holds inotify watches on these
    // paths for the whole process, so a watched dir must never be deleted and
    // recreated behind its back
    void resetFixture() {
        // The constructor records whether the fixture root could be created;
        // this is where that becomes a test failure. It lives here rather than
        // in a separate assertReady() the suites call from initTestCase because
        // this is the entry point every fixture-using test already goes through:
        // a test cannot use the fixture without passing the check, so there is
        // no pair of lines that can drift apart. The failure it names -- an
        // uncreatable /tmp root -- would otherwise surface far away, as whatever
        // write happened to fail first.
        if(!m_fixtureReady)
            qFatal("cannot create the fixture root: %s", qPrintable(m_fixtureBase));
        if(m_home.path() == nullptr)
            qFatal("fixture has no isolated home");
        // process-wide, not a member: gtest builds a fresh fixture object for
        // every TEST_F, so a member would restart at slot1 and hand each test a
        // directory an earlier test's engine still holds a watch on
        static int slotSeq = 0;
        ++slotSeq;
        m_fixtureDir = m_fixtureBase + QString("/slot%1").arg(slotSeq);
        if(!QDir().mkpath(m_fixtureDir))
            qFatal("fixture cannot create %s", qPrintable(m_fixtureDir));
        makeFile("img1.jpg");
        makeFile("img2.jpg");
        makeFile("img10.jpg");
        makeFile("skip.txt");
        if(!QDir(m_fixtureDir).mkdir("subdir"))
            qFatal("fixture cannot create %s/subdir", qPrintable(m_fixtureDir));
    }

    // the load environment for factory calls: tests run on their own thread,
    // so capturing from settings here is the legal side of R4's line
    LoadPolicy loadPolicy() const {
        return LoadPolicy::fromSettings();
    }

    QSize offscreenScreenPixels() {
        QScreen *screen = QApplication::primaryScreen();
        Q_ASSERT(screen != nullptr); // the offscreen platform always provides one
        return screen->size() * screen->devicePixelRatio();
    }

    bool writeBigImage(const QString &path, const QString &format, const QSize &size) {
        return makeImageFile(path, size, format.toStdString().c_str());
    }

    std::shared_ptr<Image> spyImage(const QSignalSpy &spy, int row) {
        return spy.at(row).at(0).value<std::shared_ptr<Image>>();
    }

    // Open and wait until the listing the bridge reports belongs to THIS
    // directory. Waiting on a count cannot do that: the engine keeps serving the
    // previous listing until its scan swaps the new one in, so a count can be
    // satisfied by another test's directory — which is how file names from a
    // previous slot turned up in a round-trip assertion, passing whenever the
    // machine happened to be fast enough to finish the scan first.
    void openListed(EngineBridge &bridge, const QString &dir, int sortMode = 0, bool hidden = false) {
        QSignalSpy listed(&bridge, &EngineBridge::dirListed);
        if(!bridge.openDir(dir, sortMode, hidden))
            qFatal("openListed: engine refused %s", qPrintable(dir));
        if(bridge.listedDirectory() != dir && !listed.wait(10000))
            qFatal("openListed: %s never listed (last: %s)", qPrintable(dir), qPrintable(bridge.listedDirectory()));
        if(bridge.listedDirectory() != dir)
            qFatal("openListed: listed %s, wanted %s", qPrintable(bridge.listedDirectory()), qPrintable(dir));
    }

    // "and nothing else arrives" needs a bound, not a sleep: the loader keeps a
    // path in its task table until the reply has been delivered on this thread,
    // so an idle loader means no queued loadFinished is still on its way. The
    // sleeps this replaces (600ms "a second tier would have arrived by now")
    // passed by luck whenever the machine was fast and failed by luck otherwise.
    void waitLoaderIdle(DirectoryModel &model) {
        // qWaitFor, not QTRY_VERIFY: the macro's failure path reports to QTest's
        // result object, which does not exist under gtest — it would time out
        // and say nothing. This fixture serves suites on both frameworks.
        if(!QTest::qWaitFor(
               [&] {
                   return !model.loaderBusy();
               },
               10000))
            qFatal("waitLoaderIdle: the loader never went idle");
    }

    // Serialize a listing exactly the way model.rs::write_listing does: u32 file
    // count, u32 dir count, then records (files first) of u64 handle, u64 size,
    // i64 mtime_ms, u32 name_len, u32 path_len, name bytes, path bytes, all
    // little-endian. A test needs a producer to go with EngineBridge::parseListing
    // because the states worth pinning are ones a real filesystem does not hand
    // over on demand — a snapshot that lags a file which is still on disk, for
    // instance. The handle is a placeholder: the parser skips it.
    static QByteArray arenaFor(const QStringList &files, const QStringList &dirs) {
        QByteArray arena;
        auto append = [&arena](const void *data, int n) {
            arena.append(static_cast<const char *>(data), n);
        };
        auto put32 = [&append](quint32 v) {
            v = qToLittleEndian(v);
            append(&v, 4);
        };
        auto put64 = [&append](quint64 v) {
            v = qToLittleEndian(v);
            append(&v, 8);
        };
        auto putI64 = [&append](qint64 v) {
            v = qToLittleEndian(v);
            append(&v, 8);
        };

        put32(quint32(files.size()));
        put32(quint32(dirs.size()));
        quint64 handle = 1;
        for(const QStringList *list : {&files, &dirs})
            for(const QString &path : *list) {
                const QByteArray name = QFileInfo(path).fileName().toUtf8();
                const QByteArray full = path.toUtf8();
                put64(handle++);
                put64(1);
                putI64(0);
                put32(quint32(name.size()));
                put32(quint32(full.size()));
                append(name.constData(), int(name.size()));
                append(full.constData(), int(full.size()));
            }
        return arena;
    }

    // THE mirror contract, as a predicate: the mirror's vectors are the engine's
    // export, order included — same entries, same positions. Tells whether they
    // agree and, when they do not, names the disagreement, because a count that
    // differs says "the mirror is wrong", which is not actionable, while the two
    // lists say which entry the mirror never heard about.
    // One snapshot, named. The export carries the listing version it was taken
    // at, and the mirror reports the version it has applied; equal means the
    // rows being compared are that export's rows. A resync landing between the
    // two reads makes the numbers differ, which reads as "not settled yet" and
    // lets the caller's retry loop wait — there is no window to guess about and
    // no need to compare two exports byte for byte.
    static bool mirrorMatchesEngineExport(DirectoryManager &dm, QString &diff) {
        QByteArray arena;
        quint32 fc = 0, dc = 0;
        quint64 revision = 0;
        if(!EngineBridge::instance().exportListing(arena, fc, dc, revision)) {
            diff = QStringLiteral("exportListing failed");
            return false;
        }
        // The export names the listing it came from. If the mirror has not
        // applied that version, its rows are a different snapshot and there is
        // nothing to compare — say "not settled yet" rather than inventing a
        // mismatch the caller would report as a bug. Both sides compare in the
        // wire's width: the mirror stores the low 32 bits.
        if(dm.appliedRevision() != quint32(revision)) {
            diff = QStringLiteral("the mirror is at revision %1, the export at %2")
                       .arg(dm.appliedRevision())
                       .arg(revision);
            return false;
        }
        std::vector<EngineBridge::EngineEntry> files, dirs;
        if(!EngineBridge::parseListing(arena, files, dirs)) {
            diff = QStringLiteral("parseListing rejected a %1-byte arena").arg(arena.size());
            return false;
        }
        auto joinNames = [](const std::vector<EngineBridge::EngineEntry> &entries) {
            QStringList out;
            for(const auto &e : entries)
                out << e.name;
            return out.join(QLatin1Char(' '));
        };
        auto joinMirrorFileNames = [&dm]() {
            QStringList out;
            for(unsigned long i = 0; i < dm.fileCount(); ++i)
                out << dm.fileNameAt(int(i));
            return out.join(QLatin1Char(' '));
        };
        auto fail = [&](const QString &what) {
            diff = QStringLiteral("%1 — engine: %2 files [%3] / mirror: %4 files [%5]")
                       .arg(what)
                       .arg(int(files.size()))
                       .arg(joinNames(files))
                       .arg(int(dm.fileCount()))
                       .arg(joinMirrorFileNames());
            return false;
        };
        if(files.size() != dm.fileCount())
            return fail(QStringLiteral("file count"));
        if(dirs.size() != dm.dirCount())
            return fail(QStringLiteral("dir count"));
        for(size_t i = 0; i < files.size(); ++i) {
            if(dm.filePathAt(static_cast<int>(i)) != files[i].path)
                return fail(QStringLiteral("file row %1").arg(int(i)));
        }
        for(size_t i = 0; i < dirs.size(); ++i) {
            if(dm.dirPathAt(static_cast<int>(i)) != dirs[i].path)
                return fail(QStringLiteral("dir row %1").arg(int(i)));
        }
        return true;
    }

    // Convergence is the moment the contract speaks about, so this waits for it.
    // The pump that moves an event from the engine's queue into the mirror's
    // vectors runs on the Qt event loop, and isSettled() says nothing about that
    // queue — it reports the mirrored directory, the applied sort and the absence
    // of a resync — so a pull taken the instant it turns true compares the
    // engine's now against the mirror's then. Any check that a mutation *did not*
    // produce some event needs that wait to mean anything, and a mirror that stops
    // projecting the engine still fails, after the deadline, with both listings in
    // the message.
    void assertMirrorMatchesEngineExport(DirectoryManager &dm) {
        QString diff;
        bool agrees = mirrorMatchesEngineExport(dm, diff);
        QDeadlineTimer deadline(10000);
        while(!agrees && !deadline.hasExpired()) {
            QTest::qWait(20);
            agrees = mirrorMatchesEngineExport(dm, diff);
        }
        if(!agrees)
            qFatal("mirror disagrees with the engine listing:\n%s", qPrintable(diff));
    }

  protected:
    IsolatedHome m_home;
    coretest::AppSingletons m_singletons;
    QString m_fixtureBase;
    QString m_fixtureDir;
    bool m_fixtureReady = false;
};
