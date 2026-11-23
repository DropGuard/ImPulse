#include <QThread>
#include <atomic>
#include <thread>

#include "gtest_qt.h"
#include "engine_stack_fixture.h"
#include "components/workerpool/workerpool.h"

// The WorkerPool's own contracts, independent of any component using it:
// persistent workers (the TSan-tree pin for "our pools stop churning
// threads"), explicit ownership (drain returns unstarted jobs, the pool is
// the single deleter), priority order, and per-owner waiting.
class TestWorkerPool : public ::testing::Test, protected EngineStackFixture {
  protected:
    TestWorkerPool() : EngineStackFixture("workerpool") {
    }
};

namespace {

class CountingJob : public PoolJob {
  public:
    CountingJob(std::atomic_int *_runs, std::atomic_int *_deaths) : m_runs(_runs), m_deaths(_deaths) {
    }
    ~CountingJob() override {
        m_deaths->fetch_add(1);
    }
    void run() override {
        m_runs->fetch_add(1);
    }

  private:
    std::atomic_int *m_runs;
    std::atomic_int *m_deaths;
};

// occupies its worker until released, so the test controls what stays queued
class GatedJob : public PoolJob {
  public:
    GatedJob(std::atomic_bool *_gate, std::atomic_int *_starts, std::atomic_int *_deaths)
        : m_gate(_gate), m_starts(_starts), m_deaths(_deaths) {
    }
    ~GatedJob() override {
        m_deaths->fetch_add(1);
    }
    void run() override {
        m_starts->fetch_add(1);
        while(!m_gate->load())
            QThread::msleep(1);
    }

  private:
    std::atomic_bool *m_gate;
    std::atomic_int *m_starts;
    std::atomic_int *m_deaths;
};

// A job type that must never appear under PoolOwner::Thumbnailer. It pins the
// type guard in hasQueuedOrRunning<Job>: the pool asserts that an owner holds
// one kind of job, so a second kind fails by name at the query instead of
// reaching the caller's lambda as a mis-typed reference.
class ForeignJob : public PoolJob {
  public:
    void run() override {
    }
};

class OrderJob : public PoolJob {
  public:
    OrderJob(int _marker, std::atomic_int *_next, int *_order) : m_marker(_marker), m_next(_next), m_order(_order) {
    }
    void run() override {
        m_order[m_next->fetch_add(1)] = m_marker;
    }

  private:
    int m_marker;
    std::atomic_int *m_next;
    int *m_order;
};

// spins inside run() until the cancel mark arrives — the deterministic
// stand-in for "cancel() fired while this job was running". The spin carries
// a deadline past every wait this file makes: an aborted test function never
// reaches the matching cancel, and an abandoned spinning job would wedge the
// pool's join forever — a ctest timeout that buries the failed assert naming
// the real cause. The CI timeout on cancelSparesOtherOwners was exactly that.
class CancelGatedJob : public PoolJob {
  public:
    CancelGatedJob(std::atomic_bool *_sawMark, std::atomic_int *_starts, std::atomic_int *_deaths)
        : m_sawMark(_sawMark), m_starts(_starts), m_deaths(_deaths) {
    }
    ~CancelGatedJob() override {
        m_deaths->fetch_add(1);
    }
    void run() override {
        m_starts->fetch_add(1);
        // 15000 × 1ms ≫ the 10s ASSERT_TRY_EQ_TIMEOUT ceilings, so a mark that
        // will ever arrive wins the race against this deadline
        for(int i = 0; i < 15000 && !cancelRequested(); ++i)
            QThread::msleep(1);
        if(cancelRequested())
            m_sawMark->store(true);
    }

  private:
    std::atomic_bool *m_sawMark;
    std::atomic_int *m_starts;
    std::atomic_int *m_deaths;
};

} // namespace

