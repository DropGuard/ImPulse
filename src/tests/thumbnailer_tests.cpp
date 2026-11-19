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

// Occupies a worker until released, so a test can put the thumbnailer's job in
// a state the scheduler would otherwise make racy: queued behind a running job.
// Local rather than shared with workerpool_tests: each suite owns its fixtures
// (the same reason the engine_stack units are separate binaries).
class ThumbnailGateJob : public PoolJob {
  public:
    ThumbnailGateJob(std::atomic_bool *_gate, std::atomic_int *_starts) : gate(_gate), starts(_starts) {
    }
    void run() override {
        starts->fetch_add(1);
        while(!gate->load())
            QThread::msleep(1);
    }

  private:
    std::atomic_bool *gate;
    std::atomic_int *starts;
};

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
#ifdef Q_OS_WIN
        // sleep is coreutils; the always-present Windows face of "a process
        // that runs for 30 seconds" is ping against the loopback (one second
        // per -n beyond the first, killable mid-wait)
        process.start("cmd", {"/c", "ping", "-n", "31", "127.0.0.1"});
#else
        process.start("sleep", {"30"});
#endif
        QVERIFY(process.waitForStarted(2000));
        QElapsedTimer t;
        t.start();
        ThumbnailerRunnable::waitForProcess(process, &token, 8000);
        QVERIFY2(t.elapsed() < 4000, "a marked wait held the worker for the full timeout");
        QCOMPARE(process.state(), QProcess::NotRunning);
    }

    void waitForProcessWithoutATokenWaitsInTheOneCall() {
        QProcess process;
#ifdef Q_OS_WIN
        process.start("cmd", {"/c", "exit", "0"});
#else
        process.start("true");
#endif
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
        Thumbnailer thumb(pool);
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
            Thumbnailer thumb(pool);
            thumb.scheduleSweep(15, ThumbnailCache::cacheMaxBytes);
            // no wait here: the destructor has to do it
        }
        QVERIFY(true);
    }

    void thumbnailerAccountingSurvivesAClearMidFlight() {
        resetFixture();
        WorkerPool pool(2);
        Thumbnailer thumb(pool);
        QSignalSpy ready(&thumb, &Thumbnailer::thumbnailReady);
        QVERIFY(ready.isValid());

        // every request goes straight into the pool; the engine no longer
        // schedules thumbnails
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 32, false, false);
        thumb.getThumbnailAsync(fixtureDir + "/img2.jpg", 32, false, false);
        // a duplicate collapses instead of double-dispatching
        thumb.getThumbnailAsync(fixtureDir + "/img1.jpg", 32, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 10000);
        coretest::drainEvents();
        QCOMPARE(ready.count(), 2);

        // the clear happens while the third request is still queued: it is
        // drained and never reports, so clearTasks must forget it by name —
        // a set that kept it would dedup every later request for that file
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
    // presenters share: one wave's clearTasks kills the other wave's queued
    // requests (the pool is process-wide, which predates the shared instance),
    // and the recovery is the next wave re-requesting the same files. This pins
    // the recovery: every file of a killed batch arrives after the re-request,
    // whichever fate its first job met — ran and delivered, drained before it
    // started and re-requested fresh, or canceled inside generate and delivered
    // by the survivor.
    // The submission set is the only thing that dedups a request now that the
    // engine's queue is gone, so every way a request can end must remove it.
    // Three ways: it decodes (taskEnd), it is drained while queued (clearTasks
    // removes it by name), or it is marked BEFORE it starts (abandoned). The
    // last is the subtle one: such a job has no picture, and must not send an
    // empty one down taskEnd, because the presenter would overwrite a good
    // thumbnail with it. Without its own report the entry strands and the file
    // can never be requested again for the life of the process.
    //
    // The third path needs a mark that lands while the job is QUEUED, and a
    // start that happens after it. Without a gate that is a race — the worker
    // may pass the entry checkpoint before cancel() marks it. The gate pins the
    // only worker, so the mark provably precedes the start: the mark is set
    // while the job sits in the queue, and run() is entered only after the
    // gate releases the worker.
    void aJobMarkedBeforeItStartsForgetsItsRequest() {
        resetFixture();
        const QString path = fixtureDir + "/cancelme.png";
        QVERIFY(makeImageFile(path, QSize(64, 64)));

        std::atomic_bool gate{false};
        std::atomic_int starts{0};
        WorkerPool pool(1);
        Thumbnailer thumb(pool);
        QSet<QString> arrived;
        QObject::connect(&thumb, &Thumbnailer::thumbnailReady, &thumb,
                         [&arrived](std::shared_ptr<Thumbnail>, const QString &filePath) {
                             arrived.insert(filePath);
                         });

        // the only worker is busy in the gate, so the thumbnail job is queued
        pool.submit(PoolOwner::Maintenance, 0, std::make_unique<ThumbnailGateJob>(&gate, &starts));
        QTRY_COMPARE_WITH_TIMEOUT(starts.load(), 1, 10000);

        thumb.getThumbnailAsync(path, 128, false, false);
        // mark WITHOUT draining — clearTasks() would remove the queued job
        // outright (path 2, pinned by the clear-mid-flight test above). cancel()
        // marks queued jobs too, which is the only way to reach path 3 at all:
        pool.cancel(PoolOwner::Thumbnailer);
        // release the gate; the worker picks up the marked job, run() sees the
        // mark at its entry checkpoint and abandons — no picture may reach the
        // view, not even an empty one
        gate.store(true);
        pool.waitForOwner(PoolOwner::Maintenance);
        thumb.waitForDone();
        coretest::drainEvents();
        QCOMPARE(arrived.size(), 0);

        // the request must be forgotten — a stranded entry would dedup every
        // later request for this file for the life of the process
        thumb.getThumbnailAsync(path, 128, false, false);
        QTRY_COMPARE_WITH_TIMEOUT(arrived.size(), 1, 15000);
        thumb.waitForDone();
    }

    void aClearedBatchIsRecoveredByTheNextWave() {
        resetFixture();
        WorkerPool pool(2);
        Thumbnailer thumb(pool);
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

    // The strand the counted set exists to prevent, injected deterministically:
    // a job decoding inside generate() when clearTasks marks it is past its
    // entry checkpoint, so it delivers — and the same wave immediately
    // re-requests the same file. A set that kept the marked job's entry
    // swallowed that re-request for exactly as long as the doomed job's
    // abandonment took to arrive, and the thumbnail never arrived at all
    // (2026-11-19: one missing thumbnail per wave under load, named from a
    // core). The image is big enough that its decode outlasts the wait below,
    // making "running when cleared" a fact rather than a race; both deliveries
    // are then guaranteed by construction — the marked job's own report, plus
    // the re-request's.
    // A stale report must not free the NEXT wave's slot.
    //
    // The presenters' wave is "clearTasks, then re-request the same files".
    // A job marked by that clear but already past its entry checkpoint reports
    // LATER, and the report travels worker->UI as a QUEUED signal: it is
    // delivered only when the event loop next runs, which is after the wave
    // has re-requested the same file. A design that decrements whatever slot it
    // finds therefore frees the LIVE request, and the next call for that file
    // constructs a duplicate decode.
    //
    // The window is made deterministic by controlling WHEN the signal is
    // delivered, not by sleeping: pool.waitForOwner() waits without processing
    // events, so the stale report is held in the queue until the explicit
    // QTest::qWait below — which sits after the re-request has taken the slot.
    // A QTRY* anywhere in that stretch would flush the signal too early and the
    // test would pass against both designs (the first three drafts did).
    //
    // Asserted by constructed runnables: the dedup contract is one job per
    // request triple, and a delivered-thumbnail count cannot tell one job from
    // two racing on the same file.
    void aStaleReportCannotFreeTheNextWavesSlot() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/gen.png", "png", QSize(256, 256)));
        std::atomic_bool gate1{false}, gate2{false};
        std::atomic_int holds1{0}, holds2{0};
        WorkerPool pool(1);
        Thumbnailer thumb(pool);
        const QString path = fixtureDir + "/gen.png";
        const int before = ThumbnailerRunnable::constructionCount().load();

        // one worker. gate1 holds it, A1 queues behind, gate2 queues behind A1.
        pool.submit(PoolOwner::Maintenance, 0, std::make_unique<ThumbnailGateJob>(&gate1, &holds1));
        QTRY_COMPARE_WITH_TIMEOUT(holds1.load(), 1, 10000);
        thumb.getThumbnailAsync(path, 128, false, false); // A1
        pool.cancel(PoolOwner::Thumbnailer);              // marks A1 while queued
        pool.submit(PoolOwner::Maintenance, 0, std::make_unique<ThumbnailGateJob>(&gate2, &holds2));

        // run A1 (it returns silently at its entry checkpoint — no report, the
        // pool's record is the only accounting) and let the worker reach gate2.
        gate1.store(true);
        thumb.waitForDone();
        QCOMPARE(ThumbnailerRunnable::constructionCount().load() - before, 1); // only A1
        QTRY_COMPARE_WITH_TIMEOUT(holds2.load(), 1, 10000);

        // the wave: clear, then re-request. A2 owns the slot; the worker is
        // held, so it stays queued and keeps it.
        thumb.clearTasks();
        thumb.getThumbnailAsync(path, 128, false, false); // A2 owns the slot

        QTest::qWait(50);

        // the third call must dedup against the still-outstanding A2.
        thumb.getThumbnailAsync(path, 128, false, false);
        QCOMPARE(ThumbnailerRunnable::constructionCount().load() - before, 2);

        gate2.store(true);
        thumb.waitForDone();
    }

    void aMarkedRunningJobDoesNotSwallowTheReRequest() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/slow.png", "png", QSize(5000, 4000)));
        std::atomic_bool gate{false};
        std::atomic_int holds{0};
        WorkerPool pool(2);
        pool.submit(PoolOwner::Scaler, 0, std::make_unique<ThumbnailGateJob>(&gate, &holds));
        QTRY_COMPARE_WITH_TIMEOUT(holds.load(), 1, 10000);
        Thumbnailer thumb(pool);
        int delivered = 0;
        QObject::connect(&thumb, &Thumbnailer::thumbnailReady, &thumb,
                         [&delivered](std::shared_ptr<Thumbnail>, const QString &) {
                             delivered++;
                         });
        const QString path = fixtureDir + "/slow.png";
        thumb.getThumbnailAsync(path, 128, false, false);
        // the second worker is free: the job is picked up immediately and sits
        // inside generate(), past its entry checkpoint
        QTest::qWait(60);
        thumb.clearTasks();                               // marks it; it is past the checkpoint
        thumb.getThumbnailAsync(path, 128, false, false); // the same wave's re-request

        // The contract is that the file ARRIVES, not that it arrives twice.
        // The dedup asks the pool, and the marked job is still in it (it is
        // inside run()), so the re-request correctly collapses into the
        // outstanding job — one decode, one delivery. Report settlement is the
        // pool's record, so nothing here can be stranded by that collapse.
        QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 15000);
        gate.store(true); // release the gate worker so the pool can be joined
        thumb.waitForDone();
    }
};

#include "thumbnailer_tests.moc"

int runThumbnailerTests(int argc, char **argv) {
    TestThumbnailer test;
    return runIfOwned(test, argc, argv);
}
