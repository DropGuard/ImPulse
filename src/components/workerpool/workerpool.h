#pragma once

#include <QtGlobal>
#include <atomic>
#include <functional>
#include <type_traits>
#include <typeinfo>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The one executor the image pipeline runs on: Loader decodes, Scaler scales,
// Thumbnailer generates. Replaces three per-component QThreadPool instances,
// whose waitForDone() tore the pool down and rebuilt it on every wave — the
// shape that trips TSan's thread-registry CHECK, because Qt 6.10 joins pool
// threads via pthread_clockjoin_np and the sanitizer does not intercept that
// call (see the QIMGV_SANITIZERS block in the root CMakeLists.txt, and
// ~/tsan-qthreadpool-check/ for the minimal repro). Workers are created in the
// constructor and joined in the destructor; in between, the pool never creates
// or joins a thread.
//
// Ownership is explicit and single:
//   - submit() transfers the job to the pool. The pool destroys it on the
//     worker thread right after run() returns — the same pattern Thumbnailer's
//     autoDelete(true) runnables already relied on. The jobs are parentless,
//     childless, timerless QObjects whose signals carry copies of their
//     arguments, so queued delivery survives the sender.
//   - drain() hands the queued-but-unstarted jobs of one owner back to the
//     caller, which decides whether to delete or re-submit them.
// Nobody else ever deletes a submitted job.
//
// Priorities: Loader's tier-1 decode runs at 2, Scaler at 1, Loader's tier-2
// upgrade and Thumbnailer at 0. This is a deliberate consequence of sharing
// one pool: an active scaling now outranks a background full-res upgrade,
// where the per-pool isolation of the QThreadPool era never let them compete.

enum class PoolOwner : quint8 {
    Loader,
    Scaler,
    Thumbnailer,
    // Background upkeep that belongs to no request: the thumbnail store's
    // sweep is the first such job. It gets its own owner rather than riding
    // Thumbnailer's, because the two have opposite lifetimes — a sweep must not
    // be cancelled by the user changing folders (clearTasks cancels every
    // Thumbnailer job), and a sweep must not keep a folder change waiting.
    Maintenance
};

// One job's cancellation state, held by the job and readable by the static
// helpers its run() delegates into. Passing the handle is not passing ownership:
// whoever holds one can read it, and only WorkerPool::cancel() can set it.
class CancelToken {
  public:
    bool requested() const {
        return m_value.load(std::memory_order_relaxed);
    }
    void request() {
        m_value.store(true, std::memory_order_relaxed);
    }

  private:
    std::atomic_bool m_value{false};
};

class PoolJob {
  public:
    virtual ~PoolJob() = default;
    // runs on a worker thread; never dereferences the settings global —
    // per-request state travels inside the job (LoadPolicy, ScalerRequest)
    virtual void run() = 0;

    // Only WorkerPool::cancel() calls this. It is public because the pool is not
    // a friend of every job type; nothing outside the pool should mark a job,
    // because a job that was never asked to stop has no reason to check.
    void requestCancel() {
        m_cancel.request();
    }
    // The read side is public because the pool's worker loop evaluates the mark
    // at the pop: a job marked while queued is discarded there, before it can
    // enter the running record that the dedup query reads.
    bool cancelRequested() const {
        return m_cancel.requested();
    }

  protected:
    // for the static helpers a run() body hands its work to, which cannot reach
    // a member of an object they were never given
    const CancelToken &cancelToken() const {
        return m_cancel;
    }

  private:
    CancelToken m_cancel;
};