TEST_F(TestWorkerPool, persistentWorkersSurviveSubmitAndDrainWaves) {
    // The TSan-tree pin for "our pools stop churning threads": QThreadPool's
    // waitForDone() joined every pool thread on every wave (through the
    // unintercepted pthread_clockjoin_np), so the next wave's pthread_create
    // reused a registry-live tid and CHECK-aborted — 40 waves were already
    // fatal (~/tsan-qthreadpool-check/). This pool creates its workers once
    // and joins them once, so the same wave shape must simply finish. The
    // regression here IS the sanitizer abort in build-tsan, not a failed
    // assert.
    std::atomic_int runs{0}, deaths{0};
    {
        WorkerPool pool(2);
        for(int wave = 0; wave < 400; ++wave) {
            pool.submit(PoolOwner::Loader, 0, std::make_unique<CountingJob>(&runs, &deaths));
            pool.waitForAll();
        }
    }
    EXPECT_EQ(runs.load(), 400);
    EXPECT_EQ(deaths.load(), 400);
}

TEST_F(TestWorkerPool, drainReturnsExactlyTheUnstartedJobs) {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, runs{0}, deaths{0};
    {
        WorkerPool pool(1); // a single worker: the first job occupies it
        pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
        ASSERT_TRY_EQ_TIMEOUT(starts.load(), 1, 10000); // worker is inside run()
        for(int i = 0; i < 3; ++i)
            pool.submit(PoolOwner::Loader, 0, std::make_unique<CountingJob>(&runs, &deaths));

        auto taken = pool.drain(PoolOwner::Loader);
        EXPECT_TRUE(taken.size() == 3) << "only the queued jobs come back, not the running one";
        EXPECT_EQ(runs.load(), 0); // so none of them ever ran
        taken.clear();             // caller deletes what it took back
        EXPECT_EQ(deaths.load(), 3);

        gate = true; // the running job finishes normally
        pool.waitForOwner(PoolOwner::Loader);
        EXPECT_EQ(runs.load(), 0); // drained jobs never ran
        // the gated job was pool-deleted. The destructor lands on the worker
        // thread right after run() returns — the documented order — but
        // waitForOwner's guarantee is run() completion, not the destructor's
        // instant: under load the two are observable apart (measured one full
        // gap apart on a busy desktop). What is pinned here is that the pool
        // destroys it, which must still hold.
        ASSERT_TRY_EQ_TIMEOUT(deaths.load(), 4, 10000);
    }
    EXPECT_EQ(deaths.load(), 4);
}

// The typed query refuses to reinterpret a job it does not own. An owner holds
// one kind of job by construction; a second kind is a programming error, and
// the query answers "no match" with a warning naming both, instead of handing
// the caller a mis-typed reference. Observed through the return value rather
// than a death test, so a query can never take the process down.
TEST_F(TestWorkerPool, typedQueryRejectsAForeignJob) {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, deaths{0};
    WorkerPool pool(1);
    // RAII: a failing QVERIFY returns from the function, and the gate must
    // still open or waitForOwner below would block forever (this hung the
    // suite once already).
    struct Release {
        std::atomic_bool &gate;
        ~Release() {
            gate.store(true);
        }
    } release{gate};

    // hold the only worker with a Maintenance job so the foreign one stays
    // QUEUED — a queued job is what the query walks first
    pool.submit(PoolOwner::Maintenance, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
    ASSERT_TRY_EQ_TIMEOUT(starts.load(), 1, 10000);

    // the only job under Thumbnailer is the foreign type
    pool.submit(PoolOwner::Thumbnailer, 0, std::make_unique<ForeignJob>());

    bool reached = false;
    // The warning is the contract, not decoration: a typed query that meets a
    // foreign job must SAY so where the query runs, because the alternative —
    // a silent "no match" — is indistinguishable from a genuine one. QTest's
    // ignoreMessage only works inside QTest::qExec (measured: it is a no-op
    // here, the message prints and nothing is checked), so the handler is ours.
    // the message is matched on its own words: typeid(...).name() is the mangled
    // spelling, and pinning that would pin the compiler.
    static QStringList warned;
    warned.clear();
    QtMessageHandler previous =
        qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
            if(type == QtWarningMsg)
                warned << message;
        });
    const bool found = pool.hasQueuedOrRunning<GatedJob>(PoolOwner::Thumbnailer, [&](const GatedJob &) {
        reached = true;
        return true;
    });
    qInstallMessageHandler(previous);
    EXPECT_TRUE(!found) << "the query reported a match for a job it could not type";
    EXPECT_TRUE(!reached) << "the caller's lambda ran on a job of the wrong type";
    EXPECT_EQ(warned.size(), 1);
    EXPECT_TRUE(!warned.isEmpty() && warned.first().startsWith(QStringLiteral("hasQueuedOrRunning: owner")));

    gate.store(true);
    pool.waitForOwner(PoolOwner::Thumbnailer);
    pool.waitForOwner(PoolOwner::Maintenance);
}

