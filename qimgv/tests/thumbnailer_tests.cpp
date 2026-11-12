// The Thumbnailer's runtime behaviour: accounting across a batch, cancellation,
// process waits, temp-frame names, and the store sweep as a pool job. Split out
// of machinery_tests.cpp.
//
// These are the tests that exercise the shared WorkerPool, so they construct
// their own pool rather than reaching for Core's.
#include "engine_stack_fixture.h"
#include "components/thumbnailer/thumbnailer.h"
#include "components/thumbnailer/thumbnailerrunnable.h"
#include "components/workerpool/workerpool.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QThread>

#include <atomic>

class TestThumbnailer : public QObject, protected EngineStackFixture {
    Q_OBJECT
  public:
    TestThumbnailer() : EngineStackFixture("thumbnailer") {
    }

  private slots:
    void canceledThumbnailJobStillProducesAWellFormedReport() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/canceled.png", "png", QSize(4, 4)));
        LoadPolicy policy = loadPolicy();
        CancelToken token;
        token.request();
        auto thumbnail = ThumbnailerRunnable::generate(nullptr, fixtureDir + "/canceled.png", 64, /*crop=*/false,
                                                       /*force=*/true, policy, &token);
        QVERIFY(thumbnail != nullptr);
        QCOMPARE(thumbnail->size(), 64);
        QVERIFY2(thumbnail->info() == QStringLiteral("error"),
                 "a canceled decode must arrive as the error thumbnail, not as a decode result");
    }

    // waitForProcess is the seam that turns a canceled frame extraction into a
    // free worker: slice-polled, kill + reap on the mark, instead of one
    // waitForFinished call that would hold the worker for the whole timeout.
    // Pinned against real processes, the way the header promises.

    // waitForProcess is the seam that turns a canceled frame extraction into a
    // free worker: slice-polled, kill + reap on the mark, instead of one
    // waitForFinished call that would hold the worker for the whole timeout.
    // Pinned against real processes, the way the header promises.
    void waitForProcessKillsARealProcessOnTheMark() {
        CancelToken token;
        token.request();
        QProcess process;
        process.start("sleep", {"30"});
        QVERIFY(process.waitForStarted(2000));
        QElapsedTimer t;
        t.start();
        ThumbnailerRunnable::waitForProcess(process, &token, 8000);
        QVERIFY2(t.elapsed() < 4000, "a marked wait held the worker for the full timeout");
        QCOMPARE(process.state(), QProcess::NotRunning);
    }

    void waitForProcessWithoutATokenWaitsInTheOneCall() {
        QProcess process;
        process.start("true");
        QVERIFY(process.waitForStarted(2000));
        ThumbnailerRunnable::waitForProcess(process, nullptr, 8000);
        QCOMPARE(process.state(), QProcess::NotRunning);
    }

    // One frame file per concurrent extraction: the same clip at two sizes (the
    // grid and the strip each run their own thumbnailer) used to hand two mpv
    // writers one path, and the reader that lost the race cached a torn frame
    // under its own correct id. The pid is the cross-instance face of the same
    // race; basename stays for a crashed run's leftovers.

    // One frame file per concurrent extraction: the same clip at two sizes (the
    // grid and the strip each run their own thumbnailer) used to hand two mpv
    // writers one path, and the reader that lost the race cached a torn frame
    // under its own correct id. The pid is the cross-instance face of the same
    // race; basename stays for a crashed run's leftovers.
    void videoFrameTempPathSeparatesConcurrentExtractions() {
        QTemporaryDir own;
        QVERIFY(own.isValid());
        const QString dir = own.filePath("tmp") + "/";
        const QString clip = "/some/where/clip.mp4";
        const QString base = ThumbnailerRunnable::videoFrameTempPath(dir, clip, 120, false);
        QCOMPARE(base, ThumbnailerRunnable::videoFrameTempPath(dir, clip, 120, false));
        QVERIFY(base != ThumbnailerRunnable::videoFrameTempPath(dir, clip, 240, false));
        QVERIFY(base != ThumbnailerRunnable::videoFrameTempPath(dir, clip, 120, true));
        QVERIFY(base != ThumbnailerRunnable::videoFrameTempPath(dir, "/other/clip.mp4", 120, false));
        QVERIFY(base.startsWith(dir));
        QVERIFY(base.contains(QStringLiteral("clip.mp4-")));
        QVERIFY2(base.contains(QString::number(QCoreApplication::applicationPid())),
                 "the frame file name must be process-unique");
    }

    // ShortcutBuilder had no coverage at all; these pin the event-to-sequence
    // mapping against the hardcoded default input map (Ctrl, WheelUp/Down,
    // the button names, and the double-click / RMB-release quirks).

    void videoFrameTempFilesDoNotCollideAcrossDirectories() {
        // Two clips that share a basename in different folders used to be handed
        // the same temp file in the shared tmpDir: one worker read and removed the
        // other's frame, then cached it under its own correct id — a wrong
        // thumbnail that stays wrong. The name has to be per source path, and
        // stable for one source because the writer deletes it after use.
        const QString a = ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/x/clip.mp4", 120, false);
        const QString b = ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/y/clip.mp4", 120, false);
        QVERIFY2(a != b, "same basename in different folders shared one temp file");
        QCOMPARE(a, ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/x/clip.mp4", 120, false));
        QVERIFY(a.startsWith("/tmp/t/clip.mp4-"));
        // a source that differs by more than its directory still differs
        QVERIFY(a != ThumbnailerRunnable::videoFrameTempPath("/tmp/t/", "/videos/x/clip.mkv", 120, false));
    }

    // The store's sweep must not run on the UI thread. Telling our entries from
    // another program's means reading each PNG's text chunks, and that measured
    // ~40x the stat-only walk it replaced (549 files: 135us -> 5519us on the
    // development machine) — a startup freeze that grows with the library.
    //
    // Proved by THREAD IDENTITY, which is the only thing that distinguishes the
    // two implementations: the sweep records the QThread it ran on, and the
    // test compares it with its own. An earlier draft asserted "the work has
    // not happened yet while the pool is busy" — that reads as plausible and
    // stays green even when the sweep is run inline, because a call cannot
    // observe its own absence. A break-test caught it.

    // The store's sweep must not run on the UI thread. Telling our entries from
    // another program's means reading each PNG's text chunks, and that measured
    // ~40x the stat-only walk it replaced (549 files: 135us -> 5519us on the
    // development machine) — a startup freeze that grows with the library.
    //
    // Proved by THREAD IDENTITY, which is the only thing that distinguishes the
    // two implementations: the sweep records the QThread it ran on, and the
    // test compares it with its own. An earlier draft asserted "the work has
    // not happened yet while the pool is busy" — that reads as plausible and
    // stays green even when the sweep is run inline, because a call cannot
    // observe its own absence. A break-test caught it.
    void theStoreSweepRunsOnAWorkerNotOnTheCallingThread() {
        resetFixture();
        WorkerPool pool(1);
        Thumbnailer thumb(pool, EngineBridge::instance());
        const Qt::HANDLE caller = QThread::currentThreadId();

        std::atomic<Qt::HANDLE> ranOn{nullptr};
        thumb.scheduleSweep(15, ThumbnailCache::cacheMaxBytes, &ranOn);
        // waitForOwner, not waitForDone: the latter is Thumbnailer's own owner
        pool.waitForOwner(PoolOwner::Maintenance);

        QVERIFY2(ranOn.load() != nullptr, "the sweep never ran");
        QVERIFY2(ranOn.load() != caller, "the store sweep ran on the calling thread; it must run on a pool worker");
    }

    // Teardown half: the Thumbnailer owns the cache, and it must WAIT for a
    // running sweep rather than cancel it. cancel() only marks; a marked sweep
    // would keep walking the store and then touch a destroyed object. Reaching
    // the end of this scope without a crash is the assertion — the repeated
    // rounds are what make it likely to be reached at all.

    // Teardown half: the Thumbnailer owns the cache, and it must WAIT for a
    // running sweep rather than cancel it. cancel() only marks; a marked sweep
    // would keep walking the store and then touch a destroyed object. Reaching
    // the end of this scope without a crash is the assertion — the repeated
    // rounds are what make it likely to be reached at all.
    void tearingDownAThumbnailerWaitsForARunningSweep() {
        resetFixture();
        for(int i = 0; i < 3; ++i) {
            WorkerPool pool(2);
            Thumbnailer thumb(pool, EngineBridge::instance());
            thumb.scheduleSweep(15, ThumbnailCache::cacheMaxBytes);
            // no wait here: the destructor has to do it
        }
        QVERIFY(true);
    }

    void thumbnailerAccountingSurvivesAClearMidFlight() {
        resetFixture();
        WorkerPool pool(2);
        Thumbnailer thumb(pool, EngineBridge::instance());
        QSignalSpy ready(&thumb, &Thumbnailer::thumbnailReady);
        QVERIFY(ready.isValid());

        // every request must come back through the engine's scheduler
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 32, false, false);
        thumb.getThumbnailAsync(fixtureDir + "/img2.jpg", 32, false, false);
        // a duplicate collapses instead of double-dispatching
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 32, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 10000);
        coretest::drainEvents();
        QCOMPARE(ready.count(), 2);

        // the clear happens before the event loop drains anything, so the
        // third request is provably still in flight: clearTasks must report
        // it back to the engine as done, or its slot leaks and the pipeline
        // silently stalls forever after
        thumb.getThumbnailAsync(fixtureDir + "/img10.jpg", 32, false, false);
        thumb.clearTasks();

        // the pipeline still works after the clear: a fresh request dispatches
        // and completes (img10's stale dispatch may also complete on drain,
        // so assert a lower bound, not an exact count)
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 64, false, false);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 3, 10000);
        thumb.waitForDone();
    }

    // The presenter-level shape of the shared instance, pinned at the unit the
    // presenters share: one wave's clearTasks kills the other wave's
    // still-pending requests (the engine's pending queue is process-wide,
    // which predates the shared instance), and the recovery is the next wave
    // re-requesting the same files. This pins the recovery: every file of a
    // killed batch arrives after the re-request, whichever fate its first job
    // met — dispatched and delivered, canceled before its checkpoint and
    // re-requested fresh, or canceled inside generate and delivered by the
    // survivor. Narrowing the kill window needs engine batch ids; while this
    // recovery holds, that protocol change buys nothing worth its pin tax.

    // The presenter-level shape of the shared instance, pinned at the unit the
    // presenters share: one wave's clearTasks kills the other wave's
    // still-pending requests (the engine's pending queue is process-wide,
    // which predates the shared instance), and the recovery is the next wave
    // re-requesting the same files. This pins the recovery: every file of a
    // killed batch arrives after the re-request, whichever fate its first job
    // met — dispatched and delivered, canceled before its checkpoint and
    // re-requested fresh, or canceled inside generate and delivered by the
    // survivor. Narrowing the kill window needs engine batch ids; while this
    // recovery holds, that protocol change buys nothing worth its pin tax.
    void aClearedBatchIsRecoveredByTheNextWave() {
        resetFixture();
        WorkerPool pool(2);
        Thumbnailer thumb(pool, EngineBridge::instance());
        QSet<QString> ready;
        QObject::connect(&thumb, &Thumbnailer::thumbnailReady, &thumb,
                         [&ready](std::shared_ptr<Thumbnail>, const QString &path) {
                             ready.insert(path);
                         });
        const QStringList batch = {
            fixtureDir + "/img1.jpg",
            fixtureDir + "/img2.jpg",
            fixtureDir + "/img10.jpg",
        };
        for(const QString &path : batch)
            thumb.getThumbnailAsync(path, 32, false, false);
        // three requests, two in flight: the clear provably kills a pending
        // request and marks jobs that may be inside generate()
        thumb.clearTasks();

        // the next wave re-requests the same files, and the batch recovers
        for(const QString &path : batch)
            thumb.getThumbnailAsync(path, 32, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), batch.count(), 10000);
        thumb.waitForDone();
    }
};

#include "thumbnailer_tests.moc"

int runThumbnailerTests(int argc, char **argv) {
    TestThumbnailer test;
    return runIfOwned(test, argc, argv);
}