class WorkerPool {
  public:
    explicit WorkerPool(int threadCount);
    // the size it was built with. Read-only: resizing a live pool is a
    // different question (jobs hold threads' identities), so this answers
    // "how many executors exist" for callers that must match it.
    int threadCount() const {
        return m_threadCount;
    }
    ~WorkerPool();
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    void submit(PoolOwner owner, int priority, std::unique_ptr<PoolJob> job);
    // returns everything of `owner` that is queued but not started; running
    // jobs are not interrupted and complete normally
    std::vector<std::unique_ptr<PoolJob>> drain(PoolOwner owner);
    // Marks every job of `owner` — queued or inside run() — so each one can drop
    // its work at the last point where dropping is still free. It is a request,
    // not a stop: it never waits, never touches a job's result and cannot unwind
    // a call already inside a library, and a job that has passed its last
    // checkpoint runs to completion exactly as it would have. What it guarantees
    // is only that no job of this owner is still marked-unfinished when
    // waitForOwner() returns.
    //
    // Which of the two reporting rules a marked job follows is the owner's
    // business, not the pool's, and the two are not interchangeable: a
    // Thumbnailer job that never started reports nothing, and needs to: its
    // owner keeps no index that a report would settle — the pool's own
    // queued/running record IS the dedup, and this job leaves it when run()
    // returns. It also must not send an empty picture down taskEnd, or the
    // presenter would overwrite a good thumbnail with it; a Loader job that never started
    // its decode reports nothing and Loader::clearTasks() drops the task entry
    // itself, because reporting a failure for a file the user already left
    // would turn "no longer wanted" into "this file is broken".
    void cancel(PoolOwner owner);
    // Is any job of `owner` still queued or inside run() right now? The pool
    // is the only queue, so this is the question a dedup asks: "would
    // submitting this again add a second copy of work already pending?"
    //
    // Typed by the job class the caller owns: every owner submits one kind of
    // job, and the pool asserts that rather than trusting a cast. A second
    // kind appearing under the same owner fails HERE, by name, instead of
    // reaching the caller's lambda as a mis-typed reference and reading
    // garbage out of an unrelated vtable. `match` runs under the pool's mutex,
    // so it must not call back into the pool.
    //
    // Owning this fact here rather than mirroring it in the caller is the
    // point: a caller-side index has to be updated on every exit — completion,
    // drain, cancel, and a queued report arriving a wave too late — and each
    // missed exit is a duplicate decode or a request that can never be made
    // again. The pool already tracks all three states; asking it cannot drift.
    //
    // There is no strand window: a job marked while queued is discarded at the
    // worker's pop (before it enters `running`), and a job already past the pop
    // runs to completion and delivers — so a collapse can only land on a job
    // that will arrive. The check that makes this true is the worker loop's pop
    // checkpoint; the count-based dedup this replaced had to be corrected at
    // every exit instead, and the exit it missed was its strand.
    template <typename Job, typename Match> bool hasQueuedOrRunning(PoolOwner owner, Match &&match) const {
        static_assert(std::is_base_of_v<PoolJob, Job>, "hasQueuedOrRunning: ask about a PoolJob subclass");
        return anyQueuedOrRunning(owner, [&](const PoolJob &job) {
            const auto *typed = dynamic_cast<const Job *>(&job);
            if(!typed) {
                // A second kind of job under one owner is a programming error,
                // not a state. Answer "no match" rather than aborting — a
                // QUERY must not take the process down — and say so by name so
                // the mistake is visible where the query runs. Under
                // QT_FATAL_WARNINGS=1 (the dev tripwire) this is still fatal.
                qWarning("hasQueuedOrRunning: owner %d holds a job that is not a %s", static_cast<int>(owner),
                         typeid(Job).name());
                return false;
            }
            return match(*typed);
        });
    }
    // blocks until `owner` has nothing queued and nothing inside run()
    void waitForOwner(PoolOwner owner);
    void waitForAll();

  private:
    struct Entry {
        PoolOwner owner;
        int priority;
        unsigned long seq; // FIFO within one priority
        std::unique_ptr<PoolJob> job;
    };
    // A running job's owner and address, for the duration of run(). The record is
    // installed and removed under `mutex`, in the same sections that publish and
    // retire the job itself, so cancel() marking a record can never reach a job
    // the worker has already destroyed — which is the only way this could be a
    // use-after-free rather than a race.
    struct Running {
        PoolOwner owner;
        PoolJob *job;
    };

    // the untyped half: the template above supplies the type check, this owns
    // the lock and the two-list walk
    bool anyQueuedOrRunning(PoolOwner owner, const std::function<bool(const PoolJob &)> &match) const;
    void workerLoop();
    bool ownerIdle(PoolOwner owner); // mutex held
    bool allIdle();                  // mutex held

    int m_threadCount = 1; // as constructed; see threadCount()
    std::vector<std::thread> m_workers;
    mutable std::mutex m_mutex;
    std::condition_variable m_workAvailable; // workers wait for submissions
    std::condition_variable m_idle;          // waitForOwner/waitForAll wait here
    std::deque<Entry> m_queue;
    std::deque<Running> m_running; // one record per job inside run()
    unsigned long m_nextSeq = 0;
    bool m_stopping = false;
};