TEST_F(TestWorkerPool, priorityOrderIsHonoredWithinASharedPool) {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, next{0}, deaths{0};
    int order[3] = {0, 0, 0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths)); // occupies
    ASSERT_TRY_EQ_TIMEOUT(starts.load(), 1, 10000);
    // submitted low first: priority, not submission order, decides
    pool.submit(PoolOwner::Loader, 0, std::make_unique<OrderJob>(0, &next, order));
    pool.submit(PoolOwner::Loader, 2, std::make_unique<OrderJob>(2, &next, order));
    pool.submit(PoolOwner::Scaler, 1, std::make_unique<OrderJob>(1, &next, order));
    gate = true;
    pool.waitForAll();
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 0);
}

TEST_F(TestWorkerPool, waitForOwnerWaitsOnlyForItsOwner) {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, runs{0}, deaths{0};
    WorkerPool pool(2);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
    ASSERT_TRY_EQ_TIMEOUT(starts.load(), 1, 10000);
    pool.submit(PoolOwner::Thumbnailer, 0, std::make_unique<CountingJob>(&runs, &deaths));
    // returns while Loader is still held: ownership scopes the wait
    pool.waitForOwner(PoolOwner::Thumbnailer);
    EXPECT_EQ(runs.load(), 1);
    EXPECT_EQ(starts.load(), 1);
    EXPECT_TRUE(!gate.load()) << "waitForOwner(Thumbnailer) must not wait out the Loader job";
    gate = true;
    pool.waitForOwner(PoolOwner::Loader);
    pool.waitForAll();
}

// waitForAll (kept as the pool's whole-drain primitive; no product caller uses
// it now, the settings bus moved to the narrow forms) must wait out EVERY owner,
// not just the one whose pipeline the caller cares about. This pins the width,
// because that width is what a caller reaching for the global drain would pay on
// the UI thread.
TEST_F(TestWorkerPool, waitForAllWaitsForEveryOwner) {
    std::atomic_bool gate{false};
    std::atomic_int loaderStarts{0}, thumbRuns{0}, deaths{0};
    WorkerPool pool(2);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &loaderStarts, &deaths));
    ASSERT_TRY_EQ_TIMEOUT(loaderStarts.load(), 1, 10000);
    pool.submit(PoolOwner::Thumbnailer, 0, std::make_unique<CountingJob>(&thumbRuns, &deaths));
    pool.waitForOwner(PoolOwner::Thumbnailer);
    EXPECT_EQ(thumbRuns.load(), 1);        // one owner is provably finished with...
    EXPECT_TRUE(loaderStarts.load() == 1); // ...while another is still inside a job

    std::atomic_bool returned{false};
    std::thread waiter([&pool, &returned]() {
        pool.waitForAll();
        returned.store(true);
    });
    QThread::msleep(100);
    EXPECT_TRUE(!returned.load()) << "waitForAll() returned while a Loader job was still running";
    gate = true;
    waiter.join(); // it was waiting: opening the gate releases it
    EXPECT_TRUE(returned.load());
    // the pool destroys completed jobs on its worker thread, so the death
    // count catches up asynchronously — asserting it as an instant value is
    // a race this test authored, and it was the one red that arrived
    // unnamed in the ASan sweep
    ASSERT_TRY_EQ_TIMEOUT(deaths.load(), 2, 10000);
}

// cancel() marks; what the pool does with the mark is exactly three facts: a
// job marked while queued is discarded at the worker's pop and never starts,
// a job marked inside run() observes the mark at its own seam, the mark
// reaches nothing outside the owner it was given, and it satisfies no waiter
// — only a retire does.
TEST_F(TestWorkerPool, cancelReachesQueuedJobs) {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, runs{0}, deaths{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
    ASSERT_TRY_EQ_TIMEOUT(starts.load(), 1, 10000);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<CountingJob>(&runs, &deaths));
    pool.cancel(PoolOwner::Loader);
    gate = true;
    pool.waitForOwner(PoolOwner::Loader);
    // the pop checkpoint is where cancel reaches a queued job: the marked job
    // is discarded there, so it never runs — cancel reaches queued jobs by
    // making sure they never start
    EXPECT_EQ(runs.load(), 0);
    ASSERT_TRY_EQ_TIMEOUT(deaths.load(), 2, 10000);
}

TEST_F(TestWorkerPool, cancelReachesAJobInsideRun) {
    std::atomic_bool sawMark{false};
    std::atomic_int starts{0}, deaths{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<CancelGatedJob>(&sawMark, &starts, &deaths));
    // it is inside run(), spinning on the mark — cancelling before the pop
    // would discard the job at the checkpoint instead of marking it, and this
    // test pins the running-job path, not the queued one
    ASSERT_TRY_EQ_TIMEOUT(starts.load(), 1, 10000);
    pool.cancel(PoolOwner::Loader);
    // this is the waitForOwner the consumers' clearTasks/dtor pair leans on:
    // it can only return once the running record was retired, so a record
    // that leaked would hang here rather than pass
    pool.waitForOwner(PoolOwner::Loader);
    EXPECT_EQ(sawMark.load(), true);
    ASSERT_TRY_EQ_TIMEOUT(deaths.load(), 1, 10000);
}

TEST_F(TestWorkerPool, cancelSparesOtherOwners) {
    std::atomic_bool loaderMark{false}, scalerMark{false};
    std::atomic_int starts{0}, deaths{0};
    WorkerPool pool(2);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<CancelGatedJob>(&loaderMark, &starts, &deaths));
    pool.submit(PoolOwner::Scaler, 0, std::make_unique<CancelGatedJob>(&scalerMark, &starts, &deaths));
    // both jobs confirmed inside run() before any cancel: a mark landing on a
    // queued job discards it at the pop checkpoint, loaderMark would never
    // rise, and the aborted test function would leave the other job spinning
    // into the pool's join (the Windows CI timeout, 300s of nothing)
    ASSERT_TRY_EQ_TIMEOUT(starts.load(), 2, 10000);
    pool.cancel(PoolOwner::Loader);
    ASSERT_TRY_EQ_TIMEOUT(loaderMark.load(), true, 10000);
    QThread::msleep(100);
    EXPECT_TRUE(!scalerMark.load()) << "cancel(Loader) marked a job that belongs to Scaler";
    pool.cancel(PoolOwner::Scaler);
    pool.waitForAll();
    EXPECT_EQ(scalerMark.load(), true);
    ASSERT_TRY_EQ_TIMEOUT(deaths.load(), 2, 10000);
}

TEST_F(TestWorkerPool, cancelDoesNotSatisfyWaiters) {
    std::atomic_bool gate{false};
    std::atomic_int starts{0}, deaths{0};
    WorkerPool pool(1);
    pool.submit(PoolOwner::Loader, 0, std::make_unique<GatedJob>(&gate, &starts, &deaths));
    ASSERT_TRY_EQ_TIMEOUT(starts.load(), 1, 10000);
    std::atomic_bool returned{false};
    std::thread waiter([&pool, &returned]() {
        pool.waitForOwner(PoolOwner::Loader);
        returned.store(true);
    });
    QThread::msleep(100);
    EXPECT_TRUE(!returned.load()); // blocked on the running job, as designed
    pool.cancel(PoolOwner::Loader);
    QThread::msleep(100);
    EXPECT_TRUE(!returned.load()) << "cancel() satisfied a waiter without a retire";
    gate = true;
    waiter.join(); // the retire, not the cancel, is what released it
    EXPECT_TRUE(returned.load());
    ASSERT_TRY_EQ_TIMEOUT(deaths.load(), 1, 10000);
}
